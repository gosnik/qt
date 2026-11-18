//! [`LlamaProviderBackend`]: the in-process llama.cpp runtime as a qt-core
//! [`ProviderBackend`], so the `llama` provider is dispatched by
//! `qt_core::provider::stream`, listed by the model picker and used for
//! compaction and branch summaries.
//!
//! The model list is `settings.llama.models` plus models registered at
//! runtime (`--discover-local-models`). The [`LlamaRuntime`] is created
//! lazily on first use and replaced when the effective llama settings
//! change (a settings edit, newly discovered models); a replaced runtime
//! finishes its running jobs before its worker exits.

use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, RwLock};

use futures::future::BoxFuture;
use qt_core::config::{AgentHome, Settings};
use qt_core::message::AssistantMessage;
use qt_core::provider::{ModelInfo, ProviderConfig, ProviderKind, Request, StreamEvent};
use tokio_util::sync::CancellationToken;

use crate::discovery::{DiscoveryOptions, DiscoveryResult, discover_models};
use crate::provider::GenerateOptions;
use crate::runtime::{EmbeddingRequest, EmbeddingResult, InstanceTag, LlamaRuntime};
use crate::settings::{LlamaSettings, ModelSettings, PROVIDER_ID, PROVIDER_LABEL};
use crate::{LlamaError, settings};

/// Adjusts the effective settings before a runtime is built (memory child
/// processes shrink the context and pin the grammar, for example).
pub type SettingsOverride = Arc<dyn Fn(&mut LlamaSettings) + Send + Sync>;

struct Cached {
    settings: LlamaSettings,
    runtime: Arc<LlamaRuntime>,
}

/// The `llama` provider backend.
pub struct LlamaProviderBackend {
    home: AgentHome,
    /// Models registered for this process only (runtime discovery).
    extra_models: RwLock<Vec<ModelSettings>>,
    overrides: Option<SettingsOverride>,
    generate_options: GenerateOptions,
    /// `llama.grammar` provider option set by `/llama grammar inline|file|off`
    /// for subsequent turns of this process (archived per-session
    /// `ProviderOptions` metadata).
    runtime_grammar: Mutex<Option<serde_json::Value>>,
    cached: Mutex<Option<Cached>>,
    diagnostics: Mutex<Vec<String>>,
    /// Settings of the latest `providers` call (the engine's layered
    /// settings), used by `stream`.
    last_settings: Mutex<Option<Settings>>,
}

impl std::fmt::Debug for LlamaProviderBackend {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("LlamaProviderBackend")
            .field("home", &self.home.root())
            .finish_non_exhaustive()
    }
}

impl LlamaProviderBackend {
    pub fn new(home: AgentHome) -> Self {
        Self {
            home,
            extra_models: RwLock::new(Vec::new()),
            overrides: None,
            generate_options: GenerateOptions::default(),
            runtime_grammar: Mutex::new(None),
            cached: Mutex::new(None),
            diagnostics: Mutex::new(Vec::new()),
            last_settings: Mutex::new(None),
        }
    }

    /// Apply `overrides` to the effective settings of every runtime.
    pub fn with_settings_override(mut self, overrides: SettingsOverride) -> Self {
        self.overrides = Some(overrides);
        self
    }

    /// Generation options for every request (grammar, output bounds).
    pub fn with_generate_options(mut self, options: GenerateOptions) -> Self {
        self.generate_options = options;
        self
    }

    /// The runtime grammar override (`{"inline"|"file", "root"}` or
    /// `{"enabled": false}`), if one is set.
    pub fn runtime_grammar(&self) -> Option<serde_json::Value> {
        self.runtime_grammar.lock().expect("grammar lock").clone()
    }

    /// Set or clear the runtime grammar override applied to every later
    /// generation (conversation turns, `/llama prompt` and `/llama run`).
    pub fn set_runtime_grammar(&self, grammar: Option<serde_json::Value>) {
        *self.runtime_grammar.lock().expect("grammar lock") = grammar;
    }

    /// The configured generation options with the runtime grammar override
    /// merged in as `llama.grammar` options metadata.
    pub fn generate_options(&self) -> GenerateOptions {
        let mut options = self.generate_options.clone();
        let Some(grammar) = self.runtime_grammar() else {
            return options;
        };
        if !options.options_metadata.is_object() {
            options.options_metadata = serde_json::json!({});
        }
        let metadata = options
            .options_metadata
            .as_object_mut()
            .expect("options metadata object");
        let llama = metadata
            .entry("llama")
            .or_insert_with(|| serde_json::json!({}));
        if !llama.is_object() {
            *llama = serde_json::json!({});
        }
        llama
            .as_object_mut()
            .expect("llama metadata object")
            .insert("grammar".into(), grammar);
        options
    }

    /// Register discovered models for this process. Ids already configured
    /// (case-insensitively) are skipped with the archived diagnostic.
    pub fn register_models(&self, settings: &Settings, models: Vec<ModelSettings>) -> Vec<String> {
        let configured = self.base_settings(settings);
        let mut existing: Vec<String> = configured
            .models
            .iter()
            .map(|m| m.id.to_lowercase())
            .collect();
        let mut extra = self.extra_models.write().expect("llama models lock");
        existing.extend(extra.iter().map(|m| m.id.to_lowercase()));
        let mut diagnostics = Vec::new();
        for model in models {
            let id = model.id.to_lowercase();
            if existing.contains(&id) {
                diagnostics.push(format!(
                    "local model discovery skipped duplicate llama model id: {}",
                    model.id
                ));
                continue;
            }
            existing.push(id);
            extra.push(model);
        }
        diagnostics
    }

    /// Discover GGUF models under `roots`, persist them to `settings.json`
    /// (archived `--discover-local-models`) and register them for this
    /// process. Returns the discovery result and every diagnostic.
    pub fn discover_and_register(
        &self,
        settings: &Settings,
        cwd: &Path,
        roots: &[PathBuf],
    ) -> (DiscoveryResult, Vec<String>) {
        let discovered = discover_models(&DiscoveryOptions {
            roots: roots.to_vec(),
            recursive: true,
        });
        let mut diagnostics = discovered.diagnostics.clone();
        match crate::discovery::persist_discovered_models(
            &self.home.settings_path(),
            cwd,
            &discovered,
        ) {
            Ok(persisted) => diagnostics.extend(persisted.diagnostics),
            Err(e) => diagnostics.push(format!(
                "local model discovery could not update settings: {e}"
            )),
        }
        diagnostics.extend(self.register_models(
            settings,
            discovered.models.iter().map(|m| m.to_settings()).collect(),
        ));
        (discovered, diagnostics)
    }

    /// Settings problems found while reading `settings.llama`.
    pub fn diagnostics(&self) -> Vec<String> {
        self.diagnostics.lock().expect("diagnostics lock").clone()
    }

    fn base_settings(&self, settings: &Settings) -> LlamaSettings {
        match LlamaSettings::from_settings(settings, &self.home) {
            Ok(s) => s,
            Err(e) => {
                let mut diagnostics = self.diagnostics.lock().expect("diagnostics lock");
                let message = format!("invalid llama settings: {e}");
                if !diagnostics.contains(&message) {
                    diagnostics.push(message);
                }
                LlamaSettings::default()
            }
        }
    }

    /// The effective llama settings: `settings.llama`, runtime-registered
    /// models and the configured override.
    pub fn effective_settings(&self, settings: &Settings) -> LlamaSettings {
        let mut effective = self.base_settings(settings);
        let extra = self.extra_models.read().expect("llama models lock");
        for model in extra.iter() {
            if !effective
                .models
                .iter()
                .any(|m| m.id.eq_ignore_ascii_case(&model.id))
            {
                effective.models.push(model.clone());
                effective.models_configured = true;
            }
        }
        drop(extra);
        if let Some(overrides) = &self.overrides {
            overrides(&mut effective);
        }
        effective
    }

    /// The runtime for the effective settings (created on first use).
    pub fn runtime(&self, settings: &Settings) -> Arc<LlamaRuntime> {
        let effective = self.effective_settings(settings);
        let mut cached = self.cached.lock().expect("llama runtime lock");
        if let Some(c) = cached.as_ref()
            && c.settings == effective
        {
            return c.runtime.clone();
        }
        let runtime = Arc::new(LlamaRuntime::new(effective.clone()));
        *cached = Some(Cached {
            settings: effective,
            runtime: runtime.clone(),
        });
        runtime
    }

    /// The runtime if one was created (status panels must not load one).
    pub fn current_runtime(&self) -> Option<Arc<LlamaRuntime>> {
        self.cached
            .lock()
            .expect("llama runtime lock")
            .as_ref()
            .map(|c| c.runtime.clone())
    }

    /// Embed `text` with model `model_id` on the embedding instance.
    pub fn embed(
        &self,
        settings: &Settings,
        request: EmbeddingRequest,
    ) -> Result<EmbeddingResult, LlamaError> {
        self.runtime(settings).embed(request, InstanceTag::Embed)
    }

    /// Model infos for the picker (configured and registered models).
    pub fn model_infos(&self, settings: &Settings) -> Vec<ModelInfo> {
        self.effective_settings(settings).model_infos()
    }
}

/// The provider config the backend serves: local (no key), the `llama.cpp`
/// API name and the configured models as catalog specs.
pub fn provider_config(effective: &LlamaSettings) -> ProviderConfig {
    let models = effective
        .models
        .iter()
        .map(|m| qt_core::catalog::ModelSpec {
            provider: PROVIDER_ID.into(),
            id: m.id.clone(),
            name: if m.name.is_empty() {
                m.id.clone()
            } else {
                m.name.clone()
            },
            api: settings::PROVIDER_API.into(),
            input: vec!["text".into()],
            context_window: effective
                .effective_context_window(m)
                .and_then(|v| u64::try_from(v).ok())
                .unwrap_or(0),
            max_tokens: effective
                .effective_max_tokens(m)
                .and_then(|v| u64::try_from(v).ok())
                .unwrap_or(0),
            ..Default::default()
        })
        .collect();
    ProviderConfig {
        id: PROVIDER_ID.into(),
        label: PROVIDER_LABEL.into(),
        kind: ProviderKind::default(),
        local: true,
        models: Arc::new(models),
        ..Default::default()
    }
}

impl qt_core::provider::ProviderBackend for LlamaProviderBackend {
    fn name(&self) -> &str {
        PROVIDER_ID
    }

    fn providers(&self, settings: &Settings) -> Vec<ProviderConfig> {
        *self.last_settings.lock().expect("settings lock") = Some(settings.clone());
        let effective = self.effective_settings(settings);
        if effective.backend.enabled == Some(false) {
            return Vec::new();
        }
        vec![provider_config(&effective)]
    }

    fn list_models<'a>(
        &'a self,
        _provider: &'a ProviderConfig,
        settings: &'a Settings,
    ) -> BoxFuture<'a, anyhow::Result<Vec<ModelInfo>>> {
        Box::pin(async move { Ok(self.model_infos(settings)) })
    }

    fn stream<'a>(
        &'a self,
        _provider: &'a ProviderConfig,
        request: &'a Request,
        cancel: &'a CancellationToken,
        on_event: &'a mut (dyn FnMut(StreamEvent) + Send),
    ) -> BoxFuture<'a, AssistantMessage> {
        Box::pin(async move {
            let last = self.last_settings.lock().expect("settings lock").clone();
            let settings = match last {
                Some(s) => s,
                None => Settings::load(&self.home).unwrap_or_default(),
            };
            let runtime = self.runtime(&settings);
            let options = self.generate_options();
            crate::provider::stream_with_options(&runtime, request, &options, cancel, on_event)
                .await
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use qt_core::provider::ProviderBackend as _;
    use serde_json::json;

    fn write_settings(home: &Path, value: serde_json::Value) {
        std::fs::create_dir_all(home).unwrap();
        std::fs::write(home.join("settings.json"), value.to_string()).unwrap();
    }

    #[test]
    fn providers_list_configured_and_registered_models() {
        let dir = tempfile::tempdir().unwrap();
        let home = dir.path().join("home");
        let model = dir.path().join("a.gguf");
        std::fs::write(&model, b"GGUF").unwrap();
        write_settings(
            &home,
            json!({"llama": {"context": {"contextSize": 4096}, "models": [
                {"id": "alpha", "name": "Alpha", "path": model.to_string_lossy()}
            ]}}),
        );
        let agent_home = AgentHome::new(&home);
        let settings = Settings::load(&agent_home).unwrap();
        let backend = LlamaProviderBackend::new(agent_home);
        let providers = backend.providers(&settings);
        assert!(
            backend.diagnostics().is_empty(),
            "{:?}",
            backend.diagnostics()
        );
        assert_eq!(providers.len(), 1);
        assert_eq!(providers[0].id, "llama");
        assert!(providers[0].is_ready());
        assert_eq!(providers[0].models[0].context_window, 4096);

        let extra = ModelSettings {
            id: "beta".into(),
            path: model.to_string_lossy().into_owned(),
            ..Default::default()
        };
        let dup = ModelSettings {
            id: "ALPHA".into(),
            ..extra.clone()
        };
        let diagnostics = backend.register_models(&settings, vec![extra, dup]);
        assert_eq!(
            diagnostics,
            ["local model discovery skipped duplicate llama model id: ALPHA"]
        );
        let ids: Vec<String> = backend
            .model_infos(&settings)
            .into_iter()
            .map(|m| m.id)
            .collect();
        assert_eq!(ids, ["alpha", "beta"]);

        // The runtime is reused until the effective settings change.
        let first = backend.runtime(&settings);
        assert!(Arc::ptr_eq(&first, &backend.runtime(&settings)));
        let overridden = LlamaProviderBackend::new(AgentHome::new(&home)).with_settings_override(
            Arc::new(|s: &mut LlamaSettings| s.context.context_size = Some(8192)),
        );
        assert_eq!(
            overridden
                .effective_settings(&settings)
                .context
                .context_size,
            Some(8192)
        );
    }

    #[test]
    fn disabled_backend_offers_no_provider() {
        let dir = tempfile::tempdir().unwrap();
        write_settings(
            dir.path(),
            json!({"llama": {"backend": {"enabled": false}}}),
        );
        let home = AgentHome::new(dir.path());
        let settings = Settings::load(&home).unwrap();
        assert!(
            LlamaProviderBackend::new(home)
                .providers(&settings)
                .is_empty()
        );
    }

    #[test]
    fn invalid_settings_are_reported_once() {
        let dir = tempfile::tempdir().unwrap();
        write_settings(
            dir.path(),
            json!({"llama": {"context": {"contextSize": "x"}}}),
        );
        let home = AgentHome::new(dir.path());
        let settings = Settings::load(&home).unwrap();
        let backend = LlamaProviderBackend::new(home);
        let _ = backend.providers(&settings);
        let _ = backend.providers(&settings);
        assert_eq!(backend.diagnostics().len(), 1);
    }

    #[test]
    fn runtime_grammar_override_merges_into_generate_options() {
        let dir = tempfile::tempdir().unwrap();
        let base = GenerateOptions {
            options_metadata: json!({"grammar": "root ::= \"x\"", "llama": {"generation": {"topK": 3}}}),
            ..Default::default()
        };
        let backend = LlamaProviderBackend::new(AgentHome::new(dir.path()))
            .with_generate_options(base.clone());
        assert_eq!(backend.generate_options(), base);
        backend.set_runtime_grammar(Some(json!({"inline": "root ::= \"y\"", "root": "root"})));
        let merged = backend.generate_options();
        assert_eq!(
            merged.options_metadata["llama"],
            json!({"generation": {"topK": 3}, "grammar": {"inline": "root ::= \"y\"", "root": "root"}})
        );
        let settings = LlamaSettings::default();
        let resolved = crate::sampling::resolve_grammar(&settings, &merged).unwrap();
        assert!(resolved.enabled);
        assert_eq!(resolved.grammar, "root ::= \"y\"");
        backend.set_runtime_grammar(Some(json!({"enabled": false})));
        let off = crate::sampling::resolve_grammar(&settings, &backend.generate_options()).unwrap();
        assert!(!off.enabled);
        backend.set_runtime_grammar(None);
        assert_eq!(backend.runtime_grammar(), None);
        assert_eq!(backend.generate_options(), base);
    }
}
