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

#define VMOD_WASM_VERSION "0.1.0"

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
		return (0);

	case VCL_EVENT_DISCARD:
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
