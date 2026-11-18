//! Pluggable provider backends.
//!
//! Built-in wire APIs (Anthropic, OpenAI chat / Responses, Codex, Azure) are
//! dispatched by [`super::ProviderKind`]. Providers that are not HTTP APIs
//! — the in-process llama.cpp runtime in `qt-llama`, providers registered
//! by extensions — implement [`ProviderBackend`] and are registered with the
//! engine (`EngineBuilder::provider_backend`). The engine stamps every
//! [`ProviderConfig`] a backend returns with a [`BackendHandle`], so
//! [`super::stream`] and [`super::list_models`] dispatch to the backend
//! wherever the config travels: conversation runs, compaction summaries,
//! branch summaries, model listing and the model picker.

use std::fmt;
use std::sync::Arc;

use anyhow::Result;
use futures::future::BoxFuture;
use tokio_util::sync::CancellationToken;

use super::{ModelInfo, ProviderConfig, Request, StreamEvent};
use crate::config::Settings;
use crate::message::AssistantMessage;

/// A provider implementation outside the built-in HTTP transports.
pub trait ProviderBackend: Send + Sync {
    /// Short name used in diagnostics (`llama`, an extension id, ...).
    fn name(&self) -> &str;

    /// The providers this backend serves, built from the current settings.
    /// Ids must be unique; an id equal to a built-in provider replaces it.
    /// `ProviderConfig::backend` is filled in by the engine.
    fn providers(&self, settings: &Settings) -> Vec<ProviderConfig>;

    /// Models offered by `provider` (one of [`Self::providers`]).
    fn list_models<'a>(
        &'a self,
        provider: &'a ProviderConfig,
        settings: &'a Settings,
    ) -> BoxFuture<'a, Result<Vec<ModelInfo>>>;

    /// Stream one assistant response. Like [`super::stream`], failures are
    /// returned as `stopReason: "error"` messages and cancellation as
    /// `stopReason: "aborted"`.
    fn stream<'a>(
        &'a self,
        provider: &'a ProviderConfig,
        request: &'a Request,
        cancel: &'a CancellationToken,
        on_event: &'a mut (dyn FnMut(StreamEvent) + Send),
    ) -> BoxFuture<'a, AssistantMessage>;
}

/// A backend attached to a [`ProviderConfig`]. Compares by identity so
/// configs stay `PartialEq`.
#[derive(Clone)]
pub struct BackendHandle(pub Arc<dyn ProviderBackend>);

impl BackendHandle {
    pub fn new(backend: Arc<dyn ProviderBackend>) -> Self {
        Self(backend)
    }

    pub fn backend(&self) -> &Arc<dyn ProviderBackend> {
        &self.0
    }
}

impl PartialEq for BackendHandle {
    fn eq(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.0, &other.0)
    }
}

impl fmt::Debug for BackendHandle {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "BackendHandle({})", self.0.name())
    }
}

/// The backends registered with an engine, in registration order.
#[derive(Clone, Default)]
pub struct ProviderBackends {
    backends: Vec<Arc<dyn ProviderBackend>>,
}

impl fmt::Debug for ProviderBackends {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_list()
            .entries(self.backends.iter().map(|b| b.name().to_string()))
            .finish()
    }
}

impl ProviderBackends {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn push(&mut self, backend: Arc<dyn ProviderBackend>) {
        self.backends.push(backend);
    }

    pub fn is_empty(&self) -> bool {
        self.backends.is_empty()
    }

    pub fn iter(&self) -> impl Iterator<Item = &Arc<dyn ProviderBackend>> {
        self.backends.iter()
    }

    /// Merge backend providers into `builtin`: each backend config is
    /// stamped with its handle and replaces a built-in config with the same
    /// id (later backends win over earlier ones).
    pub fn merge_into(&self, builtin: &mut Vec<ProviderConfig>, settings: &Settings) {
        for backend in &self.backends {
            let handle = BackendHandle::new(backend.clone());
            for mut config in backend.providers(settings) {
                if config.id.is_empty() {
                    continue;
                }
                config.backend = Some(handle.clone());
                match builtin.iter_mut().find(|p| p.id == config.id) {
                    Some(slot) => *slot = config,
                    None => builtin.push(config),
                }
            }
        }
    }
}
