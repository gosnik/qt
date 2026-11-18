//! UI-agnostic runtime for the qt coding agent.
//!
//! The crate owns everything that is not presentation: configuration in the
//! agent home (`~/.qt`), model providers, the tool set, the agent loop, and
//! session persistence. Front ends (the GPUI desktop app, the headless CLI)
//! drive it through [`engine::Engine`] and consume [`agent::AgentEvent`]s.

pub mod agent;
pub mod config;
pub mod engine;
pub mod message;
pub mod prompt;
pub mod provider;
pub mod session;
pub mod sse;
pub mod tools;
pub mod util;

pub use agent::{AgentEvent, ToolExecution};
pub use config::{AgentHome, AuthStore, Settings};
pub use engine::{Conversation, Engine};
pub use message::{AssistantMessage, ContentBlock, Message, StopReason, ToolResultMessage, Usage};
pub use provider::{ModelInfo, ModelRef};
