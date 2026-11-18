//! HTTP sending with retries.
//!
//! Transient failures (connection errors, 408/409/425/429/5xx) are retried
//! with exponential backoff (0.5 s doubling up to 8 s, with jitter, as the
//! OpenAI and Anthropic SDKs do). A server `Retry-After` / `retry-after-ms`
//! delay is honoured; when it exceeds `max_retry_delay_ms` the request fails
//! immediately so a higher layer can surface the wait to the user. Waits are
//! real sleeps that end early on cancellation.

use std::time::Duration;

use anyhow::{Result, anyhow};
use serde_json::Value;
use tokio_util::sync::CancellationToken;

use crate::config::Settings;

/// `retry.provider` settings.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RetryPolicy {
    /// Time to wait for response headers on each attempt.
    pub timeout_ms: u64,
    pub max_retries: u32,
    /// Longest server-requested delay to honour; 0 disables the cap.
    pub max_retry_delay_ms: u64,
}

impl Default for RetryPolicy {
    fn default() -> Self {
        Self {
            timeout_ms: 600_000,
            max_retries: 2,
            max_retry_delay_ms: 60_000,
        }
    }
}

impl RetryPolicy {
    /// Read `retry.provider.{timeoutMs,maxRetries,maxRetryDelayMs}`.
    pub fn from_settings(settings: &Settings) -> Self {
        let mut policy = Self::default();
        let Some(provider) = settings.raw().get("retry").and_then(|r| r.get("provider")) else {
            return policy;
        };
        if let Some(v) = provider.get("timeoutMs").and_then(Value::as_u64) {
            policy.timeout_ms = v.max(1);
        }
        if let Some(v) = provider.get("maxRetries").and_then(Value::as_u64) {
            policy.max_retries = v.min(u32::MAX as u64) as u32;
        }
        if let Some(v) = provider.get("maxRetryDelayMs").and_then(Value::as_u64) {
            policy.max_retry_delay_ms = v;
        }
        policy
    }
}

/// Statuses worth retrying (archived qt `is_retryable_http_status`).
pub fn is_retryable_status(status: u16) -> bool {
    matches!(status, 408 | 409 | 425 | 429) || (500..=599).contains(&status)
}

/// Server-requested delay from `retry-after-ms` or `retry-after`
/// (seconds or an HTTP date).
pub fn retry_after_ms(headers: &reqwest::header::HeaderMap) -> Option<u64> {
    let text = |name: &str| {
        headers
            .get(name)
            .and_then(|v| v.to_str().ok())
            .map(str::trim)
            .filter(|v| !v.is_empty())
    };
    if let Some(ms) = text("retry-after-ms").and_then(|v| v.parse::<f64>().ok())
        && ms >= 0.0
    {
        return Some(ms.round() as u64);
    }
    let value = text("retry-after")?;
    if let Ok(seconds) = value.parse::<f64>() {
        return (seconds >= 0.0).then(|| (seconds * 1000.0).round() as u64);
    }
    let at = chrono::DateTime::parse_from_rfc2822(value).ok()?;
    let delta = at.timestamp_millis() - chrono::Utc::now().timestamp_millis();
    Some(delta.max(0) as u64)
}

/// Exponential backoff for `attempt` (0-based) with up to 25 % jitter.
pub fn backoff_ms(attempt: u32) -> u64 {
    let base = (500u64 << attempt.min(5)).min(8_000);
    let jitter = 1.0 - rand::random::<f64>() * 0.25;
    (base as f64 * jitter) as u64
}

/// Summarise an error body: `error.message`, `message`, or the trimmed text.
pub fn error_detail(body: &str) -> String {
    serde_json::from_str::<Value>(body)
        .ok()
        .and_then(|v| {
            v.get("error")
                .and_then(|e| {
                    e.get("message")
                        .and_then(Value::as_str)
                        .map(str::to_string)
                        .or_else(|| e.as_str().map(str::to_string))
                })
                .or_else(|| v.get("message").and_then(Value::as_str).map(str::to_string))
                .or_else(|| v.get("detail").and_then(Value::as_str).map(str::to_string))
        })
        .unwrap_or_else(|| crate::util::shorten(body.trim(), 500))
}

/// A non-success response after retries.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HttpFailure {
    pub status: u16,
    pub body: String,
}

impl HttpFailure {
    pub fn message(&self) -> String {
        let status = reqwest::StatusCode::from_u16(self.status)
            .map(|s| s.to_string())
            .unwrap_or_else(|_| self.status.to_string());
        format!("HTTP {status}: {}", error_detail(&self.body))
    }
}

/// Outcome of [`send`].
pub enum Sent {
    Ok(reqwest::Response),
    /// Final non-success response (body already read).
    Failed(HttpFailure),
    Cancelled,
}

async fn sleep_or_cancel(ms: u64, cancel: &CancellationToken) -> bool {
    tokio::select! {
        _ = tokio::time::sleep(Duration::from_millis(ms)) => true,
        _ = cancel.cancelled() => false,
    }
}

/// Decides whether a failed response (status, body) should be retried.
pub type RetryCheck = dyn Fn(u16, &str) -> bool + Send + Sync;

/// Send the request built by `make` (called once per attempt), retrying as
/// configured. `retryable_body` lets a provider retry extra failures by
/// inspecting the status and body text.
pub async fn send(
    make: &(dyn Fn() -> reqwest::RequestBuilder + Send + Sync),
    policy: &RetryPolicy,
    cancel: &CancellationToken,
    retryable_body: Option<&RetryCheck>,
) -> Result<Sent> {
    let mut attempt: u32 = 0;
    loop {
        if cancel.is_cancelled() {
            return Ok(Sent::Cancelled);
        }
        let request = make().send();
        let timeout = Duration::from_millis(policy.timeout_ms.max(1));
        let outcome = tokio::select! {
            r = tokio::time::timeout(timeout, request) => r,
            _ = cancel.cancelled() => return Ok(Sent::Cancelled),
        };
        let can_retry = attempt < policy.max_retries;
        let response = match outcome {
            Err(_) => {
                if can_retry {
                    if !sleep_or_cancel(backoff_ms(attempt), cancel).await {
                        return Ok(Sent::Cancelled);
                    }
                    attempt += 1;
                    continue;
                }
                return Err(anyhow!("Request timed out after {} ms", policy.timeout_ms));
            }
            Ok(Err(err)) => {
                if can_retry && !err.is_builder() {
                    if !sleep_or_cancel(backoff_ms(attempt), cancel).await {
                        return Ok(Sent::Cancelled);
                    }
                    attempt += 1;
                    continue;
                }
                return Err(anyhow!(err));
            }
            Ok(Ok(response)) => response,
        };
        let status = response.status().as_u16();
        if response.status().is_success() {
            return Ok(Sent::Ok(response));
        }
        let retry_after = retry_after_ms(response.headers());
        let body = tokio::select! {
            b = response.text() => b.unwrap_or_default(),
            _ = cancel.cancelled() => return Ok(Sent::Cancelled),
        };
        let retryable =
            is_retryable_status(status) || retryable_body.is_some_and(|check| check(status, &body));
        if !retryable || !can_retry {
            return Ok(Sent::Failed(HttpFailure { status, body }));
        }
        let delay = match retry_after {
            Some(ms) if policy.max_retry_delay_ms > 0 && ms > policy.max_retry_delay_ms => {
                return Err(anyhow!(
                    "{} (server requested a retry after {ms} ms, more than the {} ms limit)",
                    HttpFailure { status, body }.message(),
                    policy.max_retry_delay_ms
                ));
            }
            Some(ms) => ms,
            None => backoff_ms(attempt),
        };
        if !sleep_or_cancel(delay, cancel).await {
            return Ok(Sent::Cancelled);
        }
        attempt += 1;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use reqwest::header::{HeaderMap, HeaderValue};

    #[test]
    fn parses_retry_after_forms() {
        let mut h = HeaderMap::new();
        assert_eq!(retry_after_ms(&h), None);
        h.insert("retry-after", HeaderValue::from_static("2"));
        assert_eq!(retry_after_ms(&h), Some(2_000));
        h.insert("retry-after-ms", HeaderValue::from_static("150"));
        assert_eq!(retry_after_ms(&h), Some(150));
        let mut h = HeaderMap::new();
        h.insert(
            "retry-after",
            HeaderValue::from_static("Wed, 21 Oct 2015 07:28:00 GMT"),
        );
        assert_eq!(retry_after_ms(&h), Some(0));
        let future = (chrono::Utc::now() + chrono::Duration::seconds(30))
            .format("%a, %d %b %Y %H:%M:%S GMT")
            .to_string();
        h.insert("retry-after", HeaderValue::from_str(&future).unwrap());
        let ms = retry_after_ms(&h).unwrap();
        assert!((28_000..=30_000).contains(&ms), "{ms}");
    }

    #[test]
    fn retryable_statuses_and_backoff() {
        for s in [408, 409, 425, 429, 500, 503, 599] {
            assert!(is_retryable_status(s));
        }
        for s in [400, 401, 403, 404, 422] {
            assert!(!is_retryable_status(s));
        }
        assert!((375..=500).contains(&backoff_ms(0)));
        assert!((6_000..=8_000).contains(&backoff_ms(10)));
    }

    #[test]
    fn reads_policy_from_settings() {
        let dir = tempfile::tempdir().unwrap();
        let home = crate::config::AgentHome::new(dir.path());
        std::fs::write(
            home.settings_path(),
            r#"{"retry":{"provider":{"timeoutMs":5000,"maxRetries":4,"maxRetryDelayMs":0}}}"#,
        )
        .unwrap();
        let settings = Settings::load(&home).unwrap();
        assert_eq!(
            RetryPolicy::from_settings(&settings),
            RetryPolicy {
                timeout_ms: 5_000,
                max_retries: 4,
                max_retry_delay_ms: 0
            }
        );
        assert_eq!(
            RetryPolicy::from_settings(&Settings::default()),
            RetryPolicy::default()
        );
    }

    #[tokio::test]
    async fn retries_honour_retry_after_and_its_limit() {
        use crate::provider::test_server::{MockResponse, serve};
        let (base, requests) = serve(vec![
            MockResponse::json(429, r#"{"error":{"message":"slow down"}}"#)
                .with_header("retry-after-ms", "300"),
            MockResponse::json(200, "{}"),
            MockResponse::json(429, r#"{"error":{"message":"slow down"}}"#)
                .with_header("retry-after", "120"),
            MockResponse::json(400, r#"{"error":{"message":"bad"}}"#),
        ])
        .await;
        let http = reqwest::Client::new();
        let url = format!("{base}/x");
        let make = || http.get(&url);
        let policy = RetryPolicy {
            max_retry_delay_ms: 1_000,
            ..RetryPolicy::default()
        };
        let cancel = CancellationToken::new();
        let started = std::time::Instant::now();
        assert!(matches!(
            send(&make, &policy, &cancel, None).await.unwrap(),
            Sent::Ok(_)
        ));
        assert!(
            started.elapsed() >= Duration::from_millis(300),
            "waited for Retry-After"
        );
        let err = send(&make, &policy, &cancel, None)
            .await
            .err()
            .unwrap()
            .to_string();
        assert_eq!(
            err,
            "HTTP 429 Too Many Requests: slow down (server requested a retry after 120000 ms, more than the 1000 ms limit)"
        );
        match send(&make, &policy, &cancel, None).await.unwrap() {
            Sent::Failed(f) => assert_eq!(f.message(), "HTTP 400 Bad Request: bad"),
            _ => panic!("expected failure"),
        }
        assert_eq!(requests.lock().unwrap().len(), 4);
        cancel.cancel();
        assert!(matches!(
            send(&make, &policy, &cancel, None).await.unwrap(),
            Sent::Cancelled
        ));
    }

    #[test]
    fn error_detail_prefers_json_message() {
        assert_eq!(
            error_detail(r#"{"error":{"message":"bad key"}}"#),
            "bad key"
        );
        assert_eq!(error_detail(r#"{"error":"nope"}"#), "nope");
        assert_eq!(error_detail(" plain "), "plain");
        let f = HttpFailure {
            status: 401,
            body: r#"{"error":{"message":"bad key"}}"#.into(),
        };
        assert_eq!(f.message(), "HTTP 401 Unauthorized: bad key");
    }
}
