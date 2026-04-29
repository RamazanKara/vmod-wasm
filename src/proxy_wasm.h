/*-
 * Copyright (c) 2026 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Proxy-Wasm ABI types and host function registration.
 *
 * Implements a subset of the Proxy-Wasm ABI v0.2.1 for HTTP filtering.
 * See: https://github.com/proxy-wasm/spec
 */

#ifndef VWASM_PROXY_WASM_H
#define VWASM_PROXY_WASM_H

#include <stdint.h>
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
	PROXY_UNIMPLEMENTED   = 12,
	PROXY_INTERNAL        = 10,
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

/* ----------------------------------------------------------------
 * Proxy-Wasm execution context
 *
 * Created per Wasm call, holds Varnish request context and
 * Wasm memory references needed by host functions.
 * ---------------------------------------------------------------- */

struct vwasm_proxy_ctx {
	const struct vrt_ctx	*vrt_ctx;
	wasmtime_context_t	*wasm_ctx;
	wasmtime_memory_t	 memory;
	int			 memory_valid;
	wasmtime_func_t		 allocator;  /* proxy_on_memory_allocate */
	int			 allocator_valid;
	uint32_t		 root_context_id;
	uint32_t		 stream_context_id;
	/* Local response for send_local_response */
	int			 local_response_set;
	int32_t			 local_response_code;
};

/* Register all Proxy-Wasm host functions with the linker */
int vwasm_proxy_wasm_define_imports(wasmtime_linker_t *linker);

#endif /* VWASM_PROXY_WASM_H */
