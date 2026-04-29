/*-
 * Copyright (c) 2026 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Host functions exposed to Wasm modules — provides access to
 * Varnish request context (headers, URL, method, client IP).
 */

#ifndef HOST_FUNCTIONS_H
#define HOST_FUNCTIONS_H

#include <wasmtime.h>

struct vrt_ctx;

/*
 * Per-request context passed to Wasm host functions via store data.
 * Contains pointers to the Varnish request context and the Wasm
 * instance's memory for string passing.
 */
struct vwasm_host_ctx {
	const struct vrt_ctx	*vrt_ctx;
	wasmtime_context_t	*wasm_ctx;
	wasmtime_memory_t	memory;
	int			memory_valid;
};

/*
 * Number of host functions we provide.
 * Must match the imports array size in vwasm_host_define_imports().
 */
#define VWASM_HOST_FUNC_COUNT 6

/*
 * Define host function imports for a Wasmtime linker.
 * The linker will resolve these when instantiating modules that
 * import from the "env" namespace.
 *
 * Host functions provided:
 *   env.get_request_header(name_ptr, name_len, buf_ptr, buf_len) -> i32
 *   env.get_request_url(buf_ptr, buf_len) -> i32
 *   env.get_request_method(buf_ptr, buf_len) -> i32
 *   env.get_client_ip(buf_ptr, buf_len) -> i32
 *   env.set_response_header(name_ptr, name_len, val_ptr, val_len) -> i32
 *   env.log_msg(level, msg_ptr, msg_len) -> void
 */
int vwasm_host_define_imports(wasmtime_linker_t *linker);

#endif /* HOST_FUNCTIONS_H */
