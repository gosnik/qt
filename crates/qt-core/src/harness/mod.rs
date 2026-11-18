//! Harness services around the agent loop: compaction and branch summaries,
//! overflow detection and auto-retry classification, message queues, the
//! hook registry, goals, statistics and the settings that drive them.
//!
//! The conversation driver in [`crate::engine`] wires these together; the
//! modules here are pure logic plus the summarizer's provider call, so they
//! can be tested without an engine.

pub mod compaction;
pub mod goals;
pub mod hooks;
pub mod overflow;
pub mod queue;
pub mod settings;
pub mod stats;
#[cfg(test)]
pub(crate) mod test_server;

pub use compaction::{CompactionPreparation, CompactionResult};
pub use goals::{Goal, GoalCommandOutput, GoalStatus};
pub use hooks::{CompactionReason, HookId, Hooks, SessionEvent, SessionStartReason};
pub use overflow::{is_context_overflow, is_retryable_error};
pub use queue::{MessageQueues, QueueSnapshot};
pub use settings::{CompactionSettings, HarnessSettings, QueueMode, RetrySettings};
pub use stats::{ContextUsage, ModelUsage, SessionStats};
