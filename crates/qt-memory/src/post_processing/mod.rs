//! Post-processing (archived `post_processing_service.cpp`): deterministic
//! association and compression candidates, Dream artifact persistence and
//! researched Dream successors. Everything it produces is review-only: edges
//! carry low confidence and every artifact or merge proposal is a pending
//! review item.
//!
//! [`consolidation`] adds the optional model-backed semantic consolidation
//! pass, which likewise only proposes pending reviews.

pub mod consolidation;

use std::collections::BTreeSet;
use std::sync::Arc;

use serde_json::json;

use crate::error::{Result, invalid};
use crate::model::*;
use crate::review::{
    ReviewCandidatePayload, ReviewItemWrite, ReviewQuery, decode_review_candidate,
};
use crate::service::MemoryService;
use crate::store::MemoryStore;
use crate::text::dump;

pub use consolidation::{
    ConsolidationConfig, SEMANTIC_CONSOLIDATION_PROMPT, SemanticConsolidationReport,
    SemanticConsolidator,
};

/// Jaccard similarity at or above which two memories are associated.
pub const ASSOCIATION_THRESHOLD: f64 = 0.34;
/// Similarity at or above which two memories are duplicate candidates.
pub const DUPLICATE_THRESHOLD: f64 = 0.72;

/// A deterministic association between two memories.
#[derive(Debug, Clone, PartialEq)]
pub struct AssociationCandidate {
    pub from_id: String,
    pub to_id: String,
    /// `duplicates` or `related`.
    pub relation: String,
    pub similarity: f64,
}

/// A deterministic merge proposal for near-duplicates.
#[derive(Debug, Clone, PartialEq)]
pub struct CompressionCandidate {
    pub source_ids: Vec<String>,
    pub content: String,
    pub confidence: f64,
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct PostProcessingReport {
    pub associations: Vec<AssociationCandidate>,
    pub compressions: Vec<CompressionCandidate>,
    /// Compression reviews created by this pass.
    pub review_ids: Vec<String>,
}

/// A validated Dream output to persist.
#[derive(Debug, Clone, PartialEq)]
pub struct DreamArtifactRequest {
    pub cycle_id: String,
    pub artifact_kind: String,
    pub content: String,
    pub source_memory_ids: Vec<String>,
    pub model: String,
    pub source_pack_hash: String,
    pub confidence: f64,
    pub tags: Vec<String>,
}

impl Default for DreamArtifactRequest {
    fn default() -> Self {
        Self {
            cycle_id: String::new(),
            artifact_kind: "hypothesis".into(),
            content: String::new(),
            source_memory_ids: Vec::new(),
            model: String::new(),
            source_pack_hash: String::new(),
            confidence: 0.2,
            tags: Vec::new(),
        }
    }
}

/// Evidence for a researched Dream successor (manual or web research).
#[derive(Debug, Clone, PartialEq)]
pub struct DreamResearchEvidence {
    pub summary: String,
    pub source_memory_ids: Vec<String>,
    pub tags: Vec<String>,
    pub confidence: f64,
    pub provenance: Provenance,
}

impl Default for DreamResearchEvidence {
    fn default() -> Self {
        Self {
            summary: String::new(),
            source_memory_ids: Vec::new(),
            tags: Vec::new(),
            confidence: 0.7,
            provenance: Provenance::default(),
        }
    }
}

/// IDs created for a Dream artifact and, optionally, its research successor.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct DreamArtifactResult {
    /// The Dream memory.
    pub id: String,
    /// The Dream's own review (empty for a research-only result).
    pub review_id: String,
    pub researched_memory_id: Option<String>,
    pub research_review_id: Option<String>,
}

fn words(content: &str) -> BTreeSet<String> {
    let mut result = BTreeSet::new();
    let mut word = String::new();
    for byte in content.bytes() {
        if byte.is_ascii_alphanumeric() {
            word.push(byte.to_ascii_lowercase() as char);
        } else {
            if word.len() > 2 {
                result.insert(std::mem::take(&mut word));
            }
            word.clear();
        }
    }
    if word.len() > 2 {
        result.insert(word);
    }
    result
}

/// Word-set Jaccard similarity over ASCII alphanumeric words longer than two
/// characters (archived `jaccard`).
pub fn jaccard(lhs: &str, rhs: &str) -> f64 {
    let left = words(lhs);
    let right = words(rhs);
    if left.is_empty() || right.is_empty() {
        return 0.0;
    }
    let intersection = left.intersection(&right).count();
    intersection as f64 / (left.len() + right.len() - intersection) as f64
}

/// Lowercase ASCII tag component with runs of other bytes as single `-`;
/// empty input becomes `hypothesis` (archived `tag_component`).
pub fn tag_component(value: &str) -> String {
    let mut out = String::with_capacity(value.len());
    for byte in value.bytes() {
        let ch = if byte.is_ascii_alphanumeric() {
            byte.to_ascii_lowercase() as char
        } else {
            '-'
        };
        if ch == '-' && out.ends_with('-') {
            continue;
        }
        out.push(ch);
    }
    let trimmed = out.trim_matches('-');
    if trimmed.is_empty() {
        "hypothesis".into()
    } else {
        trimmed.to_string()
    }
}

/// The statuses post-processing and Dreaming read.
pub(crate) fn governed_statuses() -> Vec<MemoryStatus> {
    vec![
        MemoryStatus::Candidate,
        MemoryStatus::Hypothesis,
        MemoryStatus::Active,
        MemoryStatus::Validated,
    ]
}

fn sorted_unique(mut ids: Vec<String>) -> Vec<String> {
    ids.sort();
    ids.dedup();
    ids
}

/// Review-only post-processing over one store.
#[derive(Debug, Clone)]
pub struct PostProcessingService {
    store: Arc<MemoryStore>,
    memories: MemoryService,
}

impl PostProcessingService {
    pub fn new(store: Arc<MemoryStore>) -> Self {
        Self {
            memories: MemoryService::new(store.clone()),
            store,
        }
    }

    pub fn store(&self) -> &Arc<MemoryStore> {
        &self.store
    }

    /// Whether a pending or deferred compression review for exactly these
    /// sources already exists, so repeated passes do not flood the queue.
    fn compression_review_exists(
        &self,
        scope: &MemoryScope,
        source_ids: &[String],
    ) -> Result<bool> {
        let query = ReviewQuery {
            statuses: vec![ReviewStatus::Pending, ReviewStatus::Deferred],
            subject_id: source_ids.first().cloned(),
            kinds: vec!["compression_candidate".into(), "compression".into()],
            limit: 200,
            ..Default::default()
        };
        let wanted = sorted_unique(source_ids.to_vec());
        Ok(self.store.query_reviews(scope, &query)?.iter().any(
            |item| match decode_review_candidate(item).payload {
                ReviewCandidatePayload::Consolidation(candidate) => {
                    sorted_unique(candidate.memory_ids) == wanted
                }
                _ => false,
            },
        ))
    }

    /// Pairwise Jaccard pass over up to `limit` governed memories in exactly
    /// `scope`, examining at most `maximum_pairs` pairs. Similar pairs get a
    /// `related` or `duplicates` edge (`postprocess:deterministic`);
    /// duplicates also get a pending `compression_candidate` review.
    pub fn process(
        &self,
        scope: &MemoryScope,
        limit: usize,
        maximum_pairs: usize,
    ) -> Result<PostProcessingReport> {
        let query = MemoryQuery {
            limit,
            policy: RecallPolicy::Strict,
            include_superseded: false,
            statuses: governed_statuses(),
            ..Default::default()
        };
        let memories = self.memories.query(scope, &query)?;
        let mut report = PostProcessingReport::default();
        let mut examined = 0usize;
        'outer: for (i, left) in memories.iter().enumerate() {
            for right in memories.iter().skip(i + 1) {
                if examined >= maximum_pairs {
                    break 'outer;
                }
                examined += 1;
                let similarity = jaccard(&left.content, &right.content);
                if similarity < ASSOCIATION_THRESHOLD {
                    continue;
                }
                let duplicate = similarity >= DUPLICATE_THRESHOLD;
                let relation = if duplicate { "duplicates" } else { "related" };
                report.associations.push(AssociationCandidate {
                    from_id: left.id.clone(),
                    to_id: right.id.clone(),
                    relation: relation.into(),
                    similarity,
                });
                let mut edge = GraphEdge::new(&left.id, &right.id, relation);
                edge.confidence = similarity;
                edge.source = "postprocess:deterministic".into();
                self.store.add_edge(scope, edge)?;
                if !duplicate {
                    continue;
                }
                let compression = CompressionCandidate {
                    source_ids: vec![left.id.clone(), right.id.clone()],
                    content: if left.content.len() <= right.content.len() {
                        left.content.clone()
                    } else {
                        right.content.clone()
                    },
                    confidence: similarity,
                };
                if !self.compression_review_exists(scope, &compression.source_ids)? {
                    let review = ReviewItemWrite {
                        kind: "compression_candidate".into(),
                        reason: "deterministic duplicate similarity exceeded threshold".into(),
                        subject_id: left.id.clone(),
                        candidate_json: dump(&json!({
                            "source_ids": compression.source_ids,
                            "content": compression.content,
                            "confidence": compression.confidence,
                        })),
                        tags: vec!["postprocess:compression".into(), "review:required".into()],
                        ..Default::default()
                    };
                    report
                        .review_ids
                        .push(self.store.add_review(scope, review)?);
                }
                report.compressions.push(compression);
            }
        }
        Ok(report)
    }

    /// Stores a Dream artifact (`dream` / `dream_artifact`, confidence capped
    /// at 0.49), its pending `dream_artifact` review and `derived_from` edges
    /// to every source.
    pub fn remember_dream_artifact(
        &self,
        scope: &MemoryScope,
        mut request: DreamArtifactRequest,
    ) -> Result<DreamArtifactResult> {
        if !request.confidence.is_finite() {
            return invalid("dream artifact confidence must be finite");
        }
        if request.source_memory_ids.len() < 2 {
            return invalid("dream artifact requires at least two source memories");
        }
        request.source_memory_ids = sorted_unique(request.source_memory_ids);
        if request.source_memory_ids.len() < 2 {
            return invalid("dream artifact requires two distinct source memories");
        }
        for source_id in &request.source_memory_ids {
            self.memories.get(scope, source_id)?;
        }
        let uri = format!("memory://dreaming/{}", request.cycle_id);
        let mut tags = vec![
            "dream:artifact".to_string(),
            "review:dream".into(),
            "review:required".into(),
            "trusted:false".into(),
            format!("dream:kind:{}", tag_component(&request.artifact_kind)),
        ];
        if !request.cycle_id.is_empty() {
            tags.push(format!("dream:cycle:{}", request.cycle_id));
        }
        tags.extend(request.tags.iter().filter(|tag| !tag.is_empty()).cloned());
        let write = MemoryWrite {
            memory_type: MemoryType::Dream,
            status: MemoryStatus::DreamArtifact,
            source: "postprocess:dream-cycle".into(),
            provenance: Provenance {
                source_type: SourceType::Dreaming,
                source_uri: uri.clone(),
                source_hash: request.source_pack_hash.clone(),
                created_by: "memory_dreaming_model".into(),
                evidence_strength: 0.2,
                source_span: dump(&json!(request.source_memory_ids)),
                ..Default::default()
            },
            content: request.content.clone(),
            confidence: request.confidence.clamp(0.0, 0.49),
            tags,
            ..Default::default()
        };
        let id = self.memories.remember(scope, write)?;
        let review = ReviewItemWrite {
            kind: "dream_artifact".into(),
            reason: "dream output is isolated from trusted recall".into(),
            subject_id: id.clone(),
            provenance: Provenance {
                source_type: SourceType::Dreaming,
                source_uri: uri,
                source_hash: request.source_pack_hash.clone(),
                created_by: "memory_dreaming_parent".into(),
                evidence_strength: 0.2,
                ..Default::default()
            },
            candidate_json: dump(&json!({
                "memory_id": id,
                "source_ids": request.source_memory_ids,
                "artifact_kind": request.artifact_kind,
                "model": request.model,
                "cycle_id": request.cycle_id,
                "source_pack_hash": request.source_pack_hash,
            })),
            tags: vec![
                "review:dream".into(),
                "review:required".into(),
                "model:untrusted".into(),
            ],
            ..Default::default()
        };
        let review_id = self.store.add_review(scope, review)?;
        for source_id in &request.source_memory_ids {
            let mut edge = GraphEdge::new(&id, source_id, "derived_from");
            edge.confidence = 0.55;
            edge.source = "postprocess:dream-cycle".into();
            self.store.add_edge(scope, edge)?;
        }
        Ok(DreamArtifactResult {
            id,
            review_id,
            researched_memory_id: None,
            research_review_id: None,
        })
    }

    /// Stores supplied research as a separate `research` candidate linked to
    /// a pending Dream artifact (`supports` and `follows` edges, plus
    /// `derived_from` edges to cited memories) with its own pending
    /// `dream_research_successor` review. The Dream itself is not changed.
    pub fn remember_researched_successor(
        &self,
        scope: &MemoryScope,
        dream_id: &str,
        mut evidence: DreamResearchEvidence,
    ) -> Result<DreamArtifactResult> {
        let dream = self.memories.get(scope, dream_id)?;
        if dream.memory_type != MemoryType::Dream || dream.status != MemoryStatus::DreamArtifact {
            return invalid("researched successor requires a pending dream artifact");
        }
        if evidence.summary.is_empty() {
            return invalid("researched successor requires a summary");
        }
        if !evidence.confidence.is_finite() || !evidence.provenance.evidence_strength.is_finite() {
            return invalid("researched successor confidence values must be finite");
        }
        if evidence.source_memory_ids.is_empty()
            && evidence.provenance.source_uri.is_empty()
            && evidence.provenance.source_hash.is_empty()
        {
            return invalid("researched successor requires supplied evidence provenance");
        }
        evidence.source_memory_ids = sorted_unique(evidence.source_memory_ids);
        for source_id in &evidence.source_memory_ids {
            if source_id == dream_id {
                return invalid("dream cannot be its own research source");
            }
            self.memories.get(scope, source_id)?;
        }
        let mut provenance = evidence.provenance;
        if matches!(
            provenance.source_type,
            SourceType::Unknown | SourceType::Dreaming
        ) {
            provenance.source_type = SourceType::ProcessedText;
        }
        if provenance.source_uri.is_empty() {
            provenance.source_uri = format!("memory://dream-research/{dream_id}");
        }
        if provenance.created_by.is_empty() || provenance.created_by == "agent" {
            provenance.created_by = "supplied_research_evidence".into();
        }
        provenance.evidence_strength = provenance.evidence_strength.clamp(0.5, 1.0);
        let mut span = json!({"source_memory_ids": evidence.source_memory_ids});
        if !provenance.source_span.is_empty() {
            span["supplied_span"] = json!(provenance.source_span);
        }
        provenance.source_span = dump(&span);
        let mut tags = vec![
            "research".to_string(),
            "research:supplied".into(),
            "research:pending-validation".into(),
            "dream:researched".into(),
            format!("dream-source:{dream_id}"),
            "review:required".into(),
        ];
        tags.extend(
            evidence
                .source_memory_ids
                .iter()
                .map(|id| format!("research-source:{id}")),
        );
        tags.extend(evidence.tags.into_iter().filter(|tag| !tag.is_empty()));
        let write = MemoryWrite {
            memory_type: MemoryType::Research,
            status: MemoryStatus::Candidate,
            source: "postprocess:dream-research".into(),
            provenance: provenance.clone(),
            content: evidence.summary,
            confidence: evidence.confidence.clamp(0.0, 0.9),
            tags,
            ..Default::default()
        };
        let research_id = self.memories.remember(scope, write)?;
        let mut review_provenance = provenance;
        review_provenance.created_by = "memory_dreaming_parent".into();
        let review = ReviewItemWrite {
            kind: "dream_research_successor".into(),
            reason: "supplied research evidence requires review before lifecycle promotion".into(),
            subject_id: research_id.clone(),
            provenance: review_provenance,
            candidate_json: dump(&json!({
                "memory_id": research_id,
                "dream_id": dream_id,
                "source_ids": evidence.source_memory_ids,
            })),
            tags: vec!["review:dream-research".into(), "review:required".into()],
            ..Default::default()
        };
        let research_review_id = self.store.add_review(scope, review)?;
        for relation in ["supports", "follows"] {
            let mut edge = GraphEdge::new(&research_id, dream_id, relation);
            edge.confidence = 0.7;
            edge.source = "postprocess:dream-research".into();
            self.store.add_edge(scope, edge)?;
        }
        for source_id in &evidence.source_memory_ids {
            let mut edge = GraphEdge::new(&research_id, source_id, "derived_from");
            edge.confidence = 0.8;
            edge.source = "postprocess:dream-research".into();
            self.store.add_edge(scope, edge)?;
        }
        Ok(DreamArtifactResult {
            id: dream_id.to_string(),
            review_id: String::new(),
            researched_memory_id: Some(research_id),
            research_review_id: Some(research_review_id),
        })
    }
}

#[cfg(test)]
pub(crate) mod test_support {
    use super::*;
    use crate::store::DatabaseOptions;

    pub(crate) fn store() -> (tempfile::TempDir, Arc<MemoryStore>) {
        let dir = tempfile::tempdir().unwrap();
        let store = MemoryStore::open_with_options(dir.path(), DatabaseOptions::default()).unwrap();
        (dir, Arc::new(store))
    }

    pub(crate) fn scope() -> MemoryScope {
        MemoryScope {
            kind: ScopeKind::Repository,
            user: "local_user".into(),
            project: "demo".into(),
            repository: "repo_0123456789abcdef".into(),
            repository_display: "demo".into(),
            ..Default::default()
        }
    }

    pub(crate) fn remember(store: &Arc<MemoryStore>, scope: &MemoryScope, content: &str) -> String {
        MemoryService::new(store.clone())
            .remember(
                scope,
                MemoryWrite {
                    memory_type: MemoryType::Semantic,
                    status: MemoryStatus::Candidate,
                    content: content.into(),
                    provenance: Provenance {
                        source_type: SourceType::Manual,
                        source_uri: "test://memory".into(),
                        ..Default::default()
                    },
                    confidence: 0.6,
                    ..Default::default()
                },
            )
            .unwrap()
    }
}

#[cfg(test)]
mod tests {
    use super::test_support::*;
    use super::*;

    #[test]
    fn jaccard_and_tag_component_match_archive() {
        assert_eq!(jaccard("", "abc"), 0.0);
        assert!((jaccard("The cat sat on mats", "the CAT sat on hats") - 0.6).abs() < 1e-9);
        assert_eq!(tag_component("Pattern Discovery!!"), "pattern-discovery");
        assert_eq!(tag_component("--"), "hypothesis");
        assert_eq!(tag_component("procedure_revision"), "procedure-revision");
        assert_eq!(tag_component("héllo"), "h-llo");
    }

    #[test]
    fn deterministic_pass_links_related_and_proposes_duplicates_once() {
        let (_dir, store) = store();
        let scope = scope();
        let a = remember(
            &store,
            &scope,
            "build uses cmake ninja release profile flags",
        );
        let b = remember(
            &store,
            &scope,
            "build uses cmake ninja release profile options",
        );
        let c = remember(
            &store,
            &scope,
            "unrelated note about coffee brewing temperature",
        );
        let service = PostProcessingService::new(store.clone());
        let report = service.process(&scope, 500, 100).unwrap();
        assert_eq!(report.associations.len(), 1);
        let association = &report.associations[0];
        assert_eq!(association.relation, "duplicates");
        assert!(association.similarity >= DUPLICATE_THRESHOLD);
        assert_eq!(report.compressions.len(), 1);
        assert_eq!(report.review_ids.len(), 1);
        let review = store.review(&scope, &report.review_ids[0]).unwrap();
        assert_eq!(review.kind, "compression_candidate");
        assert_eq!(review.status, ReviewStatus::Pending);
        assert_eq!(review.tags, ["postprocess:compression", "review:required"]);
        let edges = store
            .query_edges(
                &scope,
                &GraphQuery {
                    memory_id: a.clone(),
                    ..Default::default()
                },
            )
            .unwrap();
        assert_eq!(edges.len(), 1);
        assert_eq!(edges[0].source, "postprocess:deterministic");
        assert!([&a, &b].contains(&&edges[0].to_id) && [&a, &b].contains(&&edges[0].from_id));
        assert!(
            !edges
                .iter()
                .any(|edge| edge.to_id == c || edge.from_id == c)
        );

        let again = service.process(&scope, 500, 100).unwrap();
        assert_eq!(again.compressions.len(), 1);
        assert!(
            again.review_ids.is_empty(),
            "duplicate proposal is not re-queued"
        );

        let limited = service.process(&scope, 500, 0).unwrap();
        assert!(limited.associations.is_empty());
    }

    #[test]
    fn dream_artifact_is_isolated_pending_review_with_edges() {
        let (_dir, store) = store();
        let scope = scope();
        let a = remember(&store, &scope, "alpha uses retries");
        let b = remember(&store, &scope, "beta uses retries too");
        let service = PostProcessingService::new(store.clone());
        assert_eq!(
            service
                .remember_dream_artifact(
                    &scope,
                    DreamArtifactRequest {
                        source_memory_ids: vec![a.clone(), a.clone()],
                        content: "x".into(),
                        ..Default::default()
                    }
                )
                .unwrap_err()
                .to_string(),
            "dream artifact requires two distinct source memories"
        );
        assert_eq!(
            service
                .remember_dream_artifact(
                    &scope,
                    DreamArtifactRequest {
                        source_memory_ids: vec![a.clone()],
                        ..Default::default()
                    }
                )
                .unwrap_err()
                .to_string(),
            "dream artifact requires at least two source memories"
        );
        let result = service
            .remember_dream_artifact(
                &scope,
                DreamArtifactRequest {
                    cycle_id: "abcdef0123456789".into(),
                    artifact_kind: "pattern_discovery".into(),
                    content: "Both services share a retry pattern".into(),
                    source_memory_ids: vec![b.clone(), a.clone()],
                    model: "llama/test".into(),
                    source_pack_hash: "pack".into(),
                    confidence: 0.9,
                    tags: vec!["research:pending".into(), String::new()],
                },
            )
            .unwrap();
        let dream = store.get(&scope, &result.id).unwrap();
        assert_eq!(dream.memory_type, MemoryType::Dream);
        assert_eq!(dream.status, MemoryStatus::DreamArtifact);
        assert_eq!(dream.confidence, 0.49);
        assert_eq!(
            dream.provenance.source_uri,
            "memory://dreaming/abcdef0123456789"
        );
        assert_eq!(dream.provenance.created_by, "memory_dreaming_model");
        let mut expected_span = vec![a.clone(), b.clone()];
        expected_span.sort();
        assert_eq!(dream.provenance.source_span, dump(&json!(expected_span)));
        for tag in [
            "dream:artifact",
            "trusted:false",
            "dream:kind:pattern-discovery",
            "dream:cycle:abcdef0123456789",
            "research:pending",
        ] {
            assert!(dream.tags.iter().any(|item| item == tag), "{tag}");
        }
        let review = store.review(&scope, &result.review_id).unwrap();
        assert_eq!(review.kind, "dream_artifact");
        assert_eq!(review.subject_id, result.id);
        assert_eq!(review.provenance.created_by, "memory_dreaming_parent");
        let edges = store
            .query_edges(
                &scope,
                &GraphQuery {
                    from_id: result.id.clone(),
                    edge_type: "derived_from".into(),
                    ..Default::default()
                },
            )
            .unwrap();
        assert_eq!(edges.len(), 2);
        assert!(edges.iter().all(|edge| edge.confidence == 0.55));
    }

    #[test]
    fn research_successor_is_separately_reviewed_and_linked() {
        let (_dir, store) = store();
        let scope = scope();
        let a = remember(&store, &scope, "alpha uses retries");
        let b = remember(&store, &scope, "beta uses retries too");
        let service = PostProcessingService::new(store.clone());
        let dream = service
            .remember_dream_artifact(
                &scope,
                DreamArtifactRequest {
                    content: "retry pattern".into(),
                    source_memory_ids: vec![a.clone(), b.clone()],
                    ..Default::default()
                },
            )
            .unwrap();
        let error = |evidence: DreamResearchEvidence, id: &str| {
            service
                .remember_researched_successor(&scope, id, evidence)
                .unwrap_err()
                .to_string()
        };
        assert_eq!(
            error(DreamResearchEvidence::default(), &a),
            "researched successor requires a pending dream artifact"
        );
        assert_eq!(
            error(DreamResearchEvidence::default(), &dream.id),
            "researched successor requires a summary"
        );
        assert_eq!(
            error(
                DreamResearchEvidence {
                    summary: "s".into(),
                    ..Default::default()
                },
                &dream.id
            ),
            "researched successor requires supplied evidence provenance"
        );
        assert_eq!(
            error(
                DreamResearchEvidence {
                    summary: "s".into(),
                    source_memory_ids: vec![dream.id.clone()],
                    ..Default::default()
                },
                &dream.id
            ),
            "dream cannot be its own research source"
        );
        let result = service
            .remember_researched_successor(
                &scope,
                &dream.id,
                DreamResearchEvidence {
                    summary: "Public docs confirm the retry pattern".into(),
                    source_memory_ids: vec![b.clone(), a.clone(), a.clone()],
                    tags: vec!["research:web".into()],
                    confidence: 0.95,
                    provenance: Provenance {
                        source_type: SourceType::Dreaming,
                        source_uri: "https://example.com/doc".into(),
                        source_span: "{\"citations\":[]}".into(),
                        evidence_strength: 0.1,
                        ..Default::default()
                    },
                },
            )
            .unwrap();
        assert_eq!(result.id, dream.id);
        let research_id = result.researched_memory_id.unwrap();
        let research = store.get(&scope, &research_id).unwrap();
        assert_eq!(research.memory_type, MemoryType::Research);
        assert_eq!(research.status, MemoryStatus::Candidate);
        assert_eq!(research.confidence, 0.9);
        assert_eq!(research.provenance.source_type, SourceType::ProcessedText);
        assert_eq!(research.provenance.created_by, "supplied_research_evidence");
        assert_eq!(research.provenance.evidence_strength, 0.5);
        let span: serde_json::Value =
            serde_json::from_str(&research.provenance.source_span).unwrap();
        assert_eq!(span["supplied_span"], "{\"citations\":[]}");
        assert_eq!(span["source_memory_ids"].as_array().unwrap().len(), 2);
        assert!(
            research
                .tags
                .iter()
                .any(|tag| tag == "research:pending-validation")
        );
        assert!(
            research
                .tags
                .iter()
                .any(|tag| tag == &format!("dream-source:{}", dream.id))
        );
        let review = store
            .review(&scope, &result.research_review_id.unwrap())
            .unwrap();
        assert_eq!(review.kind, "dream_research_successor");
        assert_eq!(review.provenance.created_by, "memory_dreaming_parent");
        let dream_after = store.get(&scope, &dream.id).unwrap();
        assert_eq!(dream_after.status, MemoryStatus::DreamArtifact);
        let edges = store
            .query_edges(
                &scope,
                &GraphQuery {
                    from_id: research_id,
                    ..Default::default()
                },
            )
            .unwrap();
        let mut types: Vec<_> = edges.iter().map(|edge| edge.edge_type.as_str()).collect();
        types.sort();
        assert_eq!(
            types,
            ["derived_from", "derived_from", "follows", "supports"]
        );
    }
}
