//! Tool registry: model-facing names, archived aliases, argument
//! normalisation and `--tools` / `--no-tools` / `--no-builtin-tools`
//! selection (archived `registry.cpp`, `runtime_services.cpp`).

use std::sync::Arc;

use serde_json::{Map, Value};

use super::guard::apply_output_guard;
use super::mutation::PendingMutation;
use super::{
    AppendTextTool, ApplyPatchTool, BashTool, ClipboardTool, CompletePathTool, EditTool, FindTool,
    GrepTool, LsTool, PackageManagerTool, ReadTool, Tool, ToolContext, ToolOutput, WebFetchTool,
    WebRunTool, WebSearchTool, WriteTool,
};
use crate::provider::ToolSpec;

/// Archived implementation names accepted for the Pi-named tools.
const ALIASES: &[(&str, &str)] = &[
    ("read_text", "read"),
    ("write_text", "write"),
    ("edit_text", "edit"),
    ("search_text", "grep"),
    ("list_directory", "ls"),
    ("find_files", "find"),
];

/// Names of the tools that need network access.
pub const WEB_TOOL_NAMES: &[&str] = &["web_fetch", "web_search", "web_run"];

/// Model-facing name for `name`, resolving archived aliases.
pub fn canonical_tool_name(name: &str) -> &str {
    ALIASES
        .iter()
        .find(|(alias, _)| *alias == name)
        .map(|(_, canonical)| *canonical)
        .unwrap_or(name)
}

/// Archived aliases of a model-facing tool name.
pub fn tool_aliases(name: &str) -> impl Iterator<Item = &'static str> + '_ {
    ALIASES
        .iter()
        .filter(move |(_, canonical)| *canonical == name)
        .map(|(alias, _)| *alias)
}

/// The set of tools available to an agent.
#[derive(Clone)]
pub struct ToolRegistry {
    tools: Vec<Arc<dyn Tool>>,
}

/// Which tools a run exposes (CLI `--tools`, `--no-tools`,
/// `--no-builtin-tools`, `--offline`).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ToolSelection {
    /// Disable every tool (built-in and extension/custom).
    pub no_tools: bool,
    /// Disable built-in tools but keep extension/custom tools.
    pub no_builtin_tools: bool,
    /// Allowlist of tool names or `*`/`?` globs; empty means all.
    pub allow: Vec<String>,
    /// Offline mode: web tools are not offered.
    pub offline: bool,
}

impl ToolSelection {
    /// Parse a comma-separated `--tools` value (repeatable flag values are
    /// concatenated by the caller).
    pub fn parse_allow_list(value: &str) -> Vec<String> {
        value
            .split(',')
            .map(str::trim)
            .filter(|s| !s.is_empty())
            .map(str::to_string)
            .collect()
    }

    /// Whether the allowlist admits `name` (or one of its archived aliases).
    pub fn admits(&self, name: &str) -> bool {
        if self.allow.is_empty() {
            return true;
        }
        let matches = |candidate: &str| {
            self.allow.iter().any(|pattern| {
                pattern == candidate
                    || (pattern.contains(['*', '?']) && wildcard_match(pattern, candidate))
            })
        };
        matches(name) || tool_aliases(name).any(matches)
    }
}

/// `*` / `?` glob match used by `--tools` (archived `wildcard_match`).
pub fn wildcard_match(pattern: &str, value: &str) -> bool {
    let p: Vec<char> = pattern.chars().collect();
    let v: Vec<char> = value.chars().collect();
    let (mut pi, mut vi) = (0usize, 0usize);
    let mut star: Option<usize> = None;
    let mut mark = 0usize;
    while vi < v.len() {
        if pi < p.len() && (p[pi] == '?' || p[pi] == v[vi]) {
            pi += 1;
            vi += 1;
        } else if pi < p.len() && p[pi] == '*' {
            star = Some(pi);
            pi += 1;
            mark = vi;
        } else if let Some(s) = star {
            pi = s + 1;
            mark += 1;
            vi = mark;
        } else {
            return false;
        }
    }
    while pi < p.len() && p[pi] == '*' {
        pi += 1;
    }
    pi == p.len()
}

impl ToolRegistry {
    /// Pi's default coding tools plus the read-only exploration tools.
    pub fn coding() -> Self {
        Self {
            tools: vec![
                Arc::new(ReadTool::new()),
                Arc::new(BashTool),
                Arc::new(EditTool),
                Arc::new(WriteTool),
                Arc::new(GrepTool),
                Arc::new(FindTool),
                Arc::new(LsTool),
            ],
        }
    }

    /// Every built-in tool of the archived qt binary (excluding the index,
    /// memory, MCP and subagent tools, which live in their own modules).
    pub fn builtin() -> Self {
        let mut registry = Self::coding();
        let extra: Vec<Arc<dyn Tool>> = vec![
            Arc::new(AppendTextTool),
            Arc::new(ApplyPatchTool),
            Arc::new(ClipboardTool::new()),
            Arc::new(CompletePathTool),
            Arc::new(PackageManagerTool),
            Arc::new(WebFetchTool),
            Arc::new(WebSearchTool),
            Arc::new(WebRunTool::new()),
        ];
        registry.tools.extend(extra);
        registry
    }

    /// Apply a CLI selection: built-ins (unless disabled) followed by
    /// `custom` tools (extensions, MCP proxies, subagents), filtered by the
    /// allowlist. With `offline`, web tools are dropped.
    pub fn select(selection: &ToolSelection, custom: Vec<Arc<dyn Tool>>) -> Self {
        if selection.no_tools {
            return Self::empty();
        }
        let mut registry = if selection.no_builtin_tools {
            Self::empty()
        } else {
            Self::builtin()
        };
        for tool in custom {
            registry = registry.with(tool);
        }
        registry.retain(|name| {
            selection.admits(name) && !(selection.offline && WEB_TOOL_NAMES.contains(&name))
        });
        registry
    }

    pub fn empty() -> Self {
        Self { tools: Vec::new() }
    }

    /// Add or replace a tool (by name).
    pub fn with(mut self, tool: Arc<dyn Tool>) -> Self {
        self.tools.retain(|t| t.name() != tool.name());
        self.tools.push(tool);
        self
    }

    /// Keep only tools whose name satisfies `keep` (used by `/tools` toggles).
    pub fn retain(&mut self, mut keep: impl FnMut(&str) -> bool) {
        self.tools.retain(|t| keep(t.name()));
    }

    /// A copy limited to the named tools (aliases accepted).
    pub fn only(&self, names: &[&str]) -> Self {
        let wanted: Vec<&str> = names.iter().map(|n| canonical_tool_name(n)).collect();
        Self {
            tools: self
                .tools
                .iter()
                .filter(|t| wanted.contains(&t.name()))
                .cloned()
                .collect(),
        }
    }

    pub fn names(&self) -> Vec<&'static str> {
        self.tools.iter().map(|t| t.name()).collect()
    }

    pub fn len(&self) -> usize {
        self.tools.len()
    }

    pub fn is_empty(&self) -> bool {
        self.tools.is_empty()
    }

    /// Look up a tool by model-facing name or archived alias.
    pub fn get(&self, name: &str) -> Option<Arc<dyn Tool>> {
        let canonical = canonical_tool_name(name);
        self.tools
            .iter()
            .find(|t| t.name() == name)
            .or_else(|| self.tools.iter().find(|t| t.name() == canonical))
            .cloned()
    }

    pub fn iter(&self) -> impl Iterator<Item = &Arc<dyn Tool>> {
        self.tools.iter()
    }

    pub fn specs(&self) -> Vec<ToolSpec> {
        self.tools.iter().map(|t| t.spec()).collect()
    }

    /// Run `name`: resolve aliases, normalise argument aliases, execute and
    /// apply the output guard. Unknown tools are reported to the model.
    pub async fn execute(&self, name: &str, args: Value, ctx: ToolContext) -> ToolOutput {
        let Some(tool) = self.get(name) else {
            return ToolOutput::error(format!("Tool {name} not found"));
        };
        let args = normalize_arguments(tool.name(), args);
        let guard = ctx.output_guard;
        let mut output = tool.execute(args, ctx).await;
        apply_output_guard(&mut output, guard);
        output
    }

    /// Execute a mutation the user approved from the queue in `ctx`.
    pub async fn execute_approved(
        &self,
        mutation: &PendingMutation,
        ctx: ToolContext,
    ) -> ToolOutput {
        let ctx = ctx.with_approved_mutation(mutation.id.clone());
        self.execute(&mutation.tool_name, mutation.arguments.clone(), ctx)
            .await
    }
}

fn take_alias(args: &mut Map<String, Value>, canonical: &str, aliases: &[&str]) {
    if !args.contains_key(canonical) {
        for alias in aliases {
            if let Some(value) = args.get(*alias) {
                let value = value.clone();
                args.insert(canonical.to_string(), value);
                break;
            }
        }
    }
    for alias in aliases {
        args.remove(*alias);
    }
}

/// Map alternative argument spellings used by other agents (Claude Code,
/// opencode, Crush, Hermes) onto the advertised schema.
pub fn normalize_arguments(tool: &str, args: Value) -> Value {
    let Value::Object(mut map) = args else {
        return args;
    };
    match canonical_tool_name(tool) {
        "read" => take_alias(&mut map, "path", &["file_path", "filePath"]),
        "write" => {
            take_alias(&mut map, "path", &["file_path", "filePath"]);
            take_alias(&mut map, "content", &["text"]);
        }
        "append_text" => take_alias(&mut map, "text", &["content"]),
        "edit" => normalize_edit(&mut map),
        "grep" => {
            take_alias(&mut map, "pattern", &["query"]);
            take_alias(&mut map, "glob", &["file_glob", "include"]);
            take_alias(&mut map, "ignoreCase", &["-i", "ignore_case"]);
            take_alias(&mut map, "literal", &["literal_text"]);
            take_alias(&mut map, "context", &["-C"]);
        }
        "find" => take_alias(&mut map, "pattern", &["glob", "include"]),
        "ls" => {
            if let Some(Value::String(path)) = map.get("path")
                && matches!(path.as_str(), "list_files" | "list_directory" | "files")
            {
                map.insert("path".into(), Value::String(".".into()));
            }
        }
        _ => {}
    }
    Value::Object(map)
}

fn normalize_edit(map: &mut Map<String, Value>) {
    take_alias(map, "path", &["file_path", "filePath"]);
    let top_replace_all = map.get("replace_all").cloned();
    if !map.contains_key("edits") {
        let mut edit = Map::new();
        for name in ["oldText", "old_string", "oldString"] {
            if let Some(v) = map.get(name) {
                edit.insert("oldText".into(), v.clone());
                break;
            }
        }
        for name in ["newText", "new_string", "newString"] {
            if let Some(v) = map.get(name) {
                edit.insert("newText".into(), v.clone());
                break;
            }
        }
        if !edit.is_empty() {
            map.insert("edits".into(), Value::Array(vec![Value::Object(edit)]));
        }
    }
    if let Some(Value::Array(edits)) = map.get_mut("edits") {
        for edit in edits.iter_mut() {
            let Value::Object(edit) = edit else { continue };
            take_alias(edit, "oldText", &["old_string", "oldString"]);
            take_alias(edit, "newText", &["new_string", "newString"]);
            if let Some(all) = &top_replace_all
                && !edit.contains_key("replace_all")
            {
                edit.insert("replace_all".into(), all.clone());
            }
        }
    }
    for legacy in [
        "oldText",
        "newText",
        "old_string",
        "new_string",
        "oldString",
        "newString",
        "replace_all",
    ] {
        map.remove(legacy);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn wildcard_matching() {
        assert!(wildcard_match("web_*", "web_fetch"));
        assert!(wildcard_match("*", "anything"));
        assert!(wildcard_match("r?ad", "read"));
        assert!(!wildcard_match("web_*", "bash"));
        assert!(wildcard_match("*_text", "append_text"));
        assert!(!wildcard_match("read", "read_text"));
    }

    #[test]
    fn selection_filters_and_resolves_aliases() {
        let sel = ToolSelection {
            allow: ToolSelection::parse_allow_list("read_text, web_*,grep"),
            ..Default::default()
        };
        let reg = ToolRegistry::select(&sel, Vec::new());
        assert_eq!(
            reg.names(),
            ["read", "grep", "web_fetch", "web_search", "web_run"]
        );
        let reg = ToolRegistry::select(
            &ToolSelection {
                offline: true,
                ..Default::default()
            },
            Vec::new(),
        );
        assert!(reg.get("web_fetch").is_none());
        assert!(reg.get("apply_patch").is_some());
        let reg = ToolRegistry::select(
            &ToolSelection {
                no_tools: true,
                ..Default::default()
            },
            Vec::new(),
        );
        assert!(reg.is_empty());
        let custom: Arc<dyn Tool> = Arc::new(super::super::LsTool);
        let reg = ToolRegistry::select(
            &ToolSelection {
                no_builtin_tools: true,
                ..Default::default()
            },
            vec![custom],
        );
        assert_eq!(reg.names(), ["ls"]);
        let reg = ToolRegistry::builtin();
        assert_eq!(reg.get("search_text").unwrap().name(), "grep");
        assert_eq!(reg.get("list_directory").unwrap().name(), "ls");
        assert_eq!(reg.only(&["read_text", "bash"]).names(), ["read", "bash"]);
    }

    #[test]
    fn normalizes_argument_aliases() {
        assert_eq!(
            normalize_arguments("read_text", json!({"file_path": "a"})),
            json!({"path": "a"})
        );
        assert_eq!(
            normalize_arguments("write", json!({"filePath": "a", "text": "x"})),
            json!({"path": "a", "content": "x"})
        );
        assert_eq!(
            normalize_arguments(
                "edit",
                json!({"file_path": "a", "old_string": "x", "new_string": "y", "replace_all": true})
            ),
            json!({"path": "a", "edits": [{"oldText": "x", "newText": "y", "replace_all": true}]})
        );
        assert_eq!(
            normalize_arguments(
                "edit",
                json!({"path": "a", "edits": [{"oldString": "x", "newString": "y"}]})
            ),
            json!({"path": "a", "edits": [{"oldText": "x", "newText": "y"}]})
        );
        assert_eq!(
            normalize_arguments("search_text", json!({"query": "q", "include": "*.rs"})),
            json!({"pattern": "q", "glob": "*.rs"})
        );
        assert_eq!(
            normalize_arguments("find", json!({"glob": "*.rs"})),
            json!({"pattern": "*.rs"})
        );
        assert_eq!(
            normalize_arguments("ls", json!({"path": "list_files"})),
            json!({"path": "."})
        );
        assert_eq!(normalize_arguments("bash", Value::Null), Value::Null);
    }

    #[test]
    fn read_only_tools_are_parallel_safe() {
        let reg = ToolRegistry::builtin();
        let safe: Vec<_> = reg
            .iter()
            .filter(|t| t.parallel_safe())
            .map(|t| t.name())
            .collect();
        assert_eq!(
            safe,
            [
                "read",
                "grep",
                "find",
                "ls",
                "complete_path",
                "package_manager",
                "web_fetch",
                "web_search"
            ]
        );
    }

    #[tokio::test]
    async fn unknown_tool_and_bad_args_are_errors() {
        let reg = ToolRegistry::coding();
        let ctx = ToolContext::new(std::env::temp_dir());
        let out = reg.execute("nope", Value::Null, ctx.clone()).await;
        assert!(out.is_error);
        let out = reg.execute("read", Value::String("{bad".into()), ctx).await;
        assert!(out.is_error);
        assert!(out.text_content().contains("not valid JSON"));
        assert_eq!(
            reg.names(),
            ["read", "bash", "edit", "write", "grep", "find", "ls"]
        );
    }

    #[tokio::test]
    async fn queued_mutations_run_after_approval() {
        use super::super::{MutationPolicy, MutationQueue};
        let dir = tempfile::tempdir().unwrap();
        let queue = Arc::new(MutationQueue::new());
        let reg = ToolRegistry::coding();
        let ctx = ToolContext::new(dir.path())
            .with_mutation_policy(MutationPolicy::Queue, queue.clone())
            .with_call_id("call-1");
        let out = reg
            .execute(
                "write_text",
                json!({"file_path": "a.txt", "text": "hi"}),
                ctx.clone(),
            )
            .await;
        assert!(out.is_error);
        let details = out.details.unwrap();
        assert_eq!(details["status"], "pending_approval");
        assert_eq!(details["mutation_id"], "mutation-1");
        assert!(!dir.path().join("a.txt").exists());
        let pending = queue.pending();
        assert_eq!(pending[0].summary, "write 2 bytes to a.txt");
        assert_eq!(pending[0].call_id.as_deref(), Some("call-1"));

        // Re-running with an unapproved id is refused.
        let out = reg
            .execute(
                "write",
                json!({"path": "a.txt", "content": "hi"}),
                ctx.clone().with_approved_mutation("mutation-1"),
            )
            .await;
        assert_eq!(out.text_content(), "mutation approval was not granted");

        let approved = queue.approve("mutation-1").unwrap();
        let out = reg.execute_approved(&approved, ctx.clone()).await;
        assert!(!out.is_error, "{}", out.text_content());
        assert_eq!(
            std::fs::read_to_string(dir.path().join("a.txt")).unwrap(),
            "hi"
        );

        // Rejected mutations never run.
        let out = reg
            .execute("bash", json!({"command": "touch b"}), ctx.clone())
            .await;
        let id = out.details.unwrap()["mutation_id"]
            .as_str()
            .unwrap()
            .to_string();
        assert!(queue.reject(&id));
        assert!(queue.is_empty());
        assert!(!dir.path().join("b").exists());
    }
}
