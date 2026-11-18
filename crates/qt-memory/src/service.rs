//! Write policy (`MemoryService`) and lifecycle governance
//! (`GovernanceService`), archived `memory_service.cpp` and
//! `governance_service.cpp`.

use std::sync::Arc;

use crate::error::{Result, invalid};
use crate::model::*;
use crate::scope::{scope_key, validate_scope};
use crate::store::MemoryStore;
use crate::text::{infer_type, normalize_content, redact_secrets};

fn clamp_score(value: f64) -> f64 {
    value.clamp(0.0, 1.0)
}

fn append_unique(values: &mut Vec<String>, value: String) {
    if !value.is_empty() && !values.contains(&value) {
        values.push(value);
    }
}

/// Applies the archived `remember` policy to a write: normalises and redacts
/// content, infers a type for observations, keeps agent writes at
/// hypothesis, derives evidence strength and adds `type:`/`source:` tags.
pub fn prepare_remember(scope: &MemoryScope, mut write: MemoryWrite) -> Result<MemoryWrite> {
    validate_scope(scope)?;
    write.content = normalize_content(&write.content);
    if write.content.is_empty() {
        return invalid("memory content is required");
    }
    if redact_secrets(&mut write.content) {
        append_unique(&mut write.tags, "security:redacted".into());
    }
    if write.memory_type == MemoryType::Observation {
        write.memory_type = infer_type(&write.content, write.memory_type);
    }
    if write.status == MemoryStatus::Active
        && write.provenance.created_by != "user"
        && write.provenance.created_by != "import"
    {
        write.status = MemoryStatus::Hypothesis;
    }
    if write.provenance.evidence_strength <= 0.0 {
        if !write.provenance.source_hash.is_empty() {
            write.provenance.evidence_strength = 0.8;
        } else if !write.provenance.source_uri.is_empty() {
            write.provenance.evidence_strength = 0.6;
        } else if !write.source.is_empty() {
            write.provenance.evidence_strength = 0.4;
        }
    }
    write.provenance.evidence_strength = clamp_score(write.provenance.evidence_strength);
    write.confidence = clamp_score(write.confidence);
    write.quality_score = clamp_score(write.quality_score);
    append_unique(
        &mut write.tags,
        format!("type:{}", write.memory_type.as_str()),
    );
    append_unique(
        &mut write.tags,
        format!("source:{}", write.provenance.source_type.as_str()),
    );
    Ok(write)
}

fn prepare_patch(mut patch: MemoryPatch) -> Result<MemoryPatch> {
    if let Some(content) = &mut patch.content {
        *content = normalize_content(content);
        if content.is_empty() {
            return invalid("memory content is required");
        }
        redact_secrets(content);
    }
    patch.confidence = patch.confidence.map(clamp_score);
    patch.quality_score = patch.quality_score.map(clamp_score);
    Ok(patch)
}

/// Policy layer over the store for writes from agents, tools and users.
#[derive(Debug, Clone)]
pub struct MemoryService {
    store: Arc<MemoryStore>,
}

impl MemoryService {
    pub fn new(store: Arc<MemoryStore>) -> Self {
        Self { store }
    }

    pub fn store(&self) -> &Arc<MemoryStore> {
        &self.store
    }

    pub fn remember(&self, scope: &MemoryScope, write: MemoryWrite) -> Result<String> {
        let write = prepare_remember(scope, write)?;
        self.store.add(scope, write)
    }

    pub fn query(&self, scope: &MemoryScope, query: &MemoryQuery) -> Result<Vec<MemoryEntry>> {
        validate_scope(scope)?;
        self.store.query(scope, query)
    }

    pub fn get(&self, scope: &MemoryScope, id: &str) -> Result<MemoryEntry> {
        validate_scope(scope)?;
        if id.is_empty() {
            return invalid("memory id is required");
        }
        self.store.get(scope, id)
    }

    pub fn update(&self, scope: &MemoryScope, id: &str, patch: MemoryPatch) -> Result<MemoryEntry> {
        validate_scope(scope)?;
        let patch = prepare_patch(patch)?;
        self.store.update(scope, id, &patch)
    }

    pub fn apply_governed_update(
        &self,
        scope: &MemoryScope,
        id: &str,
        expected_status: MemoryStatus,
        patch: MemoryPatch,
        edge: Option<GraphEdge>,
        audit: AuditEventWrite,
    ) -> Result<MemoryEntry> {
        validate_scope(scope)?;
        if id.is_empty() {
            return invalid("memory id is required");
        }
        let patch = prepare_patch(patch)?;
        self.store
            .apply_governed_update(scope, id, expected_status, &patch, edge, audit)
    }

    pub fn forget(&self, scope: &MemoryScope, id: &str) -> Result<bool> {
        validate_scope(scope)?;
        self.store.forget(scope, id)
    }

    pub fn revisions(&self, scope: &MemoryScope, id: &str) -> Result<Vec<MemoryRevision>> {
        validate_scope(scope)?;
        self.store.revisions(scope, id)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct LifecyclePolicy {
    pub require_evidence_for_validation: bool,
    pub allow_expiration: bool,
}

impl Default for LifecyclePolicy {
    fn default() -> Self {
        Self {
            require_evidence_for_validation: true,
            allow_expiration: true,
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct LifecycleRequest {
    pub id: String,
    pub status: MemoryStatus,
    pub replacement_id: String,
    pub confidence: Option<f64>,
    pub has_evidence: bool,
    pub actor: String,
    pub reason: String,
    pub evidence_uri: String,
}

impl LifecycleRequest {
    pub fn new(
        id: impl Into<String>,
        status: MemoryStatus,
        actor: impl Into<String>,
        reason: impl Into<String>,
    ) -> Self {
        Self {
            id: id.into(),
            status,
            replacement_id: String::new(),
            confidence: None,
            has_evidence: false,
            actor: actor.into(),
            reason: reason.into(),
            evidence_uri: String::new(),
        }
    }
}

/// Audited lifecycle transitions in the subject's exact scope.
#[derive(Debug, Clone)]
pub struct GovernanceService {
    memories: MemoryService,
    policy: LifecyclePolicy,
}

impl GovernanceService {
    pub fn new(memories: MemoryService, policy: LifecyclePolicy) -> Self {
        Self { memories, policy }
    }

    /// The archived lifecycle state machine.
    pub fn can_transition(
        from: MemoryStatus,
        to: MemoryStatus,
        policy: &LifecyclePolicy,
        has_evidence: bool,
        has_replacement: bool,
    ) -> bool {
        use MemoryStatus as S;
        if from == to {
            return true;
        }
        if from == S::Rejected {
            return false;
        }
        if from == S::Expired && to != S::Rejected {
            return false;
        }
        match to {
            S::Rejected => true,
            S::Expired => policy.allow_expiration,
            S::Superseded => has_replacement,
            S::Deprecated => matches!(
                from,
                S::Candidate | S::Hypothesis | S::Active | S::Validated
            ),
            S::Active => matches!(from, S::Candidate | S::Hypothesis),
            S::Validated => {
                (!policy.require_evidence_for_validation || has_evidence)
                    && matches!(from, S::Candidate | S::Hypothesis | S::Active)
            }
            S::Hypothesis => matches!(from, S::Candidate | S::DreamArtifact),
            S::Candidate | S::DreamArtifact => false,
        }
    }

    /// Only active and validated memories are trusted for balanced recall.
    pub fn trusted_for_recall(status: MemoryStatus) -> bool {
        matches!(status, MemoryStatus::Active | MemoryStatus::Validated)
    }

    pub fn transition(
        &self,
        scope: &MemoryScope,
        request: &LifecycleRequest,
    ) -> Result<MemoryEntry> {
        if request.actor.is_empty() || request.reason.is_empty() {
            return invalid("governance transition requires actor and reason");
        }
        let before = self.memories.get(scope, &request.id)?;
        if scope_key(&before.scope)? != scope_key(scope)? {
            return invalid("governance transition subject is outside the exact authority scope");
        }
        let replacement = !request.replacement_id.is_empty();
        let has_evidence = request.has_evidence && !request.evidence_uri.is_empty();
        if !Self::can_transition(
            before.status,
            request.status,
            &self.policy,
            has_evidence,
            replacement,
        ) {
            return invalid("invalid memory lifecycle transition");
        }
        if replacement {
            let target = self.memories.get(scope, &request.replacement_id)?;
            if scope_key(&target.scope)? != scope_key(scope)? {
                return invalid("governance replacement is outside the exact authority scope");
            }
            if target.id == before.id {
                return invalid("memory cannot replace itself");
            }
        }
        let patch = MemoryPatch {
            status: Some(request.status),
            replacement_id: replacement.then(|| request.replacement_id.clone()),
            confidence: request.confidence,
            ..Default::default()
        };
        let audit = AuditEventWrite {
            actor: request.actor.clone(),
            action: format!("memory.lifecycle.{}", request.status.as_str()),
            reason: request.reason.clone(),
            before_id: before.id.clone(),
            after_id: if replacement {
                request.replacement_id.clone()
            } else {
                request.id.clone()
            },
            evidence_uri: request.evidence_uri.clone(),
            metadata_json: String::new(),
        };
        let edge = replacement.then(|| GraphEdge {
            source: "governance".into(),
            ..GraphEdge::new(
                before.id.clone(),
                request.replacement_id.clone(),
                "superseded_by",
            )
        });
        self.memories
            .apply_governed_update(scope, &request.id, before.status, patch, edge, audit)
    }

    pub fn validate(
        &self,
        scope: &MemoryScope,
        id: &str,
        actor: &str,
        reason: &str,
        evidence_uri: &str,
    ) -> Result<MemoryEntry> {
        let mut request = LifecycleRequest::new(id, MemoryStatus::Validated, actor, reason);
        request.evidence_uri = evidence_uri.into();
        request.has_evidence = !evidence_uri.is_empty();
        self.transition(scope, &request)
    }

    pub fn supersede(
        &self,
        scope: &MemoryScope,
        id: &str,
        replacement_id: &str,
        actor: &str,
        reason: &str,
    ) -> Result<MemoryEntry> {
        let mut request = LifecycleRequest::new(id, MemoryStatus::Superseded, actor, reason);
        request.replacement_id = replacement_id.into();
        self.transition(scope, &request)
    }
}
