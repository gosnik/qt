//! Pure text for the run controls: progress indicators, the message queue
//! and the goal status.

use std::time::{Duration, Instant};

use qt_core::harness::CompactionReason;
use qt_core::harness::goals::{Goal, GoalStatus, format_elapsed};
use qt_core::harness::queue::QueueSnapshot;

/// A long-running harness activity shown above the composer.
#[derive(Debug, Clone, PartialEq)]
pub enum Activity {
    Compacting {
        reason: CompactionReason,
    },
    Retrying {
        attempt: u32,
        max_attempts: u32,
        until: Instant,
        error: String,
    },
}

impl Activity {
    /// Indicator text; `now` drives the retry countdown.
    pub fn text(&self, now: Instant) -> String {
        match self {
            Activity::Compacting { reason } => match reason {
                CompactionReason::Manual => "Compacting context… (Esc to cancel)".into(),
                CompactionReason::Threshold => "Auto-compacting context… (Esc to cancel)".into(),
                CompactionReason::Overflow => {
                    "Context overflow: compacting and retrying… (Esc to cancel)".into()
                }
            },
            Activity::Retrying {
                attempt,
                max_attempts,
                until,
                error,
            } => {
                let left = until.saturating_duration_since(now);
                let secs = left.as_secs() + u64::from(left.subsec_millis() > 0);
                format!(
                    "Retrying ({attempt}/{max_attempts}) in {secs}s… {}",
                    qt_core::util::shorten(error.lines().next().unwrap_or_default(), 160)
                )
            }
        }
    }

    pub fn retrying(attempt: u32, max_attempts: u32, delay_ms: u64, error: &str) -> Self {
        Activity::Retrying {
            attempt,
            max_attempts,
            until: Instant::now() + Duration::from_millis(delay_ms),
            error: error.to_string(),
        }
    }
}

/// Rows of the queue display: `(is_steering, text)`.
pub fn queue_rows(queue: &QueueSnapshot) -> Vec<(bool, String)> {
    queue
        .steering
        .iter()
        .map(|t| (true, t.clone()))
        .chain(queue.follow_up.iter().map(|t| (false, t.clone())))
        .collect()
}

/// Composer text after restoring the queue (Pi: queued messages first, then
/// the current draft, separated by blank lines).
pub fn dequeue_text(queue: &QueueSnapshot, draft: &str) -> String {
    let mut parts: Vec<&str> = queue
        .steering
        .iter()
        .chain(queue.follow_up.iter())
        .map(String::as_str)
        .filter(|t| !t.trim().is_empty())
        .collect();
    if !draft.trim().is_empty() {
        parts.push(draft);
    }
    parts.join("\n\n")
}

fn compact_tokens(n: i64) -> String {
    if n >= 1_000_000 {
        format!("{:.1}M", n as f64 / 1e6)
    } else if n >= 1_000 {
        format!("{:.1}k", n as f64 / 1e3)
    } else {
        n.to_string()
    }
}

/// One-line goal status for the status bar.
pub fn goal_line(goal: &Goal) -> String {
    let mut out = format!("Goal {}", goal.status.label());
    if goal.time_used_seconds > 0 {
        out.push_str(&format!(" · {}", format_elapsed(goal.time_used_seconds)));
    }
    match goal.token_budget {
        Some(b) => out.push_str(&format!(
            " · {}/{} tokens",
            compact_tokens(goal.tokens_used),
            compact_tokens(b)
        )),
        None if goal.tokens_used > 0 => {
            out.push_str(&format!(" · {} tokens", compact_tokens(goal.tokens_used)))
        }
        None => {}
    }
    out
}

/// Which goal controls apply to a status (archived `/goal` command hints).
pub fn goal_controls(status: GoalStatus) -> (bool, bool) {
    let can_pause = status == GoalStatus::Active;
    let can_resume = matches!(
        status,
        GoalStatus::Paused | GoalStatus::Blocked | GoalStatus::UsageLimited
    );
    (can_pause, can_resume)
}

/// Parse a token budget field: empty is none, otherwise a positive integer
/// with an optional `k`/`m` suffix.
pub fn parse_budget(text: &str) -> Result<Option<i64>, String> {
    let t = text.trim().to_lowercase().replace([',', '_'], "");
    if t.is_empty() {
        return Ok(None);
    }
    let (digits, scale) = match t.strip_suffix('k') {
        Some(d) => (d, 1_000.0),
        None => match t.strip_suffix('m') {
            Some(d) => (d, 1_000_000.0),
            None => (t.as_str(), 1.0),
        },
    };
    match digits.trim().parse::<f64>() {
        Ok(v) if v > 0.0 && v.is_finite() => Ok(Some((v * scale).round() as i64)),
        _ => Err(format!("Invalid token budget: {text}")),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn activity_text() {
        let now = Instant::now();
        let a = Activity::Retrying {
            attempt: 1,
            max_attempts: 3,
            until: now + Duration::from_millis(1_500),
            error: "429 rate limited\nmore".into(),
        };
        assert_eq!(a.text(now), "Retrying (1/3) in 2s… 429 rate limited");
        assert_eq!(
            Activity::Compacting {
                reason: CompactionReason::Manual
            }
            .text(now),
            "Compacting context… (Esc to cancel)"
        );
    }

    #[test]
    fn queue_and_dequeue() {
        let q = QueueSnapshot {
            steering: vec!["a".into()],
            follow_up: vec!["b".into(), " ".into()],
        };
        assert_eq!(
            queue_rows(&q),
            [
                (true, "a".to_string()),
                (false, "b".into()),
                (false, " ".into())
            ]
        );
        assert_eq!(dequeue_text(&q, "draft"), "a\n\nb\n\ndraft");
        assert_eq!(dequeue_text(&q, "  "), "a\n\nb");
    }

    #[test]
    fn goal_text_and_controls() {
        let mut g = Goal::new("ship it", Some(50_000));
        g.tokens_used = 12_300;
        g.time_used_seconds = 300;
        assert_eq!(goal_line(&g), "Goal active · 5m · 12.3k/50.0k tokens");
        assert_eq!(goal_controls(GoalStatus::Active), (true, false));
        assert_eq!(goal_controls(GoalStatus::Paused), (false, true));
        assert_eq!(goal_controls(GoalStatus::Complete), (false, false));
        assert_eq!(parse_budget(""), Ok(None));
        assert_eq!(parse_budget("50k"), Ok(Some(50_000)));
        assert_eq!(parse_budget("1.5m"), Ok(Some(1_500_000)));
        assert_eq!(parse_budget("2,000"), Ok(Some(2_000)));
        assert!(parse_budget("-3").is_err());
        assert!(parse_budget("abc").is_err());
    }
}
