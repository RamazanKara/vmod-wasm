# Configuration Reference

Complete reference for all vmod-wasm VCL functions and their parameters.

## Support And Scope

The stable release line targets Varnish 9.x with Wasmtime C API 44.0.0.
Configuration functions that change engine state are restricted to `vcl_init`,
matching Varnish's expectation that loaded VCL objects become immutable on the
request path.

| Function group | VCL scope |
|----------------|-----------|
| Module loading and resource/security setters | `vcl_init` |
| Raw execution and request Proxy-Wasm functions | client side |
| Response Proxy-Wasm functions | `vcl_backend_response`, `vcl_deliver` |
| Stats, metrics, and version getters | any valid VCL context |

## Module Management

### `wasm.load(name, path)`

Load and compile a WebAssembly module into the current VCL's Wasmtime engine.
Module names must be unique within that loaded VCL.

| Parameter | Type | Description |
|-----------|------|-------------|
| `name` | STRING | Unique identifier for this module |
| `path` | STRING | Absolute path to the `.wasm` file |

**Returns**: void (errors logged to VSL)

**Scope**: `vcl_init`

**Example**:
```vcl
sub vcl_init {
    wasm.load("my_filter", "/etc/varnish/wasm/my_filter.wasm");
}
```

### `wasm.version()`

Returns the vmod-wasm version string.

**Returns**: STRING

---

## Execution

### `wasm.execute(module, func)`

Call an exported function from a raw (non-proxy-wasm) module.

| Parameter | Type | Description |
|-----------|------|-------------|
| `module` | STRING | Module name (from `wasm.load`) |
| `func` | STRING | Exported function name |

**Returns**: INT (the function's i32 return value, or `-1` on error)

**Scope**: client side

### `wasm.proxy_wasm_on_request(module)`

Run the Proxy-Wasm request lifecycle (headers + body).

| Parameter | Type | Description |
|-----------|------|-------------|
| `module` | STRING | Module name |

**Returns**: INT (`0` = continue, positive status = local response, `-1` = error)

**Scope**: client side

### `wasm.proxy_wasm_on_response(module)`

Run the Proxy-Wasm response lifecycle (headers).

| Parameter | Type | Description |
|-----------|------|-------------|
| `module` | STRING | Module name |

**Returns**: INT (`0` = continue, positive status = local response, `-1` = error)

**Scope**: `vcl_backend_response`, `vcl_deliver`

### `wasm.proxy_wasm_on_request_configured(module, vm_config, plugin_config)`

Run request lifecycle with explicit configuration.

| Parameter | Type | Description |
|-----------|------|-------------|
| `module` | STRING | Module name |
| `vm_config` | STRING | VM-level configuration (passed to `on_vm_start`) |
| `plugin_config` | STRING | Plugin configuration (passed to `on_configure`) |

**Returns**: INT (`0` = continue, positive status = local response, `-1` = error)

**Scope**: client side

### `wasm.proxy_wasm_on_response_configured(module, vm_config, plugin_config)`

Run response lifecycle with explicit configuration.

| Parameter | Type | Description |
|-----------|------|-------------|
| `module` | STRING | Module name |
| `vm_config` | STRING | VM-level configuration |
| `plugin_config` | STRING | Plugin configuration |

**Returns**: INT (`0` = continue, positive status = local response, `-1` = error)

**Scope**: `vcl_backend_response`, `vcl_deliver`

---

## Resource Limits

### `wasm.set_epoch_deadline(ms)`

Set maximum wall-clock execution time per Wasm invocation.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `ms` | INT | 5000 | Milliseconds before execution is interrupted |

**Behavior**: Exceeding the deadline causes a Wasm trap; the VMOD call returns
`-1` (error). The default is intentionally conservative. Production filters
should set an explicit lower deadline, usually 50-500ms depending on module
latency and whether callouts are used.

**Scope**: `vcl_init`

### `wasm.get_epoch_deadline()`

**Returns**: INT (current deadline in milliseconds)

### `wasm.set_memory_limit(bytes)`

Set maximum linear memory a Wasm module can allocate.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `bytes` | INT | 16777216 (16 MiB) | Maximum memory in bytes |

**Scope**: `vcl_init`

### `wasm.get_memory_limit()`

**Returns**: INT (current memory limit in bytes)

---

## Pooling And Chains

### `wasm.set_store_pool_size(module, size)`

Pre-warm a fixed number of Wasmtime stores for a loaded module. Modules that
export `_initialize` are left on the fresh-store path because they can keep
mutable state outside linear memory; the pool reset can safely restore only
linear memory.

| Parameter | Type | Description |
|-----------|------|-------------|
| `module` | STRING | Module name (from `wasm.load`) |
| `size` | INT | Number of stores to pre-warm (1-256; default 8 if not set) |

Call this from `vcl_init` after `wasm.load()`.

### `wasm.set_http_pool_size(size)`

Set the maximum number of persistent HTTP callout connections.

| Parameter | Type | Description |
|-----------|------|-------------|
| `size` | INT | Maximum pooled HTTP connections (1-1024; default 16 if not set) |

Call this from `vcl_init`. The pool is shared by Proxy-Wasm HTTP callouts in
the current loaded VCL.

### `wasm.filter_chain(chain_spec)`

Run a request-side chain of modules separated by `|`.

**Returns**: INT (`0` = success, `-1` = error)

**Scope**: client side

**Example**:
```vcl
if (wasm.filter_chain("rate_limit|auth|transform") != 0) {
    return (synth(500, "Filter chain error"));
}
```

### `wasm.filter_chain_response(chain_spec)`

Run a response-side chain of modules separated by `|`.

**Returns**: INT (`0` = success, `-1` = error)

**Scope**: `vcl_backend_response`, `vcl_deliver`

---

## Security

### `wasm.set_allowed_upstreams(list)`

Restrict which backends Wasm modules can reach via `proxy_http_call`.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `list` | STRING | unset/empty (allow all) | Comma-separated `host:port` entries |

**Production recommendation**: Always set this when modules can use HTTP
callouts. An unset or empty list allows all non-private upstreams that pass the
SSRF checks, which is too broad for production.

**Example**:
```vcl
wasm.set_allowed_upstreams("auth.internal:8080,api.backend:443");
```

### `wasm.set_http_call_limit(limit)`

Maximum HTTP callouts per request per Wasm execution.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `limit` | INT | 5 | Max callouts (0 = disable callouts entirely) |

**Scope**: `vcl_init`

### `wasm.set_fail_mode(mode)`

Behavior when Wasm execution encounters an error.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `mode` | STRING | "closed" | "closed" = return error; "open" = return continue |

- **closed**: Errors return -1, allowing VCL to block the request
- **open**: Errors silently return 0 (continue), request proceeds

**Scope**: `vcl_init`

---

## Observability

### `wasm.get_metrics_json()`

Return all Proxy-Wasm metrics as a JSON string.

**Returns**: STRING (JSON object with metric names as keys and values as numbers)

**Example output**:
```json
{
  "edge_requests_total": 15234,
  "edge_blocked_total": 42,
  "edge_rate_limited_total": 7
}
```

### `wasm.get_stats_json()`

Return internal execution statistics as JSON.

**Returns**: STRING (JSON with pool stats, execution counts, error counts)

### `wasm.get_pool_stats_json(module)`

Return store pool statistics for one module.

**Returns**: STRING (JSON with store pool counters)

### `wasm.get_http_pool_stats_json()`

Return HTTP connection pool statistics.

**Returns**: STRING (JSON with HTTP pool counters)

---

## Recommended Production Configuration

```vcl
import wasm;

sub vcl_init {
    # Load the module
    wasm.load("edge", "/etc/varnish/wasm/edge_security_filter.wasm");

    # Execution limits
    wasm.set_epoch_deadline(100);       # fast security filter
    wasm.set_memory_limit(8388608);     # 8 MiB — sufficient for most filters

    # Security
    wasm.set_allowed_upstreams("auth.internal:8080");
    wasm.set_http_call_limit(3);
    wasm.set_fail_mode("closed");       # Block on error
}

sub vcl_recv {
    # Skip Wasm for health checks
    if (req.url == "/health") {
        return (pass);
    }

    # Metrics endpoint
    if (req.url == "/__wasm_metrics") {
        return (synth(200, "Metrics"));
    }

    # Run the filter
    set req.http.X-Wasm-Action =
        wasm.proxy_wasm_on_request("edge");
    if (req.http.X-Wasm-Action != "0") {
        return (synth(403, "Blocked"));
    }
}

sub vcl_synth {
    if (req.url == "/__wasm_metrics") {
        set resp.http.Content-Type = "application/json";
        synthetic(wasm.get_metrics_json());
        return (deliver);
    }
}

sub vcl_deliver {
    set resp.http.X-Wasm-Resp =
        wasm.proxy_wasm_on_response("edge");
}
```
