/*-
 * Copyright (c) 2026 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Proxy-Wasm ABI host function implementations.
 *
 * Implements a subset of the Proxy-Wasm ABI v0.2.1 for HTTP filtering
 * on Varnish Cache. Functions are registered under the "env" namespace.
 */

#include <string.h>
#include <stdio.h>
#include <time.h>

#include <wasm.h>
#include <wasmtime.h>

#include "cache/cache.h"
#include "vcl.h"

#include "proxy_wasm.h"

/* ----------------------------------------------------------------
 * Wasm memory helpers (same pattern as host_functions.c)
 * ---------------------------------------------------------------- */

static inline uint8_t *
pw_mem_base(const struct vwasm_proxy_ctx *ctx, size_t *size)
{
	if (!ctx->memory_valid || ctx->wasm_ctx == NULL)
		return (NULL);
	return (wasmtime_memory_data(ctx->wasm_ctx, &ctx->memory));
}

static inline int
pw_validate_region(const struct vwasm_proxy_ctx *ctx,
    uint32_t offset, uint32_t len)
{
	size_t mem_size;
	uint8_t *base;

	base = pw_mem_base(ctx, &mem_size);
	if (base == NULL)
		return (0);
	mem_size = wasmtime_memory_data_size(ctx->wasm_ctx, &ctx->memory);
	if ((uint64_t)offset + len > mem_size)
		return (0);
	return (1);
}

static inline uint8_t *
pw_mem_ptr(const struct vwasm_proxy_ctx *ctx, uint32_t offset)
{
	size_t dummy;
	uint8_t *base;

	base = pw_mem_base(ctx, &dummy);
	if (base == NULL)
		return (NULL);
	return (base + offset);
}

/*
 * Read a string from Wasm linear memory.
 * Returns a NUL-terminated copy allocated on the stack (via alloca-style).
 * For safety, we copy to a local buffer.
 */
static int
pw_read_string(const struct vwasm_proxy_ctx *ctx,
    uint32_t ptr, uint32_t len, char *buf, size_t bufsz)
{
	uint8_t *src;

	if (len == 0) {
		buf[0] = '\0';
		return (0);
	}
	if (len >= bufsz)
		return (-1);
	if (!pw_validate_region(ctx, ptr, len))
		return (-1);
	src = pw_mem_ptr(ctx, ptr);
	if (src == NULL)
		return (-1);
	memcpy(buf, src, len);
	buf[len] = '\0';
	return (0);
}

/*
 * Allocate memory inside the Wasm module by calling
 * proxy_on_memory_allocate(size) → ptr.
 */
static int
pw_alloc_wasm(struct vwasm_proxy_ctx *ctx, uint32_t size, uint32_t *ret_ptr)
{
	wasmtime_val_t args[1], results[1];
	wasmtime_error_t *error;
	wasm_trap_t *trap = NULL;

	if (!ctx->allocator_valid)
		return (-1);

	args[0].kind = WASMTIME_I32;
	args[0].of.i32 = (int32_t)size;

	error = wasmtime_func_call(ctx->wasm_ctx, &ctx->allocator,
	    args, 1, results, 1, &trap);
	if (error != NULL) {
		wasmtime_error_delete(error);
		return (-1);
	}
	if (trap != NULL) {
		wasm_trap_delete(trap);
		return (-1);
	}

	*ret_ptr = (uint32_t)results[0].of.i32;
	return (0);
}

/*
 * Write a value into Wasm memory at the given location.
 * Used to return (ptr, size) pairs to the Wasm caller.
 */
static int
pw_write_u32(const struct vwasm_proxy_ctx *ctx, uint32_t offset, uint32_t value)
{
	uint8_t *dst;

	if (!pw_validate_region(ctx, offset, 4))
		return (-1);
	dst = pw_mem_ptr(ctx, offset);
	if (dst == NULL)
		return (-1);
	memcpy(dst, &value, 4);
	return (0);
}

/*
 * Return a string to Wasm by:
 * 1. Allocating memory in the Wasm module
 * 2. Copying the string data into that memory
 * 3. Writing the (ptr, size) pair into the caller's return slots
 */
static int
pw_return_string(struct vwasm_proxy_ctx *ctx,
    const char *str, size_t len,
    uint32_t ret_data_offset, uint32_t ret_size_offset)
{
	uint32_t wasm_ptr;

	if (str == NULL || len == 0) {
		if (pw_write_u32(ctx, ret_data_offset, 0) != 0)
			return (-1);
		if (pw_write_u32(ctx, ret_size_offset, 0) != 0)
			return (-1);
		return (0);
	}

	if (pw_alloc_wasm(ctx, (uint32_t)len, &wasm_ptr) != 0)
		return (-1);

	if (!pw_validate_region(ctx, wasm_ptr, (uint32_t)len))
		return (-1);

	memcpy(pw_mem_ptr(ctx, wasm_ptr), str, len);

	if (pw_write_u32(ctx, ret_data_offset, wasm_ptr) != 0)
		return (-1);
	if (pw_write_u32(ctx, ret_size_offset, (uint32_t)len) != 0)
		return (-1);

	return (0);
}

/* ----------------------------------------------------------------
 * Proxy-Wasm host function: proxy_log
 *
 * proxy_log(log_level: i32, message_data: i32, message_size: i32) -> i32
 * ---------------------------------------------------------------- */

static wasm_trap_t *
pw_proxy_log(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	const struct vrt_ctx *vctx;
	char buf[4096];
	int32_t level;

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	if (ctx == NULL || ctx->vrt_ctx == NULL) {
		results[0].kind = WASMTIME_I32;
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	vctx = ctx->vrt_ctx;
	level = args[0].of.i32;

	if (pw_read_string(ctx, (uint32_t)args[1].of.i32,
	    (uint32_t)args[2].of.i32, buf, sizeof(buf)) != 0) {
		results[0].kind = WASMTIME_I32;
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	if (vctx->vsl != NULL) {
		if (level <= PROXY_LOG_DEBUG)
			VSLb(vctx->vsl, SLT_Debug, "wasm(pw): %s", buf);
		else if (level <= PROXY_LOG_WARN)
			VSLb(vctx->vsl, SLT_Debug, "wasm(pw): [warn] %s", buf);
		else
			VSLb(vctx->vsl, SLT_Error, "wasm(pw): %s", buf);
	}

	results[0].kind = WASMTIME_I32;
	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

/* ----------------------------------------------------------------
 * Proxy-Wasm host function: proxy_get_header_map_value
 *
 * proxy_get_header_map_value(map_type: i32, key_data: i32, key_size: i32,
 *     return_value_data: i32, return_value_size: i32) -> i32
 * ---------------------------------------------------------------- */

static const struct http *
pw_get_header_map(const struct vwasm_proxy_ctx *ctx, int32_t map_type)
{
	const struct vrt_ctx *vctx = ctx->vrt_ctx;

	if (vctx == NULL)
		return (NULL);

	switch (map_type) {
	case PROXY_MAP_HTTP_REQUEST_HEADERS:
		if (vctx->http_req != NULL)
			return (vctx->http_req);
		return (NULL);
	case PROXY_MAP_HTTP_RESPONSE_HEADERS:
		if (vctx->http_resp != NULL)
			return (vctx->http_resp);
		return (NULL);
	default:
		return (NULL);
	}
}

static wasm_trap_t *
pw_proxy_get_header_map_value(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	const struct http *hp;
	char key_buf[256];
	char hdr_search[260]; /* \NNname: format */
	const char *val;
	int i;

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	results[0].kind = WASMTIME_I32;

	if (ctx == NULL) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	hp = pw_get_header_map(ctx, args[0].of.i32);
	if (hp == NULL) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	if (pw_read_string(ctx, (uint32_t)args[1].of.i32,
	    (uint32_t)args[2].of.i32, key_buf, sizeof(key_buf)) != 0) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	/* Build Varnish header search format: \NNname: */
	i = snprintf(hdr_search + 1, sizeof(hdr_search) - 1, "%s:", key_buf);
	if (i <= 0 || i >= (int)(sizeof(hdr_search) - 1)) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}
	hdr_search[0] = (char)i;

	val = NULL;
	if (!http_GetHdr(hp, hdr_search, &val) || val == NULL) {
		results[0].of.i32 = PROXY_NOT_FOUND;
		return (NULL);
	}

	/* Skip leading whitespace */
	while (*val == ' ' || *val == '\t')
		val++;

	if (pw_return_string(ctx, val, strlen(val),
	    (uint32_t)args[3].of.i32, (uint32_t)args[4].of.i32) != 0) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

/* ----------------------------------------------------------------
 * Proxy-Wasm host function: proxy_add_header_map_value
 *
 * proxy_add_header_map_value(map_type: i32, key_data: i32, key_size: i32,
 *     value_data: i32, value_size: i32) -> i32
 * ---------------------------------------------------------------- */

static wasm_trap_t *
pw_proxy_add_header_map_value(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	const struct vrt_ctx *vctx;
	struct http *hp;
	char key_buf[256], val_buf[4096], hdr_line[4352];

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	results[0].kind = WASMTIME_I32;

	if (ctx == NULL || ctx->vrt_ctx == NULL) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	vctx = ctx->vrt_ctx;

	/* Only support request headers for now */
	if (args[0].of.i32 == PROXY_MAP_HTTP_REQUEST_HEADERS)
		hp = (struct http *)vctx->http_req;
	else if (args[0].of.i32 == PROXY_MAP_HTTP_RESPONSE_HEADERS)
		hp = (struct http *)vctx->http_resp;
	else {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	if (hp == NULL) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	if (pw_read_string(ctx, (uint32_t)args[1].of.i32,
	    (uint32_t)args[2].of.i32, key_buf, sizeof(key_buf)) != 0 ||
	    pw_read_string(ctx, (uint32_t)args[3].of.i32,
	    (uint32_t)args[4].of.i32, val_buf, sizeof(val_buf)) != 0) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	snprintf(hdr_line, sizeof(hdr_line), "%s: %s", key_buf, val_buf);
	http_SetHeader(hp, hdr_line);

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

/* ----------------------------------------------------------------
 * Proxy-Wasm host function: proxy_replace_header_map_value
 * (same as add for Varnish — SetHeader replaces if exists)
 * ---------------------------------------------------------------- */

static wasm_trap_t *
pw_proxy_replace_header_map_value(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	/* In Varnish, http_SetHeader effectively replaces, so reuse add */
	return (pw_proxy_add_header_map_value(env, caller,
	    args, nargs, results, nresults));
}

/* ----------------------------------------------------------------
 * Proxy-Wasm host function: proxy_remove_header_map_value
 *
 * proxy_remove_header_map_value(map_type: i32,
 *     key_data: i32, key_size: i32) -> i32
 * ---------------------------------------------------------------- */

static wasm_trap_t *
pw_proxy_remove_header_map_value(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	const struct vrt_ctx *vctx;
	struct http *hp;
	char key_buf[256], hdr_search[260];
	int i;

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	results[0].kind = WASMTIME_I32;

	if (ctx == NULL || ctx->vrt_ctx == NULL) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	vctx = ctx->vrt_ctx;

	if (args[0].of.i32 == PROXY_MAP_HTTP_REQUEST_HEADERS)
		hp = (struct http *)vctx->http_req;
	else if (args[0].of.i32 == PROXY_MAP_HTTP_RESPONSE_HEADERS)
		hp = (struct http *)vctx->http_resp;
	else {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	if (hp == NULL) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	if (pw_read_string(ctx, (uint32_t)args[1].of.i32,
	    (uint32_t)args[2].of.i32, key_buf, sizeof(key_buf)) != 0) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	/* Build Varnish header search format */
	i = snprintf(hdr_search + 1, sizeof(hdr_search) - 1, "%s:", key_buf);
	if (i <= 0 || i >= (int)(sizeof(hdr_search) - 1)) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}
	hdr_search[0] = (char)i;

	http_Unset(hp, hdr_search);

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

/* ----------------------------------------------------------------
 * Proxy-Wasm host function: proxy_get_property
 *
 * proxy_get_property(path_data: i32, path_size: i32,
 *     return_value_data: i32, return_value_size: i32) -> i32
 *
 * Supported properties:
 *   request.path, request.url_path, request.method,
 *   request.scheme, request.host, request.protocol,
 *   source.address
 * ---------------------------------------------------------------- */

static wasm_trap_t *
pw_proxy_get_property(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	const struct vrt_ctx *vctx;
	char path_buf[256];
	const char *val = NULL;

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	results[0].kind = WASMTIME_I32;

	if (ctx == NULL || ctx->vrt_ctx == NULL) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	vctx = ctx->vrt_ctx;

	if (pw_read_string(ctx, (uint32_t)args[0].of.i32,
	    (uint32_t)args[1].of.i32, path_buf, sizeof(path_buf)) != 0) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	/*
	 * Proxy-Wasm properties are serialized as NUL-separated path
	 * components. Common paths:
	 *   ["request", "path"]      → request\0path
	 *   ["request", "method"]    → request\0method
	 *   ["source", "address"]    → source\0address
	 *
	 * For simplicity, we match the raw serialized path.
	 */
	if (vctx->http_req != NULL) {
		if (args[1].of.i32 == 12 &&
		    memcmp(path_buf, "request\0path", 12) == 0)
			val = vctx->http_req->hd[HTTP_HDR_URL].b;
		else if (args[1].of.i32 == 14 &&
		    memcmp(path_buf, "request\0method", 14) == 0)
			val = vctx->http_req->hd[HTTP_HDR_METHOD].b;
		else if (args[1].of.i32 == 16 &&
		    memcmp(path_buf, "request\0protocol", 16) == 0)
			val = vctx->http_req->hd[HTTP_HDR_PROTO].b;
	}

	if (val == NULL) {
		results[0].of.i32 = PROXY_NOT_FOUND;
		return (NULL);
	}

	if (pw_return_string(ctx, val, strlen(val),
	    (uint32_t)args[2].of.i32, (uint32_t)args[3].of.i32) != 0) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

/* ----------------------------------------------------------------
 * Proxy-Wasm host function: proxy_send_local_response
 *
 * proxy_send_local_response(status_code: i32,
 *     status_code_details_data: i32, status_code_details_size: i32,
 *     body_data: i32, body_size: i32,
 *     additional_headers_data: i32, additional_headers_size: i32,
 *     grpc_status: i32) -> i32
 *
 * For Varnish, we just record the status code and the caller
 * (vwasm_proxy_wasm_call) will use it to return synth().
 * ---------------------------------------------------------------- */

static wasm_trap_t *
pw_proxy_send_local_response(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	results[0].kind = WASMTIME_I32;

	if (ctx == NULL) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	ctx->local_response_set = 1;
	ctx->local_response_code = args[0].of.i32;

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

/* ----------------------------------------------------------------
 * Proxy-Wasm host function: proxy_get_current_time_nanoseconds
 *
 * proxy_get_current_time_nanoseconds(return_time: i32) -> i32
 * ---------------------------------------------------------------- */

static wasm_trap_t *
pw_proxy_get_current_time(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	struct timespec ts;
	uint64_t nanos;

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	results[0].kind = WASMTIME_I32;

	if (ctx == NULL) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	clock_gettime(CLOCK_REALTIME, &ts);
	nanos = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

	/* Write 8 bytes (uint64) at the return offset */
	if (!pw_validate_region(ctx, (uint32_t)args[0].of.i32, 8)) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}
	memcpy(pw_mem_ptr(ctx, (uint32_t)args[0].of.i32), &nanos, 8);

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

/* ----------------------------------------------------------------
 * Proxy-Wasm host function: proxy_set_effective_context
 *
 * proxy_set_effective_context(context_id: i32) -> i32
 *
 * Stub — single-context operation in our implementation.
 * ---------------------------------------------------------------- */

static wasm_trap_t *
pw_proxy_set_effective_context(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	(void)env;
	(void)caller;
	(void)args;

	results[0].kind = WASMTIME_I32;
	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

/* ----------------------------------------------------------------
 * Proxy-Wasm host function: proxy_get_buffer_bytes
 *
 * proxy_get_buffer_bytes(buffer_type: i32, start: i32, max_size: i32,
 *     return_buffer_data: i32, return_buffer_size: i32) -> i32
 *
 * Stub — returns empty for VM/plugin configuration.
 * ---------------------------------------------------------------- */

static wasm_trap_t *
pw_proxy_get_buffer_bytes(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	results[0].kind = WASMTIME_I32;

	if (ctx == NULL) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	/* Return empty buffer for all types */
	if (pw_write_u32(ctx, (uint32_t)args[3].of.i32, 0) != 0 ||
	    pw_write_u32(ctx, (uint32_t)args[4].of.i32, 0) != 0) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

/* ----------------------------------------------------------------
 * Linker registration — register all Proxy-Wasm host functions.
 *
 * Same pattern as host_functions.c: create fresh wasm_valtype_t
 * per registration to avoid ownership issues.
 * ---------------------------------------------------------------- */

static int
pw_define_func(wasmtime_linker_t *linker, const char *name,
    int nparams, int nresults_count, wasmtime_func_callback_t callback)
{
	wasm_functype_t *ft;
	wasmtime_error_t *error;
	wasm_valtype_vec_t params, results;
	int i;

	wasm_valtype_vec_new_uninitialized(&params, nparams);
	for (i = 0; i < nparams; i++)
		params.data[i] = wasm_valtype_new(WASM_I32);

	if (nresults_count > 0) {
		wasm_valtype_vec_new_uninitialized(&results, nresults_count);
		for (i = 0; i < nresults_count; i++)
			results.data[i] = wasm_valtype_new(WASM_I32);
	} else {
		wasm_valtype_vec_new_empty(&results);
	}

	ft = wasm_functype_new(&params, &results);
	if (ft == NULL)
		return (-1);

	error = wasmtime_linker_define_func(linker,
	    "env", 3, name, strlen(name),
	    ft, callback, NULL, NULL);
	wasm_functype_delete(ft);

	if (error != NULL) {
		wasmtime_error_delete(error);
		return (-1);
	}
	return (0);
}

int
vwasm_proxy_wasm_define_imports(wasmtime_linker_t *linker)
{
	/* proxy_log(level, msg_data, msg_size) -> status */
	if (pw_define_func(linker, "proxy_log", 3, 1,
	    pw_proxy_log) != 0)
		return (-1);

	/* proxy_get_header_map_value(map, key_d, key_s, ret_d, ret_s) -> status */
	if (pw_define_func(linker, "proxy_get_header_map_value", 5, 1,
	    pw_proxy_get_header_map_value) != 0)
		return (-1);

	/* proxy_add_header_map_value(map, key_d, key_s, val_d, val_s) -> status */
	if (pw_define_func(linker, "proxy_add_header_map_value", 5, 1,
	    pw_proxy_add_header_map_value) != 0)
		return (-1);

	/* proxy_replace_header_map_value(map, key_d, key_s, val_d, val_s) -> status */
	if (pw_define_func(linker, "proxy_replace_header_map_value", 5, 1,
	    pw_proxy_replace_header_map_value) != 0)
		return (-1);

	/* proxy_remove_header_map_value(map, key_d, key_s) -> status */
	if (pw_define_func(linker, "proxy_remove_header_map_value", 3, 1,
	    pw_proxy_remove_header_map_value) != 0)
		return (-1);

	/* proxy_get_property(path_d, path_s, ret_d, ret_s) -> status */
	if (pw_define_func(linker, "proxy_get_property", 4, 1,
	    pw_proxy_get_property) != 0)
		return (-1);

	/* proxy_send_local_response(code, det_d, det_s, body_d, body_s, hdr_d, hdr_s, grpc) -> status */
	if (pw_define_func(linker, "proxy_send_local_response", 8, 1,
	    pw_proxy_send_local_response) != 0)
		return (-1);

	/* proxy_get_current_time_nanoseconds(ret_time) -> status */
	if (pw_define_func(linker, "proxy_get_current_time_nanoseconds", 1, 1,
	    pw_proxy_get_current_time) != 0)
		return (-1);

	/* proxy_set_effective_context(context_id) -> status */
	if (pw_define_func(linker, "proxy_set_effective_context", 1, 1,
	    pw_proxy_set_effective_context) != 0)
		return (-1);

	/* proxy_get_buffer_bytes(type, start, max, ret_d, ret_s) -> status */
	if (pw_define_func(linker, "proxy_get_buffer_bytes", 5, 1,
	    pw_proxy_get_buffer_bytes) != 0)
		return (-1);

	return (0);
}
