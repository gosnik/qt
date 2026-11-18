//! `settings.llama`: parsing, merging, validation and serialisation.
//!
//! Field names, error messages and path resolution follow the archived C++
//! qt (`llama_config.cpp` and `parse_llama_settings` in `settings.cpp`).
//! Paths (`models[].path`, `grammar.files.*`) resolve relative to the
//! directory of the settings file that declared them, with `~` expansion.

use std::collections::BTreeMap;
use std::collections::HashSet;
use std::path::{Component, Path, PathBuf};

use serde_json::{Map, Value, json};

use crate::LlamaError;

pub const PROVIDER_ID: &str = "llama";
pub const PROVIDER_API: &str = "llama.cpp";
pub const PROVIDER_LABEL: &str = "llama.cpp";

/// Sampling and length settings (global, per model, or per request).
#[derive(Debug, Clone, Default, PartialEq)]
pub struct GenerationSettings {
    pub temperature: Option<f64>,
    pub top_k: Option<i32>,
    pub top_p: Option<f64>,
    pub min_p: Option<f64>,
    pub min_keep: Option<i32>,
    pub seed: Option<u32>,
    pub max_output_tokens: Option<i64>,
    pub repeat_penalty: Option<f64>,
    pub presence_penalty: Option<f64>,
    pub frequency_penalty: Option<f64>,
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct BackendSettings {
    pub enabled: Option<bool>,
    pub gpu_layers: Option<i32>,
    pub main_gpu: Option<i32>,
    pub split_mode: Option<String>,
    /// `Some` when `tensorSplit` was configured (possibly empty).
    pub tensor_split: Option<Vec<f64>>,
    pub use_mmap: Option<bool>,
    pub use_mlock: Option<bool>,
    pub offload_kqv: Option<bool>,
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct ContextSettings {
    pub context_size: Option<i64>,
    pub batch_size: Option<i64>,
    pub ubatch_size: Option<i64>,
    pub threads: Option<i32>,
    pub batch_threads: Option<i32>,
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct KvCacheSettings {
    /// `off`, `turn` or `session` (default `session`).
    pub policy: Option<String>,
    pub max_sessions: Option<i32>,
    pub clear_on_model_change: Option<bool>,
}

impl KvCacheSettings {
    pub fn effective_policy(&self) -> String {
        self.policy
            .as_deref()
            .map(|p| p.trim().to_ascii_lowercase())
            .unwrap_or_else(|| "session".to_string())
    }

    /// Whether the KV cache is retained between turns.
    pub fn retains(&self) -> bool {
        self.effective_policy() == "session"
    }
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct GrammarSettings {
    pub root: Option<String>,
    pub default_grammar: Option<String>,
    /// Alias → resolved GBNF file path; `Some` when `files` was configured.
    pub files: Option<BTreeMap<String, String>>,
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct EmbeddingSettings {
    pub enabled: Option<bool>,
    /// `none`, `mean`, `cls`, `last` or `rank`.
    pub pooling: Option<String>,
    pub normalize: Option<bool>,
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct ModelSettings {
    pub id: String,
    pub name: String,
    pub path: String,
    pub block_count: Option<i64>,
    pub context_window: Option<i64>,
    pub max_tokens: Option<i64>,
    pub supports_embeddings: Option<bool>,
    pub generation: GenerationSettings,
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct LlamaSettings {
    pub backend: BackendSettings,
    pub context: ContextSettings,
    pub generation: GenerationSettings,
    pub kv_cache: KvCacheSettings,
    pub grammar: GrammarSettings,
    pub embeddings: EmbeddingSettings,
    pub selected_model: Option<String>,
    pub models: Vec<ModelSettings>,
    /// `models` was present in the parsed document (even if empty).
    pub models_configured: bool,
}

// ---------------------------------------------------------------------------
// Emptiness
// ---------------------------------------------------------------------------

impl GenerationSettings {
    pub fn is_empty(&self) -> bool {
        *self == Self::default()
    }

    /// Overlay `other` on top of `self` (set fields win).
    pub fn merged(&self, other: &GenerationSettings) -> GenerationSettings {
        GenerationSettings {
            temperature: other.temperature.or(self.temperature),
            top_k: other.top_k.or(self.top_k),
            top_p: other.top_p.or(self.top_p),
            min_p: other.min_p.or(self.min_p),
            min_keep: other.min_keep.or(self.min_keep),
            seed: other.seed.or(self.seed),
            max_output_tokens: other.max_output_tokens.or(self.max_output_tokens),
            repeat_penalty: other.repeat_penalty.or(self.repeat_penalty),
            presence_penalty: other.presence_penalty.or(self.presence_penalty),
            frequency_penalty: other.frequency_penalty.or(self.frequency_penalty),
        }
    }
}

impl BackendSettings {
    pub fn is_empty(&self) -> bool {
        *self == Self::default()
    }
}
impl ContextSettings {
    pub fn is_empty(&self) -> bool {
        *self == Self::default()
    }
}
impl KvCacheSettings {
    pub fn is_empty(&self) -> bool {
        *self == Self::default()
    }
}
impl GrammarSettings {
    pub fn is_empty(&self) -> bool {
        *self == Self::default()
    }
}
impl EmbeddingSettings {
    pub fn is_empty(&self) -> bool {
        *self == Self::default()
    }
}

impl LlamaSettings {
    pub fn is_empty(&self) -> bool {
        self.backend.is_empty()
            && self.context.is_empty()
            && self.generation.is_empty()
            && self.kv_cache.is_empty()
            && self.grammar.is_empty()
            && self.embeddings.is_empty()
            && self.selected_model.is_none()
            && !self.models_configured
            && self.models.is_empty()
    }

    /// Find a configured model by id (exact match, as the archived provider).
    pub fn model(&self, id: &str) -> Option<&ModelSettings> {
        self.models.iter().find(|m| m.id == id)
    }

    /// Overlay `overlay` (e.g. project settings) on `self` (global settings).
    pub fn merged(&self, overlay: &LlamaSettings) -> LlamaSettings {
        let b = &overlay.backend;
        let c = &overlay.context;
        let k = &overlay.kv_cache;
        let g = &overlay.grammar;
        let e = &overlay.embeddings;
        LlamaSettings {
            backend: BackendSettings {
                enabled: b.enabled.or(self.backend.enabled),
                gpu_layers: b.gpu_layers.or(self.backend.gpu_layers),
                main_gpu: b.main_gpu.or(self.backend.main_gpu),
                split_mode: b.split_mode.clone().or(self.backend.split_mode.clone()),
                tensor_split: b.tensor_split.clone().or(self.backend.tensor_split.clone()),
                use_mmap: b.use_mmap.or(self.backend.use_mmap),
                use_mlock: b.use_mlock.or(self.backend.use_mlock),
                offload_kqv: b.offload_kqv.or(self.backend.offload_kqv),
            },
            context: ContextSettings {
                context_size: c.context_size.or(self.context.context_size),
                batch_size: c.batch_size.or(self.context.batch_size),
                ubatch_size: c.ubatch_size.or(self.context.ubatch_size),
                threads: c.threads.or(self.context.threads),
                batch_threads: c.batch_threads.or(self.context.batch_threads),
            },
            generation: self.generation.merged(&overlay.generation),
            kv_cache: KvCacheSettings {
                policy: k.policy.clone().or(self.kv_cache.policy.clone()),
                max_sessions: k.max_sessions.or(self.kv_cache.max_sessions),
                clear_on_model_change: k
                    .clear_on_model_change
                    .or(self.kv_cache.clear_on_model_change),
            },
            grammar: GrammarSettings {
                root: g.root.clone().or(self.grammar.root.clone()),
                default_grammar: g
                    .default_grammar
                    .clone()
                    .or(self.grammar.default_grammar.clone()),
                files: g.files.clone().or(self.grammar.files.clone()),
            },
            embeddings: EmbeddingSettings {
                enabled: e.enabled.or(self.embeddings.enabled),
                pooling: e.pooling.clone().or(self.embeddings.pooling.clone()),
                normalize: e.normalize.or(self.embeddings.normalize),
            },
            selected_model: overlay
                .selected_model
                .clone()
                .or(self.selected_model.clone()),
            models: if overlay.models_configured {
                overlay.models.clone()
            } else {
                self.models.clone()
            },
            models_configured: overlay.models_configured || self.models_configured,
        }
    }
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

fn invalid(message: impl Into<String>) -> LlamaError {
    LlamaError::InvalidSettings(message.into())
}

fn parse_bool(value: &Value, field: &str) -> Result<bool, LlamaError> {
    value
        .as_bool()
        .ok_or_else(|| invalid(format!("{field} must be a boolean")))
}

fn parse_i32(value: &Value, field: &str) -> Result<i32, LlamaError> {
    if !(value.is_i64() || value.is_u64()) {
        return Err(invalid(format!("{field} must be an integer")));
    }
    value
        .as_i64()
        .and_then(|v| i32::try_from(v).ok())
        .ok_or_else(|| invalid(format!("{field} is out of range")))
}

fn parse_i64(value: &Value, field: &str) -> Result<i64, LlamaError> {
    if !(value.is_i64() || value.is_u64()) {
        return Err(invalid(format!("{field} must be an integer")));
    }
    value
        .as_i64()
        .ok_or_else(|| invalid(format!("{field} is out of range")))
}

fn parse_u32(value: &Value, field: &str) -> Result<u32, LlamaError> {
    if !(value.is_i64() || value.is_u64()) {
        return Err(invalid(format!("{field} must be an integer")));
    }
    let wide = value
        .as_i64()
        .map(i128::from)
        .or(value.as_u64().map(i128::from));
    wide.and_then(|v| u32::try_from(v).ok())
        .ok_or_else(|| invalid(format!("{field} is out of range")))
}

fn parse_f64(value: &Value, field: &str) -> Result<f64, LlamaError> {
    value
        .as_f64()
        .ok_or_else(|| invalid(format!("{field} must be a number")))
}

fn parse_string(value: &Value, field: &str) -> Result<String, LlamaError> {
    value
        .as_str()
        .map(str::to_string)
        .ok_or_else(|| invalid(format!("{field} must be a string")))
}

fn opt<T>(
    object: &Map<String, Value>,
    key: &str,
    field: &str,
    parser: fn(&Value, &str) -> Result<T, LlamaError>,
) -> Result<Option<T>, LlamaError> {
    object.get(key).map(|v| parser(v, field)).transpose()
}

fn object<'a>(value: &'a Value, field: &str) -> Result<&'a Map<String, Value>, LlamaError> {
    value
        .as_object()
        .ok_or_else(|| invalid(format!("{field} must be an object")))
}

/// Expand a leading `~` / `~/` to the home directory.
pub fn expand_user_path(path: &str) -> PathBuf {
    if path == "~" {
        if let Some(home) = dirs::home_dir() {
            return home;
        }
    } else if let Some(rest) = path.strip_prefix("~/")
        && let Some(home) = dirs::home_dir()
    {
        return home.join(rest);
    }
    PathBuf::from(path)
}

/// Lexically normalise `.` and `..` components.
pub fn lexically_normal(path: &Path) -> PathBuf {
    let mut out = PathBuf::new();
    for component in path.components() {
        match component {
            Component::CurDir => {}
            Component::ParentDir => {
                if !out.pop() {
                    out.push("..");
                }
            }
            other => out.push(other.as_os_str()),
        }
    }
    out
}

/// Canonicalise the longest existing prefix and append the rest
/// (`std::filesystem::weakly_canonical`).
pub fn weakly_canonical(path: &Path) -> PathBuf {
    let normal = lexically_normal(path);
    if let Ok(canonical) = normal.canonicalize() {
        return canonical;
    }
    let mut existing = normal.clone();
    let mut tail = Vec::new();
    while !existing.as_os_str().is_empty() {
        if let Ok(canonical) = existing.canonicalize() {
            let mut out = canonical;
            for part in tail.iter().rev() {
                out.push(part);
            }
            return out;
        }
        match (
            existing.file_name().map(|n| n.to_os_string()),
            existing.parent(),
        ) {
            (Some(name), Some(parent)) => {
                tail.push(name);
                existing = parent.to_path_buf();
            }
            _ => break,
        }
    }
    normal
}

/// Resolve a settings path: `~` expansion, relative to `base_dir`, weakly
/// canonical.
pub fn resolve_setting_path(
    value: &str,
    base_dir: &Path,
    field: &str,
) -> Result<String, LlamaError> {
    if value.is_empty() {
        return Err(invalid(format!("{field} must not be empty")));
    }
    let mut path = expand_user_path(value);
    if path.is_relative() {
        path = base_dir.join(path);
    }
    Ok(weakly_canonical(&path).to_string_lossy().into_owned())
}

/// Parse a generation settings object.
pub fn parse_generation(value: &Value, prefix: &str) -> Result<GenerationSettings, LlamaError> {
    let o = object(value, prefix)?;
    let f = |name: &str| format!("{prefix}.{name}");
    Ok(GenerationSettings {
        temperature: opt(o, "temperature", &f("temperature"), parse_f64)?,
        top_k: opt(o, "topK", &f("topK"), parse_i32)?,
        top_p: opt(o, "topP", &f("topP"), parse_f64)?,
        min_p: opt(o, "minP", &f("minP"), parse_f64)?,
        min_keep: opt(o, "minKeep", &f("minKeep"), parse_i32)?,
        seed: opt(o, "seed", &f("seed"), parse_u32)?,
        max_output_tokens: opt(o, "maxOutputTokens", &f("maxOutputTokens"), parse_i64)?,
        repeat_penalty: opt(o, "repeatPenalty", &f("repeatPenalty"), parse_f64)?,
        presence_penalty: opt(o, "presencePenalty", &f("presencePenalty"), parse_f64)?,
        frequency_penalty: opt(o, "frequencyPenalty", &f("frequencyPenalty"), parse_f64)?,
    })
}

impl LlamaSettings {
    /// Parse the `llama` object of a settings document whose file lives in
    /// `base_dir`.
    pub fn parse(value: &Value, base_dir: &Path) -> Result<LlamaSettings, LlamaError> {
        let root = object(value, "llama")?;
        let mut settings = LlamaSettings {
            selected_model: opt(root, "selectedModel", "llama.selectedModel", parse_string)?,
            ..Default::default()
        };

        if let Some(backend) = root.get("backend") {
            let o = object(backend, "llama.backend")?;
            let f = |n: &str| format!("llama.backend.{n}");
            settings.backend = BackendSettings {
                enabled: opt(o, "enabled", &f("enabled"), parse_bool)?,
                gpu_layers: opt(o, "gpuLayers", &f("gpuLayers"), parse_i32)?,
                main_gpu: opt(o, "mainGpu", &f("mainGpu"), parse_i32)?,
                split_mode: opt(o, "splitMode", &f("splitMode"), parse_string)?,
                use_mmap: opt(o, "useMmap", &f("useMmap"), parse_bool)?,
                use_mlock: opt(o, "useMlock", &f("useMlock"), parse_bool)?,
                offload_kqv: opt(o, "offloadKqv", &f("offloadKqv"), parse_bool)?,
                tensor_split: None,
            };
            if let Some(split) = o.get("tensorSplit") {
                let items = split
                    .as_array()
                    .ok_or_else(|| invalid("llama.backend.tensorSplit must be an array"))?;
                let values = items
                    .iter()
                    .enumerate()
                    .map(|(i, v)| parse_f64(v, &format!("llama.backend.tensorSplit[{i}]")))
                    .collect::<Result<Vec<_>, _>>()?;
                settings.backend.tensor_split = Some(values);
            }
        }

        if let Some(context) = root.get("context") {
            let o = object(context, "llama.context")?;
            let f = |n: &str| format!("llama.context.{n}");
            settings.context = ContextSettings {
                context_size: opt(o, "contextSize", &f("contextSize"), parse_i64)?,
                batch_size: opt(o, "batchSize", &f("batchSize"), parse_i64)?,
                ubatch_size: opt(o, "ubatchSize", &f("ubatchSize"), parse_i64)?,
                threads: opt(o, "threads", &f("threads"), parse_i32)?,
                batch_threads: opt(o, "batchThreads", &f("batchThreads"), parse_i32)?,
            };
        }

        if let Some(generation) = root.get("generation") {
            settings.generation = parse_generation(generation, "llama.generation")?;
        }

        if let Some(kv) = root.get("kvCache") {
            let o = object(kv, "llama.kvCache")?;
            let f = |n: &str| format!("llama.kvCache.{n}");
            settings.kv_cache = KvCacheSettings {
                policy: opt(o, "policy", &f("policy"), parse_string)?,
                max_sessions: opt(o, "maxSessions", &f("maxSessions"), parse_i32)?,
                clear_on_model_change: opt(
                    o,
                    "clearOnModelChange",
                    &f("clearOnModelChange"),
                    parse_bool,
                )?,
            };
        }

        if let Some(grammar) = root.get("grammar") {
            let o = object(grammar, "llama.grammar")?;
            settings.grammar.root = opt(o, "root", "llama.grammar.root", parse_string)?;
            if let Some(default) = o.get("default").filter(|v| !v.is_null()) {
                settings.grammar.default_grammar =
                    Some(parse_string(default, "llama.grammar.default")?);
            }
            if let Some(files) = o.get("files") {
                let files = files
                    .as_object()
                    .ok_or_else(|| invalid("llama.grammar.files must be an object"))?;
                let mut resolved = BTreeMap::new();
                for (alias, path) in files {
                    let field = format!("llama.grammar.files.{alias}");
                    let path = path
                        .as_str()
                        .ok_or_else(|| invalid(format!("{field} must be a string")))?;
                    resolved.insert(alias.clone(), resolve_setting_path(path, base_dir, &field)?);
                }
                settings.grammar.files = Some(resolved);
            }
        }

        if let Some(embeddings) = root.get("embeddings") {
            let o = object(embeddings, "llama.embeddings")?;
            let f = |n: &str| format!("llama.embeddings.{n}");
            settings.embeddings = EmbeddingSettings {
                enabled: opt(o, "enabled", &f("enabled"), parse_bool)?,
                pooling: opt(o, "pooling", &f("pooling"), parse_string)?,
                normalize: opt(o, "normalize", &f("normalize"), parse_bool)?,
            };
        }

        if let Some(models) = root.get("models") {
            let items = models
                .as_array()
                .ok_or_else(|| invalid("llama.models must be an array"))?;
            for (index, item) in items.iter().enumerate() {
                let prefix = format!("llama.models[{index}]");
                let o = item
                    .as_object()
                    .ok_or_else(|| invalid(format!("{prefix} must be an object")))?;
                let f = |n: &str| format!("{prefix}.{n}");
                let mut model = ModelSettings {
                    id: opt(o, "id", &f("id"), parse_string)?.unwrap_or_default(),
                    name: opt(o, "name", &f("name"), parse_string)?.unwrap_or_default(),
                    block_count: opt(o, "blockCount", &f("blockCount"), parse_i64)?,
                    context_window: opt(o, "contextWindow", &f("contextWindow"), parse_i64)?,
                    max_tokens: opt(o, "maxTokens", &f("maxTokens"), parse_i64)?,
                    supports_embeddings: opt(
                        o,
                        "supportsEmbeddings",
                        &f("supportsEmbeddings"),
                        parse_bool,
                    )?,
                    ..Default::default()
                };
                if let Some(path) = o.get("path") {
                    let path = path
                        .as_str()
                        .ok_or_else(|| invalid(format!("{prefix}.path must be a string")))?;
                    model.path = resolve_setting_path(path, base_dir, &f("path"))?;
                }
                if let Some(generation) = o.get("generation") {
                    model.generation = parse_generation(generation, &f("generation"))?;
                }
                settings.models.push(model);
            }
            settings.models_configured = true;
        }

        Ok(settings)
    }

    /// Parse the `llama` key of a whole settings document. A document
    /// without `llama` yields empty settings.
    pub fn from_document(document: &Value, base_dir: &Path) -> Result<LlamaSettings, LlamaError> {
        match document.get("llama") {
            Some(llama) => {
                let settings = Self::parse(llama, base_dir)?;
                if let Some(first) = settings.validate().into_iter().next() {
                    return Err(invalid(first));
                }
                Ok(settings)
            }
            None => Ok(LlamaSettings::default()),
        }
    }

    /// Read `settings.llama` from the agent home settings file.
    pub fn from_settings(
        settings: &qt_core::Settings,
        home: &qt_core::AgentHome,
    ) -> Result<LlamaSettings, LlamaError> {
        Self::from_document(&Value::Object(settings.raw().clone()), home.root())
    }

    /// Load global settings from `global_path` and overlay an optional
    /// project settings file, each resolving paths from its own directory.
    pub fn load_files(
        global_path: &Path,
        project_path: Option<&Path>,
    ) -> Result<LlamaSettings, LlamaError> {
        let read = |path: &Path| -> Result<LlamaSettings, LlamaError> {
            let text = match std::fs::read_to_string(path) {
                Ok(text) => text,
                Err(err) if err.kind() == std::io::ErrorKind::NotFound => {
                    return Ok(LlamaSettings::default());
                }
                Err(err) => {
                    return Err(invalid(format!(
                        "settings file could not be read: {}: {err}",
                        path.display()
                    )));
                }
            };
            let document: Value = serde_json::from_str(&text).map_err(|err| {
                invalid(format!(
                    "invalid settings JSON in {}: {err}",
                    path.display()
                ))
            })?;
            let base = path.parent().unwrap_or(Path::new("."));
            Self::from_document(&document, base)
                .map_err(|err| invalid(format!("invalid settings in {}: {err}", path.display())))
        };
        let global = read(global_path)?;
        match project_path {
            Some(project) => Ok(global.merged(&read(project)?)),
            None => Ok(global),
        }
    }
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

fn trimmed_lower(value: &str) -> String {
    value.trim().to_ascii_lowercase()
}

/// Validate generation settings; messages are prefixed with `prefix`.
pub fn validate_generation(settings: &GenerationSettings, prefix: &str) -> Vec<String> {
    let mut errors = Vec::new();
    for (value, field) in [
        (settings.temperature, "temperature"),
        (settings.top_p, "topP"),
        (settings.min_p, "minP"),
        (settings.repeat_penalty, "repeatPenalty"),
        (settings.presence_penalty, "presencePenalty"),
        (settings.frequency_penalty, "frequencyPenalty"),
    ] {
        if value.is_some_and(|v| !v.is_finite()) {
            errors.push(format!("{prefix}{field} must be finite"));
        }
    }
    if settings.temperature.is_some_and(|v| v < 0.0) {
        errors.push(format!("{prefix}temperature must be >= 0"));
    }
    if settings.top_k.is_some_and(|v| v < 0) {
        errors.push(format!("{prefix}topK must be >= 0"));
    }
    if settings.top_p.is_some_and(|v| !(0.0..=1.0).contains(&v)) {
        errors.push(format!("{prefix}topP must be between 0 and 1"));
    }
    if settings.min_p.is_some_and(|v| !(0.0..=1.0).contains(&v)) {
        errors.push(format!("{prefix}minP must be between 0 and 1"));
    }
    if settings.min_keep.is_some_and(|v| v < 1) {
        errors.push(format!("{prefix}minKeep must be >= 1"));
    }
    if settings.max_output_tokens.is_some_and(|v| v < 0) {
        errors.push(format!("{prefix}maxOutputTokens must be >= 0"));
    }
    if settings.repeat_penalty.is_some_and(|v| v < 0.0) {
        errors.push(format!("{prefix}repeatPenalty must be >= 0"));
    }
    errors
}

impl LlamaSettings {
    /// All validation errors, in the archived order and wording.
    pub fn validate(&self) -> Vec<String> {
        let mut errors = Vec::new();
        if self.backend.main_gpu.is_some_and(|v| v < 0) {
            errors.push("llama.backend.mainGpu must be >= 0".to_string());
        }
        if let Some(mode) = &self.backend.split_mode
            && !matches!(trimmed_lower(mode).as_str(), "none" | "layer" | "row")
        {
            errors.push("llama.backend.splitMode must be one of: none, layer, row".into());
        }
        if let Some(split) = &self.backend.tensor_split {
            for (index, value) in split.iter().enumerate() {
                if !value.is_finite() || *value < 0.0 {
                    errors.push(format!(
                        "llama.backend.tensorSplit[{index}] must be finite and >= 0"
                    ));
                }
            }
        }
        for (value, field) in [
            (self.context.context_size, "contextSize"),
            (self.context.batch_size, "batchSize"),
            (self.context.ubatch_size, "ubatchSize"),
        ] {
            if value.is_some_and(|v| v < 0) {
                errors.push(format!("llama.context.{field} must be >= 0"));
            }
        }
        if self.context.threads.is_some_and(|v| v < 0) {
            errors.push("llama.context.threads must be >= 0".into());
        }
        if self.context.batch_threads.is_some_and(|v| v < 0) {
            errors.push("llama.context.batchThreads must be >= 0".into());
        }
        errors.extend(validate_generation(&self.generation, "llama.generation."));
        if let Some(policy) = &self.kv_cache.policy
            && !matches!(trimmed_lower(policy).as_str(), "off" | "turn" | "session")
        {
            errors.push("llama.kvCache.policy must be one of: off, turn, session".into());
        }
        if self.kv_cache.max_sessions.is_some_and(|v| v < 0) {
            errors.push("llama.kvCache.maxSessions must be >= 0".into());
        }
        if self
            .grammar
            .root
            .as_deref()
            .is_some_and(|r| r.trim().is_empty())
        {
            errors.push("llama.grammar.root must not be empty".into());
        }
        if self
            .grammar
            .default_grammar
            .as_deref()
            .is_some_and(|g| g.trim().is_empty())
        {
            errors.push("llama.grammar.default must not be empty when set".into());
        }
        for (name, path) in self.grammar.files.iter().flatten() {
            if name.trim().is_empty() {
                errors.push("llama.grammar.files keys must not be empty".into());
            }
            if path.trim().is_empty() {
                errors.push(format!("llama.grammar.files.{name} must not be empty"));
            }
        }
        if let Some(pooling) = &self.embeddings.pooling
            && !matches!(
                trimmed_lower(pooling).as_str(),
                "none" | "mean" | "cls" | "last" | "rank"
            )
        {
            errors.push(
                "llama.embeddings.pooling must be one of: none, mean, cls, last, rank".into(),
            );
        }
        if self
            .selected_model
            .as_deref()
            .is_some_and(|m| m.trim().is_empty())
        {
            errors.push("llama.selectedModel must not be empty when set".into());
        }
        let mut ids = HashSet::new();
        for (index, model) in self.models.iter().enumerate() {
            let prefix = format!("llama.models[{index}].");
            let id = model.id.trim();
            if id.is_empty() {
                errors.push(format!("{prefix}id is required"));
            } else if !ids.insert(id.to_ascii_lowercase()) {
                errors.push(format!("duplicate llama model id: {id}"));
            }
            if model.name.trim().is_empty() {
                errors.push(format!("{prefix}name is required"));
            }
            if model.path.trim().is_empty() {
                errors.push(format!("{prefix}path is required"));
            }
            if model.block_count.is_some_and(|v| v <= 0) {
                errors.push(format!("{prefix}blockCount must be > 0"));
            }
            if model.context_window.is_some_and(|v| v < 0) {
                errors.push(format!("{prefix}contextWindow must be >= 0"));
            }
            if model.max_tokens.is_some_and(|v| v < 0) {
                errors.push(format!("{prefix}maxTokens must be >= 0"));
            }
            errors.extend(validate_generation(
                &model.generation,
                &format!("{prefix}generation."),
            ));
        }
        errors
    }
}

// ---------------------------------------------------------------------------
// Serialisation
// ---------------------------------------------------------------------------

fn put<T: Into<Value>>(map: &mut Map<String, Value>, key: &str, value: Option<T>) {
    if let Some(value) = value {
        map.insert(key.to_string(), value.into());
    }
}

impl GenerationSettings {
    pub fn to_json(&self) -> Value {
        let mut m = Map::new();
        put(&mut m, "temperature", self.temperature);
        put(&mut m, "topK", self.top_k);
        put(&mut m, "topP", self.top_p);
        put(&mut m, "minP", self.min_p);
        put(&mut m, "minKeep", self.min_keep);
        put(&mut m, "seed", self.seed);
        put(&mut m, "maxOutputTokens", self.max_output_tokens);
        put(&mut m, "repeatPenalty", self.repeat_penalty);
        put(&mut m, "presencePenalty", self.presence_penalty);
        put(&mut m, "frequencyPenalty", self.frequency_penalty);
        Value::Object(m)
    }

    /// Lenient overlay from request/option metadata (wrong types ignored).
    pub fn from_json_lenient(value: &Value) -> GenerationSettings {
        let f = |k: &str| value.get(k).and_then(Value::as_f64);
        let i = |k: &str| {
            value
                .get(k)
                .filter(|v| v.is_i64() || v.is_u64())
                .and_then(Value::as_i64)
        };
        GenerationSettings {
            temperature: f("temperature"),
            top_k: i("topK").and_then(|v| i32::try_from(v).ok()),
            top_p: f("topP"),
            min_p: f("minP"),
            min_keep: i("minKeep").and_then(|v| i32::try_from(v).ok()),
            seed: value
                .get("seed")
                .and_then(Value::as_u64)
                .and_then(|v| u32::try_from(v).ok()),
            max_output_tokens: i("maxOutputTokens"),
            repeat_penalty: f("repeatPenalty"),
            presence_penalty: f("presencePenalty"),
            frequency_penalty: f("frequencyPenalty"),
        }
    }
}

impl BackendSettings {
    pub fn to_json(&self) -> Value {
        let mut m = Map::new();
        put(&mut m, "enabled", self.enabled);
        put(&mut m, "gpuLayers", self.gpu_layers);
        put(&mut m, "mainGpu", self.main_gpu);
        put(&mut m, "splitMode", self.split_mode.clone());
        if let Some(split) = &self.tensor_split {
            m.insert("tensorSplit".into(), json!(split));
        }
        put(&mut m, "useMmap", self.use_mmap);
        put(&mut m, "useMlock", self.use_mlock);
        put(&mut m, "offloadKqv", self.offload_kqv);
        Value::Object(m)
    }

    /// The status shape reported by runtime status (`null` when unset).
    pub fn status_json(&self) -> Value {
        json!({
            "gpuLayers": self.gpu_layers,
            "mainGpu": self.main_gpu,
            "splitMode": self.split_mode,
            "offloadKqv": self.offload_kqv,
            "useMmap": self.use_mmap,
            "useMlock": self.use_mlock,
            "tensorSplitConfigured": self.tensor_split.is_some(),
            "tensorSplitCount": self.tensor_split.as_ref().map_or(0, Vec::len),
        })
    }
}

impl ContextSettings {
    pub fn to_json(&self) -> Value {
        let mut m = Map::new();
        put(&mut m, "contextSize", self.context_size);
        put(&mut m, "batchSize", self.batch_size);
        put(&mut m, "ubatchSize", self.ubatch_size);
        put(&mut m, "threads", self.threads);
        put(&mut m, "batchThreads", self.batch_threads);
        Value::Object(m)
    }
}

impl KvCacheSettings {
    pub fn to_json(&self) -> Value {
        let mut m = Map::new();
        put(&mut m, "policy", self.policy.clone());
        put(&mut m, "maxSessions", self.max_sessions);
        put(&mut m, "clearOnModelChange", self.clear_on_model_change);
        Value::Object(m)
    }
}

impl GrammarSettings {
    pub fn to_json(&self) -> Value {
        let mut m = Map::new();
        put(&mut m, "root", self.root.clone());
        put(&mut m, "default", self.default_grammar.clone());
        if let Some(files) = &self.files {
            m.insert("files".into(), json!(files));
        }
        Value::Object(m)
    }
}

impl EmbeddingSettings {
    pub fn to_json(&self) -> Value {
        let mut m = Map::new();
        put(&mut m, "enabled", self.enabled);
        put(&mut m, "pooling", self.pooling.clone());
        put(&mut m, "normalize", self.normalize);
        Value::Object(m)
    }
}

impl ModelSettings {
    pub fn to_json(&self) -> Value {
        let mut m = Map::new();
        m.insert("id".into(), json!(self.id));
        m.insert("name".into(), json!(self.name));
        m.insert("path".into(), json!(self.path));
        put(&mut m, "blockCount", self.block_count);
        put(&mut m, "contextWindow", self.context_window);
        put(&mut m, "maxTokens", self.max_tokens);
        put(&mut m, "supportsEmbeddings", self.supports_embeddings);
        if !self.generation.is_empty() {
            m.insert("generation".into(), self.generation.to_json());
        }
        Value::Object(m)
    }
}

impl LlamaSettings {
    /// Serialise in the archived `settings.json` shape (empty sections
    /// omitted).
    pub fn to_json(&self) -> Value {
        let mut m = Map::new();
        put(&mut m, "selectedModel", self.selected_model.clone());
        if !self.backend.is_empty() {
            m.insert("backend".into(), self.backend.to_json());
        }
        if !self.context.is_empty() {
            m.insert("context".into(), self.context.to_json());
        }
        if !self.generation.is_empty() {
            m.insert("generation".into(), self.generation.to_json());
        }
        if !self.kv_cache.is_empty() {
            m.insert("kvCache".into(), self.kv_cache.to_json());
        }
        if !self.grammar.is_empty() {
            m.insert("grammar".into(), self.grammar.to_json());
        }
        if !self.embeddings.is_empty() {
            m.insert("embeddings".into(), self.embeddings.to_json());
        }
        if self.models_configured {
            m.insert(
                "models".into(),
                Value::Array(self.models.iter().map(ModelSettings::to_json).collect()),
            );
        }
        Value::Object(m)
    }

    /// Context window advertised for a model: its own, else the configured
    /// context size.
    pub fn effective_context_window(&self, model: &ModelSettings) -> Option<i64> {
        model.context_window.or(self.context.context_size)
    }

    /// Output limit advertised for a model.
    pub fn effective_max_tokens(&self, model: &ModelSettings) -> Option<i64> {
        model.max_tokens.or(self.generation.max_output_tokens)
    }

    /// Models for the qt-core model picker (provider `llama`).
    pub fn model_infos(&self) -> Vec<qt_core::ModelInfo> {
        self.models
            .iter()
            .map(|model| qt_core::ModelInfo {
                provider: PROVIDER_ID.to_string(),
                id: model.id.clone(),
                name: if model.name.is_empty() {
                    model.id.clone()
                } else {
                    model.name.clone()
                },
                context_window: self
                    .effective_context_window(model)
                    .and_then(|v| u64::try_from(v).ok())
                    .filter(|v| *v > 0),
                ..Default::default()
            })
            .collect()
    }

    /// Catalog metadata for one model (archived `model_catalog_from_llama_settings`).
    pub fn model_catalog_metadata(&self, model: &ModelSettings) -> Value {
        let mut metadata = json!({
            "input": ["text"],
            "cost": zero_cost_metadata(),
            "path": model.path,
            "local": true,
            "direct": true,
            "llama": {
                "model": model.to_json(),
                "generation": self.generation.merged(&model.generation).to_json(),
            },
        });
        if let Some(window) = self.effective_context_window(model) {
            metadata["contextWindow"] = json!(window);
        }
        if let Some(max) = self.effective_max_tokens(model) {
            metadata["maxTokens"] = json!(max);
        }
        if model
            .supports_embeddings
            .unwrap_or(self.embeddings.enabled.unwrap_or(false))
        {
            metadata["supportsEmbeddings"] = json!(true);
        }
        metadata
    }
}

/// Zero pricing for local models.
pub fn zero_cost_metadata() -> Value {
    json!({
        "input": 0.0,
        "output": 0.0,
        "cacheRead": 0.0,
        "cacheWrite": 0.0,
        "input_usd_per_million": 0.0,
        "output_usd_per_million": 0.0,
        "cache_read_usd_per_million": 0.0,
        "cache_write_usd_per_million": 0.0,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse(value: Value) -> Result<LlamaSettings, LlamaError> {
        LlamaSettings::parse(&value, Path::new("/base"))
    }

    #[test]
    fn parses_full_document() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(dir.path().join("tool.gbnf"), "root ::= \"x\"").unwrap();
        let settings = LlamaSettings::parse(
            &json!({
                "selectedModel": "qwen",
                "backend": {"enabled": true, "gpuLayers": 20, "mainGpu": 0, "splitMode": "layer",
                            "tensorSplit": [0.5, 0.5], "useMmap": true, "useMlock": false, "offloadKqv": true},
                "context": {"contextSize": 4096, "batchSize": 512, "ubatchSize": 256, "threads": 8, "batchThreads": 8},
                "generation": {"temperature": 0.2, "topK": 40, "topP": 0.95, "minP": 0.05, "minKeep": 2,
                               "seed": 42, "maxOutputTokens": 128, "repeatPenalty": 1.1,
                               "presencePenalty": 0.1, "frequencyPenalty": 0.2},
                "kvCache": {"policy": "turn", "maxSessions": 2, "clearOnModelChange": true},
                "grammar": {"root": "root", "default": "json", "files": {"json": "tool.gbnf"}},
                "embeddings": {"enabled": true, "pooling": "mean", "normalize": false},
                "models": [{"id": "qwen", "name": "Qwen", "path": "models/../m.gguf", "blockCount": 24,
                            "contextWindow": 32768, "maxTokens": 512, "supportsEmbeddings": false,
                            "generation": {"temperature": 0.7}}]
            }),
            dir.path(),
        )
        .unwrap();
        let base = dir.path().canonicalize().unwrap();
        assert_eq!(settings.backend.gpu_layers, Some(20));
        assert_eq!(settings.backend.tensor_split, Some(vec![0.5, 0.5]));
        assert_eq!(settings.context.context_size, Some(4096));
        assert_eq!(settings.generation.seed, Some(42));
        assert_eq!(settings.kv_cache.policy.as_deref(), Some("turn"));
        assert!(!settings.kv_cache.retains());
        assert_eq!(
            settings.grammar.files.as_ref().unwrap()["json"],
            base.join("tool.gbnf").to_string_lossy()
        );
        assert_eq!(
            settings.models[0].path,
            base.join("m.gguf").to_string_lossy()
        );
        assert_eq!(settings.models[0].generation.temperature, Some(0.7));
        assert!(settings.validate().is_empty());

        // Round trip through the archived JSON shape.
        let round = LlamaSettings::parse(&settings.to_json(), Path::new("/")).unwrap();
        assert_eq!(round, settings);
    }

    #[test]
    fn reports_archived_type_errors() {
        let cases = [
            (json!([]), "llama must be an object"),
            (json!({"backend": 1}), "llama.backend must be an object"),
            (
                json!({"backend": {"gpuLayers": "x"}}),
                "llama.backend.gpuLayers must be an integer",
            ),
            (
                json!({"backend": {"gpuLayers": 1.5}}),
                "llama.backend.gpuLayers must be an integer",
            ),
            (
                json!({"backend": {"gpuLayers": 5_000_000_000_i64}}),
                "llama.backend.gpuLayers is out of range",
            ),
            (
                json!({"backend": {"tensorSplit": 1}}),
                "llama.backend.tensorSplit must be an array",
            ),
            (
                json!({"backend": {"tensorSplit": ["a"]}}),
                "llama.backend.tensorSplit[0] must be a number",
            ),
            (
                json!({"generation": {"seed": -1}}),
                "llama.generation.seed is out of range",
            ),
            (
                json!({"generation": {"topP": "a"}}),
                "llama.generation.topP must be a number",
            ),
            (
                json!({"kvCache": {"policy": 3}}),
                "llama.kvCache.policy must be a string",
            ),
            (
                json!({"grammar": {"files": []}}),
                "llama.grammar.files must be an object",
            ),
            (
                json!({"grammar": {"files": {"a": 1}}}),
                "llama.grammar.files.a must be a string",
            ),
            (
                json!({"grammar": {"files": {"a": ""}}}),
                "llama.grammar.files.a must not be empty",
            ),
            (
                json!({"embeddings": {"enabled": 1}}),
                "llama.embeddings.enabled must be a boolean",
            ),
            (json!({"models": {}}), "llama.models must be an array"),
            (json!({"models": [1]}), "llama.models[0] must be an object"),
            (
                json!({"models": [{"path": 1}]}),
                "llama.models[0].path must be a string",
            ),
            (
                json!({"models": [{"generation": 1}]}),
                "llama.models[0].generation must be an object",
            ),
        ];
        for (value, message) in cases {
            let err = parse(value.clone()).unwrap_err();
            assert_eq!(err.to_string(), message, "{value}");
        }
    }

    #[test]
    fn validates_ranges_like_archived() {
        let settings = parse(json!({
            "selectedModel": " ",
            "backend": {"mainGpu": -1, "splitMode": "diagonal", "tensorSplit": [-1.0]},
            "context": {"contextSize": -1, "threads": -2},
            "generation": {"temperature": -0.1, "topK": -1, "topP": 1.5, "minP": -0.5, "minKeep": 0,
                           "maxOutputTokens": -1, "repeatPenalty": -1.0},
            "kvCache": {"policy": "forever", "maxSessions": -1},
            "grammar": {"root": " ", "default": ""},
            "embeddings": {"pooling": "max"},
            "models": [
                {"id": "a", "name": "A", "path": "/a.gguf", "blockCount": 0},
                {"id": "A", "name": "", "path": "/b.gguf", "contextWindow": -1, "maxTokens": -1,
                 "generation": {"topP": 2.0}},
                {"name": "x"}
            ]
        }))
        .unwrap();
        let errors = settings.validate();
        for expected in [
            "llama.backend.mainGpu must be >= 0",
            "llama.backend.splitMode must be one of: none, layer, row",
            "llama.backend.tensorSplit[0] must be finite and >= 0",
            "llama.context.contextSize must be >= 0",
            "llama.context.threads must be >= 0",
            "llama.generation.temperature must be >= 0",
            "llama.generation.topK must be >= 0",
            "llama.generation.topP must be between 0 and 1",
            "llama.generation.minP must be between 0 and 1",
            "llama.generation.minKeep must be >= 1",
            "llama.generation.maxOutputTokens must be >= 0",
            "llama.generation.repeatPenalty must be >= 0",
            "llama.kvCache.policy must be one of: off, turn, session",
            "llama.kvCache.maxSessions must be >= 0",
            "llama.grammar.root must not be empty",
            "llama.grammar.default must not be empty when set",
            "llama.embeddings.pooling must be one of: none, mean, cls, last, rank",
            "llama.selectedModel must not be empty when set",
            "llama.models[0].blockCount must be > 0",
            "duplicate llama model id: A",
            "llama.models[1].name is required",
            "llama.models[1].contextWindow must be >= 0",
            "llama.models[1].maxTokens must be >= 0",
            "llama.models[1].generation.topP must be between 0 and 1",
            "llama.models[2].id is required",
            "llama.models[2].path is required",
        ] {
            assert!(
                errors.iter().any(|e| e == expected),
                "missing {expected}: {errors:?}"
            );
        }
    }

    #[test]
    fn merge_overlays_set_fields_and_replaces_models() {
        let global = parse(json!({
            "generation": {"temperature": 0.1, "topK": 10},
            "backend": {"gpuLayers": 5, "tensorSplit": [1.0]},
            "models": [{"id": "a", "name": "A", "path": "/a"}]
        }))
        .unwrap();
        let project =
            parse(json!({"generation": {"temperature": 0.9}, "backend": {"mainGpu": 1}})).unwrap();
        let merged = global.merged(&project);
        assert_eq!(merged.generation.temperature, Some(0.9));
        assert_eq!(merged.generation.top_k, Some(10));
        assert_eq!(merged.backend.gpu_layers, Some(5));
        assert_eq!(merged.backend.main_gpu, Some(1));
        assert_eq!(merged.models.len(), 1);

        let replace = parse(json!({"models": []})).unwrap();
        assert!(global.merged(&replace).models.is_empty());
    }

    #[test]
    fn loads_global_and_project_files() {
        let dir = tempfile::tempdir().unwrap();
        let global = dir.path().join("settings.json");
        std::fs::write(
            &global,
            r#"{"llama": {"models": [{"id": "m", "name": "M", "path": "~/m.gguf"}], "context": {"contextSize": 2048}}}"#,
        )
        .unwrap();
        let project_dir = dir.path().join("proj/.qt");
        std::fs::create_dir_all(&project_dir).unwrap();
        let project = project_dir.join("settings.json");
        std::fs::write(&project, r#"{"llama": {"context": {"contextSize": 1024}}}"#).unwrap();
        let settings = LlamaSettings::load_files(&global, Some(&project)).unwrap();
        assert_eq!(settings.context.context_size, Some(1024));
        let home = dirs::home_dir().unwrap();
        assert!(settings.models[0].path.ends_with("m.gguf"));
        assert!(Path::new(&settings.models[0].path).starts_with(weakly_canonical(&home)));

        std::fs::write(&project, r#"{"llama": {"kvCache": {"policy": "nope"}}}"#).unwrap();
        let err = LlamaSettings::load_files(&global, Some(&project)).unwrap_err();
        assert!(
            err.to_string()
                .contains("llama.kvCache.policy must be one of")
        );
        assert!(
            LlamaSettings::load_files(&dir.path().join("missing.json"), None)
                .unwrap()
                .is_empty()
        );
    }

    #[test]
    fn model_infos_and_catalog_metadata() {
        let settings = parse(json!({
            "context": {"contextSize": 4096},
            "generation": {"maxOutputTokens": 64, "temperature": 0.5},
            "embeddings": {"enabled": true},
            "models": [{"id": "a", "name": "", "path": "/a.gguf", "generation": {"topK": 5}}]
        }))
        .unwrap();
        let infos = settings.model_infos();
        assert_eq!(infos[0].provider, "llama");
        assert_eq!(infos[0].name, "a");
        assert_eq!(infos[0].context_window, Some(4096));
        let meta = settings.model_catalog_metadata(&settings.models[0]);
        assert_eq!(meta["contextWindow"], 4096);
        assert_eq!(meta["maxTokens"], 64);
        assert_eq!(meta["supportsEmbeddings"], true);
        assert_eq!(meta["llama"]["generation"]["topK"], 5);
        assert_eq!(meta["llama"]["generation"]["temperature"], 0.5);
    }

    #[test]
    fn weakly_canonical_handles_missing_tails() {
        let dir = tempfile::tempdir().unwrap();
        let base = dir.path().canonicalize().unwrap();
        let p = weakly_canonical(&dir.path().join("a/../b/c.gguf"));
        assert_eq!(p, base.join("b/c.gguf"));
    }
}
