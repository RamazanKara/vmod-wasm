/*-
 * Copyright (c) 2026 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VDP (Varnish Delivery Processor) for Proxy-Wasm response body access.
 *
 * Architecture:
 *   1. proxy_wasm_execute() in wasm_engine.c runs the header phase in
 *      vcl_deliver and, if the module exports proxy_on_response_body,
 *      stores the wasm execution state in PRIV_TASK instead of tearing
 *      it down.
 *   2. The user adds `set resp.filters += "wasm_body"` in vcl_deliver.
 *   3. As Varnish delivers the response body, this VDP's bytes() callback
 *      accumulates chunks into a buffer.
 *   4. In fini(), the complete body is passed to proxy_on_response_body,
 *      then the wasm lifecycle (log, done, delete) is completed and the
 *      store is destroyed.
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

#define VWASM_VDP_MAX_BODY	(1024 * 1024)  /* 1 MiB */

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
 * VDP buffer for body accumulation
 * ---------------------------------------------------------------- */

struct vdp_wasm_buf {
	unsigned		 magic;
#define VDP_WASM_BUF_MAGIC	 0x56445042	/* "VDPB" */
	struct vdp_wasm_task	*task;
	uint8_t			*buf;
	size_t			 len;
	size_t			 cap;
	int			 truncated;
};

/* ----------------------------------------------------------------
 * VDP callbacks
 * ---------------------------------------------------------------- */

static int v_matchproto_(vdp_init_f)
vdp_wasm_init(VRT_CTX, struct vdp_ctx *vdc, void **priv)
{
	struct vdp_wasm_buf *vb;
	struct vmod_priv *task_priv;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(vdc, VDP_CTX_MAGIC);

	/* Retrieve the wasm task context from PRIV_TASK */
	task_priv = VRT_priv_task_get(ctx, vdp_wasm_task_id);
	if (task_priv == NULL || task_priv->priv == NULL)
		return (1);  /* positive = don't push this VDP */

	vb = malloc(sizeof(*vb));
	if (vb == NULL)
		return (-1);

	INIT_OBJ(vb, VDP_WASM_BUF_MAGIC);
	vb->task = task_priv->priv;
	CHECK_OBJ_NOTNULL(vb->task, VDP_WASM_TASK_MAGIC);
	vb->buf = NULL;
	vb->len = 0;
	vb->cap = 0;
	vb->truncated = 0;

	*priv = vb;
	return (0);
}

static int v_matchproto_(vdp_bytes_f)
vdp_wasm_bytes(struct vdp_ctx *vdc, enum vdp_action act, void **priv,
    const void *ptr, ssize_t len)
{
	struct vdp_wasm_buf *vb;

	CHECK_OBJ_NOTNULL(vdc, VDP_CTX_MAGIC);
	AN(priv);
	vb = *priv;
	CHECK_OBJ_NOTNULL(vb, VDP_WASM_BUF_MAGIC);

	/* Accumulate body bytes (up to limit) */
	if (len > 0 && ptr != NULL && !vb->truncated) {
		size_t need = vb->len + (size_t)len;

		if (need > VWASM_VDP_MAX_BODY) {
			vb->truncated = 1;
		} else {
			if (need > vb->cap) {
				size_t newcap = vb->cap == 0 ? 4096 : vb->cap;
				while (newcap < need)
					newcap *= 2;
				if (newcap > VWASM_VDP_MAX_BODY)
					newcap = VWASM_VDP_MAX_BODY;
				uint8_t *nb = realloc(vb->buf, newcap);
				if (nb == NULL) {
					vb->truncated = 1;
				} else {
					vb->buf = nb;
					vb->cap = newcap;
				}
			}
			if (!vb->truncated) {
				memcpy(vb->buf + vb->len, ptr, (size_t)len);
				vb->len += (size_t)len;
			}
		}
	}

	/*
	 * Pass-through + buffer mode: pass all bytes to the next filter
	 * immediately (so the client receives data without delay), and on
	 * VDP_END invoke the wasm body callback with the accumulated body.
	 */
	if (act == VDP_END) {
		struct vdp_wasm_task *task = vb->task;
		wasmtime_val_t args[3];
		struct vrt_ctx tmp_ctx;

		CHECK_OBJ_NOTNULL(task, VDP_WASM_TASK_MAGIC);

		/* Construct a valid VRT context for host function calls */
		INIT_OBJ(&tmp_ctx, VRT_CTX_MAGIC);
		tmp_ctx.vsl = vdc->vsl;
		task->proxy_ctx.vrt_ctx = &tmp_ctx;

		/* Make body available to host functions */
		if (vb->len > 0 && !vb->truncated) {
			task->proxy_ctx.response_body = vb->buf;
			task->proxy_ctx.response_body_len = vb->len;
			task->proxy_ctx.response_body_heap = 0;
		}

		/* Call proxy_on_response_body */
		args[0].kind = WASMTIME_I32;
		args[0].of.i32 = (int32_t)task->proxy_ctx.stream_context_id;
		args[1].kind = WASMTIME_I32;
		args[1].of.i32 = (int32_t)vb->len;
		args[2].kind = WASMTIME_I32;
		args[2].of.i32 = 1;  /* end_of_stream */

		/* Refuel for body phase (header phase consumes fuel) */
		wasmtime_context_set_fuel(task->context,
		    vwasm_engine_get_fuel(task->engine));

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
	struct vdp_wasm_buf *vb;
	struct vdp_wasm_task *task;
	wasmtime_val_t args[3];
	struct timespec ts_end;
	long elapsed_ms;

	CHECK_OBJ_NOTNULL(vdc, VDP_CTX_MAGIC);
	if (priv == NULL || *priv == NULL)
		return (0);

	vb = *priv;
	CHECK_OBJ_NOTNULL(vb, VDP_WASM_BUF_MAGIC);
	*priv = NULL;

	task = vb->task;
	CHECK_OBJ_NOTNULL(task, VDP_WASM_TASK_MAGIC);

	/* --- Complete proxy-wasm lifecycle: log, done, delete --- */
	{
		struct vrt_ctx tmp_ctx;
		INIT_OBJ(&tmp_ctx, VRT_CTX_MAGIC);
		tmp_ctx.vsl = vdc->vsl;
		task->proxy_ctx.vrt_ctx = &tmp_ctx;

		/* Refuel for lifecycle callbacks */
		wasmtime_context_set_fuel(task->context,
		    vwasm_engine_get_fuel(task->engine));

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
		if (vb->len > 0)
			__sync_fetch_and_add(&st->body_bytes_in, vb->len);
	}

	/* Log slow execution */
	if (elapsed_ms > 10 && vdc->vsl != NULL) {
		VSLb(vdc->vsl, SLT_VCL_Log,
		    "vmod-wasm: %s VDP execution took %ldms",
		    task->proxy_ctx.module_name, elapsed_ms);
	}

	/* Clean up wasm resources */
	vwasm_proxy_ctx_cleanup(&task->proxy_ctx);
	wasmtime_store_delete(task->store);
	task->store = NULL;  /* Signal to PRIV_TASK fini: already cleaned */

	/* Free body buffer */
	free(vb->buf);
	free(vb);

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
