# Security Model

## Overview

vmod-wasm isolates untrusted Wasm code within a sandboxed runtime. This
document describes the security boundaries, defaults, and production controls
that matter before you allow modules to process real traffic.

## Isolation Guarantees

### Memory Isolation
- Each Wasm module has its own linear memory, cannot access host memory
- Memory limit (`set_memory_limit`) prevents allocation exhaustion
- No shared memory between Wasm instances

### Execution Isolation
- Epoch-based time limits prevent infinite loops and CPU exhaustion
- Each request gets a fresh instance — no state leakage between requests
- Wasm modules cannot directly access the filesystem, network, or host system
  calls
- All host interaction goes through explicitly defined host functions

### Network Isolation
- `proxy_http_call` is the ONLY way Wasm can make network calls
- Upstream allowlist (`set_allowed_upstreams`) restricts destinations
- Rate limit (`set_http_call_limit`) prevents amplification
- DNS resolution uses the host's resolver (no custom DNS)
- Resolved private/internal IPs are rejected unless the exact `host:port` was
  explicitly allowlisted as a trusted upstream

## Threat Model

### Malicious Wasm Module
- **CPU exhaustion**: Mitigated by epoch-based time limits
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
| Epoch deadline | 5000ms | Set a smaller explicit value per expected module latency |
| Memory limit | 16 MiB | Keep at 16 MiB unless needed |
| Upstream allowlist | Allow all non-private destinations | **Always set in production** |
| HTTP call limit | 5 | 3-5 for most use cases |
| Fail mode | closed | Keep closed for security filters |

## Proxy-Wasm ABI Security

The Proxy-Wasm ABI v0.2.1 host functions are implemented with the
following security considerations:

### Header Access
- `proxy_get_header_map_value`: Read-only access to request/response headers
- `proxy_add_header_map_value`: Can only add to the current request/response
- `proxy_get_header_map_pairs`: Returns all headers including pseudo-headers
- Request pseudo-headers (`:method`, `:path`, `:authority`) can be changed via `proxy_set_property`
- `:status` is read-only

### Body Access
- `proxy_get_buffer_bytes`: Read-only access to request/response body
- `proxy_set_buffer_bytes`: Can rewrite response body chunks in the VDP;
  request-body replacements are visible to later module reads
- Body modification is per-request, not persistent

### Property Access
- `proxy_get_property`: Read access to request, response, connection, and node metadata
- Available properties: `request.*`, `response.*`, `source.*`, `destination.*`, `node.*`
- `proxy_set_property`: Write access is limited to `request.path`, `request.method`, and `request.host`
- Response status and connection/node properties are read-only

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

## Supply Chain Security

### Module Verification

Before deploying a `.wasm` module to production, verify its integrity:

```bash
# Generate checksum after trusted build
sha256sum edge_security_filter.wasm > edge_security_filter.wasm.sha256

# Verify before deployment
sha256sum -c edge_security_filter.wasm.sha256
```

### Trusted Build Pipeline

All `.wasm` modules should be built in CI — never deploy locally-built
binaries to production.

Recommended pipeline:

1. **Source**: Tag a release in the module repository
2. **Build**: CI builds the module in a reproducible Docker environment
3. **Verify**: CI checks binary size, runs clippy, cargo audit, and VTC tests
4. **Sign**: Attach SHA256 checksums to the GitHub release
5. **Deploy**: Pull verified `.wasm` from the release (not from arbitrary sources)

### SBOM (Software Bill of Materials)

Generate an SBOM for each module to track dependencies:

```bash
# Using cargo-sbom (install: cargo install cargo-sbom)
cd examples/edge-security-filter
cargo sbom --output-format spdx_json_2_3 > sbom.spdx.json
```

Include the SBOM in release artifacts for vulnerability tracking.

### Dependency Auditing

Run `cargo audit` regularly to detect known vulnerabilities:

```bash
cd examples
cargo audit
```

CI should fail if any advisory affects a production dependency.

### Binary Size Monitoring

Large binaries may indicate inclusion of unexpected code or debug info:

| Module Type | Expected Size | Alert Threshold |
|-------------|---------------|-----------------|
| Simple filter | 50-100 KiB | > 200 KiB |
| Security filter with serde | 150-300 KiB | > 500 KiB |
| Complex transform | 200-400 KiB | > 500 KiB |

### Edge Security Filter Threat Model

The in-tree `examples/edge-security-filter` module is a reference fixture. The
production product lives in the standalone
`vmod-wasm-edge-security-filter` repository, but both handle the same classes of
untrusted input:

| Threat | Mitigation |
|--------|-----------|
| Malformed User-Agent (injection) | Case-insensitive substring match only; no regex evaluation |
| Rate limit bypass (IP spoofing) | Relies on trusted `X-Forwarded-For` from upstream load balancer |
| Shared data exhaustion | Time-bucketed keys; old buckets naturally expire |
| Auth service DoS | HTTP call limit + timeout; circuit breaker in http_pool |
| Config injection | Product config rejects unknown fields; the fixture falls back to defaults on malformed JSON |
| Integer overflow in counters | u64 counters; overflow at 2^64 is not reachable |
