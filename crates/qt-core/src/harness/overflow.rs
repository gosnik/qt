//! Context-overflow detection (Pi `isContextOverflow`) and retryable-error
//! classification (Pi `_isRetryableError`).

use std::sync::LazyLock;

use regex::Regex;

use crate::message::{AssistantMessage, StopReason};

fn ci(pattern: &str) -> Regex {
    Regex::new(&format!("(?i){pattern}")).expect("static regex")
}

static OVERFLOW_PATTERNS: LazyLock<Vec<Regex>> = LazyLock::new(|| {
    [
        r"prompt is too long",
        r"request_too_large",
        r"input is too long for requested model",
        r"exceeds the context window",
        r"input token count.*exceeds the maximum",
        r"maximum prompt length is \d+",
        r"reduce the length of the messages",
        r"maximum context length is \d+ tokens",
        r"input \(\d+ tokens\) is longer than the model'?s context length \(\d+ tokens\)",
        r"exceeds the limit of \d+",
        r"exceeds the available context size",
        r"greater than the context length",
        r"context window exceeds limit",
        r"exceeded model token limit",
        r"too large for model with \d+ maximum context length",
        r"model_context_window_exceeded",
        r"prompt too long; exceeded (?:max )?context length",
        r"context[_ ]length[_ ]exceeded",
        r"too many tokens",
        r"token limit exceeded",
        r"^4(?:00|13)\s*(?:status code)?\s*\(no body\)",
        // qt's own transport wording for a body-less 400/413 response.
        r"^HTTP 4(?:00|13)\b[^:]*:\s*$",
    ]
    .iter()
    .map(|p| ci(p))
    .collect()
});

static NON_OVERFLOW_PATTERNS: LazyLock<Vec<Regex>> = LazyLock::new(|| {
    [
        r"^(Throttling error|Service unavailable):",
        r"rate limit",
        r"too many requests",
    ]
    .iter()
    .map(|p| ci(p))
    .collect()
});

static RETRYABLE: LazyLock<Regex> = LazyLock::new(|| {
    ci(
        r"overloaded|provider.?returned.?error|rate.?limit|too many requests|429|500|502|503|504|service.?unavailable|server.?error|internal.?error|network.?error|connection.?error|connection.?refused|connection.?lost|websocket.?closed|websocket.?error|other side closed|fetch failed|upstream.?connect|reset before headers|socket hang up|ended without|http2 request did not get a response|timed? out|timeout|terminated|retry delay",
    )
});

/// Whether `message` reports that the request exceeded the context window:
/// a known provider error, a "successful" response whose input exceeds the
/// window, or a length stop with no output and a full window.
pub fn is_context_overflow(message: &AssistantMessage, context_window: Option<u64>) -> bool {
    if message.stop_reason == StopReason::Error
        && let Some(err) = message.error_message.as_deref()
        && !NON_OVERFLOW_PATTERNS.iter().any(|p| p.is_match(err))
        && OVERFLOW_PATTERNS.iter().any(|p| p.is_match(err))
    {
        return true;
    }
    let Some(window) = context_window.filter(|w| *w > 0) else {
        return false;
    };
    let input = message.usage.input_tokens + message.usage.cache_read_tokens;
    match message.stop_reason {
        StopReason::Stop => input > window,
        StopReason::Length => {
            message.usage.output_tokens == 0 && input as f64 >= window as f64 * 0.99
        }
        _ => false,
    }
}

/// Whether an error response is transient and worth retrying. Context
/// overflow is handled by compaction instead.
pub fn is_retryable_error(message: &AssistantMessage, context_window: Option<u64>) -> bool {
    if message.stop_reason != StopReason::Error {
        return false;
    }
    let Some(err) = message.error_message.as_deref().filter(|e| !e.is_empty()) else {
        return false;
    };
    if is_context_overflow(message, context_window) {
        return false;
    }
    RETRYABLE.is_match(err)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::message::Usage;

    fn error(text: &str) -> AssistantMessage {
        AssistantMessage {
            stop_reason: StopReason::Error,
            error_message: Some(text.into()),
            ..Default::default()
        }
    }

    #[test]
    fn detects_provider_overflow_errors() {
        for text in [
            "HTTP 400 Bad Request: prompt is too long: 213462 tokens > 200000 maximum",
            "This model's maximum context length is 128000 tokens",
            "Your input exceeds the context window of this model",
            "HTTP 413 Payload Too Large: ",
            "413 status code (no body)",
            "context_length_exceeded",
        ] {
            assert!(is_context_overflow(&error(text), None), "{text}");
        }
        assert!(!is_context_overflow(
            &error("rate limit: too many tokens per minute"),
            None
        ));
        assert!(!is_context_overflow(&error("HTTP 500: boom"), None));
    }

    #[test]
    fn detects_silent_and_length_overflow() {
        let mut m = AssistantMessage {
            usage: Usage {
                input_tokens: 900,
                cache_read_tokens: 200,
                ..Default::default()
            },
            ..Default::default()
        };
        assert!(is_context_overflow(&m, Some(1_000)));
        assert!(!is_context_overflow(&m, None));
        m.stop_reason = StopReason::Length;
        assert!(is_context_overflow(&m, Some(1_100)));
        m.usage.output_tokens = 1;
        assert!(!is_context_overflow(&m, Some(1_100)));
    }

    #[test]
    fn classifies_retryable_errors() {
        for text in [
            "HTTP 529: Overloaded",
            "HTTP 429 Too Many Requests: slow down",
            "HTTP 503 Service Unavailable: x",
            "error sending request: connection refused",
            "operation timed out",
        ] {
            assert!(is_retryable_error(&error(text), None), "{text}");
        }
        assert!(!is_retryable_error(
            &error("HTTP 401 Unauthorized: bad key"),
            None
        ));
        assert!(!is_retryable_error(&error("prompt is too long"), None));
        let ok = AssistantMessage::default();
        assert!(!is_retryable_error(&ok, None));
    }
}
