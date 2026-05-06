/*-
 * Copyright (c) 2025 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Lock-free Store Pool — pre-allocated wasmtime stores with memory
 * snapshot restore for near-zero per-request allocation overhead.
 *
 * Architecture:
 *   - MPMC (multi-producer, multi-consumer) ring buffer
 *   - CAS-based acquire/release (no mutex contention)
 *   - Each pooled store holds a pre-instantiated module
 *   - On acquire: memcpy() memory snapshot to restore post-init state
 *   - On release: reset store state, return to pool
 *   - On exhaustion: fall back to fresh allocation (never block)
 */

#ifndef VWASM_STORE_POOL_H
#define VWASM_STORE_POOL_H

#include <stdint.h>
#include <stddef.h>
#include <wasmtime.h>

/* Pool configuration */
#define VWASM_POOL_DEFAULT_SIZE		64
#define VWASM_POOL_MIN_SIZE		8
#define VWASM_POOL_MAX_SIZE		1024

/* Forward declarations */
struct vwasm_engine;
struct wasm_module_entry;

/* ----------------------------------------------------------------
 * Warm Instance — cached post-initialization memory snapshot
 *
 * Created once per module at load time. Holds the linear memory
 * state after running _initialize, proxy_on_context_create(root),
 * proxy_on_vm_start, and proxy_on_configure.
 * ---------------------------------------------------------------- */

struct vwasm_warm_instance {
	uint8_t			*memory_snapshot;	/* Post-init memory */
	size_t			 snapshot_size;		/* Snapshot byte length */
	size_t			 memory_pages;		/* Number of Wasm pages */
	/* Cached export presence flags (avoid re-lookup per request) */
	int			 has_initialize;
	int			 has_on_context_create;
	int			 has_on_vm_start;
	int			 has_on_configure;
	int			 has_request_headers;
	int			 has_request_body;
	int			 has_response_headers;
	int			 has_response_body;
	int			 has_on_log;
	int			 has_on_done;
	int			 has_on_delete;
	int			 has_allocator;
	/* Valid flag: 0 if warm-up failed (fall back to cold start) */
	int			 valid;
};

/* ----------------------------------------------------------------
 * Pooled Store — a single pre-allocated store slot
 * ---------------------------------------------------------------- */

struct vwasm_pooled_store {
	wasmtime_store_t	*store;
	wasmtime_context_t	*context;
	wasmtime_instance_t	 instance;
	wasmtime_memory_t	 memory;
	int			 memory_valid;
	wasmtime_func_t		 allocator;
	int			 allocator_valid;
	/* Module this store was instantiated from */
	struct wasm_module_entry	*module_entry;
	/* Sequence number for ABA prevention */
	uint64_t		 seq;
	/* Slot index within the pool ring buffer */
	uint32_t		 slot_idx;
};

/* ----------------------------------------------------------------
 * Store Pool — lock-free ring buffer
 * ---------------------------------------------------------------- */

struct vwasm_store_pool {
	struct vwasm_pooled_store **slots;	/* Array of slot pointers */
	size_t			 capacity;
	/* Atomic head/tail counters (monotonically increasing) */
	volatile uint64_t	 head;	/* Next slot to acquire from */
	volatile uint64_t	 tail;	/* Next slot to release into */
	/* Pool statistics (atomic) */
	volatile uint64_t	 acquires;
	volatile uint64_t	 releases;
	volatile uint64_t	 fallbacks;	/* Fresh alloc on exhaustion */
	volatile uint64_t	 resets;
	/* Snapshot reference for reset */
	const struct vwasm_warm_instance *warm_ref;
	/* Engine reference for epoch/limiter setup */
	struct vwasm_engine	*engine;
	/* Pre-allocated pooled store objects */
	struct vwasm_pooled_store *store_objects;
};

/* ----------------------------------------------------------------
 * Pool API
 * ---------------------------------------------------------------- */

/*
 * Create a new store pool for a specific module.
 * pool_size: number of pre-allocated stores (clamped to MIN/MAX).
 * warm: the warm instance whose snapshot will be used for resets.
 * engine: parent engine (for epoch/limiter configuration).
 * entry: module entry to instantiate stores from.
 *
 * Returns NULL on allocation failure.
 */
struct vwasm_store_pool *vwasm_store_pool_new(
    size_t pool_size,
    const struct vwasm_warm_instance *warm,
    struct vwasm_engine *engine,
    struct wasm_module_entry *entry);

/*
 * Destroy the pool and all contained stores.
 * Safe to call with NULL.
 */
void vwasm_store_pool_destroy(struct vwasm_store_pool **poolp);

/*
 * Acquire a pre-instantiated store from the pool.
 * The store's linear memory is restored to the post-init snapshot.
 *
 * Returns a pooled store, or NULL if pool is exhausted.
 * Caller must call vwasm_store_pool_release() when done.
 *
 * If NULL is returned, caller should fall back to fresh allocation.
 */
struct vwasm_pooled_store *vwasm_store_pool_acquire(
    struct vwasm_store_pool *pool);

/*
 * Release a store back to the pool.
 * The store's memory will be restored from snapshot on next acquire.
 */
void vwasm_store_pool_release(struct vwasm_store_pool *pool,
    struct vwasm_pooled_store *ps);

/*
 * Get pool statistics as JSON string (caller must free).
 * Returns NULL on allocation failure.
 */
char *vwasm_store_pool_stats_json(const struct vwasm_store_pool *pool);

/* ----------------------------------------------------------------
 * Warm Instance API
 * ---------------------------------------------------------------- */

/*
 * Create a warm instance by running the module initialization lifecycle.
 * Allocates and populates the warm_instance struct.
 *
 * engine: parent engine (for linker, epoch config).
 * entry: module entry with compiled module and instance_pre.
 * vm_config: VM configuration string (may be NULL).
 * plugin_config: plugin configuration string (may be NULL).
 *
 * Returns 0 on success, -1 on failure.
 * On failure, warm->valid is set to 0 (cold-start fallback).
 */
int vwasm_warm_instance_create(struct vwasm_warm_instance *warm,
    struct vwasm_engine *engine,
    struct wasm_module_entry *entry,
    const char *vm_config,
    const char *plugin_config);

/*
 * Destroy a warm instance and free its memory snapshot.
 */
void vwasm_warm_instance_destroy(struct vwasm_warm_instance *warm);

/*
 * Restore a store's linear memory from the warm instance snapshot.
 * Called during pool acquire to reset state to post-init.
 *
 * Returns 0 on success, -1 if memory size mismatch.
 */
int vwasm_warm_instance_restore(const struct vwasm_warm_instance *warm,
    wasmtime_context_t *context,
    wasmtime_memory_t *memory);

#endif /* VWASM_STORE_POOL_H */
