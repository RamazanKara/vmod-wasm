# Production Deployment Guide

## Overview

vmod-wasm embeds a WebAssembly runtime (Wasmtime) into Varnish Cache,
enabling request/response processing via compiled Wasm modules. This
document covers production deployment considerations.

## Resource Limits

### Fuel (Instruction Limit)

```vcl
sub vcl_init {
    wasm.set_fuel(10000000);  # Default: 1000000
}
```

- Controls maximum Wasm instructions per execution
- Prevents infinite loops and runaway computations
- Set based on module complexity: simple filters need ~100K, complex logic ~10M
- Exceeding fuel causes execution to return -1 (error)

### Memory Limit

```vcl
sub vcl_init {
    wasm.set_memory_limit(16777216);  # 16 MiB (default)
}
```

- Maximum linear memory a Wasm module can allocate
- Prevents memory exhaustion attacks
- Minimum recommended: 1 MiB; Maximum recommended: 64 MiB

## Security Configuration

### Upstream Allowlist

```vcl
sub vcl_init {
    wasm.set_allowed_upstreams("api.internal:8080,auth.svc:443");
}
```

- **REQUIRED for production**: Restricts which backends Wasm modules can call via `proxy_http_call`
- Prevents SSRF attacks by limiting callout destinations
- If not set, ALL upstreams are allowed (dangerous in production)
- Format: comma-separated `host:port` entries

### HTTP Call Rate Limit

```vcl
sub vcl_init {
    wasm.set_http_call_limit(5);  # Default: 5
}
```

- Maximum HTTP callouts per request per Wasm execution
- Prevents amplification attacks
- Set to 0 to disable HTTP callouts entirely
- Recommended: 3-5 for most use cases

### Fail Mode

```vcl
sub vcl_init {
    wasm.set_fail_mode("closed");  # Default
}
```

- **"closed"** (default): Wasm errors return -1, allowing VCL to block/handle
- **"open"**: Wasm errors silently return 0 (CONTINUE), request proceeds
- Use "open" only for non-critical observability modules
- Always use "closed" for security-critical filters

## Monitoring

### Metrics Endpoint

```vcl
sub vcl_recv {
    if (req.url == "/__wasm_metrics") {
        return (synth(200, "Metrics"));
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

Returns JSON with all Proxy-Wasm metrics defined by modules:
```json
{
  "requests_total": {"type": "counter", "value": 12345},
  "active_connections": {"type": "gauge", "value": 42}
}
```

### Varnish Shared Log (VSL)

Wasm modules log to VSL via `proxy_log`:
- `SLT_Debug` tags for info/debug messages
- `SLT_Error` tags for errors

Monitor with:
```bash
varnishlog -g request -q 'Debug ~ "wasm"'
```

## Deployment Checklist

- [ ] Set `set_fuel()` appropriate for module complexity
- [ ] Set `set_memory_limit()` (16 MiB default is usually fine)
- [ ] **Set `set_allowed_upstreams()`** if module uses HTTP callouts
- [ ] Set `set_http_call_limit()` to prevent amplification
- [ ] Set `set_fail_mode("closed")` for security filters
- [ ] Expose `/__wasm_metrics` endpoint for monitoring
- [ ] Test Wasm modules with `varnishtest` before deployment
- [ ] Monitor `SLT_Error` logs for Wasm execution failures

## Performance Considerations

- Wasm modules are compiled once at `vcl_init` — instantiation is cheap
- Each request gets its own Wasm instance (no shared state between requests)
- Fuel tracking adds ~5% overhead vs unlimited execution
- Memory limit enforcement is near-zero cost (page fault based)
- HTTP callouts are synchronous — keep timeouts short

## Upgrading Modules

1. Build and test the new `.wasm` file
2. Deploy the file to the Varnish server
3. Reload VCL: `varnishadm vcl.load new_vcl /etc/varnish/default.vcl`
4. Activate: `varnishadm vcl.use new_vcl`
5. Monitor logs for errors
6. Discard old VCL: `varnishadm vcl.discard old_vcl`
