/*-
 * Copyright (c) 2026 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 */

#undef NDEBUG
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "proxy_wasm_shared.h"

static void
test_shared_keys(void)
{
	char max_key[VWASM_SHARED_DATA_MAX_KEY + 1];
	const struct {
		const char *key;
		size_t len;
		int result;
	} cases[] = {
		{"a", 1, 0},
		{"a\0b", 3, 0},
		{"a\0c", 3, 0},
		{"\0", 1, 0},
		{max_key, VWASM_SHARED_DATA_MAX_KEY - 1, 0},
		{max_key, VWASM_SHARED_DATA_MAX_KEY, 0},
		{max_key, VWASM_SHARED_DATA_MAX_KEY + 1, -2},
		{"", 0, -2},
	};
	struct vwasm_shared_data *sd;
	uint8_t value, *out;
	size_t i, len;
	uint32_t cas, updated_cas;

	memset(max_key, 'x', sizeof(max_key));
	sd = vwasm_shared_data_new();
	assert(sd != NULL);

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		value = (uint8_t)i;
		assert(vwasm_shared_data_set(sd, cases[i].key, cases[i].len,
		    &value, sizeof(value), 0) == cases[i].result);
	}

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		if (cases[i].result != 0) {
			assert(vwasm_shared_data_get(sd, cases[i].key,
			    cases[i].len, &out, &len, &cas) == -1);
			continue;
		}
		assert(vwasm_shared_data_get(sd, cases[i].key, cases[i].len,
		    &out, &len, &cas) == 0);
		assert(len == 1 && out[0] == (uint8_t)i && cas != 0);
		free(out);

		value = (uint8_t)(i + 10);
		assert(vwasm_shared_data_set(sd, cases[i].key, cases[i].len,
		    &value, sizeof(value), cas) == 0);
		value = 255;
		assert(vwasm_shared_data_set(sd, cases[i].key, cases[i].len,
		    &value, sizeof(value), cas) == -1);
		assert(vwasm_shared_data_get(sd, cases[i].key, cases[i].len,
		    &out, &len, &updated_cas) == 0);
		assert(len == 1 && out[0] == (uint8_t)(i + 10));
		assert(updated_cas != 0 && updated_cas != cas);
		free(out);
	}

	vwasm_shared_data_destroy(&sd);
	assert(sd == NULL);
}

static void
test_queue_names(void)
{
	char max_name[VWASM_QUEUE_MAX_NAME + 1];
	const struct {
		const char *name;
		size_t len;
		int valid;
	} cases[] = {
		{"a", 1, 1},
		{"a\0b", 3, 1},
		{"a\0c", 3, 1},
		{"\0", 1, 1},
		{max_name, VWASM_QUEUE_MAX_NAME - 1, 1},
		{max_name, VWASM_QUEUE_MAX_NAME, 1},
		{max_name, VWASM_QUEUE_MAX_NAME + 1, 0},
		{"", 0, 0},
	};
	struct vwasm_queue_store *qs;
	uint32_t ids[sizeof(cases) / sizeof(cases[0])];
	uint8_t value, *out;
	size_t i, j, len;

	memset(max_name, 'x', sizeof(max_name));
	qs = vwasm_queue_store_new();
	assert(qs != NULL);

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		ids[i] = vwasm_queue_register(qs, cases[i].name, cases[i].len);
		if (!cases[i].valid) {
			assert(ids[i] == 0);
			continue;
		}
		assert(ids[i] != 0);
		for (j = 0; j < i; j++)
			assert(ids[i] != ids[j]);
		value = (uint8_t)i;
		assert(vwasm_queue_enqueue(qs, ids[i], &value, 1) == 0);
	}

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		assert(vwasm_queue_resolve(qs, NULL, 0, cases[i].name,
		    cases[i].len) == ids[i]);
		if (!cases[i].valid)
			continue;
		assert(vwasm_queue_register(qs, cases[i].name, cases[i].len)
		    == ids[i]);
		assert(vwasm_queue_dequeue(qs, ids[i], &out, &len) == 0);
		assert(len == 1 && out[0] == (uint8_t)i);
		free(out);
		assert(vwasm_queue_dequeue(qs, ids[i], &out, &len) == -1);
	}

	vwasm_queue_store_destroy(&qs);
	assert(qs == NULL);
}

int
main(void)
{
	test_shared_keys();
	test_queue_names();
	return (0);
}
