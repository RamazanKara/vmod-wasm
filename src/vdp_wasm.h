/*-
 * Copyright (c) 2026 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VDP (Varnish Delivery Processor) for Proxy-Wasm response body access.
 *
 * This VDP intercepts response body bytes as they are delivered to the
 * client, buffers them, and invokes proxy_on_response_body with the
 * complete body when delivery finishes.
 */

#ifndef VWASM_VDP_WASM_H
#define VWASM_VDP_WASM_H

#include <time.h>
#include <wasmtime.h>

#include "proxy_wasm.h"

/* Forward declarations — avoid pulling in cache_filter.h here */
struct vdp;
struct vfp;
struct vmod_priv_methods;

struct vwasm_engine;

/*
 * VRT filter registration — declared in cache_filter.h but we
 * forward-declare here to avoid include conflicts.
 */
const char *VRT_AddFilter(VRT_CTX, const struct vfp *, const struct vdp *);
void VRT_RemoveFilter(VRT_CTX, const struct vfp *, const struct vdp *);

/*
 * Task context passed from vcl_deliver (proxy_wasm_execute) to VDP
 * callbacks via PRIV_TASK.  Heap-allocated; freed by PRIV_TASK fini.
 */
struct vdp_wasm_task {
	unsigned		 magic;
#define VDP_WASM_TASK_MAGIC	 0x5744504d	/* "WDPM" */
	struct vwasm_engine	*engine;
	wasmtime_store_t	*store;
	wasmtime_context_t	*context;
	wasmtime_instance_t	 instance;
	struct vwasm_proxy_ctx	 proxy_ctx;
	const char		*phase_body_fn;
	struct timespec		 ts_start;
};

/* The VDP descriptor — registered via VRT_AddFilter */
extern const struct vdp vdp_wasm_body;

/* PRIV_TASK identifier (address used as unique key) */
extern const void *vdp_wasm_task_id;

/* PRIV_TASK methods (for cleanup if VDP never runs) */
extern const struct vmod_priv_methods vdp_wasm_task_methods;

#endif /* VWASM_VDP_WASM_H */
