# vmod-wasm

A Varnish VMOD that executes WebAssembly modules for HTTP request processing at the edge.

[![License: CC BY-NC 4.0](https://img.shields.io/badge/license-CC%20BY--NC%204.0-lightgrey.svg)](LICENSE)
[![CI](https://github.com/RamazanKara/vmod-wasm/actions/workflows/ci.yml/badge.svg)](https://github.com/RamazanKara/vmod-wasm/actions)
![Wasmtime](https://img.shields.io/badge/Wasmtime-v44.0.0-blue)
![Varnish](https://img.shields.io/badge/Varnish-9.0%2B-purple)
![Proxy-Wasm ABI](https://img.shields.io/badge/Proxy--Wasm%20ABI-v0.2.1-green)


## Overview

vmod-wasm embeds the [Wasmtime](https://wasmtime.dev/) runtime into Varnish Cache, allowing you to write edge logic in **Rust**, **Go**, or **AssemblyScript**, compile to WebAssembly, and execute it during request processing.

Includes a full [Proxy-Wasm ABI v0.2.1](https://github.com/proxy-wasm/spec) implementation for running standard Wasm filters on Varnish.

## Features

- Load `.wasm` modules at VCL init time
- Call exported Wasm functions from VCL
- Full Proxy-Wasm ABI v0.2.1 (header maps, trailers, buffers, HTTP callouts, properties, shared data, metrics, tick timer, stream control)
- Deferred `proxy_on_http_call_response` callback — invoked after `on_http_request_headers` returns (avoids proxy-wasm SDK re-entrancy)
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

## Writing Wasm Modules

See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) for a complete guide and the
[`examples/`](examples/) directory for working modules including the
[edge-security-filter](examples/edge-security-filter/).

For host function signatures and Proxy-Wasm ABI coverage, see
[docs/COMPATIBILITY.md](docs/COMPATIBILITY.md).

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

## Documentation

- [Development Guide](docs/DEVELOPMENT.md) — Writing, building, and testing Proxy-Wasm modules
- [Configuration Reference](docs/CONFIGURATION.md) — All VCL functions with parameters
- [Architecture](docs/ARCHITECTURE.md) — Component design and request lifecycle
- [Proxy-Wasm Compatibility](docs/COMPATIBILITY.md) — ABI coverage matrix
- [Security Model](docs/SECURITY.md) — Isolation, threat model, supply chain security
- [Production Guide](docs/PRODUCTION.md) — Deployment, hot-reload, monitoring, capacity planning

## License

CC BY-NC 4.0 — see [LICENSE](LICENSE).

## Contributing

Contributions welcome. See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.
