mod common;

use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};

use common::*;
use qt_memory::code::{
    CodeChunk, CodeEdge, CodeNode, CodeNodeKind, CodeSnapshot, CodeSnapshotProvider,
    source_fingerprint,
};
use qt_memory::recall::GraphRecallOptions;
use qt_memory::retrieval::*;
use qt_memory::*;

/// Deterministic bag-of-words embedder (16 dimensions).
struct HashEmbedder {
    calls: AtomicUsize,
}

impl HashEmbedder {
    fn new() -> Self {
        Self {
            calls: AtomicUsize::new(0),
        }
    }
}

fn embed_text(text: &str) -> Vec<f32> {
    let mut values = vec![0.01f32; 16];
    for word in text
        .split(|ch: char| !ch.is_ascii_alphanumeric())
        .filter(|word| word.len() > 2)
    {
        let hash = qt_memory::text::stable_hash64(&word.to_ascii_lowercase());
        values[(hash % 16) as usize] += 1.0;
    }
    values
}

impl Embedder for HashEmbedder {
    fn configure(&self, _cancelled: Cancellation<'_>) -> qt_memory::Result<DenseIndexConfig> {
        Ok(DenseIndexConfig {
            generation: "dense-test".into(),
            model_id: "hash".into(),
            model_fingerprint: "fp".into(),
            tokenizer_fingerprint: "fp".into(),
            pooling: "mean".into(),
            dimensions: 16,
            normalize: true,
            ..Default::default()
        })
    }

    fn embed(
        &self,
        _config: &DenseIndexConfig,
        text: &str,
        _cancelled: Cancellation<'_>,
    ) -> qt_memory::Result<Vec<f32>> {
        self.calls.fetch_add(1, Ordering::SeqCst);
        Ok(embed_text(text))
    }
}

fn index_everything(
    store: &Arc<MemoryStore>,
    scope: &MemoryScope,
    embedder: Option<Arc<dyn Embedder>>,
) -> usize {
    let indexer = RetrievalIndexer::new(Arc::clone(store), embedder.clone());
    let generation = embedder
        .as_ref()
        .map(|embedder| embedder.configure(&never_cancelled).unwrap().generation)
        .unwrap_or_default();
    let mut completed = 0;
    for _ in 0..100 {
        let step = indexer
            .step(scope, &never_cancelled, Some(&generation), None)
            .unwrap();
        assert!(step.error.is_empty(), "{}", step.error);
        if !step.worked {
            break;
        }
        completed += usize::from(step.completed);
    }
    completed
}

#[test]
fn chunk_jobs_leases_and_rebuild() {
    let fixture = Fixture::new();
    let scope = scope();
    let id = fixture.remember(&scope, "indexable memory text", MemoryStatus::Active);
    let status = fixture.store.retrieval_index_status(&scope).unwrap();
    assert_eq!(status.pending, 1);
    let job = fixture
        .store
        .claim_index_job(&scope, 30, None)
        .unwrap()
        .unwrap();
    assert_eq!(job.owner_key, format!("memory:{id}"));
    assert_eq!(job.attempts, 1);
    assert!(
        fixture
            .store
            .claim_index_job(&scope, 30, None)
            .unwrap()
            .is_none(),
        "leased"
    );
    assert!(fixture.store.renew_index_job(&scope, &job, 60).unwrap());
    assert!(
        fixture
            .store
            .fail_index_job(&scope, &job, "boom", 5)
            .unwrap()
    );
    assert_eq!(
        fixture
            .store
            .retrieval_index_status(&scope)
            .unwrap()
            .pending,
        1
    );
    let mut stale = job.clone();
    stale.lease_token = "lease_other".into();
    assert!(
        !fixture
            .store
            .publish_chunks(&scope, &stale, IndexChunks::one())
            .unwrap()
    );

    assert_eq!(
        index_everything(&fixture.store, &scope, None),
        0,
        "retry waits for backoff"
    );
    let report = fixture.store.rebuild_retrieval(&scope, None).unwrap();
    assert_eq!(report.owners, 1);
    assert_eq!(report.jobs_enqueued, 1);
    assert_eq!(index_everything(&fixture.store, &scope, None), 1);
    let chunks = fixture
        .store
        .retrieval_chunks(&scope, &format!("memory:{id}"))
        .unwrap();
    assert_eq!(chunks.len(), 1);
    assert_eq!(chunks[0].text, "indexable memory text");
    assert_eq!(chunks[0].source_span, "bytes:0:21");
    assert_eq!(
        fixture.store.retrieval_index_status(&scope).unwrap().chunks,
        1
    );
    assert!(
        fixture
            .store
            .rebuild_retrieval(&scope, Some("memory:missing"))
            .is_err()
    );
}

struct IndexChunks;

impl IndexChunks {
    fn one() -> Vec<RetrievalChunk> {
        vec![RetrievalChunk {
            text: "x".into(),
            ..Default::default()
        }]
    }
}

#[test]
fn dense_index_search_and_recall_fusion() {
    let fixture = Fixture::new();
    let scope = scope();
    let ripgrep = fixture.remember(
        &scope,
        "ripgrep searches files quickly",
        MemoryStatus::Active,
    );
    fixture.remember(&scope, "coffee brewing guide", MemoryStatus::Active);
    let embedder = Arc::new(HashEmbedder::new());
    let config = embedder.configure(&never_cancelled).unwrap();
    index_everything(&fixture.store, &scope, None);
    fixture.store.begin_dense_index(&scope, &config).unwrap();
    assert!(
        fixture
            .store
            .dense_index(&scope, "", RecallPolicy::Balanced, false)
            .unwrap()
            .is_none()
    );
    let mut conflicting = config.clone();
    conflicting.dimensions = 8;
    assert!(
        fixture
            .store
            .begin_dense_index(&scope, &conflicting)
            .is_err()
    );
    assert_eq!(
        index_everything(&fixture.store, &scope, Some(embedder.clone())),
        2
    );
    let active = fixture
        .store
        .dense_index(&scope, "", RecallPolicy::Balanced, false)
        .unwrap()
        .unwrap();
    assert_eq!(active.generation, "dense-test");

    let dense = DenseRetrievalService::new(Arc::clone(&fixture.store), Some(embedder.clone()), 8);
    let hits = dense
        .search(
            &scope,
            "ripgrep files",
            RecallPolicy::Balanced,
            false,
            5,
            &never_cancelled,
        )
        .unwrap();
    assert_eq!(hits.len(), 2);
    assert_eq!(hits[0].chunk.owner_key, format!("memory:{ripgrep}"));
    assert!(hits[0].similarity > hits[1].similarity);
    let calls = embedder.calls.load(Ordering::SeqCst);
    dense
        .search(
            &scope,
            "ripgrep files",
            RecallPolicy::Balanced,
            false,
            5,
            &never_cancelled,
        )
        .unwrap();
    assert_eq!(
        embedder.calls.load(Ordering::SeqCst),
        calls,
        "query cache hit"
    );
    assert_eq!(dense.stats().cache_hits, 1);
    assert_eq!(dense.stats().cache_entries, 1);

    let engine = Arc::new(
        RetrievalEngine::new(
            Arc::clone(&fixture.store),
            scope.clone(),
            RetrievalEngineOptions {
                enabled: true,
                background_indexing: false,
                dense_enabled: true,
                embedder: Some(embedder.clone()),
                ..Default::default()
            },
        )
        .unwrap(),
    );
    let candidates = engine.search_candidates(
        &scope,
        "ripgrep",
        RecallPolicy::Balanced,
        false,
        5,
        &never_cancelled,
    );
    assert_eq!(candidates[0].memory.id, ripgrep);
    let provider_engine = Arc::clone(&engine);
    let recall = RecallService::new(Arc::clone(&fixture.store)).with_dense(Arc::new(
        move |scope, request| {
            provider_engine.search_candidates(
                scope,
                &request.query,
                request.policy,
                request.include_global_user,
                request.candidate_limit,
                &never_cancelled,
            )
        },
    ));
    let results = recall
        .recall(
            &scope,
            &RecallRequest {
                query: "ripgrep searches files".into(),
                dense_enabled: true,
                ..Default::default()
            },
        )
        .unwrap();
    let item = results
        .iter()
        .find(|item| item.memory.id == ripgrep)
        .unwrap();
    assert_eq!(item.score.lexical_rank, 1);
    assert_eq!(item.score.dense_rank, 1);
    assert!(item.dense_match.is_some());
    assert!((item.score.reciprocal_rank_fusion - 2.0 / 61.0).abs() < 1e-12);
    let explanation = recalled_memory_json(
        item,
        Some(item.dense_match.as_ref().unwrap().chunk.owner_revision),
        None,
    );
    assert_eq!(explanation["revision"]["basis"], "dense_capture");
    assert_eq!(explanation["dense_match"]["revision_is_current"], true);

    // Editing the memory invalidates its vector and the captured evidence.
    fixture
        .store
        .update(
            &scope,
            &ripgrep,
            &MemoryPatch {
                content: Some("ripgrep searches directories".into()),
                ..Default::default()
            },
        )
        .unwrap();
    let context = ContextService::new(Arc::clone(&fixture.store));
    let pack = context
        .build(
            &scope,
            std::slice::from_ref(item),
            &ContextOptions::default(),
            &[],
        )
        .unwrap();
    assert!(pack.memory_ids.is_empty());
    assert_eq!(pack.omitted, 1);
}

struct StaticCode(Arc<CodeSnapshot>);

impl CodeSnapshotProvider for StaticCode {
    fn snapshot(&self) -> Option<Arc<CodeSnapshot>> {
        Some(Arc::clone(&self.0))
    }
}

fn code_snapshot(root: &std::path::Path) -> CodeSnapshot {
    std::fs::write(root.join("auth.rs"), "fn login() {\n    verify();\n}\n").unwrap();
    std::fs::write(root.join("verify.rs"), "fn verify() {}\n").unwrap();
    let node = |id: &str, path: &str, name: &str| CodeNode {
        id: id.into(),
        repository: "repo_alpha".into(),
        worktree: "wt".into(),
        path: path.into(),
        kind: CodeNodeKind::Function,
        name: name.into(),
        qualified_name: name.into(),
        signature_discriminator: String::new(),
        source_fingerprint: source_fingerprint(&root.join(path)),
        generation: 7,
        line: 1,
    };
    let login = node("code:login", "auth.rs", "login");
    let verify = node("code:verify", "verify.rs", "verify");
    let chunk = |node: &CodeNode, text: &str| CodeChunk {
        id: format!("{}:chunk", node.id),
        owner_id: node.id.clone(),
        path: node.path.clone(),
        text: text.into(),
        first_line: 1,
        last_line: 3,
        source_fingerprint: node.source_fingerprint.clone(),
        generation: 7,
        bounded_source_window: true,
    };
    CodeSnapshot {
        root: root.into(),
        generation: 7,
        stale: false,
        chunks: vec![
            chunk(&login, "fn login() {\n    verify();\n}\n"),
            chunk(&verify, "fn verify() {}\n"),
        ],
        edges: vec![CodeEdge {
            id: "edge".into(),
            caller_id: "code:login".into(),
            callee_ids: vec!["code:verify".into()],
            unresolved_name: String::new(),
            path: "auth.rs".into(),
            line: 2,
            heuristic: false,
            ambiguous: false,
            relation: "calls".into(),
        }],
        nodes: vec![login, verify],
    }
}

#[test]
fn code_recall_links_graph_and_renders_bundles() {
    let fixture = Fixture::new();
    let scope = scope();
    let source = tempfile::tempdir().unwrap();
    let snapshot = Arc::new(code_snapshot(source.path()));
    let provider: Arc<dyn CodeSnapshotProvider> = Arc::new(StaticCode(Arc::clone(&snapshot)));
    let memory = fixture.remember(
        &scope,
        "login must verify credentials",
        MemoryStatus::Active,
    );
    let login_ref = CodeSnapshot::reference(&snapshot.nodes[0]);
    fixture
        .store
        .add_cross_store_link(
            &scope,
            CrossStoreLink {
                from: RetrievalReference::memory(&memory, 0),
                to: login_ref.clone(),
                relation: "documents".into(),
                evidence_id: "uri:docs/auth.md".into(),
                id: String::new(),
            },
        )
        .unwrap();
    assert!(
        fixture
            .store
            .add_cross_store_link(
                &scope,
                CrossStoreLink {
                    from: RetrievalReference::memory(&memory, 0),
                    to: login_ref.clone(),
                    relation: "documents".into(),
                    evidence_id: "graph:x".into(),
                    id: String::new(),
                },
            )
            .is_err(),
        "links require uri evidence"
    );
    let links = fixture
        .store
        .links(&scope, &login_ref, RecallPolicy::Balanced, false, 10)
        .unwrap();
    assert_eq!(links.len(), 1);
    assert_eq!(links[0].from.revision, 1);

    let engine = RetrievalEngine::new(
        Arc::clone(&fixture.store),
        scope.clone(),
        RetrievalEngineOptions {
            enabled: true,
            background_indexing: false,
            code_enabled: true,
            code_graph_enabled: true,
            code: Some(provider),
            ..Default::default()
        },
    )
    .unwrap();
    assert!(engine.validate_code(&scope, &login_ref));
    let candidates = engine.search_code(
        &scope,
        "login",
        10,
        &never_cancelled,
        &[RetrievalReference::memory(&memory, 1)],
        &[],
        None,
    );
    let login = candidates
        .iter()
        .find(|item| item.owner.id == "code:login")
        .unwrap();
    assert!(login.ranks.iter().any(|rank| rank.retriever == "code_bm25"));
    assert_eq!(login.chunks[0].render_version, "code-window-v1");
    let verify = candidates
        .iter()
        .find(|item| item.owner.id == "code:verify")
        .unwrap();
    assert!(
        verify
            .ranks
            .iter()
            .any(|rank| rank.retriever == "code_graph")
    );
    let status = engine.status();
    assert!(status.code.enabled && status.code.available);
    assert_eq!(status.code.generation, 7);

    let validator_engine = Arc::new(engine);
    let code_engine = Arc::clone(&validator_engine);
    let relation_engine = Arc::clone(&validator_engine);
    let validator_store = Arc::clone(&fixture.store);
    let context = ContextService::new(Arc::clone(&fixture.store)).with_validators(
        Some(Arc::new(move |scope, reference| {
            code_engine.validate_code(scope, reference)
        })),
        Some(Arc::new(move |scope, id, revision, policy, global| {
            validator_store
                .find_for_recall(scope, id, policy, global)
                .ok()
                .flatten()
                .is_some_and(|memory| {
                    validator_store
                        .retrieval_revision(&memory.scope, id)
                        .unwrap()
                        == revision
                })
        })),
        Some(Arc::new(move |scope, edge, policy, global| {
            relation_engine.validate_relation(scope, edge, policy, global)
        })),
    );
    let pack = context
        .build(&scope, &[], &ContextOptions::default(), &candidates)
        .unwrap();
    assert!(
        pack.markdown.contains("[Mixed graph evidence bundle]\n"),
        "{}",
        pack.markdown
    );
    assert!(pack.markdown.contains(&format!(
        "[Relation {memory} -documents-> code:login; stored relation; evidence=uri:docs/auth.md]\n"
    )));
    assert!(
        pack.markdown
            .contains("\n## Current code excerpts\n[Code code:verify")
    );
    assert!(
        pack.markdown
            .contains("; path auth.rs; source window 1:3; generation 7; bounded excerpt]\n")
    );
    assert!(pack.code_ids.contains(&"code:login".to_string()));

    // Changing the file invalidates the snapshot's code evidence.
    std::fs::write(source.path().join("auth.rs"), "fn login() {}\n").unwrap();
    assert!(!validator_engine.validate_code(&scope, &login_ref));
    let stale = context
        .build(&scope, &[], &ContextOptions::default(), &candidates)
        .unwrap();
    assert!(!stale.code_ids.contains(&"code:login".to_string()));
    assert!(
        stale.warnings.contains(
            &"mixed graph bundle omitted: stale, disconnected or ineligible evidence".to_string()
        ),
        "{:?}",
        stale.warnings
    );
}

#[test]
fn graph_adapter_respects_budgets_and_directions() {
    let fixture = Fixture::new();
    let scope = scope();
    let a = fixture.remember(&scope, "node a", MemoryStatus::Active);
    let b = fixture.remember(&scope, "node b", MemoryStatus::Active);
    let c = fixture.remember(&scope, "node c", MemoryStatus::Candidate);
    fixture
        .store
        .add_edge(&scope, GraphEdge::new(&a, &b, "related"))
        .unwrap();
    fixture
        .store
        .add_edge(&scope, GraphEdge::new(&c, &a, "related"))
        .unwrap();
    let adapter =
        RetrievalGraphAdapter::new(&fixture.store, None, RetrievalGraphIdentity::default());
    let query = RetrievalGraphQuery {
        seeds: vec![RetrievalGraphSeed {
            reference: RetrievalReference::new(RetrievalOwnerKind::Memory, &a),
            depth: 0,
        }],
        ..Default::default()
    };
    let result = adapter.traverse(&scope, &query).unwrap();
    let ids: Vec<&str> = result.nodes.iter().map(|node| node.id.as_str()).collect();
    assert_eq!(
        ids,
        vec![a.as_str(), b.as_str()],
        "candidates are not visible graph endpoints"
    );
    assert!(
        result
            .omissions
            .contains(&"edge endpoint omitted by visibility policy".to_string())
    );
    let incoming = adapter
        .traverse(
            &scope,
            &RetrievalGraphQuery {
                direction: RetrievalGraphDirection::Incoming,
                ..query.clone()
            },
        )
        .unwrap();
    assert_eq!(incoming.nodes.len(), 1);
    let none = adapter
        .traverse(
            &scope,
            &RetrievalGraphQuery {
                max_edges: 0,
                ..query
            },
        )
        .unwrap();
    assert!(none.nodes.is_empty());
    let _ = GraphRecallOptions::default();
}

#[test]
fn background_engine_indexes_and_activates_dense_generation() {
    let fixture = Fixture::new();
    let scope = scope();
    fixture.remember(&scope, "background indexing memory", MemoryStatus::Active);
    let engine = RetrievalEngine::new(
        Arc::clone(&fixture.store),
        scope.clone(),
        RetrievalEngineOptions {
            enabled: true,
            background_indexing: true,
            dense_enabled: true,
            embedder: Some(Arc::new(HashEmbedder::new())),
            ..Default::default()
        },
    )
    .unwrap();
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(20);
    loop {
        let active = fixture
            .store
            .dense_index(&scope, "", RecallPolicy::Strict, false)
            .unwrap();
        if active.is_some() {
            break;
        }
        assert!(
            std::time::Instant::now() < deadline,
            "{:?}",
            engine.status()
        );
        std::thread::sleep(std::time::Duration::from_millis(20));
    }
    let status = engine.status();
    assert_eq!(status.generation, "dense-test");
    assert!(status.completed_jobs >= 2);
    assert_eq!(status.jobs.chunks, 1);
    engine.set_foreground_active(true);
    engine.stop();
    assert!(!engine.status().running);
}
