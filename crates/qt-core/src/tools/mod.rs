//! Built-in tools offered to the model.
//!
//! Schemas, limits and result wording follow Pi's coding-agent tools
//! (`reference/pi/packages/coding-agent/src/core/tools`); the extra tools,
//! argument aliases, workspace policy and mutation queue follow the archived
//! C++ qt (`archived/src/tools`).

mod append;
mod apply_patch;
mod bash;
mod clipboard;
mod complete_path;
mod edit;
mod find;
mod grep;
mod guard;
mod ls;
mod mutation;
mod package_manager;
mod read;
mod registry;
mod web;
mod web_run;
mod write;

use std::path::{Path, PathBuf};
use std::sync::Arc;

use futures::future::BoxFuture;
use serde::de::DeserializeOwned;
use serde_json::Value;
use tokio_util::sync::CancellationToken;

use crate::message::ContentBlock;
use crate::policy::{ResolvedPath, WorkspacePolicy};
use crate::provider::ToolSpec;

pub use append::AppendTextTool;
pub use apply_patch::{ApplyPatchTool, PatchOperation, parse_patch};
pub use bash::{BashOptions, BashTool};
pub use clipboard::{
    ClipboardCommandPlan, ClipboardTool, plan_clipboard_copy_commands,
    select_clipboard_copy_command,
};
pub use complete_path::CompletePathTool;
pub use edit::{EditTool, apply_edits};
pub use find::FindTool;
pub use grep::GrepTool;
pub use guard::{OutputGuardOptions, apply_output_guard};
pub use ls::LsTool;
pub use mutation::{MutationKind, MutationPolicy, MutationQueue, PendingMutation};
pub use package_manager::{
    PackageManagerCommandPlan, PackageManagerDetection, PackageManagerKind, PackageManagerTool,
    detect_package_managers, package_manager_command_plans,
};
pub use read::{ImageInput, ImageOptions, ImageResizeFn, ReadTool, ResizedImage};
pub use registry::{
    ToolRegistry, ToolSelection, WEB_TOOL_NAMES, canonical_tool_name, normalize_arguments,
    tool_aliases, wildcard_match,
};
pub use web::{
    FetchError, FetchOptions, FetchResponse, WebFetchTool, WebOptions, WebSearchTool, fetch_url,
    html_to_text, is_public_ip, offline_mode_enabled, parse_duckduckgo_results,
    validate_public_url,
};
pub use web_run::{WebRunLimits, WebRunTool};
pub use write::WriteTool;

/// Callback receiving live output while a tool runs.
pub type ProgressFn = Arc<dyn Fn(String) + Send + Sync>;

/// How mutating tools behave for this call.
#[derive(Clone, Default)]
pub struct MutationControl {
    pub policy: MutationPolicy,
    pub queue: Option<Arc<MutationQueue>>,
    /// Id of an approved mutation this call executes.
    pub approved_id: Option<String>,
}

/// Per-call execution context.
///
/// `ToolContext::new(cwd)` is the archived secure default: paths are confined
/// to `cwd` as the workspace root, mutations execute immediately, the output
/// guard uses 64 KiB strings / 1000 array items, and web access follows
/// `PI_OFFLINE`.
#[derive(Clone)]
pub struct ToolContext {
    pub cwd: PathBuf,
    pub cancel: CancellationToken,
    pub progress: Option<ProgressFn>,
    pub policy: Arc<WorkspacePolicy>,
    pub mutations: MutationControl,
    pub output_guard: OutputGuardOptions,
    pub bash: BashOptions,
    pub images: ImageOptions,
    pub web: WebOptions,
    /// Provider tool call id, recorded on queued mutations.
    pub call_id: Option<String>,
}

impl ToolContext {
    pub fn new(cwd: impl Into<PathBuf>) -> Self {
        let cwd = cwd.into();
        Self {
            policy: Arc::new(WorkspacePolicy::confined(&cwd)),
            cwd,
            cancel: CancellationToken::new(),
            progress: None,
            mutations: MutationControl::default(),
            output_guard: OutputGuardOptions::default(),
            bash: BashOptions::default(),
            images: ImageOptions::default(),
            web: WebOptions::default(),
            call_id: None,
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

    /// Apply a workspace path policy (see [`WorkspacePolicy::load`]).
    pub fn with_policy(mut self, policy: Arc<WorkspacePolicy>) -> Self {
        self.policy = policy;
        self
    }

    /// Queue or execute mutations; `queue` collects pending approvals.
    pub fn with_mutation_policy(
        mut self,
        policy: MutationPolicy,
        queue: Arc<MutationQueue>,
    ) -> Self {
        self.mutations.policy = policy;
        self.mutations.queue = Some(queue);
        self
    }

    /// Execute the previously queued mutation `id` (it must be approved).
    pub fn with_approved_mutation(mut self, id: impl Into<String>) -> Self {
        self.mutations.approved_id = Some(id.into());
        self
    }

    pub fn with_output_guard(mut self, options: OutputGuardOptions) -> Self {
        self.output_guard = options;
        self
    }

    /// Settings `shellCommandPrefix`, extra environment, explicit shell.
    pub fn with_bash_options(mut self, options: BashOptions) -> Self {
        self.bash = options;
        self
    }

    /// Image auto-resize hook and model capabilities for `read`.
    pub fn with_image_options(mut self, options: ImageOptions) -> Self {
        self.images = options;
        self
    }

    /// Offline mode and endpoints for the web tools.
    pub fn with_web_options(mut self, options: WebOptions) -> Self {
        self.web = options;
        self
    }

    pub fn with_call_id(mut self, id: impl Into<String>) -> Self {
        self.call_id = Some(id.into());
        self
    }

    fn report(&self, text: String) {
        if let Some(progress) = &self.progress {
            progress(text);
        }
    }

    /// Resolve a path argument and enforce the workspace policy.
    pub(crate) fn resolve_path(&self, raw: &str) -> Result<ResolvedPath, ToolOutput> {
        self.policy
            .resolve(&self.cwd, raw)
            .map_err(|e| ToolOutput::error(e.to_string()))
    }

    /// Like [`Self::resolve_path`], admitting registered read artifacts.
    pub(crate) fn resolve_read_path(&self, raw: &str) -> Result<ResolvedPath, ToolOutput> {
        self.policy
            .resolve_for_read(&self.cwd, raw)
            .map_err(|e| ToolOutput::error(e.to_string()))
    }

    /// `path` relative to the working directory when it lies underneath it.
    pub(crate) fn display(&self, path: &Path) -> String {
        let shown = crate::util::display_path(&self.cwd, path);
        if cfg!(windows) {
            shown.replace('\\', "/")
        } else {
            shown
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
    /// Whether calls may run concurrently with other parallel-safe calls in
    /// the same batch (read-only tools without side effects).
    fn parallel_safe(&self) -> bool {
        false
    }

    fn spec(&self) -> ToolSpec {
        ToolSpec {
            name: self.name().into(),
            description: self.description(),
            parameters: self.parameters(),
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

/// Accept booleans sent as `true`/`false` strings or 0/1.
pub(crate) fn de_opt_bool<'de, D: serde::Deserializer<'de>>(
    d: D,
) -> Result<Option<bool>, D::Error> {
    let v: Option<Value> = serde::Deserialize::deserialize(d)?;
    Ok(match v {
        Some(Value::Bool(b)) => Some(b),
        Some(Value::Number(n)) => n.as_f64().map(|f| f != 0.0),
        Some(Value::String(s)) => match s.trim().to_ascii_lowercase().as_str() {
            "true" | "1" | "yes" => Some(true),
            "false" | "0" | "no" => Some(false),
            _ => None,
        },
        _ => None,
    })
}

/// Common file-type groups accepted by `grep`/`find` `type` arguments.
pub(crate) fn extensions_for_type(kind: &str) -> Vec<String> {
    let kind = kind.trim().trim_start_matches('.').to_ascii_lowercase();
    let list: &[&str] = match kind.as_str() {
        "cpp" | "c++" => &["cc", "cpp", "cxx", "c", "h", "hh", "hpp", "hxx"],
        "c" => &["c", "h"],
        "typescript" | "ts" => &["ts", "tsx", "mts", "cts"],
        "javascript" | "js" => &["js", "jsx", "mjs", "cjs"],
        "python" | "py" => &["py", "pyi"],
        "rust" | "rs" => &["rs"],
        "markdown" | "md" => &["md", "markdown"],
        "json" => &["json", "jsonl"],
        "go" => &["go"],
        "java" => &["java"],
        "yaml" | "yml" => &["yaml", "yml"],
        "toml" => &["toml"],
        "html" => &["html", "htm"],
        "css" => &["css", "scss", "sass", "less"],
        "sh" | "shell" => &["sh", "bash", "zsh"],
        _ => return vec![kind],
    };
    list.iter().map(|s| s.to_string()).collect()
}

/// Whether `name`'s extension is one of `extensions`.
pub(crate) fn matches_extension(name: &str, extensions: &[String]) -> bool {
    match name.rsplit_once('.') {
        Some((stem, ext)) if !stem.is_empty() || !ext.is_empty() => {
            let ext = ext.to_ascii_lowercase();
            !ext.is_empty() && extensions.contains(&ext)
        }
        _ => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn type_groups_and_extension_matching() {
        let cpp = extensions_for_type("C++");
        assert!(matches_extension("src/a.HPP", &cpp));
        assert!(!matches_extension("Makefile", &cpp));
        assert_eq!(extensions_for_type(".proto"), vec!["proto".to_string()]);
    }

    #[test]
    fn default_context_confines_to_cwd() {
        let dir = tempfile::tempdir().unwrap();
        let ctx = ToolContext::new(dir.path());
        assert!(!ctx.policy.allows_external_access());
        assert_eq!(ctx.policy.root(), dir.path().canonicalize().unwrap());
        assert!(ctx.resolve_path("/etc/hosts").is_err());
        assert!(ctx.resolve_path("x/y").is_ok());
        assert_eq!(ctx.mutations.policy, MutationPolicy::Execute);
    }
}
