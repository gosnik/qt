mod common;

use std::sync::Arc;

use common::*;
use qt_memory::context::{CONTEXT_PREFIX, CONTEXT_SUFFIX};
use qt_memory::recall::{GraphRecallOptions, StructuredRecallOptions, hashed_similarity};
use qt_memory::retrieval::{AssertionEvidence, MemoryAssertion, MemoryEntity};
use qt_memory::review::ReviewItemWrite;
use qt_memory::*;

fn active(fixture: &Fixture, scope: &MemoryScope, content: &str) -> String {
    fixture.remember(scope, content, MemoryStatus::Active)
}

#[test]
fn lexical_recall_scores_with_archived_weights() {
    let fixture = Fixture::new();
    let scope = scope();
    let id = active(&fixture, &scope, "Use ripgrep for searching the repository");
    active(&fixture, &scope, "Unrelated note about coffee");
    let recall = RecallService::new(Arc::clone(&fixture.store));
    let results = recall
        .recall(
            &scope,
            &RecallRequest {
                query: "ripgrep searching".into(),
                ..Default::default()
            },
        )
        .unwrap();
    assert_eq!(results.len(), 1, "FTS keeps only matching memories");
    let item = &results[0];
    assert_eq!(item.memory.id, id);
    let similarity = hashed_similarity("ripgrep searching", &item.memory.content);
    let score = &item.score;
    assert_eq!(score.lexical, similarity);
    assert_eq!(score.scope, 1.0);
    assert_eq!(score.lexical_rank, 1);
    let expected = 0.28 * similarity
        + 0.22 * similarity
        + 0.18 * 0.5
        + 0.10 * 0.0
        + 0.08 * score.freshness
        + 0.14 * 1.0;
    assert!((score.total - expected).abs() < 1e-12);
    assert!(item.eligible_for_context);
    assert!(item.warnings.is_empty(), "{:?}", item.warnings);

    recall.record_access(&scope, &results).unwrap();
    assert_eq!(fixture.store.get(&scope, &id).unwrap().access_count, 1);
}

#[test]
fn recall_warnings_and_critical_conflicts() {
    let fixture = Fixture::new();
    let scope = scope();
    let hypothesis = fixture
        .store
        .add(
            &scope,
            MemoryWrite {
                content: "deploy with make release".into(),
                status: MemoryStatus::Hypothesis,
                confidence: 0.2,
                ..Default::default()
            },
        )
        .unwrap();
    let blocked = active(&fixture, &scope, "deploy using the staging script");
    fixture
        .store
        .add_review(
            &scope,
            ReviewItemWrite {
                kind: "conflict".into(),
                subject_id: blocked.clone(),
                candidate_json: serde_json::json!({
                    "memory_ids": [blocked], "severity": "critical", "summary": "conflict",
                })
                .to_string(),
                ..Default::default()
            },
        )
        .unwrap();
    let recall = RecallService::new(Arc::clone(&fixture.store));
    let results = recall
        .recall(
            &scope,
            &RecallRequest {
                query: "deploy".into(),
                ..Default::default()
            },
        )
        .unwrap();
    let find = |id: &str| results.iter().find(|item| item.memory.id == id).unwrap();
    let tentative = find(&hypothesis);
    assert!(
        tentative
            .warnings
            .contains(&"hypothesis; verify before relying on it".to_string())
    );
    assert!(tentative.warnings.contains(&"low confidence".to_string()));
    assert!(
        tentative
            .warnings
            .contains(&"no durable source locator".to_string())
    );
    let conflicted = find(&blocked);
    assert!(!conflicted.eligible_for_context);
    assert!(
        conflicted
            .warnings
            .contains(&"critical unresolved conflict; excluded from guidance context".to_string())
    );
    assert!(!recall.eligible_for_context(&scope, &blocked, RecallPolicy::Balanced, false));
}

#[test]
fn graph_recall_follows_edges_and_fuses_ranks() {
    let fixture = Fixture::new();
    let scope = scope();
    let root = active(&fixture, &scope, "authentication module uses tokens");
    let linked = active(&fixture, &scope, "session expiry is fifteen minutes");
    fixture
        .store
        .add_edge(&scope, GraphEdge::new(&root, &linked, "related_to"))
        .unwrap();
    let recall = RecallService::new(Arc::clone(&fixture.store));
    let results = recall
        .recall(
            &scope,
            &RecallRequest {
                query: "authentication".into(),
                graph: GraphRecallOptions {
                    enabled: true,
                    ..Default::default()
                },
                ..Default::default()
            },
        )
        .unwrap();
    assert_eq!(results.len(), 2);
    let neighbor = results
        .iter()
        .find(|item| item.memory.id == linked)
        .unwrap();
    assert_eq!(neighbor.graph_path_ids, vec![root.clone(), linked.clone()]);
    assert_eq!(neighbor.graph_path_relations, vec!["related_to"]);
    assert_eq!(neighbor.graph_path_reverse, vec![false]);
    assert_eq!(neighbor.score.graph_rank, 2);
    assert!((neighbor.score.reciprocal_rank_fusion - 1.0 / 62.0).abs() < 1e-12);
    let first = &results[0];
    assert_eq!(first.memory.id, root);
    assert!((first.score.total - (1.0 / 61.0 + 1.0 / 61.0)).abs() < 1e-12);

    let context = ContextService::new(Arc::clone(&fixture.store));
    let pack = context
        .build(&scope, &results, &ContextOptions::default(), &[])
        .unwrap();
    assert!(pack.markdown.starts_with(CONTEXT_PREFIX));
    assert!(pack.markdown.ends_with(CONTEXT_SUFFIX));
    assert!(
        pack.markdown
            .contains(&format!("[Graph bundle root {linked}]\n"))
    );
    assert!(
        pack.markdown
            .contains(&format!("[Graph relation {root} -related_to-> {linked}]\n"))
    );
    assert_eq!(pack.memory_ids, vec![root, linked]);
}

fn structured_fixture(fixture: &Fixture, scope: &MemoryScope) -> (String, String, String) {
    let memory = active(fixture, scope, "The billing service depends on postgres");
    let billing = fixture
        .store
        .add_entity(
            scope,
            MemoryEntity {
                kind: "service".into(),
                name: "billing".into(),
                aliases: vec!["billing-svc".into()],
                ..Default::default()
            },
        )
        .unwrap();
    let postgres = fixture
        .store
        .add_entity(
            scope,
            MemoryEntity {
                kind: "database".into(),
                name: "postgres".into(),
                ..Default::default()
            },
        )
        .unwrap();
    let revision = fixture.store.retrieval_revision(scope, &memory).unwrap();
    fixture
        .store
        .add_assertion(
            scope,
            MemoryAssertion {
                memory_id: memory.clone(),
                revision,
                subject_id: billing.id.clone(),
                predicate: "depends_on".into(),
                object_id: postgres.id.clone(),
                evidence: vec![AssertionEvidence {
                    source_uri: "file:docs/arch.md".into(),
                    source_hash: "abc".into(),
                    ..Default::default()
                }],
                ..Default::default()
            },
        )
        .unwrap();
    (memory, billing.id, postgres.id)
}

#[test]
fn structured_recall_resolves_aliases_and_renders_claims() {
    let fixture = Fixture::new();
    let scope = scope();
    let (memory, billing, postgres) = structured_fixture(&fixture, &scope);
    assert_eq!(
        fixture
            .store
            .find_entities(&scope, "billing-svc", RecallPolicy::Balanced, false, 10)
            .unwrap()[0]
            .id,
        billing
    );
    let recall = RecallService::new(Arc::clone(&fixture.store));
    let results = recall
        .recall(
            &scope,
            &RecallRequest {
                query: "anything unrelated".into(),
                structured: StructuredRecallOptions {
                    enabled: true,
                    subject: "billing-svc".into(),
                    ..Default::default()
                },
                graph: GraphRecallOptions {
                    enabled: true,
                    ..Default::default()
                },
                ..Default::default()
            },
        )
        .unwrap();
    let item = results
        .iter()
        .find(|item| item.memory.id == memory)
        .unwrap();
    assert_eq!(item.assertion_path.len(), 1);
    assert_eq!(item.assertion_path[0].from_entity_id, billing);
    assert_eq!(item.assertion_path[0].to_entity_id, postgres);
    let context = ContextService::new(Arc::clone(&fixture.store));
    let pack = context
        .build(&scope, &results, &ContextOptions::default(), &[])
        .unwrap();
    assert!(pack.markdown.contains(
        "[Assertion evidence path; individual stored claims, not a transitive inferred fact]\n"
    ));
    assert!(
        pack.markdown
            .contains("[Structured claim; stored assertion, not independently verified]\n")
    );
    assert!(
        pack.markdown
            .contains(&format!("[Traversal {billing} -depends_on-> {postgres}]"))
    );

    // A historical query returns the captured snapshot with a warning.
    std::thread::sleep(std::time::Duration::from_millis(5));
    let before_update = now_ms();
    std::thread::sleep(std::time::Duration::from_millis(5));
    fixture
        .store
        .update(
            &scope,
            &memory,
            &MemoryPatch {
                content: Some("billing now uses mysql".into()),
                ..Default::default()
            },
        )
        .unwrap();
    let historical = recall
        .recall(
            &scope,
            &RecallRequest {
                query: "x".into(),
                structured: StructuredRecallOptions {
                    enabled: true,
                    subject: "billing".into(),
                    as_of: Some(before_update),
                    ..Default::default()
                },
                ..Default::default()
            },
        )
        .unwrap();
    let snapshot = historical
        .iter()
        .find(|item| item.memory.id == memory)
        .unwrap();
    assert!(snapshot.historical);
    assert_eq!(
        snapshot.memory.content,
        "The billing service depends on postgres"
    );
    assert!(snapshot.warnings.contains(
        &"historical assertion snapshot; current guidance requires revalidation".to_string()
    ));
}

#[test]
fn context_budget_and_omissions() {
    let fixture = Fixture::new();
    let scope = scope();
    for index in 0..20 {
        active(
            &fixture,
            &scope,
            &format!("budget memory number {index} {}", "x".repeat(200)),
        );
    }
    let recall = RecallService::new(Arc::clone(&fixture.store));
    let results = recall
        .recall(
            &scope,
            &RecallRequest {
                query: "budget memory".into(),
                ..Default::default()
            },
        )
        .unwrap();
    let context = ContextService::new(Arc::clone(&fixture.store));
    let pack = context
        .build(
            &scope,
            &results,
            &ContextOptions {
                token_budget: 250,
                ..Default::default()
            },
            &[],
        )
        .unwrap();
    assert!(pack.markdown.len() <= 1000);
    assert!(pack.omitted > 0);
    assert!(pack.markdown.contains("\n## Relevant memories\n[Memory "));
    assert!(pack.markdown.contains("; status=active; confidence=0.5]\n"));
    assert_eq!(pack.estimated_tokens, pack.markdown.len().div_ceil(4));
    let tiny = context
        .build(
            &scope,
            &results,
            &ContextOptions {
                token_budget: 10,
                ..Default::default()
            },
            &[],
        )
        .unwrap();
    assert!(tiny.markdown.is_empty());
    assert_eq!(tiny.omitted, results.len());
    assert!(
        tiny.item_explanations
            .iter()
            .all(|item| item.reason == "context_envelope_exceeds_budget")
    );
    // A memory edited after recall is revalidated and omitted.
    fixture
        .store
        .update(
            &scope,
            &results[0].memory.id,
            &MemoryPatch {
                content: Some("changed".into()),
                ..Default::default()
            },
        )
        .unwrap();
    let stale = context
        .build(&scope, &results[..1], &ContextOptions::default(), &[])
        .unwrap();
    assert!(stale.memory_ids.is_empty());
    assert_eq!(
        stale.item_explanations[0].reason,
        "memory_validation_failed"
    );

    context
        .update_block(&scope, "repo_rules", "Always run tests")
        .unwrap();
    let pinned = context
        .build(&scope, &[], &ContextOptions::default(), &[])
        .unwrap();
    assert!(
        pinned
            .markdown
            .contains("\n## Pinned blocks\n[Pinned block repo_rules]\nAlways run tests\n")
    );
    assert_eq!(pinned.block_ids, vec!["repo_rules"]);
}
