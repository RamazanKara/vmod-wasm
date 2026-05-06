/*-
 * Copyright (c) 2025 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Proxy-Wasm shared data and queue implementation.
 */

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "proxy_wasm_shared.h"

/* ----------------------------------------------------------------
 * FNV-1a hash for bucket selection
 * ---------------------------------------------------------------- */

static uint32_t
fnv1a(const char *key, size_t len)
{
	uint32_t hash = 2166136261u;
	size_t i;

	for (i = 0; i < len; i++) {
		hash ^= (uint8_t)key[i];
		hash *= 16777619u;
	}
	return (hash);
}

/* ================================================================
 * Shared Data Implementation
 * ================================================================ */

struct vwasm_shared_data *
vwasm_shared_data_new(void)
{
	struct vwasm_shared_data *sd;

	sd = calloc(1, sizeof(*sd));
	if (sd == NULL)
		return (NULL);

	if (pthread_rwlock_init(&sd->rwlock, NULL) != 0) {
		free(sd);
		return (NULL);
	}

	return (sd);
}

void
vwasm_shared_data_destroy(struct vwasm_shared_data **sdp)
{
	struct vwasm_shared_data *sd;
	struct vwasm_shared_entry *entry, *next;
	int i;

	if (sdp == NULL || *sdp == NULL)
		return;

	sd = *sdp;
	*sdp = NULL;

	for (i = 0; i < VWASM_SHARED_DATA_BUCKETS; i++) {
		entry = sd->buckets[i];
		while (entry != NULL) {
			next = entry->next;
			free(entry->key);
			free(entry->value);
			free(entry);
			entry = next;
		}
	}

	pthread_rwlock_destroy(&sd->rwlock);
	free(sd);
}

int
vwasm_shared_data_get(struct vwasm_shared_data *sd,
    const char *key, size_t key_len,
    uint8_t **value_out, size_t *value_len_out,
    uint32_t *cas_out)
{
	uint32_t bucket;
	struct vwasm_shared_entry *entry;

	if (sd == NULL || key == NULL || key_len == 0)
		return (-1);

	bucket = fnv1a(key, key_len) % VWASM_SHARED_DATA_BUCKETS;

	pthread_rwlock_rdlock(&sd->rwlock);

	for (entry = sd->buckets[bucket]; entry != NULL; entry = entry->next) {
		if (strlen(entry->key) == key_len &&
		    memcmp(entry->key, key, key_len) == 0) {
			/* Found - copy value out */
			if (entry->value_len > 0 && entry->value != NULL) {
				*value_out = malloc(entry->value_len);
				if (*value_out == NULL) {
					pthread_rwlock_unlock(&sd->rwlock);
					return (-1);
				}
				memcpy(*value_out, entry->value, entry->value_len);
				*value_len_out = entry->value_len;
			} else {
				*value_out = NULL;
				*value_len_out = 0;
			}
			*cas_out = entry->cas;
			pthread_rwlock_unlock(&sd->rwlock);
			return (0);
		}
	}

	pthread_rwlock_unlock(&sd->rwlock);
	return (-1); /* not found */
}

int
vwasm_shared_data_set(struct vwasm_shared_data *sd,
    const char *key, size_t key_len,
    const uint8_t *value, size_t value_len,
    uint32_t cas)
{
	uint32_t bucket;
	struct vwasm_shared_entry *entry;
	uint8_t *new_val = NULL;

	if (sd == NULL || key == NULL || key_len == 0)
		return (-2);
	if (key_len > VWASM_SHARED_DATA_MAX_KEY)
		return (-2);
	if (value_len > VWASM_SHARED_DATA_MAX_VALUE)
		return (-2);

	/* Pre-allocate value copy */
	if (value != NULL && value_len > 0) {
		new_val = malloc(value_len);
		if (new_val == NULL)
			return (-2);
		memcpy(new_val, value, value_len);
	}

	bucket = fnv1a(key, key_len) % VWASM_SHARED_DATA_BUCKETS;

	pthread_rwlock_wrlock(&sd->rwlock);

	/* Look for existing entry */
	for (entry = sd->buckets[bucket]; entry != NULL; entry = entry->next) {
		if (strlen(entry->key) == key_len &&
		    memcmp(entry->key, key, key_len) == 0) {
			/* CAS check */
			if (cas != 0 && entry->cas != cas) {
				pthread_rwlock_unlock(&sd->rwlock);
				free(new_val);
				return (-1); /* CAS mismatch */
			}
			/* Update existing */
			free(entry->value);
			entry->value = new_val;
			entry->value_len = value_len;
			entry->cas++;
			pthread_rwlock_unlock(&sd->rwlock);
			return (0);
		}
	}

	/* New entry */
	entry = calloc(1, sizeof(*entry));
	if (entry == NULL) {
		pthread_rwlock_unlock(&sd->rwlock);
		free(new_val);
		return (-2);
	}

	entry->key = malloc(key_len + 1);
	if (entry->key == NULL) {
		pthread_rwlock_unlock(&sd->rwlock);
		free(new_val);
		free(entry);
		return (-2);
	}
	memcpy(entry->key, key, key_len);
	entry->key[key_len] = '\0';
	entry->value = new_val;
	entry->value_len = value_len;
	entry->cas = 1;
	entry->next = sd->buckets[bucket];
	sd->buckets[bucket] = entry;

	pthread_rwlock_unlock(&sd->rwlock);
	return (0);
}

/* ================================================================
 * Queue Implementation
 * ================================================================ */

struct vwasm_queue_store *
vwasm_queue_store_new(void)
{
	struct vwasm_queue_store *qs;

	qs = calloc(1, sizeof(*qs));
	if (qs == NULL)
		return (NULL);

	qs->next_id = 1; /* IDs start at 1 */
	if (pthread_mutex_init(&qs->mtx, NULL) != 0) {
		free(qs);
		return (NULL);
	}

	return (qs);
}

void
vwasm_queue_store_destroy(struct vwasm_queue_store **qsp)
{
	struct vwasm_queue_store *qs;
	struct vwasm_queue *q, *qnext;
	struct vwasm_queue_msg *msg, *mnext;

	if (qsp == NULL || *qsp == NULL)
		return;

	qs = *qsp;
	*qsp = NULL;

	q = qs->queues;
	while (q != NULL) {
		qnext = q->next;
		/* Drain messages */
		msg = q->head;
		while (msg != NULL) {
			mnext = msg->next;
			free(msg->data);
			free(msg);
			msg = mnext;
		}
		free(q->vm_id);
		free(q->name);
		free(q);
		q = qnext;
	}

	pthread_mutex_destroy(&qs->mtx);
	free(qs);
}

uint32_t
vwasm_queue_register(struct vwasm_queue_store *qs,
    const char *name, size_t name_len)
{
	struct vwasm_queue *q;
	uint32_t id;

	if (qs == NULL || name == NULL || name_len == 0)
		return (0);
	if (name_len > VWASM_QUEUE_MAX_NAME)
		return (0);

	pthread_mutex_lock(&qs->mtx);

	/* Check if already exists */
	for (q = qs->queues; q != NULL; q = q->next) {
		if (q->name != NULL && strlen(q->name) == name_len &&
		    memcmp(q->name, name, name_len) == 0) {
			id = q->id;
			pthread_mutex_unlock(&qs->mtx);
			return (id);
		}
	}

	/* Check max count */
	if (qs->next_id > VWASM_QUEUE_MAX_COUNT) {
		pthread_mutex_unlock(&qs->mtx);
		return (0);
	}

	/* Create new queue */
	q = calloc(1, sizeof(*q));
	if (q == NULL) {
		pthread_mutex_unlock(&qs->mtx);
		return (0);
	}

	q->name = malloc(name_len + 1);
	if (q->name == NULL) {
		free(q);
		pthread_mutex_unlock(&qs->mtx);
		return (0);
	}
	memcpy(q->name, name, name_len);
	q->name[name_len] = '\0';

	q->vm_id = NULL; /* Will be set by caller if needed */
	q->id = qs->next_id++;
	q->next = qs->queues;
	qs->queues = q;

	id = q->id;
	pthread_mutex_unlock(&qs->mtx);
	return (id);
}

uint32_t
vwasm_queue_resolve(struct vwasm_queue_store *qs,
    const char *vm_id, size_t vm_id_len,
    const char *name, size_t name_len)
{
	struct vwasm_queue *q;
	uint32_t id;

	if (qs == NULL || name == NULL || name_len == 0)
		return (0);

	(void)vm_id;
	(void)vm_id_len;

	pthread_mutex_lock(&qs->mtx);

	for (q = qs->queues; q != NULL; q = q->next) {
		if (q->name != NULL && strlen(q->name) == name_len &&
		    memcmp(q->name, name, name_len) == 0) {
			id = q->id;
			pthread_mutex_unlock(&qs->mtx);
			return (id);
		}
	}

	pthread_mutex_unlock(&qs->mtx);
	return (0);
}

int
vwasm_queue_enqueue(struct vwasm_queue_store *qs,
    uint32_t queue_id, const uint8_t *data, size_t len)
{
	struct vwasm_queue *q;
	struct vwasm_queue_msg *msg;

	if (qs == NULL || queue_id == 0)
		return (-1);
	if (len > VWASM_QUEUE_MAX_MSG)
		return (-1);

	pthread_mutex_lock(&qs->mtx);

	/* Find queue */
	for (q = qs->queues; q != NULL; q = q->next) {
		if (q->id == queue_id)
			break;
	}
	if (q == NULL) {
		pthread_mutex_unlock(&qs->mtx);
		return (-1);
	}

	/* Check depth */
	if (q->depth >= VWASM_QUEUE_MAX_DEPTH) {
		pthread_mutex_unlock(&qs->mtx);
		return (-1);
	}

	/* Create message */
	msg = calloc(1, sizeof(*msg));
	if (msg == NULL) {
		pthread_mutex_unlock(&qs->mtx);
		return (-1);
	}

	if (data != NULL && len > 0) {
		msg->data = malloc(len);
		if (msg->data == NULL) {
			free(msg);
			pthread_mutex_unlock(&qs->mtx);
			return (-1);
		}
		memcpy(msg->data, data, len);
		msg->len = len;
	}

	/* Append to tail */
	if (q->tail != NULL)
		q->tail->next = msg;
	else
		q->head = msg;
	q->tail = msg;
	q->depth++;

	pthread_mutex_unlock(&qs->mtx);
	return (0);
}

int
vwasm_queue_dequeue(struct vwasm_queue_store *qs,
    uint32_t queue_id, uint8_t **data_out, size_t *len_out)
{
	struct vwasm_queue *q;
	struct vwasm_queue_msg *msg;

	if (qs == NULL || queue_id == 0)
		return (-1);

	pthread_mutex_lock(&qs->mtx);

	/* Find queue */
	for (q = qs->queues; q != NULL; q = q->next) {
		if (q->id == queue_id)
			break;
	}
	if (q == NULL || q->head == NULL) {
		pthread_mutex_unlock(&qs->mtx);
		return (-1); /* empty or not found */
	}

	/* Pop from head */
	msg = q->head;
	q->head = msg->next;
	if (q->head == NULL)
		q->tail = NULL;
	q->depth--;

	pthread_mutex_unlock(&qs->mtx);

	*data_out = msg->data;
	*len_out = msg->len;
	free(msg); /* data ownership transferred to caller */
	return (0);
}
