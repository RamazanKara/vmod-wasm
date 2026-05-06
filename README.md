# vmod-wasm

A Varnish VMOD that executes WebAssembly modules for HTTP request processing at the edge.

[![License: CC BY-NC 4.0](https://img.shields.io/badge/license-CC%20BY--NC%204.0-lightgrey.svg)](LICENSE)
[![CI](https://github.com/RamazanKara/vmod-wasm/actions/workflows/ci.yml/badge.svg)](https://github.com/RamazanKara/vmod-wasm/actions)

## Overview

vmod-wasm embeds the [Wasmtime](https://wasmtime.dev/) runtime into Varnish Cache, allowing you to write edge logic in **Rust**, **Go**, or **AssemblyScript**, compile to WebAssembly, and execute it during request processing.

Includes a full [Proxy-Wasm ABI v0.2.1](https://github.com/proxy-wasm/spec) implementation for running standard Wasm filters on Varnish.

## Features

- Load `.wasm` modules at VCL init time
- Call exported Wasm functions from VCL
- Full Proxy-Wasm ABI v0.2.1 (header maps, trailers, buffers, HTTP callouts, properties, shared data, metrics, tick timer, stream control)
- WASI support (`fd_write`, `clock_time_get`, `random_get` with real implementations)
- Epoch-based execution time limits (low-overhead, no per-instruction cost)
- Memory limits (default 16 MiB)
- Store pooling for fast per-request instantiation
- HTTP connection pooling with circuit breaker for outbound calls
- Streaming response body inspection via VDP
- SSRF prevention (upstream allowlist + IP rebinding protection)

## Quick Start

```vcl
import wasm;

sub vcl_init {
    wasm.load("my_filter", "/etc/varnish/wasm/filter.wasm");
    wasm.set_epoch_deadline(100);    # 100ms execution limit
    wasm.set_memory_limit(8388608);  # 8 MiB
}

sub vcl_recv {
    if (wasm.execute("my_filter", "on_request") == 403) {
        return (synth(403, "Blocked"));
    }
}
```

### Proxy-Wasm

```vcl
sub vcl_recv {
    set req.http.X-Wasm-Result = wasm.proxy_wasm_on_request("waf");
    if (req.http.X-Wasm-Result != "0") {
        return (synth(403, "Blocked"));
    }
}

sub vcl_deliver {
    set resp.http.X-Wasm-Action = wasm.proxy_wasm_on_response("waf");
    set resp.filters += "wasm_body";
}
```

## VCL Functions

| Function | Description |
|----------|-------------|
| `wasm.load(name, path)` | Load and compile a .wasm module |
| `wasm.execute(module, func)` | Call an exported function |
| `wasm.version()` | Return vmod-wasm version string |
| `wasm.set_epoch_deadline(ms)` | Max wall-clock time per execution |
| `wasm.get_epoch_deadline()` | Return current deadline |
| `wasm.set_memory_limit(bytes)` | Max Wasm linear memory |
| `wasm.get_memory_limit()` | Return current memory limit |
| `wasm.proxy_wasm_on_request(module)` | Run Proxy-Wasm request lifecycle |
| `wasm.proxy_wasm_on_response(module)` | Run Proxy-Wasm response lifecycle |
| `wasm.proxy_wasm_on_request_configured(module, vm, plugin)` | Request lifecycle with config |
| `wasm.proxy_wasm_on_response_configured(module, vm, plugin)` | Response lifecycle with config |
| `wasm.set_allowed_upstreams(list)` | Upstream allowlist (SSRF prevention) |
| `wasm.set_http_call_limit(limit)` | Max HTTP callouts per request |
| `wasm.set_http_timeout(ms)` | Default HTTP callout timeout |
| `wasm.set_fail_mode(mode)` | "closed" or "open" on error |
| `wasm.get_metrics_json()` | Return Proxy-Wasm metrics as JSON |
| `wasm.get_stats_json()` | Return execution statistics as JSON |

## Host Functions

**`env` namespace** (for non-proxy-wasm modules):

`get_request_header`, `get_request_url`, `get_request_method`, `get_client_ip`, `set_response_header`, `log_msg`

**`wasi_snapshot_preview1` namespace** (WASI stubs for wasm32-wasi modules):

`fd_write`, `clock_time_get`, `random_get`, `environ_sizes_get`, `environ_get`, `args_sizes_get`, `args_get`, `proc_exit`

For the full Proxy-Wasm ABI compatibility matrix, see [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md).

## Writing Wasm Modules

### Rust (wasm32-unknown-unknown)

```rust
extern "C" {
    fn get_request_header(name_ptr: *const u8, name_len: i32,
                          buf_ptr: *mut u8, buf_len: i32) -> i32;
}

#[no_mangle]
pub extern "C" fn on_request() -> i32 {
    // Return 0 to allow, 403 to block
    0
}
```

### Proxy-Wasm SDK (Rust)

```rust
use proxy_wasm::traits::*;
use proxy_wasm::types::*;

proxy_wasm::main! {{
    proxy_wasm::set_http_context(|_, _| -> Box<dyn HttpContext> {
        Box::new(MyFilter)
    });
}}

struct MyFilter;
impl HttpContext for MyFilter {
    fn on_http_request_headers(&mut self, _: usize, _: bool) -> Action {
        if self.get_http_request_header("x-block").is_some() {
            self.send_http_response(403, vec![], Some(b"Blocked"));
            return Action::Pause;
        }
        Action::Continue
    }
}
impl Context for MyFilter {}
```

See [`examples/`](examples/) for complete examples.

## Building

### Prerequisites

- Varnish Cache 8.0+ (with varnishapi dev headers)
- Wasmtime C API (libwasmtime)
- autotools, pkg-config, C compiler

### Build

```bash
./autogen.sh
./configure
make
make check
make install
```

### Docker

```bash
docker build -t vmod-wasm-dev .
docker run --rm vmod-wasm-dev make check
```

## Architecture

```
VCL -> vmod_wasm.c -> wasm_engine.c -> Wasmtime -> .wasm module
                           |
             host_functions.c (env + WASI)
             proxy_wasm.c     (Proxy-Wasm ABI)
             proxy_wasm_http.c (HTTP callouts)
             store_pool.c     (instance pooling)
             http_pool.c      (connection pooling)
             vdp_wasm.c       (response body streaming)
```

## Documentation

- [Proxy-Wasm Compatibility](docs/COMPATIBILITY.md) — ABI coverage matrix
- [Security Model](docs/SECURITY.md) — isolation, threat model, controls
- [Production Guide](docs/PRODUCTION.md) — deployment, monitoring, tuning

## License

CC BY-NC 4.0 — see [LICENSE](LICENSE).

## Contributing

Contributions welcome.
