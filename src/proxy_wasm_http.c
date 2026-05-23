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

#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
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

int
vwasm_http_call_cmp(const struct vwasm_http_call_entry *a,
    const struct vwasm_http_call_entry *b)
{
	if (a->token_id < b->token_id)
		return (-1);
	if (a->token_id > b->token_id)
		return (1);
	return (0);
}

VRBT_GENERATE(vwasm_http_call_tree, vwasm_http_call_entry, entry,
    vwasm_http_call_cmp)

#define PW_HTTP_MAX_RESPONSE	(256 * 1024)  /* 256 KiB max response */

static int
pw_http_is_token_char(unsigned char c)
{
	if (c >= '0' && c <= '9')
		return (1);
	if (c >= 'A' && c <= 'Z')
		return (1);
	if (c >= 'a' && c <= 'z')
		return (1);
	switch (c) {
	case '!':
	case '#':
	case '$':
	case '%':
	case '&':
	case '\'':
	case '*':
	case '+':
	case '-':
	case '.':
	case '^':
	case '_':
	case '`':
	case '|':
	case '~':
		return (1);
	default:
		return (0);
	}
}

static int
pw_http_has_line_ctl(const char *s, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '\0' || c == '\r' || c == '\n')
			return (1);
	}
	return (0);
}

static int
pw_http_host_ok(const char *s, size_t len)
{
	size_t i;

	if (len == 0)
		return (0);
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c <= ' ' || c >= 0x7f || c == '/')
			return (0);
	}
	return (1);
}

static int
pw_http_method_ok(const char *s, size_t len)
{
	size_t i;

	if (len == 0)
		return (0);
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];
		if (!pw_http_is_token_char(c))
			return (0);
	}
	return (1);
}

static int
pw_http_path_ok(const char *s, size_t len)
{
	size_t i;

	if (len == 0)
		return (0);
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c <= ' ' || c >= 0x7f)
			return (0);
	}
	return (1);
}

static int
pw_http_header_name_ok(const char *s, size_t len)
{
	size_t i;

	if (len == 0)
		return (0);
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];
		if (!pw_http_is_token_char(c))
			return (0);
	}
	return (1);
}

static int
pw_http_header_value_ok(const char *s, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '\0' || c == '\r' || c == '\n' || c == 0x7f)
			return (0);
		if (c < ' ' && c != '\t')
			return (0);
	}
	return (1);
}

static int
pw_http_append_header(char *dst, size_t dst_sz, size_t *dst_len,
    const char *key, size_t key_len, const char *value, size_t value_len)
{
	size_t avail;
	int written;

	if (!pw_http_header_name_ok(key, key_len) ||
	    !pw_http_header_value_ok(value, value_len))
		return (-1);
	if (key_len > INT_MAX || value_len > INT_MAX)
		return (-1);
	if (*dst_len >= dst_sz)
		return (-1);

	avail = dst_sz - *dst_len;
	written = snprintf(dst + *dst_len, avail, "%.*s: %.*s\r\n",
	    (int)key_len, key, (int)value_len, value);
	if (written < 0 || (size_t)written >= avail)
		return (-1);

	*dst_len += (size_t)written;
	return (0);
}

static int
pw_http_parse_size(const char *s, size_t len, size_t *out)
{
	size_t value = 0;
	size_t i = 0;
	int digits = 0;

	while (i < len && (s[i] == ' ' || s[i] == '\t'))
		i++;
	for (; i < len; i++) {
		unsigned char c = (unsigned char)s[i];

		if (c == ' ' || c == '\t') {
			while (i < len && (s[i] == ' ' || s[i] == '\t'))
				i++;
			if (i != len)
				return (-1);
			break;
		}
		if (c < '0' || c > '9')
			return (-1);
		if (value > (SIZE_MAX - (size_t)(c - '0')) / 10)
			return (-1);
		value = value * 10 + (size_t)(c - '0');
		digits = 1;
	}
	if (!digits)
		return (-1);
	*out = value;
	return (0);
}

static int
pw_http_find_header_value(const char *headers, size_t headers_len,
    const char *name, const char **value, size_t *value_len)
{
	const char *p, *end, *line_end;
	size_t name_len;

	name_len = strlen(name);
	p = headers;
	end = headers + headers_len;

	/* Skip status line. */
	line_end = strstr(p, "\r\n");
	if (line_end == NULL || line_end > end)
		return (0);
	p = line_end + 2;

	while (p < end) {
		const char *colon;
		const char *v;
		const char *vend;
		size_t line_len;

		line_end = strstr(p, "\r\n");
		if (line_end == NULL || line_end > end)
			break;
		if (line_end == p)
			break;

		line_len = (size_t)(line_end - p);
		colon = memchr(p, ':', line_len);
		if (colon != NULL && (size_t)(colon - p) == name_len &&
		    strncasecmp(p, name, name_len) == 0) {
			v = colon + 1;
			while (v < line_end && (*v == ' ' || *v == '\t'))
				v++;
			vend = line_end;
			while (vend > v &&
			    (vend[-1] == ' ' || vend[-1] == '\t'))
				vend--;
			*value = v;
			*value_len = (size_t)(vend - v);
			return (1);
		}
		p = line_end + 2;
	}
	return (0);
}

static int
pw_http_value_has_token(const char *value, size_t value_len,
    const char *token)
{
	size_t token_len;
	size_t i = 0;

	token_len = strlen(token);
	while (i < value_len) {
		const char *start;
		const char *end;
		size_t len;

		while (i < value_len &&
		    (value[i] == ' ' || value[i] == '\t' ||
		     value[i] == ','))
			i++;
		start = value + i;
		while (i < value_len && value[i] != ',')
			i++;
		end = value + i;
		while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
			end--;
		len = (size_t)(end - start);
		if (len == token_len &&
		    strncasecmp(start, token, token_len) == 0)
			return (1);
	}
	return (0);
}

static int
pw_http_parse_port(const char *s, size_t len, uint16_t *port)
{
	unsigned long value = 0;
	size_t i;

	if (len == 0)
		return (-1);

	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c < '0' || c > '9')
			return (-1);
		value = value * 10 + (unsigned long)(c - '0');
		if (value > 65535)
			return (-1);
	}
	if (value == 0)
		return (-1);

	*port = (uint16_t)value;
	return (0);
}

static ssize_t
pw_http_read_some(int fd, void *buf, size_t len)
{
	ssize_t n;

	do {
		n = read(fd, buf, len);
	} while (n < 0 && errno == EINTR);
	return (n);
}

static int
pw_http_write_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	while (len > 0) {
		ssize_t written = write(fd, p, len);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			return (-1);
		}
		if (written == 0)
			return (-1);
		p += (size_t)written;
		len -= (size_t)written;
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
		if (!ssrf_exempt && vwasm_http_addr_is_private(rp->ai_addr)) {
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
	size_t hlen, plen;

	if (len == 0 || len >= host_sz)
		return (-1);
	if (pw_http_has_line_ctl(upstream, len))
		return (-1);

	colon = memchr(upstream, ':', len);
	if (colon != NULL) {
		hlen = (size_t)(colon - upstream);
		plen = len - hlen - 1;
		if (hlen == 0 || hlen >= host_sz)
			return (-1);
		if (!pw_http_host_ok(upstream, hlen))
			return (-1);
		if (pw_http_parse_port(colon + 1, plen, port) != 0)
			return (-1);
		memcpy(host, upstream, hlen);
		host[hlen] = '\0';
	} else {
		if (!pw_http_host_ok(upstream, len))
			return (-1);
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
	const char *method;
	const char *path;
	size_t method_len;
	size_t path_len;
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
	int response_complete = 0;
	int response_reusable = 0;
	int response_status = PROXY_INTERNAL;

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
	method_len = 3;
	path = "/";
	path_len = 1;
	extra_headers[0] = '\0';
	extra_len = 0;

	if (headers_size > 0) {
		uint8_t *hdr_data;
		uint32_t num_pairs, hi;
		size_t hdr_offset;

		if (headers_size < 4 ||
		    !pw_validate_region(ctx, headers_ptr, headers_size)) {
			results[0].of.i32 = PROXY_BAD_ARGUMENT;
			return (NULL);
		}
		hdr_data = pw_mem_ptr(ctx, headers_ptr);
		if (hdr_data == NULL) {
			results[0].of.i32 = PROXY_BAD_ARGUMENT;
			return (NULL);
		}

		memcpy(&num_pairs, hdr_data, 4);
		if (num_pairs > 64) {
			results[0].of.i32 = PROXY_BAD_ARGUMENT;
			return (NULL);
		}

		hdr_offset = 4 + (size_t)num_pairs * 8;
		if (hdr_offset > headers_size) {
			results[0].of.i32 = PROXY_BAD_ARGUMENT;
			return (NULL);
		}

		for (hi = 0; hi < num_pairs; hi++) {
			uint32_t ks, vs;
			const char *k, *v;

			memcpy(&ks, hdr_data + 4 + hi * 8, 4);
			memcpy(&vs, hdr_data + 4 + hi * 8 + 4, 4);

			if ((size_t)ks + 1 > (size_t)headers_size -
			    hdr_offset) {
				results[0].of.i32 = PROXY_BAD_ARGUMENT;
				return (NULL);
			}
			k = (const char *)(hdr_data + hdr_offset);
			if (hdr_data[hdr_offset + ks] != '\0') {
				results[0].of.i32 = PROXY_BAD_ARGUMENT;
				return (NULL);
			}
			hdr_offset += (size_t)ks + 1;

			if ((size_t)vs + 1 > (size_t)headers_size -
			    hdr_offset) {
				results[0].of.i32 = PROXY_BAD_ARGUMENT;
				return (NULL);
			}
			v = (const char *)(hdr_data + hdr_offset);
			if (hdr_data[hdr_offset + vs] != '\0') {
				results[0].of.i32 = PROXY_BAD_ARGUMENT;
				return (NULL);
			}
			hdr_offset += (size_t)vs + 1;

			if (ks == 7 && memcmp(k, ":method", 7) == 0) {
				method = v;
				method_len = vs;
			} else if (ks == 5 && memcmp(k, ":path", 5) == 0) {
				path = v;
				path_len = vs;
			} else if (ks > 0 && k[0] != ':') {
				if (pw_http_append_header(extra_headers,
				    sizeof(extra_headers), &extra_len,
				    k, ks, v, vs) != 0) {
					results[0].of.i32 =
					    PROXY_BAD_ARGUMENT;
					return (NULL);
				}
			}
		}
	}

	if (!pw_http_method_ok(method, method_len) ||
	    !pw_http_path_ok(path, path_len) ||
	    method_len > INT_MAX || path_len > INT_MAX) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	if (body_size > 0 &&
	    !pw_validate_region(ctx, body_ptr, body_size)) {
		results[0].of.i32 = PROXY_BAD_ARGUMENT;
		return (NULL);
	}

	http_pool = vwasm_engine_get_http_pool(ctx->engine);

	/* Build HTTP/1.1 request */
	{
		const char *conn_hdr = (http_pool != NULL && ssrf_exempt) ?
		    "Connection: keep-alive" : "Connection: close";

		if (body_size > 0) {
			req_len = snprintf(request_buf,
			    sizeof(request_buf),
			    "%.*s %.*s HTTP/1.1\r\n"
			    "Host: %s\r\n"
			    "Content-Length: %u\r\n"
			    "%s\r\n"
			    "%s\r\n",
			    (int)method_len, method,
			    (int)path_len, path, host, body_size,
			    conn_hdr, extra_headers);
		} else {
			req_len = snprintf(request_buf,
			    sizeof(request_buf),
			    "%.*s %.*s HTTP/1.1\r\n"
			    "Host: %s\r\n"
			    "%s\r\n"
			    "%s\r\n",
			    (int)method_len, method,
			    (int)path_len, path, host,
			    conn_hdr, extra_headers);
		}
	}

	if (req_len <= 0 || req_len >= (int)sizeof(request_buf)) {
		results[0].of.i32 = PROXY_INTERNAL;
		return (NULL);
	}

	/* Connect via HTTP pool (if available) or fallback to direct */
	if (http_pool != NULL) {
		fd = vwasm_http_pool_acquire(http_pool, host,
		    port, timeout_ms, ssrf_exempt, &pool_conn);
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
	if (pw_http_write_all(fd, request_buf, (size_t)req_len) != 0) {
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
		if (body_data == NULL ||
		    pw_http_write_all(fd, body_data, body_size) != 0) {
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
	response_buf = malloc(PW_HTTP_MAX_RESPONSE + 1);
	if (response_buf == NULL) {
		if (pool_conn != NULL)
			vwasm_http_pool_close(http_pool, pool_conn);
		else
			close(fd);
		if (http_pool != NULL)
			vwasm_http_pool_cb_failure(http_pool, host, port);
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
			n = pw_http_read_some(fd, response_buf + response_len,
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
			const char *value;
			size_t value_len;
			size_t header_bytes;
			int conn_close = 0;
			int http11;

			hdr_end += 4; /* skip \r\n\r\n */
			header_bytes = (size_t)(hdr_end -
			    (char *)response_buf);
			http11 = response_len >= 8 &&
			    memcmp(response_buf, "HTTP/1.1", 8) == 0;

			if (pw_http_find_header_value((char *)response_buf,
			    header_bytes, "Transfer-Encoding", &value,
			    &value_len) &&
			    pw_http_value_has_token(value, value_len,
			    "chunked")) {
				response_status = PROXY_UNIMPLEMENTED;
			} else {
				if (pw_http_find_header_value(
				    (char *)response_buf, header_bytes,
				    "Connection", &value, &value_len) &&
				    pw_http_value_has_token(value, value_len,
				    "close"))
					conn_close = 1;

				if (pw_http_find_header_value(
				    (char *)response_buf, header_bytes,
				    "Content-Length", &value, &value_len)) {
					if (pw_http_parse_size(value,
					    value_len, &content_length) != 0) {
						response_status =
						    PROXY_PARSE_FAILURE;
						goto response_done;
					}
					cl_found = 1;
				}
			}

			if (response_status != PROXY_INTERNAL)
				goto response_done;

			if (cl_found) {
				size_t body_have;
				size_t body_need;

				if (content_length >
				    PW_HTTP_MAX_RESPONSE - header_bytes) {
					response_status = PROXY_INTERNAL;
					goto response_done;
				}

				body_have = response_len - header_bytes;
				if (body_have > content_length) {
					response_len = header_bytes +
					    content_length;
					response_buf[response_len] = '\0';
					response_complete = 1;
					response_reusable = 0;
					goto response_done;
				}

				body_need = content_length - body_have;
				while (body_need > 0) {
					size_t room, to_read;

					room = PW_HTTP_MAX_RESPONSE -
					    response_len;
					if (room == 0)
						break;
					to_read = body_need < room ?
					    body_need : room;
					n = pw_http_read_some(fd,
					    response_buf + response_len,
					    to_read);
					if (n <= 0)
						break;
					response_len += (size_t)n;
					response_buf[response_len] = '\0';
					body_need -= (size_t)n;
				}

				if (body_need == 0) {
					response_complete = 1;
					response_reusable = http11 &&
					    !conn_close && ssrf_exempt;
				}
			} else if (response_len == header_bytes) {
				response_complete = 1;
				response_reusable = 0;
			} else {
				response_status = PROXY_PARSE_FAILURE;
			}
		} else {
			response_status = PROXY_PARSE_FAILURE;
		}
response_done:
		if (response_complete)
			response_status = PROXY_OK;
	}

	/* Release only fully framed HTTP/1.1 responses back to the pool. */
	if (pool_conn != NULL)
		vwasm_http_pool_release(http_pool, pool_conn,
		    response_reusable);
	else
		close(fd);

	/* Report success/failure to circuit breaker. */
	if (http_pool != NULL) {
		if (response_complete)
			vwasm_http_pool_cb_success(http_pool, host, port);
		else
			vwasm_http_pool_cb_failure(http_pool, host, port);
	}

	if (!response_complete || response_len == 0) {
		free(response_buf);
		results[0].of.i32 = response_status;
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

	/* Store response in VRBT tree keyed by token_id */
	{
		struct vwasm_http_call_entry *ent;
		uint32_t token_id;

		token_id = ++ctx->http_call_next_token;

		ent = calloc(1, sizeof(*ent));
		if (ent == NULL) {
			free(response_buf);
			results[0].of.i32 = PROXY_INTERNAL;
			return (NULL);
		}
		ent->token_id = token_id;
		ent->response.raw_buf = response_buf;
		ent->response.raw_len = response_len;
		ent->response.body = body_start;
		ent->response.body_len = resp_body_len;
		ent->response.num_headers = resp_num_headers;
		ent->response.valid = 1;

		vwasm_http_call_tree_VRBT_INSERT(&ctx->http_calls, ent);

		/* Write token ID back to the module */
		if (pw_write_u32(ctx, (uint32_t)args[9].of.i32,
		    token_id) != 0) {
			vwasm_http_call_tree_VRBT_REMOVE(&ctx->http_calls,
			    ent);
			free(response_buf);
			free(ent);
			results[0].of.i32 = PROXY_INTERNAL;
			return (NULL);
		}
	}

	results[0].of.i32 = PROXY_OK;
	return (NULL);
}
