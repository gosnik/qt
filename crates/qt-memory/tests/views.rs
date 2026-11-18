//! GUI view models against a real runtime: the Review Inbox flow
//! (detail → inputs → preview → confirm → apply, batches, view state) and
//! the dashboard operations.

use std::collections::BTreeSet;

use qt_memory::review::*;
use qt_memory::views::dashboard::{
    self, CaptureWrite, ContextBuild, LifecycleAction, MemoryFilter,
};
use qt_memory::views::review as inbox_view;
use qt_memory::*;
use serde_json::json;

fn runtime(home: &std::path::Path, cwd: &std::path::Path) -> MemoryRuntime {
    MemoryRuntime::new(MemoryRuntimeOptions {
        agent_home: home.into(),
        cwd: cwd.into(),
        session: "s1".into(),
        agent: "agent-1".into(),
        coordinator: MemoryCoordinatorSettings {
            enabled: true,
            ..Default::default()
        },
        ..Default::default()
    })
    .unwrap()
}

fn remember(runtime: &MemoryRuntime, content: &str, status: MemoryStatus) -> String {
    runtime
        .memories()
        .remember(
            &runtime.scope(),
            MemoryWrite {
                content: content.into(),
                status,
                source: "test".into(),
                provenance: Provenance {
                    source_type: SourceType::File,
                    source_uri: "file:fixture".into(),
                    evidence_strength: 0.8,
                    ..Default::default()
                },
                ..Default::default()
            },
        )
        .unwrap()
}

fn add_review(runtime: &MemoryRuntime, subject: &str, content: &str) -> String {
    runtime
        .store()
        .add_review(
            &runtime.scope(),
            ReviewItemWrite {
                kind: "candidate_memory".into(),
                reason: "needs review".into(),
                subject_id: subject.into(),
                provenance: Provenance {
                    source_type: SourceType::File,
                    source_uri: "file:evidence".into(),
                    evidence_strength: 0.9,
                    ..Default::default()
                },
                candidate_json: json!({"content": content, "reason": "r"}).to_string(),
                ..Default::default()
            },
        )
        .unwrap()
}

#[test]
fn review_inbox_edit_and_approve_flow() {
    let home = tempfile::tempdir().unwrap();
    let cwd = tempfile::tempdir().unwrap();
    let runtime = runtime(home.path(), cwd.path());
    let subject = remember(&runtime, "candidate fact", MemoryStatus::Candidate);
    let id = add_review(&runtime, &subject, "edited proposal");
    let inbox = runtime.reviews();
    let now = now_ms();

    let landing = inbox_view::landing_lines(inbox, now).unwrap();
    assert_eq!(landing[0], "Memory Review Inbox");
    assert!(landing[2].starts_with("Pending 1"));
    assert!(
        landing
            .iter()
            .any(|line| line == "Categories: candidate_memory=1")
    );

    let items = inbox.review_items(&ReviewQuery::default()).unwrap();
    let detail = inbox.review_detail(&id).unwrap();
    let (label, _) = inbox_view::queue_card(0, items.len(), &items[0], Ok(&detail), now);
    assert_eq!(label, "1/1  normal  candidate_memory");
    let lines = inbox_view::detail_lines(&detail, "Pending reviews", &inbox.scope(), now);
    assert_eq!(lines[0], "Review Detail  [normal] candidate_memory");
    assert!(lines[3].starts_with("Proposal: edited proposal"));

    let preview = inbox.preview_review(
        &id,
        inbox_view::acceptance_preview_action(detail.candidate.kind),
    );
    let accept = inbox_view::acceptance_preview_lines(
        preview.as_ref().map_err(|error| error.to_string()),
        &inbox.scope(),
    );
    assert!(accept.iter().any(|line| line.starts_with("  Will: ")));

    let kind = ReviewActionKind::EditAndApprove;
    let mut action = inbox_view::base_action(&detail, kind, &inbox.scope()).unwrap();
    for input in inbox_view::action_inputs(&detail, kind) {
        match input {
            inbox_view::ActionInput::Content { .. } => {
                let text = inbox_view::content_initial(&detail, kind);
                inbox_view::apply_content(&mut action, &format!("{text} (reviewed)"));
            }
            inbox_view::ActionInput::EditFields => {
                inbox_view::apply_edit_field(&mut action, inbox_view::EditField::Tags, "x, y")
                    .unwrap();
                inbox_view::apply_edit_field(
                    &mut action,
                    inbox_view::EditField::Status,
                    "hypothesis",
                )
                .unwrap();
            }
            inbox_view::ActionInput::Note => action.note = "looks right".into(),
            other => panic!("unexpected input {other:?}"),
        }
    }
    let preview = inbox.preview_review(&id, action.clone()).unwrap();
    assert!(preview.applicable, "{:?}", preview.warnings);
    let confirm = inbox_view::confirmation_lines(
        &id,
        &action,
        &preview,
        detail.subject.as_ref().map(|s| &s.scope),
        &inbox.scope(),
    );
    assert_eq!(confirm[0], "Confirm edit_and_approve");
    assert!(confirm.iter().any(|line| line == "Audit note: looks right"));
    inbox_view::confirm_action(&mut action, &preview);
    let result = inbox.apply_review_action(&id, action).unwrap();
    let receipt = inbox_view::receipt_lines(&id, &result);
    assert!(receipt[0].contains("decision=persisted"), "{receipt:?}");
    let stored = runtime.memories().get(&runtime.scope(), &subject).unwrap();
    assert_eq!(stored.content, "edited proposal (reviewed)");
    assert_eq!(stored.status, MemoryStatus::Hypothesis);
    assert_eq!(stored.tags, ["x", "y"]);

    let history = inbox.review_history(None, 50).unwrap();
    assert!(inbox_view::history_matches(&history[0], "accepted"));
    assert_eq!(
        inbox_view::history_receipt_lines(&history[0])[2],
        "Action: edited_and_approved_as_hypothesis by user:interactive"
    );

    // Persisted view state round-trips.
    inbox
        .save_review_view_state(ReviewViewState {
            query: ReviewQuery {
                kinds: vec!["conflict".into()],
                ..Default::default()
            },
            selected_review_id: id.clone(),
            queue_offset: 2,
            updated_at: 0,
        })
        .unwrap();
    let restored = inbox.load_review_view_state().unwrap().unwrap();
    assert_eq!(restored.selected_review_id, id);
    assert!(inbox_view::has_active_filters(&restored.query));
}

#[test]
fn review_batch_reject_keeps_failures_selected() {
    let home = tempfile::tempdir().unwrap();
    let cwd = tempfile::tempdir().unwrap();
    let runtime = runtime(home.path(), cwd.path());
    let a = remember(&runtime, "first", MemoryStatus::Candidate);
    let b = remember(&runtime, "second", MemoryStatus::Candidate);
    let ids: BTreeSet<String> = [
        add_review(&runtime, &a, "one"),
        add_review(&runtime, &b, "two"),
    ]
    .into_iter()
    .collect();
    let inbox = runtime.reviews();
    let items = inbox.review_items(&ReviewQuery::default()).unwrap();
    let summary = inbox_view::batch_summary_lines(inbox, &items, &ids);
    assert_eq!(summary[1], "Selected: 2 / 2");
    let mut batch = inbox_view::batch_items(&ids, ReviewActionKind::Reject);
    let preview = inbox.preview_review_batch(&batch).unwrap();
    assert!(preview.applicable);
    let lines = inbox_view::batch_preview_lines(
        ReviewActionKind::Reject,
        &preview,
        &ReviewQuery::default(),
        batch.len(),
    );
    assert_eq!(lines[0], "Confirm batch reject");
    inbox_view::confirm_batch(&mut batch, &preview);
    // Revision 0 (never decided) cannot be told apart from an unset preview
    // revision; the store refuses, as archived.
    assert_eq!(
        inbox
            .apply_review_batch(batch, preview.idempotency_token.clone())
            .unwrap_err()
            .to_string(),
        "review batch apply requires each preview revision and idempotency key"
    );
    let result = ReviewBatchResult {
        items: vec![ReviewBatchItemResult {
            review_id: "r1".into(),
            error: "stale".into(),
            ..Default::default()
        }],
        failed_count: 1,
        partial_failure: true,
        ..Default::default()
    };
    let (lines, failed) = inbox_view::batch_result(&result, 1);
    assert_eq!(
        lines,
        [
            "Memory review batch result: processed=1/1; applied=0; failed=1; partial failure",
            "Failed batch selections were preserved for retry or inspection."
        ]
    );
    assert!(failed.contains("r1"));
}

#[test]
fn dashboard_operations() {
    let home = tempfile::tempdir().unwrap();
    let cwd = tempfile::tempdir().unwrap();
    let runtime = runtime(home.path(), cwd.path());
    let written = dashboard::write_memory(
        &runtime,
        &CaptureWrite {
            content: "Use cargo nextest for the workspace tests".into(),
            memory_type: MemoryType::Decision,
            // Capped: anything above hypothesis is stored as hypothesis.
            status: MemoryStatus::Validated,
            tags: vec!["testing".into()],
            ..Default::default()
        },
    )
    .unwrap();
    assert_eq!(written.status, MemoryStatus::Hypothesis);
    let other = remember(
        &runtime,
        "Prefer small focused commits",
        MemoryStatus::Active,
    );
    runtime
        .store()
        .add_edge(
            &runtime.scope(),
            GraphEdge::new(&written.id, &other, "related"),
        )
        .unwrap();

    let summary = dashboard::summary(&runtime).unwrap();
    assert_eq!(summary.memory_entries, 2);
    assert!(summary.health_score <= 100);

    let found = dashboard::memories(
        &runtime,
        &MemoryFilter {
            memory_type: Some(MemoryType::Decision),
            ..Default::default()
        },
    )
    .unwrap();
    assert_eq!(found.len(), 1);
    let graph = dashboard::graph(&runtime, &MemoryFilter::default(), "", 250).unwrap();
    assert_eq!(graph.nodes.len(), 2);
    assert_eq!(graph.edges.len(), 1);

    let promoted = dashboard::transition(&runtime, &written.id, LifecycleAction::Promote).unwrap();
    assert_eq!(promoted.status, MemoryStatus::Active);
    // Durable (branch/repository) memories are governed in the durable scope.
    let durable = durable_memory_scope(runtime.scope());
    let durable_id = runtime
        .memories()
        .remember(
            &durable,
            MemoryWrite {
                content: "Durable decision".into(),
                status: MemoryStatus::Hypothesis,
                source: "test".into(),
                ..Default::default()
            },
        )
        .unwrap();
    let deprecated =
        dashboard::transition(&runtime, &durable_id, LifecycleAction::Deprecate).unwrap();
    assert_eq!(deprecated.status, MemoryStatus::Deprecated);
    let detail = dashboard::memory_detail(&runtime, &written.id).unwrap();
    assert_eq!(detail.entry.status, MemoryStatus::Active);
    assert!(!detail.revisions.is_empty());
    assert!(dashboard::revision_line(&detail.revisions[0]).contains("  "));

    let processed = dashboard::process_text(
        &runtime,
        "We decided to always run clippy before committing.",
        "",
        Vec::new(),
        true,
    )
    .unwrap();
    assert!(processed.stored_ids.is_empty());

    let built = dashboard::build_context(
        &runtime,
        &ContextBuild {
            query: "cargo nextest".into(),
            ..Default::default()
        },
    )
    .unwrap();
    assert_eq!(built.budget_tokens, 900);
    assert!(built.memory_ids.contains(&written.id), "{built:?}");
    assert_eq!(
        dashboard::build_context(
            &runtime,
            &ContextBuild {
                query: "x".into(),
                budget_tokens: 0,
                ..Default::default()
            }
        )
        .unwrap_err()
        .to_string(),
        "context build budget_tokens must be from 1 to 16000"
    );

    let sessions = dashboard::group_rows(
        &dashboard::memories(&runtime, &MemoryFilter::default()).unwrap(),
        false,
    );
    // The session memories plus the durable one (no session: "unknown").
    let ids: Vec<&str> = sessions.iter().map(|row| row.id.as_str()).collect();
    assert_eq!(ids, ["s1", "unknown"]);
    assert!(!dashboard::audit_lines(&runtime, 20).unwrap().is_empty());
    assert!(dashboard::system_lines(&runtime)[1].starts_with("Schema version: "));
    assert!(dashboard::retrieval_lines(&runtime)[0].starts_with("Retrieval: "));
}

#[test]
fn review_inbox_landing_names_the_newest_dream_review() {
    let home = tempfile::tempdir().unwrap();
    let cwd = tempfile::tempdir().unwrap();
    let runtime = runtime(home.path(), cwd.path());
    let add = |priority: &str, subject: &str| {
        runtime
            .store()
            .add_review(
                &runtime.scope(),
                ReviewItemWrite {
                    kind: "dream_artifact".into(),
                    priority: priority.into(),
                    reason: "dream output is isolated from trusted recall".into(),
                    subject_id: subject.into(),
                    candidate_json: "{}".into(),
                    ..Default::default()
                },
            )
            .unwrap()
    };
    let first = remember(&runtime, "older dream", MemoryStatus::Hypothesis);
    let older = add("high", &first);
    std::thread::sleep(std::time::Duration::from_millis(5));
    let second = remember(&runtime, "newer dream", MemoryStatus::Hypothesis);
    let newer = add("low", &second);
    let queue = runtime
        .reviews()
        .review_items(&ReviewQuery {
            kinds: vec!["dream_artifact".into()],
            limit: 1,
            ..Default::default()
        })
        .unwrap();
    assert_eq!(queue[0].id, older, "queue order ranks the older item first");
    let landing = inbox_view::landing_lines(runtime.reviews(), now_ms()).unwrap();
    let line = landing
        .iter()
        .find(|line| line.starts_with("Latest Dreaming: "))
        .unwrap();
    assert!(
        line.starts_with(&format!("Latest Dreaming: dream_artifact {newer} (")),
        "{line}"
    );
}
