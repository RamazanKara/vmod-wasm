/*-
 * Copyright (c) 2025 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef VWASM_ENGINE_H
#define VWASM_ENGINE_H

#include <stdint.h>
#include <wasmtime.h>

#include "store_pool.h"
#include "http_pool.h"

struct vrt_ctx;
struct vwasm_engine;
struct wasm_module_entry {
	char			*name;
	wasmtime_module_t	*module;
	wasmtime_instance_pre_t	*instance_pre;
};

/* Default execution limits */
#define VWASM_DEFAULT_MEMLIMIT	(16 * 1024 * 1024)	/* 16 MiB */
#define VWASM_DEFAULT_STACKSIZE	(512 * 1024)		/* 512 KiB */

/* Epoch-based interruption for execution time limiting */
#define VWASM_DEFAULT_EPOCH_DEADLINE_MS	100	/* 100ms per callback */
#define VWASM_EPOCH_TICK_MS		1	/* Timer thread ticks every 1ms */

/* Default HTTP timeout for proxy_http_call */
#define VWASM_DEFAULT_HTTP_TIMEOUT_MS	5000	/* 5 seconds */

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
void vwasm_engine_set_memory_limit(struct vwasm_engine *engine, size_t bytes);

/* Query current limits */
size_t vwasm_engine_get_memory_limit(struct vwasm_engine *engine);

/*
 * Epoch-based interruption.
 * A background timer thread increments the engine epoch every 1ms.
 * Callbacks get epoch_deadline_ms milliseconds before trapping.
 */
void vwasm_engine_set_epoch_deadline(struct vwasm_engine *engine, uint64_t ms);
uint64_t vwasm_engine_get_epoch_deadline(struct vwasm_engine *engine);

/*
 * Reset the epoch deadline on a store context (extend time budget).
 * Called before each wasm callback phase.
 */
void vwasm_engine_reset_epoch_deadline(struct vwasm_engine *engine,
    wasmtime_context_t *context);

/*
 * Configurable HTTP timeout for proxy_http_call (milliseconds).
 */
void vwasm_engine_set_http_timeout(struct vwasm_engine *engine, uint32_t ms);
uint32_t vwasm_engine_get_http_timeout(struct vwasm_engine *engine);

/*
 * Execute a Proxy-Wasm module lifecycle for HTTP request filtering.
 *
 * Full ABI v0.2.1 lifecycle:
 *   1. _initialize                      — Wasm module init
 *   2. proxy_on_context_create(1, 0)    — root context
 *   3. proxy_on_vm_start(1, vm_config_len) — VM startup
 *   4. proxy_on_configure(1, plugin_config_len) — plugin config
 *   5. proxy_on_context_create(2, 1)    — stream context
 *   6. proxy_on_request_headers(2, n, 1) — HTTP request filtering
 *   7. proxy_on_request_body(2, body_len, 1) — body processing (request)
 *      proxy_on_response_body(2, body_len, 1) — body processing (response)
 *   8. proxy_on_log(2)                  — logging phase
 *   9. proxy_on_done(2)                 — stream done
 *  10. proxy_on_delete(2)               — stream cleanup
 *
 * Returns: action code (0=CONTINUE, 1=PAUSE), or -1 on error.
 * If the module called send_local_response, *status_code is set.
 */
int vwasm_proxy_wasm_call(struct vwasm_engine *engine,
    const struct vrt_ctx *ctx,
    const char *module_name,
    int *status_code);

/*
 * Execute a Proxy-Wasm module lifecycle for HTTP response filtering.
 *
 * Full ABI v0.2.1 lifecycle (same as above but calls
 * proxy_on_response_headers/body instead of request).
 *
 * Returns: action code (0=CONTINUE, 1=PAUSE), or -1 on error.
 * If the module called send_local_response, *status_code is set.
 */
int vwasm_proxy_wasm_response_call(struct vwasm_engine *engine,
    const struct vrt_ctx *ctx,
    const char *module_name,
    int *status_code);

/*
 * Extended proxy-wasm call with VM and plugin configuration.
 * vm_config/plugin_config can be NULL for no configuration.
 */
int vwasm_proxy_wasm_call_with_config(struct vwasm_engine *engine,
    const struct vrt_ctx *ctx,
    const char *module_name,
    const char *vm_config,
    const char *plugin_config,
    int *status_code);

int vwasm_proxy_wasm_response_call_with_config(struct vwasm_engine *engine,
    const struct vrt_ctx *ctx,
    const char *module_name,
    const char *vm_config,
    const char *plugin_config,
    int *status_code);

/* ----------------------------------------------------------------
 * Security configuration
 * ---------------------------------------------------------------- */

/* Fail mode for Wasm execution errors */
#define VWASM_FAIL_CLOSED	0	/* Return error on Wasm failure (default) */
#define VWASM_FAIL_OPEN		1	/* Continue on Wasm failure */

/* Maximum HTTP callouts per request (default) */
#define VWASM_DEFAULT_HTTP_CALL_MAX	5

/*
 * Set allowed upstream hosts for proxy_http_call.
 * The list is a comma-separated string of "host:port" entries.
 * If NULL or empty, all upstreams are allowed (unsafe for production).
 */
void vwasm_engine_set_allowed_upstreams(struct vwasm_engine *engine,
    const char *upstream_list);

/*
 * Set maximum number of HTTP callouts per request.
 * 0 disables HTTP callouts entirely.
 */
void vwasm_engine_set_http_call_max(struct vwasm_engine *engine,
    uint32_t max_calls);

/*
 * Set fail mode: VWASM_FAIL_CLOSED (default) or VWASM_FAIL_OPEN.
 */
void vwasm_engine_set_fail_mode(struct vwasm_engine *engine, int mode);

/* Query security settings */
int vwasm_engine_get_fail_mode(struct vwasm_engine *engine);
uint32_t vwasm_engine_get_http_call_max(struct vwasm_engine *engine);

/* ----------------------------------------------------------------
 * Execution statistics (atomic counters, safe from any thread)
 * ---------------------------------------------------------------- */

struct vwasm_stats {
	volatile uint64_t	calls_total;
	volatile uint64_t	calls_ok;
	volatile uint64_t	calls_error;
	volatile uint64_t	calls_timeout;	/* execution > 10ms */
	volatile uint64_t	local_responses;
	volatile uint64_t	http_calls;
	volatile uint64_t	http_calls_blocked;  /* SSRF or private IP */
	volatile uint64_t	body_bytes_in;
};

/* Get pointer to the global stats struct (never NULL after engine_new) */
struct vwasm_stats *vwasm_engine_get_stats(struct vwasm_engine *engine);

/* ----------------------------------------------------------------
 * Accessor functions for engine internals (used by store_pool.c)
 * ---------------------------------------------------------------- */

/* Get the raw wasmtime engine handle */
wasm_engine_t *vwasm_engine_get_wasm_engine(struct vwasm_engine *engine);

/* Get the epoch deadline in ms */
uint64_t vwasm_engine_get_epoch_deadline_ms(struct vwasm_engine *engine);

/* Get the memory limit */
size_t vwasm_engine_get_mem_limit(struct vwasm_engine *engine);

/* Get a module entry by name */
struct wasm_module_entry *vwasm_engine_find_module(
    struct vwasm_engine *engine, const char *name);

/* Get the store pool for a given index */
struct vwasm_store_pool *vwasm_engine_get_pool(
    struct vwasm_engine *engine, int idx);

/* ----------------------------------------------------------------
 * Phase 1+2: Store Pool Management
 * ---------------------------------------------------------------- */

/* Default store pool size */
#define VWASM_DEFAULT_STORE_POOL_SIZE	8

/*
 * Initialize warm instance and store pool for a module.
 * Must be called after load_module, typically from vcl_init.
 * pool_size: 0 for default.
 */
int vwasm_engine_init_pool(struct vwasm_engine *engine,
    const char *module_name, size_t pool_size,
    const char *vm_config, const char *plugin_config);

/*
 * Destroy all pools (called from engine_destroy).
 */
void vwasm_engine_destroy_pools(struct vwasm_engine *engine);

/*
 * Get store pool stats JSON for a specific module.
 * Caller must free returned string.
 */
char *vwasm_engine_get_pool_stats_json(struct vwasm_engine *engine,
    const char *module_name);

/* ----------------------------------------------------------------
 * Phase 3: HTTP Connection Pool
 * ---------------------------------------------------------------- */

/* Default HTTP pool size */
#define VWASM_DEFAULT_HTTP_POOL_SIZE	16

/*
 * Initialize the shared HTTP connection pool.
 * pool_size: 0 for default.
 */
int vwasm_engine_init_http_pool(struct vwasm_engine *engine,
    size_t pool_size);

/*
 * Get the HTTP pool (for use by proxy_wasm_http.c).
 */
struct vwasm_http_pool *vwasm_engine_get_http_pool(
    struct vwasm_engine *engine);

/*
 * Get HTTP pool stats JSON. Caller must free returned string.
 */
char *vwasm_engine_get_http_pool_stats_json(struct vwasm_engine *engine);

/* ----------------------------------------------------------------
 * Phase 4: Filter Chain
 * ---------------------------------------------------------------- */

/* Maximum filters in a chain */
#define VWASM_CHAIN_MAX_FILTERS		16

/*
 * Execute a filter chain (multiple modules in sequence) for request phase.
 * modules: array of module names.
 * nmodules: number of modules.
 * Returns the first non-CONTINUE action, or 0 if all pass.
 */
int vwasm_filter_chain_request(struct vwasm_engine *engine,
    const struct vrt_ctx *ctx,
    const char **modules, int nmodules,
    int *status_code);

/*
 * Execute a filter chain for response phase.
 */
int vwasm_filter_chain_response(struct vwasm_engine *engine,
    const struct vrt_ctx *ctx,
    const char **modules, int nmodules,
    int *status_code);

/*
 * Start or update a tick timer for a module.
 * Called by proxy_set_tick_period_milliseconds host function.
 * period_ms=0 stops the timer.
 */
int vwasm_engine_set_tick_period(struct vwasm_engine *engine,
    const char *module_name, uint32_t period_ms);

#endif /* VWASM_ENGINE_H */
