/// Minimal test Wasm module for vmod-wasm Phase 1 testing.
///
/// Exports simple functions that the VMOD can call to verify
/// basic load + execute functionality.

/// Returns a constant (42) — simplest possible test.
#[no_mangle]
pub extern "C" fn get_constant() -> i32 {
    42
}

/// Adds two hardcoded numbers — verifies the runtime works.
#[no_mangle]
pub extern "C" fn add_numbers() -> i32 {
    17 + 25
}

/// Simulates an "allow" decision (return 0 = allow).
#[no_mangle]
pub extern "C" fn on_request_allow() -> i32 {
    0
}

/// Simulates a "block" decision (return 403 = block).
#[no_mangle]
pub extern "C" fn on_request_block() -> i32 {
    403
}
