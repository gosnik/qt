//! Effective generation settings, sampler chain plan and GBNF grammar
//! resolution (archived `llama_provider.cpp`).

use serde_json::{Value, json};

use crate::LlamaError;
use crate::settings::{GenerationSettings, GrammarSettings, LlamaSettings, ModelSettings};

pub const DEFAULT_MAX_OUTPUT_TOKENS: i64 = 256;
pub const DEFAULT_TEMPERATURE: f64 = 0.3;
pub const DEFAULT_TOP_P: f64 = 0.9;
pub const DEFAULT_SEED: u32 = u32::MAX;

/// Per-call overrides layered over settings (archived `ProviderOptions` and
/// `CompletionRequest` metadata/temperature/max tokens).
#[derive(Debug, Clone, Default, PartialEq)]
pub struct GenerateOptions {
    /// Options-level metadata (`generation`, `llamaGeneration`, `llama`,
    /// `grammar` keys are honoured).
    pub options_metadata: Value,
    /// Request-level metadata, applied after options metadata.
    pub request_metadata: Value,
    pub temperature: Option<f64>,
    pub max_output_tokens: Option<i64>,
}

fn overlay_generation_metadata(target: &mut GenerationSettings, metadata: &Value) {
    if !metadata.is_object() {
        return;
    }
    let mut overlay = |value: &Value| {
        if value.is_object() {
            *target = target.merged(&GenerationSettings::from_json_lenient(value));
        }
    };
    overlay(metadata);
    for key in ["generation", "llamaGeneration"] {
        if let Some(v) = metadata.get(key) {
            overlay(v);
        }
    }
    if let Some(llama) = metadata.get("llama").filter(|v| v.is_object()) {
        overlay(llama);
        if let Some(g) = llama.get("generation") {
            overlay(g);
        }
    }
}

/// settings.generation ← model.generation ← options metadata ← request
/// metadata ← explicit temperature / max tokens, with default temperature
/// 0.3 and top-p 0.9.
pub fn effective_generation(
    settings: &LlamaSettings,
    model: &ModelSettings,
    options: &GenerateOptions,
) -> GenerationSettings {
    let mut result = settings.generation.merged(&model.generation);
    overlay_generation_metadata(&mut result, &options.options_metadata);
    overlay_generation_metadata(&mut result, &options.request_metadata);
    if options.temperature.is_some() {
        result.temperature = options.temperature;
    }
    if options.max_output_tokens.is_some() {
        result.max_output_tokens = options.max_output_tokens;
    }
    result.temperature.get_or_insert(DEFAULT_TEMPERATURE);
    result.top_p.get_or_insert(DEFAULT_TOP_P);
    result
}

/// Output token limit (default 256).
pub fn resolved_max_output_tokens(settings: &GenerationSettings) -> i64 {
    settings
        .max_output_tokens
        .unwrap_or(DEFAULT_MAX_OUTPUT_TOKENS)
}

/// One sampler in the llama.cpp sampler chain.
#[derive(Debug, Clone, PartialEq)]
pub enum SamplerStep {
    Grammar {
        grammar: String,
        root: String,
        source: String,
    },
    Penalties {
        last_n: i32,
        repeat_penalty: f64,
        frequency_penalty: f64,
        presence_penalty: f64,
    },
    TopK {
        k: i32,
    },
    TopP {
        p: f64,
        min_keep: i32,
    },
    MinP {
        p: f64,
        min_keep: i32,
    },
    Temperature {
        temperature: f64,
    },
    /// Greedy carries the full generation settings as parameters (archived).
    Greedy {
        parameters: Value,
    },
    Dist {
        seed: u32,
    },
}

impl SamplerStep {
    pub fn name(&self) -> &'static str {
        match self {
            Self::Grammar { .. } => "grammar",
            Self::Penalties { .. } => "penalties",
            Self::TopK { .. } => "topK",
            Self::TopP { .. } => "topP",
            Self::MinP { .. } => "minP",
            Self::Temperature { .. } => "temperature",
            Self::Greedy { .. } => "greedy",
            Self::Dist { .. } => "dist",
        }
    }

    pub fn parameters(&self) -> Value {
        match self {
            Self::Grammar {
                grammar,
                root,
                source,
            } => json!({"grammar": grammar, "root": root, "source": source}),
            Self::Penalties {
                last_n,
                repeat_penalty,
                frequency_penalty,
                presence_penalty,
            } => json!({
                "lastN": last_n,
                "repeatPenalty": repeat_penalty,
                "presencePenalty": presence_penalty,
                "frequencyPenalty": frequency_penalty,
            }),
            Self::TopK { k } => json!({"k": k}),
            Self::TopP { p, min_keep } | Self::MinP { p, min_keep } => {
                json!({"p": p, "minKeep": min_keep})
            }
            Self::Temperature { temperature } => json!({"temperature": temperature}),
            Self::Greedy { parameters } => parameters.clone(),
            Self::Dist { seed } => json!({"seed": seed}),
        }
    }

    pub fn to_json(&self) -> Value {
        json!({"name": self.name(), "parameters": self.parameters()})
    }
}

/// Build the sampler chain: penalties, top-k, top-p, min-p, temperature,
/// then greedy (temperature <= 0) or seeded dist.
pub fn sampler_plan(settings: &GenerationSettings) -> Vec<SamplerStep> {
    let mut plan = Vec::new();
    if settings.repeat_penalty.is_some()
        || settings.presence_penalty.is_some()
        || settings.frequency_penalty.is_some()
    {
        plan.push(SamplerStep::Penalties {
            last_n: -1,
            repeat_penalty: settings.repeat_penalty.unwrap_or(1.0),
            presence_penalty: settings.presence_penalty.unwrap_or(0.0),
            frequency_penalty: settings.frequency_penalty.unwrap_or(0.0),
        });
    }
    if let Some(k) = settings.top_k {
        plan.push(SamplerStep::TopK { k });
    }
    let min_keep = settings.min_keep.unwrap_or(1);
    if let Some(p) = settings.top_p {
        plan.push(SamplerStep::TopP { p, min_keep });
    }
    if let Some(p) = settings.min_p {
        plan.push(SamplerStep::MinP { p, min_keep });
    }
    if let Some(temperature) = settings.temperature {
        plan.push(SamplerStep::Temperature { temperature });
    }
    if settings.temperature.is_some_and(|t| t <= 0.0) {
        plan.push(SamplerStep::Greedy {
            parameters: settings.to_json(),
        });
    } else {
        plan.push(SamplerStep::Dist {
            seed: settings.seed.unwrap_or(DEFAULT_SEED),
        });
    }
    plan
}

/// A resolved GBNF grammar (disabled when `grammar` is empty).
#[derive(Debug, Clone, PartialEq)]
pub struct ResolvedGrammar {
    pub enabled: bool,
    pub grammar: String,
    pub root: String,
    pub source: String,
}

impl Default for ResolvedGrammar {
    fn default() -> Self {
        Self {
            enabled: false,
            grammar: String::new(),
            root: "root".into(),
            source: String::new(),
        }
    }
}

fn read_grammar_file(path: &str) -> Result<String, LlamaError> {
    std::fs::read_to_string(path)
        .map_err(|_| LlamaError::backend(format!("failed to read llama grammar file: {path}")))
}

fn inline_grammar(grammar: String, root: &str, source: String) -> ResolvedGrammar {
    ResolvedGrammar {
        enabled: !grammar.trim().is_empty(),
        grammar,
        root: if root.trim().is_empty() {
            "root".into()
        } else {
            root.to_string()
        },
        source,
    }
}

fn named_or_inline(
    settings: &GrammarSettings,
    value: &str,
    root: &str,
    source: String,
) -> Result<ResolvedGrammar, LlamaError> {
    let trimmed = value.trim();
    let lower = trimmed.to_ascii_lowercase();
    if trimmed.is_empty() || lower == "off" || lower == "none" {
        return Ok(ResolvedGrammar::default());
    }
    if let Some(path) = settings.files.as_ref().and_then(|f| f.get(trimmed)) {
        return Ok(inline_grammar(
            read_grammar_file(path)?,
            root,
            format!("file:{path}"),
        ));
    }
    Ok(inline_grammar(trimmed.to_string(), root, source))
}

fn apply_grammar_json(
    target: &mut ResolvedGrammar,
    settings: &GrammarSettings,
    value: &Value,
) -> Result<(), LlamaError> {
    match value {
        Value::Null => Ok(()),
        Value::String(text) => {
            *target = named_or_inline(settings, text, &target.root.clone(), "inline".into())?;
            Ok(())
        }
        Value::Object(o) => {
            if o.get("enabled").and_then(Value::as_bool) == Some(false) {
                *target = ResolvedGrammar::default();
                return Ok(());
            }
            if let Some(root) = o.get("root").and_then(Value::as_str)
                && !root.trim().is_empty()
            {
                target.root = root.trim().to_string();
            }
            if let Some(name) = o.get("name").and_then(Value::as_str) {
                *target = named_or_inline(
                    settings,
                    name,
                    &target.root.clone(),
                    format!("configured:{name}"),
                )?;
            }
            if let Some(file) = o.get("file").and_then(Value::as_str) {
                *target = inline_grammar(
                    read_grammar_file(file)?,
                    &target.root.clone(),
                    format!("file:{file}"),
                );
            }
            for key in ["inline", "grammar", "text"] {
                if let Some(text) = o.get(key).and_then(Value::as_str) {
                    *target =
                        inline_grammar(text.to_string(), &target.root.clone(), "inline".into());
                }
            }
            Ok(())
        }
        _ => Ok(()),
    }
}

fn apply_grammar_metadata(
    target: &mut ResolvedGrammar,
    settings: &GrammarSettings,
    metadata: &Value,
) -> Result<(), LlamaError> {
    if !metadata.is_object() {
        return Ok(());
    }
    if let Some(g) = metadata.get("grammar") {
        apply_grammar_json(target, settings, g)?;
    }
    if let Some(g) = metadata
        .get("llama")
        .filter(|v| v.is_object())
        .and_then(|l| l.get("grammar"))
    {
        apply_grammar_json(target, settings, g)?;
    }
    Ok(())
}

/// Resolve the grammar: settings default (alias or inline), then options and
/// request metadata (`grammar` or `llama.grammar`: string alias/inline, or
/// `{enabled, root, name, file, inline|grammar|text}`).
pub fn resolve_grammar(
    settings: &LlamaSettings,
    options: &GenerateOptions,
) -> Result<ResolvedGrammar, LlamaError> {
    let mut resolved = ResolvedGrammar {
        root: settings
            .grammar
            .root
            .clone()
            .unwrap_or_else(|| "root".into()),
        ..Default::default()
    };
    if let Some(default) = &settings.grammar.default_grammar {
        let root = resolved.root.clone();
        resolved = named_or_inline(&settings.grammar, default, &root, "default".into())?;
    }
    apply_grammar_metadata(&mut resolved, &settings.grammar, &options.options_metadata)?;
    apply_grammar_metadata(&mut resolved, &settings.grammar, &options.request_metadata)?;
    Ok(resolved)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::BTreeMap;

    fn model() -> ModelSettings {
        ModelSettings {
            id: "m".into(),
            name: "M".into(),
            path: "/m.gguf".into(),
            ..Default::default()
        }
    }

    #[test]
    fn effective_generation_layers_in_archived_order() {
        let settings = LlamaSettings {
            generation: GenerationSettings {
                top_k: Some(10),
                max_output_tokens: Some(100),
                ..Default::default()
            },
            ..Default::default()
        };
        let mut m = model();
        m.generation.top_k = Some(20);
        let base = effective_generation(&settings, &m, &GenerateOptions::default());
        assert_eq!(base.top_k, Some(20));
        assert_eq!(base.temperature, Some(DEFAULT_TEMPERATURE));
        assert_eq!(base.top_p, Some(DEFAULT_TOP_P));
        assert_eq!(resolved_max_output_tokens(&base), 100);

        let options = GenerateOptions {
            options_metadata: json!({"generation": {"topK": 30, "minP": 0.1}}),
            request_metadata: json!({"llama": {"generation": {"topK": 40}}, "seed": 7}),
            temperature: Some(0.0),
            max_output_tokens: Some(5),
        };
        let g = effective_generation(&settings, &m, &options);
        assert_eq!(g.top_k, Some(40));
        assert_eq!(g.min_p, Some(0.1));
        assert_eq!(g.seed, Some(7));
        assert_eq!(g.temperature, Some(0.0));
        assert_eq!(g.max_output_tokens, Some(5));
        assert_eq!(
            resolved_max_output_tokens(&GenerationSettings::default()),
            256
        );
    }

    #[test]
    fn sampler_plan_matches_archived_chain() {
        let plan = sampler_plan(&GenerationSettings {
            temperature: Some(0.7),
            top_k: Some(40),
            top_p: Some(0.9),
            min_p: Some(0.05),
            min_keep: Some(2),
            seed: Some(42),
            repeat_penalty: Some(1.1),
            ..Default::default()
        });
        let names: Vec<&str> = plan.iter().map(SamplerStep::name).collect();
        assert_eq!(
            names,
            vec!["penalties", "topK", "topP", "minP", "temperature", "dist"]
        );
        assert_eq!(
            plan[0].parameters(),
            json!({"lastN": -1, "repeatPenalty": 1.1, "presencePenalty": 0.0, "frequencyPenalty": 0.0})
        );
        assert_eq!(plan[2].parameters(), json!({"p": 0.9, "minKeep": 2}));
        assert_eq!(plan[5], SamplerStep::Dist { seed: 42 });

        let greedy = sampler_plan(&GenerationSettings {
            temperature: Some(0.0),
            ..Default::default()
        });
        assert_eq!(greedy.len(), 2);
        assert_eq!(greedy[1].to_json()["name"], "greedy");
        assert_eq!(greedy[1].parameters(), json!({"temperature": 0.0}));
        let unseeded = sampler_plan(&GenerationSettings::default());
        assert_eq!(unseeded, vec![SamplerStep::Dist { seed: DEFAULT_SEED }]);
    }

    #[test]
    fn grammar_resolution_from_default_alias_file_and_inline() {
        let dir = tempfile::tempdir().unwrap();
        let file = dir.path().join("json.gbnf");
        std::fs::write(&file, "root ::= \"{\" \"}\"").unwrap();
        let file_str = file.to_string_lossy().into_owned();
        let mut files = BTreeMap::new();
        files.insert("json".to_string(), file_str.clone());
        let mut settings = LlamaSettings::default();
        settings.grammar.files = Some(files);
        settings.grammar.default_grammar = Some("json".into());
        settings.grammar.root = Some("start".into());

        let g = resolve_grammar(&settings, &GenerateOptions::default()).unwrap();
        assert!(g.enabled);
        assert_eq!(g.root, "start");
        assert_eq!(g.source, format!("file:{file_str}"));
        assert_eq!(g.grammar, "root ::= \"{\" \"}\"");

        let off = resolve_grammar(
            &settings,
            &GenerateOptions {
                request_metadata: json!({"grammar": "off"}),
                ..Default::default()
            },
        )
        .unwrap();
        assert!(!off.enabled);

        let inline = resolve_grammar(
            &settings,
            &GenerateOptions {
                request_metadata: json!({"llama": {"grammar": {"root": "r", "inline": "r ::= \"a\""}}}),
                ..Default::default()
            },
        )
        .unwrap();
        assert_eq!(
            (inline.root.as_str(), inline.source.as_str()),
            ("r", "inline")
        );
        assert_eq!(inline.grammar, "r ::= \"a\"");

        let named = resolve_grammar(
            &LlamaSettings::default(),
            &GenerateOptions {
                options_metadata: json!({"grammar": {"name": "root ::= \"x\""}}),
                ..Default::default()
            },
        )
        .unwrap();
        assert_eq!(named.source, "configured:root ::= \"x\"");

        let disabled = resolve_grammar(
            &settings,
            &GenerateOptions {
                options_metadata: json!({"grammar": {"enabled": false}}),
                ..Default::default()
            },
        )
        .unwrap();
        assert!(!disabled.enabled);

        let missing = resolve_grammar(
            &LlamaSettings::default(),
            &GenerateOptions {
                request_metadata: json!({"grammar": {"file": "/nonexistent.gbnf"}}),
                ..Default::default()
            },
        )
        .unwrap_err();
        assert_eq!(
            missing.to_string(),
            "failed to read llama grammar file: /nonexistent.gbnf"
        );
    }
}
