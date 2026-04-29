/*-
 * Copyright (c) 2026 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef VWASM_ENGINE_H
#define VWASM_ENGINE_H

struct vrt_ctx;
struct vwasm_engine;

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

#endif /* VWASM_ENGINE_H */
