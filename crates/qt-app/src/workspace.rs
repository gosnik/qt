//! Root view: session sidebar, header with model controls, transcript and composer.

use std::path::PathBuf;
use std::time::SystemTime;

use gpui_kit::component::button::{Button, ButtonVariants as _};
use gpui_kit::component::input::{InputEvent, Textarea, TextareaState};
use gpui_kit::component::notification::Notification;
use gpui_kit::component::searchable_list::SearchableVec;
use gpui_kit::component::select::{Select, SelectEvent, SelectState};
use gpui_kit::component::{
    ActiveTheme as _, IconName, IndexPath, Sizable as _, Theme, ThemeMode, WindowExt as _, h_flex,
    v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::*;
use qt_core::provider::{ModelInfo, ModelRef, ThinkingLevel};
use qt_core::session::SessionSummary;
use qt_core::{AgentEvent, Conversation, Engine};

use crate::keys_dialog::KeyInputs;
use crate::model_picker::{ModelItem, ThinkingItem};
use crate::transcript::Transcript;

actions!(
    qt,
    [
        NewSession,
        StopAgent,
        ToggleSidebar,
        ToggleTheme,
        ManageKeys
    ]
);

const CONTEXT: &str = "QtWorkspace";

pub fn bind_keys(cx: &mut App) {
    let (new_session, toggle_sidebar) = if cfg!(target_os = "macos") {
        ("cmd-n", "cmd-b")
    } else {
        ("ctrl-n", "ctrl-b")
    };
    cx.bind_keys([
        KeyBinding::new(new_session, NewSession, Some(CONTEXT)),
        KeyBinding::new("escape", StopAgent, Some(CONTEXT)),
        KeyBinding::new(toggle_sidebar, ToggleSidebar, Some(CONTEXT)),
    ]);
}

type ModelSelect = SelectState<SearchableVec<ModelItem>>;
type ThinkingSelect = SelectState<Vec<ThinkingItem>>;

pub struct Workspace {
    engine: Engine,
    conversation: Conversation,
    transcript: Entity<Transcript>,
    composer: Entity<TextareaState>,
    model_select: Entity<ModelSelect>,
    thinking_select: Entity<ThinkingSelect>,
    key_inputs: KeyInputs,
    models: Vec<ModelInfo>,
    models_loading: bool,
    sessions: Vec<SessionSummary>,
    queued: Vec<String>,
    running: bool,
    sidebar_open: bool,
    focus: FocusHandle,
    _event_task: Option<Task<()>>,
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
                .placeholder("Ask qt to read, change or explain code…  (Enter to send, Shift+Enter for a new line)")
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
        let key_inputs = KeyInputs::new(window, cx);

        let subscriptions = vec![
            cx.subscribe_in(
                &composer,
                window,
                |this, _, event: &InputEvent, window, cx| {
                    if let InputEvent::PressEnter { shift: false, .. } = event {
                        this.submit(window, cx);
                    }
                },
            ),
            cx.subscribe_in(
                &model_select,
                window,
                |this, _, event: &SelectEvent<SearchableVec<ModelItem>>, window, cx| {
                    let SelectEvent::Confirm(Some(value)) = event else {
                        return;
                    };
                    if let Some(model) = ModelRef::parse(value) {
                        this.conversation.set_model(model.clone());
                        if let Err(err) = this.engine.set_default_model(&model) {
                            window.push_notification(
                                Notification::error(format!(
                                    "Could not save default model: {err:#}"
                                )),
                                cx,
                            );
                        }
                        cx.notify();
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
        ];

        let mut this = Self {
            sessions: engine.sessions(),
            engine,
            conversation,
            transcript,
            composer,
            model_select,
            thinking_select,
            key_inputs,
            models: Vec::new(),
            models_loading: false,
            queued: Vec::new(),
            running: false,
            sidebar_open: true,
            focus: cx.focus_handle(),
            _event_task: None,
            _subscriptions: subscriptions,
        };
        this.refresh_models(window, cx);
        this.composer
            .update(cx, |state, cx| state.focus(window, cx));
        this
    }

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

    fn set_models(
        &mut self,
        results: Vec<(String, Result<Vec<ModelInfo>, String>)>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.models_loading = false;
        let providers = self.engine.providers();
        let label = |id: &str| {
            providers
                .iter()
                .find(|p| p.id == id)
                .map(|p| p.label.clone())
                .unwrap_or_else(|| id.to_string())
        };
        let mut models = Vec::new();
        for (provider, result) in results {
            match result {
                Ok(list) => models.extend(list),
                // Ollama is probed even when not configured; stay quiet if it is not running.
                Err(_) if provider.starts_with("ollama") => {}
                Err(err) => window.push_notification(
                    Notification::error(format!("{}: {err}", label(&provider))),
                    cx,
                ),
            }
        }
        let mut items: Vec<ModelItem> = models
            .iter()
            .map(|m| ModelItem::from_info(m, &label(&m.provider)))
            .collect();
        let current = self.conversation.model();
        if let Some(model) = &current
            && !items.iter().any(|i| i.value.as_ref() == model.to_string())
        {
            items.insert(0, ModelItem::from_ref(model));
        }
        let selected = current.map(|m| m.to_string());
        self.models = models;
        self.model_select.update(cx, |state, cx| {
            state.set_items(SearchableVec::new(items), window, cx);
            if let Some(value) = selected {
                state.set_selected_value(&SharedString::from(value), window, cx);
            }
        });
        cx.notify();
    }

    fn sync_model_select(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let Some(model) = self.conversation.model() else {
            return;
        };
        let value = SharedString::from(model.to_string());
        let known = self.models.iter().any(|m| m.model_ref() == model);
        if !known {
            // Keep a stored model visible even if its provider is not listing it.
            let providers = self.engine.providers();
            let label = |id: &str| {
                providers
                    .iter()
                    .find(|p| p.id == id)
                    .map(|p| p.label.clone())
                    .unwrap_or_else(|| id.to_string())
            };
            let mut items: Vec<ModelItem> = self
                .models
                .iter()
                .map(|m| ModelItem::from_info(m, &label(&m.provider)))
                .collect();
            items.insert(0, ModelItem::from_ref(&model));
            self.model_select.update(cx, |s, cx| {
                s.set_items(SearchableVec::new(items), window, cx)
            });
        }
        self.model_select
            .update(cx, |s, cx| s.set_selected_value(&value, window, cx));
    }

    fn submit(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let text = self.composer.read(cx).value().to_string();
        if text.trim().is_empty() {
            return;
        }
        if self.running {
            // Enter while the agent runs steers it after the current tool calls.
            if let Err(err) = self.conversation.steer(&text, Vec::new()) {
                window.push_notification(Notification::error(format!("{err:#}")), cx);
                return;
            }
        } else if !self.start(text, window, cx) {
            return;
        }
        self.composer
            .update(cx, |state, cx| state.set_value("", window, cx));
        cx.notify();
    }

    /// Start the agent with `text`; returns false (and keeps the draft) on failure.
    fn start(&mut self, text: String, window: &mut Window, cx: &mut Context<Self>) -> bool {
        let rx = match self.conversation.prompt(&text, Vec::new()) {
            Ok(rx) => rx,
            Err(err) => {
                let message = format!("{err:#}");
                let hint = if message.contains("No model") || message.contains("API key") {
                    " — choose a model or add an API key (key icon in the sidebar)."
                } else {
                    ""
                };
                window.push_notification(Notification::error(format!("{message}{hint}")), cx);
                return false;
            }
        };
        self.running = true;
        self._event_task = Some(cx.spawn_in(window, async move |this, cx| {
            while let Ok(event) = rx.recv().await {
                if this
                    .update_in(cx, |this, window, cx| this.on_event(event, window, cx))
                    .is_err()
                {
                    break;
                }
            }
        }));
        cx.notify();
        true
    }

    fn on_event(&mut self, event: AgentEvent, window: &mut Window, cx: &mut Context<Self>) {
        self.transcript
            .update(cx, |t, cx| t.apply_event(&event, cx));
        match event {
            AgentEvent::Warning(message) => {
                window.push_notification(Notification::warning(message), cx)
            }
            AgentEvent::QueueUpdate {
                steering,
                follow_up,
            } => {
                self.queued = steering.into_iter().chain(follow_up).collect();
                cx.notify();
            }
            AgentEvent::CompactionEnd {
                error: Some(error), ..
            }
            | AgentEvent::RetryEnd {
                success: false,
                final_error: Some(error),
                ..
            } => window.push_notification(Notification::warning(error), cx),
            AgentEvent::AgentEnd { aborted, .. } => {
                self.running = false;
                self.sessions = self.engine.sessions();
                if aborted {
                    self.conversation.clear_queue();
                }
                let queue = self.conversation.queue();
                self.queued = queue.steering.into_iter().chain(queue.follow_up).collect();
                cx.notify();
            }
            _ => {}
        }
    }

    fn stop(&mut self, _: &StopAgent, _window: &mut Window, cx: &mut Context<Self>) {
        if self.running {
            self.conversation.abort();
            cx.notify();
        }
    }

    fn new_session(&mut self, _: &NewSession, window: &mut Window, cx: &mut Context<Self>) {
        self.switch_to(
            self.engine.new_conversation(self.conversation.model()),
            window,
            cx,
        );
    }

    fn open_session(&mut self, path: PathBuf, window: &mut Window, cx: &mut Context<Self>) {
        if path == self.conversation.session_path() {
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
        if self.running {
            self.conversation.abort();
        }
        self._event_task = None;
        self.running = false;
        self.queued.clear();
        conversation.set_thinking(self.conversation.thinking());
        self.conversation = conversation;
        let messages = self.conversation.messages();
        self.transcript.update(cx, |t, cx| t.load(&messages, cx));
        self.sync_model_select(window, cx);
        self.sessions = self.engine.sessions();
        self.composer
            .update(cx, |state, cx| state.focus(window, cx));
        cx.notify();
    }

    fn toggle_sidebar(&mut self, _: &ToggleSidebar, _: &mut Window, cx: &mut Context<Self>) {
        self.sidebar_open = !self.sidebar_open;
        cx.notify();
    }

    fn toggle_theme(&mut self, _: &ToggleTheme, window: &mut Window, cx: &mut Context<Self>) {
        let dark = !cx.theme().is_dark();
        Theme::change(
            if dark {
                ThemeMode::Dark
            } else {
                ThemeMode::Light
            },
            Some(window),
            cx,
        );
        if let Err(err) = self
            .engine
            .update_settings(|s| s.set_theme(if dark { "dark" } else { "light" }))
        {
            window.push_notification(
                Notification::error(format!("Could not save theme: {err:#}")),
                cx,
            );
        }
        cx.refresh_windows();
    }

    fn manage_keys(&mut self, _: &ManageKeys, window: &mut Window, cx: &mut Context<Self>) {
        let this = cx.entity().downgrade();
        self.key_inputs.open(
            &self.engine,
            move |window, cx| {
                let _ = this.update(cx, |this, cx| {
                    if this.conversation.model().is_none()
                        && let Some(model) = this.engine.default_model()
                    {
                        this.conversation.set_model(model);
                    }
                    this.refresh_models(window, cx);
                });
            },
            window,
            cx,
        );
    }

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
                    .gap_2()
                    .items_center()
                    .child(
                        div()
                            .flex_1()
                            .text_sm()
                            .font_weight(FontWeight::SEMIBOLD)
                            .child("Sessions"),
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
                        Button::new("keys")
                            .icon(IconName::Settings)
                            .label("API keys")
                            .ghost()
                            .small()
                            .on_click(cx.listener(|this, _, window, cx| {
                                this.manage_keys(&ManageKeys, window, cx)
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
                div()
                    .flex_1()
                    .min_w_0()
                    .overflow_hidden()
                    .whitespace_nowrap()
                    .text_ellipsis()
                    .text_sm()
                    .text_color(theme.muted_foreground)
                    .child(cwd),
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
    }

    fn render_status(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        let usage = self.conversation.usage();
        let context = self.conversation.context_tokens();
        let window_size = self
            .conversation
            .model()
            .and_then(|m| self.models.iter().find(|i| i.model_ref() == m))
            .and_then(|m| m.context_window);
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
        if !self.queued.is_empty() {
            parts.push(format!("{} queued", self.queued.len()));
        }
        h_flex()
            .w_full()
            .gap_3()
            .text_xs()
            .text_color(theme.muted_foreground)
            .child(div().flex_1().child(parts.join(" · ")))
            .child(if self.running {
                "Esc to stop"
            } else {
                "Enter to send · Shift+Enter for newline"
            })
    }

    fn render_composer(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        let action = if self.running {
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
                .on_click(cx.listener(|this, _, window, cx| this.submit(window, cx)))
        };
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
                    .child(
                        h_flex()
                            .w_full()
                            .gap_2()
                            .items_end()
                            .p_2()
                            .rounded_lg()
                            .border_1()
                            .border_color(theme.border)
                            .bg(theme.background)
                            .child(
                                div()
                                    .flex_1()
                                    .min_w_0()
                                    .child(Textarea::new(&self.composer).appearance(false)),
                            )
                            .child(action),
                    )
                    .child(self.render_status(cx)),
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
            "Describe a change, ask about the code, or paste an error.".into()
        } else if ready_providers.is_empty() {
            "Add an API key (Anthropic, OpenAI or OpenRouter) or start Ollama, then pick a model above.".into()
        } else {
            format!(
                "Pick a model above ({} configured).",
                ready_providers.join(", ")
            )
            .into()
        };
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
            .child(div().text_sm().child(hint))
            .when(!has_model && ready_providers.is_empty(), |el| {
                el.child(
                    Button::new("welcome-keys")
                        .label("Add API key")
                        .primary()
                        .on_click(cx.listener(|this, _, window, cx| {
                            this.manage_keys(&ManageKeys, window, cx)
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
            .on_action(cx.listener(Self::manage_keys))
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

fn tokens(n: u64) -> String {
    if n >= 1_000_000 {
        format!("{:.1}M", n as f64 / 1e6)
    } else if n >= 1_000 {
        format!("{:.1}k", n as f64 / 1e3)
    } else {
        n.to_string()
    }
}

fn relative_time(time: SystemTime) -> String {
    let secs = SystemTime::now()
        .duration_since(time)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    match secs {
        0..=59 => "just now".into(),
        60..=3_599 => format!("{} min ago", secs / 60),
        3_600..=86_399 => format!("{} h ago", secs / 3_600),
        86_400..=604_799 => format!("{} d ago", secs / 86_400),
        _ => chrono::DateTime::<chrono::Local>::from(time)
            .format("%Y-%m-%d")
            .to_string(),
    }
}

#[cfg(test)]
mod tests {
    // Not `super::*`: the gpui glob import would shadow `#[test]`.
    use super::tokens;

    #[test]
    fn formats_tokens() {
        assert_eq!(tokens(999), "999");
        assert_eq!(tokens(12_345), "12.3k");
        assert_eq!(tokens(2_500_000), "2.5M");
    }
}
