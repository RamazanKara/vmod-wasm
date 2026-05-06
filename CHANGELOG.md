# Changelog

All notable changes to vmod-wasm will be documented in this file.

## [3.0.0] - 2026-05-06

### Added
- **Proxy-Wasm ABI completeness**: Implemented `proxy_get_header_map_size`,
  `proxy_get_buffer_status`, and `proxy_get_log_level` host functions.
- **WASI stubs**: Modules compiled with `wasm32-wasi` target now instantiate
  without error. Provides stubs for `fd_write`, `clock_time_get`, `random_get`,
  `environ_sizes_get`, `environ_get`, `args_sizes_get`, `args_get`, and
  `proc_exit` (traps instead of exiting).
- Stream control functions (`proxy_continue_stream`, `proxy_close_stream`)
  registered and documented.

### Changed
- Removed internal scoping block in HTTP callout code (variables moved to
  function scope for clarity).
- Documentation overhaul: removed internal implementation plan, rewrote
  README as user guide, updated COMPATIBILITY matrix, replaced all "fuel"
  references with epoch-based terminology.

### Fixed
- Copyright year corrected to 2025 across all source files.

## [2.3.0] - 2026-05-05

### Changed
- **Streaming VDP body processing**: Response body is no longer buffered.
  Each chunk is passed to `proxy_on_response_body` immediately as it arrives
  (with `end_of_stream=0`), then forwarded to the client.  On the final chunk,
  `end_of_stream=1` is set.  This eliminates the 1 MiB buffer entirely —
  memory usage for body inspection is now O(chunk_size) instead of O(body_size).
- **Epoch-based interruption**: Replaced fuel-based instruction counting with
  epoch-based timeout protection.  A background timer thread increments the
  engine epoch every 1ms.  Each callback gets a configurable deadline (default
  100ms).  This reduces per-instruction overhead significantly while still
  preventing infinite loops.

### Added
- `wasm.set_epoch_deadline(ms)`: Configure the per-callback execution timeout
  in milliseconds (default: 100ms).
- `wasm.set_http_timeout(ms)`: Configure the HTTP callout timeout for
  `proxy_http_call` (default: 5000ms, cap: 30s).  Previously hardcoded.
- `vwasm_engine_reset_epoch_deadline()`: Internal API for extending the epoch
  deadline before each wasm callback phase.

### Fixed
- HTTP timeout now respects module-supplied timeout (if non-zero), falls back
  to engine-configured default, with a hard cap at 30 seconds.

## [2.2.0] - 2026-05-05

### Added
- **Response body access via VDP**: `proxy_on_response_body` now receives
  actual response body data via Varnish Delivery Processor pipeline. Body
  is buffered (up to 1 MiB) and passed to the callback on stream end.
  Activate with `set resp.filters += "wasm_body"` in `vcl_deliver`.
- **HTTP call response callback**: `proxy_on_http_call_response` is now
  invoked after `proxy_http_call` completes, matching the Proxy-Wasm ABI
  spec's async callback pattern. Previously modules had to read response
  data directly via `proxy_get_buffer_bytes`.

### Fixed
- `proxy_log` level mapping: `LogLevel::Info` no longer incorrectly gets
  "WARN:" prefix (threshold changed from DEBUG to INFO).
- Fuel exhaustion in VDP/HTTP callbacks: fuel is now refilled before each
  body phase, lifecycle callback, and HTTP call response callback.

## [2.0.0] - 2026-05-05

### Added
- **Anti-IP-rebinding**: `proxy_http_call` now validates resolved IPs against
  RFC1918, RFC5735, RFC4193, and loopback ranges to prevent SSRF attacks via
  DNS rebinding.
- **proxy_set_property**: Wasm modules can now mutate request properties:
  - `request.path` / `request.url_path` — URL rewriting
  - `request.method` — HTTP method change
  - `request.host` — Host header modification
- **Request body access**: `proxy_on_request_body` now receives actual body
  data (up to 1 MiB, automatically cached via VRT_CacheReqBody).
- **Execution statistics**: New `wasm.get_stats_json()` VCL function returns
  atomic counters: calls_total, calls_ok, calls_error, calls_timeout,
  local_responses, http_calls, http_calls_blocked, body_bytes_in, fuel_total.
- **Execution timing**: Logs VSL warning when Wasm execution exceeds 10ms.
- **Structured logging**: All proxy_log output prefixed with `[wasm:<module>]`.

### Changed
- Merged 4 duplicated lifecycle functions into single generic
  `proxy_wasm_execute()` (saved ~650 lines).
- Consolidated 11 stub functions into 2 generic stubs (`pw_stub_ok`,
  `pw_stub_not_found`).
- Removed all `pthread_rwlock` — config is immutable after `vcl_init`.
- Trimmed VCC documentation from 270 to 117 lines.
- Consolidated test suite from 18 to 16 test files.
- Updated COMPATIBILITY.md to reflect actual implementation coverage.

### Fixed
- `proxy_set_header_map_pairs` was documented as stub but was actually
  implemented — fixed documentation.

## [1.0.1] - 2026-04-29

### Changed
- Use `AN()` and `CHECK_OBJ_NOTNULL()` per Varnish idioms for all context pointer assertions
- Add `$Restrict` directives to VCC for compile-time VCL method enforcement:
  - `load()`, `set_fuel()`, `set_memory_limit()`: restricted to `vcl_init`
  - `execute()`, `proxy_wasm_on_request()`: restricted to `client` scope

## [1.0.0] - 2026-04-29

First stable release.

### Changed
- Upgraded to Varnish 8.0 and Wasmtime 44
- Fixed `hdr_t` compatibility for Varnish 8 API

## [0.1.0] - 2026-04-29

### Added

#### Wasm Execution
- `wasm.load(name, path)` — Load `.wasm` modules at VCL init
- `wasm.execute(module, func)` — Call exported Wasm functions from VCL
- `wasm.version()` — Return VMOD version string
- Wasmtime C API integration with engine/linker/store lifecycle
- Thread-safe module registry (read-write lock, up to 64 modules)

#### Host Functions
- 6 host functions under `env` namespace:
  - `get_request_header` — Read any request header
  - `get_request_url` — Read the request URL
  - `get_request_method` — Read the HTTP method
  - `get_client_ip` — Read the client IP address
  - `set_response_header` — Set a response header
  - `log_msg` — Log messages to Varnish Shared Log (VSL)

#### Execution Safety
- `wasm.set_fuel(fuel)` — Configurable fuel (instruction) limits
- `wasm.set_memory_limit(bytes)` — Configurable memory limits
- `wasm.get_fuel()` / `wasm.get_memory_limit()` — Query current limits
- Trap message extraction and logging to VSL

#### Proxy-Wasm ABI
- `wasm.proxy_wasm_on_request(module)` — Execute Proxy-Wasm filter lifecycle
- 10 Proxy-Wasm host functions:
  - `proxy_log` — Log with Proxy-Wasm log levels
  - `proxy_get_header_map_value` — Read headers by name
  - `proxy_add_header_map_value` — Add/set headers
  - `proxy_replace_header_map_value` — Replace header values
  - `proxy_remove_header_map_value` — Remove headers
  - `proxy_get_property` — Read request properties (path, method, protocol)
  - `proxy_send_local_response` — Send immediate responses (e.g. 403)
  - `proxy_get_current_time_nanoseconds` — Current time
  - `proxy_set_effective_context` — Context switching (stub)
  - `proxy_get_buffer_bytes` — Buffer access (stub)
- Full Proxy-Wasm lifecycle: context_create → vm_start → configure → request_headers
- Memory allocator protocol via `proxy_on_memory_allocate` export

#### Infrastructure
- Dockerfile with Debian bookworm-slim, Varnish 8.0, Wasmtime 44, Rust
- GitHub Actions CI pipeline
- autotools build system (automake/autoconf/libtool)
- 12 VTC tests
