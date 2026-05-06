/// Transform filter module for testing.
/// All callbacks return 0 (Action::Continue) — no modification to request/response.

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
