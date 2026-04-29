# vmod-wasm

A Varnish VMOD that executes WebAssembly (Wasm) modules for request/response processing at the edge.

[![License: BSD-2-Clause](https://img.shields.io/badge/license-BSD--2--Clause-blue.svg)](LICENSE)
[![CI](https://github.com/RamazanKara/vmod-wasm/actions/workflows/ci.yml/badge.svg)](https://github.com/RamazanKara/vmod-wasm/actions)

## Overview

vmod-wasm embeds the [Wasmtime](https://wasmtime.dev/) runtime into Varnish Cache, allowing you to write edge logic in **Rust**, **Go**, or **AssemblyScript**, compile to WebAssembly, and execute it during request processing — without writing C or modifying VCL beyond a few function calls.

The end goal is full [Proxy-Wasm ABI](https://github.com/proxy-wasm/spec) compatibility, so existing Wasm filters written for Envoy or nginx work on Varnish unchanged.

## Status

🚧 **Early development** — Phase 1 (minimal Wasm execution) is in progress.

## Features (Planned)

- Load `.wasm` modules at VCL init time (compiled once, instantiated per-request)
- Call exported Wasm functions from VCL
- Host functions: read request headers, URL, method, client IP
- Host functions: set response headers, log to VSL
- Fuel-based execution limits (prevent runaway modules)
- Memory limits (cap linear memory growth)
- Instance pooling for performance
- Proxy-Wasm ABI v0.2.1 compatibility

## Quick Start

```vcl
import wasm;

sub vcl_init {
    wasm.load("my_filter", "/etc/varnish/wasm/filter.wasm");
}

sub vcl_recv {
    if (wasm.execute("my_filter", "on_request") == 403) {
        return (synth(403, "Blocked"));
    }
}
```

## Building from Source

### Prerequisites

- Varnish Cache 7.4+ (with development headers)
- Wasmtime C API v25+ ([releases](https://github.com/bytecodealliance/wasmtime/releases))
- autotools (automake, autoconf, libtool)
- pkg-config
- C compiler (gcc or clang)

### Build

```bash
./autogen.sh
./configure
make
make check   # runs VTC tests
make install
```

### Docker (recommended for development)

```bash
docker build -t vmod-wasm-dev .
docker run --rm -v $(pwd):/src vmod-wasm-dev make check
```

## Writing Wasm Modules

Example in Rust (using the vmod-wasm guest SDK):

```rust
#[no_mangle]
pub extern "C" fn on_request() -> i32 {
    // Return 0 = allow, 403 = block
    0
}
```

Compile with:
```bash
cargo build --target wasm32-wasi --release
```

## Architecture

```
VCL → vmod_wasm.c → Wasmtime C API → Wasm Module
                  ↕
        host_functions.c (request context bridge)
```

See [plan/feature-vmod-wasm-1.md](plan/feature-vmod-wasm-1.md) for the full implementation plan.

## License

BSD-2-Clause — same as Varnish Cache itself.

## Contributing

Contributions welcome! See the implementation plan for current status and open tasks.
