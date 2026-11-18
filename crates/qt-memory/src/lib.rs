//! Governed memory for qt (archived C++ `qt::memory`).
//!
//! - [`MemoryStore`]: the SQLite store at `<agent_home>/memory/memory.sqlite3`
//!   with schema migrations v1–v7 byte-compatible with the archived binary.
//! - [`MemoryService`] / [`GovernanceService`]: write policy, redaction and
//!   audited lifecycle transitions.
//! - [`review`]: the review queue workflow.
//! - [`settings`]: `memory.*` settings with the project-may-not-broaden rule.
//! - [`qtmem`] and [`skill_import`]: offline qtmem v3 import and skill import.
//!
//! The crate is UI-agnostic and synchronous; callers on an async runtime run
//! store operations on a blocking thread.

#![recursion_limit = "256"]

macro_rules! string_enum {
    ($(#[$meta:meta])* $name:ident { $($variant:ident => $text:literal),+ $(,)? }) => {
        $(#[$meta])*
        #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
        pub enum $name { $($variant),+ }

        impl $name {
            /// Every variant in declaration order.
            pub const ALL: &'static [$name] = &[$($name::$variant),+];

            /// Persisted spelling.
            pub fn as_str(self) -> &'static str {
                match self { $($name::$variant => $text),+ }
            }
        }

        impl std::fmt::Display for $name {
            fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                f.write_str(self.as_str())
            }
        }
    };
}
pub mod error;
pub mod model;
pub mod qtmem;
pub mod review;
pub mod scope;
pub mod service;
pub mod settings;
pub mod skill_import;
pub mod store;
pub mod text;

pub use error::{MemoryError, Result};
pub use model::*;
pub use scope::{
    allowed_keys, derive_runtime_memory_scope, durable_memory_scope, recall_scopes,
    scope_can_access, scope_key, scope_score, validate_scope,
};
pub use service::{
    GovernanceService, LifecyclePolicy, LifecycleRequest, MemoryService, prepare_remember,
};
pub use store::{DatabaseOptions, MemoryStore, SCHEMA_VERSION};
