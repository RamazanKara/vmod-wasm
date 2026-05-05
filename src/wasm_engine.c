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
#include <time.h>

#include <wasm.h>
#include <wasmtime.h>

#include "cache/cache.h"
#include "vcl.h"

#include "wasm_engine.h"
#include "host_functions.h"
#include "proxy_wasm.h"
#include "proxy_wasm_shared.h"
#include "vdp_wasm.h"

#define MAX_MODULES 64
#define VWASM_MAX_BODY_CACHE	(1024 * 1024)  /* 1 MiB max body to cache */
#define VWASM_BODY_STACK_MAX	(64 * 1024)    /* 64 KiB stack buffer for bodies */

struct wasm_module_entry {
	char			*name;
	wasmtime_module_t	*module;
	wasmtime_instance_pre_t	*instance_pre;
};

struct vwasm_engine {
	wasm_engine_t		*engine;
	wasmtime_linker_t	*linker;
	struct wasm_module_entry	modules[MAX_MODULES];
	int			nmodules;
	/* Execution limits (set once in vcl_init, immutable after) */
	uint64_t		fuel_limit;
	size_t			memory_limit;
	/* Security configuration */
	char			**allowed_upstreams;
	uint32_t		num_allowed_upstreams;
	uint32_t		http_call_max;
	int			fail_mode;
	/* Execution statistics */
	struct vwasm_stats	stats;
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

	e->nmodules = 0;
	e->fuel_limit = VWASM_DEFAULT_FUEL;
	e->memory_limit = VWASM_DEFAULT_MEMLIMIT;
	e->allowed_upstreams = NULL;
	e->num_allowed_upstreams = 0;
	e->http_call_max = VWASM_DEFAULT_HTTP_CALL_MAX;
	e->fail_mode = VWASM_FAIL_CLOSED;

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
		wasmtime_instance_pre_delete(e->modules[i].instance_pre);
		wasmtime_module_delete(e->modules[i].module);
	}

	wasmtime_linker_delete(e->linker);
	wasm_engine_delete(e->engine);

	/* Free allowed upstreams list */
	if (e->allowed_upstreams != NULL) {
		for (i = 0; i < (int)e->num_allowed_upstreams; i++)
			free(e->allowed_upstreams[i]);
		free(e->allowed_upstreams);
	}

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
	engine->fuel_limit = fuel;
}

void
vwasm_engine_set_memory_limit(struct vwasm_engine *engine, size_t bytes)
{
	if (engine == NULL)
		return;
	engine->memory_limit = bytes;
}

uint64_t
vwasm_engine_get_fuel(struct vwasm_engine *engine)
{
	if (engine == NULL)
		return (VWASM_DEFAULT_FUEL);
	return (engine->fuel_limit);
}

size_t
vwasm_engine_get_memory_limit(struct vwasm_engine *engine)
{
	if (engine == NULL)
		return (VWASM_DEFAULT_MEMLIMIT);
	return (engine->memory_limit);
}

/* ----------------------------------------------------------------
 * Security configuration setters/getters
 * ---------------------------------------------------------------- */

void
vwasm_engine_set_allowed_upstreams(struct vwasm_engine *engine,
    const char *upstream_list)
{
	char *dup, *tok, *saveptr;
	uint32_t count, i;
	char **new_list;

	if (engine == NULL)
		return;

	/* Free existing list */
	if (engine->allowed_upstreams != NULL) {
		for (i = 0; i < engine->num_allowed_upstreams; i++)
			free(engine->allowed_upstreams[i]);
		free(engine->allowed_upstreams);
		engine->allowed_upstreams = NULL;
		engine->num_allowed_upstreams = 0;
	}

	/* Empty or NULL means "allow all" */
	if (upstream_list == NULL || *upstream_list == '\0')
		return;

	/* Count tokens */
	dup = strdup(upstream_list);
	if (dup == NULL)
		return;

	count = 0;
	tok = strtok_r(dup, ",", &saveptr);
	while (tok != NULL) {
		while (*tok == ' ' || *tok == '\t')
			tok++;
		if (*tok != '\0')
			count++;
		tok = strtok_r(NULL, ",", &saveptr);
	}
	free(dup);

	if (count == 0)
		return;

	new_list = calloc(count, sizeof(char *));
	if (new_list == NULL)
		return;

	/* Parse again to fill list */
	dup = strdup(upstream_list);
	if (dup == NULL) {
		free(new_list);
		return;
	}

	i = 0;
	tok = strtok_r(dup, ",", &saveptr);
	while (tok != NULL && i < count) {
		while (*tok == ' ' || *tok == '\t')
			tok++;
		if (*tok != '\0') {
			new_list[i] = strdup(tok);
			if (new_list[i] == NULL) {
				uint32_t j;
				for (j = 0; j < i; j++)
					free(new_list[j]);
				free(new_list);
				free(dup);
				return;
			}
			/* Trim trailing whitespace */
			size_t len = strlen(new_list[i]);
			while (len > 0 &&
			    (new_list[i][len-1] == ' ' ||
			     new_list[i][len-1] == '\t'))
				new_list[i][--len] = '\0';
			i++;
		}
		tok = strtok_r(NULL, ",", &saveptr);
	}
	free(dup);

	engine->allowed_upstreams = new_list;
	engine->num_allowed_upstreams = i;
}

void
vwasm_engine_set_http_call_max(struct vwasm_engine *engine, uint32_t max_calls)
{
	if (engine == NULL)
		return;
	engine->http_call_max = max_calls;
}

void
vwasm_engine_set_fail_mode(struct vwasm_engine *engine, int mode)
{
	if (engine == NULL)
		return;
	engine->fail_mode = (mode == VWASM_FAIL_OPEN) ?
	    VWASM_FAIL_OPEN : VWASM_FAIL_CLOSED;
}

int
vwasm_engine_get_fail_mode(struct vwasm_engine *engine)
{
	if (engine == NULL)
		return (VWASM_FAIL_CLOSED);
	return (engine->fail_mode);
}

uint32_t
vwasm_engine_get_http_call_max(struct vwasm_engine *engine)
{
	if (engine == NULL)
		return (VWASM_DEFAULT_HTTP_CALL_MAX);
	return (engine->http_call_max);
}

struct vwasm_stats *
vwasm_engine_get_stats(struct vwasm_engine *engine)
{
	if (engine == NULL)
		return (NULL);
	return (&engine->stats);
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
	if (engine->nmodules >= MAX_MODULES) {
		wasmtime_module_delete(module);
		return (-1);
	}

	/* Pre-instantiate: validate imports at load time (fail-fast) */
	error = wasmtime_linker_instantiate_pre(engine->linker, module,
	    &engine->modules[engine->nmodules].instance_pre);
	if (error != NULL) {
		wasmtime_error_delete(error);
		wasmtime_module_delete(module);
		return (-1);
	}

	engine->modules[engine->nmodules].name = strdup(name);
	engine->modules[engine->nmodules].module = module;
	engine->nmodules++;
	ret = 0;

	return (ret);
}

static struct wasm_module_entry *
find_module(struct vwasm_engine *engine, const char *name)
{
	int i;

	for (i = 0; i < engine->nmodules; i++) {
		if (strcmp(engine->modules[i].name, name) == 0)
			return (&engine->modules[i]);
	}
	return (NULL);
}

int
vwasm_engine_call(struct vwasm_engine *engine,
    const struct vrt_ctx *ctx,
    const char *module_name, const char *func_name,
    int *result)
{
	struct wasm_module_entry *entry;
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
	fuel_limit = engine->fuel_limit;
	mem_limit = engine->memory_limit;

	/* Find the pre-compiled module */
	entry = find_module(engine, module_name);

	if (entry == NULL)
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

	/* Instantiate from pre-validated instance (skips import resolution) */
	error = wasmtime_instance_pre_instantiate(entry->instance_pre,
	    context, &instance, &trap);
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
		wasm_message_t msg;
		wasmtime_error_message(error, &msg);
		fprintf(stderr, "WASM ERROR in %s: %.*s\n",
		    func_name, (int)msg.size, msg.data);
		wasm_byte_vec_delete(&msg);
		wasmtime_error_delete(error);
		return (-1);
	}
	if (trap != NULL) {
		wasm_message_t msg;
		wasm_trap_message(trap, &msg);
		fprintf(stderr, "WASM TRAP in %s: %.*s\n",
		    func_name, (int)msg.size, msg.data);
		wasm_byte_vec_delete(&msg);
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
		wasm_message_t msg;
		wasmtime_error_message(error, &msg);
		fprintf(stderr, "WASM ERROR in %s: %.*s\n",
		    func_name, (int)msg.size, msg.data);
		wasm_byte_vec_delete(&msg);
		wasmtime_error_delete(error);
		return (-1);
	}
	if (trap != NULL) {
		wasm_message_t msg;
		wasm_trap_message(trap, &msg);
		fprintf(stderr, "WASM TRAP in %s: %.*s\n",
		    func_name, (int)msg.size, msg.data);
		wasm_byte_vec_delete(&msg);
		wasm_trap_delete(trap);
		return (-1);
	}
	return (0);
}

/* ----------------------------------------------------------------
 * Proxy-Wasm unified lifecycle execution.
 *
 * Runs the full ABI v0.2.1 callback sequence for either request or
 * response phase, with optional vm_config and plugin_config.
 * Returns the action from proxy_on_{request,response}_headers,
 * or -1 on error.
 * ---------------------------------------------------------------- */

#define VWASM_PHASE_REQUEST  0
#define VWASM_PHASE_RESPONSE 1

/* Callback for VRB_Iterate: collects body chunks into a flat buffer */
struct body_collect {
	uint8_t		*buf;
	size_t		 len;
	size_t		 cap;
};

static int
body_collect_cb(void *priv, unsigned flush, const void *ptr, ssize_t len)
{
	struct body_collect *bc;

	(void)flush;
	bc = (struct body_collect *)priv;
	if (len <= 0)
		return (0);
	if (bc->len + (size_t)len > bc->cap)
		return (-1);
	memcpy(bc->buf + bc->len, ptr, (size_t)len);
	bc->len += (size_t)len;
	return (0);
}

static int
proxy_wasm_execute(struct vwasm_engine *engine,
    const struct vrt_ctx *ctx,
    const char *module_name, int phase,
    const char *vm_config, const char *plugin_config,
    int *status_code)
{
	struct wasm_module_entry *entry;
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
	const struct http *hp;
	size_t vm_config_len;
	size_t plugin_config_len;
	const char *phase_headers_fn;
	const char *phase_body_fn;
	int has_body_handler;
	struct timespec ts_start, ts_end;
	long elapsed_ms;

	if (engine == NULL || module_name == NULL || status_code == NULL)
		return (-1);

	*status_code = 0;
	vm_config_len = (vm_config != NULL) ? strlen(vm_config) : 0;
	plugin_config_len = (plugin_config != NULL) ? strlen(plugin_config) : 0;

	phase_headers_fn = (phase == VWASM_PHASE_REQUEST) ?
	    "proxy_on_request_headers" : "proxy_on_response_headers";
	phase_body_fn = (phase == VWASM_PHASE_REQUEST) ?
	    "proxy_on_request_body" : "proxy_on_response_body";

	/* Read current limits */
	fuel_limit = engine->fuel_limit;
	mem_limit = engine->memory_limit;

	/* Find the pre-compiled module */
	entry = find_module(engine, module_name);

	if (entry == NULL)
		return (-1);

	/* Set up proxy-wasm context */
	memset(&proxy_ctx, 0, sizeof(proxy_ctx));
	proxy_ctx.vrt_ctx = ctx;
	proxy_ctx.root_context_id = 1;
	proxy_ctx.stream_context_id = 2;
	proxy_ctx.module_name = module_name;
	proxy_ctx.vm_config = vm_config;
	proxy_ctx.vm_config_len = vm_config_len;
	proxy_ctx.plugin_config = plugin_config;
	proxy_ctx.plugin_config_len = plugin_config_len;
	proxy_ctx.shared_data = vwasm_proxy_wasm_get_shared_data();
	proxy_ctx.queue_store = vwasm_proxy_wasm_get_queue_store();
	proxy_ctx.metric_store = vwasm_proxy_wasm_get_metric_store();
	proxy_ctx.http_call_max = engine->http_call_max;
	proxy_ctx.allowed_upstreams = (const char **)engine->allowed_upstreams;
	proxy_ctx.num_allowed_upstreams = engine->num_allowed_upstreams;

	/* Create a per-call store with proxy context as data */
	clock_gettime(CLOCK_MONOTONIC, &ts_start);
	store = wasmtime_store_new(engine->engine, &proxy_ctx, NULL);
	if (store == NULL)
		return (-1);

	context = wasmtime_store_context(store);
	proxy_ctx.wasm_ctx = context;

	/* Set execution limits */
	wasmtime_context_set_fuel(context, fuel_limit);
	wasmtime_store_limiter(store, (int64_t)mem_limit, -1, -1, -1, -1);

	/* Instantiate from pre-validated instance */
	error = wasmtime_instance_pre_instantiate(entry->instance_pre,
	    context, &instance, &trap);
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

	/* Resolve allocator */
	if (wasmtime_instance_export_get(context, &instance,
	    "proxy_on_memory_allocate", 24, &item) &&
	    item.kind == WASMTIME_EXTERN_FUNC) {
		proxy_ctx.allocator = item.of.func;
		proxy_ctx.allocator_valid = 1;
	} else if (wasmtime_instance_export_get(context, &instance,
	    "malloc", 6, &item) &&
	    item.kind == WASMTIME_EXTERN_FUNC) {
		proxy_ctx.allocator = item.of.func;
		proxy_ctx.allocator_valid = 1;
	}

	/* Call _initialize if exported */
	call_wasm_void(context, &instance, "_initialize", NULL, 0);

	/* 1. Create root context */
	args[0].kind = WASMTIME_I32;
	args[0].of.i32 = (int32_t)proxy_ctx.root_context_id;
	args[1].kind = WASMTIME_I32;
	args[1].of.i32 = 0;
	call_wasm_void(context, &instance,
	    "proxy_on_context_create", args, 2);

	/* 2. VM start */
	args[0].kind = WASMTIME_I32;
	args[0].of.i32 = (int32_t)proxy_ctx.root_context_id;
	args[1].kind = WASMTIME_I32;
	args[1].of.i32 = (int32_t)vm_config_len;
	call_wasm_func(context, &instance,
	    "proxy_on_vm_start", args, 2, NULL);

	/* 3. Configure */
	args[0].kind = WASMTIME_I32;
	args[0].of.i32 = (int32_t)proxy_ctx.root_context_id;
	args[1].kind = WASMTIME_I32;
	args[1].of.i32 = (int32_t)plugin_config_len;
	call_wasm_func(context, &instance,
	    "proxy_on_configure", args, 2, NULL);

	/* 4. Create stream context */
	args[0].kind = WASMTIME_I32;
	args[0].of.i32 = (int32_t)proxy_ctx.stream_context_id;
	args[1].kind = WASMTIME_I32;
	args[1].of.i32 = (int32_t)proxy_ctx.root_context_id;
	call_wasm_void(context, &instance,
	    "proxy_on_context_create", args, 2);

	/* 5. Call phase-specific headers callback */
	if (phase == VWASM_PHASE_REQUEST) {
		hp = ctx->http_req;
	} else {
		hp = (ctx->http_beresp != NULL) ?
		    ctx->http_beresp : ctx->http_resp;
	}
	num_headers = (hp != NULL) ? hp->nhd - HTTP_HDR_FIRST : 0;

	args[0].kind = WASMTIME_I32;
	args[0].of.i32 = (int32_t)proxy_ctx.stream_context_id;
	args[1].kind = WASMTIME_I32;
	args[1].of.i32 = num_headers;
	args[2].kind = WASMTIME_I32;
	args[2].of.i32 = 1; /* end_of_stream */

	action = 0;
	if (call_wasm_func(context, &instance,
	    phase_headers_fn, args, 3, &action) != 0) {
		log_error(ctx, NULL, module_name, phase_headers_fn);
		goto cleanup;
	}

	/* Check if module called send_local_response */
	if (proxy_ctx.local_response_set) {
		*status_code = proxy_ctx.local_response_code;
		ret = 0;
		goto cleanup;
	}

	/* 6. Body callback — pass actual body if available */
	has_body_handler = wasmtime_instance_export_get(context, &instance,
	    phase_body_fn, strlen(phase_body_fn), &item) &&
	    item.kind == WASMTIME_EXTERN_FUNC;

	if (has_body_handler && phase == VWASM_PHASE_REQUEST &&
	    ctx->req != NULL &&
	    ctx->req->req_body_status != BS_NONE) {
		VCL_BYTES cached_size;
		cached_size = VRT_CacheReqBody(ctx, VWASM_MAX_BODY_CACHE);
		if (cached_size > 0) {
			struct body_collect bc;
			uint8_t stack_body[VWASM_BODY_STACK_MAX];
			int heap_alloc = 0;

			if ((size_t)cached_size <= sizeof(stack_body)) {
				bc.buf = stack_body;
			} else {
				bc.buf = malloc((size_t)cached_size);
				heap_alloc = 1;
			}
			if (bc.buf != NULL) {
				bc.len = 0;
				bc.cap = (size_t)cached_size;
				if (VRB_Iterate(ctx->req->wrk,
				    ctx->req->vsl, ctx->req,
				    body_collect_cb, &bc) >= 0 &&
				    bc.len > 0) {
					proxy_ctx.request_body = bc.buf;
					proxy_ctx.request_body_len = bc.len;
					proxy_ctx.request_body_heap =
					    heap_alloc;
				} else if (heap_alloc) {
					free(bc.buf);
				}
			}
		}
	}

	/*
	 * Response body: defer to VDP pipeline.
	 *
	 * If the module exports proxy_on_response_body, we store the wasm
	 * execution state in PRIV_TASK so the VDP "wasm_body" can invoke it
	 * after the body has been delivered.  The VDP fini() completes the
	 * lifecycle (log, done, delete) and destroys the store.
	 */
	if (has_body_handler && phase == VWASM_PHASE_RESPONSE) {
		struct vdp_wasm_task *task;
		struct vmod_priv *tp;

		task = malloc(sizeof(*task));
		if (task == NULL)
			goto cleanup;

		INIT_OBJ(task, VDP_WASM_TASK_MAGIC);
		task->engine = engine;
		task->store = store;
		task->context = context;
		task->instance = instance;
		task->phase_body_fn = phase_body_fn;
		task->ts_start = ts_start;
		/* Move proxy_ctx to heap (task owns it now) */
		memcpy(&task->proxy_ctx, &proxy_ctx, sizeof(proxy_ctx));
		/* Update wasmtime store data to point to new proxy_ctx location */
		wasmtime_context_set_data(task->context, &task->proxy_ctx);

		/* Store in PRIV_TASK for VDP to retrieve */
		tp = VRT_priv_task(ctx, vdp_wasm_task_id);
		if (tp == NULL) {
			vwasm_proxy_ctx_cleanup(&task->proxy_ctx);
			wasmtime_store_delete(store);
			free(task);
			goto cleanup;
		}
		tp->priv = task;
		tp->methods = &vdp_wasm_task_methods;

		/* Count the call (VDP fini will count success/body stats) */
		__sync_fetch_and_add(&engine->stats.calls_total, 1);
		if (proxy_ctx.local_response_set)
			__sync_fetch_and_add(
			    &engine->stats.local_responses, 1);

		/*
		 * Return success — body/log/done/delete are deferred to VDP.
		 * Do NOT free store or proxy_ctx here; VDP fini owns them.
		 * Zero out proxy_ctx so cleanup label won't double-free.
		 */
		memset(&proxy_ctx, 0, sizeof(proxy_ctx));
		store = NULL;
		*status_code = 0;
		return ((int)action);
	}

	/* Request body: invoke proxy_on_request_body inline */
	if (has_body_handler && phase == VWASM_PHASE_REQUEST) {
		args[0].kind = WASMTIME_I32;
		args[0].of.i32 = (int32_t)proxy_ctx.stream_context_id;
		args[1].kind = WASMTIME_I32;
		args[1].of.i32 = (int32_t)proxy_ctx.request_body_len;
		args[2].kind = WASMTIME_I32;
		args[2].of.i32 = 1;
		call_wasm_func(context, &instance,
		    phase_body_fn, args, 3, NULL);
	}

	/* 7. proxy_on_log */
	args[0].kind = WASMTIME_I32;
	args[0].of.i32 = (int32_t)proxy_ctx.stream_context_id;
	call_wasm_void(context, &instance, "proxy_on_log", args, 1);

	/* 8. proxy_on_done */
	args[0].kind = WASMTIME_I32;
	args[0].of.i32 = (int32_t)proxy_ctx.stream_context_id;
	call_wasm_func(context, &instance, "proxy_on_done", args, 1, NULL);

	/* 9. proxy_on_delete */
	args[0].kind = WASMTIME_I32;
	args[0].of.i32 = (int32_t)proxy_ctx.stream_context_id;
	call_wasm_void(context, &instance, "proxy_on_delete", args, 1);

	*status_code = 0;
	ret = (int)action;

cleanup:
	clock_gettime(CLOCK_MONOTONIC, &ts_end);
	elapsed_ms = (ts_end.tv_sec - ts_start.tv_sec) * 1000L +
	    (ts_end.tv_nsec - ts_start.tv_nsec) / 1000000L;
	if (elapsed_ms > 10) {
		VSLb(ctx->req != NULL ? ctx->req->vsl :
		    (ctx->bo != NULL ? ctx->bo->vsl : NULL),
		    SLT_VCL_Log,
		    "vmod-wasm: %s execution took %ldms",
		    module_name, elapsed_ms);
	}

	/* Update execution statistics (atomic, thread-safe) */
	__sync_fetch_and_add(&engine->stats.calls_total, 1);
	if (ret >= 0)
		__sync_fetch_and_add(&engine->stats.calls_ok, 1);
	else
		__sync_fetch_and_add(&engine->stats.calls_error, 1);
	if (elapsed_ms > 10)
		__sync_fetch_and_add(&engine->stats.calls_timeout, 1);
	if (proxy_ctx.local_response_set)
		__sync_fetch_and_add(&engine->stats.local_responses, 1);
	if (proxy_ctx.http_call_count > 0)
		__sync_fetch_and_add(&engine->stats.http_calls,
		    proxy_ctx.http_call_count);
	if (proxy_ctx.request_body_len > 0)
		__sync_fetch_and_add(&engine->stats.body_bytes_in,
		    proxy_ctx.request_body_len);
	if (proxy_ctx.response_body_len > 0)
		__sync_fetch_and_add(&engine->stats.body_bytes_in,
		    proxy_ctx.response_body_len);

	vwasm_proxy_ctx_cleanup(&proxy_ctx);
	if (error != NULL)
		wasmtime_error_delete(error);
	if (trap != NULL)
		wasm_trap_delete(trap);
	if (store != NULL)
		wasmtime_store_delete(store);
	return (ret);
}

/* Public API: thin wrappers around proxy_wasm_execute */

int
vwasm_proxy_wasm_call(struct vwasm_engine *engine,
    const struct vrt_ctx *ctx,
    const char *module_name,
    int *status_code)
{
	return (proxy_wasm_execute(engine, ctx, module_name,
	    VWASM_PHASE_REQUEST, "", "", status_code));
}

int
vwasm_proxy_wasm_response_call(struct vwasm_engine *engine,
    const struct vrt_ctx *ctx,
    const char *module_name,
    int *status_code)
{
	return (proxy_wasm_execute(engine, ctx, module_name,
	    VWASM_PHASE_RESPONSE, "", "", status_code));
}

int
vwasm_proxy_wasm_call_with_config(struct vwasm_engine *engine,
    const struct vrt_ctx *ctx,
    const char *module_name,
    const char *vm_config,
    const char *plugin_config,
    int *status_code)
{
	return (proxy_wasm_execute(engine, ctx, module_name,
	    VWASM_PHASE_REQUEST, vm_config, plugin_config, status_code));
}

int
vwasm_proxy_wasm_response_call_with_config(struct vwasm_engine *engine,
    const struct vrt_ctx *ctx,
    const char *module_name,
    const char *vm_config,
    const char *plugin_config,
    int *status_code)
{
	return (proxy_wasm_execute(engine, ctx, module_name,
	    VWASM_PHASE_RESPONSE, vm_config, plugin_config, status_code));
}
