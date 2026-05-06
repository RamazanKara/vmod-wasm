/*-
 * Copyright (c) 2024 OTTO GmbH & Co KG
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Portability shims for functions not available on all platforms.
 */

#ifndef VWASM_COMPAT_H
#define VWASM_COMPAT_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

/*
 * strlcpy(3) — Copy string with guaranteed NUL termination.
 * Available natively on BSD/macOS but not glibc < 2.38.
 */
#ifndef HAVE_STRLCPY
static inline size_t
strlcpy(char *dst, const char *src, size_t dstsize)
{
	size_t srclen;

	srclen = strlen(src);
	if (dstsize > 0) {
		size_t copylen = (srclen >= dstsize) ? dstsize - 1 : srclen;
		memcpy(dst, src, copylen);
		dst[copylen] = '\0';
	}
	return (srclen);
}
#endif /* !HAVE_STRLCPY */

#endif /* VWASM_COMPAT_H */
