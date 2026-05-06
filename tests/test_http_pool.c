/*-
 * Copyright (c) 2026 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Unit tests for HTTP connection pool (Phase 3).
 * Tests: creation, acquire/release, circuit breaker, DNS cache, stats.
 *
 * NOTE: These tests exercise pool logic without real network I/O.
 * Acquire will fail to connect (no server), which tests error handling.
 * Full integration is tested via VTC tests with a real backend.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_pool.h"

/*
 * Test: Pool creation with valid parameters.
 */
static void
test_pool_new(void)
{
	struct vwasm_http_pool *pool;

	pool = vwasm_http_pool_new(16, 1000);
	assert(pool != NULL);
	assert(pool->max_conns == 16);
	assert(pool->default_timeout_ms == 1000);
	assert(pool->num_active == 0);

	vwasm_http_pool_destroy(&pool);
	assert(pool == NULL);
	printf("  PASS: test_pool_new\n");
}

/*
 * Test: Pool destroy with NULL is safe.
 */
static void
test_pool_destroy_null(void)
{
	struct vwasm_http_pool *pool = NULL;

	vwasm_http_pool_destroy(&pool);
	vwasm_http_pool_destroy(NULL);
	printf("  PASS: test_pool_destroy_null\n");
}

/*
 * Test: Acquire from NULL pool returns failure.
 */
static void
test_pool_acquire_null(void)
{
	struct vwasm_http_conn *conn = NULL;
	int rc;

	rc = vwasm_http_pool_acquire(NULL, "localhost", 80, 100, &conn);
	assert(rc == -1);
	assert(conn == NULL);
	printf("  PASS: test_pool_acquire_null\n");
}

/*
 * Test: Acquire to non-listening port fails gracefully.
 */
static void
test_pool_acquire_no_server(void)
{
	struct vwasm_http_pool *pool;
	struct vwasm_http_conn *conn = NULL;
	int rc;

	pool = vwasm_http_pool_new(4, 100);
	assert(pool != NULL);

	/* Port 1 is unlikely to be listening; connect should fail fast */
	rc = vwasm_http_pool_acquire(pool, "127.0.0.1", 1, 50, &conn);
	assert(rc == -1);
	assert(conn == NULL);

	/* Stats should show the acquire attempt */
	assert(pool->stat_acquires == 1);

	vwasm_http_pool_destroy(&pool);
	printf("  PASS: test_pool_acquire_no_server\n");
}

/*
 * Test: Release with NULL params is safe.
 */
static void
test_pool_release_null(void)
{
	vwasm_http_pool_release(NULL, NULL, 0);
	printf("  PASS: test_pool_release_null\n");
}

/*
 * Test: Close with NULL params is safe.
 */
static void
test_pool_close_null(void)
{
	vwasm_http_pool_close(NULL, NULL);
	printf("  PASS: test_pool_close_null\n");
}

/*
 * Test: Circuit breaker starts in CLOSED state.
 */
static void
test_cb_initial_state(void)
{
	struct vwasm_http_pool *pool;
	int allowed;

	pool = vwasm_http_pool_new(4, 1000);
	assert(pool != NULL);

	/* First call to cb_allow should succeed (circuit closed) */
	allowed = vwasm_http_pool_cb_allow(pool, "backend.example.com", 443);
	assert(allowed == 0);

	vwasm_http_pool_destroy(&pool);
	printf("  PASS: test_cb_initial_state\n");
}

/*
 * Test: Circuit opens after threshold failures.
 */
static void
test_cb_opens_on_failures(void)
{
	struct vwasm_http_pool *pool;
	int i, allowed;

	pool = vwasm_http_pool_new(4, 1000);
	assert(pool != NULL);

	/* Record failures up to threshold */
	for (i = 0; i < VWASM_HTTP_CB_THRESHOLD_DEFAULT; i++)
		vwasm_http_pool_cb_failure(pool, "failing.host", 80);

	/* Circuit should now be open — requests rejected */
	allowed = vwasm_http_pool_cb_allow(pool, "failing.host", 80);
	assert(allowed == -1);

	vwasm_http_pool_destroy(&pool);
	printf("  PASS: test_cb_opens_on_failures\n");
}

/*
 * Test: Success resets consecutive failure count.
 */
static void
test_cb_success_resets(void)
{
	struct vwasm_http_pool *pool;
	int allowed;

	pool = vwasm_http_pool_new(4, 1000);
	assert(pool != NULL);

	/* Record some failures (below threshold) */
	vwasm_http_pool_cb_failure(pool, "intermittent.host", 80);
	vwasm_http_pool_cb_failure(pool, "intermittent.host", 80);

	/* A success resets the counter */
	vwasm_http_pool_cb_success(pool, "intermittent.host", 80);

	/* More failures below original threshold should not open */
	vwasm_http_pool_cb_failure(pool, "intermittent.host", 80);
	vwasm_http_pool_cb_failure(pool, "intermittent.host", 80);

	allowed = vwasm_http_pool_cb_allow(pool, "intermittent.host", 80);
	assert(allowed == 0);

	vwasm_http_pool_destroy(&pool);
	printf("  PASS: test_cb_success_resets\n");
}

/*
 * Test: Circuit breaker for NULL pool is safe.
 */
static void
test_cb_null_pool(void)
{
	int allowed;

	allowed = vwasm_http_pool_cb_allow(NULL, "host", 80);
	assert(allowed == -1);
	vwasm_http_pool_cb_success(NULL, "host", 80);
	vwasm_http_pool_cb_failure(NULL, "host", 80);
	printf("  PASS: test_cb_null_pool\n");
}

/*
 * Test: DNS resolve with NULL pool returns failure.
 */
static void
test_dns_resolve_null(void)
{
	struct sockaddr_storage addr;
	socklen_t addrlen;
	int rc;

	rc = vwasm_http_pool_resolve(NULL, "example.com", 80, &addr,
	    &addrlen);
	assert(rc == -1);
	printf("  PASS: test_dns_resolve_null\n");
}

/*
 * Test: Stats JSON contains expected fields.
 */
static void
test_pool_stats_json(void)
{
	struct vwasm_http_pool *pool;
	char *json;

	pool = vwasm_http_pool_new(8, 1000);
	assert(pool != NULL);

	json = vwasm_http_pool_stats_json(pool);
	assert(json != NULL);
	assert(strstr(json, "max_conns") != NULL);
	assert(strstr(json, "acquires") != NULL);
	assert(strstr(json, "releases") != NULL);
	assert(strstr(json, "creates") != NULL);
	assert(strstr(json, "dns_hits") != NULL);
	assert(strstr(json, "cb_rejections") != NULL);
	free(json);

	vwasm_http_pool_destroy(&pool);
	printf("  PASS: test_pool_stats_json\n");
}

/*
 * Test: Stats JSON from NULL pool returns NULL.
 */
static void
test_pool_stats_json_null(void)
{
	char *json;

	json = vwasm_http_pool_stats_json(NULL);
	assert(json == NULL);
	printf("  PASS: test_pool_stats_json_null\n");
}

/*
 * Test: Multiple circuit breakers are independent.
 */
static void
test_cb_independent_upstreams(void)
{
	struct vwasm_http_pool *pool;
	int i, allowed;

	pool = vwasm_http_pool_new(4, 1000);
	assert(pool != NULL);

	/* Fail one upstream */
	for (i = 0; i < VWASM_HTTP_CB_THRESHOLD_DEFAULT; i++)
		vwasm_http_pool_cb_failure(pool, "bad.host", 80);

	/* bad.host should be open */
	allowed = vwasm_http_pool_cb_allow(pool, "bad.host", 80);
	assert(allowed == -1);

	/* good.host should still be closed */
	allowed = vwasm_http_pool_cb_allow(pool, "good.host", 80);
	assert(allowed == 0);

	vwasm_http_pool_destroy(&pool);
	printf("  PASS: test_cb_independent_upstreams\n");
}

int
main(void)
{
	printf("=== HTTP Pool Unit Tests ===\n");

	test_pool_new();
	test_pool_destroy_null();
	test_pool_acquire_null();
	test_pool_acquire_no_server();
	test_pool_release_null();
	test_pool_close_null();
	test_cb_initial_state();
	test_cb_opens_on_failures();
	test_cb_success_resets();
	test_cb_null_pool();
	test_dns_resolve_null();
	test_pool_stats_json();
	test_pool_stats_json_null();
	test_cb_independent_upstreams();

	printf("=== All HTTP pool tests passed ===\n");
	return (0);
}
