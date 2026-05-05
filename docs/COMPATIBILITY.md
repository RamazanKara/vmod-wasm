# Proxy-Wasm ABI v0.2.1 Compatibility

## Overview

This document tracks the implementation status of the
[Proxy-Wasm ABI v0.2.1](https://github.com/proxy-wasm/spec) specification.

## Host Functions

### Logging
| Function | Status | Notes |
|----------|--------|-------|
| `proxy_log` | ✅ Implemented | Maps to VSL (SLT_Debug/SLT_Error) |

### Timer
| Function | Status | Notes |
|----------|--------|-------|
| `proxy_get_current_time_nanoseconds` | ✅ Implemented | Uses `clock_gettime(CLOCK_REALTIME)` |

### Header Maps
| Function | Status | Notes |
|----------|--------|-------|
| `proxy_get_header_map_value` | ✅ Implemented | Request/response headers |
| `proxy_add_header_map_value` | ✅ Implemented | Request/response headers |
| `proxy_replace_header_map_value` | ✅ Implemented | Request/response headers |
| `proxy_remove_header_map_value` | ✅ Implemented | Request/response headers |
| `proxy_get_header_map_pairs` | ✅ Implemented | Includes pseudo-headers |
| `proxy_set_header_map_pairs` | ✅ Implemented | Replaces all headers from serialized map |

### Buffers
| Function | Status | Notes |
|----------|--------|-------|
| `proxy_get_buffer_bytes` | ✅ Implemented | HTTP_REQUEST_BODY, HTTP_RESPONSE_BODY, VM_CONFIG, PLUGIN_CONFIG |
| `proxy_set_buffer_bytes` | ✅ Implemented | HTTP_REQUEST_BODY modification |

### HTTP Calls
| Function | Status | Notes |
|----------|--------|-------|
| `proxy_http_call` | ✅ Implemented | With allowlist + rate limit |

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
| `proxy_set_effective_context` | ⚠️ Stub | Returns OK (no-op) |
| `proxy_done` | ⚠️ Stub | Returns OK (no-op) |

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
| `proxy_on_response_body` | ✅ VDP | Delivered via Varnish Delivery Processor; body buffered (up to 1 MiB) and passed to callback on stream end |
| `proxy_on_log` | ✅ Called | In lifecycle step 7 |
| `proxy_on_done` | ✅ Called | In lifecycle step 8 |
| `proxy_on_http_call_response` | ✅ Called | After HTTP callout completes |

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

1. **Response body streaming**: `on_response_body` is called with body_size=0
   because Varnish streams response bodies to clients. Request bodies are
   available if ≤1 MiB (automatically cached).

2. **Synchronous HTTP calls**: `proxy_http_call` blocks the request thread
   until the upstream responds or times out (default 5s).

3. **Anti-IP-rebinding**: HTTP calls reject resolved private/internal IPs
   (RFC1918, RFC5735, RFC4193, loopback) to prevent SSRF.

4. **No gRPC support**: gRPC-specific features are not implemented.

5. **No L4 (TCP/UDP) support**: Only HTTP filter context is supported.

6. **Single filter per execution**: Unlike Envoy's filter chain, each
   `proxy_wasm_on_request()` call runs one module. Chain in VCL if needed.
