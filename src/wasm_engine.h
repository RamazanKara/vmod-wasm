/*-
 * Copyright (c) 2026 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef VWASM_ENGINE_H
#define VWASM_ENGINE_H

#include <stdint.h>

struct vrt_ctx;
struct vwasm_engine;

/* Default execution limits */
#define VWASM_DEFAULT_FUEL	1000000ULL	/* ~1M instructions */
#define VWASM_DEFAULT_MEMLIMIT	(16 * 1024 * 1024)	/* 16 MiB */
#define VWASM_DEFAULT_STACKSIZE	(512 * 1024)		/* 512 KiB */

/* Create a new Wasm engine (wraps wasmtime_engine_t) */
struct vwasm_engine *vwasm_engine_new(void);

/* Destroy the Wasm engine and free all loaded modules */
void vwasm_engine_destroy(struct vwasm_engine **enginep);

/* Load and compile a Wasm module from a file path */
int vwasm_engine_load_module(struct vwasm_engine *engine,
    const char *name, const char *path);

/* Call an exported function in a loaded module. Result stored in *result. */
int vwasm_engine_call(struct vwasm_engine *engine,
    const struct vrt_ctx *ctx,
    const char *module_name, const char *func_name,
    int *result);

/* Configure execution limits (thread-safe) */
void vwasm_engine_set_fuel(struct vwasm_engine *engine, uint64_t fuel);
void vwasm_engine_set_memory_limit(struct vwasm_engine *engine, size_t bytes);

/* Query current limits */
uint64_t vwasm_engine_get_fuel(struct vwasm_engine *engine);
size_t vwasm_engine_get_memory_limit(struct vwasm_engine *engine);

/*
 * Execute a Proxy-Wasm module lifecycle for HTTP request filtering.
 *
 * Runs the full Proxy-Wasm lifecycle:
 *   1. proxy_on_context_create(1, 0)    — root context
 *   2. proxy_on_vm_start(0, 0)          — VM startup
 *   3. proxy_on_configure(1, 0)         — plugin configuration
 *   4. proxy_on_context_create(2, 1)    — stream context
 *   5. proxy_on_request_headers(2, n, 1) — HTTP request filtering
 *
 * Returns: action code (0=CONTINUE, 1=PAUSE), or -1 on error.
 * If the module called send_local_response, *status_code is set.
 */
int vwasm_proxy_wasm_call(struct vwasm_engine *engine,
    const struct vrt_ctx *ctx,
    const char *module_name,
    int *status_code);

#endif /* VWASM_ENGINE_H */
