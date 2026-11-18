//! Agent home, settings and credentials.
//!
//! The layout is shared with the archived C++ qt: the agent home defaults to
//! `~/.qt` (override with `QT_CODING_AGENT_DIR`), settings live in
//! `settings.json`, credentials in `auth.json` and sessions in `sessions/`.
//! Unknown keys in both JSON files are preserved on write.

use std::fs;
use std::io::Write as _;
use std::path::{Path, PathBuf};

use anyhow::{Context as _, Result};
use serde_json::{Map, Value, json};

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
    pub fn sessions_dir(&self) -> PathBuf {
        self.root.join("sessions")
    }
    /// Global instruction file appended to every system prompt.
    pub fn global_agents_file(&self) -> PathBuf {
        self.root.join("AGENTS.md")
    }
}

/// An Ollama instance configured under `settings.ollama.instances`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct OllamaInstance {
    pub id: String,
    pub base_url: String,
    /// Model names recorded in settings (may be empty; the server is queried too).
    pub models: Vec<String>,
}

/// `settings.json`, kept as a JSON object so unknown keys survive rewrites.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Settings {
    raw: Map<String, Value>,
}

impl Settings {
    pub fn load(home: &AgentHome) -> Result<Self> {
        Ok(Self {
            raw: read_json_object(&home.settings_path())?,
        })
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

    pub fn ollama_instances(&self) -> Vec<OllamaInstance> {
        let Some(list) = self
            .raw
            .get("ollama")
            .and_then(|o| o.get("instances"))
            .and_then(Value::as_array)
        else {
            return Vec::new();
        };
        list.iter()
            .filter(|inst| inst.get("enabled").and_then(Value::as_bool).unwrap_or(true))
            .filter_map(|inst| {
                let id = inst.get("id").and_then(Value::as_str)?.to_string();
                let base_url = inst
                    .get("baseUrl")
                    .and_then(Value::as_str)?
                    .trim_end_matches('/')
                    .to_string();
                let models = inst
                    .get("models")
                    .and_then(Value::as_array)
                    .map(|models| {
                        models
                            .iter()
                            .filter_map(|m| {
                                m.get("name")
                                    .or_else(|| m.get("id"))
                                    .and_then(Value::as_str)
                            })
                            .map(str::to_string)
                            .collect()
                    })
                    .unwrap_or_default();
                Some(OllamaInstance {
                    id,
                    base_url,
                    models,
                })
            })
            .collect()
    }
}

/// `auth.json`: `{ "version": 1, "providers": { "<id>": { "apiKey": "..." } } }`.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct AuthStore {
    raw: Map<String, Value>,
}

impl AuthStore {
    pub fn load(home: &AgentHome) -> Result<Self> {
        Ok(Self {
            raw: read_json_object(&home.auth_path())?,
        })
    }

    pub fn save(&self, home: &AgentHome) -> Result<()> {
        let mut raw = self.raw.clone();
        raw.entry("version").or_insert(json!(1));
        write_json_atomic(&home.auth_path(), &Value::Object(raw), true)
    }

    /// Stored key for `provider`, falling back to the provider's environment variable.
    pub fn api_key(&self, provider: &str) -> Option<String> {
        self.stored_api_key(provider).or_else(|| {
            env_var_for(provider)
                .and_then(|name| std::env::var(name).ok())
                .filter(|v| !v.trim().is_empty())
        })
    }

    pub fn stored_api_key(&self, provider: &str) -> Option<String> {
        self.raw
            .get("providers")
            .and_then(|p| p.get(provider))
            .and_then(|p| p.get("apiKey"))
            .and_then(Value::as_str)
            .map(str::to_string)
            .filter(|k| !k.trim().is_empty())
    }

    pub fn set_api_key(&mut self, provider: &str, key: &str) {
        let providers = self.raw.entry("providers").or_insert_with(|| json!({}));
        if !providers.is_object() {
            *providers = json!({});
        }
        let providers = providers.as_object_mut().expect("object");
        let entry = providers
            .entry(provider.to_string())
            .or_insert_with(|| json!({}));
        if !entry.is_object() {
            *entry = json!({});
        }
        entry
            .as_object_mut()
            .expect("object")
            .insert("apiKey".into(), json!(key.trim()));
    }

    pub fn remove_api_key(&mut self, provider: &str) {
        if let Some(entry) = self
            .raw
            .get_mut("providers")
            .and_then(|p| p.get_mut(provider))
            .and_then(Value::as_object_mut)
        {
            entry.remove("apiKey");
        }
    }
}

/// Environment variable consulted when no key is stored for `provider`.
pub fn env_var_for(provider: &str) -> Option<&'static str> {
    match provider {
        "anthropic" => Some("ANTHROPIC_API_KEY"),
        "openai" => Some("OPENAI_API_KEY"),
        "openrouter" => Some("OPENROUTER_API_KEY"),
        _ => None,
    }
}

fn read_json_object(path: &Path) -> Result<Map<String, Value>> {
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
        assert_eq!(settings.default_provider().as_deref(), Some("openrouter"));
        let inst = settings.ollama_instances();
        assert_eq!(inst.len(), 1);
        assert_eq!(inst[0].base_url, "http://127.0.0.1:11434");
        assert_eq!(inst[0].models, vec!["qwen3".to_string()]);
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
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt as _;
            let mode = fs::metadata(home.auth_path()).unwrap().permissions().mode();
            assert_eq!(mode & 0o777, 0o600);
        }
    }
}
