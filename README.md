# vmod-wasm

A Varnish VMOD that executes WebAssembly modules for HTTP request processing at the edge.

[![License: CC BY-NC 4.0](https://img.shields.io/badge/license-CC%20BY--NC%204.0-lightgrey.svg)](LICENSE)
[![CI](https://github.com/RamazanKara/vmod-wasm/actions/workflows/ci.yml/badge.svg)](https://github.com/RamazanKara/vmod-wasm/actions)

## Overview

vmod-wasm embeds the [Wasmtime](https://wasmtime.dev/) runtime into Varnish Cache, allowing you to write edge logic in **Rust**, **Go**, or **AssemblyScript**, compile to WebAssembly, and execute it during request processing.

Includes a [Proxy-Wasm ABI](https://github.com/proxy-wasm/spec) compatibility layer for running standard Wasm filters on Varnish.

## Features

- Load `.wasm` modules at VCL init time
- Call exported Wasm functions from VCL
- Host functions for request inspection (headers, URL, method, client IP)
- Set response headers and log to VSL from Wasm
- Fuel-based execution limits
- Memory limits (default 16 MiB)
- Proxy-Wasm ABI support (header maps, local response, properties)

## Quick Start

```vcl
import wasm;

sub vcl_init {
    wasm.load("my_filter", "/etc/varnish/wasm/filter.wasm");
    wasm.set_fuel(1000000);
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
```

## VCL Functions

| Function | Description |
|----------|-------------|
| `wasm.load(name, path)` | Load a `.wasm` module |
| `wasm.execute(module, func)` | Call an exported function, returns i32 as string |
| `wasm.proxy_wasm_on_request(module)` | Run Proxy-Wasm filter lifecycle (0=allow, >0=status, -1=error) |
| `wasm.set_fuel(fuel)` | Set instruction limit per execution |
| `wasm.set_memory_limit(bytes)` | Set max linear memory |
| `wasm.get_fuel()` | Return current fuel limit |
| `wasm.get_memory_limit()` | Return current memory limit |
| `wasm.version()` | Return VMOD version |

## Host Functions

Registered under the `env` namespace, available to all Wasm modules:

**Request inspection:** `get_request_header`, `get_request_url`, `get_request_method`, `get_client_ip`, `set_response_header`, `log_msg`

**Proxy-Wasm ABI:** `proxy_log`, `proxy_get_header_map_value`, `proxy_add_header_map_value`, `proxy_replace_header_map_value`, `proxy_remove_header_map_value`, `proxy_get_property`, `proxy_send_local_response`, `proxy_get_current_time_nanoseconds`, `proxy_set_effective_context`, `proxy_get_buffer_bytes`

## Writing Wasm Modules

```rust
// Compile with: cargo build --target wasm32-unknown-unknown --release

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

See [`examples/rust/src/lib.rs`](examples/rust/src/lib.rs) for a complete example including Proxy-Wasm ABI usage.

## Building

### Prerequisites

- Varnish Cache 7.5+ (with dev headers)
- Wasmtime C API v25+
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
VCL → vmod_wasm.c → wasm_engine.c → Wasmtime C API → .wasm module
                          ↕
              host_functions.c / proxy_wasm.c
```

## License

CC BY-NC 4.0 — see [LICENSE](LICENSE). Non-commercial use only.

## Contributing

Contributions welcome.
