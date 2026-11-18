//! The `subagent` tool (archived `SubagentTool`).
//!
//! Synchronous modes delegate to isolated child processes and wait for them:
//! single `{agent, task}`, ordered parallel `{tasks}` (results keep source
//! order, bounded concurrency) and sequential `{chain}` with `{previous}` /
//! `{task}` templates. `action=list` discovers agents. With a
//! [`SubagentCoordinator`] the persistent worker actions `spawn`, `send`,
//! `follow_up`, `wait`, `interrupt` and `list_workers` are available too.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use futures::StreamExt as _;
use futures::future::BoxFuture;
use serde_json::{Map, Value, json};

use qt_core::provider::ModelRef;
use qt_core::tools::{
    MutationKind, MutationPolicy, PendingMutation, Tool, ToolContext, ToolOutput,
    apply_output_guard, canonical_tool_name,
};

use crate::coordinator::{ForkMode, PersistentRequest, SubagentCoordinator};
use crate::definitions::{
    SubagentDefinition, SubagentDiscovery, SubagentSource, discover_subagents,
};
use crate::runner::{ChildOutcome, ChildRequest, ChildRunner, ChildUpdate, ProcessChildRunner};

/// Hard cap on parallel children per call.
pub const MAX_CONCURRENCY: usize = 32;
/// Default parallel children per call.
pub const DEFAULT_CONCURRENCY: usize = 4;
/// Default limit of tasks in one parallel call.
pub const DEFAULT_MAX_PARALLEL_TASKS: usize = 8;
/// Default maximum nesting depth of subagents.
pub const DEFAULT_MAX_DEPTH: usize = 2;
/// Longest `wait` accepted from the model.
pub const MAX_WAIT_MS: i64 = 60_000;

pub const DESCRIPTION: &str = "Delegate work to isolated named child agents. Use exactly one execution mode: single {agent, task}, ordered parallel {tasks}, or sequential {chain}; action=list discovers configured agents. User agents are loaded from the global agents directory. Project agents are repo-controlled and require explicit scope and trust confirmation bypass.";

const ALLOWED_ARGUMENTS: [&str; 16] = [
    "action",
    "agent",
    "task",
    "tasks",
    "chain",
    "concurrency",
    "agentScope",
    "confirmProjectAgents",
    "cwd",
    "model",
    "name",
    "fork",
    "target",
    "message",
    "timeout_ms",
    "wake",
];

/// What the parent runtime currently allows, used to bound persistent workers.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ParentState {
    /// Active tool names of the parent; `None` when unrestricted.
    pub active_tools: Option<Vec<String>>,
    /// The parent's selected model, inherited by spawned workers.
    pub model: Option<ModelRef>,
}

pub type ParentStateFn = Arc<dyn Fn() -> ParentState + Send + Sync>;

/// Configuration of [`SubagentTool`].
#[derive(Clone)]
pub struct SubagentToolOptions {
    /// Agent home: user definitions in `<agent_home>/agents`, passed to
    /// children as `QT_CODING_AGENT_DIR`.
    pub agent_home: PathBuf,
    pub max_parallel_tasks: usize,
    /// Default parallel concurrency (clamped to 1..=32).
    pub default_concurrency: usize,
    /// Depth of this process (`QT_SUBAGENT_DEPTH`, 0 for the top level).
    pub depth: usize,
    /// Maximum depth (`QT_SUBAGENT_MAX_DEPTH`, default 2).
    pub max_depth: usize,
    pub runner: Arc<dyn ChildRunner>,
    pub coordinator: Option<Arc<SubagentCoordinator>>,
    pub parent: Option<ParentStateFn>,
}

fn env_depth(name: &str) -> Option<usize> {
    std::env::var(name)
        .ok()
        .filter(|v| !v.is_empty())
        .and_then(|v| crate::definitions::parse_nonnegative(&v))
}

impl SubagentToolOptions {
    /// Defaults with depth limits from the environment and the qt CLI
    /// discovered by [`ProcessChildRunner::discover`].
    pub fn new(agent_home: impl Into<PathBuf>) -> Self {
        Self {
            agent_home: agent_home.into(),
            max_parallel_tasks: DEFAULT_MAX_PARALLEL_TASKS,
            default_concurrency: DEFAULT_CONCURRENCY,
            depth: env_depth("QT_SUBAGENT_DEPTH").unwrap_or(0),
            max_depth: env_depth("QT_SUBAGENT_MAX_DEPTH").unwrap_or(DEFAULT_MAX_DEPTH),
            runner: Arc::new(ProcessChildRunner::discover()),
            coordinator: None,
            parent: None,
        }
    }

    pub fn with_runner(mut self, runner: Arc<dyn ChildRunner>) -> Self {
        self.runner = runner;
        self
    }

    pub fn with_coordinator(mut self, coordinator: Arc<SubagentCoordinator>) -> Self {
        self.coordinator = Some(coordinator);
        self
    }

    pub fn with_parent_state(mut self, parent: ParentStateFn) -> Self {
        self.parent = Some(parent);
        self
    }

    pub fn with_depth(mut self, depth: usize, max_depth: usize) -> Self {
        self.depth = depth;
        self.max_depth = max_depth;
        self
    }
}

/// The `subagent` tool.
#[derive(Clone)]
pub struct SubagentTool {
    options: Arc<SubagentToolOptions>,
}

impl SubagentTool {
    pub fn new(mut options: SubagentToolOptions) -> Self {
        options.default_concurrency = options.default_concurrency.clamp(1, MAX_CONCURRENCY);
        options.max_parallel_tasks = options.max_parallel_tasks.max(1);
        Self {
            options: Arc::new(options),
        }
    }

    pub fn options(&self) -> &SubagentToolOptions {
        &self.options
    }
}

impl Tool for SubagentTool {
    fn name(&self) -> &'static str {
        "subagent"
    }

    fn description(&self) -> String {
        DESCRIPTION.into()
    }

    fn snippet(&self) -> &'static str {
        "Delegate tasks to isolated child agents (single, parallel or chain)"
    }

    fn parameters(&self) -> Value {
        parameters_schema(
            self.options.max_parallel_tasks,
            self.options.coordinator.is_some(),
        )
    }

    fn execute(&self, args: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        let options = self.options.clone();
        Box::pin(async move {
            let mut output = execute(&options, args, &ctx).await;
            apply_output_guard(&mut output, ctx.output_guard);
            output
        })
    }
}

/// JSON schema of the tool arguments.
pub fn parameters_schema(max_parallel_tasks: usize, persistent: bool) -> Value {
    let task_schema = json!({
        "type": "object",
        "properties": {
            "agent": {"type": "string", "description": "Configured agent runtime name."},
            "task": {"type": "string", "description": "Task delegated to this child."},
            "cwd": {"type": "string", "description": "Child cwd inside the parent workspace."},
            "model": {"type": "string", "description": "Optional model override for this child."}
        },
        "required": ["agent", "task"],
        "additionalProperties": false
    });
    let actions = if persistent {
        json!([
            "list",
            "spawn",
            "send",
            "follow_up",
            "wait",
            "interrupt",
            "list_workers"
        ])
    } else {
        json!(["list"])
    };
    let mut schema = json!({
        "type": "object",
        "properties": {
            "action": {
                "type": "string",
                "enum": actions,
                "description": "Use list to inspect executable agent definitions."
            },
            "agent": {"type": "string", "description": "Agent runtime name for single mode."},
            "task": {
                "type": "string",
                "description": "Single-mode task, or original {task} value for a chain."
            },
            "tasks": {
                "type": "array",
                "items": task_schema.clone(),
                "minItems": 1,
                "maxItems": max_parallel_tasks,
                "description": "Parallel tasks; results retain this source order."
            },
            "chain": {
                "type": "array",
                "items": task_schema,
                "minItems": 1,
                "description": "Sequential tasks. Templates may use {previous} and {task}."
            },
            "concurrency": {
                "type": "integer",
                "minimum": 1,
                "description": "Parallel worker limit; default 4."
            },
            "agentScope": {
                "type": "string",
                "enum": ["user", "project", "both"],
                "default": "user",
                "description": "Discovery scope; project roles are repo-controlled."
            },
            "confirmProjectAgents": {
                "type": "boolean",
                "default": true,
                "description": "Keep true until project role files have been inspected and trusted."
            },
            "cwd": {"type": "string", "description": "Default child cwd inside the parent workspace."},
            "model": {"type": "string", "description": "Default child model override."}
        },
        "additionalProperties": false
    });
    if persistent {
        let properties = schema["properties"].as_object_mut().expect("properties");
        properties.insert("name".into(), json!({"type": "string"}));
        properties.insert(
            "fork".into(),
            json!({
                "oneOf": [
                    {"type": "string", "enum": ["none", "all"]},
                    {"type": "integer", "minimum": 1}
                ],
                "description": "none, all, or a positive user-turn count."
            }),
        );
        properties.insert("target".into(), json!({"type": "string"}));
        properties.insert("message".into(), json!({"type": "string"}));
        properties.insert(
            "timeout_ms".into(),
            json!({"type": "integer", "minimum": 0, "maximum": MAX_WAIT_MS}),
        );
        properties.insert("wake".into(), json!({"type": "boolean"}));
    }
    schema
}

fn failure(status: &str, message: impl Into<String>) -> ToolOutput {
    ToolOutput::error(message).with_details(json!({ "status": status }))
}

fn invalid(message: impl Into<String>) -> ToolOutput {
    failure("invalid_request", message)
}

fn json_output(value: Value) -> ToolOutput {
    let text = serde_json::to_string_pretty(&value).unwrap_or_default();
    ToolOutput::text(text).with_details(value)
}

/// Gate a delegation behind the mutation queue (archived
/// `require_mutation_approval` with kind `command`).
fn require_approval(ctx: &ToolContext, summary: String, arguments: &Value) -> Option<ToolOutput> {
    if ctx.mutations.policy == MutationPolicy::Execute {
        return None;
    }
    let Some(queue) = ctx.mutations.queue.as_ref() else {
        return Some(ToolOutput::error(
            "mutation queue is required by tool context",
        ));
    };
    if let Some(id) = &ctx.mutations.approved_id {
        if queue.take_approval(id) {
            return None;
        }
        return Some(
            ToolOutput::error("mutation approval was not granted").with_details(json!({
                "status": "permission_denied",
                "mutation_id": id,
            })),
        );
    }
    let id = queue.enqueue(PendingMutation {
        id: String::new(),
        kind: MutationKind::Command,
        tool_name: "subagent".into(),
        call_id: ctx.call_id.clone(),
        summary: summary.clone(),
        arguments: arguments.clone(),
    });
    Some(
        ToolOutput::error(format!(
            "Pending approval: {summary}. The change was not applied; it is queued as {id} until the user approves it."
        ))
        .with_details(json!({
            "status": "pending_approval",
            "mutation_id": id,
            "queued": true,
            "kind": MutationKind::Command.as_str(),
            "tool_name": "subagent",
        })),
    )
}

fn make_run_id() -> String {
    static SEQUENCE: AtomicU64 = AtomicU64::new(1);
    format!(
        "subagent-{}-{}-{}",
        std::process::id(),
        chrono::Utc::now().timestamp_millis(),
        SEQUENCE.fetch_add(1, Ordering::Relaxed)
    )
}

fn display(path: &Path) -> String {
    path.to_string_lossy().replace('\\', "/")
}

/// Resolve a child cwd: relative paths are taken from the workspace root;
/// the result must exist and satisfy the workspace policy.
fn resolve_child_cwd(ctx: &ToolContext, requested: Option<&str>) -> Result<PathBuf, ToolOutput> {
    let path = match requested.filter(|r| !r.is_empty()) {
        Some(raw) => {
            let raw = PathBuf::from(raw);
            if raw.is_relative() {
                ctx.policy.root().join(raw)
            } else {
                raw
            }
        }
        None => ctx.cwd.clone(),
    };
    match std::fs::canonicalize(&path) {
        Ok(canonical) if ctx.policy.permits(&canonical) => Ok(canonical),
        _ => Err(failure(
            "permission_denied",
            "subagent cwd escapes workspace root",
        )),
    }
}

fn model_with_thinking(model: Option<String>, agent: &SubagentDefinition) -> Option<String> {
    let mut model = model?;
    if let Some(thinking) = &agent.thinking
        && !model.contains(':')
        && thinking != "off"
    {
        model = format!("{model}:{thinking}");
    }
    Some(model)
}

fn child_max_depth(agent: &SubagentDefinition, configured: usize) -> usize {
    agent
        .max_subagent_depth
        .map_or(configured, |own| own.min(configured))
}

/// The shared isolation flags: context files, skills and tools.
fn isolation_arguments(agent: &SubagentDefinition, tools: &[String], out: &mut Vec<String>) {
    if !agent.inherit_project_context {
        out.push("--no-context-files".into());
    }
    if !agent.inherit_skills {
        out.push("--no-skills".into());
    }
    if tools.is_empty() {
        out.push("--no-tools".into());
    } else {
        out.push("--tools".into());
        out.push(tools.join(","));
    }
}

fn system_prompt_arguments(agent: &SubagentDefinition, out: &mut Vec<String>) {
    if !agent.system_prompt.is_empty() {
        out.push(agent.system_prompt_mode.flag().into());
        out.push(agent.system_prompt.clone());
    }
}

/// Arguments and environment for a synchronous child.
#[allow(clippy::too_many_arguments)]
pub fn sync_child_request(
    agent: &SubagentDefinition,
    task: &str,
    model: Option<&str>,
    cwd: PathBuf,
    agent_home: &Path,
    run_id: &str,
    depth: usize,
    max_depth: usize,
    allow_external: bool,
) -> ChildRequest {
    let mut arguments: Vec<String> = ["--mode", "json", "--no-session", "--no-extensions"]
        .iter()
        .map(|s| s.to_string())
        .collect();
    isolation_arguments(agent, &agent.tools, &mut arguments);
    if let Some(model) = model.filter(|m| !m.is_empty()) {
        arguments.push("--model".into());
        arguments.push(model.into());
    }
    system_prompt_arguments(agent, &mut arguments);
    arguments.push(format!("Task: {task}"));
    let mut request = ChildRequest {
        arguments,
        cwd: Some(cwd),
        ..Default::default()
    };
    request.set_env("QT_SUBAGENT_CHILD", "1");
    request.set_env("QT_SUBAGENT_DEPTH", (depth + 1).to_string());
    request.set_env(
        "QT_SUBAGENT_MAX_DEPTH",
        child_max_depth(agent, max_depth).to_string(),
    );
    request.set_env(
        "QT_SUBAGENT_ALLOW_EXTERNAL",
        if allow_external { "1" } else { "0" },
    );
    request.set_env("QT_SUBAGENT_RUN_ID", run_id);
    request.set_env("QT_SUBAGENT_NAME", agent.name.clone());
    request.set_env("QT_CODING_AGENT_DIR", display(agent_home));
    request
}

#[derive(Debug, Clone)]
struct RequestedTask {
    agent: String,
    task: String,
    cwd: Option<String>,
    model: Option<String>,
}

#[derive(Debug, Clone, Default)]
struct ChildResult {
    json: Value,
    success: bool,
    output: String,
}

fn parse_task(
    value: &Value,
    default_cwd: &Option<String>,
    default_model: &Option<String>,
) -> Option<RequestedTask> {
    let object = value.as_object()?;
    if object
        .keys()
        .any(|k| !matches!(k.as_str(), "agent" | "task" | "cwd" | "model"))
    {
        return None;
    }
    let mut task = RequestedTask {
        agent: object.get("agent")?.as_str()?.to_string(),
        task: object.get("task")?.as_str()?.to_string(),
        cwd: default_cwd.clone(),
        model: default_model.clone(),
    };
    if let Some(cwd) = object.get("cwd") {
        task.cwd = Some(cwd.as_str()?.to_string());
    }
    if let Some(model) = object.get("model") {
        task.model = Some(model.as_str()?.to_string());
    }
    if task.agent.trim().is_empty() || task.task.trim().is_empty() {
        return None;
    }
    Some(task)
}

fn progress_text(mode: &str, states: &[Value]) -> String {
    let mut lines = Vec::new();
    for (index, state) in states.iter().enumerate() {
        let agent = state.get("agent").and_then(Value::as_str).unwrap_or("");
        let status = state.get("status").and_then(Value::as_str).unwrap_or("");
        let prefix = if mode == "single" {
            format!("{agent} [{status}]")
        } else {
            format!("{}. {agent} [{status}]", index + 1)
        };
        lines.push(prefix);
        let detail = state
            .get("streaming_output")
            .or_else(|| state.get("output"))
            .or_else(|| state.get("error"))
            .and_then(Value::as_str)
            .unwrap_or("");
        if !detail.is_empty() {
            for line in detail
                .lines()
                .rev()
                .take(3)
                .collect::<Vec<_>>()
                .into_iter()
                .rev()
            {
                lines.push(format!("  {line}"));
            }
        }
    }
    lines.join("\n")
}

struct RunContext<'a> {
    options: &'a SubagentToolOptions,
    ctx: &'a ToolContext,
    agents: &'a BTreeMap<String, SubagentDefinition>,
    run_id: String,
    mode: &'static str,
    states: Mutex<Vec<Value>>,
}

impl RunContext<'_> {
    fn set_state(&self, index: usize, value: Value) {
        if let Ok(mut states) = self.states.lock() {
            states[index] = value;
        }
        self.emit_update();
    }

    fn emit_update(&self) {
        let Some(progress) = &self.ctx.progress else {
            return;
        };
        let snapshot = match self.states.lock() {
            Ok(states) => states.clone(),
            Err(_) => return,
        };
        progress(progress_text(self.mode, &snapshot));
    }

    async fn run_child(&self, task: &RequestedTask, index: usize) -> ChildResult {
        let agent = &self.agents[&task.agent];
        let failed = |error: String, cwd: Option<&Path>| {
            let mut json = json!({
                "agent": task.agent,
                "task": task.task,
                "status": "failed",
                "exit_code": 1,
                "error": error,
            });
            if let Some(cwd) = cwd {
                json["agent_source"] = json!(agent.source.as_str());
                json["cwd"] = json!(display(cwd));
            }
            ChildResult {
                json,
                success: false,
                output: String::new(),
            }
        };
        let cwd = match resolve_child_cwd(self.ctx, task.cwd.as_deref()) {
            Ok(cwd) => cwd,
            Err(_) => return failed("subagent cwd escapes workspace root".into(), None),
        };
        let model = model_with_thinking(task.model.clone().or_else(|| agent.model.clone()), agent);
        let request = sync_child_request(
            agent,
            &task.task,
            model.as_deref(),
            cwd.clone(),
            &self.options.agent_home,
            &self.run_id,
            self.options.depth,
            self.options.max_depth,
            self.ctx.policy.allows_external_access(),
        );
        {
            if let Ok(mut states) = self.states.lock() {
                states[index]["status"] = json!("running");
            }
            self.emit_update();
        }
        let outcome = match self.run_with_updates(request, index).await {
            Ok(outcome) => outcome,
            Err(error) => return failed(error, Some(&cwd)),
        };
        let output = outcome.final_output.clone();
        let success = outcome.completed() && !output.is_empty();
        let mut json = json!({
            "agent": task.agent,
            "agent_source": agent.source.as_str(),
            "task": task.task,
            "status": if success { "completed" } else { "failed" },
            "exit_code": outcome.exit_code,
            "cancelled": outcome.cancelled,
            "timed_out": outcome.timed_out,
            "messages": outcome.final_messages,
            "output": output,
            "stderr": outcome.stderr,
            "cwd": display(&cwd),
            "model": model.clone().unwrap_or_default(),
        });
        if !success && output.is_empty() {
            json["error"] = json!(if outcome.stderr.is_empty() {
                "child exited without a final assistant message".to_string()
            } else {
                outcome.stderr.clone()
            });
        }
        ChildResult {
            json,
            success,
            output,
        }
    }

    async fn run_with_updates(
        &self,
        request: ChildRequest,
        index: usize,
    ) -> Result<ChildOutcome, String> {
        // Streaming updates are forwarded through a channel so the callback
        // stays 'static while the states live on this call's stack.
        let (tx, mut rx) = tokio::sync::mpsc::unbounded_channel::<ChildUpdate>();
        let callback: crate::runner::ChildUpdateFn = Arc::new(move |update| {
            let _ = tx.send(update);
        });
        let run = self
            .options
            .runner
            .run(request, self.ctx.cancel.clone(), Some(callback));
        tokio::pin!(run);
        loop {
            tokio::select! {
                result = &mut run => {
                    while let Ok(update) = rx.try_recv() {
                        self.apply_update(index, update);
                    }
                    return result;
                }
                Some(update) = rx.recv() => self.apply_update(index, update),
            }
        }
    }

    fn apply_update(&self, index: usize, update: ChildUpdate) {
        if let Ok(mut states) = self.states.lock() {
            states[index]["status"] = json!("running");
            if !update.streaming_output.is_empty() {
                states[index]["streaming_output"] = json!(update.streaming_output);
            }
        }
        self.emit_update();
    }
}

async fn execute(options: &SubagentToolOptions, args: Value, ctx: &ToolContext) -> ToolOutput {
    let args = match args {
        Value::Null => Map::new(),
        Value::Object(map) => map,
        _ => return invalid("invalid subagent tool request"),
    };
    if args
        .keys()
        .any(|k| !ALLOWED_ARGUMENTS.contains(&k.as_str()))
    {
        return invalid("subagent received unsupported arguments");
    }
    if args.get("agentScope").is_some_and(|v| !v.is_string()) {
        return invalid("subagent agentScope must be a string");
    }
    if args
        .get("confirmProjectAgents")
        .is_some_and(|v| !v.is_boolean())
    {
        return invalid("subagent confirmProjectAgents must be a boolean");
    }
    if args.get("task").is_some_and(|v| !v.is_string()) {
        return invalid("subagent task must be a string");
    }
    for key in ["name", "fork", "target", "message"] {
        if let Some(value) = args.get(key)
            && !value.is_string()
            && !(key == "fork" && (value.is_i64() || value.is_u64()))
        {
            return invalid(format!("subagent {key} must be a string"));
        }
    }
    if args.get("model").is_some_and(|v| !v.is_string()) {
        return invalid("subagent model must be a string");
    }
    if args.get("wake").is_some_and(|v| !v.is_boolean()) {
        return invalid("subagent wake must be a boolean");
    }
    if args
        .get("timeout_ms")
        .is_some_and(|v| !(v.is_i64() || v.is_u64()))
    {
        return invalid("subagent timeout_ms must be an integer");
    }
    let scope = args
        .get("agentScope")
        .and_then(Value::as_str)
        .unwrap_or("user")
        .to_string();
    if !matches!(scope.as_str(), "user" | "project" | "both") {
        return invalid("agentScope must be user, project, or both");
    }
    let discovery = discover_subagents(&options.agent_home, &ctx.cwd, &scope);
    let agents: BTreeMap<String, SubagentDefinition> = discovery
        .agents
        .iter()
        .map(|a| (a.name.clone(), a.clone()))
        .collect();
    let confirm_project = args
        .get("confirmProjectAgents")
        .and_then(Value::as_bool)
        .unwrap_or(true);

    if let Some(action) = args.get("action") {
        let Some(action) = action.as_str() else {
            return invalid("subagent action must be a string");
        };
        return run_action(
            options,
            ctx,
            &args,
            action,
            &scope,
            &discovery,
            &agents,
            confirm_project,
        )
        .await;
    }

    if options.depth >= options.max_depth {
        return failure(
            "permission_denied",
            format!(
                "subagent depth limit reached at depth {} (max {})",
                options.depth, options.max_depth
            ),
        );
    }

    let non_empty_array = |key: &str| {
        args.get(key)
            .and_then(Value::as_array)
            .is_some_and(|a| !a.is_empty())
    };
    let has_single = args.contains_key("agent") && args.contains_key("task");
    let has_parallel = non_empty_array("tasks");
    let has_chain = non_empty_array("chain");
    if usize::from(has_single) + usize::from(has_parallel) + usize::from(has_chain) != 1 {
        return invalid("provide exactly one subagent mode: agent+task, tasks, or chain");
    }
    let has = |key: &str| args.contains_key(key);
    if (has_single && (has("tasks") || has("chain") || has("concurrency")))
        || (has_parallel && (has("agent") || has("task") || has("chain")))
        || (has_chain && (has("agent") || has("tasks") || has("concurrency")))
    {
        return invalid("subagent arguments from different execution modes cannot be combined");
    }

    let mut concurrency = options.default_concurrency;
    if let Some(value) = args.get("concurrency") {
        let Some(requested) = value.as_i64() else {
            return invalid("subagent concurrency must be an integer");
        };
        if requested < 1 {
            return invalid("subagent concurrency must be at least 1");
        }
        concurrency = requested as usize;
    }

    let default_cwd = match args.get("cwd") {
        None => None,
        Some(Value::String(s)) => Some(s.clone()),
        Some(_) => return invalid("subagent cwd must be a string"),
    };
    let default_model = args
        .get("model")
        .and_then(Value::as_str)
        .map(str::to_string);
    let mut tasks = Vec::new();
    if has_single {
        let value = json!({"agent": args["agent"], "task": args["task"]});
        match parse_task(&value, &default_cwd, &default_model) {
            Some(task) => tasks.push(task),
            None => {
                return invalid("single subagent mode requires non-empty string agent and task");
            }
        }
    } else {
        let key = if has_parallel { "tasks" } else { "chain" };
        for value in args[key].as_array().into_iter().flatten() {
            match parse_task(value, &default_cwd, &default_model) {
                Some(task) => tasks.push(task),
                None => {
                    return invalid(
                        "each subagent task requires agent and task strings and optional cwd/model",
                    );
                }
            }
        }
    }
    if has_parallel && tasks.len() > options.max_parallel_tasks {
        return invalid(format!(
            "too many parallel subagent tasks (max {})",
            options.max_parallel_tasks
        ));
    }
    for task in &tasks {
        let Some(agent) = agents.get(&task.agent) else {
            let available: Vec<&str> = agents.keys().map(String::as_str).collect();
            return invalid(format!(
                "unknown subagent '{}'. Available: {}",
                task.agent,
                if available.is_empty() {
                    "none".to_string()
                } else {
                    available.join(", ")
                }
            ));
        };
        if agent.source == SubagentSource::Project && confirm_project {
            let file = agent.file_path.as_deref().map(display).unwrap_or_default();
            return failure(
                "permission_denied",
                format!(
                    "project subagent '{}' requires trust confirmation. Inspect {file} and retry with confirmProjectAgents=false only for a trusted repository.",
                    task.agent
                ),
            );
        }
    }
    let raw_args = Value::Object(args.clone());
    if let Some(queued) = require_approval(
        ctx,
        format!(
            "delegate {} {} to isolated subagents",
            tasks.len(),
            if tasks.len() == 1 { "task" } else { "tasks" }
        ),
        &raw_args,
    ) {
        return queued;
    }

    let mode: &'static str = if has_parallel {
        "parallel"
    } else if has_chain {
        "chain"
    } else {
        "single"
    };
    let run = RunContext {
        options,
        ctx,
        agents: &agents,
        run_id: make_run_id(),
        mode,
        states: Mutex::new(
            tasks
                .iter()
                .map(|t| {
                    json!({"agent": t.agent, "task": t.task, "status": "pending", "exit_code": -1})
                })
                .collect(),
        ),
    };

    let mut results: Vec<Option<ChildResult>> = vec![None; tasks.len()];
    if has_chain {
        let original = args
            .get("task")
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_string();
        let mut previous = String::new();
        for index in 0..tasks.len() {
            tasks[index].task = tasks[index]
                .task
                .replace("{previous}", &previous)
                .replace("{task}", &original);
            let result = run.run_child(&tasks[index], index).await;
            run.set_state(index, result.json.clone());
            let success = result.success;
            previous = result.output.clone();
            results[index] = Some(result);
            if !success {
                break;
            }
        }
    } else if has_parallel {
        let limit = concurrency.min(tasks.len()).clamp(1, MAX_CONCURRENCY);
        let run_ref = &run;
        let jobs = tasks
            .iter()
            .cloned()
            .enumerate()
            .map(|(index, task)| async move {
                let result = run_ref.run_child(&task, index).await;
                run_ref.set_state(index, result.json.clone());
                (index, result)
            });
        let finished: Vec<(usize, ChildResult)> = futures::stream::iter(jobs)
            .buffer_unordered(limit)
            .collect()
            .await;
        for (index, result) in finished {
            results[index] = Some(result);
        }
    } else {
        let result = run.run_child(&tasks[0], 0).await;
        run.set_state(0, result.json.clone());
        results[0] = Some(result);
    }

    let executed: Vec<&ChildResult> = results.iter().map_while(Option::as_ref).collect();
    let completed = executed.iter().filter(|r| r.success).count();
    let text = if has_parallel {
        let mut sections = String::new();
        for (index, result) in executed.iter().enumerate() {
            sections.push_str(&format!(
                "=== Task {}: {} ===\n",
                index + 1,
                tasks[index].agent
            ));
            if result.output.is_empty() {
                sections.push_str(
                    result
                        .json
                        .get("error")
                        .and_then(Value::as_str)
                        .unwrap_or("(no output)"),
                );
            } else {
                sections.push_str(&result.output);
            }
            sections.push_str("\n\n");
        }
        format!(
            "{completed}/{} subagents completed\n\n{sections}",
            results.len()
        )
    } else {
        match executed.last() {
            Some(last) if !last.output.is_empty() => last.output.clone(),
            Some(last) => last
                .json
                .get("error")
                .and_then(Value::as_str)
                .unwrap_or("(no output)")
                .to_string(),
            None => String::new(),
        }
    };
    let text = text.trim().to_string();
    let success = completed == results.len() || has_parallel;
    let details = json!({
        "mode": mode,
        "run_id": run.run_id,
        "agent_scope": scope,
        "completed": completed,
        "total": results.len(),
        "results": executed.iter().map(|r| r.json.clone()).collect::<Vec<_>>(),
        "diagnostics": discovery.diagnostics,
        "output": text,
    });
    let output = if success {
        ToolOutput::text(text)
    } else {
        ToolOutput::error(text)
    };
    output.with_details(details)
}

#[allow(clippy::too_many_arguments)]
async fn run_action(
    options: &SubagentToolOptions,
    ctx: &ToolContext,
    args: &Map<String, Value>,
    action: &str,
    scope: &str,
    discovery: &SubagentDiscovery,
    agents: &BTreeMap<String, SubagentDefinition>,
    confirm_project: bool,
) -> ToolOutput {
    if !matches!(
        action,
        "list" | "spawn" | "send" | "follow_up" | "wait" | "interrupt" | "list_workers"
    ) {
        return invalid("unsupported subagent action");
    }
    if action == "list" {
        if args.len() != 1 && !(args.len() == 2 && args.contains_key("agentScope")) {
            return invalid("subagent action=list accepts only agentScope");
        }
        let listed: Vec<Value> = discovery
            .agents
            .iter()
            .map(|agent| {
                json!({
                    "name": agent.name,
                    "description": agent.description,
                    "source": agent.source.as_str(),
                    "model": agent.model.clone().unwrap_or_default(),
                    "tools": agent.tools,
                    "file": agent.file_path.as_deref().map(display).unwrap_or_default(),
                })
            })
            .collect();
        let summary = if listed.is_empty() {
            "No subagents configured.".to_string()
        } else {
            format!("Discovered {} subagents.", listed.len())
        };
        return json_output(json!({
            "mode": "management",
            "action": "list",
            "agent_scope": scope,
            "agents": listed,
            "diagnostics": discovery.diagnostics,
            "output": summary,
        }));
    }
    let Some(coordinator) = options.coordinator.as_ref() else {
        return failure(
            "execution_error",
            "persistent subagent coordinator is unavailable",
        );
    };
    let raw_args = Value::Object(args.clone());
    let reject_unexpected = |keys: &[&str]| -> Option<ToolOutput> {
        args.keys()
            .any(|k| !keys.contains(&k.as_str()))
            .then(|| invalid("unsupported arguments for persistent subagent action"))
    };
    let require_string = |key: &str| -> Option<String> {
        args.get(key)
            .and_then(Value::as_str)
            .filter(|s| !s.trim().is_empty())
            .map(str::to_string)
    };
    let exec_error = |message: String| failure("execution_error", message);
    let parent = options.parent.as_ref().map(|f| f()).unwrap_or_default();

    match action {
        "list_workers" => {
            if let Some(error) = reject_unexpected(&["action"]) {
                return error;
            }
            match coordinator.list() {
                Ok(value) => json_output(value),
                Err(e) => exec_error(e),
            }
        }
        "send" | "follow_up" => {
            if let Some(error) = reject_unexpected(&["action", "target", "message", "task", "wake"])
            {
                return error;
            }
            let payload_key = if action == "send" { "message" } else { "task" };
            let target = require_string("target");
            let payload = require_string(payload_key);
            let (Some(target), Some(payload)) = (target, payload) else {
                return invalid(format!(
                    "subagent {action} requires target and {payload_key}"
                ));
            };
            if (action == "send" && args.contains_key("task"))
                || (action == "follow_up" && args.contains_key("message"))
            {
                return invalid(format!(
                    "subagent {action} requires target and {payload_key}"
                ));
            }
            if let Some(queued) = require_approval(
                ctx,
                format!("message persistent subagent '{target}'"),
                &raw_args,
            ) {
                return queued;
            }
            let workers = match coordinator.list() {
                Ok(value) => value,
                Err(e) => return exec_error(e),
            };
            let worker = workers["children"]
                .as_array()
                .into_iter()
                .flatten()
                .find(|w| {
                    w.get("id").and_then(Value::as_str) == Some(target.as_str())
                        || w.get("name").and_then(Value::as_str) == Some(target.as_str())
                });
            let Some(worker) = worker else {
                return failure("not_found", format!("unknown child: {target}"));
            };
            if let Some(denied) = check_worker_permissions(worker, ctx, &parent) {
                return denied;
            }
            let wake =
                action == "follow_up" || args.get("wake").and_then(Value::as_bool).unwrap_or(false);
            match coordinator.send(&target, payload, wake) {
                Ok(value) => json_output(value),
                Err(e) => exec_error(e),
            }
        }
        "wait" | "interrupt" => {
            if let Some(error) = reject_unexpected(&["action", "target", "timeout_ms"]) {
                return error;
            }
            let Some(target) = require_string("target") else {
                return invalid(format!("subagent {action} requires target"));
            };
            if action == "interrupt" {
                if args.contains_key("timeout_ms") {
                    return invalid("interrupt does not accept timeout_ms");
                }
                return match coordinator.interrupt(&target) {
                    Ok(value) => json_output(value),
                    Err(e) => exec_error(e),
                };
            }
            let mut timeout = 0i64;
            if let Some(value) = args.get("timeout_ms") {
                let Some(requested) = value.as_i64() else {
                    return invalid("timeout_ms must be an integer");
                };
                if !(0..=MAX_WAIT_MS).contains(&requested) {
                    return invalid("timeout_ms must be between 0 and 60000");
                }
                timeout = requested;
            }
            let wait = coordinator.wait(&target, Duration::from_millis(timeout as u64));
            let result = tokio::select! {
                result = wait => result,
                _ = ctx.cancel.cancelled() => return ToolOutput::error("subagent wait was cancelled"),
            };
            match result {
                Ok(value) => json_output(value),
                Err(e) => exec_error(e),
            }
        }
        _ => spawn_worker(
            options,
            ctx,
            args,
            agents,
            confirm_project,
            coordinator,
            &parent,
            &raw_args,
        ),
    }
}

/// Re-check a worker's recorded permissions against the current context.
fn check_worker_permissions(
    worker: &Value,
    ctx: &ToolContext,
    parent: &ParentState,
) -> Option<ToolOutput> {
    let permissions = worker.get("permissions").cloned().unwrap_or(json!({}));
    let workspace = display(ctx.policy.root());
    if permissions.get("workspace").and_then(Value::as_str) != Some(workspace.as_str())
        || (permissions
            .get("allow_external")
            .and_then(Value::as_bool)
            .unwrap_or(false)
            && !ctx.policy.allows_external_access())
    {
        return Some(failure(
            "permission_denied",
            "persistent worker permissions exceed current tool context",
        ));
    }
    if let (Some(active), Some(tools)) = (
        &parent.active_tools,
        permissions.get("toolnames").and_then(Value::as_array),
    ) {
        for tool in tools {
            let allowed = tool.as_str().is_some_and(|tool| {
                active
                    .iter()
                    .any(|current| canonical_tool_name(current) == canonical_tool_name(tool))
            });
            if !allowed {
                return Some(failure(
                    "permission_denied",
                    "persistent worker tool permissions exceed current tool context",
                ));
            }
        }
    }
    None
}

#[allow(clippy::too_many_arguments)]
fn spawn_worker(
    options: &SubagentToolOptions,
    ctx: &ToolContext,
    args: &Map<String, Value>,
    agents: &BTreeMap<String, SubagentDefinition>,
    confirm_project: bool,
    coordinator: &SubagentCoordinator,
    parent: &ParentState,
    raw_args: &Value,
) -> ToolOutput {
    const KEYS: [&str; 9] = [
        "action",
        "agent",
        "task",
        "name",
        "fork",
        "agentScope",
        "confirmProjectAgents",
        "cwd",
        "model",
    ];
    if args.keys().any(|k| !KEYS.contains(&k.as_str())) {
        return invalid("unsupported arguments for persistent subagent action");
    }
    let string = |key: &str| {
        args.get(key)
            .and_then(Value::as_str)
            .filter(|s| !s.trim().is_empty())
            .map(str::to_string)
    };
    let (Some(agent_name), Some(task)) = (string("agent"), string("task")) else {
        return invalid("spawn requires non-empty agent and task");
    };
    let Some(agent) = agents.get(&agent_name) else {
        return invalid(format!("unknown subagent '{agent_name}'"));
    };
    let mut allowed_tools: Vec<String> = agent
        .tools
        .iter()
        .map(|t| canonical_tool_name(t).to_string())
        .collect();
    if let Some(active) = &parent.active_tools {
        allowed_tools.retain(|tool| {
            active
                .iter()
                .any(|current| canonical_tool_name(current) == tool.as_str())
        });
    }
    if agent.source == SubagentSource::Project && confirm_project {
        return failure(
            "permission_denied",
            format!("project subagent '{agent_name}' requires trust confirmation"),
        );
    }
    let child_max = child_max_depth(agent, options.max_depth);
    if options.depth >= options.max_depth {
        return failure(
            "permission_denied",
            format!(
                "subagent depth limit reached at depth {} (max {})",
                options.depth, options.max_depth
            ),
        );
    }
    if let Some(queued) = require_approval(
        ctx,
        format!("spawn persistent subagent '{agent_name}'"),
        raw_args,
    ) {
        return queued;
    }
    let workspace = ctx.policy.root().to_path_buf();
    let cwd = match resolve_child_cwd(ctx, args.get("cwd").and_then(Value::as_str)) {
        Ok(cwd) => cwd,
        Err(denied) => return denied,
    };
    let mut arguments: Vec<String> = ["--mode", "json", "--no-extensions"]
        .iter()
        .map(|s| s.to_string())
        .collect();
    isolation_arguments(agent, &allowed_tools, &mut arguments);
    let mut model = string("model").or_else(|| agent.model.clone());
    if model.is_none()
        && let Some(parent_model) = &parent.model
        && !parent_model.id.is_empty()
    {
        model = Some(parent_model.id.clone());
        if !parent_model.provider.is_empty() && parent_model.provider != "native" {
            arguments.push("--provider".into());
            arguments.push(parent_model.provider.clone());
        }
    }
    let model = model_with_thinking(model, agent);
    if let Some(model) = model.as_deref().filter(|m| !m.is_empty()) {
        arguments.push("--model".into());
        arguments.push(model.into());
    }
    system_prompt_arguments(agent, &mut arguments);
    let mut process = ChildRequest {
        arguments,
        cwd: Some(cwd),
        ..Default::default()
    };
    let allow_external = ctx.policy.allows_external_access();
    process.set_env("QT_SUBAGENT_CHILD", "1");
    process.set_env("QT_SUBAGENT_DEPTH", (options.depth + 1).to_string());
    process.set_env("QT_SUBAGENT_MAX_DEPTH", child_max.to_string());
    process.set_env(
        "QT_SUBAGENT_ALLOW_EXTERNAL",
        if allow_external { "1" } else { "0" },
    );
    process.set_env("QT_CODING_AGENT_DIR", display(&options.agent_home));
    let fork = match args.get("fork") {
        None => ForkMode::None,
        Some(value) => match ForkMode::from_value(value) {
            Some(fork) => fork,
            None => return invalid("fork must be none, all, or a positive user-turn count"),
        },
    };
    let request = PersistentRequest {
        name: string("name").unwrap_or_else(|| agent_name.clone()),
        agent: agent_name,
        task,
        process,
        permissions: json!({
            "workspace": display(&workspace),
            "allow_external": allow_external,
            "toolnames": allowed_tools,
            "depth": options.depth + 1,
            "trust": !confirm_project,
            "model": model.unwrap_or_default(),
        }),
        fork,
    };
    match coordinator.spawn(request) {
        Ok(value) => json_output(value),
        Err(e) => failure("execution_error", e),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::definitions::builtin_subagents;

    #[test]
    fn schema_depends_on_coordinator() {
        let plain = parameters_schema(8, false);
        assert_eq!(plain["properties"]["action"]["enum"], json!(["list"]));
        assert!(plain["properties"].get("fork").is_none());
        assert_eq!(plain["properties"]["tasks"]["maxItems"], 8);
        let persistent = parameters_schema(3, true);
        assert_eq!(
            persistent["properties"]["action"]["enum"]
                .as_array()
                .unwrap()
                .len(),
            7
        );
        assert_eq!(persistent["properties"]["timeout_ms"]["maximum"], 60000);
    }

    #[test]
    fn sync_request_shape() {
        let mut agent = builtin_subagents().remove(1);
        agent.thinking = Some("high".into());
        agent.max_subagent_depth = Some(5);
        let model = model_with_thinking(Some("gpt-5".into()), &agent);
        let request = sync_child_request(
            &agent,
            "plan it",
            model.as_deref(),
            PathBuf::from("/w"),
            Path::new("/home/x/.qt"),
            "run-1",
            0,
            2,
            false,
        );
        let args = request.arguments.join(" ");
        assert!(args.starts_with(
            "--mode json --no-session --no-extensions --no-context-files --no-skills --tools read,symbol"
        ));
        assert!(args.contains("--model gpt-5:high --system-prompt You are a planning agent."));
        assert_eq!(request.arguments.last().unwrap(), "Task: plan it");
        assert_eq!(request.env("QT_SUBAGENT_DEPTH"), Some("1"));
        assert_eq!(request.env("QT_SUBAGENT_MAX_DEPTH"), Some("2"));
        assert_eq!(request.env("QT_SUBAGENT_ALLOW_EXTERNAL"), Some("0"));
        assert_eq!(request.env("QT_SUBAGENT_NAME"), Some("planner"));
        assert_eq!(request.env("QT_CODING_AGENT_DIR"), Some("/home/x/.qt"));
        assert_eq!(
            model_with_thinking(Some("a:b".into()), &agent).unwrap(),
            "a:b"
        );
        agent.thinking = Some("off".into());
        assert_eq!(model_with_thinking(Some("m".into()), &agent).unwrap(), "m");
        assert_eq!(model_with_thinking(None, &agent), None);
    }

    #[test]
    fn task_parsing() {
        let none = None;
        assert!(parse_task(&json!({"agent": "a", "task": "t"}), &none, &none).is_some());
        assert!(parse_task(&json!({"agent": "a", "task": " "}), &none, &none).is_none());
        assert!(parse_task(&json!({"agent": "a", "task": "t", "x": 1}), &none, &none).is_none());
        assert!(parse_task(&json!({"agent": "a", "task": "t", "cwd": 1}), &none, &none).is_none());
        let cwd = Some("sub".to_string());
        let task = parse_task(&json!({"agent": "a", "task": "t"}), &cwd, &none).unwrap();
        assert_eq!(task.cwd.as_deref(), Some("sub"));
    }
}
