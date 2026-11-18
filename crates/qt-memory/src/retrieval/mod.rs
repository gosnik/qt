//! Hybrid retrieval (archived `retrieval_models.hpp`, `retrieval_graph`,
//! `retrieval_indexer`, `dense_retrieval`, `retrieval_engine` and
//! `retrieval_explanation`).
//!
//! Identifiers from different stores never share an implicit namespace: every
//! owner is a typed [`RetrievalReference`]. Derived representations (chunks,
//! vectors, code windows) are transient evidence and never canonical memory;
//! lifecycle and authority remain owned by the governed memory row.

pub mod dense;
pub mod engine;
pub mod explanation;
pub mod graph;
pub mod indexer;

use crate::model::{MemoryScope, MemoryStatus, MemoryType, Millis, Provenance};

pub use dense::{DenseRetrievalService, DenseRetrievalStats, Embedder, LocalModelEmbedder};
pub use engine::{
    RetrievalCodeStatus, RetrievalEngine, RetrievalEngineOptions, RetrievalEngineStatus,
};
pub use explanation::{context_preview_json, recalled_memory_json, retrieval_candidate_json};
pub use graph::{
    RetrievalGraphAdapter, RetrievalGraphDirection, RetrievalGraphEdge, RetrievalGraphIdentity,
    RetrievalGraphQuery, RetrievalGraphResult, RetrievalGraphSeed,
};
pub use indexer::{RetrievalIndexStep, RetrievalIndexer};

/// Cooperative cancellation probe; `true` stops the operation.
pub type Cancellation<'a> = &'a dyn Fn() -> bool;

/// A cancellation probe that never cancels.
pub fn never_cancelled() -> bool {
    false
}

/// Which store a retrieval owner belongs to.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default, PartialOrd, Ord)]
pub enum RetrievalOwnerKind {
    #[default]
    Memory,
    Code,
    Entity,
}

impl RetrievalOwnerKind {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Memory => "memory",
            Self::Code => "code",
            Self::Entity => "entity",
        }
    }

    /// Persisted spelling; unknown values read as `Memory` (archived reader).
    pub fn parse(value: &str) -> Self {
        match value {
            "code" => Self::Code,
            "entity" => Self::Entity,
            _ => Self::Memory,
        }
    }

    /// Ordinal used in archived traversal keys (`"<kind>:<id>"`).
    pub(crate) fn ordinal(self) -> u8 {
        match self {
            Self::Memory => 0,
            Self::Code => 1,
            Self::Entity => 2,
        }
    }
}

/// A typed, optionally revision- and snapshot-pinned owner.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct RetrievalReference {
    pub kind: RetrievalOwnerKind,
    pub id: String,
    pub revision: i64,
    pub repository: String,
    pub worktree: String,
    pub path: String,
    pub source_fingerprint: String,
    pub snapshot_generation: u64,
}

impl RetrievalReference {
    pub fn new(kind: RetrievalOwnerKind, id: impl Into<String>) -> Self {
        Self {
            kind,
            id: id.into(),
            ..Default::default()
        }
    }

    pub fn memory(id: impl Into<String>, revision: i64) -> Self {
        Self {
            revision,
            ..Self::new(RetrievalOwnerKind::Memory, id)
        }
    }

    /// Traversal identity: kind and id, ignoring pins.
    pub(crate) fn key(&self) -> String {
        format!("{}:{}", self.kind.ordinal(), self.id)
    }
}

/// A named entity referenced by structured assertions.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct MemoryEntity {
    pub id: String,
    pub kind: String,
    pub name: String,
    pub aliases: Vec<String>,
    pub scope: MemoryScope,
}

/// Evidence locator attached to an assertion.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct AssertionEvidence {
    pub source_id: String,
    pub source_revision: String,
    pub source_uri: String,
    pub source_hash: String,
    pub source_span: String,
    pub created_by: String,
}

/// A structured claim attached to an immutable revision of a governed memory.
/// Lifecycle and authority remain owned by that memory, never by an index.
#[derive(Debug, Clone, PartialEq)]
pub struct MemoryAssertion {
    pub memory_id: String,
    pub revision: i64,
    pub subject_id: String,
    pub predicate: String,
    pub object_id: String,
    pub literal_json: String,
    pub qualifiers_json: String,
    pub positive: bool,
    pub valid_from: Option<Millis>,
    pub valid_to: Option<Millis>,
    pub evidence: Vec<AssertionEvidence>,
    /// Knowledge time of this revision.
    pub recorded_at: Millis,
    /// The memory snapshot captured with the revision; historical queries use
    /// these values rather than the current governed row.
    pub historical_content: String,
    pub historical_source: String,
    pub historical_type: MemoryType,
    pub historical_status: MemoryStatus,
    pub historical_provenance: Provenance,
    pub historical_created_at: Millis,
    pub historical_updated_at: Millis,
}

impl Default for MemoryAssertion {
    fn default() -> Self {
        Self {
            memory_id: String::new(),
            revision: 0,
            subject_id: String::new(),
            predicate: String::new(),
            object_id: String::new(),
            literal_json: "null".into(),
            qualifiers_json: "{}".into(),
            positive: true,
            valid_from: None,
            valid_to: None,
            evidence: Vec::new(),
            recorded_at: 0,
            historical_content: String::new(),
            historical_source: String::new(),
            historical_type: MemoryType::Observation,
            historical_status: MemoryStatus::Hypothesis,
            historical_provenance: Provenance::default(),
            historical_created_at: 0,
            historical_updated_at: 0,
        }
    }
}

impl MemoryAssertion {
    /// Whether the assertion holds at `at` (Unix ms).
    pub fn valid_at(&self, at: Millis) -> bool {
        self.valid_from.is_none_or(|from| at >= from) && self.valid_to.is_none_or(|to| at < to)
    }

    /// `assertion:<memory>:<revision>` evidence identifier.
    pub fn evidence_id(&self) -> String {
        format!("assertion:{}:{}", self.memory_id, self.revision)
    }
}

/// One leased unit of indexing work.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct RetrievalIndexJob {
    pub id: i64,
    pub owner_key: String,
    pub owner_revision: i64,
    pub representation: String,
    pub generation: String,
    pub lease_token: String,
    pub attempts: usize,
    pub content: String,
    pub source_uri: String,
}

/// Job counters for one scope.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct RetrievalIndexStatus {
    pub pending: usize,
    pub leased: usize,
    pub failed: usize,
    pub complete: usize,
    pub chunks: usize,
}

impl RetrievalIndexStatus {
    pub(crate) fn add(&mut self, other: &Self) {
        self.pending += other.pending;
        self.leased += other.leased;
        self.failed += other.failed;
        self.complete += other.complete;
        self.chunks += other.chunks;
    }
}

/// The embedding-space contract of a dense generation.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct DenseIndexConfig {
    pub generation: String,
    pub model_id: String,
    pub model_fingerprint: String,
    pub tokenizer_fingerprint: String,
    pub query_prefix: String,
    pub document_prefix: String,
    pub pooling: String,
    pub dimensions: usize,
    pub normalize: bool,
}

/// One vector for one published chunk.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct DenseChunkVector {
    pub chunk_id: String,
    pub values: Vec<f32>,
}

/// Captured representation evidence; transient, never canonical memory.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RetrievalChunk {
    pub id: String,
    pub owner_key: String,
    pub owner_revision: i64,
    pub ordinal: usize,
    pub text: String,
    pub content_hash: String,
    pub source_uri: String,
    pub source_span: String,
    pub render_version: String,
}

impl Default for RetrievalChunk {
    fn default() -> Self {
        Self {
            id: String::new(),
            owner_key: String::new(),
            owner_revision: 0,
            ordinal: 0,
            text: String::new(),
            content_hash: String::new(),
            source_uri: String::new(),
            source_span: String::new(),
            render_version: "text-v1".into(),
        }
    }
}

/// A dense similarity match pinned to its chunk and index generation.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct DenseSearchHit {
    pub chunk: RetrievalChunk,
    pub similarity: f64,
    pub generation: String,
}

/// A memory row resolved from a dense hit, with the hit kept as evidence.
#[derive(Debug, Clone, PartialEq)]
pub struct DenseMemoryCandidate {
    pub memory: crate::model::MemoryEntry,
    pub hit: DenseSearchHit,
}

/// A directed hop of a typed evidence path.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct RetrievalPathEdge {
    pub from: RetrievalReference,
    pub to: RetrievalReference,
    pub relation: String,
    pub evidence_id: String,
    pub resolution: String,
    pub assertion: Option<MemoryAssertion>,
    pub heuristic: bool,
}

impl RetrievalPathEdge {
    /// Archived `retrieval_relation_provenance`.
    pub fn provenance(&self) -> &'static str {
        if self.assertion.is_some() {
            return "stored assertion";
        }
        if self.from.kind == RetrievalOwnerKind::Code
            && self.to.kind == RetrievalOwnerKind::Code
            && self.evidence_id.starts_with("code-snapshot:")
        {
            return if self.heuristic {
                "heuristic code edge"
            } else {
                "code index relation"
            };
        }
        "stored relation"
    }
}

/// One retriever's rank for a candidate.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct RetrievalRank {
    pub retriever: String,
    pub rank: usize,
    pub score: f64,
}

/// A typed memory/code/entity candidate with its supporting evidence.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct RetrievalCandidate {
    pub owner: RetrievalReference,
    /// Canonical rendered representation used for context injection.
    pub chunks: Vec<RetrievalChunk>,
    /// Transient supporting evidence (for example dense code hits).
    pub matched_chunks: Vec<DenseSearchHit>,
    pub ranks: Vec<RetrievalRank>,
    pub path: Vec<RetrievalPathEdge>,
    pub evidence_ids: Vec<String>,
    pub eligible: bool,
    pub exclusion_reasons: Vec<String>,
    pub fused_score: f64,
}

/// A persisted relation between owners of different stores.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct CrossStoreLink {
    pub from: RetrievalReference,
    pub to: RetrievalReference,
    pub relation: String,
    pub evidence_id: String,
    pub id: String,
}

/// Counters reported by a retrieval rebuild.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct RetrievalMaintenanceReport {
    pub owners: usize,
    pub chunks_invalidated: usize,
    pub vectors_invalidated: usize,
    pub jobs_superseded: usize,
    pub jobs_enqueued: usize,
}
