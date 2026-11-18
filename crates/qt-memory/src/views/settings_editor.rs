//! Memory settings editor model (archived `/memory settings` list in
//! `interactive_mode.cpp`): every `memory.*` key the archive exposed with
//! its label, description, choices and bounds; value parsing with the
//! archived messages; and the archived persistence of the whole `memory`
//! object into global settings.

use serde_json::{Map, Value, json};

use crate::settings::{MemorySettings, MemorySettingsPatch, resolve_memory_settings};

/// How a setting is edited.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SettingKind {
    /// `off` / `on`.
    Toggle,
    /// One of a fixed list.
    Choice(&'static [&'static str]),
    /// A whole number from 1 to `max`.
    Number { max: u64 },
    /// A model picker: `fallback` clears the setting; `local_only` limits it
    /// to `llama/` models.
    Model {
        fallback: &'static str,
        fallback_description: &'static str,
        local_only: bool,
    },
}

/// One editable memory setting.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SettingSpec {
    pub id: &'static str,
    /// The settings-file key (`memory.dreaming.timeoutMs`).
    pub key: &'static str,
    pub label: &'static str,
    pub description: &'static str,
    pub kind: SettingKind,
}

const ON_OFF: &[&str] = &["off", "on"];
const U32_MAX: u64 = u32::MAX as u64;
const I64_MAX: u64 = i64::MAX as u64;

/// The archived settings list, in order.
pub fn setting_specs() -> Vec<SettingSpec> {
    use SettingKind::*;
    let spec = |id, key, label, description, kind| SettingSpec {
        id,
        key,
        label,
        description,
        kind,
    };
    vec![
        spec(
            "enabled",
            "memory.enabled",
            "Enabled",
            "Enable built-in memory on the next qt start",
            Toggle,
        ),
        spec(
            "automatic_recall",
            "memory.automaticRecall",
            "Automatic recall",
            "Recall memory before prompts on the next qt start",
            Toggle,
        ),
        spec(
            "automatic_capture",
            "memory.automaticCapture",
            "Automatic capture",
            "Capture completed episodes on the next qt start",
            Toggle,
        ),
        spec(
            "recall_policy",
            "memory.recallPolicy",
            "Recall policy",
            "Retrieval policy for the isolated pre-handler",
            Choice(&[
                "strict",
                "balanced",
                "exploratory",
                "debugging",
                "architecture",
            ]),
        ),
        spec(
            "context_token_budget",
            "memory.contextTokenBudget",
            "Context token budget",
            "Maximum recalled tokens injected into a prompt",
            Number { max: U32_MAX },
        ),
        spec(
            "candidate_limit",
            "memory.candidateLimit",
            "Candidate limit",
            "Maximum memories considered during recall",
            Number { max: U32_MAX },
        ),
        spec(
            "prehandler_model",
            "memory.prehandler.model",
            "Pre-handler model",
            "Select an available model or child default selection",
            Model {
                fallback: "default",
                fallback_description: "Child default selection",
                local_only: false,
            },
        ),
        spec(
            "prehandler_thinking",
            "memory.prehandler.thinking",
            "Pre-handler thinking",
            "Thinking level for the isolated memory child",
            Choice(&["off", "minimal", "low", "medium", "high", "xhigh"]),
        ),
        spec(
            "prehandler_timeout_ms",
            "memory.prehandler.timeoutMs",
            "Pre-handler timeout",
            "Positive timeout in milliseconds",
            Number { max: I64_MAX },
        ),
        spec(
            "prehandler_fail_open",
            "memory.prehandler.failOpen",
            "Pre-handler fail-open",
            "Continue without memory when recall preprocessing fails",
            Toggle,
        ),
        spec(
            "prehandler_max_model_input_tokens",
            "memory.prehandler.maxModelInputTokens",
            "Pre-handler input limit",
            "Maximum tokens projected into the isolated child",
            Number { max: U32_MAX },
        ),
        spec(
            "capture_post_process",
            "memory.capture.postProcess",
            "Capture post-process",
            "When reviewable candidates are generated",
            Choice(&["turn_end", "session_end", "disabled"]),
        ),
        spec(
            "capture_default_status",
            "memory.capture.defaultStatus",
            "Capture default status",
            "Governance status assigned to captured memories",
            Choice(&["candidate", "hypothesis"]),
        ),
        spec(
            "capture_max_evidence_chars",
            "memory.capture.maxEvidenceChars",
            "Capture evidence limit",
            "Maximum evidence characters retained per episode",
            Number { max: U32_MAX },
        ),
        spec(
            "dreaming_enabled",
            "memory.dreaming.enabled",
            "Dreaming enabled",
            "Opt in to isolated governed memory exploration on the next start",
            Toggle,
        ),
        spec(
            "dreaming_trigger",
            "memory.dreaming.trigger",
            "Dreaming trigger",
            "Schedule a governed Dreaming cycle after memory capture",
            Choice(&["post_turn", "interval", "candidate_threshold"]),
        ),
        spec(
            "dreaming_interval_seconds",
            "memory.dreaming.intervalSeconds",
            "Dreaming interval",
            "Minimum interval in seconds for interval scheduling",
            Number { max: 31_536_000 },
        ),
        spec(
            "dreaming_idle_seconds",
            "memory.dreaming.idleSeconds",
            "Dreaming idle duration",
            "Required application idle time before background dreaming",
            Number { max: 86_400 },
        ),
        spec(
            "dreaming_candidate_threshold",
            "memory.dreaming.candidateThreshold",
            "Dreaming threshold",
            "Durable candidate count required by threshold scheduling",
            Number { max: 1000 },
        ),
        spec(
            "dreaming_model",
            "memory.dreaming.model",
            "Dreaming model",
            "Select an available model or automatic task selection",
            Model {
                fallback: "auto",
                fallback_description: "Automatic task selection",
                local_only: false,
            },
        ),
        spec(
            "dreaming_verifier_model",
            "memory.dreaming.verifierModel",
            "Dreaming verifier model",
            "Select a local llama.cpp model; unset reuses Dreaming only when local",
            Model {
                fallback: "unset",
                fallback_description: "Reuse Dreaming model only when it is local",
                local_only: true,
            },
        ),
        spec(
            "dreaming_timeout_ms",
            "memory.dreaming.timeoutMs",
            "Dreaming timeout",
            "Isolated worker timeout in milliseconds (maximum 600000)",
            Number { max: 600_000 },
        ),
        spec(
            "dreaming_max_model_input_tokens",
            "memory.dreaming.maxModelInputTokens",
            "Dreaming input limit",
            "Maximum isolated-worker input tokens (maximum 6000)",
            Number { max: 6000 },
        ),
        spec(
            "dreaming_max_candidates",
            "memory.dreaming.maxCandidates",
            "Dreaming candidate limit",
            "Governed records in one dream cycle (between 2 and 8)",
            Choice(&["2", "3", "4", "5", "6", "7", "8"]),
        ),
        spec(
            "dreaming_research_policy",
            "memory.dreaming.researchPolicy",
            "Dreaming research policy",
            "disabled, manual evidence, or required isolated web research",
            Choice(&["disabled", "manual", "required"]),
        ),
        spec(
            "dreaming_promotion_policy",
            "memory.dreaming.promotionPolicy",
            "Dreaming promotion policy",
            "review_only/manual: review approval may promote to Hypothesis; auto_validate: locally verify researched successors; disabled: no promotion",
            Choice(&["review_only", "manual", "auto_validate", "disabled"]),
        ),
    ]
}

fn on_off(value: bool) -> String {
    ON_OFF[usize::from(value)].to_string()
}

/// Displayed value of a setting (archived `setting_value`).
pub fn setting_value(settings: &MemorySettings, id: &str) -> String {
    let p = &settings.prehandler;
    let c = &settings.capture;
    let d = &settings.dreaming;
    match id {
        "enabled" => on_off(settings.enabled),
        "automatic_recall" => on_off(settings.automatic_recall),
        "automatic_capture" => on_off(settings.automatic_capture),
        "recall_policy" => settings.recall_policy.clone(),
        "context_token_budget" => settings.context_token_budget.to_string(),
        "candidate_limit" => settings.candidate_limit.to_string(),
        "prehandler_model" => p.model.clone().unwrap_or_else(|| "default".into()),
        "prehandler_thinking" => p.thinking.clone(),
        "prehandler_timeout_ms" => p.timeout_ms.to_string(),
        "prehandler_fail_open" => on_off(p.fail_open),
        "prehandler_max_model_input_tokens" => p.max_model_input_tokens.to_string(),
        "capture_post_process" => c.post_process.clone(),
        "capture_default_status" => c.default_status.clone(),
        "capture_max_evidence_chars" => c.max_evidence_chars.to_string(),
        "dreaming_enabled" => on_off(d.enabled),
        "dreaming_trigger" => d.trigger.clone(),
        "dreaming_interval_seconds" => d.interval_seconds.to_string(),
        "dreaming_idle_seconds" => d.idle_seconds.to_string(),
        "dreaming_candidate_threshold" => d.candidate_threshold.to_string(),
        "dreaming_model" => d.model.clone().unwrap_or_else(|| "auto".into()),
        "dreaming_verifier_model" => d.verifier_model.clone().unwrap_or_else(|| "unset".into()),
        "dreaming_timeout_ms" => d.timeout_ms.to_string(),
        "dreaming_max_model_input_tokens" => d.max_model_input_tokens.to_string(),
        "dreaming_max_candidates" => d.max_candidates.to_string(),
        "dreaming_research_policy" => d.research_policy.clone(),
        "dreaming_promotion_policy" => d.promotion_policy.clone(),
        _ => String::new(),
    }
}

/// A whole number from 1 to `max` (archived positive-integer submenu).
pub fn parse_positive(value: &str, max: u64) -> Result<u64, String> {
    let value = value.trim();
    let parsed = if !value.is_empty() && value.bytes().all(|b| b.is_ascii_digit()) {
        value.parse::<u64>().unwrap_or(0)
    } else {
        0
    };
    if parsed == 0 || parsed > max {
        return Err(format!(
            "error: memory setting must be a whole number between 1 and {max}"
        ));
    }
    Ok(parsed)
}

const INVALID: &str = "error: invalid memory setting value";

/// Settings with `id` set to `value` (archived change callback). Numbers are
/// validated against the field's bound first; the result is validated as a
/// whole with the settings rules.
pub fn apply_setting(
    settings: &MemorySettings,
    id: &str,
    value: &str,
) -> Result<MemorySettings, String> {
    let spec = setting_specs()
        .into_iter()
        .find(|spec| spec.id == id)
        .ok_or_else(|| INVALID.to_string())?;
    match &spec.kind {
        SettingKind::Toggle if value != "on" && value != "off" => return Err(INVALID.into()),
        SettingKind::Choice(choices) if !choices.contains(&value) => {
            return Err(INVALID.into());
        }
        SettingKind::Number { max } => {
            parse_positive(value, *max)?;
        }
        SettingKind::Model { local_only, .. } => {
            if value.trim().is_empty() {
                return Err(INVALID.into());
            }
            if id == "dreaming_verifier_model" && value.eq_ignore_ascii_case("auto") {
                return Err("error: Dreaming verifier model cannot be auto".into());
            }
            if *local_only && value != "unset" && !value.starts_with("llama/") {
                return Err(
                    "error: Dreaming verifier model must be a local llama.cpp model".into(),
                );
            }
        }
        _ => {}
    }
    let on = value == "on";
    let number = || value.trim().parse::<u64>().map_err(|_| INVALID.to_string());
    let small =
        || -> Result<u32, String> { u32::try_from(number()?).map_err(|_| INVALID.to_string()) };
    let large =
        || -> Result<i64, String> { i64::try_from(number()?).map_err(|_| INVALID.to_string()) };
    let model = |fallback: &str| (value != fallback).then(|| value.to_string());
    let mut next = settings.clone();
    match id {
        "enabled" => next.enabled = on,
        "automatic_recall" => next.automatic_recall = on,
        "automatic_capture" => next.automatic_capture = on,
        "recall_policy" => next.recall_policy = value.into(),
        "context_token_budget" => next.context_token_budget = small()?,
        "candidate_limit" => next.candidate_limit = small()?,
        "prehandler_model" => next.prehandler.model = model("default"),
        "prehandler_thinking" => next.prehandler.thinking = value.into(),
        "prehandler_timeout_ms" => next.prehandler.timeout_ms = large()?,
        "prehandler_fail_open" => next.prehandler.fail_open = on,
        "prehandler_max_model_input_tokens" => next.prehandler.max_model_input_tokens = small()?,
        "capture_post_process" => next.capture.post_process = value.into(),
        "capture_default_status" => next.capture.default_status = value.into(),
        "capture_max_evidence_chars" => next.capture.max_evidence_chars = small()?,
        "dreaming_enabled" => next.dreaming.enabled = on,
        "dreaming_trigger" => next.dreaming.trigger = value.into(),
        "dreaming_interval_seconds" => next.dreaming.interval_seconds = small()?,
        "dreaming_idle_seconds" => next.dreaming.idle_seconds = small()?,
        "dreaming_candidate_threshold" => next.dreaming.candidate_threshold = small()?,
        "dreaming_model" => next.dreaming.model = model("auto"),
        "dreaming_verifier_model" => next.dreaming.verifier_model = model("unset"),
        "dreaming_timeout_ms" => next.dreaming.timeout_ms = large()?,
        "dreaming_max_model_input_tokens" => next.dreaming.max_model_input_tokens = small()?,
        "dreaming_max_candidates" => next.dreaming.max_candidates = small()?,
        "dreaming_research_policy" => next.dreaming.research_policy = value.into(),
        "dreaming_promotion_policy" => next.dreaming.promotion_policy = value.into(),
        _ => return Err(INVALID.into()),
    }
    let mut document = Map::new();
    write_memory_settings(&mut document, &next);
    MemorySettingsPatch::from_settings(&Value::Object(document))
        .and_then(|patch| patch.validate())
        .map_err(|error| format!("error: {error}"))?;
    Ok(next)
}

fn object<'a>(parent: &'a mut Map<String, Value>, key: &str) -> &'a mut Map<String, Value> {
    if !parent.get(key).is_some_and(Value::is_object) {
        parent.insert(key.into(), Value::Object(Map::new()));
    }
    parent
        .get_mut(key)
        .and_then(Value::as_object_mut)
        .expect("object inserted above")
}

/// Writes the archived `memory` keys into a global settings document,
/// keeping other keys (including `memory.retrieval`) untouched.
pub fn write_memory_settings(document: &mut Map<String, Value>, settings: &MemorySettings) {
    let memory = object(document, "memory");
    memory.insert("enabled".into(), json!(settings.enabled));
    memory.insert("automaticRecall".into(), json!(settings.automatic_recall));
    memory.insert("automaticCapture".into(), json!(settings.automatic_capture));
    memory.insert("recallPolicy".into(), json!(settings.recall_policy));
    memory.insert(
        "contextTokenBudget".into(),
        json!(settings.context_token_budget),
    );
    memory.insert("candidateLimit".into(), json!(settings.candidate_limit));
    let p = &settings.prehandler;
    let prehandler = object(memory, "prehandler");
    prehandler.insert("model".into(), json!(p.model));
    prehandler.insert("thinking".into(), json!(p.thinking));
    prehandler.insert("timeoutMs".into(), json!(p.timeout_ms));
    prehandler.insert("failOpen".into(), json!(p.fail_open));
    prehandler.insert(
        "maxModelInputTokens".into(),
        json!(p.max_model_input_tokens),
    );
    let c = &settings.capture;
    let capture = object(memory, "capture");
    capture.insert("postProcess".into(), json!(c.post_process));
    capture.insert("defaultStatus".into(), json!(c.default_status));
    capture.insert("maxEvidenceChars".into(), json!(c.max_evidence_chars));
    let d = &settings.dreaming;
    let dreaming = object(memory, "dreaming");
    dreaming.insert("enabled".into(), json!(d.enabled));
    dreaming.insert("trigger".into(), json!(d.trigger));
    dreaming.insert("intervalSeconds".into(), json!(d.interval_seconds));
    dreaming.insert("idleSeconds".into(), json!(d.idle_seconds));
    dreaming.insert("candidateThreshold".into(), json!(d.candidate_threshold));
    dreaming.insert("model".into(), json!(d.model));
    dreaming.insert("verifierModel".into(), json!(d.verifier_model));
    dreaming.insert("timeoutMs".into(), json!(d.timeout_ms));
    dreaming.insert(
        "maxModelInputTokens".into(),
        json!(d.max_model_input_tokens),
    );
    dreaming.insert("maxCandidates".into(), json!(d.max_candidates));
    dreaming.insert("researchPolicy".into(), json!(d.research_policy));
    dreaming.insert("promotionPolicy".into(), json!(d.promotion_policy));
}

/// The settings the editor starts from: the global layer only (a project
/// layer may narrow it at runtime but is never copied into global
/// settings). Invalid global settings fall back to the defaults with the
/// error.
pub fn global_memory_settings(global: &Map<String, Value>) -> (MemorySettings, Option<String>) {
    match resolve_memory_settings(&Value::Object(global.clone()), None) {
        Ok(settings) => (settings, None),
        Err(error) => (MemorySettings::default(), Some(error.to_string())),
    }
}

/// Notice shown when the editor closes after a change (archived).
pub const SAVED_NOTICE: &str = "Memory settings saved; changes apply to the next runtime.";

/// A model offered by a model picker.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModelChoice {
    pub value: String,
    pub title: String,
    pub description: String,
}

/// Model picker entries (archived `model_submenu`): the fallback, then each
/// available model once (only `llama/` models for a local-only picker), then
/// the current value when it is not in the catalog.
pub fn model_choices(
    spec: &SettingSpec,
    current: &str,
    models: &[(String, String)],
) -> Vec<ModelChoice> {
    let SettingKind::Model {
        fallback,
        fallback_description,
        local_only,
    } = &spec.kind
    else {
        return Vec::new();
    };
    let mut choices = vec![ModelChoice {
        value: fallback.to_string(),
        title: fallback.to_string(),
        description: fallback_description.to_string(),
    }];
    for (id, title) in models {
        if (*local_only && !id.starts_with("llama/")) || choices.iter().any(|c| &c.value == id) {
            continue;
        }
        choices.push(ModelChoice {
            value: id.clone(),
            title: id.clone(),
            description: title.clone(),
        });
    }
    if current != *fallback
        && !choices.iter().any(|choice| choice.value == current)
        && (!local_only || current.starts_with("llama/"))
    {
        choices.push(ModelChoice {
            value: current.to_string(),
            title: current.to_string(),
            description: "Current setting (not available in catalog)".into(),
        });
    }
    choices
}

/// Picker heading (archived): a hint when no local model is configured.
pub fn model_picker_heading(spec: &SettingSpec, choices: &[ModelChoice]) -> String {
    let local_only = matches!(
        spec.kind,
        SettingKind::Model {
            local_only: true,
            ..
        }
    );
    format!(
        "{}{}",
        spec.label,
        if local_only && choices.len() == 1 {
            " — no local models configured; add one with /llama"
        } else {
            " — type to filter"
        }
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn values_round_trip_and_validate() {
        let settings = MemorySettings::default();
        assert_eq!(setting_value(&settings, "enabled"), "off");
        assert_eq!(setting_value(&settings, "prehandler_model"), "default");
        assert_eq!(setting_value(&settings, "dreaming_verifier_model"), "unset");
        let next = apply_setting(&settings, "enabled", "on").unwrap();
        assert!(next.enabled);
        let next = apply_setting(&next, "dreaming_timeout_ms", "1200").unwrap();
        assert_eq!(next.dreaming.timeout_ms, 1200);
        assert_eq!(
            apply_setting(&next, "dreaming_timeout_ms", "600001").unwrap_err(),
            "error: memory setting must be a whole number between 1 and 600000"
        );
        assert_eq!(
            apply_setting(&next, "context_token_budget", "0").unwrap_err(),
            "error: memory setting must be a whole number between 1 and 4294967295"
        );
        assert_eq!(
            apply_setting(&next, "dreaming_verifier_model", "AUTO").unwrap_err(),
            "error: Dreaming verifier model cannot be auto"
        );
        assert!(apply_setting(&next, "dreaming_verifier_model", "openai/gpt").is_err());
        let next = apply_setting(&next, "dreaming_verifier_model", "llama/qwen").unwrap();
        assert_eq!(next.dreaming.verifier_model.as_deref(), Some("llama/qwen"));
        let next = apply_setting(&next, "dreaming_verifier_model", "unset").unwrap();
        assert_eq!(next.dreaming.verifier_model, None);
        assert_eq!(
            apply_setting(&next, "recall_policy", "wide").unwrap_err(),
            "error: invalid memory setting value"
        );
        assert_eq!(setting_specs().len(), 26);
    }

    #[test]
    fn writes_archived_keys_and_keeps_others() {
        let mut document: Map<String, Value> = serde_json::from_value(json!({
            "theme": "dark",
            "memory": {"retrieval": {"graphEnabled": true}, "prehandler": {"x": 1}}
        }))
        .unwrap();
        let settings = apply_setting(&MemorySettings::default(), "enabled", "on").unwrap();
        write_memory_settings(&mut document, &settings);
        assert_eq!(document["theme"], "dark");
        assert_eq!(document["memory"]["retrieval"]["graphEnabled"], true);
        assert_eq!(document["memory"]["prehandler"]["x"], 1);
        assert_eq!(document["memory"]["enabled"], true);
        assert_eq!(document["memory"]["prehandler"]["model"], Value::Null);
        assert_eq!(document["memory"]["dreaming"]["maxCandidates"], 8);
        let (read, error) = global_memory_settings(&document);
        assert!(error.is_none());
        assert!(read.enabled);
        assert!(read.retrieval.graph_enabled);
    }

    #[test]
    fn model_pickers_filter_local_models() {
        let specs = setting_specs();
        let verifier = specs
            .iter()
            .find(|spec| spec.id == "dreaming_verifier_model")
            .unwrap();
        let models = vec![
            ("openai/gpt-5".to_string(), "GPT-5".to_string()),
            ("llama/qwen".to_string(), "Qwen".to_string()),
        ];
        let choices = model_choices(verifier, "unset", &models);
        let values: Vec<&str> = choices.iter().map(|c| c.value.as_str()).collect();
        assert_eq!(values, ["unset", "llama/qwen"]);
        assert_eq!(
            model_picker_heading(verifier, &choices[..1]),
            "Dreaming verifier model — no local models configured; add one with /llama"
        );
        let dreaming = specs
            .iter()
            .find(|spec| spec.id == "dreaming_model")
            .unwrap();
        let choices = model_choices(dreaming, "x/missing", &models);
        assert_eq!(choices.len(), 4);
        assert_eq!(
            choices[3].description,
            "Current setting (not available in catalog)"
        );
    }
}
