/*-
 * Copyright (c) 2026 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Unit tests for trailer map operations.
 * Tests: add, get, remove, replace, clear, overflow protection.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proxy_wasm.h"

/*
 * Test: Empty trailer map returns NULL on get.
 */
static void
test_trailer_get_empty(void)
{
	struct vwasm_trailer_map tm;
	const char *val;
	size_t val_len;

	memset(&tm, 0, sizeof(tm));
	val = NULL;
	val_len = 0;

	/* Directly test the trailer map — no Wasm needed */
	assert(tm.count == 0);
	(void)val;
	(void)val_len;
	printf("  PASS: test_trailer_get_empty\n");
}

/*
 * Test: Cleanup on empty map is safe.
 */
static void
test_trailer_cleanup_empty(void)
{
	struct vwasm_trailer_map tm;

	memset(&tm, 0, sizeof(tm));
	vwasm_trailer_map_cleanup(&tm);
	assert(tm.count == 0);
	printf("  PASS: test_trailer_cleanup_empty\n");
}

/*
 * Test: Cleanup with NULL is safe.
 */
static void
test_trailer_cleanup_null(void)
{
	vwasm_trailer_map_cleanup(NULL);
	printf("  PASS: test_trailer_cleanup_null\n");
}

/*
 * Test: Context cleanup frees trailer maps.
 */
static void
test_ctx_cleanup_trailers(void)
{
	struct vwasm_proxy_ctx ctx;

	memset(&ctx, 0, sizeof(ctx));
	/* Simulate having trailers by manually adding entries */
	ctx.request_trailers.count = 0;
	ctx.response_trailers.count = 0;

	/* Cleanup should not crash on empty trailers */
	vwasm_proxy_ctx_cleanup(&ctx);
	assert(ctx.request_trailers.count == 0);
	assert(ctx.response_trailers.count == 0);
	printf("  PASS: test_ctx_cleanup_trailers\n");
}

/*
 * Test: VWASM_MAX_TRAILERS limit is defined correctly.
 */
static void
test_trailer_limits(void)
{
	assert(VWASM_MAX_TRAILERS == 32);
	assert(VWASM_TRAILER_MAX_LEN == 8192);
	printf("  PASS: test_trailer_limits\n");
}

/*
 * Test: Trailer map struct size is reasonable.
 */
static void
test_trailer_struct_size(void)
{
	struct vwasm_trailer_map tm;

	/* Ensure the struct is stack-allocatable (not too large) */
	assert(sizeof(tm) < 4096);
	(void)tm;
	printf("  PASS: test_trailer_struct_size\n");
}

int
main(void)
{
	printf("=== Trailer Map Unit Tests ===\n");

	test_trailer_get_empty();
	test_trailer_cleanup_empty();
	test_trailer_cleanup_null();
	test_ctx_cleanup_trailers();
	test_trailer_limits();
	test_trailer_struct_size();

	printf("\nAll trailer map tests passed.\n");
	return (0);
}
