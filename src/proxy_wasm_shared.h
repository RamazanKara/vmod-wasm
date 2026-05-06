/*-
 * Copyright (c) 2025 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Proxy-Wasm shared data and queue implementation.
 *
 * Provides thread-safe key-value store (shared data) and FIFO queues
 * for cross-context communication as defined by the Proxy-Wasm ABI.
 */

#ifndef VWASM_PROXY_WASM_SHARED_H
#define VWASM_PROXY_WASM_SHARED_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

/* ----------------------------------------------------------------
 * Shared Data (key-value store with CAS)
 *
 * Thread-safe key-value store shared across all Wasm instances.
 * Supports compare-and-swap via a per-key CAS counter.
 * ---------------------------------------------------------------- */

#define VWASM_SHARED_DATA_BUCKETS	64
#define VWASM_SHARED_DATA_MAX_KEY	256
#define VWASM_SHARED_DATA_MAX_VALUE	65536

struct vwasm_shared_entry {
	char			*key;
	uint8_t			*value;
	size_t			 value_len;
	uint32_t		 cas;
	struct vwasm_shared_entry *next;
};

struct vwasm_shared_data {
	struct vwasm_shared_entry *buckets[VWASM_SHARED_DATA_BUCKETS];
	pthread_rwlock_t	 rwlock;
};

/* Initialize / destroy shared data store */
struct vwasm_shared_data *vwasm_shared_data_new(void);
void vwasm_shared_data_destroy(struct vwasm_shared_data **sdp);

/* Get a value. Returns 0 on success, -1 on not found.
 * Caller must free *value_out after use. */
int vwasm_shared_data_get(struct vwasm_shared_data *sd,
    const char *key, size_t key_len,
    uint8_t **value_out, size_t *value_len_out,
    uint32_t *cas_out);

/* Set a value. If cas != 0, performs CAS check.
 * Returns 0 on success, -1 on CAS mismatch, -2 on error. */
int vwasm_shared_data_set(struct vwasm_shared_data *sd,
    const char *key, size_t key_len,
    const uint8_t *value, size_t value_len,
    uint32_t cas);

/* ----------------------------------------------------------------
 * Shared Queues (FIFO with registered names)
 *
 * Thread-safe named queues for inter-context messaging.
 * ---------------------------------------------------------------- */

#define VWASM_QUEUE_MAX_NAME	256
#define VWASM_QUEUE_MAX_MSG	65536
#define VWASM_QUEUE_MAX_DEPTH	1024
#define VWASM_QUEUE_MAX_COUNT	64

struct vwasm_queue_msg {
	uint8_t			*data;
	size_t			 len;
	struct vwasm_queue_msg	*next;
};

struct vwasm_queue {
	char			*vm_id;
	char			*name;
	uint32_t		 id;
	struct vwasm_queue_msg	*head;
	struct vwasm_queue_msg	*tail;
	uint32_t		 depth;
	struct vwasm_queue	*next;
};

struct vwasm_queue_store {
	struct vwasm_queue	*queues;
	uint32_t		 next_id;
	pthread_mutex_t		 mtx;
};

/* Initialize / destroy queue store */
struct vwasm_queue_store *vwasm_queue_store_new(void);
void vwasm_queue_store_destroy(struct vwasm_queue_store **qsp);

/* Register a queue (creates if not exists). Returns queue_id, or 0 on error. */
uint32_t vwasm_queue_register(struct vwasm_queue_store *qs,
    const char *name, size_t name_len);

/* Resolve a queue by vm_id + name. Returns queue_id, or 0 if not found. */
uint32_t vwasm_queue_resolve(struct vwasm_queue_store *qs,
    const char *vm_id, size_t vm_id_len,
    const char *name, size_t name_len);

/* Enqueue a message. Returns 0 on success, -1 on full/error. */
int vwasm_queue_enqueue(struct vwasm_queue_store *qs,
    uint32_t queue_id, const uint8_t *data, size_t len);

/* Dequeue a message. Returns 0 on success, -1 on empty.
 * Caller must free *data_out. */
int vwasm_queue_dequeue(struct vwasm_queue_store *qs,
    uint32_t queue_id, uint8_t **data_out, size_t *len_out);

#endif /* VWASM_PROXY_WASM_SHARED_H */
