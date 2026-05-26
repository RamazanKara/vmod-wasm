# Raw Rust Host-Function Module

This crate is the low-level Rust module used to test vmod-wasm host functions
and execution controls. It is deliberately direct: no SDK, no framework, just
exported Wasm functions and imports provided by the VMOD.

Read this module when you want to understand how vmod-wasm exposes native host
functions such as request header lookup, URL/method lookup, client IP lookup,
response header mutation, logging, memory limits, and epoch deadlines.

## What It Demonstrates

- Plain exported functions callable through `wasm.execute(...)`
- Native vmod-wasm host functions:
  - `get_request_header`
  - `get_request_url`
  - `get_request_method`
  - `get_client_ip`
  - `set_response_header`
  - `log_msg`
- Safety behavior for infinite loops and memory growth
- A small raw Proxy-Wasm request/response filter without the Rust SDK

## Useful Exports

| Export | Purpose |
|--------|---------|
| `get_constant` | Smallest possible Wasm call; returns `42` |
| `add_numbers` | Basic arithmetic sanity check |
| `on_request_allow` | Returns `0`, the vmod-wasm allow convention |
| `on_request_block` | Returns `403`, useful for VCL block-path tests |
| `echo_user_agent` | Copies `User-Agent` to `X-Wasm-UA` |
| `echo_url` | Copies the request URL to `X-Wasm-URL` |
| `echo_method` | Copies the request method to `X-Wasm-Method` |
| `echo_client_ip` | Copies the client IP to `X-Wasm-ClientIP` |
| `test_logging` | Emits a log line through the host logger |
| `block_bad_bot` | Returns `403` when `User-Agent` contains `BadBot` |
| `infinite_loop` | Exercises epoch deadline interruption |
| `grow_memory` | Exercises the VMOD memory limiter |
| `compute_sum` | Normal bounded compute workload |
| `proxy_on_request_headers` | Raw Proxy-Wasm request callback |
| `proxy_on_response_headers` | Raw Proxy-Wasm response callback |

## Build

From `examples/`:

```bash
cargo build --release --target wasm32-unknown-unknown -p test-module
```

Artifact:

```text
examples/target/wasm32-unknown-unknown/release/test_module.wasm
```

## VCL Sketch

```vcl
import wasm;

sub vcl_init {
    wasm.load("test", "/path/to/test_module.wasm");
    wasm.set_epoch_deadline(100);
    wasm.set_memory_limit(8388608);
}

sub vcl_recv {
    if (wasm.execute("test", "block_bad_bot") == 403) {
        return (synth(403, "Blocked"));
    }
}

sub vcl_deliver {
    set resp.http.X-Wasm-UA-Len = wasm.execute("test", "echo_user_agent");
}
```

## Notes

This module is for host API coverage and regression testing. For a normal
Proxy-Wasm filter, start with `../proxy-wasm-filter/`. For production edge
security, use the standalone edge-security-filter repository.
