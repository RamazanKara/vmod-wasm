use serde::Deserialize;

/// Rate limiting configuration.
#[derive(Deserialize, Clone, Debug)]
pub struct RateLimitConfig {
    /// Maximum requests allowed per window.
    #[serde(default = "default_requests_per_second")]
    pub requests_per_second: u32,
    /// Time window in seconds for rate limiting.
    #[serde(default = "default_window_seconds")]
    pub window_seconds: u32,
}

impl Default for RateLimitConfig {
    fn default() -> Self {
        Self {
            requests_per_second: default_requests_per_second(),
            window_seconds: default_window_seconds(),
        }
    }
}

fn default_requests_per_second() -> u32 {
    100
}

fn default_window_seconds() -> u32 {
    60
}

/// Top-level filter configuration parsed from JSON plugin config.
#[derive(Deserialize, Clone, Debug)]
pub struct FilterConfig {
    /// Rate limiting settings.
    #[serde(default)]
    pub rate_limit: RateLimitConfig,
    /// User-Agent substrings that trigger bot blocking (403).
    #[serde(default)]
    pub bot_patterns: Vec<String>,
    /// ISO country codes to block (matched against X-Country-Code header).
    #[serde(default)]
    pub blocked_countries: Vec<String>,
    /// Whether to add enrichment headers (X-Request-ID, security headers).
    #[serde(default = "default_true")]
    pub enrich_headers: bool,
    /// Upstream authority for auth token validation HTTP callout.
    /// Format: "host:port". Empty string disables auth callouts.
    #[serde(default)]
    pub auth_service: String,
}

impl Default for FilterConfig {
    fn default() -> Self {
        Self {
            rate_limit: RateLimitConfig::default(),
            bot_patterns: vec!["BadBot".into(), "Scraper".into()],
            blocked_countries: Vec::new(),
            enrich_headers: true,
            auth_service: String::new(),
        }
    }
}

fn default_true() -> bool {
    true
}

/// Parse JSON configuration bytes into FilterConfig.
/// Returns default config on parse failure (fail-open for config).
pub fn parse_config(data: &[u8]) -> FilterConfig {
    serde_json_wasm::from_slice(data).unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_default_config() {
        let config = FilterConfig::default();
        assert_eq!(config.rate_limit.requests_per_second, 100);
        assert_eq!(config.rate_limit.window_seconds, 60);
        assert!(config.enrich_headers);
        assert!(config.auth_service.is_empty());
    }

    #[test]
    fn test_parse_full_config() {
        let json = r#"{
            "rate_limit": {"requests_per_second": 50, "window_seconds": 30},
            "bot_patterns": ["EvilBot", "Crawler"],
            "blocked_countries": ["CN", "RU"],
            "enrich_headers": false,
            "auth_service": "auth.internal:8080"
        }"#;
        let config = parse_config(json.as_bytes());
        assert_eq!(config.rate_limit.requests_per_second, 50);
        assert_eq!(config.rate_limit.window_seconds, 30);
        assert_eq!(config.bot_patterns, vec!["EvilBot", "Crawler"]);
        assert_eq!(config.blocked_countries, vec!["CN", "RU"]);
        assert!(!config.enrich_headers);
        assert_eq!(config.auth_service, "auth.internal:8080");
    }

    #[test]
    fn test_parse_empty_returns_default() {
        let config = parse_config(b"{}");
        assert_eq!(config.rate_limit.requests_per_second, 100);
        assert!(config.enrich_headers);
    }

    #[test]
    fn test_parse_invalid_returns_default() {
        let config = parse_config(b"not json");
        assert_eq!(config.rate_limit.requests_per_second, 100);
    }
}
