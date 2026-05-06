/*-
 * Copyright (c) 2025 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VDP (Varnish Delivery Processor) for Proxy-Wasm response body access.
 *
 * Architecture (streaming mode):
 *   1. proxy_wasm_execute() in wasm_engine.c runs the header phase in
 *      vcl_deliver and, if the module exports proxy_on_response_body,
 *      stores the wasm execution state in PRIV_TASK instead of tearing
 *      it down.
 *   2. The user adds `set resp.filters += "wasm_body"` in vcl_deliver.
 *   3. As Varnish delivers the response body, this VDP's bytes() callback
 *      invokes proxy_on_response_body for each chunk (end_of_stream=0),
 *      then passes the chunk through to the client immediately.
 *   4. On VDP_END, proxy_on_response_body is called with end_of_stream=1.
 *   5. In fini(), the wasm lifecycle (log, done, delete) is completed
 *      and the store is destroyed.
 *
 * This eliminates the 1 MiB buffer: body is never accumulated; each chunk
 * is made available via proxy_get_buffer_bytes for the callback duration,
 * then passed through.  The Wasm SDK accumulates internally if needed.
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cache/cache.h"
#include "cache/cache_filter.h"
#include "vcl.h"

#include "vdp_wasm.h"
#include "wasm_engine.h"
#include "store_pool.h"

/* ----------------------------------------------------------------
 * PRIV_TASK identity and cleanup
 * ---------------------------------------------------------------- */

static const char vdp_wasm_task_id_storage = '\0';
const void *vdp_wasm_task_id = &vdp_wasm_task_id_storage;

/*
 * Called by Varnish when PRIV_TASK is freed (end of client request).
 * This handles cleanup in all cases:
 *  - VDP ran and completed: store==NULL, just free the task struct
 *  - VDP never ran (resp.filters not set): full cleanup needed
 */
static void
vdp_wasm_task_fini(VRT_CTX, void *p)
{
	struct vdp_wasm_task *task;

	(void)ctx;
	if (p == NULL)
		return;

	task = p;
	CHECK_OBJ_NOTNULL(task, VDP_WASM_TASK_MAGIC);

	/* If store is still set, VDP never ran — do full cleanup */
	if (task->store != NULL) {
		vwasm_proxy_ctx_cleanup(&task->proxy_ctx);
		if (task->pooled != NULL)
			vwasm_store_pool_release(
			    vwasm_engine_get_pool(task->engine,
			        task->pool_idx),
			    task->pooled);
		else
			wasmtime_store_delete(task->store);
	}
	free(task);
}

const struct vmod_priv_methods vdp_wasm_task_methods = {
	.magic = VMOD_PRIV_METHODS_MAGIC,
	.type = "vdp_wasm_task",
	.fini = vdp_wasm_task_fini,
};

/* ----------------------------------------------------------------
 * VDP callbacks (streaming — no buffering)
 * ---------------------------------------------------------------- */

static int v_matchproto_(vdp_init_f)
vdp_wasm_init(VRT_CTX, struct vdp_ctx *vdc, void **priv)
{
	struct vmod_priv *task_priv;
	struct vdp_wasm_task *task;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(vdc, VDP_CTX_MAGIC);

	/* Retrieve the wasm task context from PRIV_TASK */
	task_priv = VRT_priv_task_get(ctx, vdp_wasm_task_id);
	if (task_priv == NULL || task_priv->priv == NULL)
		return (1);  /* positive = don't push this VDP */

	task = task_priv->priv;
	CHECK_OBJ_NOTNULL(task, VDP_WASM_TASK_MAGIC);

	*priv = task;
	return (0);
}

/*
 * Streaming body callback: invoke proxy_on_response_body for each chunk,
 * then pass data through to the client immediately.  No buffering.
 */
static int v_matchproto_(vdp_bytes_f)
vdp_wasm_bytes(struct vdp_ctx *vdc, enum vdp_action act, void **priv,
    const void *ptr, ssize_t len)
{
	struct vdp_wasm_task *task;
	int end_of_stream;

	CHECK_OBJ_NOTNULL(vdc, VDP_CTX_MAGIC);
	AN(priv);
	task = *priv;
	CHECK_OBJ_NOTNULL(task, VDP_WASM_TASK_MAGIC);

	end_of_stream = (act == VDP_END) ? 1 : 0;

	/*
	 * Call proxy_on_response_body if we have data OR this is end-of-stream.
	 * Make the current chunk available via response_body for the duration
	 * of the callback (proxy_get_buffer_bytes reads from here).
	 */
	if ((len > 0 && ptr != NULL) || end_of_stream) {
		struct vrt_ctx tmp_ctx;
		wasmtime_val_t args[3];

		INIT_OBJ(&tmp_ctx, VRT_CTX_MAGIC);
		tmp_ctx.vsl = vdc->vsl;
		task->proxy_ctx.vrt_ctx = &tmp_ctx;

		/* Expose current chunk to host functions */
		if (len > 0 && ptr != NULL) {
			task->proxy_ctx.response_body = ptr;
			task->proxy_ctx.response_body_len = (size_t)len;
		} else {
			task->proxy_ctx.response_body = NULL;
			task->proxy_ctx.response_body_len = 0;
		}

		args[0].kind = WASMTIME_I32;
		args[0].of.i32 = (int32_t)task->proxy_ctx.stream_context_id;
		args[1].kind = WASMTIME_I32;
		args[1].of.i32 = (int32_t)(len > 0 ? len : 0);
		args[2].kind = WASMTIME_I32;
		args[2].of.i32 = end_of_stream;

		/* Extend epoch deadline for body callback */
		vwasm_engine_reset_epoch_deadline(task->engine, task->context);

		{
			wasmtime_extern_t item;
			if (wasmtime_instance_export_get(task->context,
			    &task->instance, task->phase_body_fn,
			    strlen(task->phase_body_fn), &item) &&
			    item.kind == WASMTIME_EXTERN_FUNC) {
				wasmtime_val_t results[1];
				wasmtime_error_t *err;
				wasm_trap_t *trap = NULL;

				err = wasmtime_func_call(task->context,
				    &item.of.func, args, 3, results, 1,
				    &trap);
				if (err != NULL)
					wasmtime_error_delete(err);
				if (trap != NULL)
					wasm_trap_delete(trap);
			}
		}

		/* Clear body reference */
		task->proxy_ctx.response_body = NULL;
		task->proxy_ctx.response_body_len = 0;
		task->proxy_ctx.vrt_ctx = NULL;
	}

	/* Always pass bytes through to next filter (client) */
	return (VDP_bytes(vdc, act, ptr, len));
}

static int v_matchproto_(vdp_fini_f)
vdp_wasm_fini(struct vdp_ctx *vdc, void **priv)
{
	struct vdp_wasm_task *task;
	wasmtime_val_t args[3];
	struct timespec ts_end;
	long elapsed_ms;

	CHECK_OBJ_NOTNULL(vdc, VDP_CTX_MAGIC);
	if (priv == NULL || *priv == NULL)
		return (0);

	task = *priv;
	CHECK_OBJ_NOTNULL(task, VDP_WASM_TASK_MAGIC);
	*priv = NULL;

	/* --- Complete proxy-wasm lifecycle: log, done, delete --- */
	{
		struct vrt_ctx tmp_ctx;
		INIT_OBJ(&tmp_ctx, VRT_CTX_MAGIC);
		tmp_ctx.vsl = vdc->vsl;
		task->proxy_ctx.vrt_ctx = &tmp_ctx;

		/* Extend epoch deadline for lifecycle callbacks */
		vwasm_engine_reset_epoch_deadline(task->engine, task->context);

		args[0].kind = WASMTIME_I32;
		args[0].of.i32 = (int32_t)task->proxy_ctx.stream_context_id;

		/* proxy_on_log */
		{
			wasmtime_extern_t item;
			if (wasmtime_instance_export_get(task->context,
			    &task->instance, "proxy_on_log", 12, &item) &&
			    item.kind == WASMTIME_EXTERN_FUNC) {
				wasmtime_func_call(task->context,
				    &item.of.func, args, 1, NULL, 0, NULL);
			}
		}

		/* proxy_on_done */
		{
			wasmtime_extern_t item;
			wasmtime_val_t results[1];
			if (wasmtime_instance_export_get(task->context,
			    &task->instance, "proxy_on_done", 13, &item) &&
			    item.kind == WASMTIME_EXTERN_FUNC) {
				wasmtime_func_call(task->context,
				    &item.of.func, args, 1, results, 1,
				    NULL);
			}
		}

		/* proxy_on_delete */
		{
			wasmtime_extern_t item;
			if (wasmtime_instance_export_get(task->context,
			    &task->instance, "proxy_on_delete", 15, &item) &&
			    item.kind == WASMTIME_EXTERN_FUNC) {
				wasmtime_func_call(task->context,
				    &item.of.func, args, 1, NULL, 0, NULL);
			}
		}

		task->proxy_ctx.vrt_ctx = NULL;
	}

	/* --- Statistics and cleanup --- */
	clock_gettime(CLOCK_MONOTONIC, &ts_end);
	elapsed_ms = (ts_end.tv_sec - task->ts_start.tv_sec) * 1000L +
	    (ts_end.tv_nsec - task->ts_start.tv_nsec) / 1000000L;

	if (task->engine != NULL) {
		struct vwasm_stats *st = vwasm_engine_get_stats(task->engine);
		__sync_fetch_and_add(&st->calls_ok, 1);
		if (elapsed_ms > 10)
			__sync_fetch_and_add(&st->calls_timeout, 1);
	}

	/* Log slow execution */
	if (elapsed_ms > 10 && vdc->vsl != NULL) {
		VSLb(vdc->vsl, SLT_VCL_Log,
		    "vmod-wasm: %s VDP execution took %ldms",
		    task->proxy_ctx.module_name, elapsed_ms);
	}

	/* Clean up wasm resources */
	vwasm_proxy_ctx_cleanup(&task->proxy_ctx);
	if (task->pooled != NULL)
		vwasm_store_pool_release(
		    vwasm_engine_get_pool(task->engine, task->pool_idx),
		    task->pooled);
	else
		wasmtime_store_delete(task->store);
	task->store = NULL;  /* Signal to PRIV_TASK fini: already cleaned */

	/*
	 * Don't free task here — PRIV_TASK fini owns it.
	 * task->store == NULL tells fini not to double-cleanup.
	 */

	return (0);
}

/* ----------------------------------------------------------------
 * VDP descriptor
 * ---------------------------------------------------------------- */

const struct vdp vdp_wasm_body = {
	.name	= "wasm_body",
	.init	= vdp_wasm_init,
	.bytes	= vdp_wasm_bytes,
	.fini	= vdp_wasm_fini,
};

/* ================================================================
 * Phase 4: Filter Chain VDP
 *
 * Runs multiple WASM filters on each body chunk in sequence.
 * Each module's output is passed to the next module's input.
 * ================================================================ */

/* Chain task PRIV_TASK identifier */
static const int vdp_wasm_chain_task_key;
const void *vdp_wasm_chain_task_id = &vdp_wasm_chain_task_key;

static void
vdp_wasm_chain_task_free(VRT_CTX, void *priv)
{
	struct vdp_wasm_chain_task *ct;
	int i;

	(void)ctx;
	if (priv == NULL)
		return;

	ct = priv;
	CHECK_OBJ(ct, VDP_WASM_CHAIN_MAGIC);

	for (i = 0; i < ct->ntasks; i++) {
		if (ct->tasks[i].store != NULL) {
			if (ct->tasks[i].pooled != NULL)
				vwasm_store_pool_release(
				    vwasm_engine_get_pool(
				        ct->tasks[i].engine,
				        ct->tasks[i].pool_idx),
				    ct->tasks[i].pooled);
			else
				wasmtime_store_delete(ct->tasks[i].store);
		}
	}

	FREE_OBJ(ct);
}

const struct vmod_priv_methods vdp_wasm_chain_task_methods = {
	.magic = VMOD_PRIV_METHODS_MAGIC,
	.type  = "vdp_wasm_chain_task",
	.fini  = vdp_wasm_chain_task_free,
};

static int v_matchproto_(vdp_init_f)
vdp_wasm_chain_init(VRT_CTX, struct vdp_ctx *vdc, void **priv)
{
	struct vmod_priv *task_priv;
	struct vdp_wasm_chain_task *ct;

	(void)vdc;
	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);

	task_priv = VRT_priv_task_get(ctx, vdp_wasm_chain_task_id);
	if (task_priv == NULL || task_priv->priv == NULL)
		return (-1);

	ct = task_priv->priv;
	CHECK_OBJ(ct, VDP_WASM_CHAIN_MAGIC);

	*priv = ct;
	return (0);
}

static int v_matchproto_(vdp_bytes_f)
vdp_wasm_chain_bytes(struct vdp_ctx *vdc, enum vdp_action act,
    void **priv, const void *ptr, ssize_t len)
{
	struct vdp_wasm_chain_task *ct;
	wasmtime_val_t args[3];
	wasmtime_val_t results[1];
	wasmtime_error_t *error;
	wasm_trap_t *trap = NULL;
	wasmtime_extern_t item;
	int i;
	const void *current_data = ptr;
	ssize_t current_len = len;

	AN(priv);
	ct = *priv;
	CHECK_OBJ(ct, VDP_WASM_CHAIN_MAGIC);

	if (current_data == NULL || current_len <= 0)
		return (VDP_bytes(vdc, act, ptr, len));

	/* Pass body chunk through each filter in sequence */
	for (i = 0; i < ct->ntasks; i++) {
		struct vdp_wasm_task *task = &ct->tasks[i];

		if (task->store == NULL || task->phase_body_fn == NULL)
			continue;

		/* Write data into guest memory via allocator */
		if (task->proxy_ctx.allocator_valid &&
		    task->proxy_ctx.memory_valid) {
			wasmtime_val_t alloc_args[1];
			wasmtime_val_t alloc_result[1];
			uint8_t *mem_data;
			size_t mem_size;
			int32_t guest_ptr;

			alloc_args[0].kind = WASMTIME_I32;
			alloc_args[0].of.i32 = (int32_t)current_len;

			error = wasmtime_func_call(task->context,
			    &task->proxy_ctx.allocator,
			    alloc_args, 1, alloc_result, 1, &trap);
			if (error != NULL || trap != NULL) {
				if (error) wasmtime_error_delete(error);
				if (trap) wasm_trap_delete(trap);
				continue;
			}

			guest_ptr = alloc_result[0].of.i32;
			mem_data = wasmtime_memory_data(task->context,
			    &task->proxy_ctx.memory);
			mem_size = wasmtime_memory_data_size(task->context,
			    &task->proxy_ctx.memory);

			if (guest_ptr >= 0 &&
			    (size_t)guest_ptr + (size_t)current_len <= mem_size)
				memcpy(mem_data + guest_ptr, current_data,
				    (size_t)current_len);
		}

		/* Call proxy_on_response_body */
		if (!wasmtime_instance_export_get(task->context,
		    &task->instance, task->phase_body_fn,
		    strlen(task->phase_body_fn), &item) ||
		    item.kind != WASMTIME_EXTERN_FUNC)
			continue;

		args[0].kind = WASMTIME_I32;
		args[0].of.i32 = (int32_t)task->proxy_ctx.stream_context_id;
		args[1].kind = WASMTIME_I32;
		args[1].of.i32 = (int32_t)current_len;
		args[2].kind = WASMTIME_I32;
		args[2].of.i32 = (act == VDP_END) ? 1 : 0;

		error = wasmtime_func_call(task->context, &item.of.func,
		    args, 3, results, 1, &trap);
		if (error != NULL)
			wasmtime_error_delete(error);
		if (trap != NULL)
			wasm_trap_delete(trap);

		/*
		 * Chain output: if this filter modified the body, use its
		 * output as input to the next filter in the chain.
		 */
		if (task->proxy_ctx.body_modified &&
		    task->proxy_ctx.modified_body != NULL) {
			current_data = task->proxy_ctx.modified_body;
			current_len = (ssize_t)task->proxy_ctx.modified_body_len;
			task->proxy_ctx.body_modified = 0;
		}
	}

	return (VDP_bytes(vdc, act, current_data, current_len));
}

static int v_matchproto_(vdp_fini_f)
vdp_wasm_chain_fini(struct vdp_ctx *vdc, void **priv)
{
	struct vdp_wasm_chain_task *ct;
	int i;

	(void)vdc;

	if (priv == NULL || *priv == NULL)
		return (0);

	ct = *priv;
	CHECK_OBJ(ct, VDP_WASM_CHAIN_MAGIC);
	*priv = NULL;

	/* Call proxy_on_log, proxy_on_done, and proxy_on_delete for each task */
	for (i = 0; i < ct->ntasks; i++) {
		struct vdp_wasm_task *task = &ct->tasks[i];
		wasmtime_val_t args[1];
		wasmtime_extern_t item;

		if (task->store == NULL)
			continue;

		args[0].kind = WASMTIME_I32;
		args[0].of.i32 = (int32_t)task->proxy_ctx.stream_context_id;

		if (wasmtime_instance_export_get(task->context,
		    &task->instance, "proxy_on_log", 12, &item) &&
		    item.kind == WASMTIME_EXTERN_FUNC)
			wasmtime_func_call(task->context, &item.of.func,
			    args, 1, NULL, 0, NULL);

		if (wasmtime_instance_export_get(task->context,
		    &task->instance, "proxy_on_done", 13, &item) &&
		    item.kind == WASMTIME_EXTERN_FUNC)
			wasmtime_func_call(task->context, &item.of.func,
			    args, 1, NULL, 0, NULL);

		/* Notify module its context is being destroyed */
		if (wasmtime_instance_export_get(task->context,
		    &task->instance, "proxy_on_delete", 15, &item) &&
		    item.kind == WASMTIME_EXTERN_FUNC) {
			wasmtime_val_t del_args[1];
			del_args[0].kind = WASMTIME_I32;
			del_args[0].of.i32 = (int32_t)
			    task->proxy_ctx.stream_context_id;
			wasmtime_func_call(task->context, &item.of.func,
			    del_args, 1, NULL, 0, NULL);
		}
	}

	return (0);
}

const struct vdp vdp_wasm_chain_body = {
	.name	= "wasm_body_chain",
	.init	= vdp_wasm_chain_init,
	.bytes	= vdp_wasm_chain_bytes,
	.fini	= vdp_wasm_chain_fini,
};
