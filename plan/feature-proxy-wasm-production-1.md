---
goal: Complete Proxy-Wasm ABI v0.2.1 Compliance and Production Readiness for vmod-wasm
version: 1.0
date_created: 2026-05-05
last_updated: 2026-05-05
owner: Ramazan Kara
status: 'Complete'
tags: [feature, security, reliability, testing]
---

# Introduction

![Status: Complete](https://img.shields.io/badge/status-Complete-green)

This plan brings the vmod-wasm Proxy-Wasm host implementation to production readiness. It addresses critical bugs, security hardening, ABI compliance gaps, and testing coverage required for a safe production deployment.

## 1. Requirements & Constraints

- **REQ-001**: Fix the `proxy_http_call` use-after-free bug (response_buf freed before module reads it)
- **REQ-002**: Fix `proxy_send_local_response` to store response body and headers (not just status code)
- **REQ-003**: Fix `proxy_record_metric` to accept i64 value per ABI spec
- **REQ-004**: Add SSRF protection for `proxy_http_call` via upstream allowlist
- **REQ-005**: Add fail-open/fail-closed configuration mode
- **REQ-006**: Support `proxy_get_buffer_bytes` for HTTP_REQUEST_BODY and HTTP_RESPONSE_BODY
- **REQ-007**: Support pseudo-headers (`:method`, `:path`, `:authority`, `:status`) in header map pairs
- **REQ-008**: Add per-module HTTP callout rate limiting
- **REQ-009**: Expose metrics to Varnish counters (VSC)
- **REQ-010**: Add test coverage for metrics, shared data, queues, HTTP call, body, and configured variants
- **SEC-001**: Restrict HTTP callout destinations to configured upstream allowlist
- **SEC-002**: Cap maximum HTTP callouts per request (prevent amplification)
- **SEC-003**: Validate HTTP response headers before storing (prevent header injection)
- **CON-001**: Must not break existing 14 VTC tests
- **CON-002**: Synchronous HTTP call model retained (async requires Varnish core changes)
- **CON-003**: Varnish 8.0+ API compatibility required
- **CON-004**: Single-threaded request processing model (one Wasm instance per VCL call)
- **GUD-001**: All new code follows existing project style (BSD, tabs, Varnish macros)
- **GUD-002**: Each phase independently buildable and testable
- **PAT-001**: Use `pw_` prefix for all static proxy_wasm functions
- **PAT-002**: Use `VWASM_` prefix for all public constants and types

## 2. Implementation Steps

### Phase 1: Critical Bug Fixes

- GOAL-001: Fix memory safety bugs and ABI non-conformance that make the current implementation unsafe.

| Task | Description | Completed | Date |
|------|-------------|-----------|------|
| TASK-001 | Fix `proxy_http_call` use-after-free: keep `response_buf` alive in ctx until request completes; free in cleanup | | |
| TASK-002 | Fix `proxy_send_local_response` to capture body and headers from Wasm args | | |
| TASK-003 | Fix `proxy_record_metric` to use i64 value (combine two i32 args or use i64 directly) | | |
| TASK-004 | Add cleanup function to free http_response.buf after lifecycle completes | | |
| TASK-005 | Verify all 14 existing tests still pass | | |

### Phase 2: Security Hardening

- GOAL-002: Prevent SSRF, amplification attacks, and other security issues in the HTTP callout path.

| Task | Description | Completed | Date |
|------|-------------|-----------|------|
| TASK-006 | Add `upstream_allowlist` configuration to vmod (VCC + vmod_wasm.c) | | |
| TASK-007 | Add `vwasm_engine_set_allowed_upstreams()` API to wasm_engine | | |
| TASK-008 | Validate upstream host:port against allowlist in `pw_proxy_http_call` | | |
| TASK-009 | Add `max_http_calls_per_request` counter in proxy_ctx; reject after limit | | |
| TASK-010 | Validate resolved IP addresses against RFC1918/loopback (anti-rebinding) | | |
| TASK-011 | Add `fail_mode` configuration: "open" (continue on error) / "closed" (synth 500) | | |

### Phase 3: ABI Compliance — Body Buffers & Local Response

- GOAL-003: Support HTTP request/response body access and full send_local_response.

| Task | Description | Completed | Date |
|------|-------------|-----------|------|
| TASK-012 | Add `request_body` and `response_body` fields to `vwasm_proxy_ctx` | | |
| TASK-013 | Populate body fields from Varnish bereq/beresp body (VFP/VDP) or req body | | |
| TASK-014 | Implement `proxy_get_buffer_bytes` for HTTP_REQUEST_BODY, HTTP_RESPONSE_BODY | | |
| TASK-015 | Implement `proxy_set_buffer_bytes` for body mutation (store modified body in ctx) | | |
| TASK-016 | Enhance `proxy_send_local_response` to store body string + response headers | | |
| TASK-017 | Wire local_response body/headers into VCL synth in vmod_wasm.c | | |

### Phase 4: ABI Compliance — Header Pseudo-Headers & Properties

- GOAL-004: Complete header map and property path compliance.

| Task | Description | Completed | Date |
|------|-------------|-----------|------|
| TASK-018 | Add `:method`, `:path`, `:authority`, `:status` pseudo-headers to `get_header_map_pairs` | | |
| TASK-019 | Handle pseudo-header writes in `set_header_map_pairs` (set method/url/status) | | |
| TASK-020 | Expand `proxy_get_property` paths: `connection.id`, `source.address`, `source.port`, `destination.address`, `destination.port` | | |
| TASK-021 | Add `upstream.address`, `node.id` property paths from Varnish context | | |

### Phase 5: Observability & Metrics Exposition

- GOAL-005: Make Wasm-defined metrics visible to operators via Varnish statistics.

| Task | Description | Completed | Date |
|------|-------------|-----------|------|
| TASK-022 | Add `wasm.get_metrics_json()` VCL function to expose metric_store as JSON | | |
| TASK-023 | Add VSC (Varnish Statistics Counter) registration for wasm.* counters | | |
| TASK-024 | Log Wasm execution time per-request via VSL (SLT_Timestamp equivalent) | | |
| TASK-025 | Add structured log format: `wasm(module_name)[ctx_id]: message` | | |

### Phase 6: Test Coverage

- GOAL-006: Comprehensive VTC tests for all production features.

| Task | Description | Completed | Date |
|------|-------------|-----------|------|
| TASK-026 | Write test Wasm module: `test_metrics.wasm` — defines/increments/reads metrics | | |
| TASK-027 | Write test Wasm module: `test_shared_data.wasm` — get/set/CAS shared data | | |
| TASK-028 | Write test Wasm module: `test_http_call.wasm` — makes HTTP callout, reads response | | |
| TASK-029 | Write test Wasm module: `test_body.wasm` — reads/modifies request body | | |
| TASK-030 | Write test Wasm module: `test_local_response.wasm` — sends full local response with body | | |
| TASK-031 | Write VTC: `proxy_wasm_metrics.vtc` — tests metric lifecycle | | |
| TASK-032 | Write VTC: `proxy_wasm_http_call.vtc` — tests HTTP callout with mock backend | | |
| TASK-033 | Write VTC: `proxy_wasm_body.vtc` — tests body read/write | | |
| TASK-034 | Write VTC: `proxy_wasm_security.vtc` — tests SSRF rejection, rate limiting | | |
| TASK-035 | Write VTC: `proxy_wasm_local_response.vtc` — tests full synth with body/headers | | |
| TASK-036 | Write VTC: `proxy_wasm_configured.vtc` — tests _with_config variants | | |

### Phase 7: Documentation & Build

- GOAL-007: Production-ready documentation and CI.

| Task | Description | Completed | Date |
|------|-------------|-----------|------|
| TASK-037 | Write `docs/PRODUCTION.md` — deployment guide, configuration reference | | |
| TASK-038 | Write `docs/SECURITY.md` — threat model, trust boundaries, configuration | | |
| TASK-039 | Write `docs/COMPATIBILITY.md` — proxy-wasm SDK compatibility matrix | | |
| TASK-040 | Add GitHub Actions CI workflow (build + test in Docker) | | |
| TASK-041 | Update README.md with production configuration examples | | |

## 3. Alternatives

- **ALT-001**: Async HTTP callouts via Varnish VMOD_BE — rejected: requires Varnish core changes and backend fetch integration that's not available via VMOD API. Sync model with strict timeout + allowlist is pragmatic.
- **ALT-002**: Use Varnish VSC directly for metrics — considered but deferred to P2: VSC requires compile-time counter definitions. JSON exposition is more flexible for dynamic Wasm metrics.
- **ALT-003**: Body access via VFP/VDP (Varnish Fetch/Delivery Processors) — rejected for initial implementation: requires complex integration with Varnish's streaming body pipeline. Using req/beresp body pointers is simpler for Phase 3.

## 4. Dependencies

- **DEP-001**: Wasmtime C API v44.0.0+ (currently in use)
- **DEP-002**: Varnish 8.0+ with varnishapi pkg-config
- **DEP-003**: Test Wasm modules must be compiled from Rust/C proxy-wasm SDK (existing `test_module.wasm` pattern)
- **DEP-004**: Docker build environment (existing Dockerfile)

## 5. Files

- **FILE-001**: `src/proxy_wasm.c` — Main host function implementations (Phases 1-5)
- **FILE-002**: `src/proxy_wasm.h` — Type definitions (extend proxy_ctx struct)
- **FILE-003**: `src/wasm_engine.c` — Lifecycle execution, cleanup hooks
- **FILE-004**: `src/wasm_engine.h` — Engine API extensions
- **FILE-005**: `src/vmod_wasm.vcc` — VCL function declarations
- **FILE-006**: `src/vmod_wasm.c` — VMOD wrapper functions
- **FILE-007**: `tests/proxy_wasm_*.vtc` — New test files (Phase 6)
- **FILE-008**: `tests/wasm/test_*.wasm` — Test Wasm modules (Phase 6)
- **FILE-009**: `docs/*.md` — Documentation (Phase 7)
- **FILE-010**: `.github/workflows/ci.yml` — CI pipeline (Phase 7)

## 6. Testing

- **TEST-001**: All existing 14 VTC tests must pass after each phase
- **TEST-002**: `proxy_wasm_metrics.vtc` — define counter, increment, verify via get_metrics_json
- **TEST-003**: `proxy_wasm_http_call.vtc` — callout to mock backend, verify response read
- **TEST-004**: `proxy_wasm_body.vtc` — POST body read, body modification
- **TEST-005**: `proxy_wasm_security.vtc` — blocked upstream returns PROXY_BAD_ARGUMENT
- **TEST-006**: `proxy_wasm_local_response.vtc` — synth with body+headers returned to client
- **TEST-007**: `proxy_wasm_configured.vtc` — vm_config/plugin_config passed correctly
- **TEST-008**: Build + full test suite via `docker build && docker run --rm vmod-wasm-dev make check`

## 7. Risks & Assumptions

- **RISK-001**: Body access in Varnish VCL context may not be available for all request types (e.g., piped, uncacheable). Mitigation: return NOT_FOUND for unavailable bodies.
- **RISK-002**: HTTP callout rate limiting may break modules that legitimately need multiple calls. Mitigation: configurable limit (default=5, max=20).
- **RISK-003**: Pseudo-header mutation (`:method`, `:path`) modifies the request line which may have side effects in Varnish's cache key. Mitigation: document clearly.
- **ASSUMPTION-001**: Existing `test_module.wasm` exercises the basic proxy_wasm lifecycle and will continue working unchanged.
- **ASSUMPTION-002**: Varnish `http_req->hd[HTTP_HDR_METHOD]` etc. are writable from VMOD context in `vcl_recv`.
- **ASSUMPTION-003**: We can access request body via `VRT_r_req_body()` or equivalent in client context.

## 8. Related Specifications / Further Reading

- [Proxy-Wasm ABI Spec v0.2.1](https://github.com/proxy-wasm/spec/tree/master/abi-versions/vNext)
- [Proxy-Wasm Rust SDK](https://github.com/proxy-wasm/proxy-wasm-rust-sdk)
- [Wasmtime C API Documentation](https://docs.wasmtime.dev/c-api/)
- [Varnish VMOD Development Guide](https://varnish-cache.org/docs/trunk/reference/vmod.html)
- [OWASP SSRF Prevention Cheat Sheet](https://cheatsheetseries.owasp.org/cheatsheets/Server_Side_Request_Forgery_Prevention_Cheat_Sheet.html)
