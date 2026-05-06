/*-
 * Copyright (c) 2025 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VDP (Varnish Delivery Processor) for Proxy-Wasm response body access.
 *
 * Streaming mode: each body chunk is passed to proxy_on_response_body
 * immediately, then forwarded to the client.  No buffering.
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
struct vwasm_pooled_store;

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
	struct vwasm_pooled_store *pooled;
	int			 pool_idx;
};

/* The VDP descriptor — registered via VRT_AddFilter */
extern const struct vdp vdp_wasm_body;

/* Phase 4: Filter chain VDP — runs multiple filters on each chunk */
extern const struct vdp vdp_wasm_chain_body;

/*
 * Chain task: array of module tasks for filter chain VDP.
 * Each body chunk passes through all filters in sequence.
 */
#define VDP_WASM_CHAIN_MAX	16

struct vdp_wasm_chain_task {
	unsigned		 magic;
#define VDP_WASM_CHAIN_MAGIC	 0x57444343	/* "WDCC" */
	struct vwasm_engine	*engine;
	struct vdp_wasm_task	 tasks[VDP_WASM_CHAIN_MAX];
	int			 ntasks;
	struct timespec		 ts_start;
};

/* PRIV_TASK identifier (address used as unique key) */
extern const void *vdp_wasm_task_id;

/* PRIV_TASK identifier for chain tasks */
extern const void *vdp_wasm_chain_task_id;

/* PRIV_TASK methods (for cleanup if VDP never runs) */
extern const struct vmod_priv_methods vdp_wasm_task_methods;
extern const struct vmod_priv_methods vdp_wasm_chain_task_methods;

#endif /* VWASM_VDP_WASM_H */
