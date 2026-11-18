//! Extension wiring: discovery options from `-e` / `--no-extensions`,
//! extension providers as a provider backend, `/context` extension info,
//! the slash-command registry entries and the run sink for extension
//! started runs.

use std::sync::{Arc, OnceLock};

use futures::future::BoxFuture;
use qt_core::catalog::ModelCatalog;
use qt_core::config::{AgentHome, AuthStore, Settings};
use qt_core::engine::{Engine, ExtensionInfo};
use qt_core::message::AssistantMessage;
use qt_core::provider::{self, ModelInfo, ProviderBackend, ProviderConfig, Request, StreamEvent};
use qt_core::slash::SlashCommand;
use qt_extensions::Extensions;
use tokio_util::sync::CancellationToken;

/// Providers registered by extensions (`pi.registerProvider`, a
/// `models.json` provider entry each), served by the built-in transports.
pub struct ExtensionProviderBackend {
    extensions: Extensions,
    home: AgentHome,
    engine: Arc<OnceLock<Engine>>,
}

impl ExtensionProviderBackend {
    pub fn new(extensions: Extensions, home: AgentHome, engine: Arc<OnceLock<Engine>>) -> Self {
        Self {
            extensions,
            home,
            engine,
        }
    }

    fn names(&self) -> Vec<String> {
        self.extensions
            .providers()
            .into_iter()
            .map(|p| p.provider.name)
            .collect()
    }
}

impl ProviderBackend for ExtensionProviderBackend {
    fn name(&self) -> &str {
        "extensions"
    }

    fn providers(&self, settings: &Settings) -> Vec<ProviderConfig> {
        let names = self.names();
        if names.is_empty() {
            return Vec::new();
        }
        let (mut catalog, _) = ModelCatalog::load(&self.home, settings);
        for error in self.extensions.apply_providers(&mut catalog) {
            tracing::warn!(target: "qt_runtime::extensions", "{error}");
        }
        let auth = AuthStore::load(&self.home).unwrap_or_default();
        provider::catalog_provider_configs(&catalog, settings, &auth)
            .into_iter()
            .filter(|p| names.iter().any(|n| n.eq_ignore_ascii_case(&p.id)))
            .collect()
    }

    fn list_models<'a>(
        &'a self,
        provider: &'a ProviderConfig,
        _settings: &'a Settings,
    ) -> BoxFuture<'a, anyhow::Result<Vec<ModelInfo>>> {
        Box::pin(async move {
            Ok(provider
                .models
                .iter()
                .map(|m| ModelInfo::from_spec(&provider.id, m))
                .collect())
        })
    }

    fn stream<'a>(
        &'a self,
        provider: &'a ProviderConfig,
        request: &'a Request,
        cancel: &'a CancellationToken,
        on_event: &'a mut (dyn FnMut(StreamEvent) + Send),
    ) -> BoxFuture<'a, AssistantMessage> {
        Box::pin(async move {
            let mut config = provider.clone();
            config.backend = None;
            let Some(engine) = self.engine.get() else {
                return provider::PartialMessage::new(&config, &request.model)
                    .finish_error("extension providers are not available before startup".into());
            };
            provider::stream(engine.http(), &config, request, cancel, on_event).await
        })
    }
}

/// `/context` extension entries.
pub fn extension_infos(extensions: &Extensions) -> Vec<ExtensionInfo> {
    extensions
        .snapshot()
        .extensions
        .iter()
        .map(|e| ExtensionInfo {
            name: e.manifest.name.clone(),
            path: e.path.clone(),
            manifest: e.manifest.to_json(),
        })
        .collect()
}

/// Extension commands for the slash registry.
pub fn extension_slash_commands(extensions: &Extensions) -> Vec<SlashCommand> {
    let snapshot = extensions.snapshot();
    snapshot
        .extensions
        .iter()
        .flat_map(|e| {
            e.manifest.commands.iter().map(move |c| {
                SlashCommand::extension(
                    c.name.clone(),
                    c.description
                        .clone()
                        .or_else(|| c.title.clone())
                        .unwrap_or_default(),
                    Some(e.path.clone()),
                )
            })
        })
        .collect()
}
