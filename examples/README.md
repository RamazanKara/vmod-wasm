# Wasm Module Examples

This directory is a Cargo workspace for the Rust/Wasm modules used to exercise
vmod-wasm. The examples are intentionally small and focused: each one teaches a
different integration point without asking readers to understand the whole VMOD
at once.

## Module Map

- [`rust/`](rust/) uses native vmod-wasm host functions plus a small raw
  Proxy-Wasm ABI filter. Read it when you want low-level imports or execution
  limit tests.
- [`proxy-wasm-filter/`](proxy-wasm-filter/) uses the Rust `proxy-wasm` SDK.
  Read it first if you want the normal SDK shape.
- [`passthrough/`](passthrough/) is a no-op raw Proxy-Wasm ABI module for
  lifecycle smoke tests and overhead checks.
- [`transform/`](transform/) is a minimal response-header mutation using direct
  Proxy-Wasm ABI imports.
- [`edge-security-filter/`](edge-security-filter/) is the realistic fixture:
  config, metrics, shared data, callouts, body callbacks, and local responses.

For production edge security deployments, use the standalone
[vmod-wasm Edge Security Filter](https://github.com/RamazanKara/vmod-wasm-edge-security-filter)
repository and its release assets. The in-tree `edge-security-filter/` module is
kept here so vmod-wasm can test a realistic Proxy-Wasm workload without pulling
from another repository during CI.

## Build

Install the Rust Wasm target once:

```bash
rustup target add wasm32-unknown-unknown
```

Build every module from this workspace:

```bash
cd examples
cargo build --release --target wasm32-unknown-unknown
```

Build one module:

```bash
cargo build --release --target wasm32-unknown-unknown -p proxy-wasm-filter
```

Release artifacts are written under:

```text
examples/target/wasm32-unknown-unknown/release/
```

Common artifact names:

| Package | Wasm artifact |
|---------|---------------|
| `test-module` | `test_module.wasm` |
| `proxy-wasm-filter` | `proxy_wasm_filter.wasm` |
| `passthrough` | `passthrough.wasm` |
| `transform` | `transform.wasm` |
| `edge-security-filter` | `edge_security_filter.wasm` |

## Run From VCL

Raw exported-function modules are called with `wasm.execute(...)`:

```vcl
import wasm;

sub vcl_init {
    wasm.load("test", "/path/to/test_module.wasm");
}

sub vcl_recv {
    if (wasm.execute("test", "block_bad_bot") == 403) {
        return (synth(403, "Blocked"));
    }
}
```

Proxy-Wasm modules use the request and response lifecycle helpers:

```vcl
import wasm;
import std;

sub vcl_init {
    wasm.load("filter", "/path/to/proxy_wasm_filter.wasm");
    wasm.set_epoch_deadline(100);
    wasm.set_memory_limit(8388608);
    wasm.set_fail_mode("closed");
}

sub vcl_recv {
    set req.http.X-Wasm-Action = wasm.proxy_wasm_on_request("filter");
    if (req.http.X-Wasm-Action != "0") {
        return (synth(std.integer(req.http.X-Wasm-Action, 403), "Blocked"));
    }
}

sub vcl_deliver {
    set resp.http.X-Wasm-Action = wasm.proxy_wasm_on_response("filter");
}
```

## Test

From the repository root:

```bash
make test
```

Or directly in Docker:

```bash
docker build -t vmod-wasm-ci .
docker run --rm vmod-wasm-ci make check
```

That path builds vmod-wasm, compiles the example modules, and runs the VTC
integration tests that use them.

## Reading Order

Start with `passthrough/` if you want to understand the bare callback shape.
Read `transform/` next for one useful mutation. Move to `proxy-wasm-filter/` for
the SDK version of the same lifecycle. Use `rust/` when you need raw host
function details. Read `edge-security-filter/` last; it is the intentionally
busy fixture that proves the host can run a realistic filter.
