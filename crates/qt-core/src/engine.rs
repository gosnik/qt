//! Front-end facade: owns the async runtime, configuration and conversations.
//!
//! All network and tool work runs on the engine's own Tokio runtime, so a
//! front end on any executor (GPUI's, or plain blocking code) can call into
//! it. Results come back through `async_channel` receivers and Tokio join
//! handles, both of which can be awaited from any executor.

use std::future::Future;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, RwLock};

use anyhow::{Context as _, Result, bail};
use async_channel::Receiver;
use tokio::task::JoinHandle;
use tokio_util::sync::CancellationToken;

use crate::agent::{AgentEvent, AgentParams, run_agent};
use crate::config::{AgentHome, AuthStore, Settings};
use crate::message::{ContentBlock, Message, Usage, UserContent, now_millis};
use crate::prompt::{build_system_prompt, discover_context_files};
use crate::provider::{self, ModelInfo, ModelRef, ProviderConfig, ThinkingLevel};
use crate::session::{Session, SessionSummary, list_sessions};
use crate::tools::ToolRegistry;

struct EngineInner {
    runtime: tokio::runtime::Runtime,
    http: reqwest::Client,
    home: AgentHome,
    cwd: PathBuf,
    settings: RwLock<Settings>,
    auth: RwLock<AuthStore>,
    tools: ToolRegistry,
}

/// Models per provider id, or that provider's discovery error.
pub type ModelListing = Vec<(String, Result<Vec<ModelInfo>, String>)>;

/// Shared, cheaply cloneable engine handle.
#[derive(Clone)]
pub struct Engine {
    inner: Arc<EngineInner>,
}

impl Engine {
    pub fn new(home: AgentHome, cwd: impl Into<PathBuf>) -> Result<Self> {
        let cwd = cwd.into();
        let cwd = cwd.canonicalize().unwrap_or(cwd);
        let runtime = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(4)
            .thread_name("qt-core")
            .enable_all()
            .build()
            .context("starting async runtime")?;
        let http = reqwest::Client::builder()
            .user_agent(concat!("qt/", env!("CARGO_PKG_VERSION")))
            .connect_timeout(std::time::Duration::from_secs(20))
            .build()
            .context("building HTTP client")?;
        let settings = Settings::load(&home).unwrap_or_default();
        let auth = AuthStore::load(&home).unwrap_or_default();
        Ok(Self {
            inner: Arc::new(EngineInner {
                runtime,
                http,
                home,
                cwd,
                settings: RwLock::new(settings),
                auth: RwLock::new(auth),
                tools: ToolRegistry::coding(),
            }),
        })
    }

    pub fn home(&self) -> &AgentHome {
        &self.inner.home
    }
    pub fn cwd(&self) -> &Path {
        &self.inner.cwd
    }
    pub fn tools(&self) -> &ToolRegistry {
        &self.inner.tools
    }

    /// Run `future` on the engine runtime; the handle can be awaited anywhere.
    pub fn spawn<F>(&self, future: F) -> JoinHandle<F::Output>
    where
        F: Future + Send + 'static,
        F::Output: Send + 'static,
    {
        self.inner.runtime.spawn(future)
    }

    /// Block the current (non-async) thread on `future`.
    pub fn block_on<F: Future>(&self, future: F) -> F::Output {
        self.inner.runtime.block_on(future)
    }

    pub fn settings(&self) -> Settings {
        self.inner.settings.read().expect("settings lock").clone()
    }

    pub fn update_settings(&self, change: impl FnOnce(&mut Settings)) -> Result<()> {
        let mut settings = self.inner.settings.write().expect("settings lock");
        change(&mut settings);
        settings.save(&self.inner.home)
    }

    pub fn providers(&self) -> Vec<ProviderConfig> {
        let settings = self.inner.settings.read().expect("settings lock");
        let auth = self.inner.auth.read().expect("auth lock");
        provider::configured_providers(&settings, &auth)
    }

    pub fn provider(&self, id: &str) -> Option<ProviderConfig> {
        self.providers().into_iter().find(|p| p.id == id)
    }

    pub fn has_stored_api_key(&self, provider: &str) -> bool {
        self.inner
            .auth
            .read()
            .expect("auth lock")
            .stored_api_key(provider)
            .is_some()
    }

    /// Store (or with an empty key, remove) the API key for `provider`.
    pub fn set_api_key(&self, provider: &str, key: &str) -> Result<()> {
        let mut auth = self.inner.auth.write().expect("auth lock");
        if key.trim().is_empty() {
            auth.remove_api_key(provider);
        } else {
            auth.set_api_key(provider, key);
        }
        auth.save(&self.inner.home)
    }

    /// Re-read `auth.json`, for example after `qt_core::auth::login_and_store`
    /// or `qt_core::auth::logout` changed it.
    pub fn reload_auth(&self) -> Result<()> {
        let auth = AuthStore::load(&self.inner.home)?;
        *self.inner.auth.write().expect("auth lock") = auth;
        Ok(())
    }

    /// The model from settings, if its provider is usable.
    pub fn default_model(&self) -> Option<ModelRef> {
        let settings = self.settings();
        let model = ModelRef::new(settings.default_provider()?, settings.default_model()?);
        self.provider(&model.provider)
            .filter(ProviderConfig::is_ready)
            .map(|_| model)
    }

    pub fn set_default_model(&self, model: &ModelRef) -> Result<()> {
        self.update_settings(|s| s.set_default_model(&model.provider, &model.id))
    }

    /// Discover models from every ready provider, concurrently.
    pub fn list_models(&self) -> JoinHandle<ModelListing> {
        let http = self.inner.http.clone();
        let settings = self.settings();
        let providers: Vec<ProviderConfig> = self
            .providers()
            .into_iter()
            .filter(ProviderConfig::is_ready)
            .collect();
        self.spawn(async move {
            let lookups = providers.into_iter().map(|p| {
                let http = http.clone();
                let settings = settings.clone();
                async move {
                    let result = provider::list_models(&http, &p, &settings)
                        .await
                        .map_err(|e| format!("{e:#}"));
                    (p.id.clone(), result)
                }
            });
            futures::future::join_all(lookups).await
        })
    }

    pub fn sessions(&self) -> Vec<SessionSummary> {
        list_sessions(&self.inner.home, Some(&self.inner.cwd))
    }

    pub fn new_conversation(&self, model: Option<ModelRef>) -> Conversation {
        Conversation::new(
            self.clone(),
            Session::create(&self.inner.home, &self.inner.cwd),
            model,
        )
    }

    /// Resume a session file. The model is taken from its last assistant message.
    pub fn open_conversation(&self, path: &Path) -> Result<Conversation> {
        let session = Session::open(path)?;
        let model = session
            .context()
            .iter()
            .rev()
            .find_map(|m| match m {
                Message::Assistant(a) if !a.provider.is_empty() && !a.model.is_empty() => {
                    Some(ModelRef::new(a.provider.clone(), a.model.clone()))
                }
                _ => None,
            })
            .filter(|m| self.provider(&m.provider).is_some_and(|p| p.is_ready()))
            .or_else(|| self.default_model());
        Ok(Conversation::new(self.clone(), session, model))
    }

    fn system_prompt(&self) -> String {
        let files = discover_context_files(&self.inner.home, &self.inner.cwd);
        build_system_prompt(&self.inner.tools, &files, &self.inner.cwd)
    }
}

struct ConversationState {
    session: Session,
    messages: Vec<Message>,
    model: Option<ModelRef>,
    thinking: ThinkingLevel,
    running: Option<CancellationToken>,
    usage: Usage,
}

/// One conversation (session) driven by a front end.
#[derive(Clone)]
pub struct Conversation {
    engine: Engine,
    state: Arc<Mutex<ConversationState>>,
}

impl Conversation {
    fn new(engine: Engine, session: Session, model: Option<ModelRef>) -> Self {
        let messages = session.context();
        let mut usage = Usage::default();
        for m in &messages {
            if let Message::Assistant(a) = m {
                usage.add(&a.usage);
            }
        }
        Self {
            engine,
            state: Arc::new(Mutex::new(ConversationState {
                session,
                messages,
                model,
                thinking: ThinkingLevel::Off,
                running: None,
                usage,
            })),
        }
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, ConversationState> {
        self.state.lock().expect("conversation lock")
    }

    pub fn messages(&self) -> Vec<Message> {
        self.lock().messages.clone()
    }
    pub fn session_path(&self) -> PathBuf {
        self.lock().session.path().to_path_buf()
    }
    pub fn model(&self) -> Option<ModelRef> {
        self.lock().model.clone()
    }
    pub fn set_model(&self, model: ModelRef) {
        self.lock().model = Some(model);
    }
    pub fn thinking(&self) -> ThinkingLevel {
        self.lock().thinking
    }
    pub fn set_thinking(&self, level: ThinkingLevel) {
        self.lock().thinking = level;
    }
    pub fn is_running(&self) -> bool {
        self.lock().running.is_some()
    }
    /// Cumulative usage of all assistant responses.
    pub fn usage(&self) -> Usage {
        self.lock().usage
    }
    /// Context size reported by the latest successful response.
    pub fn context_tokens(&self) -> u64 {
        self.lock()
            .messages
            .iter()
            .rev()
            .find_map(|m| match m {
                Message::Assistant(a) if a.usage.context_tokens() > 0 => {
                    Some(a.usage.context_tokens())
                }
                _ => None,
            })
            .unwrap_or(0)
    }

    /// Cancel the running agent, if any.
    pub fn abort(&self) {
        if let Some(token) = &self.lock().running {
            token.cancel();
        }
    }

    /// Send a user prompt and start the agent. Events arrive on the receiver,
    /// which closes after `AgentEvent::AgentEnd`.
    pub fn prompt(&self, text: &str, images: Vec<ContentBlock>) -> Result<Receiver<AgentEvent>> {
        let text = text.trim();
        if text.is_empty() && images.is_empty() {
            bail!("Nothing to send");
        }
        let cancel = CancellationToken::new();
        let (params, history) = {
            let mut state = self.lock();
            if state.running.is_some() {
                bail!("The agent is already running");
            }
            let model = state.model.clone().context("No model selected")?;
            let provider = self
                .engine
                .provider(&model.provider)
                .with_context(|| format!("Unknown provider {}", model.provider))?;
            if !provider.is_ready() {
                bail!("No API key configured for {}", provider.label);
            }
            state.running = Some(cancel.clone());
            let params = AgentParams {
                http: self.engine.inner.http.clone(),
                provider,
                model: model.id,
                system: self.engine.system_prompt(),
                tools: self.engine.inner.tools.clone(),
                max_tokens: self.engine.settings().max_output_tokens(),
                thinking: state.thinking,
                cwd: self.engine.inner.cwd.clone(),
            };
            (params, state.messages.clone())
        };
        let content = if images.is_empty() {
            UserContent::Text(text.to_string())
        } else {
            let mut blocks = vec![ContentBlock::text(text)];
            blocks.extend(images);
            UserContent::Blocks(blocks)
        };
        let user = Message::User {
            content,
            timestamp: Some(now_millis()),
        };
        let (tx, rx) = async_channel::unbounded();
        let state = self.state.clone();
        self.engine.spawn(async move {
            let record_state = state.clone();
            let mut record = move |message: &Message| -> Result<(), String> {
                let mut s = record_state.lock().expect("conversation lock");
                s.messages.push(message.clone());
                if let Message::Assistant(a) = message {
                    s.usage.add(&a.usage);
                }
                s.session.append(message).map_err(|e| format!("{e:#}"))
            };
            run_agent(params, history, vec![user], cancel, tx, &mut record).await;
            state.lock().expect("conversation lock").running = None;
        });
        Ok(rx)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn conversation_requires_model_and_key() {
        let dir = tempfile::tempdir().unwrap();
        let engine = Engine::new(AgentHome::new(dir.path().join("home")), dir.path()).unwrap();
        let conv = engine.new_conversation(None);
        assert!(
            conv.prompt("hi", vec![])
                .unwrap_err()
                .to_string()
                .contains("No model selected")
        );
        conv.set_model(ModelRef::new("nope", "m"));
        assert!(
            conv.prompt("hi", vec![])
                .unwrap_err()
                .to_string()
                .contains("Unknown provider")
        );
        assert!(conv.prompt("  ", vec![]).is_err());
        assert!(!conv.is_running());
    }

    #[test]
    fn api_keys_persist_and_enable_providers() {
        let dir = tempfile::tempdir().unwrap();
        let home = AgentHome::new(dir.path().join("home"));
        let engine = Engine::new(home.clone(), dir.path()).unwrap();
        engine.set_api_key("anthropic", "sk-ant-test").unwrap();
        assert!(engine.provider("anthropic").unwrap().is_ready());
        engine
            .set_default_model(&ModelRef::new("anthropic", "claude-test"))
            .unwrap();
        let again = Engine::new(home, dir.path()).unwrap();
        assert_eq!(
            again.default_model(),
            Some(ModelRef::new("anthropic", "claude-test"))
        );
        again.set_api_key("anthropic", "").unwrap();
        assert!(!again.has_stored_api_key("anthropic"));
    }
}
