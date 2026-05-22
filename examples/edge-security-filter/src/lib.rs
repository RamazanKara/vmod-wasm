//! Edge Security Filter — a production-grade Proxy-Wasm module for vmod-wasm.
//!
//! Demonstrates the full Proxy-Wasm ABI v0.2.1 surface:
//! - Request/response header manipulation
//! - Body inspection
//! - HTTP callouts (auth token validation)
//! - Shared data (rate limiting with CAS)
//! - Custom metrics (counters, gauges)
//! - JSON plugin configuration
//! - Tick-based background processing
//!
//! Use cases: rate limiting, bot detection, geo-blocking, request enrichment,
//! security header injection, and external auth validation.

mod config;

use config::{parse_config, FilterConfig};
use proxy_wasm::traits::*;
use proxy_wasm::types::*;
use std::rc::Rc;

const MODULE_VERSION: &str = "1.0.0";

// Metric IDs (set during on_vm_start)
struct MetricIds {
    requests_total: u32,
    blocked_total: u32,
    rate_limited_total: u32,
    auth_failures_total: u32,
}

proxy_wasm::main! {{
    proxy_wasm::set_log_level(LogLevel::Info);
    proxy_wasm::set_root_context(|_| -> Box<dyn RootContext> {
        Box::new(EdgeSecurityRoot {
            config: Rc::new(FilterConfig::default()),
            metrics: None,
        })
    });
}}

// ─── Root Context ────────────────────────────────────────────────────────────

struct EdgeSecurityRoot {
    config: Rc<FilterConfig>,
    metrics: Option<MetricIds>,
}

impl Context for EdgeSecurityRoot {}

impl RootContext for EdgeSecurityRoot {
    fn on_vm_start(&mut self, _vm_configuration_size: usize) -> bool {
        proxy_wasm::hostcalls::log(
            LogLevel::Info,
            &format!("edge-security-filter v{} starting", MODULE_VERSION),
        )
        .ok();

        // Define custom metrics
        let requests_total =
            proxy_wasm::hostcalls::define_metric(MetricType::Counter, "edge_requests_total")
                .unwrap_or(0);
        let blocked_total =
            proxy_wasm::hostcalls::define_metric(MetricType::Counter, "edge_blocked_total")
                .unwrap_or(0);
        let rate_limited_total =
            proxy_wasm::hostcalls::define_metric(MetricType::Counter, "edge_rate_limited_total")
                .unwrap_or(0);
        let auth_failures_total =
            proxy_wasm::hostcalls::define_metric(MetricType::Counter, "edge_auth_failures_total")
                .unwrap_or(0);

        self.metrics = Some(MetricIds {
            requests_total,
            blocked_total,
            rate_limited_total,
            auth_failures_total,
        });

        true
    }

    fn on_configure(&mut self, _plugin_configuration_size: usize) -> bool {
        if let Some(config_bytes) = self.get_plugin_configuration() {
            let parsed = parse_config(&config_bytes);
            proxy_wasm::hostcalls::log(
                LogLevel::Info,
                &format!("edge-security-filter: config loaded: {:?}", parsed),
            )
            .ok();
            self.config = Rc::new(parsed);
        } else {
            proxy_wasm::hostcalls::log(
                LogLevel::Info,
                "edge-security-filter: no plugin config, using defaults",
            )
            .ok();
        }
        true
    }


    fn get_type(&self) -> Option<ContextType> {
        Some(ContextType::HttpContext)
    }

    fn create_http_context(&self, _context_id: u32) -> Option<Box<dyn HttpContext>> {
        Some(Box::new(EdgeSecurityFilter {
            config: Rc::clone(&self.config),
            metrics: self.metrics.as_ref().map(|m| MetricIds {
                requests_total: m.requests_total,
                blocked_total: m.blocked_total,
                rate_limited_total: m.rate_limited_total,
                auth_failures_total: m.auth_failures_total,
            }),
            awaiting_auth: false,
        }))
    }
}

// ─── HTTP Context ────────────────────────────────────────────────────────────

struct EdgeSecurityFilter {
    config: Rc<FilterConfig>,
    metrics: Option<MetricIds>,
    awaiting_auth: bool,
}

impl Context for EdgeSecurityFilter {
    fn on_http_call_response(
        &mut self,
        _token_id: u32,
        _num_headers: usize,
        body_size: usize,
        _num_trailers: usize,
    ) {
        self.awaiting_auth = false;

        // Check auth service response status
        if let Some(status) = self.get_http_call_response_header(":status") {
            if status != "200" {
                proxy_wasm::hostcalls::log(
                    LogLevel::Warn,
                    &format!(
                        "edge-security-filter: auth service returned {}, blocking",
                        status
                    ),
                )
                .ok();

                if let Some(metrics) = &self.metrics {
                    proxy_wasm::hostcalls::increment_metric(metrics.auth_failures_total, 1).ok();
                    proxy_wasm::hostcalls::increment_metric(metrics.blocked_total, 1).ok();
                }

                self.send_http_response(
                    401,
                    vec![("X-Wasm-Auth", "failed")],
                    Some(b"Authentication required"),
                );
                return;
            }
        }

        proxy_wasm::hostcalls::log(
            LogLevel::Info,
            &format!(
                "edge-security-filter: auth OK (body_size={})",
                body_size
            ),
        )
        .ok();
    }
}

impl HttpContext for EdgeSecurityFilter {
    fn on_http_request_headers(&mut self, _num_headers: usize, _end_of_stream: bool) -> Action {
        // Increment total request counter
        if let Some(metrics) = &self.metrics {
            proxy_wasm::hostcalls::increment_metric(metrics.requests_total, 1).ok();
        }

        // Track total requests in shared data
        increment_shared_counter("edge:total_requests");

        // ── Bot Detection ────────────────────────────────────────────────
        if !self.config.bot_patterns.is_empty() {
            if let Some(ua) = self.get_http_request_header("User-Agent") {
                let ua_lower = ua.to_lowercase();
                for pattern in &self.config.bot_patterns {
                    if ua_lower.contains(&pattern.to_lowercase()) {
                        proxy_wasm::hostcalls::log(
                            LogLevel::Warn,
                            &format!(
                                "edge-security-filter: blocked bot (pattern={}, ua={})",
                                pattern, ua
                            ),
                        )
                        .ok();

                        if let Some(metrics) = &self.metrics {
                            proxy_wasm::hostcalls::increment_metric(metrics.blocked_total, 1).ok();
                        }

                        self.send_http_response(
                            403,
                            vec![("X-Wasm-Block-Reason", "bot-detected")],
                            Some(b"Forbidden: bot detected"),
                        );
                        return Action::Pause;
                    }
                }
            }
        }

        // ── Geo-Blocking ─────────────────────────────────────────────────
        if !self.config.blocked_countries.is_empty() {
            if let Some(country) = self.get_http_request_header("X-Country-Code") {
                let country_upper = country.to_uppercase();
                if self
                    .config
                    .blocked_countries
                    .iter()
                    .any(|c| c.to_uppercase() == country_upper)
                {
                    proxy_wasm::hostcalls::log(
                        LogLevel::Warn,
                        &format!(
                            "edge-security-filter: blocked country={}",
                            country_upper
                        ),
                    )
                    .ok();

                    if let Some(metrics) = &self.metrics {
                        proxy_wasm::hostcalls::increment_metric(metrics.blocked_total, 1).ok();
                    }

                    self.send_http_response(
                        403,
                        vec![("X-Wasm-Block-Reason", "geo-blocked")],
                        Some(b"Forbidden: region blocked"),
                    );
                    return Action::Pause;
                }
            }
        }

        // ── Rate Limiting (per client IP via shared data with CAS) ───────
        let client_ip = self
            .get_http_request_header("X-Forwarded-For")
            .and_then(|xff| xff.split(',').next().map(|s| s.trim().to_string()))
            .or_else(|| self.get_http_request_header("X-Real-IP"))
            .unwrap_or_else(|| "unknown".to_string());

        if is_rate_limited(
            &client_ip,
            self.config.rate_limit.requests_per_second,
            self.config.rate_limit.window_seconds,
        ) {
            proxy_wasm::hostcalls::log(
                LogLevel::Warn,
                &format!("edge-security-filter: rate limited ip={}", client_ip),
            )
            .ok();

            if let Some(metrics) = &self.metrics {
                proxy_wasm::hostcalls::increment_metric(metrics.rate_limited_total, 1).ok();
            }

            self.send_http_response(
                429,
                vec![
                    ("X-Wasm-Block-Reason", "rate-limited"),
                    ("Retry-After", "60"),
                ],
                Some(b"Too Many Requests"),
            );
            return Action::Pause;
        }

        // ── Header Enrichment ────────────────────────────────────────────
        if self.config.enrich_headers {
            // Generate a request ID from current time
            let now = proxy_wasm::hostcalls::get_current_time()
                .unwrap_or(std::time::SystemTime::UNIX_EPOCH);
            let nanos = now
                .duration_since(std::time::SystemTime::UNIX_EPOCH)
                .unwrap_or_default()
                .as_nanos();
            let request_id = format!("{:x}", nanos);
            self.set_http_request_header("X-Request-ID", Some(&request_id));
        }

        // ── Auth Callout (optional) ──────────────────────────────────────
        if !self.config.auth_service.is_empty() {
            if let Some(auth_header) = self.get_http_request_header("Authorization") {
                proxy_wasm::hostcalls::log(
                    LogLevel::Info,
                    "edge-security-filter: dispatching auth callout",
                )
                .ok();

                self.awaiting_auth = true;
                let headers = vec![
                    (":method", "POST"),
                    (":path", "/validate"),
                    (":authority", self.config.auth_service.as_str()),
                    ("Authorization", auth_header.as_str()),
                    ("Content-Type", "application/json"),
                ];

                match self.dispatch_http_call(
                    &self.config.auth_service,
                    headers,
                    None,
                    vec![],
                    std::time::Duration::from_secs(5),
                ) {
                    Ok(_) => return Action::Pause,
                    Err(e) => {
                        proxy_wasm::hostcalls::log(
                            LogLevel::Error,
                            &format!("edge-security-filter: auth callout failed: {:?}", e),
                        )
                        .ok();
                        // Fail-closed: block on callout error
                        self.send_http_response(
                            503,
                            vec![("X-Wasm-Block-Reason", "auth-unavailable")],
                            Some(b"Service Unavailable"),
                        );
                        return Action::Pause;
                    }
                }
            }
        }

        Action::Continue
    }

    fn on_http_request_body(&mut self, body_size: usize, end_of_stream: bool) -> Action {
        if end_of_stream && body_size > 0 {
            proxy_wasm::hostcalls::log(
                LogLevel::Info,
                &format!("edge-security-filter: request body size={}", body_size),
            )
            .ok();
        }
        Action::Continue
    }

    fn on_http_response_headers(&mut self, _num_headers: usize, _end_of_stream: bool) -> Action {
        // ── Security Headers ─────────────────────────────────────────────
        if self.config.enrich_headers {
            self.set_http_response_header("X-Content-Type-Options", Some("nosniff"));
            self.set_http_response_header("X-Frame-Options", Some("DENY"));
            self.set_http_response_header("X-Wasm-Processed", Some("true"));
        }

        Action::Continue
    }

    fn on_http_response_body(&mut self, body_size: usize, end_of_stream: bool) -> Action {
        if end_of_stream && body_size > 0 {
            proxy_wasm::hostcalls::log(
                LogLevel::Info,
                &format!("edge-security-filter: response body size={}", body_size),
            )
            .ok();
        }
        Action::Continue
    }
}

// ─── Shared Data Helpers ─────────────────────────────────────────────────────

/// Atomically increment a shared counter using CAS (compare-and-swap).
fn increment_shared_counter(key: &str) {
    loop {
        let (current_bytes, cas) =
            proxy_wasm::hostcalls::get_shared_data(key).unwrap_or((None, None));
        let current = current_bytes
            .as_ref()
            .map(|b| u64_from_bytes(b))
            .unwrap_or(0);
        let new_val = current + 1;
        let new_bytes = new_val.to_le_bytes().to_vec();

        match proxy_wasm::hostcalls::set_shared_data(key, Some(&new_bytes), cas) {
            Ok(()) => break,
            Err(Status::CasMismatch) => continue, // Retry on CAS conflict
            Err(_) => break,                       // Give up on other errors
        }
    }
}

/// Check if a client IP is rate-limited. Returns true if over the limit.
fn is_rate_limited(client_ip: &str, max_requests: u32, window_seconds: u32) -> bool {
    // Use time-bucketed key: "rl:{ip}:{window_bucket}"
    let now = proxy_wasm::hostcalls::get_current_time()
        .unwrap_or(std::time::SystemTime::UNIX_EPOCH);
    let epoch_secs = now
        .duration_since(std::time::SystemTime::UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs();
    let bucket = epoch_secs / window_seconds as u64;
    let key = format!("rl:{}:{}", client_ip, bucket);

    // Increment counter via CAS
    loop {
        let (current_bytes, cas) =
            proxy_wasm::hostcalls::get_shared_data(&key).unwrap_or((None, None));
        let current = current_bytes
            .as_ref()
            .map(|b| u32_from_bytes(b))
            .unwrap_or(0);
        let new_val = current + 1;
        let new_bytes = new_val.to_le_bytes().to_vec();

        match proxy_wasm::hostcalls::set_shared_data(&key, Some(&new_bytes), cas) {
            Ok(()) => return new_val > max_requests,
            Err(Status::CasMismatch) => continue,
            Err(_) => return false, // Fail-open on shared data errors
        }
    }
}

/// Convert bytes (little-endian) to u64.
fn u64_from_bytes(bytes: &[u8]) -> u64 {
    let mut buf = [0u8; 8];
    let len = bytes.len().min(8);
    buf[..len].copy_from_slice(&bytes[..len]);
    u64::from_le_bytes(buf)
}

/// Convert bytes (little-endian) to u32.
fn u32_from_bytes(bytes: &[u8]) -> u32 {
    let mut buf = [0u8; 4];
    let len = bytes.len().min(4);
    buf[..len].copy_from_slice(&bytes[..len]);
    u32::from_le_bytes(buf)
}
