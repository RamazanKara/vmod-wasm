# Implementation Plan: Refactor & Remaining Work

**Goal**: Remove bloat, apply KISS, complete remaining features.  
**Target**: ~35% code reduction without losing functionality.

---

## Phase 1: Merge Lifecycle Functions (wasm_engine.c)

**Problem**: 4 functions × ~400 lines each = 1600 lines of 99% duplicated code.  
**Fix**: One generic function with phase enum.

### Tasks
- [ ] 1.1: Define `enum vwasm_phase { VWASM_PHASE_REQUEST, VWASM_PHASE_RESPONSE }`
- [ ] 1.2: Create `vwasm_proxy_wasm_execute(engine, module, ctx, phase, vm_config, plugin_config)`
- [ ] 1.3: Replace 4 exported functions with thin wrappers that call the generic one
- [ ] 1.4: Build + test (18 tests must pass)

**Lines saved**: ~1000

---

## Phase 2: Consolidate Stubs (proxy_wasm.c)

**Problem**: 11 stub functions that do nothing, each 7-8 lines.  
**Fix**: Two generic stubs, registered by name for all no-ops.

### Tasks
- [ ] 2.1: Create `pw_stub_ok()` (returns PROXY_OK for all no-op stubs)
- [ ] 2.2: Create `pw_stub_not_found()` (returns NOT_FOUND)
- [ ] 2.3: Replace individual stubs with registrations to generic versions
- [ ] 2.4: Build + test

**Lines saved**: ~60

---

## Phase 3: Remove Config Locks (wasm_engine.c)

**Problem**: 8 setter/getter functions each wrap a single assignment in a rwlock.  
VCL init is single-threaded — no concurrent writes possible.  
**Fix**: Direct struct assignment, no locks, fewer functions.

### Tasks
- [ ] 3.1: Remove rwlock from setter functions (keep rwlock for runtime reads if needed)
- [ ] 3.2: Collapse `set_fail_mode` string parsing into the VMOD layer
- [ ] 3.3: Build + test

**Lines saved**: ~80

---

## Phase 4: Trim VCC Documentation (vmod_wasm.vcc)

**Problem**: 310 lines of VCC, most is repeated examples.  
**Fix**: One-line description + one minimal example per function.

### Tasks
- [ ] 4.1: Cut each function doc to: description (1-2 lines) + args + one example
- [ ] 4.2: Remove redundant "Returns:" and "Arguments:" for obvious functions

**Lines saved**: ~100

---

## Phase 5: Consolidate Tests

**Problem**: 18 test files with ~40% overlap.  
**Fix**: Merge overlapping tests into fewer, focused files.

### Tasks
- [ ] 5.1: Merge `proxy_wasm_basic.vtc` + `proxy_wasm_configured.vtc` into one
- [ ] 5.2: Merge `proxy_wasm_security.vtc` + `proxy_wasm_local_response.vtc` (both test blocking)
- [ ] 5.3: Keep `proxy_wasm_metrics.vtc` (unique coverage)
- [ ] 5.4: All tests must still pass (14+ tests)

---

## Phase 6: Add Execution Timing (proxy_wasm.c / wasm_engine.c)

**Problem**: No visibility into how long Wasm execution takes.  
**Fix**: Measure wall-clock time per execution, log if > threshold.

### Tasks
- [ ] 6.1: Add `struct timespec` before/after Wasm call in the lifecycle function
- [ ] 6.2: Log elapsed time to VSL if > 10ms (configurable)
- [ ] 6.3: Build + test

**Lines added**: ~20

---

## Phase 7: Structured Logging (proxy_wasm.c)

**Problem**: `proxy_log` just writes raw message, no module context.  
**Fix**: Prefix log with `[wasm:<module>]` for easy filtering.

### Tasks
- [ ] 7.1: Store module name in proxy_ctx
- [ ] 7.2: Prefix `proxy_log` output with `[wasm:<name>] <message>`
- [ ] 7.3: Build + test

**Lines added**: ~10

---

## Phase 8: Documentation Cleanup

**Problem**: Docs are padded with obvious information.  
**Fix**: Trim to essential content only.

### Tasks
- [ ] 8.1: PRODUCTION.md — cut to essentials (config reference + checklist)
- [ ] 8.2: SECURITY.md — merge into PRODUCTION.md as a "Security" section
- [ ] 8.3: COMPATIBILITY.md — keep as-is (it's the ABI reference)
- [ ] 8.4: Update README.md with concise production example

---

## Summary

| Phase | Focus | Lines Δ |
|-------|-------|---------|
| 1 | Merge lifecycle functions | -1000 |
| 2 | Consolidate stubs | -60 |
| 3 | Remove config locks | -80 |
| 4 | Trim VCC docs | -100 |
| 5 | Consolidate tests | -200 |
| 6 | Execution timing | +20 |
| 7 | Structured logging | +10 |
| 8 | Doc cleanup | -150 |
| **Total** | | **~-1560 lines** |

**Build/test after each phase**: `docker build -t vmod-wasm-dev . && docker run --rm vmod-wasm-dev make check`
