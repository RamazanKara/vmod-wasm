# Examples

## Prerequisites

- [Rust toolchain](https://rustup.rs/) with the `wasm32-unknown-unknown` target:

```bash
rustup target add wasm32-unknown-unknown
```

## Building

### Raw Host-Function Module (`rust/`)

A minimal module using vmod-wasm's native host functions (`get_request_header`, `set_response_header`, etc.):

```bash
cd rust
cargo build --release --target wasm32-unknown-unknown
```

Output: `target/wasm32-unknown-unknown/release/test_module.wasm`

### Proxy-Wasm Filter (`proxy-wasm-filter/`)

A Proxy-Wasm ABI v0.2 filter using the [`proxy-wasm`](https://crates.io/crates/proxy-wasm) SDK:

```bash
cd proxy-wasm-filter
cargo build --release --target wasm32-unknown-unknown
```

Output: `target/wasm32-unknown-unknown/release/proxy_wasm_filter.wasm`

## Loading in VCL

```vcl
import wasm;

sub vcl_init {
    # Raw host-function module
    wasm.load("test", "/path/to/test_module.wasm");

    # Proxy-Wasm filter
    wasm.load("waf", "/path/to/proxy_wasm_filter.wasm");
    wasm.set_fuel(1000000);
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
docker build -t vmod-wasm-dev .
docker run --rm vmod-wasm-dev make check
```

This builds vmod-wasm, compiles the example modules, and runs all integration tests.
