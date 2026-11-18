//! Ollama instances: settings (`settings.ollama.instances`), the management
//! API (version, tags + show, ps, pull, load/unload, delete) and catalog
//! records for provider ids `ollama:<id>` (archived qt behaviour).

use std::time::Duration;

use anyhow::{Result, anyhow, bail};
use serde_json::{Map, Value, json};

use crate::catalog::{Cost, ModelCatalog, ModelSpec, ProviderSpec};

pub const DEFAULT_TIMEOUT_MS: u64 = 120_000;
pub const DEFAULT_CONTEXT_WINDOW: u64 = 131_072;
pub const DEFAULT_MAX_TOKENS: u64 = 16_384;
pub const DEFAULT_BASE_URL: &str = "http://localhost:11434";

/// A model installed on an instance (cached in settings).
#[derive(Debug, Clone, PartialEq, Default)]
pub struct OllamaModel {
    pub name: String,
    pub modified_at: String,
    pub digest: String,
    pub size: u64,
    pub capabilities: Vec<String>,
    pub context_window: u64,
    pub max_tokens: u64,
    pub details: Value,
}

impl OllamaModel {
    pub fn has_capability(&self, capability: &str) -> bool {
        self.capabilities
            .iter()
            .any(|c| c.eq_ignore_ascii_case(capability))
    }

    pub fn vision(&self) -> bool {
        self.has_capability("vision") || self.has_capability("image")
    }

    pub fn reasoning(&self) -> bool {
        self.has_capability("thinking") || self.has_capability("reasoning")
    }

    fn to_json(&self) -> Value {
        json!({
            "name": self.name,
            "modifiedAt": self.modified_at,
            "digest": self.digest,
            "size": self.size,
            "capabilities": self.capabilities,
            "contextWindow": self.context_window,
            "maxTokens": self.max_tokens,
            "details": if self.details.is_object() { self.details.clone() } else { json!({}) },
        })
    }

    fn from_cache(value: &Value) -> Option<Self> {
        let name = value.get("name").and_then(Value::as_str)?.to_string();
        let context_window = value
            .get("contextWindow")
            .and_then(Value::as_u64)
            .unwrap_or(DEFAULT_CONTEXT_WINDOW);
        Some(Self {
            name,
            modified_at: str_field(value, "modifiedAt"),
            digest: str_field(value, "digest"),
            size: value.get("size").and_then(Value::as_u64).unwrap_or(0),
            capabilities: strings(value.get("capabilities")),
            context_window,
            max_tokens: value
                .get("maxTokens")
                .and_then(Value::as_u64)
                .unwrap_or(context_window.min(DEFAULT_MAX_TOKENS)),
            details: value
                .get("details")
                .filter(|d| d.is_object())
                .cloned()
                .unwrap_or_else(|| json!({})),
        })
    }

    /// Build from an `/api/tags` entry and its `/api/show` response.
    pub fn from_api(tag: &Value, shown: &Value) -> Self {
        let name = tag
            .get("name")
            .or_else(|| tag.get("model"))
            .and_then(Value::as_str)
            .unwrap_or_default()
            .to_string();
        let context_window = context_length(shown.get("model_info"))
            .or_else(|| context_length(shown.get("parameters")))
            .unwrap_or(DEFAULT_CONTEXT_WINDOW);
        Self {
            name,
            modified_at: str_field(tag, "modified_at"),
            digest: str_field(tag, "digest"),
            size: tag.get("size").and_then(Value::as_u64).unwrap_or(0),
            capabilities: strings(shown.get("capabilities")),
            context_window,
            max_tokens: context_window.min(DEFAULT_MAX_TOKENS),
            details: shown
                .get("details")
                .or_else(|| tag.get("details"))
                .filter(|d| d.is_object())
                .cloned()
                .unwrap_or_else(|| json!({})),
        }
    }
}

fn str_field(value: &Value, key: &str) -> String {
    value
        .get(key)
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_string()
}

fn strings(value: Option<&Value>) -> Vec<String> {
    value
        .and_then(Value::as_array)
        .map(|a| {
            a.iter()
                .filter_map(Value::as_str)
                .map(str::to_string)
                .collect()
        })
        .unwrap_or_default()
}

/// First `*.context_length` value in a `model_info` / `parameters` object.
fn context_length(value: Option<&Value>) -> Option<u64> {
    let object = value?.as_object()?;
    object.iter().find_map(|(k, v)| {
        if !k.ends_with(".context_length") {
            return None;
        }
        v.as_u64()
            .or_else(|| v.as_str().and_then(|s| s.trim().parse().ok()))
            .map(|n: u64| n.max(1))
    })
}

/// One configured Ollama server.
#[derive(Debug, Clone, PartialEq)]
pub struct OllamaInstance {
    pub id: String,
    pub name: String,
    pub base_url: String,
    pub api_key: Option<String>,
    pub enabled: bool,
    pub timeout_ms: u64,
    pub models: Vec<OllamaModel>,
}

impl OllamaInstance {
    pub fn new(id: &str, base_url: &str) -> Self {
        Self {
            id: id.into(),
            name: id.into(),
            base_url: normalize_base_url(base_url),
            api_key: None,
            enabled: true,
            timeout_ms: DEFAULT_TIMEOUT_MS,
            models: Vec::new(),
        }
    }

    /// The implicit instance offered when none are configured.
    pub fn local_default() -> Self {
        Self::new("local", "http://127.0.0.1:11434")
    }

    pub fn display_name(&self) -> &str {
        if self.name.is_empty() {
            &self.id
        } else {
            &self.name
        }
    }

    /// Provider label, `Ollama · <name>`.
    pub fn label(&self) -> String {
        format!("Ollama · {}", self.display_name())
    }

    pub fn to_json(&self) -> Value {
        let mut value = json!({
            "id": self.id,
            "name": self.name,
            "baseUrl": self.base_url,
            "enabled": self.enabled,
            "timeoutMs": self.timeout_ms,
        });
        if let Some(key) = &self.api_key {
            value["apiKey"] = json!(key);
        }
        value["models"] = Value::Array(self.models.iter().map(OllamaModel::to_json).collect());
        value
    }
}

/// Trim, drop trailing slashes and a trailing `/v1`.
pub fn normalize_base_url(value: &str) -> String {
    let mut url = value.trim().trim_end_matches('/').to_string();
    if url.ends_with("/v1") {
        url.truncate(url.len() - 3);
    }
    url
}

pub fn provider_id(instance_id: &str) -> String {
    format!("ollama:{instance_id}")
}

pub fn is_ollama_provider_id(provider: &str) -> bool {
    provider.len() > 7 && provider.starts_with("ollama:")
}

/// Ids are letters, digits, `-` and `_`.
pub fn valid_instance_id(id: &str) -> bool {
    !id.is_empty()
        && id
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_')
}

fn valid_url(url: &str) -> bool {
    url.starts_with("http://") || url.starts_with("https://")
}

/// Parse `settings.ollama` (archived qt validation and messages).
pub fn parse_instances(ollama: &Value) -> Result<Vec<OllamaInstance>> {
    let field = "ollama";
    let Some(list) = ollama.get("instances").and_then(Value::as_array) else {
        bail!("{field}.instances must be an array");
    };
    let mut ids = std::collections::HashSet::new();
    let mut out = Vec::new();
    for (index, value) in list.iter().enumerate() {
        let prefix = format!("{field}.instances[{index}]");
        let Some(id) = value.get("id").and_then(Value::as_str) else {
            bail!("{prefix}.id must be a string");
        };
        if !valid_instance_id(id) {
            bail!("{prefix}.id must contain only letters, numbers, '-' or '_'");
        }
        if !ids.insert(id.to_lowercase()) {
            bail!("{field}.instances contains duplicate id: {id}");
        }
        let base_url = normalize_base_url(
            value
                .get("baseUrl")
                .and_then(Value::as_str)
                .unwrap_or(DEFAULT_BASE_URL),
        );
        if !valid_url(&base_url) {
            bail!("{prefix}.baseUrl must start with http:// or https://");
        }
        let api_key = match value.get("apiKey") {
            None => None,
            Some(Value::String(k)) => (!k.is_empty()).then(|| k.clone()),
            Some(_) => bail!("{prefix}.apiKey must be a string"),
        };
        let enabled = match value.get("enabled") {
            None => true,
            Some(Value::Bool(b)) => *b,
            Some(_) => bail!("{prefix}.enabled must be a boolean"),
        };
        let timeout_ms = match value.get("timeoutMs") {
            None => DEFAULT_TIMEOUT_MS,
            Some(v) => match v.as_i64() {
                Some(n) => n.max(1) as u64,
                None => bail!("{prefix}.timeoutMs must be an integer"),
            },
        };
        let models = match value.get("models") {
            None => Vec::new(),
            Some(Value::Array(models)) => {
                models.iter().filter_map(OllamaModel::from_cache).collect()
            }
            Some(_) => bail!("{prefix}.models must be an array"),
        };
        out.push(OllamaInstance {
            id: id.to_string(),
            name: value
                .get("name")
                .and_then(Value::as_str)
                .unwrap_or(id)
                .to_string(),
            base_url,
            api_key,
            enabled,
            timeout_ms,
            models,
        });
    }
    Ok(out)
}

/// Chat-completions compatibility for Ollama's OpenAI endpoint: no
/// `developer` role or `store`, and `max_tokens` for the output budget.
pub fn compat() -> Map<String, Value> {
    json!({ "supportsDeveloperRole": false, "supportsStore": false, "maxTokensField": "max_tokens" })
        .as_object()
        .cloned()
        .unwrap_or_default()
}

/// Catalog providers and models for enabled instances.
pub fn catalog_from_instances(instances: &[OllamaInstance]) -> ModelCatalog {
    let mut catalog = ModelCatalog::default();
    for instance in instances.iter().filter(|i| i.enabled && !i.id.is_empty()) {
        let id = provider_id(&instance.id);
        let base_url = format!("{}/v1", normalize_base_url(&instance.base_url));
        let mut metadata = Map::new();
        metadata.insert("ollama".into(), json!(true));
        metadata.insert("ollamaInstance".into(), json!(instance.id));
        catalog.providers.push(ProviderSpec {
            id: id.clone(),
            name: instance.label(),
            api: "openai-completions".into(),
            base_url: base_url.clone(),
            api_key: instance.api_key.clone(),
            requires_auth: false,
            compat: compat(),
            metadata,
            ..ProviderSpec::default()
        });
        for model in instance.models.iter().filter(|m| !m.name.is_empty()) {
            let mut metadata = Map::new();
            metadata.insert(
                "ollama".into(),
                json!({
                    "instance": instance.id,
                    "digest": model.digest,
                    "size": model.size,
                    "modifiedAt": model.modified_at,
                    "capabilities": model.capabilities,
                    "details": model.details,
                }),
            );
            catalog.models.push(ModelSpec {
                provider: id.clone(),
                id: model.name.clone(),
                name: format!("{} · {}", model.name, instance.display_name()),
                api: "openai-completions".into(),
                base_url: base_url.clone(),
                reasoning: model.reasoning(),
                input: if model.vision() {
                    vec!["text".into(), "image".into()]
                } else {
                    vec!["text".into()]
                },
                cost: Cost::default(),
                context_window: model.context_window,
                max_tokens: model.max_tokens,
                api_key: instance.api_key.clone(),
                timeout_ms: Some(instance.timeout_ms),
                metadata,
                ..ModelSpec::default()
            });
        }
    }
    catalog.normalize();
    catalog
}

/// Management API client for one instance.
pub struct OllamaClient<'a> {
    http: &'a reqwest::Client,
    instance: &'a OllamaInstance,
}

impl<'a> OllamaClient<'a> {
    pub fn new(http: &'a reqwest::Client, instance: &'a OllamaInstance) -> Self {
        Self { http, instance }
    }

    /// Send a JSON request; errors read `Ollama <METHOD> <path> failed: <detail>`.
    pub async fn request(
        &self,
        method: reqwest::Method,
        path: &str,
        body: Option<Value>,
    ) -> Result<Value> {
        let base = normalize_base_url(&self.instance.base_url);
        if !valid_url(&base) {
            bail!("Ollama base URL must start with http:// or https://");
        }
        let path = if path.starts_with('/') {
            path.to_string()
        } else {
            format!("/{path}")
        };
        let fail = |detail: String| anyhow!("Ollama {method} {path} failed: {detail}");
        let mut builder = self
            .http
            .request(method.clone(), format!("{base}{path}"))
            .header("Accept", "application/json")
            .timeout(Duration::from_millis(self.instance.timeout_ms.max(1)));
        if let Some(key) = self.instance.api_key.as_deref().filter(|k| !k.is_empty()) {
            builder = builder.bearer_auth(key);
        }
        if let Some(body) = body {
            builder = builder.json(&body);
        }
        let response = builder.send().await.map_err(|e| fail(e.to_string()))?;
        let status = response.status();
        let text = response.text().await.map_err(|e| fail(e.to_string()))?;
        if !status.is_success() {
            let detail = serde_json::from_str::<Value>(&text)
                .ok()
                .and_then(|v| v.get("error").and_then(Value::as_str).map(str::to_string))
                .unwrap_or_else(|| {
                    if text.is_empty() {
                        format!("HTTP {}", status.as_u16())
                    } else {
                        text.clone()
                    }
                });
            return Err(fail(detail));
        }
        if text.trim().is_empty() {
            return Ok(json!({}));
        }
        serde_json::from_str(&text).map_err(|e| fail(e.to_string()))
    }

    /// Server version string.
    pub async fn version(&self) -> Result<String> {
        let v = self
            .request(reqwest::Method::GET, "/api/version", None)
            .await?;
        Ok(v.get("version")
            .and_then(Value::as_str)
            .unwrap_or("unknown")
            .to_string())
    }

    /// Installed models with capabilities and context length (`/api/tags` + `/api/show`).
    pub async fn discover_models(&self) -> Result<Vec<OllamaModel>> {
        let listed = self
            .request(reqwest::Method::GET, "/api/tags", None)
            .await?;
        let Some(models) = listed.get("models").and_then(Value::as_array) else {
            bail!("Ollama /api/tags response is missing models array");
        };
        let mut out = Vec::new();
        for tag in models.iter().filter(|m| m.is_object()) {
            let name = tag
                .get("name")
                .or_else(|| tag.get("model"))
                .and_then(Value::as_str)
                .unwrap_or_default();
            if name.is_empty() {
                continue;
            }
            let shown = self.show(name).await.unwrap_or_else(|_| json!({}));
            out.push(OllamaModel::from_api(tag, &shown));
        }
        out.sort_by_key(|m| m.name.to_lowercase());
        Ok(out)
    }

    /// `/api/show` for a model.
    pub async fn show(&self, model: &str) -> Result<Value> {
        self.request(
            reqwest::Method::POST,
            "/api/show",
            Some(json!({ "model": model, "verbose": false })),
        )
        .await
    }

    /// Running models (`/api/ps`).
    pub async fn running(&self) -> Result<Value> {
        self.request(reqwest::Method::GET, "/api/ps", None).await
    }

    /// Download a model (blocking until done, `stream: false`).
    pub async fn pull(&self, model: &str) -> Result<Value> {
        self.request(
            reqwest::Method::POST,
            "/api/pull",
            Some(json!({ "model": model, "stream": false })),
        )
        .await
    }

    /// Load (keep alive 5 min) or unload (keep alive 0) a model via `/api/chat`.
    pub async fn set_loaded(&self, model: &str, loaded: bool) -> Result<Value> {
        self.request(
            reqwest::Method::POST,
            "/api/chat",
            Some(json!({
                "model": model,
                "messages": [],
                "stream": false,
                "keep_alive": if loaded { json!("5m") } else { json!(0) },
            })),
        )
        .await
    }

    /// Delete a model from the server.
    pub async fn delete(&self, model: &str) -> Result<Value> {
        self.request(
            reqwest::Method::DELETE,
            "/api/delete",
            Some(json!({ "model": model })),
        )
        .await
    }
}

/// A `configure` change (archived `/ollama configure <id> <field> <value>`).
pub fn configure_instance(instance: &mut OllamaInstance, field: &str, value: &str) -> Result<()> {
    match field.to_ascii_lowercase().as_str() {
        "url" => {
            let url = normalize_base_url(value);
            if !valid_url(&url) {
                bail!("Ollama URL must start with http:// or https://");
            }
            instance.base_url = url;
            instance.models.clear();
        }
        "name" => instance.name = value.to_string(),
        "enabled" => {
            instance.enabled = match value.to_ascii_lowercase().as_str() {
                "true" | "on" => true,
                "false" | "off" => false,
                _ => bail!("enabled value must be true/false or on/off"),
            }
        }
        "timeout" => {
            let ms: i64 = value
                .trim()
                .parse()
                .map_err(|_| anyhow!("timeout must be an integer number of milliseconds"))?;
            instance.timeout_ms = ms.max(1) as u64;
        }
        "key" => instance.api_key = (value != "-").then(|| value.to_string()),
        other => bail!("unknown Ollama setting: {other}"),
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::provider::test_server::{MockResponse, serve};

    #[test]
    fn parses_and_validates_instances() {
        let parsed = parse_instances(&json!({"instances":[
            {"id":"gpu","baseUrl":"http://box:11434/v1/","timeoutMs":0,"models":[{"name":"qwen3","capabilities":["thinking"],"contextWindow":40000}]},
            {"id":"off","enabled":false}
        ]}))
        .unwrap();
        assert_eq!(parsed[0].base_url, "http://box:11434");
        assert_eq!(parsed[0].name, "gpu");
        assert_eq!(parsed[0].timeout_ms, 1);
        assert_eq!(parsed[0].models[0].max_tokens, 16_384);
        assert_eq!(parsed[1].base_url, DEFAULT_BASE_URL);
        assert!(!parsed[1].enabled);
        let err = |v: Value| parse_instances(&v).unwrap_err().to_string();
        assert_eq!(err(json!({})), "ollama.instances must be an array");
        assert_eq!(
            err(json!({"instances":[{"id":"a b"}]})),
            "ollama.instances[0].id must contain only letters, numbers, '-' or '_'"
        );
        assert_eq!(
            err(json!({"instances":[{"id":"a"},{"id":"A"}]})),
            "ollama.instances contains duplicate id: A"
        );
        assert_eq!(
            err(json!({"instances":[{"id":"a","baseUrl":"ftp://x"}]})),
            "ollama.instances[0].baseUrl must start with http:// or https://"
        );
    }

    #[test]
    fn builds_catalog_records() {
        let mut instance = OllamaInstance::new("gpu", "http://box:11434");
        instance.name = "GPU".into();
        instance.models.push(OllamaModel {
            name: "llava".into(),
            capabilities: vec!["completion".into(), "vision".into()],
            context_window: 8192,
            max_tokens: 8192,
            ..Default::default()
        });
        let catalog = catalog_from_instances(&[instance]);
        let provider = catalog.find_provider("ollama:gpu").unwrap();
        assert_eq!(provider.name, "Ollama · GPU");
        assert!(!provider.requires_auth);
        let model = catalog.find_model("ollama:gpu", "llava").unwrap();
        assert_eq!(model.name, "llava · GPU");
        assert_eq!(model.base_url, "http://box:11434/v1");
        assert!(model.supports_images() && !model.reasoning);
        assert!(is_ollama_provider_id("ollama:gpu") && !is_ollama_provider_id("ollama:"));
    }

    #[test]
    fn configure_fields() {
        let mut i = OllamaInstance::new("a", "http://x");
        i.models.push(OllamaModel::default());
        configure_instance(&mut i, "URL", "https://y/v1").unwrap();
        assert_eq!(i.base_url, "https://y");
        assert!(i.models.is_empty());
        configure_instance(&mut i, "enabled", "off").unwrap();
        assert!(!i.enabled);
        configure_instance(&mut i, "key", "secret").unwrap();
        assert_eq!(i.api_key.as_deref(), Some("secret"));
        configure_instance(&mut i, "key", "-").unwrap();
        assert!(i.api_key.is_none());
        assert_eq!(
            configure_instance(&mut i, "timeout", "x")
                .unwrap_err()
                .to_string(),
            "timeout must be an integer number of milliseconds"
        );
        assert_eq!(
            configure_instance(&mut i, "colour", "x")
                .unwrap_err()
                .to_string(),
            "unknown Ollama setting: colour"
        );
    }

    #[tokio::test]
    async fn management_api_against_mock_server() {
        let (base, requests) = serve(vec![
            MockResponse::json(200, r#"{"version":"0.12.3"}"#),
            MockResponse::json(200, r#"{"models":[{"name":"qwen3:8b","digest":"d","size":5,"modified_at":"t"}]}"#),
            MockResponse::json(
                200,
                r#"{"capabilities":["completion","tools","thinking"],"model_info":{"qwen3.context_length":40960},"details":{"family":"qwen3"}}"#,
            ),
            MockResponse::json(200, r#"{"models":[]}"#),
            MockResponse::json(200, r#"{"status":"success"}"#),
            MockResponse::json(200, r#"{}"#),
            MockResponse::json(404, r#"{"error":"model 'nope' not found"}"#),
        ])
        .await;
        let mut instance = OllamaInstance::new("t", &base);
        instance.api_key = Some("k".into());
        let http = reqwest::Client::new();
        let client = OllamaClient::new(&http, &instance);
        assert_eq!(client.version().await.unwrap(), "0.12.3");
        let models = client.discover_models().await.unwrap();
        assert_eq!(models.len(), 1);
        assert_eq!(models[0].context_window, 40_960);
        assert_eq!(models[0].max_tokens, 16_384);
        assert!(models[0].reasoning());
        assert_eq!(models[0].details["family"], "qwen3");
        client.running().await.unwrap();
        client.pull("qwen3:8b").await.unwrap();
        client.set_loaded("qwen3:8b", false).await.unwrap();
        let err = client.delete("nope").await.unwrap_err().to_string();
        assert_eq!(
            err,
            "Ollama DELETE /api/delete failed: model 'nope' not found"
        );
        let reqs = requests.lock().unwrap();
        assert_eq!(reqs[0].headers["authorization"], "Bearer k");
        assert_eq!(reqs[2].json()["model"], "qwen3:8b");
        assert_eq!(reqs[4].json()["stream"], false);
        assert_eq!(reqs[5].path, "/api/chat");
        assert_eq!(reqs[5].json()["keep_alive"], 0);
        assert_eq!(reqs[6].method, "DELETE");
    }
}
