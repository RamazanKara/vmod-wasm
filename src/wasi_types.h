/*-
 * WASI types and error codes for wasi_snapshot_preview1.
 *
 * Reference: https://github.com/WebAssembly/WASI/blob/main/legacy/preview1/docs.md
 */

#ifndef VMOD_WASM_WASI_TYPES_H
#define VMOD_WASM_WASI_TYPES_H

typedef enum {
	WASI_ERRNO_SUCCESS	= 0,
	WASI_ERRNO_BADF		= 8,
	WASI_ERRNO_INVAL	= 28,
	WASI_ERRNO_IO		= 29,
} wasi_errno_t;

typedef enum {
	WASI_CLOCKID_REALTIME	= 0,
	WASI_CLOCKID_MONOTONIC	= 1,
} wasi_clockid_t;

#endif /* VMOD_WASM_WASI_TYPES_H */
