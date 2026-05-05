# Security Model

## Overview

vmod-wasm isolates untrusted Wasm code within a sandboxed runtime.
This document describes the security boundaries and controls.

## Isolation Guarantees

### Memory Isolation
- Each Wasm module has its own linear memory, cannot access host memory
- Memory limit (`set_memory_limit`) prevents allocation exhaustion
- No shared memory between Wasm instances

### Execution Isolation
- Fuel limits prevent infinite loops and CPU exhaustion
- Each request gets a fresh instance — no state leakage between requests
- Wasm modules cannot access the filesystem, network, or system calls
- All host interaction goes through explicitly defined host functions

### Network Isolation
- `proxy_http_call` is the ONLY way Wasm can make network calls
- Upstream allowlist (`set_allowed_upstreams`) restricts destinations
- Rate limit (`set_http_call_limit`) prevents amplification
- DNS resolution uses the host's resolver (no custom DNS)

## Threat Model

### Malicious Wasm Module
- **CPU exhaustion**: Mitigated by fuel limits
- **Memory exhaustion**: Mitigated by memory limits
- **SSRF via HTTP callouts**: Mitigated by upstream allowlist
- **Amplification attacks**: Mitigated by HTTP call rate limit
- **Information leakage**: Modules only see headers/body explicitly passed
- **Denial of service**: Fail-closed mode ensures failures block rather than pass

### Supply Chain Attacks
- Only load `.wasm` files from trusted sources
- Verify checksums before deployment
- Use `vcl_init` restrictions to prevent runtime loading

## Security Controls

| Control | Default | Recommendation |
|---------|---------|---------------|
| Fuel limit | 1,000,000 | Set per module complexity |
| Memory limit | 16 MiB | Keep at 16 MiB unless needed |
| Upstream allowlist | Allow all | **Always set in production** |
| HTTP call limit | 5 | 3-5 for most use cases |
| Fail mode | closed | Keep closed for security filters |

## Proxy-Wasm ABI Security

The Proxy-Wasm ABI v0.2.1 host functions are implemented with the
following security considerations:

### Header Access
- `proxy_get_header_map_value`: Read-only access to request/response headers
- `proxy_add_header_map_value`: Can only add to the current request/response
- `proxy_get_header_map_pairs`: Returns all headers including pseudo-headers
- Pseudo-headers (`:method`, `:path`, `:authority`, `:status`) are read-only

### Body Access
- `proxy_get_buffer_bytes`: Read-only access to request/response body
- `proxy_set_buffer_bytes`: Can modify request body (for rewriting)
- Body modification is per-request, not persistent

### Property Access
- `proxy_get_property`: Read-only access to connection metadata
- Available properties: `request.*`, `response.*`, `source.*`, `destination.*`, `node.*`
- No write access to properties

### Shared Data
- `proxy_get_shared_data` / `proxy_set_shared_data`: Thread-safe key-value store
- Shared across all Wasm instances (use for caching, counters)
- Protected by RWLock (FNV-1a hash-based lookup)

### Metrics
- `proxy_define_metric` / `proxy_record_metric` / `proxy_get_metric`: Thread-safe
- Maximum 256 metrics, 128-byte names
- Protected by RWLock

## Incident Response

If a Wasm module misbehaves:

1. **Immediate**: Reload VCL without the Wasm module
   ```bash
   varnishadm vcl.load safe /etc/varnish/safe.vcl
   varnishadm vcl.use safe
   ```

2. **Investigate**: Check VSL for error patterns
   ```bash
   varnishlog -g request -q 'Error ~ "wasm"'
   ```

3. **Root cause**: Review the Wasm module source code
4. **Fix**: Update limits, allowlists, or replace the module
