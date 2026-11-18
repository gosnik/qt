//! Retrieval engine: background indexing worker, dense memory search, code
//! recall with fused graph paths, and evidence revalidation (archived
//! `retrieval_engine.cpp`).

use std::collections::{BTreeSet, HashMap, HashSet};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use super::graph::{
    RetrievalGraphAdapter, RetrievalGraphIdentity, RetrievalGraphQuery, RetrievalGraphSeed,
};
use super::{
    Cancellation, DenseIndexConfig, DenseMemoryCandidate, DenseRetrievalService,
    DenseRetrievalStats, Embedder, RetrievalCandidate, RetrievalChunk, RetrievalIndexStatus,
    RetrievalIndexer, RetrievalOwnerKind, RetrievalPathEdge, RetrievalRank, RetrievalReference,
};
use crate::code::{CodeSnapshot, CodeSnapshotProvider, source_fingerprint};
use crate::error::{MemoryError, Result};
use crate::model::{GraphQuery, MemoryScope, MemoryStatus, RecallPolicy, ScopeKind, now_ms};
use crate::recall::eligible_for_context;
use crate::scope::{recall_scopes, scope_key, validate_scope};
use crate::store::MemoryStore;
use crate::text::stable_hash_hex;

/// Relations code recall may traverse.
pub const CODE_GRAPH_RELATIONS: [&str; 15] = [
    "related",
    "related_to",
    "documents",
    "discusses",
    "applies_to",
    "supported_by",
    "supports",
    "derived_from",
    "depends_on",
    "contains_file",
    "defines",
    "calls",
    "uses",
    "assertion_subject",
    "assertion_object",
];

#[derive(Clone)]
pub struct RetrievalEngineOptions {
    pub enabled: bool,
    pub background_indexing: bool,
    pub dense_enabled: bool,
    pub code_enabled: bool,
    pub code_graph_enabled: bool,
    pub graph_max_seeds: usize,
    pub graph_max_depth: usize,
    pub graph_max_fanout: usize,
    pub graph_max_edges: usize,
    pub graph_max_results: usize,
    pub code: Option<Arc<dyn CodeSnapshotProvider>>,
    pub cache_entries: usize,
    pub policy: RecallPolicy,
    pub include_global_user: bool,
    pub embedder: Option<Arc<dyn Embedder>>,
}

impl Default for RetrievalEngineOptions {
    fn default() -> Self {
        Self {
            enabled: false,
            background_indexing: true,
            dense_enabled: false,
            code_enabled: false,
            code_graph_enabled: false,
            graph_max_seeds: 16,
            graph_max_depth: 2,
            graph_max_fanout: 16,
            graph_max_edges: 256,
            graph_max_results: 64,
            code: None,
            cache_entries: 64,
            policy: RecallPolicy::Balanced,
            include_global_user: false,
            embedder: None,
        }
    }
}

/// Effective code-index freshness.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct RetrievalCodeStatus {
    pub enabled: bool,
    pub available: bool,
    pub stale: bool,
    pub generation: u64,
    pub repository: String,
    pub worktree: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct RetrievalEngineStatus {
    pub enabled: bool,
    pub running: bool,
    pub generation: String,
    pub last_error: String,
    pub completed_jobs: usize,
    pub jobs: RetrievalIndexStatus,
    pub queries: DenseRetrievalStats,
    pub code: RetrievalCodeStatus,
}

struct State {
    scope: MemoryScope,
    status: RetrievalEngineStatus,
    foreground_active: bool,
}

struct Inner {
    store: Arc<MemoryStore>,
    options: RetrievalEngineOptions,
    indexer: RetrievalIndexer,
    dense: DenseRetrievalService,
    state: Mutex<State>,
    condition: Condvar,
    stopping: AtomicBool,
}

/// Owns indexing lifetime; all store access by the worker ends before
/// [`RetrievalEngine::stop`] returns.
pub struct RetrievalEngine {
    inner: Arc<Inner>,
    worker: Mutex<Option<JoinHandle<()>>>,
}

impl std::fmt::Debug for RetrievalEngine {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("RetrievalEngine").finish_non_exhaustive()
    }
}

/// Scope that holds published code owners for a snapshot root.
pub fn code_scope(caller: &MemoryScope, snapshot: &CodeSnapshot) -> MemoryScope {
    let mut scope = caller.clone();
    scope.kind = ScopeKind::Exact;
    scope.task.clear();
    scope.session.clear();
    scope.conversation.clear();
    scope.agent = format!(
        "qt-code-index:{}",
        stable_hash_hex(&snapshot.root.to_string_lossy().replace('\\', "/"))
    );
    scope
}

fn same_reference(a: &RetrievalReference, b: &RetrievalReference) -> bool {
    a == b
}

impl Inner {
    fn lock(&self) -> MutexGuard<'_, State> {
        self.state
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    fn record_error(&self, error: impl ToString) {
        self.lock().status.last_error = error.to_string();
    }

    fn snapshot(&self) -> Option<Arc<CodeSnapshot>> {
        if !self.options.code_enabled {
            return None;
        }
        self.options.code.as_ref().and_then(|code| code.snapshot())
    }

    fn prepare_embedding(&self, cancelled: Cancellation<'_>) -> Result<()> {
        let embedder = self
            .options
            .embedder
            .as_ref()
            .ok_or_else(|| MemoryError::storage("dense model is not configured"))?;
        if !self.options.background_indexing {
            let config = embedder.configure(cancelled)?;
            self.lock().status.generation = config.generation;
        }
        embedder.validate()
    }

    fn run(&self) {
        let cancelled = || self.stopping.load(Ordering::SeqCst);
        let wait_until_ready = |timeout: Duration| -> bool {
            let guard = self.lock();
            let (guard, _) = self
                .condition
                .wait_timeout_while(guard, timeout, |state| {
                    !self.stopping.load(Ordering::SeqCst) && state.foreground_active
                })
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            self.stopping.load(Ordering::SeqCst) || !guard.foreground_active
        };
        let mut config: Option<DenseIndexConfig> = None;
        let mut initialized: BTreeSet<String> = BTreeSet::new();
        let mut published_code: Option<Arc<CodeSnapshot>> = None;
        let mut published_code_scope = String::new();
        let mut configure_after: Option<Instant> = None;
        while !self.stopping.load(Ordering::SeqCst) {
            let scope = {
                let guard = self.lock();
                let mut guard = self
                    .condition
                    .wait_while(guard, |state| {
                        !self.stopping.load(Ordering::SeqCst) && state.foreground_active
                    })
                    .unwrap_or_else(|poisoned| poisoned.into_inner());
                if self.stopping.load(Ordering::SeqCst) {
                    break;
                }
                guard.status.running = true;
                guard.scope.clone()
            };
            let mut worked = false;
            let outcome = (|| -> Result<()> {
                if config.is_some()
                    && let Some(embedder) = &self.options.embedder
                {
                    embedder.validate()?;
                }
                if self.options.dense_enabled
                    && config.is_none()
                    && configure_after.is_none_or(|after| Instant::now() >= after)
                {
                    configure_after = Some(Instant::now() + Duration::from_secs(30));
                    let embedder = self
                        .options
                        .embedder
                        .as_ref()
                        .ok_or_else(|| MemoryError::storage("dense model is not configured"))?;
                    let configured = embedder.configure(&cancelled)?;
                    let mut guard = self.lock();
                    guard.status.generation = configured.generation.clone();
                    guard.status.last_error.clear();
                    config = Some(configured);
                }
                let mut scopes = recall_scopes(
                    &scope,
                    self.options.policy,
                    self.options.include_global_user,
                )?;
                if let Some(snapshot) = self.snapshot()
                    && !snapshot.stale
                {
                    let indexed_scope = code_scope(&scope, &snapshot);
                    let key = scope_key(&indexed_scope)?;
                    let republish = published_code
                        .as_ref()
                        .is_none_or(|published| !Arc::ptr_eq(published, &snapshot))
                        || key != published_code_scope;
                    if republish {
                        self.store
                            .sync_code_snapshot(&indexed_scope, &snapshot, &cancelled)?;
                        published_code = Some(Arc::clone(&snapshot));
                        published_code_scope = key;
                    }
                    scopes.push(indexed_scope);
                }
                for visible in &scopes {
                    if self.stopping.load(Ordering::SeqCst) || self.lock().foreground_active {
                        break;
                    }
                    let key = scope_key(visible)?;
                    if let Some(config) = &config
                        && !initialized.contains(&key)
                    {
                        self.store.begin_dense_index(visible, config)?;
                        initialized.insert(key);
                    }
                    let generation = config
                        .as_ref()
                        .map(|config| config.generation.clone())
                        .unwrap_or_default();
                    let step = self.indexer.step(
                        visible,
                        &cancelled,
                        Some(&generation),
                        Some(&wait_until_ready),
                    )?;
                    worked = worked || step.worked;
                    if let Some(config) = &config {
                        let active =
                            self.store
                                .dense_index(visible, "", RecallPolicy::Strict, false)?;
                        if active.is_none_or(|active| active.generation != config.generation) {
                            let _ = self
                                .store
                                .activate_dense_index(visible, &config.generation)?;
                        }
                    }
                    let mut guard = self.lock();
                    if step.completed {
                        guard.status.completed_jobs += 1;
                    }
                    if !step.error.is_empty() {
                        guard.status.last_error = step.error;
                    }
                }
                Ok(())
            })();
            if let Err(error) = outcome {
                self.record_error(error);
            }
            let mut guard = self.lock();
            guard.status.running = false;
            if !worked && !self.stopping.load(Ordering::SeqCst) {
                let _ = self
                    .condition
                    .wait_timeout(guard, Duration::from_millis(500))
                    .unwrap_or_else(|poisoned| poisoned.into_inner());
            }
        }
        self.lock().status.running = false;
    }
}

impl RetrievalEngine {
    pub fn new(
        store: Arc<MemoryStore>,
        scope: MemoryScope,
        options: RetrievalEngineOptions,
    ) -> Result<Self> {
        validate_scope(&scope)?;
        let inner = Arc::new(Inner {
            indexer: RetrievalIndexer::new(Arc::clone(&store), options.embedder.clone()),
            dense: DenseRetrievalService::new(
                Arc::clone(&store),
                options.embedder.clone(),
                options.cache_entries,
            ),
            store,
            state: Mutex::new(State {
                scope,
                status: RetrievalEngineStatus {
                    enabled: options.enabled,
                    ..Default::default()
                },
                foreground_active: false,
            }),
            condition: Condvar::new(),
            stopping: AtomicBool::new(false),
            options,
        });
        let worker = if inner.options.enabled && inner.options.background_indexing {
            let worker_inner = Arc::clone(&inner);
            Some(
                std::thread::Builder::new()
                    .name("qt-memory-retrieval".into())
                    .spawn(move || worker_inner.run())
                    .map_err(|error| {
                        MemoryError::storage(format!("unable to start retrieval worker: {error}"))
                    })?,
            )
        } else {
            None
        };
        Ok(Self {
            inner,
            worker: Mutex::new(worker),
        })
    }

    pub fn options(&self) -> &RetrievalEngineOptions {
        &self.inner.options
    }

    /// Stops the worker and waits for it.
    pub fn stop(&self) {
        self.inner.stopping.store(true, Ordering::SeqCst);
        {
            let _guard = self.inner.lock();
            self.inner.condition.notify_all();
        }
        let handle = self
            .worker
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .take();
        if let Some(handle) = handle {
            let _ = handle.join();
        }
    }

    pub fn set_scope(&self, scope: MemoryScope) -> Result<()> {
        validate_scope(&scope)?;
        self.inner.lock().scope = scope;
        self.inner.condition.notify_all();
        Ok(())
    }

    /// Pauses background indexing while the foreground agent is active.
    pub fn set_foreground_active(&self, active: bool) {
        self.inner.lock().foreground_active = active;
        self.inner.condition.notify_all();
    }

    fn stopped(&self) -> bool {
        self.inner.stopping.load(Ordering::SeqCst)
    }

    /// Dense memory candidates with their captured hit evidence.
    pub fn search_candidates(
        &self,
        scope: &MemoryScope,
        query: &str,
        policy: RecallPolicy,
        include_global_user: bool,
        limit: usize,
        cancelled: Cancellation<'_>,
    ) -> Vec<DenseMemoryCandidate> {
        let options = &self.inner.options;
        if !options.enabled || !options.dense_enabled || self.stopped() {
            return Vec::new();
        }
        let cancel = || self.stopped() || cancelled();
        if cancel() {
            return Vec::new();
        }
        if let Err(error) = self.inner.prepare_embedding(&cancel) {
            self.inner.record_error(error);
            return Vec::new();
        }
        let hits =
            match self
                .inner
                .dense
                .search(scope, query, policy, include_global_user, limit, &cancel)
            {
                Ok(hits) => hits,
                Err(error) => {
                    self.inner.record_error(error);
                    return Vec::new();
                }
            };
        if cancel() {
            return Vec::new();
        }
        self.inner
            .store
            .resolve_dense_memory_candidates(scope, &hits, policy, include_global_user)
            .unwrap_or_else(|error| {
                self.inner.record_error(error);
                Vec::new()
            })
    }

    pub fn status(&self) -> RetrievalEngineStatus {
        let (mut result, scope) = {
            let guard = self.inner.lock();
            (guard.status.clone(), guard.scope.clone())
        };
        let options = &self.inner.options;
        if let Ok(scopes) = recall_scopes(&scope, options.policy, options.include_global_user) {
            for visible in scopes {
                if let Ok(jobs) = self.inner.store.retrieval_index_status(&visible) {
                    result.jobs.add(&jobs);
                }
            }
        }
        result.queries = self.inner.dense.stats();
        result.code.enabled = options.code_enabled && options.code.is_some();
        if result.code.enabled
            && let Some(snapshot) = self.inner.snapshot()
        {
            result.code.available = true;
            result.code.stale = snapshot.stale;
            result.code.generation = snapshot.generation;
            if let Some(node) = snapshot.nodes.first() {
                result.code.repository = node.repository.clone();
                result.code.worktree = node.worktree.clone();
            }
            if let Ok(jobs) = self
                .inner
                .store
                .retrieval_index_status(&code_scope(&scope, &snapshot))
            {
                result.jobs.add(&jobs);
            }
        }
        result
    }

    /// Whether a pinned code reference still matches the current snapshot,
    /// the caller's repository identity and the file on disk.
    pub fn validate_code(&self, caller: &MemoryScope, reference: &RetrievalReference) -> bool {
        if reference.kind != RetrievalOwnerKind::Code {
            return false;
        }
        {
            let guard = self.inner.lock();
            let scope = &guard.scope;
            if caller.user != scope.user
                || caller.organization != scope.organization
                || caller.project != scope.project
                || caller.repository != scope.repository
                || caller.branch != scope.branch
            {
                return false;
            }
        }
        let Some(snapshot) = self.inner.snapshot() else {
            return false;
        };
        if snapshot.stale || reference.snapshot_generation != snapshot.generation {
            return false;
        }
        let Some(node) = snapshot.nodes.iter().find(|node| node.id == reference.id) else {
            return false;
        };
        reference.repository == node.repository
            && reference.worktree == node.worktree
            && reference.path == node.path
            && reference.source_fingerprint == node.source_fingerprint
            && !node.path.is_empty()
            && source_fingerprint(&snapshot.root.join(&node.path)) == node.source_fingerprint
    }

    /// Whether a captured path edge still matches stored relations,
    /// assertions or code-index edges.
    pub fn validate_relation(
        &self,
        scope: &MemoryScope,
        edge: &RetrievalPathEdge,
        policy: RecallPolicy,
        include_global_user: bool,
    ) -> bool {
        self.validate_relation_inner(scope, edge, policy, include_global_user)
            .unwrap_or(false)
    }

    fn validate_relation_inner(
        &self,
        scope: &MemoryScope,
        edge: &RetrievalPathEdge,
        policy: RecallPolicy,
        include_global_user: bool,
    ) -> Result<bool> {
        let store = &self.inner.store;
        if edge.resolution != "resolved" || edge.evidence_id.is_empty() {
            return Ok(false);
        }
        if edge.heuristic
            && (edge.assertion.is_some()
                || edge.from.kind != RetrievalOwnerKind::Code
                || edge.to.kind != RetrievalOwnerKind::Code)
        {
            return Ok(false);
        }
        for endpoint in [&edge.from, &edge.to] {
            if endpoint.kind != RetrievalOwnerKind::Entity {
                continue;
            }
            let entities =
                store.find_entities(scope, &endpoint.id, policy, include_global_user, 2)?;
            if entities.len() != 1 || entities[0].id != endpoint.id {
                return Ok(false);
            }
        }
        if let Some(captured) = &edge.assertion {
            let Some(owner) =
                store.find_for_recall(scope, &captured.memory_id, policy, include_global_user)?
            else {
                return Ok(false);
            };
            if captured.revision < 1
                || store.retrieval_revision(&owner.scope, &captured.memory_id)? != captured.revision
            {
                return Ok(false);
            }
            if !eligible_for_context(
                store,
                scope,
                &captured.memory_id,
                policy,
                include_global_user,
            ) {
                return Ok(false);
            }
            let Some(actual) = store.assertion(&owner.scope, &captured.memory_id, None)? else {
                return Ok(false);
            };
            if actual.revision != captured.revision
                || actual.evidence.is_empty()
                || actual.subject_id != captured.subject_id
                || actual.object_id != captured.object_id
                || actual.predicate != captured.predicate
                || actual.positive != captured.positive
                || actual.literal_json != captured.literal_json
                || actual.qualifiers_json != captured.qualifiers_json
                || actual.valid_from != captured.valid_from
                || actual.valid_to != captured.valid_to
                || actual.evidence != captured.evidence
            {
                return Ok(false);
            }
            if !actual.valid_at(now_ms()) || edge.evidence_id != captured.evidence_id() {
                return Ok(false);
            }
            let (from, to) = (&edge.from, &edge.to);
            return Ok(match (from.kind, to.kind) {
                (RetrievalOwnerKind::Memory, RetrievalOwnerKind::Entity) => {
                    from.id == captured.memory_id
                        && from.revision == captured.revision
                        && ((edge.relation == "assertion_subject" && to.id == captured.subject_id)
                            || (edge.relation == "assertion_object"
                                && captured.positive
                                && to.id == captured.object_id))
                }
                (RetrievalOwnerKind::Entity, RetrievalOwnerKind::Memory) => {
                    captured.object_id.is_empty()
                        && from.id == captured.subject_id
                        && to.id == captured.memory_id
                        && to.revision == captured.revision
                        && edge.relation == captured.predicate
                }
                (RetrievalOwnerKind::Entity, RetrievalOwnerKind::Entity) => {
                    from.id == captured.subject_id
                        && to.id == captured.object_id
                        && edge.relation == captured.predicate
                }
                _ => false,
            });
        }
        for link in store.links(scope, &edge.from, policy, include_global_user, 256)? {
            if !edge.heuristic
                && link.relation == edge.relation
                && link.evidence_id == edge.evidence_id
                && same_reference(&link.from, &edge.from)
                && same_reference(&link.to, &edge.to)
            {
                return Ok(true);
            }
        }
        if edge.from.kind == RetrievalOwnerKind::Memory
            && edge.to.kind == RetrievalOwnerKind::Memory
        {
            let query = GraphQuery {
                memory_id: edge.from.id.clone(),
                edge_type: edge.relation.clone(),
                limit: 256,
                ..Default::default()
            };
            for stored in store.query_edges(scope, &query)? {
                let evidence = if stored.id.is_empty() {
                    stored.source.clone()
                } else {
                    format!("graph_edge:{}", stored.id)
                };
                if stored.from_id == edge.from.id
                    && stored.to_id == edge.to.id
                    && stored.edge_type == edge.relation
                    && evidence == edge.evidence_id
                {
                    return Ok(true);
                }
            }
        }
        if edge.from.kind == RetrievalOwnerKind::Code
            && edge.to.kind == RetrievalOwnerKind::Code
            && self.validate_code(scope, &edge.from)
            && self.validate_code(scope, &edge.to)
        {
            let Some(snapshot) = self.inner.snapshot() else {
                return Ok(false);
            };
            if snapshot.stale {
                return Ok(false);
            }
            for stored in &snapshot.edges {
                let evidence = format!(
                    "code-snapshot:{}:{}:{}",
                    snapshot.generation, stored.caller_id, stored.relation
                );
                if stored.caller_id == edge.from.id
                    && stored.relation == edge.relation
                    && !stored.ambiguous
                    && stored.heuristic == edge.heuristic
                    && evidence == edge.evidence_id
                    && stored.callee_ids.iter().any(|id| id == &edge.to.id)
                {
                    return Ok(true);
                }
            }
        }
        Ok(false)
    }

    /// Code recall: lexical, dense and graph-linked code owners fused with
    /// reciprocal ranks (k=60). Memory and entity seeds let validated memory
    /// paths reach code; mixed paths are returned for atomic rendering.
    #[allow(clippy::too_many_arguments)]
    pub fn search_code(
        &self,
        scope: &MemoryScope,
        query: &str,
        limit: usize,
        cancelled: Cancellation<'_>,
        memory_seeds: &[RetrievalReference],
        contextual_seeds: &[RetrievalCandidate],
        access: Option<(RecallPolicy, bool)>,
    ) -> Vec<RetrievalCandidate> {
        let options = &self.inner.options;
        let store = &self.inner.store;
        let (policy, include_global_user) =
            access.unwrap_or((options.policy, options.include_global_user));
        let limit = limit.min(1000);
        if !options.code_enabled || options.code.is_none() || limit == 0 || query.is_empty() {
            return Vec::new();
        }
        let cancel = || self.stopped() || cancelled();
        if cancel() {
            return Vec::new();
        }
        let Some(snapshot) = self.inner.snapshot() else {
            return Vec::new();
        };
        if snapshot.stale {
            return Vec::new();
        }
        let mut result: Vec<RetrievalCandidate> = Vec::new();
        let mut owners: HashMap<String, usize> = HashMap::new();
        let merge_ranks = |target: &mut RetrievalCandidate, source: &RetrievalCandidate| {
            for rank in &source.ranks {
                match target
                    .ranks
                    .iter_mut()
                    .find(|value| value.retriever == rank.retriever)
                {
                    None => target.ranks.push(rank.clone()),
                    Some(prior) if rank.rank < prior.rank => *prior = rank.clone(),
                    Some(_) => {}
                }
            }
        };
        let add = |result: &mut Vec<RetrievalCandidate>,
                   owners: &mut HashMap<String, usize>,
                   mut candidate: RetrievalCandidate|
         -> bool {
            if !self.validate_code(scope, &candidate.owner) {
                return false;
            }
            let owner_key = candidate.owner.key();
            if let Some(index) = owners.get(&owner_key) {
                let prior = &mut result[*index];
                merge_ranks(prior, &candidate);
                for hit in &candidate.matched_chunks {
                    let duplicate = prior.matched_chunks.iter().any(|existing| {
                        existing.chunk.id == hit.chunk.id
                            && existing.generation == hit.generation
                            && existing.chunk.owner_revision == hit.chunk.owner_revision
                    });
                    if !duplicate {
                        prior.matched_chunks.push(hit.clone());
                    }
                }
                if prior.path.is_empty() {
                    prior.path = std::mem::take(&mut candidate.path);
                    prior.evidence_ids = std::mem::take(&mut candidate.evidence_ids);
                }
                return true;
            }
            let Some(snippet) = snapshot
                .read_snippet(&candidate.owner.id)
                .filter(|snippet| snippet.current)
            else {
                return false;
            };
            candidate.chunks = vec![RetrievalChunk {
                id: snippet.chunk.id.clone(),
                owner_key: candidate.owner.id.clone(),
                owner_revision: candidate.owner.revision,
                ordinal: 0,
                content_hash: stable_hash_hex(&snippet.chunk.text),
                text: snippet.chunk.text.clone(),
                source_uri: candidate.owner.path.clone(),
                source_span: format!("{}:{}", snippet.chunk.first_line, snippet.chunk.last_line),
                render_version: "code-window-v1".into(),
            }];
            candidate.eligible = true;
            owners.insert(owner_key, result.len());
            result.push(candidate);
            true
        };
        if let Some(code) = &options.code {
            // Providers may return several chunks of one owner; bound the raw
            // examination separately from the distinct-owner budget.
            let matches = code.lexical(query, 1000);
            let mut lexical_owners: HashSet<String> = HashSet::new();
            for reference in matches.iter().take(1000) {
                if lexical_owners.len() >= limit {
                    break;
                }
                if cancel() {
                    return Vec::new();
                }
                if lexical_owners.contains(&reference.id) {
                    continue;
                }
                let candidate = RetrievalCandidate {
                    owner: reference.clone(),
                    ranks: vec![RetrievalRank {
                        retriever: "code_bm25".into(),
                        rank: lexical_owners.len() + 1,
                        score: 0.0,
                    }],
                    ..Default::default()
                };
                if add(&mut result, &mut owners, candidate) {
                    lexical_owners.insert(reference.id.clone());
                }
            }
        }
        if options.dense_enabled {
            let dense = (|| -> Result<Vec<RetrievalCandidate>> {
                self.inner.prepare_embedding(&cancel)?;
                let indexed_scope = code_scope(scope, &snapshot);
                let hits = self.inner.dense.search(
                    &indexed_scope,
                    query,
                    RecallPolicy::Strict,
                    false,
                    limit,
                    &cancel,
                )?;
                store.resolve_code_hits(&indexed_scope, &hits)
            })();
            match dense {
                Ok(candidates) => {
                    for candidate in candidates {
                        add(&mut result, &mut owners, candidate);
                    }
                }
                Err(error) => self.inner.record_error(error),
            }
        }
        if options.code_graph_enabled {
            match self.code_graph(
                scope,
                &snapshot,
                &result,
                memory_seeds,
                contextual_seeds,
                limit,
                policy,
                include_global_user,
                &cancel,
            ) {
                Ok(None) => return Vec::new(),
                Ok(Some(graph_candidates)) => {
                    for candidate in graph_candidates {
                        if candidate.owner.kind == RetrievalOwnerKind::Code {
                            add(&mut result, &mut owners, candidate);
                        } else {
                            self.add_graph_memory(
                                &mut result,
                                &mut owners,
                                candidate,
                                scope,
                                policy,
                                include_global_user,
                            );
                        }
                    }
                }
                Err(error) => self.inner.record_error(error),
            }
        }
        let current = self.inner.snapshot();
        if cancel() || current.is_none_or(|current| !Arc::ptr_eq(&current, &snapshot)) {
            return Vec::new();
        }
        for candidate in &mut result {
            candidate.fused_score = candidate
                .ranks
                .iter()
                .map(|rank| 1.0 / (60.0 + rank.rank as f64))
                .sum();
        }
        result.sort_by(|a, b| {
            b.fused_score
                .partial_cmp(&a.fused_score)
                .unwrap_or(std::cmp::Ordering::Equal)
                .then_with(|| a.owner.id.cmp(&b.owner.id))
        });
        result.truncate(limit);
        result
    }

    fn add_graph_memory(
        &self,
        result: &mut Vec<RetrievalCandidate>,
        owners: &mut HashMap<String, usize>,
        candidate: RetrievalCandidate,
        scope: &MemoryScope,
        policy: RecallPolicy,
        include_global_user: bool,
    ) {
        let store = &self.inner.store;
        let visible = match candidate.owner.kind {
            RetrievalOwnerKind::Memory => store
                .find_for_recall(scope, &candidate.owner.id, policy, include_global_user)
                .ok()
                .flatten()
                .is_some_and(|memory| {
                    matches!(
                        memory.status,
                        MemoryStatus::Active | MemoryStatus::Validated
                    )
                }),
            RetrievalOwnerKind::Entity => store
                .find_entities(scope, &candidate.owner.id, policy, include_global_user, 2)
                .is_ok_and(|entities| entities.len() == 1 && entities[0].id == candidate.owner.id),
            RetrievalOwnerKind::Code => false,
        };
        if !visible {
            return;
        }
        let owner_key = candidate.owner.key();
        if let Some(index) = owners.get(&owner_key) {
            let prior = &mut result[*index];
            for rank in &candidate.ranks {
                match prior
                    .ranks
                    .iter_mut()
                    .find(|value| value.retriever == rank.retriever)
                {
                    None => prior.ranks.push(rank.clone()),
                    Some(existing) if rank.rank < existing.rank => *existing = rank.clone(),
                    Some(_) => {}
                }
            }
            if prior.path.is_empty() || candidate.path.len() < prior.path.len() {
                prior.path = candidate.path;
                prior.evidence_ids = candidate.evidence_ids;
            }
            return;
        }
        owners.insert(owner_key, result.len());
        result.push(candidate);
    }

    /// Graph-linked candidates; `None` when cancelled.
    #[allow(clippy::too_many_arguments)]
    fn code_graph(
        &self,
        scope: &MemoryScope,
        snapshot: &Arc<CodeSnapshot>,
        result: &[RetrievalCandidate],
        memory_seeds: &[RetrievalReference],
        contextual_seeds: &[RetrievalCandidate],
        limit: usize,
        policy: RecallPolicy,
        include_global_user: bool,
        cancel: Cancellation<'_>,
    ) -> Result<Option<Vec<RetrievalCandidate>>> {
        let options = &self.inner.options;
        let store = &self.inner.store;
        let mut identity = RetrievalGraphIdentity::default();
        if let Some(source) = result
            .iter()
            .find(|item| item.owner.kind == RetrievalOwnerKind::Code)
        {
            identity.repository = source.owner.repository.clone();
            identity.worktree = source.owner.worktree.clone();
        }
        if identity.repository.is_empty()
            && let Some(node) = snapshot.nodes.first()
        {
            identity.repository = node.repository.clone();
            identity.worktree = node.worktree.clone();
        }
        let authorizer = |visible: &MemoryScope, reference: &RetrievalReference| -> bool {
            if reference.kind != RetrievalOwnerKind::Memory {
                return true;
            }
            let Ok(Some(memory)) =
                store.find_for_recall(visible, &reference.id, policy, include_global_user)
            else {
                return false;
            };
            (reference.revision == 0
                || store
                    .retrieval_revision(&memory.scope, &reference.id)
                    .is_ok_and(|revision| revision == reference.revision))
                && eligible_for_context(store, visible, &reference.id, policy, include_global_user)
        };
        let graph = RetrievalGraphAdapter::new(store, Some(Arc::clone(snapshot)), identity)
            .with_authorizer(&authorizer);
        let mut graph_query = RetrievalGraphQuery {
            policy,
            include_global_user,
            max_depth: options.graph_max_depth.min(8),
            max_fanout: options.graph_max_fanout.min(256),
            max_edges: options.graph_max_edges.min(8192),
            max_results: (limit + 16).min(options.graph_max_results).min(1000),
            allowed_relations: CODE_GRAPH_RELATIONS
                .iter()
                .map(|item| item.to_string())
                .collect(),
            ..Default::default()
        };
        let seed_limit = options.graph_max_seeds.min(128);
        let mut paths: HashMap<String, Vec<RetrievalPathEdge>> = HashMap::new();
        // Interleave independent code and memory seeds so neither modality
        // exhausts the shared seed budget.
        let mut index = 0usize;
        while graph_query.seeds.len() < seed_limit
            && (index < result.len()
                || index < memory_seeds.len()
                || index < contextual_seeds.len())
        {
            if let Some(item) = result.get(index)
                && item.owner.kind == RetrievalOwnerKind::Code
            {
                graph_query.seeds.push(RetrievalGraphSeed {
                    reference: item.owner.clone(),
                    depth: 0,
                });
            }
            if graph_query.seeds.len() < seed_limit
                && let Some(seed) = memory_seeds.get(index)
                && matches!(
                    seed.kind,
                    RetrievalOwnerKind::Memory | RetrievalOwnerKind::Entity
                )
            {
                graph_query.seeds.push(RetrievalGraphSeed {
                    reference: seed.clone(),
                    depth: 0,
                });
            }
            if graph_query.seeds.len() < seed_limit
                && let Some(seed) = contextual_seeds.get(index)
            {
                if cancel() {
                    return Ok(None);
                }
                if self.contextual_seed(
                    scope,
                    seed,
                    &mut graph_query,
                    &mut paths,
                    policy,
                    include_global_user,
                ) {
                    graph_query.seeds.push(RetrievalGraphSeed {
                        reference: seed.owner.clone(),
                        depth: seed.path.len(),
                    });
                }
            }
            index += 1;
        }
        if cancel() {
            return Ok(None);
        }
        let graph_result = graph.traverse(scope, &graph_query)?;
        let pin = |mut reference: RetrievalReference| -> RetrievalReference {
            if reference.kind == RetrievalOwnerKind::Memory
                && let Ok(Some(memory)) =
                    store.find_for_recall(scope, &reference.id, policy, include_global_user)
                && let Ok(revision) = store.retrieval_revision(&memory.scope, &reference.id)
            {
                reference.revision = revision;
            }
            reference
        };
        for seed in &graph_query.seeds {
            paths.entry(seed.reference.key()).or_default();
        }
        let mut graph_candidates: Vec<RetrievalCandidate> = Vec::new();
        for edge in &graph_result.edges {
            if cancel() {
                return Ok(None);
            }
            let Some(parent_path) = paths.get(&edge.from.key()).cloned() else {
                continue;
            };
            if edge.resolution != "resolved" {
                continue;
            }
            for target in &edge.candidates {
                if parent_path.iter().any(|prior| {
                    prior.from.key() == target.key()
                        || prior.to.key() == target.key()
                        || (prior.assertion.is_some()
                            && target.kind == RetrievalOwnerKind::Memory
                            && prior
                                .assertion
                                .as_ref()
                                .is_some_and(|assertion| assertion.memory_id == target.id))
                }) {
                    continue;
                }
                if let Some(existing) = paths.get(&target.key())
                    && (!existing.is_empty()
                        || !parent_path.is_empty()
                        || target.kind == edge.from.kind)
                {
                    continue;
                }
                let mut path = parent_path.clone();
                let (from, to) = if edge.incoming {
                    (target.clone(), edge.from.clone())
                } else {
                    (edge.from.clone(), target.clone())
                };
                path.push(RetrievalPathEdge {
                    from: pin(from),
                    to: pin(to),
                    relation: edge.relation.clone(),
                    evidence_id: edge.evidence_id.clone(),
                    resolution: edge.resolution.clone(),
                    assertion: edge.assertion.clone(),
                    heuristic: edge.heuristic,
                });
                if edge.assertion_positive {
                    paths.entry(target.key()).or_insert_with(|| path.clone());
                }
                let mut has_code = false;
                let mut has_memory = false;
                let mut has_entity = false;
                for hop in &path {
                    for kind in [hop.from.kind, hop.to.kind] {
                        has_code |= kind == RetrievalOwnerKind::Code;
                        has_memory |= kind == RetrievalOwnerKind::Memory;
                        has_entity |= kind == RetrievalOwnerKind::Entity;
                    }
                    has_memory |= hop.assertion.is_some();
                }
                let mut linked = RetrievalCandidate {
                    owner: pin(target.clone()),
                    eligible: true,
                    ranks: vec![RetrievalRank {
                        retriever: "code_graph".into(),
                        rank: graph_candidates.len() + 1,
                        score: 0.0,
                    }],
                    ..Default::default()
                };
                if has_code && (has_memory || has_entity) {
                    linked.path = path;
                }
                linked.evidence_ids = linked
                    .path
                    .iter()
                    .filter(|hop| !hop.evidence_id.is_empty())
                    .map(|hop| hop.evidence_id.clone())
                    .collect();
                // Intermediate code nodes supply the windows a longer path
                // needs; pure-memory paths stay with the recall service.
                if linked.owner.kind == RetrievalOwnerKind::Code || !linked.path.is_empty() {
                    graph_candidates.push(linked);
                }
            }
        }
        Ok(Some(graph_candidates))
    }

    /// Validates a contextual (assertion-path) seed and reserves its budget.
    fn contextual_seed(
        &self,
        scope: &MemoryScope,
        seed: &RetrievalCandidate,
        graph_query: &mut RetrievalGraphQuery,
        paths: &mut HashMap<String, Vec<RetrievalPathEdge>>,
        policy: RecallPolicy,
        include_global_user: bool,
    ) -> bool {
        if !seed.eligible
            || seed.path.is_empty()
            || seed.path.len() >= graph_query.max_depth
            || seed.path.len() > graph_query.max_edges
            || !matches!(
                seed.owner.kind,
                RetrievalOwnerKind::Memory | RetrievalOwnerKind::Entity
            )
        {
            return false;
        }
        let mut valid = false;
        let mut prefix_nodes: HashSet<String> = HashSet::new();
        for edge in &seed.path {
            if !self.validate_relation(scope, edge, policy, include_global_user) {
                return false;
            }
            valid = edge
                .assertion
                .as_ref()
                .is_some_and(|assertion| match seed.owner.kind {
                    RetrievalOwnerKind::Memory => assertion.memory_id == seed.owner.id,
                    _ => edge.from.id == seed.owner.id || edge.to.id == seed.owner.id,
                });
            if edge
                .assertion
                .as_ref()
                .is_some_and(|assertion| !assertion.positive)
            {
                return false;
            }
            prefix_nodes.insert(edge.from.key());
            prefix_nodes.insert(edge.to.key());
            if let Some(assertion) = &edge.assertion {
                prefix_nodes.insert(
                    RetrievalReference::new(RetrievalOwnerKind::Memory, &assertion.memory_id).key(),
                );
            }
        }
        if !valid {
            return false;
        }
        prefix_nodes.remove(&seed.owner.key());
        if prefix_nodes.len() >= graph_query.max_results {
            return false;
        }
        graph_query.max_results -= prefix_nodes.len();
        graph_query.max_edges -= seed.path.len();
        paths.insert(seed.owner.key(), seed.path.clone());
        true
    }
}

impl Drop for RetrievalEngine {
    fn drop(&mut self) {
        self.stop();
    }
}
