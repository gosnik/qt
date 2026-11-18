//! Harness settings read from `settings.json` with Pi's keys and defaults.

use serde_json::{Map, Value, json};

use crate::config::Settings;

/// How queued steering / follow-up messages are delivered (Pi `QueueMode`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum QueueMode {
    /// Deliver every queued message at once.
    All,
    /// Deliver one queued message per turn (Pi default).
    #[default]
    OneAtATime,
}

impl QueueMode {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::All => "all",
            Self::OneAtATime => "one-at-a-time",
        }
    }

    pub fn parse(s: &str) -> Option<Self> {
        match s {
            "all" => Some(Self::All),
            "one-at-a-time" => Some(Self::OneAtATime),
            _ => None,
        }
    }
}

/// `compaction.*` (Pi `CompactionSettings`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CompactionSettings {
    pub enabled: bool,
    pub reserve_tokens: u64,
    pub keep_recent_tokens: u64,
}

impl Default for CompactionSettings {
    fn default() -> Self {
        Self {
            enabled: true,
            reserve_tokens: 16_384,
            keep_recent_tokens: 20_000,
        }
    }
}

/// `retry.*` (Pi `RetrySettings`, agent-level auto-retry).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RetrySettings {
    pub enabled: bool,
    pub max_retries: u32,
    pub base_delay_ms: u64,
}

impl Default for RetrySettings {
    fn default() -> Self {
        Self {
            enabled: true,
            max_retries: 3,
            base_delay_ms: 2_000,
        }
    }
}

impl RetrySettings {
    /// Exponential backoff: `base * 2^(attempt-1)` (2 s, 4 s, 8 s by default).
    pub fn delay_ms(&self, attempt: u32) -> u64 {
        let shift = attempt.saturating_sub(1).min(32);
        self.base_delay_ms.saturating_mul(1u64 << shift)
    }
}

/// Everything the harness reads from settings.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct HarnessSettings {
    pub compaction: CompactionSettings,
    pub retry: RetrySettings,
    /// `branchSummary.reserveTokens` (default 16384).
    pub branch_summary_reserve_tokens: u64,
    pub steering_mode: QueueMode,
    pub follow_up_mode: QueueMode,
}

fn obj<'a>(raw: &'a Map<String, Value>, key: &str) -> Option<&'a Map<String, Value>> {
    raw.get(key).and_then(Value::as_object)
}

impl HarnessSettings {
    pub fn from_settings(settings: &Settings) -> Self {
        let raw = settings.raw();
        let d = CompactionSettings::default();
        let c = obj(raw, "compaction");
        let compaction = CompactionSettings {
            enabled: c
                .and_then(|c| c.get("enabled"))
                .and_then(Value::as_bool)
                .unwrap_or(d.enabled),
            reserve_tokens: c
                .and_then(|c| c.get("reserveTokens"))
                .and_then(Value::as_u64)
                .unwrap_or(d.reserve_tokens),
            keep_recent_tokens: c
                .and_then(|c| c.get("keepRecentTokens"))
                .and_then(Value::as_u64)
                .unwrap_or(d.keep_recent_tokens),
        };
        let d = RetrySettings::default();
        let r = obj(raw, "retry");
        let retry = RetrySettings {
            enabled: r
                .and_then(|r| r.get("enabled"))
                .and_then(Value::as_bool)
                .unwrap_or(d.enabled),
            max_retries: r
                .and_then(|r| r.get("maxRetries"))
                .and_then(Value::as_u64)
                .map(|v| v.min(u32::MAX as u64) as u32)
                .unwrap_or(d.max_retries),
            base_delay_ms: r
                .and_then(|r| r.get("baseDelayMs"))
                .and_then(Value::as_u64)
                .unwrap_or(d.base_delay_ms),
        };
        let mode = |key: &str| {
            raw.get(key)
                .and_then(Value::as_str)
                .and_then(QueueMode::parse)
        };
        Self {
            compaction,
            retry,
            branch_summary_reserve_tokens: obj(raw, "branchSummary")
                .and_then(|b| b.get("reserveTokens"))
                .and_then(Value::as_u64)
                .unwrap_or(16_384),
            // Pi migrates the legacy `queueMode` key to `steeringMode`.
            steering_mode: mode("steeringMode")
                .or_else(|| mode("queueMode"))
                .unwrap_or_default(),
            follow_up_mode: mode("followUpMode").unwrap_or_default(),
        }
    }
}

fn set_nested(settings: &mut Settings, section: &str, key: &str, value: Value) {
    let raw = settings.raw_mut();
    let entry = raw.entry(section).or_insert_with(|| json!({}));
    if !entry.is_object() {
        *entry = json!({});
    }
    if let Some(map) = entry.as_object_mut() {
        map.insert(key.into(), value);
    }
}

/// `compaction.enabled`.
pub fn set_compaction_enabled(settings: &mut Settings, enabled: bool) {
    set_nested(settings, "compaction", "enabled", json!(enabled));
}

/// `retry.enabled`.
pub fn set_retry_enabled(settings: &mut Settings, enabled: bool) {
    set_nested(settings, "retry", "enabled", json!(enabled));
}

/// `steeringMode`.
pub fn set_steering_mode(settings: &mut Settings, mode: QueueMode) {
    settings
        .raw_mut()
        .insert("steeringMode".into(), json!(mode.as_str()));
}

/// `followUpMode`.
pub fn set_follow_up_mode(settings: &mut Settings, mode: QueueMode) {
    settings
        .raw_mut()
        .insert("followUpMode".into(), json!(mode.as_str()));
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_and_overrides_follow_pi() {
        let s = HarnessSettings::from_settings(&Settings::default());
        assert!(s.compaction.enabled && s.retry.enabled);
        assert_eq!(s.compaction.reserve_tokens, 16_384);
        assert_eq!(s.compaction.keep_recent_tokens, 20_000);
        assert_eq!(s.retry.max_retries, 3);
        assert_eq!(
            [1, 2, 3].map(|a| s.retry.delay_ms(a)),
            [2_000, 4_000, 8_000]
        );
        assert_eq!(s.steering_mode, QueueMode::OneAtATime);

        let mut settings = Settings::default();
        settings.raw_mut().insert("queueMode".into(), json!("all"));
        set_compaction_enabled(&mut settings, false);
        set_retry_enabled(&mut settings, false);
        set_follow_up_mode(&mut settings, QueueMode::All);
        settings.raw_mut().insert(
            "retry".into(),
            json!({"enabled": false, "maxRetries": 5, "baseDelayMs": 10}),
        );
        let s = HarnessSettings::from_settings(&settings);
        assert!(!s.compaction.enabled && !s.retry.enabled);
        assert_eq!(s.retry.max_retries, 5);
        assert_eq!(s.retry.delay_ms(3), 40);
        assert_eq!(s.steering_mode, QueueMode::All);
        assert_eq!(s.follow_up_mode, QueueMode::All);
    }
}
