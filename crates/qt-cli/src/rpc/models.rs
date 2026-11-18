//! Model listing, selection and thinking levels for RPC (Pi
//! `modelRegistry.getAvailable`, `cycleModel`, `cycleThinkingLevel`).

use serde_json::{Value, json};

use qt_core::engine::Engine;
use qt_core::provider::{ModelInfo, ModelRef, ThinkingLevel};

/// Models of providers with usable credentials: catalog records, models
/// discovered earlier and, with `live`, a fresh discovery (skipped offline).
pub fn available_models(engine: &Engine, live: bool) -> Vec<ModelInfo> {
    if live && !engine.is_offline() {
        let job = engine.list_models();
        let _ = engine.block_on(job);
    }
    let ready: Vec<_> = engine
        .providers()
        .into_iter()
        .filter(|p| p.is_ready())
        .collect();
    let mut models: Vec<ModelInfo> = Vec::new();
    let mut push = |info: ModelInfo| {
        if !models
            .iter()
            .any(|m| m.provider == info.provider && m.id.eq_ignore_ascii_case(&info.id))
        {
            models.push(info);
        }
    };
    for provider in &ready {
        for spec in provider.models.iter() {
            push(ModelInfo::from_spec(&provider.id, spec));
        }
    }
    for info in engine.known_models() {
        if ready.iter().any(|p| p.id == info.provider) {
            push(info);
        }
    }
    models
}

/// Find `provider/id` among the available models, discovering live models
/// when it is not known yet.
pub fn find_available(engine: &Engine, provider: &str, id: &str) -> Option<ModelInfo> {
    let find = |models: Vec<ModelInfo>| {
        models
            .into_iter()
            .find(|m| m.provider == provider && m.id == id)
    };
    find(available_models(engine, false)).or_else(|| find(available_models(engine, true)))
}

/// The model in Pi's `Model` shape (`id`, `name`, `api`, `provider`,
/// `baseUrl`, `reasoning`, `input`, `cost`, `contextWindow`, `maxTokens`)
/// plus the archived `displayName` and `vision`.
pub fn model_json(engine: &Engine, model: &ModelRef) -> Value {
    let provider = engine.provider(&model.provider);
    let spec = provider.as_ref().map(|p| p.model_spec(&model.id));
    let info = engine.model_info(model);
    let name = info
        .as_ref()
        .map(|i| i.name.clone())
        .filter(|n| !n.is_empty())
        .or_else(|| spec.as_ref().map(|s| s.display_name().to_string()))
        .unwrap_or_else(|| model.id.clone());
    let api = spec
        .as_ref()
        .map(|s| s.api.clone())
        .filter(|a| !a.is_empty())
        .or_else(|| provider.as_ref().map(|p| p.kind.api_name().to_string()))
        .unwrap_or_default();
    let base_url = spec
        .as_ref()
        .map(|s| s.base_url.clone())
        .filter(|u| !u.is_empty())
        .or_else(|| provider.as_ref().map(|p| p.base_url.clone()))
        .unwrap_or_default();
    let reasoning = info.as_ref().map(|i| i.reasoning).unwrap_or(false)
        || spec.as_ref().is_some_and(|s| s.reasoning);
    let vision = info.as_ref().map(|i| i.vision).unwrap_or(false)
        || spec.as_ref().is_some_and(|s| s.supports_images());
    let context_window = info
        .as_ref()
        .and_then(|i| i.context_window)
        .or_else(|| spec.as_ref().map(|s| s.context_window).filter(|w| *w > 0));
    let max_tokens = info
        .as_ref()
        .and_then(|i| i.max_tokens)
        .or_else(|| spec.as_ref().map(|s| s.max_tokens).filter(|t| *t > 0));
    let input = if vision {
        json!(["text", "image"])
    } else {
        json!(["text"])
    };
    let mut out = json!({
        "id": model.id,
        "name": name,
        "displayName": name,
        "api": api,
        "provider": model.provider,
        "baseUrl": base_url,
        "reasoning": reasoning,
        "vision": vision,
        "input": input,
    });
    if let Some(spec) = &spec {
        out["cost"] = spec.cost.to_json();
    }
    if let Some(window) = context_window {
        out["contextWindow"] = json!(window);
    }
    if let Some(tokens) = max_tokens {
        out["maxTokens"] = json!(tokens);
    }
    out
}

/// Thinking levels `model` accepts (`[off]` without reasoning).
pub fn thinking_levels(engine: &Engine, model: &ModelRef) -> Vec<ThinkingLevel> {
    engine
        .model_info(model)
        .map(|i| i.thinking_levels)
        .filter(|l| !l.is_empty())
        .or_else(|| {
            engine
                .provider(&model.provider)
                .map(|p| p.model_spec(&model.id).thinking_levels())
        })
        .unwrap_or_else(|| vec![ThinkingLevel::Off])
}

/// Whether `model` supports reasoning (Pi `supportsThinking`).
pub fn supports_thinking(engine: &Engine, model: &ModelRef) -> bool {
    thinking_levels(engine, model)
        .iter()
        .any(|l| *l != ThinkingLevel::Off)
}

/// `level` clamped to the levels `model` accepts (Pi `clampThinkingLevel`:
/// nearest supported level, searching upwards first).
pub fn clamp_thinking(engine: &Engine, model: &ModelRef, level: ThinkingLevel) -> ThinkingLevel {
    let available = thinking_levels(engine, model);
    if available.contains(&level) {
        return level;
    }
    let all = ThinkingLevel::ALL;
    let index = all.iter().position(|l| *l == level).unwrap_or(0);
    all[index..]
        .iter()
        .chain(all[..index].iter().rev())
        .find(|l| available.contains(l))
        .copied()
        .unwrap_or(ThinkingLevel::Off)
}

/// The level after `current` in the model's cycle, `None` without
/// reasoning support.
pub fn next_thinking_level(
    engine: &Engine,
    model: &ModelRef,
    current: ThinkingLevel,
) -> Option<ThinkingLevel> {
    if !supports_thinking(engine, model) {
        return None;
    }
    let levels = thinking_levels(engine, model);
    let next = match levels.iter().position(|l| *l == current) {
        Some(index) => levels[(index + 1) % levels.len()],
        None => levels[0],
    };
    Some(next)
}

#[cfg(test)]
mod tests {
    use super::*;
    use qt_core::config::AgentHome;

    fn engine(settings: Value) -> (tempfile::TempDir, Engine) {
        let dir = tempfile::tempdir().unwrap();
        let home = dir.path().join("home");
        std::fs::create_dir_all(&home).unwrap();
        std::fs::write(home.join("settings.json"), settings.to_string()).unwrap();
        let engine = Engine::builder(AgentHome::new(home), dir.path())
            .offline(true)
            .build()
            .unwrap();
        (dir, engine)
    }

    #[test]
    fn ready_providers_only_and_model_shape() {
        let (_dir, engine) = engine(json!({
            "ollama": {"instances": [{"id": "box", "baseUrl": "http://127.0.0.1:9"}]},
        }));
        let models = available_models(&engine, true);
        // Providers that need a key and have none are not offered.
        assert!(
            models
                .iter()
                .all(|m| engine.provider(&m.provider).is_some_and(|p| p.is_ready()))
        );
        let model = ModelRef::new("ollama:box", "llama3");
        let json = model_json(&engine, &model);
        assert_eq!(json["provider"], "ollama:box");
        assert_eq!(json["id"], "llama3");
        assert_eq!(json["name"], "llama3");
        assert_eq!(json["displayName"], "llama3");
        let expected = if json["vision"] == true {
            json!(["text", "image"])
        } else {
            json!(["text"])
        };
        assert_eq!(json["input"], expected);
        assert!(json["api"].as_str().is_some_and(|a| !a.is_empty()));
    }

    #[test]
    fn thinking_cycle_and_clamp() {
        let (_dir, engine) = engine(json!({}));
        let anthropic = engine
            .provider("anthropic")
            .and_then(|p| p.models.iter().find(|m| m.reasoning).cloned())
            .expect("a reasoning catalog model");
        let model = ModelRef::new("anthropic", anthropic.id.clone());
        let levels = thinking_levels(&engine, &model);
        assert_eq!(levels[0], ThinkingLevel::Off);
        assert!(supports_thinking(&engine, &model));
        let last = *levels.last().unwrap();
        assert_eq!(
            next_thinking_level(&engine, &model, last),
            Some(ThinkingLevel::Off)
        );
        assert_eq!(
            next_thinking_level(&engine, &model, ThinkingLevel::Off),
            Some(levels[1])
        );
        let plain = ModelRef::new("nowhere", "m");
        assert_eq!(
            next_thinking_level(&engine, &plain, ThinkingLevel::Off),
            None
        );
        assert_eq!(
            clamp_thinking(&engine, &plain, ThinkingLevel::High),
            ThinkingLevel::Off
        );
        assert_eq!(
            clamp_thinking(&engine, &model, ThinkingLevel::Low),
            ThinkingLevel::Low
        );
    }
}
