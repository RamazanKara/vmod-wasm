# Transform Proxy-Wasm Module

This raw Proxy-Wasm module adds one response header:

```http
X-Transform: applied
```

It is the smallest example that mutates response headers without using the Rust
`proxy-wasm` SDK.

## What It Demonstrates

- Raw `proxy_add_header_map_value`
- Response header map type `2`
- Manual memory allocation through `proxy_on_memory_allocate`
- Pass-through request callbacks with a response-side mutation

## Build

From `examples/`:

```bash
cargo build --release --target wasm32-unknown-unknown -p transform
```

Artifact:

```text
examples/target/wasm32-unknown-unknown/release/transform.wasm
```

## VCL Sketch

```vcl
import wasm;

sub vcl_init {
    wasm.load("transform", "/path/to/transform.wasm");
}

sub vcl_recv {
    set req.http.X-Wasm-Action = wasm.proxy_wasm_on_request("transform");
}

sub vcl_deliver {
    set resp.http.X-Wasm-Action = wasm.proxy_wasm_on_response("transform");
}
```

Expected response marker:

```http
X-Transform: applied
```

## Notes

This is intentionally raw and compact. It is useful when debugging the ABI
surface itself. If you want idiomatic Rust filter code, compare it with
`../proxy-wasm-filter/`.
