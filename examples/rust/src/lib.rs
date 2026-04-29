/// Test Wasm module for vmod-wasm.
///
/// Phase 1: Simple exported functions (no host calls)
/// Phase 2: Functions that call host functions to inspect requests

// --- Host function imports (provided by vmod-wasm) ---

extern "C" {
    fn get_request_header(
        name_ptr: *const u8,
        name_len: i32,
        buf_ptr: *mut u8,
        buf_len: i32,
    ) -> i32;

    fn get_request_url(buf_ptr: *mut u8, buf_len: i32) -> i32;

    fn get_request_method(buf_ptr: *mut u8, buf_len: i32) -> i32;

    fn get_client_ip(buf_ptr: *mut u8, buf_len: i32) -> i32;

    fn set_response_header(
        name_ptr: *const u8,
        name_len: i32,
        val_ptr: *const u8,
        val_len: i32,
    ) -> i32;

    fn log_msg(level: i32, msg_ptr: *const u8, msg_len: i32);
}

// --- Phase 1: Simple functions (still needed for backward compat) ---

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

// --- Phase 2: Functions using host calls ---

/// Read the User-Agent header and set it as X-Wasm-UA response header.
/// Returns the length of the User-Agent string, or -1 on error.
#[no_mangle]
pub extern "C" fn echo_user_agent() -> i32 {
    let name = b"User-Agent";
    let mut buf = [0u8; 512];

    let len = unsafe {
        get_request_header(
            name.as_ptr(),
            name.len() as i32,
            buf.as_mut_ptr(),
            buf.len() as i32,
        )
    };

    if len > 0 {
        let actual_len = if len < buf.len() as i32 { len } else { buf.len() as i32 };
        let resp_name = b"X-Wasm-UA";
        unsafe {
            set_response_header(
                resp_name.as_ptr(),
                resp_name.len() as i32,
                buf.as_ptr(),
                actual_len,
            );
        }
    }

    len
}

/// Read the request URL and set it as X-Wasm-URL response header.
#[no_mangle]
pub extern "C" fn echo_url() -> i32 {
    let mut buf = [0u8; 2048];

    let len = unsafe { get_request_url(buf.as_mut_ptr(), buf.len() as i32) };

    if len > 0 {
        let actual_len = if len < buf.len() as i32 { len } else { buf.len() as i32 };
        let resp_name = b"X-Wasm-URL";
        unsafe {
            set_response_header(
                resp_name.as_ptr(),
                resp_name.len() as i32,
                buf.as_ptr(),
                actual_len,
            );
        }
    }

    len
}

/// Read the request method and set it as X-Wasm-Method response header.
#[no_mangle]
pub extern "C" fn echo_method() -> i32 {
    let mut buf = [0u8; 32];

    let len = unsafe { get_request_method(buf.as_mut_ptr(), buf.len() as i32) };

    if len > 0 {
        let actual_len = if len < buf.len() as i32 { len } else { buf.len() as i32 };
        let resp_name = b"X-Wasm-Method";
        unsafe {
            set_response_header(
                resp_name.as_ptr(),
                resp_name.len() as i32,
                buf.as_ptr(),
                actual_len,
            );
        }
    }

    len
}

/// Read the client IP and set it as X-Wasm-ClientIP response header.
#[no_mangle]
pub extern "C" fn echo_client_ip() -> i32 {
    let mut buf = [0u8; 64];

    let len = unsafe { get_client_ip(buf.as_mut_ptr(), buf.len() as i32) };

    if len > 0 {
        let actual_len = if len < buf.len() as i32 { len } else { buf.len() as i32 };
        let resp_name = b"X-Wasm-ClientIP";
        unsafe {
            set_response_header(
                resp_name.as_ptr(),
                resp_name.len() as i32,
                buf.as_ptr(),
                actual_len,
            );
        }
    }

    len
}

/// Log a test message to VSL at info level.
#[no_mangle]
pub extern "C" fn test_logging() -> i32 {
    let msg = b"hello from wasm module";
    unsafe {
        log_msg(1, msg.as_ptr(), msg.len() as i32);
    }
    0
}

/// Block requests with User-Agent containing "BadBot".
/// Returns 403 to block, 0 to allow.
#[no_mangle]
pub extern "C" fn block_bad_bot() -> i32 {
    let name = b"User-Agent";
    let mut buf = [0u8; 512];

    let len = unsafe {
        get_request_header(
            name.as_ptr(),
            name.len() as i32,
            buf.as_mut_ptr(),
            buf.len() as i32,
        )
    };

    if len <= 0 {
        return 0; // No User-Agent, allow
    }

    let actual_len = if len < buf.len() as i32 { len as usize } else { buf.len() };

    // Simple substring search for "BadBot"
    let ua = &buf[..actual_len];
    let needle = b"BadBot";
    for i in 0..ua.len() {
        if ua.len() - i >= needle.len() && &ua[i..i + needle.len()] == needle {
            let log = b"Blocked BadBot user agent";
            unsafe { log_msg(2, log.as_ptr(), log.len() as i32) };
            return 403;
        }
    }

    0
}
