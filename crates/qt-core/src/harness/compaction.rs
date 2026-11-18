//! Model-backed context compaction and branch summarization (Pi
//! `core/compaction/*`).
//!
//! Compaction keeps roughly `keepRecentTokens` of the newest messages and
//! replaces everything older with a structured summary. When the cut lands
//! inside a turn, the turn's prefix gets its own summary. Summaries are
//! updated iteratively: a new compaction feeds the previous summary to the
//! model with the update prompt. Files read and modified by tools are tracked
//! across compactions in the entry's `details`.

use std::collections::BTreeSet;

use serde_json::{Value, json};
use tokio_util::sync::CancellationToken;

use super::settings::CompactionSettings;
use crate::message::{
    AgentMessage, ContentBlock, Message, StopReason, Usage, UserContent, convert_to_llm, now_millis,
};
use crate::provider::{self, ProviderConfig, Request, ThinkingLevel};
use crate::session::{EntryKind, Session, SessionEntry, context_from_path};

/// Pi `SUMMARIZATION_SYSTEM_PROMPT`.
pub const SUMMARIZATION_SYSTEM_PROMPT: &str = "You are a context summarization assistant. Your task is to read a conversation between a user and an AI coding assistant, then produce a structured summary following the exact format specified.

Do NOT continue the conversation. Do NOT respond to any questions in the conversation. ONLY output the structured summary.";

/// Pi `SUMMARIZATION_PROMPT`.
pub const SUMMARIZATION_PROMPT: &str = "The messages above are a conversation to summarize. Create a structured context checkpoint summary that another LLM will use to continue the work.

Use this EXACT format:

## Goal
[What is the user trying to accomplish? Can be multiple items if the session covers different tasks.]

## Constraints & Preferences
- [Any constraints, preferences, or requirements mentioned by user]
- [Or \"(none)\" if none were mentioned]

## Progress
### Done
- [x] [Completed tasks/changes]

### In Progress
- [ ] [Current work]

### Blocked
- [Issues preventing progress, if any]

## Key Decisions
- **[Decision]**: [Brief rationale]

## Next Steps
1. [Ordered list of what should happen next]

## Critical Context
- [Any data, examples, or references needed to continue]
- [Or \"(none)\" if not applicable]

Keep each section concise. Preserve exact file paths, function names, and error messages.";

/// Pi `UPDATE_SUMMARIZATION_PROMPT`.
pub const UPDATE_SUMMARIZATION_PROMPT: &str = "The messages above are NEW conversation messages to incorporate into the existing summary provided in <previous-summary> tags.

Update the existing structured summary with new information. RULES:
- PRESERVE all existing information from the previous summary
- ADD new progress, decisions, and context from the new messages
- UPDATE the Progress section: move items from \"In Progress\" to \"Done\" when completed
- UPDATE \"Next Steps\" based on what was accomplished
- PRESERVE exact file paths, function names, and error messages
- If something is no longer relevant, you may remove it

Use this EXACT format:

## Goal
[Preserve existing goals, add new ones if the task expanded]

## Constraints & Preferences
- [Preserve existing, add new ones discovered]

## Progress
### Done
- [x] [Include previously done items AND newly completed items]

### In Progress
- [ ] [Current work - update based on progress]

### Blocked
- [Current blockers - remove if resolved]

## Key Decisions
- **[Decision]**: [Brief rationale] (preserve all previous, add new)

## Next Steps
1. [Update based on current state]

## Critical Context
- [Preserve important context, add new if needed]

Keep each section concise. Preserve exact file paths, function names, and error messages.";

/// Pi `TURN_PREFIX_SUMMARIZATION_PROMPT`.
pub const TURN_PREFIX_SUMMARIZATION_PROMPT: &str =
    "This is the PREFIX of a turn that was too large to keep. The SUFFIX (recent work) is retained.

Summarize the prefix to provide context for the retained suffix:

## Original Request
[What did the user ask for in this turn?]

## Early Progress
- [Key decisions and work done in the prefix]

## Context for Suffix
- [Information needed to understand the retained recent work]

Be concise. Focus on what's needed to understand the kept suffix.";

/// Pi `BRANCH_SUMMARY_PREAMBLE`.
pub const BRANCH_SUMMARY_PREAMBLE: &str = "The user explored a different conversation branch before returning here.\nSummary of that exploration:\n\n";

/// Pi `BRANCH_SUMMARY_PROMPT`.
pub const BRANCH_SUMMARY_PROMPT: &str =
    "Create a structured summary of this conversation branch for context when returning later.

Use this EXACT format:

## Goal
[What was the user trying to accomplish in this branch?]

## Constraints & Preferences
- [Any constraints, preferences, or requirements mentioned]
- [Or \"(none)\" if none were mentioned]

## Progress
### Done
- [x] [Completed tasks/changes]

### In Progress
- [ ] [Work that was started but not finished]

### Blocked
- [Issues preventing progress, if any]

## Key Decisions
- **[Decision]**: [Brief rationale]

## Next Steps
1. [What should happen next to continue this work]

Keep each section concise. Preserve exact file paths, function names, and error messages.";

const TOOL_RESULT_MAX_CHARS: usize = 2_000;

// ----- token estimation ------------------------------------------------------

fn chars(s: &str) -> u64 {
    s.chars().count() as u64
}

fn div_ceil4(n: u64) -> u64 {
    n.div_ceil(4)
}

fn content_chars(content: &UserContent, images: bool) -> u64 {
    match content {
        UserContent::Text(t) => chars(t),
        UserContent::Blocks(blocks) => blocks_chars(blocks, images),
    }
}

fn blocks_chars(blocks: &[ContentBlock], images: bool) -> u64 {
    blocks
        .iter()
        .map(|b| match b {
            ContentBlock::Text { text } => chars(text),
            ContentBlock::Image { .. } if images => 4_800,
            _ => 0,
        })
        .sum()
}

/// Conservative chars/4 token estimate for one message (Pi `estimateTokens`).
pub fn estimate_tokens(message: &AgentMessage) -> u64 {
    let n = match message {
        AgentMessage::Llm(Message::User { content, .. }) => content_chars(content, false),
        AgentMessage::Llm(Message::Assistant(a)) => a
            .content
            .iter()
            .map(|b| match b {
                ContentBlock::Text { text } => chars(text),
                ContentBlock::Thinking { thinking, .. } => chars(thinking),
                ContentBlock::ToolCall {
                    name, arguments, ..
                } => chars(name) + chars(&arguments.to_string()),
                ContentBlock::Image { .. } => 0,
            })
            .sum(),
        AgentMessage::Llm(Message::ToolResult(r)) => blocks_chars(&r.content, true),
        AgentMessage::Custom(c) => content_chars(&c.content, true),
        AgentMessage::BashExecution(b) => chars(&b.command) + chars(&b.output),
        AgentMessage::BranchSummary(b) => chars(&b.summary),
        AgentMessage::CompactionSummary(c) => chars(&c.summary),
    };
    div_ceil4(n)
}

/// Total context tokens reported by a response (Pi `calculateContextTokens`).
pub fn calculate_context_tokens(usage: &Usage) -> u64 {
    usage.context_tokens()
}

fn usable_usage(message: &AgentMessage) -> Option<&Usage> {
    message
        .as_assistant()
        .filter(|a| !matches!(a.stop_reason, StopReason::Aborted | StopReason::Error))
        .map(|a| &a.usage)
}

/// Context size from the last usable response plus estimates for later messages.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ContextUsageEstimate {
    pub tokens: u64,
    pub usage_tokens: u64,
    pub trailing_tokens: u64,
    pub last_usage_index: Option<usize>,
}

/// Pi `estimateContextTokens`.
pub fn estimate_context_tokens(messages: &[AgentMessage]) -> ContextUsageEstimate {
    let last = messages
        .iter()
        .enumerate()
        .rev()
        .find_map(|(i, m)| usable_usage(m).map(|u| (i, *u)));
    match last {
        None => {
            let estimated = messages.iter().map(estimate_tokens).sum();
            ContextUsageEstimate {
                tokens: estimated,
                usage_tokens: 0,
                trailing_tokens: estimated,
                last_usage_index: None,
            }
        }
        Some((index, usage)) => {
            let usage_tokens = calculate_context_tokens(&usage);
            let trailing: u64 = messages[index + 1..].iter().map(estimate_tokens).sum();
            ContextUsageEstimate {
                tokens: usage_tokens + trailing,
                usage_tokens,
                trailing_tokens: trailing,
                last_usage_index: Some(index),
            }
        }
    }
}

/// Whether the context is within `reserveTokens` of the window (Pi `shouldCompact`).
pub fn should_compact(
    context_tokens: u64,
    context_window: u64,
    settings: &CompactionSettings,
) -> bool {
    settings.enabled
        && context_tokens as i128 > context_window as i128 - settings.reserve_tokens as i128
}

// ----- cut point -------------------------------------------------------------

fn is_cut_point(entry: &SessionEntry) -> bool {
    match &entry.kind {
        EntryKind::Message(m) => !matches!(m, AgentMessage::Llm(Message::ToolResult(_))),
        EntryKind::BranchSummary(_) | EntryKind::CustomMessage(_) => true,
        _ => false,
    }
}

fn starts_turn(entry: &SessionEntry) -> bool {
    match &entry.kind {
        EntryKind::BranchSummary(_) | EntryKind::CustomMessage(_) => true,
        EntryKind::Message(m) => m.is_user() || matches!(m, AgentMessage::BashExecution(_)),
        _ => false,
    }
}

/// Index of the entry that starts the turn containing `index`.
pub fn find_turn_start_index(
    entries: &[SessionEntry],
    index: usize,
    start: usize,
) -> Option<usize> {
    (start..=index).rev().find(|&i| starts_turn(&entries[i]))
}

/// Where to cut (Pi `CutPointResult`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CutPoint {
    pub first_kept_entry_index: usize,
    /// The user message starting the split turn, when splitting.
    pub turn_start_index: Option<usize>,
    pub is_split_turn: bool,
}

/// Keep about `keep_recent_tokens` of the newest messages in `[start, end)`,
/// cutting only at user/assistant/custom messages (never tool results).
pub fn find_cut_point(
    entries: &[SessionEntry],
    start: usize,
    end: usize,
    keep_recent_tokens: u64,
) -> CutPoint {
    let cut_points: Vec<usize> = (start..end)
        .filter(|&i| is_cut_point(&entries[i]))
        .collect();
    let Some(&first) = cut_points.first() else {
        return CutPoint {
            first_kept_entry_index: start,
            turn_start_index: None,
            is_split_turn: false,
        };
    };
    let mut cut = first;
    let mut accumulated = 0;
    for i in (start..end).rev() {
        let EntryKind::Message(m) = &entries[i].kind else {
            continue;
        };
        accumulated += estimate_tokens(m);
        if accumulated >= keep_recent_tokens {
            if let Some(&c) = cut_points.iter().find(|&&c| c >= i) {
                cut = c;
            }
            break;
        }
    }
    while cut > start {
        let prev = &entries[cut - 1];
        if matches!(prev.kind, EntryKind::Compaction(_) | EntryKind::Message(_)) {
            break;
        }
        cut -= 1;
    }
    let is_user = entries[cut].is_user_message();
    let turn_start = if is_user {
        None
    } else {
        find_turn_start_index(entries, cut, start)
    };
    CutPoint {
        first_kept_entry_index: cut,
        turn_start_index: turn_start,
        is_split_turn: !is_user && turn_start.is_some(),
    }
}

// ----- file operations -------------------------------------------------------

/// Paths touched by `read`, `write` and `edit` tool calls.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct FileOperations {
    pub read: BTreeSet<String>,
    pub written: BTreeSet<String>,
    pub edited: BTreeSet<String>,
}

impl FileOperations {
    /// Record the tool calls of an assistant message.
    pub fn extract(&mut self, message: &AgentMessage) {
        let Some(a) = message.as_assistant() else {
            return;
        };
        for (_, name, args) in a.tool_calls() {
            let Some(path) = args
                .get("path")
                .and_then(Value::as_str)
                .filter(|p| !p.is_empty())
            else {
                continue;
            };
            match name {
                "read" => self.read.insert(path.to_string()),
                "write" => self.written.insert(path.to_string()),
                "edit" => self.edited.insert(path.to_string()),
                _ => false,
            };
        }
    }

    /// Merge `{readFiles, modifiedFiles}` details from an earlier summary.
    pub fn merge_details(&mut self, details: &Value) {
        let list = |key: &str| {
            details
                .get(key)
                .and_then(Value::as_array)
                .map(|a| {
                    a.iter()
                        .filter_map(Value::as_str)
                        .map(str::to_string)
                        .collect::<Vec<_>>()
                })
                .unwrap_or_default()
        };
        self.read.extend(list("readFiles"));
        self.edited.extend(list("modifiedFiles"));
    }

    /// `(readFiles, modifiedFiles)`: read-only files and modified files, sorted.
    pub fn file_lists(&self) -> (Vec<String>, Vec<String>) {
        let modified: BTreeSet<&String> = self.edited.iter().chain(&self.written).collect();
        let read = self
            .read
            .iter()
            .filter(|f| !modified.contains(f))
            .cloned()
            .collect();
        (read, modified.into_iter().cloned().collect())
    }
}

/// Pi `formatFileOperations`.
pub fn format_file_operations(read: &[String], modified: &[String]) -> String {
    let mut sections = Vec::new();
    if !read.is_empty() {
        sections.push(format!("<read-files>\n{}\n</read-files>", read.join("\n")));
    }
    if !modified.is_empty() {
        sections.push(format!(
            "<modified-files>\n{}\n</modified-files>",
            modified.join("\n")
        ));
    }
    if sections.is_empty() {
        String::new()
    } else {
        format!("\n\n{}", sections.join("\n\n"))
    }
}

fn file_details(read: &[String], modified: &[String]) -> Value {
    json!({"readFiles": read, "modifiedFiles": modified})
}

// ----- serialization ---------------------------------------------------------

fn truncate_for_summary(text: &str, max_chars: usize) -> String {
    let total = text.chars().count();
    if total <= max_chars {
        return text.to_string();
    }
    let kept: String = text.chars().take(max_chars).collect();
    format!(
        "{kept}\n\n[... {} more characters truncated]",
        total - max_chars
    )
}

fn json_arg(v: &Value) -> String {
    serde_json::to_string(v).unwrap_or_default()
}

/// Flatten provider messages into labelled text so the summarizer does not
/// try to continue the conversation (Pi `serializeConversation`).
pub fn serialize_conversation(messages: &[Message]) -> String {
    let mut parts = Vec::new();
    for message in messages {
        match message {
            Message::User { content, .. } => {
                let text = match content {
                    UserContent::Text(t) => t.clone(),
                    UserContent::Blocks(b) => b
                        .iter()
                        .filter_map(|b| match b {
                            ContentBlock::Text { text } => Some(text.as_str()),
                            _ => None,
                        })
                        .collect(),
                };
                if !text.is_empty() {
                    parts.push(format!("[User]: {text}"));
                }
            }
            Message::Assistant(a) => {
                let mut text = Vec::new();
                let mut thinking = Vec::new();
                let mut calls = Vec::new();
                for block in &a.content {
                    match block {
                        ContentBlock::Text { text: t } => text.push(t.as_str()),
                        ContentBlock::Thinking { thinking: t, .. } => thinking.push(t.as_str()),
                        ContentBlock::ToolCall {
                            name, arguments, ..
                        } => {
                            let args = match arguments {
                                Value::Object(map) => map
                                    .iter()
                                    .map(|(k, v)| format!("{k}={}", json_arg(v)))
                                    .collect::<Vec<_>>()
                                    .join(", "),
                                other => json_arg(other),
                            };
                            calls.push(format!("{name}({args})"));
                        }
                        ContentBlock::Image { .. } => {}
                    }
                }
                if !thinking.is_empty() {
                    parts.push(format!("[Assistant thinking]: {}", thinking.join("\n")));
                }
                if !text.is_empty() {
                    parts.push(format!("[Assistant]: {}", text.join("\n")));
                }
                if !calls.is_empty() {
                    parts.push(format!("[Assistant tool calls]: {}", calls.join("; ")));
                }
            }
            Message::ToolResult(r) => {
                let text: String = r
                    .content
                    .iter()
                    .filter_map(|b| match b {
                        ContentBlock::Text { text } => Some(text.as_str()),
                        _ => None,
                    })
                    .collect();
                if !text.is_empty() {
                    parts.push(format!(
                        "[Tool result]: {}",
                        truncate_for_summary(&text, TOOL_RESULT_MAX_CHARS)
                    ));
                }
            }
        }
    }
    parts.join("\n\n")
}

// ----- summarizer ------------------------------------------------------------

/// Why a summarization request produced no summary.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SummaryError {
    Aborted,
    Failed(String),
}

impl std::fmt::Display for SummaryError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Aborted => f.write_str("Compaction cancelled"),
            Self::Failed(m) => f.write_str(m),
        }
    }
}

/// The model used for summaries (the conversation's current model).
#[derive(Clone)]
pub struct Summarizer {
    pub http: reqwest::Client,
    pub provider: ProviderConfig,
    pub model: String,
    /// The model's output limit; summaries never ask for more.
    pub max_output_tokens: u32,
    pub context_window: Option<u64>,
}

impl Summarizer {
    /// One summarization call. Thinking stays off so the token budget goes
    /// to the summary itself.
    pub async fn complete(
        &self,
        prompt_text: String,
        max_tokens: u64,
        cancel: &CancellationToken,
    ) -> Result<String, SummaryError> {
        let request = Request {
            model: self.model.clone(),
            system: SUMMARIZATION_SYSTEM_PROMPT.to_string(),
            messages: vec![Message::User {
                content: UserContent::Blocks(vec![ContentBlock::text(prompt_text)]),
                timestamp: Some(now_millis()),
            }],
            tools: Vec::new(),
            max_tokens: max_tokens
                .min(self.max_output_tokens as u64)
                .max(1)
                .min(u32::MAX as u64) as u32,
            thinking: ThinkingLevel::Off,
            ..Default::default()
        };
        let mut sink = |_e: provider::StreamEvent| {};
        let response =
            provider::stream(&self.http, &self.provider, &request, cancel, &mut sink).await;
        match response.stop_reason {
            StopReason::Aborted => Err(SummaryError::Aborted),
            StopReason::Error => Err(SummaryError::Failed(
                response
                    .error_message
                    .unwrap_or_else(|| "Unknown error".into()),
            )),
            _ => Ok(response
                .content
                .iter()
                .filter_map(|b| match b {
                    ContentBlock::Text { text } => Some(text.as_str()),
                    _ => None,
                })
                .collect::<Vec<_>>()
                .join("\n")),
        }
    }
}

// ----- compaction ------------------------------------------------------------

/// Everything decided before the summary is generated (Pi `CompactionPreparation`).
#[derive(Debug, Clone, PartialEq)]
pub struct CompactionPreparation {
    pub first_kept_entry_id: String,
    pub messages_to_summarize: Vec<AgentMessage>,
    pub turn_prefix_messages: Vec<AgentMessage>,
    pub is_split_turn: bool,
    pub tokens_before: u64,
    pub previous_summary: Option<String>,
    pub file_ops: FileOperations,
    pub settings: CompactionSettings,
}

/// A finished compaction (Pi `CompactionResult`).
#[derive(Debug, Clone, PartialEq)]
pub struct CompactionResult {
    pub summary: String,
    pub first_kept_entry_id: String,
    pub tokens_before: u64,
    pub details: Option<Value>,
}

fn message_for_compaction(entry: &SessionEntry) -> Option<AgentMessage> {
    match &entry.kind {
        EntryKind::Compaction(_) => None,
        _ => entry.context_message(),
    }
}

/// Plan a compaction of the root → leaf `path` (Pi `prepareCompaction`).
/// `None` when the path already ends in a compaction or has no id to keep.
pub fn prepare_compaction(
    path: &[SessionEntry],
    settings: &CompactionSettings,
) -> Option<CompactionPreparation> {
    if path
        .last()
        .is_some_and(|e| matches!(e.kind, EntryKind::Compaction(_)))
    {
        return None;
    }
    let prev = path
        .iter()
        .rposition(|e| matches!(e.kind, EntryKind::Compaction(_)));
    let mut previous_summary = None;
    let mut start = 0;
    if let Some(pi) = prev
        && let EntryKind::Compaction(c) = &path[pi].kind
    {
        previous_summary = Some(c.summary.clone());
        start = path
            .iter()
            .position(|e| e.id == c.first_kept_entry_id)
            .unwrap_or(pi + 1);
    }
    let end = path.len();
    if start >= end {
        return None;
    }
    let tokens_before = estimate_context_tokens(&context_from_path(path).messages).tokens;
    let cut = find_cut_point(path, start, end, settings.keep_recent_tokens);
    let first_kept = path.get(cut.first_kept_entry_index)?;
    let history_end = if cut.is_split_turn {
        cut.turn_start_index.unwrap_or(cut.first_kept_entry_index)
    } else {
        cut.first_kept_entry_index
    };
    let messages_to_summarize: Vec<AgentMessage> = path[start..history_end]
        .iter()
        .filter_map(message_for_compaction)
        .collect();
    let turn_prefix_messages: Vec<AgentMessage> = match cut.turn_start_index {
        Some(ts) if cut.is_split_turn => path[ts..cut.first_kept_entry_index]
            .iter()
            .filter_map(message_for_compaction)
            .collect(),
        _ => Vec::new(),
    };
    let mut file_ops = FileOperations::default();
    if let Some(pi) = prev
        && let EntryKind::Compaction(c) = &path[pi].kind
        && c.from_hook != Some(true)
        && let Some(details) = &c.details
    {
        file_ops.merge_details(details);
    }
    for m in messages_to_summarize.iter().chain(&turn_prefix_messages) {
        file_ops.extract(m);
    }
    Some(CompactionPreparation {
        first_kept_entry_id: first_kept.id.clone(),
        messages_to_summarize,
        turn_prefix_messages,
        is_split_turn: cut.is_split_turn,
        tokens_before,
        previous_summary,
        file_ops,
        settings: *settings,
    })
}

/// The prompt text for the history summary (Pi `generateSummary`).
pub fn summary_prompt(
    messages: &[AgentMessage],
    custom_instructions: Option<&str>,
    previous_summary: Option<&str>,
) -> String {
    let mut base = if previous_summary.is_some() {
        UPDATE_SUMMARIZATION_PROMPT.to_string()
    } else {
        SUMMARIZATION_PROMPT.to_string()
    };
    if let Some(extra) = custom_instructions.filter(|s| !s.trim().is_empty()) {
        base = format!("{base}\n\nAdditional focus: {extra}");
    }
    let mut prompt = format!(
        "<conversation>\n{}\n</conversation>\n\n",
        serialize_conversation(&convert_to_llm(messages))
    );
    if let Some(prev) = previous_summary {
        prompt.push_str(&format!(
            "<previous-summary>\n{prev}\n</previous-summary>\n\n"
        ));
    }
    prompt.push_str(&base);
    prompt
}

/// The prompt text for a split turn's prefix.
pub fn turn_prefix_prompt(messages: &[AgentMessage]) -> String {
    format!(
        "<conversation>\n{}\n</conversation>\n\n{TURN_PREFIX_SUMMARIZATION_PROMPT}",
        serialize_conversation(&convert_to_llm(messages))
    )
}

/// Generate the summary for a prepared compaction (Pi `compact`).
pub async fn compact(
    prep: &CompactionPreparation,
    summarizer: &Summarizer,
    custom_instructions: Option<&str>,
    cancel: &CancellationToken,
) -> Result<CompactionResult, SummaryError> {
    let reserve = prep.settings.reserve_tokens;
    let history = async {
        if prep.is_split_turn && prep.messages_to_summarize.is_empty() {
            return Ok("No prior history.".to_string());
        }
        summarizer
            .complete(
                summary_prompt(
                    &prep.messages_to_summarize,
                    custom_instructions,
                    prep.previous_summary.as_deref(),
                ),
                reserve * 8 / 10,
                cancel,
            )
            .await
            .map_err(|e| match e {
                SummaryError::Failed(m) => {
                    SummaryError::Failed(format!("Summarization failed: {m}"))
                }
                other => other,
            })
    };
    let mut summary = if prep.is_split_turn && !prep.turn_prefix_messages.is_empty() {
        let prefix = async {
            summarizer
                .complete(
                    turn_prefix_prompt(&prep.turn_prefix_messages),
                    reserve / 2,
                    cancel,
                )
                .await
                .map_err(|e| match e {
                    SummaryError::Failed(m) => {
                        SummaryError::Failed(format!("Turn prefix summarization failed: {m}"))
                    }
                    other => other,
                })
        };
        let (history, prefix) = futures::join!(history, prefix);
        format!(
            "{}\n\n---\n\n**Turn Context (split turn):**\n\n{}",
            history?, prefix?
        )
    } else {
        history.await?
    };
    let (read, modified) = prep.file_ops.file_lists();
    summary.push_str(&format_file_operations(&read, &modified));
    Ok(CompactionResult {
        summary,
        first_kept_entry_id: prep.first_kept_entry_id.clone(),
        tokens_before: prep.tokens_before,
        details: Some(file_details(&read, &modified)),
    })
}

// ----- branch summaries ------------------------------------------------------

/// Entries to summarize when moving from `old_leaf` to `target` and their
/// common ancestor (Pi `collectEntriesForBranchSummary`).
pub fn collect_entries_for_branch_summary(
    session: &Session,
    old_leaf: Option<&str>,
    target: &str,
) -> (Vec<SessionEntry>, Option<String>) {
    let Some(old_leaf) = old_leaf else {
        return (Vec::new(), None);
    };
    let old_path: std::collections::HashSet<String> = session
        .branch_path(Some(old_leaf))
        .into_iter()
        .map(|e| e.id)
        .collect();
    let target_path = session.branch_path(Some(target));
    let common = target_path
        .iter()
        .rev()
        .find(|e| old_path.contains(&e.id))
        .map(|e| e.id.clone());
    let mut entries = Vec::new();
    let mut cursor = Some(old_leaf.to_string());
    let mut guard = 0;
    while let Some(id) = cursor {
        if Some(&id) == common.as_ref() || guard > session.entries().len() {
            break;
        }
        guard += 1;
        let Some(entry) = session.entry(&id) else {
            break;
        };
        entries.push(entry.clone());
        cursor = entry.parent_id.clone();
    }
    entries.reverse();
    (entries, common)
}

/// Messages chosen for a branch summary (Pi `BranchPreparation`).
#[derive(Debug, Clone, PartialEq)]
pub struct BranchPreparation {
    pub messages: Vec<AgentMessage>,
    pub file_ops: FileOperations,
    pub total_tokens: u64,
}

fn branch_message(entry: &SessionEntry) -> Option<AgentMessage> {
    match &entry.kind {
        EntryKind::Message(AgentMessage::Llm(Message::ToolResult(_))) => None,
        EntryKind::Compaction(_) => entry.compaction_message(),
        _ => entry.context_message(),
    }
}

/// Newest-first selection within `token_budget` (0 = unlimited) (Pi `prepareBranchEntries`).
pub fn prepare_branch_entries(entries: &[SessionEntry], token_budget: u64) -> BranchPreparation {
    let mut file_ops = FileOperations::default();
    for entry in entries {
        if let EntryKind::BranchSummary(b) = &entry.kind
            && b.from_hook != Some(true)
            && let Some(details) = &b.details
        {
            file_ops.merge_details(details);
        }
    }
    let mut messages = Vec::new();
    let mut total = 0;
    for entry in entries.iter().rev() {
        let Some(message) = branch_message(entry) else {
            continue;
        };
        file_ops.extract(&message);
        let tokens = estimate_tokens(&message);
        if token_budget > 0 && total + tokens > token_budget {
            let is_summary = matches!(
                entry.kind,
                EntryKind::Compaction(_) | EntryKind::BranchSummary(_)
            );
            if is_summary && (total as f64) < token_budget as f64 * 0.9 {
                messages.push(message);
                total += tokens;
            }
            break;
        }
        messages.push(message);
        total += tokens;
    }
    messages.reverse();
    BranchPreparation {
        messages,
        file_ops,
        total_tokens: total,
    }
}

/// A generated branch summary.
#[derive(Debug, Clone, PartialEq)]
pub struct BranchSummary {
    pub summary: String,
    pub read_files: Vec<String>,
    pub modified_files: Vec<String>,
}

impl BranchSummary {
    pub fn details(&self) -> Value {
        file_details(&self.read_files, &self.modified_files)
    }
}

/// Summarize abandoned `entries` (Pi `generateBranchSummary`).
pub async fn generate_branch_summary(
    entries: &[SessionEntry],
    summarizer: &Summarizer,
    custom_instructions: Option<&str>,
    replace_instructions: bool,
    reserve_tokens: u64,
    cancel: &CancellationToken,
) -> Result<BranchSummary, SummaryError> {
    let window = summarizer
        .context_window
        .filter(|w| *w > 0)
        .unwrap_or(128_000);
    let prep = prepare_branch_entries(entries, window.saturating_sub(reserve_tokens));
    if prep.messages.is_empty() {
        return Ok(BranchSummary {
            summary: "No content to summarize".into(),
            read_files: Vec::new(),
            modified_files: Vec::new(),
        });
    }
    let instructions = match custom_instructions.filter(|s| !s.trim().is_empty()) {
        Some(custom) if replace_instructions => custom.to_string(),
        Some(custom) => format!("{BRANCH_SUMMARY_PROMPT}\n\nAdditional focus: {custom}"),
        None => BRANCH_SUMMARY_PROMPT.to_string(),
    };
    let prompt = format!(
        "<conversation>\n{}\n</conversation>\n\n{instructions}",
        serialize_conversation(&convert_to_llm(&prep.messages))
    );
    let text = summarizer.complete(prompt, 2_048, cancel).await?;
    let (read, modified) = prep.file_ops.file_lists();
    let mut summary = format!("{BRANCH_SUMMARY_PREAMBLE}{text}");
    summary.push_str(&format_file_operations(&read, &modified));
    Ok(BranchSummary {
        summary,
        read_files: read,
        modified_files: modified,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::message::{AssistantMessage, ToolResultMessage};
    use crate::session::CompactionEntry;
    use std::path::Path;

    fn user(text: &str) -> AgentMessage {
        AgentMessage::Llm(Message::user_text(text))
    }

    fn assistant_call(id: &str, name: &str, path: &str) -> AgentMessage {
        AgentMessage::Llm(Message::Assistant(AssistantMessage {
            content: vec![ContentBlock::ToolCall {
                id: id.into(),
                name: name.into(),
                arguments: json!({"path": path}),
            }],
            stop_reason: StopReason::ToolCall,
            ..Default::default()
        }))
    }

    fn result(id: &str, text: &str) -> AgentMessage {
        AgentMessage::Llm(Message::ToolResult(ToolResultMessage {
            tool_call_id: id.into(),
            tool_name: "read".into(),
            content: vec![ContentBlock::text(text)],
            ..Default::default()
        }))
    }

    #[test]
    fn estimates_follow_chars_over_four() {
        assert_eq!(estimate_tokens(&user("12345")), 2);
        let with_image = AgentMessage::Llm(Message::ToolResult(ToolResultMessage {
            content: vec![ContentBlock::Image {
                data: String::new(),
                mime_type: "image/png".into(),
            }],
            ..Default::default()
        }));
        assert_eq!(estimate_tokens(&with_image), 1_200);
        let usage_msg = AgentMessage::Llm(Message::Assistant(AssistantMessage {
            usage: Usage {
                input_tokens: 100,
                output_tokens: 20,
                ..Default::default()
            },
            ..Default::default()
        }));
        let est = estimate_context_tokens(&[user("x"), usage_msg, user("12345678")]);
        assert_eq!(est.tokens, 122);
        assert_eq!(est.last_usage_index, Some(1));
        let s = CompactionSettings::default();
        assert!(should_compact(120_000, 128_000, &s));
        assert!(!should_compact(100_000, 128_000, &s));
    }

    #[test]
    fn serializes_conversation_like_pi() {
        let long = "x".repeat(2_005);
        let messages = convert_to_llm(&[
            user("hi"),
            AgentMessage::Llm(Message::Assistant(AssistantMessage {
                content: vec![
                    ContentBlock::Thinking {
                        thinking: "hmm".into(),
                        signature: None,
                        redacted: false,
                    },
                    ContentBlock::text("ok"),
                    ContentBlock::ToolCall {
                        id: "1".into(),
                        name: "read".into(),
                        arguments: json!({"path": "a.rs", "limit": 5}),
                    },
                ],
                ..Default::default()
            })),
            result("1", &long),
        ]);
        let text = serialize_conversation(&messages);
        assert!(text.starts_with(
            "[User]: hi\n\n[Assistant thinking]: hmm\n\n[Assistant]: ok\n\n[Assistant tool calls]: read(path=\"a.rs\", limit=5)\n\n[Tool result]: "
        ));
        assert!(text.ends_with("\n\n[... 5 more characters truncated]"));
    }

    fn path_session(messages: &[AgentMessage]) -> Session {
        let mut s = Session::in_memory(Path::new("/p"));
        for m in messages {
            s.append_message(m).unwrap();
        }
        s
    }

    #[test]
    fn cut_point_never_splits_tool_results_and_detects_split_turns() {
        let big = "y".repeat(400);
        let s = path_session(&[
            user("first"),
            assistant_call("1", "read", "a.rs"),
            result("1", &big),
            user("second"),
            assistant_call("2", "edit", "b.rs"),
            result("2", &big),
            assistant_call("3", "read", "c.rs"),
            result("3", &big),
        ]);
        let path = s.branch_path(None);
        let cut = find_cut_point(&path, 0, path.len(), 150);
        // 100 tokens per result: the budget is reached at entry 5, so the
        // cut lands on the assistant message at 6 and splits turn "second".
        assert_eq!(cut.first_kept_entry_index, 6);
        assert!(cut.is_split_turn);
        assert_eq!(cut.turn_start_index, Some(3));

        let prep = prepare_compaction(
            &path,
            &CompactionSettings {
                keep_recent_tokens: 150,
                ..Default::default()
            },
        )
        .unwrap();
        assert_eq!(prep.first_kept_entry_id, path[6].id);
        assert_eq!(prep.messages_to_summarize.len(), 3);
        assert_eq!(prep.turn_prefix_messages.len(), 3);
        let (read, modified) = prep.file_ops.file_lists();
        assert_eq!(read, ["a.rs"]);
        assert_eq!(modified, ["b.rs"]);
    }

    #[test]
    fn iterative_compaction_uses_previous_summary_and_details() {
        let mut s = path_session(&[
            user("a"),
            assistant_call("1", "read", "x.rs"),
            result("1", "r"),
        ]);
        let keep = s.entries()[2].id.clone();
        s.append_compaction(CompactionEntry {
            summary: "OLD".into(),
            first_kept_entry_id: keep,
            tokens_before: 10,
            details: Some(json!({"readFiles": ["old.rs"], "modifiedFiles": ["mod.rs"]})),
            from_hook: None,
        })
        .unwrap();
        assert!(prepare_compaction(&s.branch_path(None), &CompactionSettings::default()).is_none());
        s.append_message(&user("next")).unwrap();
        let prep =
            prepare_compaction(&s.branch_path(None), &CompactionSettings::default()).unwrap();
        assert_eq!(prep.previous_summary.as_deref(), Some("OLD"));
        let (read, modified) = prep.file_ops.file_lists();
        assert_eq!(read, ["old.rs"]);
        assert_eq!(modified, ["mod.rs"]);
        let prompt = summary_prompt(&[user("q")], Some("focus on tests"), Some("OLD"));
        assert!(prompt.starts_with("<conversation>\n[User]: q\n</conversation>\n\n<previous-summary>\nOLD\n</previous-summary>\n\nThe messages above are NEW"));
        assert!(prompt.ends_with("\n\nAdditional focus: focus on tests"));
        assert_eq!(
            format_file_operations(&["a".into()], &["b".into()]),
            "\n\n<read-files>\na\n</read-files>\n\n<modified-files>\nb\n</modified-files>"
        );
    }

    #[test]
    fn branch_collection_and_budget() {
        let mut s = path_session(&[user("root"), user("old1"), user("old2")]);
        let root = s.entries()[0].id.clone();
        let old_leaf = s.leaf_id().unwrap().to_string();
        s.branch(&root).unwrap();
        let target = s.append_message(&user("new")).unwrap();
        let (entries, common) = collect_entries_for_branch_summary(&s, Some(&old_leaf), &target);
        assert_eq!(common.as_deref(), Some(root.as_str()));
        assert_eq!(entries.len(), 2);
        let prep = prepare_branch_entries(&entries, 0);
        assert_eq!(prep.messages.len(), 2);
        // One token each: the budget keeps only the newest message.
        let prep = prepare_branch_entries(&entries, 1);
        assert_eq!(prep.messages.len(), 1);
        assert_eq!(prep.messages[0].user_text().as_deref(), Some("old2"));
        assert!(
            collect_entries_for_branch_summary(&s, None, &target)
                .0
                .is_empty()
        );
    }
}
