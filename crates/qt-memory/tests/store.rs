mod common;

use common::*;
use qt_memory::*;
use serde_json::Value;

#[test]
fn remember_applies_write_policy_and_redaction() {
    let fixture = Fixture::new();
    let service = fixture.service();
    let id = service
        .remember(
            &scope(),
            MemoryWrite {
                content:
                    "  We decided to rotate sk-abcdefghijklmnopqrstuvwxyz keys\r\n\n\n\nweekly  "
                        .into(),
                status: MemoryStatus::Active,
                source: "chat".into(),
                confidence: 3.0,
                ..Default::default()
            },
        )
        .unwrap();
    let memory = service.get(&scope(), &id).unwrap();
    assert!(id.starts_with("mem_"));
    assert_eq!(
        memory.content,
        "We decided to rotate [REDACTED] keys\n\nweekly"
    );
    assert_eq!(memory.memory_type, MemoryType::Decision);
    // Agent writes cannot self-promote to active.
    assert_eq!(memory.status, MemoryStatus::Hypothesis);
    assert_eq!(memory.confidence, 1.0);
    assert_eq!(memory.provenance.evidence_strength, 0.4);
    assert_eq!(
        memory.tags,
        ["security:redacted", "source:unknown", "type:decision"]
    );
    assert_eq!(
        service
            .remember(&scope(), MemoryWrite::default())
            .unwrap_err()
            .to_string(),
        "memory content is required"
    );

    let user = service
        .remember(
            &scope(),
            MemoryWrite {
                content: "prefer tabs".into(),
                status: MemoryStatus::Active,
                provenance: Provenance {
                    created_by: "user".into(),
                    source_hash: "h".into(),
                    ..Default::default()
                },
                ..Default::default()
            },
        )
        .unwrap();
    let user = service.get(&scope(), &user).unwrap();
    assert_eq!(user.status, MemoryStatus::Active);
    assert_eq!(user.provenance.evidence_strength, 0.8);
}

#[test]
fn query_filters_by_scope_status_tags_and_text() {
    let fixture = Fixture::new();
    let exact = scope();
    let durable = durable_memory_scope(exact.clone());
    let other = exact_scope("beta", "repo_beta", "s");
    let a = fixture.remember(
        &exact,
        "alpha exact note about sqlite",
        MemoryStatus::Hypothesis,
    );
    let b = fixture.remember(&durable, "branch note about sqlite", MemoryStatus::Active);
    let rejected = fixture.remember(&exact, "rejected sqlite idea", MemoryStatus::Rejected);
    fixture.remember(&other, "other project sqlite", MemoryStatus::Active);

    let balanced = fixture
        .store
        .query(
            &exact,
            &MemoryQuery {
                text: "sqlite".into(),
                ..Default::default()
            },
        )
        .unwrap();
    let ids: Vec<_> = balanced.iter().map(|memory| memory.id.clone()).collect();
    assert_eq!(
        ids,
        [a.clone(), b.clone()],
        "nearest scope first, inactive excluded, other scopes hidden"
    );
    let strict = fixture
        .store
        .query(
            &exact,
            &MemoryQuery {
                policy: RecallPolicy::Strict,
                include_superseded: true,
                ..Default::default()
            },
        )
        .unwrap();
    assert_eq!(strict.len(), 2);
    assert!(strict.iter().any(|memory| memory.id == rejected));
    let by_status = fixture
        .store
        .query(
            &exact,
            &MemoryQuery {
                statuses: vec![MemoryStatus::Active],
                ..Default::default()
            },
        )
        .unwrap();
    assert_eq!(by_status.len(), 1);
    fixture
        .store
        .update(
            &exact,
            &a,
            &MemoryPatch {
                tags: Some(vec!["x".into(), "x".into(), "".into()]),
                ..Default::default()
            },
        )
        .unwrap();
    let tagged = fixture
        .store
        .query(
            &exact,
            &MemoryQuery {
                tags: vec!["x".into()],
                ..Default::default()
            },
        )
        .unwrap();
    assert_eq!(tagged.len(), 1);
    assert_eq!(tagged[0].tags, ["x"]);
    assert_eq!(
        fixture.store.get(&other, &a).unwrap_err().to_string(),
        "memory not found in scope"
    );
    assert!(
        fixture
            .store
            .find_for_recall(&exact, &b, RecallPolicy::Strict, false)
            .unwrap()
            .is_none()
    );
    assert!(
        fixture
            .store
            .find_for_recall(&exact, &b, RecallPolicy::Balanced, false)
            .unwrap()
            .is_some()
    );
    assert_eq!(fixture.store.size(&exact).unwrap(), 2);
}

#[test]
fn updates_keep_revisions_and_bump_retrieval_revision() {
    let fixture = Fixture::new();
    let id = fixture.remember(&scope(), "original", MemoryStatus::Hypothesis);
    let updated = fixture
        .store
        .update(
            &scope(),
            &id,
            &MemoryPatch {
                content: Some("token: ghp_abcdefghijklmnopqrstuvwxyz".into()),
                replacement_id: Some("mem_x".into()),
                provenance: Some(Provenance {
                    evidence_strength: 4.0,
                    ..Default::default()
                }),
                ..Default::default()
            },
        )
        .unwrap();
    assert_eq!(updated.content, "token: [REDACTED]");
    assert_eq!(updated.superseded_by, "mem_x");
    assert_eq!(updated.provenance.evidence_strength, 1.0);
    let revisions = fixture.store.revisions(&scope(), &id).unwrap();
    assert_eq!(revisions.len(), 1);
    assert_eq!(revisions[0].kind, "update");
    let snapshot: Value = serde_json::from_str(&revisions[0].entry_json).unwrap();
    assert_eq!(snapshot["content"], "original");
    assert_eq!(fixture.store.retrieval_revision(&scope(), &id).unwrap(), 2);
    assert!(fixture.store.forget(&scope(), &id).unwrap());
    assert!(!fixture.store.forget(&scope(), &id).unwrap());
    let conn = fixture.raw();
    let kinds: Vec<String> = conn
        .prepare(
            "SELECT revision_kind FROM memory_revisions WHERE memory_id=? ORDER BY revision_kind;",
        )
        .unwrap()
        .query_map([&id], |row| row.get(0))
        .unwrap()
        .collect::<rusqlite::Result<_>>()
        .unwrap();
    assert_eq!(kinds, ["delete", "update"]);
    let deleted: i64 = conn
        .query_row(
            "SELECT deleted FROM retrieval_owners WHERE owner_key=?;",
            [format!("memory:{id}")],
            |row| row.get(0),
        )
        .unwrap();
    assert_eq!(deleted, 1);
}

#[test]
fn governance_transitions_are_audited_and_exact_scoped() {
    let fixture = Fixture::new();
    let governance = GovernanceService::new(fixture.service(), LifecyclePolicy::default());
    let old = fixture.remember(&scope(), "old fact", MemoryStatus::Hypothesis);
    let new = fixture.remember(&scope(), "new fact", MemoryStatus::Hypothesis);
    assert_eq!(
        governance
            .validate(&scope(), &old, "user", "checked", "")
            .unwrap_err()
            .to_string(),
        "invalid memory lifecycle transition"
    );
    let validated = governance
        .validate(
            &scope(),
            &old,
            "user",
            "checked",
            "https://example.test/doc",
        )
        .unwrap();
    assert_eq!(validated.status, MemoryStatus::Validated);
    let superseded = governance
        .supersede(&scope(), &old, &new, "user", "replaced")
        .unwrap();
    assert_eq!(superseded.status, MemoryStatus::Superseded);
    assert_eq!(superseded.replacement_id, new);
    let edges = fixture
        .store
        .query_edges(
            &scope(),
            &GraphQuery {
                memory_id: old.clone(),
                ..Default::default()
            },
        )
        .unwrap();
    assert_eq!(edges.len(), 1);
    assert_eq!(edges[0].edge_type, "superseded_by");
    assert_eq!(edges[0].source, "governance");
    let audits = fixture.store.audit_events(&scope(), 10).unwrap();
    let actions: Vec<_> = audits.iter().map(|audit| audit.action.as_str()).collect();
    assert!(
        actions.contains(&"memory.lifecycle.validated")
            && actions.contains(&"memory.lifecycle.superseded")
    );
    assert_eq!(
        governance
            .transition(
                &scope(),
                &LifecycleRequest::new(old.clone(), MemoryStatus::Active, "", "r")
            )
            .unwrap_err()
            .to_string(),
        "governance transition requires actor and reason"
    );
    let durable = durable_memory_scope(scope());
    let branch = fixture.remember(&durable, "branch fact", MemoryStatus::Hypothesis);
    assert_eq!(
        governance
            .transition(
                &scope(),
                &LifecycleRequest::new(branch, MemoryStatus::Active, "u", "r")
            )
            .unwrap_err()
            .to_string(),
        "governance transition subject is outside the exact authority scope"
    );
}

#[test]
fn lifecycle_state_machine_matches_archive() {
    use MemoryStatus as S;
    let policy = LifecyclePolicy::default();
    let can = |from, to, evidence, replacement| {
        GovernanceService::can_transition(from, to, &policy, evidence, replacement)
    };
    assert!(can(S::Candidate, S::Active, false, false));
    assert!(!can(S::Validated, S::Active, false, false));
    assert!(can(S::Active, S::Validated, true, false));
    assert!(!can(S::Active, S::Validated, false, false));
    assert!(can(S::DreamArtifact, S::Hypothesis, false, false));
    assert!(!can(S::Rejected, S::Active, false, false));
    assert!(can(S::Expired, S::Rejected, false, false));
    assert!(!can(S::Expired, S::Active, false, false));
    assert!(!can(S::Active, S::Superseded, false, false));
    assert!(can(S::Active, S::Superseded, false, true));
    assert!(!GovernanceService::can_transition(
        S::Active,
        S::Expired,
        &LifecyclePolicy {
            allow_expiration: false,
            ..policy
        },
        false,
        false
    ));
    assert!(
        GovernanceService::trusted_for_recall(S::Active)
            && !GovernanceService::trusted_for_recall(S::Hypothesis)
    );
}

#[test]
fn procedure_feedback_is_idempotent_and_updates_confidence() {
    let fixture = Fixture::new();
    let procedure = fixture
        .store
        .add(
            &scope(),
            MemoryWrite {
                content: "steps".into(),
                memory_type: MemoryType::Procedural,
                ..Default::default()
            },
        )
        .unwrap();
    let feedback = ProcedureFeedback {
        procedure_id: procedure.clone(),
        outcome: ProcedureOutcome::Success,
        deduplication_key: "turn-1".into(),
        ..Default::default()
    };
    let first = fixture
        .store
        .add_feedback_once(&scope(), feedback.clone())
        .unwrap();
    assert!(first.inserted);
    assert_eq!(first.memory.success_count, 1);
    assert!((first.memory.confidence - 0.65).abs() < 1e-9);
    let replay = fixture.store.add_feedback_once(&scope(), feedback).unwrap();
    assert!(!replay.inserted);
    assert_eq!(replay.run.id, first.run.id);
    assert_eq!(fixture.store.runs(&scope(), &procedure).unwrap().len(), 1);
    let note = fixture.remember(&scope(), "not a procedure", MemoryStatus::Hypothesis);
    assert_eq!(
        fixture
            .store
            .add_feedback(
                &scope(),
                ProcedureFeedback {
                    procedure_id: note,
                    ..Default::default()
                }
            )
            .unwrap_err()
            .to_string(),
        "procedure feedback target is not procedural memory"
    );
}

#[test]
fn sources_sessions_blocks_and_capture_receipts() {
    let fixture = Fixture::new();
    let store = &fixture.store;
    let source = SourceRecord {
        id: "src_1".into(),
        uri: "/home/me/notes.md".into(),
        ..Default::default()
    };
    store
        .record_event(
            &scope(),
            source.clone(),
            SourceEvent {
                action: "ingest".into(),
                ..Default::default()
            },
        )
        .unwrap();
    store
        .record_event(
            &scope(),
            source,
            SourceEvent {
                action: "ingest".into(),
                ..Default::default()
            },
        )
        .unwrap();
    let sources = store.sources(&scope()).unwrap();
    assert_eq!(sources.len(), 1);
    assert_eq!(sources[0].ingest_count, 2);
    assert_eq!(sources[0].uri, "[REDACTED]");
    assert_eq!(store.source_events(&scope(), "src_1").unwrap().len(), 2);

    let event = SessionEventWrite {
        event_type: "turn".into(),
        payload_json: r#"{"api_key":"x","n":1}"#.into(),
        deduplication_key: "k".into(),
        ..Default::default()
    };
    let first = store
        .record_session_event_once(&scope(), event.clone())
        .unwrap();
    let second = store.record_session_event_once(&scope(), event).unwrap();
    assert!(first.inserted && !second.inserted && first.id == second.id);
    let events = store.session_events(&scope(), "turn", 10, 0).unwrap();
    assert_eq!(events[0].payload_json, r#"{"api_key":"[REDACTED]","n":1}"#);

    store
        .update_block(&durable_memory_scope(scope()), "repo_rules", "branch rules")
        .unwrap();
    store
        .update_block(&scope(), "repo_rules", "exact rules")
        .unwrap();
    assert_eq!(
        store.get_block(&scope(), "repo_rules").unwrap().content,
        "exact rules"
    );
    assert_eq!(store.blocks(&scope()).unwrap().len(), 1);
    assert_eq!(store.get_block(&scope(), "missing").unwrap().content, "");

    let claim = |key: &str| SessionEventWrite {
        event_type: "candidate_ingestion_claim".into(),
        deduplication_key: key.into(),
        ..Default::default()
    };
    let write = MemoryWrite {
        content: "Use WAL mode".into(),
        ..Default::default()
    };
    let (id, inserted) = store
        .add_capture_candidate_once(&scope(), write.clone(), claim("c1"))
        .unwrap();
    assert!(inserted);
    assert_eq!(
        store.get(&scope(), &id).unwrap().status,
        MemoryStatus::Candidate
    );
    assert_eq!(
        store
            .add_capture_candidate_once(&scope(), write, claim("c1"))
            .unwrap(),
        (id.clone(), false)
    );
    let same_content = MemoryWrite {
        content: "use wal MODE".into(),
        ..Default::default()
    };
    assert_eq!(
        store
            .add_capture_candidate_once(&scope(), same_content.clone(), claim("c2"))
            .unwrap(),
        (id, false)
    );
    let different = MemoryWrite {
        content: "other".into(),
        ..Default::default()
    };
    assert_eq!(
        store
            .add_capture_candidate_once(&scope(), different, claim("c1"))
            .unwrap_err()
            .to_string(),
        "capture receipt key reused for different content"
    );
}

#[test]
fn statistics_aggregate_scoped_state() {
    let fixture = Fixture::new();
    let store = &fixture.store;
    let a = fixture.remember(&scope(), "a", MemoryStatus::Hypothesis);
    let b = fixture.remember(&scope(), "b", MemoryStatus::Deprecated);
    store
        .record_access(&scope(), &[a.clone(), a.clone(), a.clone()])
        .unwrap();
    store
        .add_edge(&scope(), GraphEdge::new(a.clone(), b, "conflicts_with"))
        .unwrap();
    store
        .record_session_event(
            &scope(),
            SessionEventWrite {
                event_type: "memory_dream_claim".into(),
                payload_json: r#"{"trigger":"manual","model":"llama/x"}"#.into(),
                ..Default::default()
            },
        )
        .unwrap();
    store
        .record_session_event(
            &scope(),
            SessionEventWrite {
                event_type: "memory_dream_receipt".into(),
                payload_json: r#"{"error":"child timed out"}"#.into(),
                ..Default::default()
            },
        )
        .unwrap();
    let stats = store
        .statistics(&scope(), RecallPolicy::Balanced, false)
        .unwrap();
    assert_eq!(stats.memory_entries, 2);
    assert_eq!(stats.hypotheses, 1);
    assert_eq!(stats.deprecated, 1);
    assert_eq!(stats.high_use_hypotheses, 1);
    assert_eq!(stats.total_accesses, 3);
    assert_eq!(stats.conflict_count, 1);
    assert_eq!(stats.unresolved_conflicts, 1);
    assert_eq!(stats.memory_counts["observation"], 2);
    assert_eq!(stats.dreaming.attempted, 1);
    assert_eq!(stats.dreaming.timed_out, 1);
    assert_eq!(stats.dreaming.failures_by_reason["timeout"], 1);
    assert_eq!(stats.dreaming.by_model["llama/x"], 1);
}
