/*-
 * Copyright (c) 2026 Ramazan Kara
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Wasm memory helpers — shared between proxy_wasm_*.c files.
 * Included as static inline for zero-cost abstraction.
 */

#ifndef PROXY_WASM_MEM_H
#define PROXY_WASM_MEM_H

#include <string.h>
#include <wasmtime.h>
#include "proxy_wasm.h"

static inline uint8_t *
pw_mem_base(const struct vwasm_proxy_ctx *ctx, size_t *size)
{
	(void)size;
	if (!ctx->memory_valid || ctx->wasm_ctx == NULL)
		return (NULL);
	return (wasmtime_memory_data(ctx->wasm_ctx, &ctx->memory));
}

static inline int
pw_validate_region(const struct vwasm_proxy_ctx *ctx,
    uint32_t offset, uint32_t len)
{
	size_t mem_size;
	uint8_t *base;

	base = pw_mem_base(ctx, &mem_size);
	if (base == NULL)
		return (0);
	mem_size = wasmtime_memory_data_size(ctx->wasm_ctx, &ctx->memory);
	if ((uint64_t)offset + len > mem_size)
		return (0);
	return (1);
}

static inline uint8_t *
pw_mem_ptr(const struct vwasm_proxy_ctx *ctx, uint32_t offset)
{
	size_t dummy;
	uint8_t *base;

	base = pw_mem_base(ctx, &dummy);
	if (base == NULL)
		return (NULL);
	return (base + offset);
}

static inline int
pw_read_string(const struct vwasm_proxy_ctx *ctx,
    uint32_t ptr, uint32_t len, char *buf, size_t bufsz)
{
	uint8_t *src;

	if (len == 0) {
		buf[0] = '\0';
		return (0);
	}
	if (len >= bufsz)
		return (-1);
	if (!pw_validate_region(ctx, ptr, len))
		return (-1);
	src = pw_mem_ptr(ctx, ptr);
	if (src == NULL)
		return (-1);
	memcpy(buf, src, len);
	buf[len] = '\0';
	return (0);
}

static inline int
pw_alloc_wasm(struct vwasm_proxy_ctx *ctx, uint32_t size, uint32_t *ret_ptr)
{
	wasmtime_val_t args[1], results[1];
	wasmtime_error_t *error;
	wasm_trap_t *trap = NULL;

	if (!ctx->allocator_valid)
		return (-1);

	args[0].kind = WASMTIME_I32;
	args[0].of.i32 = (int32_t)size;

	error = wasmtime_func_call(ctx->wasm_ctx, &ctx->allocator,
	    args, 1, results, 1, &trap);
	if (error != NULL) {
		wasmtime_error_delete(error);
		return (-1);
	}
	if (trap != NULL) {
		wasm_trap_delete(trap);
		return (-1);
	}

	*ret_ptr = (uint32_t)results[0].of.i32;
	return (0);
}

static inline int
pw_write_u32(const struct vwasm_proxy_ctx *ctx, uint32_t offset, uint32_t value)
{
	uint8_t *dst;

	if (!pw_validate_region(ctx, offset, 4))
		return (-1);
	dst = pw_mem_ptr(ctx, offset);
	if (dst == NULL)
		return (-1);
	memcpy(dst, &value, 4);
	return (0);
}

static inline int
pw_return_bytes(struct vwasm_proxy_ctx *ctx,
    const void *data, size_t len,
    uint32_t ret_data_offset, uint32_t ret_size_offset)
{
	uint32_t wasm_ptr;

	if (data == NULL || len == 0) {
		if (pw_write_u32(ctx, ret_data_offset, 0) != 0)
			return (-1);
		if (pw_write_u32(ctx, ret_size_offset, 0) != 0)
			return (-1);
		return (0);
	}

	if (pw_alloc_wasm(ctx, (uint32_t)len, &wasm_ptr) != 0)
		return (-1);

	if (!pw_validate_region(ctx, wasm_ptr, (uint32_t)len))
		return (-1);

	memcpy(pw_mem_ptr(ctx, wasm_ptr), data, len);

	if (pw_write_u32(ctx, ret_data_offset, wasm_ptr) != 0)
		return (-1);
	if (pw_write_u32(ctx, ret_size_offset, (uint32_t)len) != 0)
		return (-1);

	return (0);
}

static inline int
pw_return_string(struct vwasm_proxy_ctx *ctx,
    const char *str, size_t len,
    uint32_t ret_data_offset, uint32_t ret_size_offset)
{
	return (pw_return_bytes(ctx, str, len,
	    ret_data_offset, ret_size_offset));
}

#endif /* PROXY_WASM_MEM_H */
