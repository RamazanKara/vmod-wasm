# Changelog

All notable changes to vmod-wasm will be documented in this file.

## [0.1.0] - 2026-04-29

### Added

#### Phase 1 — Basic Wasm Execution
- `wasm.load(name, path)` — Load `.wasm` modules at VCL init
- `wasm.execute(module, func)` — Call exported Wasm functions from VCL
- `wasm.version()` — Return VMOD version string
- Wasmtime C API v25 integration with engine/linker/store lifecycle
- Thread-safe module registry (read-write lock, up to 64 modules)
- 4 VTC tests: basic_load, allow_decision, block_decision, error_handling

#### Phase 2 — Host Functions
- 6 host functions under `env` namespace:
  - `get_request_header` — Read any request header
  - `get_request_url` — Read the request URL
  - `get_request_method` — Read the HTTP method
  - `get_client_ip` — Read the client IP address
  - `set_response_header` — Set a response header
  - `log_msg` — Log messages to Varnish Shared Log (VSL)
- 3 VTC tests: host_header, host_url_method, host_block_bot

#### Phase 3 — Execution Safety
- `wasm.set_fuel(fuel)` — Configurable fuel (instruction) limits
- `wasm.set_memory_limit(bytes)` — Configurable memory limits
- `wasm.get_fuel()` / `wasm.get_memory_limit()` — Query current limits
- Trap message extraction and logging to VSL
- 3 VTC tests: fuel_exhaustion, memory_limit, resource_config

#### Phase 4 — Proxy-Wasm ABI Compatibility
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
- 2 VTC tests: proxy_wasm_basic, proxy_wasm_block

### Infrastructure
- Dockerfile with Debian bookworm-slim, Varnish 7.5, Wasmtime 25, Rust
- GitHub Actions CI pipeline
- autotools build system (automake/autoconf/libtool)
- Rust test module (wasm32-unknown-unknown) with functions for all phases
