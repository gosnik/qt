//! Review workflow: typed candidates and actions, preview/confirm/apply with
//! revision compare-and-set and idempotent action keys, constrained batches,
//! background batch jobs, persisted view state and statistics.

pub mod inbox;
pub mod json;
pub mod model;
pub mod service;

pub use inbox::ReviewInbox;
pub use json::*;
pub use model::*;
pub use service::{ReviewGovernancePolicy, ReviewService};
