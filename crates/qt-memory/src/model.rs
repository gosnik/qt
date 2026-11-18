//! Domain model: memory types, lifecycle states, provenance and records.
//!
//! String spellings match the archived `models.cpp`; they are persisted in
//! SQLite and exported JSON, so they must not change. Timestamps are Unix
//! milliseconds (`0` means "unset", as in the archived store).

use crate::error::{MemoryError, Result};

/// Unix timestamp in milliseconds.
pub type Millis = i64;

/// Current wall-clock time in Unix milliseconds.
pub fn now_ms() -> Millis {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|duration| i64::try_from(duration.as_millis()).unwrap_or(i64::MAX))
        .unwrap_or(0)
}

string_enum!(
    /// Memory category.
    MemoryType {
        Semantic => "semantic",
        Episodic => "episodic",
        Procedural => "procedural",
        Decision => "decision",
        Constraint => "constraint",
        Bug => "bug",
        Preference => "preference",
        Skill => "skill",
        Observation => "observation",
        Todo => "todo",
        Unprocessed => "unprocessed",
        Dream => "dream",
        Conversation => "conversation",
        Tool => "tool",
        Pattern => "pattern",
        Raw => "raw",
        Research => "research",
    }
);

impl MemoryType {
    /// Accepts the persisted spellings plus the qtmem aliases `episode`,
    /// `procedure` and `failure`.
    pub fn parse(value: &str) -> Result<Self> {
        Ok(match value {
            "episode" => Self::Episodic,
            "procedure" => Self::Procedural,
            "failure" => Self::Bug,
            other => Self::ALL
                .iter()
                .copied()
                .find(|item| item.as_str() == other)
                .ok_or_else(|| MemoryError::invalid(format!("unknown memory type: {value}")))?,
        })
    }

    /// Types whose mistakes have outsized impact; reviews of them rank higher.
    pub fn is_critical(self) -> bool {
        matches!(
            self,
            Self::Constraint | Self::Decision | Self::Procedural | Self::Preference
        )
    }
}

string_enum!(
    /// Lifecycle (trust) state.
    MemoryStatus {
        Candidate => "candidate",
        Hypothesis => "hypothesis",
        Active => "active",
        Validated => "validated",
        Deprecated => "deprecated",
        Superseded => "superseded",
        Rejected => "rejected",
        Expired => "expired",
        DreamArtifact => "dream_artifact",
    }
);

impl MemoryStatus {
    pub fn parse(value: &str) -> Result<Self> {
        Self::ALL
            .iter()
            .copied()
            .find(|item| item.as_str() == value)
            .ok_or_else(|| MemoryError::invalid(format!("unknown memory status: {value}")))
    }
}

string_enum!(
    /// Origin of a memory's evidence.
    SourceType {
        Chat => "chat",
        File => "file",
        Commit => "commit",
        Issue => "issue",
        PullRequest => "pull_request",
        Document => "doc",
        Manual => "manual",
        ToolResult => "tool_result",
        TestLog => "test_log",
        ProcessedText => "processed_text",
        Dreaming => "dreaming",
        Unknown => "unknown",
    }
);

impl SourceType {
    /// Lenient: aliases `pr` and `document`; anything else is `Unknown`.
    pub fn parse(value: &str) -> Self {
        match value {
            "pr" => Self::PullRequest,
            "document" => Self::Document,
            other => Self::ALL
                .iter()
                .copied()
                .find(|item| item.as_str() == other)
                .unwrap_or(Self::Unknown),
        }
    }
}

string_enum!(
    /// Scope granularity, ordered from narrowest to broadest.
    ScopeKind {
        Exact => "exact",
        Branch => "branch",
        Repository => "repository",
        Project => "project",
        GlobalUser => "global_user",
    }
);

impl ScopeKind {
    pub fn parse(value: &str) -> Result<Self> {
        Self::ALL
            .iter()
            .copied()
            .find(|item| item.as_str() == value)
            .ok_or_else(|| MemoryError::invalid(format!("unknown scope kind: {value}")))
    }

    /// Breadth rank: `Exact` = 0 … `GlobalUser` = 4.
    pub fn breadth(self) -> u8 {
        match self {
            Self::Exact => 0,
            Self::Branch => 1,
            Self::Repository => 2,
            Self::Project => 3,
            Self::GlobalUser => 4,
        }
    }
}

string_enum!(
    /// Which ancestor scopes recall may read.
    RecallPolicy {
        Strict => "strict",
        Balanced => "balanced",
        Broad => "broad",
    }
);

impl RecallPolicy {
    /// Settings spelling (`memory.recallPolicy`) to storage policy. The archived
    /// coordinator treats `strict` as strict, `exploratory` as broad and every
    /// other accepted value (`balanced`, `debugging`, `architecture`) as balanced.
    pub fn from_setting(value: &str) -> Self {
        match value {
            "strict" => Self::Strict,
            "exploratory" | "broad" => Self::Broad,
            _ => Self::Balanced,
        }
    }
}

string_enum!(
    /// Review queue item state.
    ReviewStatus {
        Pending => "pending",
        Accepted => "accepted",
        Rejected => "rejected",
        Deferred => "deferred",
    }
);

impl ReviewStatus {
    pub fn parse(value: &str) -> Result<Self> {
        Self::ALL
            .iter()
            .copied()
            .find(|item| item.as_str() == value)
            .ok_or_else(|| MemoryError::invalid(format!("unknown review status: {value}")))
    }
}

string_enum!(
    /// Outcome reported for a procedural memory run.
    ProcedureOutcome {
        Success => "success",
        Failure => "failure",
        Partial => "partial",
        Unused => "unused",
        Unknown => "unknown",
    }
);

impl ProcedureOutcome {
    /// Lenient: unrecognised values are `Unknown`.
    pub fn parse(value: &str) -> Self {
        Self::ALL
            .iter()
            .copied()
            .find(|item| item.as_str() == value)
            .unwrap_or(Self::Unknown)
    }
}

/// The dimensions that identify where a memory lives.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct MemoryScope {
    pub kind: ScopeKind,
    pub user: String,
    pub organization: String,
    pub project: String,
    pub repository: String,
    /// Display label only; not part of the scope key.
    pub repository_display: String,
    pub branch: String,
    pub task: String,
    pub agent: String,
    pub session: String,
    pub conversation: String,
}

#[allow(clippy::derivable_impls)] // the enum is macro-generated
impl Default for ScopeKind {
    fn default() -> Self {
        Self::Exact
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct Provenance {
    pub source_id: String,
    pub source_type: SourceType,
    pub source_uri: String,
    pub source_hash: String,
    pub source_span: String,
    pub created_by: String,
    pub evidence_strength: f64,
}

impl Default for Provenance {
    fn default() -> Self {
        Self {
            source_id: String::new(),
            source_type: SourceType::Unknown,
            source_uri: String::new(),
            source_hash: String::new(),
            source_span: String::new(),
            created_by: "agent".into(),
            evidence_strength: 0.0,
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct MemoryEntry {
    pub id: String,
    pub scope: MemoryScope,
    pub memory_type: MemoryType,
    pub status: MemoryStatus,
    pub provenance: Provenance,
    pub source: String,
    pub content: String,
    pub tags: Vec<String>,
    pub confidence: f64,
    pub quality_score: f64,
    pub success_count: u64,
    pub failure_count: u64,
    pub last_outcome: ProcedureOutcome,
    pub access_count: u64,
    pub superseded_by: String,
    pub replacement_id: String,
    pub created_at: Millis,
    pub updated_at: Millis,
    pub last_accessed_at: Millis,
}

impl Default for MemoryEntry {
    fn default() -> Self {
        Self {
            id: String::new(),
            scope: MemoryScope::default(),
            memory_type: MemoryType::Observation,
            status: MemoryStatus::Hypothesis,
            provenance: Provenance::default(),
            source: String::new(),
            content: String::new(),
            tags: Vec::new(),
            confidence: 0.5,
            quality_score: 0.0,
            success_count: 0,
            failure_count: 0,
            last_outcome: ProcedureOutcome::Unknown,
            access_count: 0,
            superseded_by: String::new(),
            replacement_id: String::new(),
            created_at: 0,
            updated_at: 0,
            last_accessed_at: 0,
        }
    }
}

/// A new memory.
#[derive(Debug, Clone, PartialEq)]
pub struct MemoryWrite {
    pub memory_type: MemoryType,
    pub status: MemoryStatus,
    pub provenance: Provenance,
    pub source: String,
    pub content: String,
    pub tags: Vec<String>,
    pub confidence: f64,
    pub quality_score: f64,
}

impl Default for MemoryWrite {
    fn default() -> Self {
        Self {
            memory_type: MemoryType::Observation,
            status: MemoryStatus::Hypothesis,
            provenance: Provenance::default(),
            source: String::new(),
            content: String::new(),
            tags: Vec::new(),
            confidence: 0.5,
            quality_score: 0.0,
        }
    }
}

/// Partial update; `None` leaves a field unchanged.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct MemoryPatch {
    pub content: Option<String>,
    pub memory_type: Option<MemoryType>,
    pub status: Option<MemoryStatus>,
    /// Sets both `replacement_id` and `superseded_by`.
    pub replacement_id: Option<String>,
    pub confidence: Option<f64>,
    pub quality_score: Option<f64>,
    pub provenance: Option<Provenance>,
    pub tags: Option<Vec<String>>,
}

#[derive(Debug, Clone)]
pub struct MemoryQuery {
    pub text: String,
    pub memory_type: Option<MemoryType>,
    pub statuses: Vec<MemoryStatus>,
    pub tags: Vec<String>,
    pub policy: RecallPolicy,
    pub include_global_user: bool,
    pub include_superseded: bool,
    /// `0` means unlimited.
    pub limit: usize,
    pub offset: usize,
}

impl Default for MemoryQuery {
    fn default() -> Self {
        Self {
            text: String::new(),
            memory_type: None,
            statuses: Vec::new(),
            tags: Vec::new(),
            policy: RecallPolicy::Balanced,
            include_global_user: false,
            include_superseded: false,
            limit: 20,
            offset: 0,
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct MemoryRevision {
    pub id: String,
    pub memory_id: String,
    pub kind: String,
    pub entry_json: String,
    pub created_at: Millis,
}

#[derive(Debug, Clone, PartialEq)]
pub struct GraphEdge {
    pub id: String,
    pub from_id: String,
    pub to_id: String,
    pub edge_type: String,
    pub confidence: f64,
    pub source: String,
    pub created_at: Millis,
}

impl GraphEdge {
    pub fn new(
        from_id: impl Into<String>,
        to_id: impl Into<String>,
        edge_type: impl Into<String>,
    ) -> Self {
        Self {
            id: String::new(),
            from_id: from_id.into(),
            to_id: to_id.into(),
            edge_type: edge_type.into(),
            confidence: 1.0,
            source: String::new(),
            created_at: 0,
        }
    }
}

#[derive(Debug, Clone)]
pub struct GraphQuery {
    pub memory_id: String,
    pub from_id: String,
    pub to_id: String,
    pub edge_type: String,
    pub limit: usize,
}

impl Default for GraphQuery {
    fn default() -> Self {
        Self {
            memory_id: String::new(),
            from_id: String::new(),
            to_id: String::new(),
            edge_type: String::new(),
            limit: 50,
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct ProcedureFeedback {
    pub procedure_id: String,
    pub task_id: String,
    pub outcome: ProcedureOutcome,
    pub notes: String,
    pub evidence_uri: String,
    pub deduplication_key: String,
}

impl Default for ProcedureFeedback {
    fn default() -> Self {
        Self {
            procedure_id: String::new(),
            task_id: String::new(),
            outcome: ProcedureOutcome::Unknown,
            notes: String::new(),
            evidence_uri: String::new(),
            deduplication_key: String::new(),
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct ProcedureRun {
    pub id: String,
    pub procedure_id: String,
    pub task_id: String,
    pub outcome: ProcedureOutcome,
    pub notes: String,
    pub evidence_uri: String,
    pub created_at: Millis,
    pub deduplication_key: String,
}

#[derive(Debug, Clone, PartialEq)]
pub struct ProcedureFeedbackResult {
    pub run: ProcedureRun,
    pub memory: MemoryEntry,
    pub inserted: bool,
}

#[derive(Debug, Clone, PartialEq)]
pub struct SourceRecord {
    pub id: String,
    pub source_type: SourceType,
    pub uri: String,
    pub hash: String,
    pub display_name: String,
    pub trust_level: String,
    pub hash_status: String,
    pub ingest_count: u64,
    pub imported_at: Millis,
    pub last_ingested_at: Millis,
    pub last_checked_at: Millis,
}

impl Default for SourceRecord {
    fn default() -> Self {
        Self {
            id: String::new(),
            source_type: SourceType::Unknown,
            uri: String::new(),
            hash: String::new(),
            display_name: String::new(),
            trust_level: "working".into(),
            hash_status: "unknown".into(),
            ingest_count: 0,
            imported_at: 0,
            last_ingested_at: 0,
            last_checked_at: 0,
        }
    }
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct SourceEvent {
    pub id: String,
    pub source_id: String,
    pub action: String,
    pub status: String,
    pub source_hash: String,
    pub note: String,
    pub created_at: Millis,
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct AuditEvent {
    pub id: String,
    pub actor: String,
    pub action: String,
    pub reason: String,
    pub before_id: String,
    pub after_id: String,
    pub evidence_uri: String,
    pub metadata_json: String,
    pub created_at: Millis,
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct AuditEventWrite {
    pub actor: String,
    pub action: String,
    pub reason: String,
    pub before_id: String,
    pub after_id: String,
    pub evidence_uri: String,
    pub metadata_json: String,
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct SessionEvent {
    pub id: String,
    pub event_type: String,
    pub source: String,
    pub payload_json: String,
    pub created_at: Millis,
    pub deduplication_key: String,
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct SessionEventWrite {
    pub event_type: String,
    pub source: String,
    pub payload_json: String,
    pub deduplication_key: String,
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct SessionEventRecordResult {
    pub id: String,
    pub inserted: bool,
}

/// A pinned context block (for example `repo_rules`).
#[derive(Debug, Clone, Default, PartialEq)]
pub struct ContextBlock {
    pub id: String,
    pub content: String,
    pub updated_at: Millis,
}

/// Dreaming run counters derived from session events.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct DreamingRunStatistics {
    pub attempted: u64,
    pub completed: u64,
    pub failed: u64,
    pub pending: u64,
    pub protocol_rejections: u64,
    pub timed_out: u64,
    pub cancelled: u64,
    pub start_failures: u64,
    pub cpu_fallbacks: u64,
    pub reviews_created: u64,
    pub by_trigger: std::collections::BTreeMap<String, u64>,
    pub by_model: std::collections::BTreeMap<String, u64>,
    pub failures_by_reason: std::collections::BTreeMap<String, u64>,
}

/// Scoped statistics snapshot (qtmem-compatible totals).
#[derive(Debug, Clone, Default, PartialEq)]
pub struct MemoryStatistics {
    pub dreaming: DreamingRunStatistics,
    pub memory_counts: std::collections::BTreeMap<String, u64>,
    pub trust_states: std::collections::BTreeMap<String, u64>,
    pub memory_entries: u64,
    pub hypotheses: u64,
    pub low_confidence: u64,
    pub unprovenanced: u64,
    pub deprecated: u64,
    pub failed_procedures: u64,
    pub high_use_hypotheses: u64,
    pub conflict_count: u64,
    pub unresolved_conflicts: u64,
    pub accessed_memories: u64,
    pub total_accesses: u64,
    pub successful_outcomes: u64,
    pub failed_outcomes: u64,
    pub pending_reviews: u64,
}

/// Per-collection row limits for a bounded export (each 1..=100000).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ExportLimits {
    pub memories: usize,
    pub graph_edges: usize,
    pub procedure_runs: usize,
    pub revisions: usize,
    pub sources: usize,
    pub source_events: usize,
    pub reviews: usize,
    pub audit_events: usize,
    pub session_events: usize,
    pub blocks: usize,
}

impl Default for ExportLimits {
    fn default() -> Self {
        Self::uniform(10_000)
    }
}

impl ExportLimits {
    pub fn uniform(limit: usize) -> Self {
        Self {
            memories: limit,
            graph_edges: limit,
            procedure_runs: limit,
            revisions: limit,
            sources: limit,
            source_events: limit,
            reviews: limit,
            audit_events: limit,
            session_events: limit,
            blocks: limit,
        }
    }

    /// The limits the archived CLI uses for a pre-import backup.
    pub fn backup() -> Self {
        Self::uniform(100_000)
    }

    pub(crate) fn all(&self) -> [usize; 10] {
        [
            self.memories,
            self.graph_edges,
            self.procedure_runs,
            self.revisions,
            self.sources,
            self.source_events,
            self.reviews,
            self.audit_events,
            self.session_events,
            self.blocks,
        ]
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct RetentionPolicy {
    /// Delete rows older than this (Unix ms); `0` disables the age cutoff.
    pub older_than: Millis,
    pub keep_latest: usize,
    pub batch_limit: usize,
    pub raw_evidence: bool,
    pub canonical_memories: bool,
    pub audit_events: bool,
    pub context_blocks: bool,
    pub dry_run: bool,
}

impl Default for RetentionPolicy {
    fn default() -> Self {
        Self {
            older_than: 0,
            keep_latest: 0,
            batch_limit: 1000,
            raw_evidence: true,
            canonical_memories: false,
            audit_events: false,
            context_blocks: false,
            dry_run: true,
        }
    }
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct RetentionReport {
    pub dry_run: bool,
    pub session_events: usize,
    pub source_events: usize,
    pub orphan_sources: usize,
    pub canonical_memories: usize,
    pub audit_events: usize,
    pub context_blocks: usize,
}

impl RetentionReport {
    pub fn total(&self) -> usize {
        self.session_events
            + self.source_events
            + self.orphan_sources
            + self.canonical_memories
            + self.audit_events
            + self.context_blocks
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RedactionScrubPolicy {
    pub batch_limit: usize,
    pub dry_run: bool,
}

impl Default for RedactionScrubPolicy {
    fn default() -> Self {
        Self {
            batch_limit: 10_000,
            dry_run: true,
        }
    }
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct RedactionScrubCollection {
    pub name: String,
    pub examined: usize,
    pub changed: usize,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct RedactionScrubReport {
    pub dry_run: bool,
    pub truncated: bool,
    pub collections: Vec<RedactionScrubCollection>,
}

impl RedactionScrubReport {
    pub fn examined(&self) -> usize {
        self.collections.iter().map(|item| item.examined).sum()
    }

    pub fn changed(&self) -> usize {
        self.collections.iter().map(|item| item.changed).sum()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn enum_spellings_match_archived_models() {
        let types: Vec<_> = MemoryType::ALL.iter().map(|item| item.as_str()).collect();
        assert_eq!(
            types,
            [
                "semantic",
                "episodic",
                "procedural",
                "decision",
                "constraint",
                "bug",
                "preference",
                "skill",
                "observation",
                "todo",
                "unprocessed",
                "dream",
                "conversation",
                "tool",
                "pattern",
                "raw",
                "research"
            ]
        );
        let statuses: Vec<_> = MemoryStatus::ALL.iter().map(|item| item.as_str()).collect();
        assert_eq!(
            statuses,
            [
                "candidate",
                "hypothesis",
                "active",
                "validated",
                "deprecated",
                "superseded",
                "rejected",
                "expired",
                "dream_artifact"
            ]
        );
        let sources: Vec<_> = SourceType::ALL.iter().map(|item| item.as_str()).collect();
        assert_eq!(
            sources,
            [
                "chat",
                "file",
                "commit",
                "issue",
                "pull_request",
                "doc",
                "manual",
                "tool_result",
                "test_log",
                "processed_text",
                "dreaming",
                "unknown"
            ]
        );
        let kinds: Vec<_> = ScopeKind::ALL.iter().map(|item| item.as_str()).collect();
        assert_eq!(
            kinds,
            ["exact", "branch", "repository", "project", "global_user"]
        );
        let policies: Vec<_> = RecallPolicy::ALL.iter().map(|item| item.as_str()).collect();
        assert_eq!(policies, ["strict", "balanced", "broad"]);
        let reviews: Vec<_> = ReviewStatus::ALL.iter().map(|item| item.as_str()).collect();
        assert_eq!(reviews, ["pending", "accepted", "rejected", "deferred"]);
        let outcomes: Vec<_> = ProcedureOutcome::ALL
            .iter()
            .map(|item| item.as_str())
            .collect();
        assert_eq!(
            outcomes,
            ["success", "failure", "partial", "unused", "unknown"]
        );
    }

    #[test]
    fn parsing_accepts_aliases_and_rejects_unknowns() {
        assert_eq!(MemoryType::parse("episode").unwrap(), MemoryType::Episodic);
        assert_eq!(
            MemoryType::parse("procedure").unwrap(),
            MemoryType::Procedural
        );
        assert_eq!(MemoryType::parse("failure").unwrap(), MemoryType::Bug);
        assert_eq!(
            MemoryType::parse("nope").unwrap_err().to_string(),
            "unknown memory type: nope"
        );
        assert_eq!(
            MemoryStatus::parse("dream_artifact").unwrap(),
            MemoryStatus::DreamArtifact
        );
        assert_eq!(
            MemoryStatus::parse("live").unwrap_err().to_string(),
            "unknown memory status: live"
        );
        assert_eq!(SourceType::parse("pr"), SourceType::PullRequest);
        assert_eq!(SourceType::parse("document"), SourceType::Document);
        assert_eq!(SourceType::parse("weird"), SourceType::Unknown);
        assert_eq!(
            ScopeKind::parse("planet").unwrap_err().to_string(),
            "unknown scope kind: planet"
        );
        assert_eq!(
            ReviewStatus::parse("x").unwrap_err().to_string(),
            "unknown review status: x"
        );
        assert_eq!(ProcedureOutcome::parse("x"), ProcedureOutcome::Unknown);
        assert_eq!(
            RecallPolicy::from_setting("exploratory"),
            RecallPolicy::Broad
        );
        assert_eq!(
            RecallPolicy::from_setting("architecture"),
            RecallPolicy::Balanced
        );
    }
}
