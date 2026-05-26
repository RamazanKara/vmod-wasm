# Proxy-Wasm SDK Filter

This is the readable starter filter for vmod-wasm. It uses the Rust
`proxy-wasm` SDK instead of hand-writing raw ABI imports.

Read this module when you want to see the normal SDK shape: a root context, per
request HTTP contexts, request and response header callbacks, local responses,
response body access, and logging.

## Behavior

- Logs request and response processing.
- Blocks requests whose `User-Agent` contains `BadBot`.
- Adds `X-Wasm-SDK: request-processed` to requests that continue.
- Adds `X-Wasm-SDK-Response: processed` to responses.
- Logs response body size.
- Rewrites a response body exactly equal to `rewrite-me` to `rewrote-it` when
  the host delivers response body bytes to the callback.

## Build

From `examples/`:

```bash
cargo build --release --target wasm32-unknown-unknown -p proxy-wasm-filter
```

Artifact:

```text
examples/target/wasm32-unknown-unknown/release/proxy_wasm_filter.wasm
```

## VCL Sketch

```vcl
import wasm;
import std;

sub vcl_init {
    wasm.load("sdk", "/path/to/proxy_wasm_filter.wasm");
    wasm.set_epoch_deadline(100);
    wasm.set_memory_limit(8388608);
    wasm.set_fail_mode("closed");
}

sub vcl_recv {
    set req.http.X-Wasm-Action = wasm.proxy_wasm_on_request("sdk");
    if (req.http.X-Wasm-Action != "0") {
        return (synth(std.integer(req.http.X-Wasm-Action, 403), "Blocked"));
    }
}

sub vcl_deliver {
    set resp.http.X-Wasm-Action = wasm.proxy_wasm_on_response("sdk");
}
```

## Notes

This module is intentionally small enough to read in one sitting. For a broader
stress test of the Proxy-Wasm host surface, read `../edge-security-filter/`.
