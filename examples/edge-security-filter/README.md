# Edge Security Filter

A production-grade Proxy-Wasm module demonstrating the full vmod-wasm ABI surface with a realistic edge security and observability use case.

## Features

- **Bot Detection** — Block requests based on configurable User-Agent patterns
- **Rate Limiting** — Per-IP rate limiting using Proxy-Wasm shared data with CAS
- **Geo-Blocking** — Block requests by country code (via `X-Country-Code` header)
- **Request Enrichment** — Add `X-Request-ID`, security headers (`X-Content-Type-Options`, `X-Frame-Options`)
- **Auth Callout** — Optional HTTP callout to an external auth service for token validation
- **Custom Metrics** — `edge_requests_total`, `edge_blocked_total`, `edge_rate_limited_total`, `edge_auth_failures_total`
- **Background Tick** — Periodic logging of aggregate request statistics

## Proxy-Wasm ABI Coverage

| ABI Feature | Usage |
|-------------|-------|
| `on_vm_start` | Define metrics, set tick period |
| `on_configure` | Parse JSON plugin configuration |
| `on_tick` | Periodic stats reporting via shared data |
| `on_http_request_headers` | Bot detection, geo-blocking, rate limiting, enrichment, auth callout |
| `on_http_request_body` | Body size logging |
| `on_http_response_headers` | Security header injection |
| `on_http_response_body` | Response body size logging |
| `on_http_call_response` | Auth service callback handling |
| `proxy_get_shared_data` / `proxy_set_shared_data` | Rate limiting counters (CAS) |
| `proxy_define_metric` / `proxy_increment_metric` | Custom counter metrics |
| `proxy_http_call` | External auth service validation |
| `send_http_response` | Direct 403/429/401/503 responses |
| `get_current_time` | Request ID generation, rate limit bucketing |

## Configuration

The module accepts JSON plugin configuration via `proxy_wasm_on_request_configured()`:

```json
{
  "rate_limit": {
    "requests_per_second": 100,
    "window_seconds": 60
  },
  "bot_patterns": ["BadBot", "Scraper"],
  "blocked_countries": ["XX"],
  "enrich_headers": true,
  "auth_service": "auth.internal:8080"
}
```

### Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `rate_limit.requests_per_second` | u32 | 100 | Max requests per IP per window |
| `rate_limit.window_seconds` | u32 | 60 | Rate limit window duration |
| `bot_patterns` | string[] | `["BadBot", "Scraper"]` | User-Agent substrings to block |
| `blocked_countries` | string[] | `[]` | Country codes to block |
| `enrich_headers` | bool | true | Add enrichment/security headers |
| `auth_service` | string | `""` | Auth service upstream (empty = disabled) |

## Building

```bash
cd examples/edge-security-filter
cargo build --release --target wasm32-unknown-unknown
```

Output: `target/wasm32-unknown-unknown/release/edge_security_filter.wasm`

## VCL Integration

```vcl
import wasm;

sub vcl_init {
    wasm.load("edge", "/etc/varnish/wasm/edge_security_filter.wasm");
    wasm.set_epoch_deadline(100);
    wasm.set_memory_limit(8388608);
    wasm.set_allowed_upstreams("auth.internal:8080");
    wasm.set_http_call_limit(3);
}

sub vcl_recv {
    # Without config (uses defaults):
    set req.http.X-Wasm-Action = wasm.proxy_wasm_on_request("edge");

    # With custom config:
    # set req.http.X-Wasm-Action = wasm.proxy_wasm_on_request_configured(
    #     "edge", "", "{\"bot_patterns\":[\"EvilBot\"],\"rate_limit\":{\"requests_per_second\":50}}");

    if (req.http.X-Wasm-Action != "0") {
        # Module already sent a response (403/429/401/503)
        return (synth(403, "Blocked"));
    }
}

sub vcl_deliver {
    set resp.http.X-Wasm-Resp = wasm.proxy_wasm_on_response("edge");
}
```

## Decision Flow

```
Request → Bot Check → Geo Check → Rate Limit → Auth Callout → Enrich → Backend
              ↓ 403       ↓ 403       ↓ 429         ↓ 401/503
```

## Metrics

After processing requests, retrieve metrics via:

```vcl
synthetic(wasm.get_metrics_json());
```

Returns:
```json
{
  "edge_requests_total": 1234,
  "edge_blocked_total": 12,
  "edge_rate_limited_total": 5,
  "edge_auth_failures_total": 2
}
```
