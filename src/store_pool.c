/*-
 * Copyright (c) 2025 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Lock-free Store Pool implementation.
 *
 * Uses per-slot atomic flags for MPMC (multi-producer, multi-consumer)
 * allocation without ABA issues. Each slot has a volatile int "in_use"
 * flag: 0=free, 1=acquired. Acquire scans from a rotating start index
 * to distribute load evenly across slots.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <wasm.h>
#include <wasmtime.h>

#include "store_pool.h"
#include "wasm_engine.h"

/* Per-slot in-use flag — separate from pooled_store to avoid false sharing */
struct pool_slot_state {
	volatile int		 in_use;	/* 0=free, 1=acquired */
	char			 _pad[60];	/* Cache line padding */
};

/* Internal pool structure with slot states */
struct store_pool_internal {
	struct vwasm_store_pool	 pub;		/* Public pool struct */
	struct pool_slot_state	*slot_states;	/* Per-slot atomic flags */
	volatile uint64_t	 scan_hint;	/* Rotating scan start */
};

/* ----------------------------------------------------------------
 * Warm Instance Implementation
 * ---------------------------------------------------------------- */

int
vwasm_warm_instance_create(struct vwasm_warm_instance *warm,
    struct vwasm_engine *engine,
    struct wasm_module_entry *entry,
    const char *vm_config,
    const char *plugin_config)
{
	wasmtime_store_t *store = NULL;
	wasmtime_context_t *context;
	wasmtime_instance_t instance;
	wasmtime_error_t *error = NULL;
	wasm_trap_t *trap = NULL;
	wasmtime_extern_t item;
	wasmtime_val_t args[2];
	size_t vm_config_len, plugin_config_len;
	uint8_t *mem_data;
	size_t mem_size;

	memset(warm, 0, sizeof(*warm));
	warm->valid = 0;

	if (engine == NULL || entry == NULL || entry->instance_pre == NULL)
		return (-1);

	vm_config_len = (vm_config != NULL) ? strlen(vm_config) : 0;
	plugin_config_len = (plugin_config != NULL) ? strlen(plugin_config) : 0;

	/* Create a temporary store for warm-up */
	store = wasmtime_store_new(vwasm_engine_get_wasm_engine(engine),
	    NULL, NULL);
	if (store == NULL)
		return (-1);

	context = wasmtime_store_context(store);
	wasmtime_context_set_epoch_deadline(context,
	    vwasm_engine_get_epoch_deadline_ms(engine));
	wasmtime_store_limiter(store,
	    (int64_t)vwasm_engine_get_mem_limit(engine), -1, -1, -1, -1);

	/* Instantiate the module */
	error = wasmtime_instance_pre_instantiate(entry->instance_pre,
	    context, &instance, &trap);
	if (error != NULL || trap != NULL)
		goto fail;

	/* Probe exports and set presence flags */
	warm->has_initialize = wasmtime_instance_export_get(context,
	    &instance, "_initialize", 11, &item) &&
	    item.kind == WASMTIME_EXTERN_FUNC;

	warm->has_on_context_create = wasmtime_instance_export_get(context,
	    &instance, "proxy_on_context_create", 23, &item) &&
	    item.kind == WASMTIME_EXTERN_FUNC;

	warm->has_on_vm_start = wasmtime_instance_export_get(context,
	    &instance, "proxy_on_vm_start", 17, &item) &&
	    item.kind == WASMTIME_EXTERN_FUNC;

	warm->has_on_configure = wasmtime_instance_export_get(context,
	    &instance, "proxy_on_configure", 18, &item) &&
	    item.kind == WASMTIME_EXTERN_FUNC;

	warm->has_request_headers = wasmtime_instance_export_get(context,
	    &instance, "proxy_on_request_headers", 24, &item) &&
	    item.kind == WASMTIME_EXTERN_FUNC;

	warm->has_request_body = wasmtime_instance_export_get(context,
	    &instance, "proxy_on_request_body", 21, &item) &&
	    item.kind == WASMTIME_EXTERN_FUNC;

	warm->has_response_headers = wasmtime_instance_export_get(context,
	    &instance, "proxy_on_response_headers", 25, &item) &&
	    item.kind == WASMTIME_EXTERN_FUNC;

	warm->has_response_body = wasmtime_instance_export_get(context,
	    &instance, "proxy_on_response_body", 22, &item) &&
	    item.kind == WASMTIME_EXTERN_FUNC;

	warm->has_on_log = wasmtime_instance_export_get(context,
	    &instance, "proxy_on_log", 12, &item) &&
	    item.kind == WASMTIME_EXTERN_FUNC;

	warm->has_on_done = wasmtime_instance_export_get(context,
	    &instance, "proxy_on_done", 13, &item) &&
	    item.kind == WASMTIME_EXTERN_FUNC;

	warm->has_on_delete = wasmtime_instance_export_get(context,
	    &instance, "proxy_on_delete", 15, &item) &&
	    item.kind == WASMTIME_EXTERN_FUNC;

	warm->has_allocator = (
	    wasmtime_instance_export_get(context, &instance,
		"proxy_on_memory_allocate", 24, &item) &&
	    item.kind == WASMTIME_EXTERN_FUNC) ||
	    (wasmtime_instance_export_get(context, &instance,
		"malloc", 6, &item) &&
	    item.kind == WASMTIME_EXTERN_FUNC);

	/* Run initialization lifecycle: _initialize → root context → vm_start → configure */
	if (warm->has_initialize) {
		wasmtime_extern_t init_item;
		if (wasmtime_instance_export_get(context, &instance,
		    "_initialize", 11, &init_item) &&
		    init_item.kind == WASMTIME_EXTERN_FUNC) {
			error = wasmtime_func_call(context, &init_item.of.func,
			    NULL, 0, NULL, 0, &trap);
			if (error != NULL || trap != NULL)
				goto fail;
		}
	}

	if (warm->has_on_context_create) {
		wasmtime_extern_t ctx_item;
		if (wasmtime_instance_export_get(context, &instance,
		    "proxy_on_context_create", 23, &ctx_item) &&
		    ctx_item.kind == WASMTIME_EXTERN_FUNC) {
			args[0].kind = WASMTIME_I32;
			args[0].of.i32 = 1;  /* root context id */
			args[1].kind = WASMTIME_I32;
			args[1].of.i32 = 0;  /* parent = 0 (root) */
			error = wasmtime_func_call(context,
			    &ctx_item.of.func, args, 2, NULL, 0, &trap);
			if (error != NULL || trap != NULL)
				goto fail;
		}
	}

	if (warm->has_on_vm_start) {
		wasmtime_extern_t vm_item;
		wasmtime_val_t results[1];
		if (wasmtime_instance_export_get(context, &instance,
		    "proxy_on_vm_start", 17, &vm_item) &&
		    vm_item.kind == WASMTIME_EXTERN_FUNC) {
			args[0].kind = WASMTIME_I32;
			args[0].of.i32 = 1;  /* root context id */
			args[1].kind = WASMTIME_I32;
			args[1].of.i32 = (int32_t)vm_config_len;
			error = wasmtime_func_call(context,
			    &vm_item.of.func, args, 2, results, 1, &trap);
			if (error != NULL || trap != NULL)
				goto fail;
		}
	}

	if (warm->has_on_configure) {
		wasmtime_extern_t cfg_item;
		wasmtime_val_t results[1];
		if (wasmtime_instance_export_get(context, &instance,
		    "proxy_on_configure", 18, &cfg_item) &&
		    cfg_item.kind == WASMTIME_EXTERN_FUNC) {
			args[0].kind = WASMTIME_I32;
			args[0].of.i32 = 1;  /* root context id */
			args[1].kind = WASMTIME_I32;
			args[1].of.i32 = (int32_t)plugin_config_len;
			error = wasmtime_func_call(context,
			    &cfg_item.of.func, args, 2, results, 1, &trap);
			if (error != NULL || trap != NULL)
				goto fail;
		}
	}

	/* Capture linear memory snapshot */
	if (!wasmtime_instance_export_get(context, &instance,
	    "memory", 6, &item) || item.kind != WASMTIME_EXTERN_MEMORY)
		goto fail;

	mem_data = wasmtime_memory_data(context, &item.of.memory);
	mem_size = wasmtime_memory_data_size(context, &item.of.memory);

	if (mem_data == NULL || mem_size == 0)
		goto fail;

	warm->memory_snapshot = malloc(mem_size);
	if (warm->memory_snapshot == NULL)
		goto fail;

	memcpy(warm->memory_snapshot, mem_data, mem_size);
	warm->snapshot_size = mem_size;
	warm->memory_pages = wasmtime_memory_size(context, &item.of.memory);
	warm->valid = 1;

	wasmtime_store_delete(store);
	return (0);

fail:
	if (error != NULL)
		wasmtime_error_delete(error);
	if (trap != NULL)
		wasm_trap_delete(trap);
	if (store != NULL)
		wasmtime_store_delete(store);
	warm->valid = 0;
	return (-1);
}

void
vwasm_warm_instance_destroy(struct vwasm_warm_instance *warm)
{
	if (warm == NULL)
		return;
	if (warm->memory_snapshot != NULL) {
		free(warm->memory_snapshot);
		warm->memory_snapshot = NULL;
	}
	warm->valid = 0;
	warm->snapshot_size = 0;
	warm->memory_pages = 0;
}

int
vwasm_warm_instance_restore(const struct vwasm_warm_instance *warm,
    wasmtime_context_t *context,
    wasmtime_memory_t *memory)
{
	uint8_t *mem_data;
	size_t mem_size;

	if (warm == NULL || !warm->valid || warm->memory_snapshot == NULL)
		return (-1);

	mem_data = wasmtime_memory_data(context, memory);
	mem_size = wasmtime_memory_data_size(context, memory);

	if (mem_data == NULL || mem_size < warm->snapshot_size)
		return (-1);

	memcpy(mem_data, warm->memory_snapshot, warm->snapshot_size);
	return (0);
}

/* ----------------------------------------------------------------
 * Store Pool Implementation
 * ---------------------------------------------------------------- */

/*
 * Initialize a single pooled store slot by instantiating the module.
 */
static int
pool_init_slot(struct vwasm_pooled_store *ps,
    struct vwasm_engine *engine,
    struct wasm_module_entry *entry,
    uint32_t slot_idx)
{
	wasmtime_error_t *error;
	wasm_trap_t *trap = NULL;
	wasmtime_extern_t item;

	ps->store = wasmtime_store_new(
	    vwasm_engine_get_wasm_engine(engine), NULL, NULL);
	if (ps->store == NULL)
		return (-1);

	ps->context = wasmtime_store_context(ps->store);
	wasmtime_context_set_epoch_deadline(ps->context,
	    vwasm_engine_get_epoch_deadline_ms(engine));
	wasmtime_store_limiter(ps->store,
	    (int64_t)vwasm_engine_get_mem_limit(engine), -1, -1, -1, -1);

	error = wasmtime_instance_pre_instantiate(entry->instance_pre,
	    ps->context, &ps->instance, &trap);
	if (error != NULL) {
		wasmtime_error_delete(error);
		wasmtime_store_delete(ps->store);
		ps->store = NULL;
		return (-1);
	}
	if (trap != NULL) {
		wasm_trap_delete(trap);
		wasmtime_store_delete(ps->store);
		ps->store = NULL;
		return (-1);
	}

	/* Resolve memory export */
	ps->memory_valid = 0;
	if (wasmtime_instance_export_get(ps->context, &ps->instance,
	    "memory", 6, &item) && item.kind == WASMTIME_EXTERN_MEMORY) {
		ps->memory = item.of.memory;
		ps->memory_valid = 1;
	}

	/* Resolve allocator */
	ps->allocator_valid = 0;
	if (wasmtime_instance_export_get(ps->context, &ps->instance,
	    "proxy_on_memory_allocate", 24, &item) &&
	    item.kind == WASMTIME_EXTERN_FUNC) {
		ps->allocator = item.of.func;
		ps->allocator_valid = 1;
	} else if (wasmtime_instance_export_get(ps->context, &ps->instance,
	    "malloc", 6, &item) && item.kind == WASMTIME_EXTERN_FUNC) {
		ps->allocator = item.of.func;
		ps->allocator_valid = 1;
	}

	ps->module_entry = entry;
	ps->seq = 0;
	ps->slot_idx = slot_idx;

	return (0);
}

struct vwasm_store_pool *
vwasm_store_pool_new(size_t pool_size,
    const struct vwasm_warm_instance *warm,
    struct vwasm_engine *engine,
    struct wasm_module_entry *entry)
{
	struct store_pool_internal *pi;
	size_t i;

	if (engine == NULL || entry == NULL)
		return (NULL);

	/* Clamp pool size */
	if (pool_size < VWASM_POOL_MIN_SIZE)
		pool_size = VWASM_POOL_MIN_SIZE;
	if (pool_size > VWASM_POOL_MAX_SIZE)
		pool_size = VWASM_POOL_MAX_SIZE;

	pi = calloc(1, sizeof(*pi));
	if (pi == NULL)
		return (NULL);

	pi->slot_states = calloc(pool_size, sizeof(struct pool_slot_state));
	if (pi->slot_states == NULL) {
		free(pi);
		return (NULL);
	}

	pi->pub.slots = calloc(pool_size, sizeof(struct vwasm_pooled_store *));
	if (pi->pub.slots == NULL) {
		free(pi->slot_states);
		free(pi);
		return (NULL);
	}

	pi->pub.store_objects = calloc(pool_size,
	    sizeof(struct vwasm_pooled_store));
	if (pi->pub.store_objects == NULL) {
		free(pi->pub.slots);
		free(pi->slot_states);
		free(pi);
		return (NULL);
	}

	pi->pub.capacity = pool_size;
	pi->pub.warm_ref = warm;
	pi->pub.engine = engine;
	pi->scan_hint = 0;

	/* Initialize all slots */
	for (i = 0; i < pool_size; i++) {
		struct vwasm_pooled_store *ps = &pi->pub.store_objects[i];

		if (pool_init_slot(ps, engine, entry, (uint32_t)i) != 0) {
			/* Partial init: mark remaining slots as invalid */
			ps->store = NULL;
			continue;
		}

		/* Restore warm snapshot into each slot */
		if (warm != NULL && warm->valid && ps->memory_valid) {
			vwasm_warm_instance_restore(warm, ps->context,
			    &ps->memory);
		}

		pi->pub.slots[i] = ps;
		pi->slot_states[i].in_use = 0;  /* Mark as free */
	}

	return (&pi->pub);
}

void
vwasm_store_pool_destroy(struct vwasm_store_pool **poolp)
{
	struct store_pool_internal *pi;
	struct vwasm_store_pool *pool;
	size_t i;

	if (poolp == NULL || *poolp == NULL)
		return;

	pool = *poolp;
	*poolp = NULL;

	pi = (struct store_pool_internal *)pool;

	/* Destroy all stores */
	for (i = 0; i < pool->capacity; i++) {
		struct vwasm_pooled_store *ps = &pool->store_objects[i];
		if (ps->store != NULL) {
			wasmtime_store_delete(ps->store);
			ps->store = NULL;
		}
	}

	free(pool->store_objects);
	free(pool->slots);
	free(pi->slot_states);
	free(pi);
}

struct vwasm_pooled_store *
vwasm_store_pool_acquire(struct vwasm_store_pool *pool)
{
	struct store_pool_internal *pi;
	uint64_t start;
	size_t i, idx;

	if (pool == NULL)
		return (NULL);

	pi = (struct store_pool_internal *)pool;

	/* Rotate scan start to distribute across slots */
	start = __sync_fetch_and_add(&pi->scan_hint, 1);

	for (i = 0; i < pool->capacity; i++) {
		idx = (start + i) % pool->capacity;

		/* Try to claim this slot via CAS */
		if (__sync_val_compare_and_swap(
		    &pi->slot_states[idx].in_use, 0, 1) == 0) {
			struct vwasm_pooled_store *ps = pool->slots[idx];

			if (ps == NULL || ps->store == NULL) {
				/* Slot was never initialized; release it */
				__sync_lock_release(
				    &pi->slot_states[idx].in_use);
				continue;
			}

			/* Restore memory snapshot */
			if (pool->warm_ref != NULL &&
			    pool->warm_ref->valid && ps->memory_valid) {
				vwasm_warm_instance_restore(pool->warm_ref,
				    ps->context, &ps->memory);
				__sync_fetch_and_add(&pool->resets, 1);
			}

			/* Reset epoch deadline */
			wasmtime_context_set_epoch_deadline(ps->context,
			    vwasm_engine_get_epoch_deadline_ms(pool->engine));

			ps->seq++;
			__sync_fetch_and_add(&pool->acquires, 1);
			return (ps);
		}
	}

	/* Pool exhausted — caller must fall back to fresh allocation */
	__sync_fetch_and_add(&pool->fallbacks, 1);
	return (NULL);
}

void
vwasm_store_pool_release(struct vwasm_store_pool *pool,
    struct vwasm_pooled_store *ps)
{
	struct store_pool_internal *pi;

	if (pool == NULL || ps == NULL)
		return;

	pi = (struct store_pool_internal *)pool;

	/* Clear the store data pointer (was pointing to per-request proxy_ctx) */
	wasmtime_context_set_data(ps->context, NULL);

	/* Mark slot as free (atomic release) */
	__sync_lock_release(&pi->slot_states[ps->slot_idx].in_use);
	__sync_fetch_and_add(&pool->releases, 1);
}

char *
vwasm_store_pool_stats_json(const struct vwasm_store_pool *pool)
{
	char *buf;
	int len;

	if (pool == NULL)
		return (NULL);

	buf = malloc(512);
	if (buf == NULL)
		return (NULL);

	len = snprintf(buf, 512,
	    "{\"capacity\":%zu,\"acquires\":%llu,\"releases\":%llu,"
	    "\"fallbacks\":%llu,\"resets\":%llu}",
	    pool->capacity,
	    (unsigned long long)pool->acquires,
	    (unsigned long long)pool->releases,
	    (unsigned long long)pool->fallbacks,
	    (unsigned long long)pool->resets);

	if (len < 0 || len >= 512) {
		free(buf);
		return (NULL);
	}

	return (buf);
}
