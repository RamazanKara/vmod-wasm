/*-
 * Copyright (c) 2026 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Wasmtime engine wrapper — handles module compilation, instantiation,
 * and function execution.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <wasm.h>
#include <wasmtime.h>

#include "cache/cache.h"
#include "vcl.h"

#include "wasm_engine.h"
#include "host_functions.h"
#include "proxy_wasm.h"

#define MAX_MODULES 64

struct wasm_module_entry {
	char			*name;
	wasmtime_module_t	*module;
};

struct vwasm_engine {
	wasm_engine_t		*engine;
	wasmtime_linker_t	*linker;
	struct wasm_module_entry	modules[MAX_MODULES];
	int			nmodules;
	pthread_rwlock_t	rwlock;
	/* Execution limits (atomic access via rwlock) */
	uint64_t		fuel_limit;
	size_t			memory_limit;
};

struct vwasm_engine *
vwasm_engine_new(void)
{
	struct vwasm_engine *e;
	wasm_config_t *config;

	e = calloc(1, sizeof(*e));
	if (e == NULL)
		return (NULL);

	config = wasm_config_new();
	if (config == NULL) {
		free(e);
		return (NULL);
	}

	/* Enable fuel consumption for execution limits */
	wasmtime_config_consume_fuel_set(config, true);

	/* Set maximum Wasm call stack size */
	wasmtime_config_max_wasm_stack_set(config, VWASM_DEFAULT_STACKSIZE);

	e->engine = wasm_engine_new_with_config(config);
	if (e->engine == NULL) {
		free(e);
		return (NULL);
	}

	/* Create linker and register host functions */
	e->linker = wasmtime_linker_new(e->engine);
	if (e->linker == NULL) {
		wasm_engine_delete(e->engine);
		free(e);
		return (NULL);
	}

	if (vwasm_host_define_imports(e->linker) != 0) {
		wasmtime_linker_delete(e->linker);
		wasm_engine_delete(e->engine);
		free(e);
		return (NULL);
	}

	if (vwasm_proxy_wasm_define_imports(e->linker) != 0) {
		wasmtime_linker_delete(e->linker);
		wasm_engine_delete(e->engine);
		free(e);
		return (NULL);
	}

	pthread_rwlock_init(&e->rwlock, NULL);
	e->nmodules = 0;
	e->fuel_limit = VWASM_DEFAULT_FUEL;
	e->memory_limit = VWASM_DEFAULT_MEMLIMIT;

	return (e);
}

void
vwasm_engine_destroy(struct vwasm_engine **enginep)
{
	struct vwasm_engine *e;
	int i;

	if (enginep == NULL || *enginep == NULL)
		return;

	e = *enginep;
	*enginep = NULL;

	for (i = 0; i < e->nmodules; i++) {
		free(e->modules[i].name);
		wasmtime_module_delete(e->modules[i].module);
	}

	wasmtime_linker_delete(e->linker);
	wasm_engine_delete(e->engine);
	pthread_rwlock_destroy(&e->rwlock);
	free(e);
}

/* ----------------------------------------------------------------
 * Configuration setters/getters (thread-safe via rwlock)
 * ---------------------------------------------------------------- */

void
vwasm_engine_set_fuel(struct vwasm_engine *engine, uint64_t fuel)
{
	if (engine == NULL)
		return;
	pthread_rwlock_wrlock(&engine->rwlock);
	engine->fuel_limit = fuel;
	pthread_rwlock_unlock(&engine->rwlock);
}

void
vwasm_engine_set_memory_limit(struct vwasm_engine *engine, size_t bytes)
{
	if (engine == NULL)
		return;
	pthread_rwlock_wrlock(&engine->rwlock);
	engine->memory_limit = bytes;
	pthread_rwlock_unlock(&engine->rwlock);
}

uint64_t
vwasm_engine_get_fuel(struct vwasm_engine *engine)
{
	uint64_t f;

	if (engine == NULL)
		return (VWASM_DEFAULT_FUEL);
	pthread_rwlock_rdlock(&engine->rwlock);
	f = engine->fuel_limit;
	pthread_rwlock_unlock(&engine->rwlock);
	return (f);
}

size_t
vwasm_engine_get_memory_limit(struct vwasm_engine *engine)
{
	size_t m;

	if (engine == NULL)
		return (VWASM_DEFAULT_MEMLIMIT);
	pthread_rwlock_rdlock(&engine->rwlock);
	m = engine->memory_limit;
	pthread_rwlock_unlock(&engine->rwlock);
	return (m);
}

/* ----------------------------------------------------------------
 * Store memory limiter callback — called when Wasm linear memory
 * tries to grow beyond the configured limit.
 *
 * We use wasmtime_store_limiter() in vwasm_engine_call() instead
 * of a manual callback — it takes memory_size directly.
 * ---------------------------------------------------------------- */

/* Extract human-readable message from a Wasmtime trap */
static void
log_trap(const struct vrt_ctx *ctx, wasm_trap_t *trap,
    const char *module_name, const char *func_name)
{
	wasm_message_t msg;

	if (ctx == NULL || ctx->vsl == NULL || trap == NULL)
		return;

	wasm_trap_message(trap, &msg);
	if (msg.size > 0 && msg.data != NULL)
		VSLb(ctx->vsl, SLT_Error,
		    "wasm: trap in %s.%s: %.*s",
		    module_name, func_name, (int)msg.size, msg.data);
	else
		VSLb(ctx->vsl, SLT_Error,
		    "wasm: trap in %s.%s (no message)",
		    module_name, func_name);
	wasm_byte_vec_delete(&msg);
}

/* Extract human-readable message from a Wasmtime error */
static void
log_error(const struct vrt_ctx *ctx, wasmtime_error_t *error,
    const char *module_name, const char *func_name)
{
	wasm_message_t msg;

	if (ctx == NULL || ctx->vsl == NULL || error == NULL)
		return;

	wasmtime_error_message(error, &msg);
	if (msg.size > 0 && msg.data != NULL)
		VSLb(ctx->vsl, SLT_Error,
		    "wasm: error in %s.%s: %.*s",
		    module_name, func_name, (int)msg.size, msg.data);
	else
		VSLb(ctx->vsl, SLT_Error,
		    "wasm: error in %s.%s (no message)",
		    module_name, func_name);
	wasm_byte_vec_delete(&msg);
}

int
vwasm_engine_load_module(struct vwasm_engine *engine,
    const char *name, const char *path)
{
	FILE *fp;
	long fsize;
	unsigned char *bytes = NULL;
	wasmtime_module_t *module = NULL;
	wasmtime_error_t *error = NULL;
	int ret = -1;

	if (engine == NULL || name == NULL || path == NULL)
		return (-1);

	/* Read the .wasm file */
	fp = fopen(path, "rb");
	if (fp == NULL)
		return (-1);

	fseek(fp, 0, SEEK_END);
	fsize = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	if (fsize <= 0) {
		fclose(fp);
		return (-1);
	}

	bytes = malloc((size_t)fsize);
	if (bytes == NULL) {
		fclose(fp);
		return (-1);
	}

	if (fread(bytes, 1, (size_t)fsize, fp) != (size_t)fsize) {
		free(bytes);
		fclose(fp);
		return (-1);
	}
	fclose(fp);

	/* Compile the module */
	error = wasmtime_module_new(engine->engine, bytes, (size_t)fsize, &module);
	free(bytes);

	if (error != NULL) {
		wasmtime_error_delete(error);
		return (-1);
	}

	/* Store the module */
	pthread_rwlock_wrlock(&engine->rwlock);
	if (engine->nmodules >= MAX_MODULES) {
		pthread_rwlock_unlock(&engine->rwlock);
		wasmtime_module_delete(module);
		return (-1);
	}

	engine->modules[engine->nmodules].name = strdup(name);
	engine->modules[engine->nmodules].module = module;
	engine->nmodules++;
	ret = 0;
	pthread_rwlock_unlock(&engine->rwlock);

	return (ret);
}

static wasmtime_module_t *
find_module(struct vwasm_engine *engine, const char *name)
{
	int i;

	for (i = 0; i < engine->nmodules; i++) {
		if (strcmp(engine->modules[i].name, name) == 0)
			return (engine->modules[i].module);
	}
	return (NULL);
}

int
vwasm_engine_call(struct vwasm_engine *engine,
    const struct vrt_ctx *ctx,
    const char *module_name, const char *func_name,
    int *result)
{
	wasmtime_module_t *module;
	wasmtime_store_t *store;
	wasmtime_context_t *context;
	wasmtime_instance_t instance;
	wasmtime_error_t *error = NULL;
	wasm_trap_t *trap = NULL;
	wasmtime_extern_t item;
	wasmtime_val_t results[1];
	struct vwasm_host_ctx host_ctx;
	uint64_t fuel_limit, fuel_remaining;
	size_t mem_limit;
	int ret = -1;

	if (engine == NULL || module_name == NULL || func_name == NULL)
		return (-1);

	/* Read current limits */
	pthread_rwlock_rdlock(&engine->rwlock);
	fuel_limit = engine->fuel_limit;
	mem_limit = engine->memory_limit;
	pthread_rwlock_unlock(&engine->rwlock);

	/* Find the pre-compiled module */
	pthread_rwlock_rdlock(&engine->rwlock);
	module = find_module(engine, module_name);
	pthread_rwlock_unlock(&engine->rwlock);

	if (module == NULL)
		return (-1);

	/* Set up host context with Varnish request context */
	memset(&host_ctx, 0, sizeof(host_ctx));
	host_ctx.vrt_ctx = ctx;
	host_ctx.memory_valid = 0;

	/* Create a per-call store with host context as data */
	store = wasmtime_store_new(engine->engine, &host_ctx, NULL);
	if (store == NULL)
		return (-1);

	context = wasmtime_store_context(store);

	/* Set fuel limit for this execution */
	wasmtime_context_set_fuel(context, fuel_limit);

	/* Set memory limiter — cap linear memory growth */
	wasmtime_store_limiter(store, (int64_t)mem_limit, -1, -1, -1, -1);

	/* Instantiate the module via linker (resolves host function imports) */
	error = wasmtime_linker_instantiate(engine->linker, context,
	    module, &instance, &trap);
	if (error != NULL) {
		log_error(ctx, error, module_name, func_name);
		goto cleanup;
	}
	if (trap != NULL) {
		log_trap(ctx, trap, module_name, func_name);
		goto cleanup;
	}

	/* Resolve the "memory" export for host function string passing */
	if (wasmtime_instance_export_get(context, &instance,
	    "memory", 6, &item) && item.kind == WASMTIME_EXTERN_MEMORY) {
		host_ctx.memory = item.of.memory;
		host_ctx.memory_valid = 1;
	}

	/* Look up the exported function */
	if (!wasmtime_instance_export_get(context, &instance,
	    func_name, strlen(func_name), &item))
		goto cleanup;

	if (item.kind != WASMTIME_EXTERN_FUNC)
		goto cleanup;

	/* Call the function (no arguments, one i32 result) */
	error = wasmtime_func_call(context, &item.of.func,
	    NULL, 0, results, 1, &trap);
	if (error != NULL) {
		log_error(ctx, error, module_name, func_name);
		goto cleanup;
	}
	if (trap != NULL) {
		log_trap(ctx, trap, module_name, func_name);
		goto cleanup;
	}

	*result = (int)results[0].of.i32;
	ret = 0;

	/* Log execution metrics to VSL */
	if (ctx != NULL && ctx->vsl != NULL) {
		fuel_remaining = 0;
		wasmtime_context_get_fuel(context, &fuel_remaining);
		VSLb(ctx->vsl, SLT_Debug,
		    "wasm: %s.%s ok, fuel=%llu/%llu used",
		    module_name, func_name,
		    (unsigned long long)(fuel_limit - fuel_remaining),
		    (unsigned long long)fuel_limit);
	}

cleanup:
	if (error != NULL)
		wasmtime_error_delete(error);
	if (trap != NULL)
		wasm_trap_delete(trap);
	wasmtime_store_delete(store);
	return (ret);
}

/* ----------------------------------------------------------------
 * Call a Wasm function with i32 arguments and i32 result.
 * Helper for Proxy-Wasm lifecycle callbacks.
 * ---------------------------------------------------------------- */

static int
call_wasm_func(wasmtime_context_t *context, wasmtime_instance_t *instance,
    const char *func_name, const wasmtime_val_t *args, size_t nargs,
    int32_t *result)
{
	wasmtime_extern_t item;
	wasmtime_val_t results[1];
	wasmtime_error_t *error;
	wasm_trap_t *trap = NULL;

	if (!wasmtime_instance_export_get(context, instance,
	    func_name, strlen(func_name), &item))
		return (-1);

	if (item.kind != WASMTIME_EXTERN_FUNC)
		return (-1);

	error = wasmtime_func_call(context, &item.of.func,
	    args, nargs, results, 1, &trap);
	if (error != NULL) {
		wasmtime_error_delete(error);
		return (-1);
	}
	if (trap != NULL) {
		wasm_trap_delete(trap);
		return (-1);
	}

	if (result != NULL)
		*result = results[0].of.i32;
	return (0);
}

static int
call_wasm_void(wasmtime_context_t *context, wasmtime_instance_t *instance,
    const char *func_name, const wasmtime_val_t *args, size_t nargs)
{
	wasmtime_extern_t item;
	wasmtime_error_t *error;
	wasm_trap_t *trap = NULL;

	if (!wasmtime_instance_export_get(context, instance,
	    func_name, strlen(func_name), &item))
		return (-1);

	if (item.kind != WASMTIME_EXTERN_FUNC)
		return (-1);

	error = wasmtime_func_call(context, &item.of.func,
	    args, nargs, NULL, 0, &trap);
	if (error != NULL) {
		wasmtime_error_delete(error);
		return (-1);
	}
	if (trap != NULL) {
		wasm_trap_delete(trap);
		return (-1);
	}
	return (0);
}

/* ----------------------------------------------------------------
 * Proxy-Wasm lifecycle execution for HTTP request filtering.
 *
 * Runs the Proxy-Wasm callback sequence and returns the action
 * from proxy_on_request_headers, or -1 on error.
 * ---------------------------------------------------------------- */

int
vwasm_proxy_wasm_call(struct vwasm_engine *engine,
    const struct vrt_ctx *ctx,
    const char *module_name,
    int *status_code)
{
	wasmtime_module_t *module;
	wasmtime_store_t *store;
	wasmtime_context_t *context;
	wasmtime_instance_t instance;
	wasmtime_error_t *error = NULL;
	wasm_trap_t *trap = NULL;
	wasmtime_extern_t item;
	struct vwasm_proxy_ctx proxy_ctx;
	wasmtime_val_t args[3];
	int32_t action;
	uint64_t fuel_limit;
	size_t mem_limit;
	int ret = -1;
	int num_headers;

	if (engine == NULL || module_name == NULL || status_code == NULL)
		return (-1);

	*status_code = 0;

	/* Read current limits */
	pthread_rwlock_rdlock(&engine->rwlock);
	fuel_limit = engine->fuel_limit;
	mem_limit = engine->memory_limit;
	pthread_rwlock_unlock(&engine->rwlock);

	/* Find the pre-compiled module */
	pthread_rwlock_rdlock(&engine->rwlock);
	module = find_module(engine, module_name);
	pthread_rwlock_unlock(&engine->rwlock);

	if (module == NULL)
		return (-1);

	/* Set up proxy-wasm context */
	memset(&proxy_ctx, 0, sizeof(proxy_ctx));
	proxy_ctx.vrt_ctx = ctx;
	proxy_ctx.memory_valid = 0;
	proxy_ctx.allocator_valid = 0;
	proxy_ctx.root_context_id = 1;
	proxy_ctx.stream_context_id = 2;
	proxy_ctx.local_response_set = 0;
	proxy_ctx.local_response_code = 0;

	/* Create a per-call store with proxy context as data */
	store = wasmtime_store_new(engine->engine, &proxy_ctx, NULL);
	if (store == NULL)
		return (-1);

	context = wasmtime_store_context(store);
	proxy_ctx.wasm_ctx = context;

	/* Set execution limits */
	wasmtime_context_set_fuel(context, fuel_limit);
	wasmtime_store_limiter(store, (int64_t)mem_limit, -1, -1, -1, -1);

	/* Instantiate the module via linker */
	error = wasmtime_linker_instantiate(engine->linker, context,
	    module, &instance, &trap);
	if (error != NULL) {
		log_error(ctx, error, module_name, "instantiate");
		goto cleanup;
	}
	if (trap != NULL) {
		log_trap(ctx, trap, module_name, "instantiate");
		goto cleanup;
	}

	/* Resolve "memory" export */
	if (wasmtime_instance_export_get(context, &instance,
	    "memory", 6, &item) && item.kind == WASMTIME_EXTERN_MEMORY) {
		proxy_ctx.memory = item.of.memory;
		proxy_ctx.memory_valid = 1;
	}

	/* Resolve "proxy_on_memory_allocate" export (for returning strings) */
	if (wasmtime_instance_export_get(context, &instance,
	    "proxy_on_memory_allocate", 24, &item) &&
	    item.kind == WASMTIME_EXTERN_FUNC) {
		proxy_ctx.allocator = item.of.func;
		proxy_ctx.allocator_valid = 1;
	}

	/*
	 * Proxy-Wasm lifecycle sequence:
	 *
	 * 1. proxy_on_context_create(root_id=1, parent_id=0)
	 * 2. proxy_on_vm_start(0, vm_config_size=0)
	 * 3. proxy_on_configure(root_id=1, plugin_config_size=0)
	 * 4. proxy_on_context_create(stream_id=2, root_id=1)
	 * 5. proxy_on_request_headers(stream_id=2, num_headers, end_of_stream=1)
	 */

	/* 1. Create root context */
	args[0].kind = WASMTIME_I32;
	args[0].of.i32 = (int32_t)proxy_ctx.root_context_id;
	args[1].kind = WASMTIME_I32;
	args[1].of.i32 = 0; /* no parent */
	call_wasm_void(context, &instance,
	    "proxy_on_context_create", args, 2);

	/* 2. VM start (unused=0, vm_config_size=0) */
	args[0].kind = WASMTIME_I32;
	args[0].of.i32 = 0;
	args[1].kind = WASMTIME_I32;
	args[1].of.i32 = 0;
	call_wasm_func(context, &instance,
	    "proxy_on_vm_start", args, 2, NULL);

	/* 3. Configure root context */
	args[0].kind = WASMTIME_I32;
	args[0].of.i32 = (int32_t)proxy_ctx.root_context_id;
	args[1].kind = WASMTIME_I32;
	args[1].of.i32 = 0; /* no plugin config */
	call_wasm_func(context, &instance,
	    "proxy_on_configure", args, 2, NULL);

	/* 4. Create stream context */
	args[0].kind = WASMTIME_I32;
	args[0].of.i32 = (int32_t)proxy_ctx.stream_context_id;
	args[1].kind = WASMTIME_I32;
	args[1].of.i32 = (int32_t)proxy_ctx.root_context_id;
	call_wasm_void(context, &instance,
	    "proxy_on_context_create", args, 2);

	/* 5. Call on_request_headers */
	num_headers = 0;
	if (ctx->http_req != NULL)
		num_headers = ctx->http_req->nhd - HTTP_HDR_FIRST;

	args[0].kind = WASMTIME_I32;
	args[0].of.i32 = (int32_t)proxy_ctx.stream_context_id;
	args[1].kind = WASMTIME_I32;
	args[1].of.i32 = num_headers;
	args[2].kind = WASMTIME_I32;
	args[2].of.i32 = 1; /* end_of_stream = true */

	action = 0;
	if (call_wasm_func(context, &instance,
	    "proxy_on_request_headers", args, 3, &action) != 0) {
		log_error(ctx, NULL, module_name, "proxy_on_request_headers");
		goto cleanup;
	}

	/* Check if module called send_local_response */
	if (proxy_ctx.local_response_set) {
		*status_code = proxy_ctx.local_response_code;
		ret = 0;
		goto cleanup;
	}

	*status_code = 0;
	ret = (int)action;

cleanup:
	if (error != NULL)
		wasmtime_error_delete(error);
	if (trap != NULL)
		wasm_trap_delete(trap);
	wasmtime_store_delete(store);
	return (ret);
}
