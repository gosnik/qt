//! Model listing per provider: catalog records merged with live discovery
//! (Anthropic and OpenAI `/models`, OpenRouter metadata with its disk cache,
//! Ollama `/api/tags` + `/api/show`).

use anyhow::{Result, bail};
use serde_json::Value;

use super::{ProviderConfig, anthropic, openai};
use crate::catalog::{ModelCatalog, ModelSpec};
use crate::config::{AgentHome, Settings};
use crate::provider::ThinkingLevel;

/// A model offered by a provider.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct ModelInfo {
    pub provider: String,
    pub id: String,
    pub name: String,
    pub context_window: Option<u64>,
    pub max_tokens: Option<u64>,
    pub reasoning: bool,
    pub vision: bool,
    /// Thinking levels the model accepts.
    pub thinking_levels: Vec<ThinkingLevel>,
}

impl ModelInfo {
    pub fn model_ref(&self) -> super::ModelRef {
        super::ModelRef::new(self.provider.clone(), self.id.clone())
    }

    pub fn from_spec(provider: &str, spec: &ModelSpec) -> Self {
        Self {
            provider: provider.to_string(),
            id: spec.id.clone(),
            name: spec.display_name().to_string(),
            context_window: (spec.context_window > 0).then_some(spec.context_window),
            max_tokens: (spec.max_tokens > 0).then_some(spec.max_tokens),
            reasoning: spec.reasoning,
            vision: spec.supports_images(),
            thinking_levels: spec.thinking_levels(),
        }
    }
}

fn supported_api(spec: &ModelSpec) -> bool {
    spec.api.is_empty() || super::ProviderKind::from_api(&spec.api).is_some()
}

/// Merge `extra` into `base`, keeping the first record per id.
fn merge(base: &mut Vec<ModelSpec>, extra: impl IntoIterator<Item = ModelSpec>) {
    for model in extra {
        if !base.iter().any(|m| m.id.eq_ignore_ascii_case(&model.id)) {
            base.push(model);
        }
    }
}

/// List tool-capable models for `provider`.
pub async fn list_models(
    http: &reqwest::Client,
    provider: &ProviderConfig,
    settings: &Settings,
) -> Result<Vec<ModelInfo>> {
    if let Some(handle) = &provider.backend {
        return handle.backend().list_models(provider, settings).await;
    }
    if provider.needs_api_key() && provider.api_key.is_none() {
        return Ok(Vec::new());
    }
    let mut models: Vec<ModelSpec> = provider.models.iter().cloned().collect();
    let discovered: Result<Vec<ModelSpec>> = match provider.id.as_str() {
        "anthropic" => discover_anthropic(http, provider).await,
        "openai" => discover_openai(http, provider).await,
        "openrouter" => {
            let home = settings.home().cloned().unwrap_or_else(AgentHome::from_env);
            crate::catalog::openrouter::discover(
                http,
                &home,
                provider.api_key.as_deref(),
                &provider.base_url,
                true,
            )
            .await
            .map(|c| c.models)
        }
        id if crate::ollama::is_ollama_provider_id(id) => {
            discover_ollama(http, provider, settings).await
        }
        _ => Ok(Vec::new()),
    };
    match discovered {
        Ok(found) if provider.id == "openrouter" => {
            // Fresh OpenRouter metadata replaces cached records.
            let mut fresh = found;
            merge(&mut fresh, models);
            models = fresh;
        }
        Ok(found) => merge(&mut models, found),
        Err(err) if models.is_empty() => return Err(err),
        Err(_) => {}
    }
    let mut infos: Vec<ModelInfo> = models
        .iter()
        .filter(|m| supported_api(m))
        .filter(|m| provider.id != "openrouter" || crate::catalog::openrouter::supports_tools(m))
        .map(|m| ModelInfo::from_spec(&provider.id, m))
        .collect();
    infos.sort_by_key(|a| a.name.to_lowercase());
    Ok(infos)
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

async fn discover_anthropic(
    http: &reqwest::Client,
    provider: &ProviderConfig,
) -> Result<Vec<ModelSpec>> {
    let url = format!("{}/v1/models?limit=1000", provider.base_url);
    let body = get_json(anthropic::authorize(http.get(url), provider)).await?;
    Ok(items(&body, "data")
        .filter_map(|m| {
            let id = m.get("id")?.as_str()?.to_string();
            let mut spec = provider.model_spec(&id);
            if let Some(name) = m.get("display_name").and_then(Value::as_str)
                && spec.name == spec.id
            {
                spec.name = name.to_string();
            }
            if spec.max_tokens == 16_384 && spec.context_window == 128_000 {
                // Unknown Claude models: Claude limits are at least these.
                spec.context_window = 200_000;
                spec.max_tokens = 64_000;
            }
            Some(spec)
        })
        .collect())
}

async fn discover_openai(
    http: &reqwest::Client,
    provider: &ProviderConfig,
) -> Result<Vec<ModelSpec>> {
    let body = get_json(openai::authorize(
        http.get(format!("{}/models", provider.base_url)),
        provider,
    ))
    .await?;
    let mut known = ModelCatalog::builtin().clone();
    known.remove_models_of("openai");
    known.models.extend(provider.models.iter().cloned());
    if let Some(p) = known.providers.iter_mut().find(|p| p.id == "openai") {
        p.base_url = provider.base_url.clone();
    }
    crate::catalog::openai::models_from_response(&body, &known, "openai")
}

async fn discover_ollama(
    http: &reqwest::Client,
    provider: &ProviderConfig,
    settings: &Settings,
) -> Result<Vec<ModelSpec>> {
    let instance_id = provider.id.trim_start_matches("ollama:");
    let instance = settings
        .ollama_instances()
        .into_iter()
        .find(|i| i.id == instance_id)
        .unwrap_or_else(|| {
            let mut i = crate::ollama::OllamaInstance::local_default();
            i.base_url = crate::ollama::normalize_base_url(&provider.base_url);
            i.api_key = provider.api_key.clone();
            i
        });
    let client = crate::ollama::OllamaClient::new(http, &instance);
    let found = match client.discover_models().await {
        Ok(found) => found,
        Err(_) if instance.models.is_empty() && provider.models.is_empty() => {
            bail!("Ollama is not reachable at {}", instance.base_url)
        }
        Err(err) => return Err(err),
    };
    let mut live = instance.clone();
    live.models = found;
    Ok(crate::ollama::catalog_from_instances(&[live])
        .models
        .into_iter()
        .map(|m| ModelSpec {
            provider: provider.id.clone(),
            ..m
        })
        .collect())
}

/// Query an instance's models and persist them under `ollama.instances`
/// (the archived `/ollama models` refresh). Returns the model count.
pub async fn refresh_ollama_instance(
    http: &reqwest::Client,
    settings: &mut Settings,
    instance_id: &str,
) -> Result<usize> {
    let mut instance = settings
        .try_ollama_settings()?
        .into_iter()
        .find(|i| i.id.eq_ignore_ascii_case(instance_id))
        .ok_or_else(|| anyhow::anyhow!("unknown Ollama instance: {instance_id}"))?;
    instance.models = crate::ollama::OllamaClient::new(http, &instance)
        .discover_models()
        .await?;
    let count = instance.models.len();
    settings.update_ollama_instance(instance)?;
    if let Some(home) = settings.home().cloned() {
        settings.save(&home)?;
    }
    Ok(count)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::provider::test_server::{MockResponse, serve};
    use std::sync::Arc;

    #[tokio::test]
    async fn lists_catalog_and_discovered_models() {
        let (base, _) = serve(vec![
            MockResponse::json(
                200,
                r#"{"data":[{"id":"gpt-5"},{"id":"gpt-4.1-mini"},{"id":"dall-e-3"}]}"#,
            ),
            MockResponse::json(500, "x"),
        ])
        .await;
        let models: Vec<ModelSpec> = ModelCatalog::builtin()
            .models_for("openai")
            .cloned()
            .collect();
        let provider = ProviderConfig {
            id: "openai".into(),
            base_url: base,
            api_key: Some("k".into()),
            models: Arc::new(models.clone()),
            ..Default::default()
        };
        let http = reqwest::Client::new();
        let listed = list_models(&http, &provider, &Settings::default())
            .await
            .unwrap();
        assert_eq!(listed.len(), models.len() + 1);
        let gpt5 = listed.iter().find(|m| m.id == "gpt-5").unwrap();
        assert_eq!(gpt5.context_window, Some(400_000));
        assert!(!gpt5.thinking_levels.contains(&ThinkingLevel::Off));
        assert!(listed.iter().any(|m| m.id == "gpt-4.1-mini"));
        // Discovery failures fall back to the catalog.
        assert_eq!(
            list_models(&http, &provider, &Settings::default())
                .await
                .unwrap()
                .len(),
            models.len()
        );
        let keyless = ProviderConfig {
            api_key: None,
            ..provider
        };
        assert!(
            list_models(&http, &keyless, &Settings::default())
                .await
                .unwrap()
                .is_empty()
        );
    }

    #[tokio::test]
    async fn ollama_refresh_persists_models() {
        let dir = tempfile::tempdir().unwrap();
        let home = AgentHome::new(dir.path());
        let (base, _) = serve(vec![
            MockResponse::json(200, r#"{"models":[{"name":"llama3"}]}"#),
            MockResponse::json(200, r#"{"capabilities":["completion","vision"]}"#),
        ])
        .await;
        let mut settings = Settings::load(&home).unwrap();
        settings
            .add_ollama_instance(crate::ollama::OllamaInstance::new("box", &base))
            .unwrap();
        assert_eq!(
            refresh_ollama_instance(&reqwest::Client::new(), &mut settings, "BOX")
                .await
                .unwrap(),
            1
        );
        let again = Settings::load(&home).unwrap();
        let model = &again.ollama_settings()[0].models[0];
        assert_eq!(model.name, "llama3");
        assert!(model.vision());
    }
}
