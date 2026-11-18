//! Subagents for qt.
//!
//! - [`definitions`]: agent definition files, the built-in roles and
//!   discovery across the user and project scopes.
//! - [`tool`]: the `subagent` tool (single, parallel, chain, list and the
//!   persistent worker actions).
//! - [`runner`]: child execution through `qt-cli --mode json` processes.
//! - [`coordinator`]: persistent workers with mailboxes, storage and
//!   restart recovery.
//! - [`delivery`]: worker completions as `subagent_event` follow-ups in the
//!   parent conversation.
//! - [`commands`]: `/subagent` slash command handlers.
//!
//! Behaviour follows the archived C++ qt (`archived/src/tools/subagent_tool.cpp`,
//! `archived/src/app/runtime/subagent_coordinator.cpp`,
//! `archived/docs/persistent-subagents.md`).

pub mod commands;
pub mod coordinator;
pub mod definitions;
pub mod delivery;
pub mod runner;
pub mod tool;

pub use coordinator::{
    CoordinatorOptions, ForkMode, PersistentRequest, SubagentCoordinator, coordinator_directory,
    stable_hash_hex,
};
pub use definitions::{
    AgentScope, SubagentDefinition, SubagentDiscovery, SubagentSource, SystemPromptMode,
    builtin_subagents, discover_subagents,
};
pub use delivery::{attach_conversation, attach_delivery, event_message};
pub use runner::{ChildOutcome, ChildRequest, ChildRunner, ProcessChildRunner};
pub use tool::{ParentState, SubagentTool, SubagentToolOptions};
