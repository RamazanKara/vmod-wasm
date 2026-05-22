/// Transform filter module — demonstrates response header manipulation
/// using the raw Proxy-Wasm ABI host functions.
///
/// This module adds an `X-Transform: applied` response header to every
/// response that passes through it, demonstrating direct host-function
/// interaction without the proxy-wasm SDK.

// Import the host function for setting header map values.
// map_type 3 = ResponseHeaders in the Proxy-Wasm ABI.
extern "C" {
    fn proxy_set_header_map_pairs(map_type: i32, data_ptr: i32, data_size: i32) -> i32;
    fn proxy_get_header_map_pairs(map_type: i32, data_ptr: i32, data_size: i32) -> i32;
    fn proxy_add_header_map_value(
        map_type: i32,
        key_ptr: i32,
        key_len: i32,
        value_ptr: i32,
        value_len: i32,
    ) -> i32;
}

const MAP_TYPE_RESPONSE_HEADERS: i32 = 3;

#[no_mangle]
pub extern "C" fn proxy_on_memory_allocate(size: i32) -> i32 {
    let layout = unsafe { std::alloc::Layout::from_size_align_unchecked(size as usize, 1) };
    unsafe { std::alloc::alloc(layout) as i32 }
}

#[no_mangle]
pub extern "C" fn proxy_on_context_create(_context_id: i32, _root_context_id: i32) {}

#[no_mangle]
pub extern "C" fn proxy_on_context_finalize(_context_id: i32) -> i32 {
    0
}

#[no_mangle]
pub extern "C" fn proxy_on_request_headers(
    _context_id: i32,
    _num_headers: i32,
    _end_of_stream: i32,
) -> i32 {
    0 // Action::Continue
}

#[no_mangle]
pub extern "C" fn proxy_on_request_body(
    _context_id: i32,
    _body_size: i32,
    _end_of_stream: i32,
) -> i32 {
    0 // Action::Continue
}

#[no_mangle]
pub extern "C" fn proxy_on_response_headers(
    _context_id: i32,
    _num_headers: i32,
    _end_of_stream: i32,
) -> i32 {
    let key = b"X-Transform";
    let value = b"applied";

    unsafe {
        proxy_add_header_map_value(
            MAP_TYPE_RESPONSE_HEADERS,
            key.as_ptr() as i32,
            key.len() as i32,
            value.as_ptr() as i32,
            value.len() as i32,
        );
    }

    0 // Action::Continue
}

#[no_mangle]
pub extern "C" fn proxy_on_response_body(
    _context_id: i32,
    _body_size: i32,
    _end_of_stream: i32,
) -> i32 {
    0 // Action::Continue
}
