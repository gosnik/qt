//! Subsystem slash commands (`SlashAction::Subsystem`): `/memory`,
//! `/subagent`, `/index`, `/mcp` and `/llama`, answered from the runtime so
//! every front end shares them.

use std::path::PathBuf;

use qt_core::engine::Conversation;
use qt_mcp::ServerStatus;
use qt_memory::slash::{SlashMemoryAction, SlashMemoryContext, memory_slash_command};
use serde_json::Value;

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

fn json_lines(value: &Value) -> Vec<String> {
    serde_json::to_string_pretty(value)
        .unwrap_or_default()
        .lines()
        .map(str::to_string)
        .collect()
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
            "index" => self.index_command(arguments),
            "mcp" => self.mcp_command(arguments).await,
            "llama" => self.llama_command(arguments),
            other => SubsystemResponse::lines(vec![format!(
                "/{other} is handled by the desktop app panel"
            )]),
        }
    }

    fn index_command(&self, arguments: &str) -> SubsystemResponse {
        let service = self.index_service();
        match arguments.split_whitespace().next().unwrap_or("status") {
            "status" => {
                let mut lines = vec![format!(
                    "Project index: {}",
                    service.project_root().display()
                )];
                lines.extend(json_lines(&service.status().to_json()));
                SubsystemResponse::lines(lines)
            }
            "scan" | "rescan" => {
                service.request_scan();
                SubsystemResponse::lines(vec!["Project index scan requested.".into()])
            }
            other => SubsystemResponse::lines(vec![
                format!("unknown index command: {other}"),
                "usage: /index [status|scan]".into(),
            ]),
        }
    }

    async fn mcp_command(&self, arguments: &str) -> SubsystemResponse {
        let Some(manager) = self.mcp() else {
            return SubsystemResponse::lines(vec!["No MCP servers are configured.".into()]);
        };
        let mut words = arguments.split_whitespace();
        match words.next().unwrap_or("status") {
            "reconnect" => {
                let Some(name) = words.next() else {
                    return SubsystemResponse::lines(vec!["usage: /mcp reconnect <server>".into()]);
                };
                match manager.reconnect(name).await {
                    Ok(notes) if notes.is_empty() => {
                        SubsystemResponse::lines(vec![format!("Reconnected MCP server {name}.")])
                    }
                    Ok(notes) => SubsystemResponse::lines(notes),
                    Err(e) => SubsystemResponse::lines(vec![e.to_string()]),
                }
            }
            _ => {
                let mut lines = vec!["MCP servers:".to_string()];
                for server in manager.snapshots() {
                    let status = match &server.status {
                        ServerStatus::Failed(e) => format!("failed: {e}"),
                        ServerStatus::Unsupported(t) => format!("unsupported transport: {t}"),
                        other => other.label().to_string(),
                    };
                    lines.push(format!(
                        "  {} [{}] {} tools={}",
                        server.name,
                        status,
                        server.transport,
                        server.tools.len()
                    ));
                }
                SubsystemResponse::lines(lines)
            }
        }
    }

    fn llama_command(&self, arguments: &str) -> SubsystemResponse {
        let engine = self.engine();
        let settings = engine.settings();
        let llama = self.llama();
        let mut words = arguments.split_whitespace();
        match words.next().unwrap_or("status") {
            "status" => {
                let mut lines = vec![format!(
                    "llama.cpp: {}",
                    if qt_llama::native_available() {
                        "available"
                    } else {
                        "not built into this binary"
                    }
                )];
                let backends = qt_llama::compiled_backends();
                if !backends.is_empty() {
                    lines.push(format!("backends: {}", backends.join(", ")));
                }
                if let Some(runtime) = llama.current_runtime()
                    && let Ok(status) = runtime.runtime_status(qt_llama::InstanceTag::Chat)
                {
                    lines.extend(json_lines(&status.to_json()));
                }
                lines.extend(llama.diagnostics());
                SubsystemResponse::lines(lines)
            }
            "models" => {
                let models = llama.model_infos(&settings);
                if models.is_empty() {
                    return SubsystemResponse::lines(vec!["No llama models configured.".into()]);
                }
                SubsystemResponse::lines(models.iter().map(|m| format!("llama/{}", m.id)).collect())
            }
            "discover" => {
                let roots: Vec<PathBuf> = words
                    .flat_map(|w| w.split(','))
                    .filter(|w| !w.is_empty())
                    .map(|w| {
                        let p = PathBuf::from(w);
                        if p.is_absolute() {
                            p
                        } else {
                            engine.cwd().join(p)
                        }
                    })
                    .collect();
                let roots = if roots.is_empty() {
                    qt_llama::discovery::default_roots(engine.cwd(), self.agent_home().root(), &[])
                } else {
                    roots
                };
                let (found, notes) = llama.discover_and_register(&settings, engine.cwd(), &roots);
                let mut lines = vec![format!("Discovered {} local model(s).", found.models.len())];
                lines.extend(
                    found
                        .models
                        .iter()
                        .map(|m| format!("  llama/{} ({})", m.id, m.path)),
                );
                lines.extend(notes);
                SubsystemResponse::lines(lines)
            }
            other => SubsystemResponse::lines(vec![
                format!("unknown llama command: {other}"),
                "usage: /llama [status|models|discover [dirs]]".into(),
            ]),
        }
    }
}
