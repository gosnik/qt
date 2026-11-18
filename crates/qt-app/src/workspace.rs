//! Root view: session sidebar, header with model controls, transcript,
//! run indicators, queue, attachments and composer.

mod completion;
mod composer_ui;
mod history;
mod panels;
mod picker;
mod slash;
mod startup_input;
mod status_line;
mod subsystems;

use std::collections::VecDeque;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::time::{Duration, Instant};

use async_channel::Receiver;
use gpui_kit::component::button::{Button, ButtonVariants as _};
use gpui_kit::component::input::{InputEvent, Textarea, TextareaState};
use qt_core::message::{AgentMessage, BashExecutionMessage};
use qt_core::tools::MutationPolicy;

use self::composer_ui::{BashRun, CompletionPopup};
use self::history::PromptHistory;
use self::startup_input::{LineKind, StartupLine, classify_line};
use self::status_line::StatusLine;
use gpui_kit::component::menu::DropdownMenu as _;
use gpui_kit::component::notification::Notification;
use gpui_kit::component::searchable_list::SearchableVec;
use gpui_kit::component::select::{Select, SelectEvent, SelectState};
use gpui_kit::component::spinner::Spinner;
use gpui_kit::component::{
    ActiveTheme as _, Icon, IconName, IndexPath, Sizable as _, WindowExt as _, h_flex, v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::*;
use qt_core::harness::queue::QueueSnapshot;
use qt_core::provider::{ModelInfo, ModelRef, ThinkingLevel};
use qt_core::session::SessionSummary;
use qt_core::{AgentEvent, CompactionReason, Conversation, Engine};

use crate::composer::{
    Attachment, dropped_paths_text, editor_command, has_image_extension, prompt_payload,
    run_external_editor,
};
use crate::dialogs::{self, RunHandler, UiCallback};
use crate::extensions::{ChromeEvent, ExtensionChrome};
use crate::model_picker::{
    ModelItem, ThinkingItem, clamp_level, enabled_models, in_scope, levels_for,
};
use crate::run_status::{Activity, dequeue_text, goal_line, queue_rows};
use crate::session_views::{self, ActionHandler, SessionAction, relative_time};
use crate::settings_panel::{self, SettingsHooks, SettingsPanel, get_bool};
use crate::theme::{self, ThemeState};
use crate::transcript::{Transcript, TranscriptEvent, initial_window};
use crate::{login, ollama_panel};
use qt_core::session_log::BranchLog;
use std::sync::Arc;

actions!(
    qt,
    [
        NewSession,
        StopAgent,
        ToggleSidebar,
        ToggleTheme,
        OpenLogin,
        OpenSettings,
        OpenOllama,
        OpenTree,
        OpenResume,
        ForkSession,
        CloneSession,
        RenameSession,
        CompactContext,
        OpenGoal,
        QueueFollowUp,
        Dequeue,
        ExternalEditor,
        RefreshModels,
        CommandPalette,
        OpenTools,
        OpenContext,
        ExportSession
    ]
);

const CONTEXT: &str = "QtWorkspace";

pub fn bind_keys(cx: &mut App) {
    let (new_session, sidebar, settings, resume, palette) = if cfg!(target_os = "macos") {
        ("cmd-n", "cmd-b", "cmd-,", "cmd-r", "cmd-k")
    } else {
        ("ctrl-n", "ctrl-b", "ctrl-,", "ctrl-r", "ctrl-k")
    };
    cx.bind_keys([
        KeyBinding::new(palette, CommandPalette, Some(CONTEXT)),
        KeyBinding::new(new_session, NewSession, Some(CONTEXT)),
        KeyBinding::new("escape", StopAgent, Some(CONTEXT)),
        KeyBinding::new(sidebar, ToggleSidebar, Some(CONTEXT)),
        KeyBinding::new(settings, OpenSettings, Some(CONTEXT)),
        KeyBinding::new(resume, OpenResume, Some(CONTEXT)),
        KeyBinding::new("alt-enter", QueueFollowUp, Some(CONTEXT)),
        KeyBinding::new("alt-up", Dequeue, Some(CONTEXT)),
        KeyBinding::new("ctrl-g", ExternalEditor, Some(CONTEXT)),
    ]);
}

type ModelSelect = SelectState<SearchableVec<ModelItem>>;
type ThinkingSelect = SelectState<Vec<ThinkingItem>>;

/// What the current event pump is running.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum RunKind {
    Agent,
    Compaction,
}

pub struct Workspace {
    engine: Engine,
    http: reqwest::Client,
    conversation: Conversation,
    transcript: Entity<Transcript>,
    composer: Entity<TextareaState>,
    model_select: Entity<ModelSelect>,
    thinking_select: Entity<ThinkingSelect>,
    models: Vec<ModelInfo>,
    models_loading: bool,
    sessions: Vec<SessionSummary>,
    queue: QueueSnapshot,
    running: Option<RunKind>,
    activity: Option<Activity>,
    attachments: Vec<Attachment>,
    sidebar_open: bool,
    hide_thinking: bool,
    quiet_startup: bool,
    settings_view: Option<WeakEntity<SettingsPanel>>,
    focus: FocusHandle,
    /// The open autocomplete popup.
    completion: Option<CompletionPopup>,
    /// Bumped on every refresh so stale path searches are dropped.
    completion_generation: u64,
    _completion_task: Option<Task<()>>,
    history: PromptHistory,
    /// The running user `!command`.
    bash: Option<BashRun>,
    /// `!command` results already shown (skipped when a run re-emits them).
    shown_bash: Vec<BashExecutionMessage>,
    /// Mutations waiting for approval (`queue` policy).
    pending_mutations: usize,
    /// Extension UI (dialogs, status, widgets, header/footer, shortcuts).
    ext: Entity<ExtensionChrome>,
    /// Runs started by extensions or subagent completions while another
    /// run was being shown.
    pending_runs: VecDeque<Receiver<AgentEvent>>,
    /// Command-line messages not submitted yet (waiting for a run or user
    /// bash to finish).
    startup_lines: VecDeque<StartupLine>,
    /// Status line: git branch, OpenRouter credits, review poll state.
    status: StatusLine,
    _event_task: Option<Task<()>>,
    _ticker: Option<Task<()>>,
    _editor_task: Option<Task<()>>,
    _subscriptions: Vec<Subscription>,
}

impl Workspace {
    pub fn new(engine: Engine, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let conversation = engine.new_conversation(engine.default_model());
        let transcript = cx.new(|cx| Transcript::new(engine.cwd(), cx));
        let composer = cx.new(|cx| {
            TextareaState::new(window, cx)
                .auto_grow(1, 14)
                .submit_on_enter(true)
                .placeholder("Ask qt to read, change or explain code…  (/ for commands, @ for files, ! to run a shell command, Ctrl+K for the palette)")
        });
        let initial_items: SearchableVec<ModelItem> = conversation
            .model()
            .map(|m| vec![ModelItem::from_ref(&m)])
            .unwrap_or_default()
            .into();
        let selected = conversation.model().map(|_| IndexPath::new(0));
        let model_select =
            cx.new(|cx| SelectState::new(initial_items, selected, window, cx).searchable(true));
        let thinking_select =
            cx.new(|cx| SelectState::new(ThinkingItem::all(), Some(IndexPath::new(0)), window, cx));
        let ext = ExtensionChrome::install(
            cx.global::<crate::RuntimeGlobal>().0.clone(),
            composer.clone(),
            transcript.clone(),
            window,
            cx,
        );
        let http = reqwest::Client::builder()
            .user_agent(concat!("qt/", env!("CARGO_PKG_VERSION")))
            .connect_timeout(Duration::from_secs(20))
            .build()
            .unwrap_or_default();

        let subscriptions = vec![
            cx.subscribe_in(
                &composer,
                window,
                |this, _, event: &InputEvent, window, cx| match event {
                    InputEvent::PressEnter { shift: false, .. } => this.submit(false, window, cx),
                    InputEvent::Change => this.on_composer_changed(window, cx),
                    _ => {}
                },
            ),
            cx.subscribe_in(
                &model_select,
                window,
                |this, _, event: &SelectEvent<SearchableVec<ModelItem>>, window, cx| {
                    let SelectEvent::Confirm(Some(value)) = event else {
                        return;
                    };
                    if let Some(model) = ModelRef::parse(value)
                        && this.conversation.model().as_ref() != Some(&model)
                    {
                        this.choose_model(model, window, cx);
                    }
                },
            ),
            cx.subscribe_in(
                &thinking_select,
                window,
                |this, _, event: &SelectEvent<Vec<ThinkingItem>>, _, cx| {
                    let SelectEvent::Confirm(Some(value)) = event else {
                        return;
                    };
                    if let Some(level) = ThinkingLevel::parse(value) {
                        this.conversation.set_thinking(level);
                        cx.notify();
                    }
                },
            ),
            cx.subscribe_in(
                &transcript,
                window,
                |this, _, event: &TranscriptEvent, window, cx| match event {
                    TranscriptEvent::Fork(id) => this.fork_from(id.clone(), window, cx),
                    TranscriptEvent::Reload => this.reload_transcript(cx),
                },
            ),
            window.observe_window_appearance(|_, cx| theme::on_appearance_changed(cx)),
            cx.subscribe_in(&ext, window, |this, _, event: &ChromeEvent, window, cx| {
                this.on_chrome_event(event, window, cx)
            }),
            cx.observe(&ext, |_, _, cx| cx.notify()),
        ];

        let settings = engine.settings();
        let mut this = Self {
            sessions: engine.sessions(),
            engine,
            http,
            conversation,
            transcript,
            composer,
            model_select,
            thinking_select,
            models: Vec::new(),
            models_loading: false,
            queue: QueueSnapshot::default(),
            running: None,
            activity: None,
            attachments: Vec::new(),
            sidebar_open: true,
            hide_thinking: get_bool(&settings, "hideThinkingBlock", false),
            quiet_startup: get_bool(&settings, "quietStartup", false),
            settings_view: None,
            focus: cx.focus_handle(),
            completion: None,
            completion_generation: 0,
            _completion_task: None,
            history: PromptHistory::default(),
            bash: None,
            shown_bash: Vec::new(),
            pending_mutations: 0,
            ext,
            pending_runs: VecDeque::new(),
            startup_lines: VecDeque::new(),
            status: StatusLine::default(),
            _event_task: None,
            _ticker: None,
            _editor_task: None,
            _subscriptions: subscriptions,
        };
        let hide = this.hide_thinking;
        this.transcript
            .update(cx, |t, cx| t.set_hide_thinking(hide, cx));
        this.refresh_models(window, cx);
        this.watch_model_refresh(window, cx);
        this.reload_history(window, cx);
        this.start_status_line(window, cx);
        this.composer
            .update(cx, |state, cx| state.focus(window, cx));
        this
    }

    // ----- models -----------------------------------------------------------

    fn refresh_models(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.models_loading = true;
        let handle = self.engine.list_models();
        cx.spawn_in(window, async move |this, cx| {
            let results = handle.await.unwrap_or_default();
            let _ = this.update_in(cx, |this, window, cx| this.set_models(results, window, cx));
        })
        .detach();
        cx.notify();
    }

    /// Apply the runtime's periodic OpenAI / OpenRouter refresh (every five
    /// minutes, Pi) to the model picker.
    fn watch_model_refresh(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let Some(listings) = cx.global::<crate::RuntimeGlobal>().0.model_refresh() else {
            return;
        };
        cx.spawn_in(window, async move |this, cx| {
            while let Ok(results) = listings.recv().await {
                if this
                    .update_in(cx, |this, window, cx| {
                        this.merge_models(results, false, window, cx)
                    })
                    .is_err()
                {
                    break;
                }
            }
        })
        .detach();
    }

    fn provider_label(&self, id: &str) -> String {
        self.engine
            .provider(id)
            .map(|p| p.label)
            .unwrap_or_else(|| id.to_string())
    }

    fn set_models(
        &mut self,
        results: Vec<(String, Result<Vec<ModelInfo>, String>)>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.models_loading = false;
        let mut models = Vec::new();
        for (provider, result) in results {
            match result {
                Ok(list) => models.extend(list),
                // Ollama is probed even when not configured; stay quiet if it is not running.
                Err(_) if provider.starts_with("ollama") => {}
                Err(err) => window.push_notification(
                    Notification::error(format!("{}: {err}", self.provider_label(&provider))),
                    cx,
                ),
            }
        }
        self.models = models;
        self.models_changed(window, cx);
    }

    /// A partial listing (`/model refresh`, the periodic refresh): models of
    /// the providers that answered replace that provider's entries; models
    /// of every other provider are kept. Failures are reported only when
    /// `report_errors` is set.
    fn merge_models(
        &mut self,
        results: Vec<(String, Result<Vec<ModelInfo>, String>)>,
        report_errors: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.models_loading = false;
        let mut models = std::mem::take(&mut self.models);
        merge_listing(&mut models, &results);
        self.models = models;
        if report_errors {
            for (provider, result) in &results {
                if let Err(err) = result {
                    window.push_notification(
                        Notification::error(format!("{}: {err}", self.provider_label(provider))),
                        cx,
                    );
                }
            }
        }
        self.models_changed(window, cx);
    }

    fn models_changed(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if let Some(window_size) = self
            .conversation
            .model()
            .and_then(|m| self.engine.context_window(&m))
        {
            self.conversation.set_context_window(Some(window_size));
        }
        self.rebuild_model_items(window, cx);
        self.sync_thinking(window, cx);
        if let Some(panel) = self.settings_view.as_ref().and_then(WeakEntity::upgrade) {
            let list = self.model_list();
            panel.update(cx, |p, cx| p.set_models(list, cx));
        }
        cx.notify();
    }

    /// `(provider/id, title)` of every discovered model.
    fn model_list(&self) -> Vec<(String, String)> {
        self.models
            .iter()
            .map(|m| {
                let item = ModelItem::from_info(m, &self.provider_label(&m.provider));
                (item.value.to_string(), item.title.to_string())
            })
            .collect()
    }

    /// Model picker items: scoped models (`enabledModels`) plus the current one.
    fn rebuild_model_items(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let scope = enabled_models(&self.engine.settings());
        let mut items: Vec<ModelItem> = self
            .models
            .iter()
            .filter(|m| in_scope(&scope, &m.model_ref().to_string()))
            .map(|m| ModelItem::from_info(m, &self.provider_label(&m.provider)))
            .collect();
        let current = self.conversation.model();
        if let Some(model) = &current
            && !items.iter().any(|i| i.value.as_ref() == model.to_string())
        {
            let item = self
                .models
                .iter()
                .find(|m| m.model_ref() == *model)
                .map(|m| ModelItem::from_info(m, &self.provider_label(&m.provider)))
                .unwrap_or_else(|| ModelItem::from_ref(model));
            items.insert(0, item);
        }
        let selected = current.map(|m| SharedString::from(m.to_string()));
        self.model_select.update(cx, |state, cx| {
            state.set_items(SearchableVec::new(items), window, cx);
            if let Some(value) = selected {
                state.set_selected_value(&value, window, cx);
            }
        });
    }

    /// Offer the model's thinking levels and clamp the current one.
    fn sync_thinking(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let info = self
            .conversation
            .model()
            .and_then(|m| self.models.iter().find(|i| i.model_ref() == m).cloned());
        let levels = levels_for(info.as_ref());
        let current = self.conversation.thinking();
        let clamped = if info.is_some() {
            clamp_level(current, &levels)
        } else {
            current
        };
        if clamped != current {
            self.conversation.set_thinking(clamped);
        }
        let mut offered = levels;
        if !offered.contains(&clamped) {
            offered.push(clamped);
            offered.sort();
        }
        let value = SharedString::from(clamped.as_str());
        self.thinking_select.update(cx, |s, cx| {
            s.set_items(ThinkingItem::for_levels(&offered), window, cx);
            s.set_selected_value(&value, window, cx);
        });
    }

    // ----- prompting and runs -------------------------------------------------

    fn composer_text(&self, cx: &App) -> String {
        self.composer.read(cx).value().to_string()
    }

    fn set_composer(&mut self, text: &str, window: &mut Window, cx: &mut Context<Self>) {
        let text = text.to_string();
        self.composer.update(cx, |state, cx| {
            state.set_value(text, window, cx);
            state.focus(window, cx);
        });
    }

    /// Enter: `!cmd` runs user bash, `/command` runs a slash command, other
    /// text prompts when idle and steers while running. `follow_up`
    /// (Alt+Enter) queues a follow-up instead of steering.
    fn submit(&mut self, follow_up: bool, window: &mut Window, cx: &mut Context<Self>) {
        let raw = self.composer_text(cx);
        if raw.trim().is_empty() && self.attachments.is_empty() {
            return;
        }
        self.completion = None;
        match classify_line(&raw, !self.attachments.is_empty()) {
            LineKind::Bash { command, exclude } => {
                if self.run_user_bash(&command, exclude, window, cx) {
                    self.history_record(&raw, window, cx);
                    self.clear_composer(window, cx);
                }
                cx.notify();
                return;
            }
            LineKind::Slash => {
                self.history_record(&raw, window, cx);
                self.clear_composer(window, cx);
                self.run_slash(&raw, window, cx);
                cx.notify();
                return;
            }
            LineKind::Prompt => {}
        }
        let (text, images) = prompt_payload(&raw, &self.attachments);
        if !self.send_text(&text, images, follow_up, window, cx) {
            return;
        }
        self.history_record(&raw, window, cx);
        self.attachments.clear();
        self.clear_composer(window, cx);
        cx.notify();
    }

    /// Start the agent; returns false (and keeps the draft) on failure.
    fn start(
        &mut self,
        text: &str,
        images: Vec<qt_core::ContentBlock>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> bool {
        match self.conversation.prompt(text, images) {
            Ok(rx) => {
                self.attach_run(rx, RunKind::Agent, window, cx);
                true
            }
            Err(err) => {
                let message = format!("{err:#}");
                let hint = if message.contains("No model") || message.contains("API key") {
                    " — choose a model or log in (Login in the sidebar)."
                } else {
                    ""
                };
                window.push_notification(Notification::error(format!("{message}{hint}")), cx);
                false
            }
        }
    }

    /// Pump a run's events into the view until the receiver closes.
    fn attach_run(
        &mut self,
        rx: Receiver<AgentEvent>,
        kind: RunKind,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.running = Some(kind);
        self._event_task = Some(cx.spawn_in(window, async move |this, cx| {
            while let Ok(event) = rx.recv().await {
                if this
                    .update_in(cx, |this, window, cx| this.on_event(event, window, cx))
                    .is_err()
                {
                    return;
                }
            }
            let _ = this.update_in(cx, |this, window, cx| this.run_finished(window, cx));
        }));
        cx.notify();
    }

    fn run_finished(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.running.is_none() {
            return;
        }
        self.running = None;
        self.activity = None;
        self._ticker = None;
        self.sessions = self.engine.sessions();
        self.queue = self.conversation.queue();
        self.sync_entry_ids(cx);
        self.next_pending_run(window, cx);
        self.drain_startup_lines(window, cx);
        cx.notify();
    }

    fn on_event(&mut self, event: AgentEvent, window: &mut Window, cx: &mut Context<Self>) {
        let already_shown = matches!(
            &event,
            AgentEvent::CustomMessageEnd(AgentMessage::BashExecution(b)) if self.take_shown_bash(b)
        );
        if !already_shown {
            self.transcript
                .update(cx, |t, cx| t.apply_event(&event, cx));
        }
        if self.engine.mutation_policy() == MutationPolicy::Queue {
            let pending = self.engine.mutation_queue().len();
            if pending != self.pending_mutations {
                self.pending_mutations = pending;
                cx.notify();
            }
        }
        match event {
            AgentEvent::Warning(message) => {
                window.push_notification(Notification::warning(message), cx)
            }
            AgentEvent::QueueUpdate {
                steering,
                follow_up,
            } => {
                self.queue = QueueSnapshot {
                    steering,
                    follow_up,
                };
                cx.notify();
            }
            AgentEvent::CompactionStart { reason } => {
                self.activity = Some(Activity::Compacting { reason });
                cx.notify();
            }
            AgentEvent::CompactionEnd {
                reason,
                result,
                aborted,
                error,
                ..
            } => {
                self.activity = None;
                if let Some(error) = error {
                    window.push_notification(Notification::warning(error), cx);
                } else if aborted {
                    window.push_notification(Notification::info("Compaction cancelled"), cx);
                } else if result.is_some() {
                    self.reload_transcript(cx);
                    if reason == CompactionReason::Manual {
                        window.push_notification(Notification::info("Context compacted"), cx);
                    }
                }
                cx.notify();
            }
            AgentEvent::RetryStart {
                attempt,
                max_attempts,
                delay_ms,
                error,
            } => {
                self.activity = Some(Activity::retrying(attempt, max_attempts, delay_ms, &error));
                self.start_ticker(window, cx);
                cx.notify();
            }
            AgentEvent::RetryEnd {
                success,
                final_error,
                ..
            } => {
                self.activity = None;
                self._ticker = None;
                if !success && let Some(error) = final_error {
                    window.push_notification(Notification::warning(error), cx);
                }
                cx.notify();
            }
            AgentEvent::AgentEnd { aborted, .. } => {
                if aborted {
                    let mut restored = self.conversation.clear_queue();
                    // Unsent command-line messages come back like queued
                    // follow-ups instead of running after the abort.
                    restored
                        .follow_up
                        .extend(self.startup_lines.drain(..).map(|(text, _)| text));
                    if !restored.is_empty() {
                        let draft = self.composer_text(cx);
                        let text = dequeue_text(&restored, &draft);
                        self.set_composer(&text, window, cx);
                    }
                }
                self.run_finished(window, cx);
            }
            _ => {}
        }
    }

    /// Re-render once a second while a retry countdown is visible.
    fn start_ticker(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self._ticker = Some(cx.spawn_in(window, async move |this, cx| {
            loop {
                cx.background_executor()
                    .timer(Duration::from_millis(500))
                    .await;
                let keep = this
                    .update(cx, |this, cx| {
                        cx.notify();
                        matches!(this.activity, Some(Activity::Retrying { .. }))
                    })
                    .unwrap_or(false);
                if !keep {
                    break;
                }
            }
        }));
    }

    /// Show the current branch: a window of its last entries read from the
    /// session file (older ones are read back as the user scrolls), or the
    /// whole in-memory branch while the session has not been written yet.
    fn reload_transcript(&mut self, cx: &mut Context<Self>) {
        let leaf = self.conversation.leaf_id();
        let log = leaf.as_deref().and_then(|leaf| {
            BranchLog::open(&self.conversation.session_path(), Some(leaf))
                .ok()
                .filter(|log| !log.is_empty())
        });
        match log {
            Some(log) => {
                let range = initial_window(&log);
                let entries = log.read(range.clone()).unwrap_or_default();
                let log = Some((Arc::new(log), range));
                self.transcript
                    .update(cx, |t, cx| t.load_window(log, &entries, cx));
            }
            None => {
                let branch = self.conversation.branch();
                self.transcript
                    .update(cx, |t, cx| t.load_window(None, &branch, cx));
            }
        }
    }

    /// After a run, give live user messages their persisted entry ids.
    fn sync_entry_ids(&mut self, cx: &mut Context<Self>) {
        let ids: Vec<String> = self
            .conversation
            .branch()
            .into_iter()
            .filter(|e| e.is_user_message())
            .map(|e| e.id)
            .collect();
        self.transcript
            .update(cx, |t, cx| t.set_user_entry_ids(&ids, cx));
    }

    /// Esc: cancel the running `!command` first, else stop the agent.
    fn stop(&mut self, _: &StopAgent, _window: &mut Window, cx: &mut Context<Self>) {
        if self.conversation.is_bash_running() {
            self.conversation.abort_bash();
            cx.notify();
            return;
        }
        if self.running.is_some() {
            self.conversation.abort();
            cx.notify();
        }
    }

    fn queue_follow_up(&mut self, _: &QueueFollowUp, window: &mut Window, cx: &mut Context<Self>) {
        self.submit(true, window, cx);
    }

    fn dequeue(&mut self, _: &Dequeue, window: &mut Window, cx: &mut Context<Self>) {
        let restored = self.conversation.clear_queue();
        if restored.is_empty() {
            return;
        }
        let draft = self.composer_text(cx);
        let text = dequeue_text(&restored, &draft);
        self.set_composer(&text, window, cx);
        self.queue = QueueSnapshot::default();
        cx.notify();
    }

    // ----- sessions -------------------------------------------------------------

    fn new_session(&mut self, _: &NewSession, window: &mut Window, cx: &mut Context<Self>) {
        let conversation = self.engine.new_conversation(self.conversation.model());
        self.switch_to(conversation, window, cx);
    }

    /// Apply the command line's session choice once the window is open.
    pub fn apply_startup(
        &mut self,
        action: crate::startup::StartupAction,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        use crate::startup::StartupAction;
        match action {
            StartupAction::None => {}
            StartupAction::Open(path) => self.open_session(path, window, cx),
            StartupAction::Resume => self.open_resume(&OpenResume, window, cx),
            StartupAction::Fork(path) => match self.engine.fork_session(&path) {
                Ok(conversation) => {
                    self.switch_to(conversation, window, cx);
                    self.sessions = self.engine.sessions();
                }
                Err(err) => {
                    window.push_notification(Notification::error(format!("Error: {err:#}")), cx)
                }
            },
        }
    }

    /// Resume `path`, asking first when its working directory is gone.
    fn open_session(&mut self, path: PathBuf, window: &mut Window, cx: &mut Context<Self>) {
        if path == self.conversation.session_path() {
            return;
        }
        if let Some(issue) = self.engine.session_cwd_issue(&path) {
            let this = cx.entity().downgrade();
            let prompt = issue.prompt_text();
            window.open_alert_dialog(cx, move |alert, _, _| {
                let this = this.clone();
                let path = path.clone();
                alert
                    .confirm()
                    .title("Session cwd not found")
                    .description(prompt.clone())
                    .ok_text("Continue")
                    .cancel_text("Cancel")
                    .on_ok(move |_, window, cx| {
                        let path = path.clone();
                        let _ = this.update(cx, |this, cx| {
                            let cwd = this.engine.cwd().to_path_buf();
                            match this.engine.open_conversation_in(&path, &cwd) {
                                Ok(c) => this.switch_to(c, window, cx),
                                Err(err) => window.push_notification(
                                    Notification::error(format!("Could not open session: {err:#}")),
                                    cx,
                                ),
                            }
                        });
                        true
                    })
            });
            return;
        }
        match self.engine.open_conversation(&path) {
            Ok(conversation) => self.switch_to(conversation, window, cx),
            Err(err) => window.push_notification(
                Notification::error(format!("Could not open session: {err:#}")),
                cx,
            ),
        }
    }

    fn switch_to(
        &mut self,
        conversation: Conversation,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.running.is_some() {
            self.conversation.abort();
        }
        self._event_task = None;
        self._ticker = None;
        self.running = None;
        self.activity = None;
        self.pending_runs.clear();
        self.queue = QueueSnapshot::default();
        if conversation.model().is_none()
            && let Some(model) = self.conversation.model()
        {
            conversation.set_model(model);
        }
        self.conversation.abort_bash();
        self.conversation.shutdown();
        self.conversation = conversation;
        self.completion = None;
        self.bash = None;
        self.reload_history(window, cx);
        self.reload_transcript(cx);
        self.rebuild_model_items(window, cx);
        self.sync_thinking(window, cx);
        self.refresh_credits(false, cx);
        self.sessions = self.engine.sessions();
        self.composer
            .update(cx, |state, cx| state.focus(window, cx));
        cx.notify();
    }

    fn session_handler(&self, cx: &mut Context<Self>) -> ActionHandler {
        let this = cx.entity().downgrade();
        Rc::new(move |action, window, cx| {
            let _ = this.update(cx, |this, cx| this.on_session_action(action, window, cx));
        })
    }

    fn on_session_action(
        &mut self,
        action: SessionAction,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        match action {
            SessionAction::Navigated { editor_text } => {
                self.reload_transcript(cx);
                if let Some(text) = editor_text
                    && self.composer_text(cx).trim().is_empty()
                {
                    self.set_composer(&text, window, cx);
                }
                window.push_notification(Notification::info("Navigated to selected point"), cx);
            }
            SessionAction::Labeled => {}
            SessionAction::Fork(id) => self.fork_from(id, window, cx),
            SessionAction::Open(path) => {
                window.close_dialog(cx);
                self.open_session(path, window, cx);
            }
            SessionAction::Import(path) => self.confirm_import(path, window, cx),
            SessionAction::Renamed => {}
            SessionAction::RenameCurrent(name) => self.rename_to(&name, window, cx),
        }
        self.sessions = self.engine.sessions();
        cx.notify();
    }

    fn open_tree(&mut self, _: &OpenTree, window: &mut Window, cx: &mut Context<Self>) {
        if self.conversation.entries().is_empty() {
            window.push_notification(Notification::info("No entries in session"), cx);
            return;
        }
        let handler = self.session_handler(cx);
        session_views::open_tree(&self.engine, &self.conversation, handler, window, cx);
    }

    fn open_resume(&mut self, _: &OpenResume, window: &mut Window, cx: &mut Context<Self>) {
        let handler = self.session_handler(cx);
        let current = self.conversation.session_path();
        session_views::open_picker(&self.engine, current, handler, window, cx);
    }

    fn fork_session(&mut self, _: &ForkSession, window: &mut Window, cx: &mut Context<Self>) {
        let messages = self.conversation.user_messages_for_forking();
        let this = cx.entity().downgrade();
        dialogs::open_fork_picker(
            messages,
            move |id, window, cx| {
                let _ = this.update(cx, |this, cx| this.fork_from(id, window, cx));
            },
            window,
            cx,
        );
    }

    /// Fork before the user message `entry_id`; its text seeds the composer.
    fn fork_from(&mut self, entry_id: String, window: &mut Window, cx: &mut Context<Self>) {
        if self.running.is_some() {
            window.push_notification(
                Notification::warning("Wait for the agent to finish before forking."),
                cx,
            );
            return;
        }
        match self.conversation.fork(&entry_id, false) {
            Ok(fork) => {
                self.switch_to(fork.conversation, window, cx);
                if let Some(text) = fork.selected_text {
                    self.set_composer(&text, window, cx);
                }
                window.push_notification(Notification::info("Forked to new session"), cx);
            }
            Err(err) => window.push_notification(Notification::error(format!("{err:#}")), cx),
        }
    }

    fn clone_session(&mut self, _: &CloneSession, window: &mut Window, cx: &mut Context<Self>) {
        if self.conversation.leaf_id().is_none() {
            window.push_notification(Notification::info("Nothing to clone yet"), cx);
            return;
        }
        if self.running.is_some() {
            window.push_notification(
                Notification::warning("Wait for the agent to finish before cloning."),
                cx,
            );
            return;
        }
        match self.conversation.clone_session() {
            Ok(conversation) => {
                self.switch_to(conversation, window, cx);
                self.composer
                    .update(cx, |state, cx| state.set_value("", window, cx));
                window.push_notification(Notification::info("Cloned to new session"), cx);
            }
            Err(err) => window.push_notification(Notification::error(format!("{err:#}")), cx),
        }
    }

    fn rename_session(&mut self, _: &RenameSession, window: &mut Window, cx: &mut Context<Self>) {
        let this = cx.entity().downgrade();
        dialogs::open_rename(
            self.conversation.name(),
            move |name, window, cx| {
                let _ = this.update(cx, |this, cx| this.rename_to(&name, window, cx));
            },
            window,
            cx,
        );
    }

    fn rename_to(&mut self, name: &str, window: &mut Window, cx: &mut Context<Self>) {
        match self.conversation.set_name(name) {
            Ok(()) => {
                let text = if name.trim().is_empty() {
                    "Session name cleared".to_string()
                } else {
                    format!("Session name set: {}", name.trim())
                };
                window.push_notification(Notification::info(text), cx);
            }
            Err(err) => window.push_notification(Notification::error(format!("{err:#}")), cx),
        }
        self.sessions = self.engine.sessions();
        cx.notify();
    }

    fn confirm_import(&mut self, path: PathBuf, window: &mut Window, cx: &mut Context<Self>) {
        let this = cx.entity().downgrade();
        let shown = path.display().to_string();
        window.open_alert_dialog(cx, move |alert, _, _| {
            let this = this.clone();
            let path = path.clone();
            alert
                .confirm()
                .title("Import session")
                .description(format!("Replace current session with {shown}?"))
                .ok_text("Import")
                .on_ok(move |_, window, cx| {
                    let path = path.clone();
                    let _ = this.update(cx, |this, cx| this.import(path, window, cx));
                    true
                })
        });
    }

    fn import(&mut self, path: PathBuf, window: &mut Window, cx: &mut Context<Self>) {
        let resolved = qt_core::util::resolve_path(self.engine.cwd(), &path.to_string_lossy());
        if resolved.exists()
            && let Some(issue) = self.engine.session_cwd_issue(&resolved)
        {
            let this = cx.entity().downgrade();
            let prompt = issue.prompt_text();
            window.open_alert_dialog(cx, move |alert, _, _| {
                let this = this.clone();
                let path = resolved.clone();
                alert
                    .confirm()
                    .title("Session cwd not found")
                    .description(prompt.clone())
                    .ok_text("Continue")
                    .on_ok(move |_, window, cx| {
                        let path = path.clone();
                        let _ = this.update(cx, |this, cx| {
                            let cwd = this.engine.cwd().to_path_buf();
                            this.finish_import(&path, Some(&cwd), window, cx)
                        });
                        true
                    })
            });
            return;
        }
        self.finish_import(&resolved, None, window, cx);
    }

    fn finish_import(
        &mut self,
        path: &Path,
        cwd: Option<&Path>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        match self.engine.import_session(path, cwd) {
            Ok(conversation) => {
                window.close_all_dialogs(cx);
                self.switch_to(conversation, window, cx);
                window.push_notification(
                    Notification::info(format!("Session imported from: {}", path.display())),
                    cx,
                );
            }
            Err(err) => window.push_notification(
                Notification::error(format!("Failed to import session: {err:#}")),
                cx,
            ),
        }
    }

    // ----- harness controls ---------------------------------------------------

    fn run_handler(&self, cx: &mut Context<Self>) -> RunHandler {
        let this = cx.entity().downgrade();
        Rc::new(move |rx, window, cx| {
            let _ = this.update(cx, |this, cx| {
                this.attach_run(rx, RunKind::Agent, window, cx)
            });
        })
    }

    fn compact_context(&mut self, _: &CompactContext, window: &mut Window, cx: &mut Context<Self>) {
        if self.running.is_some() {
            window.push_notification(
                Notification::warning("Wait for the agent to finish before compacting."),
                cx,
            );
            return;
        }
        let this = cx.entity().downgrade();
        dialogs::open_compact(
            move |instructions, window, cx| {
                let _ = this.update(cx, |this, cx| {
                    match this.conversation.compact(instructions.as_deref()) {
                        Ok(rx) => this.attach_run(rx, RunKind::Compaction, window, cx),
                        Err(err) => {
                            window.push_notification(Notification::error(format!("{err:#}")), cx)
                        }
                    }
                });
            },
            window,
            cx,
        );
    }

    fn open_goal(&mut self, _: &OpenGoal, window: &mut Window, cx: &mut Context<Self>) {
        let this = cx.entity().downgrade();
        let changed: UiCallback = Rc::new(move |_, cx| {
            let _ = this.update(cx, |_, cx| cx.notify());
        });
        dialogs::open_goal(
            &self.conversation,
            self.run_handler(cx),
            changed,
            window,
            cx,
        );
    }

    // ----- composer -------------------------------------------------------------

    fn attach_files(&mut self, paths: &[PathBuf], window: &mut Window, cx: &mut Context<Self>) {
        let auto_resize = self.engine.settings().image_auto_resize();
        let mut others = Vec::new();
        for path in paths {
            if !has_image_extension(path) {
                others.push(path.clone());
                continue;
            }
            match Attachment::from_path(path, auto_resize) {
                Ok(a) => self.attachments.push(a),
                Err(err) => window.push_notification(Notification::error(format!("{err:#}")), cx),
            }
        }
        if !others.is_empty() {
            let insert = dropped_paths_text(&others);
            let draft = self.composer_text(cx);
            let text = if draft.is_empty() || draft.ends_with(char::is_whitespace) {
                format!("{draft}{insert}")
            } else {
                format!("{draft} {insert}")
            };
            self.set_composer(&text, window, cx);
        }
        cx.notify();
    }

    /// Clipboard paste: images become attachments; text is left to the input.
    fn paste(&mut self, item: &ClipboardItem, window: &mut Window, cx: &mut Context<Self>) -> bool {
        let auto_resize = self.engine.settings().image_auto_resize();
        let mut took = false;
        for entry in item.entries() {
            match entry {
                ClipboardEntry::Image(image) => {
                    let n = self.attachments.len() + 1;
                    match Attachment::from_bytes(
                        format!("pasted-image-{n}"),
                        &image.bytes,
                        auto_resize,
                    ) {
                        Ok(a) => self.attachments.push(a),
                        Err(err) => window.push_notification(
                            Notification::error(format!("Could not paste image: {err:#}")),
                            cx,
                        ),
                    }
                    took = true;
                }
                ClipboardEntry::ExternalPaths(paths)
                    if paths.paths().iter().any(|p| has_image_extension(p)) =>
                {
                    let paths = paths.paths().to_vec();
                    self.attach_files(&paths, window, cx);
                    took = true;
                }
                _ => {}
            }
        }
        if took {
            cx.notify();
        }
        took
    }

    fn external_editor(&mut self, _: &ExternalEditor, window: &mut Window, cx: &mut Context<Self>) {
        let visual = std::env::var("VISUAL").ok();
        let editor = std::env::var("EDITOR").ok();
        let Some(command) = editor_command(visual.as_deref(), editor.as_deref()) else {
            window.push_notification(
                Notification::warning(
                    "No editor configured. Set $VISUAL or $EDITOR environment variable.",
                ),
                cx,
            );
            return;
        };
        if self._editor_task.is_some() {
            return;
        }
        let text = self.composer_text(cx);
        let (tx, rx) = async_channel::bounded(1);
        std::thread::spawn(move || {
            let _ = tx.send_blocking(run_external_editor(&command, &text));
        });
        self._editor_task = Some(cx.spawn_in(window, async move |this, cx| {
            let result = rx.recv().await;
            let _ = this.update_in(cx, |this, window, cx| {
                this._editor_task = None;
                match result {
                    Ok(Ok(Some(text))) => this.set_composer(&text, window, cx),
                    Ok(Ok(None)) => {}
                    Ok(Err(err)) => {
                        window.push_notification(Notification::error(format!("{err:#}")), cx)
                    }
                    Err(_) => {}
                }
                cx.notify();
            });
        }));
        cx.notify();
    }

    // ----- panels -----------------------------------------------------------------

    fn after_auth_change(
        &self,
        cx: &mut Context<Self>,
    ) -> impl Fn(&mut Window, &mut App) + 'static {
        let this = cx.entity().downgrade();
        move |window, cx| {
            let _ = this.update(cx, |this, cx| {
                if this.conversation.model().is_none()
                    && let Some(model) = this.engine.default_model()
                {
                    this.conversation.set_model(model);
                }
                this.refresh_models(window, cx);
            });
        }
    }

    fn open_login(&mut self, _: &OpenLogin, window: &mut Window, cx: &mut Context<Self>) {
        let changed = self.after_auth_change(cx);
        login::open(&self.engine, &self.http, changed, window, cx);
    }

    fn open_ollama(&mut self, _: &OpenOllama, window: &mut Window, cx: &mut Context<Self>) {
        let changed = self.after_auth_change(cx);
        ollama_panel::open(&self.engine, &self.http, changed, window, cx);
    }

    fn open_settings(&mut self, _: &OpenSettings, window: &mut Window, cx: &mut Context<Self>) {
        let this = cx.entity().downgrade();
        let refresh_this = this.clone();
        let hooks = SettingsHooks {
            changed: Rc::new(move |window, cx| {
                let _ = this.update(cx, |this, cx| this.settings_changed(window, cx));
            }),
            refresh_models: Rc::new(move |window, cx| {
                let _ = refresh_this.update(cx, |this, cx| this.refresh_models(window, cx));
            }),
        };
        let panel = settings_panel::open(
            &self.engine,
            &self.conversation,
            self.model_list(),
            hooks,
            window,
            cx,
        );
        self.settings_view = Some(panel.downgrade());
    }

    fn settings_changed(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let settings = self.engine.settings();
        self.hide_thinking = get_bool(&settings, "hideThinkingBlock", false);
        self.quiet_startup = get_bool(&settings, "quietStartup", false);
        let hide = self.hide_thinking;
        self.transcript
            .update(cx, |t, cx| t.set_hide_thinking(hide, cx));
        self.rebuild_model_items(window, cx);
        // `terminal.promptHistoryLimit` may have changed.
        self.reload_history(window, cx);
        cx.notify();
    }

    fn refresh_models_action(
        &mut self,
        _: &RefreshModels,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.refresh_models(window, cx);
    }

    fn toggle_sidebar(&mut self, _: &ToggleSidebar, _: &mut Window, cx: &mut Context<Self>) {
        self.sidebar_open = !self.sidebar_open;
        cx.notify();
    }

    /// Switch between the built-in dark and light themes.
    fn toggle_theme(&mut self, _: &ToggleTheme, window: &mut Window, cx: &mut Context<Self>) {
        let name = if cx.theme().is_dark() {
            "light"
        } else {
            "dark"
        };
        if let Err(err) = self
            .engine
            .update_settings(|s| settings_panel::set_theme_setting(s, Some(name)))
        {
            window.push_notification(
                Notification::error(format!("Could not save theme: {err:#}")),
                cx,
            );
        }
        theme::apply_selection(Some(name), cx);
    }

    // ----- rendering --------------------------------------------------------------

    fn render_sidebar(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        let current = self.conversation.session_path();
        let mut list = v_flex()
            .id("sessions")
            .flex_1()
            .min_h_0()
            .overflow_y_scroll()
            .gap_0p5()
            .px_2();
        if self.sessions.is_empty() {
            list = list.child(
                div()
                    .px_2()
                    .py_1()
                    .text_xs()
                    .text_color(theme.muted_foreground)
                    .child("No saved sessions yet."),
            );
        }
        for (i, session) in self.sessions.iter().enumerate() {
            let path = session.path.clone();
            let active = session.path == current;
            list = list.child(
                v_flex()
                    .id(("session", i))
                    .px_2()
                    .py_1p5()
                    .rounded_md()
                    .cursor_pointer()
                    .when(active, |el| {
                        el.bg(theme.sidebar_accent)
                            .text_color(theme.sidebar_accent_foreground)
                    })
                    .when(!active, |el| {
                        el.hover(|s| s.bg(theme.sidebar_accent.opacity(0.6)))
                    })
                    .on_click(cx.listener(move |this, _, window, cx| {
                        this.open_session(path.clone(), window, cx)
                    }))
                    .child(
                        div()
                            .text_sm()
                            .overflow_hidden()
                            .whitespace_nowrap()
                            .text_ellipsis()
                            .child(session.title.clone()),
                    )
                    .child(
                        div()
                            .text_xs()
                            .text_color(theme.muted_foreground)
                            .child(relative_time(session.modified)),
                    ),
            );
        }
        v_flex()
            .w(px(260.))
            .h_full()
            .flex_none()
            .bg(theme.sidebar)
            .text_color(theme.sidebar_foreground)
            .border_r_1()
            .border_color(theme.sidebar_border)
            .child(
                h_flex()
                    .px_3()
                    .py_2()
                    .gap_1()
                    .items_center()
                    .child(
                        div()
                            .flex_1()
                            .text_sm()
                            .font_weight(FontWeight::SEMIBOLD)
                            .child("Sessions"),
                    )
                    .child(
                        Button::new("resume")
                            .icon(IconName::Search)
                            .ghost()
                            .small()
                            .tooltip("Resume a session (Ctrl/Cmd+R)")
                            .on_click(cx.listener(|this, _, window, cx| {
                                this.open_resume(&OpenResume, window, cx)
                            })),
                    )
                    .child(
                        Button::new("new-session")
                            .icon(IconName::Plus)
                            .ghost()
                            .small()
                            .tooltip("New session (Ctrl/Cmd+N)")
                            .on_click(cx.listener(|this, _, window, cx| {
                                this.new_session(&NewSession, window, cx)
                            })),
                    ),
            )
            .child(list)
            .child(
                h_flex()
                    .px_2()
                    .py_2()
                    .gap_1()
                    .border_t_1()
                    .border_color(theme.sidebar_border)
                    .child(
                        Button::new("login")
                            .icon(IconName::User)
                            .label("Login")
                            .ghost()
                            .small()
                            .on_click(cx.listener(|this, _, window, cx| {
                                this.open_login(&OpenLogin, window, cx)
                            })),
                    )
                    .child(
                        Button::new("ollama")
                            .icon(IconName::Cpu)
                            .ghost()
                            .small()
                            .tooltip("Ollama")
                            .on_click(cx.listener(|this, _, window, cx| {
                                this.open_ollama(&OpenOllama, window, cx)
                            })),
                    )
                    .child(
                        Button::new("memory")
                            .icon(IconName::BookOpen)
                            .ghost()
                            .small()
                            .tooltip("Memory (/memory)")
                            .on_click(cx.listener(|this, _, window, cx| {
                                let host = this.memory_host(cx);
                                crate::memory::open_panel(
                                    host,
                                    crate::memory::Section::Status,
                                    window,
                                    cx,
                                )
                            })),
                    )
                    .child(
                        Button::new("settings")
                            .icon(IconName::Settings)
                            .ghost()
                            .small()
                            .tooltip("Settings (Ctrl/Cmd+,)")
                            .on_click(cx.listener(|this, _, window, cx| {
                                this.open_settings(&OpenSettings, window, cx)
                            })),
                    )
                    .child(div().flex_1())
                    .child(
                        Button::new("theme")
                            .icon(if theme.is_dark() {
                                IconName::Sun
                            } else {
                                IconName::Moon
                            })
                            .ghost()
                            .small()
                            .tooltip("Toggle light/dark theme")
                            .on_click(cx.listener(|this, _, window, cx| {
                                this.toggle_theme(&ToggleTheme, window, cx)
                            })),
                    ),
            )
    }

    fn render_header(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        let cwd = self.engine.cwd().display().to_string();
        let title = self.conversation.name();
        let focus = self.focus.clone();
        h_flex()
            .w_full()
            .px_3()
            .py_2()
            .gap_2()
            .items_center()
            .border_b_1()
            .border_color(theme.border)
            .child(
                Button::new("toggle-sidebar")
                    .icon(if self.sidebar_open {
                        IconName::PanelLeftClose
                    } else {
                        IconName::PanelLeftOpen
                    })
                    .ghost()
                    .small()
                    .tooltip("Toggle sessions (Ctrl/Cmd+B)")
                    .on_click(cx.listener(|this, _, window, cx| {
                        this.toggle_sidebar(&ToggleSidebar, window, cx)
                    })),
            )
            .child(
                v_flex()
                    .flex_1()
                    .min_w_0()
                    .overflow_hidden()
                    .when_some(title, |el, title| {
                        el.child(
                            div()
                                .text_sm()
                                .font_weight(FontWeight::SEMIBOLD)
                                .whitespace_nowrap()
                                .text_ellipsis()
                                .child(title),
                        )
                    })
                    .child(
                        div()
                            .whitespace_nowrap()
                            .text_ellipsis()
                            .text_xs()
                            .text_color(theme.muted_foreground)
                            .child(cwd),
                    ),
            )
            .child(
                div().w(px(340.)).child(
                    Select::new(&self.model_select)
                        .small()
                        .placeholder(if self.models_loading {
                            "Loading models…"
                        } else {
                            "Select a model"
                        })
                        .search_placeholder("Search models")
                        .menu_width(px(460.)),
                ),
            )
            .child(
                div()
                    .w(px(170.))
                    .child(Select::new(&self.thinking_select).small()),
            )
            .child(
                Button::new("session-menu")
                    .icon(IconName::EllipsisVertical)
                    .ghost()
                    .small()
                    .tooltip("Session")
                    .dropdown_menu(move |menu, _, _| {
                        menu.action_context(focus.clone())
                            .menu("Command palette… (Ctrl/Cmd+K)", Box::new(CommandPalette))
                            .separator()
                            .menu("Session tree…", Box::new(OpenTree))
                            .menu("Fork from a message…", Box::new(ForkSession))
                            .menu("Clone session", Box::new(CloneSession))
                            .menu("Rename session…", Box::new(RenameSession))
                            .menu("Resume session…", Box::new(OpenResume))
                            .menu("Export session…", Box::new(ExportSession))
                            .separator()
                            .menu("Tools and approvals…", Box::new(OpenTools))
                            .menu("Context inspector…", Box::new(OpenContext))
                            .menu("Compact context…", Box::new(CompactContext))
                            .menu("Goal…", Box::new(OpenGoal))
                            .separator()
                            .menu("Refresh models", Box::new(RefreshModels))
                            .menu("Settings…", Box::new(OpenSettings))
                    }),
            )
    }

    fn render_status(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        let usage = self.conversation.usage();
        let context = self.conversation.context_tokens();
        let window_size = self.conversation.context_window();
        let mut parts = Vec::new();
        if context > 0 {
            parts.push(match window_size {
                Some(max) => format!(
                    "context {} / {} ({:.0}%)",
                    tokens(context),
                    tokens(max),
                    context as f64 * 100.0 / max as f64
                ),
                None => format!("context {}", tokens(context)),
            });
        }
        if usage.output_tokens > 0 {
            parts.push(format!(
                "↑{} ↓{}",
                tokens(usage.input_tokens + usage.cache_read_tokens + usage.cache_write_tokens),
                tokens(usage.output_tokens)
            ));
        }
        if usage.total_cost_usd > 0.0 {
            parts.push(format!("${:.4}", usage.total_cost_usd));
        }
        let goal = self.conversation.goal();
        let hint = if self.bash.is_some() {
            "Esc cancels the shell command"
        } else {
            match self.running {
                Some(RunKind::Agent) => "Esc to stop · Enter steers · Alt+Enter follows up",
                Some(RunKind::Compaction) => "Esc to cancel compaction",
                None => "Enter to send · Shift+Enter for newline · ↑ history",
            }
        };
        let pending = self.pending_mutations;
        h_flex()
            .w_full()
            .gap_3()
            .items_center()
            .text_xs()
            .text_color(theme.muted_foreground)
            .child(div().flex_1().child(parts.join(" · ")))
            .when(pending > 0, |el| {
                el.child(
                    div()
                        .id("pending-mutations")
                        .px_1p5()
                        .rounded_sm()
                        .cursor_pointer()
                        .bg(theme.warning.opacity(0.2))
                        .text_color(theme.warning)
                        .hover(|s| s.bg(theme.warning.opacity(0.3)))
                        .on_click(cx.listener(|this, _, window, cx| {
                            this.open_tools_panel(None, window, cx)
                        }))
                        .child(format!(
                            "{pending} pending approval{}",
                            if pending == 1 { "" } else { "s" }
                        )),
                )
            })
            .when_some(goal, |el, goal| {
                el.child(
                    div()
                        .id("goal-status")
                        .px_1p5()
                        .rounded_sm()
                        .cursor_pointer()
                        .bg(theme.muted.opacity(0.6))
                        .text_color(if goal.is_active() {
                            theme.primary
                        } else {
                            theme.muted_foreground
                        })
                        .hover(|s| s.bg(theme.muted))
                        .on_click(
                            cx.listener(|this, _, window, cx| {
                                this.open_goal(&OpenGoal, window, cx)
                            }),
                        )
                        .child(goal_line(&goal)),
                )
            })
            .child(hint)
    }

    /// Archived footer: `~/dir (branch) • session` on the left, `(provider)
    /// model • thinking • credits` on the right.
    fn render_status_line(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        h_flex()
            .id("status-line")
            .w_full()
            .gap_3()
            .items_center()
            .text_xs()
            .child(
                div()
                    .flex_1()
                    .min_w_0()
                    .overflow_hidden()
                    .whitespace_nowrap()
                    .text_ellipsis()
                    .text_color(theme.success)
                    .child(self.path_status_text()),
            )
            .child(
                div()
                    .flex_none()
                    .whitespace_nowrap()
                    .text_color(theme.info)
                    .child(self.model_status_text()),
            )
    }

    fn render_activity(&self, cx: &mut Context<Self>) -> Option<AnyElement> {
        let activity = self.activity.as_ref()?;
        let theme = cx.theme().clone();
        let text = activity.text(Instant::now());
        Some(
            h_flex()
                .gap_2()
                .items_center()
                .px_2()
                .py_1()
                .rounded_md()
                .bg(theme.muted.opacity(0.5))
                .text_xs()
                .child(Spinner::new().small())
                .child(div().flex_1().child(text))
                .child(
                    Button::new("activity-cancel")
                        .label("Cancel")
                        .xsmall()
                        .ghost()
                        .on_click(cx.listener(|this, _, w, cx| this.stop(&StopAgent, w, cx))),
                )
                .into_any_element(),
        )
    }

    fn render_queue(&self, cx: &mut Context<Self>) -> Option<AnyElement> {
        if self.queue.is_empty() {
            return None;
        }
        let theme = cx.theme().clone();
        let mut col = v_flex()
            .gap_0p5()
            .px_2()
            .py_1()
            .rounded_md()
            .border_1()
            .border_color(theme.border)
            .text_xs();
        for (steering, text) in queue_rows(&self.queue) {
            col = col.child(
                h_flex()
                    .gap_2()
                    .child(
                        div()
                            .flex_none()
                            .text_color(theme.primary)
                            .child(if steering { "Steering" } else { "Follow-up" }),
                    )
                    .child(
                        div()
                            .flex_1()
                            .min_w_0()
                            .overflow_hidden()
                            .whitespace_nowrap()
                            .text_ellipsis()
                            .child(SharedString::from(
                                text.lines().next().unwrap_or_default().to_string(),
                            )),
                    ),
            );
        }
        col = col.child(
            h_flex().justify_end().child(
                Button::new("dequeue")
                    .label("Restore to editor (Alt+Up)")
                    .xsmall()
                    .ghost()
                    .on_click(cx.listener(|this, _, w, cx| this.dequeue(&Dequeue, w, cx))),
            ),
        );
        Some(col.into_any_element())
    }

    fn render_attachments(&self, cx: &mut Context<Self>) -> Option<AnyElement> {
        if self.attachments.is_empty() {
            return None;
        }
        let theme = cx.theme().clone();
        let mut row = h_flex().gap_1().flex_wrap();
        for (i, a) in self.attachments.iter().enumerate() {
            row = row.child(
                h_flex()
                    .gap_1()
                    .items_center()
                    .pl_2()
                    .pr_0p5()
                    .py_0p5()
                    .rounded_md()
                    .border_1()
                    .border_color(theme.border)
                    .bg(theme.muted.opacity(0.5))
                    .text_xs()
                    .child(Icon::new(IconName::File).xsmall())
                    .child(a.caption())
                    .child(
                        Button::new(("remove-attachment", i))
                            .icon(IconName::Close)
                            .xsmall()
                            .ghost()
                            .on_click(cx.listener(move |this, _, _, cx| {
                                if i < this.attachments.len() {
                                    this.attachments.remove(i);
                                }
                                cx.notify();
                            })),
                    ),
            );
        }
        Some(row.into_any_element())
    }

    fn render_composer(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let action = if self.running.is_some() {
            Button::new("stop")
                .icon(IconName::Square)
                .danger()
                .tooltip("Stop (Esc)")
                .on_click(cx.listener(|this, _, window, cx| this.stop(&StopAgent, window, cx)))
        } else {
            Button::new("send")
                .icon(IconName::ArrowUp)
                .primary()
                .tooltip("Send (Enter)")
                .on_click(cx.listener(|this, _, window, cx| this.submit(false, window, cx)))
        };
        let this = cx.entity().downgrade();
        let textarea =
            Textarea::new(&self.composer)
                .appearance(false)
                .on_paste(move |item, window, cx| {
                    this.update(cx, |this, cx| this.paste(item, window, cx))
                        .unwrap_or(false)
                });
        h_flex()
            .w_full()
            .justify_center()
            .px_6()
            .pb_3()
            .pt_2()
            .child(
                v_flex()
                    .w_full()
                    .max_w(px(920.))
                    .gap_1p5()
                    .children(self.render_activity(cx))
                    .children(
                        self.ext
                            .read(cx)
                            .render_above_composer(self.running == Some(RunKind::Agent), cx),
                    )
                    .children(self.render_queue(cx))
                    .child(
                        div()
                            .relative()
                            .w_full()
                            .children(self.render_completion(cx).map(|popup| {
                                div()
                                    .absolute()
                                    .bottom_full()
                                    .left_0()
                                    .w_full()
                                    .pb_1()
                                    .child(popup)
                            }))
                            .child(self.render_composer_box(textarea, action, cx)),
                    )
                    .child(self.render_status_line(cx))
                    .child(self.render_status(cx))
                    .children(self.ext.read(cx).render_footer(cx)),
            )
    }

    fn render_composer_box(
        &self,
        textarea: Textarea,
        action: Button,
        cx: &mut Context<Self>,
    ) -> impl IntoElement {
        let theme = cx.theme().clone();
        v_flex()
            .id("composer-box")
            .capture_action(cx.listener(Self::capture_up))
            .capture_action(cx.listener(Self::capture_down))
            .capture_action(cx.listener(Self::capture_tab))
            .capture_action(cx.listener(Self::capture_enter))
            .capture_action(cx.listener(Self::capture_escape))
            .w_full()
            .gap_1()
            .p_2()
            .rounded_lg()
            .border_1()
            .border_color(theme.border)
            .bg(theme.background)
            .drag_over::<ExternalPaths>(|style, _, _, cx| {
                style
                    .border_color(cx.theme().primary)
                    .bg(cx.theme().primary.opacity(0.06))
            })
            .on_drop(cx.listener(|this, paths: &ExternalPaths, window, cx| {
                let paths = paths.paths().to_vec();
                this.attach_files(&paths, window, cx);
            }))
            .children(self.render_attachments(cx))
            .child(
                h_flex()
                    .w_full()
                    .gap_2()
                    .items_end()
                    .child(div().flex_1().min_w_0().child(textarea))
                    .child(action),
            )
    }

    fn render_welcome(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        let has_model = self.conversation.model().is_some();
        let ready_providers: Vec<String> = self
            .engine
            .providers()
            .into_iter()
            .filter(|p| p.is_ready() && p.needs_api_key())
            .map(|p| p.label)
            .collect();
        let hint: SharedString = if has_model {
            "Describe a change, ask about the code, or paste an error. Drop or paste images to attach them.".into()
        } else if ready_providers.is_empty() {
            "Log in (Claude Pro/Max, ChatGPT) or add an API key, or start Ollama, then pick a model above.".into()
        } else {
            format!(
                "Pick a model above ({} configured).",
                ready_providers.join(", ")
            )
            .into()
        };
        let theme_name = ThemeState::global(cx)
            .map(|s| s.applied.clone())
            .unwrap_or_default();
        v_flex()
            .size_full()
            .items_center()
            .justify_center()
            .gap_3()
            .child(div().text_3xl().font_weight(FontWeight::BOLD).child("qt"))
            .child(
                div()
                    .text_sm()
                    .text_color(theme.muted_foreground)
                    .child(self.engine.cwd().display().to_string()),
            )
            .when(!self.quiet_startup || !has_model, |el| {
                el.child(div().text_sm().child(hint))
            })
            .when(!self.quiet_startup && !theme_name.is_empty(), |el| {
                el.child(
                    div()
                        .text_xs()
                        .text_color(theme.muted_foreground)
                        .child(format!("Theme: {theme_name}")),
                )
            })
            .when(!has_model && ready_providers.is_empty(), |el| {
                el.child(
                    Button::new("welcome-login")
                        .label("Log in or add an API key")
                        .primary()
                        .on_click(cx.listener(|this, _, window, cx| {
                            this.open_login(&OpenLogin, window, cx)
                        })),
                )
            })
    }
}

impl Render for Workspace {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        let empty = self.transcript.read(cx).is_empty();
        h_flex()
            .key_context(CONTEXT)
            .track_focus(&self.focus)
            .on_action(cx.listener(Self::new_session))
            .on_action(cx.listener(Self::stop))
            .on_action(cx.listener(Self::toggle_sidebar))
            .on_action(cx.listener(Self::toggle_theme))
            .on_action(cx.listener(Self::open_login))
            .on_action(cx.listener(Self::open_settings))
            .on_action(cx.listener(Self::open_ollama))
            .on_action(cx.listener(Self::open_tree))
            .on_action(cx.listener(Self::open_resume))
            .on_action(cx.listener(Self::fork_session))
            .on_action(cx.listener(Self::clone_session))
            .on_action(cx.listener(Self::rename_session))
            .on_action(cx.listener(Self::compact_context))
            .on_action(cx.listener(Self::open_goal))
            .on_action(cx.listener(Self::queue_follow_up))
            .on_action(cx.listener(Self::dequeue))
            .on_action(cx.listener(Self::external_editor))
            .on_action(cx.listener(Self::refresh_models_action))
            .on_action(cx.listener(Self::open_palette))
            .on_action(cx.listener(Self::open_tools_action))
            .on_action(cx.listener(Self::open_context_action))
            .on_action(cx.listener(Self::export_action))
            .capture_key_down(cx.listener(Self::extension_shortcut))
            .size_full()
            .bg(theme.background)
            .text_color(theme.foreground)
            .when(self.sidebar_open, |el| el.child(self.render_sidebar(cx)))
            .child(
                v_flex()
                    .flex_1()
                    .min_w_0()
                    .h_full()
                    .child(self.render_header(cx))
                    .children(self.ext.read(cx).render_header(cx))
                    .child(div().flex_1().min_h_0().map(|el| {
                        if empty {
                            el.child(self.render_welcome(cx))
                        } else {
                            el.child(self.transcript.clone())
                        }
                    }))
                    .child(self.render_composer(cx)),
            )
    }
}

/// Replace the models of every provider that answered in `results`; models
/// of providers that are absent or failed stay as they were.
fn merge_listing(
    models: &mut Vec<ModelInfo>,
    results: &[(String, Result<Vec<ModelInfo>, String>)],
) {
    for (provider, result) in results {
        if let Ok(list) = result {
            models.retain(|m| &m.provider != provider);
            models.extend(list.iter().cloned());
        }
    }
}

fn tokens(n: u64) -> String {
    if n >= 1_000_000 {
        format!("{:.1}M", n as f64 / 1e6)
    } else if n >= 1_000 {
        format!("{:.1}k", n as f64 / 1e3)
    } else {
        n.to_string()
    }
}

#[cfg(test)]
mod tests {
    // Not `super::*`: the gpui glob import would shadow `#[test]`.
    use super::{merge_listing, tokens};
    use qt_core::ModelInfo;

    fn model(provider: &str, id: &str) -> ModelInfo {
        ModelInfo {
            provider: provider.into(),
            id: id.into(),
            ..Default::default()
        }
    }

    #[test]
    fn partial_listings_keep_other_providers() {
        let mut models = vec![
            model("ollama:local", "qwen"),
            model("openai", "gpt-old"),
            model("openrouter", "x"),
        ];
        let results = vec![
            ("openai".to_string(), Ok(vec![model("openai", "gpt-new")])),
            ("openrouter".to_string(), Err("offline".to_string())),
        ];
        merge_listing(&mut models, &results);
        let ids: Vec<&str> = models.iter().map(|m| m.id.as_str()).collect();
        assert_eq!(ids, ["qwen", "x", "gpt-new"]);
    }

    #[test]
    fn formats_tokens() {
        assert_eq!(tokens(999), "999");
        assert_eq!(tokens(12_345), "12.3k");
        assert_eq!(tokens(2_500_000), "2.5M");
    }
}
