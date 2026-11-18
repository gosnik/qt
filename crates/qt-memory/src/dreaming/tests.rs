use std::collections::BTreeSet;
use std::sync::Mutex;

use serde_json::json;

use super::*;
use crate::post_processing::test_support::{remember, scope, store};
use crate::settings::MemorySettings;
use crate::text::{dump, stable_hash_hex};

type Handler =
    Box<dyn Fn(&ChildRequest) -> std::result::Result<ChildOutcome, String> + Send + Sync>;

struct FakeChild {
    handler: Handler,
    requests: Mutex<Vec<ChildRequest>>,
}

impl FakeChild {
    fn new(
        handler: impl Fn(&ChildRequest) -> std::result::Result<ChildOutcome, String>
        + Send
        + Sync
        + 'static,
    ) -> Arc<Self> {
        Arc::new(Self {
            handler: Box::new(handler),
            requests: Mutex::new(Vec::new()),
        })
    }

    fn requests(&self) -> Vec<ChildRequest> {
        self.requests.lock().unwrap().clone()
    }
}

impl IsolatedChild for FakeChild {
    fn run(&self, request: &ChildRequest) -> std::result::Result<ChildOutcome, String> {
        self.requests.lock().unwrap().push(request.clone());
        (self.handler)(request)
    }
}

fn settings() -> MemoryDreamingSettings {
    let mut settings = MemorySettings::default().dreaming;
    settings.enabled = true;
    settings.trigger = "post_turn".into();
    settings.model = Some("llama/dreamer".into());
    settings
}

fn config(settings: MemoryDreamingSettings) -> DreamingConfig {
    let mut config = DreamingConfig::new(settings);
    config.agent_home = Some(PathBuf::from("/agent"));
    config
}

/// A Dream naming the first two candidate IDs the child was shown.
fn dream_answer(request: &ChildRequest, research_required: bool) -> String {
    let input: Value = serde_json::from_str(&request.input).unwrap();
    let ids: Vec<String> = input["candidates"]
        .as_array()
        .unwrap()
        .iter()
        .take(2)
        .map(|candidate| candidate["id"].as_str().unwrap().to_string())
        .collect();
    json!({
        "version": 1,
        "source_memory_ids": ids,
        "artifact_kind": "pattern_discovery",
        "content": "Both services retry with exponential backoff.",
        "confidence": 0.8,
        "research_required": research_required
    })
    .to_string()
}

fn seeded(
    count: usize,
) -> (
    tempfile::TempDir,
    Arc<MemoryStore>,
    MemoryScope,
    Vec<String>,
) {
    let (dir, store) = store();
    let scope = scope();
    let ids = (0..count)
        .map(|index| {
            remember(
                &store,
                &scope,
                &format!("governed fact number {index} about retries"),
            )
        })
        .collect();
    (dir, store, scope, ids)
}

#[test]
fn policies_config_and_cycle_ids() {
    assert_eq!(
        PromotionPolicy::AutoValidate.capped_by(PromotionPolicy::ReviewOnly),
        PromotionPolicy::ReviewOnly
    );
    assert_eq!(
        PromotionPolicy::Disabled.capped_by(PromotionPolicy::AutoValidate),
        PromotionPolicy::Disabled
    );
    assert_eq!(
        PromotionPolicy::Manual.capped_by(PromotionPolicy::ReviewOnly),
        PromotionPolicy::Manual
    );
    assert!(
        !PromotionPolicy::Disabled
            .review_governance()
            .allow_dream_hypothesis_promotion
    );
    assert!(
        PromotionPolicy::ReviewOnly
            .review_governance()
            .allow_dream_hypothesis_promotion
    );
    assert_eq!(
        PromotionPolicy::parse("auto_validate").as_str(),
        "auto_validate"
    );
    assert_eq!(ResearchPolicy::parse("required"), ResearchPolicy::Required);
    assert_eq!(
        DreamTrigger::parse("candidate_threshold"),
        Some(DreamTrigger::CandidateThreshold)
    );
    assert_eq!(DreamTrigger::parse("never"), None);

    let mut settings = settings();
    settings.model = Some("auto".into());
    let mut config = config(settings);
    assert_eq!(config.resolved_model(), None);
    config.prehandler_model = Some("llama/small".into());
    assert_eq!(config.resolved_model().as_deref(), Some("llama/small"));
    config.consolidation_model = Some("llama/big".into());
    assert_eq!(config.resolved_model().as_deref(), Some("llama/big"));
    config.settings.max_candidates = 40;
    assert_eq!(config.candidate_limit(), 16);
    config.settings.max_candidates = 0;
    assert_eq!(config.candidate_limit(), 2);

    let id = generate_cycle_id();
    assert_eq!(id.len(), 36);
    assert_eq!(&id[14..15], "4");
    assert!(matches!(&id[19..20], "8" | "9" | "a" | "b"));
    assert_ne!(id, generate_cycle_id());
}

#[test]
fn enqueue_gates_coalesces_and_prioritises() {
    let (_dir, store, scope, _) = seeded(0);
    let child = FakeChild::new(|_| Err("unused".into()));
    let mut disabled = settings();
    disabled.enabled = false;
    let service = DreamingService::new(
        store.clone(),
        config(disabled),
        scope.clone(),
        Some(child.clone()),
    );
    assert!(!service.enqueue(DreamTrigger::Manual, None));
    let no_child = DreamingService::new(store.clone(), config(settings()), scope.clone(), None);
    assert!(!no_child.enqueue(DreamTrigger::Manual, None));
    assert_eq!(
        dream_command(&no_child, "run"),
        [
            "Memory Dreaming cycle was not queued; enable Dreaming and verify its queue/model settings."
        ]
    );

    let service = DreamingService::new(
        store.clone(),
        config(settings()),
        scope.clone(),
        Some(child),
    );
    assert!(
        !service.enqueue(DreamTrigger::Manual, Some(DreamResearchEvidence::default())),
        "research disabled"
    );
    assert!(service.enqueue(DreamTrigger::PostTurn, None));
    let first_cycle = service.shared.lock().pending[0].cycle_id.clone();
    assert!(service.enqueue(DreamTrigger::PostTurn, None));
    {
        let state = service.shared.lock();
        assert_eq!(state.pending.len(), 1, "post_turn requests coalesce");
        assert_ne!(state.pending[0].cycle_id, first_cycle);
        assert_eq!(state.pending[0].scope.kind, ScopeKind::Repository);
    }
    assert!(service.enqueue(DreamTrigger::Manual, None));
    assert_eq!(
        service.shared.lock().pending[0].trigger,
        DreamTrigger::Manual
    );
    for _ in 0..6 {
        assert!(service.enqueue(DreamTrigger::CandidateThreshold, None));
    }
    assert_eq!(service.status().pending_runs, MAX_PENDING_DREAMS);
    assert!(!service.enqueue(DreamTrigger::CandidateThreshold, None));
}

#[test]
fn post_turn_waits_for_idle_and_foreground() {
    let (_dir, store, scope, _) = seeded(0);
    let child = FakeChild::new(|_| Err("unused".into()));
    let mut settings = settings();
    settings.idle_seconds = 30;
    let service = DreamingService::new(store, config(settings), scope, Some(child));
    assert!(service.note_capture(2, 2));
    assert!(!service.note_capture(0, 0), "nothing extracted");
    let now = Instant::now();
    let status = service.status_at(now);
    assert!(status.waiting_for_idle);
    let config = service.config().clone();
    let mut state = service.shared.lock();
    let start = state.last_activity;
    assert_eq!(
        state.poll(&config, false, start),
        Poll::WaitUntil(start + Duration::from_secs(30))
    );
    state.foreground_active = true;
    assert_eq!(
        state.poll(&config, false, start + Duration::from_secs(60)),
        Poll::WaitForSignal
    );
    state.foreground_active = false;
    match state.poll(&config, false, start + Duration::from_secs(31)) {
        Poll::Ready(dream) => assert_eq!(dream.trigger, DreamTrigger::PostTurn),
        other => panic!("{other:?}"),
    }
    assert!(state.running);
    assert_eq!(
        state.poll(&config, false, start + Duration::from_secs(31)),
        Poll::WaitForSignal
    );
}

#[test]
fn interval_and_threshold_triggers() {
    let (_dir, store, scope, _) = seeded(0);
    let child = FakeChild::new(|_| Err("unused".into()));
    let mut interval = settings();
    interval.trigger = "interval".into();
    interval.interval_seconds = 100;
    interval.idle_seconds = 10;
    let service = DreamingService::new(
        store.clone(),
        config(interval),
        scope.clone(),
        Some(child.clone()),
    );
    assert!(!service.note_capture(3, 3), "interval mode ignores capture");
    let cfg = service.config().clone();
    let mut state = service.shared.lock();
    let start = state.last_activity;
    assert_eq!(
        state.poll(&cfg, true, start),
        Poll::WaitUntil(start + Duration::from_secs(100))
    );
    state.foreground_active = true;
    assert_eq!(
        state.poll(&cfg, true, start + Duration::from_secs(200)),
        Poll::WaitForSignal
    );
    state.foreground_active = false;
    state.last_activity = start + Duration::from_secs(195);
    assert_eq!(
        state.poll(&cfg, true, start + Duration::from_secs(200)),
        Poll::WaitUntil(start + Duration::from_secs(205))
    );
    match state.poll(&cfg, true, start + Duration::from_secs(206)) {
        Poll::Ready(dream) => assert_eq!(dream.trigger, DreamTrigger::Interval),
        other => panic!("{other:?}"),
    }
    assert_eq!(state.next_dream_at, Some(start + Duration::from_secs(306)));
    drop(state);

    let mut threshold = settings();
    threshold.trigger = "candidate_threshold".into();
    threshold.candidate_threshold = 3;
    let service = DreamingService::new(store, config(threshold), scope, Some(child));
    assert!(!service.note_capture(2, 2));
    assert!(service.note_capture(1, 1));
    assert_eq!(service.shared.lock().new_candidate_count, 0);
    assert!(!service.note_capture(5, 0));
    let mut state = service.shared.lock();
    match state.poll(&service.shared.config, false, Instant::now()) {
        Poll::Ready(dream) => assert_eq!(dream.trigger, DreamTrigger::CandidateThreshold),
        other => panic!("{other:?}"),
    }
}

#[test]
fn successful_cycle_creates_pending_artifact_and_receipts() {
    let (_dir, store, scope, ids) = seeded(5);
    let child = FakeChild::new(|request| Ok(ChildOutcome::completed(dream_answer(request, false))));
    let mut settings = settings();
    settings.max_candidates = 4;
    let service = DreamingService::new(
        store.clone(),
        config(settings),
        scope.clone(),
        Some(child.clone()),
    );
    assert!(service.enqueue(DreamTrigger::Manual, None));
    let report = service.run_ready().unwrap();
    assert!(report.success, "{report:?}");
    assert!(report.error.is_empty());

    let request = &child.requests()[0];
    assert_eq!(request.task, ChildTask::Dreaming);
    assert_eq!(request.system_prompt, DREAMING_PROMPT);
    assert_eq!(request.model, "llama/dreamer");
    assert!(request.cli_arguments().contains(&"--no-tools".to_string()));
    let input: Value = serde_json::from_str(&request.input).unwrap();
    assert_eq!(input["trigger"], "manual");
    assert_eq!(input["scope"]["project"], "demo");
    assert_eq!(input["candidates"].as_array().unwrap().len(), 4);

    let dream = store.get(&scope, &report.dream_id).unwrap();
    assert_eq!(dream.status, MemoryStatus::DreamArtifact);
    assert!(
        dream
            .tags
            .iter()
            .any(|tag| tag == "dream:kind:pattern-discovery")
    );
    assert!(!dream.tags.iter().any(|tag| tag == "research:pending"));
    let review = store.review(&scope, &report.review_id).unwrap();
    assert_eq!(review.kind, "dream_artifact");
    assert_eq!(review.status, ReviewStatus::Pending);

    let receipt = report.receipt.unwrap();
    assert_eq!(receipt["review_created"], true);
    assert_eq!(receipt["selection_algorithm"], DREAMING_SELECTION_ALGORITHM);
    assert_eq!(receipt["protocol_version"], DREAMING_PROTOCOL_VERSION);
    assert_eq!(receipt["candidate_pool_count"], 5);
    let pool: Vec<String> = store
        .query(
            &scope,
            &MemoryQuery {
                limit: 32,
                policy: RecallPolicy::Strict,
                statuses: crate::post_processing::governed_statuses(),
                ..Default::default()
            },
        )
        .unwrap()
        .into_iter()
        .map(|memory| memory.id)
        .collect();
    assert_eq!(pool.len(), ids.len());
    assert_eq!(
        receipt["candidate_pool_hash"],
        stable_hash_hex(&dump(&json!(pool)))
    );
    assert_eq!(receipt["source_pack_hash"], stable_hash_hex(&request.input));
    assert_eq!(receipt["selection_seed"], receipt["cycle_id"]);
    assert_eq!(receipt["input_candidate_ids"].as_array().unwrap().len(), 4);
    assert_eq!(receipt["model_output_chars"], request_output_len(&child));
    assert!(receipt.get("auto_validation").is_none());
    assert_eq!(
        dream.provenance.source_uri,
        format!(
            "memory://dreaming/{}",
            receipt["claim_key"].as_str().unwrap()
        )
    );

    let status = service.status();
    assert_eq!(status.completed_runs, 1);
    assert_eq!(status.last_dream_id, report.dream_id);
    assert!(!status.running);
    let statistics = service.statistics().unwrap();
    assert_eq!(statistics.attempted, 1);
    assert_eq!(statistics.completed, 1);
    assert_eq!(statistics.reviews_created, 1);
    assert_eq!(statistics.by_trigger.get("manual"), Some(&1));
    assert_eq!(statistics.by_model.get("llama/dreamer"), Some(&1));
}

fn request_output_len(child: &FakeChild) -> usize {
    dream_answer(&child.requests()[0], false).len()
}

#[test]
fn sampling_prefers_least_dreamed_records() {
    let (_dir, store, scope, ids) = seeded(6);
    let child = FakeChild::new(|request| Ok(ChildOutcome::completed(dream_answer(request, false))));
    let mut settings = settings();
    settings.max_candidates = 2;
    let service = DreamingService::new(
        store.clone(),
        config(settings),
        scope.clone(),
        Some(child.clone()),
    );
    let mut seen = BTreeSet::new();
    for _ in 0..3 {
        assert!(service.enqueue(DreamTrigger::Manual, None));
        let report = service.run_ready().unwrap();
        assert!(report.success);
        let receipt = report.receipt.unwrap();
        for id in receipt["input_candidate_ids"].as_array().unwrap() {
            assert!(
                seen.insert(id.as_str().unwrap().to_string()),
                "{id} reused before others"
            );
        }
    }
    assert_eq!(seen, ids.into_iter().collect::<BTreeSet<_>>());
}

#[test]
fn failures_record_typed_receipts_and_statistics() {
    let (_dir, store, scope, _) = seeded(3);
    let calls = Mutex::new(0usize);
    let child = FakeChild::new(move |request| {
        let mut calls = calls.lock().unwrap();
        *calls += 1;
        match *calls {
            1 => Ok(ChildOutcome::completed("I think these are related.")),
            2 => Ok(ChildOutcome {
                exit: ChildExit::TimedOut,
                exit_code: None,
                ..ChildOutcome::completed("")
            }),
            3 => Ok(ChildOutcome {
                exit: ChildExit::Failed,
                exit_code: Some(1),
                stderr: "ErrorOutOfDeviceMemory".into(),
                ..ChildOutcome::completed("")
            }),
            4 => {
                assert!(request.force_cpu);
                Ok(ChildOutcome::completed(dream_answer(request, false)))
            }
            _ => Err("cannot start".into()),
        }
    });
    let service = DreamingService::new(
        store.clone(),
        config(settings()),
        scope.clone(),
        Some(child.clone()),
    );
    let run = || {
        assert!(service.enqueue(DreamTrigger::Manual, None));
        service.run_ready().unwrap()
    };
    let rejected = run();
    assert!(!rejected.success);
    assert_eq!(
        rejected.error,
        "model output rejected by the Dreaming protocol: invalid_json"
    );
    let receipt = rejected.receipt.unwrap();
    assert_eq!(receipt["failure_reason"], "protocol_rejection");
    assert_eq!(receipt["protocol_error"], "invalid_json");
    assert_eq!(
        receipt["model_output_hash"],
        stable_hash_hex("I think these are related.")
    );
    assert!(
        !receipt.to_string().contains("I think these"),
        "raw output never persisted"
    );
    assert_eq!(run().error, "isolated Dreaming model timed out");
    let cpu = run();
    assert!(cpu.success);
    assert_eq!(cpu.receipt.unwrap()["backend_fallback"], "cpu");
    assert_eq!(run().error, "isolated Dreaming model failed to start");
    let status = service.status();
    assert_eq!((status.completed_runs, status.failed_runs), (1, 3));
    assert_eq!(status.last_error, "isolated Dreaming model failed to start");
    let statistics = service.statistics().unwrap();
    assert_eq!(statistics.attempted, 4);
    assert_eq!(statistics.failed, 3);
    assert_eq!(statistics.protocol_rejections, 1);
    assert_eq!(statistics.timed_out, 1);
    assert_eq!(statistics.start_failures, 1);
    assert_eq!(statistics.cpu_fallbacks, 1);
    assert_eq!(statistics.failures_by_reason.get("timeout"), Some(&1));

    let (_dir2, store2, scope2, _) = seeded(1);
    let service = DreamingService::new(store2, config(settings()), scope2, Some(child));
    assert!(service.enqueue(DreamTrigger::Manual, None));
    assert_eq!(
        service.run_ready().unwrap().error,
        "Dreaming requires at least two governed memories"
    );
    let mut no_model = settings();
    no_model.model = None;
    let service = DreamingService::new(
        store,
        config(no_model),
        scope,
        Some(FakeChild::new(|_| Err("x".into()))),
    );
    assert!(service.enqueue(DreamTrigger::Manual, None));
    assert_eq!(
        service.run_ready().unwrap().error,
        "no Dreaming model is available"
    );
}

const SOURCE_TEXT: &str =
    "Exponential backoff with jitter is the recommended retry strategy for transient failures.";

fn research_events() -> Vec<ChildEvent> {
    vec![
        ChildEvent::ToolStart {
            tool_name: "web_search".into(),
        },
        ChildEvent::ToolEnd {
            tool_name: "web_search".into(),
            is_error: false,
            details: Some(
                json!({"results": [{"title": "Retry", "url": "https://docs.example/retry"}]}),
            ),
        },
        ChildEvent::ToolStart {
            tool_name: "web_fetch".into(),
        },
        ChildEvent::ToolEnd {
            tool_name: "web_fetch".into(),
            is_error: false,
            details: Some(
                json!({"url": "https://docs.example/retry", "status": 200, "content": SOURCE_TEXT}),
            ),
        },
    ]
}

fn research_child() -> Arc<FakeChild> {
    FakeChild::new(|request| {
        match request.task {
        ChildTask::Dreaming => Ok(ChildOutcome::completed(dream_answer(request, true))),
        ChildTask::DreamResearch => Ok(ChildOutcome {
            events: research_events(),
            ..ChildOutcome::completed(
                json!({"version": 1, "summary": "Backoff with jitter is recommended for transient failures.",
                       "source_urls": ["https://docs.example/retry"], "confidence": 0.7})
                .to_string(),
            )
        }),
        ChildTask::DreamVerification => {
            let input: Value = serde_json::from_str(&request.input).unwrap();
            Ok(ChildOutcome::completed(
                json!({
                    "version": 1, "verdict": "supported",
                    "summary_hash": input["summary_hash"], "evidence_hash": input["evidence_hash"],
                    "rationale": "Directly stated.",
                    "citations": [{"url": "https://docs.example/retry", "quote": "recommended retry strategy for transient failures"}]
                })
                .to_string(),
            ))
        }
        ChildTask::SemanticConsolidation => Err("unexpected".into()),
    }
    })
}

#[test]
fn required_research_with_auto_validate_validates_only_the_successor() {
    let (_dir, store, scope, _) = seeded(3);
    let child = research_child();
    let mut settings = settings();
    settings.research_policy = "required".into();
    settings.promotion_policy = "auto_validate".into();
    let service = DreamingService::new(
        store.clone(),
        config(settings),
        scope.clone(),
        Some(child.clone()),
    );
    assert!(service.enqueue(DreamTrigger::Manual, None));
    let report = service.run_ready().unwrap();
    assert!(report.success, "{report:?}");
    let receipt = report.receipt.unwrap();
    assert_eq!(receipt["research_attempted"], true);
    assert_eq!(receipt["research_completed"], true, "{receipt}");
    assert_eq!(
        receipt["research_tools"],
        json!(["web_run", "web_search", "web_fetch"])
    );
    assert_eq!(receipt["auto_validation"]["validated"], true, "{receipt}");
    let tasks: Vec<ChildTask> = child
        .requests()
        .iter()
        .map(|request| request.task)
        .collect();
    assert_eq!(
        tasks,
        [
            ChildTask::Dreaming,
            ChildTask::DreamResearch,
            ChildTask::DreamVerification
        ]
    );
    let research_request = &child.requests()[1];
    assert!(
        research_request
            .cli_arguments()
            .contains(&"web_run,web_search,web_fetch".to_string())
    );
    let research_input: Value = serde_json::from_str(&research_request.input).unwrap();
    assert_eq!(research_input["dream_id"], report.dream_id);
    assert_eq!(
        research_input["source_memories"].as_array().unwrap().len(),
        2
    );

    let research = store
        .get(&scope, receipt["research_id"].as_str().unwrap())
        .unwrap();
    assert_eq!(research.status, MemoryStatus::Validated);
    assert_eq!(research.provenance.source_uri, "https://docs.example/retry");
    assert!(research.tags.iter().any(|tag| tag == "research:web"));
    let dream = store.get(&scope, &report.dream_id).unwrap();
    assert_eq!(dream.status, MemoryStatus::DreamArtifact);
    assert!(dream.tags.iter().any(|tag| tag == "research:pending"));
    assert_eq!(
        store.review(&scope, &report.review_id).unwrap().status,
        ReviewStatus::Pending
    );
}

#[test]
fn research_rejections_keep_the_dream_and_record_reasons() {
    let (_dir, store, scope, _) = seeded(3);
    let child = FakeChild::new(|request| {
        match request.task {
        ChildTask::Dreaming => Ok(ChildOutcome::completed(dream_answer(request, false))),
        _ => Ok(ChildOutcome::completed(
            json!({"version": 1, "summary": "s", "source_urls": ["https://made.up/x"], "confidence": 0.5}).to_string(),
        )),
    }
    });
    let mut settings = settings();
    settings.research_policy = "required".into();
    let service = DreamingService::new(store.clone(), config(settings), scope.clone(), Some(child));
    assert!(service.enqueue(DreamTrigger::Manual, None));
    let report = service.run_ready().unwrap();
    assert!(report.success);
    let receipt = report.receipt.unwrap();
    assert_eq!(
        receipt["research_failure_reason"],
        "protocol_rejection:web_tool_not_used"
    );
    assert_eq!(receipt["research_completed"], false);
    assert_eq!(receipt["research_id"], "");
}

#[test]
fn manual_evidence_creates_successor_without_auto_validation() {
    let (_dir, store, scope, ids) = seeded(3);
    let child = research_child();
    let mut settings = settings();
    settings.research_policy = "manual".into();
    settings.promotion_policy = "auto_validate".into();
    let service = DreamingService::new(
        store.clone(),
        config(settings),
        scope.clone(),
        Some(child.clone()),
    );
    let evidence = DreamResearchEvidence {
        summary: "A maintainer confirmed the pattern.".into(),
        source_memory_ids: vec![ids[0].clone()],
        ..Default::default()
    };
    assert!(service.enqueue(DreamTrigger::Manual, Some(evidence)));
    let report = service.run_ready().unwrap();
    let receipt = report.receipt.unwrap();
    assert_eq!(receipt["research_completed"], true);
    assert_eq!(
        receipt["auto_validation"]["reason"],
        "manual_evidence_has_no_fetched_source_content"
    );
    assert_eq!(child.requests().len(), 1, "no research or verifier child");
    let research = store
        .get(&scope, receipt["research_id"].as_str().unwrap())
        .unwrap();
    assert_eq!(research.status, MemoryStatus::Candidate);
}

#[test]
fn commands_and_rpc() {
    let (_dir, store, scope, _) = seeded(3);
    let child = FakeChild::new(|request| Ok(ChildOutcome::completed(dream_answer(request, false))));
    let mut settings = settings();
    settings.research_policy = "manual".into();
    let service = DreamingService::new(
        store.clone(),
        config(settings.clone()),
        scope.clone(),
        Some(child.clone()),
    );
    let status = dream_command(&service, "");
    assert_eq!(status[0], "Memory Dreaming:");
    assert_eq!(status[1], "  enabled: true");
    assert_eq!(status[2], "  trigger: post_turn");
    assert_eq!(status[3], "  idle duration: 300s");
    assert_eq!(status[6], "  state: idle");
    assert_eq!(status[10], "  last dream: none");
    assert_eq!(dream_command(&service, "STATUS"), status);
    assert_eq!(
        dream_command(&service, "run"),
        ["Memory Dreaming cycle queued in the background."]
    );
    let report = service.run_ready().unwrap();
    assert!(report.success);
    assert_eq!(
        dream_command(&service, "bogus"),
        ["usage: /memory dream [status|run|research <json>]"]
    );

    let lines = dream_command(
        &service,
        &format!(
            "research {}",
            json!({"dream_id": report.dream_id, "summary": "Docs agree.", "source_uri": "https://docs"})
        ),
    );
    assert!(
        lines[0].starts_with("Dream research successor stored for review: mem"),
        "{lines:?}"
    );
    assert!(lines[1].starts_with("Research review: review_"));
    let bad = dream_command(&service, "research {\"dream_id\":1}");
    assert_eq!(
        bad[0],
        "Unable to store dream research: research JSON requires string dream_id and summary"
    );
    assert_eq!(bad[1], RESEARCH_USAGE_FOR_TEST);

    let rpc_status = dream_rpc(&service, &json!({})).unwrap();
    assert_eq!(rpc_status["completed"], 1);
    assert_eq!(rpc_status["last_dream_id"], report.dream_id);
    assert_eq!(
        dream_rpc(&service, &json!({"operation": "run"})).unwrap(),
        json!({"queued": true})
    );
    let stored = dream_rpc(
        &service,
        &json!({"operation": "research", "dream_id": report.dream_id, "summary": "More docs.", "source_hash": "h"}),
    )
    .unwrap();
    assert_eq!(stored["dream_id"], report.dream_id);
    assert!(stored["research_id"].is_string());
    let research = store
        .get(&scope, stored["research_id"].as_str().unwrap())
        .unwrap();
    assert_eq!(research.provenance.created_by, "user:rpc");
    assert_eq!(
        dream_rpc(&service, &json!({"operation": "x"}))
            .unwrap_err()
            .to_string(),
        "memory dream operation must be status, run, or research"
    );

    settings.research_policy = "disabled".into();
    settings.enabled = false;
    let disabled = DreamingService::new(store, config(settings), scope, Some(child));
    assert_eq!(
        dream_rpc(&disabled, &json!({"operation": "run"}))
            .unwrap_err()
            .to_string(),
        "Dreaming cycle was not queued; enable Dreaming and verify its queue/model settings"
    );
    assert_eq!(
        dream_rpc(&disabled, &json!({"operation": "research", "dream_id": report.dream_id, "summary": "s", "source_uri": "u"}))
            .unwrap_err()
            .to_string(),
        "Dreaming research successors are disabled by policy"
    );
}

const RESEARCH_USAGE_FOR_TEST: &str = "usage: /memory dream research {\"dream_id\":\"...\",\"summary\":\"...\",\"source_uri\":\"...\"}";

#[test]
fn background_worker_runs_manual_cycles_and_stops() {
    let (_dir, store, scope, _) = seeded(3);
    let child = FakeChild::new(|request| Ok(ChildOutcome::completed(dream_answer(request, false))));
    let service = DreamingService::new(store, config(settings()), scope, Some(child));
    let worker = service.start_worker();
    assert!(service.enqueue(DreamTrigger::Manual, None));
    let deadline = Instant::now() + Duration::from_secs(20);
    while service.status().completed_runs == 0 {
        assert!(Instant::now() < deadline, "worker did not run the cycle");
        std::thread::sleep(Duration::from_millis(10));
    }
    service.set_foreground_active(true);
    assert!(service.note_capture(1, 1));
    std::thread::sleep(Duration::from_millis(50));
    assert_eq!(
        service.status().pending_runs,
        1,
        "post_turn waits while foreground is active"
    );
    assert!(service.status().waiting_for_idle);
    worker.stop();
    assert_eq!(service.status().pending_runs, 0);
    assert!(
        !service.enqueue(DreamTrigger::Manual, None),
        "stopped service refuses work"
    );
}
