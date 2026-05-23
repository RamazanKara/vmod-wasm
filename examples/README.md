# Examples

This directory is a [Cargo workspace](https://doc.rust-lang.org/cargo/reference/workspaces.html) containing all Wasm module examples for vmod-wasm.

## Modules

| Module | Description | ABI |
|--------|-------------|-----|
| `rust/` | Minimal module using vmod-wasm native host functions (`get_request_header`, `set_response_header`) | Raw host-function |
| `proxy-wasm-filter/` | Basic Proxy-Wasm SDK filter demonstrating request/response lifecycle | Proxy-Wasm v0.2 |
| `passthrough/` | No-op passthrough module — useful as a baseline for benchmarking | Raw ABI |
| `transform/` | Response header transformation using raw ABI (`proxy_add_header_map_value`) | Raw ABI |
| `edge-security-filter/` | Production-grade security filter: rate limiting, bot detection, geo-blocking, auth callout, metrics | Proxy-Wasm v0.2 |

## Prerequisites

- [Rust toolchain](https://rustup.rs/) with the `wasm32-unknown-unknown` target:

```bash
rustup target add wasm32-unknown-unknown
```

## Building All Modules

From this directory (workspace root):

```bash
cargo build --release --target wasm32-unknown-unknown
```

Output binaries are placed in `target/wasm32-unknown-unknown/release/*.wasm`.

Or from the repo root using the convenience Makefile:

```bash
make build
```

### Building a Single Module

```bash
cargo build --release --target wasm32-unknown-unknown -p edge-security-filter
```

## Loading in VCL

```vcl
import wasm;

sub vcl_init {
    # Raw host-function module
    wasm.load("test", "/path/to/test_module.wasm");

    # Proxy-Wasm filter
    wasm.load("waf", "/path/to/proxy_wasm_filter.wasm");
    wasm.set_epoch_deadline(100);
    wasm.set_memory_limit(8388608);
}

sub vcl_recv {
    # Raw module: call exported function directly
    if (wasm.execute("test", "on_request") == 403) {
        return (synth(403, "Blocked"));
    }

    # Proxy-Wasm: run full request lifecycle
    set req.http.X-Wasm-Result = wasm.proxy_wasm_on_request("waf");
    if (req.http.X-Wasm-Result != "0") {
        return (synth(403, "Blocked by filter"));
    }
}
```

## Testing

From the repo root:

```bash
make test
```

Or directly with Docker:

```bash
docker build -t vmod-wasm-dev .
docker run --rm vmod-wasm-dev make check
```

This builds vmod-wasm, compiles all example modules, and runs the full VTC integration test suite.

## Linting

```bash
make lint
```

Runs `cargo fmt --check` and `cargo clippy` across all workspace members.
