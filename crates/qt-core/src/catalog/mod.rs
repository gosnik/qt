//! Model catalog: built-in providers and models, `models.json` overrides,
//! discovered models (OpenRouter, OpenAI, Ollama), thinking-level clamping and
//! cost accounting.
//!
//! Matching follows the archived qt: provider and model ids compare
//! case-insensitively after trimming.

mod data;
pub mod openai;
pub mod openrouter;

use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::path::Path;
use std::sync::{Mutex, OnceLock};
use std::time::Duration;

use anyhow::{Result, bail};
use serde_json::{Map, Value};

use crate::config::{AgentHome, Settings};
use crate::message::Usage;
use crate::provider::ThinkingLevel;

/// Prices in USD per million tokens.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct Cost {
    pub input: f64,
    pub output: f64,
    pub cache_read: f64,
    pub cache_write: f64,
}

impl Cost {
    pub fn is_zero(&self) -> bool {
        self.input == 0.0 && self.output == 0.0 && self.cache_read == 0.0 && self.cache_write == 0.0
    }

    /// Total USD cost of `usage` at these rates.
    pub fn compute(&self, usage: &Usage) -> f64 {
        (self.input * usage.input_tokens as f64
            + self.output * usage.output_tokens as f64
            + self.cache_read * usage.cache_read_tokens as f64
            + self.cache_write * usage.cache_write_tokens as f64)
            / 1_000_000.0
    }

    fn from_json(value: &Value, base: Cost) -> Cost {
        let get = |primary: &str, fallback: &str, current: f64| {
            value
                .get(primary)
                .and_then(Value::as_f64)
                .or_else(|| value.get(fallback).and_then(Value::as_f64))
                .unwrap_or(current)
        };
        Cost {
            input: get("input", "input_usd_per_million", base.input),
            output: get("output", "output_usd_per_million", base.output),
            cache_read: get("cacheRead", "cache_read_usd_per_million", base.cache_read),
            cache_write: get(
                "cacheWrite",
                "cache_write_usd_per_million",
                base.cache_write,
            ),
        }
    }

    pub fn to_json(&self) -> Value {
        serde_json::json!({
            "input": self.input,
            "output": self.output,
            "cacheRead": self.cache_read,
            "cacheWrite": self.cache_write,
        })
    }
}

/// A model and everything a transport needs to call it.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct ModelSpec {
    pub provider: String,
    pub id: String,
    pub name: String,
    /// Wire API (`anthropic-messages`, `openai-completions`, `openai-responses`, ...).
    pub api: String,
    pub base_url: String,
    pub reasoning: bool,
    /// Input modalities: `"text"` and optionally `"image"`.
    pub input: Vec<String>,
    pub cost: Cost,
    pub context_window: u64,
    pub max_tokens: u64,
    /// Pi thinking level -> provider value; `None` marks a level unsupported.
    pub thinking_level_map: BTreeMap<String, Option<String>>,
    /// API-specific compatibility overrides (Pi `compat`).
    pub compat: Map<String, Value>,
    pub headers: BTreeMap<String, String>,
    /// Configured key: a literal, an environment variable name or `!command`.
    pub api_key: Option<String>,
    /// Send `Authorization: Bearer <key>` in addition to the API's own auth.
    pub auth_header: bool,
    pub timeout_ms: Option<u64>,
    /// Extra metadata (Ollama details, discovery markers, ...).
    pub metadata: Map<String, Value>,
}

impl ModelSpec {
    pub fn supports_images(&self) -> bool {
        self.input.iter().any(|i| i == "image")
    }

    pub fn display_name(&self) -> &str {
        if self.name.is_empty() {
            &self.id
        } else {
            &self.name
        }
    }

    /// Thinking levels this model accepts (Pi `getSupportedThinkingLevels`).
    pub fn thinking_levels(&self) -> Vec<ThinkingLevel> {
        if !self.reasoning {
            return vec![ThinkingLevel::Off];
        }
        ThinkingLevel::ALL
            .into_iter()
            .filter(|level| match self.thinking_level_map.get(level.as_str()) {
                Some(None) => false,
                Some(Some(_)) => true,
                None => *level != ThinkingLevel::XHigh,
            })
            .collect()
    }

    /// Nearest supported level, searching upwards first (Pi `clampThinkingLevel`).
    pub fn clamp_thinking(&self, level: ThinkingLevel) -> ThinkingLevel {
        let available = self.thinking_levels();
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

    /// Provider value for `level` from the thinking map: `Some(Some(v))` when
    /// mapped, `Some(None)` when explicitly unsupported, `None` when unmapped.
    pub fn mapped_level(&self, level: ThinkingLevel) -> Option<Option<&str>> {
        self.thinking_level_map
            .get(level.as_str())
            .map(|v| v.as_deref())
    }

    /// The effort string sent for `level`: the mapped value or the level name.
    pub fn effort_for(&self, level: ThinkingLevel) -> String {
        match self.mapped_level(level) {
            Some(Some(v)) => v.to_string(),
            _ => level.as_str().to_string(),
        }
    }

    pub fn compat_bool(&self, key: &str) -> Option<bool> {
        self.compat.get(key).and_then(Value::as_bool)
    }

    pub fn compat_str(&self, key: &str) -> Option<&str> {
        self.compat.get(key).and_then(Value::as_str)
    }

    /// A permissive record for a model id the catalog does not know.
    pub fn fallback(provider: &str, api: &str, base_url: &str, id: &str) -> Self {
        Self {
            provider: provider.into(),
            id: id.into(),
            name: id.into(),
            api: api.into(),
            base_url: base_url.into(),
            reasoning: true,
            input: vec!["text".into(), "image".into()],
            context_window: 128_000,
            max_tokens: 16_384,
            ..Self::default()
        }
    }
}

/// A provider record.
#[derive(Debug, Clone, PartialEq)]
pub struct ProviderSpec {
    pub id: String,
    pub name: String,
    /// Default wire API for the provider's models.
    pub api: String,
    pub base_url: String,
    pub headers: BTreeMap<String, String>,
    /// Configured key (literal, env var name or `!command`).
    pub api_key: Option<String>,
    pub auth_header: bool,
    pub compat: Map<String, Value>,
    /// Other wire APIs used by this provider's models.
    pub accepted_apis: Vec<String>,
    /// Why the provider cannot be used, when it has no transport.
    pub unsupported: Option<String>,
    pub oauth: Option<Value>,
    /// Local providers (Ollama) work without credentials.
    pub requires_auth: bool,
    pub metadata: Map<String, Value>,
}

impl Default for ProviderSpec {
    fn default() -> Self {
        Self {
            id: String::new(),
            name: String::new(),
            api: String::new(),
            base_url: String::new(),
            headers: BTreeMap::new(),
            api_key: None,
            auth_header: false,
            compat: Map::new(),
            accepted_apis: Vec::new(),
            unsupported: None,
            oauth: None,
            requires_auth: true,
            metadata: Map::new(),
        }
    }
}

impl ProviderSpec {
    pub fn accepts_api(&self, api: &str) -> bool {
        if key(&self.id) == "openrouter" {
            return !api.is_empty();
        }
        self.api == api || self.accepted_apis.iter().any(|a| a == api)
    }
}

/// An OpenRouter image-generation model.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct ImageModelSpec {
    pub provider: String,
    pub api: String,
    pub id: String,
    pub name: String,
    pub base_url: String,
    pub input: Vec<String>,
    pub output: Vec<String>,
    pub cost: Cost,
    pub headers: BTreeMap<String, String>,
}

/// Providers and models known to qt.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct ModelCatalog {
    pub providers: Vec<ProviderSpec>,
    pub models: Vec<ModelSpec>,
}

fn key(id: &str) -> String {
    id.trim().to_lowercase()
}

fn model_key(provider: &str, id: &str) -> (String, String) {
    (key(provider), key(id))
}

impl ModelCatalog {
    /// The built-in catalog (cached; clone to modify).
    pub fn builtin() -> &'static ModelCatalog {
        static BUILTIN: OnceLock<ModelCatalog> = OnceLock::new();
        BUILTIN.get_or_init(|| {
            let mut catalog = ModelCatalog {
                providers: data::providers(),
                models: data::models(),
            };
            catalog.normalize();
            catalog
        })
    }

    /// Fill inherited base URLs and sort providers and models by id.
    pub fn normalize(&mut self) {
        for provider in &mut self.providers {
            provider.id = provider.id.trim().to_string();
            provider.name = provider.name.trim().to_string();
            provider.api = provider.api.trim().to_string();
        }
        let bases: HashMap<String, String> = self
            .providers
            .iter()
            .map(|p| (key(&p.id), p.base_url.clone()))
            .collect();
        for model in &mut self.models {
            model.provider = model.provider.trim().to_string();
            model.id = model.id.trim().to_string();
            model.name = model.name.trim().to_string();
            if model.base_url.is_empty()
                && let Some(base) = bases.get(&key(&model.provider))
            {
                model.base_url = base.clone();
            }
            if model.input.is_empty() {
                model.input = vec!["text".into()];
            }
        }
        self.providers
            .sort_by(|a, b| (key(&a.id), &a.id).cmp(&(key(&b.id), &b.id)));
        self.models.sort_by(|a, b| {
            (model_key(&a.provider, &a.id), &a.provider, &a.id).cmp(&(
                model_key(&b.provider, &b.id),
                &b.provider,
                &b.id,
            ))
        });
    }

    /// Image-generation models.
    pub fn image_models() -> &'static [ImageModelSpec] {
        static IMAGES: OnceLock<Vec<ImageModelSpec>> = OnceLock::new();
        IMAGES.get_or_init(|| {
            let mut list = data::image_models();
            list.sort_by_key(|m| (key(&m.provider), key(&m.id)));
            list
        })
    }

    pub fn find_image_model(provider: &str, id: &str) -> Option<&'static ImageModelSpec> {
        Self::image_models()
            .iter()
            .find(|m| key(&m.provider) == key(provider) && key(&m.id) == key(id))
    }

    pub fn find_provider(&self, id: &str) -> Option<&ProviderSpec> {
        let wanted = key(id);
        if wanted.is_empty() {
            return None;
        }
        self.providers.iter().find(|p| key(&p.id) == wanted)
    }

    pub fn find_model(&self, provider: &str, id: &str) -> Option<&ModelSpec> {
        let wanted = model_key(provider, id);
        if wanted.0.is_empty() || wanted.1.is_empty() {
            return None;
        }
        self.models
            .iter()
            .find(|m| model_key(&m.provider, &m.id) == wanted)
    }

    pub fn models_for<'a>(&'a self, provider: &str) -> impl Iterator<Item = &'a ModelSpec> + 'a {
        let wanted = key(provider);
        self.models
            .iter()
            .filter(move |m| key(&m.provider) == wanted)
    }

    /// Resolve `provider/model` or a bare model id (unique match only).
    pub fn match_model(&self, reference: &str) -> Option<&ModelSpec> {
        let trimmed = reference.trim();
        if trimmed.is_empty() {
            return None;
        }
        let canonical = trimmed.to_lowercase();
        if let Some(m) = self
            .models
            .iter()
            .find(|m| format!("{}/{}", m.provider, m.id).to_lowercase() == canonical)
        {
            return Some(m);
        }
        let (provider, model) = match trimmed.split_once('/') {
            Some((p, m)) if !p.trim().is_empty() && !m.trim().is_empty() => {
                (Some(p.trim()), m.trim())
            }
            _ => (None, trimmed),
        };
        if let Some(provider) = provider
            && self.find_provider(provider).is_some()
        {
            return self
                .find_model(provider, model)
                .or_else(|| self.find_model(provider, trimmed));
        }
        let wanted = key(model);
        let mut matches = self.models.iter().filter(|m| key(&m.id) == wanted);
        let first = matches.next()?;
        matches.next().is_none().then_some(first)
    }

    /// Overlay providers and models, replacing records with the same id.
    pub fn merge(&mut self, overlay: ModelCatalog) {
        for provider in overlay.providers {
            match self
                .providers
                .iter_mut()
                .find(|p| key(&p.id) == key(&provider.id))
            {
                Some(existing) => *existing = provider,
                None => self.providers.push(provider),
            }
        }
        for model in overlay.models {
            let k = model_key(&model.provider, &model.id);
            match self
                .models
                .iter_mut()
                .find(|m| model_key(&m.provider, &m.id) == k)
            {
                Some(existing) => *existing = model,
                None => self.models.push(model),
            }
        }
        self.normalize();
    }

    /// Remove every model of `provider`.
    pub fn remove_models_of(&mut self, provider: &str) {
        let wanted = key(provider);
        self.models.retain(|m| key(&m.provider) != wanted);
    }

    /// Apply one `models.json` provider entry (Pi custom provider config).
    pub fn apply_provider_config(&mut self, provider_id: &str, config: &Value) -> Result<()> {
        let overlay = provider_config_overlay(self, provider_id, config)?;
        let replaces_models = config
            .get("models")
            .and_then(Value::as_array)
            .is_some_and(|m| !m.is_empty());
        if replaces_models {
            self.remove_models_of(provider_id);
        }
        self.merge(overlay);
        Ok(())
    }

    /// Apply a whole `models.json` document.
    pub fn apply_models_json(&mut self, config: &Value) -> Result<()> {
        let Some(object) = config.as_object() else {
            bail!("models.json must be a JSON object");
        };
        let Some(providers) = object.get("providers").and_then(Value::as_object) else {
            bail!("models.json providers must be an object");
        };
        for (id, entry) in providers {
            self.apply_provider_config(id, entry)
                .map_err(|e| anyhow::anyhow!("models.json providers.{id}: {e}"))?;
        }
        Ok(())
    }

    /// Validation messages (empty when the catalog is consistent).
    pub fn validate(&self) -> Vec<String> {
        let mut errors = Vec::new();
        let mut seen = BTreeSet::new();
        for provider in &self.providers {
            if provider.id.is_empty() {
                errors.push("provider descriptor.id is required".to_string());
            }
            if provider.name.is_empty() {
                errors.push("provider descriptor.display_name is required".to_string());
            }
            if provider.api.is_empty() {
                errors.push("provider descriptor.api is required".to_string());
            }
            if !seen.insert(key(&provider.id)) {
                errors.push(format!("duplicate provider id: {}", provider.id));
            }
        }
        let mut seen = BTreeSet::new();
        for model in &self.models {
            match self.find_provider(&model.provider) {
                None => errors.push(format!(
                    "model references unknown provider: {}/{}",
                    model.provider, model.id
                )),
                Some(p) if !p.accepts_api(&model.api) => errors.push(format!(
                    "model api does not match provider api: {}/{}",
                    model.provider, model.id
                )),
                _ => {}
            }
            if !seen.insert(model_key(&model.provider, &model.id)) {
                errors.push(format!(
                    "duplicate model ref: {}/{}",
                    model.provider, model.id
                ));
            }
        }
        errors
    }

    /// Built-in catalog plus the cached OpenRouter list, `settings.providers`
    /// base URL overrides, Ollama instances and `models.json`. A malformed
    /// `models.json` is reported and otherwise ignored.
    pub fn load(home: &AgentHome, settings: &Settings) -> (ModelCatalog, Option<String>) {
        let mut catalog = Self::builtin().clone();
        let cached = openrouter::load_cache(home, true);
        if !cached.models.is_empty() {
            let mut overlay = cached;
            overlay.providers.clear();
            catalog.merge(overlay);
        }
        if let Some(overrides) = settings.raw().get("providers").and_then(Value::as_object) {
            for (id, entry) in overrides {
                if let Some(base) = entry.get("baseUrl").and_then(Value::as_str)
                    && catalog.find_provider(id).is_some()
                {
                    let config = serde_json::json!({ "baseUrl": base.trim_end_matches('/') });
                    let _ = catalog.apply_provider_config(id, &config);
                }
            }
        }
        catalog.merge(crate::ollama::catalog_from_instances(
            &settings.ollama_settings(),
        ));
        let error = match read_models_json(&home.models_path()) {
            Ok(Some(config)) => catalog
                .apply_models_json(&config)
                .err()
                .map(|e| format!("{e:#}")),
            Ok(None) => None,
            Err(e) => Some(format!("{e:#}")),
        };
        (catalog, error)
    }
}

fn read_models_json(path: &Path) -> Result<Option<Value>> {
    match std::fs::read_to_string(path) {
        Ok(text) if text.trim().is_empty() => Ok(None),
        Ok(text) => Ok(Some(serde_json::from_str(&text).map_err(|e| {
            anyhow::anyhow!("invalid models.json at {}: {e}", path.display())
        })?)),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(e.into()),
    }
}

fn opt_str(value: &Value, field: &str) -> Option<String> {
    value
        .get(field)
        .and_then(Value::as_str)
        .map(str::trim)
        .filter(|s| !s.is_empty())
        .map(str::to_string)
}

fn positive_int(value: &Value, field: &str) -> Option<u64> {
    value.get(field).and_then(Value::as_u64).filter(|v| *v > 0)
}

fn string_map(value: Option<&Value>) -> BTreeMap<String, String> {
    value
        .and_then(Value::as_object)
        .map(|o| {
            o.iter()
                .filter_map(|(k, v)| v.as_str().map(|s| (k.clone(), s.to_string())))
                .collect()
        })
        .unwrap_or_default()
}

fn string_list(value: Option<&Value>) -> Option<Vec<String>> {
    let list: Vec<String> = value?
        .as_array()?
        .iter()
        .filter_map(Value::as_str)
        .map(str::to_string)
        .collect();
    (!list.is_empty()).then_some(list)
}

fn level_map(value: &Value) -> BTreeMap<String, Option<String>> {
    value
        .as_object()
        .map(|o| {
            o.iter()
                .map(|(k, v)| (k.clone(), v.as_str().map(str::to_string)))
                .collect()
        })
        .unwrap_or_default()
}

fn merged_compat(base: &Map<String, Value>, overlay: Option<&Value>) -> Map<String, Value> {
    let mut out = base.clone();
    if let Some(o) = overlay.and_then(Value::as_object) {
        for (k, v) in o {
            out.insert(k.clone(), v.clone());
        }
    }
    out
}

/// Apply a `modelOverrides` entry to a model.
fn apply_model_override(model: &mut ModelSpec, o: &Value) {
    if !o.is_object() {
        return;
    }
    if let Some(name) = opt_str(o, "name") {
        model.name = name;
    }
    if let Some(r) = o.get("reasoning").and_then(Value::as_bool) {
        model.reasoning = r;
    }
    if let Some(map) = o.get("thinkingLevelMap").filter(|v| v.is_object()) {
        model.thinking_level_map.extend(level_map(map));
    }
    if let Some(input) = string_list(o.get("input")) {
        model.input = input;
    }
    if let Some(v) = positive_int(o, "contextWindow") {
        model.context_window = v;
    }
    if let Some(v) = positive_int(o, "maxTokens") {
        model.max_tokens = v;
    }
    if let Some(cost) = o.get("cost").filter(|c| c.is_object()) {
        model.cost = Cost::from_json(cost, model.cost);
    }
    model.headers.extend(string_map(o.get("headers")));
    model.compat = merged_compat(&model.compat, o.get("compat"));
}

/// Port of the archived `model_catalog_from_provider_config`.
fn provider_config_overlay(
    base: &ModelCatalog,
    provider_id: &str,
    config: &Value,
) -> Result<ModelCatalog> {
    let provider = provider_id.trim();
    if provider.is_empty() {
        bail!("provider name is required");
    }
    if !config.is_object() {
        bail!("Provider {provider}: config must be a JSON object.");
    }
    if config.get("streamSimple").is_some() && opt_str(config, "api").is_none() {
        bail!("Provider {provider}: \"api\" is required when registering streamSimple.");
    }
    let models = config.get("models");
    if models.is_some_and(|m| !m.is_array()) {
        bail!("Provider {provider}: \"models\" must be an array.");
    }
    let models: &[Value] = models
        .and_then(Value::as_array)
        .map(Vec::as_slice)
        .unwrap_or(&[]);
    let overrides = config.get("modelOverrides");
    if overrides.is_some_and(|o| !o.is_object()) {
        bail!("Provider {provider}: \"modelOverrides\" must be an object.");
    }
    let overrides = overrides
        .and_then(Value::as_object)
        .filter(|o| !o.is_empty());

    let base_provider = base.find_provider(provider);
    let mut record = match base_provider {
        Some(p) => p.clone(),
        None => ProviderSpec {
            id: provider.to_string(),
            api: opt_str(config, "api").unwrap_or_else(|| "custom".into()),
            name: opt_str(config, "name").unwrap_or_else(|| provider.to_string()),
            ..ProviderSpec::default()
        },
    };
    if let Some(name) = opt_str(config, "name") {
        record.name = name;
    }
    if let Some(api) = opt_str(config, "api") {
        record.api = api;
    }
    let base_url = opt_str(config, "baseUrl");
    if let Some(url) = &base_url {
        record.base_url = url.clone();
    }
    let headers = config.get("headers").filter(|h| h.is_object());
    if let Some(h) = headers {
        record.headers = string_map(Some(h));
    }
    let api_key = config
        .get("apiKey")
        .and_then(Value::as_str)
        .map(str::to_string);
    if let Some(k) = &api_key {
        record.api_key = Some(k.clone());
    }
    let auth_header = config.get("authHeader").and_then(Value::as_bool);
    if let Some(a) = auth_header {
        record.auth_header = a;
    }
    if let Some(stream_simple) = config.get("streamSimple") {
        record
            .metadata
            .insert("streamSimple".into(), stream_simple.clone());
    }
    if let Some(oauth) = config.get("oauth").filter(|o| o.is_object()) {
        record.oauth = Some(oauth.clone());
        if opt_str(config, "name").is_none()
            && let Some(name) = opt_str(oauth, "name")
        {
            record.name = name;
        }
    }
    let compat = config.get("compat").filter(|c| c.is_object());
    if let Some(c) = compat {
        record.compat = c.as_object().cloned().unwrap_or_default();
    }
    if let Some(local) = config.get("requiresAuth").and_then(Value::as_bool) {
        record.requires_auth = local;
    }

    let mut overlay = ModelCatalog::default();
    if models.is_empty() {
        if base_provider.is_none()
            && overrides.is_none()
            && base_url.is_none()
            && headers.is_none()
            && compat.is_none()
        {
            bail!(
                "Provider {provider}: must specify \"baseUrl\", \"headers\", \"compat\", \"modelOverrides\", or \"models\"."
            );
        }
        let touches_models = config.get("baseUrl").is_some()
            || headers.is_some()
            || compat.is_some()
            || api_key.is_some()
            || auth_header.is_some()
            || overrides.is_some();
        overlay.providers.push(record);
        if touches_models {
            for model in base.models_for(provider) {
                let mut m = model.clone();
                if let Some(url) = &base_url {
                    m.base_url = url.clone();
                }
                if let Some(h) = headers {
                    m.headers.extend(string_map(Some(h)));
                }
                m.compat = merged_compat(&m.compat, compat);
                if let Some(k) = &api_key {
                    m.api_key = Some(k.clone());
                }
                if let Some(a) = auth_header {
                    m.auth_header = a;
                }
                if let Some(o) = overrides.and_then(|o| o.get(&model.id)) {
                    apply_model_override(&mut m, o);
                }
                overlay.models.push(m);
            }
        }
        return Ok(overlay);
    }

    if base_provider.is_none() && base_url.is_none() {
        bail!("Provider {provider}: \"baseUrl\" is required when defining models.");
    }
    if base_provider.is_none() && opt_str(config, "apiKey").is_none() && record.oauth.is_none() {
        bail!("Provider {provider}: \"apiKey\" or \"oauth\" is required when defining models.");
    }

    let mut accepted = BTreeSet::new();
    for entry in models {
        let Some(id) = opt_str(entry, "id") else {
            bail!("Provider {provider}: model missing \"id\".");
        };
        let api = opt_str(entry, "api")
            .or_else(|| opt_str(config, "api"))
            .or_else(|| base_provider.map(|_| record.api.clone()))
            .unwrap_or_default();
        if api.is_empty() {
            bail!("Provider {provider}, model {id}: no \"api\" specified.");
        }
        accepted.insert(api.clone());
        let context_window = positive_int(entry, "contextWindow");
        if entry.get("contextWindow").is_some() && context_window.is_none() {
            bail!("Provider {provider}, model {id}: invalid contextWindow.");
        }
        let max_tokens = positive_int(entry, "maxTokens");
        if entry.get("maxTokens").is_some() && max_tokens.is_none() {
            bail!("Provider {provider}, model {id}: invalid maxTokens.");
        }
        let mut headers_map = headers.map(|h| string_map(Some(h))).unwrap_or_default();
        headers_map.extend(string_map(entry.get("headers")));
        let model = ModelSpec {
            provider: provider.to_string(),
            name: opt_str(entry, "name").unwrap_or_else(|| id.clone()),
            id,
            api,
            base_url: opt_str(entry, "baseUrl")
                .or_else(|| base_url.clone())
                .unwrap_or_else(|| record.base_url.clone()),
            reasoning: entry
                .get("reasoning")
                .and_then(Value::as_bool)
                .unwrap_or(false),
            input: string_list(entry.get("input")).unwrap_or_else(|| vec!["text".into()]),
            cost: entry
                .get("cost")
                .map(|c| Cost::from_json(c, Cost::default()))
                .unwrap_or_default(),
            context_window: context_window.unwrap_or(128_000),
            max_tokens: max_tokens.unwrap_or(16_384),
            thinking_level_map: entry
                .get("thinkingLevelMap")
                .map(level_map)
                .unwrap_or_default(),
            compat: merged_compat(
                &compat
                    .and_then(Value::as_object)
                    .cloned()
                    .unwrap_or_default(),
                entry.get("compat"),
            ),
            headers: headers_map,
            api_key: api_key.clone(),
            auth_header: auth_header.unwrap_or(false),
            timeout_ms: positive_int(entry, "timeoutMs"),
            metadata: Map::new(),
        };
        overlay.models.push(model);
    }
    if let Some(first) = accepted.iter().next().cloned() {
        let keep_previous = base_provider.is_some() && !record.accepts_api(&first);
        record.api = first;
        if accepted.len() > 1 || keep_previous {
            record.accepted_apis = accepted.into_iter().collect();
        }
    }
    overlay.providers.push(record);
    overlay.normalize();
    Ok(overlay)
}

/// OpenAI service-tier price multiplier (Pi `getServiceTierCostMultiplier`).
pub fn service_tier_multiplier(model_id: &str, tier: Option<&str>) -> f64 {
    match tier {
        Some("flex") => 0.5,
        Some("priority") if model_id == "gpt-5.5" => 2.5,
        Some("priority") => 2.0,
        _ => 1.0,
    }
}

/// Codex reports `default` even when a flex/priority request was honoured.
pub fn resolve_codex_service_tier<'a>(
    response: Option<&'a str>,
    request: Option<&'a str>,
) -> Option<&'a str> {
    if response == Some("default") && matches!(request, Some("flex") | Some("priority")) {
        return request;
    }
    response.or(request)
}

/// Resolve a configured secret: `!command` runs the command (10 s timeout,
/// stdout trimmed, cached per process), an environment variable name yields
/// its value, anything else is used literally.
pub fn resolve_config_value(configured: &str) -> Option<String> {
    let trimmed = configured.trim();
    if trimmed.is_empty() {
        return None;
    }
    if let Some(command) = trimmed.strip_prefix('!') {
        static CACHE: OnceLock<Mutex<HashMap<String, Option<String>>>> = OnceLock::new();
        let cache = CACHE.get_or_init(Default::default);
        if let Some(hit) = cache.lock().expect("cache lock").get(trimmed) {
            return hit.clone();
        }
        let resolved = run_secret_command(command);
        cache
            .lock()
            .expect("cache lock")
            .insert(trimmed.to_string(), resolved.clone());
        return resolved;
    }
    if let Ok(value) = std::env::var(trimmed)
        && !value.is_empty()
    {
        return Some(value);
    }
    Some(trimmed.to_string())
}

fn run_secret_command(command: &str) -> Option<String> {
    use std::process::{Command, Stdio};
    #[cfg(windows)]
    let mut cmd = {
        let mut c = Command::new("cmd");
        c.args(["/C", command]);
        c
    };
    #[cfg(not(windows))]
    let mut cmd = {
        let mut c = Command::new("sh");
        c.args(["-c", command]);
        c
    };
    let mut child = cmd
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .ok()?;
    let deadline = std::time::Instant::now() + Duration::from_secs(10);
    loop {
        match child.try_wait() {
            Ok(Some(status)) => {
                if !status.success() {
                    return None;
                }
                let mut out = String::new();
                use std::io::Read as _;
                child.stdout.take()?.read_to_string(&mut out).ok()?;
                let out = out.trim().to_string();
                return (!out.is_empty()).then_some(out);
            }
            Ok(None) if std::time::Instant::now() < deadline => {
                std::thread::sleep(Duration::from_millis(20));
            }
            _ => {
                let _ = child.kill();
                let _ = child.wait();
                return None;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn builtin_catalog_is_valid_and_ported() {
        let catalog = ModelCatalog::builtin();
        assert!(catalog.validate().is_empty(), "{:?}", catalog.validate());
        assert!(catalog.models.len() >= 70);
        let opus = catalog.find_model("Anthropic", "CLAUDE-OPUS-4-7").unwrap();
        assert_eq!(opus.context_window, 1_000_000);
        assert_eq!(opus.max_tokens, 128_000);
        assert_eq!(opus.cost.output, 25.0);
        assert_eq!(opus.base_url, "https://api.anthropic.com");
        let together = catalog
            .find_model("together", "openai/gpt-oss-120b")
            .unwrap();
        assert_eq!(together.compat_str("maxTokensField"), Some("max_tokens"));
        assert_eq!(together.compat_bool("supportsStore"), Some(false));
        let codex = catalog.find_model("openai-codex", "gpt-5.5").unwrap();
        assert_eq!(codex.api, "openai-codex-responses");
        assert_eq!(codex.effort_for(ThinkingLevel::Minimal), "low");
        assert!(catalog.find_provider("xai").unwrap().unsupported.is_some());
        assert_eq!(ModelCatalog::image_models().len(), 17);
    }

    #[test]
    fn thinking_levels_and_clamping_follow_pi() {
        let catalog = ModelCatalog::builtin();
        let gpt5 = catalog.find_model("openai", "gpt-5").unwrap();
        use ThinkingLevel::*;
        assert_eq!(gpt5.thinking_levels(), vec![Minimal, Low, Medium, High]);
        assert_eq!(gpt5.clamp_thinking(Off), Minimal);
        assert_eq!(gpt5.clamp_thinking(XHigh), High);
        let gpt52 = catalog.find_model("openai", "gpt-5.2").unwrap();
        assert_eq!(gpt52.thinking_levels(), ThinkingLevel::ALL.to_vec());
        let kimi = catalog
            .find_model("together", "moonshotai/Kimi-K2.6")
            .unwrap();
        assert_eq!(kimi.thinking_levels(), vec![Off, High]);
        assert_eq!(kimi.clamp_thinking(Low), High);
        let mut plain = gpt5.clone();
        plain.reasoning = false;
        assert_eq!(plain.thinking_levels(), vec![Off]);
        assert_eq!(plain.clamp_thinking(High), Off);
    }

    #[test]
    fn match_model_resolves_references() {
        let catalog = ModelCatalog::builtin();
        assert_eq!(
            catalog
                .match_model("openrouter/openai/gpt-5")
                .unwrap()
                .provider,
            "openrouter"
        );
        assert_eq!(catalog.match_model("glm-4.5-air").unwrap().provider, "zai");
        // Ambiguous bare ids do not match.
        assert!(catalog.match_model("gpt-5.3-codex").is_none());
        assert!(catalog.match_model("").is_none());
    }

    #[test]
    fn models_json_overrides_and_custom_providers() {
        let mut catalog = ModelCatalog::builtin().clone();
        catalog
            .apply_models_json(&json!({
                "providers": {
                    "anthropic": {
                        "baseUrl": "https://proxy.example/anthropic",
                        "modelOverrides": { "claude-haiku-4-5": { "name": "Haiku", "cost": { "input": 2 } } }
                    },
                    "local": {
                        "baseUrl": "http://127.0.0.1:8080/v1",
                        "api": "openai-completions",
                        "apiKey": "LOCAL_KEY",
                        "models": [
                            { "id": "qwen", "contextWindow": 32000, "reasoning": true },
                            { "id": "other", "api": "openai-responses" }
                        ]
                    }
                }
            }))
            .unwrap();
        let haiku = catalog.find_model("anthropic", "claude-haiku-4-5").unwrap();
        assert_eq!(haiku.name, "Haiku");
        assert_eq!(haiku.cost.input, 2.0);
        assert_eq!(haiku.cost.output, 5.0);
        assert_eq!(haiku.base_url, "https://proxy.example/anthropic");
        let local = catalog.find_provider("local").unwrap();
        assert_eq!(local.api, "openai-completions");
        assert_eq!(local.accepted_apis.len(), 2);
        let qwen = catalog.find_model("local", "qwen").unwrap();
        assert_eq!(qwen.context_window, 32_000);
        assert_eq!(qwen.max_tokens, 16_384);
        assert_eq!(qwen.api_key.as_deref(), Some("LOCAL_KEY"));
        assert!(catalog.validate().is_empty());

        let err = catalog
            .apply_models_json(&json!({ "providers": { "nope": {} } }))
            .unwrap_err()
            .to_string();
        assert_eq!(
            err,
            "models.json providers.nope: Provider nope: must specify \"baseUrl\", \"headers\", \"compat\", \"modelOverrides\", or \"models\"."
        );
        let err = catalog
            .apply_models_json(&json!({ "providers": { "x": { "baseUrl": "http://x", "models": [{ "id": "m", "api": "a" }] } } }))
            .unwrap_err()
            .to_string();
        assert!(err.ends_with("\"apiKey\" or \"oauth\" is required when defining models."));
    }

    #[test]
    fn replacing_models_drops_previous_ones() {
        let mut catalog = ModelCatalog::builtin().clone();
        catalog
            .apply_provider_config(
                "zai",
                &json!({ "models": [{ "id": "glm-x", "reasoning": true }] }),
            )
            .unwrap();
        let ids: Vec<_> = catalog.models_for("zai").map(|m| m.id.clone()).collect();
        assert_eq!(ids, ["glm-x"]);
        assert_eq!(
            catalog.find_model("zai", "glm-x").unwrap().api,
            "openai-completions"
        );
    }

    #[test]
    fn cost_and_service_tiers() {
        let cost = Cost {
            input: 1.25,
            output: 10.0,
            cache_read: 0.125,
            cache_write: 0.0,
        };
        let usage = Usage {
            input_tokens: 1_000_000,
            output_tokens: 100_000,
            cache_read_tokens: 200_000,
            ..Default::default()
        };
        assert!((cost.compute(&usage) - 2.275).abs() < 1e-9);
        assert_eq!(service_tier_multiplier("gpt-5.5", Some("priority")), 2.5);
        assert_eq!(service_tier_multiplier("gpt-5", Some("priority")), 2.0);
        assert_eq!(service_tier_multiplier("gpt-5", Some("flex")), 0.5);
        assert_eq!(service_tier_multiplier("gpt-5", None), 1.0);
        assert_eq!(
            resolve_codex_service_tier(Some("default"), Some("flex")),
            Some("flex")
        );
        assert_eq!(
            resolve_codex_service_tier(None, Some("priority")),
            Some("priority")
        );
    }

    #[test]
    fn config_values_resolve_commands_env_and_literals() {
        assert_eq!(resolve_config_value("  "), None);
        assert_eq!(
            resolve_config_value("sk-literal").as_deref(),
            Some("sk-literal")
        );
        // SAFETY of the test: PATH is always set and non-empty.
        assert_eq!(resolve_config_value("PATH"), std::env::var("PATH").ok());
        #[cfg(unix)]
        {
            assert_eq!(
                resolve_config_value("!echo secret").as_deref(),
                Some("secret")
            );
            assert_eq!(resolve_config_value("!exit 3"), None);
        }
    }
}
