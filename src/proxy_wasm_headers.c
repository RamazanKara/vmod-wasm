/*-
 * Copyright (c) 2026 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Proxy-Wasm header map operations.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <wasm.h>
#include <wasmtime.h>

#include "cache/cache.h"
#include "vcl.h"

#include "proxy_wasm.h"
#include "proxy_wasm_mem.h"

/* ----------------------------------------------------------------
 * Header map resolution
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
		return (NULL);
	default:
		return (NULL);
	}
}

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

/* ----------------------------------------------------------------
 * proxy_get_header_map_value
 * ---------------------------------------------------------------- */

wasm_trap_t *
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
	(void)nargs;
	(void)nresults;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;
	fprintf(stderr, "VMOD-WASM-DEBUG: get_header_map_value map=%d\n",
	    args[0].of.i32);
	fflush(stderr);

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

wasm_trap_t *
pw_proxy_add_header_map_value(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	struct http *hp;
	char key_buf[256], val_buf[4096], hdr_line[4352];

	(void)env;
	(void)nargs;
	(void)nresults;
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

wasm_trap_t *
pw_proxy_replace_header_map_value(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	struct http *hp;
	char key_buf[256], val_buf[4096], hdr_search[260], hdr_line[4352];
	int i;

	(void)env;
	(void)nargs;
	(void)nresults;
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

	i = snprintf(hdr_search + 1, sizeof(hdr_search) - 1, "%s:", key_buf);
	if (i > 0 && i < (int)(sizeof(hdr_search) - 1)) {
		hdr_search[0] = (char)i;
		http_Unset(hp, (hdr_t)hdr_search);
	}

	snprintf(hdr_line, sizeof(hdr_line), "%s: %s", key_buf, val_buf);
	http_SetHeader(hp, hdr_line);

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

/* ----------------------------------------------------------------
 * proxy_remove_header_map_value
 * ---------------------------------------------------------------- */

wasm_trap_t *
pw_proxy_remove_header_map_value(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	struct http *hp;
	char key_buf[256], hdr_search[260];
	int i;

	(void)env;
	(void)nargs;
	(void)nresults;
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
 * ---------------------------------------------------------------- */

wasm_trap_t *
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
	(void)nargs;
	(void)nresults;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	hp = pw_get_header_map(ctx, args[0].of.i32);
	if (hp == NULL) {
		if (pw_return_bytes(ctx, NULL, 0,
		    (uint32_t)args[1].of.i32,
		    (uint32_t)args[2].of.i32) != 0) {
			results[0].of.i32 = PROXY_INTERNAL;
			return (NULL);
		}
		results[0].of.i32 = PROXY_OK;
		return (NULL);
	}

	map_type = args[0].of.i32;
	pseudo_keys[0] = NULL;
	pseudo_keys[1] = NULL;
	pseudo_keys[2] = NULL;
	pseudo_vals[0] = NULL;
	pseudo_vals[1] = NULL;
	pseudo_vals[2] = NULL;
	num_pseudo = 0;

	if (map_type == 0) {
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
	} else if (map_type == 1) {
		if (hp->hd[HTTP_HDR_STATUS].b != NULL) {
			pseudo_keys[num_pseudo] = ":status";
			pseudo_vals[num_pseudo] = hp->hd[HTTP_HDR_STATUS].b;
			num_pseudo++;
		}
	}

	num_headers = hp->nhd - HTTP_HDR_FIRST + num_pseudo;

	total_size = 4 + (size_t)num_headers * 8;

	for (i = 0; i < num_pseudo; i++) {
		total_size += strlen(pseudo_keys[i]) + 1 +
		    strlen(pseudo_vals[i]) + 1;
	}

	for (i = 0; i < (uint32_t)(hp->nhd - HTTP_HDR_FIRST); i++) {
		const char *hdr_line;
		const char *colon;

		idx = HTTP_HDR_FIRST + i;
		if (idx >= hp->nhd)
			break;
		hdr_line = hp->hd[idx].b;
		if (hdr_line == NULL) {
			total_size += 2; /* two null terminators */
			continue;
		}
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

	memcpy(buf, &num_headers, 4);
	offset = 4;

	for (i = 0; i < num_pseudo; i++) {
		uint32_t key_len = (uint32_t)strlen(pseudo_keys[i]);
		uint32_t val_len = (uint32_t)strlen(pseudo_vals[i]);
		memcpy(buf + offset, &key_len, 4);
		offset += 4;
		memcpy(buf + offset, &val_len, 4);
		offset += 4;
	}

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

	for (i = 0; i < (uint32_t)(hp->nhd - HTTP_HDR_FIRST); i++) {
		const char *hdr_line;
		const char *colon;

		idx = HTTP_HDR_FIRST + i;
		if (idx >= hp->nhd)
			break;
		hdr_line = hp->hd[idx].b;
		if (hdr_line == NULL) {
			buf[offset++] = '\0';
			buf[offset++] = '\0';
			continue;
		}

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

wasm_trap_t *
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
	(void)nargs;
	(void)nresults;
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

	for (i = HTTP_HDR_FIRST; i < hp->nhd; i++) {
		hp->hd[i].b = NULL;
		hp->hd[i].e = NULL;
	}
	hp->nhd = HTTP_HDR_FIRST;

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
