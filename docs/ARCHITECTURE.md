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
│  │ • Response body  │  │   pool (TCP)     │                       │
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
            │    ├─▶ [module calls proxy_http_call] ──▶ proxy_wasm_http.c
            │    │    └─▶ synchronous TCP call; stores response in proxy_ctx
            │    └─▶ returns Action (Continue | Pause)
            ├─▶ [if http_call_pending] ─▶ proxy_on_http_call_response(ctx_id, ...)
            │    └─▶ invoked AFTER on_http_request_headers returns
            │         (deferred to avoid proxy-wasm SDK RefCell re-entrancy)
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

### Per-VCL Engine Lifetime

Each loaded VCL gets its own `vwasm_engine` and module registry. Varnish may keep
old VCLs alive while in-flight requests drain, so vmod-wasm keeps the old
Wasmtime engine, compiled modules, store pools, tick timers, and HTTP pool tied
to that VCL until Varnish sends `VCL_EVENT_DISCARD`.

This mirrors Varnish's normal hot-reload model:

- `VCL_EVENT_LOAD`: create a new engine and register the `wasm_body` VDP filter
- `vcl_init`: load modules and configure limits/pools for that VCL
- `vcl.use`: new traffic moves to the new VCL
- `VCL_EVENT_DISCARD`: unregister the filter, stop timers, destroy pools, then delete Wasmtime objects

No Wasmtime engine is shared between different loaded VCLs. That avoids
cross-VCL lifetime bugs and lets a reload compile a new module version without
mutating the module set used by older traffic.

### Store Pooling

Instead of creating a new Wasmtime instance per request, vmod-wasm pre-creates a pool of instances at VCL init time. This eliminates compilation latency from the request path.

- Default pool size: 8 stores per module
- Configurable with `wasm.set_store_pool_size(module, size)` after `wasm.load()`
- Valid range: 1-256 stores per module
- Checkout: O(1) via atomic counter
- Return: instance memory is restored from the warm snapshot before reuse
- Modules exporting `_initialize` bypass store pooling because mutable Wasm
  globals and VM state cannot be reset from a linear-memory snapshot alone

If no warm store is available, the request falls back to a fresh Wasmtime store
and instance. The pool is an optimization, not a correctness dependency.

### Epoch-Based Time Limits

Unlike fuel-based metering (which adds per-instruction overhead), epoch interruption uses a background thread that increments a global epoch counter. When a Wasm execution exceeds its deadline, the next epoch check traps the execution.

- Zero overhead during normal execution
- Background ticker thread (1ms resolution)
- Configurable per VCL load via `wasm.set_epoch_deadline(ms)`
- Default deadline: 5000ms; production filters should set an explicit lower value

### HTTP Connection Pooling

Proxy-Wasm HTTP callouts (`proxy_http_call`) reuse connections via an internal pool:

- Blocking TCP sockets from the Varnish worker thread executing the filter
- Default pool size: 16 persistent connections; configurable with `wasm.set_http_pool_size(size)`
- Circuit breaker: after N consecutive failures, short-circuit for cooldown period
- SSRF prevention: upstream allowlist plus private/internal IP checks for non-allowlisted destinations after DNS resolution

### Deferred HTTP Callout Callback

The proxy-wasm Rust SDK uses `RefCell` to manage the active context.
Calling `proxy_on_http_call_response` from _within_ `proxy_on_http_request_headers`
would cause a second mutable borrow of that `RefCell` — a runtime panic.

vmod-wasm avoids this by making `proxy_http_call` synchronous (blocking the thread
until the upstream responds) and deferring `proxy_on_http_call_response` until
_after_ `proxy_on_http_request_headers` has returned and released its borrow:

```
proxy_on_request_headers()   ← RefCell borrowed here
  └─▶ proxy_http_call()      ← TCP call blocks; response stored in proxy_ctx
  └─▶ returns Action::Pause  ← RefCell released

[host sees http_call_pending = true]
proxy_on_http_call_response() ← called now, RefCell is free
  └─▶ sends 401 / sets Action::Continue
```

This matches the expected proxy-wasm programming model: the module pauses the
request in `on_http_request_headers` and the runtime calls back with the result.

### VDP Integration

Response body streaming uses Varnish Delivery Processors (VDP):

- Registered as `wasm_body` filter in VCL (`set resp.filters += "wasm_body"`)
- Delivers body chunks to `proxy_on_response_body` callback
- Streaming: does not buffer entire response in memory

## Thread Safety

- **VCL registry**: protected by a process-wide mutex when mapping `ctx->vcl` to the correct engine
- **Engine**: one Wasmtime engine per loaded VCL, immutable after `vcl_init`
- **Modules**: compiled once during VCL load/init and never mutated on the request path
- **Store pool**: per-module lock-free checkout via atomic operations
- **Shared data**: reader-writer lock (RWLock) per hash bucket
- **Metrics**: atomic u64 counters; RWLock for metric definition
- **HTTP pool**: shared pool protected by internal locks; pooled connections are reused only after clean idle return
- **Teardown**: tick timers and pools are stopped before Wasmtime modules, linker, and engine are deleted

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
