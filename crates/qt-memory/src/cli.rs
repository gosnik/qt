//! `qt memory` command set as a library function (archived
//! `memory_command.cpp`): arguments in, text or JSON out.

use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use serde_json::{Value, json};

use crate::code::CodeSnapshotProvider;
use crate::coordinator::MemoryCoordinatorSettings;
use crate::error::{MemoryError, Result};
use crate::model::*;
use crate::qtmem::{is_sqlite_database, read_qtmem_v3};
use crate::recall::{GraphRecallOptions, RecallRequest, RecallService};
use crate::retrieval::{
    Embedder, RetrievalEngineOptions, context_preview_json, recalled_memory_json,
    retrieval_candidate_json,
};
use crate::review::{
    ReviewGovernancePolicy, ReviewItem, ReviewQuery, ReviewScopeSelector, ReviewService,
    hydrated_review_json, review_action_from_json, review_action_record_json,
    review_application_json, review_item_json, review_preview_json, review_queue_score,
    review_statistics_json,
};
use crate::runtime::{MemoryRuntime, MemoryRuntimeOptions};
use crate::scope::{derive_runtime_memory_scope, durable_memory_scope, scope_key};
use crate::service::MemoryService;
use crate::settings::MemorySettings;
use crate::store::{MemoryStore, SCHEMA_VERSION};
use crate::text::{canonical, dump, normalize_content};

/// Usage text of `qt memory --help`.
pub const HELP: &str = "Usage: qt memory <command> [arguments] [options]

Commands:
  status                         Show configuration, scope, pre-handler, and statistics
  recall|search <query>          Recall governed memories
  explain <query>                Preview memory selection without recording usage
  remember <content>             Store a candidate/hypothesis (never auto-validates)
  review list [filters]          List prioritized review items
  review show <id>               Show hydrated proposal, sources, and risk
  review next [filters]          Show the next prioritized review
  review preview <id> <action>   Preview exact governed effects
  review decide <id> <action>    Apply a previewed action (--yes required)
  review defer <id> --until TIME Defer without changing memory (--yes required)
  review reopen <id> --note TEXT Reopen a terminal review (--yes required)
  review history [id]            Show durable review action history
  conflicts                      List explicit contradiction graph edges
  stale                          List expired, deprecated, and superseded memories
  health                         Check schema, database path, and accessibility
  feedback <id> <outcome>        Record procedure outcome
  forget <id> --yes              Permanently delete one in-scope memory
  export <path|->                Export the current scope as JSON
  import <path> [--dry-run] [--replace --yes]
                                 Import native JSON or standalone qtmem v3 SQLite
  prune --older-than-days N [--keep-latest N] [--dry-run|--yes]
                                 Prune raw evidence; canonical, audit, and blocks require
                                 --include-memories, --include-audit, or --include-blocks
  scrub [--batch-limit N] [--dry-run|--yes]
                                 Redact sensitive data in historical in-scope rows

Review filters: --status STATUS --kind KIND --priority PRIORITY --tag TAG
  --scope all|exact|durable --source-type TYPE --dream-cycle ID
  --stale|--not-stale --conflict|--not-conflict
  --created-after AGE --created-before AGE --decided-after AGE
Retrieval: --subject ENTITY --predicate RELATION --object ENTITY --as-of EPOCH_SECONDS
  explain also accepts --token-budget N (1..10000)
Options: --json --global --limit N --offset N --oldest-first --note TEXT
";

/// Where and with which configuration the command runs.
#[derive(Clone, Default)]
pub struct MemoryCommandEnvironment {
    pub cwd: PathBuf,
    pub agent_home: PathBuf,
    pub settings: MemorySettings,
    /// Code index for `explain` (qt-index).
    pub code: Option<Arc<dyn CodeSnapshotProvider>>,
    /// Embedding provider for dense `explain` (qt-llama).
    pub embedder: Option<Arc<dyn Embedder>>,
    /// Diagnostics from building the providers above.
    pub retrieval_warnings: Vec<String>,
}

/// Exit code and captured streams.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct CommandOutput {
    pub exit_code: i32,
    pub stdout: String,
    pub stderr: String,
}

#[derive(Debug, Clone)]
struct Parsed {
    operation: String,
    positional: Vec<String>,
    tags: Vec<String>,
    memory_type: String,
    status: String,
    status_set: bool,
    source: String,
    source_uri: String,
    note: String,
    task_id: String,
    evidence_uri: String,
    until: String,
    target_id: String,
    edit_content: String,
    kinds: Vec<String>,
    priorities: Vec<String>,
    source_types: Vec<String>,
    dream_cycles: Vec<String>,
    review_scope: String,
    created_after: String,
    created_before: String,
    decided_after: String,
    subject: String,
    predicate: String,
    object: String,
    as_of: Option<i64>,
    token_budget: Option<usize>,
    limit: usize,
    offset: usize,
    older_than_days: usize,
    keep_latest: usize,
    batch_limit: usize,
    json: bool,
    global_user: bool,
    yes: bool,
    replace: bool,
    dry_run: bool,
    no_memory: bool,
    include_memories: bool,
    include_audit: bool,
    include_blocks: bool,
    oldest_first: bool,
    stale: Option<bool>,
    conflict: Option<bool>,
    help: bool,
}

impl Default for Parsed {
    fn default() -> Self {
        Self {
            operation: "status".into(),
            positional: Vec::new(),
            tags: Vec::new(),
            memory_type: "observation".into(),
            status: "hypothesis".into(),
            status_set: false,
            source: String::new(),
            source_uri: String::new(),
            note: String::new(),
            task_id: String::new(),
            evidence_uri: String::new(),
            until: String::new(),
            target_id: String::new(),
            edit_content: String::new(),
            kinds: Vec::new(),
            priorities: Vec::new(),
            source_types: Vec::new(),
            dream_cycles: Vec::new(),
            review_scope: "all".into(),
            created_after: String::new(),
            created_before: String::new(),
            decided_after: String::new(),
            subject: String::new(),
            predicate: String::new(),
            object: String::new(),
            as_of: None,
            token_budget: None,
            limit: 20,
            offset: 0,
            older_than_days: 0,
            keep_latest: 0,
            batch_limit: 1000,
            json: false,
            global_user: false,
            yes: false,
            replace: false,
            dry_run: false,
            no_memory: false,
            include_memories: false,
            include_audit: false,
            include_blocks: false,
            oldest_first: false,
            stale: None,
            conflict: None,
            help: false,
        }
    }
}

fn invalid<T>(message: impl Into<String>) -> Result<T> {
    Err(MemoryError::invalid(message))
}

fn parse_size(value: &str, option: &str) -> Result<usize> {
    match value.parse::<u64>() {
        Ok(parsed) if (1..=10000).contains(&parsed) => Ok(parsed as usize),
        _ => invalid(format!("{option} requires a number from 1 to 10000")),
    }
}

fn parse(args: &[String]) -> Result<Parsed> {
    let mut parsed = Parsed::default();
    let mut index = 0usize;
    if let Some(first) = args.first()
        && !first.is_empty()
        && !first.starts_with('-')
    {
        parsed.operation = first.clone();
        index = 1;
    }
    while index < args.len() {
        let arg = args[index].as_str();
        let mut value = |option: &str| -> Result<String> {
            index += 1;
            args.get(index)
                .cloned()
                .ok_or_else(|| MemoryError::invalid(format!("{option} requires a value")))
        };
        match arg {
            "--json" => parsed.json = true,
            "--global" => parsed.global_user = true,
            "--yes" | "-y" => parsed.yes = true,
            "--replace" => parsed.replace = true,
            "--dry-run" => parsed.dry_run = true,
            "--include-memories" => parsed.include_memories = true,
            "--include-audit" => parsed.include_audit = true,
            "--include-blocks" => parsed.include_blocks = true,
            "--no-memory" => parsed.no_memory = true,
            "--no-tools" => {}
            "--help" | "-h" => parsed.help = true,
            "--limit" => parsed.limit = parse_size(&value("--limit")?, "--limit")?,
            "--token-budget" => {
                parsed.token_budget = Some(parse_size(&value("--token-budget")?, "--token-budget")?)
            }
            "--subject" => parsed.subject = value("--subject")?,
            "--predicate" => parsed.predicate = value("--predicate")?,
            "--object" => parsed.object = value("--object")?,
            "--as-of" => {
                let seconds = value("--as-of")?
                    .parse::<i64>()
                    .ok()
                    .filter(|seconds| seconds.checked_mul(1000).is_some())
                    .ok_or_else(|| {
                        MemoryError::invalid("--as-of requires supported epoch seconds")
                    })?;
                parsed.as_of = Some(seconds * 1000);
            }
            "--older-than-days" => {
                parsed.older_than_days =
                    parse_size(&value("--older-than-days")?, "--older-than-days")?
            }
            "--keep-latest" => {
                parsed.keep_latest = parse_size(&value("--keep-latest")?, "--keep-latest")?
            }
            "--batch-limit" => {
                parsed.batch_limit = parse_size(&value("--batch-limit")?, "--batch-limit")?
            }
            "--tag" => parsed.tags.push(value("--tag")?),
            "--type" => parsed.memory_type = value("--type")?,
            "--status" => {
                parsed.status = value("--status")?;
                parsed.status_set = true;
            }
            "--kind" => parsed.kinds.push(value("--kind")?),
            "--priority" => parsed.priorities.push(value("--priority")?),
            "--source-type" => parsed.source_types.push(value("--source-type")?),
            "--dream-cycle" => parsed.dream_cycles.push(value("--dream-cycle")?),
            "--scope" => parsed.review_scope = value("--scope")?,
            "--created-after" => parsed.created_after = value("--created-after")?,
            "--created-before" => parsed.created_before = value("--created-before")?,
            "--decided-after" => parsed.decided_after = value("--decided-after")?,
            "--stale" => parsed.stale = Some(true),
            "--not-stale" => parsed.stale = Some(false),
            "--conflict" => parsed.conflict = Some(true),
            "--not-conflict" => parsed.conflict = Some(false),
            "--offset" => {
                parsed.offset = value("--offset")?
                    .parse::<usize>()
                    .ok()
                    .filter(|offset| *offset <= 100000)
                    .ok_or_else(|| {
                        MemoryError::invalid("--offset requires a number from 0 to 100000")
                    })?;
            }
            "--oldest-first" => parsed.oldest_first = true,
            "--source" => parsed.source = value("--source")?,
            "--source-uri" => parsed.source_uri = value("--source-uri")?,
            "--note" => parsed.note = value("--note")?,
            "--task-id" => parsed.task_id = value("--task-id")?,
            "--evidence-uri" => parsed.evidence_uri = value("--evidence-uri")?,
            "--until" => parsed.until = value("--until")?,
            "--target-id" => parsed.target_id = value("--target-id")?,
            "--content" => parsed.edit_content = value("--content")?,
            other if other.starts_with('-') && !other.is_empty() => {
                return invalid(format!("unknown memory option: {other}"));
            }
            other => parsed.positional.push(other.to_string()),
        }
        index += 1;
    }
    Ok(parsed)
}

fn duration_seconds(amount: i64, suffix: char) -> i64 {
    match suffix {
        'm' => amount * 60,
        'h' => amount * 3600,
        'd' => amount * 86400,
        _ => amount * 604800,
    }
}

/// `--until`: a positive duration (`2h`, `3d`) from now or Unix ms.
pub fn parse_review_time(value: &str, now: Millis) -> Result<Millis> {
    let Some(suffix) = value.chars().last() else {
        return invalid("--until requires a value");
    };
    if matches!(suffix, 'm' | 'h' | 'd' | 'w') {
        return match value[..value.len() - 1].parse::<i64>() {
            Ok(amount) if amount > 0 => Ok(now + duration_seconds(amount, suffix) * 1000),
            _ => invalid("--until duration must be positive, for example 2h or 3d"),
        };
    }
    value.parse::<i64>().map_err(|_| {
        MemoryError::invalid("--until must be a Unix millisecond timestamp or duration such as 2h")
    })
}

/// Review filter times: an age (`7d`) before now or Unix ms.
pub fn parse_review_filter_time(value: &str, now: Millis) -> Result<Millis> {
    let Some(suffix) = value.chars().last() else {
        return invalid("review time filter requires a value");
    };
    if matches!(suffix, 'm' | 'h' | 'd' | 'w') {
        return match value[..value.len() - 1].parse::<i64>() {
            Ok(amount) if amount >= 0 => Ok(now - duration_seconds(amount, suffix) * 1000),
            _ => invalid("review age must be positive, for example 2h or 7d"),
        };
    }
    value.parse::<i64>().map_err(|_| {
        MemoryError::invalid("review time must be a Unix millisecond timestamp or age such as 7d")
    })
}

/// Normalises qtmem JSON exports (`entries`, `edges`, legacy field names).
pub fn normalize_import_state(mut state: Value) -> Result<Value> {
    let Some(object) = state.as_object_mut() else {
        return invalid("memory import root must be a JSON object");
    };
    if object.contains_key("entries") && !object.contains_key("memories") {
        let entries = object["entries"].clone();
        object.insert("memories".into(), entries);
    }
    if object.contains_key("edges") && !object.contains_key("graph_edges") {
        let edges = object["edges"].clone();
        object.insert("graph_edges".into(), edges);
    }
    if !object.get("memories").is_some_and(Value::is_array) {
        return invalid("memory import requires a memories or qtmem entries array");
    }
    for collection in [
        "graph_edges",
        "procedure_runs",
        "memory_revisions",
        "sources",
        "source_events",
        "review_items",
        "audit_events",
        "session_events",
        "blocks",
    ] {
        let entry = object.entry(collection).or_insert_with(|| json!([]));
        if !entry.is_array() {
            return invalid(format!(
                "memory import collection must be an array: {collection}"
            ));
        }
    }
    let rename = |items: &mut Value, pairs: &[(&str, &str)]| {
        for item in items.as_array_mut().into_iter().flatten() {
            let Some(item) = item.as_object_mut() else {
                continue;
            };
            for (target, source) in pairs {
                if !item.contains_key(*target)
                    && let Some(value) = item.get(*source).cloned()
                {
                    let value = if *target == "entry_json" {
                        json!(dump(&value))
                    } else {
                        value
                    };
                    item.insert((*target).into(), value);
                }
            }
        }
    };
    rename(
        object.get_mut("memories").expect("checked"),
        &[
            ("type", "memory_type"),
            ("agent", "agent_id"),
            ("conversation", "conversation_id"),
        ],
    );
    rename(
        object.get_mut("memory_revisions").expect("inserted"),
        &[("kind", "revision_kind"), ("entry_json", "entry")],
    );
    rename(
        object.get_mut("sources").expect("inserted"),
        &[
            ("type", "source_type"),
            ("uri", "source_uri"),
            ("hash", "source_hash"),
        ],
    );
    Ok(state)
}

fn write_secure_json(path: &Path, value: &Value) -> Result<()> {
    let text = format!(
        "{}\n",
        serde_json::to_string_pretty(&canonical(value)).unwrap_or_default()
    );
    std::fs::write(path, text).map_err(|_| {
        MemoryError::storage(format!("unable to write memory JSON: {}", path.display()))
    })?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt as _;
        std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o600)).map_err(
            |error| MemoryError::storage(format!("unable to secure memory JSON: {error}")),
        )?;
    }
    Ok(())
}

/// Scope summary shown by status and import/prune/scrub.
pub fn scope_summary_json(scope: &MemoryScope) -> Value {
    json!({
        "kind": scope.kind.as_str(), "user": scope.user, "project": scope.project,
        "repository": scope.repository, "repository_display": scope.repository_display,
        "branch": scope.branch, "session": scope.session,
    })
}

/// Grouped statistics (`catalog`, `usage`, `governance`, `dreaming`).
pub fn statistics_json(stats: &MemoryStatistics) -> Value {
    let dreaming = &stats.dreaming;
    json!({
        "catalog": {
            "total": stats.memory_entries, "by_type": stats.memory_counts,
            "by_status": stats.trust_states, "hypotheses": stats.hypotheses,
            "low_confidence": stats.low_confidence, "unprovenanced": stats.unprovenanced,
            "deprecated": stats.deprecated, "failed_procedures": stats.failed_procedures,
            "high_use_hypotheses": stats.high_use_hypotheses,
        },
        "usage": {
            "accessed_memories": stats.accessed_memories, "total_accesses": stats.total_accesses,
            "successful_outcomes": stats.successful_outcomes, "failed_outcomes": stats.failed_outcomes,
        },
        "governance": {
            "conflicts": stats.conflict_count, "unresolved_conflicts": stats.unresolved_conflicts,
            "pending_reviews": stats.pending_reviews,
        },
        "dreaming": {
            "attempted": dreaming.attempted, "completed": dreaming.completed, "failed": dreaming.failed,
            "pending": dreaming.pending, "protocol_rejections": dreaming.protocol_rejections,
            "timed_out": dreaming.timed_out, "cancelled": dreaming.cancelled,
            "start_failures": dreaming.start_failures, "cpu_fallbacks": dreaming.cpu_fallbacks,
            "reviews_created": dreaming.reviews_created, "by_trigger": dreaming.by_trigger,
            "by_model": dreaming.by_model, "failures_by_reason": dreaming.failures_by_reason,
        },
    })
}

/// CLI memory row (archived `memory_json` of `memory_command.cpp`).
pub fn cli_memory_json(memory: &MemoryEntry) -> Value {
    json!({
        "id": memory.id, "type": memory.memory_type.as_str(), "status": memory.status.as_str(),
        "content": memory.content, "source": memory.source, "tags": memory.tags,
        "confidence": memory.confidence, "quality_score": memory.quality_score,
        "access_count": memory.access_count, "source_uri": memory.provenance.source_uri,
        "created_by": memory.provenance.created_by, "success_count": memory.success_count,
        "failure_count": memory.failure_count, "last_outcome": memory.last_outcome.as_str(),
    })
}

/// Dreaming configuration summary shared by status surfaces.
pub fn dreaming_settings_json(settings: &MemorySettings, active: bool) -> Value {
    let dreaming = &settings.dreaming;
    json!({
        "enabled": active && dreaming.enabled, "configured": dreaming.enabled,
        "trigger": dreaming.trigger, "interval_seconds": dreaming.interval_seconds,
        "idle_seconds": dreaming.idle_seconds, "candidate_threshold": dreaming.candidate_threshold,
        "model": dreaming.model, "verifier_model": dreaming.verifier_model,
        "timeout_ms": dreaming.timeout_ms, "max_model_input_tokens": dreaming.max_model_input_tokens,
        "max_candidates": dreaming.max_candidates, "research_policy": dreaming.research_policy,
        "promotion_policy": dreaming.promotion_policy,
    })
}

/// Text rendering of the archived `emit`: arrays list `id [status] (type)
/// content from -> to`; objects list `key: value`.
pub fn render_text(value: &Value, heading: &str) -> String {
    let mut out = String::new();
    if !heading.is_empty() {
        out += &format!("{heading}:\n");
    }
    let value = canonical(value);
    match &value {
        Value::Array(items) => {
            if items.is_empty() {
                out += "  (none)\n";
            }
            for item in items {
                let field = |name: &str| item.get(name).and_then(Value::as_str).unwrap_or("");
                if item.get("id").is_some() {
                    out += &format!("  {}", field("id"));
                }
                if item.get("status").is_some() {
                    out += &format!(" [{}]", field("status"));
                }
                if item.get("type").is_some() {
                    out += &format!(" ({})", field("type"));
                }
                if item.get("content").is_some() {
                    out += &format!(" {}", field("content"));
                }
                if item.get("from_id").is_some() {
                    out += &format!(" {} -> {}", field("from_id"), field("to_id"));
                }
                out.push('\n');
            }
        }
        Value::Object(map) => {
            for (key, item) in map {
                match item {
                    Value::String(text) => out += &format!("  {key}: {text}\n"),
                    other => out += &format!("  {key}: {}\n", other),
                }
            }
        }
        other => out += &format!("{other}\n"),
    }
    out
}

struct Emitter {
    json: bool,
    stdout: String,
}

impl Emitter {
    fn emit(&mut self, value: Value, heading: &str) {
        if self.json {
            self.stdout += &dump(&value);
            self.stdout.push('\n');
        } else {
            self.stdout += &render_text(&value, heading);
        }
    }
}

fn review_cards(reviews: &[ReviewItem], json: bool) -> String {
    if json {
        return format!(
            "{}\n",
            dump(
                &json!({"items": reviews.iter().map(review_item_json).collect::<Vec<_>>(), "count": reviews.len()})
            )
        );
    }
    let mut out = format!("Memory Review Inbox ({}):\n", reviews.len());
    if reviews.is_empty() {
        out += "  No reviews match the active filters.\n";
        return out;
    }
    for (index, review) in reviews.iter().enumerate() {
        out += &format!(
            "  {}. [{}] {} — {}\n     {}  subject: {}  status: {}\n",
            index + 1,
            review.priority,
            review.kind,
            review.reason,
            review.id,
            if review.subject_id.is_empty() {
                "none"
            } else {
                &review.subject_id
            },
            review.status
        );
    }
    out
}

/// The scope a command runs in: the derived runtime scope, or the global
/// user scope with `--global`.
pub fn command_scope(cwd: &Path, global_user: bool) -> MemoryScope {
    let scope = derive_runtime_memory_scope(cwd, "", "");
    if !global_user {
        return scope;
    }
    MemoryScope {
        kind: ScopeKind::GlobalUser,
        user: scope.user,
        ..Default::default()
    }
}

/// Recall policy configured by `memory.recallPolicy`.
pub fn configured_policy(settings: &MemorySettings) -> RecallPolicy {
    RecallPolicy::from_setting(&settings.recall_policy)
}

/// Graph options configured by `memory.retrieval`.
pub fn configured_graph(settings: &MemorySettings) -> GraphRecallOptions {
    let retrieval = &settings.retrieval;
    GraphRecallOptions {
        enabled: retrieval.graph_enabled,
        max_depth: retrieval.max_depth as usize,
        max_seeds: retrieval.max_seeds as usize,
        max_edges: retrieval.max_edges as usize,
        max_fanout: retrieval.max_fanout as usize,
        max_results: retrieval.max_graph_results as usize,
        ..Default::default()
    }
}

/// A non-capturing, non-indexing runtime used for retrieval previews
/// (archived `make_memory_preview_services`).
pub fn preview_runtime(
    environment: &MemoryCommandEnvironment,
    scope: MemoryScope,
) -> Result<MemoryRuntime> {
    let settings = &environment.settings;
    let retrieval = &settings.retrieval;
    let policy = configured_policy(settings);
    let code_enabled = retrieval.code_enabled && environment.code.is_some();
    MemoryRuntime::new(MemoryRuntimeOptions {
        agent_home: environment.agent_home.clone(),
        cwd: environment.cwd.clone(),
        initial_scope: Some(scope),
        background_processing: false,
        automatic_capture: false,
        coordinator: MemoryCoordinatorSettings {
            enabled: true,
            automatic_recall: false,
            context_token_budget: settings.context_token_budget as usize,
            candidate_limit: settings.candidate_limit as usize,
            recall_policy: policy,
            graph: configured_graph(settings),
            dense_enabled: retrieval.dense_enabled,
            ..Default::default()
        },
        retrieval: RetrievalEngineOptions {
            enabled: retrieval.graph_enabled || retrieval.dense_enabled || retrieval.code_enabled,
            background_indexing: false,
            dense_enabled: retrieval.dense_enabled,
            code_enabled,
            code_graph_enabled: code_enabled && retrieval.graph_enabled,
            code: environment.code.clone(),
            policy,
            cache_entries: retrieval.query_cache_entries as usize,
            graph_max_seeds: retrieval.max_seeds as usize,
            graph_max_depth: retrieval.max_depth as usize,
            graph_max_edges: retrieval.max_edges as usize,
            graph_max_results: retrieval.max_graph_results as usize,
            graph_max_fanout: retrieval.max_fanout as usize,
            embedder: environment.embedder.clone(),
            ..Default::default()
        },
        ..Default::default()
    })
}

/// Runs `qt memory <args>` (arguments after `memory`).
pub fn run_memory_command(
    args: &[String],
    environment: &MemoryCommandEnvironment,
) -> CommandOutput {
    let parsed = match parse(args) {
        Ok(parsed) => parsed,
        Err(error) => {
            return CommandOutput {
                exit_code: 1,
                stdout: String::new(),
                stderr: format!("qt memory: {error}\n"),
            };
        }
    };
    if parsed.help {
        return CommandOutput {
            exit_code: 0,
            stdout: HELP.into(),
            stderr: String::new(),
        };
    }
    if parsed.no_memory {
        return CommandOutput {
            exit_code: 1,
            stdout: String::new(),
            stderr: "qt memory: disabled for this invocation by --no-memory\n".into(),
        };
    }
    let mut emitter = Emitter {
        json: parsed.json,
        stdout: String::new(),
    };
    match execute(&parsed, environment, &mut emitter) {
        Ok(exit_code) => CommandOutput {
            exit_code,
            stdout: emitter.stdout,
            stderr: String::new(),
        },
        Err(error) => CommandOutput {
            exit_code: 1,
            stdout: emitter.stdout,
            stderr: format!("qt memory: {error}\n"),
        },
    }
}

fn recall_request(parsed: &Parsed, settings: &MemorySettings) -> RecallRequest {
    let mut request = RecallRequest {
        query: parsed.positional.join(" "),
        tags: parsed.tags.clone(),
        include_global_user: parsed.global_user,
        candidate_limit: parsed.limit.max(50),
        result_limit: parsed.limit,
        graph: configured_graph(settings),
        policy: configured_policy(settings),
        ..Default::default()
    };
    request.structured.subject = parsed.subject.clone();
    request.structured.predicate = parsed.predicate.clone();
    request.structured.object = parsed.object.clone();
    request.structured.as_of = parsed.as_of;
    request.structured.enabled = parsed.as_of.is_some()
        || !parsed.subject.is_empty()
        || !parsed.predicate.is_empty()
        || !parsed.object.is_empty();
    request
}

fn execute(
    parsed: &Parsed,
    environment: &MemoryCommandEnvironment,
    out: &mut Emitter,
) -> Result<i32> {
    let settings = &environment.settings;
    let store = Arc::new(MemoryStore::open(&environment.agent_home)?);
    let memories = MemoryService::new(Arc::clone(&store));
    let reviews = ReviewService::new(
        Arc::clone(&store),
        ReviewGovernancePolicy {
            allow_dream_hypothesis_promotion: settings.dreaming.promotion_policy != "disabled",
        },
    );
    let scope = command_scope(&environment.cwd, parsed.global_user);
    let operation = parsed.operation.as_str();
    match operation {
        "status" | "health" => {
            let statistics = store.statistics(
                &scope,
                if parsed.global_user {
                    RecallPolicy::Strict
                } else {
                    RecallPolicy::Balanced
                },
                parsed.global_user,
            )?;
            let mut result = json!({
                "enabled": settings.enabled,
                "automatic_recall": settings.automatic_recall,
                "automatic_capture": settings.automatic_capture,
                "prehandler_enabled": settings.enabled && settings.automatic_recall,
                "dreaming": dreaming_settings_json(settings, settings.enabled),
                "database": store.path().to_string_lossy(),
                "memory_root": store.root().to_string_lossy(),
                "schema_version": store.schema_version(),
                "scope": scope_summary_json(&scope),
                "scope_memory_count": statistics.memory_entries,
                "statistics": statistics_json(&statistics),
                "review_statistics": review_statistics_json(&reviews.statistics_including_durable(&scope)?),
            });
            if operation == "health" {
                let integrity = store.integrity_check()?;
                let bytes = std::fs::metadata(store.path()).map(|metadata| metadata.len());
                result["healthy"] =
                    json!(store.schema_version() == SCHEMA_VERSION && integrity == "ok");
                result["integrity"] = json!(integrity);
                result["database_bytes"] = json!(bytes.as_ref().map_or(0, |bytes| *bytes));
                result["database_readable"] = json!(bytes.is_ok());
            }
            out.emit(
                result,
                if operation == "health" {
                    "Memory health"
                } else {
                    "Memory status"
                },
            );
            Ok(0)
        }
        "recall" | "search" | "explain" => {
            if parsed.positional.is_empty() {
                return invalid(format!("{operation} requires a query"));
            }
            let request = recall_request(parsed, settings);
            if operation == "explain" {
                let runtime = preview_runtime(environment, scope)?;
                let token_budget = parsed
                    .token_budget
                    .unwrap_or(settings.context_token_budget as usize);
                let preview = runtime.preview_retrieval(request.clone(), token_budget)?;
                let mut observed: Vec<String> = Vec::new();
                let mut note = |name: &str| {
                    if !observed.iter().any(|item| item == name) {
                        observed.push(name.to_string());
                    }
                };
                let memories: Vec<Value> = preview
                    .memories
                    .iter()
                    .map(|item| {
                        if item.score.lexical_rank != 0 {
                            note("lexical");
                        }
                        if item.score.graph_rank != 0 {
                            note("graph");
                        }
                        if item.score.dense_rank != 0 {
                            note("dense");
                        }
                        recalled_memory_json(
                            item,
                            runtime.current_retrieval_revision(
                                &item.memory.id,
                                request.policy,
                                request.include_global_user,
                            ),
                            Some(&preview.context),
                        )
                    })
                    .collect();
                let code: Vec<Value> = preview
                    .code
                    .iter()
                    .map(|item| {
                        for rank in &item.ranks {
                            note(&rank.retriever);
                        }
                        retrieval_candidate_json(item, Some(&preview.context))
                    })
                    .collect();
                let status = runtime.retrieval_status();
                let mut warnings = environment.retrieval_warnings.clone();
                for error in [&status.last_error, &status.queries.last_error] {
                    if !error.is_empty() {
                        warnings.push(error.clone());
                    }
                }
                let historical = parsed.as_of.is_some();
                let retrieval = &settings.retrieval;
                out.emit(
                    json!({
                        "memories": memories, "code_candidates": code,
                        "context": context_preview_json(&preview.context, token_budget, 4),
                        "requested_modes": {
                            "lexical": true, "graph": retrieval.graph_enabled && !historical,
                            "dense": retrieval.dense_enabled && !historical,
                            "code": retrieval.code_enabled && !historical,
                        },
                        "observed_retrievers": observed, "dense_generation": status.generation,
                        "retrieval_warnings": warnings,
                    }),
                    "Memory retrieval preview",
                );
                return Ok(0);
            }
            let recall = RecallService::new(Arc::clone(&store));
            let recalled = recall.recall(&scope, &request)?;
            recall.record_access(&scope, &recalled)?;
            let mut result = Vec::new();
            for item in &recalled {
                let mut value = cli_memory_json(&item.memory);
                value["score"] = json!(item.score.total);
                value["warnings"] = json!(item.warnings);
                value["explanation"] = recalled_memory_json(
                    item,
                    store
                        .retrieval_revision(&item.memory.scope, &item.memory.id)
                        .ok(),
                    None,
                );
                result.push(value);
            }
            out.emit(Value::Array(result), "Recalled memories");
            Ok(0)
        }
        "remember" => {
            if parsed.positional.is_empty() {
                return invalid("remember requires content");
            }
            if parsed.status != "candidate" && parsed.status != "hypothesis" {
                return invalid("remember status must be candidate or hypothesis");
            }
            let id = memories.remember(
                &scope,
                MemoryWrite {
                    content: parsed.positional.join(" "),
                    memory_type: MemoryType::parse(&parsed.memory_type)?,
                    status: MemoryStatus::parse(&parsed.status)?,
                    source: if parsed.source.is_empty() {
                        "user:cli".into()
                    } else {
                        parsed.source.clone()
                    },
                    tags: parsed.tags.clone(),
                    provenance: Provenance {
                        source_type: SourceType::Manual,
                        source_uri: parsed.source_uri.clone(),
                        created_by: "user".into(),
                        ..Default::default()
                    },
                    ..Default::default()
                },
            )?;
            out.emit(
                json!({"id": id, "status": parsed.status, "created": true}),
                "Memory created",
            );
            Ok(0)
        }
        "review" => review(parsed, &store, &reviews, &scope, out),
        "conflicts" => {
            let edges = store.query_edges(
                &scope,
                &GraphQuery {
                    edge_type: "contradicts".into(),
                    limit: parsed.limit,
                    ..Default::default()
                },
            )?;
            let result: Vec<Value> = edges
                .iter()
                .map(|edge| {
                    json!({
                        "id": edge.id, "from_id": edge.from_id, "to_id": edge.to_id,
                        "type": edge.edge_type, "confidence": edge.confidence, "source": edge.source,
                    })
                })
                .collect();
            out.emit(Value::Array(result), "Memory conflicts");
            Ok(0)
        }
        "stale" => {
            let stale = memories.query(
                &scope,
                &MemoryQuery {
                    statuses: vec![
                        MemoryStatus::Deprecated,
                        MemoryStatus::Expired,
                        MemoryStatus::Superseded,
                    ],
                    policy: RecallPolicy::Strict,
                    limit: parsed.limit,
                    ..Default::default()
                },
            )?;
            out.emit(
                Value::Array(stale.iter().map(cli_memory_json).collect()),
                "Stale memories",
            );
            Ok(0)
        }
        "feedback" => {
            if parsed.positional.len() != 2 {
                return invalid("feedback requires <id> <outcome>");
            }
            let outcome = ProcedureOutcome::parse(&parsed.positional[1]);
            if outcome == ProcedureOutcome::Unknown {
                return invalid("feedback outcome must be success, failure, partial, or unused");
            }
            let (run, memory) = store.add_feedback(
                &scope,
                ProcedureFeedback {
                    procedure_id: parsed.positional[0].clone(),
                    outcome,
                    task_id: parsed.task_id.clone(),
                    notes: parsed.note.clone(),
                    evidence_uri: parsed.evidence_uri.clone(),
                    deduplication_key: String::new(),
                },
            )?;
            out.emit(
                json!({"run_id": run.id, "memory": cli_memory_json(&memory)}),
                "Feedback recorded",
            );
            Ok(0)
        }
        "forget" => {
            if parsed.positional.len() != 1 {
                return invalid("forget requires exactly one memory id");
            }
            if !parsed.yes {
                return invalid("forget requires --yes authority");
            }
            let removed = memories.forget(&scope, &parsed.positional[0])?;
            out.emit(
                json!({"id": parsed.positional[0], "forgotten": removed}),
                "Memory forget",
            );
            Ok(if removed { 0 } else { 1 })
        }
        "export" => {
            if parsed.positional.len() != 1 {
                return invalid("export requires exactly one path or '-'");
            }
            let state = store.export_state(&scope)?;
            if parsed.positional[0] == "-" {
                out.stdout += &serde_json::to_string_pretty(&canonical(&state)).unwrap_or_default();
                out.stdout.push('\n');
            } else {
                let path = PathBuf::from(&parsed.positional[0]);
                write_secure_json(&path, &state)?;
                out.emit(
                    json!({"path": path.to_string_lossy(), "exported": true}),
                    "Memory exported",
                );
            }
            Ok(0)
        }
        "import" => import(parsed, &store, &memories, &scope, out),
        "prune" => {
            if !parsed.positional.is_empty() {
                return invalid("prune does not accept positional arguments");
            }
            if !parsed.dry_run && !parsed.yes {
                return invalid("prune requires --dry-run or --yes authority");
            }
            if parsed.older_than_days == 0 && parsed.keep_latest == 0 {
                return invalid("prune requires --older-than-days or --keep-latest");
            }
            let report = store.enforce_retention(
                &scope,
                &RetentionPolicy {
                    older_than: if parsed.older_than_days == 0 {
                        0
                    } else {
                        now_ms() - parsed.older_than_days as i64 * 86_400_000
                    },
                    keep_latest: parsed.keep_latest,
                    batch_limit: parsed.batch_limit,
                    raw_evidence: true,
                    canonical_memories: parsed.include_memories,
                    audit_events: parsed.include_audit,
                    context_blocks: parsed.include_blocks,
                    dry_run: parsed.dry_run,
                },
            )?;
            out.emit(
                json!({
                    "dry_run": report.dry_run, "session_events": report.session_events,
                    "source_events": report.source_events, "orphan_sources": report.orphan_sources,
                    "canonical_memories": report.canonical_memories, "audit_events": report.audit_events,
                    "context_blocks": report.context_blocks, "total": report.total(),
                    "target_scope": scope_summary_json(&scope),
                }),
                "Memory retention",
            );
            Ok(0)
        }
        "scrub" => {
            if !parsed.positional.is_empty() {
                return invalid("scrub does not accept positional arguments");
            }
            if !parsed.dry_run && !parsed.yes {
                return invalid("scrub requires --dry-run or --yes authority");
            }
            let report = store.scrub_sensitive_data(
                &scope,
                &RedactionScrubPolicy {
                    batch_limit: parsed.batch_limit,
                    dry_run: parsed.dry_run,
                },
            )?;
            let collections: Vec<Value> = report
                .collections
                .iter()
                .map(|item| json!({"name": item.name, "examined": item.examined, "changed": item.changed}))
                .collect();
            out.emit(
                json!({
                    "dry_run": report.dry_run, "truncated": report.truncated,
                    "examined": report.examined(), "changed": report.changed(),
                    "collections": collections, "target_scope": scope_summary_json(&scope),
                }),
                "Memory redaction scrub",
            );
            Ok(0)
        }
        other => invalid(format!("unknown memory command: {other}")),
    }
}

fn review(
    parsed: &Parsed,
    store: &MemoryStore,
    reviews: &ReviewService,
    scope: &MemoryScope,
    out: &mut Emitter,
) -> Result<i32> {
    let operation = parsed
        .positional
        .first()
        .map(String::as_str)
        .unwrap_or("list");
    let now = now_ms();
    let mut query = ReviewQuery {
        limit: parsed.limit,
        offset: parsed.offset,
        kinds: parsed.kinds.clone(),
        priorities: parsed.priorities.clone(),
        tags: parsed.tags.clone(),
        dream_cycles: parsed.dream_cycles.clone(),
        stale: parsed.stale,
        conflict: parsed.conflict,
        oldest_first: parsed.oldest_first,
        ..Default::default()
    };
    for source_type in &parsed.source_types {
        let value = SourceType::parse(source_type);
        if value == SourceType::Unknown && source_type != "unknown" {
            return invalid(format!("invalid review source type: {source_type}"));
        }
        query.source_types.push(value);
    }
    if !parsed.created_after.is_empty() {
        query.created_after = Some(parse_review_filter_time(&parsed.created_after, now)?);
    }
    if !parsed.created_before.is_empty() {
        query.created_before = Some(parse_review_filter_time(&parsed.created_before, now)?);
    }
    if !parsed.decided_after.is_empty() {
        query.decided_after = Some(parse_review_filter_time(&parsed.decided_after, now)?);
    }
    query.scope = match parsed.review_scope.as_str() {
        "all" => ReviewScopeSelector::All,
        "exact" => ReviewScopeSelector::Exact,
        "durable" => ReviewScopeSelector::Durable,
        _ => return invalid("--scope must be all, exact, or durable"),
    };
    if parsed.status_set {
        query.statuses = vec![ReviewStatus::parse(&parsed.status)?];
    }
    match operation {
        "list" | "next" => {
            if operation == "next" {
                query.limit = 1;
            }
            let mut expanded = query.clone();
            expanded.offset = 0;
            expanded.limit = (query.offset + query.limit).min(1000);
            let mut items = Vec::new();
            if query.scope != ReviewScopeSelector::Durable {
                items = reviews.query(scope, &expanded)?;
            }
            let durable = durable_memory_scope(scope.clone());
            if query.scope != ReviewScopeSelector::Exact
                && scope_key(&durable)? != scope_key(scope)?
            {
                items.extend(reviews.query(&durable, &expanded)?);
            }
            items.sort_by(|left, right| {
                review_queue_score(right, now)
                    .partial_cmp(&review_queue_score(left, now))
                    .unwrap_or(std::cmp::Ordering::Equal)
                    .then_with(|| {
                        if query.oldest_first {
                            left.created_at.cmp(&right.created_at)
                        } else {
                            right.created_at.cmp(&left.created_at)
                        }
                    })
                    .then_with(|| left.id.cmp(&right.id))
            });
            items.dedup_by(|right, left| right.id == left.id);
            let items: Vec<ReviewItem> = items
                .into_iter()
                .skip(query.offset)
                .take(query.limit)
                .collect();
            out.stdout += &review_cards(&items, out.json);
            Ok(0)
        }
        "show" => {
            if parsed.positional.len() != 2 {
                return invalid("review show requires a review id");
            }
            out.emit(
                hydrated_review_json(&reviews.hydrate(scope, &parsed.positional[1])?),
                "Memory review detail",
            );
            Ok(0)
        }
        "history" => {
            let id = parsed.positional.get(1).map(String::as_str);
            let items: Vec<Value> = store
                .review_history(scope, id, parsed.limit)?
                .iter()
                .map(review_action_record_json)
                .collect();
            out.emit(json!({"items": items}), "Memory review history");
            Ok(0)
        }
        "preview" | "decide" | "defer" | "reopen" => {
            let defer = operation == "defer";
            let reopen = operation == "reopen";
            let required = if defer || reopen { 2 } else { 3 };
            if parsed.positional.len() != required {
                return invalid(
                    "review action requires <id> and an action (defer/reopen infer the action)",
                );
            }
            let id = &parsed.positional[1];
            let mut input = json!({
                "action": if defer { "defer" } else if reopen { "reopen" } else { parsed.positional[2].as_str() },
                "note": parsed.note, "target_id": parsed.target_id, "evidence_uri": parsed.evidence_uri,
            });
            if !parsed.edit_content.is_empty() {
                input["edit"] = json!({"content": parsed.edit_content});
            }
            if defer {
                input["deferred_until"] = json!(parse_review_time(&parsed.until, now)?);
            }
            let action = review_action_from_json(&input, "user:cli", operation != "preview")?;
            if operation == "preview" {
                out.emit(
                    review_preview_json(&reviews.preview(scope, id, action)?),
                    "Memory review preview",
                );
                return Ok(0);
            }
            if !parsed.yes {
                return invalid("review mutation requires --yes authority");
            }
            if reopen && parsed.note.is_empty() {
                return invalid("review reopen requires --note with a reason");
            }
            out.emit(
                review_application_json(&reviews.apply(scope, id, action)?),
                "Review applied",
            );
            Ok(0)
        }
        _ => invalid("review expects list, show, next, preview, decide, defer, reopen, or history"),
    }
}

fn import(
    parsed: &Parsed,
    store: &MemoryStore,
    memories: &MemoryService,
    scope: &MemoryScope,
    out: &mut Emitter,
) -> Result<i32> {
    if parsed.positional.len() != 1 {
        return invalid("import requires exactly one JSON or SQLite path");
    }
    if parsed.replace && !parsed.yes && !parsed.dry_run {
        return invalid("import --replace requires --yes authority");
    }
    let path = PathBuf::from(&parsed.positional[0]);
    let (mut state, format, source_scopes, warnings) = if is_sqlite_database(&path) {
        let package = read_qtmem_v3(&path)?;
        (
            normalize_import_state(package.state)?,
            package.format,
            package.source_scopes,
            package.warnings,
        )
    } else {
        let text = std::fs::read_to_string(&path).map_err(|_| {
            MemoryError::storage(format!("unable to read memory import: {}", path.display()))
        })?;
        let state: Value = serde_json::from_str(&text).map_err(|error| {
            MemoryError::invalid(format!("memory import is not valid JSON: {error}"))
        })?;
        let format = if state.get("entries").is_some() {
            "qtmem-json-v3"
        } else {
            "native-json-v3"
        };
        (
            normalize_import_state(state)?,
            format.to_string(),
            Vec::new(),
            Vec::new(),
        )
    };
    let incoming = state["memories"].as_array().map_or(0, Vec::len);
    let existing_memories = memories.query(
        scope,
        &MemoryQuery {
            limit: 0,
            include_superseded: true,
            statuses: MemoryStatus::ALL.to_vec(),
            ..Default::default()
        },
    )?;
    let mut duplicates = 0usize;
    let mut duplicate_report = Vec::new();
    if !parsed.replace {
        let mut existing: HashSet<String> = existing_memories
            .iter()
            .map(|memory| normalize_content(&memory.content))
            .collect();
        let mut filtered = Vec::new();
        for memory in state["memories"].as_array().cloned().unwrap_or_default() {
            let content =
                normalize_content(memory.get("content").and_then(Value::as_str).unwrap_or(""));
            if !content.is_empty() && existing.contains(&content) {
                duplicates += 1;
                if duplicate_report.len() < 100 {
                    let field = |name: &str, fallback: &str| {
                        memory
                            .get(name)
                            .and_then(Value::as_str)
                            .unwrap_or(fallback)
                            .to_string()
                    };
                    duplicate_report.push(json!({
                        "source_id": field("id", ""), "type": field("type", "observation"),
                        "status": field("status", "hypothesis"),
                    }));
                }
                continue;
            }
            existing.insert(content);
            filtered.push(memory);
        }
        state["memories"] = Value::Array(filtered);
    }
    let to_import = state["memories"].as_array().map_or(0, Vec::len);
    if parsed.dry_run {
        out.emit(
            json!({
                "path": parsed.positional[0], "dry_run": true, "format": format,
                "incoming_memories": incoming, "duplicate_memories": duplicates,
                "duplicates": duplicate_report, "would_import_memories": to_import,
                "existing_scope_memories": existing_memories.len(), "replace": parsed.replace,
                "source_scopes": source_scopes, "target_scope": scope_summary_json(scope),
                "warnings": warnings,
            }),
            "Memory import dry run",
        );
        return Ok(0);
    }
    let backup = store.export_state_with_limits(scope, &ExportLimits::backup())?;
    let has_existing = [
        "memories",
        "graph_edges",
        "procedure_runs",
        "memory_revisions",
        "sources",
        "source_events",
        "review_items",
        "audit_events",
        "session_events",
        "blocks",
    ]
    .iter()
    .any(|collection| {
        backup
            .get(*collection)
            .and_then(Value::as_array)
            .is_some_and(|items| !items.is_empty())
    });
    let mut backup_path: Option<PathBuf> = None;
    if has_existing {
        if let Some(truncated) = backup.get("truncated").and_then(Value::as_object) {
            for (collection, value) in truncated {
                if value.as_bool() == Some(true) {
                    return Err(MemoryError::storage(format!(
                        "scope is too large for a complete reversible import backup: {collection}"
                    )));
                }
            }
        }
        let stamp = now_ms();
        let exports = store.root().join("exports");
        let mut candidate = exports.join(format!("pre-import-{stamp}.json"));
        let mut collision = 0;
        while candidate.exists() {
            collision += 1;
            candidate = exports.join(format!("pre-import-{stamp}-{collision}.json"));
        }
        write_secure_json(&candidate, &backup)?;
        backup_path = Some(candidate);
    }
    store.import_state(scope, &state, parsed.replace)?;
    let backup_text = backup_path
        .as_ref()
        .map(|path| path.to_string_lossy().into_owned())
        .unwrap_or_default();
    out.emit(
        json!({
            "path": parsed.positional[0], "imported": true, "format": format,
            "incoming_memories": incoming, "duplicate_memories": duplicates,
            "duplicates": duplicate_report, "imported_memories": to_import,
            "replaced": parsed.replace, "source_scopes": source_scopes,
            "target_scope": scope_summary_json(scope), "warnings": warnings,
            "backup": backup_text,
            "rollback": if backup_path.is_some() {
                format!("qt memory import {backup_text} --replace --yes")
            } else {
                "transactional; no pre-existing scope data".to_string()
            },
        }),
        "Memory imported",
    );
    Ok(0)
}
