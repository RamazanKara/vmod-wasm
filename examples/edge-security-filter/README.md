# Edge Security Filter Reference Fixture

This directory is the vmod-wasm integration-test reference for the standalone
[vmod-wasm Edge Security Filter](https://github.com/RamazanKara/vmod-wasm-edge-security-filter).

Use the standalone repository and its release assets for production deployment.
This in-tree copy stays close to the VMOD test suite so vmod-wasm can exercise
Proxy-Wasm headers, body callbacks, shared data, metrics, HTTP callouts, ticks,
and local responses without depending on a network checkout.

## Why It Exists

Most examples prove one thing at a time. This fixture intentionally combines
many host features into one module so the VTC suite can catch regressions in the
interactions between them.

It is useful to read when you want to understand how vmod-wasm behaves under a
realistic filter workload. It is not the product documentation for production
edge security policy.

## Behavior

- Bot detection through configurable `User-Agent` substrings.
- Geo blocking through a trusted `X-Country-Code` request header.
- Per-client rate limiting with Proxy-Wasm shared data and CAS.
- Request enrichment with `X-Request-ID`.
- Security response headers.
- Optional external auth callouts.
- Custom Proxy-Wasm counters.
- Request and response body callback logging.
- Tick-based aggregate request logging.

## Proxy-Wasm Coverage

| ABI feature | What this fixture does with it |
|-------------|--------------------------------|
| `on_vm_start` | Defines metrics |
| `on_configure` | Parses JSON plugin configuration |
| `on_tick` | Logs aggregate request counts |
| `on_http_request_headers` | Runs bot, geo, rate-limit, enrichment, and auth logic |
| `on_http_request_body` | Logs cached request body size |
| `on_http_response_headers` | Adds security headers |
| `on_http_response_body` | Logs response body size |
| `on_http_call_response` | Handles auth service responses |
| `proxy_get_shared_data` / `proxy_set_shared_data` | Maintains counters and rate-limit buckets |
| `proxy_define_metric` / `proxy_increment_metric` | Publishes custom counters |
| `proxy_http_call` | Dispatches auth validation |
| `send_http_response` | Sends local 403, 429, 401, and 503 responses |
| `get_current_time` | Builds request IDs and rate-limit buckets |

## Configuration

Pass JSON plugin configuration through
`wasm.proxy_wasm_on_request_configured(...)`.

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

| Field | Type | Default | Meaning |
|-------|------|---------|---------|
| `rate_limit.requests_per_second` | `u32` | `100` | Max requests per client IP per window |
| `rate_limit.window_seconds` | `u32` | `60` | Rate-limit window size |
| `bot_patterns` | `string[]` | `["BadBot", "Scraper"]` | Case-insensitive substrings that trigger 403 |
| `blocked_countries` | `string[]` | `[]` | Country codes matched against `X-Country-Code` |
| `enrich_headers` | `bool` | `true` | Adds request and response marker/security headers |
| `auth_service` | `string` | `""` | Auth upstream authority, such as `auth.internal:8080`; empty disables callouts |

Invalid JSON falls back to the default fixture config. The standalone product is
stricter and fails closed on invalid configuration.

## Build

From `examples/`:

```bash
cargo build --release --target wasm32-unknown-unknown -p edge-security-filter
```

Artifact:

```text
examples/target/wasm32-unknown-unknown/release/edge_security_filter.wasm
```

## VCL Sketch

```vcl
import wasm;
import std;

sub vcl_init {
    wasm.load("edge", "/path/to/edge_security_filter.wasm");
    wasm.set_epoch_deadline(100);
    wasm.set_memory_limit(8388608);
    wasm.set_allowed_upstreams("auth.internal:8080");
    wasm.set_http_call_limit(3);
    wasm.set_fail_mode("closed");
}

sub vcl_recv {
    set req.http.X-Wasm-Action = wasm.proxy_wasm_on_request_configured(
        "edge",
        "",
        {"{"bot_patterns":["BadBot"],"rate_limit":{"requests_per_second":50}}"}
    );

    if (req.http.X-Wasm-Action != "0") {
        return (synth(std.integer(req.http.X-Wasm-Action, 403), "Blocked"));
    }
}

sub vcl_deliver {
    set resp.http.X-Wasm-Action = wasm.proxy_wasm_on_response("edge");
}
```

## Decision Flow

```text
request
  -> bot check
  -> geo check
  -> rate limit
  -> optional auth callout
  -> enrichment
  -> backend
```

Blocking outcomes:

| Check | Status |
|-------|--------|
| Bot match | `403` |
| Country match | `403` |
| Rate limit exceeded | `429` |
| Auth denied | `401` |
| Auth service unavailable | `503` |

## Metrics

With vmod-wasm, retrieve counters through:

```vcl
synthetic(wasm.get_metrics_json());
```

Fixture counters:

- `edge_requests_total`
- `edge_blocked_total`
- `edge_rate_limited_total`
- `edge_auth_failures_total`

## Related Product

For production policy, release artifacts, hardened validation, report-only mode,
route-aware auth, IP CIDR policy, body caps, and operator docs, use the
standalone product repository:

```text
https://github.com/RamazanKara/vmod-wasm-edge-security-filter
```
