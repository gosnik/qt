//! UI-agnostic runtime for the qt coding agent.
//!
//! The crate owns everything that is not presentation: configuration in the
//! agent home (`~/.qt`), model providers, the tool set, the agent loop, the
//! harness around it (compaction, retry, queues, hooks, goals) and session
//! persistence. Front ends (the GPUI desktop app, the headless CLI) drive it
//! through [`engine::Engine`] / [`engine::Conversation`] and consume
//! [`agent::AgentEvent`]s.

pub mod agent;
pub mod auth;
pub mod catalog;
pub mod config;
pub mod engine;
pub mod harness;
pub mod images;
pub mod message;
pub mod ollama;
pub mod policy;
pub mod prompt;
pub mod provider;
pub mod session;
pub mod sse;
pub mod tools;
pub mod util;

pub use agent::{AgentEvent, ToolExecution};
pub use config::{AgentHome, AuthStore, Settings};
pub use engine::{Conversation, Delivery, Engine, Fork, NavigateOptions, TreeNavigation};
pub use harness::{CompactionReason, CompactionResult, Hooks, QueueMode, SessionStats};
pub use message::{
    AgentMessage, AssistantMessage, ContentBlock, CustomMessage, Message, StopReason,
    ToolResultMessage, Usage,
};
pub use provider::{ModelInfo, ModelRef};
pub use session::{SessionEntry, SessionSummary, SessionTree};
