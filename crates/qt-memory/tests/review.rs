mod common;

use std::sync::Arc;

use common::*;
use qt_memory::review::*;
use qt_memory::*;
use serde_json::json;

fn service(fixture: &Fixture) -> ReviewService {
    ReviewService::new(
        Arc::clone(&fixture.store),
        ReviewGovernancePolicy {
            allow_dream_hypothesis_promotion: true,
        },
    )
}

fn add_review(
    fixture: &Fixture,
    scope: &MemoryScope,
    kind: &str,
    subject: &str,
    candidate: serde_json::Value,
) -> String {
    fixture
        .store
        .add_review(
            scope,
            ReviewItemWrite {
                kind: kind.into(),
                reason: "needs review".into(),
                subject_id: subject.into(),
                provenance: Provenance {
                    source_type: SourceType::File,
                    source_uri: "file:evidence".into(),
                    evidence_strength: 0.9,
                    created_by: "agent".into(),
                    ..Default::default()
                },
                candidate_json: candidate.to_string(),
                ..Default::default()
            },
        )
        .unwrap()
}

fn action(kind: ReviewActionKind) -> ReviewAction {
    ReviewAction {
        human_authority: true,
        ..ReviewAction::new(kind, "user:test")
    }
}

#[test]
fn add_review_normalises_candidate_and_audits() {
    let fixture = Fixture::new();
    let subject = fixture.remember(&scope(), "candidate fact", MemoryStatus::Candidate);
    let id = add_review(
        &fixture,
        &scope(),
        "candidate_memory",
        &subject,
        json!({"content": "x", "reason": "r"}),
    );
    let item = fixture.store.review(&scope(), &id).unwrap();
    assert!(id.starts_with("review_"));
    assert_eq!(item.status, ReviewStatus::Pending);
    assert_eq!(
        item.candidate_json,
        r#"{"kind":"general","payload":{"content":"x","memory_ids":[],"reason":"r","tags":[]},"version":1}"#
    );
    let audit = &fixture.store.audit_events(&scope(), 1).unwrap()[0];
    assert_eq!(audit.action, "review_create");
    assert_eq!(
        audit.metadata_json,
        format!(r#"{{"candidate_version":1,"kind":"candidate_memory","review_id":"{id}"}}"#)
    );
    assert_eq!(
        fixture
            .store
            .review(&exact_scope("x", "y", "z"), &id)
            .unwrap_err()
            .to_string(),
        "review item not found in scope"
    );
}

#[test]
fn approve_preview_then_apply_with_revision_and_token() {
    let fixture = Fixture::new();
    let reviews = service(&fixture);
    let subject = fixture.remember(&scope(), "candidate fact", MemoryStatus::Candidate);
    let id = add_review(
        &fixture,
        &scope(),
        "candidate_memory",
        &subject,
        json!({"content": "x"}),
    );

    let preview = reviews
        .preview(&scope(), &id, action(ReviewActionKind::Approve))
        .unwrap();
    assert!(preview.applicable);
    assert_eq!(
        preview.will_do,
        ["advance the candidate to its previewed trust state"]
    );
    assert_eq!(preview.changes[0].record_type, "review");
    assert_eq!(preview.changes[1].record_type, "memory");
    assert_eq!(preview.idempotency_token.len(), 16);

    let mut unauthorised = action(ReviewActionKind::Approve);
    unauthorised.human_authority = false;
    assert_eq!(
        reviews
            .apply(&scope(), &id, unauthorised)
            .unwrap_err()
            .to_string(),
        "review action requires explicit human authority"
    );
    let mut stale = action(ReviewActionKind::Approve);
    stale.idempotency_key = "0000000000000000".into();
    assert_eq!(
        reviews.apply(&scope(), &id, stale).unwrap_err().to_string(),
        "review or subject changed after preview: stale idempotency token"
    );

    let mut confirmed = action(ReviewActionKind::Approve);
    confirmed.idempotency_key = preview.idempotency_token.clone();
    confirmed.expected_revision = preview.review_revision;
    let result = reviews.apply(&scope(), &id, confirmed.clone()).unwrap();
    assert_eq!(result.status, ReviewApplicationStatus::Applied);
    assert!(
        result.decision_persisted && result.domain_mutations_applied && !result.idempotent_replay
    );
    assert_eq!(result.revision, 1);
    assert_eq!(result.changed_ids, std::slice::from_ref(&subject));
    assert_eq!(
        fixture.store.get(&scope(), &subject).unwrap().status,
        MemoryStatus::Active
    );
    let item = fixture.store.review(&scope(), &id).unwrap();
    assert_eq!(item.status, ReviewStatus::Accepted);
    assert_eq!(item.decision, "approved_as_active");

    // Replaying the same key returns the recorded result and counts the replay.
    let replay = reviews.apply(&scope(), &id, confirmed).unwrap();
    assert!(replay.idempotent_replay);
    assert_eq!(replay.action_id, result.action_id);
    assert_eq!(replay.changed_ids, result.changed_ids);
    let history = fixture
        .store
        .review_history(&scope(), Some(&id), 10)
        .unwrap();
    assert_eq!(history.len(), 1);
    assert_eq!(history[0].replay_count, 1);
    assert_eq!(history[0].after_status, ReviewStatus::Accepted);
    let audit_actions: Vec<_> = fixture
        .store
        .audit_events(&scope(), 20)
        .unwrap()
        .into_iter()
        .map(|audit| audit.action)
        .collect();
    assert!(audit_actions.contains(&"review_memory_patch".to_string()));
    assert!(audit_actions.contains(&"review_action".to_string()));

    // The review is terminal: a fresh approval is no longer applicable.
    assert_eq!(
        reviews
            .apply(&scope(), &id, action(ReviewActionKind::Approve))
            .unwrap_err()
            .to_string(),
        "review is no longer pending"
    );
    let statistics = reviews.statistics(&scope()).unwrap();
    assert_eq!(statistics.approved, 1);
    assert_eq!(statistics.reviewed, 1);
    assert_eq!(statistics.idempotent_replays, 1);
}

#[test]
fn validation_requires_evidence_and_records_provenance() {
    let fixture = Fixture::new();
    let reviews = service(&fixture);
    let subject = fixture.remember(&scope(), "fact", MemoryStatus::Hypothesis);
    let id = add_review(
        &fixture,
        &scope(),
        "candidate_memory",
        &subject,
        json!({"target_status": "validated"}),
    );
    assert_eq!(
        reviews
            .apply(&scope(), &id, action(ReviewActionKind::Approve))
            .unwrap_err()
            .to_string(),
        "validation requires independently supplied evidence"
    );
    let mut with_evidence = action(ReviewActionKind::Approve);
    with_evidence.evidence_uri = "https://example.test/spec".into();
    let result = reviews.apply(&scope(), &id, with_evidence).unwrap();
    assert_eq!(result.status, ReviewApplicationStatus::Applied);
    let memory = fixture.store.get(&scope(), &subject).unwrap();
    assert_eq!(memory.status, MemoryStatus::Validated);
    assert_eq!(memory.provenance.source_type, SourceType::Document);
    assert_eq!(memory.provenance.evidence_strength, 1.0);
    assert_eq!(
        fixture.store.review(&scope(), &id).unwrap().decision,
        "approved_as_validated"
    );
}

#[test]
fn reject_defer_resurface_and_reopen() {
    let fixture = Fixture::new();
    let reviews = service(&fixture);
    let subject = fixture.remember(&scope(), "dubious", MemoryStatus::Hypothesis);
    let rejected = add_review(&fixture, &scope(), "candidate_memory", &subject, json!({}));
    reviews
        .apply(&scope(), &rejected, action(ReviewActionKind::Reject))
        .unwrap();
    assert_eq!(
        fixture.store.get(&scope(), &subject).unwrap().status,
        MemoryStatus::Rejected
    );

    let mut reopen = action(ReviewActionKind::Reopen);
    assert_eq!(
        reviews
            .apply(&scope(), &rejected, reopen.clone())
            .unwrap_err()
            .to_string(),
        "reopen requires an explicit reason"
    );
    reopen.note = "new evidence".into();
    let reopened = reviews.apply(&scope(), &rejected, reopen).unwrap();
    assert_eq!(reopened.status, ReviewApplicationStatus::NoOp);
    assert_eq!(
        fixture.store.review(&scope(), &rejected).unwrap().status,
        ReviewStatus::Pending
    );

    let other = fixture.remember(&scope(), "later", MemoryStatus::Hypothesis);
    let deferred = add_review(&fixture, &scope(), "candidate_memory", &other, json!({}));
    let mut defer = action(ReviewActionKind::Defer);
    defer.deferred_until = Some(now_ms() - 1);
    assert_eq!(
        reviews
            .apply(&scope(), &deferred, defer.clone())
            .unwrap_err()
            .to_string(),
        "deferred review requires a future resurface time"
    );
    defer.deferred_until = Some(now_ms() + 60_000);
    reviews.apply(&scope(), &deferred, defer).unwrap();
    let item = fixture.store.review(&scope(), &deferred).unwrap();
    assert_eq!(item.status, ReviewStatus::Deferred);
    assert_eq!(
        fixture
            .store
            .resurface_due_reviews(&scope(), now_ms())
            .unwrap(),
        0
    );
    assert_eq!(
        fixture
            .store
            .resurface_due_reviews(&scope(), item.deferred_until)
            .unwrap(),
        1
    );
    let resurfaced = fixture.store.review(&scope(), &deferred).unwrap();
    assert_eq!(resurfaced.status, ReviewStatus::Pending);
    assert_eq!(resurfaced.decision, "resurface");
    let statistics = reviews.statistics(&scope()).unwrap();
    assert_eq!(
        (
            statistics.rejected,
            statistics.deferred,
            statistics.reopened,
            statistics.resurfaced
        ),
        (1, 1, 1, 1)
    );
}

#[test]
fn decide_review_enforces_compare_and_set() {
    let fixture = Fixture::new();
    let subject = fixture.remember(&scope(), "x", MemoryStatus::Hypothesis);
    let id = add_review(&fixture, &scope(), "candidate_memory", &subject, json!({}));
    let decision = ReviewDecision {
        status: ReviewStatus::Rejected,
        decision: "rejected".into(),
        decided_by: "user".into(),
        expected_revision: 5,
        ..Default::default()
    };
    assert_eq!(
        fixture
            .store
            .decide_review(&scope(), &id, decision.clone())
            .unwrap_err()
            .to_string(),
        "review decision rejected: stale revision"
    );
    let decided = fixture
        .store
        .decide_review(
            &scope(),
            &id,
            ReviewDecision {
                expected_revision: 0,
                ..decision.clone()
            },
        )
        .unwrap();
    assert_eq!(decided.revision, 1);
    assert_eq!(
        fixture
            .store
            .decide_review(
                &scope(),
                &id,
                ReviewDecision {
                    expected_revision: 1,
                    ..decision
                }
            )
            .unwrap_err()
            .to_string(),
        "review decision rejected: item is not in the required state"
    );
    let other = add_review(&fixture, &scope(), "candidate_memory", &subject, json!({}));
    let keyed = ReviewDecision {
        status: ReviewStatus::Rejected,
        decision: "rejected".into(),
        decided_by: "user".into(),
        action_key: "key-1".into(),
        ..Default::default()
    };
    fixture
        .store
        .decide_review(&scope(), &other, keyed.clone())
        .unwrap();
    assert_eq!(
        fixture
            .store
            .decide_review(&scope(), &id, keyed)
            .unwrap_err()
            .to_string(),
        "review action key belongs to another review"
    );
}

#[test]
fn consolidation_merge_creates_canonical_and_supersedes_sources() {
    let fixture = Fixture::new();
    let reviews = service(&fixture);
    let a = fixture.remember(&scope(), "use sqlite wal", MemoryStatus::Active);
    let b = fixture.remember(&scope(), "sqlite should use WAL", MemoryStatus::Hypothesis);
    let id = add_review(
        &fixture,
        &scope(),
        "semantic_consolidation",
        "",
        json!({"memory_ids": [a, b], "canonical_content": "SQLite uses WAL journaling"}),
    );
    let approve = reviews
        .preview(&scope(), &id, action(ReviewActionKind::Approve))
        .unwrap();
    assert!(!approve.applicable);
    assert!(approve.warnings.contains(
        &"choose merge, relate, supersede, or contradiction before applying".to_string()
    ));
    let preview = reviews
        .preview(&scope(), &id, action(ReviewActionKind::Merge))
        .unwrap();
    assert!(preview.applicable);
    assert!(
        preview
            .changes
            .iter()
            .any(|change| change.record_id == "$created:0" && change.operation == "create")
    );
    let result = reviews
        .apply(&scope(), &id, action(ReviewActionKind::Merge))
        .unwrap();
    let canonical = &result.changed_ids[0];
    let merged = fixture.store.get(&scope(), canonical).unwrap();
    assert_eq!(merged.content, "SQLite uses WAL journaling");
    assert_eq!(merged.status, MemoryStatus::Hypothesis);
    assert_eq!(merged.source, "review:canonical_merge");
    assert_eq!(merged.tags, ["reviewed_canonical"]);
    for source in [&a, &b] {
        let memory = fixture.store.get(&scope(), source).unwrap();
        assert_eq!(memory.status, MemoryStatus::Superseded);
        assert_eq!(&memory.replacement_id, canonical);
    }
    let edges = fixture
        .store
        .query_edges(
            &scope(),
            &GraphQuery {
                to_id: canonical.clone(),
                edge_type: "superseded_by".into(),
                ..Default::default()
            },
        )
        .unwrap();
    assert_eq!(edges.len(), 2);
    assert_eq!(
        fixture.store.review(&scope(), &id).unwrap().decision,
        "merged"
    );
    let history = fixture
        .store
        .review_history(&scope(), Some(&id), 1)
        .unwrap();
    let recorded: serde_json::Value = serde_json::from_str(&history[0].result_json).unwrap();
    assert_eq!(recorded["created_ids"], json!([canonical]));
}

#[test]
fn conflict_resolutions_relate_supersede_and_contradict() {
    let fixture = Fixture::new();
    let reviews = service(&fixture);
    let a = fixture.remember(&scope(), "port 80", MemoryStatus::Active);
    let b = fixture.remember(&scope(), "port 8080", MemoryStatus::Active);
    let conflict = |fixture: &Fixture| {
        add_review(
            fixture,
            &scope(),
            "conflict",
            "",
            json!({"memory_ids": [a, b], "severity": "critical"}),
        )
    };
    let first = conflict(&fixture);
    assert_eq!(
        fixture
            .store
            .critical_conflict_memory_ids(&scope())
            .unwrap(),
        {
            let mut ids = vec![a.clone(), b.clone()];
            ids.sort();
            ids
        }
    );
    reviews
        .apply(
            &scope(),
            &first,
            action(ReviewActionKind::MarkContradiction),
        )
        .unwrap();
    let edges = fixture
        .store
        .query_edges(
            &scope(),
            &GraphQuery {
                edge_type: "contradicts".into(),
                ..Default::default()
            },
        )
        .unwrap();
    assert_eq!(edges.len(), 1);
    let second = conflict(&fixture);
    let mut supersede = action(ReviewActionKind::Supersede);
    supersede.target_id = "mem_outside".into();
    assert!(
        !reviews
            .preview(&scope(), &second, supersede.clone())
            .unwrap()
            .applicable
    );
    supersede.target_id = b.clone();
    reviews.apply(&scope(), &second, supersede).unwrap();
    assert_eq!(
        fixture.store.get(&scope(), &a).unwrap().status,
        MemoryStatus::Superseded
    );
    assert_eq!(
        fixture.store.get(&scope(), &b).unwrap().status,
        MemoryStatus::Active
    );
    assert_eq!(
        fixture.store.review(&scope(), &second).unwrap().decision,
        "superseded"
    );
}

#[test]
fn staleness_retain_revalidate_deprecate_and_evidence() {
    let fixture = Fixture::new();
    let reviews = service(&fixture);
    let subject = fixture.remember(&scope(), "build uses make", MemoryStatus::Validated);
    let stale = add_review(&fixture, &scope(), "staleness", &subject, json!({}));
    assert!(
        !reviews
            .preview(&scope(), &stale, action(ReviewActionKind::Retain))
            .unwrap()
            .applicable
    );
    let mut retain = action(ReviewActionKind::Retain);
    retain.note = "still true".into();
    reviews.apply(&scope(), &stale, retain).unwrap();
    assert!(
        fixture
            .store
            .get(&scope(), &subject)
            .unwrap()
            .tags
            .contains(&"review:retained_current".to_string())
    );

    let stale2 = add_review(&fixture, &scope(), "staleness", &subject, json!({}));
    let mut revalidate = action(ReviewActionKind::Revalidate);
    revalidate.evidence_uri = "https://example.test/build".into();
    reviews.apply(&scope(), &stale2, revalidate).unwrap();
    assert_eq!(
        fixture.store.review(&scope(), &stale2).unwrap().decision,
        "revalidated"
    );

    let repair = add_review(&fixture, &scope(), "provenance_repair", &subject, json!({}));
    let mut attach = action(ReviewActionKind::AttachEvidence);
    attach.evidence_uri = "doc://handbook".into();
    reviews.apply(&scope(), &repair, attach).unwrap();
    let memory = fixture.store.get(&scope(), &subject).unwrap();
    assert_eq!(memory.provenance.source_uri, "doc://handbook");
    assert_eq!(memory.status, MemoryStatus::Validated);

    let stale3 = add_review(&fixture, &scope(), "staleness", &subject, json!({}));
    reviews
        .apply(&scope(), &stale3, action(ReviewActionKind::Deprecate))
        .unwrap();
    assert_eq!(
        fixture.store.get(&scope(), &subject).unwrap().status,
        MemoryStatus::Deprecated
    );
}

#[test]
fn narrow_scope_and_compensation() {
    let fixture = Fixture::new();
    let reviews = service(&fixture);
    let authority = scope();
    let subject = fixture.remember(&authority, "session rule", MemoryStatus::Validated);
    let id = add_review(
        &fixture,
        &authority,
        "candidate_memory",
        &subject,
        json!({}),
    );
    let mut narrow = action(ReviewActionKind::NarrowScope);
    narrow.edit = Some(ReviewEdit {
        scope: Some(durable_memory_scope(authority.clone())),
        ..Default::default()
    });
    assert_eq!(
        reviews
            .apply(&authority, &id, narrow.clone())
            .unwrap_err()
            .to_string(),
        "review create target scope cannot be broader than the review authority scope"
    );
    narrow.edit = Some(ReviewEdit {
        scope: Some(exact_scope("other", "repo_other", "s")),
        ..Default::default()
    });
    assert_eq!(
        reviews
            .apply(&authority, &id, narrow.clone())
            .unwrap_err()
            .to_string(),
        "review create target scope is unrelated to the review authority scope"
    );
    // As in the archived service, the created copy must be readable from the
    // authority scope, so the only accepted target is the authority scope
    // itself; the copy carries the reviewed lineage and drops validated trust.
    narrow.edit = Some(ReviewEdit {
        scope: Some(authority.clone()),
        content: Some("session rule (narrowed)".into()),
        ..Default::default()
    });
    let result = reviews.apply(&authority, &id, narrow).unwrap();
    let narrowed = fixture
        .store
        .get(&authority, &result.changed_ids[0])
        .unwrap();
    assert_eq!(narrowed.content, "session rule (narrowed)");
    assert_eq!(
        narrowed.status,
        MemoryStatus::Active,
        "validated trust does not carry over"
    );
    assert_eq!(narrowed.source, "review:narrow_scope");
    let original = fixture.store.get(&authority, &subject).unwrap();
    assert_eq!(original.status, MemoryStatus::Superseded);
    assert_eq!(original.replacement_id, narrowed.id);

    let mut compensate = action(ReviewActionKind::Compensate);
    compensate.note = "fix wording".into();
    compensate.target_id = narrowed.id.clone();
    compensate.edit = Some(ReviewEdit {
        target_status: Some(MemoryStatus::Validated),
        ..Default::default()
    });
    assert_eq!(
        reviews
            .apply(&authority, &id, compensate.clone())
            .unwrap_err()
            .to_string(),
        "compensation cannot bypass lifecycle or replacement governance"
    );
    compensate.edit = Some(ReviewEdit {
        content: Some("session rule (corrected)".into()),
        ..Default::default()
    });
    let compensated = reviews.apply(&authority, &id, compensate).unwrap();
    assert_eq!(compensated.status, ReviewApplicationStatus::Compensated);
    assert_eq!(
        fixture.store.get(&authority, &narrowed.id).unwrap().content,
        "session rule (corrected)"
    );
    let item = fixture.store.review(&authority, &id).unwrap();
    assert_eq!(item.status, ReviewStatus::Accepted);
    assert_eq!(
        item.decision, "narrowed_scope",
        "compensation keeps the original decision"
    );
    assert_eq!(item.application_status, "compensated");
    assert_eq!(reviews.statistics(&authority).unwrap().compensated, 1);
}

#[test]
fn dream_promotion_follows_policy_and_hydration_reports_diagnostics() {
    let fixture = Fixture::new();
    let dream = fixture.remember(&scope(), "maybe cache results", MemoryStatus::DreamArtifact);
    let id = add_review(
        &fixture,
        &scope(),
        "dream_artifact",
        &dream,
        json!({"source_memory_ids": ["mem_missing"], "cycle_id": "c1"}),
    );
    let disabled = ReviewService::new(
        Arc::clone(&fixture.store),
        ReviewGovernancePolicy::default(),
    );
    let preview = disabled
        .preview(&scope(), &id, action(ReviewActionKind::Approve))
        .unwrap();
    assert!(!preview.applicable);
    assert!(preview.warnings.contains(
        &"Dreaming promotion is disabled by the configured promotion policy".to_string()
    ));
    assert!(
        preview
            .warnings
            .contains(&"source memory mem_missing is missing or outside scope".to_string())
    );
    let hydrated = service(&fixture).hydrate(&scope(), &id).unwrap();
    match &hydrated.candidate.payload {
        ReviewCandidatePayload::DreamArtifact(candidate) => {
            assert_eq!(candidate.content, "maybe cache results")
        }
        other => panic!("unexpected payload {other:?}"),
    }
    assert_eq!(
        hydrated.recall.warnings,
        ["dream artifact is isolated from ordinary recall"]
    );
    assert!(hydrated.risk.components.contains_key("waiting_age"));
    let cycles = fixture
        .store
        .query_reviews(
            &scope(),
            &ReviewQuery {
                dream_cycles: vec!["c1".into()],
                ..Default::default()
            },
        )
        .unwrap();
    assert_eq!(cycles.len(), 1);
    // Reject remains available despite blocking diagnostics.
    let result = service(&fixture)
        .apply(&scope(), &id, action(ReviewActionKind::Reject))
        .unwrap();
    assert_eq!(result.status, ReviewApplicationStatus::Applied);
    assert_eq!(
        fixture.store.get(&scope(), &dream).unwrap().status,
        MemoryStatus::Rejected
    );
}

#[test]
fn queue_ranking_filters_and_view_state() {
    let fixture = Fixture::new();
    let reviews = service(&fixture);
    let subject = fixture.remember(&scope(), "x", MemoryStatus::Hypothesis);
    let low = fixture
        .store
        .add_review(
            &scope(),
            ReviewItemWrite {
                priority: "low".into(),
                subject_id: subject.clone(),
                tags: vec!["t1".into()],
                ..Default::default()
            },
        )
        .unwrap();
    let critical = add_review(
        &fixture,
        &scope(),
        "conflict",
        "",
        json!({"memory_ids": ["a", "b"], "severity": "critical"}),
    );
    let page = reviews.query(&scope(), &ReviewQuery::default()).unwrap();
    assert_eq!(
        page.iter().map(|item| item.id.clone()).collect::<Vec<_>>(),
        [critical.clone(), low.clone()]
    );
    assert!(
        page[1].queue_signals.trust_risk > 0.0,
        "subject signals are joined in"
    );
    let tagged = reviews
        .query(
            &scope(),
            &ReviewQuery {
                tags: vec!["t1".into()],
                ..Default::default()
            },
        )
        .unwrap();
    assert_eq!(tagged.len(), 1);
    let conflicts = reviews
        .query(
            &scope(),
            &ReviewQuery {
                conflict: Some(true),
                ..Default::default()
            },
        )
        .unwrap();
    assert_eq!(conflicts[0].id, critical);
    let second_page = reviews
        .query(
            &scope(),
            &ReviewQuery {
                offset: 1,
                limit: 1,
                ..Default::default()
            },
        )
        .unwrap();
    assert_eq!(second_page[0].id, low);

    let state = ReviewViewState {
        query: ReviewQuery {
            kinds: vec!["conflict".into()],
            stale: Some(false),
            limit: 25,
            ..Default::default()
        },
        selected_review_id: critical.clone(),
        queue_offset: 3,
        updated_at: 0,
    };
    fixture
        .store
        .save_review_view_state(&scope(), state.clone())
        .unwrap();
    let loaded = fixture
        .store
        .load_review_view_state(&scope())
        .unwrap()
        .unwrap();
    assert_eq!(loaded.query, state.query);
    assert_eq!(loaded.selected_review_id, critical);
    assert_eq!(loaded.queue_offset, 3);
    fixture.store.clear_review_view_state(&scope()).unwrap();
    assert!(
        fixture
            .store
            .load_review_view_state(&scope())
            .unwrap()
            .is_none()
    );

    let statistics = reviews.statistics(&scope()).unwrap();
    assert_eq!(statistics.pending, 2);
    assert_eq!(statistics.unresolved_critical_conflicts, 1);
    assert_eq!(statistics.pending_by_risk_class["critical"], 1);
    let encoded = review_statistics_json(&statistics);
    assert_eq!(encoded["pending_by_kind"]["conflict"], 1);
}

#[test]
fn inbox_merges_durable_queue_and_runs_batches() {
    let fixture = Fixture::new();
    let inbox = ReviewInbox::new(service(&fixture), scope());
    let durable = durable_memory_scope(scope());
    let exact_subject = fixture.remember(&scope(), "exact", MemoryStatus::Hypothesis);
    let durable_subject = fixture.remember(&durable, "durable", MemoryStatus::Hypothesis);
    let exact_review = add_review(
        &fixture,
        &scope(),
        "candidate_memory",
        &exact_subject,
        json!({}),
    );
    let durable_review = add_review(
        &fixture,
        &durable,
        "candidate_memory",
        &durable_subject,
        json!({}),
    );
    assert_eq!(
        inbox.review_items(&ReviewQuery::default()).unwrap().len(),
        2
    );
    let only_exact = inbox
        .review_items(&ReviewQuery {
            scope: ReviewScopeSelector::Exact,
            ..Default::default()
        })
        .unwrap();
    assert_eq!(only_exact.len(), 1);
    assert_eq!(
        inbox.review_detail(&durable_review).unwrap().item.id,
        durable_review,
        "durable fallback"
    );
    assert_eq!(inbox.review_statistics().unwrap().pending, 2);

    let mut items: Vec<ReviewBatchItem> = [&exact_review, &durable_review]
        .iter()
        .map(|id| ReviewBatchItem {
            review_id: (*id).clone(),
            action: action(ReviewActionKind::Reject),
        })
        .collect();
    let preview = inbox.preview_review_batch(&items).unwrap();
    assert!(preview.applicable, "{:?}", preview.warnings);
    for (item, previewed) in items.iter_mut().zip(&preview.items) {
        item.action.idempotency_key = previewed.idempotency_token.clone();
        item.action.expected_revision = previewed.review_revision;
    }
    assert_eq!(
        inbox
            .apply_review_batch(items.clone(), preview.idempotency_token.clone())
            .unwrap_err()
            .to_string(),
        "review batch apply requires each preview revision and idempotency key",
        "revision 0 is indistinguishable from an unset preview revision"
    );
    let mixed = vec![
        ReviewBatchItem {
            review_id: exact_review.clone(),
            action: action(ReviewActionKind::Reject),
        },
        ReviewBatchItem {
            review_id: durable_review.clone(),
            action: action(ReviewActionKind::Defer),
        },
    ];
    assert_eq!(
        inbox.preview_review_batch(&mixed).unwrap().warnings,
        ["batch items must have IDs and homogeneous action semantics"]
    );
    let merge = vec![ReviewBatchItem {
        review_id: exact_review.clone(),
        action: action(ReviewActionKind::Merge),
    }];
    assert_eq!(
        inbox.preview_review_batch(&merge).unwrap().warnings,
        ["batch supports only reject, defer, or homogeneous low-risk approval"]
    );
}

#[test]
fn batch_jobs_apply_previewed_items_in_background() {
    let fixture = Fixture::new();
    let inbox = ReviewInbox::new(service(&fixture), scope());
    // Defer then resurface so the reviews have a non-zero revision, as a
    // batch apply requires the preview revision.
    let mut ids = Vec::new();
    for content in ["one", "two"] {
        let subject = fixture.remember(&scope(), content, MemoryStatus::Hypothesis);
        let id = add_review(&fixture, &scope(), "candidate_memory", &subject, json!({}));
        let mut defer = action(ReviewActionKind::Defer);
        defer.deferred_until = Some(now_ms() + 100);
        inbox.apply_review_action(&id, defer).unwrap();
        ids.push(id);
    }
    std::thread::sleep(std::time::Duration::from_millis(150));
    fixture
        .store
        .resurface_due_reviews(&scope(), now_ms())
        .unwrap();
    let mut items: Vec<ReviewBatchItem> = ids
        .iter()
        .map(|id| ReviewBatchItem {
            review_id: id.clone(),
            action: action(ReviewActionKind::Reject),
        })
        .collect();
    let preview = inbox.preview_review_batch(&items).unwrap();
    assert!(preview.applicable);
    for (item, previewed) in items.iter_mut().zip(&preview.items) {
        item.action.idempotency_key = previewed.idempotency_token.clone();
        item.action.expected_revision = previewed.review_revision;
    }
    assert_eq!(
        inbox
            .start_review_batch(items.clone(), String::new())
            .unwrap_err()
            .to_string(),
        "review batch apply requires the preview idempotency token"
    );
    let job = inbox
        .start_review_batch(items.clone(), preview.idempotency_token.clone())
        .unwrap();
    assert!(job.id.starts_with("review-batch-job-1-"));
    assert_eq!(job.status, ReviewBatchJobStatus::Queued);
    let finished = loop {
        let current = inbox.review_batch_job(&job.id).unwrap();
        if matches!(
            current.status,
            ReviewBatchJobStatus::Completed | ReviewBatchJobStatus::Failed
        ) {
            break current;
        }
        std::thread::sleep(std::time::Duration::from_millis(5));
    };
    assert_eq!(
        finished.status,
        ReviewBatchJobStatus::Completed,
        "{}",
        finished.error
    );
    let result = finished.result.unwrap();
    assert_eq!(
        (
            result.applied_count,
            result.failed_count,
            result.partial_failure
        ),
        (2, 0, false)
    );
    assert_eq!(
        review_batch_job_json(&inbox.review_batch_job(&job.id).unwrap())["status"],
        "completed"
    );
    // A replay of the same batch is idempotent per item.
    let replay = inbox
        .apply_review_batch(items, preview.idempotency_token)
        .unwrap();
    assert!(replay.items.iter().all(|item| {
        item.result
            .as_ref()
            .is_some_and(|result| result.idempotent_replay)
    }));
    let statistics = inbox.review_statistics().unwrap();
    assert_eq!(statistics.batch_runs, 2);
    assert_eq!(statistics.batch_idempotent_replays, 2);
    assert_eq!(
        inbox.review_batch_job("nope").unwrap_err().to_string(),
        "review batch job not found"
    );
    inbox.shutdown();
}
