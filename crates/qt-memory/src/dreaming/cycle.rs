//! One Dreaming cycle (archived `MemoryRuntime::run_dream_cycle`).
//!
//! Sampling: up to 64 governed records are read; those used least by the
//! last eight Dreaming receipts come first, ties broken by a hash of the
//! cycle's random UUID, and the kept candidates are re-ordered by a second
//! hash so the prompt order is randomised too. The receipt records the
//! algorithm, seed, pool hash and exact input IDs for audit and replay.
//!
//! Persistence: a per-cycle claim event (idempotency key over the protocol
//! version, model, source pack and cycle ID), then a pending-review
//! `dream_artifact` with `derived_from` edges, an optional researched
//! successor, and a receipt event. Failures write a receipt with a typed
//! reason, an output hash and length, and never the rejected output itself.

use std::collections::{BTreeMap, BTreeSet};
use std::path::PathBuf;

use serde_json::{Value, json};

use super::child::{
    CancelFlag, ChildExit, ChildRequest, ChildTask, IsolatedChild, RESEARCH_CHILD_TOOLS,
    run_with_cpu_fallback,
};
use super::protocol::{
    DREAMING_PROMPT, DREAMING_PROTOCOL_VERSION, DREAMING_SELECTION_ALGORITHM, ValidatedDream,
    validate_dream_output,
};
use super::{DreamingConfig, PendingDream, PromotionPolicy, ResearchPolicy};
use crate::error::Result;
use crate::model::*;
use crate::post_processing::{
    DreamArtifactRequest, DreamResearchEvidence, PostProcessingService, governed_statuses,
};
use crate::research::{DREAM_RESEARCH_PROMPT, ValidatedResearch, validate_research_output};
use crate::review::{ReviewGovernancePolicy, ReviewService};
use crate::service::MemoryService;
use crate::store::MemoryStore;
use crate::text::{dump, stable_hash_hex};
use crate::verifier::{AutoValidator, VerificationSubject};

/// What one cycle did.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct DreamCycleReport {
    pub success: bool,
    pub error: String,
    pub dream_id: String,
    pub review_id: String,
    /// The persisted receipt, when a claim was taken.
    pub receipt: Option<Value>,
}

impl DreamCycleReport {
    fn failed(error: impl Into<String>) -> Self {
        Self {
            error: error.into(),
            ..Default::default()
        }
    }
}

pub(crate) struct CycleContext<'a> {
    pub store: &'a std::sync::Arc<MemoryStore>,
    pub config: &'a DreamingConfig,
    pub child: &'a dyn IsolatedChild,
    pub child_handle: std::sync::Arc<dyn IsolatedChild>,
    pub cancel: &'a CancelFlag,
}

struct ActiveClaim {
    claim_key: String,
    model: String,
    source_pack_hash: String,
}

fn truncated(content: &str, limit: usize) -> String {
    let mut end = content.len().min(limit);
    while !content.is_char_boundary(end) {
        end -= 1;
    }
    content[..end].to_string()
}

/// Runs `dream` to completion; never panics on child or store failures.
pub(crate) fn run_dream_cycle(ctx: &CycleContext<'_>, dream: &PendingDream) -> DreamCycleReport {
    let mut claim = None;
    match cycle(ctx, dream, &mut claim) {
        Ok(report) => report,
        Err(error) => {
            if let Some(claim) = claim {
                let receipt = json!({
                    "claim_key": claim.claim_key,
                    "cycle_id": dream.cycle_id,
                    "trigger": dream.trigger.as_str(),
                    "model": claim.model,
                    "source_pack_hash": claim.source_pack_hash,
                    "isolated_agent": true,
                    "review_created": false,
                    "error": error.message(),
                });
                let _ = ctx.store.record_session_event_once(
                    &dream.scope,
                    SessionEventWrite {
                        event_type: "memory_dream_receipt".into(),
                        source: "qt:memory_dreaming".into(),
                        payload_json: dump(&receipt),
                        deduplication_key: claim.claim_key,
                    },
                );
            }
            DreamCycleReport::failed(error.message())
        }
    }
}

fn child_request(
    ctx: &CycleContext<'_>,
    task: ChildTask,
    model: &str,
    prompt: &str,
    input: String,
) -> ChildRequest {
    let mut request = ChildRequest::new(task, model, prompt);
    request.input = input;
    request.timeout = ctx.config.timeout();
    request.discovery_roots = ctx.config.discovery_roots.clone();
    request.agent_home = ctx.config.agent_home.clone();
    if !ctx.config.cwd.is_empty() {
        request.cwd = Some(PathBuf::from(&ctx.config.cwd));
    }
    request.cancel = ctx.cancel.clone();
    request
}

fn cycle(
    ctx: &CycleContext<'_>,
    dream: &PendingDream,
    active: &mut Option<ActiveClaim>,
) -> Result<DreamCycleReport> {
    let Some(model) = ctx.config.resolved_model() else {
        return Ok(DreamCycleReport::failed("no Dreaming model is available"));
    };
    let memories = MemoryService::new(ctx.store.clone());
    let candidate_limit = ctx.config.candidate_limit();
    let query = MemoryQuery {
        limit: (candidate_limit * 4).clamp(32, 64),
        policy: RecallPolicy::Strict,
        statuses: governed_statuses(),
        ..Default::default()
    };
    let mut eligible = memories.query(&dream.scope, &query)?;
    if eligible.len() < 2 {
        return Ok(DreamCycleReport::failed(
            "Dreaming requires at least two governed memories",
        ));
    }
    let pool_ids: Vec<String> = eligible.iter().map(|memory| memory.id.clone()).collect();
    let candidate_pool_hash = stable_hash_hex(&dump(&json!(pool_ids)));

    let mut recent_use: BTreeMap<String, usize> = BTreeMap::new();
    for event in ctx
        .store
        .session_events(&dream.scope, "memory_dream_receipt", 8, 0)?
    {
        let Ok(payload) = serde_json::from_str::<Value>(&event.payload_json) else {
            continue;
        };
        for id in payload
            .get("input_candidate_ids")
            .and_then(Value::as_array)
            .into_iter()
            .flatten()
            .filter_map(Value::as_str)
        {
            *recent_use.entry(id.to_string()).or_default() += 1;
        }
    }
    let selection_key =
        |phase: &str, id: &str| stable_hash_hex(&format!("{}:{phase}:{id}", dream.cycle_id));
    let recent = |id: &str| recent_use.get(id).copied().unwrap_or(0);
    eligible.sort_by(|left, right| {
        recent(&left.id)
            .cmp(&recent(&right.id))
            .then_with(|| {
                selection_key("select", &left.id).cmp(&selection_key("select", &right.id))
            })
            .then_with(|| left.id.cmp(&right.id))
    });
    eligible.truncate(candidate_limit);
    eligible.sort_by(|left, right| {
        selection_key("order", &left.id)
            .cmp(&selection_key("order", &right.id))
            .then_with(|| left.id.cmp(&right.id))
    });

    let maximum_chars = (ctx.config.settings.max_model_input_tokens as usize * 4).max(1024);
    let per_record_chars = (maximum_chars / eligible.len().max(2)).max(256);
    let mut candidates: Vec<Value> = Vec::new();
    let mut input_candidate_ids: Vec<String> = Vec::new();
    let mut governed_ids: BTreeSet<String> = BTreeSet::new();
    for memory in &eligible {
        let candidate = json!({
            "id": memory.id,
            "type": memory.memory_type.as_str(),
            "status": memory.status.as_str(),
            "confidence": memory.confidence,
            "quality_score": memory.quality_score,
            "tags": memory.tags,
            "content": truncated(&memory.content, per_record_chars),
        });
        let mut proposed = candidates.clone();
        proposed.push(candidate.clone());
        if !candidates.is_empty() && dump(&Value::Array(proposed)).len() > maximum_chars {
            break;
        }
        candidates.push(candidate);
        input_candidate_ids.push(memory.id.clone());
        governed_ids.insert(memory.id.clone());
    }
    if candidates.len() < 2 {
        return Ok(DreamCycleReport::failed(
            "Dreaming input budget admitted fewer than two memories",
        ));
    }

    let input = json!({
        "version": 1,
        "trigger": dream.trigger.as_str(),
        "scope": {
            "project": dream.scope.project,
            "repository": dream.scope.repository,
            "branch": dream.scope.branch,
        },
        "candidates": candidates,
    });
    let input_text = dump(&input);
    let source_pack_hash = stable_hash_hex(&input_text);
    let claim_key = stable_hash_hex(&format!(
        "qt-memory-dream:{DREAMING_PROTOCOL_VERSION}:{model}:{source_pack_hash}:{}",
        dream.cycle_id
    ));
    let identity = json!({
        "claim_key": claim_key,
        "cycle_id": dream.cycle_id,
        "trigger": dream.trigger.as_str(),
        "model": model,
        "protocol_version": DREAMING_PROTOCOL_VERSION,
        "selection_algorithm": DREAMING_SELECTION_ALGORITHM,
        "selection_seed": dream.cycle_id,
        "candidate_pool_hash": candidate_pool_hash,
        "candidate_pool_count": pool_ids.len(),
        "input_candidate_ids": input_candidate_ids,
        "source_pack_hash": source_pack_hash,
    });
    let mut claim_payload = identity.clone();
    claim_payload["candidate_count"] = json!(candidates.len());
    let claim = ctx.store.record_session_event_once(
        &dream.scope,
        SessionEventWrite {
            event_type: "memory_dream_claim".into(),
            source: "qt:memory_dreaming".into(),
            payload_json: dump(&claim_payload),
            deduplication_key: claim_key.clone(),
        },
    )?;
    if !claim.inserted {
        return Ok(DreamCycleReport {
            success: true,
            ..Default::default()
        });
    }
    *active = Some(ActiveClaim {
        claim_key: claim_key.clone(),
        model: model.clone(),
        source_pack_hash: source_pack_hash.clone(),
    });

    let mut receipt = identity;
    for (key, value) in [
        ("bounded_source_ids", json!([])),
        ("isolated_agent", json!(true)),
        ("review_created", json!(false)),
        ("dream_id", json!("")),
        ("review_id", json!("")),
        ("research_id", json!("")),
        ("research_review_id", json!("")),
        ("research_attempted", json!(false)),
        ("research_completed", json!(false)),
        ("research_tools", json!([])),
        ("research_failure_reason", json!("")),
        ("research_output_hash", json!("")),
        ("backend_fallback", json!("none")),
        ("failure_reason", json!("")),
        ("protocol_error", json!("")),
        ("model_output_hash", json!("")),
        ("model_output_chars", json!(0)),
        ("child_exit_code", Value::Null),
        ("child_stderr_hash", json!("")),
        ("child_stderr_chars", json!(0)),
        ("error", json!("")),
    ] {
        receipt[key] = value;
    }
    let auto_validate = ctx.config.promotion_policy() == PromotionPolicy::AutoValidate;
    if auto_validate {
        receipt["auto_validation"] = json!({
            "attempted": false, "validated": false, "reason": "verified_web_research_required"
        });
    }

    let request = child_request(
        ctx,
        ChildTask::Dreaming,
        &model,
        DREAMING_PROMPT,
        input_text,
    );
    let (result, cpu_fallback) = run_with_cpu_fallback(ctx.child, &request);
    receipt["backend_fallback"] = json!(if cpu_fallback { "cpu" } else { "none" });
    if let Ok(outcome) = &result {
        receipt["child_exit_code"] = json!(outcome.exit_code);
        receipt["child_stderr_chars"] = json!(outcome.stderr.len());
        if !outcome.stderr.is_empty() {
            receipt["child_stderr_hash"] = json!(stable_hash_hex(&outcome.stderr));
        }
    }

    let mut report = DreamCycleReport::default();
    match result {
        Ok(outcome) if outcome.is_completed() => {
            receipt["model_output_hash"] = json!(stable_hash_hex(&outcome.final_output));
            receipt["model_output_chars"] = json!(outcome.final_output.len());
            match validate_dream_output(&outcome.final_output, &governed_ids) {
                Ok(output) => {
                    persist(
                        ctx,
                        dream,
                        &model,
                        &claim_key,
                        &source_pack_hash,
                        &candidates,
                        &output,
                        &mut receipt,
                        &mut report,
                    )?;
                    report.success = true;
                }
                Err(protocol_error) => {
                    receipt["failure_reason"] = json!("protocol_rejection");
                    receipt["error"] = json!(format!(
                        "model output rejected by the Dreaming protocol: {protocol_error}"
                    ));
                    receipt["protocol_error"] = json!(protocol_error);
                }
            }
        }
        Ok(outcome) => {
            let (reason, error) = match outcome.exit {
                ChildExit::TimedOut => ("timeout", "isolated Dreaming model timed out".to_string()),
                ChildExit::Cancelled => {
                    ("cancelled", "isolated Dreaming model was cancelled".into())
                }
                _ => (
                    "model_failure",
                    format!(
                        "isolated Dreaming model failed (exit code {})",
                        outcome.exit_code.unwrap_or(-1)
                    ),
                ),
            };
            receipt["failure_reason"] = json!(reason);
            receipt["error"] = json!(error);
        }
        Err(_) => {
            receipt["failure_reason"] = json!("start_failure");
            receipt["error"] = json!("isolated Dreaming model failed to start");
        }
    }
    ctx.store.record_session_event_once(
        &dream.scope,
        SessionEventWrite {
            event_type: "memory_dream_receipt".into(),
            source: "qt:memory_dreaming".into(),
            payload_json: dump(&receipt),
            deduplication_key: claim_key,
        },
    )?;
    *active = None;
    report.error = receipt["error"].as_str().unwrap_or_default().to_string();
    report.receipt = Some(receipt);
    Ok(report)
}

#[allow(clippy::too_many_arguments)]
fn persist(
    ctx: &CycleContext<'_>,
    dream: &PendingDream,
    model: &str,
    claim_key: &str,
    source_pack_hash: &str,
    candidates: &[Value],
    output: &ValidatedDream,
    receipt: &mut Value,
    report: &mut DreamCycleReport,
) -> Result<()> {
    let post = PostProcessingService::new(ctx.store.clone());
    let research_policy = ctx.config.research_policy();
    let auto_validate = ctx.config.promotion_policy() == PromotionPolicy::AutoValidate;
    let mut tags = Vec::new();
    if output.research_required || research_policy == ResearchPolicy::Required {
        tags.push("research:pending".to_string());
    }
    let stored = post.remember_dream_artifact(
        &dream.scope,
        DreamArtifactRequest {
            cycle_id: claim_key.chars().take(16).collect(),
            artifact_kind: output.artifact_kind.clone(),
            content: output.content.clone(),
            source_memory_ids: output.source_memory_ids.clone(),
            model: model.into(),
            source_pack_hash: source_pack_hash.into(),
            confidence: output.confidence,
            tags,
        },
    )?;
    report.dream_id = stored.id.clone();
    report.review_id = stored.review_id.clone();
    receipt["dream_id"] = json!(stored.id);
    receipt["review_id"] = json!(stored.review_id);
    receipt["bounded_source_ids"] = json!(output.source_memory_ids);
    receipt["review_created"] = json!(true);

    if let Some(evidence) = dream
        .evidence
        .clone()
        .filter(|_| research_policy != ResearchPolicy::Disabled)
    {
        let researched = post.remember_researched_successor(&dream.scope, &stored.id, evidence)?;
        receipt["research_id"] = json!(researched.researched_memory_id.unwrap_or_default());
        receipt["research_review_id"] = json!(researched.research_review_id.unwrap_or_default());
        receipt["research_completed"] = json!(true);
        if auto_validate {
            receipt["auto_validation"] = json!({
                "attempted": false, "validated": false,
                "reason": "manual_evidence_has_no_fetched_source_content"
            });
        }
        return Ok(());
    }
    if research_policy != ResearchPolicy::Required {
        return Ok(());
    }

    receipt["research_attempted"] = json!(true);
    receipt["research_tools"] = json!(RESEARCH_CHILD_TOOLS);
    let sources: Vec<Value> = candidates
        .iter()
        .filter(|candidate| {
            candidate
                .get("id")
                .and_then(Value::as_str)
                .is_some_and(|id| output.source_memory_ids.iter().any(|source| source == id))
        })
        .cloned()
        .collect();
    let research_input = json!({
        "version": 1,
        "dream_id": stored.id,
        "dream": {
            "artifact_kind": output.artifact_kind,
            "content": output.content,
            "confidence": output.confidence,
        },
        "source_memories": sources,
    });
    let request = child_request(
        ctx,
        ChildTask::DreamResearch,
        model,
        DREAM_RESEARCH_PROMPT,
        dump(&research_input),
    );
    let (result, cpu_fallback) = run_with_cpu_fallback(ctx.child, &request);
    receipt["research_backend_fallback"] = json!(if cpu_fallback { "cpu" } else { "none" });
    let outcome = match result {
        Ok(outcome) if outcome.is_completed() => outcome,
        Ok(outcome) => {
            receipt["research_failure_reason"] = json!(match outcome.exit {
                ChildExit::TimedOut => "timeout".to_string(),
                ChildExit::Cancelled => "cancelled".to_string(),
                _ => format!("model_failure:{}", outcome.exit_code.unwrap_or(-1)),
            });
            return Ok(());
        }
        Err(_) => {
            receipt["research_failure_reason"] = json!("start_failure");
            return Ok(());
        }
    };
    receipt["research_output_hash"] = json!(stable_hash_hex(&outcome.final_output));
    let research: ValidatedResearch = match validate_research_output(&outcome) {
        Ok(research) => research,
        Err(error) => {
            receipt["research_failure_reason"] = json!(format!("protocol_rejection:{error}"));
            return Ok(());
        }
    };
    let evidence = DreamResearchEvidence {
        summary: research.summary.clone(),
        source_memory_ids: output.source_memory_ids.clone(),
        tags: vec!["research:web".into(), "research:isolated-agent".into()],
        confidence: research.confidence,
        provenance: Provenance {
            source_type: SourceType::ToolResult,
            source_uri: research.source_urls[0].clone(),
            source_hash: research.evidence_hash.clone(),
            source_span: dump(&json!({"citations": research.source_urls})),
            created_by: "memory_dream_research_parent".into(),
            evidence_strength: research.confidence,
            ..Default::default()
        },
    };
    let researched = post.remember_researched_successor(&dream.scope, &stored.id, evidence)?;
    receipt["research_id"] = json!(researched.researched_memory_id.clone().unwrap_or_default());
    receipt["research_review_id"] =
        json!(researched.research_review_id.clone().unwrap_or_default());
    receipt["research_completed"] = json!(true);
    if auto_validate
        && let (Some(memory_id), Some(review_id)) = (
            researched.researched_memory_id,
            researched.research_review_id,
        )
    {
        let validator = AutoValidator::new(
            ctx.store.clone(),
            ReviewService::new(
                ctx.store.clone(),
                ReviewGovernancePolicy {
                    allow_dream_hypothesis_promotion: true,
                },
            ),
            ctx.config.verifier_config(),
            ctx.child_handle.clone(),
            ctx.cancel.clone(),
        );
        receipt["auto_validation"] = validator.validate(&VerificationSubject {
            scope: &dream.scope,
            memory_id: &memory_id,
            review_id: &review_id,
            documents: &research.documents,
            evidence_hash: &research.evidence_hash,
            dream_model: model,
        });
    }
    Ok(())
}
