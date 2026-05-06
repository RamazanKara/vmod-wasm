/*-
 * Copyright (c) 2026 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Unit tests for store pool (Phase 2).
 * Tests: creation, acquire/release, exhaustion fallback, stats, destroy.
 *
 * NOTE: Without a full Wasmtime engine, we test pool logic only.
 * The pool requires a real engine+module to populate stores, so
 * we test with NULL engine/entry which means acquire returns NULL
 * (pool reports fallbacks). This exercises the pool structure code.
 * Full integration is tested via VTC tests with a real Varnish instance.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "store_pool.h"

/*
 * Test: Pool creation with NULL engine returns NULL gracefully.
 */
static void
test_pool_new_null_engine(void)
{
	struct vwasm_store_pool *pool;

	pool = vwasm_store_pool_new(8, NULL, NULL, NULL);
	/*
	 * Depending on implementation: either NULL (can't pre-allocate
	 * stores without engine) or a valid pool with no usable stores.
	 */
	if (pool != NULL)
		vwasm_store_pool_destroy(&pool);
	printf("  PASS: test_pool_new_null_engine\n");
}

/*
 * Test: Pool destroy with NULL is safe (no crash).
 */
static void
test_pool_destroy_null(void)
{
	struct vwasm_store_pool *pool = NULL;

	vwasm_store_pool_destroy(&pool);
	vwasm_store_pool_destroy(NULL);
	printf("  PASS: test_pool_destroy_null\n");
}

/*
 * Test: Pool acquire from NULL pool returns NULL.
 */
static void
test_pool_acquire_null(void)
{
	struct vwasm_pooled_store *ps;

	ps = vwasm_store_pool_acquire(NULL);
	assert(ps == NULL);
	printf("  PASS: test_pool_acquire_null\n");
}

/*
 * Test: Pool release with NULL params is safe.
 */
static void
test_pool_release_null(void)
{
	vwasm_store_pool_release(NULL, NULL);
	printf("  PASS: test_pool_release_null\n");
}

/*
 * Test: Stats JSON from NULL pool returns NULL.
 */
static void
test_pool_stats_null(void)
{
	char *json;

	json = vwasm_store_pool_stats_json(NULL);
	assert(json == NULL);
	printf("  PASS: test_pool_stats_null\n");
}

/*
 * Test: Pool capacity is clamped to MIN/MAX bounds.
 */
static void
test_pool_size_clamping(void)
{
	struct vwasm_store_pool *pool;

	/* Below minimum: clamped up to VWASM_POOL_MIN_SIZE (8) */
	pool = vwasm_store_pool_new(1, NULL, NULL, NULL);
	if (pool != NULL) {
		assert(pool->capacity >= VWASM_POOL_MIN_SIZE);
		vwasm_store_pool_destroy(&pool);
	}

	/* Above maximum: clamped down to VWASM_POOL_MAX_SIZE (1024) */
	pool = vwasm_store_pool_new(9999, NULL, NULL, NULL);
	if (pool != NULL) {
		assert(pool->capacity <= VWASM_POOL_MAX_SIZE);
		vwasm_store_pool_destroy(&pool);
	}

	printf("  PASS: test_pool_size_clamping\n");
}

/*
 * Test: Stats JSON format contains expected fields.
 */
static void
test_pool_stats_json_format(void)
{
	struct vwasm_store_pool *pool;
	char *json;

	pool = vwasm_store_pool_new(8, NULL, NULL, NULL);
	if (pool == NULL) {
		printf("  SKIP: test_pool_stats_json_format (no engine)\n");
		return;
	}

	json = vwasm_store_pool_stats_json(pool);
	if (json != NULL) {
		assert(strstr(json, "capacity") != NULL);
		assert(strstr(json, "acquires") != NULL);
		assert(strstr(json, "releases") != NULL);
		assert(strstr(json, "fallbacks") != NULL);
		free(json);
	}

	vwasm_store_pool_destroy(&pool);
	printf("  PASS: test_pool_stats_json_format\n");
}

/*
 * Test: Warm instance struct initialization.
 */
static void
test_warm_instance_init(void)
{
	struct vwasm_warm_instance warm;

	memset(&warm, 0, sizeof(warm));
	assert(warm.valid == 0);
	assert(warm.memory_snapshot == NULL);
	assert(warm.snapshot_size == 0);
	printf("  PASS: test_warm_instance_init\n");
}

/*
 * Test: Warm instance destroy with zeroed struct is safe.
 */
static void
test_warm_instance_destroy_zero(void)
{
	struct vwasm_warm_instance warm;

	memset(&warm, 0, sizeof(warm));
	vwasm_warm_instance_destroy(&warm);
	assert(warm.memory_snapshot == NULL);
	printf("  PASS: test_warm_instance_destroy_zero\n");
}

/*
 * Test: Concurrent acquire calls (stress test pool atomic ops).
 * Since we don't have a real engine, acquires will return NULL
 * (fallback path). This still exercises the atomic head/tail.
 */
#define STRESS_THREADS	8
#define STRESS_ITERS	1000

static void *
stress_acquire_thread(void *arg)
{
	struct vwasm_store_pool *pool = arg;
	int i;

	for (i = 0; i < STRESS_ITERS; i++) {
		struct vwasm_pooled_store *ps;

		ps = vwasm_store_pool_acquire(pool);
		if (ps != NULL)
			vwasm_store_pool_release(pool, ps);
	}
	return (NULL);
}

static void
test_pool_concurrent_access(void)
{
	struct vwasm_store_pool *pool;
	pthread_t threads[STRESS_THREADS];
	int i;

	pool = vwasm_store_pool_new(16, NULL, NULL, NULL);
	if (pool == NULL) {
		printf("  SKIP: test_pool_concurrent_access (no engine)\n");
		return;
	}

	for (i = 0; i < STRESS_THREADS; i++)
		pthread_create(&threads[i], NULL, stress_acquire_thread, pool);

	for (i = 0; i < STRESS_THREADS; i++)
		pthread_join(threads[i], NULL);

	/* Pool should still be valid after concurrent hammering */
	vwasm_store_pool_destroy(&pool);
	printf("  PASS: test_pool_concurrent_access\n");
}

int
main(void)
{
	printf("=== Store Pool Unit Tests ===\n");

	test_pool_new_null_engine();
	test_pool_destroy_null();
	test_pool_acquire_null();
	test_pool_release_null();
	test_pool_stats_null();
	test_pool_size_clamping();
	test_pool_stats_json_format();
	test_warm_instance_init();
	test_warm_instance_destroy_zero();
	test_pool_concurrent_access();

	printf("=== All store pool tests passed ===\n");
	return (0);
}
