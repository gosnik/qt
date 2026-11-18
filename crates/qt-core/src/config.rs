//! Agent home, settings and credentials.
//!
//! The layout is shared with the archived C++ qt: the agent home defaults to
//! `~/.qt` (override with `QT_CODING_AGENT_DIR`), settings live in
//! `settings.json`, credentials in `auth.json` (see [`crate::auth`]), custom
//! models in `models.json` and sessions in `sessions/`. Unknown keys in the
//! JSON files are preserved on write.

use std::fs;
use std::io::Write as _;
use std::path::{Path, PathBuf};

use anyhow::{Context as _, Result, bail};
use serde_json::{Map, Value, json};

pub use crate::auth::{AuthStore, env_var_for};
pub use crate::ollama::OllamaInstance;

/// Root directory for qt state.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AgentHome {
    root: PathBuf,
}

impl AgentHome {
    pub fn new(root: impl Into<PathBuf>) -> Self {
        Self { root: root.into() }
    }

    /// `$QT_CODING_AGENT_DIR`, else `~/.qt`.
    pub fn from_env() -> Self {
        if let Some(dir) = std::env::var_os("QT_CODING_AGENT_DIR").filter(|v| !v.is_empty()) {
            return Self::new(PathBuf::from(dir));
        }
        let home = dirs::home_dir().unwrap_or_else(|| PathBuf::from("."));
        Self::new(home.join(".qt"))
    }

    pub fn root(&self) -> &Path {
        &self.root
    }
    pub fn settings_path(&self) -> PathBuf {
        self.root.join("settings.json")
    }
    pub fn auth_path(&self) -> PathBuf {
        self.root.join("auth.json")
    }
    /// Custom providers and model overrides (Pi `models.json`).
    pub fn models_path(&self) -> PathBuf {
        self.root.join("models.json")
    }
    /// Cached OpenRouter model metadata.
    pub fn openrouter_cache_path(&self) -> PathBuf {
        self.root.join("openrouter_models.json")
    }
    pub fn sessions_dir(&self) -> PathBuf {
        self.root.join("sessions")
    }
    /// Global instruction file appended to every system prompt.
    pub fn global_agents_file(&self) -> PathBuf {
        self.root.join("AGENTS.md")
    }
}

/// `settings.json`, kept as a JSON object so unknown keys survive rewrites.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Settings {
    raw: Map<String, Value>,
    /// The agent home the settings were loaded from.
    home: Option<AgentHome>,
}

impl Settings {
    /// Read `settings.json`; when it does not exist, `settings.yaml` or
    /// `settings.yml` (archived lookup order).
    pub fn load(home: &AgentHome) -> Result<Self> {
        Ok(Self {
            raw: read_settings_object(home.root())?,
            home: Some(home.clone()),
        })
    }

    /// Settings from an already parsed object.
    pub fn from_raw(raw: Map<String, Value>, home: Option<AgentHome>) -> Self {
        Self { raw, home }
    }

    /// A copy with `overlay` deep-merged on top: objects merge key by key,
    /// any other value replaces the base value.
    pub fn merged_with(&self, overlay: &Map<String, Value>) -> Self {
        let mut raw = self.raw.clone();
        deep_merge(&mut raw, overlay);
        Self {
            raw,
            home: self.home.clone(),
        }
    }

    /// `enabledModels` patterns (Pi model scoping).
    pub fn enabled_models(&self) -> Vec<String> {
        self.raw
            .get("enabledModels")
            .and_then(Value::as_array)
            .map(|items| {
                items
                    .iter()
                    .filter_map(Value::as_str)
                    .map(str::trim)
                    .filter(|s| !s.is_empty())
                    .map(str::to_string)
                    .collect()
            })
            .unwrap_or_default()
    }

    /// Replace `enabledModels`; an empty list removes the key.
    pub fn set_enabled_models(&mut self, patterns: &[String]) {
        if patterns.is_empty() {
            self.raw.remove("enabledModels");
        } else {
            self.raw.insert("enabledModels".into(), json!(patterns));
        }
    }

    /// `shellPath`: explicit shell for bash.
    pub fn shell_path(&self) -> Option<String> {
        self.str_key("shellPath")
    }

    /// `shellCommandPrefix`: prepended to every bash command.
    pub fn shell_command_prefix(&self) -> Option<String> {
        self.str_key("shellCommandPrefix")
    }

    /// `terminal.promptHistoryLimit` (default 500).
    pub fn prompt_history_limit(&self) -> usize {
        self.raw
            .get("terminal")
            .and_then(|t| t.get("promptHistoryLimit"))
            .and_then(Value::as_u64)
            .map(|v| v as usize)
            .unwrap_or(crate::history::DEFAULT_PROMPT_HISTORY_LIMIT)
    }

    pub fn save(&self, home: &AgentHome) -> Result<()> {
        write_json_atomic(
            &home.settings_path(),
            &Value::Object(self.raw.clone()),
            false,
        )
    }

    pub fn raw(&self) -> &Map<String, Value> {
        &self.raw
    }

    /// Mutable access for subsystems that own their own settings keys.
    pub fn raw_mut(&mut self) -> &mut Map<String, Value> {
        &mut self.raw
    }

    /// Where these settings were loaded from, if from disk.
    pub fn home(&self) -> Option<&AgentHome> {
        self.home.as_ref()
    }

    fn str_key(&self, key: &str) -> Option<String> {
        self.raw
            .get(key)
            .and_then(Value::as_str)
            .map(str::to_string)
            .filter(|s| !s.is_empty())
    }

    pub fn default_provider(&self) -> Option<String> {
        self.str_key("defaultProvider")
    }
    pub fn default_model(&self) -> Option<String> {
        self.str_key("defaultModel")
    }
    pub fn set_default_model(&mut self, provider: &str, model: &str) {
        self.raw.insert("defaultProvider".into(), json!(provider));
        self.raw.insert("defaultModel".into(), json!(model));
    }

    /// GUI theme preference: `"light"`, `"dark"` or `None` (follow system).
    pub fn theme(&self) -> Option<String> {
        self.raw
            .get("gui")
            .and_then(|g| g.get("theme"))
            .and_then(Value::as_str)
            .map(str::to_string)
    }
    pub fn set_theme(&mut self, theme: &str) {
        let gui = self.raw.entry("gui").or_insert_with(|| json!({}));
        if !gui.is_object() {
            *gui = json!({});
        }
        gui.as_object_mut()
            .expect("object")
            .insert("theme".into(), json!(theme));
    }

    /// Output token budget per response for providers that require one.
    pub fn max_output_tokens(&self) -> u32 {
        self.raw
            .get("maxOutputTokens")
            .and_then(Value::as_u64)
            .map(|v| v as u32)
            .unwrap_or(16_384)
    }

    /// `images.autoResize` (default true): shrink images to provider limits.
    pub fn image_auto_resize(&self) -> bool {
        self.raw
            .get("images")
            .and_then(|i| i.get("autoResize"))
            .and_then(Value::as_bool)
            .unwrap_or(true)
    }

    /// `retry.provider` as a policy for provider HTTP requests.
    pub fn provider_retry(&self) -> crate::provider::RetryPolicy {
        crate::provider::RetryPolicy::from_settings(self)
    }

    /// All configured Ollama instances (including disabled ones); malformed
    /// settings yield an empty list (see [`Settings::try_ollama_settings`]).
    pub fn ollama_settings(&self) -> Vec<OllamaInstance> {
        self.try_ollama_settings().unwrap_or_default()
    }

    /// Parse `ollama.instances`, reporting validation errors.
    pub fn try_ollama_settings(&self) -> Result<Vec<OllamaInstance>> {
        match self.raw.get("ollama") {
            Some(value) if value.get("instances").is_some() => {
                crate::ollama::parse_instances(value)
            }
            _ => Ok(Vec::new()),
        }
    }

    /// Enabled Ollama instances.
    pub fn ollama_instances(&self) -> Vec<OllamaInstance> {
        self.ollama_settings()
            .into_iter()
            .filter(|i| i.enabled)
            .collect()
    }

    /// Replace `ollama.instances`, keeping other keys under `ollama`.
    pub fn set_ollama_instances(&mut self, instances: &[OllamaInstance]) {
        let ollama = self.raw.entry("ollama").or_insert_with(|| json!({}));
        if !ollama.is_object() {
            *ollama = json!({});
        }
        ollama.as_object_mut().expect("object").insert(
            "instances".into(),
            Value::Array(instances.iter().map(OllamaInstance::to_json).collect()),
        );
    }

    /// Add an instance; ids are validated and must be unique (case-insensitive).
    pub fn add_ollama_instance(&mut self, instance: OllamaInstance) -> Result<()> {
        if !crate::ollama::valid_instance_id(&instance.id) {
            bail!("Ollama instance id must contain only letters, numbers, '-' or '_'");
        }
        if !(instance.base_url.starts_with("http://") || instance.base_url.starts_with("https://"))
        {
            bail!("Ollama URL must start with http:// or https://");
        }
        let mut instances = self.try_ollama_settings()?;
        if instances
            .iter()
            .any(|i| i.id.eq_ignore_ascii_case(&instance.id))
        {
            bail!("Ollama instance already exists: {}", instance.id);
        }
        instances.push(instance);
        self.set_ollama_instances(&instances);
        Ok(())
    }

    /// Replace the instance with the same id.
    pub fn update_ollama_instance(&mut self, instance: OllamaInstance) -> Result<()> {
        let mut instances = self.try_ollama_settings()?;
        let Some(slot) = instances
            .iter_mut()
            .find(|i| i.id.eq_ignore_ascii_case(&instance.id))
        else {
            bail!("unknown Ollama instance: {}", instance.id);
        };
        *slot = instance;
        self.set_ollama_instances(&instances);
        Ok(())
    }

    /// Remove an instance; returns the removed record.
    pub fn remove_ollama_instance(&mut self, id: &str) -> Result<OllamaInstance> {
        let mut instances = self.try_ollama_settings()?;
        let Some(index) = instances.iter().position(|i| i.id.eq_ignore_ascii_case(id)) else {
            bail!("unknown Ollama instance: {id}");
        };
        let removed = instances.remove(index);
        self.set_ollama_instances(&instances);
        Ok(removed)
    }
}

/// Merge `overlay` into `base` recursively (objects by key, other values replace).
pub fn deep_merge(base: &mut Map<String, Value>, overlay: &Map<String, Value>) {
    for (key, value) in overlay {
        match (base.get_mut(key), value) {
            (Some(Value::Object(existing)), Value::Object(incoming)) => {
                deep_merge(existing, incoming);
            }
            _ => {
                base.insert(key.clone(), value.clone());
            }
        }
    }
}

/// The settings file in `dir`: `settings.json`, else `settings.yaml`, else
/// `settings.yml`, when one exists.
pub fn existing_settings_file(dir: &Path) -> Option<PathBuf> {
    ["settings.json", "settings.yaml", "settings.yml"]
        .iter()
        .map(|name| dir.join(name))
        .find(|p| p.is_file())
}

/// Read the settings object stored in `dir` (JSON or YAML); missing files
/// yield an empty object.
pub fn read_settings_object(dir: &Path) -> Result<Map<String, Value>> {
    match existing_settings_file(dir) {
        Some(path) => read_settings_file(&path),
        None => Ok(Map::new()),
    }
}

/// Read one settings file, JSON or (by extension) YAML.
pub fn read_settings_file(path: &Path) -> Result<Map<String, Value>> {
    let is_yaml = path
        .extension()
        .and_then(|e| e.to_str())
        .is_some_and(|e| e.eq_ignore_ascii_case("yaml") || e.eq_ignore_ascii_case("yml"));
    if !is_yaml {
        return read_json_object(path);
    }
    let text = fs::read_to_string(path).with_context(|| format!("reading {}", path.display()))?;
    crate::yaml::parse_yaml_settings(&text)
        .map_err(|e| anyhow::anyhow!("parsing {}: {e}", path.display()))
}

pub(crate) fn read_json_object(path: &Path) -> Result<Map<String, Value>> {
    match fs::read_to_string(path) {
        Ok(text) if text.trim().is_empty() => Ok(Map::new()),
        Ok(text) => match serde_json::from_str::<Value>(&text)
            .with_context(|| format!("parsing {}", path.display()))?
        {
            Value::Object(map) => Ok(map),
            _ => anyhow::bail!("{} must contain a JSON object", path.display()),
        },
        Err(err) if err.kind() == std::io::ErrorKind::NotFound => Ok(Map::new()),
        Err(err) => Err(err).with_context(|| format!("reading {}", path.display())),
    }
}

/// Write JSON through a temporary file and rename, optionally owner-only.
pub fn write_json_atomic(path: &Path, value: &Value, private: bool) -> Result<()> {
    let dir = path.parent().context("path has no parent")?;
    fs::create_dir_all(dir).with_context(|| format!("creating {}", dir.display()))?;
    let tmp = dir.join(format!(
        ".{}.{}.tmp",
        path.file_name().and_then(|n| n.to_str()).unwrap_or("file"),
        std::process::id()
    ));
    {
        let mut options = fs::OpenOptions::new();
        options.write(true).create(true).truncate(true);
        #[cfg(unix)]
        if private {
            use std::os::unix::fs::OpenOptionsExt as _;
            options.mode(0o600);
        }
        #[cfg(not(unix))]
        let _ = private;
        let mut file = options
            .open(&tmp)
            .with_context(|| format!("writing {}", tmp.display()))?;
        let mut text = serde_json::to_string_pretty(value)?;
        text.push('\n');
        file.write_all(text.as_bytes())?;
        file.sync_all()?;
    }
    fs::rename(&tmp, path).with_context(|| format!("replacing {}", path.display()))?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn settings_and_auth_round_trip_preserving_unknown_keys() {
        let dir = tempfile::tempdir().unwrap();
        let home = AgentHome::new(dir.path());
        fs::write(
            home.settings_path(),
            r#"{"defaultProvider":"openrouter","memory":{"x":1},
               "ollama":{"instances":[{"id":"local","baseUrl":"http://127.0.0.1:11434/","enabled":true,"models":[{"name":"qwen3"}]},
                                      {"id":"off","baseUrl":"http://x","enabled":false}]}}"#,
        )
        .unwrap();
        let mut settings = Settings::load(&home).unwrap();
        assert_eq!(settings.home(), Some(&home));
        assert_eq!(settings.default_provider().as_deref(), Some("openrouter"));
        let inst = settings.ollama_instances();
        assert_eq!(inst.len(), 1);
        assert_eq!(inst[0].base_url, "http://127.0.0.1:11434");
        assert_eq!(inst[0].models[0].name, "qwen3");
        assert_eq!(settings.ollama_settings().len(), 2);
        assert!(settings.image_auto_resize());
        settings.set_default_model("anthropic", "claude-x");
        settings.set_theme("dark");
        settings.save(&home).unwrap();
        let again = Settings::load(&home).unwrap();
        assert_eq!(again.default_model().as_deref(), Some("claude-x"));
        assert_eq!(again.theme().as_deref(), Some("dark"));
        assert!(again.raw().contains_key("memory"));

        let mut auth = AuthStore::load(&home).unwrap();
        assert!(auth.stored_api_key("openrouter").is_none());
        auth.set_api_key("openrouter", " sk-test ");
        auth.save(&home).unwrap();
        let auth = AuthStore::load(&home).unwrap();
        assert_eq!(
            auth.stored_api_key("openrouter").as_deref(),
            Some("sk-test")
        );
    }

    #[test]
    fn ollama_instance_crud_persists() {
        let dir = tempfile::tempdir().unwrap();
        let home = AgentHome::new(dir.path());
        fs::write(
            home.settings_path(),
            r#"{"ollama":{"keep":true},"images":{"autoResize":false}}"#,
        )
        .unwrap();
        let mut settings = Settings::load(&home).unwrap();
        assert!(!settings.image_auto_resize());
        settings
            .add_ollama_instance(OllamaInstance::new("gpu", "http://box:11434/v1"))
            .unwrap();
        assert_eq!(
            settings
                .add_ollama_instance(OllamaInstance::new("GPU", "http://x"))
                .unwrap_err()
                .to_string(),
            "Ollama instance already exists: GPU"
        );
        assert!(
            settings
                .add_ollama_instance(OllamaInstance::new("bad id", "http://x"))
                .is_err()
        );
        let mut gpu = settings.ollama_settings()[0].clone();
        crate::ollama::configure_instance(&mut gpu, "name", "Big GPU").unwrap();
        settings.update_ollama_instance(gpu).unwrap();
        settings.save(&home).unwrap();
        let mut settings = Settings::load(&home).unwrap();
        assert_eq!(settings.raw()["ollama"]["keep"], true);
        let gpu = &settings.ollama_settings()[0];
        assert_eq!(
            (gpu.name.as_str(), gpu.base_url.as_str()),
            ("Big GPU", "http://box:11434")
        );
        assert_eq!(settings.remove_ollama_instance("gpu").unwrap().id, "gpu");
        assert!(settings.remove_ollama_instance("gpu").is_err());
        assert!(settings.ollama_settings().is_empty());
    }
}
