/*-
 * Copyright (c) 2025 Ramazan Kara
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "cache/cache.h"
#include "vcl.h"

#include "vcc_if.h"
#include "wasm_engine.h"
#include "proxy_wasm.h"
#include "vdp_wasm.h"

#define VMOD_WASM_VERSION "4.3.3"

/* One Wasm engine per loaded VCL. */
struct vmod_wasm_vcl {
	VCL_VCL			 vcl;
	struct vwasm_engine	*engine;
	struct vmod_wasm_vcl	*next;
};

static struct vmod_wasm_vcl *vwasm_vcls = NULL;
static pthread_mutex_t engine_mtx = PTHREAD_MUTEX_INITIALIZER;

static struct vmod_wasm_vcl *
vmod_wasm_find_vcl_locked(VCL_VCL vcl)
{
	struct vmod_wasm_vcl *vwv;

	for (vwv = vwasm_vcls; vwv != NULL; vwv = vwv->next) {
		if (vwv->vcl == vcl)
			return (vwv);
	}
	return (NULL);
}

static void
vmod_wasm_unlink_vcl_locked(struct vmod_wasm_vcl *node)
{
	struct vmod_wasm_vcl **pp;

	for (pp = &vwasm_vcls; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == node) {
			*pp = node->next;
			node->next = NULL;
			return;
		}
	}
}

static struct vwasm_engine *
vmod_wasm_get_engine(VRT_CTX)
{
	struct vmod_wasm_vcl *vwv;
	struct vwasm_engine *engine = NULL;

	if (ctx == NULL)
		return (NULL);

	AZ(pthread_mutex_lock(&engine_mtx));
	vwv = vmod_wasm_find_vcl_locked(ctx->vcl);
	if (vwv != NULL)
		engine = vwv->engine;
	AZ(pthread_mutex_unlock(&engine_mtx));
	return (engine);
}

static VCL_INT
vmod_wasm_error_result(struct vwasm_engine *engine)
{
	if (engine != NULL &&
	    vwasm_engine_get_fail_mode(engine) == VWASM_FAIL_OPEN)
		return (0);
	return (-1);
}

/*
 * VMOD event handler — called on VCL lifecycle events.
 * LOAD: initialize the Wasm engine
 * DISCARD: destroy the Wasm engine
 */
int v_matchproto_(vmod_event_f)
vmod_vmod_event(VRT_CTX, struct vmod_priv *priv, enum vcl_event_e e)
{
	struct vmod_wasm_vcl *vwv;
	struct vwasm_engine *engine;
	const char *err;

	switch (e) {
	case VCL_EVENT_LOAD:
		vwv = calloc(1, sizeof(*vwv));
		if (vwv == NULL)
			return (-1);
		vwv->engine = vwasm_engine_new();
		if (vwv->engine == NULL) {
			free(vwv);
			return (-1);
		}
		vwv->vcl = ctx->vcl;

		AZ(pthread_mutex_lock(&engine_mtx));
		if (vmod_wasm_find_vcl_locked(ctx->vcl) != NULL) {
			AZ(pthread_mutex_unlock(&engine_mtx));
			vwasm_engine_destroy(&vwv->engine);
			free(vwv);
			return (-1);
		}
		vwv->next = vwasm_vcls;
		vwasm_vcls = vwv;
		if (priv != NULL)
			priv->priv = vwv;
		AZ(pthread_mutex_unlock(&engine_mtx));

		/* Register VDP for response body interception */
		err = VRT_AddFilter(ctx, NULL, &vdp_wasm_body);
		if (err != NULL) {
			AZ(pthread_mutex_lock(&engine_mtx));
			vmod_wasm_unlink_vcl_locked(vwv);
			if (priv != NULL && priv->priv == vwv)
				priv->priv = NULL;
			AZ(pthread_mutex_unlock(&engine_mtx));
			vwasm_engine_destroy(&vwv->engine);
			free(vwv);
			return (-1);
		}
		return (0);

	case VCL_EVENT_DISCARD:
		/* Unregister VDP */
		VRT_RemoveFilter(ctx, NULL, &vdp_wasm_body);

		vwv = NULL;
		AZ(pthread_mutex_lock(&engine_mtx));
		if (priv != NULL && priv->priv != NULL)
			vwv = priv->priv;
		else
			vwv = vmod_wasm_find_vcl_locked(ctx->vcl);
		if (vwv != NULL) {
			vmod_wasm_unlink_vcl_locked(vwv);
			if (priv != NULL && priv->priv == vwv)
				priv->priv = NULL;
		}
		AZ(pthread_mutex_unlock(&engine_mtx));

		if (vwv != NULL) {
			engine = vwv->engine;
			vwv->engine = NULL;
			vwasm_engine_destroy(&engine);
			free(vwv);
		}
		return (0);

	default:
		return (0);
	}
}

/*
 * wasm.load(name, path) — Load and compile a Wasm module from disk.
 * Must be called from vcl_init.
 */
VCL_VOID
vmod_load(VRT_CTX, VCL_STRING name, VCL_STRING path)
{
	struct vwasm_engine *engine;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);

	if (name == NULL || *name == '\0') {
		VRT_fail(ctx, "wasm.load(): module name is required");
		return;
	}
	if (path == NULL || *path == '\0') {
		VRT_fail(ctx, "wasm.load(): path is required");
		return;
	}

	engine = vmod_wasm_get_engine(ctx);
	if (engine == NULL) {
		VRT_fail(ctx, "wasm.load(): engine not initialized");
		return;
	}

	if (vwasm_engine_load_module(engine, name, path) != 0) {
		VRT_fail(ctx, "wasm.load(): failed to load module '%s' from '%s'",
		    name, path);
	}
}

/*
 * wasm.execute(name, function) — Execute an exported function from a
 * previously loaded Wasm module. Returns the integer result.
 */
VCL_INT
vmod_execute(VRT_CTX, VCL_STRING name, VCL_STRING function)
{
	struct vwasm_engine *engine;
	int result = 0;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);

	if (name == NULL || *name == '\0') {
		VRT_fail(ctx, "wasm.execute(): module name is required");
		return (-1);
	}
	if (function == NULL || *function == '\0') {
		VRT_fail(ctx, "wasm.execute(): function name is required");
		return (-1);
	}

	engine = vmod_wasm_get_engine(ctx);
	if (engine == NULL)
		return (-1);

	if (vwasm_engine_call(engine, ctx, name, function, &result) != 0) {
		VSLb(ctx->vsl, SLT_Error,
		    "wasm.execute(): failed to call '%s' in module '%s'",
		    function, name);
		return (vmod_wasm_error_result(engine));
	}

	return (result);
}

/*
 * wasm.version() — Return the VMOD version string.
 */
VCL_STRING
vmod_version(VRT_CTX)
{
	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	return (VMOD_WASM_VERSION);
}

/*
 * wasm.set_memory_limit(limit) — Set maximum Wasm linear memory size.
 * Must be called from vcl_init.
 */
VCL_VOID
vmod_set_memory_limit(VRT_CTX, VCL_INT limit)
{
	struct vwasm_engine *engine;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);

	if (limit <= 0) {
		VRT_fail(ctx, "wasm.set_memory_limit(): limit must be positive");
		return;
	}

	engine = vmod_wasm_get_engine(ctx);
	if (engine == NULL) {
		VRT_fail(ctx, "wasm.set_memory_limit(): engine not initialized");
		return;
	}
	vwasm_engine_set_memory_limit(engine, (size_t)limit);
}

/*
 * wasm.get_memory_limit() — Return current memory limit in bytes.
 */
VCL_INT
vmod_get_memory_limit(VRT_CTX)
{
	struct vwasm_engine *engine;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);

	engine = vmod_wasm_get_engine(ctx);
	if (engine == NULL)
		return (0);
	return ((VCL_INT)vwasm_engine_get_memory_limit(engine));
}

/*
 * wasm.set_epoch_deadline(ms) — Set epoch-based execution deadline.
 * Must be called from vcl_init.
 */
VCL_VOID
vmod_set_epoch_deadline(VRT_CTX, VCL_INT ms)
{
	struct vwasm_engine *engine;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);

	if (ms <= 0) {
		VRT_fail(ctx,
		    "wasm.set_epoch_deadline(): ms must be positive");
		return;
	}

	engine = vmod_wasm_get_engine(ctx);
	if (engine == NULL) {
		VRT_fail(ctx, "wasm.set_epoch_deadline(): engine not initialized");
		return;
	}
	vwasm_engine_set_epoch_deadline(engine, (uint64_t)ms);
}

/*
 * wasm.get_epoch_deadline() — Return current epoch deadline in ms.
 */
VCL_INT
vmod_get_epoch_deadline(VRT_CTX)
{
	struct vwasm_engine *engine;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);

	engine = vmod_wasm_get_engine(ctx);
	if (engine == NULL)
		return (0);
	return ((VCL_INT)vwasm_engine_get_epoch_deadline(engine));
}

/*
 * wasm.proxy_wasm_on_request(module) — Execute a Proxy-Wasm filter.
 *
 * Returns:
 *   0    — CONTINUE (allow)
 *   >0   — HTTP status code from send_local_response (e.g. 403)
 *   -1   — execution error
 */
VCL_INT
vmod_proxy_wasm_on_request(VRT_CTX, VCL_STRING module)
{
	struct vwasm_engine *engine;
	int status_code = 0;
	int ret;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	engine = vmod_wasm_get_engine(ctx);
	if (engine == NULL)
		return (-1);

	if (module == NULL || *module == '\0') {
		VSLb(ctx->vsl, SLT_Error,
		    "wasm.proxy_wasm_on_request(): module name required");
		return (-1);
	}

	ret = vwasm_proxy_wasm_call(engine, ctx,
	    module, &status_code);
	if (ret < 0)
		return (vmod_wasm_error_result(engine));

	/* If the filter called send_local_response, return the status code */
	if (status_code > 0)
		return (status_code);

	/* Otherwise return the action (0=CONTINUE, 1=PAUSE) */
	return (ret);
}

/*
 * wasm.proxy_wasm_on_response(module) — Execute a Proxy-Wasm response filter.
 *
 * Returns:
 *   0    — CONTINUE (allow)
 *   >0   — HTTP status code from send_local_response
 *   -1   — execution error
 */
VCL_INT
vmod_proxy_wasm_on_response(VRT_CTX, VCL_STRING module)
{
	struct vwasm_engine *engine;
	int status_code = 0;
	int ret;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	engine = vmod_wasm_get_engine(ctx);
	if (engine == NULL)
		return (-1);

	if (module == NULL || *module == '\0') {
		VSLb(ctx->vsl, SLT_Error,
		    "wasm.proxy_wasm_on_response(): module name required");
		return (-1);
	}

	ret = vwasm_proxy_wasm_response_call(engine, ctx,
	    module, &status_code);
	if (ret < 0)
		return (vmod_wasm_error_result(engine));

	/* If the filter called send_local_response, return the status code */
	if (status_code > 0)
		return (status_code);

	/* Otherwise return the action (0=CONTINUE, 1=PAUSE) */
	return (ret);
}

/*
 * wasm.proxy_wasm_on_request_configured(module, vm_config, plugin_config)
 *
 * Execute a Proxy-Wasm request filter with explicit configuration.
 */
VCL_INT
vmod_proxy_wasm_on_request_configured(VRT_CTX, VCL_STRING module,
    VCL_STRING vm_config, VCL_STRING plugin_config)
{
	struct vwasm_engine *engine;
	int status_code = 0;
	int ret;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	engine = vmod_wasm_get_engine(ctx);
	if (engine == NULL)
		return (-1);

	if (module == NULL || *module == '\0') {
		VSLb(ctx->vsl, SLT_Error,
		    "wasm.proxy_wasm_on_request_configured(): "
		    "module name required");
		return (-1);
	}

	ret = vwasm_proxy_wasm_call_with_config(engine, ctx,
	    module, vm_config, plugin_config, &status_code);
	if (ret < 0)
		return (vmod_wasm_error_result(engine));

	if (status_code > 0)
		return (status_code);

	return (ret);
}

/*
 * wasm.proxy_wasm_on_response_configured(module, vm_config, plugin_config)
 *
 * Execute a Proxy-Wasm response filter with explicit configuration.
 */
VCL_INT
vmod_proxy_wasm_on_response_configured(VRT_CTX, VCL_STRING module,
    VCL_STRING vm_config, VCL_STRING plugin_config)
{
	struct vwasm_engine *engine;
	int status_code = 0;
	int ret;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	engine = vmod_wasm_get_engine(ctx);
	if (engine == NULL)
		return (-1);

	if (module == NULL || *module == '\0') {
		VSLb(ctx->vsl, SLT_Error,
		    "wasm.proxy_wasm_on_response_configured(): "
		    "module name required");
		return (-1);
	}

	ret = vwasm_proxy_wasm_response_call_with_config(engine,
	    ctx, module, vm_config, plugin_config, &status_code);
	if (ret < 0)
		return (vmod_wasm_error_result(engine));

	if (status_code > 0)
		return (status_code);

	return (ret);
}

/*
 * wasm.set_allowed_upstreams(upstream_list) — Set allowed upstream hosts
 * for proxy_http_call (SSRF protection).
 */
VCL_VOID
vmod_set_allowed_upstreams(VRT_CTX, VCL_STRING upstream_list)
{
	struct vwasm_engine *engine;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	engine = vmod_wasm_get_engine(ctx);
	if (engine == NULL) {
		VRT_fail(ctx, "wasm.set_allowed_upstreams(): engine not initialized");
		return;
	}

	vwasm_engine_set_allowed_upstreams(engine, upstream_list);
}

/*
 * wasm.set_http_call_limit(limit) — Set max HTTP callouts per request.
 */
VCL_VOID
vmod_set_http_call_limit(VRT_CTX, VCL_INT limit)
{
	struct vwasm_engine *engine;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);

	if (limit < 0) {
		VRT_fail(ctx, "wasm.set_http_call_limit(): limit must be >= 0");
		return;
	}

	engine = vmod_wasm_get_engine(ctx);
	if (engine == NULL) {
		VRT_fail(ctx, "wasm.set_http_call_limit(): engine not initialized");
		return;
	}
	vwasm_engine_set_http_call_max(engine, (uint32_t)limit);
}

/*
 * wasm.set_fail_mode(mode) — Set fail mode ("open" or "closed").
 */
VCL_VOID
vmod_set_fail_mode(VRT_CTX, VCL_STRING mode)
{
	struct vwasm_engine *engine;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	engine = vmod_wasm_get_engine(ctx);
	if (engine == NULL) {
		VRT_fail(ctx, "wasm.set_fail_mode(): engine not initialized");
		return;
	}

	if (mode == NULL || *mode == '\0') {
		VRT_fail(ctx, "wasm.set_fail_mode(): mode is required");
		return;
	}

	if (strcmp(mode, "open") == 0) {
		vwasm_engine_set_fail_mode(engine,
		    VWASM_FAIL_OPEN);
	} else if (strcmp(mode, "closed") == 0) {
		vwasm_engine_set_fail_mode(engine,
		    VWASM_FAIL_CLOSED);
	} else {
		VRT_fail(ctx,
		    "wasm.set_fail_mode(): invalid mode '%s' "
		    "(use \"open\" or \"closed\")", mode);
	}
}

/*
 * wasm.get_metrics_json() — Return all proxy-wasm metrics as JSON.
 */
VCL_STRING
vmod_get_metrics_json(VRT_CTX)
{
	struct vwasm_metric_store *store;
	char *buf;
	size_t buf_size;
	size_t pos;
	uint32_t i;
	const char *type_str;
	const char *result;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);

	store = vwasm_proxy_wasm_get_metric_store();
	if (store == NULL)
		return ("{}");

	pthread_rwlock_rdlock(&store->rwlock);

	if (store->count == 0) {
		pthread_rwlock_unlock(&store->rwlock);
		return ("{}");
	}

	/* Allocate generous buffer: each metric needs ~200 chars max */
	buf_size = 2 + (size_t)store->count * 200;
	buf = malloc(buf_size);
	if (buf == NULL) {
		pthread_rwlock_unlock(&store->rwlock);
		return ("{}");
	}

	pos = 0;
	buf[pos++] = '{';

	for (i = 0; i < store->count; i++) {
		if (i > 0) {
			buf[pos++] = ',';
		}

		switch (store->metrics[i].type) {
		case PROXY_METRIC_COUNTER:
			type_str = "counter";
			break;
		case PROXY_METRIC_GAUGE:
			type_str = "gauge";
			break;
		case PROXY_METRIC_HISTOGRAM:
			type_str = "histogram";
			break;
		default:
			type_str = "unknown";
			break;
		}

		pos += (size_t)snprintf(buf + pos, buf_size - pos,
		    "\"%.*s\":{\"type\":\"%s\",\"value\":%lu}",
		    (int)store->metrics[i].name_len,
		    store->metrics[i].name,
		    type_str,
		    (unsigned long)store->metrics[i].value);
	}

	buf[pos++] = '}';
	buf[pos] = '\0';

	pthread_rwlock_unlock(&store->rwlock);

	/* Copy to workspace so VCL can use it */
	result = WS_Copy(ctx->ws, buf, (int)(pos + 1));
	free(buf);

	if (result == NULL)
		return ("{}");

	return (result);
}

/*
 * wasm.get_stats_json() — Return execution statistics as JSON.
 */
VCL_STRING
vmod_get_stats_json(VRT_CTX)
{
	struct vwasm_engine *engine;
	struct vwasm_stats *s;
	char buf[1024];
	int len;
	const char *result;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	engine = vmod_wasm_get_engine(ctx);
	if (engine == NULL)
		return ("{}");

	s = vwasm_engine_get_stats(engine);
	if (s == NULL)
		return ("{}");

	len = snprintf(buf, sizeof(buf),
	    "{\"calls_total\":%lu,\"calls_ok\":%lu,\"calls_error\":%lu,"
	    "\"calls_timeout\":%lu,\"local_responses\":%lu,"
	    "\"http_calls\":%lu,\"http_calls_blocked\":%lu,"
	    "\"body_bytes_in\":%lu}",
	    (unsigned long)s->calls_total, (unsigned long)s->calls_ok,
	    (unsigned long)s->calls_error, (unsigned long)s->calls_timeout,
	    (unsigned long)s->local_responses, (unsigned long)s->http_calls,
	    (unsigned long)s->http_calls_blocked,
	    (unsigned long)s->body_bytes_in);

	result = WS_Copy(ctx->ws, buf, len + 1);
	if (result == NULL)
		return ("{}");

	return (result);
}

/* ----------------------------------------------------------------
 * Phase 2: Store pool management
 * ---------------------------------------------------------------- */

/*
 * wasm.set_store_pool_size(module, size) — Pre-warm N stores for a module.
 */
VCL_VOID
vmod_set_store_pool_size(VRT_CTX, VCL_STRING module, VCL_INT size)
{
	struct vwasm_engine *engine;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);

	engine = vmod_wasm_get_engine(ctx);
	if (engine == NULL) {
		VRT_fail(ctx, "wasm.set_store_pool_size: engine not initialized");
		return;
	}

	if (module == NULL || *module == '\0') {
		VRT_fail(ctx, "wasm.set_store_pool_size: module name required");
		return;
	}

	if (size < 1 || size > 256) {
		VRT_fail(ctx, "wasm.set_store_pool_size: size must be 1-256");
		return;
	}

	if (vwasm_engine_init_pool(engine, module,
	    (size_t)size, NULL, NULL) != 0)
		VRT_fail(ctx, "wasm.set_store_pool_size: pool init failed for %s",
		    module);
}

/* ----------------------------------------------------------------
 * Phase 3: HTTP connection pool
 * ---------------------------------------------------------------- */

/*
 * wasm.set_http_pool_size(size) — Set max persistent HTTP connections.
 */
VCL_VOID
vmod_set_http_pool_size(VRT_CTX, VCL_INT size)
{
	struct vwasm_engine *engine;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);

	engine = vmod_wasm_get_engine(ctx);
	if (engine == NULL) {
		VRT_fail(ctx, "wasm.set_http_pool_size: engine not initialized");
		return;
	}

	if (size < 1 || size > 1024) {
		VRT_fail(ctx, "wasm.set_http_pool_size: size must be 1-1024");
		return;
	}

	if (vwasm_engine_init_http_pool(engine,
	    (size_t)size) != 0)
		VRT_fail(ctx, "wasm.set_http_pool_size: pool init failed");
}

/* ----------------------------------------------------------------
 * Phase 4: Filter chain
 * ---------------------------------------------------------------- */

/*
 * wasm.filter_chain(chain_spec) — Execute request filter chain.
 */
VCL_INT
vmod_filter_chain(VRT_CTX, VCL_STRING chain_spec)
{
	struct vwasm_engine *engine;
	const char *modules[64];
	char *buf, *p, *start;
	int nmodules = 0;
	int status_code = 0;
	int ret;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);

	engine = vmod_wasm_get_engine(ctx);
	if (engine == NULL)
		return (-1);

	if (chain_spec == NULL || *chain_spec == '\0')
		return (-1);

	/* Parse pipe-separated chain spec */
	buf = strdup(chain_spec);
	if (buf == NULL)
		return (-1);

	p = buf;
	while (*p != '\0' && nmodules < 64) {
		while (*p == '|' || *p == ' ' || *p == '\t')
			p++;
		if (*p == '\0')
			break;
		start = p;
		while (*p != '|' && *p != '\0' && *p != ' ' && *p != '\t')
			p++;
		if (p > start) {
			if (*p != '\0')
				*p++ = '\0';
			modules[nmodules++] = start;
		}
	}

	ret = vwasm_filter_chain_request(engine,
	    ctx, modules, nmodules, &status_code);

	free(buf);
	if (ret < 0)
		return (vmod_wasm_error_result(engine));
	return (ret);
}

/*
 * wasm.filter_chain_response(chain_spec) — Execute response filter chain.
 */
VCL_INT
vmod_filter_chain_response(VRT_CTX, VCL_STRING chain_spec)
{
	struct vwasm_engine *engine;
	const char *modules[64];
	char *buf, *p, *start;
	int nmodules = 0;
	int status_code = 0;
	int ret;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);

	engine = vmod_wasm_get_engine(ctx);
	if (engine == NULL)
		return (-1);

	if (chain_spec == NULL || *chain_spec == '\0')
		return (-1);

	/* Parse pipe-separated chain spec */
	buf = strdup(chain_spec);
	if (buf == NULL)
		return (-1);

	p = buf;
	while (*p != '\0' && nmodules < 64) {
		while (*p == '|' || *p == ' ' || *p == '\t')
			p++;
		if (*p == '\0')
			break;
		start = p;
		while (*p != '|' && *p != '\0' && *p != ' ' && *p != '\t')
			p++;
		if (p > start) {
			if (*p != '\0')
				*p++ = '\0';
			modules[nmodules++] = start;
		}
	}

	ret = vwasm_filter_chain_response(engine,
	    ctx, modules, nmodules, &status_code);

	free(buf);
	if (ret < 0)
		return (vmod_wasm_error_result(engine));
	return (ret);
}

/* ----------------------------------------------------------------
 * Pool statistics
 * ---------------------------------------------------------------- */

/*
 * wasm.get_pool_stats_json(module) — Return store pool stats as JSON.
 */
VCL_STRING
vmod_get_pool_stats_json(VRT_CTX, VCL_STRING module)
{
	struct vwasm_engine *engine;
	char *json;
	const char *result;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);

	engine = vmod_wasm_get_engine(ctx);
	if (engine == NULL || module == NULL)
		return ("{}");

	json = vwasm_engine_get_pool_stats_json(engine, module);
	if (json == NULL)
		return ("{}");

	result = WS_Copy(ctx->ws, json, (int)(strlen(json) + 1));
	free(json);

	if (result == NULL)
		return ("{}");

	return (result);
}

/*
 * wasm.get_http_pool_stats_json() — Return HTTP pool stats as JSON.
 */
VCL_STRING
vmod_get_http_pool_stats_json(VRT_CTX)
{
	struct vwasm_engine *engine;
	char *json;
	const char *result;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);

	engine = vmod_wasm_get_engine(ctx);
	if (engine == NULL)
		return ("{}");

	json = vwasm_engine_get_http_pool_stats_json(engine);
	if (json == NULL)
		return ("{}");

	result = WS_Copy(ctx->ws, json, (int)(strlen(json) + 1));
	free(json);

	if (result == NULL)
		return ("{}");

	return (result);
}
