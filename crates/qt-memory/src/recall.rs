//! Governed recall: lexical (FTS5 + hashed-token similarity) with exact score
//! weights, structured assertions, bounded graph traversal, dense candidates
//! and reciprocal-rank fusion (archived `recall_service.cpp`).

use std::collections::{BTreeMap, HashMap, HashSet, VecDeque};
use std::sync::Arc;

use crate::error::Result;
use crate::model::*;
use crate::retrieval::{DenseMemoryCandidate, DenseSearchHit, MemoryAssertion};
use crate::scope::{scope_can_access, scope_key, scope_score};
use crate::service::GovernanceService;
use crate::store::MemoryStore;

/// Recall score breakdown.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct RecallScore {
    pub total: f64,
    pub lexical: f64,
    pub hashed_similarity: f64,
    pub confidence: f64,
    pub quality: f64,
    pub freshness: f64,
    pub scope: f64,
    pub lexical_rank: usize,
    pub graph_rank: usize,
    pub dense_rank: usize,
    pub reciprocal_rank_fusion: f64,
}

/// One hop of an assertion evidence path.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct RecalledAssertionStep {
    pub memory_id: String,
    pub revision: i64,
    pub from_entity_id: String,
    pub to_entity_id: String,
    pub reverse: bool,
}

/// A recalled memory with its score, warnings and evidence paths.
#[derive(Debug, Clone, PartialEq)]
pub struct RecalledMemory {
    pub memory: MemoryEntry,
    pub score: RecallScore,
    pub warnings: Vec<String>,
    pub eligible_for_context: bool,
    pub graph_path_ids: Vec<String>,
    pub graph_path_relations: Vec<String>,
    pub graph_path_reverse: Vec<bool>,
    pub connecting_memory: bool,
    pub historical: bool,
    pub historical_revision: i64,
    pub historical_recorded_at: Option<Millis>,
    pub historical_as_of: Option<Millis>,
    pub assertion_path: Vec<RecalledAssertionStep>,
    pub dense_match: Option<DenseSearchHit>,
}

impl RecalledMemory {
    pub fn new(memory: MemoryEntry) -> Self {
        Self {
            memory,
            score: RecallScore::default(),
            warnings: Vec::new(),
            eligible_for_context: true,
            graph_path_ids: Vec::new(),
            graph_path_relations: Vec::new(),
            graph_path_reverse: Vec::new(),
            connecting_memory: false,
            historical: false,
            historical_revision: 0,
            historical_recorded_at: None,
            historical_as_of: None,
            assertion_path: Vec::new(),
            dense_match: None,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum GraphDirection {
    Outgoing,
    Incoming,
    #[default]
    Both,
}

#[derive(Debug, Clone, PartialEq)]
pub struct GraphRecallOptions {
    pub enabled: bool,
    pub seed_memory_ids: Vec<String>,
    pub max_depth: usize,
    pub max_fanout: usize,
    pub max_seeds: usize,
    pub max_edges: usize,
    pub max_results: usize,
    pub direction: GraphDirection,
    pub allowed_relation_types: Vec<String>,
    pub include_connecting_memories: bool,
}

impl Default for GraphRecallOptions {
    fn default() -> Self {
        Self {
            enabled: false,
            seed_memory_ids: Vec::new(),
            max_depth: 2,
            max_fanout: 16,
            max_seeds: 16,
            max_edges: 256,
            max_results: 64,
            direction: GraphDirection::Both,
            allowed_relation_types: Vec::new(),
            include_connecting_memories: true,
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct StructuredRecallOptions {
    pub enabled: bool,
    pub subject: String,
    pub predicate: String,
    pub object: String,
    /// Unix ms; a historical answer disables graph and dense recall.
    pub as_of: Option<Millis>,
    pub limit: usize,
}

impl Default for StructuredRecallOptions {
    fn default() -> Self {
        Self {
            enabled: false,
            subject: String::new(),
            predicate: String::new(),
            object: String::new(),
            as_of: None,
            limit: 32,
        }
    }
}

/// Cancellation probe shared with background providers.
pub type CancelFn = Arc<dyn Fn() -> bool + Send + Sync>;

#[derive(Clone)]
pub struct RecallRequest {
    pub query: String,
    pub tags: Vec<String>,
    pub policy: RecallPolicy,
    pub include_global_user: bool,
    pub include_candidates: bool,
    pub candidate_limit: usize,
    pub result_limit: usize,
    pub graph: GraphRecallOptions,
    pub structured: StructuredRecallOptions,
    pub dense_enabled: bool,
    pub cancelled: Option<CancelFn>,
}

impl Default for RecallRequest {
    fn default() -> Self {
        Self {
            query: String::new(),
            tags: Vec::new(),
            policy: RecallPolicy::Balanced,
            include_global_user: false,
            include_candidates: true,
            candidate_limit: 50,
            result_limit: 20,
            graph: GraphRecallOptions::default(),
            structured: StructuredRecallOptions::default(),
            dense_enabled: false,
            cancelled: None,
        }
    }
}

impl std::fmt::Debug for RecallRequest {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("RecallRequest")
            .field("query", &self.query)
            .field("policy", &self.policy)
            .field("candidate_limit", &self.candidate_limit)
            .field("result_limit", &self.result_limit)
            .finish_non_exhaustive()
    }
}

impl RecallRequest {
    pub fn is_cancelled(&self) -> bool {
        self.cancelled.as_ref().is_some_and(|cancelled| cancelled())
    }
}

/// Supplies dense memory candidates for a request.
pub type DenseCandidateProvider =
    Arc<dyn Fn(&MemoryScope, &RecallRequest) -> Vec<DenseMemoryCandidate> + Send + Sync>;

/// Recall over one store (memories, graph, sources, reviews and assertions).
#[derive(Clone)]
pub struct RecallService {
    store: Arc<MemoryStore>,
    dense: Option<DenseCandidateProvider>,
}

impl std::fmt::Debug for RecallService {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("RecallService").finish_non_exhaustive()
    }
}

/// Lowercased ASCII `[A-Za-z0-9_-]` tokens longer than one byte, sorted and
/// deduplicated.
pub(crate) fn tokens(text: &str) -> Vec<String> {
    let mut result: Vec<String> = text
        .split(|ch: char| !(ch.is_ascii_alphanumeric() || ch == '_' || ch == '-'))
        .filter(|token| token.len() > 1)
        .map(str::to_ascii_lowercase)
        .collect();
    result.sort();
    result.dedup();
    result
}

/// FNV-1a with the archived recall constant (note: not the standard offset).
fn recall_hash(value: &str) -> u64 {
    let mut result: u64 = 1_469_598_103_934_665_603;
    for byte in value.bytes() {
        result ^= u64::from(byte);
        result = result.wrapping_mul(1_099_511_628_211);
    }
    result
}

/// Signed 256-bucket token-hash cosine similarity, clamped at zero.
pub fn hashed_similarity(lhs: &str, rhs: &str) -> f64 {
    const DIMENSIONS: usize = 256;
    let vector = |text: &str| {
        let mut values = [0.0f64; DIMENSIONS];
        for token in tokens(text) {
            let hash = recall_hash(&token);
            values[(hash % DIMENSIONS as u64) as usize] +=
                if (hash >> 8) & 1 == 0 { 1.0 } else { -1.0 };
        }
        values
    };
    let left = vector(lhs);
    let right = vector(rhs);
    let mut dot = 0.0;
    let mut left_norm = 0.0;
    let mut right_norm = 0.0;
    for index in 0..DIMENSIONS {
        dot += left[index] * right[index];
        left_norm += left[index] * left[index];
        right_norm += right[index] * right[index];
    }
    if left_norm == 0.0 || right_norm == 0.0 {
        return 0.0;
    }
    (dot / (left_norm * right_norm).sqrt()).max(0.0)
}

fn freshness(updated: Millis, now: Millis) -> f64 {
    if updated == 0 {
        return 0.25;
    }
    let hours = (now - updated) / 3_600_000;
    let age = hours as f64 / 24.0;
    if age <= 0.0 {
        1.0
    } else {
        (-age / 180.0).exp()
    }
}

fn normalized_key(text: &str) -> String {
    tokens(text).iter().map(|word| format!("{word} ")).collect()
}

fn known_stale_status(value: &str) -> bool {
    matches!(
        value.to_ascii_lowercase().as_str(),
        "changed"
            | "deleted"
            | "drifted"
            | "invalid"
            | "mismatch"
            | "missing"
            | "outdated"
            | "stale"
    )
}

/// Conflict and lifecycle warnings for one memory.
fn warnings_for(
    store: &MemoryStore,
    scope: &MemoryScope,
    memory: &MemoryEntry,
    now: Millis,
) -> Result<Vec<String>> {
    let mut warnings = Vec::new();
    match memory.status {
        MemoryStatus::Candidate => warnings.push("candidate memory; not validated".to_string()),
        MemoryStatus::Hypothesis => {
            warnings.push("hypothesis; verify before relying on it".to_string())
        }
        _ => {}
    }
    if memory.confidence < 0.5 {
        warnings.push("low confidence".into());
    }
    if !memory.replacement_id.is_empty() {
        warnings.push(format!(
            "replacement relationship: {}",
            memory.replacement_id
        ));
    }
    if memory.provenance.source_uri.is_empty() && memory.provenance.source_hash.is_empty() {
        warnings.push("no durable source locator".into());
    }
    if memory.updated_at != 0 && now - memory.updated_at > 365 * 24 * 3_600_000 {
        warnings.push("possibly stale; last update is over one year old".into());
    }
    let query = GraphQuery {
        memory_id: memory.id.clone(),
        limit: 50,
        ..Default::default()
    };
    for edge in store.query_edges(scope, &query)? {
        if edge.edge_type == "conflicts_with" || edge.edge_type == "contradicts" {
            let peer = if edge.from_id == memory.id {
                &edge.to_id
            } else {
                &edge.from_id
            };
            warnings.push(format!("conflict relationship with memory {peer}"));
        }
    }
    Ok(warnings)
}

/// A drift warning when a registered source (or an assertion's evidence
/// source) is known to have changed.
pub(crate) fn source_drift_warning(
    store: &MemoryStore,
    memory: &MemoryEntry,
) -> Result<Option<String>> {
    let registered = store.sources(&memory.scope)?;
    let drift = |id: &str, uri: &str, hash: &str| -> Option<String> {
        for source in &registered {
            let matches = if id.is_empty() {
                !uri.is_empty() && source.uri == uri
            } else {
                source.id == id
            };
            if !matches {
                continue;
            }
            if known_stale_status(&source.hash_status) {
                return Some(format!(
                    "known stale source: hash status is {}",
                    source.hash_status
                ));
            }
            if !hash.is_empty() && !source.hash.is_empty() && hash != source.hash {
                return Some("known stale source: recorded source hash has changed".into());
            }
        }
        None
    };
    let provenance = &memory.provenance;
    if let Some(warning) = drift(
        &provenance.source_id,
        &provenance.source_uri,
        &provenance.source_hash,
    ) {
        return Ok(Some(warning));
    }
    if let Some(assertion) = store.assertion(&memory.scope, &memory.id, None)? {
        for evidence in &assertion.evidence {
            if let Some(warning) = drift(
                &evidence.source_id,
                &evidence.source_uri,
                &evidence.source_hash,
            ) {
                return Ok(Some(warning));
            }
        }
    }
    Ok(None)
}

/// Whether a memory may currently be used as guidance: visible, trusted,
/// without source drift and not blocked by a critical conflict review.
pub fn eligible_for_context(
    store: &MemoryStore,
    scope: &MemoryScope,
    memory_id: &str,
    policy: RecallPolicy,
    include_global_user: bool,
) -> bool {
    let Ok(Some(current)) = store.find_for_recall(scope, memory_id, policy, include_global_user)
    else {
        return false;
    };
    if !GovernanceService::trusted_for_recall(current.status) {
        return false;
    }
    if !matches!(source_drift_warning(store, &current), Ok(None)) {
        return false;
    }
    let Ok(mut blocked) = store.critical_conflict_memory_ids(scope) else {
        return false;
    };
    match (scope_key(&current.scope), scope_key(scope)) {
        (Ok(stored), Ok(caller)) if stored != caller => {
            match store.critical_conflict_memory_ids(&current.scope) {
                Ok(canonical) => blocked.extend(canonical),
                Err(_) => return false,
            }
        }
        (Ok(_), Ok(_)) => {}
        _ => return false,
    }
    !blocked.iter().any(|id| id == memory_id)
}

struct EntityFrontier {
    id: String,
    path: Vec<RecalledAssertionStep>,
}

struct FrontierItem {
    id: String,
    depth: usize,
    ids: Vec<String>,
    relations: Vec<String>,
    reverse: Vec<bool>,
}

/// Per-recall caches of authority lookups.
struct Run<'a> {
    store: &'a MemoryStore,
    scope: &'a MemoryScope,
    request: &'a RecallRequest,
    now: Millis,
    conflict_scopes: HashMap<String, HashSet<String>>,
    source_assessments: HashMap<String, Option<String>>,
}

impl Run<'_> {
    fn blocked_in_scope(&mut self, review_scope: &MemoryScope, id: &str) -> Result<bool> {
        let key = scope_key(review_scope)?;
        if !self.conflict_scopes.contains_key(&key) {
            let ids = self.store.critical_conflict_memory_ids(review_scope)?;
            self.conflict_scopes
                .insert(key.clone(), ids.into_iter().collect());
        }
        Ok(self.conflict_scopes[&key].contains(id))
    }

    fn has_critical_conflict(&mut self, memory: &MemoryEntry) -> bool {
        let scope = self.scope;
        let check = |run: &mut Self| -> Result<bool> {
            Ok(run.blocked_in_scope(scope, &memory.id)?
                || (scope_key(&memory.scope)? != scope_key(scope)?
                    && run.blocked_in_scope(&memory.scope, &memory.id)?))
        };
        // An unavailable authority lookup must not authorise a graph bridge.
        check(self).unwrap_or(true)
    }

    fn source_assessment(&mut self, memory: &MemoryEntry) -> Result<Option<String>> {
        if let Some(found) = self.source_assessments.get(&memory.id) {
            return Ok(found.clone());
        }
        let assessment = source_drift_warning(self.store, memory)?;
        self.source_assessments
            .insert(memory.id.clone(), assessment.clone());
        Ok(assessment)
    }

    fn eligible_for_graph(&mut self, memory: &MemoryEntry) -> Result<bool> {
        let request = self.request;
        if !scope_can_access(
            self.scope,
            &memory.scope,
            request.policy,
            request.include_global_user,
        ) {
            return Ok(false);
        }
        let tentative = matches!(
            memory.status,
            MemoryStatus::Candidate | MemoryStatus::Hypothesis
        );
        if !GovernanceService::trusted_for_recall(memory.status) && !tentative {
            return Ok(false);
        }
        if !request.include_candidates && tentative {
            return Ok(false);
        }
        if request
            .tags
            .iter()
            .any(|required| !memory.tags.iter().any(|tag| tag == required))
        {
            return Ok(false);
        }
        if self.has_critical_conflict(memory) {
            return Ok(false);
        }
        Ok(self.source_assessment(memory)?.is_none())
    }

    fn score_memory(&mut self, memory: MemoryEntry) -> Result<RecalledMemory> {
        let request = self.request;
        let mut recalled = RecalledMemory::new(MemoryEntry::default());
        let score = &mut recalled.score;
        score.hashed_similarity = hashed_similarity(&request.query, &memory.content);
        score.lexical = if request.query.is_empty() {
            0.5
        } else {
            score.hashed_similarity
        };
        score.confidence = memory.confidence;
        score.quality = memory.quality_score;
        score.freshness = freshness(memory.updated_at, self.now);
        score.scope = scope_score(self.scope, &memory.scope);
        score.total = 0.28 * score.lexical
            + 0.22 * score.hashed_similarity
            + 0.18 * score.confidence
            + 0.10 * score.quality
            + 0.08 * score.freshness
            + 0.14 * score.scope;
        match memory.status {
            MemoryStatus::Validated => score.total += 0.08,
            MemoryStatus::Candidate => score.total *= 0.68,
            MemoryStatus::Hypothesis => score.total *= 0.78,
            _ => {}
        }
        score.total = score.total.min(1.0);
        recalled.warnings = warnings_for(self.store, self.scope, &memory, self.now)?;
        if self.has_critical_conflict(&memory) {
            recalled
                .warnings
                .push("critical unresolved conflict; excluded from guidance context".into());
            recalled.eligible_for_context = false;
        }
        if let Some(drift) = self.source_assessment(&memory)? {
            recalled.warnings.push(drift);
            recalled.eligible_for_context = false;
        }
        recalled.memory = memory;
        Ok(recalled)
    }
}

impl RecallService {
    pub fn new(store: Arc<MemoryStore>) -> Self {
        Self { store, dense: None }
    }

    /// Adds dense candidates (normally [`crate::retrieval::RetrievalEngine::search_candidates`]).
    pub fn with_dense(mut self, provider: DenseCandidateProvider) -> Self {
        self.dense = Some(provider);
        self
    }

    pub fn store(&self) -> &Arc<MemoryStore> {
        &self.store
    }

    /// See [`eligible_for_context`].
    pub fn eligible_for_context(
        &self,
        scope: &MemoryScope,
        memory_id: &str,
        policy: RecallPolicy,
        include_global_user: bool,
    ) -> bool {
        eligible_for_context(&self.store, scope, memory_id, policy, include_global_user)
    }

    /// Increments access counts of the context-eligible results.
    pub fn record_access(&self, scope: &MemoryScope, memories: &[RecalledMemory]) -> Result<()> {
        let ids: Vec<String> = memories
            .iter()
            .filter(|item| item.eligible_for_context)
            .map(|item| item.memory.id.clone())
            .collect();
        self.store.record_access(scope, &ids)
    }

    pub fn recall(
        &self,
        scope: &MemoryScope,
        input: &RecallRequest,
    ) -> Result<Vec<RecalledMemory>> {
        let mut request = input.clone();
        request.candidate_limit = request.candidate_limit.min(1000);
        request.result_limit = request.result_limit.min(1000);
        if request.candidate_limit == 0 || request.result_limit == 0 {
            return Ok(Vec::new());
        }
        request.graph.max_seeds = request.graph.max_seeds.min(128);
        request.graph.max_depth = request.graph.max_depth.min(8);
        request.graph.max_fanout = request.graph.max_fanout.min(256);
        request.graph.max_edges = request.graph.max_edges.min(8192);
        request.graph.max_results = request.graph.max_results.min(1000);
        request.structured.limit = request.structured.limit.min(128);
        // A historical structured answer is never mixed with current
        // candidates presented as evidence from that point in time.
        if request.structured.as_of.is_some() {
            request.graph.enabled = false;
            request.dense_enabled = false;
        }
        let store = &*self.store;
        let mut statuses = vec![MemoryStatus::Active, MemoryStatus::Validated];
        if request.include_candidates {
            statuses.extend([MemoryStatus::Candidate, MemoryStatus::Hypothesis]);
        }
        let query = MemoryQuery {
            text: request.query.clone(),
            tags: request.tags.clone(),
            policy: request.policy,
            include_global_user: request.include_global_user,
            limit: request.candidate_limit,
            statuses,
            ..Default::default()
        };
        let mut candidates = store.query(scope, &query)?;
        let mut structured_seed_ids: Vec<String> = Vec::new();
        let mut historical_assertions: BTreeMap<String, MemoryAssertion> = BTreeMap::new();
        if request.structured.enabled && request.structured.limit != 0 {
            let structured = &request.structured;
            let resolve = |value: &str| -> Result<Vec<String>> {
                if value.is_empty() {
                    return Ok(Vec::new());
                }
                Ok(store
                    .find_entities(
                        scope,
                        value,
                        request.policy,
                        request.include_global_user,
                        structured.limit,
                    )?
                    .into_iter()
                    .map(|entity| entity.id)
                    .collect())
            };
            let subjects = resolve(&structured.subject)?;
            let objects = resolve(&structured.object)?;
            let mut seen_assertions: HashSet<String> = HashSet::new();
            let mut structured_queries = 0usize;
            let mut query_assertions = |subject_id: &str, object_id: &str| -> Result<()> {
                if structured_seed_ids.len() >= structured.limit {
                    return Ok(());
                }
                structured_queries += 1;
                if structured_queries > structured.limit {
                    return Ok(());
                }
                for assertion in store.query_assertions(
                    scope,
                    subject_id,
                    &structured.predicate,
                    object_id,
                    structured.as_of,
                    request.policy,
                    request.include_global_user,
                    structured.limit - structured_seed_ids.len(),
                )? {
                    if seen_assertions.insert(assertion.memory_id.clone()) {
                        structured_seed_ids.push(assertion.memory_id.clone());
                        if structured.as_of.is_some() {
                            historical_assertions.insert(assertion.memory_id.clone(), assertion);
                        }
                    }
                }
                Ok(())
            };
            let unresolved = (!structured.subject.is_empty() && subjects.is_empty())
                || (!structured.object.is_empty() && objects.is_empty());
            if !unresolved && !subjects.is_empty() {
                for subject in &subjects {
                    if objects.is_empty() {
                        query_assertions(subject, "")?;
                    } else {
                        for object in &objects {
                            query_assertions(subject, object)?;
                        }
                    }
                }
            } else if !unresolved && !objects.is_empty() {
                for object in &objects {
                    query_assertions("", object)?;
                }
            } else if !unresolved
                && (!structured.predicate.is_empty() || structured.as_of.is_some())
            {
                query_assertions("", "")?;
            }
        }
        if request.structured.as_of.is_some() {
            candidates.clear();
            for (id, assertion) in &historical_assertions {
                let Some(current) = store.find_for_recall(
                    scope,
                    id,
                    request.policy,
                    request.include_global_user,
                )?
                else {
                    continue;
                };
                if !GovernanceService::trusted_for_recall(current.status) {
                    continue;
                }
                let mut historical = current;
                historical.content = assertion.historical_content.clone();
                historical.source = assertion.historical_source.clone();
                historical.memory_type = assertion.historical_type;
                historical.status = assertion.historical_status;
                historical.provenance = assertion.historical_provenance.clone();
                historical.created_at = assertion.historical_created_at;
                historical.updated_at = assertion.historical_updated_at;
                candidates.push(historical);
            }
        }
        let mut run = Run {
            store,
            scope,
            request: &request,
            now: now_ms(),
            conflict_scopes: HashMap::new(),
            source_assessments: HashMap::new(),
        };
        let mut result: Vec<RecalledMemory> = Vec::with_capacity(candidates.len());
        let mut result_by_id: HashMap<String, usize> = HashMap::new();
        let mut duplicates: HashSet<String> = HashSet::new();
        for (rank, memory) in candidates.into_iter().enumerate() {
            // Non-graph recall keeps excluded items as ineligible results.
            if !run.eligible_for_graph(&memory)? && request.graph.enabled {
                continue;
            }
            let key =
                if request.graph.enabled || request.dense_enabled || request.structured.enabled {
                    memory.id.clone()
                } else {
                    normalized_key(&memory.content)
                };
            if !duplicates.insert(key) {
                continue;
            }
            let mut recalled = run.score_memory(memory)?;
            if let Some(historical) = historical_assertions.get(&recalled.memory.id) {
                recalled.historical = true;
                recalled.historical_revision = historical.revision;
                recalled.historical_recorded_at = Some(historical.recorded_at);
                recalled.historical_as_of = request.structured.as_of;
                recalled.warnings.push(
                    "historical assertion snapshot; current guidance requires revalidation".into(),
                );
            }
            recalled.score.lexical_rank = rank + 1;
            result_by_id.insert(recalled.memory.id.clone(), result.len());
            result.push(recalled);
        }
        if request.dense_enabled
            && let Some(dense) = &self.dense
            && !request.is_cancelled()
        {
            let dense_candidates = dense(scope, &request);
            let mut dense_owners: HashSet<String> = HashSet::new();
            for candidate in dense_candidates.iter().take(1000) {
                if dense_owners.len() >= request.candidate_limit || request.is_cancelled() {
                    break;
                }
                let captured = &candidate.memory;
                let hit = &candidate.hit;
                if dense_owners.contains(&captured.id) {
                    continue;
                }
                let Ok(Some(current)) = store.find_for_recall(
                    scope,
                    &captured.id,
                    request.policy,
                    request.include_global_user,
                ) else {
                    continue;
                };
                let Ok(current_revision) = store.retrieval_revision(&current.scope, &current.id)
                else {
                    continue;
                };
                // A provider snapshot is ranking evidence, never authority.
                let same_scope = matches!(
                    (scope_key(&current.scope), scope_key(&captured.scope)),
                    (Ok(left), Ok(right)) if left == right
                );
                if !run.eligible_for_graph(&current).unwrap_or(false)
                    || current.content != captured.content
                    || current.updated_at != captured.updated_at
                    || !same_scope
                    || hit.chunk.owner_key != format!("memory:{}", captured.id)
                    || hit.chunk.id.is_empty()
                    || hit.generation.is_empty()
                    || hit.chunk.owner_revision <= 0
                    || hit.chunk.owner_revision != current_revision
                    || !hit.similarity.is_finite()
                    || !(-1.0..=1.0).contains(&hit.similarity)
                {
                    continue;
                }
                dense_owners.insert(current.id.clone());
                let owner_rank = dense_owners.len();
                if let Some(index) = result_by_id.get(&current.id) {
                    result[*index].score.dense_rank = owner_rank;
                    result[*index].dense_match = Some(hit.clone());
                    continue;
                }
                if !duplicates.insert(current.id.clone()) {
                    continue;
                }
                let Ok(mut recalled) = run.score_memory(current) else {
                    continue;
                };
                recalled.score.dense_rank = owner_rank;
                recalled.dense_match = Some(hit.clone());
                result_by_id.insert(recalled.memory.id.clone(), result.len());
                result.push(recalled);
            }
        }
        let graph_requested = request.structured.as_of.is_none()
            && (request.graph.enabled || !structured_seed_ids.is_empty());
        if graph_requested && request.graph.max_results != 0 {
            self.graph_recall(
                &mut run,
                &mut result,
                &mut result_by_id,
                &structured_seed_ids,
            )?;
        }
        for recalled in &mut result {
            let rrf = |rank: usize| {
                if rank == 0 {
                    0.0
                } else {
                    1.0 / (60.0 + rank as f64)
                }
            };
            if graph_requested || request.dense_enabled {
                recalled.score.reciprocal_rank_fusion = rrf(recalled.score.lexical_rank)
                    + rrf(recalled.score.graph_rank)
                    + rrf(recalled.score.dense_rank);
                recalled.score.total = recalled.score.reciprocal_rank_fusion;
            }
        }
        result.sort_by(|lhs, rhs| {
            rhs.score
                .reciprocal_rank_fusion
                .partial_cmp(&lhs.score.reciprocal_rank_fusion)
                .unwrap_or(std::cmp::Ordering::Equal)
                .then_with(|| {
                    rhs.score
                        .scope
                        .partial_cmp(&lhs.score.scope)
                        .unwrap_or(std::cmp::Ordering::Equal)
                })
                .then_with(|| {
                    rhs.score
                        .total
                        .partial_cmp(&lhs.score.total)
                        .unwrap_or(std::cmp::Ordering::Equal)
                })
                .then_with(|| lhs.memory.id.cmp(&rhs.memory.id))
        });
        result.truncate(request.result_limit);
        Ok(result)
    }

    fn graph_recall(
        &self,
        run: &mut Run<'_>,
        result: &mut Vec<RecalledMemory>,
        result_by_id: &mut HashMap<String, usize>,
        structured_seed_ids: &[String],
    ) -> Result<()> {
        let request = run.request;
        let scope = run.scope;
        let store = run.store;
        let graph = &request.graph;
        let mut edges_examined = 0usize;
        let mut seeds_added = 0usize;
        let mut results_added = 0usize;
        // Entity relations are grounded in governed assertion owners; each
        // owner stays in the path so context can revalidate it.
        if graph.enabled {
            let mut entity_frontier: Vec<EntityFrontier> = Vec::new();
            let mut entity_seen: HashSet<String> = HashSet::new();
            let visible_entity = |id: &str| -> Result<bool> {
                let found = store.find_entities(
                    scope,
                    id,
                    request.policy,
                    request.include_global_user,
                    2,
                )?;
                Ok(found.len() == 1 && found[0].id == id)
            };
            let entity_seed = |id: &str,
                               frontier: &mut Vec<EntityFrontier>,
                               seen: &mut HashSet<String>,
                               seeds_added: &mut usize|
             -> Result<()> {
                if id.is_empty()
                    || *seeds_added >= graph.max_seeds
                    || !visible_entity(id)?
                    || !seen.insert(id.into())
                {
                    return Ok(());
                }
                *seeds_added += 1;
                frontier.push(EntityFrontier {
                    id: id.into(),
                    path: Vec::new(),
                });
                Ok(())
            };
            for value in [&request.structured.subject, &request.structured.object] {
                if value.is_empty() {
                    continue;
                }
                let matches = store.find_entities(
                    scope,
                    value,
                    request.policy,
                    request.include_global_user,
                    2,
                )?;
                if matches.len() == 1 {
                    entity_seed(
                        &matches[0].id,
                        &mut entity_frontier,
                        &mut entity_seen,
                        &mut seeds_added,
                    )?;
                }
            }
            let seeds: Vec<MemoryEntry> = result
                .iter()
                .filter(|item| item.eligible_for_context)
                .map(|item| item.memory.clone())
                .collect();
            for memory in seeds {
                if !run.eligible_for_graph(&memory)? {
                    continue;
                }
                if let Some(assertion) = store.assertion(&memory.scope, &memory.id, None)? {
                    entity_seed(
                        &assertion.subject_id,
                        &mut entity_frontier,
                        &mut entity_seen,
                        &mut seeds_added,
                    )?;
                }
            }
            let allowed: HashSet<&str> = if graph.allowed_relation_types.is_empty() {
                [
                    "supported_by",
                    "derived_from",
                    "discusses",
                    "applies_to",
                    "depends_on",
                    "uses",
                ]
                .into_iter()
                .collect()
            } else {
                graph
                    .allowed_relation_types
                    .iter()
                    .map(String::as_str)
                    .collect()
            };
            let mut front = 0usize;
            while front < entity_frontier.len()
                && edges_examined < graph.max_edges
                && results_added < graph.max_results
            {
                if request.is_cancelled() {
                    break;
                }
                let current_id = entity_frontier[front].id.clone();
                let current_path = entity_frontier[front].path.clone();
                front += 1;
                if current_path.len() >= graph.max_depth {
                    continue;
                }
                let remaining = graph.max_fanout.min(graph.max_edges - edges_examined);
                if remaining == 0 {
                    break;
                }
                let mut adjacent = Vec::new();
                if graph.direction != GraphDirection::Incoming {
                    adjacent = store.query_assertions(
                        scope,
                        &current_id,
                        "",
                        "",
                        None,
                        request.policy,
                        request.include_global_user,
                        remaining,
                    )?;
                }
                if graph.direction != GraphDirection::Outgoing && adjacent.len() < remaining {
                    adjacent.extend(store.query_assertions(
                        scope,
                        "",
                        "",
                        &current_id,
                        None,
                        request.policy,
                        request.include_global_user,
                        remaining - adjacent.len(),
                    )?);
                }
                for assertion in adjacent {
                    if edges_examined >= graph.max_edges || results_added >= graph.max_results {
                        edges_examined += 1;
                        break;
                    }
                    edges_examined += 1;
                    if !allowed.contains(assertion.predicate.as_str())
                        || assertion.evidence.is_empty()
                    {
                        continue;
                    }
                    let Some(owner) = store.find_for_recall(
                        scope,
                        &assertion.memory_id,
                        request.policy,
                        request.include_global_user,
                    )?
                    else {
                        continue;
                    };
                    if !run.eligible_for_graph(&owner)? {
                        continue;
                    }
                    let reverse = assertion.subject_id != current_id;
                    let target = if reverse {
                        assertion.subject_id.clone()
                    } else {
                        assertion.object_id.clone()
                    };
                    if !target.is_empty() && !visible_entity(&target)? {
                        continue;
                    }
                    if current_path
                        .iter()
                        .any(|step| step.memory_id == assertion.memory_id)
                    {
                        continue;
                    }
                    let mut path = current_path.clone();
                    path.push(RecalledAssertionStep {
                        memory_id: assertion.memory_id.clone(),
                        revision: assertion.revision,
                        from_entity_id: current_id.clone(),
                        to_entity_id: target.clone(),
                        reverse,
                    });
                    match result_by_id.get(&assertion.memory_id) {
                        None => {
                            let mut recalled = run.score_memory(owner)?;
                            recalled.assertion_path = path.clone();
                            recalled.connecting_memory = true;
                            results_added += 1;
                            recalled.score.graph_rank = results_added;
                            result_by_id.insert(assertion.memory_id.clone(), result.len());
                            result.push(recalled);
                        }
                        Some(index) => {
                            let recalled = &mut result[*index];
                            if recalled.assertion_path.is_empty() {
                                recalled.assertion_path = path.clone();
                            }
                            if recalled.score.graph_rank == 0 {
                                results_added += 1;
                                recalled.score.graph_rank = results_added;
                            }
                        }
                    }
                    if assertion.positive
                        && !target.is_empty()
                        && entity_seen.insert(target.clone())
                    {
                        entity_frontier.push(EntityFrontier { id: target, path });
                    }
                }
            }
        }
        let mut frontier: VecDeque<FrontierItem> = VecDeque::new();
        let mut visited: HashSet<String> = HashSet::new();
        let mut seed = |id: &str,
                        run: &mut Run<'_>,
                        result: &mut Vec<RecalledMemory>,
                        result_by_id: &mut HashMap<String, usize>,
                        frontier: &mut VecDeque<FrontierItem>|
         -> Result<()> {
            if id.is_empty() || seeds_added >= graph.max_seeds || !visited.insert(id.into()) {
                return Ok(());
            }
            let Some(memory) =
                store.find_for_recall(scope, id, request.policy, request.include_global_user)?
            else {
                return Ok(());
            };
            if !run.eligible_for_graph(&memory)? {
                return Ok(());
            }
            seeds_added += 1;
            frontier.push_back(FrontierItem {
                id: id.into(),
                depth: 0,
                ids: vec![id.into()],
                relations: Vec::new(),
                reverse: Vec::new(),
            });
            match result_by_id.get(id) {
                None => {
                    let mut recalled = run.score_memory(memory)?;
                    recalled.connecting_memory = true;
                    recalled.score.graph_rank = 1;
                    result_by_id.insert(id.into(), result.len());
                    result.push(recalled);
                }
                Some(index) => {
                    if result[*index].score.graph_rank == 0 {
                        result[*index].score.graph_rank = 1;
                    }
                }
            }
            Ok(())
        };
        for id in &graph.seed_memory_ids {
            seed(id, run, result, result_by_id, &mut frontier)?;
        }
        for id in structured_seed_ids {
            seed(id, run, result, result_by_id, &mut frontier)?;
        }
        let lexical_seed_ids: Vec<String> =
            result.iter().map(|item| item.memory.id.clone()).collect();
        for id in &lexical_seed_ids {
            seed(id, run, result, result_by_id, &mut frontier)?;
        }
        let allowed: HashSet<&str> = graph
            .allowed_relation_types
            .iter()
            .map(String::as_str)
            .collect();
        let mut graph_rank = results_added + 1;
        while edges_examined < graph.max_edges && graph_rank < graph.max_results {
            let Some(current) = frontier.pop_front() else {
                break;
            };
            if current.depth >= graph.max_depth || graph.max_fanout == 0 {
                continue;
            }
            let query = GraphQuery {
                memory_id: current.id.clone(),
                limit: graph.max_fanout,
                ..Default::default()
            };
            for edge in store.query_edges(scope, &query)? {
                let examined = edges_examined;
                edges_examined += 1;
                if examined >= graph.max_edges {
                    break;
                }
                if !allowed.is_empty() && !allowed.contains(edge.edge_type.as_str()) {
                    continue;
                }
                let neighbor = match graph.direction {
                    GraphDirection::Outgoing => {
                        if edge.from_id != current.id {
                            continue;
                        }
                        edge.to_id.clone()
                    }
                    GraphDirection::Incoming => {
                        if edge.to_id != current.id {
                            continue;
                        }
                        edge.from_id.clone()
                    }
                    GraphDirection::Both => {
                        if edge.from_id == current.id {
                            edge.to_id.clone()
                        } else {
                            edge.from_id.clone()
                        }
                    }
                };
                if neighbor.is_empty() || !visited.insert(neighbor.clone()) {
                    continue;
                }
                let Some(memory) = store.find_for_recall(
                    scope,
                    &neighbor,
                    request.policy,
                    request.include_global_user,
                )?
                else {
                    continue;
                };
                if !run.eligible_for_graph(&memory)? {
                    continue;
                }
                graph_rank += 1;
                let mut path_ids = current.ids.clone();
                path_ids.push(neighbor.clone());
                let mut path_relations = current.relations.clone();
                path_relations.push(edge.edge_type.clone());
                let mut path_reverse = current.reverse.clone();
                path_reverse.push(edge.to_id == current.id);
                match result_by_id.get(&neighbor) {
                    None => {
                        let mut recalled = run.score_memory(memory)?;
                        recalled.connecting_memory = graph.include_connecting_memories;
                        recalled.graph_path_ids = path_ids.clone();
                        recalled.graph_path_relations = path_relations.clone();
                        recalled.graph_path_reverse = path_reverse.clone();
                        recalled.score.graph_rank = graph_rank;
                        result_by_id.insert(neighbor.clone(), result.len());
                        result.push(recalled);
                    }
                    Some(index) => {
                        let recalled = &mut result[*index];
                        if recalled.graph_path_ids.is_empty() {
                            recalled.graph_path_ids = path_ids.clone();
                            recalled.graph_path_relations = path_relations.clone();
                            recalled.graph_path_reverse = path_reverse.clone();
                        }
                        if recalled.score.graph_rank == 0 {
                            recalled.score.graph_rank = graph_rank;
                        }
                    }
                }
                if current.depth + 1 < graph.max_depth {
                    frontier.push_back(FrontierItem {
                        id: neighbor,
                        depth: current.depth + 1,
                        ids: path_ids,
                        relations: path_relations,
                        reverse: path_reverse,
                    });
                }
                if graph_rank >= graph.max_results {
                    break;
                }
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tokens_and_similarity_follow_archived_rules() {
        assert_eq!(tokens("A b_c, d-e x!"), vec!["b_c", "d-e"]);
        assert_eq!(hashed_similarity("", "anything"), 0.0);
        let same = hashed_similarity("rust memory recall", "rust memory recall");
        assert!((same - 1.0).abs() < 1e-12);
        assert!(hashed_similarity("rust memory", "python tooling") < 1.0);
        assert_eq!(normalized_key("Hello, hello World"), "hello world ");
    }

    #[test]
    fn freshness_decays_by_whole_hours() {
        let now = 1_000_000_000_000;
        assert_eq!(freshness(0, now), 0.25);
        assert_eq!(freshness(now, now), 1.0);
        let day = freshness(now - 86_400_000, now);
        assert!((day - (-1.0f64 / 180.0).exp()).abs() < 1e-12);
    }
}
