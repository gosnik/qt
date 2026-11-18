//! Front-end facade: owns the async runtime, configuration and conversations.
//!
//! All network and tool work runs on the engine's own Tokio runtime, so a
//! front end on any executor (GPUI's, or plain blocking code) can call into
//! it. Results come back through `async_channel` receivers and Tokio join
//! handles, both of which can be awaited from any executor.
//!
//! A [`Conversation`] drives one session: it runs the agent loop and wraps
//! it with auto-retry, auto-compaction (threshold and overflow recovery),
//! steering / follow-up queues, deferred runtime changes and goal
//! continuation, following Pi's `AgentSession`.

use std::collections::HashMap;
use std::future::Future;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, RwLock};
use std::time::Instant;

use anyhow::{Context as _, Result, bail};
use async_channel::Receiver;
use serde_json::{Value, json};
use tokio::task::JoinHandle;
use tokio_util::sync::CancellationToken;

use crate::agent::{AgentEvent, AgentParams, Emitter, LoopCallbacks, LoopOutcome, run_loop};
use crate::config::{AgentHome, AuthStore, Settings};
use crate::harness::compaction::{
    self, CompactionResult, Summarizer, SummaryError, calculate_context_tokens,
    estimate_context_tokens, prepare_compaction, should_compact,
};
use crate::harness::goals::{
    self, GOAL_CONTINUATION_TYPE, GOAL_ENTRY_TYPE, Goal, GoalCell, GoalCommandOutput, GoalSlot,
    UpdateGoalTool,
};
use crate::harness::hooks::{
    BeforeAgentStartEvent, BeforeCompactEvent, CompactionReason, Hooks, SessionEvent,
    SessionStartReason,
};
use crate::harness::overflow::{is_context_overflow, is_retryable_error};
use crate::harness::queue::{MessageQueues, QueueSnapshot};
use crate::harness::settings::{self as hsettings, HarnessSettings, QueueMode};
use crate::harness::stats::{self, ContextUsage, SessionStats};
use crate::message::{
    AgentMessage, AssistantMessage, ContentBlock, CustomMessage, Message, StopReason, Usage,
    UserContent, now_millis,
};
use crate::prompt::{build_system_prompt, discover_context_files};
use crate::provider::{self, ModelInfo, ModelRef, ProviderConfig, ThinkingLevel};
use crate::session::{
    self, CompactionEntry, EntryKind, Session, SessionCwdIssue, SessionEntry, SessionSummary,
    SessionTree,
};
use crate::tools::ToolRegistry;

/// `customType` of the session entry recording the active tool set.
pub const TOOLS_CONFIG_ENTRY_TYPE: &str = "tools-config";

const OVERFLOW_RECOVERY_FAILED: &str = "Context overflow recovery failed after one compact-and-retry attempt. Try reducing context or switching to a larger-context model.";

struct EngineInner {
    runtime: tokio::runtime::Runtime,
    http: reqwest::Client,
    home: AgentHome,
    cwd: PathBuf,
    settings: RwLock<Settings>,
    auth: RwLock<AuthStore>,
    tools: ToolRegistry,
    hooks: Hooks,
    session_dir_override: RwLock<Option<PathBuf>>,
    model_info: RwLock<HashMap<ModelRef, ModelInfo>>,
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
                hooks: Hooks::new(),
                session_dir_override: RwLock::new(None),
                model_info: RwLock::new(HashMap::new()),
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
    /// The hook registry shared by all conversations.
    pub fn hooks(&self) -> &Hooks {
        &self.inner.hooks
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

    pub fn harness_settings(&self) -> HarnessSettings {
        HarnessSettings::from_settings(&self.inner.settings.read().expect("settings lock"))
    }

    /// Toggle auto-compaction (`compaction.enabled`).
    pub fn set_auto_compaction(&self, enabled: bool) -> Result<()> {
        self.update_settings(|s| hsettings::set_compaction_enabled(s, enabled))
    }

    /// Toggle auto-retry (`retry.enabled`).
    pub fn set_auto_retry(&self, enabled: bool) -> Result<()> {
        self.update_settings(|s| hsettings::set_retry_enabled(s, enabled))
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

    /// Discover models from every ready provider, concurrently. Results are
    /// also remembered for context-window lookups.
    pub fn list_models(&self) -> JoinHandle<ModelListing> {
        let http = self.inner.http.clone();
        let settings = self.settings();
        let providers: Vec<ProviderConfig> = self
            .providers()
            .into_iter()
            .filter(ProviderConfig::is_ready)
            .collect();
        let engine = self.clone();
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
            let listing = futures::future::join_all(lookups).await;
            for (_, result) in &listing {
                if let Ok(models) = result {
                    engine.remember_models(models);
                }
            }
            listing
        })
    }

    /// Record model metadata (context windows) from a discovery result.
    pub fn remember_models(&self, models: &[ModelInfo]) {
        let mut map = self.inner.model_info.write().expect("model info lock");
        for m in models {
            map.insert(m.model_ref(), m.clone());
        }
    }

    /// Known context window of `model`, if discovered.
    pub fn context_window(&self, model: &ModelRef) -> Option<u64> {
        self.inner
            .model_info
            .read()
            .expect("model info lock")
            .get(model)
            .and_then(|m| m.context_window)
    }

    // ----- session catalog -------------------------------------------------

    /// Override the session directory (`--session-dir`).
    pub fn set_session_dir(&self, dir: Option<PathBuf>) {
        *self.inner.session_dir_override.write().expect("dir lock") = dir;
    }

    /// The session directory: `--session-dir` > `$PI_CODING_AGENT_SESSION_DIR`
    /// > `sessionDir` setting > `<agent home>/sessions`.
    pub fn session_dir(&self) -> PathBuf {
        let cli = self
            .inner
            .session_dir_override
            .read()
            .expect("dir lock")
            .clone();
        session::resolve_session_dir(
            cli.as_deref(),
            &self.inner.settings.read().expect("settings lock"),
            &self.inner.home,
            &self.inner.cwd,
        )
    }

    /// Sessions recorded for the working directory, most recent first.
    pub fn sessions(&self) -> Vec<SessionSummary> {
        session::list_sessions(&self.session_dir(), Some(&self.inner.cwd))
    }

    /// Every session in the session directory, most recent first.
    pub fn all_sessions(&self) -> Vec<SessionSummary> {
        session::list_sessions(&self.session_dir(), None)
    }

    /// Resolve `--session <arg>`: a path, or a session id prefix (sessions
    /// for this working directory win).
    pub fn resolve_session(&self, arg: &str) -> Result<PathBuf> {
        if session::looks_like_path(arg) {
            let path = PathBuf::from(arg);
            return Ok(if path.is_absolute() {
                path
            } else {
                self.inner.cwd.join(path)
            });
        }
        session::resolve_id_prefix(&self.session_dir(), arg, Some(&self.inner.cwd))
    }

    /// Most recently modified session, preferring this working directory.
    pub fn most_recent_session(&self) -> Option<PathBuf> {
        session::most_recent_session(&self.session_dir(), Some(&self.inner.cwd))
    }

    /// Whether `path`'s recorded working directory is gone.
    pub fn session_cwd_issue(&self, path: &Path) -> Option<SessionCwdIssue> {
        session::missing_cwd_issue(path, &self.inner.cwd)
    }

    pub fn new_conversation(&self, model: Option<ModelRef>) -> Conversation {
        let session = Session::create(&self.session_dir(), &self.inner.cwd);
        Conversation::new(self.clone(), session, model, SessionStartReason::New, None)
    }

    /// A conversation that is never written to disk (`--no-session`).
    pub fn new_ephemeral_conversation(&self, model: Option<ModelRef>) -> Conversation {
        let session = Session::in_memory(&self.inner.cwd);
        Conversation::new(self.clone(), session, model, SessionStartReason::New, None)
    }

    /// Resume a session file. The model comes from the session context when
    /// its provider is usable, else the default model.
    pub fn open_conversation(&self, path: &Path) -> Result<Conversation> {
        self.open_conversation_with(path, None, SessionStartReason::Resume, None)
    }

    /// Resume with the stored working directory replaced (missing-cwd prompt).
    pub fn open_conversation_in(&self, path: &Path, cwd: &Path) -> Result<Conversation> {
        self.open_conversation_with(path, Some(cwd), SessionStartReason::Resume, None)
    }

    fn open_conversation_with(
        &self,
        path: &Path,
        cwd: Option<&Path>,
        reason: SessionStartReason,
        previous: Option<PathBuf>,
    ) -> Result<Conversation> {
        let session = Session::open_with(path, cwd)?;
        let model = session
            .build_context()
            .model
            .filter(|m| !m.provider.is_empty() && !m.id.is_empty())
            .filter(|m| self.provider(&m.provider).is_some_and(|p| p.is_ready()))
            .or_else(|| self.default_model());
        Ok(Conversation::new(
            self.clone(),
            session,
            Some(model).flatten(),
            reason,
            previous,
        ))
    }

    /// `--continue`: the most recent session, or a new one.
    pub fn continue_recent(&self, model: Option<ModelRef>) -> Result<Conversation> {
        match self.most_recent_session() {
            Some(path) => self.open_conversation(&path),
            None => Ok(self.new_conversation(model)),
        }
    }

    /// `--fork <session>`: copy a whole session into a new file
    /// (`fork-<id>`) for this working directory (Pi `forkFrom`).
    pub fn fork_session(&self, source: &Path) -> Result<Conversation> {
        let source_session = Session::open(source).map_err(|e| {
            anyhow::anyhow!(
                "Cannot fork: source session file is empty or invalid: {}: {e:#}",
                source.display()
            )
        })?;
        let id = format!("fork-{}", source_session.id());
        let mut copy = Session::create_with(
            &self.session_dir(),
            &self.inner.cwd,
            id,
            Some(source.to_string_lossy().into_owned()),
        );
        for entry in source_session.entries() {
            copy.append_entry(entry.clone())?;
        }
        if let Some(leaf) = source_session.leaf_id() {
            copy.branch(leaf)?;
        }
        copy.rewrite()?;
        let model = copy.build_context().model.or_else(|| self.default_model());
        Ok(Conversation::new(
            self.clone(),
            copy,
            model,
            SessionStartReason::Fork,
            Some(source.to_path_buf()),
        ))
    }

    /// Import a session JSONL file into the session directory and open it
    /// (Pi `importFromJsonl`). Without `cwd_override`, a missing stored
    /// working directory is an error carrying Pi's message.
    pub fn import_session(&self, path: &Path, cwd_override: Option<&Path>) -> Result<Conversation> {
        let resolved = if path.is_absolute() {
            path.to_path_buf()
        } else {
            self.inner.cwd.join(path)
        };
        if !resolved.exists() {
            bail!("Session file not found: {}", resolved.display());
        }
        let dir = self.session_dir();
        std::fs::create_dir_all(&dir).with_context(|| format!("creating {}", dir.display()))?;
        let dest = dir.join(
            resolved
                .file_name()
                .context("session path has no file name")?,
        );
        let same = dest.canonicalize().ok() == resolved.canonicalize().ok();
        if !same {
            std::fs::copy(&resolved, &dest)
                .with_context(|| format!("copying {} to {}", resolved.display(), dest.display()))?;
        }
        if cwd_override.is_none()
            && let Some(issue) = self.session_cwd_issue(&dest)
        {
            bail!("{}", issue.error_text());
        }
        self.open_conversation_with(&dest, cwd_override, SessionStartReason::Import, None)
    }

    fn system_prompt(&self, tools: &ToolRegistry) -> String {
        let files = discover_context_files(&self.inner.home, &self.inner.cwd);
        build_system_prompt(tools, &files, &self.inner.cwd)
    }
}

/// How a custom message is delivered (Pi `deliverAs`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Delivery {
    /// After the current turn's tool calls.
    Steer,
    /// When the agent would otherwise stop.
    FollowUp,
    /// Together with the next user prompt.
    NextTurn,
}

/// Options for [`Conversation::navigate_tree`].
#[derive(Debug, Clone, Default)]
pub struct NavigateOptions {
    /// Summarize the branch being left (model-backed).
    pub summarize: bool,
    pub custom_instructions: Option<String>,
    /// Use `custom_instructions` instead of the default summary prompt.
    pub replace_instructions: bool,
    /// Label for the summary entry (or the target when not summarizing).
    pub label: Option<String>,
}

/// Result of a tree navigation.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct TreeNavigation {
    /// Text of the selected user message, for re-editing.
    pub editor_text: Option<String>,
    pub cancelled: bool,
    pub summary_entry_id: Option<String>,
}

/// A forked conversation and the selected user message text.
pub struct Fork {
    pub conversation: Conversation,
    pub selected_text: Option<String>,
}

#[derive(Default)]
struct Pending {
    entries: Vec<EntryKind>,
    params_dirty: bool,
}

struct ConversationState {
    session: Session,
    messages: Vec<AgentMessage>,
    model: Option<ModelRef>,
    thinking: ThinkingLevel,
    active_tools: Option<Vec<String>>,
    context_window: Option<u64>,
    running: Option<CancellationToken>,
    events: Option<Emitter>,
    usage: Usage,
    queues: MessageQueues,
    pending: Pending,
    retry_attempt: u32,
    overflow_recovery_attempted: bool,
}

/// One conversation (session) driven by a front end.
#[derive(Clone)]
pub struct Conversation {
    engine: Engine,
    state: Arc<Mutex<ConversationState>>,
    goal: GoalSlot,
}

impl std::fmt::Debug for Conversation {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Conversation")
            .field("session_id", &self.session_id())
            .finish_non_exhaustive()
    }
}

fn thinking_from_str(s: &str) -> ThinkingLevel {
    match s {
        "minimal" => ThinkingLevel::Low,
        "xhigh" => ThinkingLevel::High,
        other => ThinkingLevel::parse(other).unwrap_or_default(),
    }
}

fn user_message(text: &str, images: Vec<ContentBlock>) -> AgentMessage {
    let content = if images.is_empty() {
        UserContent::Text(text.to_string())
    } else {
        let mut blocks = vec![ContentBlock::text(text)];
        blocks.extend(images);
        UserContent::Blocks(blocks)
    };
    AgentMessage::Llm(Message::User {
        content,
        timestamp: Some(now_millis()),
    })
}

fn last_assistant(messages: &[AgentMessage]) -> Option<AssistantMessage> {
    messages
        .iter()
        .rev()
        .find_map(|m| m.as_assistant().cloned())
}

fn pop_trailing_assistant(messages: &mut Vec<AgentMessage>, only_errors: bool) {
    if let Some(a) = messages.last().and_then(AgentMessage::as_assistant)
        && (!only_errors || a.stop_reason == StopReason::Error)
    {
        messages.pop();
    }
}

enum CompactionCheck {
    Nothing,
    /// Overflow recovered; continue the run without a new prompt.
    Retry,
    /// Threshold compaction finished while messages are queued.
    Resume,
}

impl Conversation {
    fn new(
        engine: Engine,
        session: Session,
        model: Option<ModelRef>,
        reason: SessionStartReason,
        previous: Option<PathBuf>,
    ) -> Self {
        let ctx = session.build_context();
        let mut usage = Usage::default();
        for m in &ctx.messages {
            if let Some(a) = m.as_assistant() {
                usage.add(&a.usage);
            }
        }
        let names: Vec<&str> = engine.tools().iter().map(|t| t.name()).collect();
        let active_tools = session
            .latest_custom_on_branch(TOOLS_CONFIG_ENTRY_TYPE)
            .flatten()
            .and_then(|d| d.get("enabledTools").cloned())
            .and_then(|v| serde_json::from_value::<Vec<String>>(v).ok())
            .map(|list| {
                let mut seen = Vec::new();
                for name in list {
                    if names.contains(&name.as_str()) && !seen.contains(&name) {
                        seen.push(name);
                    }
                }
                seen
            });
        let goal = session
            .latest_custom_on_branch(GOAL_ENTRY_TYPE)
            .flatten()
            .and_then(|d| serde_json::from_value::<Goal>(d).ok());
        let hs = engine.harness_settings();
        let context_window = model.as_ref().and_then(|m| engine.context_window(m));
        let start = SessionEvent::Start {
            reason,
            session_id: session.id().to_string(),
            session_file: session.is_persisted().then(|| session.path().to_path_buf()),
            previous_session_file: previous,
        };
        let conversation = Self {
            state: Arc::new(Mutex::new(ConversationState {
                messages: ctx.messages,
                thinking: thinking_from_str(&ctx.thinking_level),
                session,
                model,
                active_tools,
                context_window,
                running: None,
                events: None,
                usage,
                queues: MessageQueues::new(hs.steering_mode, hs.follow_up_mode),
                pending: Pending::default(),
                retry_attempt: 0,
                overflow_recovery_attempted: false,
            })),
            goal: Arc::new(Mutex::new(GoalCell { goal, dirty: false })),
            engine,
        };
        conversation.engine.hooks().emit_session(&start);
        conversation
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, ConversationState> {
        self.state.lock().expect("conversation lock")
    }

    pub fn engine(&self) -> &Engine {
        &self.engine
    }

    /// User, assistant and tool-result messages of the current context.
    pub fn messages(&self) -> Vec<Message> {
        self.lock()
            .messages
            .iter()
            .filter_map(|m| m.as_llm().cloned())
            .collect()
    }
    /// The full agent context including harness messages and summaries.
    pub fn agent_messages(&self) -> Vec<AgentMessage> {
        self.lock().messages.clone()
    }
    pub fn session_path(&self) -> PathBuf {
        self.lock().session.path().to_path_buf()
    }
    pub fn session_id(&self) -> String {
        self.lock().session.id().to_string()
    }
    pub fn parent_session(&self) -> Option<String> {
        self.lock().session.parent_session().map(str::to_string)
    }
    pub fn is_persisted(&self) -> bool {
        self.lock().session.is_persisted()
    }
    pub fn model(&self) -> Option<ModelRef> {
        self.lock().model.clone()
    }
    pub fn thinking(&self) -> ThinkingLevel {
        self.lock().thinking
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
            .find_map(|m| match m.as_assistant() {
                Some(a) if a.usage.context_tokens() > 0 => Some(a.usage.context_tokens()),
                _ => None,
            })
            .unwrap_or(0)
    }
    /// The current model's context window: set explicitly, else discovered.
    pub fn context_window(&self) -> Option<u64> {
        self.window_of(&self.lock())
    }

    fn window_of(&self, state: &ConversationState) -> Option<u64> {
        state.context_window.or_else(|| {
            state
                .model
                .as_ref()
                .and_then(|m| self.engine.context_window(m))
        })
    }
    /// Set the model's context window (enables threshold auto-compaction).
    pub fn set_context_window(&self, window: Option<u64>) {
        self.lock().context_window = window.filter(|w| *w > 0);
    }

    /// Record an entry now, or at the next save point while running.
    fn record_entry(state: &mut ConversationState, kind: EntryKind) -> Result<()> {
        if state.running.is_some() {
            state.pending.entries.push(kind);
            return Ok(());
        }
        Self::append_kind(&mut state.session, kind)
    }

    fn append_kind(session: &mut Session, kind: EntryKind) -> Result<()> {
        match kind {
            EntryKind::ModelChange { provider, model_id } => {
                session.append_model_change(&provider, &model_id)?;
            }
            EntryKind::ThinkingLevelChange { thinking_level } => {
                session.append_thinking_level_change(&thinking_level)?;
            }
            EntryKind::Custom { custom_type, data } => {
                session.append_custom_entry(&custom_type, data)?;
            }
            EntryKind::SessionInfo { name } => {
                session.append_session_info(name.as_deref().unwrap_or(""))?;
            }
            EntryKind::Label { target_id, label } => {
                session.append_label_change(&target_id, label.as_deref())?;
            }
            other => bail!("cannot record a {other:?} entry directly"),
        }
        Ok(())
    }

    fn flush_pending(state: &mut ConversationState) -> Vec<String> {
        let mut warnings = Vec::new();
        for kind in std::mem::take(&mut state.pending.entries) {
            if let Err(e) = Self::append_kind(&mut state.session, kind) {
                warnings.push(format!("Could not save session: {e:#}"));
            }
        }
        warnings
    }

    /// Switch models. While running, the change applies at the next turn.
    pub fn set_model(&self, model: ModelRef) {
        let window = self.engine.context_window(&model);
        let mut state = self.lock();
        if state.model.as_ref() == Some(&model) {
            if window.is_some() {
                state.context_window = window;
            }
            return;
        }
        state.context_window = window;
        state.model = Some(model.clone());
        state.pending.params_dirty = true;
        let _ = Self::record_entry(
            &mut state,
            EntryKind::ModelChange {
                provider: model.provider,
                model_id: model.id,
            },
        );
    }

    /// Change the thinking level. While running, it applies at the next turn.
    pub fn set_thinking(&self, level: ThinkingLevel) {
        let mut state = self.lock();
        if state.thinking == level {
            return;
        }
        state.thinking = level;
        state.pending.params_dirty = true;
        let _ = Self::record_entry(
            &mut state,
            EntryKind::ThinkingLevelChange {
                thinking_level: level.as_str().into(),
            },
        );
    }

    /// Names of the tools offered to the model.
    pub fn active_tools(&self) -> Vec<String> {
        let state = self.lock();
        match &state.active_tools {
            Some(list) => list.clone(),
            None => self
                .engine
                .tools()
                .iter()
                .map(|t| t.name().to_string())
                .collect(),
        }
    }

    /// Restrict the tool set (persisted as a `tools-config` entry).
    pub fn set_active_tools(&self, names: &[String]) -> Result<()> {
        for name in names {
            if self.engine.tools().get(name).is_none() {
                bail!("Unknown tool: {name}");
            }
        }
        let mut list: Vec<String> = Vec::new();
        for n in names {
            if !list.contains(n) {
                list.push(n.clone());
            }
        }
        let mut state = self.lock();
        state.active_tools = Some(list.clone());
        state.pending.params_dirty = true;
        Self::record_entry(
            &mut state,
            EntryKind::Custom {
                custom_type: TOOLS_CONFIG_ENTRY_TYPE.into(),
                data: Some(json!({ "enabledTools": list })),
            },
        )
    }

    // ----- session metadata ------------------------------------------------

    pub fn name(&self) -> Option<String> {
        let state = self.lock();
        let pending = state.pending.entries.iter().rev().find_map(|k| match k {
            EntryKind::SessionInfo { name } => Some(name.clone()),
            _ => None,
        });
        match pending {
            Some(name) => name.filter(|n| !n.trim().is_empty()),
            None => state.session.name(),
        }
    }

    /// Set the display name (`/name`); empty clears it.
    pub fn set_name(&self, name: &str) -> Result<()> {
        let session_id = {
            let mut state = self.lock();
            Self::record_entry(
                &mut state,
                EntryKind::SessionInfo {
                    name: Some(name.trim().to_string()),
                },
            )?;
            state.session.id().to_string()
        };
        let name = Some(name.trim().to_string()).filter(|n| !n.is_empty());
        self.engine
            .hooks()
            .emit_session(&SessionEvent::NameChanged { session_id, name });
        Ok(())
    }

    /// Set or clear (`None`/empty) a bookmark label on an entry.
    pub fn set_label(&self, entry_id: &str, label: Option<&str>) -> Result<()> {
        let mut state = self.lock();
        if state.session.entry(entry_id).is_none() {
            bail!("Entry {entry_id} not found");
        }
        Self::record_entry(
            &mut state,
            EntryKind::Label {
                target_id: entry_id.into(),
                label: label
                    .map(str::trim)
                    .filter(|l| !l.is_empty())
                    .map(str::to_string),
            },
        )
    }

    pub fn entries(&self) -> Vec<SessionEntry> {
        self.lock().session.entries().to_vec()
    }
    pub fn entry(&self, id: &str) -> Option<SessionEntry> {
        self.lock().session.entry(id).cloned()
    }
    pub fn leaf_id(&self) -> Option<String> {
        self.lock().session.leaf_id().map(str::to_string)
    }
    /// Entries on the path root → leaf.
    pub fn branch(&self) -> Vec<SessionEntry> {
        self.lock().session.branch_path(None)
    }
    pub fn tree(&self) -> SessionTree {
        self.lock().session.tree()
    }
    pub fn label(&self, id: &str) -> Option<String> {
        self.lock().session.label(id).map(str::to_string)
    }

    /// User messages that can start a fork (Pi `getUserMessagesForForking`).
    pub fn user_messages_for_forking(&self) -> Vec<(String, String)> {
        self.lock()
            .session
            .entries()
            .iter()
            .filter_map(|e| match e.message() {
                Some(m) if m.is_user() => m
                    .user_text()
                    .filter(|t| !t.is_empty())
                    .map(|t| (e.id.clone(), t)),
                _ => None,
            })
            .collect()
    }

    pub fn stats(&self) -> SessionStats {
        let state = self.lock();
        let usage = stats::context_usage(
            &state.session.branch_path(None),
            &state.messages,
            self.window_of(&state),
        );
        stats::compute_stats(
            &state.messages,
            state
                .session
                .is_persisted()
                .then(|| state.session.path().to_path_buf()),
            state.session.id().to_string(),
            usage,
        )
    }

    pub fn context_usage(&self) -> Option<ContextUsage> {
        let state = self.lock();
        stats::context_usage(
            &state.session.branch_path(None),
            &state.messages,
            self.window_of(&state),
        )
    }

    // ----- queues ----------------------------------------------------------

    /// Emit a queue update after releasing the lock, so event listeners may
    /// call back into the conversation.
    fn emit_queue(state: std::sync::MutexGuard<'_, ConversationState>) {
        let Some(emit) = state.events.clone() else {
            return;
        };
        let snap = state.queues.snapshot();
        drop(state);
        emit.emit(AgentEvent::QueueUpdate {
            steering: snap.steering,
            follow_up: snap.follow_up,
        });
    }

    /// Queue a steering message, delivered after the current turn's tools
    /// (or at the start of the next run when idle).
    pub fn steer(&self, text: &str, images: Vec<ContentBlock>) -> Result<()> {
        let text = text.trim();
        if text.is_empty() && images.is_empty() {
            bail!("Nothing to send");
        }
        let mut state = self.lock();
        state
            .queues
            .push_steering(text.to_string(), user_message(text, images));
        Self::emit_queue(state);
        Ok(())
    }

    /// Queue a follow-up, delivered when the agent would otherwise stop.
    pub fn follow_up(&self, text: &str, images: Vec<ContentBlock>) -> Result<()> {
        let text = text.trim();
        if text.is_empty() && images.is_empty() {
            bail!("Nothing to send");
        }
        let mut state = self.lock();
        state
            .queues
            .push_follow_up(text.to_string(), user_message(text, images));
        Self::emit_queue(state);
        Ok(())
    }

    pub fn queue(&self) -> QueueSnapshot {
        self.lock().queues.snapshot()
    }

    /// Drop queued steering / follow-up messages, returning their texts.
    pub fn clear_queue(&self) -> QueueSnapshot {
        let mut state = self.lock();
        let cleared = state.queues.clear();
        Self::emit_queue(state);
        cleared
    }

    pub fn set_steering_mode(&self, mode: QueueMode) -> Result<()> {
        self.lock().queues.steering_mode = mode;
        self.engine
            .update_settings(|s| hsettings::set_steering_mode(s, mode))
    }

    pub fn set_follow_up_mode(&self, mode: QueueMode) -> Result<()> {
        self.lock().queues.follow_up_mode = mode;
        self.engine
            .update_settings(|s| hsettings::set_follow_up_mode(s, mode))
    }

    /// Send an extension message (Pi `sendCustomMessage`). While running it
    /// is queued per `delivery`; when idle it is appended to the context,
    /// and with `trigger_turn` a run starts with it.
    pub fn send_custom_message(
        &self,
        message: CustomMessage,
        delivery: Delivery,
        trigger_turn: bool,
    ) -> Result<Option<Receiver<AgentEvent>>> {
        let text = message.content.plain_text();
        {
            let mut state = self.lock();
            if delivery == Delivery::NextTurn {
                state.queues.push_next_turn(message);
                return Ok(None);
            }
            if state.running.is_some() {
                let m = AgentMessage::Custom(message);
                match delivery {
                    Delivery::Steer => state.queues.push_steering(text, m),
                    _ => state.queues.push_follow_up(text, m),
                }
                Self::emit_queue(state);
                return Ok(None);
            }
            if !trigger_turn {
                state.session.append_custom_message_entry(&message)?;
                state.messages.push(AgentMessage::Custom(message));
                return Ok(None);
            }
        }
        self.start_run(vec![AgentMessage::Custom(message)], None)
            .map(Some)
    }

    // ----- running ---------------------------------------------------------

    /// Cancel the running agent, retry wait, compaction or summary.
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
        let user = user_message(text, images);
        let Some(AgentMessage::Llm(prompt)) = Some(user.clone()) else {
            unreachable!()
        };
        let mut messages = vec![user];
        {
            let mut state = self.lock();
            if state.running.is_some() {
                bail!("The agent is already running");
            }
            self.check_ready(&state)?;
            messages.extend(
                state
                    .queues
                    .take_next_turn()
                    .into_iter()
                    .map(AgentMessage::Custom),
            );
        }
        let system = self.base_system_prompt();
        let hook_result = self
            .engine
            .hooks()
            .run_before_agent_start(&BeforeAgentStartEvent {
                prompt,
                system_prompt: system,
                cwd: self.engine.cwd().to_path_buf(),
            });
        for mut m in hook_result.messages {
            if m.timestamp == 0 {
                m.timestamp = now_millis();
            }
            messages.push(AgentMessage::Custom(m));
        }
        self.start_run(messages, hook_result.system_prompt)
    }

    fn check_ready(&self, state: &ConversationState) -> Result<ProviderConfig> {
        let model = state.model.clone().context("No model selected")?;
        let provider = self
            .engine
            .provider(&model.provider)
            .with_context(|| format!("Unknown provider {}", model.provider))?;
        if !provider.is_ready() {
            bail!("No API key configured for {}", provider.label);
        }
        Ok(provider)
    }

    fn tool_registry(&self, state: &ConversationState) -> ToolRegistry {
        let mut registry = match &state.active_tools {
            None => self.engine.tools().clone(),
            Some(names) => {
                let mut r = ToolRegistry::empty();
                for name in names {
                    if let Some(t) = self.engine.tools().get(name) {
                        r = r.with(t);
                    }
                }
                r
            }
        };
        let has_goal = self
            .goal
            .lock()
            .expect("goal lock")
            .goal
            .as_ref()
            .is_some_and(|g| g.is_active() || g.status == goals::GoalStatus::BudgetLimited);
        if has_goal {
            registry = registry.with(Arc::new(UpdateGoalTool::new(self.goal.clone())));
        }
        registry
    }

    fn base_system_prompt(&self) -> String {
        let registry = self.tool_registry(&self.lock());
        self.engine.system_prompt(&registry)
    }

    fn build_params(&self, system_override: Option<&str>) -> Result<AgentParams> {
        let state = self.lock();
        let provider = self.check_ready(&state)?;
        let model = state.model.clone().context("No model selected")?;
        let tools = self.tool_registry(&state);
        let system = match system_override {
            Some(s) => s.to_string(),
            None => self.engine.system_prompt(&tools),
        };
        Ok(AgentParams {
            http: self.engine.inner.http.clone(),
            provider,
            model: model.id,
            system,
            tools,
            max_tokens: self.engine.settings().max_output_tokens(),
            thinking: state.thinking,
            cwd: self.engine.cwd().to_path_buf(),
            hooks: self.engine.hooks().clone(),
        })
    }

    /// Record model / thinking entries when the context disagrees with the
    /// conversation (first prompt of a new session, resumed sessions).
    fn sync_runtime_entries(state: &mut ConversationState) {
        let ctx_model = state.session.build_context();
        if let Some(model) = state.model.clone()
            && ctx_model.model.as_ref() != Some(&model)
        {
            let _ = state
                .session
                .append_model_change(&model.provider, &model.id);
        }
        if thinking_from_str(&ctx_model.thinking_level) != state.thinking {
            let level = state.thinking.as_str().to_string();
            let _ = state.session.append_thinking_level_change(&level);
        }
    }

    fn start_run(
        &self,
        messages: Vec<AgentMessage>,
        system_override: Option<String>,
    ) -> Result<Receiver<AgentEvent>> {
        let cancel = CancellationToken::new();
        let (tx, rx) = async_channel::unbounded();
        let emitter = Emitter::new(tx, self.engine.hooks().clone());
        {
            let mut state = self.lock();
            if state.running.is_some() {
                bail!("The agent is already running");
            }
            self.check_ready(&state)?;
            Self::sync_runtime_entries(&mut state);
            state.running = Some(cancel.clone());
            state.events = Some(emitter.clone());
        }
        let conv = self.clone();
        self.engine.spawn(async move {
            conv.drive(messages, system_override, cancel, emitter).await;
        });
        Ok(rx)
    }

    fn finish_run(&self, emitter: &Emitter) {
        let warnings = {
            let mut state = self.lock();
            state.running = None;
            state.events = None;
            state.pending.params_dirty = false;
            Self::flush_pending(&mut state)
        };
        self.persist_goal_if_dirty();
        for w in warnings {
            emitter.emit(AgentEvent::Warning(w));
        }
    }

    async fn drive(
        self,
        prompts: Vec<AgentMessage>,
        system_override: Option<String>,
        cancel: CancellationToken,
        emitter: Emitter,
    ) {
        emitter.emit(AgentEvent::AgentStart);
        let mut total = Usage::default();
        // Pi checks compaction before sending a prompt (catches aborted responses).
        let previous = last_assistant(&self.lock().messages);
        if let Some(prev) = previous {
            self.check_compaction(&prev, false, true, &cancel, &emitter)
                .await;
        }
        let mut pending = prompts;
        let mut continuation_run = pending.iter().any(
            |m| matches!(m, AgentMessage::Custom(c) if c.custom_type == GOAL_CONTINUATION_TYPE),
        );
        loop {
            if cancel.is_cancelled() {
                break;
            }
            let params = match self.build_params(system_override.as_deref()) {
                Ok(p) => p,
                Err(e) => {
                    emitter.emit(AgentEvent::Warning(format!("{e:#}")));
                    break;
                }
            };
            let started = Instant::now();
            let outcome = self
                .run_once(
                    params,
                    std::mem::take(&mut pending),
                    system_override.as_deref(),
                    &cancel,
                    &emitter,
                )
                .await;
            self.account_goal_time(started.elapsed().as_secs() as i64);
            total.add(&outcome.usage);
            if cancel.is_cancelled() {
                break;
            }
            let Some(last) = outcome.last_assistant.clone() else {
                break;
            };
            if self.handle_retry(&last, &cancel, &emitter).await {
                continue;
            }
            if cancel.is_cancelled() {
                break;
            }
            match self
                .check_compaction(&last, true, false, &cancel, &emitter)
                .await
            {
                CompactionCheck::Retry | CompactionCheck::Resume => continue,
                CompactionCheck::Nothing => {}
            }
            if let Some(next) = self.goal_continuation(&last, &outcome, continuation_run) {
                pending = vec![next];
                continuation_run = true;
                continue;
            }
            break;
        }
        self.finish_run(&emitter);
        emitter.emit(AgentEvent::AgentEnd {
            usage: total,
            aborted: cancel.is_cancelled(),
        });
    }

    async fn run_once(
        &self,
        params: AgentParams,
        prompts: Vec<AgentMessage>,
        system_override: Option<&str>,
        cancel: &CancellationToken,
        emitter: &Emitter,
    ) -> LoopOutcome {
        let context = self.lock().messages.clone();
        let state = self.state.clone();
        let goal = self.goal.clone();
        let record_emitter = emitter.clone();
        let mut record = move |message: &AgentMessage| -> Result<(), String> {
            let mut retry_success = None;
            let mut budget_crossed = None;
            let result = {
                let mut s = state.lock().expect("conversation lock");
                s.messages.push(message.clone());
                match message {
                    AgentMessage::Llm(Message::User { .. }) => {
                        s.overflow_recovery_attempted = false;
                    }
                    AgentMessage::Llm(Message::Assistant(a)) => {
                        s.usage.add(&a.usage);
                        if a.stop_reason != StopReason::Error {
                            s.overflow_recovery_attempted = false;
                            if s.retry_attempt > 0 {
                                retry_success = Some(s.retry_attempt);
                                s.retry_attempt = 0;
                            }
                        }
                        let u = &a.usage;
                        let tokens = u.input_tokens
                            + u.output_tokens
                            + u.cache_read_tokens
                            + u.cache_write_tokens;
                        let mut cell = goal.lock().expect("goal lock");
                        if let Some(g) = cell.goal.as_mut()
                            && g.account(0, tokens as i64)
                        {
                            budget_crossed = Some(goals::budget_limit_prompt(g));
                            cell.dirty = true;
                        }
                    }
                    _ => {}
                }
                if let Some(prompt) = &budget_crossed {
                    let m = CustomMessage::new(
                        GOAL_CONTINUATION_TYPE,
                        UserContent::Text(prompt.clone()),
                        false,
                    );
                    s.queues
                        .push_steering("Goal budget reached".into(), AgentMessage::Custom(m));
                }
                let r = match message {
                    AgentMessage::Custom(c) => s.session.append_custom_message_entry(c),
                    other => s.session.append_message(other),
                };
                r.map(|_| ()).map_err(|e| format!("{e:#}"))
            };
            if let Some(attempt) = retry_success {
                record_emitter.emit(AgentEvent::RetryEnd {
                    success: true,
                    attempt,
                    final_error: None,
                });
            }
            result
        };
        let steer_state = self.state.clone();
        let mut steering = move || -> Vec<AgentMessage> {
            let mut s = steer_state.lock().expect("conversation lock");
            let taken = s.queues.take_steering();
            if !taken.is_empty() {
                Self::emit_queue(s);
            }
            taken
        };
        let follow_state = self.state.clone();
        let mut follow_up = move || -> Vec<AgentMessage> {
            let mut s = follow_state.lock().expect("conversation lock");
            let taken = s.queues.take_follow_up();
            if !taken.is_empty() {
                Self::emit_queue(s);
            }
            taken
        };
        let me = self.clone();
        let boundary_emitter = emitter.clone();
        let system_override = system_override.map(str::to_string);
        let mut turn_end = move || -> Option<AgentParams> {
            let (warnings, dirty) = {
                let mut s = me.state.lock().expect("conversation lock");
                let w = Self::flush_pending(&mut s);
                let dirty = std::mem::take(&mut s.pending.params_dirty);
                (w, dirty)
            };
            me.persist_goal_if_dirty();
            for w in warnings {
                boundary_emitter.emit(AgentEvent::Warning(w));
            }
            if !dirty {
                return None;
            }
            match me.build_params(system_override.as_deref()) {
                Ok(p) => Some(p),
                Err(e) => {
                    boundary_emitter.emit(AgentEvent::Warning(format!("{e:#}")));
                    None
                }
            }
        };
        let mut callbacks = LoopCallbacks {
            record: &mut record,
            steering: &mut steering,
            follow_up: &mut follow_up,
            turn_end: &mut turn_end,
        };
        run_loop(params, context, prompts, cancel, emitter, &mut callbacks).await
    }

    /// Pi `_handleRetryableError`. Returns true when the run should continue.
    async fn handle_retry(
        &self,
        last: &AssistantMessage,
        cancel: &CancellationToken,
        emitter: &Emitter,
    ) -> bool {
        let settings = self.engine.harness_settings().retry;
        let window = self.window_of(&self.lock());
        if !settings.enabled || !is_retryable_error(last, window) {
            return false;
        }
        let attempt = {
            let mut s = self.lock();
            s.retry_attempt += 1;
            s.retry_attempt
        };
        if attempt > settings.max_retries {
            self.lock().retry_attempt = 0;
            emitter.emit(AgentEvent::RetryEnd {
                success: false,
                attempt: attempt - 1,
                final_error: last.error_message.clone(),
            });
            return false;
        }
        let delay_ms = settings.delay_ms(attempt);
        emitter.emit(AgentEvent::RetryStart {
            attempt,
            max_attempts: settings.max_retries,
            delay_ms,
            error: last
                .error_message
                .clone()
                .unwrap_or_else(|| "Unknown error".into()),
        });
        pop_trailing_assistant(&mut self.lock().messages, false);
        let slept = tokio::select! {
            _ = tokio::time::sleep(std::time::Duration::from_millis(delay_ms)) => true,
            _ = cancel.cancelled() => false,
        };
        if !slept {
            self.lock().retry_attempt = 0;
            emitter.emit(AgentEvent::RetryEnd {
                success: false,
                attempt,
                final_error: Some("Retry cancelled".into()),
            });
            return false;
        }
        true
    }

    /// Pi `_checkCompaction`: overflow recovery and threshold compaction.
    async fn check_compaction(
        &self,
        last: &AssistantMessage,
        skip_aborted: bool,
        before_prompt: bool,
        cancel: &CancellationToken,
        emitter: &Emitter,
    ) -> CompactionCheck {
        let settings = self.engine.harness_settings().compaction;
        if !settings.enabled || (skip_aborted && last.stop_reason == StopReason::Aborted) {
            return CompactionCheck::Nothing;
        }
        let (window, same_model, compaction_ts, messages) = {
            let s = self.lock();
            let same = s
                .model
                .as_ref()
                .is_some_and(|m| m.provider == last.provider && m.id == last.model);
            let ts = s
                .session
                .branch_path(None)
                .iter()
                .rev()
                .find(|e| matches!(e.kind, EntryKind::Compaction(_)))
                .map(SessionEntry::timestamp_millis);
            (self.window_of(&s), same, ts, s.messages.clone())
        };
        if let Some(ts) = compaction_ts
            && last.timestamp.unwrap_or(0) <= ts
        {
            return CompactionCheck::Nothing;
        }
        if same_model && is_context_overflow(last, window) {
            let attempted = std::mem::replace(&mut self.lock().overflow_recovery_attempted, true);
            if attempted {
                emitter.emit(AgentEvent::CompactionEnd {
                    reason: CompactionReason::Overflow,
                    result: None,
                    aborted: false,
                    will_retry: false,
                    error: Some(OVERFLOW_RECOVERY_FAILED.into()),
                });
                return CompactionCheck::Nothing;
            }
            pop_trailing_assistant(&mut self.lock().messages, false);
            let will_retry = !before_prompt;
            let result = self
                .run_compaction(
                    CompactionReason::Overflow,
                    None,
                    will_retry,
                    cancel,
                    emitter,
                )
                .await;
            if result.is_ok() {
                pop_trailing_assistant(&mut self.lock().messages, true);
                if will_retry {
                    return CompactionCheck::Retry;
                }
            }
            return CompactionCheck::Nothing;
        }
        let Some(window) = window else {
            return CompactionCheck::Nothing;
        };
        let tokens = if last.stop_reason == StopReason::Error {
            let estimate = estimate_context_tokens(&messages);
            let Some(index) = estimate.last_usage_index else {
                return CompactionCheck::Nothing;
            };
            if let (Some(ts), Some(a)) = (compaction_ts, messages[index].as_assistant())
                && a.timestamp.unwrap_or(0) <= ts
            {
                return CompactionCheck::Nothing;
            }
            estimate.tokens
        } else {
            calculate_context_tokens(&last.usage)
        };
        if should_compact(tokens, window, &settings) {
            let result = self
                .run_compaction(CompactionReason::Threshold, None, false, cancel, emitter)
                .await;
            if result.is_ok() && !before_prompt && self.lock().queues.has_queued() {
                return CompactionCheck::Resume;
            }
        }
        CompactionCheck::Nothing
    }

    fn summarizer(&self) -> Result<Summarizer> {
        let state = self.lock();
        let provider = self.check_ready(&state)?;
        let model = state.model.clone().context("No model selected")?;
        Ok(Summarizer {
            http: self.engine.inner.http.clone(),
            provider,
            model: model.id,
            max_output_tokens: self.engine.settings().max_output_tokens(),
            context_window: self.window_of(&state),
        })
    }

    /// Run one compaction with start/end events. Pi `compact` /
    /// `_runAutoCompaction`.
    async fn run_compaction(
        &self,
        reason: CompactionReason,
        custom_instructions: Option<String>,
        will_retry: bool,
        cancel: &CancellationToken,
        emitter: &Emitter,
    ) -> Result<CompactionResult, String> {
        emitter.emit(AgentEvent::CompactionStart { reason });
        let end = |result: Option<CompactionResult>,
                   aborted: bool,
                   error: Option<String>,
                   retry: bool| {
            emitter.emit(AgentEvent::CompactionEnd {
                reason,
                result,
                aborted,
                will_retry: retry,
                error,
            });
        };
        let fail_text = |m: &str| match reason {
            CompactionReason::Manual => format!("Compaction failed: {m}"),
            CompactionReason::Overflow => format!("Context overflow recovery failed: {m}"),
            CompactionReason::Threshold => format!("Auto-compaction failed: {m}"),
        };
        let summarizer = match self.summarizer() {
            Ok(s) => s,
            Err(e) => {
                let msg = format!("{e:#}");
                let error = (reason == CompactionReason::Manual).then(|| fail_text(&msg));
                end(None, false, error, false);
                return Err(msg);
            }
        };
        let settings = self.engine.harness_settings().compaction;
        let path = self.lock().session.branch_path(None);
        let Some(prep) = prepare_compaction(&path, &settings) else {
            let msg = if path
                .last()
                .is_some_and(|e| matches!(e.kind, EntryKind::Compaction(_)))
            {
                "Already compacted"
            } else {
                "Nothing to compact (session too small)"
            };
            let error = (reason == CompactionReason::Manual).then(|| fail_text(msg));
            end(None, false, error, false);
            return Err(msg.into());
        };
        let hooks = self.engine.hooks();
        let mut from_hook = false;
        let mut provided = None;
        if hooks.has_before_compact() {
            let decision = hooks.run_before_compact(&BeforeCompactEvent {
                reason,
                preparation: prep.clone(),
                branch_entries: path.clone(),
                custom_instructions: custom_instructions.clone(),
            });
            if decision.cancel {
                let error =
                    (reason == CompactionReason::Manual).then(|| fail_text("Compaction cancelled"));
                end(None, true, error, false);
                return Err("Compaction cancelled".into());
            }
            if let Some(c) = decision.compaction {
                provided = Some(c);
                from_hook = true;
            }
        }
        let result = match provided {
            Some(c) => c,
            None => match compaction::compact(
                &prep,
                &summarizer,
                custom_instructions.as_deref(),
                cancel,
            )
            .await
            {
                Ok(r) => r,
                Err(SummaryError::Aborted) => {
                    end(None, true, None, false);
                    return Err("Compaction cancelled".into());
                }
                Err(SummaryError::Failed(m)) => {
                    end(None, false, Some(fail_text(&m)), false);
                    return Err(m);
                }
            },
        };
        if cancel.is_cancelled() {
            end(None, true, None, false);
            return Err("Compaction cancelled".into());
        }
        let appended = {
            let mut s = self.lock();
            let r = s.session.append_compaction(CompactionEntry {
                summary: result.summary.clone(),
                first_kept_entry_id: result.first_kept_entry_id.clone(),
                tokens_before: result.tokens_before,
                details: result.details.clone(),
                from_hook: from_hook.then_some(true),
            });
            s.messages = s.session.build_context().messages;
            let id = s.session.leaf_id().unwrap_or_default().to_string();
            (r, id, s.session.id().to_string())
        };
        if let Err(e) = appended.0 {
            emitter.emit(AgentEvent::Warning(format!(
                "Could not save session: {e:#}"
            )));
        }
        hooks.emit_session(&SessionEvent::Compact {
            session_id: appended.2,
            entry_id: appended.1,
            result: result.clone(),
            from_hook,
        });
        end(Some(result.clone()), false, None, will_retry);
        Ok(result)
    }

    /// Manually compact the context (`/compact [instructions]`). Events
    /// (`CompactionStart` / `CompactionEnd`) arrive on the receiver, which
    /// closes when the compaction is done.
    pub fn compact(&self, custom_instructions: Option<&str>) -> Result<Receiver<AgentEvent>> {
        let cancel = CancellationToken::new();
        let (tx, rx) = async_channel::unbounded();
        let emitter = Emitter::new(tx, self.engine.hooks().clone());
        {
            let mut state = self.lock();
            if state.running.is_some() {
                bail!("The agent is already running");
            }
            self.check_ready(&state)?;
            state.running = Some(cancel.clone());
            state.events = Some(emitter.clone());
        }
        let conv = self.clone();
        let instructions = custom_instructions
            .map(str::trim)
            .filter(|s| !s.is_empty())
            .map(str::to_string);
        self.engine.spawn(async move {
            let _ = conv
                .run_compaction(
                    CompactionReason::Manual,
                    instructions,
                    false,
                    &cancel,
                    &emitter,
                )
                .await;
            conv.finish_run(&emitter);
        });
        Ok(rx)
    }

    // ----- tree, fork, clone ----------------------------------------------

    /// Move to another entry of the session tree (Pi `navigateTree`).
    /// Selecting a user message moves to its parent and returns its text for
    /// editing; optionally the abandoned branch is summarized first.
    pub fn navigate_tree(
        &self,
        target_id: &str,
        options: NavigateOptions,
    ) -> JoinHandle<Result<TreeNavigation>> {
        let conv = self.clone();
        let target_id = target_id.to_string();
        self.engine
            .spawn(async move { conv.navigate_tree_inner(target_id, options).await })
    }

    async fn navigate_tree_inner(
        &self,
        target_id: String,
        options: NavigateOptions,
    ) -> Result<TreeNavigation> {
        let cancel = CancellationToken::new();
        let (old_leaf, target, entries) = {
            let mut state = self.lock();
            if state.running.is_some() {
                bail!("The agent is already running");
            }
            let old_leaf = state.session.leaf_id().map(str::to_string);
            if old_leaf.as_deref() == Some(target_id.as_str()) {
                return Ok(TreeNavigation::default());
            }
            let target = state
                .session
                .entry(&target_id)
                .cloned()
                .with_context(|| format!("Entry {target_id} not found"))?;
            if options.summarize && state.model.is_none() {
                bail!("No model available for summarization");
            }
            let (entries, _) = compaction::collect_entries_for_branch_summary(
                &state.session,
                old_leaf.as_deref(),
                &target_id,
            );
            state.running = Some(cancel.clone());
            (old_leaf, target, entries)
        };
        let result = self
            .navigate_tree_finish(old_leaf, target, entries, options, &cancel)
            .await;
        {
            let mut state = self.lock();
            state.running = None;
            // Entries recorded while the summary ran (names, labels) land now.
            let _ = Self::flush_pending(&mut state);
        }
        result
    }

    async fn navigate_tree_finish(
        &self,
        old_leaf: Option<String>,
        target: SessionEntry,
        entries: Vec<SessionEntry>,
        options: NavigateOptions,
        cancel: &CancellationToken,
    ) -> Result<TreeNavigation> {
        let mut summary = None;
        if options.summarize && !entries.is_empty() {
            let summarizer = self.summarizer()?;
            let reserve = self.engine.harness_settings().branch_summary_reserve_tokens;
            match compaction::generate_branch_summary(
                &entries,
                &summarizer,
                options.custom_instructions.as_deref(),
                options.replace_instructions,
                reserve,
                cancel,
            )
            .await
            {
                Ok(s) => summary = Some(s),
                Err(SummaryError::Aborted) => {
                    return Ok(TreeNavigation {
                        cancelled: true,
                        ..Default::default()
                    });
                }
                Err(SummaryError::Failed(m)) => bail!("{m}"),
            }
        }
        let (new_leaf, editor_text) = match &target.kind {
            EntryKind::Message(m) if m.is_user() => (target.parent_id.clone(), m.user_text()),
            EntryKind::CustomMessage(c) => (target.parent_id.clone(), Some(c.content.plain_text())),
            _ => (Some(target.id.clone()), None),
        };
        let mut state = self.lock();
        let mut summary_entry_id = None;
        if let Some(s) = summary {
            let id = state.session.branch_with_summary(
                new_leaf.as_deref(),
                &s.summary,
                Some(s.details()),
                None,
            )?;
            if let Some(label) = options.label.as_deref().filter(|l| !l.trim().is_empty()) {
                state.session.append_label_change(&id, Some(label))?;
            }
            summary_entry_id = Some(id);
        } else {
            match new_leaf.as_deref() {
                None => state.session.reset_leaf(),
                Some(id) => state.session.branch(id)?,
            }
            if let Some(label) = options.label.as_deref().filter(|l| !l.trim().is_empty()) {
                state.session.append_label_change(&target.id, Some(label))?;
            }
        }
        let ctx = state.session.build_context();
        state.messages = ctx.messages;
        let event = SessionEvent::Tree {
            session_id: state.session.id().to_string(),
            old_leaf_id: old_leaf,
            new_leaf_id: state.session.leaf_id().map(str::to_string),
            summary_entry_id: summary_entry_id.clone(),
        };
        drop(state);
        self.engine.hooks().emit_session(&event);
        Ok(TreeNavigation {
            editor_text,
            cancelled: false,
            summary_entry_id,
        })
    }

    /// Fork into a new session file. With `at == false` (Pi default
    /// "before"), `entry_id` must be a user message: the new session ends at
    /// its parent and its text is returned for editing. With `at == true`
    /// the new session ends at `entry_id`.
    pub fn fork(&self, entry_id: &str, at: bool) -> Result<Fork> {
        let state = self.lock();
        if state.running.is_some() {
            bail!("The agent is already running");
        }
        let entry = state
            .session
            .entry(entry_id)
            .cloned()
            .context("Invalid entry ID for forking")?;
        let (target, selected_text) = if at {
            (Some(entry.id.clone()), None)
        } else {
            match entry.message() {
                Some(m) if m.is_user() => (entry.parent_id.clone(), m.user_text()),
                _ => bail!("Invalid entry ID for forking"),
            }
        };
        let id = format!("fork-{}", state.session.id());
        let previous = state
            .session
            .is_persisted()
            .then(|| state.session.path().to_path_buf());
        let session = match target {
            Some(leaf) => {
                let mut s = state.session.clone();
                s.create_branched_session(&leaf, id)?;
                s
            }
            None if state.session.is_persisted() => Session::create_with(
                state.session.dir(),
                state.session.cwd(),
                id,
                previous.as_ref().map(|p| p.to_string_lossy().into_owned()),
            ),
            None => Session::in_memory(state.session.cwd()),
        };
        let model = state.model.clone();
        drop(state);
        let conversation = Conversation::new(
            self.engine.clone(),
            session,
            model,
            SessionStartReason::Fork,
            previous,
        );
        conversation.lock().thinking = self.thinking();
        Ok(Fork {
            conversation,
            selected_text,
        })
    }

    /// Copy the whole session (every branch) into a new file `clone-<id>`.
    pub fn clone_session(&self) -> Result<Conversation> {
        let state = self.lock();
        let id = format!("clone-{}", state.session.id());
        let session = state.session.clone_to(id)?;
        let model = state.model.clone();
        let previous = state
            .session
            .is_persisted()
            .then(|| state.session.path().to_path_buf());
        drop(state);
        Ok(Conversation::new(
            self.engine.clone(),
            session,
            model,
            SessionStartReason::Clone,
            previous,
        ))
    }

    /// Notify listeners that this conversation is being closed and stop it.
    pub fn shutdown(&self) {
        self.abort();
        let (session_id, session_file) = {
            let s = self.lock();
            (
                s.session.id().to_string(),
                s.session
                    .is_persisted()
                    .then(|| s.session.path().to_path_buf()),
            )
        };
        self.engine.hooks().emit_session(&SessionEvent::Shutdown {
            session_id,
            session_file,
        });
    }

    // ----- goals -----------------------------------------------------------

    pub fn goal(&self) -> Option<Goal> {
        self.goal.lock().expect("goal lock").goal.clone()
    }

    fn persist_goal(&self, goal: Option<&Goal>) -> Result<()> {
        let data = goal
            .map(|g| serde_json::to_value(g).unwrap_or(Value::Null))
            .unwrap_or(Value::Null);
        let mut state = self.lock();
        state.pending.params_dirty = true;
        Self::record_entry(
            &mut state,
            EntryKind::Custom {
                custom_type: GOAL_ENTRY_TYPE.into(),
                data: Some(data).filter(|d| !d.is_null()),
            },
        )
    }

    fn persist_goal_if_dirty(&self) {
        let goal = {
            let mut cell = self.goal.lock().expect("goal lock");
            if !std::mem::take(&mut cell.dirty) {
                return;
            }
            cell.goal.clone()
        };
        let _ = self.persist_goal(goal.as_ref());
    }

    fn account_goal_time(&self, seconds: i64) {
        let mut cell = self.goal.lock().expect("goal lock");
        if let Some(g) = cell.goal.as_mut()
            && seconds > 0
        {
            let before = g.time_used_seconds;
            g.account(seconds, 0);
            if g.time_used_seconds != before {
                cell.dirty = true;
            }
        }
    }

    /// Set a new active goal and start working on it when idle.
    pub fn set_goal(
        &self,
        objective: &str,
        token_budget: Option<i64>,
    ) -> Result<Option<Receiver<AgentEvent>>> {
        let objective = objective.trim();
        if objective.is_empty() {
            bail!("{}", goals::GOAL_USAGE);
        }
        let goal = Goal::new(objective, token_budget);
        {
            let mut cell = self.goal.lock().expect("goal lock");
            cell.goal = Some(goal.clone());
            cell.dirty = false;
        }
        self.persist_goal(Some(&goal))?;
        if self.is_running() || !goal.is_active() {
            return Ok(None);
        }
        self.start_goal_continuation(&goal).map(Some)
    }

    /// Run `/goal <args>`; the receiver is set when a continuation started.
    pub fn goal_command(
        &self,
        args: &str,
    ) -> Result<(GoalCommandOutput, Option<Receiver<AgentEvent>>)> {
        let running = self.is_running();
        let (output, goal) = {
            let mut cell = self.goal.lock().expect("goal lock");
            let out = goals::goal_command(&mut cell.goal, args, running);
            cell.dirty = false;
            (out, cell.goal.clone())
        };
        if output.changed {
            self.persist_goal(goal.as_ref())?;
        }
        if let Some(steer) = &output.steer {
            let m = CustomMessage::new(
                GOAL_CONTINUATION_TYPE,
                UserContent::Text(steer.clone()),
                false,
            );
            let mut state = self.lock();
            state
                .queues
                .push_steering("Goal objective updated".into(), AgentMessage::Custom(m));
            Self::emit_queue(state);
        }
        let rx = match (&output.continuation, goal) {
            (Some(_), Some(g)) if !running => Some(self.start_goal_continuation(&g)?),
            _ => None,
        };
        Ok((output, rx))
    }

    fn start_goal_continuation(&self, goal: &Goal) -> Result<Receiver<AgentEvent>> {
        let m = CustomMessage::new(
            GOAL_CONTINUATION_TYPE,
            UserContent::Text(goals::continuation_prompt(goal)),
            false,
        );
        self.start_run(vec![AgentMessage::Custom(m)], None)
    }

    /// The next continuation message when an active goal should keep going.
    /// A continuation turn that made no tool calls ends the chain so a model
    /// with nothing left to do cannot spin.
    fn goal_continuation(
        &self,
        last: &AssistantMessage,
        outcome: &LoopOutcome,
        continuation_run: bool,
    ) -> Option<AgentMessage> {
        if last.stop_reason != StopReason::Stop || self.lock().queues.has_queued() {
            return None;
        }
        if continuation_run && outcome.tool_calls == 0 {
            return None;
        }
        let goal = self.goal.lock().expect("goal lock").goal.clone()?;
        if !goal.is_active() {
            return None;
        }
        Some(AgentMessage::Custom(CustomMessage::new(
            GOAL_CONTINUATION_TYPE,
            UserContent::Text(goals::continuation_prompt(&goal)),
            false,
        )))
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
        assert!(conv.compact(None).is_err());
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

    #[test]
    fn metadata_tools_and_goals_persist() {
        let dir = tempfile::tempdir().unwrap();
        let engine = Engine::new(AgentHome::new(dir.path().join("home")), dir.path()).unwrap();
        let conv = engine.new_conversation(Some(ModelRef::new("ollama", "m")));
        assert!(conv.set_active_tools(&["nope".into()]).is_err());
        conv.set_active_tools(&["read".into(), "grep".into(), "read".into()])
            .unwrap();
        assert_eq!(conv.active_tools(), ["read", "grep"]);
        conv.set_name("  Named ").unwrap();
        assert_eq!(conv.name().as_deref(), Some("Named"));
        conv.set_thinking(ThinkingLevel::High);
        let (out, rx) = conv.goal_command("pause").unwrap();
        assert_eq!(out.lines, ["No active goal to pause."]);
        assert!(rx.is_none());
        {
            let mut cell = conv.goal.lock().unwrap();
            cell.goal = Some(Goal::new("finish", Some(1_000)));
        }
        conv.persist_goal(conv.goal().as_ref()).unwrap();
        // Force the file to be written (sessions persist once an assistant replies).
        {
            let mut s = conv.lock();
            s.session
                .append_message(&AgentMessage::Llm(Message::Assistant(AssistantMessage {
                    provider: "ollama".into(),
                    model: "m".into(),
                    ..Default::default()
                })))
                .unwrap();
        }
        let label_target = conv.leaf_id().unwrap();
        conv.set_label(&label_target, Some("here")).unwrap();
        let reopened = engine.open_conversation(&conv.session_path()).unwrap();
        assert_eq!(reopened.active_tools(), ["read", "grep"]);
        assert_eq!(reopened.name().as_deref(), Some("Named"));
        assert_eq!(reopened.thinking(), ThinkingLevel::High);
        assert_eq!(reopened.goal().unwrap().objective, "finish");
        assert_eq!(reopened.label(&label_target).as_deref(), Some("here"));
        let registry = reopened.tool_registry(&reopened.lock());
        let names: Vec<_> = registry.iter().map(|t| t.name()).collect();
        assert_eq!(names, ["read", "grep", "update_goal"]);
        assert_eq!(engine.sessions().len(), 1);
        assert_eq!(engine.sessions()[0].title, "Named");
    }
}
