# Production Deployment Guide

## Overview

vmod-wasm embeds a WebAssembly runtime (Wasmtime) into Varnish Cache,
enabling request/response processing via compiled Wasm modules. This guide is
for operators deciding how to install, constrain, monitor, reload, and roll back
Wasm filters in production.

The stable release support target is Varnish 9.x on Linux `amd64` and `arm64`.
The release workflow bundles the Wasmtime 49.0.2 runtime library used at build
time. Keep `libvmod_wasm.so` and the bundled `libwasmtime.so` together, or set
`LD_LIBRARY_PATH` so `varnishd` can resolve `libwasmtime.so` at startup.

## Installing Release Bundles

Binary release bundles are convenience artifacts for Varnish 9 users. Each one
contains:

- `libvmod_wasm.so`
- bundled `libwasmtime.so`
- BSD-2-Clause license text and third-party notices
- concise install/runtime notes

Install both shared libraries into paths visible to `varnishd`. If you keep the
bundle layout intact, set the service environment so the dynamic loader can find
the bundled Wasmtime library:

```bash
export LD_LIBRARY_PATH=/opt/vmod-wasm/lib:${LD_LIBRARY_PATH}
```

Then verify linkage before traffic:

```bash
ldd /usr/lib/varnish/vmods/libvmod_wasm.so | grep libwasmtime
```

## Minimal Production Shape

A production VCL should load modules only in `vcl_init`, set explicit runtime
limits, fail closed for security decisions, and expose metrics on an internal
path:

```vcl
import wasm;
import std;

acl wasm_metrics_clients {
    "127.0.0.1";
    "::1";
}

sub vcl_init {
    wasm.load("edge", "/etc/varnish/wasm/edge_security_filter.wasm");
    wasm.set_epoch_deadline(100);
    wasm.set_memory_limit(8388608);
    wasm.set_allowed_upstreams("auth.internal:8080");
    wasm.set_http_call_limit(3);
    wasm.set_fail_mode("closed");
}

sub vcl_recv {
    if (req.url == "/__wasm_metrics") {
        if (client.ip !~ wasm_metrics_clients) {
            return (synth(403, "Forbidden"));
        }
        return (synth(200, "Metrics"));
    }

    set req.http.X-Wasm-Action = wasm.proxy_wasm_on_request("edge");
    if (req.http.X-Wasm-Action == "-1") {
        return (synth(503, "Wasm execution failed"));
    }
    if (req.http.X-Wasm-Action != "0") {
        return (synth(std.integer(req.http.X-Wasm-Action, 403), "Blocked"));
    }
}

sub vcl_synth {
    if (req.url == "/__wasm_metrics" && resp.status == 200) {
        set resp.http.Content-Type = "application/json";
        synthetic(wasm.get_metrics_json());
        return (deliver);
    }
}
```

## Resource Limits

### Epoch Deadline (Execution Time Limit)

```vcl
sub vcl_init {
    wasm.set_epoch_deadline(100);  # explicit production deadline
}
```

- Controls maximum wall-clock time per Wasm execution
- Prevents infinite loops and runaway computations
- Based on epoch checks performed by Wasmtime
- Exceeding the deadline causes a trap; execution returns -1 in fail-closed mode
- Built-in default is 5000ms; production VCL should set a smaller explicit value
- Set based on expected module latency: fast filters 50ms, complex logic 200-500ms

### Memory Limit

```vcl
sub vcl_init {
    wasm.set_memory_limit(16777216);  # 16 MiB (default)
}
```

- Maximum linear memory a Wasm module can allocate
- Does not cap shared data, compiled code, or other host allocations
- Set limits before creating an optional store pool

## Security Configuration

### Upstream Allowlist

```vcl
sub vcl_init {
    wasm.set_allowed_upstreams("api.internal:8080,auth.svc:8080");
}
```

- **REQUIRED for production**: Restricts which backends Wasm modules can call via `proxy_http_call`
- Prevents SSRF attacks by limiting callout destinations
- If not set, all non-private destinations that pass SSRF checks are allowed
  (too broad for production)
- Format: comma-separated `host:port` entries
- Callouts use plain HTTP; the port number does not enable TLS

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

Use an ACL appropriate for the network that directly connects to Varnish. A
client-supplied header is not an access-control boundary.

```vcl
acl wasm_metrics_clients {
    "127.0.0.1";
    "::1";
}

sub vcl_recv {
    if (req.url == "/__wasm_metrics") {
        if (client.ip !~ wasm_metrics_clients) {
            return (synth(403, "Forbidden"));
        }
        return (synth(200, "Metrics"));
    }
}

sub vcl_synth {
    if (req.url == "/__wasm_metrics" && resp.status == 200) {
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

- [ ] Verify the release tag matches your Varnish ABI line, for example `varnish9-v4.3.5`
- [ ] Confirm `ldd libvmod_wasm.so` resolves the intended bundled `libwasmtime.so`
- [ ] Set `set_epoch_deadline()` appropriate for expected module latency
- [ ] Set `set_memory_limit()` (16 MiB default is usually fine)
- [ ] **Set `set_allowed_upstreams()`** if module uses HTTP callouts
- [ ] Set `set_http_call_limit()` to prevent amplification
- [ ] Set `set_fail_mode("closed")` for security filters
- [ ] Expose `/__wasm_metrics` endpoint for monitoring
- [ ] Test Wasm modules with `varnishtest` before deployment
- [ ] Run a soak or canary test for VCL reload, pooling, and sustained traffic changes
- [ ] Monitor `SLT_Error` logs for Wasm execution failures

## Performance Considerations

- Wasm modules are compiled once at `vcl_init`; measure instantiation costs for your modules
- Each loaded VCL owns its own Wasmtime engine and compiled module set
- Executions use fresh stores unless a compatible Proxy-Wasm store pool is explicitly configured
- Pool resets restore only the initial linear-memory snapshot, not grown memory, globals, or tables
- HTTP callouts are synchronous — keep timeouts short

For a quick local throughput sweep before larger canary tests, run:

```bash
make perf-test
```

The perf harness compares baseline Varnish proxying with raw `wasm.execute`,
Proxy-Wasm header callbacks, response-body inspection, and response-body
rewrite paths. Results are written under `perf-logs/` and are best used for
relative comparisons on the same machine.

## Response Body Inspection

To enable response body inspection via Proxy-Wasm:

```vcl
sub vcl_deliver {
    set resp.http.X-Wasm-Action = wasm.proxy_wasm_on_response("filter");
    set resp.filters += "wasm_body";
}
```

- The `wasm_body` VDP passes each response body chunk to
  `proxy_on_response_body` and forwards the inspected or rewritten chunk
- `end_of_stream=1` is set on the final chunk
- Memory usage is O(chunk_size), not O(body_size)

## Module Lifecycle Management

### Hot-Reload via VCL Reload

Wasm modules are loaded at `vcl_init` time. To update a module without downtime:

```bash
# 1. Deploy the new .wasm file to disk
cp new_filter.wasm /etc/varnish/wasm/filter.wasm

# 2. Load a new VCL (re-runs vcl_init, recompiles the module)
varnishadm vcl.load reload_1 /etc/varnish/default.vcl

# 3. Switch to the new VCL
varnishadm vcl.use reload_1

# 4. Discard the old VCL (frees old module memory)
varnishadm vcl.discard boot
```

The old VCL and its Wasm engine remain active until all in-flight requests
complete and Varnish discards the VCL. On discard, vmod-wasm unregisters the
VDP filter, stops tick timers, destroys store and HTTP pools, and only then
deletes Wasmtime modules and the engine. Validate reload behavior under your
workload before rollout.

### Canary And Soak Testing

Before promoting a new module or VMOD release, run a short canary and at least
one soak that exercises VCL reloads:

```bash
make soak-test
```

For a longer local run:

```bash
scripts/soak-test.sh \
  --duration 21600 \
  --concurrency 16 \
  --reload-interval 60 \
  --sample-interval 30
```

Check the resulting `soak-logs/<timestamp>/` directory for:

- zero client errors and zero reload errors
- empty Varnish error log
- no bad worker response files
- `MGT.child_panic = 0`
- `MGT.child_died = 0`
- `MAIN.backend_fail = 0`
- `MAIN.threads_failed = 0`

### Graceful Degradation

Use `set_fail_mode()` to control behavior when modules fail:

| Scenario | Recommended Mode | Rationale |
|----------|-----------------|-----------|
| Security filter (WAF, bot detection) | `closed` | Block on failure — safety first |
| Observability (logging, metrics) | `open` | Continue on failure — don't break traffic |
| Header enrichment (non-critical) | `open` | Best-effort enrichment |
| Auth validation | `closed` | Never skip auth |

### Rollback Procedure

If a new module causes issues:

```bash
# Keep a known-good VCL loaded
varnishadm vcl.load safe /etc/varnish/safe.vcl
varnishadm vcl.use safe
```

## Monitoring Integration

### Prometheus Exposition

The protected metrics endpoint above returns JSON, not Prometheus exposition
format. A separate exporter or conversion step is needed for Prometheus.

### Key Metrics to Alert On

| Metric | Alert Threshold | Action |
|--------|----------------|--------|
| `edge_blocked_total` rate | > 10% of total requests | Investigate traffic pattern |
| `edge_rate_limited_total` rate | Sudden spike | Check for DDoS |
| `edge_auth_failures_total` rate | > 5% of auth requests | Check auth service health |
| Wasm execution errors (VSL) | Any sustained errors | Check module health, consider rollback |

### Health Check Integration

Exclude the Wasm filter from health check paths to ensure monitoring is unaffected:

```vcl
sub vcl_recv {
    if (req.url == "/health") {
        return (synth(200, "OK"));
    }
    # ... wasm filter runs for all other paths ...
}
```

## Capacity Planning

### Memory Budget

Per loaded VCL, vmod-wasm allocates:

| Component | Memory | Notes |
|-----------|--------|-------|
| Wasm linear memory | Up to `memory_limit` per active or pooled instance | Default 16 MiB |
| Store pool instances | `store_pool_size * memory_limit` per poolable module | Opt-in; modules exporting `_initialize` bypass pooling |
| Compiled module | Depends on module | Shared within one VCL engine |
| HTTP connection pool | up to `http_pool_size` sockets + buffers | Opt-in |
| Shared data | Depends on stored keys and values | Process-wide; no TTL or eviction |

Budget for active fresh stores as well as pooled stores, compiled modules,
Wasmtime overhead, host buffers, and shared data. The linear-memory limit does
not bound total process memory.

During VCL reloads, old and new VCLs may overlap until in-flight traffic drains,
so budget for at least two loaded VCL generations during deployment.

### CPU Budget

Measure compilation and request latency for the deployed modules. Each loaded
VCL has an epoch ticker thread; HTTP callouts also occupy Varnish worker threads
while waiting for upstream responses.

## Upgrading Modules

1. Build and test the new `.wasm` file
2. Deploy the file to the Varnish server
3. Reload VCL: `varnishadm vcl.load new_vcl /etc/varnish/default.vcl`
4. Activate: `varnishadm vcl.use new_vcl`
5. Monitor logs for errors
6. Discard old VCL: `varnishadm vcl.discard old_vcl`

## Store Pool Performance

### Store Pool Memory Snapshots

When pooling is enabled, checkout copies the initial linear-memory snapshot.
The copy cost depends on the snapshot size. The reset does not cover grown
memory, mutable globals, or tables, so enable pooling only for compatible
modules. Modules exporting `_initialize` bypass pooling.

`wasm.get_pool_stats_json(module)` reports `capacity`, `acquires`, `releases`,
`fallbacks`, and `resets`; it does not report acquisition latency. Use
`make perf-test` to measure the effect of pooling for a representative workload.
