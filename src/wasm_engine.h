/*-
 * Copyright (c) 2026 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef WASM_ENGINE_H
#define WASM_ENGINE_H

struct vrt_ctx;
struct wasm_engine;

/* Create a new Wasm engine (wraps wasmtime_engine_t) */
struct wasm_engine *wasm_engine_new(void);

/* Destroy the Wasm engine and free all loaded modules */
void wasm_engine_destroy(struct wasm_engine **enginep);

/* Load and compile a Wasm module from a file path */
int wasm_engine_load_module(struct wasm_engine *engine,
    const char *name, const char *path);

/* Call an exported function in a loaded module. Result stored in *result. */
int wasm_engine_call(struct wasm_engine *engine,
    const struct vrt_ctx *ctx,
    const char *module_name, const char *func_name,
    int *result);

#endif /* WASM_ENGINE_H */
