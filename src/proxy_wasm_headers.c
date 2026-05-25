/*-
 * Copyright (c) 2025 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Proxy-Wasm header map operations.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "cache/cache.h"
#include "vcl.h"

#include <wasm.h>
#include <wasmtime.h>

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
                return (NULL);  /* trailers handled separately */
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
 * Trailer map helpers
 * ---------------------------------------------------------------- */

static int
pw_is_trailer_map(int32_t map_type)
{
	return (map_type == PROXY_MAP_HTTP_REQUEST_TRAILERS ||
	    map_type == PROXY_MAP_HTTP_RESPONSE_TRAILERS);
}

static int
pw_is_known_map(int32_t map_type)
{
	return (map_type == PROXY_MAP_HTTP_REQUEST_HEADERS ||
	    map_type == PROXY_MAP_HTTP_REQUEST_TRAILERS ||
	    map_type == PROXY_MAP_HTTP_RESPONSE_HEADERS ||
	    map_type == PROXY_MAP_HTTP_RESPONSE_TRAILERS ||
	    map_type == PROXY_MAP_GRPC_CALL_INITIAL_MD ||
	    map_type == PROXY_MAP_GRPC_CALL_TRAILING_MD ||
	    map_type == PROXY_MAP_HTTP_CALL_RESP_HEADERS ||
	    map_type == PROXY_MAP_HTTP_CALL_RESP_TRAILERS);
}

static struct vwasm_trailer_map *
pw_get_trailer_map(struct vwasm_proxy_ctx *ctx, int32_t map_type)
{
	if (map_type == PROXY_MAP_HTTP_REQUEST_TRAILERS)
		return (&ctx->request_trailers);
	if (map_type == PROXY_MAP_HTTP_RESPONSE_TRAILERS)
		return (&ctx->response_trailers);
	return (NULL);
}

static const char *
pw_trailer_get(const struct vwasm_trailer_map *tm, const char *name,
    size_t name_len, size_t *val_len_out)
{
	uint32_t i;

	for (i = 0; i < tm->count; i++) {
		if (tm->entries[i].name_len == name_len &&
		    strncasecmp(tm->entries[i].name, name, name_len) == 0) {
			if (val_len_out != NULL)
				*val_len_out = tm->entries[i].value_len;
			return (tm->entries[i].value);
		}
	}
	return (NULL);
}

static int
pw_trailer_add(struct vwasm_trailer_map *tm, const char *name,
    size_t name_len, const char *value, size_t value_len)
{
	struct vwasm_trailer_entry *e;

	if (tm->count >= VWASM_MAX_TRAILERS)
		return (-1);

	e = &tm->entries[tm->count];
	e->name = malloc(name_len + 1);
	if (e->name == NULL)
		return (-1);
	memcpy(e->name, name, name_len);
	e->name[name_len] = '\0';
	e->name_len = name_len;

	e->value = malloc(value_len + 1);
	if (e->value == NULL) {
		free(e->name);
		e->name = NULL;
		return (-1);
	}
	memcpy(e->value, value, value_len);
	e->value[value_len] = '\0';
	e->value_len = value_len;

	tm->count++;
	return (0);
}

static void
pw_trailer_remove(struct vwasm_trailer_map *tm, const char *name,
    size_t name_len)
{
	uint32_t i;

	for (i = 0; i < tm->count; i++) {
		if (tm->entries[i].name_len == name_len &&
		    strncasecmp(tm->entries[i].name, name, name_len) == 0) {
			free(tm->entries[i].name);
			free(tm->entries[i].value);
			if (i < tm->count - 1) {
				tm->entries[i] = tm->entries[tm->count - 1];
			}
			tm->count--;
			return;
		}
	}
}

static void
pw_trailer_clear(struct vwasm_trailer_map *tm)
{
	uint32_t i;

	for (i = 0; i < tm->count; i++) {
		free(tm->entries[i].name);
		free(tm->entries[i].value);
	}
	tm->count = 0;
}

/* ----------------------------------------------------------------
 * HTTP call response header helpers
 *
 * Parse raw HTTP response stored in ctx->active_http_call->response
 * to extract headers.  The raw format is:
 * "HTTP/1.1 200 OK\r\n<headers>\r\n\r\n<body>"
 * ---------------------------------------------------------------- */

/*
 * Find a header value in the raw HTTP call response.
 * For ":status" pseudo-header, returns the status code string.
 * Returns pointer to value (within raw_buf) and sets *val_len.
 * Returns NULL if not found.
 */
static const char *
pw_http_call_response_find_header(const struct vwasm_proxy_ctx *ctx,
    const char *key, size_t *val_len)
{
	const char *raw, *end, *p, *line_end;
	size_t key_len;

	if (ctx->active_http_call == NULL ||
	    !ctx->active_http_call->response.valid ||
	    ctx->active_http_call->response.raw_buf == NULL)
		return (NULL);

	raw = (const char *)ctx->active_http_call->response.raw_buf;
	end = raw + ctx->active_http_call->response.raw_len;

	/* Find end of status line */
	p = raw;
	line_end = strstr(p, "\r\n");
	if (line_end == NULL)
		return (NULL);

	/* Handle :status pseudo-header */
	if (strcmp(key, ":status") == 0) {
		/* Parse "HTTP/1.x <status> <reason>" */
		const char *sp = memchr(p, ' ', (size_t)(line_end - p));
		if (sp == NULL)
			return (NULL);
		sp++;
		const char *sp2 = memchr(sp, ' ', (size_t)(line_end - sp));
		if (sp2 == NULL)
			sp2 = line_end;
		*val_len = (size_t)(sp2 - sp);
		return (sp);
	}

	/* Search headers after status line */
	key_len = strlen(key);
	p = line_end + 2;
	while (p < end) {
		line_end = strstr(p, "\r\n");
		if (line_end == NULL || line_end == p)
			break; /* end of headers */

		/* Check if this line matches "key: value" */
		if ((size_t)(line_end - p) > key_len + 1 &&
		    strncasecmp(p, key, key_len) == 0 &&
		    p[key_len] == ':') {
			const char *v = p + key_len + 1;
			while (v < line_end && (*v == ' ' || *v == '\t'))
				v++;
			*val_len = (size_t)(line_end - v);
			return (v);
		}
		p = line_end + 2;
	}
	return (NULL);
}

static size_t
pw_http_call_response_headers_size(const struct vwasm_proxy_ctx *ctx)
{
	const char *raw, *raw_end, *p, *line_end;
	size_t size;
	uint32_t count;

	if (ctx->active_http_call == NULL ||
	    !ctx->active_http_call->response.valid ||
	    ctx->active_http_call->response.raw_buf == NULL)
		return (0);

	raw = (const char *)ctx->active_http_call->response.raw_buf;
	raw_end = raw + ctx->active_http_call->response.raw_len;
	count = 0;
	size = 4;

	line_end = strstr(raw, "\r\n");
	if (line_end != NULL) {
		const char *sp = memchr(raw, ' ', (size_t)(line_end - raw));
		if (sp != NULL) {
			const char *sp2;
			sp++;
			sp2 = memchr(sp, ' ', (size_t)(line_end - sp));
			if (sp2 == NULL)
				sp2 = line_end;
			count++;
			size += 8 + strlen(":status") + 1 +
			    (size_t)(sp2 - sp) + 1;
		}
		p = line_end + 2;
	} else {
		p = raw_end;
	}

	while (p < raw_end && count < 64) {
		const char *colon;

		line_end = strstr(p, "\r\n");
		if (line_end == NULL || line_end == p)
			break;
		colon = memchr(p, ':', (size_t)(line_end - p));
		if (colon != NULL) {
			const char *v = colon + 1;
			while (v < line_end && (*v == ' ' || *v == '\t'))
				v++;
			count++;
			size += 8 + (size_t)(colon - p) + 1 +
			    (size_t)(line_end - v) + 1;
		}
		p = line_end + 2;
	}

	return (count == 0 ? 0 : size);
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
	int32_t map_type;

	(void)env;
	(void)nargs;
	(void)nresults;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	map_type = args[0].of.i32;

	if (pw_read_string(ctx, (uint32_t)args[1].of.i32,
	    (uint32_t)args[2].of.i32, key_buf, sizeof(key_buf)) != 0) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	/* Handle trailer maps */
	if (pw_is_trailer_map(map_type)) {
		struct vwasm_trailer_map *tm = pw_get_trailer_map(ctx, map_type);
		size_t val_len;

		if (tm == NULL) {
			results[0].of.i32 = PROXY_BAD_ARGUMENT;
			return (NULL);
		}
		val = pw_trailer_get(tm, key_buf, strlen(key_buf), &val_len);
		if (val == NULL) {
			results[0].of.i32 = PROXY_NOT_FOUND;
			return (NULL);
		}
		if (pw_return_string(ctx, val, val_len,
		    (uint32_t)args[3].of.i32,
		    (uint32_t)args[4].of.i32) != 0) {
			results[0].of.i32 = PROXY_INTERNAL;
			return (NULL);
		}
		results[0].of.i32 = PROXY_OK;
		return (NULL);
	}

	/* Handle HTTP call response headers (map_type 6) */
	if (map_type == PROXY_MAP_HTTP_CALL_RESP_HEADERS) {
		size_t hval_len;
		const char *hval;

		hval = pw_http_call_response_find_header(ctx, key_buf,
		    &hval_len);
		if (hval == NULL) {
			results[0].of.i32 = PROXY_NOT_FOUND;
			return (NULL);
		}
		if (pw_return_string(ctx, hval, hval_len,
		    (uint32_t)args[3].of.i32,
		    (uint32_t)args[4].of.i32) != 0) {
			results[0].of.i32 = PROXY_INTERNAL;
			return (NULL);
		}
		results[0].of.i32 = PROXY_OK;
		return (NULL);
	}
	if (map_type == PROXY_MAP_HTTP_CALL_RESP_TRAILERS ||
	    map_type == PROXY_MAP_GRPC_CALL_INITIAL_MD ||
	    map_type == PROXY_MAP_GRPC_CALL_TRAILING_MD) {
		results[0].of.i32 = PROXY_NOT_FOUND;
		return (NULL);
	}

	hp = pw_get_header_map(ctx, map_type);
	if (hp == NULL) {
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
	const char *ws_hdr;
	int32_t map_type;

	(void)env;
	(void)nargs;
	(void)nresults;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	map_type = args[0].of.i32;

	if (pw_read_string(ctx, (uint32_t)args[1].of.i32,
	    (uint32_t)args[2].of.i32, key_buf, sizeof(key_buf)) != 0 ||
	    pw_read_string(ctx, (uint32_t)args[3].of.i32,
	    (uint32_t)args[4].of.i32, val_buf, sizeof(val_buf)) != 0) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	/* Handle trailer maps */
	if (pw_is_trailer_map(map_type)) {
		struct vwasm_trailer_map *tm = pw_get_trailer_map(ctx, map_type);
		if (tm == NULL) {
			results[0].of.i32 = PROXY_BAD_ARGUMENT;
			return (NULL);
		}
		if (pw_trailer_add(tm, key_buf, strlen(key_buf),
		    val_buf, strlen(val_buf)) != 0) {
			results[0].of.i32 = PROXY_INTERNAL;
			return (NULL);
		}
		results[0].of.i32 = PROXY_OK;
		return (NULL);
	}

	hp = pw_get_header_map_mutable(ctx, map_type);
	if (hp == NULL) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	snprintf(hdr_line, sizeof(hdr_line), "%s: %s", key_buf, val_buf);
	ws_hdr = WS_Copy(ctx->vrt_ctx->ws, hdr_line,
	    (int)(strlen(hdr_line) + 1));
	if (ws_hdr == NULL) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}
	http_SetHeader(hp, ws_hdr);

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
	const char *ws_hdr;
	int i;
	int32_t map_type;

	(void)env;
	(void)nargs;
	(void)nresults;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	map_type = args[0].of.i32;

	if (pw_read_string(ctx, (uint32_t)args[1].of.i32,
	    (uint32_t)args[2].of.i32, key_buf, sizeof(key_buf)) != 0 ||
	    pw_read_string(ctx, (uint32_t)args[3].of.i32,
	    (uint32_t)args[4].of.i32, val_buf, sizeof(val_buf)) != 0) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	/* Handle trailer maps */
	if (pw_is_trailer_map(map_type)) {
		struct vwasm_trailer_map *tm = pw_get_trailer_map(ctx, map_type);
		if (tm == NULL) {
			results[0].of.i32 = PROXY_BAD_ARGUMENT;
			return (NULL);
		}
		pw_trailer_remove(tm, key_buf, strlen(key_buf));
		pw_trailer_add(tm, key_buf, strlen(key_buf),
		    val_buf, strlen(val_buf));
		results[0].of.i32 = PROXY_OK;
		return (NULL);
	}

	hp = pw_get_header_map_mutable(ctx, map_type);
	if (hp == NULL) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	i = snprintf(hdr_search + 1, sizeof(hdr_search) - 1, "%s:", key_buf);
	if (i > 0 && i < (int)(sizeof(hdr_search) - 1)) {
		hdr_search[0] = (char)i;
		http_Unset(hp, (hdr_t)hdr_search);
	}

	snprintf(hdr_line, sizeof(hdr_line), "%s: %s", key_buf, val_buf);
	ws_hdr = WS_Copy(ctx->vrt_ctx->ws, hdr_line,
	    (int)(strlen(hdr_line) + 1));
	if (ws_hdr == NULL) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}
	http_SetHeader(hp, ws_hdr);

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
	int32_t map_type;

	(void)env;
	(void)nargs;
	(void)nresults;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	map_type = args[0].of.i32;

	if (pw_read_string(ctx, (uint32_t)args[1].of.i32,
	    (uint32_t)args[2].of.i32, key_buf, sizeof(key_buf)) != 0) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	/* Handle trailer maps */
	if (pw_is_trailer_map(map_type)) {
		struct vwasm_trailer_map *tm = pw_get_trailer_map(ctx, map_type);
		if (tm == NULL) {
			results[0].of.i32 = PROXY_BAD_ARGUMENT;
			return (NULL);
		}
		pw_trailer_remove(tm, key_buf, strlen(key_buf));
		results[0].of.i32 = PROXY_OK;
		return (NULL);
	}

	hp = pw_get_header_map_mutable(ctx, map_type);
	if (hp == NULL) {
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

	map_type = args[0].of.i32;

	/* Handle trailer maps */
	if (pw_is_trailer_map(map_type)) {
		struct vwasm_trailer_map *tm = pw_get_trailer_map(ctx, map_type);
		uint8_t *tbuf;
		size_t tsize;
		uint32_t toffset;

		if (tm == NULL || tm->count == 0) {
			if (pw_return_bytes(ctx, NULL, 0,
			    (uint32_t)args[1].of.i32,
			    (uint32_t)args[2].of.i32) != 0) {
				results[0].of.i32 = PROXY_INTERNAL;
				return (NULL);
			}
			results[0].of.i32 = PROXY_OK;
			return (NULL);
		}

		/* Calculate size: 4 (count) + count*8 (key/val sizes) + data */
		tsize = 4 + (size_t)tm->count * 8;
		for (i = 0; i < tm->count; i++)
			tsize += tm->entries[i].name_len + 1 +
			    tm->entries[i].value_len + 1;

		tbuf = malloc(tsize);
		if (tbuf == NULL) {
			results[0].of.i32 = PROXY_INTERNAL;
			return (NULL);
		}

		memcpy(tbuf, &tm->count, 4);
		toffset = 4;
		for (i = 0; i < tm->count; i++) {
			uint32_t klen = (uint32_t)tm->entries[i].name_len;
			uint32_t vlen = (uint32_t)tm->entries[i].value_len;
			memcpy(tbuf + toffset, &klen, 4);
			toffset += 4;
			memcpy(tbuf + toffset, &vlen, 4);
			toffset += 4;
		}
		for (i = 0; i < tm->count; i++) {
			memcpy(tbuf + toffset, tm->entries[i].name,
			    tm->entries[i].name_len);
			toffset += (uint32_t)tm->entries[i].name_len;
			tbuf[toffset++] = '\0';
			memcpy(tbuf + toffset, tm->entries[i].value,
			    tm->entries[i].value_len);
			toffset += (uint32_t)tm->entries[i].value_len;
			tbuf[toffset++] = '\0';
		}

		if (pw_return_bytes(ctx, tbuf, toffset,
		    (uint32_t)args[1].of.i32,
		    (uint32_t)args[2].of.i32) != 0) {
			free(tbuf);
			results[0].of.i32 = PROXY_INTERNAL;
			return (NULL);
		}
		free(tbuf);
		results[0].of.i32 = PROXY_OK;
		return (NULL);
	}

	/* Handle HTTP call response headers (map_type 6) */
	if (map_type == PROXY_MAP_HTTP_CALL_RESP_HEADERS) {
		const char *raw, *raw_end, *p, *line_end;
		uint8_t *hbuf;
		size_t hsize;
		uint32_t hcount, hoffset;
		struct { const char *k; size_t kl; const char *v; size_t vl; } hdrs[64];

		if (ctx->active_http_call == NULL ||
		    !ctx->active_http_call->response.valid ||
		    ctx->active_http_call->response.raw_buf == NULL) {
			if (pw_return_bytes(ctx, NULL, 0,
			    (uint32_t)args[1].of.i32,
			    (uint32_t)args[2].of.i32) != 0) {
				results[0].of.i32 = PROXY_INTERNAL;
				return (NULL);
			}
			results[0].of.i32 = PROXY_OK;
			return (NULL);
		}

		raw = (const char *)ctx->active_http_call->response.raw_buf;
		raw_end = raw + ctx->active_http_call->response.raw_len;
		hcount = 0;

		/* Parse status line → :status pseudo-header */
		line_end = strstr(raw, "\r\n");
		if (line_end != NULL) {
			const char *sp = memchr(raw, ' ',
			    (size_t)(line_end - raw));
			if (sp != NULL) {
				sp++;
				const char *sp2 = memchr(sp, ' ',
				    (size_t)(line_end - sp));
				if (sp2 == NULL) sp2 = line_end;
				hdrs[hcount].k = ":status";
				hdrs[hcount].kl = 7;
				hdrs[hcount].v = sp;
				hdrs[hcount].vl = (size_t)(sp2 - sp);
				hcount++;
			}
			p = line_end + 2;
		} else {
			p = raw_end;
		}

		/* Parse remaining headers */
		while (p < raw_end && hcount < 64) {
			line_end = strstr(p, "\r\n");
			if (line_end == NULL || line_end == p)
				break;
			const char *colon = memchr(p, ':',
			    (size_t)(line_end - p));
			if (colon != NULL) {
				const char *v = colon + 1;
				while (v < line_end &&
				    (*v == ' ' || *v == '\t'))
					v++;
				hdrs[hcount].k = p;
				hdrs[hcount].kl = (size_t)(colon - p);
				hdrs[hcount].v = v;
				hdrs[hcount].vl = (size_t)(line_end - v);
				hcount++;
			}
			p = line_end + 2;
		}

		/* Serialize in proxy-wasm format */
		hsize = 4 + (size_t)hcount * 8;
		for (i = 0; i < hcount; i++)
			hsize += hdrs[i].kl + 1 + hdrs[i].vl + 1;

		hbuf = malloc(hsize);
		if (hbuf == NULL) {
			results[0].of.i32 = PROXY_INTERNAL;
			return (NULL);
		}

		memcpy(hbuf, &hcount, 4);
		hoffset = 4;
		for (i = 0; i < hcount; i++) {
			uint32_t kl = (uint32_t)hdrs[i].kl;
			uint32_t vl = (uint32_t)hdrs[i].vl;
			memcpy(hbuf + hoffset, &kl, 4); hoffset += 4;
			memcpy(hbuf + hoffset, &vl, 4); hoffset += 4;
		}
		for (i = 0; i < hcount; i++) {
			memcpy(hbuf + hoffset, hdrs[i].k, hdrs[i].kl);
			hoffset += (uint32_t)hdrs[i].kl;
			hbuf[hoffset++] = '\0';
			memcpy(hbuf + hoffset, hdrs[i].v, hdrs[i].vl);
			hoffset += (uint32_t)hdrs[i].vl;
			hbuf[hoffset++] = '\0';
		}

		if (pw_return_bytes(ctx, hbuf, hoffset,
		    (uint32_t)args[1].of.i32,
		    (uint32_t)args[2].of.i32) != 0) {
			free(hbuf);
			results[0].of.i32 = PROXY_INTERNAL;
			return (NULL);
		}
		free(hbuf);
		results[0].of.i32 = PROXY_OK;
		return (NULL);
	}
	if (map_type == PROXY_MAP_HTTP_CALL_RESP_TRAILERS ||
	    map_type == PROXY_MAP_GRPC_CALL_INITIAL_MD ||
	    map_type == PROXY_MAP_GRPC_CALL_TRAILING_MD) {
		if (pw_return_bytes(ctx, NULL, 0,
		    (uint32_t)args[1].of.i32,
		    (uint32_t)args[2].of.i32) != 0) {
			results[0].of.i32 = PROXY_INTERNAL;
			return (NULL);
		}
		results[0].of.i32 = PROXY_OK;
		return (NULL);
	}

	hp = pw_get_header_map(ctx, map_type);
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
	} else if (map_type == PROXY_MAP_HTTP_RESPONSE_HEADERS) {
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
	const char *ws_hdr;

	(void)env;
	(void)nargs;
	(void)nresults;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	map_type = (uint32_t)args[0].of.i32;
	data_ptr = (uint32_t)args[1].of.i32;
	data_size = (uint32_t)args[2].of.i32;

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

	/* Handle trailer maps */
	if (pw_is_trailer_map((int32_t)map_type)) {
		struct vwasm_trailer_map *tm =
		    pw_get_trailer_map(ctx, (int32_t)map_type);
		if (tm == NULL) {
			results[0].of.i32 = PROXY_BAD_ARGUMENT;
			return (NULL);
		}
		pw_trailer_clear(tm);
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

			if (key_size > 0 && key_size < 256 &&
			    val_size < 4096)
				pw_trailer_add(tm, key, key_size,
				    val, val_size);
		}
		results[0].of.i32 = PROXY_OK;
		return (NULL);
	}

	hp = pw_get_header_map_mutable(ctx, (int32_t)map_type);
	if (hp == NULL) {
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
			ws_hdr = WS_Copy(ctx->vrt_ctx->ws, hdr_line,
			    (int)(strlen(hdr_line) + 1));
			if (ws_hdr == NULL) {
				results[0].of.i32 = PROXY_INTERNAL;
				return (NULL);
			}
			http_SetHeader(hp, ws_hdr);
		}
	}

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}

/* ----------------------------------------------------------------
 * proxy_get_header_map_size — return serialized map size
 *
 * ABI: proxy_get_header_map_size(map_type, return_size_ptr) -> status
 * ---------------------------------------------------------------- */

wasm_trap_t *
pw_proxy_get_header_map_size(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	int32_t map_type;
	uint32_t serialized_size = 0;
	uint32_t i;

	(void)env;
	(void)nargs;
	(void)nresults;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	map_type = args[0].of.i32;

	if (!pw_is_known_map(map_type)) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	if (pw_is_trailer_map(map_type)) {
		const struct vwasm_trailer_map *tm =
		    pw_get_trailer_map(ctx, map_type);
		if (tm != NULL && tm->count > 0) {
			serialized_size = 4 + tm->count * 8;
			for (i = 0; i < tm->count; i++)
				serialized_size +=
				    (uint32_t)tm->entries[i].name_len + 1 +
				    (uint32_t)tm->entries[i].value_len + 1;
		}
	} else if (map_type == PROXY_MAP_HTTP_CALL_RESP_HEADERS) {
		serialized_size = (uint32_t)
		    pw_http_call_response_headers_size(ctx);
	} else if (map_type == PROXY_MAP_HTTP_CALL_RESP_TRAILERS) {
		serialized_size = 0;
	} else if (map_type == PROXY_MAP_GRPC_CALL_INITIAL_MD ||
	    map_type == PROXY_MAP_GRPC_CALL_TRAILING_MD) {
		results[0].of.i32 = PROXY_NOT_FOUND;
		return (NULL);
	} else {
		const struct http *hp = pw_get_header_map(ctx, map_type);
		if (hp != NULL) {
			uint32_t num_headers, data_size;

			num_headers = (uint32_t)(hp->nhd - HTTP_HDR_FIRST);
			data_size = 0;

			if (map_type == PROXY_MAP_HTTP_REQUEST_HEADERS) {
				if (hp->hd[HTTP_HDR_METHOD].b != NULL) {
					num_headers++;
					data_size += strlen(":method") + 1 +
					    strlen(hp->hd[HTTP_HDR_METHOD].b) + 1;
				}
				if (hp->hd[HTTP_HDR_URL].b != NULL) {
					num_headers++;
					data_size += strlen(":path") + 1 +
					    strlen(hp->hd[HTTP_HDR_URL].b) + 1;
				}
				for (i = HTTP_HDR_FIRST; i < hp->nhd; i++) {
					if (hp->hd[i].b != NULL &&
					    strncasecmp(hp->hd[i].b, "Host:",
					    5) == 0) {
						const char *v = hp->hd[i].b + 5;
						while (*v == ' ' || *v == '\t')
							v++;
						num_headers++;
						data_size += strlen(":authority") + 1 +
						    strlen(v) + 1;
						break;
					}
				}
			} else if (map_type == PROXY_MAP_HTTP_RESPONSE_HEADERS &&
			    hp->hd[HTTP_HDR_STATUS].b != NULL) {
				num_headers++;
				data_size += strlen(":status") + 1 +
				    strlen(hp->hd[HTTP_HDR_STATUS].b) + 1;
			}

			for (i = 0; i < (uint32_t)(hp->nhd - HTTP_HDR_FIRST);
			    i++) {
				uint32_t idx = HTTP_HDR_FIRST + i;
				const char *hdr_line;
				const char *colon;

				if (idx >= hp->nhd)
					break;
				hdr_line = hp->hd[idx].b;
				if (hdr_line == NULL) {
					data_size += 2;
					continue;
				}
				colon = strchr(hdr_line, ':');
				if (colon == NULL) {
					data_size += (uint32_t)strlen(hdr_line) +
					    2;
				} else {
					const char *val = colon + 1;
					while (*val == ' ' || *val == '\t')
						val++;
					data_size += (uint32_t)(colon - hdr_line) +
					    1 +
					    (uint32_t)strlen(val) + 1;
				}
			}
			serialized_size = 4 + num_headers * 8 + data_size;
		}
	}

	if (pw_write_u32(ctx, (uint32_t)args[1].of.i32,
	    serialized_size) != 0) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}
