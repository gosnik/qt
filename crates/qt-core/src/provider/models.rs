//! Model discovery per provider.

use anyhow::{Result, bail};
use serde_json::Value;

use super::{ProviderConfig, ProviderKind, anthropic, openai};
use crate::config::Settings;

/// A model offered by a provider.
#[derive(Debug, Clone, PartialEq)]
pub struct ModelInfo {
    pub provider: String,
    pub id: String,
    pub name: String,
    pub context_window: Option<u64>,
}

impl ModelInfo {
    pub fn model_ref(&self) -> super::ModelRef {
        super::ModelRef::new(self.provider.clone(), self.id.clone())
    }
}

/// List tool-capable models for `provider`.
pub async fn list_models(
    http: &reqwest::Client,
    provider: &ProviderConfig,
    settings: &Settings,
) -> Result<Vec<ModelInfo>> {
    if provider.needs_api_key() && provider.api_key.is_none() {
        return Ok(Vec::new());
    }
    let mut models = match provider.kind {
        ProviderKind::Anthropic => {
            let url = format!("{}/v1/models?limit=1000", provider.base_url);
            let body = get_json(anthropic::authorize(http.get(url), provider)).await?;
            items(&body, "data")
                .filter_map(|m| {
                    let id = m.get("id")?.as_str()?.to_string();
                    let name = m
                        .get("display_name")
                        .and_then(Value::as_str)
                        .unwrap_or(&id)
                        .to_string();
                    Some(ModelInfo {
                        provider: provider.id.clone(),
                        name,
                        id,
                        context_window: Some(200_000),
                    })
                })
                .collect::<Vec<_>>()
        }
        ProviderKind::OpenAi => {
            let body = get_json(openai::authorize(
                http.get(format!("{}/models", provider.base_url)),
                provider,
            ))
            .await?;
            items(&body, "data")
                .filter_map(|m| m.get("id")?.as_str().map(str::to_string))
                .filter(|id| is_openai_chat_model(id))
                .map(|id| ModelInfo {
                    provider: provider.id.clone(),
                    name: id.clone(),
                    id,
                    context_window: None,
                })
                .collect()
        }
        ProviderKind::OpenRouter => {
            let body = get_json(openai::authorize(
                http.get(format!("{}/models", provider.base_url)),
                provider,
            ))
            .await?;
            items(&body, "data")
                .filter(|m| {
                    m.get("supported_parameters")
                        .and_then(Value::as_array)
                        .is_none_or(|p| p.iter().any(|v| v.as_str() == Some("tools")))
                })
                .filter_map(|m| {
                    let id = m.get("id")?.as_str()?.to_string();
                    let name = m
                        .get("name")
                        .and_then(Value::as_str)
                        .unwrap_or(&id)
                        .to_string();
                    Some(ModelInfo {
                        provider: provider.id.clone(),
                        name,
                        id,
                        context_window: m.get("context_length").and_then(Value::as_u64),
                    })
                })
                .collect()
        }
        ProviderKind::Ollama => {
            let mut names: Vec<String> = settings
                .ollama_instances()
                .into_iter()
                .find(|i| i.base_url == provider.base_url)
                .map(|i| i.models)
                .unwrap_or_default();
            if let Ok(body) = get_json(http.get(format!("{}/api/tags", provider.base_url))).await {
                for name in
                    items(&body, "models").filter_map(|m| m.get("name").and_then(Value::as_str))
                {
                    if !names.iter().any(|n| n == name) {
                        names.push(name.to_string());
                    }
                }
            } else if names.is_empty() {
                bail!("Ollama is not reachable at {}", provider.base_url);
            }
            names
                .into_iter()
                .map(|id| ModelInfo {
                    provider: provider.id.clone(),
                    name: id.clone(),
                    id,
                    context_window: None,
                })
                .collect()
        }
    };
    models.sort_by_key(|a| a.name.to_lowercase());
    Ok(models)
}

fn is_openai_chat_model(id: &str) -> bool {
    let chat_family = id.starts_with("gpt-")
        || id.starts_with("chatgpt-")
        || id.starts_with("codex-")
        || (id.starts_with('o') && id.chars().nth(1).is_some_and(|c| c.is_ascii_digit()));
    let excluded = [
        "audio",
        "realtime",
        "tts",
        "transcribe",
        "image",
        "embedding",
        "search",
        "instruct",
        "moderation",
    ];
    chat_family && !excluded.iter().any(|x| id.contains(x))
}

async fn get_json(builder: reqwest::RequestBuilder) -> Result<Value> {
    let response = builder
        .timeout(std::time::Duration::from_secs(20))
        .send()
        .await?;
    if !response.status().is_success() {
        return Err(super::http_error(response).await);
    }
    Ok(response.json().await?)
}

fn items<'a>(body: &'a Value, key: &str) -> impl Iterator<Item = &'a Value> {
    body.get(key)
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn filters_openai_models() {
        assert!(is_openai_chat_model("gpt-5"));
        assert!(is_openai_chat_model("o4-mini"));
        assert!(!is_openai_chat_model("gpt-4o-realtime-preview"));
        assert!(!is_openai_chat_model("text-embedding-3-small"));
        assert!(!is_openai_chat_model("omni-moderation-latest"));
    }
}
