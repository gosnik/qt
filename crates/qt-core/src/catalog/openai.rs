//! OpenAI model discovery (archived `openai_model_discovery.cpp`): models the
//! catalog knows keep their records; other chat-capable ids are added as
//! Responses models with conservative limits.

use anyhow::{Result, bail};
use serde_json::{Value, json};

use super::{ModelCatalog, ModelSpec};

/// Whether an `/models` id is usable through chat / responses.
pub fn is_chat_model_id(id: &str) -> bool {
    let lower = id.to_lowercase();
    if let Some(rest) = lower.strip_prefix("ft:") {
        return rest
            .split_once(':')
            .is_some_and(|(base, _)| is_chat_model_id(base));
    }
    let gpt = lower.starts_with("gpt-");
    let chat_alias = lower == "chat-latest"
        || lower.starts_with("chat-latest-")
        || lower.starts_with("chatgpt-");
    let reasoning = ["o1", "o3", "o4"]
        .iter()
        .any(|p| lower == *p || lower.starts_with(&format!("{p}-")));
    if !gpt && !chat_alias && !reasoning {
        return false;
    }
    const EXCLUDED: &[&str] = &[
        "-image",
        "image-",
        "-audio",
        "audio-",
        "-realtime",
        "realtime-",
        "-transcribe",
        "transcribe-",
        "-tts",
        "tts-",
        "embedding",
        "-instruct",
        "instruct-",
        "-search",
        "search-",
    ];
    !EXCLUDED.iter().any(|token| lower.contains(token))
}

/// Models listed by `GET /models`, resolved against `known` for `provider`.
pub fn models_from_response(
    response: &Value,
    known: &ModelCatalog,
    provider: &str,
) -> Result<Vec<ModelSpec>> {
    if !response.is_object() {
        bail!("OpenAI models response must be an object");
    }
    let Some(data) = response.get("data").and_then(Value::as_array) else {
        bail!("OpenAI models response did not contain a data array");
    };
    let Some(record) = known.find_provider(provider) else {
        bail!("known catalog does not contain the OpenAI provider");
    };
    let mut seen = std::collections::HashSet::new();
    let mut out = Vec::new();
    for item in data {
        let Some(id) = item
            .get("id")
            .and_then(Value::as_str)
            .map(str::trim)
            .filter(|s| !s.is_empty())
        else {
            continue;
        };
        if let Some(existing) = known.find_model(provider, id) {
            if seen.insert(id.to_lowercase()) {
                out.push(existing.clone());
            }
            continue;
        }
        if !is_chat_model_id(id) || !seen.insert(id.to_lowercase()) {
            continue;
        }
        let mut model = ModelSpec {
            provider: record.id.clone(),
            id: id.to_string(),
            name: id.to_string(),
            api: "openai-responses".into(),
            base_url: record.base_url.clone(),
            input: vec!["text".into()],
            context_window: 128_000,
            max_tokens: 16_384,
            ..ModelSpec::default()
        };
        model.metadata.insert("openaiDiscovery".into(), json!(true));
        out.push(model);
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn filters_chat_models() {
        for id in [
            "gpt-5",
            "o4-mini",
            "o1",
            "chatgpt-4o-latest",
            "ft:gpt-4o:org:x",
            "chat-latest",
        ] {
            assert!(is_chat_model_id(id), "{id}");
        }
        for id in [
            "gpt-4o-realtime-preview",
            "text-embedding-3-small",
            "omni-moderation-latest",
            "gpt-image-1",
            "o1x",
            "ft:davinci-002:org:x",
            "gpt-4o-mini-tts",
            "gpt-3.5-turbo-instruct",
        ] {
            assert!(!is_chat_model_id(id), "{id}");
        }
    }

    #[test]
    fn merges_known_and_discovered() {
        let known = ModelCatalog::builtin();
        let response =
            json!({"data":[{"id":"gpt-5"},{"id":"gpt-4.1"},{"id":"GPT-5"},{"id":"whisper-1"}]});
        let models = models_from_response(&response, known, "openai").unwrap();
        assert_eq!(models.len(), 2);
        assert_eq!(models[0].cost.input, 1.25);
        assert_eq!(models[1].id, "gpt-4.1");
        assert_eq!(models[1].api, "openai-responses");
        assert_eq!(models[1].metadata["openaiDiscovery"], true);
        assert!(models_from_response(&json!([]), known, "openai").is_err());
    }
}
