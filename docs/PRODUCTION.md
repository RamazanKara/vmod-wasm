# Production Deployment Guide

## Overview

vmod-wasm embeds a WebAssembly runtime (Wasmtime) into Varnish Cache,
enabling request/response processing via compiled Wasm modules. This
document covers production deployment considerations.

## Resource Limits

### Epoch Deadline (Execution Time Limit)

```vcl
sub vcl_init {
    wasm.set_epoch_deadline(100);  # Default: 100ms
}
```

- Controls maximum wall-clock time per Wasm execution
- Prevents infinite loops and runaway computations
- Based on epoch-based interruption (low overhead, no per-instruction cost)
- Exceeding the deadline causes a trap; execution returns -1 (error)
- Set based on expected module latency: fast filters 50ms, complex logic 200ms

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

- [ ] Set `set_epoch_deadline()` appropriate for expected module latency
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
- Epoch-based time limits have near-zero overhead (no per-instruction cost)
- Memory limit enforcement is near-zero cost (page fault based)
- HTTP callouts are synchronous — keep timeouts short

## Response Body Inspection

To enable response body inspection via Proxy-Wasm:

```vcl
sub vcl_deliver {
    set resp.http.X-Wasm-Action = wasm.proxy_wasm_on_response("filter");
    set resp.filters += "wasm_body";
}
```

- The `wasm_body` VDP streams response body chunks directly to
  `proxy_on_response_body` as they arrive — no buffering
- Each chunk is forwarded to the client immediately after inspection
- `end_of_stream=1` is set on the final chunk
- Memory usage is O(chunk_size), not O(body_size)

## Upgrading Modules

1. Build and test the new `.wasm` file
2. Deploy the file to the Varnish server
3. Reload VCL: `varnishadm vcl.load new_vcl /etc/varnish/default.vcl`
4. Activate: `varnishadm vcl.use new_vcl`
5. Monitor logs for errors
6. Discard old VCL: `varnishadm vcl.discard old_vcl`

## Performance Considerations

### Store Pool Memory Snapshots

Each request acquires a pre-warmed Wasm instance from the store pool. To ensure
isolation, the module's linear memory is restored from a snapshot (`memcpy`) on
every acquisition. For typical modules (1-2 Wasm pages = 64-128 KB) this adds
negligible overhead.

For modules declaring large initial memory (10+ pages / 640 KB+), the per-request
`memcpy` can become a throughput bottleneck at very high request rates (10K+/s).
Mitigation strategies:
- Keep module memory declarations minimal
- Use `memory.grow` only when needed (lazy allocation inside the module)
- Monitor `store_pool_acquire_ns` in stats for latency impact
