//! Subsystem slash commands (`SlashAction::Subsystem`): `/memory`,
//! `/subagent`, `/index`, `/mcp` and `/llama`, answered from the runtime so
//! every front end shares them.

use qt_core::engine::Conversation;
use qt_memory::slash::{SlashMemoryAction, SlashMemoryContext, memory_slash_command};

use crate::runtime::Runtime;

/// Output of a subsystem command.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct SubsystemResponse {
    pub lines: Vec<String>,
    /// A `/memory` view the front end should open.
    pub memory_action: Option<SlashMemoryAction>,
}

impl SubsystemResponse {
    fn lines(lines: Vec<String>) -> Self {
        Self {
            lines,
            memory_action: None,
        }
    }
}

impl Runtime {
    /// The `/memory` handler inputs for this runtime.
    pub fn memory_slash_context(&self) -> SlashMemoryContext {
        let engine = self.engine();
        let policy = engine.workspace_policy();
        SlashMemoryContext {
            runtime: self.memory().map(|m| m.runtime().clone()),
            settings: self.memory_settings().clone(),
            agent_home: self.agent_home().root().to_path_buf(),
            cwd: engine.cwd().to_path_buf(),
            workspace_root: policy.root().to_path_buf(),
            allow_external_filesystem_access: policy.allows_external_access(),
            diagnostics: self.diagnostics(),
        }
    }

    /// Run `/<command> <arguments>` for a subsystem command.
    pub async fn run_subsystem_command(
        &self,
        conversation: &Conversation,
        command: &str,
        arguments: &str,
    ) -> SubsystemResponse {
        match command {
            "memory" => {
                let response = memory_slash_command(&self.memory_slash_context(), arguments).await;
                SubsystemResponse {
                    lines: response.lines,
                    memory_action: response.action,
                }
            }
            "subagent" => {
                let engine = conversation.engine();
                let ctx = engine
                    .tool_environment(None)
                    .context(engine.cwd().to_path_buf());
                let tool = engine
                    .tools()
                    .get("subagent")
                    .map(|_| self.subagents().current_tool());
                SubsystemResponse::lines(
                    qt_subagents::commands::run_subagent_command(tool.as_ref(), arguments, ctx)
                        .await,
                )
            }
            "index" => SubsystemResponse::lines(self.index_command(arguments).await),
            "mcp" => SubsystemResponse::lines(self.mcp_command(arguments).await),
            "llama" => {
                let runtime = self.clone();
                let arguments = arguments.to_string();
                let lines = tokio::task::spawn_blocking(move || runtime.llama_command(&arguments))
                    .await
                    .unwrap_or_else(|e| vec![format!("llama command failed: {e}")]);
                SubsystemResponse::lines(lines)
            }
            other => SubsystemResponse::lines(vec![format!(
                "/{other} is handled by the desktop app panel"
            )]),
        }
    }
}
