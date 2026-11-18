//! Model providers.
//!
//! Wire APIs (Pi naming):
//! - `anthropic-messages` ([`anthropic`]): Anthropic Messages, including
//!   Claude Pro/Max OAuth tokens.
//! - `openai-completions` ([`openai`]): OpenAI-compatible chat completions
//!   with the per-host compatibility matrix ([`compat`]).
//! - `openai-responses`, `openai-codex-responses`, `azure-openai-responses`
//!   ([`responses`]): the OpenAI Responses API, the ChatGPT (Codex)
//!   subscription endpoint and Azure OpenAI.
//!
//! Every stream call returns a complete [`AssistantMessage`]; transport and
//! API failures become `stopReason: "error"` messages and cancellation
//! becomes `stopReason: "aborted"` with whatever content had streamed.

pub mod anthropic;
pub mod backend;
pub mod compat;
pub mod http;
pub mod models;
pub mod openai;
pub mod responses;
pub mod transform;

use std::collections::{BTreeMap, HashMap, HashSet};
use std::fmt;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};

use serde_json::{Map, Value};
use tokio_util::sync::CancellationToken;

use crate::auth::{AuthStore, Credential, OAuthCredentials};
use crate::catalog::{ModelCatalog, ModelSpec};
use crate::config::{AgentHome, Settings};
use crate::message::{AssistantMessage, ContentBlock, Message, StopReason, now_millis};

pub use backend::{BackendHandle, ProviderBackend, ProviderBackends};
pub use http::RetryPolicy;
pub use models::{ModelInfo, list_models};

/// Wire protocol spoken to a provider.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum ProviderKind {
    AnthropicMessages,
    #[default]
    OpenAiCompletions,
    OpenAiResponses,
    OpenAiCodexResponses,
    AzureOpenAiResponses,
}

impl ProviderKind {
    /// The `api` string recorded on assistant messages (Pi naming).
    pub fn api_name(self) -> &'static str {
        match self {
            Self::AnthropicMessages => "anthropic-messages",
            Self::OpenAiCompletions => "openai-completions",
            Self::OpenAiResponses => "openai-responses",
            Self::OpenAiCodexResponses => "openai-codex-responses",
            Self::AzureOpenAiResponses => "azure-openai-responses",
        }
    }

    /// Parse a Pi `api` string; `None` for APIs without a transport.
    pub fn from_api(api: &str) -> Option<Self> {
        Some(match api {
            "anthropic-messages" => Self::AnthropicMessages,
            "openai-completions" => Self::OpenAiCompletions,
            "openai-responses" => Self::OpenAiResponses,
            "openai-codex-responses" => Self::OpenAiCodexResponses,
            "azure-openai-responses" => Self::AzureOpenAiResponses,
            _ => return None,
        })
    }
}

/// Stored OAuth credentials backing a provider's access token, refreshed
/// before use when expired.
#[derive(Clone, PartialEq)]
pub struct OAuthBinding {
    pub credentials: OAuthCredentials,
    /// `auth.json` to update after a refresh.
    pub auth_path: Option<PathBuf>,
}

impl fmt::Debug for OAuthBinding {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("OAuthBinding")
            .field("expires", &self.credentials.expires)
            .field("auth_path", &self.auth_path)
            .finish()
    }
}

/// A configured provider endpoint.
///
/// Construct with `..Default::default()` so new fields stay source-compatible.
#[derive(Clone, PartialEq, Default)]
pub struct ProviderConfig {
    pub id: String,
    pub label: String,
    /// Default wire API; a model's own `api` takes precedence.
    pub kind: ProviderKind,
    pub base_url: String,
    /// API key or OAuth access token.
    pub api_key: Option<String>,
    pub headers: BTreeMap<String, String>,
    /// Provider-level compatibility overrides.
    pub compat: Map<String, Value>,
    /// Local providers (Ollama) need no credentials.
    pub local: bool,
    /// Known models of this provider (catalog, overrides and discovery cache).
    pub models: Arc<Vec<ModelSpec>>,
    pub oauth: Option<OAuthBinding>,
    pub retry: RetryPolicy,
    /// Also send `Authorization: Bearer <key>` (models.json `authHeader`).
    pub auth_header: bool,
    /// Backend serving this provider instead of the built-in transports.
    pub backend: Option<BackendHandle>,
}

impl fmt::Debug for ProviderConfig {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("ProviderConfig")
            .field("id", &self.id)
            .field("kind", &self.kind)
            .field("base_url", &self.base_url)
            .field("api_key", &self.api_key.as_ref().map(|_| "<set>"))
            .field("models", &self.models.len())
            .field("oauth", &self.oauth.is_some())
            .field("backend", &self.backend)
            .finish()
    }
}

impl ProviderConfig {
    /// Whether requests can be made (local providers need no key).
    pub fn is_ready(&self) -> bool {
        self.local || self.api_key.is_some()
    }

    pub fn needs_api_key(&self) -> bool {
        !self.local
    }

    /// Whether OpenRouter attribution headers are sent (disabled by
    /// `telemetryEnabled: false` / `enableInstallTelemetry: false` in settings,
    /// recorded as compat `attribution: false`).
    pub fn attribution(&self) -> bool {
        self.compat.get("attribution").and_then(Value::as_bool) != Some(false)
    }

    /// The catalog record for `model`, or a permissive fallback.
    pub fn model_spec(&self, model: &str) -> ModelSpec {
        let wanted = model.trim().to_lowercase();
        self.models
            .iter()
            .find(|m| m.id.to_lowercase() == wanted)
            .cloned()
            .unwrap_or_else(|| {
                ModelSpec::fallback(&self.id, self.kind.api_name(), &self.base_url, model)
            })
    }
}

/// Providers that accept an API key from the user, in display order.
pub const KEYED_PROVIDERS: [(&str, &str); 3] = [
    ("anthropic", "Anthropic"),
    ("openai", "OpenAI"),
    ("openrouter", "OpenRouter"),
];

/// Providers excluded although their API has a transport: GitHub Copilot
/// needs its device-flow token exchange, which is out of scope.
const EXCLUDED_PROVIDERS: &[&str] = &["github-copilot"];

/// Catalog models grouped by lower-cased provider id.
type ModelsByProvider = HashMap<String, Arc<Vec<ModelSpec>>>;

struct CatalogCache {
    key: String,
    catalog: Arc<ModelCatalog>,
    by_provider: ModelsByProvider,
    error: Option<String>,
}

fn file_stamp(path: &std::path::Path) -> String {
    std::fs::metadata(path)
        .map(|m| format!("{:?}:{}", m.modified().ok(), m.len()))
        .unwrap_or_default()
}

fn cached_catalog(
    home: &AgentHome,
    settings: &Settings,
) -> (Arc<ModelCatalog>, ModelsByProvider, Option<String>) {
    static CACHE: Mutex<Option<CatalogCache>> = Mutex::new(None);
    let key = format!(
        "{}|{}|{}|{}|{}",
        home.root().display(),
        file_stamp(&home.models_path()),
        file_stamp(&home.openrouter_cache_path()),
        settings
            .raw()
            .get("providers")
            .map(Value::to_string)
            .unwrap_or_default(),
        settings
            .raw()
            .get("ollama")
            .map(Value::to_string)
            .unwrap_or_default(),
    );
    let mut guard = CACHE.lock().expect("catalog cache lock");
    if let Some(hit) = guard.as_ref().filter(|c| c.key == key) {
        return (
            hit.catalog.clone(),
            hit.by_provider.clone(),
            hit.error.clone(),
        );
    }
    let (catalog, error) = ModelCatalog::load(home, settings);
    let mut grouped: HashMap<String, Vec<ModelSpec>> = HashMap::new();
    for model in &catalog.models {
        grouped
            .entry(model.provider.to_lowercase())
            .or_default()
            .push(model.clone());
    }
    let by_provider: ModelsByProvider =
        grouped.into_iter().map(|(k, v)| (k, Arc::new(v))).collect();
    let catalog = Arc::new(catalog);
    *guard = Some(CatalogCache {
        key,
        catalog: catalog.clone(),
        by_provider: by_provider.clone(),
        error: error.clone(),
    });
    (catalog, by_provider, error)
}

/// The model catalog for `settings` (agent home taken from where the
/// settings were loaded), plus any `models.json` error.
pub fn model_catalog(settings: &Settings) -> (Arc<ModelCatalog>, Option<String>) {
    let home = settings.home().cloned().unwrap_or_else(AgentHome::from_env);
    let (catalog, _, error) = cached_catalog(&home, settings);
    (catalog, error)
}

/// Build the provider list from settings, credentials and the catalog.
///
/// Providers whose API has no transport (Google, Bedrock, Mistral, ...) or
/// that the catalog marks unsupported are omitted. Ollama instances appear as
/// `ollama:<id>`; when none are configured a local instance at
/// `http://127.0.0.1:11434` is offered as `ollama:local`.
pub fn configured_providers(settings: &Settings, auth: &AuthStore) -> Vec<ProviderConfig> {
    let home = settings.home().cloned().unwrap_or_else(AgentHome::from_env);
    let (catalog, by_provider, _) = cached_catalog(&home, settings);
    let retry = RetryPolicy::from_settings(settings);
    let mut providers = catalog_providers(&catalog, &by_provider, settings, auth);
    let mut instances = settings.ollama_settings();
    if instances.is_empty() {
        instances.push(crate::ollama::OllamaInstance::local_default());
    }
    for instance in instances.iter().filter(|i| i.enabled) {
        let id = crate::ollama::provider_id(&instance.id);
        providers.push(ProviderConfig {
            label: instance.label(),
            kind: ProviderKind::OpenAiCompletions,
            base_url: format!("{}/v1", instance.base_url),
            api_key: instance.api_key.clone(),
            compat: crate::ollama::compat(),
            local: true,
            models: by_provider
                .get(&id.to_lowercase())
                .cloned()
                .unwrap_or_default(),
            retry: RetryPolicy {
                timeout_ms: instance.timeout_ms,
                ..retry
            },
            id,
            ..ProviderConfig::default()
        });
    }
    providers
}

/// Provider configs for every usable provider of `catalog` (not Ollama
/// instances), with credentials from `auth`, the environment or the
/// catalog. Used for catalogs extended at runtime (extension providers).
pub fn catalog_provider_configs(
    catalog: &ModelCatalog,
    settings: &Settings,
    auth: &AuthStore,
) -> Vec<ProviderConfig> {
    let mut grouped: HashMap<String, Vec<ModelSpec>> = HashMap::new();
    for model in &catalog.models {
        grouped
            .entry(model.provider.to_lowercase())
            .or_default()
            .push(model.clone());
    }
    let by_provider: ModelsByProvider =
        grouped.into_iter().map(|(k, v)| (k, Arc::new(v))).collect();
    catalog_providers(catalog, &by_provider, settings, auth)
}

fn catalog_providers(
    catalog: &ModelCatalog,
    by_provider: &ModelsByProvider,
    settings: &Settings,
    auth: &AuthStore,
) -> Vec<ProviderConfig> {
    let retry = RetryPolicy::from_settings(settings);
    let auth_path = auth.path().map(PathBuf::from);
    let telemetry = ["telemetryEnabled", "enableInstallTelemetry"]
        .iter()
        .all(|k| settings.raw().get(*k).and_then(Value::as_bool) != Some(false));
    let mut providers = Vec::new();
    for spec in &catalog.providers {
        if spec.unsupported.is_some()
            || EXCLUDED_PROVIDERS.contains(&spec.id.as_str())
            || crate::ollama::is_ollama_provider_id(&spec.id)
        {
            continue;
        }
        let Some(kind) = ProviderKind::from_api(&spec.api) else {
            continue;
        };
        let mut config = ProviderConfig {
            id: spec.id.clone(),
            label: spec.name.clone(),
            kind,
            base_url: spec.base_url.trim_end_matches('/').to_string(),
            headers: spec.headers.clone(),
            compat: spec.compat.clone(),
            local: !spec.requires_auth,
            models: by_provider
                .get(&spec.id.to_lowercase())
                .cloned()
                .unwrap_or_default(),
            retry,
            auth_header: spec.auth_header,
            ..ProviderConfig::default()
        };
        if !telemetry {
            config
                .compat
                .insert("attribution".into(), Value::Bool(false));
        }
        match auth.credential(&spec.id) {
            Some(Credential::OAuth(creds)) => {
                config.api_key = Some(creds.access.clone());
                config.oauth = Some(OAuthBinding {
                    credentials: creds,
                    auth_path: auth_path.clone(),
                });
            }
            Some(Credential::ApiKey(key)) => config.api_key = Some(key),
            None => {
                config.api_key = crate::auth::env_api_key(&spec.id).or_else(|| {
                    spec.api_key
                        .as_deref()
                        .and_then(crate::catalog::resolve_config_value)
                })
            }
        }
        providers.push(config);
    }
    providers
}

/// Identifies a model on a provider.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct ModelRef {
    pub provider: String,
    pub id: String,
}

impl ModelRef {
    pub fn new(provider: impl Into<String>, id: impl Into<String>) -> Self {
        Self {
            provider: provider.into(),
            id: id.into(),
        }
    }

    /// Parse `provider/model`; the model part may itself contain slashes.
    pub fn parse(spec: &str) -> Option<Self> {
        let (provider, id) = spec.split_once('/')?;
        (!provider.is_empty() && !id.is_empty()).then(|| Self::new(provider, id))
    }
}

impl fmt::Display for ModelRef {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}/{}", self.provider, self.id)
    }
}

/// Reasoning effort requested from models that support it (Pi levels).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Hash, PartialOrd, Ord)]
pub enum ThinkingLevel {
    /// No reasoning (the model's `off` mapping, where it has one).
    #[default]
    Off,
    Minimal,
    Low,
    Medium,
    High,
    XHigh,
}

impl ThinkingLevel {
    pub const ALL: [ThinkingLevel; 6] = [
        Self::Off,
        Self::Minimal,
        Self::Low,
        Self::Medium,
        Self::High,
        Self::XHigh,
    ];

    pub fn as_str(self) -> &'static str {
        match self {
            Self::Off => "off",
            Self::Minimal => "minimal",
            Self::Low => "low",
            Self::Medium => "medium",
            Self::High => "high",
            Self::XHigh => "xhigh",
        }
    }

    pub fn parse(s: &str) -> Option<Self> {
        let s = s.trim().to_ascii_lowercase();
        Self::ALL.into_iter().find(|l| l.as_str() == s)
    }

    /// Default token budget for budget-based thinking (Pi
    /// `adjustMaxTokensForThinking`; `xhigh` uses the `high` budget).
    pub fn budget_tokens(self) -> Option<u32> {
        match self {
            Self::Off => None,
            Self::Minimal => Some(1_024),
            Self::Low => Some(2_048),
            Self::Medium => Some(8_192),
            Self::High | Self::XHigh => Some(16_384),
        }
    }
}

/// Prompt-cache retention preference (Pi `cacheRetention`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum CacheRetention {
    None,
    #[default]
    Short,
    Long,
}

impl CacheRetention {
    /// The request's preference, else `PI_CACHE_RETENTION=long`, else short.
    pub fn resolve(requested: Option<CacheRetention>) -> CacheRetention {
        requested.unwrap_or_else(|| {
            if std::env::var("PI_CACHE_RETENTION").as_deref() == Ok("long") {
                CacheRetention::Long
            } else {
                CacheRetention::Short
            }
        })
    }
}

/// JSON-schema description of a tool offered to the model.
#[derive(Debug, Clone, PartialEq)]
pub struct ToolSpec {
    pub name: String,
    pub description: String,
    pub parameters: Value,
}

/// Everything a provider needs for one completion.
///
/// Construct with `..Default::default()` so new fields stay source-compatible.
#[derive(Debug, Clone, Default)]
pub struct Request {
    pub model: String,
    pub system: String,
    pub messages: Vec<Message>,
    pub tools: Vec<ToolSpec>,
    /// Output token budget; 0 uses `min(model maxTokens, 32000)`.
    pub max_tokens: u32,
    pub thinking: ThinkingLevel,
    /// Session id for prompt caching and session-affinity headers.
    pub session_id: Option<String>,
    pub cache_retention: Option<CacheRetention>,
    pub temperature: Option<f64>,
    /// OpenAI `service_tier` (`flex`, `priority`, ...).
    pub service_tier: Option<String>,
    /// Responses `reasoning.summary` (default `auto`).
    pub reasoning_summary: Option<String>,
    /// Codex `text.verbosity` (default `low`).
    pub text_verbosity: Option<String>,
    /// Extra request headers, applied last.
    pub headers: BTreeMap<String, String>,
    /// Model metadata to use instead of the provider's catalog entry.
    pub model_spec: Option<ModelSpec>,
}

/// Incremental updates while an assistant message streams.
#[derive(Debug, Clone, PartialEq)]
pub enum StreamEvent {
    /// A new content block was opened at `index`.
    BlockStart {
        index: usize,
        block: ContentBlock,
    },
    TextDelta {
        index: usize,
        delta: String,
    },
    ThinkingDelta {
        index: usize,
        delta: String,
    },
    /// Raw JSON argument text for the tool call at `index`.
    ToolArgsDelta {
        index: usize,
        delta: String,
    },
}

/// The model record a request runs against.
pub fn resolve_model(provider: &ProviderConfig, request: &Request) -> ModelSpec {
    request
        .model_spec
        .clone()
        .unwrap_or_else(|| provider.model_spec(&request.model))
}

/// The level sent to the provider: `off` stays off (Pi passes no reasoning
/// option), anything else is clamped to what the model supports.
pub(crate) fn request_level(model: &ModelSpec, level: ThinkingLevel) -> ThinkingLevel {
    if level == ThinkingLevel::Off || !model.reasoning {
        ThinkingLevel::Off
    } else {
        model.clamp_thinking(level)
    }
}

/// Output budget: the request's (bounded by the model) or Pi's default.
pub(crate) fn effective_max_tokens(request: &Request, model: &ModelSpec) -> u32 {
    let model_max = u32::try_from(model.max_tokens).unwrap_or(u32::MAX);
    match (request.max_tokens, model_max) {
        (0, 0) => 16_384,
        (0, m) => m.min(32_000),
        (r, 0) => r,
        (r, m) => r.min(m),
    }
}

/// Stream one assistant response.
pub async fn stream(
    http: &reqwest::Client,
    provider: &ProviderConfig,
    request: &Request,
    cancel: &CancellationToken,
    on_event: &mut (dyn FnMut(StreamEvent) + Send),
) -> AssistantMessage {
    if let Some(handle) = &provider.backend {
        return handle
            .backend()
            .stream(provider, request, cancel, on_event)
            .await;
    }
    let model = resolve_model(provider, request);
    let kind = ProviderKind::from_api(&model.api).unwrap_or(provider.kind);
    let mut partial = PartialMessage::new(provider, &request.model);
    partial.message.api = if ProviderKind::from_api(&model.api).is_some() || model.api.is_empty() {
        kind.api_name().to_string()
    } else {
        model.api.clone()
    };
    let result = async {
        if ProviderKind::from_api(&model.api).is_none() && !model.api.is_empty() {
            anyhow::bail!(
                "The {} API used by {}/{} is not supported by qt",
                model.api,
                provider.id,
                model.id
            );
        }
        let mut provider = provider.clone();
        if let Some(binding) = &provider.oauth {
            let access = crate::auth::fresh_access_token(http, &provider.id, binding).await?;
            provider.api_key = Some(access);
        }
        match kind {
            ProviderKind::AnthropicMessages => {
                anthropic::stream(
                    http,
                    &provider,
                    &model,
                    request,
                    cancel,
                    &mut partial,
                    on_event,
                )
                .await
            }
            ProviderKind::OpenAiCompletions => {
                openai::stream(
                    http,
                    &provider,
                    &model,
                    request,
                    cancel,
                    &mut partial,
                    on_event,
                )
                .await
            }
            ProviderKind::OpenAiResponses
            | ProviderKind::OpenAiCodexResponses
            | ProviderKind::AzureOpenAiResponses => {
                responses::stream(
                    http,
                    &provider,
                    &model,
                    kind,
                    request,
                    cancel,
                    &mut partial,
                    on_event,
                )
                .await
            }
        }
    }
    .await;
    partial.apply_cost(&model);
    match result {
        Ok(()) if cancel.is_cancelled() => partial.finish_aborted(),
        Ok(()) => partial.finish(),
        Err(_) if cancel.is_cancelled() => partial.finish_aborted(),
        Err(err) => partial.finish_error(format!("{err:#}")),
    }
}

/// Accumulates streamed content into an [`AssistantMessage`].
#[derive(Debug)]
pub struct PartialMessage {
    pub message: AssistantMessage,
    /// Raw JSON argument text per content index.
    tool_args: Vec<(usize, String)>,
    stop: Option<StopReason>,
    /// Error reported in-band (for example `finish_reason: content_filter`).
    stop_error: Option<String>,
    /// Cost reported by the provider (OpenRouter), used when the model has no prices.
    pub(crate) reported_cost: Option<f64>,
    /// Price multiplier from the effective service tier.
    pub(crate) cost_multiplier: f64,
}

impl PartialMessage {
    pub fn new(provider: &ProviderConfig, model: &str) -> Self {
        Self {
            message: AssistantMessage {
                api: provider.kind.api_name().into(),
                provider: provider.id.clone(),
                model: model.to_string(),
                ..Default::default()
            },
            tool_args: Vec::new(),
            stop: None,
            stop_error: None,
            reported_cost: None,
            cost_multiplier: 1.0,
        }
    }

    pub fn open(
        &mut self,
        block: ContentBlock,
        on_event: &mut (dyn FnMut(StreamEvent) + Send),
    ) -> usize {
        let index = self.message.content.len();
        if matches!(block, ContentBlock::ToolCall { .. }) {
            self.tool_args.push((index, String::new()));
        }
        self.message.content.push(block.clone());
        on_event(StreamEvent::BlockStart { index, block });
        index
    }

    pub fn push_text(
        &mut self,
        index: usize,
        delta: &str,
        on_event: &mut (dyn FnMut(StreamEvent) + Send),
    ) {
        if delta.is_empty() {
            return;
        }
        if let Some(ContentBlock::Text { text }) = self.message.content.get_mut(index) {
            text.push_str(delta);
            on_event(StreamEvent::TextDelta {
                index,
                delta: delta.to_string(),
            });
        }
    }

    pub fn push_thinking(
        &mut self,
        index: usize,
        delta: &str,
        on_event: &mut (dyn FnMut(StreamEvent) + Send),
    ) {
        if delta.is_empty() {
            return;
        }
        if let Some(ContentBlock::Thinking { thinking, .. }) = self.message.content.get_mut(index) {
            thinking.push_str(delta);
            on_event(StreamEvent::ThinkingDelta {
                index,
                delta: delta.to_string(),
            });
        }
    }

    pub fn set_signature(&mut self, index: usize, sig: &str) {
        if let Some(ContentBlock::Thinking { signature, .. }) = self.message.content.get_mut(index)
        {
            signature.get_or_insert_with(String::new).push_str(sig);
        }
    }

    pub fn push_tool_args(
        &mut self,
        index: usize,
        delta: &str,
        on_event: &mut (dyn FnMut(StreamEvent) + Send),
    ) {
        if let Some((_, args)) = self.tool_args.iter_mut().find(|(i, _)| *i == index) {
            args.push_str(delta);
            on_event(StreamEvent::ToolArgsDelta {
                index,
                delta: delta.to_string(),
            });
        }
    }

    /// Raw arguments streamed so far for the tool call at `index`.
    pub fn tool_args(&self, index: usize) -> Option<&str> {
        self.tool_args
            .iter()
            .find(|(i, _)| *i == index)
            .map(|(_, a)| a.as_str())
    }

    /// Replace the raw arguments of the tool call at `index`.
    pub fn set_tool_args(&mut self, index: usize, raw: &str) {
        if let Some((_, args)) = self.tool_args.iter_mut().find(|(i, _)| *i == index) {
            *args = raw.to_string();
        }
    }

    pub fn set_stop(&mut self, stop: StopReason) {
        self.stop = Some(stop);
    }

    /// Mark the response as failed with `message` once streaming ends.
    pub fn set_stop_error(&mut self, message: String) {
        self.stop = Some(StopReason::Error);
        self.stop_error = Some(message);
    }

    fn apply_cost(&mut self, model: &ModelSpec) {
        let usage = &mut self.message.usage;
        if usage.total_tokens == 0 {
            usage.total_tokens = usage.input_tokens
                + usage.output_tokens
                + usage.cache_read_tokens
                + usage.cache_write_tokens;
        }
        usage.total_cost_usd = if !model.cost.is_zero() {
            model.cost.compute(usage) * self.cost_multiplier
        } else {
            self.reported_cost.unwrap_or(0.0)
        };
    }

    fn finalize_tool_args(&mut self) {
        for (index, raw) in std::mem::take(&mut self.tool_args) {
            if let Some(ContentBlock::ToolCall { arguments, .. }) =
                self.message.content.get_mut(index)
            {
                *arguments = parse_tool_arguments(&raw);
            }
        }
    }

    pub fn finish(mut self) -> AssistantMessage {
        self.finalize_tool_args();
        if let Some(error) = self.stop_error.take() {
            self.message.stop_reason = StopReason::Error;
            self.message.error_message = Some(error);
            self.message.timestamp = Some(now_millis());
            return self.message;
        }
        let has_calls = self.message.tool_calls().next().is_some();
        self.message.stop_reason = match self.stop {
            Some(StopReason::Stop) | None if has_calls => StopReason::ToolCall,
            Some(stop) => stop,
            None => StopReason::Stop,
        };
        if self.message.stop_reason == StopReason::Error && self.message.error_message.is_none() {
            self.message.error_message = Some("An unknown error occurred".into());
        }
        self.message.timestamp = Some(now_millis());
        self.message
    }

    pub fn finish_aborted(mut self) -> AssistantMessage {
        self.finalize_tool_args();
        self.message.stop_reason = StopReason::Aborted;
        self.message.error_message = Some("Request was aborted".into());
        self.message.timestamp = Some(now_millis());
        self.message
    }

    pub fn finish_error(mut self, error: String) -> AssistantMessage {
        self.finalize_tool_args();
        self.message.stop_reason = StopReason::Error;
        self.message.error_message = Some(error);
        self.message.timestamp = Some(now_millis());
        self.message
    }
}

/// Parse streamed tool arguments; invalid JSON is kept as a string so the
/// tool reports a validation error to the model instead of silently running.
pub fn parse_tool_arguments(raw: &str) -> Value {
    if raw.trim().is_empty() {
        return Value::Object(Default::default());
    }
    serde_json::from_str(raw).unwrap_or_else(|_| Value::String(raw.to_string()))
}

/// Make a history safe to send: drop tool calls from failed or aborted
/// assistant turns, drop tool results without a matching call, and drop
/// assistant messages left empty.
pub fn sanitize_history(messages: &[Message]) -> Vec<Message> {
    let mut out = Vec::with_capacity(messages.len());
    let mut open_calls: HashSet<String> = HashSet::new();
    for message in messages {
        match message {
            Message::Assistant(a) => {
                let failed = matches!(a.stop_reason, StopReason::Error | StopReason::Aborted);
                let content: Vec<ContentBlock> = a
                    .content
                    .iter()
                    .filter(|b| match b {
                        ContentBlock::ToolCall { .. } => !failed,
                        ContentBlock::Text { text } => !text.is_empty(),
                        ContentBlock::Thinking {
                            thinking, redacted, ..
                        } => !thinking.is_empty() || *redacted,
                        ContentBlock::Image { .. } => true,
                    })
                    .cloned()
                    .collect();
                if content.is_empty() {
                    continue;
                }
                open_calls.clear();
                for block in &content {
                    if let ContentBlock::ToolCall { id, .. } = block {
                        open_calls.insert(id.clone());
                    }
                }
                out.push(Message::Assistant(AssistantMessage {
                    content,
                    ..a.clone()
                }));
            }
            Message::ToolResult(r) => {
                if open_calls.remove(&r.tool_call_id) {
                    out.push(message.clone());
                }
            }
            Message::User { .. } => {
                open_calls.clear();
                out.push(message.clone());
            }
        }
    }
    out
}

/// Read an error body from a failed HTTP response into a short message.
pub(crate) async fn http_error(response: reqwest::Response) -> anyhow::Error {
    let status = response.status().as_u16();
    let body = response.text().await.unwrap_or_default();
    anyhow::anyhow!(http::HttpFailure { status, body }.message())
}

/// Replace characters that cannot be sent as JSON text: lone surrogates
/// cannot occur in Rust strings, but NUL and other C0 controls except tab,
/// newline and carriage return are stripped as Pi's `sanitizeSurrogates` +
/// provider-safe text handling do.
pub(crate) fn sanitize_text(text: &str) -> String {
    if !text
        .chars()
        .any(|c| c.is_control() && !matches!(c, '\t' | '\n' | '\r'))
    {
        return text.to_string();
    }
    text.chars()
        .filter(|c| !c.is_control() || matches!(c, '\t' | '\n' | '\r'))
        .collect()
}

/// Substitute `{VAR}` placeholders in a base URL from the environment.
pub(crate) fn resolve_url_placeholders(url: &str, provider: &str) -> anyhow::Result<String> {
    let mut out = String::with_capacity(url.len());
    let mut rest = url;
    while let Some(open) = rest.find('{') {
        out.push_str(&rest[..open]);
        let after = &rest[open + 1..];
        let Some(close) = after.find('}') else {
            out.push_str(&rest[open..]);
            return Ok(out);
        };
        let name = &after[..close];
        match std::env::var(name).ok().filter(|v| !v.is_empty()) {
            Some(value) => out.push_str(&value),
            None => anyhow::bail!("{name} is required for provider {provider} but is not set."),
        }
        rest = &after[close + 1..];
    }
    out.push_str(rest);
    Ok(out)
}

/// Apply headers to a request builder.
pub(crate) fn with_headers(
    mut builder: reqwest::RequestBuilder,
    headers: &BTreeMap<String, String>,
) -> reqwest::RequestBuilder {
    for (k, v) in headers {
        builder = builder.header(k.as_str(), v.as_str());
    }
    builder
}

#[cfg(test)]
pub(crate) mod test_server;

#[cfg(test)]
mod tests {
    use super::*;
    use crate::message::ToolResultMessage;

    #[test]
    fn model_ref_parses_nested_ids() {
        let r = ModelRef::parse("openrouter/~deepseek/deepseek-flash-latest").unwrap();
        assert_eq!(r.provider, "openrouter");
        assert_eq!(r.id, "~deepseek/deepseek-flash-latest");
        assert!(ModelRef::parse("nomodel").is_none());
    }

    #[test]
    fn thinking_levels_parse_and_budget() {
        assert_eq!(ThinkingLevel::parse("XHigh"), Some(ThinkingLevel::XHigh));
        assert_eq!(
            ThinkingLevel::parse("minimal"),
            Some(ThinkingLevel::Minimal)
        );
        assert_eq!(ThinkingLevel::parse("max"), None);
        assert_eq!(ThinkingLevel::Minimal.budget_tokens(), Some(1_024));
        assert_eq!(ThinkingLevel::XHigh.budget_tokens(), Some(16_384));
        assert_eq!(ThinkingLevel::Off.budget_tokens(), None);
    }

    #[test]
    fn sanitize_drops_orphans() {
        let aborted = Message::Assistant(AssistantMessage {
            content: vec![ContentBlock::ToolCall {
                id: "a".into(),
                name: "read".into(),
                arguments: Value::Null,
            }],
            stop_reason: StopReason::Aborted,
            ..Default::default()
        });
        let orphan = Message::ToolResult(ToolResultMessage {
            tool_call_id: "a".into(),
            ..Default::default()
        });
        let good_call = Message::Assistant(AssistantMessage {
            content: vec![ContentBlock::ToolCall {
                id: "b".into(),
                name: "read".into(),
                arguments: Value::Null,
            }],
            stop_reason: StopReason::ToolCall,
            ..Default::default()
        });
        let good_result = Message::ToolResult(ToolResultMessage {
            tool_call_id: "b".into(),
            ..Default::default()
        });
        let out = sanitize_history(&[
            Message::user_text("hi"),
            aborted,
            orphan,
            good_call,
            good_result,
        ]);
        assert_eq!(out.len(), 3);
    }

    #[test]
    fn finish_infers_tool_call_stop() {
        let cfg = ProviderConfig {
            id: "x".into(),
            ..Default::default()
        };
        let mut p = PartialMessage::new(&cfg, "m");
        let mut sink = |_e: StreamEvent| {};
        let i = p.open(
            ContentBlock::ToolCall {
                id: "1".into(),
                name: "read".into(),
                arguments: Value::Null,
            },
            &mut sink,
        );
        p.push_tool_args(i, "{\"path\":", &mut sink);
        p.push_tool_args(i, "\"a\"}", &mut sink);
        let m = p.finish();
        assert_eq!(m.stop_reason, StopReason::ToolCall);
        let (_, _, args) = m.tool_calls().next().unwrap();
        assert_eq!(args["path"], "a");
    }

    #[test]
    fn max_tokens_follow_model_limits() {
        let mut model = ModelSpec::fallback("p", "openai-completions", "", "m");
        model.max_tokens = 128_000;
        let mut request = Request::default();
        assert_eq!(effective_max_tokens(&request, &model), 32_000);
        request.max_tokens = 200_000;
        assert_eq!(effective_max_tokens(&request, &model), 128_000);
        request.max_tokens = 1_000;
        assert_eq!(effective_max_tokens(&request, &model), 1_000);
    }

    #[test]
    fn url_placeholders_resolve_from_env() {
        assert_eq!(
            resolve_url_placeholders("https://x/{PATH}/y", "p").unwrap(),
            format!("https://x/{}/y", std::env::var("PATH").unwrap())
        );
        let err = resolve_url_placeholders("https://x/{QT_TEST_UNSET_VAR_123}", "cf")
            .unwrap_err()
            .to_string();
        assert_eq!(
            err,
            "QT_TEST_UNSET_VAR_123 is required for provider cf but is not set."
        );
    }

    #[test]
    fn providers_come_from_catalog_and_credentials() {
        let dir = tempfile::tempdir().unwrap();
        let home = AgentHome::new(dir.path());
        std::fs::write(
            home.settings_path(),
            r#"{"ollama":{"instances":[{"id":"gpu","name":"GPU box","baseUrl":"http://10.0.0.2:11434/v1/"}]},
                "retry":{"provider":{"maxRetries":5}}}"#,
        )
        .unwrap();
        std::fs::write(
            home.auth_path(),
            r#"{"version":1,"providers":{"zai":{"apiKey":"zk"},
                "openai-codex":{"token":"tok","metadata":{"type":"oauth","access":"tok","refresh":"r","expires":1,"accountId":"acc"}}}}"#,
        )
        .unwrap();
        let settings = Settings::load(&home).unwrap();
        let auth = AuthStore::load(&home).unwrap();
        let providers = configured_providers(&settings, &auth);
        let get = |id: &str| providers.iter().find(|p| p.id == id).cloned();
        let zai = get("zai").unwrap();
        assert!(zai.is_ready());
        assert_eq!(zai.retry.max_retries, 5);
        assert!(zai.models.iter().any(|m| m.id == "glm-4.5-air"));
        let codex = get("openai-codex").unwrap();
        assert_eq!(codex.kind, ProviderKind::OpenAiCodexResponses);
        assert_eq!(codex.oauth.as_ref().unwrap().credentials.refresh, "r");
        let ollama = get("ollama:gpu").unwrap();
        assert_eq!(ollama.base_url, "http://10.0.0.2:11434/v1");
        assert_eq!(ollama.label, "Ollama · GPU box");
        assert!(ollama.is_ready() && !ollama.needs_api_key());
        assert!(get("google").is_none());
        assert!(get("github-copilot").is_none());
        assert!(get("xai").is_none());
        assert!(get("azure-openai-responses").is_some());
    }
}
