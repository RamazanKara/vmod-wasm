/*-
 * Copyright (c) 2025 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Proxy-Wasm HTTP call implementation.
 * Provides proxy_http_call for outbound HTTP requests from Wasm modules.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>
#include <fcntl.h>

#include "cache/cache.h"
#include "vcl.h"

#include <wasm.h>
#include <wasmtime.h>

#include "proxy_wasm.h"
#include "proxy_wasm_mem.h"
#include "wasm_engine.h"
#include "http_pool.h"

#define PW_HTTP_MAX_RESPONSE	(256 * 1024)  /* 256 KiB max response */

/* ----------------------------------------------------------------
 * Anti-IP-rebinding: reject connections to private/internal IPs.
 *
 * Prevents SSRF via DNS rebinding by validating resolved addresses
 * against RFC1918, RFC5735, RFC4193, and loopback ranges.
 * ---------------------------------------------------------------- */
static int
pw_is_private_addr(const struct sockaddr *sa)
{
	if (sa->sa_family == AF_INET) {
		const struct sockaddr_in *sin;
		uint32_t ip;

		sin = (const struct sockaddr_in *)sa;
		ip = ntohl(sin->sin_addr.s_addr);

		/* 127.0.0.0/8 — loopback */
		if ((ip >> 24) == 127)
			return (1);
		/* 10.0.0.0/8 — RFC1918 */
		if ((ip >> 24) == 10)
			return (1);
		/* 172.16.0.0/12 — RFC1918 */
		if ((ip >> 20) == (172 << 4 | 1))
			return (1);
		/* 192.168.0.0/16 — RFC1918 */
		if ((ip >> 16) == ((192 << 8) | 168))
			return (1);
		/* 169.254.0.0/16 — link-local */
		if ((ip >> 16) == ((169 << 8) | 254))
			return (1);
		/* 0.0.0.0/8 — "this" network */
		if ((ip >> 24) == 0)
			return (1);
		/* 100.64.0.0/10 — shared address space (CGN) */
		if ((ip >> 22) == (100 << 2 | 1))
			return (1);
		/* 192.0.0.0/24 — IETF protocol assignments */
		if ((ip >> 8) == ((192 << 16) | 0))
			return (1);
		/* 198.18.0.0/15 — benchmarking */
		if ((ip >> 17) == ((198 << 7) | 9))
			return (1);
		/* 240.0.0.0/4 — reserved (includes broadcast) */
		if ((ip >> 28) == 15)
			return (1);
	} else if (sa->sa_family == AF_INET6) {
		const struct sockaddr_in6 *sin6;
		const uint8_t *b;

		sin6 = (const struct sockaddr_in6 *)sa;
		b = sin6->sin6_addr.s6_addr;

		/* ::1/128 — loopback */
		if (memcmp(b, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\1", 16) == 0)
			return (1);
		/* ::/128 — unspecified */
		if (memcmp(b, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) == 0)
			return (1);
		/* fc00::/7 — unique local (RFC4193) */
		if ((b[0] & 0xfe) == 0xfc)
			return (1);
		/* fe80::/10 — link-local */
		if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80)
			return (1);
		/* ::ffff:0:0/96 — IPv4-mapped, check inner IPv4 */
		if (memcmp(b, "\0\0\0\0\0\0\0\0\0\0\xff\xff", 12) == 0) {
			struct sockaddr_in inner;
			memset(&inner, 0, sizeof(inner));
			inner.sin_family = AF_INET;
			memcpy(&inner.sin_addr.s_addr, b + 12, 4);
			return (pw_is_private_addr(
			    (const struct sockaddr *)&inner));
		}
	}
	return (0);
}

static int
pw_http_connect(const char *host, uint16_t port, int timeout_ms,
    int ssrf_exempt)
{
	struct addrinfo hints, *res, *rp;
	char port_str[8];
	int fd = -1;
	int flags;
	struct pollfd pfd;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	snprintf(port_str, sizeof(port_str), "%u", port);
	if (getaddrinfo(host, port_str, &hints, &res) != 0)
		return (-1);

	for (rp = res; rp != NULL; rp = rp->ai_next) {
		/* Anti-IP-rebinding: reject private/internal IPs
		 * unless the upstream was explicitly allowed */
		if (!ssrf_exempt && pw_is_private_addr(rp->ai_addr)) {
			continue;
		}

		fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (fd < 0)
			continue;

		/* Non-blocking connect with timeout */
		flags = fcntl(fd, F_GETFL, 0);
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);

		if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
			break;

		if (errno == EINPROGRESS) {
			pfd.fd = fd;
			pfd.events = POLLOUT;
			if (poll(&pfd, 1, timeout_ms) > 0 &&
			    (pfd.revents & POLLOUT)) {
				int err = 0;
				socklen_t len = sizeof(err);
				getsockopt(fd, SOL_SOCKET, SO_ERROR,
				    &err, &len);
				if (err == 0)
					break;
			}
		}

		close(fd);
		fd = -1;
	}

	freeaddrinfo(res);

	if (fd >= 0) {
		struct timeval tv;

		/* Back to blocking for I/O */
		flags = fcntl(fd, F_GETFL, 0);
		fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

		/* Set socket timeout */
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	}

	return (fd);
}

static int
pw_http_parse_upstream(const char *upstream, size_t len,
    char *host, size_t host_sz, uint16_t *port)
{
	const char *colon;
	size_t hlen;

	if (len == 0 || len >= host_sz)
		return (-1);

	colon = memchr(upstream, ':', len);
	if (colon != NULL) {
		hlen = (size_t)(colon - upstream);
		if (hlen == 0 || hlen >= host_sz)
			return (-1);
		memcpy(host, upstream, hlen);
		host[hlen] = '\0';
		*port = (uint16_t)atoi(colon + 1);
		if (*port == 0)
			*port = 80;
	} else {
		memcpy(host, upstream, len);
		host[len] = '\0';
		*port = 80;
	}
	return (0);
}

wasm_trap_t *
pw_proxy_http_call(void *env, wasmtime_caller_t *caller,
    const wasmtime_val_t *args, size_t nargs,
    wasmtime_val_t *results, size_t nresults)
{
	struct vwasm_proxy_ctx *ctx;
	char upstream_buf[256], host[256];
	uint16_t port;
	uint32_t upstream_size, headers_ptr, headers_size;
	uint32_t body_ptr, body_size, timeout_ms;
	int fd = -1;
	char request_buf[8192];
	int req_len;
	uint8_t *response_buf = NULL;
	size_t response_len = 0;
	ssize_t n;
	uint32_t token_id = 1;
	const char *method;
	const char *path;
	char extra_headers[4096];
	size_t extra_len;
	uint8_t *body_start;
	size_t resp_body_len;
	uint32_t resp_num_headers;
	size_t header_end_offset;
	size_t i;
	struct vwasm_http_pool *http_pool;
	struct vwasm_http_conn *pool_conn = NULL;
	int ssrf_exempt = 0;

	(void)env;
	(void)nargs;
	(void)nresults;
	ctx = wasmtime_context_get_data(wasmtime_caller_context(caller));
	AN(ctx);
	results[0].kind = WASMTIME_I32;

	/* Rate limiting: reject if exceeded max calls per request */
	if (ctx->http_call_max > 0 &&
	    ctx->http_call_count >= ctx->http_call_max) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}
	ctx->http_call_count++;

	upstream_size = (uint32_t)args[1].of.i32;
	headers_ptr = (uint32_t)args[2].of.i32;
	headers_size = (uint32_t)args[3].of.i32;
	body_ptr = (uint32_t)args[4].of.i32;
	body_size = (uint32_t)args[5].of.i32;
	/* args[6], args[7] = trailers (ignored) */
	timeout_ms = (uint32_t)args[8].of.i32;

	/*
	 * Timeout priority: module-supplied > engine-configured > cap.
	 * If module passes 0, use the engine-configured default.
	 */
	if (timeout_ms == 0)
		timeout_ms = ctx->http_timeout_ms;
	if (timeout_ms == 0)
		timeout_ms = VWASM_DEFAULT_HTTP_TIMEOUT_MS;
	if (timeout_ms > 30000)
		timeout_ms = 30000; /* Cap at 30s */

	/* Read upstream host:port */
	if (pw_read_string(ctx, (uint32_t)args[0].of.i32,
	    upstream_size, upstream_buf, sizeof(upstream_buf)) != 0) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	if (pw_http_parse_upstream(upstream_buf, upstream_size,
	    host, sizeof(host), &port) != 0) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	/* SSRF protection: check upstream against allowlist */
	if (ctx->num_allowed_upstreams > 0 &&
	    ctx->allowed_upstreams != NULL) {
		char upstream_check[280];
		uint32_t ui;
		int allowed = 0;

		snprintf(upstream_check, sizeof(upstream_check),
		    "%s:%u", host, (unsigned)port);

		for (ui = 0; ui < ctx->num_allowed_upstreams; ui++) {
			if (strcmp(upstream_check,
			    ctx->allowed_upstreams[ui]) == 0) {
				allowed = 1;
				break;
			}
		}
		if (!allowed) {
			results[0].of.i32 = PROXY_BAD_ARGUMENT;
			return (NULL);
		}
		ssrf_exempt = 1;
	}

	/*
	 * Parse request headers from serialized format.
	 * Extract :method, :path, and other headers.
	 */
	method = "GET";
	path = "/";
	extra_headers[0] = '\0';
	extra_len = 0;

	if (headers_size >= 4 &&
	    pw_validate_region(ctx, headers_ptr, headers_size)) {
		uint8_t *hdr_data = pw_mem_ptr(ctx, headers_ptr);
		uint32_t num_pairs, hdr_offset, hi;

		memcpy(&num_pairs, hdr_data, 4);
		if (num_pairs <= 64) {
			hdr_offset = 4 + num_pairs * 8;
			for (hi = 0; hi < num_pairs &&
			    hdr_offset < headers_size; hi++) {
				uint32_t ks, vs;
				const char *k, *v;

				memcpy(&ks, hdr_data + 4 + hi * 8, 4);
				memcpy(&vs, hdr_data + 4 + hi * 8 + 4, 4);

				if (hdr_offset + ks + 1 + vs + 1 >
				    headers_size)
					break;

				k = (const char *)(hdr_data + hdr_offset);
				hdr_offset += ks + 1;
				v = (const char *)(hdr_data + hdr_offset);
				hdr_offset += vs + 1;

				if (ks == 7 &&
				    memcmp(k, ":method", 7) == 0)
					method = v;
				else if (ks == 5 &&
				    memcmp(k, ":path", 5) == 0)
					path = v;
				else if (ks > 0 && k[0] != ':') {
					int written = snprintf(
					    extra_headers + extra_len,
					    sizeof(extra_headers) -
					    extra_len,
					    "%.*s: %.*s\r\n",
					    (int)ks, k, (int)vs, v);
					if (written > 0)
						extra_len +=
						    (size_t)written;
				}
			}
		}
	}

	/* Build HTTP/1.1 request */
	{
		const char *conn_hdr = (pool_conn != NULL) ?
		    "Connection: keep-alive" : "Connection: close";

		if (body_size > 0 &&
		    pw_validate_region(ctx, body_ptr, body_size)) {
			req_len = snprintf(request_buf,
			    sizeof(request_buf),
			    "%s %s HTTP/1.1\r\n"
			    "Host: %s\r\n"
			    "Content-Length: %u\r\n"
			    "%s\r\n"
			    "%s\r\n",
			    method, path, host, body_size,
			    conn_hdr, extra_headers);
		} else {
			body_size = 0;
			req_len = snprintf(request_buf,
			    sizeof(request_buf),
			    "%s %s HTTP/1.1\r\n"
			    "Host: %s\r\n"
			    "%s\r\n"
			    "%s\r\n",
			    method, path, host,
			    conn_hdr, extra_headers);
		}
	}

	if (req_len <= 0 || req_len >= (int)sizeof(request_buf)) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	/* Connect via HTTP pool (if available) or fallback to direct */
	http_pool = vwasm_engine_get_http_pool(ctx->engine);
	if (http_pool != NULL) {
		fd = vwasm_http_pool_acquire(http_pool, host,
		    port, timeout_ms, &pool_conn);
	} else {
		fd = pw_http_connect(host, port, (int)timeout_ms,
		    ssrf_exempt);
	}

	if (fd < 0) {
		if (http_pool != NULL)
			vwasm_http_pool_cb_failure(http_pool,
			    host, port);
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	/* Send request */
	if (write(fd, request_buf, (size_t)req_len) != req_len) {
		if (pool_conn != NULL)
			vwasm_http_pool_close(http_pool, pool_conn);
		else
			close(fd);
		if (http_pool != NULL)
			vwasm_http_pool_cb_failure(http_pool, host, port);
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	/* Send body if present */
	if (body_size > 0) {
		uint8_t *body_data = pw_mem_ptr(ctx, body_ptr);
		if (write(fd, body_data, body_size) != (ssize_t)body_size) {
			if (pool_conn != NULL)
				vwasm_http_pool_close(http_pool, pool_conn);
			else
				close(fd);
			if (http_pool != NULL)
				vwasm_http_pool_cb_failure(http_pool,
				    host, port);
			results[0].of.i32 = PROXY_INTERNAL;
			return (NULL);
		}
	}

	/* Read response (Content-Length aware to avoid blocking) */
	response_buf = malloc(PW_HTTP_MAX_RESPONSE);
	if (response_buf == NULL) {
		if (pool_conn != NULL)
			vwasm_http_pool_close(http_pool, pool_conn);
		else
			close(fd);
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	response_len = 0;
	{
		char *hdr_end = NULL;
		size_t content_length = 0;
		int cl_found = 0;

		/* Phase 1: read until end of headers (\r\n\r\n) */
		while (response_len < PW_HTTP_MAX_RESPONSE) {
			n = read(fd, response_buf + response_len,
			    PW_HTTP_MAX_RESPONSE - response_len);
			if (n <= 0)
				break;
			response_len += (size_t)n;
			response_buf[response_len] = '\0';
			hdr_end = strstr((char *)response_buf, "\r\n\r\n");
			if (hdr_end != NULL)
				break;
		}

		/* Phase 2: parse Content-Length and read remaining body */
		if (hdr_end != NULL) {
			char *cl;

			hdr_end += 4; /* skip \r\n\r\n */
			cl = strcasestr((char *)response_buf,
			    "Content-Length:");
			if (cl != NULL && cl < hdr_end) {
				content_length = (size_t)atoi(cl + 15);
				cl_found = 1;
			}

			if (cl_found) {
				size_t body_have = response_len -
				    (size_t)(hdr_end -
				    (char *)response_buf);
				size_t body_need = (content_length > body_have)
				    ? content_length - body_have : 0;

				while (body_need > 0 &&
				    response_len < PW_HTTP_MAX_RESPONSE) {
					n = read(fd,
					    response_buf + response_len,
					    (body_need <
					    PW_HTTP_MAX_RESPONSE - response_len)
					    ? body_need
					    : PW_HTTP_MAX_RESPONSE -
					    response_len);
					if (n <= 0)
						break;
					response_len += (size_t)n;
					body_need -= (size_t)n;
				}
			}
		}
	}

	/* Release connection back to pool (keep-alive) */
	if (pool_conn != NULL)
		vwasm_http_pool_release(http_pool, pool_conn,
		    response_len > 0 ? 1 : 0);
	else
		close(fd);

	/* Report success/failure to circuit breaker */
	if (http_pool != NULL) {
		if (response_len > 0)
			vwasm_http_pool_cb_success(http_pool, host, port);
		else
			vwasm_http_pool_cb_failure(http_pool, host, port);
	}

	if (response_len == 0) {
		free(response_buf);
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	/*
	 * Parse HTTP response: find headers and body.
	 * Headers end at \r\n\r\n.
	 */
	body_start = NULL;
	resp_body_len = 0;
	resp_num_headers = 0;
	header_end_offset = 0;

	for (i = 0; i + 3 < response_len; i++) {
		if (response_buf[i] == '\r' && response_buf[i+1] == '\n' &&
		    response_buf[i+2] == '\r' && response_buf[i+3] == '\n') {
			header_end_offset = i;
			body_start = response_buf + i + 4;
			resp_body_len = response_len - (i + 4);
			break;
		}
	}

	/* Count response headers (skip status line) */
	if (header_end_offset > 0) {
		const char *p = (const char *)response_buf;
		const char *end = (const char *)(response_buf +
		    header_end_offset);
		/* Skip first line (HTTP/1.1 200 OK) */
		while (p < end && *p != '\n')
			p++;
		if (p < end)
			p++;
		while (p < end) {
			resp_num_headers++;
			while (p < end && *p != '\n')
				p++;
			if (p < end)
				p++;
		}
	}

	/* Store response in context for get_buffer_bytes(HTTP_CALL_BODY) */
	ctx->http_response.raw_buf = response_buf;
	ctx->http_response.raw_len = response_len;
	ctx->http_response.body = body_start;
	ctx->http_response.body_len = resp_body_len;
	ctx->http_response.num_headers = resp_num_headers;
	ctx->http_response.valid = 1;

	/* Write token ID */
	if (pw_write_u32(ctx, (uint32_t)args[9].of.i32, token_id) != 0) {
		free(response_buf);
		ctx->http_response.raw_buf = NULL;
		ctx->http_response.valid = 0;
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	/*
	 * Defer proxy_on_http_call_response to after the current host
	 * function returns.  The proxy-wasm SDK uses RefCell internally
	 * and panics on re-entrant borrow if we call the callback from
	 * within the same execution of proxy_on_http_request_headers.
	 *
	 * The engine (wasm_engine.c) checks http_call_pending after the
	 * header function returns and invokes the callback then.
	 */
	ctx->http_call_pending = 1;
	ctx->http_call_token_id = token_id;
	ctx->http_call_num_headers = resp_num_headers;
	ctx->http_call_body_len = resp_body_len;

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}
