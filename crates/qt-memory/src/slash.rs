//! `/memory` slash subcommands as library functions returning transcript
//! lines (archived `interactive_mode.cpp` memory handler). Subcommands that
//! open a view (`dashboard`, `settings`, the review inbox, the skill folder
//! picker) or start background work (`scan-skills`) return a
//! [`SlashMemoryAction`] for the front end to perform.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use serde_json::{Value, json};

use crate::model::*;
use crate::review::{ReviewAction, ReviewActionKind};
use crate::runtime::MemoryRuntime;
use crate::settings::MemorySettings;
use crate::skill_import::{
    SkillImportItem, SkillImportOptions, SkillImportReport, import_skill_memories,
};
use crate::tools::memory_tools;

/// Usage line of `/memory`.
pub const USAGE: &str = "usage: /memory [dashboard|status|settings|scan-skills|dream|recall|remember|review|feedback|forget]";

/// Front-end work requested by a subcommand.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SlashMemoryAction {
    /// `/memory` without a subcommand opens the operation menu.
    OpenMenu,
    OpenDashboard,
    OpenSettings,
    OpenReviewInbox,
    PickSkillFolder,
    /// Run [`run_skill_scan`] in the background for this canonical folder.
    ScanSkills {
        folder: PathBuf,
        recursive: bool,
    },
}

/// Lines to append to the transcript plus an optional front-end action.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct SlashMemoryResponse {
    pub lines: Vec<String>,
    pub action: Option<SlashMemoryAction>,
}

impl SlashMemoryResponse {
    fn lines(lines: Vec<String>) -> Self {
        Self {
            lines,
            action: None,
        }
    }

    fn action(action: SlashMemoryAction) -> Self {
        Self {
            lines: Vec::new(),
            action: Some(action),
        }
    }
}

/// Inputs of the `/memory` handler.
#[derive(Clone, Default)]
pub struct SlashMemoryContext {
    pub runtime: Option<Arc<MemoryRuntime>>,
    pub settings: MemorySettings,
    pub agent_home: PathBuf,
    pub cwd: PathBuf,
    pub workspace_root: PathBuf,
    pub allow_external_filesystem_access: bool,
    /// Startup diagnostics; lines mentioning memory are reported by `status`.
    pub diagnostics: Vec<String>,
}

fn split_first(text: &str) -> (&str, &str) {
    let text = text.trim();
    match text.find(|ch: char| ch.is_ascii_whitespace()) {
        Some(position) => (&text[..position], text[position..].trim()),
        None => (text, ""),
    }
}

fn counts(values: &BTreeMap<String, u64>) -> String {
    if values.is_empty() {
        return "(none)".into();
    }
    values
        .iter()
        .map(|(name, count)| format!("{name}={count}"))
        .collect::<Vec<_>>()
        .join(", ")
}

/// Statistics block of `/memory status`.
pub fn statistics_lines(stats: &MemoryStatistics) -> Vec<String> {
    vec![
        "  statistics:".into(),
        format!("    stored memories: {}", stats.memory_entries),
        format!("    by status: {}", counts(&stats.trust_states)),
        format!("    by type: {}", counts(&stats.memory_counts)),
        format!("    hypotheses: {}", stats.hypotheses),
        format!("    low confidence: {}", stats.low_confidence),
        format!("    unprovenanced: {}", stats.unprovenanced),
        format!("    deprecated: {}", stats.deprecated),
        format!("    accessed memories: {}", stats.accessed_memories),
        format!("    total accesses: {}", stats.total_accesses),
        format!("    successful outcomes: {}", stats.successful_outcomes),
        format!("    failed outcomes: {}", stats.failed_outcomes),
        format!("    failed procedures: {}", stats.failed_procedures),
        format!("    high-use hypotheses: {}", stats.high_use_hypotheses),
        format!("    conflicts: {}", stats.conflict_count),
        format!("    unresolved conflicts: {}", stats.unresolved_conflicts),
        format!("    pending reviews: {}", stats.pending_reviews),
    ]
}

/// `45s`, `12m`, `3h`, `2d`.
pub fn review_duration(seconds: i64) -> String {
    let seconds = seconds.max(0);
    if seconds < 60 {
        format!("{seconds}s")
    } else if seconds < 3600 {
        format!("{}m", seconds / 60)
    } else if seconds < 86400 {
        format!("{}h", seconds / 3600)
    } else {
        format!("{}d", seconds / 86400)
    }
}

fn yes(value: bool) -> &'static str {
    if value { "true" } else { "false" }
}

/// `/memory status`.
pub fn status_lines(context: &SlashMemoryContext) -> Vec<String> {
    let settings = &context.settings;
    let dreaming = &settings.dreaming;
    let memory = context.runtime.as_ref();
    let mut lines = vec![
        "Built-in memory:".to_string(),
        format!("  enabled: {}", yes(memory.is_some())),
        format!("  configured: {}", yes(settings.enabled)),
        format!("  automatic recall: {}", yes(settings.automatic_recall)),
        format!("  automatic capture: {}", yes(settings.automatic_capture)),
        format!("  dreaming configured: {}", yes(dreaming.enabled)),
        format!("  dreaming trigger: {}", dreaming.trigger),
        format!(
            "  dreaming schedule: interval={} s, idle={} s",
            dreaming.interval_seconds, dreaming.idle_seconds
        ),
        format!(
            "  dreaming model: {}",
            dreaming.model.as_deref().unwrap_or("auto")
        ),
        format!(
            "  dreaming verifier model: {}",
            dreaming.verifier_model.as_deref().unwrap_or("unset")
        ),
        format!(
            "  dreaming bounds: {} candidates / {} tokens / {} ms",
            dreaming.max_candidates, dreaming.max_model_input_tokens, dreaming.timeout_ms
        ),
        format!(
            "  dreaming policies: research={}, promotion={}",
            dreaming.research_policy, dreaming.promotion_policy
        ),
        format!(
            "  pre-handler fail-open: {}",
            yes(settings.prehandler.fail_open)
        ),
        format!(
            "  database: {}/memory/memory.sqlite3",
            context.agent_home.display()
        ),
        format!(
            "  pre-handler: {}",
            if memory.is_some() && settings.automatic_recall {
                "isolated child with deterministic fallback"
            } else {
                "inactive"
            }
        ),
    ];
    match memory {
        Some(memory) => {
            lines.push(format!("  schema: {}", memory.schema_version()));
            let scope = memory.scope();
            lines.push(format!(
                "  scope: {}{}",
                scope.repository_display,
                if scope.branch.is_empty() {
                    String::new()
                } else {
                    format!(" @ {}", scope.branch)
                }
            ));
            match memory.statistics() {
                Ok(statistics) => lines.extend(statistics_lines(&statistics)),
                Err(error) => lines.push(format!("  statistics: unavailable ({error})")),
            }
            if let Ok(reviews) = memory.reviews().review_statistics() {
                lines.push(format!(
                    "  review queue: pending={}, urgent/high={}, due deferred={}, oldest={}",
                    reviews.pending,
                    reviews.urgent,
                    reviews.due_deferred,
                    if reviews.pending == 0 {
                        "none".to_string()
                    } else {
                        review_duration(reviews.oldest_pending_age_seconds)
                    }
                ));
                if reviews.pending != 0 || reviews.due_deferred != 0 {
                    lines.push(
                        "  review attention: open /memory review to triage the scope-aware Inbox"
                            .into(),
                    );
                }
            }
            let dreaming = memory.dreaming_status();
            lines.push(format!(
                "  dreaming runtime: {}, pending={}, completed={}, failed={}",
                if dreaming.running { "running" } else { "idle" },
                dreaming.pending_runs,
                dreaming.completed_runs,
                dreaming.failed_runs
            ));
            if dreaming.trigger == "interval" || dreaming.trigger == "post_turn" {
                lines.push(format!(
                    "  dreaming idle: {}/{}s{}",
                    dreaming.idle_for_seconds,
                    dreaming.idle_seconds,
                    if dreaming.waiting_for_idle {
                        " (waiting)"
                    } else {
                        ""
                    }
                ));
            }
            if !dreaming.last_dream_id.is_empty() {
                lines.push(format!(
                    "  last dream: {} (review {})",
                    dreaming.last_dream_id, dreaming.last_review_id
                ));
            }
            if !dreaming.last_error.is_empty() {
                lines.push(format!("  dreaming last error: {}", dreaming.last_error));
            }
        }
        None => lines.push("  statistics: unavailable (memory runtime inactive)".into()),
    }
    let warnings: Vec<&String> = context
        .diagnostics
        .iter()
        .filter(|diagnostic| diagnostic.to_ascii_lowercase().contains("memory"))
        .collect();
    if warnings.is_empty() {
        lines.push("  warnings: none".into());
    } else {
        lines.push("  warnings:".into());
        lines.extend(warnings.iter().map(|warning| format!("    - {warning}")));
    }
    lines
}

/// `/memory dream [status|run|research <json>]`.
pub fn dream_lines(memory: &MemoryRuntime, arguments: &str) -> Vec<String> {
    let (action, rest) = split_first(arguments);
    match action.to_ascii_lowercase().as_str() {
        "" | "status" => {
            let status = memory.dreaming_status();
            let or_none = |value: &str| {
                if value.is_empty() {
                    "none".to_string()
                } else {
                    value.to_string()
                }
            };
            vec![
                "Memory Dreaming:".into(),
                format!("  enabled: {}", yes(status.enabled)),
                format!("  trigger: {}", status.trigger),
                format!("  idle duration: {}s", status.idle_seconds),
                format!("  idle for: {}s", status.idle_for_seconds),
                format!("  waiting for idle: {}", yes(status.waiting_for_idle)),
                format!(
                    "  state: {}",
                    if status.running { "running" } else { "idle" }
                ),
                format!("  pending: {}", status.pending_runs),
                format!("  completed: {}", status.completed_runs),
                format!("  failed: {}", status.failed_runs),
                format!("  last dream: {}", or_none(&status.last_dream_id)),
                format!("  last review: {}", or_none(&status.last_review_id)),
                format!("  last error: {}", or_none(&status.last_error)),
            ]
        }
        "run" => vec![if memory.enqueue_dream_cycle("manual") {
            "Memory Dreaming cycle queued in the background.".into()
        } else {
            "Memory Dreaming cycle was not queued; enable Dreaming and verify its queue/model settings.".into()
        }],
        "research" => {
            let stored = (|| -> Result<_, String> {
                let input: Value = serde_json::from_str(rest).map_err(|error| error.to_string())?;
                let (Some(dream_id), Some(_)) = (
                    input.get("dream_id").and_then(Value::as_str),
                    input.get("summary").and_then(Value::as_str),
                ) else {
                    return Err("research JSON requires string dream_id and summary".into());
                };
                let control = memory.dreaming().ok_or_else(|| {
                    "Dreaming research successors are disabled by policy".to_string()
                })?;
                control
                    .add_research(dream_id, &input, "user:interactive")
                    .map_err(|error| error.to_string())
            })();
            match stored {
                Ok(stored) => vec![
                    format!(
                        "Dream research successor stored for review: {}",
                        stored.research_id.as_deref().unwrap_or("none")
                    ),
                    format!(
                        "Research review: {}",
                        stored.research_review_id.as_deref().unwrap_or("none")
                    ),
                ],
                Err(error) => vec![
                    format!("Unable to store dream research: {error}"),
                    "usage: /memory dream research {\"dream_id\":\"...\",\"summary\":\"...\",\"source_uri\":\"...\"}"
                        .into(),
                ],
            }
        }
        _ => vec!["usage: /memory dream [status|run|research <json>]".into()],
    }
}

/// `<label>` followed by the pretty JSON output, or `<label> failed: ...`.
async fn tool_lines(
    memory: &Arc<MemoryRuntime>,
    tool: &str,
    arguments: Value,
    label: &str,
) -> Vec<String> {
    // Slash commands are an explicit user surface: they reuse the tool
    // implementations even when model-facing tools are filtered.
    let Some(tool) = memory_tools(memory, |name| name == tool).into_iter().next() else {
        return vec![format!("{label} failed: tool unavailable")];
    };
    let output = tool
        .execute(arguments, qt_core::tools::ToolContext::new("."))
        .await;
    if output.is_error {
        let status = output
            .details
            .as_ref()
            .and_then(|details| details.get("status"))
            .and_then(Value::as_str)
            .unwrap_or("execution_error")
            .to_string();
        return vec![format!(
            "{label} failed: {status} - {}",
            output.text_content()
        )];
    }
    let mut lines = vec![label.to_string()];
    lines.extend(output.text_content().lines().map(str::to_string));
    lines
}

/// Validates a skill-scan folder against the workspace policy and returns
/// its canonical path.
pub fn resolve_skill_folder(
    context: &SlashMemoryContext,
    path_text: &str,
) -> Result<PathBuf, String> {
    let requested = PathBuf::from(path_text);
    let requested = if requested.is_relative() {
        context.cwd.join(requested)
    } else {
        requested
    };
    let folder = std::fs::canonicalize(&requested)
        .ok()
        .filter(|folder| folder.is_dir())
        .ok_or_else(|| format!("memory skill scan folder is not readable: {path_text}"))?;
    if !context.allow_external_filesystem_access {
        let workspace = std::fs::canonicalize(&context.workspace_root)
            .unwrap_or_else(|_| context.workspace_root.clone());
        if !folder.starts_with(&workspace) {
            return Err(
                "memory skill scan folder is outside the workspace; enable external filesystem access in /settings to allow it"
                    .into(),
            );
        }
    }
    Ok(folder)
}

/// Imports skills from `folder` into the repository scope, reporting each
/// item and the summary as transcript lines.
pub fn run_skill_scan(
    memory: &MemoryRuntime,
    folder: &Path,
    recursive: bool,
    containment_root: Option<PathBuf>,
    progress: impl Fn(String) + Send + Sync + 'static,
) -> Vec<String> {
    let mut scope = memory.scope();
    scope.kind = ScopeKind::Repository;
    scope.branch.clear();
    scope.task.clear();
    scope.agent.clear();
    scope.session.clear();
    scope.conversation.clear();
    let display_root = folder.to_path_buf();
    let mut options = SkillImportOptions::new(folder);
    options.containment_root = containment_root;
    options.recursive = recursive;
    options.progress = Some(Box::new(move |item: &SkillImportItem| {
        let display = item
            .skill_path
            .strip_prefix(&display_root)
            .unwrap_or(&item.skill_path)
            .to_string_lossy()
            .replace('\\', "/");
        let mut line = format!("Memory skill scan: {} {display}", item.action);
        if item.related_files != 0 {
            line += &format!(" ({} related files)", item.related_files);
        }
        if !item.diagnostic.is_empty() {
            line += &format!(" — {}", item.diagnostic);
        }
        progress(line);
    }));
    match import_skill_memories(memory.memories(), memory.governance(), &scope, &options) {
        Ok(report) => vec![skill_scan_summary(&report)],
        Err(error) => vec![format!("Memory skill scan failed: {error}")],
    }
}

/// `Memory skill scan finished: discovered=…, created=…, …`.
pub fn skill_scan_summary(report: &SkillImportReport) -> String {
    format!(
        "Memory skill scan finished: discovered={}, created={}, updated={}, unchanged={}, failed={}",
        report.discovered, report.created, report.updated, report.unchanged, report.failed
    )
}

fn review_decide(memory: &MemoryRuntime, arguments: &str) -> Vec<String> {
    let (id, rest) = split_first(arguments);
    let (decision, confirm) = split_first(rest);
    let usage = "usage: /memory review decide <review-id> <accepted|rejected|deferred> confirm";
    if id.is_empty() || decision.is_empty() || !confirm.eq_ignore_ascii_case("confirm") {
        return vec![usage.into()];
    }
    let kind = match decision.to_ascii_lowercase().as_str() {
        "accepted" => ReviewActionKind::Approve,
        "rejected" => ReviewActionKind::Reject,
        "deferred" => ReviewActionKind::Defer,
        _ => return vec!["decision must be accepted, rejected, or deferred".into()],
    };
    let mut action = ReviewAction::new(kind, "user:interactive");
    if kind == ReviewActionKind::Defer {
        action.deferred_until = Some(now_ms() + 24 * 3_600_000);
    }
    action.human_authority = true;
    let inbox = memory.reviews();
    let outcome = inbox
        .preview_review(id, action.clone())
        .and_then(|preview| {
            if !preview.applicable {
                return Ok(Err(format!(
                    "Memory review action is not applicable: {}",
                    preview
                        .warnings
                        .first()
                        .cloned()
                        .unwrap_or_else(|| "no safe mutation".into())
                )));
            }
            action.expected_revision = preview.review_revision;
            action.idempotency_key = preview.idempotency_token;
            inbox.apply_review_action(id, action).map(Ok)
        });
    match outcome {
        Ok(Ok(result)) => vec![format!(
            "Memory review {id}: {}; decision={}; domain={}; revision={}",
            result.status.as_str(),
            if result.decision_persisted {
                "persisted"
            } else {
                "not persisted"
            },
            if result.domain_mutations_applied {
                "applied"
            } else {
                "unchanged"
            },
            result.revision
        )],
        Ok(Err(message)) => vec![message],
        Err(error) => vec![format!("Memory review was not changed: {error}")],
    }
}

/// Handles `/memory <arguments>`.
pub async fn memory_slash_command(
    context: &SlashMemoryContext,
    arguments: &str,
) -> SlashMemoryResponse {
    let (subcommand, rest) = split_first(arguments);
    let subcommand = subcommand.to_ascii_lowercase();
    match subcommand.as_str() {
        "" => return SlashMemoryResponse::action(SlashMemoryAction::OpenMenu),
        "settings" => return SlashMemoryResponse::action(SlashMemoryAction::OpenSettings),
        "dashboard" => return SlashMemoryResponse::action(SlashMemoryAction::OpenDashboard),
        "scan-skills" if rest.is_empty() => {
            return SlashMemoryResponse::action(SlashMemoryAction::PickSkillFolder);
        }
        "status" => return SlashMemoryResponse::lines(status_lines(context)),
        _ => {}
    }
    let Some(memory) = context.runtime.as_ref() else {
        return SlashMemoryResponse::lines(vec![
            "Built-in memory is disabled for this runtime.".into(),
            "Enable memory.enabled in global settings and start a new runtime.".into(),
        ]);
    };
    match subcommand.as_str() {
        "scan-skills" => {
            let (mode, path) = split_first(rest);
            let recursive = mode.eq_ignore_ascii_case("recursive");
            let path_text = if recursive { path } else { rest };
            if path_text.is_empty() {
                return SlashMemoryResponse::lines(vec![
                    "usage: /memory scan-skills [recursive] <folder>".into(),
                ]);
            }
            match resolve_skill_folder(context, path_text) {
                Err(message) => SlashMemoryResponse::lines(vec![message]),
                Ok(folder) => SlashMemoryResponse {
                    lines: vec![format!(
                        "Memory skill scan started: {} (recursive: {})",
                        folder.display(),
                        if recursive { "yes" } else { "no" }
                    )],
                    action: Some(SlashMemoryAction::ScanSkills { folder, recursive }),
                },
            }
        }
        "dream" => SlashMemoryResponse::lines(dream_lines(memory, rest)),
        "recall" => {
            if rest.is_empty() {
                return SlashMemoryResponse::lines(vec!["usage: /memory recall <query>".into()]);
            }
            SlashMemoryResponse::lines(
                tool_lines(
                    memory,
                    "memory_recall",
                    json!({"query": rest}),
                    "memory recall",
                )
                .await,
            )
        }
        "remember" => {
            if rest.is_empty() {
                return SlashMemoryResponse::lines(vec![
                    "usage: /memory remember <content>".into(),
                ]);
            }
            SlashMemoryResponse::lines(
                tool_lines(
                    memory,
                    "memory_remember",
                    json!({"content": rest, "status": "hypothesis", "source": "user:interactive"}),
                    "memory remember",
                )
                .await,
            )
        }
        "review" => {
            let (operation, arguments) = split_first(rest);
            match operation.to_ascii_lowercase().as_str() {
                "" | "list" => SlashMemoryResponse::action(SlashMemoryAction::OpenReviewInbox),
                "decide" => SlashMemoryResponse::lines(review_decide(memory, arguments)),
                _ => SlashMemoryResponse::lines(vec![
                    "usage: /memory review".into(),
                    "usage: /memory review decide <review-id> <accepted|rejected|deferred> confirm"
                        .into(),
                ]),
            }
        }
        "feedback" => {
            let (id, rest) = split_first(rest);
            let (outcome, confirm) = split_first(rest);
            if id.is_empty() || outcome.is_empty() || !confirm.eq_ignore_ascii_case("confirm") {
                return SlashMemoryResponse::lines(vec![
                    "usage: /memory feedback <memory-id> <success|failure|partial|unused> confirm"
                        .into(),
                ]);
            }
            SlashMemoryResponse::lines(
                tool_lines(
                    memory,
                    "memory_feedback",
                    json!({"id": id, "outcome": outcome.to_ascii_lowercase()}),
                    "memory feedback",
                )
                .await,
            )
        }
        "forget" => {
            let (id, confirm) = split_first(rest);
            if id.is_empty() || !confirm.eq_ignore_ascii_case("confirm") {
                return SlashMemoryResponse::lines(vec![
                    "usage: /memory forget <memory-id> confirm".into(),
                    "Deletion also requires the normal mutation approval policy.".into(),
                ]);
            }
            SlashMemoryResponse::lines(
                tool_lines(
                    memory,
                    "memory_forget",
                    json!({"id": id, "confirm": true}),
                    "memory forget",
                )
                .await,
            )
        }
        _ => SlashMemoryResponse::lines(vec![
            format!("unknown memory command: {subcommand}"),
            USAGE.into(),
        ]),
    }
}
