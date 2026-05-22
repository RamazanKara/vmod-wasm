# Proxy-Wasm ABI v0.2.1 Compatibility

## Overview

This document tracks the implementation status of the
[Proxy-Wasm ABI v0.2.1](https://github.com/proxy-wasm/spec) specification.

## Host Functions

### Logging
| Function | Status | Notes |
|----------|--------|-------|
| `proxy_log` | ✅ Implemented | Maps to VSL (SLT_Debug/SLT_Error) |
| `proxy_get_log_level` | ✅ Implemented | Returns DEBUG; Varnish controls filtering via VSL |

### Timer
| Function | Status | Notes |
|----------|--------|-------|
| `proxy_get_current_time_nanoseconds` | ✅ Implemented | Uses `clock_gettime(CLOCK_REALTIME)` |
| `proxy_set_tick_period_milliseconds` | ✅ Implemented | Background thread delivers ticks per module |

### Header Maps
| Function | Status | Notes |
|----------|--------|-------|
| `proxy_get_header_map_value` | ✅ Implemented | Request/response headers |
| `proxy_add_header_map_value` | ✅ Implemented | Request/response headers |
| `proxy_replace_header_map_value` | ✅ Implemented | Request/response headers |
| `proxy_remove_header_map_value` | ✅ Implemented | Request/response headers |
| `proxy_get_header_map_pairs` | ✅ Implemented | Includes pseudo-headers |
| `proxy_set_header_map_pairs` | ✅ Implemented | Replaces all headers from serialized map |
| `proxy_get_header_map_size` | ✅ Implemented | Returns entry count for any map type |

### Buffers
| Function | Status | Notes |
|----------|--------|-------|
| `proxy_get_buffer_bytes` | ✅ Implemented | HTTP_REQUEST_BODY, HTTP_RESPONSE_BODY, VM_CONFIG, PLUGIN_CONFIG |
| `proxy_set_buffer_bytes` | ✅ Implemented | HTTP_REQUEST_BODY modification |
| `proxy_get_buffer_status` | ✅ Implemented | Returns buffer size and flags |

### HTTP Calls
| Function | Status | Notes |
|----------|--------|-------|
| `proxy_http_call` | ✅ Implemented | Synchronous TCP call with allowlist, rate limit, and SSRF protection |

### Shared Data
| Function | Status | Notes |
|----------|--------|-------|
| `proxy_get_shared_data` | ✅ Implemented | Thread-safe FNV-1a hash table |
| `proxy_set_shared_data` | ✅ Implemented | With CAS support |

### Shared Queue
| Function | Status | Notes |
|----------|--------|-------|
| `proxy_register_shared_queue` | ✅ Implemented | Thread-safe FIFO queues |
| `proxy_enqueue_shared_queue` | ✅ Implemented | |
| `proxy_dequeue_shared_queue` | ✅ Implemented | |
| `proxy_resolve_shared_queue` | ✅ Implemented | |

### Metrics
| Function | Status | Notes |
|----------|--------|-------|
| `proxy_define_metric` | ✅ Implemented | Counter, gauge, histogram |
| `proxy_record_metric` | ✅ Implemented | Thread-safe |
| `proxy_increment_metric` | ✅ Implemented | |
| `proxy_get_metric` | ✅ Implemented | |

### Properties
| Function | Status | Notes |
|----------|--------|-------|
| `proxy_get_property` | ✅ Implemented | See property table below |
| `proxy_set_property` | ✅ Implemented | request.path, request.method, request.host |

### Lifecycle
| Function | Status | Notes |
|----------|--------|-------|
| `proxy_send_local_response` | ✅ Implemented | Captures body + headers |
| `proxy_set_effective_context` | ✅ No-op | Single context per call; switching not needed |
| `proxy_done` | ✅ No-op | Host manages lifecycle; module signal not required |
| `proxy_continue_stream` | ✅ Implemented | Resumes paused request/response processing |
| `proxy_close_stream` | ✅ Implemented | Terminates stream processing |

### Trailers
| Function | Status | Notes |
|----------|--------|-------|
| Request/response trailers | ✅ Implemented | In-memory map; accessible via header map operations with trailer map types |

### WASI Support
| Function | Status | Notes |
|----------|--------|-------|
| `fd_write` | ✅ Implemented | Full iovec parsing; stdout/stderr only; output to stderr for journal visibility |
| `clock_time_get` | ✅ Implemented | Real nanosecond precision via `clock_gettime` (REALTIME + MONOTONIC) |
| `random_get` | ✅ Implemented | Cryptographically secure: `getrandom(2)` (Linux), `arc4random_buf` (macOS/FreeBSD) |
| `environ_sizes_get` | ✅ Stub | Reports 0 env vars |
| `environ_get` | ✅ Stub | No-op |
| `args_sizes_get` | ✅ Stub | Reports 0 args |
| `args_get` | ✅ Stub | No-op |
| `proc_exit` | ✅ Trap | Traps instead of exiting process |

### Memory
| Function | Status | Notes |
|----------|--------|-------|
| `proxy_on_memory_allocate` | ✅ Required export | Called by host to allocate Wasm memory |

## Callbacks (Module Exports)

| Export | Status | Notes |
|--------|--------|-------|
| `proxy_on_context_create` | ✅ Called | Root and stream contexts |
| `proxy_on_vm_start` | ✅ Called | With vm_config buffer |
| `proxy_on_configure` | ✅ Called | With plugin_config buffer |
| `proxy_on_request_headers` | ✅ Called | In vcl_recv |
| `proxy_on_response_headers` | ✅ Called | In vcl_deliver/vcl_backend_response |
| `proxy_on_request_body` | ✅ Called | Cached body via VRT_CacheReqBody (≤1 MiB) |
| `proxy_on_response_body` | ✅ VDP | Streaming via VDP: each chunk passed immediately with end_of_stream flag; no buffering |
| `proxy_on_log` | ✅ Called | In lifecycle step 7 |
| `proxy_on_done` | ✅ Called | In lifecycle step 8 |
| `proxy_on_http_call_response` | ✅ Called | Deferred: invoked after `on_http_request_headers` returns to avoid proxy-wasm SDK `RefCell` re-entrancy |

## Properties

| Property Path | Status | Varnish Source |
|--------------|--------|---------------|
| `request.path` | ✅ | `http_req->hd[HTTP_HDR_URL]` |
| `request.url_path` | ✅ | `http_req->hd[HTTP_HDR_URL]` |
| `request.method` | ✅ | `http_req->hd[HTTP_HDR_METHOD]` |
| `request.protocol` | ✅ | `http_req->hd[HTTP_HDR_PROTO]` |
| `request.host` | ✅ | Host header value |
| `request.scheme` | ✅ | Always "http" |
| `response.code` | ✅ | `resp->status` |
| `source.address` | ✅ | `VRT_r_client_ip()` |
| `source.port` | ✅ | `VSA_Port(client_ip)` |
| `destination.address` | ✅ | `VRT_r_server_ip()` |
| `destination.port` | ✅ | `VSA_Port(local_ip)` |
| `node.id` | ✅ | `VRT_r_server_identity()` |
| `connection.id` | ✅ | Root context ID |
| `plugin_root_id` | ✅ | Root context ID |

## Pseudo-Headers

Request maps include:
- `:method` — HTTP method
- `:path` — Request URL path
- `:authority` — Host header value

Response maps include:
- `:status` — Response status code

## Limitations

1. **Synchronous HTTP calls**: `proxy_http_call` blocks the Varnish worker thread
   until the upstream responds or times out.
   `proxy_on_http_call_response` is then invoked immediately after
   `on_http_request_headers` returns (deferred callback pattern).
   Timeout is configurable via `wasm.set_http_timeout(ms)` (default 5 s, max 30 s).
   The module-supplied timeout takes priority if non-zero.

2. **Anti-IP-rebinding**: HTTP calls reject resolved private/internal IPs
   (RFC1918, RFC5735, RFC4193, loopback) to prevent SSRF.

3. **No gRPC support**: gRPC-specific features are not implemented.

4. **No L4 (TCP/UDP) support**: Only HTTP filter context is supported.

5. **Single filter per execution**: Unlike Envoy's filter chain, each
   `proxy_wasm_on_request()` call runs one module. Chain in VCL if needed.
