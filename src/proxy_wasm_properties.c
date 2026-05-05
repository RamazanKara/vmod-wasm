/*-
 * Copyright (c) 2026 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Proxy-Wasm property get/set operations.
 */

#include <string.h>
#include <stdio.h>
#include <sys/socket.h>

#include <wasm.h>
#include <wasmtime.h>

#include "cache/cache.h"
#include "vrt_obj.h"
#include "vsa.h"
#include "vcl.h"

#include "proxy_wasm.h"
#include "proxy_wasm_mem.h"

/* ----------------------------------------------------------------
 * proxy_get_property
 * ---------------------------------------------------------------- */

wasm_trap_t *
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
	(void)nargs;
	(void)nresults;
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
 * ---------------------------------------------------------------- */

wasm_trap_t *
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
	(void)nargs;
	(void)nresults;
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

	/* request.path / request.url_path */
	if ((path_size == 12 &&
	    memcmp(path_buf, "request\0path", 12) == 0) ||
	    (path_size == 16 &&
	    memcmp(path_buf, "request\0url_path", 16) == 0)) {
		if (value_size == 0 || value_buf[0] != '/') {
			results[0].of.i32 = PROXY_BAD_ARGUMENT;
			return (NULL);
		}
		http_SetH(hp, HTTP_HDR_URL, WS_Copy(vctx->ws,
		    value_buf, (int)(value_size + 1)));
		results[0].of.i32 = PROXY_OK;
		return (NULL);
	}

	/* request.method */
	if (path_size == 14 &&
	    memcmp(path_buf, "request\0method", 14) == 0) {
		if (value_size == 0 || value_size > 16) {
			results[0].of.i32 = PROXY_BAD_ARGUMENT;
			return (NULL);
		}
		http_SetH(hp, HTTP_HDR_METHOD, WS_Copy(vctx->ws,
		    value_buf, (int)(value_size + 1)));
		results[0].of.i32 = PROXY_OK;
		return (NULL);
	}

	/* request.host */
	if (path_size == 12 &&
	    memcmp(path_buf, "request\0host", 12) == 0) {
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

	results[0].of.i32 = PROXY_NOT_FOUND;
	return (NULL);
}
