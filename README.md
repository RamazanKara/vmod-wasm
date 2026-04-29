# vmod-wasm

A Varnish VMOD that executes WebAssembly (Wasm) modules for request/response processing at the edge.

[![License: BSD-2-Clause](https://img.shields.io/badge/license-BSD--2--Clause-blue.svg)](LICENSE)
[![CI](https://github.com/RamazanKara/vmod-wasm/actions/workflows/ci.yml/badge.svg)](https://github.com/RamazanKara/vmod-wasm/actions)

## Overview

vmod-wasm embeds the [Wasmtime](https://wasmtime.dev/) runtime into Varnish Cache, allowing you to write edge logic in **Rust**, **Go**, or **AssemblyScript**, compile to WebAssembly, and execute it during request processing — without writing C or modifying VCL beyond a few function calls.

It includes a [Proxy-Wasm ABI](https://github.com/proxy-wasm/spec) compatibility layer, so Wasm filters that follow the Proxy-Wasm standard can run on Varnish.

## Status

**v0.1.0** — Feature-complete for HTTP request filtering. All 12 VTC tests pass.

| Phase | Description | Status |
|-------|-------------|--------|
| 1 — Basic execution | Load & call Wasm functions from VCL | ✅ Done |
| 2 — Host functions | Read headers/URL/method/IP, set response headers, log to VSL | ✅ Done |
| 3 — Execution safety | Fuel limits, memory limits, trap/error reporting | ✅ Done |
| 4 — Proxy-Wasm ABI | Proxy-Wasm lifecycle, header maps, send_local_response | ✅ Done |

## Features

- **Load `.wasm` modules** at VCL init (compiled once, instantiated per-request)
- **Call exported Wasm functions** from VCL and use the return value
- **6 host functions** for request inspection: `get_request_header`, `get_request_url`, `get_request_method`, `get_client_ip`, `set_response_header`, `log_msg`
- **Fuel-based execution limits** — prevent runaway or infinite-loop modules
- **Memory limits** — cap linear memory growth (default 16 MiB)
- **Trap & error reporting** — Wasm trap messages logged to VSL
- **Proxy-Wasm ABI** — 10 host functions implementing the core Proxy-Wasm lifecycle for HTTP request filtering

## Quick Start

### Simple Wasm execution

```vcl
import wasm;

sub vcl_init {
    wasm.load("my_filter", "/etc/varnish/wasm/filter.wasm");
    wasm.set_fuel(1000000);          # instruction limit
    wasm.set_memory_limit(8388608);  # 8 MiB
}

sub vcl_recv {
    # Call an exported function — returns its i32 result as a string
    if (wasm.execute("my_filter", "on_request") == 403) {
        return (synth(403, "Blocked"));
    }
}
```

### Proxy-Wasm filter

```vcl
import wasm;

sub vcl_init {
    wasm.load("waf", "/etc/varnish/wasm/waf_filter.wasm");
    wasm.set_fuel(10000000);
}

sub vcl_recv {
    # Runs the full Proxy-Wasm lifecycle (context_create → vm_start →
    # configure → request_headers). Returns 0=allow, >0=HTTP status, -1=error
    set req.http.X-Wasm-Result = wasm.proxy_wasm_on_request("waf");
    if (req.http.X-Wasm-Result != "0") {
        return (synth(403, "Blocked by WAF"));
    }
}
```

## VCL Functions

| Function | Context | Description |
|----------|---------|-------------|
| `wasm.load(STRING name, STRING path)` | `vcl_init` | Load a `.wasm` module and register it by name |
| `wasm.execute(STRING module, STRING func)` | any | Call an exported function, return its i32 result as a string |
| `wasm.proxy_wasm_on_request(STRING module)` | `vcl_recv` | Execute a Proxy-Wasm filter lifecycle for the current request |
| `wasm.set_fuel(INT fuel)` | `vcl_init` | Set the maximum fuel (instruction count) per execution |
| `wasm.set_memory_limit(INT bytes)` | `vcl_init` | Set the maximum linear memory size in bytes |
| `wasm.get_fuel()` | any | Return the current fuel limit |
| `wasm.get_memory_limit()` | any | Return the current memory limit |
| `wasm.version()` | any | Return the VMOD version string |

## Host Functions (available to Wasm modules)

### Basic host functions (Phase 2)

Registered under the `env` namespace:

| Function | Description |
|----------|-------------|
| `get_request_header(name_ptr, name_len, buf_ptr, buf_len) → i32` | Read a request header into the buffer, returns length |
| `get_request_url(buf_ptr, buf_len) → i32` | Read the request URL |
| `get_request_method(buf_ptr, buf_len) → i32` | Read the request method (GET, POST, etc.) |
| `get_client_ip(buf_ptr, buf_len) → i32` | Read the client IP address |
| `set_response_header(name_ptr, name_len, val_ptr, val_len) → i32` | Set a response header |
| `log_msg(level, msg_ptr, msg_len)` | Log a message to VSL (0=debug, 1=info, 2=warn) |

### Proxy-Wasm host functions (Phase 4)

Also under `env` namespace, following the [Proxy-Wasm ABI spec](https://github.com/proxy-wasm/spec):

| Function | Description |
|----------|-------------|
| `proxy_log(level, msg_data, msg_size)` | Log to VSL with Proxy-Wasm log levels |
| `proxy_get_header_map_value(map, key_data, key_size, ret_data, ret_size)` | Read a header by name |
| `proxy_add_header_map_value(map, key_data, key_size, val_data, val_size)` | Add/set a header |
| `proxy_replace_header_map_value(...)` | Replace a header value |
| `proxy_remove_header_map_value(map, key_data, key_size)` | Remove a header |
| `proxy_get_property(path_data, path_size, ret_data, ret_size)` | Get request properties (path, method, protocol) |
| `proxy_send_local_response(status, ...)` | Send an immediate response (e.g. 403) |
| `proxy_get_current_time_nanoseconds(ret_time)` | Current time in nanoseconds |
| `proxy_set_effective_context(context_id)` | Switch context (stub) |
| `proxy_get_buffer_bytes(buffer_type, start, max, ret_data, ret_size)` | Read buffer data (stub) |

## Writing Wasm Modules

### Simple filter (Rust)

```rust
// Compile with: cargo build --target wasm32-unknown-unknown --release

extern "C" {
    fn get_request_header(name_ptr: *const u8, name_len: i32,
                          buf_ptr: *mut u8, buf_len: i32) -> i32;
    fn log_msg(level: i32, msg_ptr: *const u8, msg_len: i32);
}

#[no_mangle]
pub extern "C" fn on_request() -> i32 {
    let name = b"User-Agent";
    let mut buf = [0u8; 512];
    let len = unsafe {
        get_request_header(name.as_ptr(), name.len() as i32,
                           buf.as_mut_ptr(), buf.len() as i32)
    };
    // Check for bad bots, return 403 to block or 0 to allow
    0
}
```

### Proxy-Wasm filter (raw ABI)

```rust
// Compile with: cargo build --target wasm32-unknown-unknown --release
// Module must export: proxy_on_context_create, proxy_on_vm_start,
//   proxy_on_configure, proxy_on_request_headers, proxy_on_memory_allocate

extern "C" {
    fn proxy_log(level: i32, msg_data: i32, msg_size: i32) -> i32;
    fn proxy_get_header_map_value(map: i32, key_data: i32, key_size: i32,
                                  ret_data: i32, ret_size: i32) -> i32;
    fn proxy_send_local_response(status: i32, details_data: i32,
        details_size: i32, body_data: i32, body_size: i32,
        headers_data: i32, headers_size: i32, grpc_status: i32) -> i32;
}

#[no_mangle]
pub extern "C" fn proxy_on_request_headers(_ctx: i32, _n: i32, _eos: i32) -> i32 {
    // Inspect headers, block bad requests, add headers, etc.
    0 // CONTINUE
}
```

See [`examples/rust/src/lib.rs`](examples/rust/src/lib.rs) for a complete working example.

## Building from Source

### Prerequisites

- Varnish Cache 7.5+ (with development headers)
- Wasmtime C API v25+ ([releases](https://github.com/bytecodealliance/wasmtime/releases))
- autotools (automake, autoconf, libtool)
- pkg-config
- C compiler (gcc or clang)

### Build

```bash
./autogen.sh
./configure
make
make check   # runs 12 VTC tests
make install
```

### Docker (recommended for development)

```bash
docker build -t vmod-wasm-dev .
docker run --rm vmod-wasm-dev make check
```

## Architecture

```
                       ┌─────────────────────────────┐
                       │         VCL layer            │
                       │  wasm.load / wasm.execute /  │
                       │  wasm.proxy_wasm_on_request  │
                       └──────────────┬──────────────┘
                                      │
                       ┌──────────────▼──────────────┐
                       │       vmod_wasm.c            │
                       │  VCL function dispatch       │
                       └──────────────┬──────────────┘
                                      │
                       ┌──────────────▼──────────────┐
                       │      wasm_engine.c           │
                       │  Wasmtime lifecycle mgmt     │
                       │  • engine_call (Phase 1-3)   │
                       │  • proxy_wasm_call (Phase 4) │
                       └──────┬──────────────┬───────┘
                              │              │
               ┌──────────────▼──┐   ┌───────▼──────────────┐
               │ host_functions.c│   │   proxy_wasm.c       │
               │ 6 basic host    │   │ 10 Proxy-Wasm host   │
               │ functions       │   │ functions             │
               └────────┬────────┘   └───────┬──────────────┘
                        │                    │
                        └───────┬────────────┘
                                │
                       ┌────────▼────────┐
                       │  Wasmtime C API │
                       │  (libwasmtime)  │
                       └────────┬────────┘
                                │
                       ┌────────▼────────┐
                       │  .wasm module   │
                       └─────────────────┘
```

## Tests

12 VTC tests covering all functionality:

| Test | Phase | Description |
|------|-------|-------------|
| `basic_load` | 1 | Load module, call `get_constant` and `add_numbers` |
| `allow_decision` | 1 | `on_request_allow` returns 0 |
| `block_decision` | 1 | `on_request_block` returns 403 |
| `error_handling` | 1 | Missing module/function handling |
| `host_header` | 2 | Read User-Agent via `get_request_header` |
| `host_url_method` | 2 | Read URL and method via host functions |
| `host_block_bot` | 2 | Block BadBot via host function header inspection |
| `fuel_exhaustion` | 3 | Infinite loop stopped by fuel limit |
| `memory_limit` | 3 | Memory growth capped by limit |
| `resource_config` | 3 | `set_fuel`/`set_memory_limit`/`get_*` round-trip |
| `proxy_wasm_basic` | 4 | Proxy-Wasm lifecycle with header addition |
| `proxy_wasm_block` | 4 | Proxy-Wasm BadBot blocking via `send_local_response` |

## License

BSD-2-Clause — same as Varnish Cache itself.

## Contributing

Contributions welcome! See the [implementation plan](plan/feature-vmod-wasm-1.md) for background.
