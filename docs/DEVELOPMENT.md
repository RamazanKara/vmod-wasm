# Development Guide

How to write, build, test, and deploy Proxy-Wasm modules for vmod-wasm.

## Prerequisites

- [Rust toolchain](https://rustup.rs/) (stable)
- `wasm32-unknown-unknown` target: `rustup target add wasm32-unknown-unknown`
- Docker (for running integration tests)
- Varnish Cache 9.x with `varnish-dev` (optional, for local VTC testing)

## Project Structure

```
examples/
├── Cargo.toml              # Workspace root
├── rust/                   # Raw host-function module (testing)
├── proxy-wasm-filter/      # Simple Proxy-Wasm SDK filter
├── passthrough/            # No-op passthrough module
├── transform/              # Header-adding transform module
└── edge-security-filter/   # Production-grade security filter
    ├── Cargo.toml
    ├── config.json         # Example configuration
    ├── README.md
    └── src/
        ├── lib.rs          # Module entry point + HTTP context
        └── config.rs       # Configuration parsing
```

## Quick Start: New Proxy-Wasm Module

### 1. Create a new crate

```bash
cd examples
cargo new --lib my-filter
```

### 2. Configure Cargo.toml

```toml
[package]
name = "my-filter"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["cdylib"]

[dependencies]
proxy-wasm.workspace = true
```

### 3. Add to workspace

Edit `examples/Cargo.toml`:

```toml
[workspace]
members = [
    # ... existing members ...
    "my-filter",
]
```

### 4. Implement the filter

```rust
use proxy_wasm::traits::*;
use proxy_wasm::types::*;

proxy_wasm::main! {{
    proxy_wasm::set_log_level(LogLevel::Info);
    proxy_wasm::set_root_context(|_| -> Box<dyn RootContext> {
        Box::new(MyFilterRoot)
    });
}}

struct MyFilterRoot;

impl Context for MyFilterRoot {}

impl RootContext for MyFilterRoot {
    fn get_type(&self) -> Option<ContextType> {
        Some(ContextType::HttpContext)
    }

    fn create_http_context(&self, _context_id: u32) -> Option<Box<dyn HttpContext>> {
        Some(Box::new(MyFilter))
    }
}

struct MyFilter;

impl Context for MyFilter {}

impl HttpContext for MyFilter {
    fn on_http_request_headers(&mut self, _num_headers: usize, _end_of_stream: bool) -> Action {
        // Your logic here
        self.set_http_request_header("X-My-Filter", Some("processed"));
        Action::Continue
    }

    fn on_http_response_headers(&mut self, _num_headers: usize, _end_of_stream: bool) -> Action {
        self.set_http_response_header("X-My-Filter-Response", Some("done"));
        Action::Continue
    }
}
```

### 5. Build

```bash
cd examples
cargo build --release --target wasm32-unknown-unknown
```

Output: `examples/target/wasm32-unknown-unknown/release/my_filter.wasm`

## Available Proxy-Wasm ABI Functions

All functions from the [Proxy-Wasm ABI v0.2.1](https://github.com/proxy-wasm/spec) are available. Key categories:

### Root Context Callbacks

| Callback | When Called |
|----------|------------|
| `on_vm_start` | Module loaded, before any requests |
| `on_configure` | Plugin configuration provided |
| `on_tick` | Periodic timer (set via `set_tick_period`) |

### HTTP Context Callbacks

| Callback | When Called |
|----------|------------|
| `on_http_request_headers` | Request headers received |
| `on_http_request_body` | Request body chunk received |
| `on_http_response_headers` | Response headers from backend |
| `on_http_response_body` | Response body chunk (via VDP) |
| `on_http_call_response` | HTTP callout response received |

### Host Functions (called by your module)

| Function | Purpose |
|----------|---------|
| `get_http_request_header` | Read a request header |
| `set_http_request_header` | Modify a request header |
| `get_http_response_header` | Read a response header |
| `set_http_response_header` | Add/modify a response header |
| `send_http_response` | Send a direct response (403, 429, etc.) |
| `dispatch_http_call` | Make an outbound HTTP call |
| `get_shared_data` / `set_shared_data` | Shared key-value store (with CAS) |
| `define_metric` / `increment_metric` | Custom metrics |
| `get_current_time` | Current wall-clock time |
| `get_property` | Connection/request metadata |
| `log` | Emit a log message (maps to VSL) |

## HTTP Callouts

Use `dispatch_http_call` to call an upstream service during request processing.
The proxy-wasm SDK requires the module to **pause** the request and handle the
response in `on_http_call_response`.

vmod-wasm makes the callout synchronous (blocks the Varnish thread until the
upstream responds) and then invokes `on_http_call_response` _after_
`on_http_request_headers` returns — avoiding the SDK's `RefCell` re-entrancy panic.

```rust
use proxy_wasm::traits::*;
use proxy_wasm::types::*;

struct MyFilter {
    awaiting_auth: bool,
}

impl Context for MyFilter {
    fn on_http_call_response(
        &mut self,
        _token_id: u32,
        _num_headers: usize,
        _body_size: usize,
        _num_trailers: usize,
    ) {
        let status = self
            .get_http_call_response_header(":status")
            .unwrap_or_default();
        if status == "200" {
            self.resume_http_request();
        } else {
            self.send_http_response(401, vec![], Some(b"Unauthorized"));
        }
        self.awaiting_auth = false;
    }
}

impl HttpContext for MyFilter {
    fn on_http_request_headers(&mut self, _num_headers: usize, _end_of_stream: bool) -> Action {
        if let Some(token) = self.get_http_request_header("authorization") {
            self.dispatch_http_call(
                "auth-backend",          // upstream name in VCL backend config
                vec![
                    (":method", "GET"),
                    (":path", "/validate"),
                    (":authority", "auth.internal"),
                    ("authorization", &token),
                ],
                None,
                vec![],
                Duration::from_secs(5),
            )
            .ok();
            self.awaiting_auth = true;
            return Action::Pause;        // resume happens in on_http_call_response
        }
        Action::Continue
    }
}
```

**VCL configuration** — always set the allowlist:

```vcl
sub vcl_init {
    wasm.load("my_filter", "/etc/varnish/wasm/my_filter.wasm");
    wasm.set_allowed_upstreams("auth.internal:8080");
    wasm.set_http_call_limit(3);
}
```

Modules can receive JSON configuration via two mechanisms:

1. **Default config** — Applied when loaded via `wasm.proxy_wasm_on_request("module")`
2. **Per-request config** — Passed via `wasm.proxy_wasm_on_request_configured("module", vm_config, plugin_config)`

Parse configuration in `RootContext::on_configure()`:

```rust
fn on_configure(&mut self, _size: usize) -> bool {
    if let Some(bytes) = self.get_plugin_configuration() {
        // Parse JSON, YAML, or any format
        self.config = serde_json_wasm::from_slice(&bytes).unwrap_or_default();
    }
    true
}
```

## Testing

### Unit Tests (Rust)

```bash
cd examples
cargo test
```

### Integration Tests (VTC)

VTC (Varnish Test Case) files test the module running inside actual Varnish:

```bash
# Build everything and run tests in Docker
docker build -t vmod-wasm-dev .
docker run --rm vmod-wasm-dev make check
```

### Writing VTC Tests

Create a `.vtc` file in `tests/`:

```
varnishtest "my-filter: adds header"

server s1 {
    rxreq
    expect req.http.X-My-Filter == "processed"
    txresp -status 200
} -start

varnish v1 -arg "-p thread_pool_stack=262144" -vcl+backend {
    import wasm from "${vmod_wasm}";

    sub vcl_init {
        wasm.load("mine", "${my_filter_module}");
    }

    sub vcl_recv {
        set req.http.X-Wasm-Action =
            wasm.proxy_wasm_on_request("mine");
    }
} -start

client c1 {
    txreq -url "/"
    rxresp
    expect resp.status == 200
} -run
```

## Deployment

### VCL Integration

```vcl
import wasm;

sub vcl_init {
    wasm.load("my_filter", "/etc/varnish/wasm/my_filter.wasm");
    wasm.set_epoch_deadline(100);      # 100ms max execution
    wasm.set_memory_limit(8388608);    # 8 MiB max memory
    wasm.set_fail_mode("closed");      # Block on error
}
```

### Production Checklist

- [ ] Set `epoch_deadline` appropriate for your module's complexity
- [ ] Set `memory_limit` (default 16 MiB is usually fine)
- [ ] Configure `allowed_upstreams` if using HTTP callouts
- [ ] Set `http_call_limit` to prevent amplification
- [ ] Use `fail_mode("closed")` for security-critical filters
- [ ] Verify `.wasm` binary size is under 500 KiB
- [ ] Test with `make check` before deploying

## Debugging

### VSL Logging

All `proxy_wasm::hostcalls::log()` calls map to Varnish Shared Log:

```bash
# Watch all Wasm log output
varnishlog -g request -q 'Debug ~ "wasm"'

# Watch errors only
varnishlog -g request -q 'Error ~ "wasm"'
```

### Metrics

```vcl
sub vcl_recv {
    if (req.url == "/__wasm_metrics") {
        return (synth(200, ""));
    }
}

sub vcl_synth {
    if (req.url == "/__wasm_metrics") {
        set resp.http.Content-Type = "application/json";
        synthetic(wasm.get_metrics_json());
        return (deliver);
    }
}
```

### Common Issues

| Issue | Cause | Fix |
|-------|-------|-----|
| Module returns -1 | Execution timeout (epoch deadline exceeded) | Increase `set_epoch_deadline()` |
| HTTP callout fails | Upstream not in allowlist | Add to `set_allowed_upstreams()` |
| Module panics on config | Invalid JSON | Validate config schema; use `unwrap_or_default()` |
| Large `.wasm` binary | Debug info included | Use `opt-level = "s"`, `lto = true`, `strip = "debuginfo"` |
