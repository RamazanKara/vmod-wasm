# vmod-wasm

A Varnish VMOD that executes WebAssembly modules for HTTP request processing at the edge.

[![License: BSD-2-Clause](https://img.shields.io/badge/license-BSD--2--Clause-blue.svg)](LICENSE)
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
| `wasm.set_fail_mode(mode)` | "closed" or "open" on error |
| `wasm.set_store_pool_size(module, size)` | Pre-warmed store count for a module |
| `wasm.set_http_pool_size(size)` | Max persistent HTTP connections |
| `wasm.filter_chain(chain)` | Run a request-side module chain |
| `wasm.filter_chain_response(chain)` | Run a response-side module chain |
| `wasm.get_metrics_json()` | Return Proxy-Wasm metrics as JSON |
| `wasm.get_stats_json()` | Return execution statistics as JSON |
| `wasm.get_pool_stats_json(module)` | Return store pool stats for a module |
| `wasm.get_http_pool_stats_json()` | Return HTTP pool stats |

## Writing Wasm Modules

See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) for a complete guide and the
[`examples/`](examples/) directory for working modules including the
[edge-security-filter](examples/edge-security-filter/).

For host function signatures and Proxy-Wasm ABI coverage, see
[docs/COMPATIBILITY.md](docs/COMPATIBILITY.md).

## Building

### Prerequisites

- Varnish Cache 9.x (with varnishapi dev headers)
- Wasmtime C API 44.0.0 (libwasmtime)
- autotools, pkg-config, C compiler

### Build

```bash
./autogen.sh
./configure
make
make check
make install
```

### Release Bundles

GitHub releases use Varnish-specific tags such as `varnish9-v4.3.3` so the
supported Varnish ABI line is visible before download. The package version
remains semantic (`4.3.3`), while the release channel identifies Varnish 9.

Release assets include source and convenience binary bundles for Linux `amd64`
and `arm64` on Varnish 9. Binary bundles include `libvmod_wasm.so`,
`libwasmtime.so`, notices, checksums, and an install note. Source builds remain
the authoritative path for custom Varnish installations.

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

BSD-2-Clause — see [LICENSE](LICENSE).

## Contributing

Contributions welcome. See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.
