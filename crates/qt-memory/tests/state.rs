mod common;

use std::sync::Arc;

use common::*;
use qt_memory::review::*;
use qt_memory::*;
use serde_json::{Value, json};

fn populated() -> (Fixture, String, String, String) {
    let fixture = Fixture::new();
    let store = &fixture.store;
    let a = fixture.remember(&scope(), "first fact", MemoryStatus::Active);
    let b = fixture.remember(&scope(), "second fact", MemoryStatus::Hypothesis);
    store
        .add_edge(&scope(), GraphEdge::new(a.clone(), b.clone(), "related"))
        .unwrap();
    store
        .update(
            &scope(),
            &a,
            &MemoryPatch {
                confidence: Some(0.9),
                ..Default::default()
            },
        )
        .unwrap();
    store
        .update_block(&scope(), "repo_rules", "be careful")
        .unwrap();
    store
        .record_event(
            &scope(),
            SourceRecord {
                id: "src_1".into(),
                ..Default::default()
            },
            SourceEvent {
                action: "ingest".into(),
                ..Default::default()
            },
        )
        .unwrap();
    store
        .record_session_event_once(
            &scope(),
            SessionEventWrite {
                event_type: "turn".into(),
                payload_json: "{}".into(),
                deduplication_key: "k1".into(),
                ..Default::default()
            },
        )
        .unwrap();
    let review = store
        .add_review(
            &scope(),
            ReviewItemWrite {
                kind: "semantic_consolidation".into(),
                candidate_json: json!({"memory_ids": [a, b], "canonical_content": "merged"})
                    .to_string(),
                ..Default::default()
            },
        )
        .unwrap();
    let service = ReviewService::new(Arc::clone(store), ReviewGovernancePolicy::default());
    service
        .apply(
            &scope(),
            &review,
            ReviewAction {
                human_authority: true,
                ..ReviewAction::new(ReviewActionKind::Relate, "user")
            },
        )
        .unwrap();
    store
        .save_review_view_state(
            &scope(),
            ReviewViewState {
                selected_review_id: review.clone(),
                ..Default::default()
            },
        )
        .unwrap();
    (fixture, a, b, review)
}

#[test]
fn export_is_bounded_and_complete() {
    let (fixture, a, _, review) = populated();
    let state = fixture.store.export_state(&scope()).unwrap();
    assert_eq!(state["schema_version"], 7);
    assert_eq!(state["scope_key"], scope_key(&scope()).unwrap());
    assert_eq!(state["memories"].as_array().unwrap().len(), 2);
    let first = state["memories"]
        .as_array()
        .unwrap()
        .iter()
        .find(|memory| memory["id"] == a.as_str())
        .unwrap();
    assert_eq!(first["retrieval_revision"], 2);
    // The governed relate edge upserts the identical fixture edge.
    assert_eq!(state["graph_edges"].as_array().unwrap().len(), 1);
    assert_eq!(state["review_items"][0]["id"], review.as_str());
    assert_eq!(state["review_actions"].as_array().unwrap().len(), 1);
    assert_eq!(
        state["review_view_state"]["state"]["selected_review_id"],
        review.as_str()
    );
    assert_eq!(state["memory_revisions"].as_array().unwrap().len(), 1);
    assert_eq!(
        state["assertion_revision_boundaries"]
            .as_array()
            .unwrap()
            .len(),
        3
    );
    assert!(
        state["truncated"]
            .as_object()
            .unwrap()
            .values()
            .all(|value| value == false)
    );
    let bounded = fixture
        .store
        .export_state_with_limits(
            &scope(),
            &ExportLimits {
                memories: 1,
                ..Default::default()
            },
        )
        .unwrap();
    assert_eq!(bounded["truncated"]["memories"], true);
    assert_eq!(
        fixture
            .store
            .export_state_with_limits(&scope(), &ExportLimits::uniform(0))
            .unwrap_err()
            .to_string(),
        "memory export collection limits must be from 1 to 100000"
    );
    assert_eq!(
        fixture
            .store
            .import_state(&scope(), &bounded, false)
            .unwrap_err()
            .to_string(),
        "cannot import a truncated canonical export"
    );
}

#[test]
fn restore_into_same_scope_preserves_identity() {
    let (fixture, a, b, review) = populated();
    let state = fixture.store.export_state(&scope()).unwrap();
    let restored = Fixture::new();
    restored
        .store
        .import_state(&scope(), &state, false)
        .unwrap();
    let memory = restored.store.get(&scope(), &a).unwrap();
    assert_eq!(memory.confidence, 0.9);
    assert_eq!(restored.store.retrieval_revision(&scope(), &a).unwrap(), 2);
    assert_eq!(
        restored.store.get(&scope(), &b).unwrap().content,
        "second fact"
    );
    let reviews = restored
        .store
        .reviews(&scope(), ReviewStatus::Accepted, 10)
        .unwrap();
    assert_eq!(reviews.len(), 1);
    assert_eq!(reviews[0].revision, 1);
    assert_eq!(reviews[0].decision, "relate");
    let candidate: Value = serde_json::from_str(&reviews[0].candidate_json).unwrap();
    assert_eq!(candidate["payload"]["memory_ids"], json!([a, b]));
    let history = restored
        .store
        .review_history(&scope(), Some(&reviews[0].id), 10)
        .unwrap();
    assert_eq!(history.len(), 1);
    let view = restored
        .store
        .load_review_view_state(&scope())
        .unwrap()
        .unwrap();
    assert_eq!(
        view.selected_review_id, reviews[0].id,
        "view state follows the re-issued review id"
    );
    assert_ne!(reviews[0].id, review);
    assert_eq!(
        restored.store.blocks(&scope()).unwrap()[0].content,
        "be careful"
    );
    assert_eq!(restored.store.sources(&scope()).unwrap().len(), 1);
    assert_eq!(
        restored
            .store
            .session_events(&scope(), "turn", 10, 0)
            .unwrap()[0]
            .deduplication_key,
        "k1"
    );
    assert_eq!(restored.store.integrity_check().unwrap(), "ok");
}

#[test]
fn restore_into_other_scope_remaps_every_reference() {
    let (fixture, a, b, _) = populated();
    let state = fixture.store.export_state(&scope()).unwrap();
    let target = exact_scope("gamma", "repo_gamma", "s2");
    // Importing into a different scope of the same database must not reuse IDs.
    fixture.store.import_state(&target, &state, false).unwrap();
    let imported = fixture
        .store
        .query(
            &target,
            &MemoryQuery {
                include_superseded: true,
                ..Default::default()
            },
        )
        .unwrap();
    assert_eq!(imported.len(), 2);
    assert!(
        imported
            .iter()
            .all(|memory| memory.id != a && memory.id != b)
    );
    let new_ids: Vec<String> = imported.iter().map(|memory| memory.id.clone()).collect();
    let edges = fixture
        .store
        .query_edges(&target, &GraphQuery::default())
        .unwrap();
    assert!(
        edges
            .iter()
            .all(|edge| new_ids.contains(&edge.from_id) && new_ids.contains(&edge.to_id))
    );
    let review = &fixture
        .store
        .reviews(&target, ReviewStatus::Accepted, 10)
        .unwrap()[0];
    let candidate: Value = serde_json::from_str(&review.candidate_json).unwrap();
    let candidate_ids: Vec<String> =
        serde_json::from_value(candidate["payload"]["memory_ids"].clone()).unwrap();
    assert!(candidate_ids.iter().all(|id| new_ids.contains(id)));
    let action = &fixture.store.review_history(&target, None, 10).unwrap()[0];
    let result: Value = serde_json::from_str(&action.result_json).unwrap();
    let edge_ids: Vec<String> = edges.iter().map(|edge| edge.id.clone()).collect();
    for changed in result["changed_ids"].as_array().unwrap() {
        assert!(
            edge_ids.contains(&changed.as_str().unwrap().to_string()),
            "result ids are remapped to new edges"
        );
    }
}

#[test]
fn import_validates_scope_declaration_and_replaces() {
    let (fixture, _, _, _) = populated();
    let mut state = fixture.store.export_state(&scope()).unwrap();
    state["scope_key"] = json!("forged");
    assert_eq!(
        fixture
            .store
            .import_state(&scope(), &state, false)
            .unwrap_err()
            .to_string(),
        "exported scope declaration is inconsistent"
    );
    let mut state = fixture.store.export_state(&scope()).unwrap();
    state["memories"][0]["project"] = json!("elsewhere");
    assert_eq!(
        fixture
            .store
            .import_state(&scope(), &state, false)
            .unwrap_err()
            .to_string(),
        "imported memory scope does not match export declaration"
    );
    let state = fixture.store.export_state(&scope()).unwrap();
    fixture.store.import_state(&scope(), &state, true).unwrap();
    assert_eq!(
        fixture.store.size(&scope()).unwrap(),
        2,
        "replace clears the scope before restoring"
    );
    let backup = fixture.store.write_backup(&scope()).unwrap().unwrap();
    assert!(backup.starts_with(fixture.store.root().join("exports")));
    let written: Value = serde_json::from_str(&std::fs::read_to_string(&backup).unwrap()).unwrap();
    assert_eq!(written["export_limits"]["memories"], 100_000);
    assert!(
        fixture
            .store
            .write_backup(&exact_scope("empty", "repo_empty", "s"))
            .unwrap()
            .is_none()
    );
}

#[test]
fn retention_prunes_old_raw_evidence() {
    let fixture = Fixture::new();
    let store = &fixture.store;
    for index in 0..3 {
        store
            .record_session_event(
                &scope(),
                SessionEventWrite {
                    event_type: format!("e{index}"),
                    ..Default::default()
                },
            )
            .unwrap();
    }
    let memory = fixture.remember(&scope(), "keep me", MemoryStatus::Active);
    let policy = RetentionPolicy {
        keep_latest: 1,
        ..Default::default()
    };
    let dry = store.enforce_retention(&scope(), &policy).unwrap();
    assert!(dry.dry_run);
    assert_eq!(dry.session_events, 2);
    assert_eq!(store.session_events(&scope(), "", 10, 0).unwrap().len(), 3);
    let applied = store
        .enforce_retention(
            &scope(),
            &RetentionPolicy {
                dry_run: false,
                ..policy
            },
        )
        .unwrap();
    assert_eq!(applied.total(), 2);
    assert_eq!(store.session_events(&scope(), "", 10, 0).unwrap().len(), 1);
    assert!(
        store.get(&scope(), &memory).is_ok(),
        "canonical memories are kept unless requested"
    );
    let canonical = RetentionPolicy {
        older_than: now_ms() + 1000,
        canonical_memories: true,
        raw_evidence: false,
        dry_run: false,
        ..Default::default()
    };
    assert_eq!(
        store
            .enforce_retention(&scope(), &canonical)
            .unwrap()
            .canonical_memories,
        1
    );
    assert!(store.get(&scope(), &memory).is_err());
    assert_eq!(
        store
            .enforce_retention(&scope(), &RetentionPolicy::default())
            .unwrap_err()
            .to_string(),
        "retention requires an age cutoff or non-zero keep_latest"
    );
}

#[test]
fn scrub_rewrites_historical_rows_and_fts() {
    let fixture = Fixture::new();
    let store = &fixture.store;
    let other = exact_scope("beta", "repo_beta", "s");
    let id = fixture.remember(&scope(), "placeholder", MemoryStatus::Hypothesis);
    let other_id = fixture.remember(&other, "other placeholder", MemoryStatus::Hypothesis);
    let event = store
        .record_session_event(
            &scope(),
            SessionEventWrite {
                event_type: "legacy".into(),
                ..Default::default()
            },
        )
        .unwrap();
    store
        .update_block(&scope(), "legacy", "placeholder")
        .unwrap();
    let conn = fixture.raw();
    conn.execute(
        "UPDATE memories SET content=?,source_uri=? WHERE id=?;",
        rusqlite::params![
            "password=historicalalphaunique",
            "/home/legacy/private/source.txt",
            id
        ],
    )
    .unwrap();
    conn.execute(
        "UPDATE memory_fts SET content=? WHERE memory_id=?;",
        rusqlite::params!["password=historicalalphaunique", id],
    )
    .unwrap();
    conn.execute(
        "UPDATE memories SET content=? WHERE id=?;",
        rusqlite::params!["password=historicalbetaunique", other_id],
    )
    .unwrap();
    conn.execute(
        "UPDATE session_events SET payload_json=? WHERE id=?;",
        rusqlite::params![
            r#"{"api_key":"legacy-token-value","path":"/home/legacy/private.json"}"#,
            event
        ],
    )
    .unwrap();
    conn.execute(
        "UPDATE context_blocks SET content=? WHERE id='legacy';",
        ["secret=legacy-block-value /home/legacy/block.txt"],
    )
    .unwrap();
    let dry = store
        .scrub_sensitive_data(&scope(), &RedactionScrubPolicy::default())
        .unwrap();
    assert!(dry.changed() >= 3);
    let raw_content: String = conn
        .query_row("SELECT content FROM memories WHERE id=?;", [&id], |row| {
            row.get(0)
        })
        .unwrap();
    assert_eq!(
        raw_content, "password=historicalalphaunique",
        "dry run changes nothing"
    );
    let applied = store
        .scrub_sensitive_data(
            &scope(),
            &RedactionScrubPolicy {
                dry_run: false,
                ..Default::default()
            },
        )
        .unwrap();
    assert_eq!(applied.changed(), dry.changed());
    let raw_content: String = conn
        .query_row("SELECT content FROM memories WHERE id=?;", [&id], |row| {
            row.get(0)
        })
        .unwrap();
    assert_eq!(raw_content, "[REDACTED]");
    let fts_hits: i64 = conn
        .query_row(
            "SELECT COUNT(*) FROM memory_fts WHERE memory_fts MATCH 'historicalalphaunique';",
            [],
            |row| row.get(0),
        )
        .unwrap();
    assert_eq!(fts_hits, 0);
    let payload: String = conn
        .query_row(
            "SELECT payload_json FROM session_events WHERE id=?;",
            [&event],
            |row| row.get(0),
        )
        .unwrap();
    assert_eq!(payload, r#"{"api_key":"[REDACTED]","path":"[REDACTED]"}"#);
    let block: String = conn
        .query_row(
            "SELECT content FROM context_blocks WHERE id='legacy';",
            [],
            |row| row.get(0),
        )
        .unwrap();
    assert_eq!(block, "[REDACTED] [REDACTED]");
    let untouched: String = conn
        .query_row(
            "SELECT content FROM memories WHERE id=?;",
            [&other_id],
            |row| row.get(0),
        )
        .unwrap();
    assert_eq!(
        untouched, "password=historicalbetaunique",
        "other scopes are not scrubbed"
    );
    assert_eq!(
        store.audit_events(&scope(), 1).unwrap()[0].action,
        "redaction_scrub"
    );
    let again = store
        .scrub_sensitive_data(
            &scope(),
            &RedactionScrubPolicy {
                dry_run: false,
                ..Default::default()
            },
        )
        .unwrap();
    assert_eq!(
        again.changed(),
        0,
        "stored JSON already uses the canonical encoding"
    );
}
