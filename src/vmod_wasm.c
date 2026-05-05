/*-
 * Copyright (c) 2026 Ramazan Kara
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

#define VMOD_WASM_VERSION "1.0.0"

/* Global Wasm engine — shared across all VCL instances and threads */
static struct vwasm_engine *vwasm_engine_global = NULL;
static pthread_mutex_t engine_mtx = PTHREAD_MUTEX_INITIALIZER;

/*
 * VMOD event handler — called on VCL lifecycle events.
 * LOAD: initialize the Wasm engine
 * DISCARD: destroy the Wasm engine
 */
int v_matchproto_(vmod_event_f)
vmod_vmod_event(VRT_CTX, struct vmod_priv *priv, enum vcl_event_e e)
{
	(void)priv;

	switch (e) {
	case VCL_EVENT_LOAD:
		AZ(pthread_mutex_lock(&engine_mtx));
		if (vwasm_engine_global == NULL)
			vwasm_engine_global = vwasm_engine_new();
		AZ(pthread_mutex_unlock(&engine_mtx));
		if (vwasm_engine_global == NULL)
			return (-1);
		/* Register VDP for response body interception */
		VRT_AddFilter(ctx, NULL, &vdp_wasm_body);
		return (0);

	case VCL_EVENT_DISCARD:
		/* Unregister VDP */
		VRT_RemoveFilter(ctx, NULL, &vdp_wasm_body);
		AZ(pthread_mutex_lock(&engine_mtx));
		if (vwasm_engine_global != NULL) {
			vwasm_engine_destroy(&vwasm_engine_global);
			vwasm_engine_global = NULL;
		}
		AZ(pthread_mutex_unlock(&engine_mtx));
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
	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);

	if (name == NULL || *name == '\0') {
		VRT_fail(ctx, "wasm.load(): module name is required");
		return;
	}
	if (path == NULL || *path == '\0') {
		VRT_fail(ctx, "wasm.load(): path is required");
		return;
	}

	AN(vwasm_engine_global);

	if (vwasm_engine_load_module(vwasm_engine_global, name, path) != 0) {
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
	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);

	if (name == NULL || *name == '\0') {
		VRT_fail(ctx, "wasm.execute(): module name is required");
		return (-1);
	}
	if (function == NULL || *function == '\0') {
		VRT_fail(ctx, "wasm.execute(): function name is required");
		return (-1);
	}

	AN(vwasm_engine_global);

	int result = 0;
	if (vwasm_engine_call(vwasm_engine_global, ctx, name, function, &result) != 0) {
		VSLb(ctx->vsl, SLT_Error,
		    "wasm.execute(): failed to call '%s' in module '%s'",
		    function, name);
		return (-1);
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
 * wasm.set_fuel(fuel) — Set fuel (instruction) limit for execution.
 * Must be called from vcl_init.
 */
VCL_VOID
vmod_set_fuel(VRT_CTX, VCL_INT fuel)
{
	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);

	if (fuel <= 0) {
		VRT_fail(ctx, "wasm.set_fuel(): fuel must be positive");
		return;
	}

	AN(vwasm_engine_global);
	vwasm_engine_set_fuel(vwasm_engine_global, (uint64_t)fuel);
}

/*
 * wasm.set_memory_limit(limit) — Set maximum Wasm linear memory size.
 * Must be called from vcl_init.
 */
VCL_VOID
vmod_set_memory_limit(VRT_CTX, VCL_INT limit)
{
	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);

	if (limit <= 0) {
		VRT_fail(ctx, "wasm.set_memory_limit(): limit must be positive");
		return;
	}

	AN(vwasm_engine_global);
	vwasm_engine_set_memory_limit(vwasm_engine_global, (size_t)limit);
}

/*
 * wasm.get_fuel() — Return current fuel limit.
 */
VCL_INT
vmod_get_fuel(VRT_CTX)
{
	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);

	AN(vwasm_engine_global);
	return ((VCL_INT)vwasm_engine_get_fuel(vwasm_engine_global));
}

/*
 * wasm.get_memory_limit() — Return current memory limit in bytes.
 */
VCL_INT
vmod_get_memory_limit(VRT_CTX)
{
	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);

	AN(vwasm_engine_global);
	return ((VCL_INT)vwasm_engine_get_memory_limit(vwasm_engine_global));
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
	int status_code = 0;
	int ret;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	AN(vwasm_engine_global);

	if (module == NULL || *module == '\0') {
		VSLb(ctx->vsl, SLT_Error,
		    "wasm.proxy_wasm_on_request(): module name required");
		return (-1);
	}

	ret = vwasm_proxy_wasm_call(vwasm_engine_global, ctx,
	    module, &status_code);
	if (ret < 0)
		return (-1);

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
	int status_code = 0;
	int ret;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	AN(vwasm_engine_global);

	if (module == NULL || *module == '\0') {
		VSLb(ctx->vsl, SLT_Error,
		    "wasm.proxy_wasm_on_response(): module name required");
		return (-1);
	}

	ret = vwasm_proxy_wasm_response_call(vwasm_engine_global, ctx,
	    module, &status_code);
	if (ret < 0)
		return (-1);

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
	int status_code = 0;
	int ret;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	AN(vwasm_engine_global);

	if (module == NULL || *module == '\0') {
		VSLb(ctx->vsl, SLT_Error,
		    "wasm.proxy_wasm_on_request_configured(): "
		    "module name required");
		return (-1);
	}

	ret = vwasm_proxy_wasm_call_with_config(vwasm_engine_global, ctx,
	    module, vm_config, plugin_config, &status_code);
	if (ret < 0)
		return (-1);

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
	int status_code = 0;
	int ret;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	AN(vwasm_engine_global);

	if (module == NULL || *module == '\0') {
		VSLb(ctx->vsl, SLT_Error,
		    "wasm.proxy_wasm_on_response_configured(): "
		    "module name required");
		return (-1);
	}

	ret = vwasm_proxy_wasm_response_call_with_config(vwasm_engine_global,
	    ctx, module, vm_config, plugin_config, &status_code);
	if (ret < 0)
		return (-1);

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
	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	AN(vwasm_engine_global);

	vwasm_engine_set_allowed_upstreams(vwasm_engine_global, upstream_list);
}

/*
 * wasm.set_http_call_limit(limit) — Set max HTTP callouts per request.
 */
VCL_VOID
vmod_set_http_call_limit(VRT_CTX, VCL_INT limit)
{
	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	AN(vwasm_engine_global);

	if (limit < 0) {
		VRT_fail(ctx, "wasm.set_http_call_limit(): limit must be >= 0");
		return;
	}

	vwasm_engine_set_http_call_max(vwasm_engine_global, (uint32_t)limit);
}

/*
 * wasm.set_fail_mode(mode) — Set fail mode ("open" or "closed").
 */
VCL_VOID
vmod_set_fail_mode(VRT_CTX, VCL_STRING mode)
{
	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	AN(vwasm_engine_global);

	if (mode == NULL || *mode == '\0') {
		VRT_fail(ctx, "wasm.set_fail_mode(): mode is required");
		return;
	}

	if (strcmp(mode, "open") == 0) {
		vwasm_engine_set_fail_mode(vwasm_engine_global,
		    VWASM_FAIL_OPEN);
	} else if (strcmp(mode, "closed") == 0) {
		vwasm_engine_set_fail_mode(vwasm_engine_global,
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
	struct vwasm_stats *s;
	char buf[1024];
	int len;
	const char *result;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	AN(vwasm_engine_global);

	s = vwasm_engine_get_stats(vwasm_engine_global);
	if (s == NULL)
		return ("{}");

	len = snprintf(buf, sizeof(buf),
	    "{\"calls_total\":%lu,\"calls_ok\":%lu,\"calls_error\":%lu,"
	    "\"calls_timeout\":%lu,\"local_responses\":%lu,"
	    "\"http_calls\":%lu,\"http_calls_blocked\":%lu,"
	    "\"body_bytes_in\":%lu,\"fuel_total\":%lu}",
	    (unsigned long)s->calls_total, (unsigned long)s->calls_ok,
	    (unsigned long)s->calls_error, (unsigned long)s->calls_timeout,
	    (unsigned long)s->local_responses, (unsigned long)s->http_calls,
	    (unsigned long)s->http_calls_blocked,
	    (unsigned long)s->body_bytes_in, (unsigned long)s->fuel_total);

	result = WS_Copy(ctx->ws, buf, len + 1);
	if (result == NULL)
		return ("{}");

	return (result);
}
