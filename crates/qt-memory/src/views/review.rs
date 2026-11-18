//! Review Inbox view model (archived `ReviewInboxController` in
//! `interactive_mode.cpp`): queue cards, detail and evidence lines, the
//! per-kind action catalogue, the edit-and-approve field rules, the inputs
//! each action collects before its preview, confirmation and receipt text,
//! filters, history and batches.

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};

use crate::error::Result;
use crate::model::*;
use crate::review::*;
use crate::text::redact_secrets;

/// Actor recorded for decisions taken in the GUI (archived interactive actor).
pub const ACTOR: &str = "user:interactive";
/// Actor of the side-effect-free acceptance preview shown in the detail.
pub const PREVIEW_ACTOR: &str = "user:interactive-preview";

/// Archived `review_text_excerpt`: newlines become spaces, runs of spaces
/// collapse, and text longer than `limit` bytes ends with `...` (cut on a
/// character boundary).
pub fn text_excerpt(text: &str, limit: usize) -> String {
    let mut collapsed = String::with_capacity(text.len());
    for ch in text.chars() {
        let ch = if ch == '\n' || ch == '\r' { ' ' } else { ch };
        if ch == ' ' && collapsed.ends_with(' ') {
            continue;
        }
        collapsed.push(ch);
    }
    if collapsed.len() <= limit {
        return collapsed;
    }
    let keep = if limit > 3 { limit - 3 } else { limit };
    let mut end = keep.min(collapsed.len());
    while !collapsed.is_char_boundary(end) {
        end -= 1;
    }
    format!("{}...", &collapsed[..end])
}

/// `45s ago`, `3m ago`, `2h ago`, `5d ago`; `unknown age` when unset.
pub fn age(time: Millis, now: Millis) -> String {
    if time == 0 {
        return "unknown age".into();
    }
    let seconds = ((now - time) / 1000).max(0);
    if seconds < 60 {
        format!("{seconds}s ago")
    } else if seconds < 3600 {
        format!("{}m ago", seconds / 60)
    } else if seconds < 86_400 {
        format!("{}h ago", seconds / 3600)
    } else {
        format!("{}d ago", seconds / 86_400)
    }
}

/// Two decimals (archived `format_decimal`).
pub fn decimal(value: f64) -> String {
    format!("{value:.2}")
}

/// Comma-joined values, `none` when empty.
pub fn join_values(values: &[String]) -> String {
    if values.is_empty() {
        "none".into()
    } else {
        values.join(", ")
    }
}

fn yes_no(value: bool) -> &'static str {
    if value { "yes" } else { "no" }
}

/// `project=… repository=… branch=… session=… task=…` with `<none>`.
pub fn exact_scope(scope: &MemoryScope) -> String {
    let shown = |value: &str| {
        if value.is_empty() {
            "<none>".to_string()
        } else {
            value.to_string()
        }
    };
    format!(
        "project={} repository={} branch={} session={} task={}",
        shown(&scope.project),
        shown(&scope.repository),
        shown(&scope.branch),
        shown(&scope.session),
        shown(&scope.task)
    )
}

/// Inbox scope label: repository display (or repository, project,
/// `global`) plus `@ branch`.
pub fn scope_label(scope: &MemoryScope) -> String {
    let mut label = if !scope.repository_display.is_empty() {
        scope.repository_display.clone()
    } else if !scope.repository.is_empty() {
        scope.repository.clone()
    } else if !scope.project.is_empty() {
        scope.project.clone()
    } else {
        "global".into()
    };
    if !scope.branch.is_empty() {
        label += &format!(" @ {}", scope.branch);
    }
    label
}

/// The proposal text a candidate carries (archived
/// `review_candidate_content`).
pub fn candidate_content(review: &HydratedReview) -> String {
    match &review.candidate.payload {
        ReviewCandidatePayload::DreamArtifact(value) => value.content.clone(),
        ReviewCandidatePayload::Consolidation(value) => value.canonical_content.clone(),
        ReviewCandidatePayload::Conflict(value) => value.explanation.clone(),
        ReviewCandidatePayload::ProcedureRevision(value) => value.proposed_content.clone(),
        ReviewCandidatePayload::General(value) => value.content.clone(),
        ReviewCandidatePayload::DreamResearch(_) => String::new(),
    }
}

/// Whether `kind` may be applied to `review` (archived `actionable`).
pub fn actionable(review: &HydratedReview, kind: ReviewActionKind) -> bool {
    use ReviewActionKind as A;
    use ReviewCandidateKind as K;
    let candidate = review.candidate.kind;
    if review.item.status != ReviewStatus::Pending {
        return kind == A::Reopen || (kind == A::Compensate && review.subject.is_some());
    }
    match kind {
        A::Reject | A::Defer => true,
        A::Approve | A::EditAndApprove => matches!(
            candidate,
            K::DreamArtifact | K::DreamResearchSuccessor | K::General | K::ProcedureRevision
        ),
        A::Relate | A::MarkContradiction | A::KeepBoth | A::Supersede => {
            matches!(
                candidate,
                K::Conflict | K::SemanticConsolidation | K::Compression
            ) || (kind == A::Supersede && candidate == K::Staleness)
        }
        A::Merge => matches!(
            candidate,
            K::SemanticConsolidation | K::Compression | K::Conflict
        ),
        A::NarrowScope => review.subject.is_some(),
        A::AttachEvidence => {
            let research_required = matches!(
                &review.candidate.payload,
                ReviewCandidatePayload::DreamArtifact(dream) if dream.research_required
            );
            review.subject.is_some()
                && (candidate == K::ProvenanceRepair
                    || review.item.provenance.source_uri.is_empty()
                    || research_required)
        }
        A::Deprecate | A::Revalidate | A::Retain => {
            candidate == K::Staleness || (kind == A::Deprecate && candidate == K::ProvenanceRepair)
        }
        A::Reopen | A::Compensate => false,
    }
}

/// Label of the approve outcome for a candidate kind.
pub fn visible_approval(kind: ReviewCandidateKind) -> &'static str {
    match kind {
        ReviewCandidateKind::DreamArtifact
        | ReviewCandidateKind::DreamResearchSuccessor
        | ReviewCandidateKind::ProcedureRevision => "Approve as hypothesis",
        ReviewCandidateKind::General => "Approve as active",
        _ => "Approve",
    }
}

/// One outcome offered in the review detail.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ActionChoice {
    pub kind: ReviewActionKind,
    /// Label including the archived shortcut suffix (`Reject [R]`).
    pub label: String,
    pub shortcut: Option<char>,
    pub description: String,
}

/// The actionable outcomes of a review, in archived order and wording.
pub fn action_choices(review: &HydratedReview) -> Vec<ActionChoice> {
    use ReviewActionKind as A;
    use ReviewCandidateKind as K;
    let candidate = review.candidate.kind;
    let relationship = matches!(candidate, K::SemanticConsolidation | K::Compression);
    let conflict = candidate == K::Conflict;
    let entries: Vec<(A, String, Option<char>, &str)> = vec![
        (
            A::Approve,
            visible_approval(candidate).into(),
            Some('A'),
            "Apply the typed proposal",
        ),
        (
            A::EditAndApprove,
            "Edit & approve".into(),
            Some('E'),
            "Edit proposal text before applying",
        ),
        (
            A::Reject,
            if relationship {
                "No relationship"
            } else if conflict {
                "Not a conflict"
            } else {
                "Reject"
            }
            .into(),
            Some('R'),
            if relationship {
                "Reject the proposed relationship without changing memories"
            } else if conflict {
                "Dismiss this conflict without changing memories"
            } else {
                "Reject without deleting audit history"
            },
        ),
        (
            A::Defer,
            if candidate == K::Staleness {
                "Retain / review later"
            } else {
                "Defer / Snooze"
            }
            .into(),
            Some('D'),
            "Resurface later without domain mutation",
        ),
        (
            A::Relate,
            "Relate".into(),
            None,
            "Preserve memories and add related edges",
        ),
        (
            A::MarkContradiction,
            "Mark contradiction".into(),
            None,
            "Preserve both with contradiction edges",
        ),
        (
            A::KeepBoth,
            "Keep both".into(),
            None,
            "Preserve both with a conflict warning",
        ),
        (
            A::Supersede,
            if candidate == K::Staleness {
                "Supersede with current memory"
            } else {
                "Supersede"
            }
            .into(),
            None,
            "Choose the canonical surviving memory",
        ),
        (
            A::Merge,
            match candidate {
                K::Conflict => "Merge conflicting memories",
                K::Compression => "Merge compressed canonical memory",
                _ => "Merge canonical memory",
            }
            .into(),
            None,
            "Create a canonical hypothesis and preserve lineage",
        ),
        (
            A::NarrowScope,
            "Narrow to current scope".into(),
            None,
            "Create a current-scope copy and supersede the broader memory",
        ),
        (
            A::Revalidate,
            if candidate == K::Staleness {
                "Update / revalidate from evidence"
            } else {
                "Revalidate"
            }
            .into(),
            None,
            "Require current independent evidence",
        ),
        (
            A::Deprecate,
            if candidate == K::ProvenanceRepair {
                "Deprecate / downgrade trust"
            } else {
                "Deprecate stale memory"
            }
            .into(),
            None,
            "Remove the memory from ordinary recall",
        ),
        (
            A::Retain,
            "Retain current memory".into(),
            None,
            "Keep the memory with an explicit reviewed-current marker",
        ),
        (
            A::AttachEvidence,
            if candidate == K::DreamArtifact {
                "Attach independent evidence"
            } else {
                "Attach evidence"
            }
            .into(),
            None,
            "Repair provenance with independently supplied evidence",
        ),
        (
            A::Reopen,
            "Reopen".into(),
            None,
            "Return this decision to the pending queue",
        ),
        (
            A::Compensate,
            "Correct with compensation".into(),
            None,
            "Append a governed correction without rewriting history",
        ),
    ];
    entries
        .into_iter()
        .filter(|(kind, ..)| actionable(review, *kind))
        .map(|(kind, label, shortcut, description)| ActionChoice {
            kind,
            label: match shortcut {
                Some(key) => format!("{label} [{key}]"),
                None => label,
            },
            shortcut,
            description: description.into(),
        })
        .collect()
}

/// The default typed outcome previewed in the detail ("What accepting will
/// do").
pub fn acceptance_preview_action(kind: ReviewCandidateKind) -> ReviewAction {
    let action_kind = match kind {
        ReviewCandidateKind::SemanticConsolidation | ReviewCandidateKind::Compression => {
            ReviewActionKind::Merge
        }
        ReviewCandidateKind::Conflict => ReviewActionKind::KeepBoth,
        ReviewCandidateKind::Staleness => ReviewActionKind::Retain,
        ReviewCandidateKind::ProvenanceRepair => ReviewActionKind::Deprecate,
        _ => ReviewActionKind::Approve,
    };
    let mut action = ReviewAction::new(action_kind, PREVIEW_ACTOR);
    action.human_authority = true;
    if action_kind == ReviewActionKind::Retain {
        action.note = "preview current-memory retention".into();
    }
    action
}

/// "What accepting will do" lines from a preview (or its error).
pub fn acceptance_preview_lines(
    preview: std::result::Result<&ReviewMutationPreview, String>,
    scope: &MemoryScope,
) -> Vec<String> {
    let mut lines = vec!["What accepting will do (default typed outcome):".to_string()];
    match preview {
        Ok(preview) => {
            for value in &preview.will_do {
                lines.push(format!("  Will: {}", text_excerpt(value, 140)));
            }
            for change in &preview.changes {
                lines.push(format!(
                    "  Exact: {} {} {} scope={{{}}} after={}",
                    change.record_type,
                    change.record_id,
                    change.operation,
                    exact_scope(scope),
                    text_excerpt(&change.after_json, 100)
                ));
            }
            if preview.will_do.is_empty() && preview.changes.is_empty() {
                lines.push("  Will: require one of the typed outcomes below".into());
            }
            lines.push("Will not:".into());
            if preview.will_not_do.is_empty() {
                lines.push(
                    "  Will not: validate, promote, or widen recall beyond the exact preview"
                        .into(),
                );
            } else {
                for value in &preview.will_not_do {
                    lines.push(format!("  Will not: {}", text_excerpt(value, 140)));
                }
            }
        }
        Err(error) => {
            lines.push(format!("  BLOCKED: {}", text_excerpt(&error, 140)));
            lines.push("Will not:".into());
            lines.push(
                "  Will not: mutate any memory until a valid typed outcome is previewed".into(),
            );
        }
    }
    lines
}

/// The detail lines of a hydrated review (archived `show_detail`), without
/// the acceptance preview.
pub fn detail_lines(
    review: &HydratedReview,
    queue_title: &str,
    inbox_scope: &MemoryScope,
    now: Millis,
) -> Vec<String> {
    use ReviewCandidateKind as K;
    let scope = review
        .subject
        .as_ref()
        .map(|subject| &subject.scope)
        .unwrap_or(inbox_scope);
    let mut lines = vec![
        format!(
            "Review Detail  [{}] {}",
            review.item.priority, review.item.kind
        ),
        format!(
            "Queue: {queue_title}  |  {}",
            age(review.item.created_at, now)
        ),
        format!("Scope (exact): {}", exact_scope(scope)),
        format!(
            "Proposal: {}",
            text_excerpt(&candidate_content(review), 150)
        ),
        format!("Reason: {}", text_excerpt(&review.item.reason, 150)),
        format!(
            "Risk: {} ({})  |  evidence {}",
            review.risk.risk_class,
            decimal(review.risk.score),
            decimal(review.item.provenance.evidence_strength)
        ),
        format!(
            "Recall now: strict={} balanced={} exploratory={}",
            yes_no(review.recall.strict),
            yes_no(review.recall.balanced),
            yes_no(review.recall.broad)
        ),
    ];
    match &review.subject {
        Some(subject) => lines.push(format!(
            "Subject: {}  {}/{}  confidence={}",
            subject.id,
            subject.memory_type,
            subject.status,
            decimal(subject.confidence)
        )),
        None => lines.push("Subject: unavailable".into()),
    }
    match (&review.candidate.kind, &review.candidate.payload) {
        (K::DreamArtifact, ReviewCandidatePayload::DreamArtifact(dream))
            if dream.research_required =>
        {
            lines.push(
                "Research required: attach independently supplied evidence before stronger trust."
                    .into(),
            );
            if review.related.is_empty() {
                lines.push("Linked research successors: none".into());
            }
            for related in &review.related {
                lines.push(format!(
                    "Linked research successor: {}  {}",
                    related.id,
                    text_excerpt(&related.content, 120)
                ));
            }
        }
        (K::DreamResearchSuccessor, ReviewCandidatePayload::DreamResearch(research)) => {
            let original = review
                .sources
                .iter()
                .find(|source| source.id == research.dream_id)
                .map(|source| text_excerpt(&source.content, 130))
                .unwrap_or_else(|| "unavailable".into());
            lines.push(format!("Original dream: {}  {original}", research.dream_id));
            lines.push(format!(
                "Supplied evidence: {}",
                if research.evidence_uri.is_empty() {
                    "unavailable"
                } else {
                    &research.evidence_uri
                }
            ));
            if let Some(subject) = &review.subject {
                lines.push(format!(
                    "Research successor: {}  {}",
                    subject.id,
                    text_excerpt(&subject.content, 130)
                ));
            }
        }
        (K::Staleness, _) => lines.push(
            "Staleness: needs review; this is not an assertion that the memory is false.".into(),
        ),
        (K::ProvenanceRepair, _) => lines.push(
            "Provenance repair: attach evidence, narrow scope, or deprecate to downgrade trust."
                .into(),
        ),
        _ => {}
    }
    if review.candidate.kind == K::SemanticConsolidation {
        for source in &review.sources {
            lines.push(format!(
                "Selected memory {}: {} confidence={} evidence={} uses={} scope={{{}}} source={} {}",
                source.id,
                source.status,
                decimal(source.confidence),
                decimal(source.provenance.evidence_strength),
                source.access_count,
                exact_scope(&source.scope),
                source.provenance.source_type,
                text_excerpt(&source.content, 90)
            ));
        }
        for edge in &review.edges {
            lines.push(format!(
                "Relationship/conflict: {} --{}--> {}",
                edge.from_id, edge.edge_type, edge.to_id
            ));
        }
    }
    lines
}

/// Lines after the acceptance preview: blocking diagnostics and the most
/// recent procedure outcome.
pub fn detail_footer_lines(review: &HydratedReview) -> Vec<String> {
    let mut lines: Vec<String> = review
        .blocking_diagnostics
        .iter()
        .map(|diagnostic| format!("BLOCKED: {}", text_excerpt(diagnostic, 150)))
        .collect();
    if let Some(run) = review.procedure_runs.first() {
        lines.push(format!(
            "Recent procedure outcome: {}  {}{}",
            run.outcome,
            text_excerpt(&run.notes, 100),
            if run.evidence_uri.is_empty() {
                String::new()
            } else {
                format!("  evidence={}", run.evidence_uri)
            }
        ));
    }
    lines
}

/// Source Inspector lines (archived `show_evidence`).
pub fn evidence_lines(review: &HydratedReview) -> Vec<String> {
    let item = &review.item;
    let mut lines = vec![
        "Evidence and Context".to_string(),
        format!("Review: {}", item.id),
        format!(
            "Provenance: {}  {}",
            item.provenance.source_type, item.provenance.source_uri
        ),
        format!(
            "Source hash: {}",
            if item.provenance.source_hash.is_empty() {
                "none"
            } else {
                &item.provenance.source_hash
            }
        ),
        format!("Source/model: {}", item.provenance.created_by),
        format!("Tags: {}", join_values(&item.tags)),
    ];
    if let Some(subject) = &review.subject {
        lines.push(format!(
            "Subject content: {}",
            text_excerpt(&subject.content, 180)
        ));
        lines.push(format!(
            "Usage/outcomes: accesses={} success={} failure={}",
            subject.access_count, subject.success_count, subject.failure_count
        ));
    }
    for source in &review.sources {
        lines.push(format!(
            "Source {} [{} confidence={} evidence={}] scope={{{}}}: {}",
            source.id,
            source.status,
            decimal(source.confidence),
            decimal(source.provenance.evidence_strength),
            exact_scope(&source.scope),
            text_excerpt(&source.content, 130)
        ));
        lines.push(format!(
            "  provenance={} uri={} hash={}",
            source.provenance.source_type,
            source.provenance.source_uri,
            source.provenance.source_hash
        ));
    }
    for related in &review.related {
        lines.push(format!(
            "Related {}: {}",
            related.id,
            text_excerpt(&related.content, 150)
        ));
    }
    for edge in &review.edges {
        lines.push(format!(
            "Edge: {} --{}--> {}",
            edge.from_id, edge.edge_type, edge.to_id
        ));
    }
    for warning in &review.recall.warnings {
        lines.push(format!("Warning: {warning}"));
    }
    for run in review.procedure_runs.iter().take(5) {
        lines.push(format!(
            "Procedure run {}: {}  {}{}",
            run.id,
            run.outcome,
            text_excerpt(&run.notes, 120),
            if run.evidence_uri.is_empty() {
                String::new()
            } else {
                format!("  evidence={}", run.evidence_uri)
            }
        ));
    }
    lines
}

/// Where the Source Inspector may read evidence from.
#[derive(Debug, Clone, Default)]
pub struct EvidenceAccess {
    pub cwd: PathBuf,
    pub workspace_root: PathBuf,
    pub allow_external_filesystem_access: bool,
}

/// The local file a provenance URI names, when it is a regular file the
/// workspace policy allows (archived `source_evidence_path`).
pub fn source_evidence_path(uri: &str, access: &EvidenceAccess) -> Option<PathBuf> {
    let uri = uri.strip_prefix("file://").unwrap_or(uri);
    if uri.is_empty() || uri.contains("://") {
        return None;
    }
    let requested = Path::new(uri);
    let requested = if requested.is_relative() {
        if !access.cwd.as_os_str().is_empty() {
            access.cwd.join(requested)
        } else {
            access.workspace_root.join(requested)
        }
    } else {
        requested.to_path_buf()
    };
    let path = std::fs::canonicalize(&requested).ok()?;
    if !path.is_file() {
        return None;
    }
    if access.allow_external_filesystem_access {
        return Some(path);
    }
    if access.workspace_root.as_os_str().is_empty() {
        return None;
    }
    let root = std::fs::canonicalize(&access.workspace_root)
        .unwrap_or_else(|_| access.workspace_root.clone());
    path.starts_with(&root).then_some(path)
}

/// The first 8 KiB of an evidence file with secrets redacted.
pub fn read_source_evidence(path: &Path) -> std::io::Result<String> {
    use std::io::Read as _;
    let mut buffer = vec![0u8; 8192];
    let mut file = std::fs::File::open(path)?;
    let mut read = 0;
    while read < buffer.len() {
        let count = file.read(&mut buffer[read..])?;
        if count == 0 {
            break;
        }
        read += count;
    }
    buffer.truncate(read);
    let mut content = String::from_utf8_lossy(&buffer).into_owned();
    redact_secrets(&mut content);
    Ok(content)
}

/// A supported edit-and-approve field.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EditField {
    Type,
    Status,
    Tags,
    Confidence,
    Replacement,
}

impl EditField {
    pub fn id(self) -> &'static str {
        match self {
            Self::Type => "type",
            Self::Status => "status",
            Self::Tags => "tags",
            Self::Confidence => "confidence",
            Self::Replacement => "replacement",
        }
    }

    /// Prompt hint for free-text fields.
    pub fn hint(self) -> &'static str {
        match self {
            Self::Tags => "Comma-separated tags",
            Self::Confidence => "Number from 0 to 1",
            Self::Replacement => "Existing in-scope memory ID",
            Self::Type => "Typed memory category",
            Self::Status => "Governed target trust state",
        }
    }
}

/// One editable field with its archived label and description.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct EditFieldSpec {
    pub field: EditField,
    pub label: &'static str,
    pub description: &'static str,
}

/// Fields edit-and-approve may change for a candidate kind (archived
/// `show_edit_fields`): general reviews edit type, target status, tags,
/// confidence and replacement; procedure revisions edit tags, confidence and
/// replacement; every other kind edits content only.
pub fn edit_fields(kind: ReviewCandidateKind) -> Vec<EditFieldSpec> {
    let spec = |field, label, description| EditFieldSpec {
        field,
        label,
        description,
    };
    match kind {
        ReviewCandidateKind::General => vec![
            spec(EditField::Type, "Type", "Set the typed memory category"),
            spec(
                EditField::Status,
                "Target trust state",
                "Candidate, hypothesis, or active",
            ),
            spec(EditField::Tags, "Tags", "Replace the governed tag set"),
            spec(
                EditField::Confidence,
                "Confidence",
                "Set a value from 0 to 1",
            ),
            spec(
                EditField::Replacement,
                "Replacement link",
                "Set an explicit replacement memory ID",
            ),
        ],
        ReviewCandidateKind::ProcedureRevision => vec![
            spec(EditField::Tags, "Tags", "Replace procedure tags"),
            spec(
                EditField::Confidence,
                "Confidence",
                "Set a value from 0 to 1",
            ),
            spec(
                EditField::Replacement,
                "Replacement link",
                "Link the revised procedure",
            ),
        ],
        _ => Vec::new(),
    }
}

/// Memory types offered for an edited general review.
pub const EDIT_TYPES: [MemoryType; 6] = [
    MemoryType::Observation,
    MemoryType::Semantic,
    MemoryType::Decision,
    MemoryType::Constraint,
    MemoryType::Preference,
    MemoryType::Procedural,
];

/// Target trust states offered for an edited general review.
pub const EDIT_STATUSES: [MemoryStatus; 3] = [
    MemoryStatus::Candidate,
    MemoryStatus::Hypothesis,
    MemoryStatus::Active,
];

/// Comma-separated tags, trimmed, empties dropped.
pub fn parse_tags(value: &str) -> Vec<String> {
    value
        .split(',')
        .map(str::trim)
        .filter(|tag| !tag.is_empty())
        .map(str::to_string)
        .collect()
}

/// A confidence from 0 to 1.
pub fn parse_confidence(value: &str) -> std::result::Result<f64, String> {
    let parsed: f64 = value
        .trim()
        .parse()
        .map_err(|_| "Invalid review edit: confidence must be a number".to_string())?;
    if !(0.0..=1.0).contains(&parsed) {
        return Err("Invalid review edit: confidence must be from 0 to 1".into());
    }
    Ok(parsed)
}

/// Applies one edit field value to the staged action.
pub fn apply_edit_field(
    action: &mut ReviewAction,
    field: EditField,
    value: &str,
) -> std::result::Result<(), String> {
    let edit = action.edit.get_or_insert_with(ReviewEdit::default);
    match field {
        EditField::Type => {
            edit.memory_type = Some(MemoryType::parse(value).map_err(|error| error.to_string())?);
        }
        EditField::Status => {
            edit.target_status =
                Some(MemoryStatus::parse(value).map_err(|error| error.to_string())?);
        }
        EditField::Tags => edit.tags = Some(parse_tags(value)),
        EditField::Confidence => edit.confidence = Some(parse_confidence(value)?),
        EditField::Replacement => edit.replacement_id = Some(value.trim().to_string()),
    }
    Ok(())
}

/// The staged value of an edit field, for display.
pub fn edit_field_value(action: &ReviewAction, field: EditField) -> String {
    let Some(edit) = &action.edit else {
        return "unchanged".into();
    };
    let shown = match field {
        EditField::Type => edit.memory_type.map(|value| value.to_string()),
        EditField::Status => edit.target_status.map(|value| value.to_string()),
        EditField::Tags => edit.tags.as_ref().map(|tags| join_values(tags)),
        EditField::Confidence => edit.confidence.map(decimal),
        EditField::Replacement => edit.replacement_id.clone(),
    };
    shown.unwrap_or_else(|| "unchanged".into())
}

/// Input an action collects before its preview.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ActionInput {
    /// Edit text: `(title, prompt, hint)`; the initial value comes from
    /// [`content_initial`].
    Content {
        title: &'static str,
        prompt: &'static str,
        hint: &'static str,
    },
    /// The edit-and-approve field list (see [`edit_fields`]).
    EditFields,
    /// An independent evidence URI.
    Evidence { title: &'static str },
    /// When a deferred review resurfaces ([`DEFER_CHOICES`]).
    DeferChoice,
    /// The surviving memory ([`target_choices`]).
    TargetChoice,
    /// Optional audit note; submitting it previews.
    Note,
}

/// The inputs `kind` collects for `review`, in order (archived
/// `begin_action`).
pub fn action_inputs(review: &HydratedReview, kind: ReviewActionKind) -> Vec<ActionInput> {
    use ReviewActionKind as A;
    let note = ActionInput::Note;
    match kind {
        A::EditAndApprove => vec![
            ActionInput::Content {
                title: "Edit review proposal",
                prompt: "Content",
                hint: "Applies this text to the preview",
            },
            ActionInput::EditFields,
            note,
        ],
        A::Merge => vec![
            ActionInput::Content {
                title: "Edit canonical merge",
                prompt: "Content",
                hint: "Applies this text to the preview",
            },
            note,
        ],
        A::Defer => vec![ActionInput::DeferChoice, note],
        A::Supersede => vec![ActionInput::TargetChoice, note],
        A::Compensate => vec![
            ActionInput::Content {
                title: "Correct reviewed memory",
                prompt: "Replacement content",
                hint: "A required audit reason is requested next",
            },
            note,
        ],
        A::Revalidate if review.candidate.kind == ReviewCandidateKind::Staleness => vec![
            ActionInput::Content {
                title: "Update stale memory",
                prompt: "Current content",
                hint: "Edit the current value, then attach independent evidence",
            },
            ActionInput::Evidence {
                title: "Revalidation evidence",
            },
            note,
        ],
        A::Revalidate => vec![
            ActionInput::Evidence {
                title: "Revalidation evidence",
            },
            note,
        ],
        A::AttachEvidence => vec![
            ActionInput::Evidence {
                title: "Attach review evidence",
            },
            note,
        ],
        _ => vec![note],
    }
}

/// Initial text of a content input.
pub fn content_initial(review: &HydratedReview, kind: ReviewActionKind) -> String {
    let subject = review
        .subject
        .as_ref()
        .map(|subject| subject.content.clone())
        .unwrap_or_default();
    match kind {
        ReviewActionKind::Compensate | ReviewActionKind::Revalidate => subject,
        _ => {
            let current = candidate_content(review);
            if current.is_empty() { subject } else { current }
        }
    }
}

/// The action before its inputs are collected: actor, human authority,
/// revision, the current scope for narrowing and the subject for
/// compensation.
pub fn base_action(
    review: &HydratedReview,
    kind: ReviewActionKind,
    current_scope: &MemoryScope,
) -> std::result::Result<ReviewAction, String> {
    if !actionable(review, kind) {
        return Err("Review action is not available for this candidate kind.".into());
    }
    let mut action = ReviewAction::new(kind, ACTOR);
    action.human_authority = true;
    action.expected_revision = review.item.revision;
    match kind {
        ReviewActionKind::NarrowScope => {
            action.edit = Some(ReviewEdit {
                scope: Some(current_scope.clone()),
                ..Default::default()
            });
        }
        ReviewActionKind::Compensate => {
            let Some(subject) = &review.subject else {
                return Err("compensation requires an available subject memory".into());
            };
            action.target_id = subject.id.clone();
        }
        _ => {}
    }
    Ok(action)
}

/// Stores the text of a content input.
pub fn apply_content(action: &mut ReviewAction, content: &str) {
    action.edit.get_or_insert_with(ReviewEdit::default).content = Some(content.to_string());
}

/// `(seconds, label, description)` of the defer choices.
pub const DEFER_CHOICES: [(i64, &str, &str); 3] = [
    (3600, "In one hour", "Short interruption"),
    (86_400, "Tomorrow", "Resurface in 24 hours"),
    (604_800, "In one week", "Resurface in 7 days"),
];

/// A memory that may survive a supersede.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TargetChoice {
    pub id: String,
    pub label: String,
    pub description: String,
}

/// Sources then the subject; conflicts label the first memory "Old remains
/// current" and the others "New supersedes old".
pub fn target_choices(review: &HydratedReview) -> Vec<TargetChoice> {
    let first_conflict = review
        .candidate
        .conflict()
        .and_then(|conflict| conflict.memory_ids.first().cloned());
    let label = |id: &str| match &first_conflict {
        Some(first) if first == id => "Old remains current".to_string(),
        Some(_) => "New supersedes old".to_string(),
        None => id.to_string(),
    };
    let mut seen = BTreeSet::new();
    review
        .sources
        .iter()
        .chain(review.subject.iter())
        // A subject that is also a source is offered once.
        .filter(|memory| seen.insert(memory.id.clone()))
        .map(|memory| TargetChoice {
            id: memory.id.clone(),
            label: label(&memory.id),
            description: text_excerpt(&memory.content, 80),
        })
        .collect()
}

/// Lines of the confirmation step (archived `show_confirmation`).
pub fn confirmation_lines(
    review_id: &str,
    action: &ReviewAction,
    preview: &ReviewMutationPreview,
    subject_scope: Option<&MemoryScope>,
    inbox_scope: &MemoryScope,
) -> Vec<String> {
    let scope = action
        .edit
        .as_ref()
        .and_then(|edit| edit.scope.as_ref())
        .or(subject_scope)
        .unwrap_or(inbox_scope);
    let mut lines = vec![
        format!("Confirm {}", action.kind),
        format!("Review: {review_id}"),
        format!("Scope (exact): {}", exact_scope(scope)),
    ];
    for value in &preview.will_do {
        lines.push(format!("Will: {}", text_excerpt(value, 160)));
    }
    for value in &preview.will_not_do {
        lines.push(format!("Will not: {}", text_excerpt(value, 160)));
    }
    for warning in &preview.warnings {
        lines.push(format!("Warning: {}", text_excerpt(warning, 160)));
    }
    for change in &preview.changes {
        lines.push(format!(
            "Change: {} {} {}",
            change.record_type, change.record_id, change.operation
        ));
        lines.push(format!(
            "  Before: {}",
            text_excerpt(&change.before_json, 140)
        ));
        lines.push(format!(
            "  After: {}",
            text_excerpt(&change.after_json, 140)
        ));
    }
    if !action.note.is_empty() {
        lines.push(format!("Audit note: {}", action.note));
    }
    lines
}

/// Why a preview cannot be applied (archived wording).
pub fn not_applicable_line(preview: &ReviewMutationPreview) -> String {
    format!(
        "Review action cannot be applied: {}",
        preview
            .warnings
            .first()
            .map(String::as_str)
            .unwrap_or("not applicable")
    )
}

/// Copies the preview's revision and token onto the action to apply.
pub fn confirm_action(action: &mut ReviewAction, preview: &ReviewMutationPreview) {
    action.expected_revision = preview.review_revision;
    action.idempotency_key = preview.idempotency_token.clone();
}

/// Transcript receipt of an applied action.
pub fn receipt_lines(review_id: &str, result: &ReviewApplicationResult) -> Vec<String> {
    let mut receipt = format!(
        "Memory review {review_id}: {}; decision={}; domain={}; revision={}",
        result.status,
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
    );
    if !result.changed_ids.is_empty() {
        receipt += &format!("; changed={}", join_values(&result.changed_ids));
    }
    if result.idempotent_replay {
        receipt += "; idempotent replay";
    }
    if !result.error.is_empty() {
        receipt += &format!("; error={}", result.error);
    }
    let mut lines = vec![receipt];
    if !result.failed_ids.is_empty() {
        lines.push(format!(
            "Review failed records: {}",
            join_values(&result.failed_ids)
        ));
    }
    lines
}

/// Whether a query differs from the complete pending queue.
pub fn has_active_filters(query: &ReviewQuery) -> bool {
    !query.kinds.is_empty()
        || !query.priorities.is_empty()
        || !query.tags.is_empty()
        || query.created_after.is_some()
        || query.created_before.is_some()
        || query.decided_after.is_some()
        || query.deferred_due_before.is_some()
        || query.scope != ReviewScopeSelector::All
        || query.statuses != [ReviewStatus::Pending]
}

/// `kind=…, priority=…, tags=…, status=…, scope=…` (archived
/// `query_filter_label`, plus the scope selector).
pub fn query_filter_label(query: &ReviewQuery) -> String {
    let mut filters = Vec::new();
    if !query.kinds.is_empty() {
        filters.push(format!("kind={}", join_values(&query.kinds)));
    }
    if !query.priorities.is_empty() {
        filters.push(format!("priority={}", join_values(&query.priorities)));
    }
    if !query.tags.is_empty() {
        filters.push(format!("tags={}", join_values(&query.tags)));
    }
    if query.statuses != [ReviewStatus::Pending] {
        let statuses: Vec<String> = query.statuses.iter().map(|s| s.to_string()).collect();
        filters.push(format!("status={}", join_values(&statuses)));
    }
    if query.scope != ReviewScopeSelector::All {
        filters.push(format!("scope={}", query.scope.as_str()));
    }
    if query.created_after.is_some() || query.created_before.is_some() {
        filters.push("created-age".into());
    }
    if query.decided_after.is_some() {
        filters.push("decided-age".into());
    }
    if query.deferred_due_before.is_some() {
        filters.push("deferred-due".into());
    }
    join_values(&filters)
}

/// The status filter presets.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StatusFilter {
    Pending,
    Deferred,
    Terminal,
    All,
}

impl StatusFilter {
    pub const ALL: [StatusFilter; 4] = [Self::Pending, Self::Deferred, Self::Terminal, Self::All];

    pub fn label(self) -> &'static str {
        match self {
            Self::Pending => "Pending",
            Self::Deferred => "Deferred",
            Self::Terminal => "History",
            Self::All => "All statuses",
        }
    }

    pub fn statuses(self) -> Vec<ReviewStatus> {
        match self {
            Self::Pending => vec![ReviewStatus::Pending],
            Self::Deferred => vec![ReviewStatus::Deferred],
            Self::Terminal => vec![ReviewStatus::Accepted, ReviewStatus::Rejected],
            Self::All => vec![
                ReviewStatus::Pending,
                ReviewStatus::Deferred,
                ReviewStatus::Accepted,
                ReviewStatus::Rejected,
            ],
        }
    }

    /// The preset a query's statuses match, if any.
    pub fn of(query: &ReviewQuery) -> Option<Self> {
        Self::ALL
            .into_iter()
            .find(|preset| preset.statuses() == query.statuses)
    }
}

/// `(value, label)` of the priority filter; `""` clears it.
pub const PRIORITY_FILTERS: [(&str, &str); 5] = [
    ("", "Any priority"),
    ("urgent", "Urgent"),
    ("high", "High"),
    ("normal", "Normal"),
    ("low", "Low"),
];

/// `(value, label)` of the history filter.
pub const HISTORY_FILTERS: [(&str, &str); 7] = [
    ("all", "All"),
    ("accepted", "Accepted"),
    ("rejected", "Rejected"),
    ("deferred", "Deferred"),
    ("reopened", "Reopened"),
    ("failed", "Failed"),
    ("compensated", "Compensated"),
];

/// Whether a history record passes the history filter.
pub fn history_matches(record: &ReviewActionRecord, filter: &str) -> bool {
    match filter {
        "all" => true,
        "failed" => record.application_status == ReviewApplicationStatus::Failed,
        "reopened" => record.action == "reopen",
        "compensated" => record.action == "compensate",
        other => record.after_status.as_str() == other,
    }
}

/// `(label, description)` of a history row.
pub fn history_row(record: &ReviewActionRecord) -> (String, String) {
    (
        format!(
            "{}  {}  {}",
            record.action, record.after_status, record.application_status
        ),
        format!(
            "review={} actor={} receipt={}",
            record.review_id,
            record.actor,
            text_excerpt(&record.result_json, 90)
        ),
    )
}

/// Lines of a history receipt.
pub fn history_receipt_lines(record: &ReviewActionRecord) -> Vec<String> {
    vec![
        "Review history receipt".into(),
        format!("Review: {}", record.review_id),
        format!("Action: {} by {}", record.action, record.actor),
        format!(
            "Status: {} -> {}",
            record.before_status, record.after_status
        ),
        format!("Application: {}", record.application_status),
        format!("Note: {}", record.note),
        format!("Error: {}", record.application_error),
        format!("Linked receipt: {}", record.result_json),
    ]
}

/// `(label, description)` of a queue card (archived `show_queue`).
pub fn queue_card(
    index: usize,
    total: usize,
    item: &ReviewItem,
    detail: std::result::Result<&HydratedReview, String>,
    now: Millis,
) -> (String, String) {
    let mut proposal = item.reason.clone();
    let badges = match detail {
        Ok(hydrated) => {
            let candidate = candidate_content(hydrated);
            if !candidate.is_empty() {
                proposal = candidate;
            }
            let mut badges = format!("risk {}", hydrated.risk.risk_class);
            if !hydrated.blocking_diagnostics.is_empty() {
                badges += ", blocked";
            }
            if let Some(subject) = &hydrated.subject {
                badges += &format!(
                    ", {}, confidence {}, evidence {}, scope {}",
                    subject.status,
                    decimal(subject.confidence),
                    decimal(subject.provenance.evidence_strength),
                    exact_scope(&subject.scope)
                );
            }
            if !hydrated.recall.warnings.is_empty() {
                badges += ", recall warning";
            }
            badges
        }
        Err(_) => "unavailable detail".into(),
    };
    (
        format!("{}/{}  {}  {}", index + 1, total, item.priority, item.kind),
        format!(
            "{} | reason {} | {badges} | {}",
            text_excerpt(&proposal, 62),
            text_excerpt(&item.reason, 52),
            age(item.created_at, now)
        ),
    )
}

/// Pending review counts by kind (first 1000 items).
pub fn pending_kind_counts(inbox: &ReviewInbox) -> Result<BTreeMap<String, usize>> {
    let query = ReviewQuery {
        limit: 1000,
        ..Default::default()
    };
    let mut counts = BTreeMap::new();
    for item in inbox.review_items(&query)? {
        *counts.entry(item.kind).or_insert(0) += 1;
    }
    Ok(counts)
}

/// Kinds of Dreaming reviews.
pub const DREAM_KINDS: [&str; 2] = ["dream_artifact", "dream_research_successor"];

/// Landing summary lines (archived `show_landing`).
pub fn landing_lines(inbox: &ReviewInbox, now: Millis) -> Result<Vec<String>> {
    let categories = pending_kind_counts(inbox)?;
    let stats = inbox.review_statistics()?;
    let dream_query = ReviewQuery {
        kinds: DREAM_KINDS.iter().map(|kind| kind.to_string()).collect(),
        statuses: StatusFilter::All.statuses(),
        include_due_deferred: false,
        limit: 1000,
        ..Default::default()
    };
    // The newest Dream review by creation time (the archived landing took the
    // first item in queue order, which could name an older decided review
    // while a newer one was pending).
    let dreams = inbox.review_items(&dream_query)?;
    let latest: Vec<&ReviewItem> = dreams
        .iter()
        .max_by(|left, right| {
            left.created_at
                .cmp(&right.created_at)
                .then_with(|| right.id.cmp(&left.id))
        })
        .into_iter()
        .collect();
    let mut lines = vec![
        "Memory Review Inbox".to_string(),
        format!("Scope: {}", scope_label(&inbox.scope())),
        format!(
            "Pending {}  |  urgent/high {}  |  due deferred {}",
            stats.pending, stats.urgent, stats.due_deferred
        ),
        format!(
            "Oldest pending: {}",
            if stats.pending == 0 {
                "none".to_string()
            } else {
                crate::slash::review_duration(stats.oldest_pending_age_seconds)
            }
        ),
        format!(
            "Latest Dreaming: {}",
            match latest.first() {
                Some(item) => format!("{} {} ({})", item.kind, item.id, age(item.created_at, now)),
                None => "none".into(),
            }
        ),
        format!(
            "History: approved={} rejected={} deferred={} reopened={} failed={} compensated={}",
            stats.approved,
            stats.rejected,
            stats.deferred,
            stats.reopened,
            stats.failed,
            stats.compensated
        ),
    ];
    if !categories.is_empty() {
        let counts: Vec<String> = categories
            .iter()
            .map(|(kind, count)| format!("{kind}={count}"))
            .collect();
        lines.push(format!("Categories: {}", join_values(&counts)));
    }
    Ok(lines)
}

/// The newest Dreaming review: its queue and position (archived
/// `show_latest_dream`).
pub fn latest_dream(inbox: &ReviewInbox) -> Result<Option<(ReviewQuery, usize, String)>> {
    let query = ReviewQuery {
        kinds: DREAM_KINDS.iter().map(|kind| kind.to_string()).collect(),
        limit: 1000,
        ..Default::default()
    };
    let reviews = inbox.review_items(&query)?;
    let latest = reviews.iter().enumerate().max_by(|(_, left), (_, right)| {
        left.created_at
            .cmp(&right.created_at)
            .then_with(|| right.id.cmp(&left.id))
    });
    Ok(latest.map(|(position, item)| (query.clone(), position, item.id.clone())))
}

/// Lines of the batch selection (archived `show_batch_selection`).
pub fn batch_summary_lines(
    inbox: &ReviewInbox,
    items: &[ReviewItem],
    selected: &BTreeSet<String>,
) -> Vec<String> {
    let mut kinds: BTreeMap<String, usize> = BTreeMap::new();
    let mut risks: BTreeMap<String, usize> = BTreeMap::new();
    let mut scopes: BTreeSet<String> = BTreeSet::new();
    let inbox_scope = inbox.scope();
    for item in items.iter().filter(|item| selected.contains(&item.id)) {
        *kinds.entry(item.kind.clone()).or_insert(0) += 1;
        if let Ok(detail) = inbox.review_detail(&item.id) {
            *risks.entry(detail.risk.risk_class.clone()).or_insert(0) += 1;
            scopes.insert(exact_scope(
                detail
                    .subject
                    .as_ref()
                    .map(|subject| &subject.scope)
                    .unwrap_or(&inbox_scope),
            ));
        }
    }
    let counts = |values: &BTreeMap<String, usize>| {
        join_values(
            &values
                .iter()
                .map(|(name, count)| format!("{name}={count}"))
                .collect::<Vec<_>>(),
        )
    };
    vec![
        "Batch pending reviews".into(),
        format!("Selected: {} / {}", selected.len(), items.len()),
        format!("Kinds: {}", counts(&kinds)),
        format!(
            "Scopes: {}",
            join_values(&scopes.into_iter().collect::<Vec<_>>())
        ),
        format!("Risk: {}", counts(&risks)),
        "Approval is limited by runtime policy to homogeneous, low-risk, source-backed items."
            .into(),
    ]
}

/// Batch items for the selected reviews.
pub fn batch_items(selected: &BTreeSet<String>, kind: ReviewActionKind) -> Vec<ReviewBatchItem> {
    selected
        .iter()
        .map(|id| {
            let mut action = ReviewAction::new(kind, ACTOR);
            action.human_authority = true;
            ReviewBatchItem {
                review_id: id.clone(),
                action,
            }
        })
        .collect()
}

/// Copies each preview's revision and token onto the batch items.
pub fn confirm_batch(items: &mut [ReviewBatchItem], preview: &ReviewBatchPreview) {
    for (item, previewed) in items.iter_mut().zip(&preview.items) {
        item.action.expected_revision = previewed.review_revision;
        item.action.idempotency_key = previewed.idempotency_token.clone();
    }
}

/// Lines of a batch preview: aggregate effects and every change, or why it
/// cannot be applied.
pub fn batch_preview_lines(
    kind: ReviewActionKind,
    preview: &ReviewBatchPreview,
    query: &ReviewQuery,
    count: usize,
) -> Vec<String> {
    if !preview.applicable {
        let mut lines = vec![
            "Batch cannot be applied".to_string(),
            "Selection and filters were preserved.".to_string(),
        ];
        lines.extend(
            preview
                .warnings
                .iter()
                .map(|warning| format!("Warning: {}", text_excerpt(warning, 180))),
        );
        return lines;
    }
    let mut lines = vec![
        format!("Confirm batch {kind}"),
        format!("Count: {count}"),
        format!("Scope filter: {}", query_filter_label(query)),
    ];
    let mut effects: BTreeMap<String, usize> = BTreeMap::new();
    for item in &preview.items {
        for change in &item.changes {
            *effects
                .entry(format!("{}:{}", change.record_type, change.operation))
                .or_insert(0) += 1;
        }
    }
    for (effect, count) in effects {
        lines.push(format!("Aggregate effect: {effect} x{count}"));
    }
    for item in &preview.items {
        for change in &item.changes {
            lines.push(format!(
                "Change: {} {} {}",
                change.record_type, change.record_id, change.operation
            ));
        }
    }
    lines
}

/// Result lines of an applied batch and the review IDs to keep selected
/// (failed items are preserved for retry, archived).
pub fn batch_result(result: &ReviewBatchResult, total: usize) -> (Vec<String>, BTreeSet<String>) {
    let failed: BTreeSet<String> = result
        .items
        .iter()
        .filter(|item| {
            !item.error.is_empty()
                || !item
                    .result
                    .as_ref()
                    .is_some_and(|result| result.decision_persisted)
        })
        .map(|item| item.review_id.clone())
        .collect();
    let mut lines = vec![format!(
        "Memory review batch result: processed={total}/{total}; applied={}; failed={}{}",
        result.applied_count,
        result.failed_count,
        if result.partial_failure {
            "; partial failure"
        } else {
            ""
        }
    )];
    if !failed.is_empty() {
        lines.push("Failed batch selections were preserved for retry or inspection.".into());
    }
    (lines, failed)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hydrated(kind: ReviewCandidateKind, payload: ReviewCandidatePayload) -> HydratedReview {
        HydratedReview {
            item: ReviewItem {
                id: "review_1".into(),
                revision: 3,
                ..Default::default()
            },
            candidate: ReviewCandidate {
                kind,
                payload,
                ..Default::default()
            },
            subject: Some(MemoryEntry {
                id: "mem_1".into(),
                content: "subject text".into(),
                ..Default::default()
            }),
            ..Default::default()
        }
    }

    fn general() -> HydratedReview {
        hydrated(
            ReviewCandidateKind::General,
            ReviewCandidatePayload::General(GeneralReviewCandidate {
                content: "proposal".into(),
                ..Default::default()
            }),
        )
    }

    #[test]
    fn excerpt_and_age_follow_archive() {
        assert_eq!(text_excerpt("a\n\nb   c", 110), "a b c");
        assert_eq!(text_excerpt("abcdefghij", 6), "abc...");
        assert_eq!(text_excerpt("ééééé", 6), "é...");
        assert_eq!(age(0, 5), "unknown age");
        assert_eq!(age(1000, 46_000), "45s ago");
        assert_eq!(age(1, 7_200_001), "2h ago");
        assert_eq!(join_values(&[]), "none");
        assert_eq!(
            exact_scope(&MemoryScope {
                project: "p".into(),
                ..Default::default()
            }),
            "project=p repository=<none> branch=<none> session=<none> task=<none>"
        );
    }

    #[test]
    fn actionable_rules_per_kind() {
        use ReviewActionKind as A;
        let review = general();
        let kinds: Vec<A> = action_choices(&review).iter().map(|c| c.kind).collect();
        assert_eq!(
            kinds,
            [
                A::Approve,
                A::EditAndApprove,
                A::Reject,
                A::Defer,
                A::NarrowScope,
                // No provenance URI: evidence may be attached.
                A::AttachEvidence
            ]
        );
        assert_eq!(action_choices(&review)[0].label, "Approve as active [A]");

        let conflict = hydrated(
            ReviewCandidateKind::Conflict,
            ReviewCandidatePayload::Conflict(ConflictCandidate::default()),
        );
        let labels: Vec<String> = action_choices(&conflict)
            .into_iter()
            .map(|c| c.label)
            .collect();
        assert!(labels.contains(&"Not a conflict [R]".to_string()));
        assert!(labels.contains(&"Merge conflicting memories".to_string()));
        assert!(!actionable(&conflict, A::Approve));

        let mut staleness = hydrated(
            ReviewCandidateKind::Staleness,
            ReviewCandidatePayload::General(Default::default()),
        );
        assert!(actionable(&staleness, A::Retain));
        assert!(actionable(&staleness, A::Supersede));
        assert!(!actionable(&staleness, A::Merge));
        staleness.item.status = ReviewStatus::Accepted;
        let kinds: Vec<A> = action_choices(&staleness).iter().map(|c| c.kind).collect();
        assert_eq!(kinds, [A::Reopen, A::Compensate]);

        let mut dream = hydrated(
            ReviewCandidateKind::DreamArtifact,
            ReviewCandidatePayload::DreamArtifact(DreamArtifactCandidate {
                research_required: true,
                ..Default::default()
            }),
        );
        dream.item.provenance.source_uri = "file:x".into();
        assert!(actionable(&dream, A::AttachEvidence));
        assert_eq!(
            visible_approval(dream.candidate.kind),
            "Approve as hypothesis"
        );
    }

    #[test]
    fn edit_and_approve_field_rules() {
        let general_fields: Vec<EditField> = edit_fields(ReviewCandidateKind::General)
            .iter()
            .map(|f| f.field)
            .collect();
        assert_eq!(
            general_fields,
            [
                EditField::Type,
                EditField::Status,
                EditField::Tags,
                EditField::Confidence,
                EditField::Replacement
            ]
        );
        let procedure: Vec<EditField> = edit_fields(ReviewCandidateKind::ProcedureRevision)
            .iter()
            .map(|f| f.field)
            .collect();
        assert_eq!(
            procedure,
            [
                EditField::Tags,
                EditField::Confidence,
                EditField::Replacement
            ]
        );
        assert!(edit_fields(ReviewCandidateKind::DreamArtifact).is_empty());

        let mut action = ReviewAction::new(ReviewActionKind::EditAndApprove, ACTOR);
        apply_edit_field(&mut action, EditField::Tags, " a, ,b ").unwrap();
        apply_edit_field(&mut action, EditField::Confidence, "0.4").unwrap();
        apply_edit_field(&mut action, EditField::Type, "decision").unwrap();
        apply_edit_field(&mut action, EditField::Status, "active").unwrap();
        apply_edit_field(&mut action, EditField::Replacement, " mem_9 ").unwrap();
        let edit = action.edit.clone().unwrap();
        assert_eq!(edit.tags.unwrap(), ["a", "b"]);
        assert_eq!(edit.confidence, Some(0.4));
        assert_eq!(edit.memory_type, Some(MemoryType::Decision));
        assert_eq!(edit.target_status, Some(MemoryStatus::Active));
        assert_eq!(edit.replacement_id.as_deref(), Some("mem_9"));
        assert_eq!(
            apply_edit_field(&mut action, EditField::Confidence, "2").unwrap_err(),
            "Invalid review edit: confidence must be from 0 to 1"
        );
        assert_eq!(edit_field_value(&action, EditField::Confidence), "0.40");
    }

    #[test]
    fn action_inputs_and_base_action() {
        use ReviewActionKind as A;
        let review = general();
        assert_eq!(
            action_inputs(&review, A::EditAndApprove)[1],
            ActionInput::EditFields
        );
        assert_eq!(
            action_inputs(&review, A::Defer),
            [ActionInput::DeferChoice, ActionInput::Note]
        );
        assert_eq!(content_initial(&review, A::EditAndApprove), "proposal");
        let scope = MemoryScope {
            project: "here".into(),
            ..Default::default()
        };
        let narrow = base_action(&review, A::NarrowScope, &scope).unwrap();
        assert_eq!(narrow.edit.unwrap().scope.unwrap().project, "here");
        assert_eq!(narrow.expected_revision, 3);
        assert!(narrow.human_authority);
        assert_eq!(
            base_action(&review, A::Merge, &scope).unwrap_err(),
            "Review action is not available for this candidate kind."
        );
    }

    #[test]
    fn filters_and_receipts() {
        let mut query = ReviewQuery::default();
        assert!(!has_active_filters(&query));
        query.kinds = vec!["conflict".into()];
        query.statuses = StatusFilter::Terminal.statuses();
        query.scope = ReviewScopeSelector::Durable;
        assert_eq!(
            query_filter_label(&query),
            "kind=conflict, status=accepted, rejected, scope=durable"
        );
        assert_eq!(StatusFilter::of(&query), Some(StatusFilter::Terminal));
        let result = ReviewApplicationResult {
            status: ReviewApplicationStatus::Applied,
            decision_persisted: true,
            domain_mutations_applied: true,
            revision: 4,
            changed_ids: vec!["m".into()],
            ..Default::default()
        };
        assert_eq!(
            receipt_lines("r", &result),
            ["Memory review r: applied; decision=persisted; domain=applied; revision=4; changed=m"]
        );
    }
}
