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

#include "wasm_engine.h"
#include "host_functions.h"

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

	pthread_rwlock_init(&e->rwlock, NULL);
	e->nmodules = 0;

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
	int ret = -1;

	if (engine == NULL || module_name == NULL || func_name == NULL)
		return (-1);

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
	wasmtime_context_set_fuel(context, 100000);

	/* Instantiate the module via linker (resolves host function imports) */
	error = wasmtime_linker_instantiate(engine->linker, context,
	    module, &instance, &trap);
	if (error != NULL || trap != NULL)
		goto cleanup;

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
	if (error != NULL || trap != NULL)
		goto cleanup;

	*result = (int)results[0].of.i32;
	ret = 0;

cleanup:
	if (error != NULL)
		wasmtime_error_delete(error);
	if (trap != NULL)
		wasm_trap_delete(trap);
	wasmtime_store_delete(store);
	return (ret);
}
