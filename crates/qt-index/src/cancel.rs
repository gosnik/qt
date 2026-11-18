//! Scan cancellation and low-priority pacing.
//!
//! The archived service threw `"index scan cancelled"` from checkpoints when
//! the service was stopping and slept briefly at checkpoints during
//! background (low-priority) scans so the interactive process stays
//! responsive. [`Pacer`] carries both concerns explicitly.

use std::fmt;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

/// A scan was cancelled because the service is stopping.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Cancelled;

impl fmt::Display for Cancelled {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("index scan cancelled")
    }
}

impl std::error::Error for Cancelled {}

/// Cancellation flag plus low-priority pacing for one scan.
#[derive(Clone)]
pub struct Pacer {
    stop: Arc<AtomicBool>,
    low_priority: bool,
}

impl Pacer {
    pub fn new(stop: Arc<AtomicBool>, low_priority: bool) -> Self {
        Self { stop, low_priority }
    }

    /// A pacer that never cancels and never sleeps.
    pub fn unpaced() -> Self {
        Self::new(Arc::new(AtomicBool::new(false)), false)
    }

    pub fn low_priority(&self) -> bool {
        self.low_priority
    }

    /// Fail when cancellation was requested.
    pub fn check(&self) -> Result<(), Cancelled> {
        if self.stop.load(Ordering::Relaxed) {
            Err(Cancelled)
        } else {
            Ok(())
        }
    }

    /// Cancellation check; during low-priority scans yield and sleep `ms`
    /// every `every` operations (`every == 0` sleeps every time).
    pub fn checkpoint(&self, operations: usize, every: usize, ms: u64) -> Result<(), Cancelled> {
        self.check()?;
        if self.low_priority {
            std::thread::yield_now();
            if every == 0 || operations.is_multiple_of(every) {
                std::thread::sleep(Duration::from_millis(ms));
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cancellation_is_reported() {
        let stop = Arc::new(AtomicBool::new(false));
        let pacer = Pacer::new(stop.clone(), false);
        assert!(pacer.checkpoint(1, 1, 0).is_ok());
        stop.store(true, Ordering::Relaxed);
        assert_eq!(pacer.check(), Err(Cancelled));
        assert_eq!(Cancelled.to_string(), "index scan cancelled");
    }
}
