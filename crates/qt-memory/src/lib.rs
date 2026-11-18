//! Governed memory for qt (archived C++ `qt::memory`).
//!
//! - [`MemoryStore`]: the SQLite store at `<agent_home>/memory/memory.sqlite3`
//!   with schema migrations v1–v7 byte-compatible with the archived binary.
//! - [`MemoryService`] / [`GovernanceService`]: write policy, redaction and
//!   audited lifecycle transitions.
//! - [`review`]: the review queue workflow.
//! - [`settings`]: `memory.*` settings with the project-may-not-broaden rule.
//! - [`qtmem`] and [`skill_import`]: offline qtmem v3 import and skill import.
//! - [`recall`], [`retrieval`], [`code`]: lexical, structured, graph, dense
//!   and code recall with reciprocal-rank fusion and explanations.
//! - [`context`], [`prehandler`], [`coordinator`], [`isolated`]: the
//!   `<qt_memory_context>` envelope, the isolated pre-handler protocol and
//!   automatic recall.
//! - [`ingestion`], [`runtime`]: capture of finished runs (spool, receipts,
//!   attribution) and the per-agent-home runtime.
//! - [`integration`], [`tools`], [`cli`], [`rpc`], [`slash`]: qt-core hooks,
//!   model tools and the CLI/RPC/slash surfaces.
//!
//! The crate is UI-agnostic. Store and service calls are synchronous;
//! callers on an async runtime run them on a blocking thread (the tools do).

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
pub mod cli;
pub mod code;
pub mod context;
pub mod coordinator;
pub mod error;
pub mod ingestion;
pub mod integration;
pub mod isolated;
pub mod model;
pub mod prehandler;
pub mod qtmem;
pub mod recall;
pub mod retrieval;
pub mod review;
pub mod rpc;
pub mod runtime;
pub mod scope;
pub mod service;
pub mod settings;
pub mod skill_import;
pub mod slash;
pub mod store;
pub mod text;
pub mod tools;

pub use context::{ContextOptions, ContextPack, ContextService};
pub use coordinator::{MemoryAugmentation, MemoryCoordinator, MemoryCoordinatorSettings};
pub use error::{MemoryError, Result};
pub use model::*;
pub use recall::{RecallRequest, RecallService, RecalledMemory};
pub use runtime::{CompletedEpisodeMetadata, MemoryRuntime, MemoryRuntimeOptions};
pub use scope::{
    allowed_keys, derive_runtime_memory_scope, durable_memory_scope, recall_scopes,
    scope_can_access, scope_key, scope_score, validate_scope,
};
pub use service::{
    GovernanceService, LifecyclePolicy, LifecycleRequest, MemoryService, prepare_remember,
};
pub use store::{DatabaseOptions, MemoryStore, SCHEMA_VERSION};
