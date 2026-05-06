/*-
 * Copyright (c) 2025 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * HTTP Connection Pool implementation — persistent connections with
 * keep-alive, DNS cache, circuit breaker, and response caching.
 */

#include "config.h"
#include "compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "http_pool.h"

/* ----------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------- */

static uint32_t
cache_hash(const char *key)
{
	uint32_t hash = 5381;
	int c;

	while ((c = (unsigned char)*key++) != 0)
		hash = ((hash << 5) + hash) + c;
	return (hash % VWASM_HTTP_CACHE_BUCKETS);
}

static char *
make_cache_key(const char *method, const char *host, uint16_t port,
    const char *path)
{
	char *key;
	int len;

	len = snprintf(NULL, 0, "%s:%s:%u:%s", method, host, port, path);
	if (len < 0)
		return (NULL);

	key = malloc((size_t)len + 1);
	if (key == NULL)
		return (NULL);

	snprintf(key, (size_t)len + 1, "%s:%s:%u:%s",
	    method, host, port, path);
	return (key);
}

static char *
make_upstream_key(const char *host, uint16_t port)
{
	char key[280];

	snprintf(key, sizeof(key), "%s:%u", host, port);
	return (strdup(key));
}

static int
set_nonblocking(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1)
		return (-1);
	return (fcntl(fd, F_SETFL, flags | O_NONBLOCK));
}

static int
set_blocking(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1)
		return (-1);
	return (fcntl(fd, F_SETFL, flags & ~O_NONBLOCK));
}

/*
 * Check if a connection is still alive (not closed by peer).
 */
static int
conn_is_alive(int fd)
{
	struct pollfd pfd;
	char buf;
	int ret;

	pfd.fd = fd;
	pfd.events = POLLIN;
	pfd.revents = 0;

	ret = poll(&pfd, 1, 0);
	if (ret < 0)
		return (0);
	if (ret == 0)
		return (1);  /* No data pending = alive and idle */

	/* Data pending: either response data (unlikely) or EOF */
	ret = (int)recv(fd, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
	if (ret <= 0)
		return (0);  /* EOF or error: peer closed */
	return (1);  /* Data available: might be stale response */
}

/*
 * Connect to host:port with timeout.
 */
static int
connect_with_timeout(const struct sockaddr_storage *addr, socklen_t addrlen,
    uint32_t timeout_ms)
{
	int fd, ret, err;
	socklen_t errlen;
	struct pollfd pfd;

	fd = socket(addr->ss_family, SOCK_STREAM, 0);
	if (fd < 0)
		return (-1);

	/* Set TCP_NODELAY for low latency */
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	if (set_nonblocking(fd) != 0) {
		close(fd);
		return (-1);
	}

	ret = connect(fd, (const struct sockaddr *)addr, addrlen);
	if (ret == 0) {
		set_blocking(fd);
		return (fd);
	}

	if (errno != EINPROGRESS) {
		close(fd);
		return (-1);
	}

	/* Wait for connection with timeout */
	pfd.fd = fd;
	pfd.events = POLLOUT;
	pfd.revents = 0;

	ret = poll(&pfd, 1, (int)timeout_ms);
	if (ret <= 0) {
		close(fd);
		return (-1);
	}

	/* Check for connection error */
	errlen = sizeof(err);
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) != 0 ||
	    err != 0) {
		close(fd);
		return (-1);
	}

	set_blocking(fd);
	return (fd);
}

/* ----------------------------------------------------------------
 * Pool Creation / Destruction
 * ---------------------------------------------------------------- */

struct vwasm_http_pool *
vwasm_http_pool_new(size_t max_conns, uint32_t timeout_ms)
{
	struct vwasm_http_pool *pool;

	if (max_conns == 0)
		max_conns = VWASM_HTTP_POOL_DEFAULT_SIZE;
	if (max_conns > VWASM_HTTP_POOL_MAX_SIZE)
		max_conns = VWASM_HTTP_POOL_MAX_SIZE;
	if (timeout_ms == 0)
		timeout_ms = VWASM_HTTP_DEFAULT_TIMEOUT_MS;

	pool = calloc(1, sizeof(*pool));
	if (pool == NULL)
		return (NULL);

	pool->conns = calloc(max_conns, sizeof(struct vwasm_http_conn));
	if (pool->conns == NULL) {
		free(pool);
		return (NULL);
	}

	/* Initialize all connection slots */
	for (size_t i = 0; i < max_conns; i++) {
		pool->conns[i].fd = -1;
		pool->conns[i].in_use = 0;
	}

	pool->max_conns = max_conns;
	pool->num_active = 0;
	pool->default_timeout_ms = timeout_ms;
	pool->max_requests_per_conn = 100;
	pool->max_conn_age_s = 300;  /* 5 minutes */

	/* DNS cache */
	pool->dns_cache_capacity = 64;
	pool->dns_cache = calloc(pool->dns_cache_capacity,
	    sizeof(struct vwasm_http_dns_entry));
	pool->dns_cache_size = 0;
	pool->dns_ttl_s = VWASM_HTTP_DNS_TTL_DEFAULT_S;

	/* Circuit breakers */
	pool->max_breakers = 32;
	pool->breakers = calloc(pool->max_breakers,
	    sizeof(struct vwasm_http_circuit_breaker));
	pool->num_breakers = 0;

	/* Response cache */
	pool->cache_ttl_ms = VWASM_HTTP_CACHE_DEFAULT_TTL_MS;
	memset(pool->cache_buckets, 0, sizeof(pool->cache_buckets));

	/* Initialize locks */
	pthread_mutex_init(&pool->lock, NULL);
	pthread_mutex_init(&pool->dns_lock, NULL);
	pthread_mutex_init(&pool->cb_lock, NULL);
	pthread_mutex_init(&pool->cache_lock, NULL);

	if (pool->dns_cache == NULL || pool->breakers == NULL) {
		vwasm_http_pool_destroy(&pool);
		return (NULL);
	}

	return (pool);
}

void
vwasm_http_pool_destroy(struct vwasm_http_pool **poolp)
{
	struct vwasm_http_pool *pool;
	size_t i;

	if (poolp == NULL || *poolp == NULL)
		return;

	pool = *poolp;
	*poolp = NULL;

	/* Close all connections */
	if (pool->conns != NULL) {
		for (i = 0; i < pool->max_conns; i++) {
			if (pool->conns[i].fd >= 0) {
				close(pool->conns[i].fd);
				pool->conns[i].fd = -1;
			}
		}
		free(pool->conns);
	}

	/* Free DNS cache */
	free(pool->dns_cache);

	/* Free circuit breakers */
	free(pool->breakers);

	/* Free response cache */
	for (i = 0; i < VWASM_HTTP_CACHE_BUCKETS; i++) {
		struct vwasm_http_cache_entry *entry = pool->cache_buckets[i];
		while (entry != NULL) {
			struct vwasm_http_cache_entry *next = entry->next;
			free(entry->key);
			free(entry->response_data);
			free(entry);
			entry = next;
		}
	}

	pthread_mutex_destroy(&pool->lock);
	pthread_mutex_destroy(&pool->dns_lock);
	pthread_mutex_destroy(&pool->cb_lock);
	pthread_mutex_destroy(&pool->cache_lock);

	free(pool);
}

/* ----------------------------------------------------------------
 * DNS Cache Implementation
 * ---------------------------------------------------------------- */

int
vwasm_http_pool_resolve(struct vwasm_http_pool *pool,
    const char *host, uint16_t port,
    struct sockaddr_storage *addr_out, socklen_t *addrlen_out)
{
	struct addrinfo hints, *result = NULL;
	char port_str[8];
	time_t now;
	size_t i;

	if (pool == NULL || host == NULL)
		return (-1);

	now = time(NULL);

	/* Check DNS cache */
	pthread_mutex_lock(&pool->dns_lock);
	for (i = 0; i < pool->dns_cache_size; i++) {
		if (pool->dns_cache[i].valid &&
		    strcmp(pool->dns_cache[i].host, host) == 0 &&
		    (now - pool->dns_cache[i].resolved_at) <
		    (time_t)pool->dns_ttl_s) {
			memcpy(addr_out, &pool->dns_cache[i].addr,
			    pool->dns_cache[i].addrlen);
			*addrlen_out = pool->dns_cache[i].addrlen;
			pthread_mutex_unlock(&pool->dns_lock);
			__sync_fetch_and_add(&pool->stat_dns_hits, 1);

			/* Set port in the cached address */
			if (addr_out->ss_family == AF_INET) {
				((struct sockaddr_in *)addr_out)->sin_port =
				    htons(port);
			} else if (addr_out->ss_family == AF_INET6) {
				((struct sockaddr_in6 *)addr_out)->sin6_port =
				    htons(port);
			}
			return (0);
		}
	}
	pthread_mutex_unlock(&pool->dns_lock);

	__sync_fetch_and_add(&pool->stat_dns_misses, 1);

	/* Resolve hostname */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(port_str, sizeof(port_str), "%u", port);

	if (getaddrinfo(host, port_str, &hints, &result) != 0 ||
	    result == NULL)
		return (-1);

	memcpy(addr_out, result->ai_addr, result->ai_addrlen);
	*addrlen_out = result->ai_addrlen;

	/* Update DNS cache */
	pthread_mutex_lock(&pool->dns_lock);
	/* Find existing entry or empty slot */
	size_t slot = pool->dns_cache_size;
	for (i = 0; i < pool->dns_cache_size; i++) {
		if (strcmp(pool->dns_cache[i].host, host) == 0) {
			slot = i;
			break;
		}
	}
	if (slot >= pool->dns_cache_capacity)
		slot = 0;  /* Evict first entry on overflow */
	if (slot == pool->dns_cache_size && slot < pool->dns_cache_capacity)
		pool->dns_cache_size++;

	strlcpy(pool->dns_cache[slot].host, host,
	    sizeof(pool->dns_cache[slot].host));
	memcpy(&pool->dns_cache[slot].addr, result->ai_addr,
	    result->ai_addrlen);
	pool->dns_cache[slot].addrlen = result->ai_addrlen;
	pool->dns_cache[slot].resolved_at = now;
	pool->dns_cache[slot].valid = 1;
	pthread_mutex_unlock(&pool->dns_lock);

	freeaddrinfo(result);
	return (0);
}

/* ----------------------------------------------------------------
 * Circuit Breaker Implementation
 * ---------------------------------------------------------------- */

static struct vwasm_http_circuit_breaker *
cb_find_or_create(struct vwasm_http_pool *pool, const char *host,
    uint16_t port)
{
	char key[280];
	size_t i;

	snprintf(key, sizeof(key), "%s:%u", host, port);

	/* Find existing */
	for (i = 0; i < pool->num_breakers; i++) {
		if (strcmp(pool->breakers[i].upstream, key) == 0)
			return (&pool->breakers[i]);
	}

	/* Create new */
	if (pool->num_breakers >= pool->max_breakers)
		return (NULL);  /* No space, allow request */

	struct vwasm_http_circuit_breaker *cb =
	    &pool->breakers[pool->num_breakers++];
	strlcpy(cb->upstream, key, sizeof(cb->upstream));
	cb->state = VWASM_CB_CLOSED;
	cb->consecutive_failures = 0;
	cb->consecutive_successes = 0;
	cb->threshold = VWASM_HTTP_CB_THRESHOLD_DEFAULT;
	cb->half_open_timeout_s = VWASM_HTTP_CB_HALFOPEN_TIMEOUT_S;
	cb->total_requests = 0;
	cb->total_failures = 0;
	cb->total_rejections = 0;

	return (cb);
}

int
vwasm_http_pool_cb_allow(struct vwasm_http_pool *pool,
    const char *host, uint16_t port)
{
	struct vwasm_http_circuit_breaker *cb;
	time_t now;

	if (pool == NULL)
		return (0);

	pthread_mutex_lock(&pool->cb_lock);
	cb = cb_find_or_create(pool, host, port);
	if (cb == NULL) {
		pthread_mutex_unlock(&pool->cb_lock);
		return (0);  /* No CB tracking = allow */
	}

	__sync_fetch_and_add(&cb->total_requests, 1);

	switch (cb->state) {
	case VWASM_CB_CLOSED:
		pthread_mutex_unlock(&pool->cb_lock);
		return (0);

	case VWASM_CB_OPEN:
		now = time(NULL);
		if ((now - cb->last_failure_time) >=
		    (time_t)cb->half_open_timeout_s) {
			/* Transition to half-open: allow one probe */
			cb->state = VWASM_CB_HALF_OPEN;
			cb->consecutive_successes = 0;
			pthread_mutex_unlock(&pool->cb_lock);
			return (0);
		}
		__sync_fetch_and_add(&cb->total_rejections, 1);
		__sync_fetch_and_add(&pool->stat_cb_rejections, 1);
		pthread_mutex_unlock(&pool->cb_lock);
		return (-1);

	case VWASM_CB_HALF_OPEN:
		/* In half-open: allow requests (probing) */
		pthread_mutex_unlock(&pool->cb_lock);
		return (0);

	default:
		pthread_mutex_unlock(&pool->cb_lock);
		return (0);
	}
}

void
vwasm_http_pool_cb_success(struct vwasm_http_pool *pool,
    const char *host, uint16_t port)
{
	struct vwasm_http_circuit_breaker *cb;

	if (pool == NULL)
		return;

	pthread_mutex_lock(&pool->cb_lock);
	cb = cb_find_or_create(pool, host, port);
	if (cb == NULL) {
		pthread_mutex_unlock(&pool->cb_lock);
		return;
	}

	cb->consecutive_failures = 0;
	cb->consecutive_successes++;
	cb->last_success_time = time(NULL);

	if (cb->state == VWASM_CB_HALF_OPEN) {
		/* After successful probe, close the circuit */
		cb->state = VWASM_CB_CLOSED;
	}

	pthread_mutex_unlock(&pool->cb_lock);
}

void
vwasm_http_pool_cb_failure(struct vwasm_http_pool *pool,
    const char *host, uint16_t port)
{
	struct vwasm_http_circuit_breaker *cb;

	if (pool == NULL)
		return;

	pthread_mutex_lock(&pool->cb_lock);
	cb = cb_find_or_create(pool, host, port);
	if (cb == NULL) {
		pthread_mutex_unlock(&pool->cb_lock);
		return;
	}

	cb->consecutive_failures++;
	cb->consecutive_successes = 0;
	cb->last_failure_time = time(NULL);
	__sync_fetch_and_add(&cb->total_failures, 1);

	if (cb->state == VWASM_CB_HALF_OPEN) {
		/* Failed probe: go back to open */
		cb->state = VWASM_CB_OPEN;
	} else if (cb->state == VWASM_CB_CLOSED &&
	    cb->consecutive_failures >= cb->threshold) {
		/* Threshold exceeded: open the circuit */
		cb->state = VWASM_CB_OPEN;
	}

	pthread_mutex_unlock(&pool->cb_lock);
}

/* ----------------------------------------------------------------
 * Connection Pool Implementation
 * ---------------------------------------------------------------- */

int
vwasm_http_pool_acquire(struct vwasm_http_pool *pool,
    const char *host, uint16_t port, uint32_t timeout_ms,
    struct vwasm_http_conn **conn_out)
{
	struct sockaddr_storage addr;
	socklen_t addrlen;
	time_t now;
	size_t i;
	int fd;

	if (pool == NULL || host == NULL || conn_out == NULL)
		return (-1);

	*conn_out = NULL;
	if (timeout_ms == 0)
		timeout_ms = pool->default_timeout_ms;

	/* Check circuit breaker */
	if (vwasm_http_pool_cb_allow(pool, host, port) != 0)
		return (-1);

	now = time(NULL);

	/* Try to find an existing idle connection to this host:port */
	pthread_mutex_lock(&pool->lock);
	for (i = 0; i < pool->max_conns; i++) {
		struct vwasm_http_conn *c = &pool->conns[i];

		if (c->fd < 0 || c->in_use)
			continue;
		if (strcmp(c->host, host) != 0 || c->port != port)
			continue;

		/* Check connection age */
		if ((now - c->created_at) > (time_t)pool->max_conn_age_s) {
			close(c->fd);
			c->fd = -1;
			continue;
		}

		/* Check max requests per connection */
		if (c->requests_served >= (int)pool->max_requests_per_conn) {
			close(c->fd);
			c->fd = -1;
			continue;
		}

		/* Check if connection is still alive */
		if (!conn_is_alive(c->fd)) {
			close(c->fd);
			c->fd = -1;
			continue;
		}

		/* Found a valid idle connection */
		c->in_use = 1;
		c->last_used = now;
		c->requests_served++;
		*conn_out = c;
		pthread_mutex_unlock(&pool->lock);
		__sync_fetch_and_add(&pool->stat_reuses, 1);
		__sync_fetch_and_add(&pool->stat_acquires, 1);
		return (c->fd);
	}
	pthread_mutex_unlock(&pool->lock);

	/* No existing connection available, create a new one */

	/* Resolve DNS */
	if (vwasm_http_pool_resolve(pool, host, port, &addr, &addrlen) != 0)
		return (-1);

	/* Connect with timeout */
	fd = connect_with_timeout(&addr, addrlen, timeout_ms);
	if (fd < 0)
		return (-1);

	/* Find a free slot in the pool */
	pthread_mutex_lock(&pool->lock);
	for (i = 0; i < pool->max_conns; i++) {
		struct vwasm_http_conn *c = &pool->conns[i];

		if (c->fd >= 0)
			continue;

		/* Use this empty slot */
		c->fd = fd;
		strlcpy(c->host, host, sizeof(c->host));
		c->port = port;
		c->created_at = now;
		c->last_used = now;
		c->in_use = 1;
		c->keep_alive = 1;
		c->requests_served = 1;
		*conn_out = c;
		pool->num_active++;
		pthread_mutex_unlock(&pool->lock);
		__sync_fetch_and_add(&pool->stat_creates, 1);
		__sync_fetch_and_add(&pool->stat_acquires, 1);
		return (fd);
	}
	pthread_mutex_unlock(&pool->lock);

	/*
	 * No free slot available. Evict the oldest idle connection.
	 */
	pthread_mutex_lock(&pool->lock);
	time_t oldest_time = now;
	size_t oldest_idx = 0;
	int found_idle = 0;

	for (i = 0; i < pool->max_conns; i++) {
		struct vwasm_http_conn *c = &pool->conns[i];
		if (c->fd >= 0 && !c->in_use &&
		    c->last_used <= oldest_time) {
			oldest_time = c->last_used;
			oldest_idx = i;
			found_idle = 1;
		}
	}

	if (found_idle) {
		struct vwasm_http_conn *c = &pool->conns[oldest_idx];
		close(c->fd);
		c->fd = fd;
		strlcpy(c->host, host, sizeof(c->host));
		c->port = port;
		c->created_at = now;
		c->last_used = now;
		c->in_use = 1;
		c->keep_alive = 1;
		c->requests_served = 1;
		*conn_out = c;
		pthread_mutex_unlock(&pool->lock);
		__sync_fetch_and_add(&pool->stat_evictions, 1);
		__sync_fetch_and_add(&pool->stat_creates, 1);
		__sync_fetch_and_add(&pool->stat_acquires, 1);
		return (fd);
	}
	pthread_mutex_unlock(&pool->lock);

	/* Pool completely full (all in-use). Use the fd without a slot.
	 * Caller gets fd but no conn pointer — they must close it manually. */
	/* Actually, let's allocate a temporary conn on the heap */
	struct vwasm_http_conn *tmp = calloc(1, sizeof(*tmp));
	if (tmp == NULL) {
		close(fd);
		return (-1);
	}
	tmp->fd = fd;
	strlcpy(tmp->host, host, sizeof(tmp->host));
	tmp->port = port;
	tmp->created_at = now;
	tmp->last_used = now;
	tmp->in_use = 1;
	tmp->keep_alive = 0;  /* Don't try to pool this one */
	tmp->requests_served = 1;
	*conn_out = tmp;
	__sync_fetch_and_add(&pool->stat_creates, 1);
	__sync_fetch_and_add(&pool->stat_acquires, 1);
	return (fd);
}

void
vwasm_http_pool_release(struct vwasm_http_pool *pool,
    struct vwasm_http_conn *conn, int keep_alive)
{
	if (pool == NULL || conn == NULL)
		return;

	__sync_fetch_and_add(&pool->stat_releases, 1);

	if (!keep_alive || !conn->keep_alive || conn->fd < 0) {
		/* Close the connection */
		if (conn->fd >= 0) {
			close(conn->fd);
			conn->fd = -1;
		}
		conn->in_use = 0;

		/* Check if this is a heap-allocated overflow conn */
		pthread_mutex_lock(&pool->lock);
		int is_pooled = 0;
		for (size_t i = 0; i < pool->max_conns; i++) {
			if (&pool->conns[i] == conn) {
				is_pooled = 1;
				break;
			}
		}
		pthread_mutex_unlock(&pool->lock);

		if (!is_pooled)
			free(conn);
		return;
	}

	/* Return to pool for reuse */
	conn->in_use = 0;
	conn->last_used = time(NULL);

	/* Check if this is a heap-allocated overflow conn */
	pthread_mutex_lock(&pool->lock);
	int is_pooled = 0;
	for (size_t i = 0; i < pool->max_conns; i++) {
		if (&pool->conns[i] == conn) {
			is_pooled = 1;
			break;
		}
	}
	pthread_mutex_unlock(&pool->lock);

	if (!is_pooled) {
		/* Can't return overflow conn to pool, close it */
		if (conn->fd >= 0)
			close(conn->fd);
		free(conn);
	}
}

void
vwasm_http_pool_close(struct vwasm_http_pool *pool,
    struct vwasm_http_conn *conn)
{
	vwasm_http_pool_release(pool, conn, 0);
}

/* ----------------------------------------------------------------
 * Response Cache Implementation
 * ---------------------------------------------------------------- */

uint8_t *
vwasm_http_pool_cache_get(struct vwasm_http_pool *pool,
    const char *method, const char *host, uint16_t port,
    const char *path, size_t *len_out, int *status_out)
{
	char *key;
	uint32_t bucket;
	struct vwasm_http_cache_entry *entry;
	time_t now;
	uint8_t *data;

	if (pool == NULL || method == NULL || host == NULL || path == NULL)
		return (NULL);

	key = make_cache_key(method, host, port, path);
	if (key == NULL)
		return (NULL);

	bucket = cache_hash(key);
	now = time(NULL);

	pthread_mutex_lock(&pool->cache_lock);
	entry = pool->cache_buckets[bucket];
	while (entry != NULL) {
		if (strcmp(entry->key, key) == 0) {
			/* Check TTL */
			time_t age_ms = (now - entry->cached_at) * 1000;
			if (age_ms < (time_t)entry->ttl_ms) {
				/* Cache hit */
				data = malloc(entry->response_len);
				if (data != NULL) {
					memcpy(data, entry->response_data,
					    entry->response_len);
					*len_out = entry->response_len;
					*status_out = entry->status_code;
				}
				pthread_mutex_unlock(&pool->cache_lock);
				free(key);
				__sync_fetch_and_add(
				    &pool->stat_cache_hits, 1);
				return (data);
			}
			break;  /* Expired */
		}
		entry = entry->next;
	}
	pthread_mutex_unlock(&pool->cache_lock);
	free(key);
	__sync_fetch_and_add(&pool->stat_cache_misses, 1);
	return (NULL);
}

void
vwasm_http_pool_cache_put(struct vwasm_http_pool *pool,
    const char *method, const char *host, uint16_t port,
    const char *path, const uint8_t *data, size_t len,
    int status_code)
{
	char *key;
	uint32_t bucket;
	struct vwasm_http_cache_entry *entry, *prev;

	if (pool == NULL || method == NULL || host == NULL || path == NULL)
		return;
	if (data == NULL || len == 0 || len > VWASM_HTTP_MAX_RESPONSE_SIZE)
		return;

	key = make_cache_key(method, host, port, path);
	if (key == NULL)
		return;

	bucket = cache_hash(key);

	pthread_mutex_lock(&pool->cache_lock);

	/* Check if entry already exists */
	entry = pool->cache_buckets[bucket];
	while (entry != NULL) {
		if (strcmp(entry->key, key) == 0) {
			/* Update existing */
			uint8_t *new_data = malloc(len);
			if (new_data != NULL) {
				free(entry->response_data);
				entry->response_data = new_data;
				memcpy(entry->response_data, data, len);
				entry->response_len = len;
				entry->status_code = status_code;
				entry->cached_at = time(NULL);
				entry->ttl_ms = pool->cache_ttl_ms;
			}
			pthread_mutex_unlock(&pool->cache_lock);
			free(key);
			return;
		}
		entry = entry->next;
	}

	/* Create new entry */
	entry = calloc(1, sizeof(*entry));
	if (entry == NULL) {
		pthread_mutex_unlock(&pool->cache_lock);
		free(key);
		return;
	}

	entry->key = key;
	entry->response_data = malloc(len);
	if (entry->response_data == NULL) {
		pthread_mutex_unlock(&pool->cache_lock);
		free(entry->key);
		free(entry);
		return;
	}
	memcpy(entry->response_data, data, len);
	entry->response_len = len;
	entry->status_code = status_code;
	entry->cached_at = time(NULL);
	entry->ttl_ms = pool->cache_ttl_ms;

	/* Prepend to bucket chain */
	entry->next = pool->cache_buckets[bucket];
	pool->cache_buckets[bucket] = entry;

	pthread_mutex_unlock(&pool->cache_lock);
	(void)prev;
}

/* ----------------------------------------------------------------
 * Statistics
 * ---------------------------------------------------------------- */

char *
vwasm_http_pool_stats_json(const struct vwasm_http_pool *pool)
{
	char *buf;
	int len;

	if (pool == NULL)
		return (NULL);

	buf = malloc(1024);
	if (buf == NULL)
		return (NULL);

	len = snprintf(buf, 1024,
	    "{\"max_conns\":%zu,\"acquires\":%llu,\"releases\":%llu,"
	    "\"creates\":%llu,\"reuses\":%llu,\"evictions\":%llu,"
	    "\"dns_hits\":%llu,\"dns_misses\":%llu,"
	    "\"cache_hits\":%llu,\"cache_misses\":%llu,"
	    "\"cb_rejections\":%llu}",
	    pool->max_conns,
	    (unsigned long long)pool->stat_acquires,
	    (unsigned long long)pool->stat_releases,
	    (unsigned long long)pool->stat_creates,
	    (unsigned long long)pool->stat_reuses,
	    (unsigned long long)pool->stat_evictions,
	    (unsigned long long)pool->stat_dns_hits,
	    (unsigned long long)pool->stat_dns_misses,
	    (unsigned long long)pool->stat_cache_hits,
	    (unsigned long long)pool->stat_cache_misses,
	    (unsigned long long)pool->stat_cb_rejections);

	if (len < 0 || len >= 1024) {
		free(buf);
		return (NULL);
	}

	return (buf);
}
