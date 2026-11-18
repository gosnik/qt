//! Built-in tools offered to the model.
//!
//! Schemas, limits and result wording follow Pi's coding-agent tools
//! (`reference/pi/packages/coding-agent/src/core/tools`).

mod bash;
mod edit;
mod find;
mod grep;
mod ls;
mod read;
mod write;

use std::path::PathBuf;
use std::sync::Arc;

use futures::future::BoxFuture;
use serde::de::DeserializeOwned;
use serde_json::Value;
use tokio_util::sync::CancellationToken;

use crate::message::ContentBlock;
use crate::provider::ToolSpec;

pub use bash::BashTool;
pub use edit::{EditTool, apply_edits};
pub use find::FindTool;
pub use grep::GrepTool;
pub use ls::LsTool;
pub use read::ReadTool;
pub use write::WriteTool;

/// Callback receiving live output while a tool runs.
pub type ProgressFn = Arc<dyn Fn(String) + Send + Sync>;

/// Per-call execution context.
#[derive(Clone)]
pub struct ToolContext {
    pub cwd: PathBuf,
    pub cancel: CancellationToken,
    pub progress: Option<ProgressFn>,
}

impl ToolContext {
    pub fn new(cwd: impl Into<PathBuf>) -> Self {
        Self {
            cwd: cwd.into(),
            cancel: CancellationToken::new(),
            progress: None,
        }
    }

    /// Use `cancel` (typically a child token of the run) for this call.
    pub fn with_cancel(mut self, cancel: CancellationToken) -> Self {
        self.cancel = cancel;
        self
    }

    /// Receive live output while the tool runs.
    pub fn with_progress(mut self, progress: ProgressFn) -> Self {
        self.progress = Some(progress);
        self
    }

    fn report(&self, text: String) {
        if let Some(progress) = &self.progress {
            progress(text);
        }
    }
}

/// Result of one tool call.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct ToolOutput {
    pub content: Vec<ContentBlock>,
    pub is_error: bool,
    pub details: Option<Value>,
}

impl ToolOutput {
    pub fn text(text: impl Into<String>) -> Self {
        Self {
            content: vec![ContentBlock::text(text)],
            ..Default::default()
        }
    }

    pub fn error(text: impl Into<String>) -> Self {
        Self {
            content: vec![ContentBlock::text(text)],
            is_error: true,
            details: None,
        }
    }

    pub fn with_details(mut self, details: Value) -> Self {
        self.details = Some(details);
        self
    }

    pub fn text_content(&self) -> String {
        crate::message::join_text(&self.content)
    }
}

/// A tool the agent can call.
pub trait Tool: Send + Sync {
    fn name(&self) -> &'static str;
    /// Full description sent with the tool schema.
    fn description(&self) -> String;
    /// One-line summary listed in the system prompt.
    fn snippet(&self) -> &'static str;
    /// Extra system-prompt guidelines contributed by this tool.
    fn guidelines(&self) -> &'static [&'static str] {
        &[]
    }
    /// JSON schema of the arguments.
    fn parameters(&self) -> Value;
    fn execute(&self, args: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput>;

    fn spec(&self) -> ToolSpec {
        ToolSpec {
            name: self.name().into(),
            description: self.description(),
            parameters: self.parameters(),
        }
    }
}

/// The set of tools available to an agent.
#[derive(Clone)]
pub struct ToolRegistry {
    tools: Vec<Arc<dyn Tool>>,
}

impl ToolRegistry {
    /// Pi's default coding tools plus the read-only exploration tools.
    pub fn coding() -> Self {
        Self {
            tools: vec![
                Arc::new(ReadTool),
                Arc::new(BashTool),
                Arc::new(EditTool),
                Arc::new(WriteTool),
                Arc::new(GrepTool),
                Arc::new(FindTool),
                Arc::new(LsTool),
            ],
        }
    }

    pub fn empty() -> Self {
        Self { tools: Vec::new() }
    }

    pub fn with(mut self, tool: Arc<dyn Tool>) -> Self {
        self.tools.retain(|t| t.name() != tool.name());
        self.tools.push(tool);
        self
    }

    pub fn get(&self, name: &str) -> Option<Arc<dyn Tool>> {
        self.tools.iter().find(|t| t.name() == name).cloned()
    }

    pub fn iter(&self) -> impl Iterator<Item = &Arc<dyn Tool>> {
        self.tools.iter()
    }

    pub fn specs(&self) -> Vec<ToolSpec> {
        self.tools.iter().map(|t| t.spec()).collect()
    }

    /// Run `name`, reporting unknown tools as errors to the model.
    pub async fn execute(&self, name: &str, args: Value, ctx: ToolContext) -> ToolOutput {
        match self.get(name) {
            Some(tool) => tool.execute(args, ctx).await,
            None => ToolOutput::error(format!("Tool {name} not found")),
        }
    }
}

/// Deserialize tool arguments with a model-readable error.
pub(crate) fn parse_args<T: DeserializeOwned>(tool: &str, args: Value) -> Result<T, ToolOutput> {
    let args = match args {
        Value::String(raw) => {
            return Err(ToolOutput::error(format!(
                "Invalid arguments for {tool}: not valid JSON: {}",
                crate::util::shorten(&raw, 200)
            )));
        }
        Value::Null => Value::Object(Default::default()),
        other => other,
    };
    serde_json::from_value(args)
        .map_err(|e| ToolOutput::error(format!("Invalid arguments for {tool}: {e}")))
}

/// Accept numbers sent as floats or numeric strings.
pub(crate) fn de_opt_usize<'de, D: serde::Deserializer<'de>>(
    d: D,
) -> Result<Option<usize>, D::Error> {
    let v: Option<Value> = serde::Deserialize::deserialize(d)?;
    Ok(match v {
        Some(Value::Number(n)) => n.as_f64().filter(|f| *f >= 0.0).map(|f| f as usize),
        Some(Value::String(s)) => s
            .trim()
            .parse::<f64>()
            .ok()
            .filter(|f| *f >= 0.0)
            .map(|f| f as usize),
        _ => None,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn unknown_tool_and_bad_args_are_errors() {
        let reg = ToolRegistry::coding();
        let ctx = ToolContext::new(std::env::temp_dir());
        let out = reg.execute("nope", Value::Null, ctx.clone()).await;
        assert!(out.is_error);
        let out = reg.execute("read", Value::String("{bad".into()), ctx).await;
        assert!(out.is_error);
        assert!(out.text_content().contains("not valid JSON"));
        let names: Vec<_> = reg.iter().map(|t| t.name()).collect();
        assert_eq!(
            names,
            ["read", "bash", "edit", "write", "grep", "find", "ls"]
        );
    }
}
