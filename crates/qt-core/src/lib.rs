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
pub mod events_json;
pub mod export;
pub mod harness;
pub mod history;
pub mod images;
pub mod input;
pub mod logging;
pub mod message;
pub mod migrations;
pub mod model_scope;
pub mod ollama;
pub mod policy;
pub mod prompt;
pub mod provider;
pub mod resources;
pub mod session;
pub mod session_log;
pub mod settings_layers;
pub mod slash;
pub mod sse;
pub mod tools;
pub mod util;
pub mod yaml;

pub use agent::{AgentEvent, ToolExecution};
pub use config::{AgentHome, AuthStore, Settings};
pub use engine::{
    Conversation, Delivery, Engine, EngineBuilder, ExtensionInfo, Fork, ModelRefresh,
    NavigateOptions, ToolSource, TreeNavigation, UserBash,
};
pub use harness::{CompactionReason, CompactionResult, Hooks, QueueMode, SessionStats};
pub use message::{
    AgentMessage, AssistantMessage, ContentBlock, CustomMessage, Message, StopReason,
    ToolResultMessage, Usage,
};
pub use provider::{ModelInfo, ModelRef};
pub use session::{SessionEntry, SessionSummary, SessionTree};

#[cfg(all(test, unix))]
pub(crate) mod test_support {
    use std::io::Write as _;
    use std::os::unix::fs::PermissionsExt as _;
    use std::path::Path;
    use std::process::{Command, Stdio};

    /// Write an executable script without opening it for writing in this
    /// process. Exec fails with ETXTBSY while any process holds a file open
    /// for writing, and a test thread that forks while another writes a
    /// script inherits that handle until it execs; a child `sh` writing the
    /// file keeps the handle out of this process.
    pub fn write_script(path: &Path, text: &str) {
        let mut child = Command::new("sh")
            .args(["-c", "cat > \"$0\"", &path.to_string_lossy()])
            .stdin(Stdio::piped())
            .spawn()
            .expect("spawn sh");
        child
            .stdin
            .take()
            .unwrap()
            .write_all(text.as_bytes())
            .unwrap();
        assert!(
            child.wait().unwrap().success(),
            "writing {}",
            path.display()
        );
        std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o755)).unwrap();
    }
}
