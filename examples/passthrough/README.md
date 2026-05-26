# Passthrough Proxy-Wasm Module

This is the smallest useful raw Proxy-Wasm lifecycle module in the workspace.
Every callback returns `0`, which means continue.

It exists as a baseline. When you need to check that vmod-wasm can load a
module, create contexts, call request and response callbacks, and return without
modifying traffic, this is the module to use.

## What It Demonstrates

- `proxy_on_memory_allocate`
- Context creation and finalization
- Request header and body callbacks
- Response header and body callbacks
- A no-op `Action::Continue` path through the raw ABI

## Build

From `examples/`:

```bash
cargo build --release --target wasm32-unknown-unknown -p passthrough
```

Artifact:

```text
examples/target/wasm32-unknown-unknown/release/passthrough.wasm
```

## VCL Sketch

```vcl
import wasm;

sub vcl_init {
    wasm.load("pass", "/path/to/passthrough.wasm");
}

sub vcl_recv {
    set req.http.X-Wasm-Action = wasm.proxy_wasm_on_request("pass");
}

sub vcl_deliver {
    set resp.http.X-Wasm-Action = wasm.proxy_wasm_on_response("pass");
}
```

Expected result: both actions return `0` and traffic is unchanged.

## Notes

Use this module for lifecycle smoke tests and overhead comparisons. It is not a
template for policy logic; use `../proxy-wasm-filter/` or `../transform/` when
you want an example that changes request or response state.
