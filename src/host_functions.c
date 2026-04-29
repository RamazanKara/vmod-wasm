/*-
 * Copyright (c) 2026 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Host functions exposed to Wasm modules — provides access to
 * Varnish request context (headers, URL, method, client IP).
 *
 * String passing convention:
 *   The Wasm module allocates a buffer in linear memory and passes
 *   (ptr, len) to the host. The host writes the string into that
 *   buffer and returns the actual string length. If the string is
 *   longer than buf_len, it is truncated. Returns -1 on error.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wasm.h>
#include <wasmtime.h>

#include "cache/cache.h"
#include "vrt_obj.h"
#include "vcl.h"

#include "host_functions.h"

/*
 * Helper: validate a Wasm memory region [ptr, ptr+len) is in bounds.
 */
static int
validate_mem_region(struct vwasm_host_ctx *hctx, int32_t ptr, int32_t len)
{
	size_t mem_size;

	if (!hctx->memory_valid || ptr < 0 || len < 0)
		return (0);

	mem_size = wasmtime_memory_data_size(hctx->wasm_ctx, &hctx->memory);
	if ((size_t)ptr + (size_t)len > mem_size)
		return (0);

	return (1);
}

/*
 * Helper: get pointer into Wasm linear memory at offset.
 */
static uint8_t *
mem_ptr(struct vwasm_host_ctx *hctx, int32_t offset)
{
	return (wasmtime_memory_data(hctx->wasm_ctx, &hctx->memory) + offset);
}

/*
 * Helper: read a string from Wasm memory [ptr, ptr+len).
 * Returns a NUL-terminated copy that must be freed by caller.
 */
static char *
read_wasm_string(struct vwasm_host_ctx *hctx, int32_t ptr, int32_t len)
{
	char *s;

	if (!validate_mem_region(hctx, ptr, len))
		return (NULL);

	s = malloc((size_t)len + 1);
	if (s == NULL)
		return (NULL);

	memcpy(s, mem_ptr(hctx, ptr), (size_t)len);
	s[len] = '\0';
	return (s);
}

/*
 * Helper: write a C string into Wasm memory buffer.
 * Returns actual string length (may exceed buf_len = truncated).
 */
static int32_t
write_to_wasm_buf(struct vwasm_host_ctx *hctx, int32_t buf_ptr,
    int32_t buf_len, const char *str)
{
	int32_t slen;

	if (str == NULL)
		return (-1);

	slen = (int32_t)strlen(str);

	if (buf_len > 0 && validate_mem_region(hctx, buf_ptr, buf_len)) {
		int32_t copy_len = slen < buf_len ? slen : buf_len;
		memcpy(mem_ptr(hctx, buf_ptr), str, (size_t)copy_len);
	}

	return (slen);
}

/* ----------------------------------------------------------------
 * Host function: get_request_header
 *
 * Reads a request header by name.
 * Args:  (name_ptr, name_len, buf_ptr, buf_len) -> i32
 * Returns: actual header value length, or -1 if not found
 * ---------------------------------------------------------------- */
static wasm_trap_t *
host_get_request_header(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_host_ctx *hctx;
	wasmtime_context_t *ctx;
	const struct vrt_ctx *vctx;
	char *hdr_name;
	const char *hdr_val;
	struct http *hp;
	int i;

	(void)env;
	(void)nargs;
	(void)nresults;

	ctx = wasmtime_caller_context(caller);
	hctx = (struct vwasm_host_ctx *)wasmtime_context_get_data(ctx);
	AN(hctx);
	hctx->wasm_ctx = ctx;

	results[0].kind = WASMTIME_I32;
	results[0].of.i32 = -1;

	vctx = hctx->vrt_ctx;
	if (vctx == NULL || vctx->req == NULL)
		return (NULL);
	CHECK_OBJ_NOTNULL(vctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(vctx->req, REQ_MAGIC);

	hp = vctx->req->http;
	if (hp == NULL)
		return (NULL);
	CHECK_OBJ_NOTNULL(hp, HTTP_MAGIC);

	hdr_name = read_wasm_string(hctx, args[0].of.i32, args[1].of.i32);
	if (hdr_name == NULL)
		return (NULL);

	/* Search headers — Varnish stores them as "Name: Value" */
	hdr_val = NULL;
	for (i = HTTP_HDR_FIRST; i < hp->nhd; i++) {
		size_t nlen = strlen(hdr_name);
		if (hp->hd[i].b == NULL)
			continue;
		if (strlen(hp->hd[i].b) > nlen &&
		    hp->hd[i].b[nlen] == ':' &&
		    strncasecmp(hp->hd[i].b, hdr_name, nlen) == 0) {
			hdr_val = hp->hd[i].b + nlen + 1;
			/* Skip leading whitespace */
			while (*hdr_val == ' ' || *hdr_val == '\t')
				hdr_val++;
			break;
		}
	}

	free(hdr_name);

	if (hdr_val == NULL)
		return (NULL);

	results[0].of.i32 = write_to_wasm_buf(hctx,
	    args[2].of.i32, args[3].of.i32, hdr_val);
	return (NULL);
}

/* ----------------------------------------------------------------
 * Host function: get_request_url
 *
 * Returns the request URL.
 * Args:  (buf_ptr, buf_len) -> i32
 * ---------------------------------------------------------------- */
static wasm_trap_t *
host_get_request_url(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_host_ctx *hctx;
	wasmtime_context_t *ctx;
	const struct vrt_ctx *vctx;
	const char *url;

	(void)env;
	(void)nargs;
	(void)nresults;

	ctx = wasmtime_caller_context(caller);
	hctx = (struct vwasm_host_ctx *)wasmtime_context_get_data(ctx);
	AN(hctx);
	hctx->wasm_ctx = ctx;

	results[0].kind = WASMTIME_I32;
	results[0].of.i32 = -1;

	vctx = hctx->vrt_ctx;
	if (vctx == NULL || vctx->req == NULL || vctx->req->http == NULL)
		return (NULL);
	CHECK_OBJ_NOTNULL(vctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(vctx->req, REQ_MAGIC);
	CHECK_OBJ_NOTNULL(vctx->req->http, HTTP_MAGIC);

	url = vctx->req->http->hd[HTTP_HDR_URL].b;
	if (url == NULL)
		return (NULL);

	results[0].of.i32 = write_to_wasm_buf(hctx,
	    args[0].of.i32, args[1].of.i32, url);
	return (NULL);
}

/* ----------------------------------------------------------------
 * Host function: get_request_method
 *
 * Returns the request method (GET, POST, etc.).
 * Args:  (buf_ptr, buf_len) -> i32
 * ---------------------------------------------------------------- */
static wasm_trap_t *
host_get_request_method(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_host_ctx *hctx;
	wasmtime_context_t *ctx;
	const struct vrt_ctx *vctx;
	const char *method;

	(void)env;
	(void)nargs;
	(void)nresults;

	ctx = wasmtime_caller_context(caller);
	hctx = (struct vwasm_host_ctx *)wasmtime_context_get_data(ctx);
	AN(hctx);
	hctx->wasm_ctx = ctx;

	results[0].kind = WASMTIME_I32;
	results[0].of.i32 = -1;

	vctx = hctx->vrt_ctx;
	if (vctx == NULL || vctx->req == NULL || vctx->req->http == NULL)
		return (NULL);
	CHECK_OBJ_NOTNULL(vctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(vctx->req, REQ_MAGIC);
	CHECK_OBJ_NOTNULL(vctx->req->http, HTTP_MAGIC);

	method = vctx->req->http->hd[HTTP_HDR_METHOD].b;
	if (method == NULL)
		return (NULL);

	results[0].of.i32 = write_to_wasm_buf(hctx,
	    args[0].of.i32, args[1].of.i32, method);
	return (NULL);
}

/* ----------------------------------------------------------------
 * Host function: get_client_ip
 *
 * Returns the client IP address as a string.
 * Args:  (buf_ptr, buf_len) -> i32
 * ---------------------------------------------------------------- */
static wasm_trap_t *
host_get_client_ip(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_host_ctx *hctx;
	wasmtime_context_t *ctx;
	const struct vrt_ctx *vctx;
	const char *ip_str;

	(void)env;
	(void)nargs;
	(void)nresults;

	ctx = wasmtime_caller_context(caller);
	hctx = (struct vwasm_host_ctx *)wasmtime_context_get_data(ctx);
	AN(hctx);
	hctx->wasm_ctx = ctx;

	results[0].kind = WASMTIME_I32;
	results[0].of.i32 = -1;

	vctx = hctx->vrt_ctx;
	if (vctx == NULL)
		return (NULL);
	CHECK_OBJ_NOTNULL(vctx, VRT_CTX_MAGIC);

	ip_str = VRT_IP_string(vctx, VRT_r_client_ip(vctx));
	if (ip_str == NULL)
		return (NULL);

	results[0].of.i32 = write_to_wasm_buf(hctx,
	    args[0].of.i32, args[1].of.i32, ip_str);
	return (NULL);
}

/* ----------------------------------------------------------------
 * Host function: set_response_header
 *
 * Sets a response header (for use in vcl_deliver).
 * Args:  (name_ptr, name_len, val_ptr, val_len) -> i32
 * Returns: 0 on success, -1 on error
 * ---------------------------------------------------------------- */
static wasm_trap_t *
host_set_response_header(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_host_ctx *hctx;
	wasmtime_context_t *ctx;
	const struct vrt_ctx *vctx;
	char *name, *val;
	char *hdr_line;
	size_t hdr_len;

	(void)env;
	(void)nargs;
	(void)nresults;

	ctx = wasmtime_caller_context(caller);
	hctx = (struct vwasm_host_ctx *)wasmtime_context_get_data(ctx);
	AN(hctx);
	hctx->wasm_ctx = ctx;

	results[0].kind = WASMTIME_I32;
	results[0].of.i32 = -1;

	vctx = hctx->vrt_ctx;
	if (vctx == NULL || vctx->req == NULL)
		return (NULL);
	CHECK_OBJ_NOTNULL(vctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(vctx->req, REQ_MAGIC);

	name = read_wasm_string(hctx, args[0].of.i32, args[1].of.i32);
	if (name == NULL)
		return (NULL);

	val = read_wasm_string(hctx, args[2].of.i32, args[3].of.i32);
	if (val == NULL) {
		free(name);
		return (NULL);
	}

	/* Build "Name: Value" header line on workspace */
	hdr_len = strlen(name) + 2 + strlen(val) + 1;
	hdr_line = WS_Alloc(vctx->req->ws, (unsigned)hdr_len);
	if (hdr_line != NULL) {
		snprintf(hdr_line, hdr_len, "%s: %s", name, val);
		/* Store in req for later use — actual resp header setting
		 * needs to happen in vcl_deliver context */
		http_SetHeader(vctx->req->http, hdr_line);
		results[0].of.i32 = 0;
	}

	free(name);
	free(val);
	return (NULL);
}

/* ----------------------------------------------------------------
 * Host function: log_msg
 *
 * Writes a log message to Varnish Shared Log (VSL).
 * Args:  (level, msg_ptr, msg_len) -> void
 * Level: 0=Debug, 1=Info, 2=Error
 * ---------------------------------------------------------------- */
static wasm_trap_t *
host_log_msg(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_host_ctx *hctx;
	wasmtime_context_t *ctx;
	const struct vrt_ctx *vctx;
	char *msg;
	int level;

	(void)env;
	(void)nargs;
	(void)nresults;
	(void)results;

	ctx = wasmtime_caller_context(caller);
	hctx = (struct vwasm_host_ctx *)wasmtime_context_get_data(ctx);
	AN(hctx);
	hctx->wasm_ctx = ctx;

	vctx = hctx->vrt_ctx;
	if (vctx == NULL || vctx->vsl == NULL)
		return (NULL);
	CHECK_OBJ_NOTNULL(vctx, VRT_CTX_MAGIC);

	level = args[0].of.i32;
	msg = read_wasm_string(hctx, args[1].of.i32, args[2].of.i32);
	if (msg == NULL)
		return (NULL);

	switch (level) {
	case 0:
		VSLb(vctx->vsl, SLT_Debug, "wasm: %s", msg);
		break;
	case 1:
		VSLb(vctx->vsl, SLT_VCL_Log, "wasm: %s", msg);
		break;
	case 2:
	default:
		VSLb(vctx->vsl, SLT_Error, "wasm: %s", msg);
		break;
	}

	free(msg);
	return (NULL);
}

/* ----------------------------------------------------------------
 * Register all host functions with the Wasmtime linker.
 *
 * Functions are registered under the "env" module namespace,
 * which is the default for Rust's extern "C" imports.
 *
 * IMPORTANT: wasm_functype_new() takes ownership of the valtype
 * vectors' contents, so we must create fresh wasm_valtype_t
 * instances for each function registration.
 * ---------------------------------------------------------------- */

static int
define_func_4_1(wasmtime_linker_t *linker, const char *name,
    wasmtime_func_callback_t callback)
{
	wasm_functype_t *ft;
	wasmtime_error_t *error;
	wasm_valtype_vec_t params, results;

	wasm_valtype_vec_new_uninitialized(&params, 4);
	params.data[0] = wasm_valtype_new(WASM_I32);
	params.data[1] = wasm_valtype_new(WASM_I32);
	params.data[2] = wasm_valtype_new(WASM_I32);
	params.data[3] = wasm_valtype_new(WASM_I32);
	wasm_valtype_vec_new_uninitialized(&results, 1);
	results.data[0] = wasm_valtype_new(WASM_I32);

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

static int
define_func_2_1(wasmtime_linker_t *linker, const char *name,
    wasmtime_func_callback_t callback)
{
	wasm_functype_t *ft;
	wasmtime_error_t *error;
	wasm_valtype_vec_t params, results;

	wasm_valtype_vec_new_uninitialized(&params, 2);
	params.data[0] = wasm_valtype_new(WASM_I32);
	params.data[1] = wasm_valtype_new(WASM_I32);
	wasm_valtype_vec_new_uninitialized(&results, 1);
	results.data[0] = wasm_valtype_new(WASM_I32);

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

static int
define_func_3_0(wasmtime_linker_t *linker, const char *name,
    wasmtime_func_callback_t callback)
{
	wasm_functype_t *ft;
	wasmtime_error_t *error;
	wasm_valtype_vec_t params, results;

	wasm_valtype_vec_new_uninitialized(&params, 3);
	params.data[0] = wasm_valtype_new(WASM_I32);
	params.data[1] = wasm_valtype_new(WASM_I32);
	params.data[2] = wasm_valtype_new(WASM_I32);
	wasm_valtype_vec_new_empty(&results);

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
vwasm_host_define_imports(wasmtime_linker_t *linker)
{
	if (define_func_4_1(linker, "get_request_header",
	    host_get_request_header) != 0)
		return (-1);

	if (define_func_2_1(linker, "get_request_url",
	    host_get_request_url) != 0)
		return (-1);

	if (define_func_2_1(linker, "get_request_method",
	    host_get_request_method) != 0)
		return (-1);

	if (define_func_2_1(linker, "get_client_ip",
	    host_get_client_ip) != 0)
		return (-1);

	if (define_func_4_1(linker, "set_response_header",
	    host_set_response_header) != 0)
		return (-1);

	if (define_func_3_0(linker, "log_msg",
	    host_log_msg) != 0)
		return (-1);

	return (0);
}
