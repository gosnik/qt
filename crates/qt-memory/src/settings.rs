//! `memory.*` settings: parsing, defaults, validation, layering and the
//! project-may-not-broaden rule (archived `settings.cpp`).
//!
//! Global settings (`<agent_home>/settings.json`) are the policy; a project
//! settings file may only narrow it. Unknown keys are ignored.

use serde_json::{Map, Value, json};

use crate::error::{MemoryError, Result};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MemoryPrehandlerSettings {
    pub model: Option<String>,
    pub thinking: String,
    pub timeout_ms: i64,
    pub fail_open: bool,
    pub max_model_input_tokens: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MemoryCaptureSettings {
    pub post_process: String,
    pub default_status: String,
    pub max_evidence_chars: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MemoryDreamingSettings {
    pub enabled: bool,
    pub trigger: String,
    pub interval_seconds: u32,
    pub idle_seconds: u32,
    pub candidate_threshold: u32,
    pub model: Option<String>,
    pub verifier_model: Option<String>,
    pub timeout_ms: i64,
    pub max_model_input_tokens: u32,
    pub max_candidates: u32,
    pub research_policy: String,
    pub promotion_policy: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MemoryRetrievalSettings {
    pub graph_enabled: bool,
    pub dense_enabled: bool,
    pub code_enabled: bool,
    pub embedding_model: Option<String>,
    pub query_prefix: String,
    pub document_prefix: String,
    pub max_depth: u32,
    pub max_seeds: u32,
    pub max_edges: u32,
    pub max_graph_results: u32,
    pub max_fanout: u32,
    pub query_cache_entries: u32,
}

/// Resolved memory settings.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MemorySettings {
    pub enabled: bool,
    pub automatic_recall: bool,
    pub automatic_capture: bool,
    pub recall_policy: String,
    pub context_token_budget: u32,
    pub candidate_limit: u32,
    pub prehandler: MemoryPrehandlerSettings,
    pub capture: MemoryCaptureSettings,
    pub dreaming: MemoryDreamingSettings,
    pub retrieval: MemoryRetrievalSettings,
}

impl Default for MemorySettings {
    fn default() -> Self {
        MemorySettingsPatch::defaults().resolve()
    }
}

/// `Some(None)` on a nullable model means "explicitly unset".
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct MemoryPrehandlerSettingsPatch {
    pub model: Option<Option<String>>,
    pub thinking: Option<String>,
    pub timeout_ms: Option<i64>,
    pub fail_open: Option<bool>,
    pub max_model_input_tokens: Option<u32>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct MemoryCaptureSettingsPatch {
    pub post_process: Option<String>,
    pub default_status: Option<String>,
    pub max_evidence_chars: Option<u32>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct MemoryDreamingSettingsPatch {
    pub enabled: Option<bool>,
    pub trigger: Option<String>,
    pub interval_seconds: Option<u32>,
    pub idle_seconds: Option<u32>,
    pub candidate_threshold: Option<u32>,
    pub model: Option<Option<String>>,
    pub verifier_model: Option<Option<String>>,
    pub timeout_ms: Option<i64>,
    pub max_model_input_tokens: Option<u32>,
    pub max_candidates: Option<u32>,
    pub research_policy: Option<String>,
    pub promotion_policy: Option<String>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct MemoryRetrievalSettingsPatch {
    pub graph_enabled: Option<bool>,
    pub dense_enabled: Option<bool>,
    pub code_enabled: Option<bool>,
    pub embedding_model: Option<Option<String>>,
    pub query_prefix: Option<String>,
    pub document_prefix: Option<String>,
    pub max_depth: Option<u32>,
    pub max_seeds: Option<u32>,
    pub max_edges: Option<u32>,
    pub max_graph_results: Option<u32>,
    pub max_fanout: Option<u32>,
    pub query_cache_entries: Option<u32>,
}

/// One settings layer's `memory` object.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct MemorySettingsPatch {
    pub enabled: Option<bool>,
    pub automatic_recall: Option<bool>,
    pub automatic_capture: Option<bool>,
    pub recall_policy: Option<String>,
    pub context_token_budget: Option<u32>,
    pub candidate_limit: Option<u32>,
    pub prehandler: MemoryPrehandlerSettingsPatch,
    pub capture: MemoryCaptureSettingsPatch,
    pub dreaming: MemoryDreamingSettingsPatch,
    pub retrieval: MemoryRetrievalSettingsPatch,
}

fn fail<T>(message: String) -> Result<T> {
    Err(MemoryError::InvalidArgument(message))
}

struct Fields<'a> {
    object: &'a Value,
    prefix: String,
}

impl Fields<'_> {
    fn name(&self, key: &str) -> String {
        format!("{}.{key}", self.prefix)
    }

    fn bool(&self, key: &str) -> Result<Option<bool>> {
        match self.object.get(key) {
            None => Ok(None),
            Some(Value::Bool(value)) => Ok(Some(*value)),
            Some(_) => fail(format!("{} must be a boolean", self.name(key))),
        }
    }

    fn string(&self, key: &str) -> Result<Option<String>> {
        match self.object.get(key) {
            None => Ok(None),
            Some(Value::String(value)) => Ok(Some(value.clone())),
            Some(_) => fail(format!("{} must be a string", self.name(key))),
        }
    }

    fn nullable_string(&self, key: &str) -> Result<Option<Option<String>>> {
        match self.object.get(key) {
            None => Ok(None),
            Some(Value::Null) => Ok(Some(None)),
            Some(Value::String(value)) => Ok(Some(Some(value.clone()))),
            Some(_) => fail(format!("{} must be a string", self.name(key))),
        }
    }

    fn uint32(&self, key: &str) -> Result<Option<u32>> {
        let Some(value) = self.object.get(key) else {
            return Ok(None);
        };
        if !(value.is_i64() || value.is_u64()) {
            return fail(format!("{} must be an integer", self.name(key)));
        }
        match value.as_u64().and_then(|value| u32::try_from(value).ok()) {
            Some(value) => Ok(Some(value)),
            None => fail(format!("{} is out of range", self.name(key))),
        }
    }

    fn int64(&self, key: &str) -> Result<Option<i64>> {
        let Some(value) = self.object.get(key) else {
            return Ok(None);
        };
        if !(value.is_i64() || value.is_u64()) {
            return fail(format!("{} must be an integer", self.name(key)));
        }
        match value.as_i64() {
            Some(value) => Ok(Some(value)),
            None => fail(format!("{} is out of range", self.name(key))),
        }
    }

    fn object(&self, key: &str) -> Result<Option<Fields<'_>>> {
        match self.object.get(key) {
            None => Ok(None),
            Some(value @ Value::Object(_)) => Ok(Some(Fields {
                object: value,
                prefix: self.name(key),
            })),
            Some(_) => fail(format!("{} must be an object", self.name(key))),
        }
    }
}

fn overlay<T: Clone>(base: &mut Option<T>, value: &Option<T>) {
    if value.is_some() {
        base.clone_from(value);
    }
}

fn validate_choice(value: &Option<String>, field: &str, allowed: &[&str]) -> Result<()> {
    match value {
        Some(value) if !value.is_empty() && !allowed.contains(&value.as_str()) => fail(format!(
            "resolved settings {field} has unsupported value: {value}"
        )),
        _ => Ok(()),
    }
}

impl MemorySettingsPatch {
    /// Parses a `memory` settings object (`field_name` prefixes errors).
    pub fn parse(json: &Value, field_name: &str) -> Result<Self> {
        if !json.is_object() {
            return fail(format!("{field_name} must be an object"));
        }
        let root = Fields {
            object: json,
            prefix: field_name.to_string(),
        };
        let mut patch = Self {
            enabled: root.bool("enabled")?,
            automatic_recall: root.bool("automaticRecall")?,
            automatic_capture: root.bool("automaticCapture")?,
            recall_policy: root.string("recallPolicy")?,
            context_token_budget: root.uint32("contextTokenBudget")?,
            candidate_limit: root.uint32("candidateLimit")?,
            ..Default::default()
        };
        if let Some(retrieval) = root.object("retrieval")? {
            patch.retrieval = MemoryRetrievalSettingsPatch {
                graph_enabled: retrieval.bool("graphEnabled")?,
                dense_enabled: retrieval.bool("denseEnabled")?,
                code_enabled: retrieval.bool("codeEnabled")?,
                embedding_model: retrieval.nullable_string("embeddingModel")?,
                query_prefix: retrieval.string("queryPrefix")?,
                document_prefix: retrieval.string("documentPrefix")?,
                max_depth: retrieval.uint32("maxDepth")?,
                max_seeds: retrieval.uint32("maxSeeds")?,
                max_edges: retrieval.uint32("maxEdges")?,
                max_graph_results: retrieval.uint32("maxGraphResults")?,
                max_fanout: retrieval.uint32("maxFanout")?,
                query_cache_entries: retrieval.uint32("queryCacheEntries")?,
            };
        }
        if let Some(prehandler) = root.object("prehandler")? {
            patch.prehandler = MemoryPrehandlerSettingsPatch {
                model: prehandler.nullable_string("model")?,
                thinking: prehandler.string("thinking")?,
                timeout_ms: prehandler.int64("timeoutMs")?,
                fail_open: prehandler.bool("failOpen")?,
                max_model_input_tokens: prehandler.uint32("maxModelInputTokens")?,
            };
        }
        if let Some(capture) = root.object("capture")? {
            patch.capture = MemoryCaptureSettingsPatch {
                post_process: capture.string("postProcess")?,
                default_status: capture.string("defaultStatus")?,
                max_evidence_chars: capture.uint32("maxEvidenceChars")?,
            };
        }
        if let Some(dreaming) = root.object("dreaming")? {
            patch.dreaming = MemoryDreamingSettingsPatch {
                enabled: dreaming.bool("enabled")?,
                trigger: dreaming.string("trigger")?,
                research_policy: dreaming.string("researchPolicy")?,
                promotion_policy: dreaming.string("promotionPolicy")?,
                candidate_threshold: dreaming.uint32("candidateThreshold")?,
                max_model_input_tokens: dreaming.uint32("maxModelInputTokens")?,
                max_candidates: dreaming.uint32("maxCandidates")?,
                interval_seconds: dreaming.uint32("intervalSeconds")?,
                idle_seconds: dreaming.uint32("idleSeconds")?,
                model: dreaming.nullable_string("model")?,
                verifier_model: dreaming.nullable_string("verifierModel")?,
                timeout_ms: dreaming.int64("timeoutMs")?,
            };
        }
        Ok(patch)
    }

    /// The `memory` object of a whole settings document (absent: empty patch).
    pub fn from_settings(settings: &Value) -> Result<Self> {
        match settings.get("memory") {
            None => Ok(Self::default()),
            Some(memory) => Self::parse(memory, "memory"),
        }
    }

    /// Every field at its archived default.
    pub fn defaults() -> Self {
        Self {
            enabled: Some(false),
            automatic_recall: Some(true),
            automatic_capture: Some(true),
            recall_policy: Some("balanced".into()),
            context_token_budget: Some(900),
            candidate_limit: Some(50),
            prehandler: MemoryPrehandlerSettingsPatch {
                model: Some(None),
                thinking: Some("off".into()),
                timeout_ms: Some(30_000),
                fail_open: Some(true),
                max_model_input_tokens: Some(12_000),
            },
            capture: MemoryCaptureSettingsPatch {
                post_process: Some("turn_end".into()),
                default_status: Some("hypothesis".into()),
                max_evidence_chars: Some(32_000),
            },
            dreaming: MemoryDreamingSettingsPatch {
                enabled: Some(false),
                trigger: Some("post_turn".into()),
                interval_seconds: Some(3600),
                idle_seconds: Some(300),
                candidate_threshold: Some(1),
                model: Some(None),
                verifier_model: Some(None),
                timeout_ms: Some(600_000),
                max_model_input_tokens: Some(6000),
                max_candidates: Some(8),
                research_policy: Some("disabled".into()),
                promotion_policy: Some("review_only".into()),
            },
            retrieval: MemoryRetrievalSettingsPatch {
                graph_enabled: Some(false),
                dense_enabled: Some(false),
                code_enabled: Some(false),
                embedding_model: Some(None),
                query_prefix: Some(String::new()),
                document_prefix: Some(String::new()),
                max_depth: Some(2),
                max_seeds: Some(16),
                max_edges: Some(256),
                max_graph_results: Some(64),
                max_fanout: Some(32),
                query_cache_entries: Some(64),
            },
        }
    }

    pub fn is_empty(&self) -> bool {
        self == &Self::default()
    }

    /// Fields set in `overlay` replace those in `self`.
    pub fn merged(&self, overlay_patch: &Self) -> Self {
        let mut merged = self.clone();
        let o = overlay_patch;
        overlay(&mut merged.enabled, &o.enabled);
        overlay(&mut merged.automatic_recall, &o.automatic_recall);
        overlay(&mut merged.automatic_capture, &o.automatic_capture);
        overlay(&mut merged.recall_policy, &o.recall_policy);
        overlay(&mut merged.context_token_budget, &o.context_token_budget);
        overlay(&mut merged.candidate_limit, &o.candidate_limit);
        let (p, op) = (&mut merged.prehandler, &o.prehandler);
        overlay(&mut p.model, &op.model);
        overlay(&mut p.thinking, &op.thinking);
        overlay(&mut p.timeout_ms, &op.timeout_ms);
        overlay(&mut p.fail_open, &op.fail_open);
        overlay(&mut p.max_model_input_tokens, &op.max_model_input_tokens);
        let (c, oc) = (&mut merged.capture, &o.capture);
        overlay(&mut c.post_process, &oc.post_process);
        overlay(&mut c.default_status, &oc.default_status);
        overlay(&mut c.max_evidence_chars, &oc.max_evidence_chars);
        let (d, od) = (&mut merged.dreaming, &o.dreaming);
        overlay(&mut d.enabled, &od.enabled);
        overlay(&mut d.trigger, &od.trigger);
        overlay(&mut d.interval_seconds, &od.interval_seconds);
        overlay(&mut d.idle_seconds, &od.idle_seconds);
        overlay(&mut d.candidate_threshold, &od.candidate_threshold);
        overlay(&mut d.model, &od.model);
        overlay(&mut d.verifier_model, &od.verifier_model);
        overlay(&mut d.timeout_ms, &od.timeout_ms);
        overlay(&mut d.max_model_input_tokens, &od.max_model_input_tokens);
        overlay(&mut d.max_candidates, &od.max_candidates);
        overlay(&mut d.research_policy, &od.research_policy);
        overlay(&mut d.promotion_policy, &od.promotion_policy);
        let (r, or) = (&mut merged.retrieval, &o.retrieval);
        overlay(&mut r.graph_enabled, &or.graph_enabled);
        overlay(&mut r.dense_enabled, &or.dense_enabled);
        overlay(&mut r.code_enabled, &or.code_enabled);
        overlay(&mut r.embedding_model, &or.embedding_model);
        overlay(&mut r.query_prefix, &or.query_prefix);
        overlay(&mut r.document_prefix, &or.document_prefix);
        overlay(&mut r.max_depth, &or.max_depth);
        overlay(&mut r.max_seeds, &or.max_seeds);
        overlay(&mut r.max_edges, &or.max_edges);
        overlay(&mut r.max_graph_results, &or.max_graph_results);
        overlay(&mut r.max_fanout, &or.max_fanout);
        overlay(&mut r.query_cache_entries, &or.query_cache_entries);
        merged
    }

    /// Resolved-settings validation with the archived messages.
    pub fn validate(&self) -> Result<()> {
        validate_choice(
            &self.recall_policy,
            "memory.recallPolicy",
            &[
                "strict",
                "balanced",
                "exploratory",
                "debugging",
                "architecture",
            ],
        )?;
        validate_choice(
            &self.prehandler.thinking,
            "memory.prehandler.thinking",
            &["off", "minimal", "low", "medium", "high", "xhigh"],
        )?;
        validate_choice(
            &self.capture.post_process,
            "memory.capture.postProcess",
            &["turn_end", "session_end", "disabled"],
        )?;
        validate_choice(
            &self.capture.default_status,
            "memory.capture.defaultStatus",
            &["candidate", "hypothesis"],
        )?;
        validate_choice(
            &self.dreaming.trigger,
            "memory.dreaming.trigger",
            &["post_turn", "interval", "candidate_threshold"],
        )?;
        validate_choice(
            &self.dreaming.research_policy,
            "memory.dreaming.researchPolicy",
            &["disabled", "manual", "required"],
        )?;
        validate_choice(
            &self.dreaming.promotion_policy,
            "memory.dreaming.promotionPolicy",
            &["review_only", "manual", "auto_validate", "disabled"],
        )?;
        if matches!(&self.prehandler.model, Some(Some(model)) if model.is_empty()) {
            return fail("resolved settings memory.prehandler.model must not be empty".into());
        }
        if matches!(&self.dreaming.model, Some(Some(model)) if model.is_empty()) {
            return fail("resolved settings memory.dreaming.model must not be empty".into());
        }
        if let Some(Some(verifier)) = &self.dreaming.verifier_model
            && (verifier.is_empty() || verifier.eq_ignore_ascii_case("auto"))
        {
            return fail(
                "resolved settings memory.dreaming.verifierModel must not be empty or auto".into(),
            );
        }
        if self.context_token_budget == Some(0) {
            return fail("resolved settings memory.contextTokenBudget must be positive".into());
        }
        if self.candidate_limit == Some(0) {
            return fail("resolved settings memory.candidateLimit must be positive".into());
        }
        if matches!(&self.retrieval.embedding_model, Some(Some(model)) if model.is_empty()) {
            return fail(
                "resolved settings memory.retrieval.embeddingModel must not be empty".into(),
            );
        }
        let r = &self.retrieval;
        for (value, cap, field) in [
            (r.max_depth, 8, "memory.retrieval.maxDepth"),
            (r.max_seeds, 128, "memory.retrieval.maxSeeds"),
            (r.max_edges, 8192, "memory.retrieval.maxEdges"),
            (
                r.max_graph_results,
                1000,
                "memory.retrieval.maxGraphResults",
            ),
            (r.max_fanout, 256, "memory.retrieval.maxFanout"),
            (
                r.query_cache_entries,
                1024,
                "memory.retrieval.queryCacheEntries",
            ),
        ] {
            if let Some(value) = value
                && (value == 0 || value > cap)
            {
                return fail(format!(
                    "resolved settings {field} must be between 1 and {cap}"
                ));
            }
        }
        if self.prehandler.timeout_ms.is_some_and(|value| value <= 0) {
            return fail("resolved settings memory.prehandler.timeoutMs must be positive".into());
        }
        if self.prehandler.max_model_input_tokens == Some(0) {
            return fail(
                "resolved settings memory.prehandler.maxModelInputTokens must be positive".into(),
            );
        }
        if self.capture.max_evidence_chars == Some(0) {
            return fail(
                "resolved settings memory.capture.maxEvidenceChars must be positive".into(),
            );
        }
        let d = &self.dreaming;
        let ranges: [(Option<i64>, i64, i64, &str); 5] = [
            (
                d.interval_seconds.map(i64::from),
                1,
                31_536_000,
                "memory.dreaming.intervalSeconds",
            ),
            (
                d.idle_seconds.map(i64::from),
                1,
                86_400,
                "memory.dreaming.idleSeconds",
            ),
            (
                d.candidate_threshold.map(i64::from),
                1,
                1000,
                "memory.dreaming.candidateThreshold",
            ),
            (d.timeout_ms, 1, 600_000, "memory.dreaming.timeoutMs"),
            (
                d.max_model_input_tokens.map(i64::from),
                1,
                6000,
                "memory.dreaming.maxModelInputTokens",
            ),
        ];
        for (value, low, high, field) in ranges {
            if let Some(value) = value
                && (value < low || value > high)
            {
                return fail(format!(
                    "resolved settings {field} must be between {low} and {high}"
                ));
            }
        }
        if let Some(value) = d.max_candidates
            && !(2..=8).contains(&value)
        {
            return fail(
                "resolved settings memory.dreaming.maxCandidates must be between 2 and 8".into(),
            );
        }
        Ok(())
    }

    /// Fills unset fields with archived defaults (no validation).
    pub fn resolve(&self) -> MemorySettings {
        let full = Self::defaults().merged(self);
        let s = |value: Option<String>| value.unwrap_or_default();
        MemorySettings {
            enabled: full.enabled.unwrap_or(false),
            automatic_recall: full.automatic_recall.unwrap_or(true),
            automatic_capture: full.automatic_capture.unwrap_or(true),
            recall_policy: s(full.recall_policy),
            context_token_budget: full.context_token_budget.unwrap_or(900),
            candidate_limit: full.candidate_limit.unwrap_or(50),
            prehandler: MemoryPrehandlerSettings {
                model: full.prehandler.model.flatten(),
                thinking: s(full.prehandler.thinking),
                timeout_ms: full.prehandler.timeout_ms.unwrap_or(30_000),
                fail_open: full.prehandler.fail_open.unwrap_or(true),
                max_model_input_tokens: full.prehandler.max_model_input_tokens.unwrap_or(12_000),
            },
            capture: MemoryCaptureSettings {
                post_process: s(full.capture.post_process),
                default_status: s(full.capture.default_status),
                max_evidence_chars: full.capture.max_evidence_chars.unwrap_or(32_000),
            },
            dreaming: MemoryDreamingSettings {
                enabled: full.dreaming.enabled.unwrap_or(false),
                trigger: s(full.dreaming.trigger),
                interval_seconds: full.dreaming.interval_seconds.unwrap_or(3600),
                idle_seconds: full.dreaming.idle_seconds.unwrap_or(300),
                candidate_threshold: full.dreaming.candidate_threshold.unwrap_or(1),
                model: full.dreaming.model.flatten(),
                verifier_model: full.dreaming.verifier_model.flatten(),
                timeout_ms: full.dreaming.timeout_ms.unwrap_or(600_000),
                max_model_input_tokens: full.dreaming.max_model_input_tokens.unwrap_or(6000),
                max_candidates: full.dreaming.max_candidates.unwrap_or(8),
                research_policy: s(full.dreaming.research_policy),
                promotion_policy: s(full.dreaming.promotion_policy),
            },
            retrieval: MemoryRetrievalSettings {
                graph_enabled: full.retrieval.graph_enabled.unwrap_or(false),
                dense_enabled: full.retrieval.dense_enabled.unwrap_or(false),
                code_enabled: full.retrieval.code_enabled.unwrap_or(false),
                embedding_model: full.retrieval.embedding_model.flatten(),
                query_prefix: s(full.retrieval.query_prefix),
                document_prefix: s(full.retrieval.document_prefix),
                max_depth: full.retrieval.max_depth.unwrap_or(2),
                max_seeds: full.retrieval.max_seeds.unwrap_or(16),
                max_edges: full.retrieval.max_edges.unwrap_or(256),
                max_graph_results: full.retrieval.max_graph_results.unwrap_or(64),
                max_fanout: full.retrieval.max_fanout.unwrap_or(32),
                query_cache_entries: full.retrieval.query_cache_entries.unwrap_or(64),
            },
        }
    }

    /// Settings-file JSON for the fields this patch sets.
    pub fn to_json(&self) -> Value {
        let mut root = Map::new();
        let set = |map: &mut Map<String, Value>, key: &str, value: Option<Value>| {
            if let Some(value) = value {
                map.insert(key.into(), value);
            }
        };
        let nullable = |value: &Option<Option<String>>| value.as_ref().map(|value| json!(value));
        set(&mut root, "enabled", self.enabled.map(Value::from));
        set(
            &mut root,
            "automaticRecall",
            self.automatic_recall.map(Value::from),
        );
        set(
            &mut root,
            "automaticCapture",
            self.automatic_capture.map(Value::from),
        );
        set(
            &mut root,
            "recallPolicy",
            self.recall_policy.clone().map(Value::from),
        );
        set(
            &mut root,
            "contextTokenBudget",
            self.context_token_budget.map(Value::from),
        );
        set(
            &mut root,
            "candidateLimit",
            self.candidate_limit.map(Value::from),
        );
        let r = &self.retrieval;
        let mut retrieval = Map::new();
        set(
            &mut retrieval,
            "graphEnabled",
            r.graph_enabled.map(Value::from),
        );
        set(
            &mut retrieval,
            "denseEnabled",
            r.dense_enabled.map(Value::from),
        );
        set(
            &mut retrieval,
            "codeEnabled",
            r.code_enabled.map(Value::from),
        );
        set(
            &mut retrieval,
            "embeddingModel",
            nullable(&r.embedding_model),
        );
        set(
            &mut retrieval,
            "queryPrefix",
            r.query_prefix.clone().map(Value::from),
        );
        set(
            &mut retrieval,
            "documentPrefix",
            r.document_prefix.clone().map(Value::from),
        );
        set(&mut retrieval, "maxDepth", r.max_depth.map(Value::from));
        set(&mut retrieval, "maxSeeds", r.max_seeds.map(Value::from));
        set(&mut retrieval, "maxEdges", r.max_edges.map(Value::from));
        set(
            &mut retrieval,
            "maxGraphResults",
            r.max_graph_results.map(Value::from),
        );
        set(&mut retrieval, "maxFanout", r.max_fanout.map(Value::from));
        set(
            &mut retrieval,
            "queryCacheEntries",
            r.query_cache_entries.map(Value::from),
        );
        let p = &self.prehandler;
        let mut prehandler = Map::new();
        set(&mut prehandler, "model", nullable(&p.model));
        set(
            &mut prehandler,
            "thinking",
            p.thinking.clone().map(Value::from),
        );
        set(&mut prehandler, "timeoutMs", p.timeout_ms.map(Value::from));
        set(&mut prehandler, "failOpen", p.fail_open.map(Value::from));
        set(
            &mut prehandler,
            "maxModelInputTokens",
            p.max_model_input_tokens.map(Value::from),
        );
        let c = &self.capture;
        let mut capture = Map::new();
        set(
            &mut capture,
            "postProcess",
            c.post_process.clone().map(Value::from),
        );
        set(
            &mut capture,
            "defaultStatus",
            c.default_status.clone().map(Value::from),
        );
        set(
            &mut capture,
            "maxEvidenceChars",
            c.max_evidence_chars.map(Value::from),
        );
        let d = &self.dreaming;
        let mut dreaming = Map::new();
        set(&mut dreaming, "enabled", d.enabled.map(Value::from));
        set(&mut dreaming, "trigger", d.trigger.clone().map(Value::from));
        set(
            &mut dreaming,
            "intervalSeconds",
            d.interval_seconds.map(Value::from),
        );
        set(
            &mut dreaming,
            "idleSeconds",
            d.idle_seconds.map(Value::from),
        );
        set(
            &mut dreaming,
            "candidateThreshold",
            d.candidate_threshold.map(Value::from),
        );
        set(&mut dreaming, "model", nullable(&d.model));
        set(&mut dreaming, "verifierModel", nullable(&d.verifier_model));
        set(&mut dreaming, "timeoutMs", d.timeout_ms.map(Value::from));
        set(
            &mut dreaming,
            "maxModelInputTokens",
            d.max_model_input_tokens.map(Value::from),
        );
        set(
            &mut dreaming,
            "maxCandidates",
            d.max_candidates.map(Value::from),
        );
        set(
            &mut dreaming,
            "researchPolicy",
            d.research_policy.clone().map(Value::from),
        );
        set(
            &mut dreaming,
            "promotionPolicy",
            d.promotion_policy.clone().map(Value::from),
        );
        for (key, section) in [
            ("retrieval", retrieval),
            ("prehandler", prehandler),
            ("capture", capture),
            ("dreaming", dreaming),
        ] {
            if !section.is_empty() {
                root.insert(key.into(), Value::Object(section));
            }
        }
        Value::Object(root)
    }
}

fn capture_scope(value: &str) -> u8 {
    match value {
        "disabled" => 0,
        "session_end" => 1,
        _ => 2,
    }
}

fn recall_scope(value: &str) -> u8 {
    match value {
        "strict" => 0,
        "exploratory" => 2,
        _ => 1,
    }
}

fn research_scope(value: &str) -> u8 {
    match value {
        "disabled" => 0,
        "manual" => 1,
        _ => 2,
    }
}

fn promotion_scope(value: &str) -> u8 {
    match value {
        "disabled" => 0,
        "auto_validate" => 2,
        _ => 1,
    }
}

/// Layers a project patch over the global policy, rejecting any project
/// value that broadens it ("project settings may not broaden global policy:
/// <field>").
pub fn merge_project_settings(
    global: &MemorySettingsPatch,
    project: &MemorySettingsPatch,
) -> Result<MemorySettingsPatch> {
    let reject = |field: &str| {
        fail(format!(
            "project settings may not broaden global policy: {field}"
        ))
    };
    let (g, p) = (global, project);
    if p.enabled.unwrap_or(false) && !g.enabled.unwrap_or(false) {
        return reject("memory.enabled");
    }
    if p.automatic_recall.unwrap_or(false) && !g.automatic_recall.unwrap_or(true) {
        return reject("memory.automaticRecall");
    }
    if p.automatic_capture.unwrap_or(false) && !g.automatic_capture.unwrap_or(true) {
        return reject("memory.automaticCapture");
    }
    if p.dreaming.enabled.unwrap_or(false) && !g.dreaming.enabled.unwrap_or(false) {
        return reject("memory.dreaming.enabled");
    }
    let greater = |project: Option<i64>, global: Option<i64>| matches!((project, global), (Some(p), Some(g)) if p > g);
    let smaller = |project: Option<i64>, global: Option<i64>| matches!((project, global), (Some(p), Some(g)) if p < g);
    let w = |value: Option<u32>| value.map(i64::from);
    if greater(w(p.context_token_budget), w(g.context_token_budget)) {
        return reject("memory.contextTokenBudget");
    }
    if greater(w(p.candidate_limit), w(g.candidate_limit)) {
        return reject("memory.candidateLimit");
    }
    if greater(
        w(p.prehandler.max_model_input_tokens),
        w(g.prehandler.max_model_input_tokens),
    ) {
        return reject("memory.prehandler.maxModelInputTokens");
    }
    if greater(
        w(p.capture.max_evidence_chars),
        w(g.capture.max_evidence_chars),
    ) {
        return reject("memory.capture.maxEvidenceChars");
    }
    if let (Some(project), Some(global)) = (&p.dreaming.trigger, &g.dreaming.trigger)
        && project != global
    {
        return reject("memory.dreaming.trigger");
    }
    if smaller(
        w(p.dreaming.interval_seconds),
        w(g.dreaming.interval_seconds),
    ) {
        return reject("memory.dreaming.intervalSeconds");
    }
    if smaller(w(p.dreaming.idle_seconds), w(g.dreaming.idle_seconds)) {
        return reject("memory.dreaming.idleSeconds");
    }
    if smaller(
        w(p.dreaming.candidate_threshold),
        w(g.dreaming.candidate_threshold),
    ) {
        return reject("memory.dreaming.candidateThreshold");
    }
    if p.dreaming.model.is_some() && p.dreaming.model != g.dreaming.model {
        return reject("memory.dreaming.model");
    }
    if p.dreaming.verifier_model.is_some() && p.dreaming.verifier_model != g.dreaming.verifier_model
    {
        return reject("memory.dreaming.verifierModel");
    }
    if greater(p.dreaming.timeout_ms, g.dreaming.timeout_ms) {
        return reject("memory.dreaming.timeoutMs");
    }
    if greater(
        w(p.dreaming.max_model_input_tokens),
        w(g.dreaming.max_model_input_tokens),
    ) {
        return reject("memory.dreaming.maxModelInputTokens");
    }
    if greater(w(p.dreaming.max_candidates), w(g.dreaming.max_candidates)) {
        return reject("memory.dreaming.maxCandidates");
    }
    let broader = |project: &Option<String>, global: &Option<String>, rank: fn(&str) -> u8| matches!((project, global), (Some(p), Some(g)) if rank(p) > rank(g));
    if broader(
        &p.capture.post_process,
        &g.capture.post_process,
        capture_scope,
    ) {
        return reject("memory.capture.postProcess");
    }
    if broader(&p.recall_policy, &g.recall_policy, recall_scope) {
        return reject("memory.recallPolicy");
    }
    if broader(
        &p.dreaming.research_policy,
        &g.dreaming.research_policy,
        research_scope,
    ) {
        return reject("memory.dreaming.researchPolicy");
    }
    if broader(
        &p.dreaming.promotion_policy,
        &g.dreaming.promotion_policy,
        promotion_scope,
    ) {
        return reject("memory.dreaming.promotionPolicy");
    }
    Ok(global.merged(project))
}

/// Resolves memory settings from the global settings document and an
/// optional project settings document, validating the result.
pub fn resolve_memory_settings(global: &Value, project: Option<&Value>) -> Result<MemorySettings> {
    let global_patch = MemorySettingsPatch::from_settings(global)?;
    let merged = match project {
        Some(project) => {
            merge_project_settings(&global_patch, &MemorySettingsPatch::from_settings(project)?)?
        }
        None => global_patch,
    };
    let full = MemorySettingsPatch::defaults().merged(&merged);
    full.validate()?;
    Ok(full.resolve())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_match_archive() {
        let settings = MemorySettings::default();
        assert!(!settings.enabled);
        assert!(settings.automatic_recall && settings.automatic_capture);
        assert_eq!(settings.recall_policy, "balanced");
        assert_eq!(
            (settings.context_token_budget, settings.candidate_limit),
            (900, 50)
        );
        assert_eq!(settings.prehandler.thinking, "off");
        assert_eq!(settings.prehandler.timeout_ms, 30_000);
        assert_eq!(settings.prehandler.max_model_input_tokens, 12_000);
        assert_eq!(settings.capture.post_process, "turn_end");
        assert_eq!(settings.capture.default_status, "hypothesis");
        assert_eq!(settings.capture.max_evidence_chars, 32_000);
        assert_eq!(settings.dreaming.trigger, "post_turn");
        assert_eq!(
            (
                settings.dreaming.interval_seconds,
                settings.dreaming.idle_seconds
            ),
            (3600, 300)
        );
        assert_eq!(settings.dreaming.timeout_ms, 600_000);
        assert_eq!(
            (
                settings.dreaming.max_model_input_tokens,
                settings.dreaming.max_candidates
            ),
            (6000, 8)
        );
        assert_eq!(settings.dreaming.research_policy, "disabled");
        assert_eq!(settings.dreaming.promotion_policy, "review_only");
        assert_eq!(settings.retrieval.max_depth, 2);
        assert_eq!(settings.retrieval.max_edges, 256);
        assert_eq!(settings.retrieval.query_cache_entries, 64);
        assert_eq!(resolve_memory_settings(&json!({}), None).unwrap(), settings);
    }

    #[test]
    fn parses_and_round_trips() {
        let input = json!({"memory": {
            "enabled": true, "recallPolicy": "strict", "contextTokenBudget": 500,
            "prehandler": {"model": null, "thinking": "low"},
            "dreaming": {"enabled": true, "model": "llama/x", "maxCandidates": 4},
            "retrieval": {"graphEnabled": true, "maxDepth": 3}
        }});
        let patch = MemorySettingsPatch::from_settings(&input).unwrap();
        assert_eq!(patch.prehandler.model, Some(None));
        assert_eq!(patch.dreaming.model, Some(Some("llama/x".into())));
        assert_eq!(
            MemorySettingsPatch::parse(&patch.to_json(), "memory").unwrap(),
            patch
        );
        let resolved = resolve_memory_settings(&input, None).unwrap();
        assert!(resolved.enabled && resolved.dreaming.enabled && resolved.retrieval.graph_enabled);
        assert_eq!(resolved.retrieval.max_depth, 3);
    }

    #[test]
    fn rejects_invalid_values_with_archived_messages() {
        let error = |value: Value| {
            resolve_memory_settings(&json!({ "memory": value }), None)
                .unwrap_err()
                .to_string()
        };
        assert_eq!(
            error(json!({"enabled": 1})),
            "memory.enabled must be a boolean"
        );
        assert_eq!(
            error(json!({"contextTokenBudget": -1})),
            "memory.contextTokenBudget is out of range"
        );
        assert_eq!(
            error(json!({"capture": 3})),
            "memory.capture must be an object"
        );
        assert_eq!(
            error(json!({"recallPolicy": "wild"})),
            "resolved settings memory.recallPolicy has unsupported value: wild"
        );
        assert_eq!(
            error(json!({"contextTokenBudget": 0})),
            "resolved settings memory.contextTokenBudget must be positive"
        );
        assert_eq!(
            error(json!({"dreaming": {"verifierModel": "AUTO"}})),
            "resolved settings memory.dreaming.verifierModel must not be empty or auto"
        );
        assert_eq!(
            error(json!({"dreaming": {"maxCandidates": 1}})),
            "resolved settings memory.dreaming.maxCandidates must be between 2 and 8"
        );
        assert_eq!(
            error(json!({"retrieval": {"maxDepth": 9}})),
            "resolved settings memory.retrieval.maxDepth must be between 1 and 8"
        );
        assert_eq!(
            error(json!({"dreaming": {"timeoutMs": 700000}})),
            "resolved settings memory.dreaming.timeoutMs must be between 1 and 600000"
        );
    }

    #[test]
    fn project_may_not_broaden_global_policy() {
        let global = json!({"memory": {"enabled": true, "recallPolicy": "balanced", "contextTokenBudget": 900,
            "capture": {"postProcess": "session_end"}, "dreaming": {"promotionPolicy": "review_only"}}});
        let reject = |project: Value| {
            resolve_memory_settings(&global, Some(&json!({ "memory": project })))
                .unwrap_err()
                .to_string()
        };
        assert_eq!(
            reject(json!({"recallPolicy": "exploratory"})),
            "project settings may not broaden global policy: memory.recallPolicy"
        );
        assert_eq!(
            reject(json!({"contextTokenBudget": 1000})),
            "project settings may not broaden global policy: memory.contextTokenBudget"
        );
        assert_eq!(
            reject(json!({"capture": {"postProcess": "turn_end"}})),
            "project settings may not broaden global policy: memory.capture.postProcess"
        );
        assert_eq!(
            reject(json!({"dreaming": {"promotionPolicy": "auto_validate"}})),
            "project settings may not broaden global policy: memory.dreaming.promotionPolicy"
        );
        assert_eq!(
            reject(json!({"dreaming": {"enabled": true}})),
            "project settings may not broaden global policy: memory.dreaming.enabled"
        );
        assert_eq!(
            reject(json!({"dreaming": {"model": "llama/y"}})),
            "project settings may not broaden global policy: memory.dreaming.model"
        );
        let disabled = json!({"memory": {}});
        assert_eq!(
            resolve_memory_settings(&disabled, Some(&json!({"memory": {"enabled": true}})))
                .unwrap_err()
                .to_string(),
            "project settings may not broaden global policy: memory.enabled"
        );
        let narrowed = resolve_memory_settings(
            &global,
            Some(&json!({"memory": {"recallPolicy": "strict", "contextTokenBudget": 400}})),
        )
        .unwrap();
        assert_eq!(narrowed.recall_policy, "strict");
        assert_eq!(narrowed.context_token_budget, 400);
    }
}
