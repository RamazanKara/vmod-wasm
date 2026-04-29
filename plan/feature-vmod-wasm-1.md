---
goal: Build a Varnish VMOD that executes WebAssembly modules for request/response processing with Proxy-Wasm ABI compatibility
version: 1.0
date_created: 2026-04-29
last_updated: 2026-04-29
owner: RamazanKara
status: 'In progress'
tags: [feature, architecture, varnish, wasm, proxy-wasm, open-source]
---

# Introduction

![Status: In progress](https://img.shields.io/badge/status-In%20progress-yellow)

vmod-wasm is a Varnish VMOD that embeds a WebAssembly runtime (Wasmtime) to execute Wasm modules during request processing. The end goal is full Proxy-Wasm ABI compatibility, allowing users to write edge logic in Rust, Go, or AssemblyScript and deploy it on Varnish without writing C or VCL.

This is the first open-source Wasm VMOD for Varnish — existing solutions are proprietary (Fastly Compute@Edge).

## 1. Requirements & Constraints

- **REQ-001**: Load and execute `.wasm` modules from disk at VCL init time
- **REQ-002**: Call exported Wasm functions from VCL (vcl_recv, vcl_hash, vcl_backend_response, vcl_deliver)
- **REQ-003**: Provide host functions that expose Varnish request context to Wasm modules
- **REQ-004**: Support Proxy-Wasm ABI v0.2.1 for ecosystem compatibility with Envoy/nginx filters
- **REQ-005**: Thread-safe execution — Varnish worker threads must not block each other
- **REQ-006**: Execution limits (fuel/gas) to prevent runaway Wasm modules from blocking requests
- **REQ-007**: Memory limits to prevent Wasm modules from exhausting host memory
- **SEC-001**: Wasm modules run in sandboxed linear memory — no access to host filesystem or network
- **SEC-002**: Host functions must validate all inputs from Wasm (pointer/length bounds checks)
- **SEC-003**: No arbitrary host function registration — only allowlisted functions exposed
- **CON-001**: Must build against Varnish 7.x (current stable OSS)
- **CON-002**: Must link against Wasmtime C API (stable ABI)
- **CON-003**: VMOD must be loadable via standard `import wasm;` in VCL
- **CON-004**: Zero external runtime dependencies beyond Wasmtime and Varnish
- **GUD-001**: Follow standard VMOD autotools build conventions
- **GUD-002**: Pre-compile Wasm modules at VCL load time, instantiate cheaply per-request
- **GUD-003**: Use Varnish PRIV_TASK for per-request Wasm instance state
- **PAT-001**: Instance pooling pattern — reuse Wasm instances across requests where safe
- **PAT-002**: Host function dispatch table — static registration at compile time

## 2. Implementation Steps

### Phase 1: Project Skeleton & Minimal Wasm Execution

- GOAL-001: Establish build system, load a Wasm module, call an exported function, return result to VCL

| Task | Description | Completed | Date |
|------|-------------|-----------|------|
| TASK-001 | Create autotools build system (configure.ac, Makefile.am, autogen.sh) | | |
| TASK-002 | Create VMOD interface definition (src/vmod_wasm.vcc) with initial functions: `load()`, `execute()`, `version()` | | |
| TASK-003 | Implement `vmod_wasm.c` — module loading via Wasmtime C API (`wasm_module_new`, `wasm_instance_new`) | | |
| TASK-004 | Implement `$Event` handler for LOAD/DISCARD lifecycle (create/destroy engine+store) | | |
| TASK-005 | Create test Wasm module in Rust (returns a constant, adds two integers) | | |
| TASK-006 | Write VTC (Varnish Test Case) for basic load + execute | | |
| TASK-007 | Create Dockerfile for reproducible build environment (Varnish + Wasmtime + build tools) | | |
| TASK-008 | Create CI workflow (GitHub Actions) — build + test | | |

### Phase 2: Host Functions & Request Context Bridge

- GOAL-002: Wasm modules can read request headers, URL, method, client IP and make decisions

| Task | Description | Completed | Date |
|------|-------------|-----------|------|
| TASK-009 | Design host function ABI: memory allocation protocol (Wasm exports `alloc`/`dealloc`) | | |
| TASK-010 | Implement string passing: host → Wasm (write to linear memory, pass ptr+len) | | |
| TASK-011 | Implement string passing: Wasm → host (read from linear memory with bounds check) | | |
| TASK-012 | Implement host function: `get_request_header(name_ptr, name_len) → (ptr, len)` | | |
| TASK-013 | Implement host function: `get_request_url() → (ptr, len)` | | |
| TASK-014 | Implement host function: `get_request_method() → (ptr, len)` | | |
| TASK-015 | Implement host function: `get_client_ip() → (ptr, len)` | | |
| TASK-016 | Implement host function: `set_response_header(name_ptr, name_len, val_ptr, val_len)` | | |
| TASK-017 | Implement host function: `log_msg(level, msg_ptr, msg_len)` — writes to VSL | | |
| TASK-018 | Wire host functions into Wasm instance via `wasm_func_new_with_env` | | |
| TASK-019 | Use PRIV_TASK to store per-request context (VRT_CTX pointer for host functions) | | |
| TASK-020 | Create Rust test module: block requests with specific User-Agent | | |
| TASK-021 | Write VTC tests for all host functions | | |

### Phase 3: Execution Safety & Instance Management

- GOAL-003: Production-safe execution with resource limits and efficient instance reuse

| Task | Description | Completed | Date |
|------|-------------|-----------|------|
| TASK-022 | Implement fuel-based execution limits (Wasmtime `wasmtime_store_set_fuel`) | | |
| TASK-023 | Implement linear memory limits (`wasmtime_config_max_wasm_stack`) | | |
| TASK-024 | Implement Wasm trap handling — convert traps to VCL errors (don't crash Varnish) | | |
| TASK-025 | Implement instance pooling: pre-create N instances from compiled module, checkout/return | | |
| TASK-026 | Add VCL function `wasm.set_fuel(INT)` — configurable per-module execution budget | | |
| TASK-027 | Add VCL function `wasm.set_memory_limit(BYTES)` — configurable memory cap | | |
| TASK-028 | Add execution metrics: fuel consumed, execution time, trap count → VSL tags | | |
| TASK-029 | Stress test: concurrent requests with varying Wasm workloads | | |
| TASK-030 | Write VTC tests for fuel exhaustion, memory limit, trap recovery | | |

### Phase 4: Proxy-Wasm ABI Compatibility

- GOAL-004: Support Proxy-Wasm ABI so existing Envoy/nginx Wasm filters work on Varnish

| Task | Description | Completed | Date |
|------|-------------|-----------|------|
| TASK-031 | Implement Proxy-Wasm L4/L7 lifecycle callbacks: `proxy_on_request_headers`, `proxy_on_response_headers` | | |
| TASK-032 | Implement `proxy_get_header_map_value` / `proxy_get_header_map_pairs` (request headers) | | |
| TASK-033 | Implement `proxy_set_header_map_pairs` / `proxy_add_header_map_value` | | |
| TASK-034 | Implement `proxy_get_property` (request.path, request.method, source.address, etc.) | | |
| TASK-035 | Implement `proxy_log` (map to VSL) | | |
| TASK-036 | Implement `proxy_send_local_response` (trigger synth in Varnish) | | |
| TASK-037 | Implement `proxy_get_buffer_bytes` / `proxy_set_buffer_bytes` for body access | | |
| TASK-038 | Map Varnish VCL lifecycle to Proxy-Wasm context IDs and stream model | | |
| TASK-039 | Create adapter layer: translate Varnish request model → Proxy-Wasm property model | | |
| TASK-040 | Test with existing Proxy-Wasm filters: basic-auth (Rust SDK), rate-limiting (Go SDK) | | |
| TASK-041 | Document supported/unsupported Proxy-Wasm ABI functions | | |

### Phase 5: Documentation, Packaging & Release

- GOAL-005: Production-ready release with docs, packages, and example modules

| Task | Description | Completed | Date |
|------|-------------|-----------|------|
| TASK-042 | Write README.md: quick start, build from source, VCL examples | | |
| TASK-043 | Write CONTRIBUTING.md: how to add host functions, how to build test modules | | |
| TASK-044 | Create example Wasm modules: WAF (block SQL injection), rate limiter, header rewriter | | |
| TASK-045 | Create Rust SDK crate (`vmod-wasm-sdk`) with helper macros for writing modules | | |
| TASK-046 | Package as .deb/.rpm for common distros (Debian 12, Ubuntu 24.04, RHEL 9) | | |
| TASK-047 | Create Helm chart / container image for Kubernetes deployments | | |
| TASK-048 | Performance benchmarks: baseline Varnish vs. Varnish + vmod-wasm (wrk2) | | |
| TASK-049 | Write architecture doc explaining memory model, threading, and instance lifecycle | | |
| TASK-050 | Tag v0.1.0 release | | |

## 3. Alternatives

- **ALT-001**: Use Wasmer instead of Wasmtime — rejected because Wasmtime has a more stable C API, is used by Fastly (Varnish ecosystem), and has better WASI support
- **ALT-002**: Use V8 (wasm execution via Chrome's engine) — rejected due to massive binary size, complex embedding, and non-standard C API
- **ALT-003**: Custom ABI instead of Proxy-Wasm — rejected because Proxy-Wasm gives immediate ecosystem access (existing filters from Envoy/nginx world)
- **ALT-004**: Write VMOD in Rust via FFI — rejected for now; autotools C is the standard VMOD pattern and ensures compatibility with Varnish build system

## 4. Dependencies

- **DEP-001**: Varnish Cache 7.x source/headers (`varnishapi-dev` / `varnish-devel`)
- **DEP-002**: Wasmtime C API v25+ (libwasmtime.a + wasmtime.h) — https://github.com/bytecodealliance/wasmtime/releases
- **DEP-003**: autotools (automake >= 1.16, autoconf >= 2.69, libtool >= 2.4)
- **DEP-004**: Python 3 (for vmodtool.py — ships with Varnish source)
- **DEP-005**: Rust toolchain (for building test/example Wasm modules only — not required at runtime)
- **DEP-006**: pkg-config (for detecting Varnish install paths)

## 5. Files

- **FILE-001**: `src/vmod_wasm.vcc` — VMOD interface definition (VCL-facing API)
- **FILE-002**: `src/vmod_wasm.c` — Core VMOD implementation (lifecycle, VCL function dispatch)
- **FILE-003**: `src/wasm_engine.c` — Wasmtime engine wrapper (compile, instantiate, execute)
- **FILE-004**: `src/wasm_engine.h` — Engine interface header
- **FILE-005**: `src/host_functions.c` — Host function implementations (request context bridge)
- **FILE-006**: `src/host_functions.h` — Host function registration table
- **FILE-007**: `src/instance_pool.c` — Thread-safe Wasm instance pool
- **FILE-008**: `src/instance_pool.h` — Pool interface
- **FILE-009**: `src/proxy_wasm.c` — Proxy-Wasm ABI adapter layer (Phase 4)
- **FILE-010**: `src/proxy_wasm.h` — Proxy-Wasm types and dispatch
- **FILE-011**: `configure.ac` — Autoconf configuration
- **FILE-012**: `Makefile.am` — Automake build rules
- **FILE-013**: `autogen.sh` — Bootstrap script
- **FILE-014**: `tests/*.vtc` — Varnish Test Cases
- **FILE-015**: `examples/rust/` — Example Wasm modules written in Rust
- **FILE-016**: `Dockerfile` — Reproducible build environment
- **FILE-017**: `.github/workflows/ci.yml` — CI pipeline

## 6. Testing

- **TEST-001**: VTC — Load a minimal Wasm module, call exported function, verify return value
- **TEST-002**: VTC — Call `wasm.execute()` with non-existent module → graceful VCL error
- **TEST-003**: VTC — Host function `get_request_header` returns correct value
- **TEST-004**: VTC — Host function `get_request_url` returns full URL
- **TEST-005**: VTC — Wasm module that blocks requests based on User-Agent header
- **TEST-006**: VTC — Fuel exhaustion → request continues (fail-open) with VSL log entry
- **TEST-007**: VTC — Memory limit exceeded → trap handled, request not crashed
- **TEST-008**: VTC — Concurrent requests (thread safety) — 100 parallel requests through Wasm
- **TEST-009**: VTC — VCL reload: old module unloaded, new module loaded cleanly
- **TEST-010**: VTC — Proxy-Wasm basic-auth filter blocks unauthorized requests
- **TEST-011**: Benchmark — wrk2 baseline vs. no-op Wasm module (measure overhead floor)
- **TEST-012**: Benchmark — wrk2 with header-inspection Wasm module (realistic workload)

## 7. Risks & Assumptions

- **RISK-001**: Wasmtime C API stability — API may change between major versions. Mitigation: pin to a specific Wasmtime release, abstract behind internal wrapper.
- **RISK-002**: Performance overhead of Wasm instantiation per-request. Mitigation: instance pooling (TASK-025), module pre-compilation.
- **RISK-003**: Proxy-Wasm ABI mismatch with Varnish model — Proxy-Wasm assumes Envoy's stream model. Mitigation: adapter layer that maps Varnish request lifecycle to Proxy-Wasm contexts.
- **RISK-004**: Request body access in Varnish is limited and requires `std.cache_req_body()`. Mitigation: document limitation, body inspection is opt-in.
- **RISK-005**: Large Wasm modules may have slow compilation at VCL load time. Mitigation: support serialized/pre-compiled modules (`wasmtime_module_serialize`).
- **ASSUMPTION-001**: Users will compile Wasm modules externally (Rust/Go toolchain) — the VMOD only loads `.wasm` files.
- **ASSUMPTION-002**: Target Varnish version is 7.4+ (latest OSS stable at time of writing).
- **ASSUMPTION-003**: Wasmtime provides sufficient WASI preview1 support for Proxy-Wasm filters that need basic I/O.

## 8. Related Specifications / Further Reading

- [Proxy-Wasm ABI Specification](https://github.com/proxy-wasm/spec)
- [Proxy-Wasm Rust SDK](https://github.com/proxy-wasm/proxy-wasm-rust-sdk)
- [Wasmtime C API Documentation](https://docs.wasmtime.dev/c-api/)
- [Varnish VMOD Development Guide](https://varnish-cache.org/docs/trunk/reference/vmod.html)
- [Varnish VCC/vmodtool Documentation](https://varnish-cache.org/docs/trunk/reference/vmod_std.html)
- [Fastly Compute@Edge Architecture](https://www.fastly.com/blog/compute-edge-architecture) (proprietary reference)
- [proxy-wasm-nginx](https://github.com/api7/wasm-nginx-module) (reference implementation for another proxy)
- [libvmod-modsecurity](https://github.com/AirisX/libvmod-modsecurity) (VMOD pattern reference)
