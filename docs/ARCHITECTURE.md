# Architecture

## Overview

vmod-wasm embeds the [Wasmtime](https://wasmtime.dev/) WebAssembly runtime into Varnish Cache as a VMOD (Varnish Module). It supports two execution models:

1. **Raw host-function modules** — Wasm modules that import/export C-ABI functions directly
2. **Proxy-Wasm ABI modules** — Standard filters using the [Proxy-Wasm ABI v0.2.1](https://github.com/proxy-wasm/spec)

## Component Diagram

```
┌─────────────────────────────────────────────────────────────────────┐
│                           Varnish Cache                              │
│                                                                     │
│  ┌──────────┐    ┌──────────────────────────────────────────────┐  │
│  │   VCL    │───▶│              vmod_wasm.c                      │  │
│  │ (vcl_*)  │    │  • wasm.load() / wasm.execute()              │  │
│  │          │    │  • wasm.proxy_wasm_on_request()               │  │
│  │          │    │  • wasm.proxy_wasm_on_response()              │  │
│  └──────────┘    └──────────────┬───────────────────────────────┘  │
│                                  │                                   │
│                    ┌─────────────▼─────────────┐                    │
│                    │      wasm_engine.c         │                    │
│                    │  • Engine lifecycle        │                    │
│                    │  • Module compilation      │                    │
│                    │  • Epoch interruption      │                    │
│                    │  • Memory limits           │                    │
│                    └─────────────┬─────────────┘                    │
│                                  │                                   │
│              ┌───────────────────┼───────────────────┐              │
│              │                   │                   │              │
│  ┌───────────▼──────┐ ┌─────────▼─────────┐ ┌─────▼────────┐     │
│  │  store_pool.c    │ │  proxy_wasm.c      │ │ host_funcs.c │     │
│  │  • Pool of pre-  │ │  • ABI lifecycle   │ │ • env ns     │     │
│  │    compiled       │ │  • Context mgmt   │ │ • WASI ns    │     │
│  │    instances      │ │  • Header maps    │ │              │     │
│  │  • Per-request   │ │  • Properties     │ │              │     │
│  │    checkout       │ │  • Stream ctrl    │ └──────────────┘     │
│  └──────────────────┘ └────────┬──────────┘                        │
│                                 │                                    │
│              ┌──────────────────┼──────────────────┐               │
│              │                  │                  │               │
│  ┌───────────▼──────┐ ┌────────▼────────┐ ┌─────▼──────────┐    │
│  │ proxy_wasm_      │ │ proxy_wasm_     │ │ proxy_wasm_    │    │
│  │ headers.c        │ │ http.c          │ │ shared.c       │    │
│  │ • Header maps    │ │ • HTTP callouts │ │ • Shared KV    │    │
│  │ • Pseudo-headers│ │ • Conn pooling  │ │ • CAS support  │    │
│  │ • Trailers      │ │ • Circuit break │ │ • Shared queue │    │
│  └──────────────────┘ │ • SSRF prevent │ │ • Metrics      │    │
│                        └────────────────┘ └────────────────┘    │
│                                                                     │
│  ┌──────────────────┐  ┌──────────────────┐                       │
│  │   vdp_wasm.c     │  │   http_pool.c    │                       │
│  │ • VDP filter     │  │ • Connection     │                       │
│  │ • Response body  │  │   pool (curl)    │                       │
│  │   streaming      │  │ • Circuit breaker│                       │
│  └──────────────────┘  └──────────────────┘                       │
│                                                                     │
│                    ┌───────────────────────┐                        │
│                    │      Wasmtime         │                        │
│                    │  (libwasmtime C API)  │                        │
│                    └───────────────────────┘                        │
└─────────────────────────────────────────────────────────────────────┘
```

## Request Lifecycle

### Raw Module Execution

```
vcl_recv
  └─▶ wasm.execute("module", "func")
       └─▶ store_pool: checkout instance
            └─▶ wasmtime: call exported function
                 ├─▶ host_functions.c (if module imports env)
                 └─▶ return i32 result to VCL
            └─▶ store_pool: return instance
```

### Proxy-Wasm Request Lifecycle

```
vcl_recv
  └─▶ wasm.proxy_wasm_on_request("module")
       └─▶ store_pool: checkout instance
            ├─▶ proxy_on_context_create(ctx_id, root_id)
            ├─▶ proxy_on_request_headers(ctx_id, num_headers, end_of_stream)
            │    ├─▶ [module calls proxy_get_header_map_value]
            │    ├─▶ [module calls proxy_set_shared_data]
            │    ├─▶ [module calls proxy_http_call] ──▶ http_pool.c
            │    └─▶ returns Action (Continue | Pause)
            ├─▶ proxy_on_request_body(ctx_id, body_size, end_of_stream)
            └─▶ proxy_on_context_finalize(ctx_id)
       └─▶ store_pool: return instance
       └─▶ return action code to VCL ("0" = continue)

vcl_deliver
  └─▶ wasm.proxy_wasm_on_response("module")
       └─▶ proxy_on_response_headers(ctx_id, num_headers, end_of_stream)
       └─▶ proxy_on_response_body (via VDP if wasm_body filter active)
```

## Key Design Decisions

### Store Pooling

Instead of creating a new Wasmtime instance per request, vmod-wasm pre-creates a pool of instances at VCL init time. This eliminates compilation latency from the request path.

- Pool size: number of Varnish worker threads
- Checkout: O(1) via atomic counter
- Return: instance is reset (memory zeroed) before return to pool

### Epoch-Based Time Limits

Unlike fuel-based metering (which adds per-instruction overhead), epoch interruption uses a background thread that increments a global epoch counter. When a Wasm execution exceeds its deadline, the next epoch check traps the execution.

- Zero overhead during normal execution
- Background ticker thread (1ms resolution)
- Configurable per VCL load via `wasm.set_epoch_deadline(ms)`

### HTTP Connection Pooling

Proxy-Wasm HTTP callouts (`proxy_http_call`) reuse connections via an internal pool:

- libcurl multi-handle per Varnish thread
- Connection reuse within a configurable window
- Circuit breaker: after N consecutive failures, short-circuit for cooldown period
- SSRF prevention: upstream allowlist + IP rebinding checks after DNS resolution

### VDP Integration

Response body streaming uses Varnish Delivery Processors (VDP):

- Registered as `wasm_body` filter in VCL (`set resp.filters += "wasm_body"`)
- Delivers body chunks to `proxy_on_response_body` callback
- Streaming: does not buffer entire response in memory

## Thread Safety

- **Engine**: single Wasmtime engine shared across all threads (immutable after init)
- **Store pool**: lock-free checkout via atomic operations
- **Shared data**: reader-writer lock (RWLock) per hash bucket
- **Metrics**: atomic u64 counters; RWLock for metric definition
- **HTTP pool**: per-thread curl multi-handle (no cross-thread sharing)

## File Map

| File | Responsibility |
|------|---------------|
| `vmod_wasm.c` | VCL interface (all `wasm.*` functions) |
| `vmod_wasm.vcc` | VMOD function signatures (vmodtool input) |
| `wasm_engine.c/h` | Wasmtime engine, module compilation, config |
| `store_pool.c/h` | Instance pool management |
| `host_functions.c/h` | Raw host functions (env + WASI namespaces) |
| `proxy_wasm.c/h` | Proxy-Wasm ABI lifecycle orchestration |
| `proxy_wasm_headers.c` | Header map operations |
| `proxy_wasm_http.c` | HTTP callout dispatch and callbacks |
| `proxy_wasm_properties.c` | Property get/set |
| `proxy_wasm_shared.c/h` | Shared data, queues, metrics |
| `proxy_wasm_mem.h` | Memory allocation helpers |
| `http_pool.c/h` | HTTP connection pool with circuit breaker |
| `vdp_wasm.c/h` | VDP filter for response body delivery |
| `compat.h` | Platform compatibility macros |
| `wasi_types.h` | WASI type definitions |
