//! Model-facing memory tools (archived `memory_tool.cpp`): `memory_recall`,
//! `memory_remember`, `memory_forget`, `memory_review_list`,
//! `memory_review_show`, `memory_review_preview`, `memory_review_apply` and
//! `memory_feedback`. Mutating tools honour the mutation approval queue.

use std::sync::Arc;

use futures::future::BoxFuture;
use qt_core::tools::{
    MutationKind, MutationPolicy, PendingMutation, Tool, ToolContext, ToolOutput, ToolRegistry,
};
use serde_json::{Value, json};

use crate::error::{MemoryError, Result};
use crate::model::*;
use crate::recall::RecallRequest;
use crate::review::{
    hydrated_review_json, review_action_from_json, review_application_json, review_item_json,
    review_preview_json, review_query_from_json,
};
use crate::runtime::MemoryRuntime;
use crate::settings::MemorySettings;

/// Every memory tool name.
pub const MEMORY_TOOL_NAMES: [&str; 8] = [
    "memory_recall",
    "memory_remember",
    "memory_forget",
    "memory_review_list",
    "memory_review_show",
    "memory_review_preview",
    "memory_review_apply",
    "memory_feedback",
];

/// Archived tool `memory_json`.
pub fn memory_json(memory: &MemoryEntry) -> Value {
    json!({
        "id": memory.id, "type": memory.memory_type.as_str(), "status": memory.status.as_str(),
        "content": memory.content, "source": memory.source, "tags": memory.tags,
        "confidence": memory.confidence, "quality_score": memory.quality_score,
        "access_count": memory.access_count, "success_count": memory.success_count,
        "failure_count": memory.failure_count, "last_outcome": memory.last_outcome.as_str(),
        "source_uri": memory.provenance.source_uri, "created_by": memory.provenance.created_by,
        "evidence_strength": memory.provenance.evidence_strength,
    })
}

fn success(value: Value) -> ToolOutput {
    let value = crate::text::canonical(&value);
    ToolOutput::text(serde_json::to_string_pretty(&value).unwrap_or_default()).with_details(value)
}

fn failure(status: &str, message: impl Into<String>) -> ToolOutput {
    let message = message.into();
    ToolOutput::error(message.clone()).with_details(json!({"status": status, "error": message}))
}

fn invalid_request(message: impl Into<String>) -> ToolOutput {
    failure("invalid_request", message)
}

fn from_error(error: MemoryError) -> ToolOutput {
    if error.is_invalid_argument() {
        invalid_request(error.to_string())
    } else {
        failure("execution_error", error.to_string())
    }
}

fn has_only(args: &Value, allowed: &[&str]) -> bool {
    args.as_object()
        .is_some_and(|object| object.keys().all(|key| allowed.contains(&key.as_str())))
}

fn normalize(args: Value) -> Value {
    match args {
        Value::Null => json!({}),
        other => other,
    }
}

fn string_array(value: &Value) -> Result<Vec<String>> {
    value
        .as_array()
        .ok_or_else(|| MemoryError::invalid("tags must be an array of strings"))?
        .iter()
        .map(|item| {
            item.as_str()
                .map(str::to_string)
                .ok_or_else(|| MemoryError::invalid("tags must be an array of strings"))
        })
        .collect()
}

/// Queues the call for approval under the `queue` mutation policy (archived
/// `require_mutation_approval`); `None` lets the tool proceed.
fn require_approval(
    ctx: &ToolContext,
    tool: &str,
    summary: String,
    arguments: &Value,
) -> Option<ToolOutput> {
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
            ToolOutput::error("mutation approval was not granted")
                .with_details(json!({"status": "permission_denied", "mutation_id": id})),
        );
    }
    let id = queue.enqueue(PendingMutation {
        id: String::new(),
        kind: MutationKind::Command,
        tool_name: tool.into(),
        call_id: ctx.call_id.clone(),
        summary: summary.clone(),
        arguments: arguments.clone(),
    });
    Some(
        ToolOutput::error(format!(
            "Pending approval: {summary}. The change was not applied; it is queued as {id} until the user approves it."
        ))
        .with_details(json!({
            "status": "pending_approval", "mutation_id": id, "queued": true,
            "kind": MutationKind::Command.as_str(), "tool_name": tool,
        })),
    )
}

async fn blocking(work: impl FnOnce() -> ToolOutput + Send + 'static) -> ToolOutput {
    match tokio::task::spawn_blocking(work).await {
        Ok(output) => output,
        Err(error) => failure("execution_error", format!("memory tool failed: {error}")),
    }
}

macro_rules! memory_tool {
    ($name:ident, $tool:literal, $snippet:literal) => {
        pub struct $name {
            runtime: Arc<MemoryRuntime>,
        }

        impl $name {
            pub fn new(runtime: Arc<MemoryRuntime>) -> Self {
                Self { runtime }
            }
        }

        impl Tool for $name {
            fn name(&self) -> &'static str {
                $tool
            }

            fn snippet(&self) -> &'static str {
                $snippet
            }

            fn description(&self) -> String {
                Self::DESCRIPTION.into()
            }

            fn parameters(&self) -> Value {
                Self::schema()
            }

            fn parallel_safe(&self) -> bool {
                Self::PARALLEL_SAFE
            }

            fn execute(&self, args: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
                let runtime = Arc::clone(&self.runtime);
                let args = normalize(args);
                Box::pin(async move {
                    if let Some(early) = Self::gate(&args, &ctx) {
                        return early;
                    }
                    blocking(move || Self::run(&runtime, &args)).await
                })
            }
        }
    };
}

memory_tool!(
    MemoryRecallTool,
    "memory_recall",
    "Recall governed memories for the current repository scope"
);
memory_tool!(
    MemoryRememberTool,
    "memory_remember",
    "Create a reviewable candidate or hypothesis memory"
);
memory_tool!(
    MemoryForgetTool,
    "memory_forget",
    "Permanently delete one in-scope memory (confirm=true)"
);
memory_tool!(
    MemoryReviewListTool,
    "memory_review_list",
    "List the prioritized memory review queue"
);
memory_tool!(
    MemoryReviewShowTool,
    "memory_review_show",
    "Show one hydrated memory review"
);
memory_tool!(
    MemoryReviewPreviewTool,
    "memory_review_preview",
    "Preview a governed memory review action"
);
memory_tool!(
    MemoryReviewApplyTool,
    "memory_review_apply",
    "Apply an approved memory review action"
);
memory_tool!(
    MemoryFeedbackTool,
    "memory_feedback",
    "Record an outcome for a procedural memory"
);

fn string_schema() -> Value {
    json!({"type": "string"})
}

fn string_list_schema() -> Value {
    json!({"type": "array", "items": {"type": "string"}})
}

impl MemoryRecallTool {
    const DESCRIPTION: &'static str =
        "Recall governed built-in memories for the current repository scope.";
    const PARALLEL_SAFE: bool = true;
    const ALLOWED: [&'static str; 5] = [
        "query",
        "limit",
        "include_candidates",
        "include_global_user",
        "tags",
    ];

    fn schema() -> Value {
        json!({
            "type": "object",
            "properties": {
                "query": string_schema(),
                "limit": {"type": "integer", "minimum": 1, "maximum": 50},
                "include_candidates": {"type": "boolean"},
                "include_global_user": {"type": "boolean"},
                "tags": string_list_schema(),
            },
            "required": ["query"],
            "additionalProperties": false,
        })
    }

    fn gate(args: &Value, _ctx: &ToolContext) -> Option<ToolOutput> {
        if !has_only(args, &Self::ALLOWED) || !args.get("query").is_some_and(Value::is_string) {
            return Some(invalid_request(
                "memory_recall requires a string query and accepts only query, limit, include_candidates, include_global_user, and tags",
            ));
        }
        None
    }

    fn run(runtime: &MemoryRuntime, args: &Value) -> ToolOutput {
        let result_limit = match args.get("limit") {
            None => 10,
            Some(value) => match value.as_u64() {
                Some(limit) => limit as usize,
                None => return invalid_request("memory_recall limit must be from 1 to 50"),
            },
        };
        let tags = match args.get("tags").map(string_array).transpose() {
            Ok(tags) => tags.unwrap_or_default(),
            Err(error) => return from_error(error),
        };
        if !(1..=50).contains(&result_limit) {
            return invalid_request("memory_recall limit must be from 1 to 50");
        }
        let request = RecallRequest {
            query: args["query"].as_str().unwrap_or_default().to_string(),
            result_limit,
            candidate_limit: result_limit.max(20),
            include_candidates: args
                .get("include_candidates")
                .and_then(Value::as_bool)
                .unwrap_or(true),
            include_global_user: args
                .get("include_global_user")
                .and_then(Value::as_bool)
                .unwrap_or(false),
            tags,
            ..Default::default()
        };
        let scope = runtime.scope();
        let recalled = match runtime.recall().recall(&scope, &request) {
            Ok(recalled) => recalled,
            Err(error) => return failure("execution_error", error.to_string()),
        };
        if let Err(error) = runtime.recall().record_access(&scope, &recalled) {
            return failure("execution_error", error.to_string());
        }
        let memories: Vec<Value> = recalled
            .iter()
            .map(|item| {
                let mut value = memory_json(&item.memory);
                value["score"] = json!(item.score.total);
                value["warnings"] = json!(item.warnings);
                value
            })
            .collect();
        success(
            json!({"memories": memories, "count": recalled.len(), "scope_repository": scope.repository}),
        )
    }
}

impl MemoryRememberTool {
    const DESCRIPTION: &'static str =
        "Create a reviewable candidate or hypothesis in built-in memory.";
    const PARALLEL_SAFE: bool = false;
    const ALLOWED: [&'static str; 7] = [
        "content",
        "type",
        "status",
        "source",
        "source_uri",
        "tags",
        "confidence",
    ];

    fn schema() -> Value {
        json!({
            "type": "object",
            "properties": {
                "content": {"type": "string", "minLength": 1},
                "type": string_schema(),
                "status": {"type": "string", "enum": ["candidate", "hypothesis"]},
                "source": string_schema(),
                "source_uri": string_schema(),
                "tags": string_list_schema(),
                "confidence": {"type": "number", "minimum": 0, "maximum": 1},
            },
            "required": ["content"],
            "additionalProperties": false,
        })
    }

    fn gate(args: &Value, ctx: &ToolContext) -> Option<ToolOutput> {
        if !has_only(args, &Self::ALLOWED)
            || args
                .get("content")
                .and_then(Value::as_str)
                .is_none_or(str::is_empty)
        {
            return Some(invalid_request(
                "memory_remember requires non-empty content and accepts only content, type, status, source, source_uri, tags, and confidence",
            ));
        }
        let status = args
            .get("status")
            .and_then(Value::as_str)
            .unwrap_or("hypothesis");
        if status != "candidate" && status != "hypothesis" {
            return Some(invalid_request(
                "memory_remember status must be candidate or hypothesis",
            ));
        }
        require_approval(
            ctx,
            "memory_remember",
            "create reviewable built-in memory".into(),
            args,
        )
    }

    fn run(runtime: &MemoryRuntime, args: &Value) -> ToolOutput {
        let status = args
            .get("status")
            .and_then(Value::as_str)
            .unwrap_or("hypothesis")
            .to_string();
        let write = (|| -> Result<MemoryWrite> {
            Ok(MemoryWrite {
                content: args["content"].as_str().unwrap_or_default().to_string(),
                memory_type: MemoryType::parse(
                    args.get("type")
                        .and_then(Value::as_str)
                        .unwrap_or("observation"),
                )?,
                status: MemoryStatus::parse(&status)?,
                source: args
                    .get("source")
                    .and_then(Value::as_str)
                    .unwrap_or("agent:tool")
                    .to_string(),
                confidence: args
                    .get("confidence")
                    .and_then(Value::as_f64)
                    .unwrap_or(0.5),
                provenance: Provenance {
                    source_type: SourceType::Chat,
                    source_uri: args
                        .get("source_uri")
                        .and_then(Value::as_str)
                        .unwrap_or("")
                        .to_string(),
                    created_by: "agent".into(),
                    ..Default::default()
                },
                tags: args
                    .get("tags")
                    .map(string_array)
                    .transpose()?
                    .unwrap_or_default(),
                quality_score: 0.0,
            })
        })();
        let write = match write {
            Ok(write) => write,
            Err(error) => return from_error(error),
        };
        match runtime.memories().remember(&runtime.scope(), write) {
            Ok(id) => success(json!({"id": id, "status": status, "created": true})),
            Err(error) => from_error(error),
        }
    }
}

impl MemoryForgetTool {
    const DESCRIPTION: &'static str =
        "Permanently delete one in-scope memory after explicit confirmation and approval.";
    const PARALLEL_SAFE: bool = false;

    fn schema() -> Value {
        json!({
            "type": "object",
            "properties": {"id": {"type": "string", "minLength": 1}, "confirm": {"type": "boolean"}},
            "required": ["id", "confirm"],
            "additionalProperties": false,
        })
    }

    fn gate(args: &Value, ctx: &ToolContext) -> Option<ToolOutput> {
        let id = args.get("id").and_then(Value::as_str).unwrap_or_default();
        if !has_only(args, &["id", "confirm"])
            || id.is_empty()
            || !args
                .get("confirm")
                .and_then(Value::as_bool)
                .unwrap_or(false)
        {
            return Some(invalid_request(
                "memory_forget requires a non-empty id and confirm=true",
            ));
        }
        require_approval(
            ctx,
            "memory_forget",
            format!("permanently forget memory {id}"),
            args,
        )
    }

    fn run(runtime: &MemoryRuntime, args: &Value) -> ToolOutput {
        let id = args["id"].as_str().unwrap_or_default();
        match runtime.memories().forget(&runtime.scope(), id) {
            Ok(true) => success(json!({"id": id, "forgotten": true})),
            Ok(false) => {
                ToolOutput::error("memory not found in current scope").with_details(json!({
                    "status": "not_found", "id": id, "forgotten": false,
                }))
            }
            Err(error) => failure("execution_error", error.to_string()),
        }
    }
}

impl MemoryReviewListTool {
    const DESCRIPTION: &'static str = "List the indexed, priority-ordered memory review queue. Use this instead of \
text search for review IDs, kinds, priority, status, or tags.";
    const PARALLEL_SAFE: bool = true;
    const ALLOWED: [&'static str; 15] = [
        "statuses",
        "kinds",
        "priorities",
        "source_types",
        "tags",
        "dream_cycles",
        "scope",
        "stale",
        "conflict",
        "created_after",
        "created_before",
        "decided_after",
        "limit",
        "offset",
        "oldest_first",
    ];

    fn schema() -> Value {
        json!({
            "type": "object",
            "properties": {
                "statuses": string_list_schema(), "kinds": string_list_schema(),
                "priorities": string_list_schema(), "source_types": string_list_schema(),
                "tags": string_list_schema(), "dream_cycles": string_list_schema(),
                "scope": {"type": "string", "enum": ["all", "exact", "durable"]},
                "stale": {"type": "boolean"}, "conflict": {"type": "boolean"},
                "created_after": {"type": "integer"}, "created_before": {"type": "integer"},
                "decided_after": {"type": "integer"},
                "limit": {"type": "integer", "minimum": 1, "maximum": 100},
                "offset": {"type": "integer", "minimum": 0},
                "oldest_first": {"type": "boolean"},
            },
            "additionalProperties": false,
        })
    }

    fn gate(args: &Value, _ctx: &ToolContext) -> Option<ToolOutput> {
        (!has_only(args, &Self::ALLOWED))
            .then(|| invalid_request("invalid memory_review_list request"))
    }

    fn run(runtime: &MemoryRuntime, args: &Value) -> ToolOutput {
        let query = match review_query_from_json(args) {
            Ok(query) => query,
            Err(error) => return from_error(error),
        };
        if query.limit > 100 {
            return invalid_request("memory_review_list limit must be from 1 to 100");
        }
        match runtime.reviews().review_items(&query) {
            Ok(items) => success(json!({
                "items": items.iter().map(review_item_json).collect::<Vec<_>>(),
                "count": items.len(),
            })),
            Err(error) => from_error(error),
        }
    }
}

impl MemoryReviewShowTool {
    const DESCRIPTION: &'static str = "Hydrate one indexed memory review with its typed candidate, governed subject, \
sources, graph context, recall impact, and risk explanation.";
    const PARALLEL_SAFE: bool = true;

    fn schema() -> Value {
        json!({
            "type": "object",
            "properties": {"id": string_schema()},
            "required": ["id"],
            "additionalProperties": false,
        })
    }

    fn gate(args: &Value, _ctx: &ToolContext) -> Option<ToolOutput> {
        (!has_only(args, &["id"]) || !args.get("id").is_some_and(Value::is_string))
            .then(|| invalid_request("memory_review_show requires string id"))
    }

    fn run(runtime: &MemoryRuntime, args: &Value) -> ToolOutput {
        match runtime
            .reviews()
            .review_detail(args["id"].as_str().unwrap_or_default())
        {
            Ok(review) => success(hydrated_review_json(&review)),
            Err(error) => from_error(error),
        }
    }
}

impl MemoryReviewPreviewTool {
    const DESCRIPTION: &'static str = "Prepare a non-mutating governed review action preview. This can suggest an \
outcome but cannot authorize or apply it.";
    const PARALLEL_SAFE: bool = true;
    const ALLOWED: [&'static str; 7] = [
        "id",
        "action",
        "note",
        "target_id",
        "evidence_uri",
        "deferred_until",
        "edit",
    ];

    fn schema() -> Value {
        json!({
            "type": "object",
            "properties": {
                "id": string_schema(), "action": string_schema(), "note": string_schema(),
                "target_id": string_schema(), "evidence_uri": string_schema(),
                "deferred_until": {"type": "integer"}, "edit": {"type": "object"},
            },
            "required": ["id", "action"],
            "additionalProperties": false,
        })
    }

    fn gate(args: &Value, _ctx: &ToolContext) -> Option<ToolOutput> {
        (!has_only(args, &Self::ALLOWED) || !args.get("id").is_some_and(Value::is_string))
            .then(|| invalid_request("memory_review_preview requires string id and action"))
    }

    fn run(runtime: &MemoryRuntime, args: &Value) -> ToolOutput {
        let action = match review_action_from_json(args, "agent:suggestion", false) {
            Ok(action) => action,
            Err(error) => return from_error(error),
        };
        match runtime
            .reviews()
            .preview_review(args["id"].as_str().unwrap_or_default(), action)
        {
            Ok(preview) => success(review_preview_json(&preview)),
            Err(error) => from_error(error),
        }
    }
}

impl MemoryReviewApplyTool {
    const DESCRIPTION: &'static str = "Apply one typed memory review action only after explicit user approval. Use \
memory_review_preview first and pass its revision and token.";
    const PARALLEL_SAFE: bool = false;
    const ALLOWED: [&'static str; 9] = [
        "id",
        "action",
        "note",
        "target_id",
        "evidence_uri",
        "deferred_until",
        "expected_revision",
        "idempotency_key",
        "edit",
    ];

    fn schema() -> Value {
        json!({
            "type": "object",
            "properties": {
                "id": string_schema(), "action": string_schema(), "note": string_schema(),
                "target_id": string_schema(), "evidence_uri": string_schema(),
                "deferred_until": {"type": "integer"}, "expected_revision": {"type": "integer"},
                "idempotency_key": string_schema(), "edit": {"type": "object"},
            },
            "required": ["id", "action", "expected_revision", "idempotency_key"],
            "additionalProperties": false,
        })
    }

    fn gate(args: &Value, ctx: &ToolContext) -> Option<ToolOutput> {
        if !has_only(args, &Self::ALLOWED) || !args.get("id").is_some_and(Value::is_string) {
            return Some(invalid_request(
                "memory_review_apply requires id, action, expected_revision, and idempotency_key",
            ));
        }
        let id = args["id"].as_str().unwrap_or_default();
        require_approval(
            ctx,
            "memory_review_apply",
            format!("apply memory review {id}"),
            args,
        )
    }

    fn run(runtime: &MemoryRuntime, args: &Value) -> ToolOutput {
        let action = match review_action_from_json(args, "user:tool-approval", true) {
            Ok(action) => action,
            Err(error) => return from_error(error),
        };
        match runtime
            .reviews()
            .apply_review_action(args["id"].as_str().unwrap_or_default(), action)
        {
            Ok(result) => success(review_application_json(&result)),
            Err(error) => from_error(error),
        }
    }
}

impl MemoryFeedbackTool {
    const DESCRIPTION: &'static str = "Record an outcome for an in-scope procedural memory.";
    const PARALLEL_SAFE: bool = false;

    fn schema() -> Value {
        json!({
            "type": "object",
            "properties": {
                "id": {"type": "string", "minLength": 1},
                "outcome": {"type": "string", "enum": ["success", "failure", "partial", "unused"]},
                "task_id": string_schema(), "notes": string_schema(), "evidence_uri": string_schema(),
            },
            "required": ["id", "outcome"],
            "additionalProperties": false,
        })
    }

    fn gate(args: &Value, ctx: &ToolContext) -> Option<ToolOutput> {
        if !has_only(args, &["id", "outcome", "task_id", "notes", "evidence_uri"])
            || !args.get("id").is_some_and(Value::is_string)
            || !args.get("outcome").is_some_and(Value::is_string)
        {
            return Some(invalid_request(
                "memory_feedback requires string id and outcome",
            ));
        }
        let id = args["id"].as_str().unwrap_or_default();
        require_approval(
            ctx,
            "memory_feedback",
            format!("record procedural memory outcome for {id}"),
            args,
        )
    }

    fn run(runtime: &MemoryRuntime, args: &Value) -> ToolOutput {
        let outcome = ProcedureOutcome::parse(args["outcome"].as_str().unwrap_or_default());
        if outcome == ProcedureOutcome::Unknown {
            return invalid_request("memory_feedback outcome is invalid");
        }
        let text = |field: &str| {
            args.get(field)
                .and_then(Value::as_str)
                .unwrap_or("")
                .to_string()
        };
        let feedback = ProcedureFeedback {
            procedure_id: text("id"),
            outcome,
            task_id: text("task_id"),
            notes: text("notes"),
            evidence_uri: text("evidence_uri"),
            deduplication_key: String::new(),
        };
        match runtime.record_feedback(feedback) {
            Ok((run, memory)) => success(json!({"run_id": run.id, "memory": memory_json(&memory)})),
            Err(error) => from_error(error),
        }
    }
}

/// Memory tools admitted by `enabled` (`memory_review` admits every review
/// tool).
pub fn memory_tools(
    runtime: &Arc<MemoryRuntime>,
    enabled: impl Fn(&str) -> bool,
) -> Vec<Arc<dyn Tool>> {
    let mut tools: Vec<Arc<dyn Tool>> = Vec::new();
    let review = enabled("memory_review");
    if enabled("memory_recall") {
        tools.push(Arc::new(MemoryRecallTool::new(Arc::clone(runtime))));
    }
    if enabled("memory_remember") {
        tools.push(Arc::new(MemoryRememberTool::new(Arc::clone(runtime))));
    }
    if enabled("memory_forget") {
        tools.push(Arc::new(MemoryForgetTool::new(Arc::clone(runtime))));
    }
    if review || enabled("memory_review_list") {
        tools.push(Arc::new(MemoryReviewListTool::new(Arc::clone(runtime))));
    }
    if review || enabled("memory_review_show") {
        tools.push(Arc::new(MemoryReviewShowTool::new(Arc::clone(runtime))));
    }
    if review || enabled("memory_review_preview") {
        tools.push(Arc::new(MemoryReviewPreviewTool::new(Arc::clone(runtime))));
    }
    if review || enabled("memory_review_apply") {
        tools.push(Arc::new(MemoryReviewApplyTool::new(Arc::clone(runtime))));
    }
    if enabled("memory_feedback") {
        tools.push(Arc::new(MemoryFeedbackTool::new(Arc::clone(runtime))));
    }
    tools
}

/// Adds the memory tools to `registry` only when `memory.enabled` and a
/// runtime exists.
pub fn register_memory_tools(
    registry: ToolRegistry,
    settings: &MemorySettings,
    runtime: Option<&Arc<MemoryRuntime>>,
    enabled: impl Fn(&str) -> bool,
) -> ToolRegistry {
    let Some(runtime) = runtime.filter(|_| settings.enabled) else {
        return registry;
    };
    memory_tools(runtime, enabled)
        .into_iter()
        .fold(registry, |registry, tool| registry.with(tool))
}
