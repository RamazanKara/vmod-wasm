/*-
 * Copyright (c) 2025 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * HTTP Connection Pool — persistent connections with keep-alive,
 * DNS cache, circuit breaker, and response caching for proxy_http_call.
 */

#ifndef VWASM_HTTP_POOL_H
#define VWASM_HTTP_POOL_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <pthread.h>
#include <netdb.h>

/* Pool configuration defaults */
#define VWASM_HTTP_POOL_DEFAULT_SIZE		16
#define VWASM_HTTP_POOL_MAX_SIZE		256
#define VWASM_HTTP_DNS_TTL_DEFAULT_S		60
#define VWASM_HTTP_CB_THRESHOLD_DEFAULT		5
#define VWASM_HTTP_CB_HALFOPEN_TIMEOUT_S	10
#define VWASM_HTTP_DEFAULT_TIMEOUT_MS		1000
#define VWASM_HTTP_MAX_RESPONSE_SIZE		(1024 * 1024)  /* 1MB */
#define VWASM_HTTP_CACHE_DEFAULT_TTL_MS		1000

/* Circuit breaker states */
enum vwasm_cb_state {
	VWASM_CB_CLOSED = 0,	/* Normal operation */
	VWASM_CB_OPEN = 1,	/* Failing, reject calls */
	VWASM_CB_HALF_OPEN = 2	/* Probing after timeout */
};

/* ----------------------------------------------------------------
 * Connection — a single persistent TCP connection
 * ---------------------------------------------------------------- */

struct vwasm_http_conn {
	int			 fd;		/* Socket FD, -1 if invalid */
	char			 host[256];	/* Remote host */
	uint16_t		 port;		/* Remote port */
	time_t			 last_used;	/* Last successful use time */
	time_t			 created_at;	/* Connection creation time */
	volatile int		 in_use;	/* 0=free, 1=acquired */
	int			 keep_alive;	/* Server supports keep-alive */
	int			 requests_served; /* # requests on this conn */
};

/* ----------------------------------------------------------------
 * DNS Cache Entry
 * ---------------------------------------------------------------- */

struct vwasm_http_dns_entry {
	char			 host[256];
	struct sockaddr_storage	 addr;
	socklen_t		 addrlen;
	time_t			 resolved_at;
	int			 valid;
};

/* ----------------------------------------------------------------
 * Circuit Breaker — per upstream host:port
 * ---------------------------------------------------------------- */

struct vwasm_http_circuit_breaker {
	char			 upstream[280];	/* "host:port" key */
	volatile int		 state;		/* enum vwasm_cb_state */
	volatile uint32_t	 consecutive_failures;
	volatile uint32_t	 consecutive_successes;
	time_t			 last_failure_time;
	time_t			 last_success_time;
	uint32_t		 threshold;	/* Failures to open */
	uint32_t		 half_open_timeout_s;
	/* Statistics */
	volatile uint64_t	 total_requests;
	volatile uint64_t	 total_failures;
	volatile uint64_t	 total_rejections;
};

/* ----------------------------------------------------------------
 * Response Cache Entry
 * ---------------------------------------------------------------- */

struct vwasm_http_cache_entry {
	char			*key;		/* "method:host:port:path" */
	uint8_t			*response_data;
	size_t			 response_len;
	int			 status_code;
	time_t			 cached_at;
	uint32_t		 ttl_ms;
	struct vwasm_http_cache_entry *next;	/* Hash chain */
};

/* ----------------------------------------------------------------
 * HTTP Pool
 * ---------------------------------------------------------------- */

#define VWASM_HTTP_CACHE_BUCKETS	64

struct vwasm_http_pool {
	pthread_mutex_t		 lock;		/* Protects conn array */
	struct vwasm_http_conn	*conns;		/* Connection slots */
	size_t			 max_conns;	/* Pool capacity */
	size_t			 num_active;	/* Currently in-use count */
	/* DNS cache */
	pthread_mutex_t		 dns_lock;
	struct vwasm_http_dns_entry *dns_cache;
	size_t			 dns_cache_capacity;
	size_t			 dns_cache_size;
	uint32_t		 dns_ttl_s;
	/* Circuit breakers (one per upstream) */
	pthread_mutex_t		 cb_lock;
	struct vwasm_http_circuit_breaker *breakers;
	size_t			 num_breakers;
	size_t			 max_breakers;
	/* Response cache */
	pthread_mutex_t		 cache_lock;
	struct vwasm_http_cache_entry *cache_buckets[VWASM_HTTP_CACHE_BUCKETS];
	uint32_t		 cache_ttl_ms;
	/* Configuration */
	uint32_t		 default_timeout_ms;
	uint32_t		 max_requests_per_conn;
	uint32_t		 max_conn_age_s;
	/* Statistics */
	volatile uint64_t	 stat_acquires;
	volatile uint64_t	 stat_releases;
	volatile uint64_t	 stat_creates;
	volatile uint64_t	 stat_reuses;
	volatile uint64_t	 stat_evictions;
	volatile uint64_t	 stat_dns_hits;
	volatile uint64_t	 stat_dns_misses;
	volatile uint64_t	 stat_cache_hits;
	volatile uint64_t	 stat_cache_misses;
	volatile uint64_t	 stat_cb_rejections;
};

/* ----------------------------------------------------------------
 * Pool API
 * ---------------------------------------------------------------- */

/*
 * Create a new HTTP connection pool.
 * max_conns: maximum number of persistent connections.
 * timeout_ms: default HTTP request timeout.
 *
 * Returns NULL on failure.
 */
struct vwasm_http_pool *vwasm_http_pool_new(size_t max_conns,
    uint32_t timeout_ms);

/*
 * Destroy the pool, closing all connections.
 */
void vwasm_http_pool_destroy(struct vwasm_http_pool **poolp);

/*
 * Acquire a connection to host:port from the pool.
 * If an existing keep-alive connection is available, returns it.
 * Otherwise creates a new connection.
 *
 * timeout_ms: connect timeout (0 = use pool default).
 * ssrf_exempt: allow private/internal resolved addresses.
 * Returns FD >= 0 on success, -1 on failure.
 * On success, conn_out is set (caller must release when done).
 */
int vwasm_http_pool_acquire(struct vwasm_http_pool *pool,
    const char *host, uint16_t port, uint32_t timeout_ms, int ssrf_exempt,
    struct vwasm_http_conn **conn_out);

/*
 * Release a connection back to the pool.
 * If keep_alive is set and the connection is healthy, it's returned
 * to the pool for reuse. Otherwise it's closed.
 */
void vwasm_http_pool_release(struct vwasm_http_pool *pool,
    struct vwasm_http_conn *conn, int keep_alive);

/*
 * Close and destroy a connection (on error).
 */
void vwasm_http_pool_close(struct vwasm_http_pool *pool,
    struct vwasm_http_conn *conn);

/* ----------------------------------------------------------------
 * Circuit Breaker API
 * ---------------------------------------------------------------- */

/*
 * Check if a request to upstream should be allowed.
 * Returns 0 if allowed, -1 if circuit is open (request rejected).
 */
int vwasm_http_pool_cb_allow(struct vwasm_http_pool *pool,
    const char *host, uint16_t port);

/*
 * Record a successful request to upstream.
 */
void vwasm_http_pool_cb_success(struct vwasm_http_pool *pool,
    const char *host, uint16_t port);

/*
 * Record a failed request to upstream.
 */
void vwasm_http_pool_cb_failure(struct vwasm_http_pool *pool,
    const char *host, uint16_t port);

/* ----------------------------------------------------------------
 * DNS Cache API
 * ---------------------------------------------------------------- */

/*
 * Resolve a hostname, using the cache if available.
 * addr_out: filled with resolved address.
 * addrlen_out: filled with address length.
 *
 * Returns 0 on success, -1 on failure.
 */
int vwasm_http_pool_resolve(struct vwasm_http_pool *pool,
    const char *host, uint16_t port,
    struct sockaddr_storage *addr_out, socklen_t *addrlen_out);

/* ----------------------------------------------------------------
 * Response Cache API
 * ---------------------------------------------------------------- */

/*
 * Look up a cached response.
 * Returns a copy of the cached response data, or NULL if not cached.
 * Caller must free the returned data.
 */
uint8_t *vwasm_http_pool_cache_get(struct vwasm_http_pool *pool,
    const char *method, const char *host, uint16_t port,
    const char *path, size_t *len_out, int *status_out);

/*
 * Store a response in the cache.
 */
void vwasm_http_pool_cache_put(struct vwasm_http_pool *pool,
    const char *method, const char *host, uint16_t port,
    const char *path, const uint8_t *data, size_t len,
    int status_code);

/*
 * Get pool statistics as JSON string (caller must free).
 */
char *vwasm_http_pool_stats_json(const struct vwasm_http_pool *pool);

/*
 * Return true for private/internal addresses that proxy_http_call must not
 * reach unless the upstream was explicitly allowlisted.
 */
int vwasm_http_addr_is_private(const struct sockaddr *sa);

#endif /* VWASM_HTTP_POOL_H */
