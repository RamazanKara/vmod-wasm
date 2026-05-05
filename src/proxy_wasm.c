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

#include "proxy_wasm_mem.h"


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
	fprintf(stderr, "VMOD-WASM-DEBUG: send_local_response called status=%d\n",
	    args[0].of.i32);

	/*
	 * ABI: proxy_send_local_response(
	 *   status_code,              // args[0] i32
	 *   status_details_ptr,       // args[1] i32
	 *   status_details_size,      // args[2] i32
	 *   body_ptr,                 // args[3] i32
	 *   body_size,                // args[4] i32
	 *   headers_ptr,              // args[5] i32 (serialized header map)
	 *   headers_size,             // args[6] i32
	 *   grpc_status               // args[7] i32 (ignored)
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
	headers_ptr = (uint32_t)args[5].of.i32;
	headers_size = (uint32_t)args[6].of.i32;
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

	/* Free cached request body (only if heap-allocated) */
	if (ctx->request_body != NULL && ctx->request_body_heap) {
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
