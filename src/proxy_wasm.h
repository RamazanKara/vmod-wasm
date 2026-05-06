/*-
 * Copyright (c) 2025 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Proxy-Wasm ABI types and host function registration.
 *
 * Full implementation of the Proxy-Wasm ABI v0.2.1 for HTTP filtering.
 * See: https://github.com/proxy-wasm/spec
 */

#ifndef VWASM_PROXY_WASM_H
#define VWASM_PROXY_WASM_H

#include <stdint.h>
#include <pthread.h>
#include <wasmtime.h>

/* ----------------------------------------------------------------
 * Proxy-Wasm ABI enums
 * ---------------------------------------------------------------- */

typedef enum {
	PROXY_LOG_TRACE    = 0,
	PROXY_LOG_DEBUG    = 1,
	PROXY_LOG_INFO     = 2,
	PROXY_LOG_WARN     = 3,
	PROXY_LOG_ERROR    = 4,
	PROXY_LOG_CRITICAL = 5,
} proxy_log_level_t;

typedef enum {
	PROXY_OK              = 0,
	PROXY_NOT_FOUND       = 1,
	PROXY_BAD_ARGUMENT    = 2,
	PROXY_SERIALIZATION   = 3,
	PROXY_PARSE_FAILURE   = 4,
	PROXY_BAD_EXPRESSION  = 5,
	PROXY_INVALID_OP      = 6,
	PROXY_EMPTY           = 7,
	PROXY_CAS_MISMATCH    = 8,
	PROXY_INTERNAL        = 10,
	PROXY_UNIMPLEMENTED   = 12,
} proxy_status_t;

typedef enum {
	PROXY_ACTION_CONTINUE = 0,
	PROXY_ACTION_PAUSE    = 1,
} proxy_action_t;

typedef enum {
	PROXY_MAP_HTTP_REQUEST_HEADERS    = 0,
	PROXY_MAP_HTTP_REQUEST_TRAILERS   = 1,
	PROXY_MAP_HTTP_RESPONSE_HEADERS   = 2,
	PROXY_MAP_HTTP_RESPONSE_TRAILERS  = 3,
} proxy_map_type_t;

typedef enum {
	PROXY_BUFFER_HTTP_REQUEST_BODY  = 0,
	PROXY_BUFFER_HTTP_RESPONSE_BODY = 1,
	PROXY_BUFFER_DOWNSTREAM_DATA    = 2,
	PROXY_BUFFER_UPSTREAM_DATA      = 3,
	PROXY_BUFFER_HTTP_CALL_BODY     = 4,
	PROXY_BUFFER_GRPC_RECV_MSG      = 5,
	PROXY_BUFFER_VM_CONFIGURATION   = 6,
	PROXY_BUFFER_PLUGIN_CONFIG      = 7,
} proxy_buffer_type_t;

typedef enum {
	PROXY_STREAM_TYPE_HTTP_REQUEST  = 0,
	PROXY_STREAM_TYPE_HTTP_RESPONSE = 1,
	PROXY_STREAM_TYPE_DOWNSTREAM    = 2,
	PROXY_STREAM_TYPE_UPSTREAM      = 3,
} proxy_stream_type_t;

typedef enum {
	PROXY_METRIC_COUNTER   = 0,
	PROXY_METRIC_GAUGE     = 1,
	PROXY_METRIC_HISTOGRAM = 2,
} proxy_metric_type_t;

/* Forward declarations */
struct vwasm_shared_data;
struct vwasm_queue_store;
struct vwasm_metric_store;
struct vwasm_engine;

/* ----------------------------------------------------------------
 * Trailer storage — simple key-value pair list
 *
 * Varnish does not expose trailers natively, so we store them
 * in a flat buffer that mirrors the Proxy-Wasm header map format.
 * ---------------------------------------------------------------- */

#define VWASM_MAX_TRAILERS	32
#define VWASM_TRAILER_MAX_LEN	8192  /* Max total trailer data */

struct vwasm_trailer_entry {
	char		*name;
	size_t		 name_len;
	char		*value;
	size_t		 value_len;
};

struct vwasm_trailer_map {
	struct vwasm_trailer_entry entries[VWASM_MAX_TRAILERS];
	uint32_t	count;
};

/* ----------------------------------------------------------------
 * Metric store — thread-safe counter/gauge/histogram storage
 * ---------------------------------------------------------------- */

#define VWASM_MAX_METRICS	256
#define VWASM_METRIC_MAX_NAME	128

struct vwasm_metric {
	char		name[VWASM_METRIC_MAX_NAME];
	uint32_t	name_len;
	proxy_metric_type_t type;
	uint64_t	value;
};

struct vwasm_metric_store {
	pthread_rwlock_t	rwlock;
	struct vwasm_metric	metrics[VWASM_MAX_METRICS];
	uint32_t		count;
};

/* ----------------------------------------------------------------
 * HTTP call response buffer — stores response for callback
 * ---------------------------------------------------------------- */

struct vwasm_http_call_response {
	uint8_t		*raw_buf;	/* Raw response buffer (owns memory) */
	size_t		 raw_len;
	uint8_t		*headers_buf;	/* Pointer into raw_buf */
	size_t		 headers_len;
	uint32_t	 num_headers;
	uint8_t		*body;		/* Pointer into raw_buf */
	size_t		 body_len;
	int		 valid;
};

/* ----------------------------------------------------------------
 * Proxy-Wasm execution context
 *
 * Created per Wasm call, holds Varnish request context and
 * Wasm memory references needed by host functions.
 * ---------------------------------------------------------------- */

struct vwasm_proxy_ctx {
	const struct vrt_ctx	*vrt_ctx;
	struct vwasm_engine	*engine;	/* Back-pointer for http pool */
	wasmtime_context_t	*wasm_ctx;
	wasmtime_memory_t	 memory;
	int			 memory_valid;
	wasmtime_func_t		 allocator;  /* proxy_on_memory_allocate */
	int			 allocator_valid;
	uint32_t		 root_context_id;
	uint32_t		 stream_context_id;
	const char		*module_name;

	/* Local response for send_local_response */
	int			 local_response_set;
	int32_t			 local_response_code;
	char			*local_response_body;
	size_t			 local_response_body_len;
	char			*local_response_headers;
	size_t			 local_response_headers_len;

	/* VM and plugin configuration */
	const char		*vm_config;
	size_t			 vm_config_len;
	const char		*plugin_config;
	size_t			 plugin_config_len;

	/* Tick period (milliseconds, 0 = disabled) */
	uint32_t		 tick_period_ms;

	/* Stream continuation state */
	int			 paused;

	/* Last status for proxy_get_status */
	uint32_t		 last_status_code;
	const char		*last_status_msg;

	/* Done flag */
	int			 done;

	/* Shared data store (global, not per-ctx) */
	struct vwasm_shared_data	*shared_data;

	/* Shared queue store (global, not per-ctx) */
	struct vwasm_queue_store	*queue_store;

	/* Metric store (global, not per-ctx) */
	struct vwasm_metric_store *metric_store;

	/* HTTP call response (for proxy_on_http_call_response) */
	struct vwasm_http_call_response http_response;

	/* HTTP request/response body access */
	const uint8_t		*request_body;
	size_t			 request_body_len;
	int			 request_body_heap;	/* 1 if heap-allocated */
	const uint8_t		*response_body;
	size_t			 response_body_len;
	int			 response_body_heap;	/* 1 if heap-allocated */
	/* Modified body (if module called set_buffer_bytes on body) */
	uint8_t			*modified_body;
	size_t			 modified_body_len;
	int			 body_modified;

	/* HTTP callout rate limiting */
	uint32_t		 http_call_count;
	uint32_t		 http_call_max;

	/* HTTP callout timeout (milliseconds, 0 = use module-supplied) */
	uint32_t		 http_timeout_ms;

	/* Upstream allowlist (NULL = allow all, for backwards compat) */
	const char		**allowed_upstreams;
	uint32_t		 num_allowed_upstreams;

	/* Trailer maps (Varnish has no native trailer support) */
	struct vwasm_trailer_map request_trailers;
	struct vwasm_trailer_map response_trailers;
};

/* ----------------------------------------------------------------
 * Host function declarations (split across proxy_wasm_*.c files)
 * ---------------------------------------------------------------- */

/* proxy_wasm_headers.c */
wasm_trap_t *pw_proxy_get_header_map_value(void *, wasmtime_caller_t *,
    const wasmtime_val_t *, size_t, wasmtime_val_t *, size_t);
wasm_trap_t *pw_proxy_add_header_map_value(void *, wasmtime_caller_t *,
    const wasmtime_val_t *, size_t, wasmtime_val_t *, size_t);
wasm_trap_t *pw_proxy_replace_header_map_value(void *, wasmtime_caller_t *,
    const wasmtime_val_t *, size_t, wasmtime_val_t *, size_t);
wasm_trap_t *pw_proxy_remove_header_map_value(void *, wasmtime_caller_t *,
    const wasmtime_val_t *, size_t, wasmtime_val_t *, size_t);
wasm_trap_t *pw_proxy_get_header_map_pairs(void *, wasmtime_caller_t *,
    const wasmtime_val_t *, size_t, wasmtime_val_t *, size_t);
wasm_trap_t *pw_proxy_set_header_map_pairs(void *, wasmtime_caller_t *,
    const wasmtime_val_t *, size_t, wasmtime_val_t *, size_t);
wasm_trap_t *pw_proxy_get_header_map_size(void *, wasmtime_caller_t *,
    const wasmtime_val_t *, size_t, wasmtime_val_t *, size_t);

/* proxy_wasm_properties.c */
wasm_trap_t *pw_proxy_get_property(void *, wasmtime_caller_t *,
    const wasmtime_val_t *, size_t, wasmtime_val_t *, size_t);
wasm_trap_t *pw_proxy_set_property(void *, wasmtime_caller_t *,
    const wasmtime_val_t *, size_t, wasmtime_val_t *, size_t);

/* proxy_wasm_http.c */
wasm_trap_t *pw_proxy_http_call(void *, wasmtime_caller_t *,
    const wasmtime_val_t *, size_t, wasmtime_val_t *, size_t);

/* Register all Proxy-Wasm host functions with the linker */
int vwasm_proxy_wasm_define_imports(wasmtime_linker_t *linker);

/* Cleanup proxy context resources (call after lifecycle completes) */
void vwasm_proxy_ctx_cleanup(struct vwasm_proxy_ctx *ctx);

/* Cleanup a trailer map (free all entries) */
void vwasm_trailer_map_cleanup(struct vwasm_trailer_map *tm);

/* Global shared state (initialized once, shared across all calls) */
struct vwasm_shared_data *vwasm_proxy_wasm_get_shared_data(void);
struct vwasm_queue_store *vwasm_proxy_wasm_get_queue_store(void);
struct vwasm_metric_store *vwasm_proxy_wasm_get_metric_store(void);
void vwasm_proxy_wasm_destroy_shared(void);

#endif /* VWASM_PROXY_WASM_H */
