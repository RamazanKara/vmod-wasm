use proxy_wasm::traits::*;
use proxy_wasm::types::*;

proxy_wasm::main! {{
    proxy_wasm::set_log_level(LogLevel::Info);
    proxy_wasm::set_root_context(|_| -> Box<dyn RootContext> {
        Box::new(FilterRoot)
    });
}}

struct FilterRoot;

impl Context for FilterRoot {}

impl RootContext for FilterRoot {
    fn get_type(&self) -> Option<ContextType> {
        Some(ContextType::HttpContext)
    }

    fn create_http_context(&self, _context_id: u32) -> Option<Box<dyn HttpContext>> {
        Some(Box::new(FilterHttp))
    }
}

struct FilterHttp;

impl Context for FilterHttp {
    fn on_http_call_response(
        &mut self,
        _token_id: u32,
        _num_headers: usize,
        body_size: usize,
        _num_trailers: usize,
    ) {
        proxy_wasm::hostcalls::log(
            LogLevel::Info,
            &format!("sdk-filter: http call response body_size={}", body_size),
        )
        .ok();
    }
}

impl HttpContext for FilterHttp {
    fn on_http_request_headers(&mut self, _num_headers: usize, _end_of_stream: bool) -> Action {
        // Log processing
        proxy_wasm::hostcalls::log(LogLevel::Info, "sdk-filter: processing request").ok();

        // Check User-Agent for BadBot
        if let Some(ua) = self.get_http_request_header("User-Agent") {
            if ua.contains("BadBot") {
                self.send_http_response(403, vec![], Some(b"Blocked by SDK filter"));
                return Action::Pause;
            }
        }

        // Add a marker header to the request
        self.set_http_request_header("X-Wasm-SDK", Some("request-processed"));
        Action::Continue
    }

    fn on_http_response_headers(&mut self, _num_headers: usize, _end_of_stream: bool) -> Action {
        // Log processing
        proxy_wasm::hostcalls::log(LogLevel::Info, "sdk-filter: processing response").ok();

        // Add response header
        self.set_http_response_header("X-Wasm-SDK-Response", Some("processed"));
        Action::Continue
    }

    fn on_http_response_body(&mut self, body_size: usize, _end_of_stream: bool) -> Action {
        // Log body size (VDP delivers body after headers are sent, so we log instead)
        proxy_wasm::hostcalls::log(
            LogLevel::Info,
            &format!("sdk-filter: response body size={}", body_size),
        )
        .ok();
        Action::Continue
    }
}
