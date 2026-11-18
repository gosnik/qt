//! RPC `memory` command handlers as a library function (archived
//! `rpc_mode.cpp` memory actions): `status`, `dream`, `recall`, `explain`,
//! `retrieval_rebuild`, `remember`, `review`, `feedback` and `forget`.

use std::path::PathBuf;
use std::sync::Arc;

use serde_json::{Value, json};

use crate::cli::{configured_graph, dreaming_settings_json, statistics_json};
use crate::error::MemoryError;
use crate::model::*;
use crate::recall::RecallRequest;
use crate::retrieval::{context_preview_json, recalled_memory_json, retrieval_candidate_json};
use crate::review::{
    ReviewBatchItem, hydrated_review_json, review_action_from_json, review_action_record_json,
    review_application_json, review_batch_job_json, review_batch_preview_json,
    review_batch_result_json, review_item_json, review_preview_json, review_query_from_json,
    review_statistics_json,
};
use crate::runtime::{MemoryDreamingStatus, MemoryRuntime};
use crate::settings::MemorySettings;
use crate::text::canonical;

/// Runtime state the RPC handler reports on.
#[derive(Clone, Default)]
pub struct RpcMemoryContext {
    pub runtime: Option<Arc<MemoryRuntime>>,
    pub settings: MemorySettings,
    pub disabled_by_cli: bool,
    pub initialization_error: String,
    pub agent_home: PathBuf,
}

/// One `memory` RPC command.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct RpcMemoryCommand {
    pub action: String,
    pub params: Value,
    pub confirm: bool,
}

/// Failure with the archived error code (`invalid_argument` or `io_error`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RpcMemoryError {
    pub code: &'static str,
    pub message: String,
}

impl std::fmt::Display for RpcMemoryError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.message)
    }
}

impl std::error::Error for RpcMemoryError {}

type RpcResult = Result<Value, RpcMemoryError>;

fn invalid(message: impl Into<String>) -> RpcMemoryError {
    RpcMemoryError {
        code: "invalid_argument",
        message: message.into(),
    }
}

fn from_error(error: MemoryError) -> RpcMemoryError {
    if error.is_invalid_argument() {
        invalid(error.to_string())
    } else {
        RpcMemoryError {
            code: "io_error",
            message: error.to_string(),
        }
    }
}

/// Classifies a review failure as `rpc_error:<code>:<message>`.
pub fn review_rpc_error(message: &str) -> String {
    let code = if message.contains("not found") {
        "review_not_found"
    } else if message.contains("stale") || message.contains("changed after preview") {
        "stale_preview"
    } else if message.contains("confirm=true") || message.contains("human authority") {
        "confirmation_required"
    } else if message.contains("not applicable") || message.contains("requires individual review") {
        "review_not_applicable"
    } else if message.contains("invalid review action") {
        "invalid_review_action"
    } else if message.contains(" must be ")
        || message.contains(" requires string ")
        || message.contains("requires items")
        || message.contains("items array")
        || message.contains("batch item requires")
    {
        "invalid_review_schema"
    } else {
        "invalid_review_request"
    };
    format!("rpc_error:{code}:{message}")
}

/// Dreaming runtime status JSON.
pub fn dreaming_status_json(status: &MemoryDreamingStatus) -> Value {
    json!({
        "enabled": status.enabled, "trigger": status.trigger, "idle_seconds": status.idle_seconds,
        "idle_for_seconds": status.idle_for_seconds, "waiting_for_idle": status.waiting_for_idle,
        "running": status.running, "pending": status.pending_runs, "completed": status.completed_runs,
        "failed": status.failed_runs, "last_error": status.last_error,
        "last_dream_id": status.last_dream_id, "last_review_id": status.last_review_id,
    })
}

fn status(context: &RpcMemoryContext) -> RpcResult {
    let settings = &context.settings;
    let memory = context.runtime.as_ref();
    let mut result = json!({
        "enabled": memory.is_some(),
        "configured": settings.enabled,
        "disabled_by_cli": context.disabled_by_cli,
        "automatic_recall": settings.automatic_recall,
        "automatic_capture": settings.automatic_capture,
        "dreaming": dreaming_settings_json(settings, memory.is_some()),
        "database": context.agent_home.join("memory").join("memory.sqlite3").to_string_lossy(),
        "prehandler": if memory.is_some() && settings.automatic_recall {
            "isolated_child_with_deterministic_fallback"
        } else {
            "inactive"
        },
        "warnings": [],
    });
    if !context.initialization_error.is_empty() {
        result["initialization_error"] = json!(context.initialization_error);
        result["warnings"] = json!([format!(
            "memory initialization failed open: {}",
            context.initialization_error
        )]);
    }
    let Some(memory) = memory else {
        result["statistics"] = Value::Null;
        return Ok(canonical(&result));
    };
    let scope = memory.scope();
    let statistics = memory.statistics().map_err(from_error)?;
    result["schema_version"] = json!(memory.schema_version());
    result["scope_memory_count"] = json!(statistics.memory_entries);
    result["statistics"] = statistics_json(&statistics);
    result["review_statistics"] =
        review_statistics_json(&memory.reviews().review_statistics().map_err(from_error)?);
    let retrieval = memory.retrieval_status();
    let configured = &settings.retrieval;
    result["retrieval"] = json!({
        "enabled": retrieval.enabled, "running": retrieval.running,
        "generation": retrieval.generation, "last_error": retrieval.last_error,
        "completed_jobs": retrieval.completed_jobs,
        "jobs": {
            "pending": retrieval.jobs.pending, "leased": retrieval.jobs.leased,
            "failed": retrieval.jobs.failed, "complete": retrieval.jobs.complete,
            "chunks": retrieval.jobs.chunks,
        },
        "queries": {
            "cache_hits": retrieval.queries.cache_hits,
            "embedding_failures": retrieval.queries.embedding_failures,
            "cache_entries": retrieval.queries.cache_entries,
            "cache_bytes": retrieval.queries.cache_bytes,
            "last_error": retrieval.queries.last_error,
        },
        "code": {
            "enabled": retrieval.code.enabled, "available": retrieval.code.available,
            "stale": retrieval.code.stale, "generation": retrieval.code.generation,
            "repository": retrieval.code.repository, "worktree": retrieval.code.worktree,
        },
        "configured": {
            "graph_enabled": configured.graph_enabled, "dense_enabled": configured.dense_enabled,
            "code_enabled": configured.code_enabled, "query_cache_entries": configured.query_cache_entries,
            "max_depth": configured.max_depth, "max_seeds": configured.max_seeds,
            "max_edges": configured.max_edges, "max_graph_results": configured.max_graph_results,
            "max_fanout": configured.max_fanout,
        },
    });
    result["scope"] = json!({
        "user": scope.user, "project": scope.project, "repository": scope.repository,
        "repository_display": scope.repository_display, "branch": scope.branch, "session": memory.session_context().0,
    });
    let dreaming = memory.dreaming_status();
    result["dreaming"]["runtime"] = json!({
        "running": dreaming.running, "idle_seconds": dreaming.idle_seconds,
        "idle_for_seconds": dreaming.idle_for_seconds, "waiting_for_idle": dreaming.waiting_for_idle,
        "pending": dreaming.pending_runs, "completed": dreaming.completed_runs,
        "failed": dreaming.failed_runs, "last_error": dreaming.last_error,
        "last_dream_id": dreaming.last_dream_id, "last_review_id": dreaming.last_review_id,
    });
    Ok(canonical(&result))
}

fn string_param<'a>(params: &'a Value, key: &str) -> Option<&'a str> {
    params.get(key).and_then(Value::as_str)
}

fn dream(memory: &MemoryRuntime, params: &Value) -> RpcResult {
    let operation = string_param(params, "operation").unwrap_or("status");
    match operation {
        "status" => Ok(canonical(&dreaming_status_json(&memory.dreaming_status()))),
        "run" => {
            if memory.enqueue_dream_cycle("manual") {
                Ok(json!({"queued": true}))
            } else {
                Err(invalid(
                    "Dreaming cycle was not queued; enable Dreaming and verify its queue/model settings",
                ))
            }
        }
        "research" => {
            let (Some(dream_id), Some(_)) = (
                string_param(params, "dream_id"),
                string_param(params, "summary"),
            ) else {
                return Err(invalid(
                    "memory dream research requires string dream_id and summary",
                ));
            };
            let control = memory
                .dreaming()
                .ok_or_else(|| invalid("Dreaming research successors are disabled by policy"))?;
            let stored = control
                .add_research(dream_id, params, "user:rpc")
                .map_err(from_error)?;
            Ok(canonical(&json!({
                "dream_id": stored.dream_id, "research_id": stored.research_id,
                "research_review_id": stored.research_review_id,
            })))
        }
        _ => Err(invalid(
            "memory dream operation must be status, run, or research",
        )),
    }
}

fn recall(
    context: &RpcMemoryContext,
    memory: &MemoryRuntime,
    command: &RpcMemoryCommand,
) -> RpcResult {
    let params = &command.params;
    let query = string_param(params, "query").filter(|query| !query.is_empty());
    let Some(query) = query else {
        return Err(invalid(
            "memory recall requires non-empty string params.query",
        ));
    };
    let settings = &context.settings;
    let policy = match settings.recall_policy.as_str() {
        "strict" => RecallPolicy::Strict,
        "exploratory" | "broad" => RecallPolicy::Broad,
        "balanced" => RecallPolicy::Balanced,
        _ => {
            return Err(invalid(
                "memory recall has invalid configured recall policy",
            ));
        }
    };
    if params
        .get("include_global_user")
        .is_some_and(|value| !value.is_boolean())
    {
        return Err(invalid("memory recall include_global_user must be boolean"));
    }
    for key in ["limit", "token_budget"] {
        if params
            .get(key)
            .is_some_and(|value| !value.is_i64() && !value.is_u64())
        {
            return Err(invalid(format!("memory recall {key} must be an integer")));
        }
    }
    let result_limit = params.get("limit").and_then(Value::as_u64).unwrap_or(10) as usize;
    if !(1..=50).contains(&result_limit) {
        return Err(invalid("memory recall limit must be from 1 to 50"));
    }
    let mut request = RecallRequest {
        query: query.to_string(),
        policy,
        include_global_user: params
            .get("include_global_user")
            .and_then(Value::as_bool)
            .unwrap_or(false),
        graph: configured_graph(settings),
        dense_enabled: settings.retrieval.dense_enabled,
        result_limit,
        candidate_limit: result_limit.max(20),
        ..Default::default()
    };
    for key in ["subject", "predicate", "object"] {
        if params.get(key).is_some_and(|value| !value.is_string()) {
            return Err(invalid(format!("memory recall {key} must be a string")));
        }
    }
    let structured = &mut request.structured;
    structured.subject = string_param(params, "subject").unwrap_or("").to_string();
    structured.predicate = string_param(params, "predicate").unwrap_or("").to_string();
    structured.object = string_param(params, "object").unwrap_or("").to_string();
    if !structured.subject.is_empty()
        || !structured.predicate.is_empty()
        || !structured.object.is_empty()
    {
        structured.enabled = true;
        structured.limit = request.candidate_limit.min(100);
    }
    if let Some(value) = params.get("as_of") {
        let Some(seconds) = value.as_i64() else {
            if value.is_u64() {
                return Err(invalid("as_of is outside supported time range"));
            }
            return Err(invalid("memory recall as_of must be epoch seconds"));
        };
        let millis = seconds
            .checked_mul(1000)
            .ok_or_else(|| invalid("memory recall as_of is outside supported time range"))?;
        request.structured.as_of = Some(millis);
        request.structured.enabled = true;
    }
    let token_budget = params
        .get("token_budget")
        .and_then(Value::as_u64)
        .map_or(settings.context_token_budget as usize, |budget| {
            budget as usize
        });
    let explain = command.action == "explain";
    let preview = if explain {
        Some(
            memory
                .preview_retrieval(request.clone(), token_budget)
                .map_err(from_error)?,
        )
    } else {
        None
    };
    let recalled = match &preview {
        Some(preview) => preview.memories.clone(),
        None => {
            let scope = memory.scope();
            let recalled = memory
                .recall()
                .recall(&scope, &request)
                .map_err(from_error)?;
            memory
                .recall()
                .record_access(&scope, &recalled)
                .map_err(from_error)?;
            recalled
        }
    };
    let context = preview.as_ref().map(|preview| &preview.context);
    let memories: Vec<Value> = recalled
        .iter()
        .map(|item| {
            let revision = if item.historical {
                None
            } else {
                memory.current_retrieval_revision(
                    &item.memory.id,
                    request.policy,
                    request.include_global_user,
                )
            };
            recalled_memory_json(item, revision, context)
        })
        .collect();
    let mut result = json!({"memories": memories, "count": recalled.len()});
    if let Some(preview) = &preview {
        let historical = request.structured.as_of.is_some();
        result["code_candidates"] = Value::Array(
            preview
                .code
                .iter()
                .map(|item| retrieval_candidate_json(item, Some(&preview.context)))
                .collect(),
        );
        result["context"] = context_preview_json(&preview.context, token_budget, 4);
        result["requested_modes"] = json!({
            "lexical": true, "graph": request.graph.enabled && !historical,
            "dense": request.dense_enabled && !historical,
            "code": settings.retrieval.code_enabled && !historical,
        });
        let mut observed: Vec<String> = Vec::new();
        let mut note = |name: &str| {
            if !observed.iter().any(|item| item == name) {
                observed.push(name.into());
            }
        };
        for item in &preview.memories {
            if item.score.lexical_rank != 0 {
                note("lexical");
            }
            if item.score.graph_rank != 0 {
                note("graph");
            }
            if item.score.dense_rank != 0 {
                note("dense");
            }
        }
        for item in &preview.code {
            for rank in &item.ranks {
                note(&rank.retriever);
            }
        }
        result["observed_retrievers"] = json!(observed);
        let status = memory.retrieval_status();
        result["dense_generation"] = json!(status.generation);
        result["retrieval_warnings"] = json!(
            [&status.last_error, &status.queries.last_error]
                .into_iter()
                .filter(|warning| !warning.is_empty())
                .collect::<Vec<_>>()
        );
    }
    Ok(canonical(&result))
}

fn review(memory: &MemoryRuntime, command: &RpcMemoryCommand) -> RpcResult {
    let params = &command.params;
    let schema = |error: MemoryError| -> RpcMemoryError {
        if error.is_invalid_argument() {
            invalid(review_rpc_error(error.message()))
        } else {
            from_error(error)
        }
    };
    if params.get("action").is_some_and(|value| !value.is_string()) {
        return Err(invalid(review_rpc_error(
            "memory review action must be a string",
        )));
    }
    let action = string_param(params, "action").unwrap_or("list");
    let inbox = memory.reviews();
    let id = || -> Result<&str, RpcMemoryError> {
        string_param(params, "id").ok_or_else(|| {
            invalid(review_rpc_error(&format!(
                "memory review {action} requires string id"
            )))
        })
    };
    let decision = || -> String {
        string_param(params, "review_action")
            .or_else(|| string_param(params, "decision"))
            .unwrap_or("")
            .to_string()
    };
    match action {
        "list" | "next" => {
            let mut query = review_query_from_json(params).map_err(schema)?;
            if query.limit > 100 {
                return Err(invalid(review_rpc_error(
                    "memory review limit must be from 1 to 100",
                )));
            }
            if action == "next" {
                query.limit = 1;
            }
            let items = inbox.review_items(&query).map_err(schema)?;
            Ok(canonical(&json!({
                "items": items.iter().map(review_item_json).collect::<Vec<_>>(),
                "count": items.len(),
            })))
        }
        "show" => Ok(hydrated_review_json(
            &inbox.review_detail(id()?).map_err(schema)?,
        )),
        "history" => {
            if params.get("id").is_some_and(|value| !value.is_string()) {
                return Err(invalid(review_rpc_error(
                    "memory review history id must be a string",
                )));
            }
            let limit = params.get("limit").and_then(Value::as_u64).unwrap_or(20) as usize;
            let items: Vec<Value> = inbox
                .review_history(string_param(params, "id"), limit)
                .map_err(schema)?
                .iter()
                .map(review_action_record_json)
                .collect();
            Ok(json!({"items": items}))
        }
        "preview" => {
            let id = id()?;
            let mut input = params.clone();
            input["action"] = json!(decision());
            let review_action =
                review_action_from_json(&input, "user:rpc-preview", false).map_err(schema)?;
            Ok(review_preview_json(
                &inbox.preview_review(id, review_action).map_err(schema)?,
            ))
        }
        "decide" | "defer" | "reopen" => {
            if !command.confirm
                || string_param(params, "id").is_none()
                || params.get("expected_revision").is_none()
                || params.get("idempotency_key").is_none()
            {
                return Err(invalid(review_rpc_error(
                    "memory review mutation requires id, preview revision, idempotency key, and confirm=true",
                )));
            }
            let mut input = params.clone();
            input["action"] = json!(if action == "decide" {
                decision()
            } else {
                action.to_string()
            });
            let review_action =
                review_action_from_json(&input, "user:rpc", true).map_err(schema)?;
            Ok(review_application_json(
                &inbox
                    .apply_review_action(id()?, review_action)
                    .map_err(schema)?,
            ))
        }
        "batch_status" => {
            let job = string_param(params, "job_id").ok_or_else(|| {
                invalid(review_rpc_error(
                    "memory review batch_status requires string job_id",
                ))
            })?;
            Ok(review_batch_job_json(
                &inbox.review_batch_job(job).map_err(schema)?,
            ))
        }
        "preview_batch" | "batch" | "batch_start" => {
            let Some(raw_items) = params.get("items").and_then(Value::as_array) else {
                return Err(invalid(review_rpc_error(
                    "memory review batch requires an items array",
                )));
            };
            let apply = action != "preview_batch";
            if apply && !command.confirm {
                return Err(invalid(review_rpc_error(
                    "memory review batch apply requires confirm=true",
                )));
            }
            let mut items = Vec::new();
            for item in raw_items {
                let Some(review_id) = item.get("id").and_then(Value::as_str) else {
                    return Err(invalid(review_rpc_error("batch item requires string id")));
                };
                items.push(ReviewBatchItem {
                    review_id: review_id.to_string(),
                    action: review_action_from_json(
                        item,
                        if apply {
                            "user:rpc"
                        } else {
                            "user:rpc-preview"
                        },
                        apply,
                    )
                    .map_err(schema)?,
                });
            }
            let token = string_param(params, "idempotency_token")
                .unwrap_or("")
                .to_string();
            match action {
                "preview_batch" => Ok(review_batch_preview_json(
                    &inbox.preview_review_batch(&items).map_err(schema)?,
                )),
                "batch_start" => Ok(review_batch_job_json(
                    &inbox.start_review_batch(items, token).map_err(schema)?,
                )),
                _ => Ok(review_batch_result_json(
                    &inbox.apply_review_batch(items, token).map_err(schema)?,
                )),
            }
        }
        _ => Err(invalid(review_rpc_error(
            "memory review action must be list, show, next, preview, decide, defer, reopen, history, \
             preview_batch, batch_start, batch_status, or batch",
        ))),
    }
}

/// Handles one RPC `memory` command.
pub fn handle_memory_rpc(context: &RpcMemoryContext, command: &RpcMemoryCommand) -> RpcResult {
    if command.action == "status" {
        return status(context);
    }
    let Some(memory) = context.runtime.as_deref() else {
        return Err(invalid("built-in memory is disabled for this RPC runtime"));
    };
    let params = &command.params;
    match command.action.as_str() {
        "dream" => dream(memory, params),
        "recall" | "explain" => recall(context, memory, command),
        "retrieval_rebuild" => {
            if !command.confirm {
                return Err(invalid("memory retrieval_rebuild requires confirm=true"));
            }
            if params
                .get("owner_key")
                .is_some_and(|value| !value.is_string())
            {
                return Err(invalid(
                    "memory retrieval_rebuild owner_key must be a string",
                ));
            }
            let report = memory
                .rebuild_retrieval(string_param(params, "owner_key"))
                .map_err(from_error)?;
            Ok(canonical(&json!({
                "owners": report.owners, "chunks_invalidated": report.chunks_invalidated,
                "vectors_invalidated": report.vectors_invalidated,
                "jobs_superseded": report.jobs_superseded, "jobs_enqueued": report.jobs_enqueued,
            })))
        }
        "remember" => {
            if !command.confirm {
                return Err(invalid("memory remember requires confirm=true"));
            }
            let Some(content) =
                string_param(params, "content").filter(|content| !content.is_empty())
            else {
                return Err(invalid("memory remember requires non-empty params.content"));
            };
            let status = string_param(params, "status").unwrap_or("hypothesis");
            if status != "candidate" && status != "hypothesis" {
                return Err(invalid(
                    "memory remember status must be candidate or hypothesis",
                ));
            }
            let write = MemoryWrite {
                content: content.to_string(),
                memory_type: MemoryType::parse(
                    string_param(params, "memory_type").unwrap_or("observation"),
                )
                .map_err(from_error)?,
                status: MemoryStatus::parse(status).map_err(from_error)?,
                source: "user:rpc".into(),
                provenance: Provenance {
                    source_type: SourceType::Manual,
                    created_by: "user".into(),
                    ..Default::default()
                },
                ..Default::default()
            };
            let id = memory
                .memories()
                .remember(&memory.scope(), write)
                .map_err(from_error)?;
            Ok(json!({"created": true, "id": id, "status": status}))
        }
        "review" => review(memory, command),
        "feedback" => {
            if !command.confirm || params.get("id").is_none() || params.get("outcome").is_none() {
                return Err(invalid(
                    "memory feedback requires id, outcome, and confirm=true",
                ));
            }
            let (Some(id), Some(outcome)) =
                (string_param(params, "id"), string_param(params, "outcome"))
            else {
                return Err(invalid(
                    "memory feedback requires id, outcome, and confirm=true",
                ));
            };
            let outcome = ProcedureOutcome::parse(outcome);
            if outcome == ProcedureOutcome::Unknown {
                return Err(invalid(
                    "memory feedback outcome must be success, failure, partial, or unused",
                ));
            }
            let (run, recorded) = memory
                .record_feedback(ProcedureFeedback {
                    procedure_id: id.to_string(),
                    outcome,
                    task_id: string_param(params, "task_id").unwrap_or("").to_string(),
                    notes: string_param(params, "notes").unwrap_or("").to_string(),
                    evidence_uri: String::new(),
                    deduplication_key: String::new(),
                })
                .map_err(from_error)?;
            Ok(json!({"memory_id": recorded.id, "run_id": run.id}))
        }
        "forget" => {
            let id = string_param(params, "id").filter(|id| !id.is_empty());
            let (true, Some(id)) = (command.confirm, id) else {
                return Err(invalid("memory forget requires string id and confirm=true"));
            };
            let removed = memory
                .memories()
                .forget(&memory.scope(), id)
                .map_err(from_error)?;
            Ok(json!({"forgotten": removed, "id": id}))
        }
        _ => Err(invalid("unsupported memory RPC action")),
    }
}
