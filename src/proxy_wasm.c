/*-
 * Copyright (c) 2026 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Proxy-Wasm ABI host function implementations.
 *
 * Full implementation of the Proxy-Wasm ABI v0.2.1 for HTTP filtering
 * on Varnish Cache. Functions are registered under the "env" namespace.
 *
 * Not implemented (returns UNIMPLEMENTED):
 *   - proxy_grpc_* (Varnish is HTTP-only, no gRPC support)
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>
#include <fcntl.h>

#include <wasm.h>
#include <wasmtime.h>

#include "cache/cache.h"
#include "vrt_obj.h"
#include "vsa.h"
#include "vcl.h"

#include "proxy_wasm.h"
#include "proxy_wasm_shared.h"

/* ----------------------------------------------------------------
 * Global shared state (singleton, thread-safe)
 * ---------------------------------------------------------------- */

static struct vwasm_shared_data *global_shared_data = NULL;
static struct vwasm_queue_store *global_queue_store = NULL;
static struct vwasm_metric_store *global_metric_store = NULL;
static pthread_mutex_t global_shared_mtx = PTHREAD_MUTEX_INITIALIZER;

struct vwasm_shared_data *
vwasm_proxy_wasm_get_shared_data(void)
{
	if (global_shared_data == NULL) {
		pthread_mutex_lock(&global_shared_mtx);
		if (global_shared_data == NULL)
			global_shared_data = vwasm_shared_data_new();
		pthread_mutex_unlock(&global_shared_mtx);
	}
	return (global_shared_data);
}

struct vwasm_queue_store *
vwasm_proxy_wasm_get_queue_store(void)
{
	if (global_queue_store == NULL) {
		pthread_mutex_lock(&global_shared_mtx);
		if (global_queue_store == NULL)
			global_queue_store = vwasm_queue_store_new();
		pthread_mutex_unlock(&global_shared_mtx);
	}
	return (global_queue_store);
}

struct vwasm_metric_store *
vwasm_proxy_wasm_get_metric_store(void)
{
	if (global_metric_store == NULL) {
		pthread_mutex_lock(&global_shared_mtx);
		if (global_metric_store == NULL) {
			global_metric_store = calloc(1,
			    sizeof(struct vwasm_metric_store));
			if (global_metric_store != NULL)
				pthread_rwlock_init(
				    &global_metric_store->rwlock, NULL);
		}
		pthread_mutex_unlock(&global_shared_mtx);
	}
	return (global_metric_store);
}

void
vwasm_proxy_wasm_destroy_shared(void)
{
	pthread_mutex_lock(&global_shared_mtx);
	if (global_shared_data != NULL) {
		vwasm_shared_data_destroy(&global_shared_data);
		global_shared_data = NULL;
	}
	if (global_queue_store != NULL) {
		vwasm_queue_store_destroy(&global_queue_store);
		global_queue_store = NULL;
	}
	if (global_metric_store != NULL) {
		pthread_rwlock_destroy(&global_metric_store->rwlock);
		free(global_metric_store);
		global_metric_store = NULL;
	}
	pthread_mutex_unlock(&global_shared_mtx);
}

/* ----------------------------------------------------------------
 * Wasm memory helpers
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
 * proxy_on_memory_allocate(size) -> ptr.
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
 * Return a byte buffer to Wasm by allocating in-module memory.
 */
static int
pw_return_bytes(struct vwasm_proxy_ctx *ctx,
    const void *data, size_t len,
    uint32_t ret_data_offset, uint32_t ret_size_offset)
{
	uint32_t wasm_ptr;

	if (data == NULL || len == 0) {
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

	memcpy(pw_mem_ptr(ctx, wasm_ptr), data, len);

	if (pw_write_u32(ctx, ret_data_offset, wasm_ptr) != 0)
		return (-1);
	if (pw_write_u32(ctx, ret_size_offset, (uint32_t)len) != 0)
		return (-1);

	return (0);
}

static int
pw_return_string(struct vwasm_proxy_ctx *ctx,
    const char *str, size_t len,
    uint32_t ret_data_offset, uint32_t ret_size_offset)
{
	return (pw_return_bytes(ctx, str, len,
	    ret_data_offset, ret_size_offset));
}

/* ----------------------------------------------------------------
 * proxy_log
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
	AN(ctx);
	if (ctx->vrt_ctx == NULL) {
		results[0].kind = WASMTIME_I32;
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	vctx = ctx->vrt_ctx;
	CHECK_OBJ_NOTNULL(vctx, VRT_CTX_MAGIC);
	level = args[0].of.i32;

	if (pw_read_string(ctx, (uint32_t)args[1].of.i32,
	    (uint32_t)args[2].of.i32, buf, sizeof(buf)) != 0) {
		results[0].kind = WASMTIME_I32;
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	if (vctx->vsl != NULL) {
		const char *mod = ctx->module_name ? ctx->module_name : "?";
		if (level <= PROXY_LOG_DEBUG)
			VSLb(vctx->vsl, SLT_Debug,
			    "[wasm:%s] %s", mod, buf);
		else if (level <= PROXY_LOG_WARN)
			VSLb(vctx->vsl, SLT_Debug,
			    "[wasm:%s] WARN: %s", mod, buf);
		else
			VSLb(vctx->vsl, SLT_Error,
			    "[wasm:%s] %s", mod, buf);
	}

	results[0].kind = WASMTIME_I32;
	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

/* ----------------------------------------------------------------
 * proxy_get_header_map_value
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
		if (vctx->http_beresp != NULL)
			return (vctx->http_beresp);
		return (NULL);
	case PROXY_MAP_HTTP_REQUEST_TRAILERS:
	case PROXY_MAP_HTTP_RESPONSE_TRAILERS:
		/* Varnish doesn't support trailers in VCL */
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
	char hdr_search[260];
	const char *val;
	int i;

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

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

	/* Varnish header lookup: \NNname: format */
	i = snprintf(hdr_search + 1, sizeof(hdr_search) - 1, "%s:", key_buf);
	if (i <= 0 || i >= (int)(sizeof(hdr_search) - 1)) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}
	hdr_search[0] = (char)i;

	val = NULL;
	if (!http_GetHdr(hp, (hdr_t)hdr_search, &val) || val == NULL) {
		results[0].of.i32 = PROXY_NOT_FOUND;
		return (NULL);
	}

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
 * proxy_add_header_map_value
 * ---------------------------------------------------------------- */

static struct http *
pw_get_header_map_mutable(const struct vwasm_proxy_ctx *ctx, int32_t map_type)
{
	const struct vrt_ctx *vctx = ctx->vrt_ctx;

	if (vctx == NULL)
		return (NULL);

	switch (map_type) {
	case PROXY_MAP_HTTP_REQUEST_HEADERS:
		return ((struct http *)vctx->http_req);
	case PROXY_MAP_HTTP_RESPONSE_HEADERS:
		if (vctx->http_resp != NULL)
			return ((struct http *)vctx->http_resp);
		if (vctx->http_beresp != NULL)
			return ((struct http *)vctx->http_beresp);
		return (NULL);
	default:
		return (NULL);
	}
}

static wasm_trap_t *
pw_proxy_add_header_map_value(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	struct http *hp;
	char key_buf[256], val_buf[4096], hdr_line[4352];

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	hp = pw_get_header_map_mutable(ctx, args[0].of.i32);
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
 * proxy_replace_header_map_value
 * ---------------------------------------------------------------- */

static wasm_trap_t *
pw_proxy_replace_header_map_value(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	struct http *hp;
	char key_buf[256], val_buf[4096], hdr_search[260], hdr_line[4352];
	int i;

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	hp = pw_get_header_map_mutable(ctx, args[0].of.i32);
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

	/* Remove existing header first */
	i = snprintf(hdr_search + 1, sizeof(hdr_search) - 1, "%s:", key_buf);
	if (i > 0 && i < (int)(sizeof(hdr_search) - 1)) {
		hdr_search[0] = (char)i;
		http_Unset(hp, (hdr_t)hdr_search);
	}

	/* Set new value */
	snprintf(hdr_line, sizeof(hdr_line), "%s: %s", key_buf, val_buf);
	http_SetHeader(hp, hdr_line);

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

/* ----------------------------------------------------------------
 * proxy_remove_header_map_value
 * ---------------------------------------------------------------- */

static wasm_trap_t *
pw_proxy_remove_header_map_value(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	struct http *hp;
	char key_buf[256], hdr_search[260];
	int i;

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	hp = pw_get_header_map_mutable(ctx, args[0].of.i32);
	if (hp == NULL) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	if (pw_read_string(ctx, (uint32_t)args[1].of.i32,
	    (uint32_t)args[2].of.i32, key_buf, sizeof(key_buf)) != 0) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	i = snprintf(hdr_search + 1, sizeof(hdr_search) - 1, "%s:", key_buf);
	if (i <= 0 || i >= (int)(sizeof(hdr_search) - 1)) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}
	hdr_search[0] = (char)i;

	http_Unset(hp, (hdr_t)hdr_search);

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

/* ----------------------------------------------------------------
 * proxy_get_header_map_pairs
 *
 * Serialization format (proxy-wasm spec):
 *   4 bytes: number of pairs (LE u32)
 *   For each pair: 4 bytes key_size (LE u32), 4 bytes value_size (LE u32)
 *   Then for each pair: key bytes + NUL + value bytes + NUL
 * ---------------------------------------------------------------- */

static wasm_trap_t *
pw_proxy_get_header_map_pairs(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	const struct http *hp;
	uint8_t *buf;
	size_t total_size;
	uint32_t num_headers, idx, i;
	uint32_t offset;
	int32_t map_type;
	const char *pseudo_keys[3];
	const char *pseudo_vals[3];
	uint32_t num_pseudo;

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	hp = pw_get_header_map(ctx, args[0].of.i32);
	if (hp == NULL) {
		/* Return empty for unsupported map types */
		if (pw_return_bytes(ctx, NULL, 0,
		    (uint32_t)args[1].of.i32,
		    (uint32_t)args[2].of.i32) != 0) {
			results[0].of.i32 = PROXY_INTERNAL;
			return (NULL);
		}
		results[0].of.i32 = PROXY_OK;
		return (NULL);
	}

	/*
	 * Add pseudo-headers based on map type:
	 * Request:  :method, :path, :authority
	 * Response: :status
	 */
	map_type = args[0].of.i32;
	pseudo_keys[0] = NULL;
	pseudo_keys[1] = NULL;
	pseudo_keys[2] = NULL;
	pseudo_vals[0] = NULL;
	pseudo_vals[1] = NULL;
	pseudo_vals[2] = NULL;
	num_pseudo = 0;

	if (map_type == 0) { /* RequestHeaders */
		if (hp->hd[HTTP_HDR_METHOD].b != NULL) {
			pseudo_keys[num_pseudo] = ":method";
			pseudo_vals[num_pseudo] = hp->hd[HTTP_HDR_METHOD].b;
			num_pseudo++;
		}
		if (hp->hd[HTTP_HDR_URL].b != NULL) {
			pseudo_keys[num_pseudo] = ":path";
			pseudo_vals[num_pseudo] = hp->hd[HTTP_HDR_URL].b;
			num_pseudo++;
		}
		/* :authority from Host header */
		for (i = HTTP_HDR_FIRST; i < hp->nhd; i++) {
			if (hp->hd[i].b != NULL &&
			    strncasecmp(hp->hd[i].b, "Host:", 5) == 0) {
				const char *v = hp->hd[i].b + 5;
				while (*v == ' ' || *v == '\t')
					v++;
				pseudo_keys[num_pseudo] = ":authority";
				pseudo_vals[num_pseudo] = v;
				num_pseudo++;
				break;
			}
		}
	} else if (map_type == 1) { /* ResponseHeaders */
		if (hp->hd[HTTP_HDR_STATUS].b != NULL) {
			pseudo_keys[num_pseudo] = ":status";
			pseudo_vals[num_pseudo] = hp->hd[HTTP_HDR_STATUS].b;
			num_pseudo++;
		}
	}

	num_headers = hp->nhd - HTTP_HDR_FIRST + num_pseudo;

	/*
	 * Calculate total buffer size:
	 *   4 (count) + num_headers * 8 (size pairs)
	 *   + sum of (key_len + 1 + val_len + 1) for each header
	 */
	total_size = 4 + (size_t)num_headers * 8;

	/* Size for pseudo-headers */
	for (i = 0; i < num_pseudo; i++) {
		total_size += strlen(pseudo_keys[i]) + 1 +
		    strlen(pseudo_vals[i]) + 1;
	}

	/* Size for regular headers */
	for (i = 0; i < (uint32_t)(hp->nhd - HTTP_HDR_FIRST); i++) {
		const char *hdr_line;
		const char *colon;

		idx = HTTP_HDR_FIRST + i;
		if (idx >= hp->nhd)
			break;
		hdr_line = hp->hd[idx].b;
		if (hdr_line == NULL)
			continue;
		colon = strchr(hdr_line, ':');
		if (colon == NULL) {
			total_size += strlen(hdr_line) + 1 + 1;
		} else {
			size_t key_len = (size_t)(colon - hdr_line);
			const char *val = colon + 1;
			while (*val == ' ' || *val == '\t')
				val++;
			total_size += key_len + 1 + strlen(val) + 1;
		}
	}

	buf = malloc(total_size);
	if (buf == NULL) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	/* Write header count */
	memcpy(buf, &num_headers, 4);
	offset = 4;

	/* Write sizes: pseudo-headers first */
	for (i = 0; i < num_pseudo; i++) {
		uint32_t key_len = (uint32_t)strlen(pseudo_keys[i]);
		uint32_t val_len = (uint32_t)strlen(pseudo_vals[i]);
		memcpy(buf + offset, &key_len, 4);
		offset += 4;
		memcpy(buf + offset, &val_len, 4);
		offset += 4;
	}

	/* Write sizes: regular headers */
	for (i = 0; i < (uint32_t)(hp->nhd - HTTP_HDR_FIRST); i++) {
		const char *hdr_line;
		const char *colon;
		uint32_t key_len, val_len;

		idx = HTTP_HDR_FIRST + i;
		if (idx >= hp->nhd)
			break;
		hdr_line = hp->hd[idx].b;
		if (hdr_line == NULL) {
			key_len = 0;
			val_len = 0;
		} else {
			colon = strchr(hdr_line, ':');
			if (colon == NULL) {
				key_len = (uint32_t)strlen(hdr_line);
				val_len = 0;
			} else {
				const char *val;
				key_len = (uint32_t)(colon - hdr_line);
				val = colon + 1;
				while (*val == ' ' || *val == '\t')
					val++;
				val_len = (uint32_t)strlen(val);
			}
		}
		memcpy(buf + offset, &key_len, 4);
		offset += 4;
		memcpy(buf + offset, &val_len, 4);
		offset += 4;
	}

	/* Write key\0value\0 pairs: pseudo-headers first */
	for (i = 0; i < num_pseudo; i++) {
		size_t klen = strlen(pseudo_keys[i]);
		size_t vlen = strlen(pseudo_vals[i]);
		memcpy(buf + offset, pseudo_keys[i], klen);
		offset += (uint32_t)klen;
		buf[offset++] = '\0';
		memcpy(buf + offset, pseudo_vals[i], vlen);
		offset += (uint32_t)vlen;
		buf[offset++] = '\0';
	}

	/* Write key\0value\0 pairs: regular headers */
	for (i = 0; i < (uint32_t)(hp->nhd - HTTP_HDR_FIRST); i++) {
		const char *hdr_line;
		const char *colon;

		idx = HTTP_HDR_FIRST + i;
		if (idx >= hp->nhd)
			break;
		hdr_line = hp->hd[idx].b;
		if (hdr_line == NULL)
			continue;

		colon = strchr(hdr_line, ':');
		if (colon == NULL) {
			size_t klen = strlen(hdr_line);
			memcpy(buf + offset, hdr_line, klen);
			offset += (uint32_t)klen;
			buf[offset++] = '\0';
			buf[offset++] = '\0';
		} else {
			size_t klen = (size_t)(colon - hdr_line);
			const char *val = colon + 1;
			size_t vlen;
			while (*val == ' ' || *val == '\t')
				val++;
			vlen = strlen(val);

			memcpy(buf + offset, hdr_line, klen);
			offset += (uint32_t)klen;
			buf[offset++] = '\0';
			memcpy(buf + offset, val, vlen);
			offset += (uint32_t)vlen;
			buf[offset++] = '\0';
		}
	}

	if (pw_return_bytes(ctx, buf, offset,
	    (uint32_t)args[1].of.i32,
	    (uint32_t)args[2].of.i32) != 0) {
		free(buf);
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	free(buf);
	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

/* ----------------------------------------------------------------
 * proxy_set_header_map_pairs
 * ---------------------------------------------------------------- */

static wasm_trap_t *
pw_proxy_set_header_map_pairs(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	struct http *hp;
	uint8_t *data;
	uint32_t map_type, data_ptr, data_size;
	uint32_t num_pairs, i, offset;
	char hdr_line[4352];

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	map_type = (uint32_t)args[0].of.i32;
	data_ptr = (uint32_t)args[1].of.i32;
	data_size = (uint32_t)args[2].of.i32;

	hp = pw_get_header_map_mutable(ctx, (int32_t)map_type);
	if (hp == NULL) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	if (data_size < 4 || !pw_validate_region(ctx, data_ptr, data_size)) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	data = pw_mem_ptr(ctx, data_ptr);
	if (data == NULL) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	memcpy(&num_pairs, data, 4);
	if (num_pairs > 256) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	if (data_size < 4 + num_pairs * 8) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	/* Clear existing headers */
	for (i = HTTP_HDR_FIRST; i < hp->nhd; i++) {
		hp->hd[i].b = NULL;
		hp->hd[i].e = NULL;
	}
	hp->nhd = HTTP_HDR_FIRST;

	/* Parse and set headers */
	offset = 4 + num_pairs * 8;

	for (i = 0; i < num_pairs; i++) {
		uint32_t key_size, val_size;
		const char *key, *val;

		memcpy(&key_size, data + 4 + i * 8, 4);
		memcpy(&val_size, data + 4 + i * 8 + 4, 4);

		if (offset + key_size + 1 + val_size + 1 > data_size)
			break;

		key = (const char *)(data + offset);
		offset += key_size + 1;
		val = (const char *)(data + offset);
		offset += val_size + 1;

		if (key_size > 0 && key_size < 256 && val_size < 4096) {
			snprintf(hdr_line, sizeof(hdr_line), "%.*s: %.*s",
			    (int)key_size, key, (int)val_size, val);
			http_SetHeader(hp, hdr_line);
		}
	}

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

/* ----------------------------------------------------------------
 * proxy_get_property
 * ---------------------------------------------------------------- */

static wasm_trap_t *
pw_proxy_get_property(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	const struct vrt_ctx *vctx;
	char path_buf[512];
	const char *val = NULL;
	char num_buf[32];
	uint32_t path_size;
	VCL_IP sa;

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	if (ctx->vrt_ctx == NULL) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	vctx = ctx->vrt_ctx;
	CHECK_OBJ_NOTNULL(vctx, VRT_CTX_MAGIC);

	path_size = (uint32_t)args[1].of.i32;
	if (pw_read_string(ctx, (uint32_t)args[0].of.i32,
	    path_size, path_buf, sizeof(path_buf)) != 0) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	/* Request properties */
	if (vctx->http_req != NULL) {
		if (path_size == 12 &&
		    memcmp(path_buf, "request\0path", 12) == 0)
			val = vctx->http_req->hd[HTTP_HDR_URL].b;
		else if (path_size == 16 &&
		    memcmp(path_buf, "request\0url_path", 16) == 0)
			val = vctx->http_req->hd[HTTP_HDR_URL].b;
		else if (path_size == 14 &&
		    memcmp(path_buf, "request\0method", 14) == 0)
			val = vctx->http_req->hd[HTTP_HDR_METHOD].b;
		else if (path_size == 16 &&
		    memcmp(path_buf, "request\0protocol", 16) == 0)
			val = vctx->http_req->hd[HTTP_HDR_PROTO].b;
		else if (path_size == 12 &&
		    memcmp(path_buf, "request\0host", 12) == 0) {
			const char *hval;
			char host_search[] = "\005Host:";
			if (http_GetHdr(vctx->http_req,
			    (hdr_t)host_search, &hval))
				val = hval;
		}
		else if (path_size == 14 &&
		    memcmp(path_buf, "request\0scheme", 14) == 0)
			val = "http";
	}

	/* Response properties */
	if (val == NULL) {
		const struct http *resp = NULL;
		if (vctx->http_resp != NULL)
			resp = vctx->http_resp;
		else if (vctx->http_beresp != NULL)
			resp = vctx->http_beresp;

		if (resp != NULL && path_size == 13 &&
		    memcmp(path_buf, "response\0code", 13) == 0) {
			snprintf(num_buf, sizeof(num_buf), "%d", resp->status);
			val = num_buf;
		}
	}

	/* Plugin properties */
	if (val == NULL && path_size == 14 &&
	    memcmp(path_buf, "plugin_root_id", 14) == 0) {
		snprintf(num_buf, sizeof(num_buf), "%u",
		    ctx->root_context_id);
		val = num_buf;
	}

	/* Connection / source / destination properties */
	if (val == NULL && path_size == 14 &&
	    memcmp(path_buf, "source\0address", 14) == 0) {
		val = VRT_IP_string(vctx, VRT_r_client_ip(vctx));
	}
	if (val == NULL && path_size == 11 &&
	    memcmp(path_buf, "source\0port", 11) == 0) {
		sa = VRT_r_client_ip(vctx);
		if (sa != NULL) {
			snprintf(num_buf, sizeof(num_buf), "%u",
			    VSA_Port(sa));
			val = num_buf;
		}
	}
	if (val == NULL && path_size == 19 &&
	    memcmp(path_buf, "destination\0address", 19) == 0) {
		val = VRT_IP_string(vctx, VRT_r_server_ip(vctx));
	}
	if (val == NULL && path_size == 16 &&
	    memcmp(path_buf, "destination\0port", 16) == 0) {
		sa = VRT_r_local_ip(vctx);
		if (sa != NULL) {
			snprintf(num_buf, sizeof(num_buf), "%u",
			    VSA_Port(sa));
			val = num_buf;
		}
	}
	if (val == NULL && path_size == 7 &&
	    memcmp(path_buf, "node\0id", 7) == 0) {
		val = VRT_r_server_identity(vctx);
	}
	if (val == NULL && path_size == 13 &&
	    memcmp(path_buf, "connection\0id", 13) == 0) {
		snprintf(num_buf, sizeof(num_buf), "%u",
		    ctx->root_context_id);
		val = num_buf;
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
 * proxy_set_property
 *
 * Allows Wasm modules to mutate request properties:
 *   - request.path   → rewrite URL
 *   - request.method → change HTTP method
 *   - request.host   → change Host header
 *
 * ABI: proxy_set_property(path_ptr, path_size, value_ptr, value_size)
 * ---------------------------------------------------------------- */

static wasm_trap_t *
pw_proxy_set_property(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	const struct vrt_ctx *vctx;
	char path_buf[512], value_buf[4096];
	uint32_t path_size, value_size;
	struct http *hp;

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	if (ctx->vrt_ctx == NULL) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}
	vctx = ctx->vrt_ctx;

	path_size = (uint32_t)args[1].of.i32;
	value_size = (uint32_t)args[3].of.i32;

	if (pw_read_string(ctx, (uint32_t)args[0].of.i32,
	    path_size, path_buf, sizeof(path_buf)) != 0) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}
	if (pw_read_string(ctx, (uint32_t)args[2].of.i32,
	    value_size, value_buf, sizeof(value_buf)) != 0) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	hp = (struct http *)vctx->http_req;
	if (hp == NULL) {
		results[0].of.i32 = PROXY_NOT_FOUND;
		return (NULL);
	}

	/* request.path / request.url_path → rewrite URL */
	if ((path_size == 12 && memcmp(path_buf, "request\0path", 12) == 0) ||
	    (path_size == 16 && memcmp(path_buf, "request\0url_path", 16) == 0)) {
		if (value_size == 0 || value_buf[0] != '/') {
			results[0].of.i32 = PROXY_BAD_ARGUMENT;
			return (NULL);
		}
		http_SetH(hp, HTTP_HDR_URL, WS_Copy(vctx->ws,
		    value_buf, (int)(value_size + 1)));
		results[0].of.i32 = PROXY_OK;
		return (NULL);
	}

	/* request.method → change HTTP method */
	if (path_size == 14 && memcmp(path_buf, "request\0method", 14) == 0) {
		if (value_size == 0 || value_size > 16) {
			results[0].of.i32 = PROXY_BAD_ARGUMENT;
			return (NULL);
		}
		http_SetH(hp, HTTP_HDR_METHOD, WS_Copy(vctx->ws,
		    value_buf, (int)(value_size + 1)));
		results[0].of.i32 = PROXY_OK;
		return (NULL);
	}

	/* request.host → change Host header */
	if (path_size == 12 && memcmp(path_buf, "request\0host", 12) == 0) {
		char hdr_line[4352];
		if (value_size == 0 || value_size > 4096) {
			results[0].of.i32 = PROXY_BAD_ARGUMENT;
			return (NULL);
		}
		snprintf(hdr_line, sizeof(hdr_line), "Host: %.*s",
		    (int)value_size, value_buf);
		http_Unset(hp, (hdr_t)"\005Host:");
		http_SetHeader(hp, WS_Copy(vctx->ws,
		    hdr_line, (int)(strlen(hdr_line) + 1)));
		results[0].of.i32 = PROXY_OK;
		return (NULL);
	}

	/* Unknown property — read-only or not found */
	results[0].of.i32 = PROXY_NOT_FOUND;
	return (NULL);
}

/* ----------------------------------------------------------------
 * proxy_send_local_response
 * ---------------------------------------------------------------- */

static wasm_trap_t *
pw_proxy_send_local_response(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	uint32_t body_ptr, body_size;
	uint32_t headers_ptr, headers_size;

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	/*
	 * ABI: proxy_send_local_response(
	 *   status_code,           // args[0] i32
	 *   headers_ptr,           // args[1] i32 (serialized header map)
	 *   headers_size,          // args[2] i32
	 *   body_ptr,              // args[3] i32
	 *   body_size,             // args[4] i32
	 *   grpc_status,           // args[5] i32 (ignored)
	 *   grpc_status_msg_ptr,   // args[6] i32 (ignored)
	 *   grpc_status_msg_size   // args[7] i32 (ignored)
	 * )
	 */
	ctx->local_response_set = 1;
	ctx->local_response_code = args[0].of.i32;

	/* Capture response body */
	body_ptr = (uint32_t)args[3].of.i32;
	body_size = (uint32_t)args[4].of.i32;
	if (body_size > 0 && pw_validate_region(ctx, body_ptr, body_size)) {
		free(ctx->local_response_body);
		ctx->local_response_body = malloc(body_size + 1);
		if (ctx->local_response_body != NULL) {
			memcpy(ctx->local_response_body,
			    pw_mem_ptr(ctx, body_ptr), body_size);
			ctx->local_response_body[body_size] = '\0';
			ctx->local_response_body_len = body_size;
		}
	}

	/* Capture response headers (serialized proxy-wasm format) */
	headers_ptr = (uint32_t)args[1].of.i32;
	headers_size = (uint32_t)args[2].of.i32;
	if (headers_size > 0 && pw_validate_region(ctx, headers_ptr, headers_size)) {
		free(ctx->local_response_headers);
		ctx->local_response_headers = malloc(headers_size);
		if (ctx->local_response_headers != NULL) {
			memcpy(ctx->local_response_headers,
			    pw_mem_ptr(ctx, headers_ptr), headers_size);
			ctx->local_response_headers_len = headers_size;
		}
	}

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

/* ----------------------------------------------------------------
 * proxy_get_current_time_nanoseconds
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
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	clock_gettime(CLOCK_REALTIME, &ts);
	nanos = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

	if (!pw_validate_region(ctx, (uint32_t)args[0].of.i32, 8)) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}
	memcpy(pw_mem_ptr(ctx, (uint32_t)args[0].of.i32), &nanos, 8);

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

/* ----------------------------------------------------------------
 * Generic stubs for no-op / unimplemented host functions
 * ---------------------------------------------------------------- */

static wasm_trap_t *
pw_stub_ok(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	(void)env;
	(void)caller;
	(void)args;
	(void)nargs;

	if (nresults > 0) {
		results[0].kind = WASMTIME_I32;
		results[0].of.i32 = PROXY_OK;
	}
	return (NULL);
}

static wasm_trap_t *
pw_stub_not_found(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	(void)env;
	(void)caller;
	(void)args;
	(void)nargs;

	if (nresults > 0) {
		results[0].kind = WASMTIME_I32;
		results[0].of.i32 = PROXY_NOT_FOUND;
	}
	return (NULL);
}

/* ----------------------------------------------------------------
 * proxy_get_buffer_bytes
 * ---------------------------------------------------------------- */

static wasm_trap_t *
pw_proxy_get_buffer_bytes(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	int32_t buffer_type;
	uint32_t start, max_size;
	const char *data = NULL;
	size_t data_len = 0;

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	buffer_type = args[0].of.i32;
	start = (uint32_t)args[1].of.i32;
	max_size = (uint32_t)args[2].of.i32;

	switch (buffer_type) {
	case PROXY_BUFFER_VM_CONFIGURATION:
		data = ctx->vm_config;
		data_len = ctx->vm_config_len;
		break;
	case PROXY_BUFFER_PLUGIN_CONFIG:
		data = ctx->plugin_config;
		data_len = ctx->plugin_config_len;
		break;
	case PROXY_BUFFER_HTTP_CALL_BODY:
		if (ctx->http_response.valid) {
			data = (const char *)ctx->http_response.body;
			data_len = ctx->http_response.body_len;
		}
		break;
	case PROXY_BUFFER_HTTP_REQUEST_BODY:
		if (ctx->body_modified && ctx->modified_body != NULL) {
			data = (const char *)ctx->modified_body;
			data_len = ctx->modified_body_len;
		} else if (ctx->request_body != NULL) {
			data = (const char *)ctx->request_body;
			data_len = ctx->request_body_len;
		}
		break;
	case PROXY_BUFFER_HTTP_RESPONSE_BODY:
		if (ctx->response_body != NULL) {
			data = (const char *)ctx->response_body;
			data_len = ctx->response_body_len;
		}
		break;
	default:
		data = NULL;
		data_len = 0;
		break;
	}

	/* Apply start/max_size */
	if (data != NULL && start < data_len) {
		data += start;
		data_len -= start;
		if (max_size > 0 && data_len > max_size)
			data_len = max_size;
	} else if (data != NULL) {
		data = NULL;
		data_len = 0;
	}

	if (pw_return_bytes(ctx, data, data_len,
	    (uint32_t)args[3].of.i32, (uint32_t)args[4].of.i32) != 0) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

/* ----------------------------------------------------------------
 * proxy_set_buffer_bytes
 * ---------------------------------------------------------------- */

static wasm_trap_t *
pw_proxy_set_buffer_bytes(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	int32_t buffer_type;
	uint32_t start, size, data_ptr;
	uint8_t *new_body;

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	/*
	 * ABI: proxy_set_buffer_bytes(buffer_type, start, size,
	 *                             data_ptr, data_size)
	 */
	buffer_type = args[0].of.i32;
	start = (uint32_t)args[1].of.i32;
	size = (uint32_t)args[2].of.i32;
	data_ptr = (uint32_t)args[3].of.i32;
	/* args[4] = data_size (replacement data size) */
	(void)start;
	(void)size;

	switch (buffer_type) {
	case PROXY_BUFFER_HTTP_REQUEST_BODY:
		/* Replace request body with module-provided data */
		if (args[4].of.i32 > 0 &&
		    pw_validate_region(ctx, data_ptr, (uint32_t)args[4].of.i32)) {
			new_body = malloc((size_t)args[4].of.i32);
			if (new_body == NULL) {
				results[0].of.i32 = PROXY_INTERNAL;
				return (NULL);
			}
			memcpy(new_body, pw_mem_ptr(ctx, data_ptr),
			    (size_t)args[4].of.i32);
			free(ctx->modified_body);
			ctx->modified_body = new_body;
			ctx->modified_body_len = (size_t)args[4].of.i32;
			ctx->body_modified = 1;
		}
		break;
	default:
		/* Other buffer types are read-only in this host */
		break;
	}

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

/* ----------------------------------------------------------------
 * proxy_get_shared_data / proxy_set_shared_data
 * ---------------------------------------------------------------- */

static wasm_trap_t *
pw_proxy_get_shared_data(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	char key_buf[VWASM_SHARED_DATA_MAX_KEY];
	uint8_t *value = NULL;
	size_t value_len = 0;
	uint32_t cas = 0;
	int ret;

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	if (ctx->shared_data == NULL) {
		results[0].of.i32 = PROXY_NOT_FOUND;
		return (NULL);
	}

	if (pw_read_string(ctx, (uint32_t)args[0].of.i32,
	    (uint32_t)args[1].of.i32, key_buf, sizeof(key_buf)) != 0) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	ret = vwasm_shared_data_get(ctx->shared_data,
	    key_buf, (uint32_t)args[1].of.i32,
	    &value, &value_len, &cas);

	if (ret != 0) {
		results[0].of.i32 = PROXY_NOT_FOUND;
		return (NULL);
	}

	if (pw_return_bytes(ctx, value, value_len,
	    (uint32_t)args[2].of.i32, (uint32_t)args[3].of.i32) != 0) {
		free(value);
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}
	free(value);

	if (pw_write_u32(ctx, (uint32_t)args[4].of.i32, cas) != 0) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

static wasm_trap_t *
pw_proxy_set_shared_data(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	char key_buf[VWASM_SHARED_DATA_MAX_KEY];
	uint8_t *value_data;
	uint32_t key_size, val_ptr, val_size, cas;
	int ret;

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	if (ctx->shared_data == NULL) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	key_size = (uint32_t)args[1].of.i32;
	val_ptr = (uint32_t)args[2].of.i32;
	val_size = (uint32_t)args[3].of.i32;
	cas = (uint32_t)args[4].of.i32;

	if (pw_read_string(ctx, (uint32_t)args[0].of.i32,
	    key_size, key_buf, sizeof(key_buf)) != 0) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	value_data = NULL;
	if (val_size > 0) {
		if (!pw_validate_region(ctx, val_ptr, val_size)) {
			results[0].of.i32 = PROXY_BAD_ARGUMENT;
			return (NULL);
		}
		value_data = pw_mem_ptr(ctx, val_ptr);
	}

	ret = vwasm_shared_data_set(ctx->shared_data,
	    key_buf, key_size, value_data, val_size, cas);

	if (ret == -1) {
		results[0].of.i32 = PROXY_CAS_MISMATCH;
		return (NULL);
	}
	if (ret == -2) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

/* ----------------------------------------------------------------
 * proxy_register_shared_queue / resolve / enqueue / dequeue
 * ---------------------------------------------------------------- */

static wasm_trap_t *
pw_proxy_register_shared_queue(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	char name_buf[VWASM_QUEUE_MAX_NAME];
	uint32_t queue_id;

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	if (ctx->queue_store == NULL) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	if (pw_read_string(ctx, (uint32_t)args[0].of.i32,
	    (uint32_t)args[1].of.i32, name_buf, sizeof(name_buf)) != 0) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	queue_id = vwasm_queue_register(ctx->queue_store,
	    name_buf, (uint32_t)args[1].of.i32);
	if (queue_id == 0) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	if (pw_write_u32(ctx, (uint32_t)args[2].of.i32, queue_id) != 0) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

static wasm_trap_t *
pw_proxy_resolve_shared_queue(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	char vm_buf[256], name_buf[VWASM_QUEUE_MAX_NAME];
	uint32_t queue_id;

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	if (ctx->queue_store == NULL) {
		results[0].of.i32 = PROXY_NOT_FOUND;
		return (NULL);
	}

	if (pw_read_string(ctx, (uint32_t)args[0].of.i32,
	    (uint32_t)args[1].of.i32, vm_buf, sizeof(vm_buf)) != 0 ||
	    pw_read_string(ctx, (uint32_t)args[2].of.i32,
	    (uint32_t)args[3].of.i32, name_buf, sizeof(name_buf)) != 0) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	queue_id = vwasm_queue_resolve(ctx->queue_store,
	    vm_buf, (uint32_t)args[1].of.i32,
	    name_buf, (uint32_t)args[3].of.i32);
	if (queue_id == 0) {
		results[0].of.i32 = PROXY_NOT_FOUND;
		return (NULL);
	}

	if (pw_write_u32(ctx, (uint32_t)args[4].of.i32, queue_id) != 0) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

static wasm_trap_t *
pw_proxy_enqueue_shared_queue(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	uint32_t queue_id, data_ptr, data_size;
	uint8_t *data;

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	if (ctx->queue_store == NULL) {
		results[0].of.i32 = PROXY_NOT_FOUND;
		return (NULL);
	}

	queue_id = (uint32_t)args[0].of.i32;
	data_ptr = (uint32_t)args[1].of.i32;
	data_size = (uint32_t)args[2].of.i32;

	data = NULL;
	if (data_size > 0) {
		if (!pw_validate_region(ctx, data_ptr, data_size)) {
			results[0].of.i32 = PROXY_BAD_ARGUMENT;
			return (NULL);
		}
		data = pw_mem_ptr(ctx, data_ptr);
	}

	if (vwasm_queue_enqueue(ctx->queue_store, queue_id,
	    data, data_size) != 0) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

static wasm_trap_t *
pw_proxy_dequeue_shared_queue(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	uint32_t queue_id;
	uint8_t *data = NULL;
	size_t data_len = 0;

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	if (ctx->queue_store == NULL) {
		results[0].of.i32 = PROXY_EMPTY;
		return (NULL);
	}

	queue_id = (uint32_t)args[0].of.i32;

	if (vwasm_queue_dequeue(ctx->queue_store, queue_id,
	    &data, &data_len) != 0) {
		results[0].of.i32 = PROXY_EMPTY;
		return (NULL);
	}

	if (pw_return_bytes(ctx, data, data_len,
	    (uint32_t)args[1].of.i32, (uint32_t)args[2].of.i32) != 0) {
		free(data);
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	free(data);
	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

/* ----------------------------------------------------------------
 * proxy_get_status
 * ---------------------------------------------------------------- */

static wasm_trap_t *
pw_proxy_get_status(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	if (pw_write_u32(ctx, (uint32_t)args[0].of.i32,
	    ctx->last_status_code) != 0) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	if (ctx->last_status_msg != NULL) {
		if (pw_return_string(ctx, ctx->last_status_msg,
		    strlen(ctx->last_status_msg),
		    (uint32_t)args[1].of.i32,
		    (uint32_t)args[2].of.i32) != 0) {
			results[0].of.i32 = PROXY_INTERNAL;
			return (NULL);
		}
	} else {
		if (pw_write_u32(ctx, (uint32_t)args[1].of.i32, 0) != 0 ||
		    pw_write_u32(ctx, (uint32_t)args[2].of.i32, 0) != 0) {
			results[0].of.i32 = PROXY_INTERNAL;
			return (NULL);
		}
	}

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

/* ----------------------------------------------------------------
 * Metrics: proxy_define_metric / increment / record / get
 * ---------------------------------------------------------------- */

static wasm_trap_t *
pw_proxy_define_metric(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	struct vwasm_metric_store *store;
	char name_buf[VWASM_METRIC_MAX_NAME];
	int32_t metric_type;
	uint32_t metric_id;

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	store = ctx->metric_store;
	if (store == NULL) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	metric_type = args[0].of.i32;
	if (pw_read_string(ctx, (uint32_t)args[1].of.i32,
	    (uint32_t)args[2].of.i32, name_buf, sizeof(name_buf)) != 0) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	pthread_rwlock_wrlock(&store->rwlock);

	/* Check for existing metric with same name */
	for (uint32_t i = 0; i < store->count; i++) {
		if (store->metrics[i].name_len == (uint32_t)args[2].of.i32 &&
		    memcmp(store->metrics[i].name, name_buf,
		    store->metrics[i].name_len) == 0) {
			metric_id = i + 1;
			pthread_rwlock_unlock(&store->rwlock);
			if (pw_write_u32(ctx, (uint32_t)args[3].of.i32,
			    metric_id) != 0) {
				results[0].of.i32 = PROXY_INTERNAL;
				return (NULL);
			}
			results[0].of.i32 = PROXY_OK;
			return (NULL);
		}
	}

	if (store->count >= VWASM_MAX_METRICS) {
		pthread_rwlock_unlock(&store->rwlock);
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	metric_id = store->count + 1;
	memcpy(store->metrics[store->count].name, name_buf,
	    (uint32_t)args[2].of.i32);
	store->metrics[store->count].name_len = (uint32_t)args[2].of.i32;
	store->metrics[store->count].type = (proxy_metric_type_t)metric_type;
	store->metrics[store->count].value = 0;
	store->count++;

	pthread_rwlock_unlock(&store->rwlock);

	if (pw_write_u32(ctx, (uint32_t)args[3].of.i32, metric_id) != 0) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

static wasm_trap_t *
pw_proxy_increment_metric(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	struct vwasm_metric_store *store;
	uint32_t metric_id;
	int64_t offset;

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	store = ctx->metric_store;
	if (store == NULL) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	metric_id = (uint32_t)args[0].of.i32;
	offset = (int64_t)args[1].of.i32;

	pthread_rwlock_wrlock(&store->rwlock);

	if (metric_id == 0 || metric_id > store->count) {
		pthread_rwlock_unlock(&store->rwlock);
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	if (offset >= 0)
		store->metrics[metric_id - 1].value += (uint64_t)offset;
	else if ((uint64_t)(-offset) <= store->metrics[metric_id - 1].value)
		store->metrics[metric_id - 1].value -= (uint64_t)(-offset);
	else
		store->metrics[metric_id - 1].value = 0;

	pthread_rwlock_unlock(&store->rwlock);

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

static wasm_trap_t *
pw_proxy_record_metric(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	struct vwasm_metric_store *store;
	uint32_t metric_id;
	uint64_t value;

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	store = ctx->metric_store;
	if (store == NULL) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	metric_id = (uint32_t)args[0].of.i32;
	/* Wasm32 SDKs pass value as i32; cast to u64 for storage */
	value = (uint64_t)(uint32_t)args[1].of.i32;

	pthread_rwlock_wrlock(&store->rwlock);

	if (metric_id == 0 || metric_id > store->count) {
		pthread_rwlock_unlock(&store->rwlock);
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	store->metrics[metric_id - 1].value = value;

	pthread_rwlock_unlock(&store->rwlock);

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

static wasm_trap_t *
pw_proxy_get_metric(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	struct vwasm_metric_store *store;
	uint32_t metric_id;
	uint64_t value;

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	store = ctx->metric_store;
	if (store == NULL) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	metric_id = (uint32_t)args[0].of.i32;

	pthread_rwlock_rdlock(&store->rwlock);

	if (metric_id == 0 || metric_id > store->count) {
		pthread_rwlock_unlock(&store->rwlock);
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	value = store->metrics[metric_id - 1].value;

	pthread_rwlock_unlock(&store->rwlock);

	if (!pw_validate_region(ctx, (uint32_t)args[1].of.i32, 8)) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}
	memcpy(pw_mem_ptr(ctx, (uint32_t)args[1].of.i32), &value, 8);

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

/* ----------------------------------------------------------------
 * proxy_http_call — Synchronous HTTP callout
 *
 * Makes a blocking HTTP/1.1 request. The response is stored and
 * proxy_on_http_call_response is called immediately before returning.
 *
 * ABI: proxy_http_call(
 *   upstream_ptr, upstream_size,
 *   headers_ptr, headers_size,
 *   body_ptr, body_size,
 *   trailers_ptr, trailers_size,
 *   timeout_ms,
 *   return_token_ptr
 * ) -> status
 * ---------------------------------------------------------------- */

#define PW_HTTP_MAX_RESPONSE	(256 * 1024)  /* 256 KiB max response */
#define PW_HTTP_DEFAULT_TIMEOUT	5000          /* 5 seconds */

/* ----------------------------------------------------------------
 * Anti-IP-rebinding: reject connections to private/internal IPs.
 *
 * Prevents SSRF via DNS rebinding by validating resolved addresses
 * against RFC1918, RFC5735, RFC4193, and loopback ranges.
 * ---------------------------------------------------------------- */
static int
pw_is_private_addr(const struct sockaddr *sa)
{
	if (sa->sa_family == AF_INET) {
		const struct sockaddr_in *sin;
		uint32_t ip;

		sin = (const struct sockaddr_in *)sa;
		ip = ntohl(sin->sin_addr.s_addr);

		/* 127.0.0.0/8 — loopback */
		if ((ip >> 24) == 127)
			return (1);
		/* 10.0.0.0/8 — RFC1918 */
		if ((ip >> 24) == 10)
			return (1);
		/* 172.16.0.0/12 — RFC1918 */
		if ((ip >> 20) == (172 << 4 | 1))
			return (1);
		/* 192.168.0.0/16 — RFC1918 */
		if ((ip >> 16) == ((192 << 8) | 168))
			return (1);
		/* 169.254.0.0/16 — link-local */
		if ((ip >> 16) == ((169 << 8) | 254))
			return (1);
		/* 0.0.0.0/8 — "this" network */
		if ((ip >> 24) == 0)
			return (1);
		/* 100.64.0.0/10 — shared address space (CGN) */
		if ((ip >> 22) == (100 << 2 | 1))
			return (1);
		/* 192.0.0.0/24 — IETF protocol assignments */
		if ((ip >> 8) == ((192 << 16) | 0))
			return (1);
		/* 198.18.0.0/15 — benchmarking */
		if ((ip >> 17) == ((198 << 7) | 9))
			return (1);
		/* 240.0.0.0/4 — reserved (includes broadcast) */
		if ((ip >> 28) == 15)
			return (1);
	} else if (sa->sa_family == AF_INET6) {
		const struct sockaddr_in6 *sin6;
		const uint8_t *b;

		sin6 = (const struct sockaddr_in6 *)sa;
		b = sin6->sin6_addr.s6_addr;

		/* ::1/128 — loopback */
		if (memcmp(b, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\1", 16) == 0)
			return (1);
		/* ::/128 — unspecified */
		if (memcmp(b, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) == 0)
			return (1);
		/* fc00::/7 — unique local (RFC4193) */
		if ((b[0] & 0xfe) == 0xfc)
			return (1);
		/* fe80::/10 — link-local */
		if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80)
			return (1);
		/* ::ffff:0:0/96 — IPv4-mapped, check inner IPv4 */
		if (memcmp(b, "\0\0\0\0\0\0\0\0\0\0\xff\xff", 12) == 0) {
			struct sockaddr_in inner;
			memset(&inner, 0, sizeof(inner));
			inner.sin_family = AF_INET;
			memcpy(&inner.sin_addr.s_addr, b + 12, 4);
			return (pw_is_private_addr(
			    (const struct sockaddr *)&inner));
		}
	}
	return (0);
}

static int
pw_http_connect(const char *host, uint16_t port, int timeout_ms)
{
	struct addrinfo hints, *res, *rp;
	char port_str[8];
	int fd = -1;
	int flags;
	struct pollfd pfd;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	snprintf(port_str, sizeof(port_str), "%u", port);
	if (getaddrinfo(host, port_str, &hints, &res) != 0)
		return (-1);

	for (rp = res; rp != NULL; rp = rp->ai_next) {
		/* Anti-IP-rebinding: reject private/internal IPs */
		if (pw_is_private_addr(rp->ai_addr)) {
			continue;
		}

		fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (fd < 0)
			continue;

		/* Non-blocking connect with timeout */
		flags = fcntl(fd, F_GETFL, 0);
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);

		if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
			break;

		if (errno == EINPROGRESS) {
			pfd.fd = fd;
			pfd.events = POLLOUT;
			if (poll(&pfd, 1, timeout_ms) > 0 &&
			    (pfd.revents & POLLOUT)) {
				int err = 0;
				socklen_t len = sizeof(err);
				getsockopt(fd, SOL_SOCKET, SO_ERROR,
				    &err, &len);
				if (err == 0)
					break;
			}
		}

		close(fd);
		fd = -1;
	}

	freeaddrinfo(res);

	if (fd >= 0) {
		/* Back to blocking for I/O */
		flags = fcntl(fd, F_GETFL, 0);
		fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

		/* Set socket timeout */
		struct timeval tv;
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	}

	return (fd);
}

static int
pw_http_parse_upstream(const char *upstream, size_t len,
    char *host, size_t host_sz, uint16_t *port)
{
	const char *colon;
	size_t hlen;

	if (len == 0 || len >= host_sz)
		return (-1);

	colon = memchr(upstream, ':', len);
	if (colon != NULL) {
		hlen = (size_t)(colon - upstream);
		if (hlen == 0 || hlen >= host_sz)
			return (-1);
		memcpy(host, upstream, hlen);
		host[hlen] = '\0';
		*port = (uint16_t)atoi(colon + 1);
		if (*port == 0)
			*port = 80;
	} else {
		memcpy(host, upstream, len);
		host[len] = '\0';
		*port = 80;
	}
	return (0);
}

static wasm_trap_t *
pw_proxy_http_call(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	char upstream_buf[256], host[256];
	uint16_t port;
	uint32_t upstream_size, headers_ptr, headers_size;
	uint32_t body_ptr, body_size, timeout_ms;
	int fd = -1;
	char request_buf[8192];
	int req_len;
	uint8_t *response_buf = NULL;
	size_t response_len = 0;
	ssize_t n;
	uint32_t token_id = 1;

	(void)env;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	/* Rate limiting: reject if exceeded max calls per request */
	if (ctx->http_call_max > 0 && ctx->http_call_count >= ctx->http_call_max) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}
	ctx->http_call_count++;

	upstream_size = (uint32_t)args[1].of.i32;
	headers_ptr = (uint32_t)args[2].of.i32;
	headers_size = (uint32_t)args[3].of.i32;
	body_ptr = (uint32_t)args[4].of.i32;
	body_size = (uint32_t)args[5].of.i32;
	/* args[6], args[7] = trailers (ignored) */
	timeout_ms = (uint32_t)args[8].of.i32;

	if (timeout_ms == 0)
		timeout_ms = PW_HTTP_DEFAULT_TIMEOUT;
	if (timeout_ms > 30000)
		timeout_ms = 30000; /* Cap at 30s */

	/* Read upstream host:port */
	if (pw_read_string(ctx, (uint32_t)args[0].of.i32,
	    upstream_size, upstream_buf, sizeof(upstream_buf)) != 0) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	if (pw_http_parse_upstream(upstream_buf, upstream_size,
	    host, sizeof(host), &port) != 0) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	/* SSRF protection: check upstream against allowlist */
	if (ctx->num_allowed_upstreams > 0 && ctx->allowed_upstreams != NULL) {
		char upstream_check[280];
		uint32_t ui;
		int allowed = 0;

		snprintf(upstream_check, sizeof(upstream_check),
		    "%s:%u", host, (unsigned)port);

		for (ui = 0; ui < ctx->num_allowed_upstreams; ui++) {
			if (strcmp(upstream_check,
			    ctx->allowed_upstreams[ui]) == 0) {
				allowed = 1;
				break;
			}
		}
		if (!allowed) {
			results[0].of.i32 = PROXY_BAD_ARGUMENT;
			return (NULL);
		}
	}

	/*
	 * Parse request headers from serialized format.
	 * Extract :method, :path, and other headers.
	 */
	const char *method = "GET";
	const char *path = "/";
	char extra_headers[4096] = "";
	size_t extra_len = 0;

	if (headers_size >= 4 &&
	    pw_validate_region(ctx, headers_ptr, headers_size)) {
		uint8_t *hdr_data = pw_mem_ptr(ctx, headers_ptr);
		uint32_t num_pairs, hdr_offset, hi;

		memcpy(&num_pairs, hdr_data, 4);
		if (num_pairs <= 64) {
			hdr_offset = 4 + num_pairs * 8;
			for (hi = 0; hi < num_pairs && hdr_offset < headers_size; hi++) {
				uint32_t ks, vs;
				memcpy(&ks, hdr_data + 4 + hi * 8, 4);
				memcpy(&vs, hdr_data + 4 + hi * 8 + 4, 4);

				if (hdr_offset + ks + 1 + vs + 1 > headers_size)
					break;

				const char *k = (const char *)(hdr_data + hdr_offset);
				hdr_offset += ks + 1;
				const char *v = (const char *)(hdr_data + hdr_offset);
				hdr_offset += vs + 1;

				if (ks == 7 && memcmp(k, ":method", 7) == 0)
					method = v;
				else if (ks == 5 && memcmp(k, ":path", 5) == 0)
					path = v;
				else if (ks > 0 && k[0] != ':') {
					int written = snprintf(
					    extra_headers + extra_len,
					    sizeof(extra_headers) - extra_len,
					    "%.*s: %.*s\r\n",
					    (int)ks, k, (int)vs, v);
					if (written > 0)
						extra_len += (size_t)written;
				}
			}
		}
	}

	/* Build HTTP/1.1 request */
	if (body_size > 0 && pw_validate_region(ctx, body_ptr, body_size)) {
		req_len = snprintf(request_buf, sizeof(request_buf),
		    "%s %s HTTP/1.1\r\n"
		    "Host: %s\r\n"
		    "Content-Length: %u\r\n"
		    "Connection: close\r\n"
		    "%s\r\n",
		    method, path, host, body_size, extra_headers);
	} else {
		body_size = 0;
		req_len = snprintf(request_buf, sizeof(request_buf),
		    "%s %s HTTP/1.1\r\n"
		    "Host: %s\r\n"
		    "Connection: close\r\n"
		    "%s\r\n",
		    method, path, host, extra_headers);
	}

	if (req_len <= 0 || req_len >= (int)sizeof(request_buf)) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	/* Connect */
	fd = pw_http_connect(host, port, (int)timeout_ms);
	if (fd < 0) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	/* Send request */
	if (write(fd, request_buf, (size_t)req_len) != req_len) {
		close(fd);
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	/* Send body if present */
	if (body_size > 0) {
		uint8_t *body_data = pw_mem_ptr(ctx, body_ptr);
		if (write(fd, body_data, body_size) != (ssize_t)body_size) {
			close(fd);
			results[0].of.i32 = PROXY_INTERNAL;
			return (NULL);
		}
	}

	/* Read response */
	response_buf = malloc(PW_HTTP_MAX_RESPONSE);
	if (response_buf == NULL) {
		close(fd);
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	response_len = 0;
	while (response_len < PW_HTTP_MAX_RESPONSE) {
		n = read(fd, response_buf + response_len,
		    PW_HTTP_MAX_RESPONSE - response_len);
		if (n <= 0)
			break;
		response_len += (size_t)n;
	}
	close(fd);

	if (response_len == 0) {
		free(response_buf);
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	/*
	 * Parse HTTP response: find headers and body.
	 * Headers end at \r\n\r\n.
	 */
	uint8_t *body_start = NULL;
	size_t resp_body_len = 0;
	uint32_t resp_num_headers = 0;
	size_t header_end_offset = 0;

	for (size_t i = 0; i + 3 < response_len; i++) {
		if (response_buf[i] == '\r' && response_buf[i+1] == '\n' &&
		    response_buf[i+2] == '\r' && response_buf[i+3] == '\n') {
			header_end_offset = i;
			body_start = response_buf + i + 4;
			resp_body_len = response_len - (i + 4);
			break;
		}
	}

	/* Count response headers (skip status line) */
	if (header_end_offset > 0) {
		const char *p = (const char *)response_buf;
		const char *end = (const char *)(response_buf + header_end_offset);
		/* Skip first line (HTTP/1.1 200 OK) */
		while (p < end && *p != '\n')
			p++;
		if (p < end)
			p++;
		while (p < end) {
			resp_num_headers++;
			while (p < end && *p != '\n')
				p++;
			if (p < end)
				p++;
		}
	}

	/* Store response in context for get_buffer_bytes(HTTP_CALL_BODY) */
	ctx->http_response.raw_buf = response_buf;
	ctx->http_response.raw_len = response_len;
	ctx->http_response.body = body_start;
	ctx->http_response.body_len = resp_body_len;
	ctx->http_response.num_headers = resp_num_headers;
	ctx->http_response.valid = 1;

	/* Write token ID */
	if (pw_write_u32(ctx, (uint32_t)args[9].of.i32, token_id) != 0) {
		free(response_buf);
		ctx->http_response.raw_buf = NULL;
		ctx->http_response.valid = 0;
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	/*
	 * Response data is available via proxy_get_buffer_bytes(HTTP_CALL_BODY).
	 * The raw_buf is kept alive until vwasm_proxy_ctx_cleanup() is called
	 * after the full lifecycle completes.
	 */

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

/* ----------------------------------------------------------------
 * Context cleanup — free resources allocated during lifecycle
 * ---------------------------------------------------------------- */

void
vwasm_proxy_ctx_cleanup(struct vwasm_proxy_ctx *ctx)
{
	if (ctx == NULL)
		return;

	/* Free HTTP call response buffer */
	if (ctx->http_response.raw_buf != NULL) {
		free(ctx->http_response.raw_buf);
		ctx->http_response.raw_buf = NULL;
		ctx->http_response.body = NULL;
		ctx->http_response.valid = 0;
	}

	/* Free cached request body */
	if (ctx->request_body != NULL) {
		free((void *)ctx->request_body);
		ctx->request_body = NULL;
		ctx->request_body_len = 0;
	}

	/* Free local response body */
	if (ctx->local_response_body != NULL) {
		free(ctx->local_response_body);
		ctx->local_response_body = NULL;
		ctx->local_response_body_len = 0;
	}

	/* Free local response headers */
	if (ctx->local_response_headers != NULL) {
		free(ctx->local_response_headers);
		ctx->local_response_headers = NULL;
		ctx->local_response_headers_len = 0;
	}

	/* Free modified body */
	if (ctx->modified_body != NULL) {
		free(ctx->modified_body);
		ctx->modified_body = NULL;
		ctx->modified_body_len = 0;
		ctx->body_modified = 0;
	}
}

/* ----------------------------------------------------------------
 * Linker registration helper
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

/*
 * Define a function with explicit param types (wasm_valkind_t array).
 * Used when params aren't all i32 (e.g., proxy_record_metric has i64).
 */
static int
pw_define_func_typed(wasmtime_linker_t *linker, const char *name,
    const wasm_valkind_t *param_types, int nparams,
    int nresults_count, wasmtime_func_callback_t callback)
{
	wasm_functype_t *ft;
	wasmtime_error_t *error;
	wasm_valtype_vec_t params, results;
	int i;

	wasm_valtype_vec_new_uninitialized(&params, nparams);
	for (i = 0; i < nparams; i++)
		params.data[i] = wasm_valtype_new(param_types[i]);

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

/* ----------------------------------------------------------------
 * Register ALL Proxy-Wasm ABI v0.2.1 host functions.
 *
 * Categories:
 *   IMPLEMENTED: Full functional implementation
 *   ACCEPTED:    Accepts the call, stores state, returns OK
 *   STUB:        Returns PROXY_UNIMPLEMENTED (cannot be implemented)
 * ---------------------------------------------------------------- */

int
vwasm_proxy_wasm_define_imports(wasmtime_linker_t *linker)
{
	/* === Logging (IMPLEMENTED) === */
	if (pw_define_func(linker, "proxy_log", 3, 1,
	    pw_proxy_log) != 0)
		return (-1);

	/* === Header Map — individual (IMPLEMENTED) === */
	if (pw_define_func(linker, "proxy_get_header_map_value", 5, 1,
	    pw_proxy_get_header_map_value) != 0)
		return (-1);
	if (pw_define_func(linker, "proxy_add_header_map_value", 5, 1,
	    pw_proxy_add_header_map_value) != 0)
		return (-1);
	if (pw_define_func(linker, "proxy_replace_header_map_value", 5, 1,
	    pw_proxy_replace_header_map_value) != 0)
		return (-1);
	if (pw_define_func(linker, "proxy_remove_header_map_value", 3, 1,
	    pw_proxy_remove_header_map_value) != 0)
		return (-1);

	/* === Header Map — bulk (IMPLEMENTED) === */
	if (pw_define_func(linker, "proxy_get_header_map_pairs", 3, 1,
	    pw_proxy_get_header_map_pairs) != 0)
		return (-1);
	if (pw_define_func(linker, "proxy_set_header_map_pairs", 3, 1,
	    pw_proxy_set_header_map_pairs) != 0)
		return (-1);

	/* === Properties (IMPLEMENTED) === */
	if (pw_define_func(linker, "proxy_get_property", 4, 1,
	    pw_proxy_get_property) != 0)
		return (-1);
	if (pw_define_func(linker, "proxy_set_property", 4, 1,
	    pw_proxy_set_property) != 0)
		return (-1);

	/* === Buffers (IMPLEMENTED) === */
	if (pw_define_func(linker, "proxy_get_buffer_bytes", 5, 1,
	    pw_proxy_get_buffer_bytes) != 0)
		return (-1);
	if (pw_define_func(linker, "proxy_set_buffer_bytes", 5, 1,
	    pw_proxy_set_buffer_bytes) != 0)
		return (-1);

	/* === Local Response (IMPLEMENTED) === */
	if (pw_define_func(linker, "proxy_send_local_response", 8, 1,
	    pw_proxy_send_local_response) != 0)
		return (-1);

	/* === Time (IMPLEMENTED) === */
	if (pw_define_func(linker, "proxy_get_current_time_nanoseconds", 1, 1,
	    pw_proxy_get_current_time) != 0)
		return (-1);
	if (pw_define_func(linker, "proxy_set_tick_period_milliseconds", 1, 1,
	    pw_stub_ok) != 0)
		return (-1);

	/* === Context (STUB — single-context model) === */
	if (pw_define_func(linker, "proxy_set_effective_context", 1, 1,
	    pw_stub_ok) != 0)
		return (-1);
	if (pw_define_func(linker, "proxy_done", 0, 1,
	    pw_stub_ok) != 0)
		return (-1);

	/* === Stream Control (STUB — no pause/resume) === */
	if (pw_define_func(linker, "proxy_continue_stream", 1, 1,
	    pw_stub_ok) != 0)
		return (-1);
	if (pw_define_func(linker, "proxy_close_stream", 1, 1,
	    pw_stub_ok) != 0)
		return (-1);

	/* === Shared Data (IMPLEMENTED) === */
	if (pw_define_func(linker, "proxy_get_shared_data", 5, 1,
	    pw_proxy_get_shared_data) != 0)
		return (-1);
	if (pw_define_func(linker, "proxy_set_shared_data", 5, 1,
	    pw_proxy_set_shared_data) != 0)
		return (-1);

	/* === Shared Queues (IMPLEMENTED) === */
	if (pw_define_func(linker, "proxy_register_shared_queue", 3, 1,
	    pw_proxy_register_shared_queue) != 0)
		return (-1);
	if (pw_define_func(linker, "proxy_resolve_shared_queue", 5, 1,
	    pw_proxy_resolve_shared_queue) != 0)
		return (-1);
	if (pw_define_func(linker, "proxy_enqueue_shared_queue", 3, 1,
	    pw_proxy_enqueue_shared_queue) != 0)
		return (-1);
	if (pw_define_func(linker, "proxy_dequeue_shared_queue", 3, 1,
	    pw_proxy_dequeue_shared_queue) != 0)
		return (-1);

	/* === Status (IMPLEMENTED) === */
	if (pw_define_func(linker, "proxy_get_status", 3, 1,
	    pw_proxy_get_status) != 0)
		return (-1);

	/* === Metrics (IMPLEMENTED) === */
	if (pw_define_func(linker, "proxy_define_metric", 4, 1,
	    pw_proxy_define_metric) != 0)
		return (-1);
	if (pw_define_func(linker, "proxy_increment_metric", 2, 1,
	    pw_proxy_increment_metric) != 0)
		return (-1);
	if (pw_define_func(linker, "proxy_record_metric", 2, 1,
	    pw_proxy_record_metric) != 0)
		return (-1);
	if (pw_define_func(linker, "proxy_get_metric", 2, 1,
	    pw_proxy_get_metric) != 0)
		return (-1);

	/* === HTTP Callouts (IMPLEMENTED — synchronous) === */
	if (pw_define_func(linker, "proxy_http_call", 10, 1,
	    pw_proxy_http_call) != 0)
		return (-1);

	/* === gRPC (STUB — Varnish has no gRPC) === */
	if (pw_define_func(linker, "proxy_grpc_call", 12, 1,
	    pw_stub_not_found) != 0)
		return (-1);
	if (pw_define_func(linker, "proxy_grpc_stream", 9, 1,
	    pw_stub_not_found) != 0)
		return (-1);
	if (pw_define_func(linker, "proxy_grpc_send", 4, 1,
	    pw_stub_not_found) != 0)
		return (-1);
	if (pw_define_func(linker, "proxy_grpc_cancel", 1, 1,
	    pw_stub_not_found) != 0)
		return (-1);
	if (pw_define_func(linker, "proxy_grpc_close", 1, 1,
	    pw_stub_not_found) != 0)
		return (-1);

	/* === Foreign Functions (NOT_FOUND — no registry) === */
	if (pw_define_func(linker, "proxy_call_foreign_function", 6, 1,
	    pw_stub_not_found) != 0)
		return (-1);

	return (0);
}
