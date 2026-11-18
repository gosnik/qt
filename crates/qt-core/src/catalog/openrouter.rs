//! OpenRouter model metadata (pricing, limits, modalities), the
//! `openrouter_models.json` disk cache (7-day freshness for network
//! fallback, stale entries allowed at startup), the credits endpoint and
//! image generation (archived `openrouter_model_discovery.cpp`, Pi
//! `providers/images/openrouter.ts`).

use std::time::Duration;

use anyhow::{Result, anyhow, bail};
use serde_json::{Map, Value, json};

use super::{Cost, ImageModelSpec, ModelCatalog, ModelSpec};
use crate::config::{AgentHome, write_json_atomic};
use crate::message::{ContentBlock, Usage};

pub const BASE_URL: &str = "https://openrouter.ai/api/v1";
const DEFAULT_TOKEN_LIMIT: u64 = 4096;
const CACHE_MAX_AGE_HOURS: i64 = 168;

fn number_or_string(object: &Value, key: &str) -> Option<f64> {
    match object.get(key)? {
        Value::Number(n) => n.as_f64(),
        Value::String(s) => s.trim().parse().ok(),
        _ => None,
    }
}

fn positive(object: &Value, key: &str) -> Option<u64> {
    match object.get(key)? {
        Value::Number(n) => n
            .as_u64()
            .or_else(|| {
                n.as_f64()
                    .filter(|f| f.fract() == 0.0 && *f >= 1.0)
                    .map(|f| f as u64)
            })
            .filter(|v| *v > 0),
        _ => None,
    }
}

fn contains(array: Option<&Value>, needle: &str) -> bool {
    array
        .and_then(Value::as_array)
        .is_some_and(|a| a.iter().any(|v| v.as_str() == Some(needle)))
}

/// Convert one `GET /models` entry; `None` for models without text output.
pub fn model_from_json(model: &Value) -> Option<ModelSpec> {
    let id = model
        .get("id")
        .and_then(Value::as_str)
        .filter(|s| !s.is_empty())?;
    let architecture = model.get("architecture");
    let outputs = architecture.and_then(|a| a.get("output_modalities"));
    if outputs
        .and_then(Value::as_array)
        .is_some_and(|o| !o.is_empty())
        && !contains(outputs, "text")
    {
        return None;
    }
    let name = model
        .get("name")
        .and_then(Value::as_str)
        .filter(|s| !s.is_empty())
        .map(str::to_string)
        .unwrap_or_else(|| format!("OpenRouter {id}"));
    let model_context = positive(model, "context_length").unwrap_or(DEFAULT_TOKEN_LIMIT);
    let top = model.get("top_provider").cloned().unwrap_or(Value::Null);
    let provider_context = positive(&top, "context_length");
    let context_window = provider_context.map_or(model_context, |p| p.min(model_context));
    let max_tokens = positive(&top, "max_completion_tokens")
        .unwrap_or(DEFAULT_TOKEN_LIMIT)
        .min(context_window);
    let pricing = model.get("pricing").cloned().unwrap_or(Value::Null);
    let per_million = |k: &str| number_or_string(&pricing, k).unwrap_or(0.0) * 1_000_000.0;
    let params = model.get("supported_parameters");
    let mut input = vec!["text".to_string()];
    if contains(
        architecture.and_then(|a| a.get("input_modalities")),
        "image",
    ) {
        input.push("image".into());
    }
    let mut metadata = Map::new();
    metadata.insert("modelContextWindow".into(), json!(model_context));
    if let Some(p) = provider_context {
        metadata.insert("providerContextWindow".into(), json!(p));
    }
    if let Some(p) = params.filter(|p| p.is_array()) {
        metadata.insert("supportedParameters".into(), p.clone());
    }
    Some(ModelSpec {
        provider: "openrouter".into(),
        id: id.to_string(),
        name,
        api: "openai-completions".into(),
        base_url: BASE_URL.into(),
        reasoning: ["reasoning", "include_reasoning", "reasoning_effort"]
            .iter()
            .any(|p| contains(params, p)),
        input,
        cost: Cost {
            input: per_million("prompt"),
            output: per_million("completion"),
            cache_read: per_million("input_cache_read"),
            cache_write: per_million("input_cache_write"),
        },
        context_window,
        max_tokens,
        metadata,
        ..ModelSpec::default()
    })
}

/// Whether the model accepts tools (unknown when OpenRouter did not say).
pub fn supports_tools(model: &ModelSpec) -> bool {
    model
        .metadata
        .get("supportedParameters")
        .and_then(Value::as_array)
        .is_none_or(|p| p.iter().any(|v| v.as_str() == Some("tools")))
}

/// Parse a whole `GET /models` response.
pub fn catalog_from_response(response: &Value) -> Result<ModelCatalog> {
    if !response.is_object() {
        bail!("OpenRouter models response must be an object");
    }
    let Some(data) = response.get("data").and_then(Value::as_array) else {
        bail!("OpenRouter models response did not contain a data array");
    };
    let mut catalog = ModelCatalog::default();
    if let Some(p) = ModelCatalog::builtin().find_provider("openrouter") {
        catalog.providers.push(p.clone());
    }
    catalog.models = data.iter().filter_map(model_from_json).collect();
    catalog.normalize();
    Ok(catalog)
}

fn cost_json(cost: &Cost) -> Value {
    json!({
        "input_usd_per_million": cost.input,
        "output_usd_per_million": cost.output,
        "cache_read_usd_per_million": cost.cache_read,
        "cache_write_usd_per_million": cost.cache_write,
    })
}

/// Write the cache (archived qt format, `timestamp` in Unix seconds).
pub fn save_cache(home: &AgentHome, catalog: &ModelCatalog) -> Result<()> {
    let models: Vec<Value> = catalog
        .models
        .iter()
        .filter(|m| m.provider == "openrouter")
        .map(|m| {
            let mut metadata = m.metadata.clone();
            metadata.insert("input".into(), json!(m.input));
            metadata.insert("cost".into(), cost_json(&m.cost));
            metadata.insert("contextWindow".into(), json!(m.context_window));
            metadata.insert("maxTokens".into(), json!(m.max_tokens));
            metadata.insert("baseUrl".into(), json!(m.base_url));
            json!({
                "id": m.id,
                "display_name": m.name,
                "reasoning": m.reasoning,
                "vision": m.supports_images(),
                "metadata": metadata,
            })
        })
        .collect();
    let value = json!({ "timestamp": chrono::Utc::now().timestamp(), "models": models });
    write_json_atomic(&home.openrouter_cache_path(), &value, false)
}

fn model_from_cache(entry: &Value) -> Option<ModelSpec> {
    let id = entry
        .get("id")
        .and_then(Value::as_str)
        .filter(|s| !s.is_empty())?;
    let metadata = entry.get("metadata").cloned().unwrap_or_else(|| json!({}));
    let cached_context = positive(&metadata, "contextWindow").unwrap_or(DEFAULT_TOKEN_LIMIT);
    let model_context = positive(&metadata, "modelContextWindow").unwrap_or(cached_context);
    let provider_context = positive(&metadata, "providerContextWindow");
    let mut context_window = cached_context.min(model_context);
    if let Some(p) = provider_context {
        context_window = context_window.min(p);
    }
    let max_tokens = positive(&metadata, "maxTokens")
        .unwrap_or(DEFAULT_TOKEN_LIMIT)
        .min(context_window);
    let mut extra = metadata.as_object().cloned().unwrap_or_default();
    for key in ["input", "cost", "contextWindow", "maxTokens", "baseUrl"] {
        extra.remove(key);
    }
    extra.insert("modelContextWindow".into(), json!(model_context));
    let vision = entry
        .get("vision")
        .and_then(Value::as_bool)
        .unwrap_or(false);
    let input = metadata
        .get("input")
        .and_then(Value::as_array)
        .map(|a| {
            a.iter()
                .filter_map(Value::as_str)
                .map(str::to_string)
                .collect::<Vec<_>>()
        })
        .filter(|a| !a.is_empty())
        .unwrap_or_else(|| {
            if vision {
                vec!["text".into(), "image".into()]
            } else {
                vec!["text".into()]
            }
        });
    Some(ModelSpec {
        provider: "openrouter".into(),
        id: id.to_string(),
        name: entry
            .get("display_name")
            .and_then(Value::as_str)
            .filter(|s| !s.is_empty())
            .map(str::to_string)
            .unwrap_or_else(|| format!("OpenRouter {id}")),
        api: "openai-completions".into(),
        base_url: metadata
            .get("baseUrl")
            .and_then(Value::as_str)
            .filter(|s| !s.is_empty())
            .unwrap_or(BASE_URL)
            .to_string(),
        reasoning: entry
            .get("reasoning")
            .and_then(Value::as_bool)
            .unwrap_or(false),
        input,
        cost: metadata
            .get("cost")
            .map(|c| Cost::from_json(c, Cost::default()))
            .unwrap_or_default(),
        context_window,
        max_tokens,
        metadata: extra,
        ..ModelSpec::default()
    })
}

/// Read the cache; without `allow_stale` entries older than 7 days (or
/// without a timestamp) are ignored. Returns an empty catalog when absent.
pub fn load_cache(home: &AgentHome, allow_stale: bool) -> ModelCatalog {
    let Ok(text) = std::fs::read_to_string(home.openrouter_cache_path()) else {
        return ModelCatalog::default();
    };
    let Ok(value) = serde_json::from_str::<Value>(&text) else {
        return ModelCatalog::default();
    };
    let Some(models) = value.get("models").and_then(Value::as_array) else {
        return ModelCatalog::default();
    };
    if !allow_stale {
        let Some(timestamp) = value
            .get("timestamp")
            .and_then(Value::as_i64)
            .filter(|t| *t > 0)
        else {
            return ModelCatalog::default();
        };
        let age_hours = (chrono::Utc::now().timestamp() - timestamp) / 3600;
        if age_hours > CACHE_MAX_AGE_HOURS {
            return ModelCatalog::default();
        }
    }
    let mut catalog = ModelCatalog::default();
    if let Some(p) = ModelCatalog::builtin().find_provider("openrouter") {
        catalog.providers.push(p.clone());
    }
    catalog.models = models.iter().filter_map(model_from_cache).collect();
    catalog.normalize();
    catalog
}

fn offline() -> bool {
    matches!(
        std::env::var("PI_OFFLINE").as_deref(),
        Ok("1") | Ok("true") | Ok("yes")
    )
}

/// Fetch OpenRouter models, updating the cache; on failure fall back to a
/// fresh cache when `allow_cached_fallback`. `PI_OFFLINE` reads only the cache.
pub async fn discover(
    http: &reqwest::Client,
    home: &AgentHome,
    api_key: Option<&str>,
    base_url: &str,
    allow_cached_fallback: bool,
) -> Result<ModelCatalog> {
    if offline() {
        return Ok(load_cache(home, false));
    }
    let mut request = http
        .get(format!("{}/models", base_url.trim_end_matches('/')))
        .header("Accept", "application/json")
        .timeout(Duration::from_secs(10));
    if let Some(key) = api_key.filter(|k| !k.is_empty()) {
        request = request.bearer_auth(key);
    }
    let outcome: Result<ModelCatalog> = async {
        let response = request
            .send()
            .await
            .map_err(|e| anyhow!("failed to fetch OpenRouter models: {e}"))?;
        let status = response.status();
        if !status.is_success() {
            bail!(
                "OpenRouter models request failed with HTTP {}",
                status.as_u16()
            );
        }
        let body: Value = response
            .json()
            .await
            .map_err(|e| anyhow!("invalid OpenRouter models response: {e}"))?;
        catalog_from_response(&body)
    }
    .await;
    match outcome {
        Ok(catalog) => {
            let _ = save_cache(home, &catalog);
            Ok(catalog)
        }
        Err(err) => {
            if allow_cached_fallback {
                let cached = load_cache(home, false);
                if !cached.models.is_empty() {
                    return Ok(cached);
                }
            }
            Err(err)
        }
    }
}

/// Account credit totals in USD.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct Credits {
    pub total_credits: f64,
    pub total_usage: f64,
}

impl Credits {
    pub fn remaining(&self) -> f64 {
        self.total_credits - self.total_usage
    }
}

/// `GET /credits`.
pub async fn fetch_credits(
    http: &reqwest::Client,
    api_key: &str,
    base_url: &str,
) -> Result<Credits> {
    if api_key.is_empty() {
        bail!("OpenRouter credits request requires an API key");
    }
    let response = http
        .get(format!("{}/credits", base_url.trim_end_matches('/')))
        .header("Accept", "application/json")
        .bearer_auth(api_key)
        .timeout(Duration::from_secs(10))
        .send()
        .await
        .map_err(|e| anyhow!("failed to fetch OpenRouter credits: {e}"))?;
    let status = response.status();
    if !status.is_success() {
        bail!(
            "OpenRouter credits request failed with HTTP {}",
            status.as_u16()
        );
    }
    let body: Value = response
        .json()
        .await
        .map_err(|e| anyhow!("invalid OpenRouter credits response: {e}"))?;
    let Some(data) = body.get("data").filter(|d| d.is_object()) else {
        bail!("OpenRouter credits response did not contain a data object");
    };
    Ok(Credits {
        total_credits: number_or_string(data, "total_credits").unwrap_or(0.0),
        total_usage: number_or_string(data, "total_usage").unwrap_or(0.0),
    })
}

/// Outcome of an image generation request.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct GeneratedImages {
    pub model: String,
    /// Text and image blocks returned by the model.
    pub output: Vec<ContentBlock>,
    pub response_id: Option<String>,
    pub usage: Option<Usage>,
    pub error_message: Option<String>,
}

/// Request body for an image model (Pi `buildParams`).
pub fn image_request_body(model: &ImageModelSpec, input: &[ContentBlock]) -> Value {
    let content: Vec<Value> = input
        .iter()
        .filter_map(|b| match b {
            ContentBlock::Text { text } => Some(json!({ "type": "text", "text": text })),
            ContentBlock::Image { data, mime_type } => Some(json!({
                "type": "image_url",
                "image_url": { "url": format!("data:{mime_type};base64,{data}") },
            })),
            _ => None,
        })
        .collect();
    json!({
        "model": model.id,
        "messages": [{ "role": "user", "content": content }],
        "stream": false,
        "modalities": if model.output.iter().any(|o| o == "text") { json!(["image", "text"]) } else { json!(["image"]) },
    })
}

/// Parse a chat-completions image response.
pub fn parse_image_response(model: &ImageModelSpec, response: &Value) -> GeneratedImages {
    let mut out = GeneratedImages {
        model: model.id.clone(),
        response_id: response
            .get("id")
            .and_then(Value::as_str)
            .map(str::to_string),
        ..Default::default()
    };
    if let Some(raw) = response.get("usage").filter(|u| u.is_object()) {
        let get = |v: &Value, k: &str| v.get(k).and_then(Value::as_u64).unwrap_or(0);
        let details = raw
            .get("prompt_tokens_details")
            .cloned()
            .unwrap_or(Value::Null);
        let cached = get(&details, "cached_tokens");
        let cache_write = get(&details, "cache_write_tokens");
        let cache_read = if cache_write > 0 {
            cached.saturating_sub(cache_write)
        } else {
            cached
        };
        let input = get(raw, "prompt_tokens").saturating_sub(cache_read + cache_write);
        let output = get(raw, "completion_tokens");
        let mut usage = Usage {
            input_tokens: input,
            output_tokens: output,
            cache_read_tokens: cache_read,
            cache_write_tokens: cache_write,
            total_tokens: input + output + cache_read + cache_write,
            total_cost_usd: 0.0,
        };
        usage.total_cost_usd = model.cost.compute(&usage);
        out.usage = Some(usage);
    }
    if let Some(message) = response
        .get("choices")
        .and_then(|c| c.get(0))
        .and_then(|c| c.get("message"))
    {
        if let Some(text) = message
            .get("content")
            .and_then(Value::as_str)
            .filter(|t| !t.is_empty())
        {
            out.output.push(ContentBlock::text(text));
        }
        for image in message
            .get("images")
            .and_then(Value::as_array)
            .into_iter()
            .flatten()
        {
            let url = match image.get("image_url") {
                Some(Value::String(s)) => Some(s.as_str()),
                Some(obj) => obj.get("url").and_then(Value::as_str),
                None => None,
            };
            if let Some((mime, data)) = url.and_then(crate::images::parse_data_url) {
                out.output.push(ContentBlock::Image {
                    data,
                    mime_type: mime,
                });
            }
        }
    }
    out
}

/// Generate images with an OpenRouter image model; failures are reported in
/// `error_message` (Pi `generateImagesOpenRouter`).
pub async fn generate_images(
    http: &reqwest::Client,
    model: &ImageModelSpec,
    input: &[ContentBlock],
    api_key: Option<&str>,
) -> GeneratedImages {
    let result: Result<GeneratedImages> = async {
        let key = api_key
            .map(str::to_string)
            .or_else(|| crate::auth::env_api_key(&model.provider))
            .filter(|k| !k.is_empty())
            .ok_or_else(|| anyhow!("No API key available for provider: {}", model.provider))?;
        let mut request = http
            .post(format!(
                "{}/chat/completions",
                model.base_url.trim_end_matches('/')
            ))
            .bearer_auth(key)
            .json(&image_request_body(model, input));
        for (k, v) in &model.headers {
            request = request.header(k.as_str(), v.as_str());
        }
        let response = request.send().await?;
        let status = response.status().as_u16();
        let text = response.text().await.unwrap_or_default();
        if !(200..300).contains(&status) {
            bail!(crate::provider::http::HttpFailure { status, body: text }.message());
        }
        let value: Value = serde_json::from_str(&text)?;
        Ok(parse_image_response(model, &value))
    }
    .await;
    result.unwrap_or_else(|err| GeneratedImages {
        model: model.id.clone(),
        error_message: Some(format!("{err:#}")),
        ..Default::default()
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::provider::test_server::{MockResponse, serve};

    fn sample() -> Value {
        json!({"data":[
            {"id":"anthropic/claude-sonnet-4","name":"Claude Sonnet 4","context_length":200000,
             "top_provider":{"context_length":180000,"max_completion_tokens":64000},
             "pricing":{"prompt":"0.000003","completion":"0.000015","input_cache_read":"0.0000003","input_cache_write":"0.00000375"},
             "architecture":{"input_modalities":["text","image"],"output_modalities":["text"]},
             "supported_parameters":["tools","reasoning"]},
            {"id":"img/only","architecture":{"output_modalities":["image"]}},
            {"id":"no/tools","supported_parameters":["temperature"]}
        ]})
    }

    #[test]
    fn converts_pricing_and_limits() {
        let catalog = catalog_from_response(&sample()).unwrap();
        assert_eq!(catalog.models.len(), 2);
        let claude = catalog
            .find_model("openrouter", "anthropic/claude-sonnet-4")
            .unwrap();
        assert_eq!(claude.context_window, 180_000);
        assert_eq!(claude.max_tokens, 64_000);
        assert!((claude.cost.input - 3.0).abs() < 1e-9);
        assert!((claude.cost.cache_write - 3.75).abs() < 1e-9);
        assert!(claude.reasoning && claude.supports_images() && supports_tools(claude));
        let plain = catalog.find_model("openrouter", "no/tools").unwrap();
        assert_eq!(plain.name, "OpenRouter no/tools");
        assert_eq!((plain.context_window, plain.max_tokens), (4096, 4096));
        assert!(!supports_tools(plain));
        assert!(catalog_from_response(&json!({})).is_err());
    }

    #[test]
    fn cache_round_trip_and_staleness() {
        let dir = tempfile::tempdir().unwrap();
        let home = AgentHome::new(dir.path());
        assert!(load_cache(&home, true).models.is_empty());
        let catalog = catalog_from_response(&sample()).unwrap();
        save_cache(&home, &catalog).unwrap();
        let loaded = load_cache(&home, false);
        let a = catalog
            .find_model("openrouter", "anthropic/claude-sonnet-4")
            .unwrap();
        let b = loaded
            .find_model("openrouter", "anthropic/claude-sonnet-4")
            .unwrap();
        assert_eq!(a.cost, b.cost);
        assert_eq!(
            (a.context_window, a.max_tokens, a.reasoning),
            (b.context_window, b.max_tokens, b.reasoning)
        );
        assert_eq!(a.input, b.input);
        assert!(!supports_tools(
            loaded.find_model("openrouter", "no/tools").unwrap()
        ));

        let mut raw: Value =
            serde_json::from_str(&std::fs::read_to_string(home.openrouter_cache_path()).unwrap())
                .unwrap();
        raw["timestamp"] = json!(chrono::Utc::now().timestamp() - 8 * 24 * 3600);
        std::fs::write(home.openrouter_cache_path(), raw.to_string()).unwrap();
        assert!(load_cache(&home, false).models.is_empty());
        assert_eq!(load_cache(&home, true).models.len(), 2);
    }

    #[tokio::test]
    async fn discovery_falls_back_to_cache_and_credits() {
        let dir = tempfile::tempdir().unwrap();
        let home = AgentHome::new(dir.path());
        let (base, requests) = serve(vec![
            MockResponse::json(200, sample().to_string()),
            MockResponse::json(503, "down"),
            MockResponse::json(503, "down"),
            MockResponse::json(200, r#"{"data":{"total_credits":"10.5","total_usage":2}}"#),
            MockResponse::json(200, r#"{"nope":1}"#),
        ])
        .await;
        let http = reqwest::Client::new();
        let fresh = discover(&http, &home, Some("k"), &base, true)
            .await
            .unwrap();
        assert_eq!(fresh.models.len(), 2);
        assert_eq!(
            requests.lock().unwrap()[0].headers["authorization"],
            "Bearer k"
        );
        let cached = discover(&http, &home, None, &base, true).await.unwrap();
        assert_eq!(cached.models.len(), 2);
        let err = discover(&http, &home, None, &base, false)
            .await
            .unwrap_err();
        assert_eq!(
            err.to_string(),
            "OpenRouter models request failed with HTTP 503"
        );
        let credits = fetch_credits(&http, "k", &base).await.unwrap();
        assert_eq!(credits.remaining(), 8.5);
        let err = fetch_credits(&http, "k", &base).await.unwrap_err();
        assert_eq!(
            err.to_string(),
            "OpenRouter credits response did not contain a data object"
        );
        assert!(fetch_credits(&http, "", &base).await.is_err());
    }

    #[tokio::test]
    async fn generates_images() {
        let model =
            ModelCatalog::find_image_model("openrouter", "google/gemini-2.5-flash-image").unwrap();
        let body = image_request_body(model, &[ContentBlock::text("a cat")]);
        assert_eq!(body["modalities"], json!(["image", "text"]));
        let flux =
            ModelCatalog::find_image_model("openrouter", "black-forest-labs/flux.2-pro").unwrap();
        assert_eq!(
            image_request_body(flux, &[])["modalities"],
            json!(["image"])
        );

        let (base, requests) = serve(vec![MockResponse::json(
            200,
            r#"{"id":"gen-1","choices":[{"message":{"content":"Here","images":[{"image_url":{"url":"data:image/png;base64,AAAA"}},{"image_url":"https://not-inline"}]}}],
                "usage":{"prompt_tokens":1000000,"completion_tokens":0}}"#,
        )])
        .await;
        let mut local = model.clone();
        local.base_url = base;
        let out = generate_images(
            &reqwest::Client::new(),
            &local,
            &[ContentBlock::text("a cat")],
            Some("k"),
        )
        .await;
        assert_eq!(out.error_message, None);
        assert_eq!(out.response_id.as_deref(), Some("gen-1"));
        assert_eq!(out.output.len(), 2);
        assert!(
            matches!(&out.output[1], ContentBlock::Image { data, mime_type } if data == "AAAA" && mime_type == "image/png")
        );
        assert!((out.usage.unwrap().total_cost_usd - 0.3).abs() < 1e-9);
        assert_eq!(requests.lock().unwrap()[0].json()["stream"], false);
    }
}
