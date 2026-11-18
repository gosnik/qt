//! [`ExtensionChrome`]: the window side of the extension UI. It owns the
//! state extensions set (status entries, widgets, header, footer, working
//! indicator, editor component, autocomplete providers, shortcuts), answers
//! [`UiRequest`]s, renders the chrome slots the workspace places around the
//! transcript and composer, runs extension commands and shortcuts off the
//! UI thread and forwards runs started by extensions or subagent
//! completions to the workspace.

use std::collections::{BTreeMap, HashMap};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use async_channel::Receiver;
use gpui_kit::component::input::{InputEvent, TextareaState};
use gpui_kit::component::notification::Notification;
use gpui_kit::component::spinner::Spinner;
use gpui_kit::component::{ActiveTheme as _, Sizable as _, WindowExt as _, h_flex, v_flex};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::*;
use qt_core::AgentEvent;
use qt_runtime::Runtime;
use qt_runtime::qt_extensions::ui::{AutocompleteSuggestions, NotifyKind, merge_autocomplete};

use super::bridge::{GpuiExtensionUi, Mirror, UiRequest, status_line};
use super::dialogs::{
    ComponentView, DIALOG_POLL, EditorComponentView, ExtDialog, open_component, open_dialog,
    pressed,
};
use super::keys::{Shortcut, parse_shortcut, shortcut_matches};
use crate::settings_panel;
use crate::theme::{self, ThemeState};
use crate::transcript::Transcript;

/// What the workspace should do on behalf of extensions and panels.
pub enum ChromeEvent {
    /// Show command output as a transcript card.
    Card {
        title: String,
        lines: Vec<String>,
        error: bool,
        /// For extension commands: the conversation's message count before
        /// the command ran (messages it appended are shown after the card).
        messages_before: Option<usize>,
    },
    /// Pump a run started outside the composer (extension prompts,
    /// triggered custom messages, subagent completions).
    Run(Receiver<AgentEvent>),
}

impl EventEmitter<ChromeEvent> for ExtensionChrome {}

/// Default spinner frames when an extension sets none.
const WORKING_TEXT: &str = "Working…";

pub struct ExtensionChrome {
    runtime: Runtime,
    composer: Entity<TextareaState>,
    transcript: Entity<Transcript>,
    mirror: Arc<Mutex<Mirror>>,
    requests: async_channel::Sender<UiRequest>,
    statuses: BTreeMap<String, String>,
    widgets: BTreeMap<String, Vec<String>>,
    header: Option<Vec<String>>,
    footer: Option<Vec<String>>,
    working_message: Option<String>,
    working_visible: bool,
    frames: Option<Vec<String>>,
    frame_interval: Duration,
    frame: usize,
    dialogs: Vec<WeakEntity<ExtDialog>>,
    components: HashMap<u64, WeakEntity<ComponentView>>,
    editor_component: Option<Entity<EditorComponentView>>,
    shortcuts: Vec<(Shortcut, String)>,
    _tasks: Vec<Task<()>>,
    _animation: Option<Task<()>>,
    _subscriptions: Vec<Subscription>,
}

impl ExtensionChrome {
    /// Register the GPUI extension UI and run sink with the runtime.
    pub fn install(
        runtime: Runtime,
        composer: Entity<TextareaState>,
        transcript: Entity<Transcript>,
        window: &mut Window,
        cx: &mut App,
    ) -> Entity<Self> {
        cx.new(|cx| Self::new(runtime, composer, transcript, window, cx))
    }

    fn new(
        runtime: Runtime,
        composer: Entity<TextareaState>,
        transcript: Entity<Transcript>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let (tx, rx) = async_channel::unbounded::<UiRequest>();
        let mirror = Arc::new(Mutex::new(Mirror {
            editor_text: composer.read(cx).value().to_string(),
            ..Default::default()
        }));
        runtime.set_extension_ui(Arc::new(GpuiExtensionUi::new(
            tx.clone(),
            mirror.clone(),
            std::thread::current().id(),
        )));
        let (run_tx, run_rx) = async_channel::unbounded::<Receiver<AgentEvent>>();
        runtime.set_run_sink(Some(Arc::new(move |events| {
            let _ = run_tx.try_send(events);
        })));

        let requests = cx.spawn_in(window, async move |this, cx| {
            while let Ok(request) = rx.recv().await {
                if this
                    .update_in(cx, |this, window, cx| this.handle(request, window, cx))
                    .is_err()
                {
                    break;
                }
            }
        });
        let runs = cx.spawn(async move |this, cx| {
            while let Ok(events) = run_rx.recv().await {
                if this
                    .update(cx, |_, cx| cx.emit(ChromeEvent::Run(events)))
                    .is_err()
                {
                    break;
                }
            }
        });
        let poll = cx.spawn_in(window, async move |this, cx| {
            loop {
                cx.background_executor().timer(DIALOG_POLL).await;
                if this
                    .update_in(cx, |this, window, cx| this.poll_dialogs(window, cx))
                    .is_err()
                {
                    break;
                }
            }
        });
        let mirror_text = mirror.clone();
        let subscriptions =
            vec![
                cx.subscribe(&composer, move |_, composer, event: &InputEvent, cx| {
                    if matches!(event, InputEvent::Change) {
                        let text = composer.read(cx).value().to_string();
                        mirror_text
                            .lock()
                            .unwrap_or_else(|e| e.into_inner())
                            .editor_text = text;
                    }
                }),
            ];

        let renderer_runtime = runtime.clone();
        transcript.update(cx, |t, _| {
            t.set_custom_renderer(Some(std::rc::Rc::new(
                move |message| match renderer_runtime
                    .extensions()
                    .render_message(message, 100, false)?
                {
                    Ok(lines) if !lines.is_empty() => Some(lines),
                    _ => None,
                },
            )));
        });

        let mut this = Self {
            runtime,
            composer,
            transcript,
            mirror,
            requests: tx,
            statuses: BTreeMap::new(),
            widgets: BTreeMap::new(),
            header: None,
            footer: None,
            working_message: None,
            working_visible: true,
            frames: None,
            frame_interval: Duration::from_millis(80),
            frame: 0,
            dialogs: Vec::new(),
            components: HashMap::new(),
            editor_component: None,
            shortcuts: Vec::new(),
            _tasks: vec![requests, runs, poll],
            _animation: None,
            _subscriptions: subscriptions,
        };
        this.sync_themes(cx);
        this.load_shortcuts();
        this
    }

    fn mirror(&self) -> std::sync::MutexGuard<'_, Mirror> {
        self.mirror.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn sync_themes(&mut self, cx: &App) {
        let names = ThemeState::available(cx);
        let applied = ThemeState::global(cx)
            .map(|s| s.applied.clone())
            .unwrap_or_default();
        let mut mirror = self.mirror();
        mirror.themes = names;
        mirror.applied_theme = applied;
        mirror.tools_expanded = self.transcript.read(cx).tools_expanded();
    }

    fn load_shortcuts(&mut self) {
        self.shortcuts = self
            .runtime
            .extensions()
            .shortcuts()
            .into_iter()
            .filter_map(|s| parse_shortcut(&s.key).map(|parsed| (parsed, s.key)))
            .collect();
    }

    fn notify_window(message: String, kind: NotifyKind, window: &mut Window, cx: &mut App) {
        let note = match kind {
            NotifyKind::Info => Notification::info(message),
            NotifyKind::Warning => Notification::warning(message),
            NotifyKind::Error => Notification::error(message),
            NotifyKind::Success => Notification::success(message),
        };
        window.push_notification(note, cx);
    }

    fn handle(&mut self, request: UiRequest, window: &mut Window, cx: &mut Context<Self>) {
        match request {
            UiRequest::Dialog(dialog) => {
                if dialog.expired.load(std::sync::atomic::Ordering::SeqCst) {
                    return;
                }
                let view = open_dialog(dialog, window, cx);
                self.dialogs.push(view.downgrade());
            }
            UiRequest::Notify { message, kind } => Self::notify_window(message, kind, window, cx),
            UiRequest::Status { key, text } => {
                match text {
                    Some(text) => self.statuses.insert(key, text),
                    None => self.statuses.remove(&key),
                };
            }
            UiRequest::WorkingMessage(message) => self.working_message = message,
            UiRequest::WorkingVisible(visible) => self.working_visible = visible,
            UiRequest::WorkingIndicator {
                frames,
                interval_ms,
            } => {
                self.frames = frames.filter(|f| !f.is_empty());
                self.frame_interval = Duration::from_millis(interval_ms.max(16));
                self.frame = 0;
                self.restart_animation(window, cx);
            }
            UiRequest::HiddenThinkingLabel(label) => self
                .transcript
                .update(cx, |t, cx| t.set_hidden_thinking_label(label, cx)),
            UiRequest::Widget { key, lines } => {
                match lines {
                    Some(lines) => self.widgets.insert(key, lines),
                    None => self.widgets.remove(&key),
                };
            }
            UiRequest::Header(lines) => self.header = lines,
            UiRequest::Footer(lines) => self.footer = lines,
            UiRequest::Title(title) => window.set_window_title(&title),
            UiRequest::ToolsExpanded(expanded) => self
                .transcript
                .update(cx, |t, cx| t.set_tools_expanded(expanded, cx)),
            UiRequest::SetEditorText(text) => {
                let end = text.len();
                self.composer.update(cx, |state, cx| {
                    state.set_value(text, window, cx);
                    state.set_selected_range(end..end, cx);
                    state.focus(window, cx);
                });
            }
            UiRequest::PasteToEditor(text) => {
                let (value, cursor) = {
                    let state = self.composer.read(cx);
                    (state.value().to_string(), state.cursor())
                };
                let (next, cursor) = splice(&value, cursor, &text);
                self.mirror().editor_text = next.clone();
                self.composer.update(cx, |state, cx| {
                    state.set_value(next, window, cx);
                    state.set_selected_range(cursor..cursor, cx);
                    state.focus(window, cx);
                });
            }
            UiRequest::SetTheme(name) => {
                if let Err(err) = self
                    .runtime
                    .engine()
                    .update_settings(|s| settings_panel::set_theme_setting(s, Some(&name)))
                {
                    window.push_notification(
                        Notification::error(format!("Could not save theme: {err:#}")),
                        cx,
                    );
                }
                theme::apply_selection(Some(&name), cx);
                self.sync_themes(cx);
            }
            UiRequest::ComponentOpen { session, lines } => {
                let id = session.id;
                let view = open_component(id, session.title, lines, session.events, window, cx);
                self.components.insert(id, view.downgrade());
            }
            UiRequest::ComponentUpdate { id, lines } => {
                if let Some(view) = self.components.get(&id).and_then(WeakEntity::upgrade) {
                    view.update(cx, |v, cx| v.set_lines(lines, cx));
                }
            }
            UiRequest::ComponentClose { id } => {
                if let Some(view) = self.components.remove(&id).and_then(|w| w.upgrade()) {
                    view.update(cx, |v, _| v.mark_closed());
                    window.close_dialog(cx);
                }
            }
            UiRequest::EditorComponent(component) => {
                self.editor_component = component.map(|c| {
                    let view = cx.new(|cx| EditorComponentView::new(c, cx));
                    let focus = view.read(cx).focus_handle();
                    window.focus(&focus, cx);
                    cx.observe(&view, |this, view, cx| {
                        if view.read(cx).is_done() {
                            this.editor_component = None;
                            this.mirror().editor_component = None;
                        }
                        cx.notify();
                    })
                    .detach();
                    view
                });
                if self.editor_component.is_none() {
                    self.composer
                        .update(cx, |state, cx| state.focus(window, cx));
                }
            }
        }
        cx.notify();
    }

    fn restart_animation(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.frames.is_none() {
            self._animation = None;
            return;
        }
        let interval = self.frame_interval;
        self._animation = Some(cx.spawn_in(window, async move |this, cx| {
            loop {
                cx.background_executor().timer(interval).await;
                let alive = this
                    .update(cx, |this, cx| {
                        this.frame = this.frame.wrapping_add(1);
                        cx.notify();
                        this.frames.is_some()
                    })
                    .unwrap_or(false);
                if !alive {
                    break;
                }
            }
        }));
    }

    /// Close dialogs whose extension stopped waiting (timeouts); answered
    /// dialogs closed themselves.
    fn poll_dialogs(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let mut expired = Vec::new();
        self.dialogs.retain(|weak| match weak.upgrade() {
            Some(view) if view.read(cx).answered() => false,
            Some(view) => {
                if view.read(cx).expired() {
                    expired.push(view);
                    false
                } else {
                    view.update(cx, |_, cx| cx.notify());
                    true
                }
            }
            None => false,
        });
        for view in expired {
            view.update(cx, |v, _| v.expire());
            if window.has_active_dialog(cx) {
                window.close_dialog(cx);
            }
        }
        self.components.retain(|_, w| w.upgrade().is_some());
    }

    // ----- workspace hooks ------------------------------------------------------

    /// Run extension command `/name args` off the UI thread (it may open
    /// dialogs); its output becomes a card.
    pub fn run_command(
        &mut self,
        name: String,
        arguments: String,
        messages_before: usize,
        cx: &mut Context<Self>,
    ) {
        let runtime = self.runtime.clone();
        let (tx, rx) = async_channel::bounded(1);
        let thread_name = name.clone();
        let spawned = std::thread::Builder::new()
            .name(format!("ext-command-{thread_name}"))
            .spawn(move || {
                let _ = tx.send_blocking(runtime.run_extension_command(&name, &arguments));
            });
        let title = format!("/{thread_name}");
        if let Err(e) = spawned {
            cx.emit(ChromeEvent::Card {
                title,
                lines: vec![format!("could not start the extension command: {e}")],
                error: true,
                messages_before: None,
            });
            return;
        }
        cx.spawn(async move |this, cx| {
            let result = rx.recv().await;
            let _ = this.update(cx, |_, cx| {
                let (lines, error) = match result {
                    Ok(Ok(lines)) => (lines, false),
                    Ok(Err(err)) => (vec![format!("{err:#}")], true),
                    Err(_) => (
                        vec!["extension command ended without a result".into()],
                        true,
                    ),
                };
                cx.emit(ChromeEvent::Card {
                    title,
                    lines,
                    error,
                    messages_before: Some(messages_before),
                });
            });
        })
        .detach();
    }

    /// Run a registered shortcut for `keystroke`; `true` when one matched.
    pub fn handle_keystroke(&mut self, keystroke: &Keystroke) -> bool {
        let pressed = pressed(keystroke);
        let Some(key) = self
            .shortcuts
            .iter()
            .find(|(s, _)| shortcut_matches(s, &pressed))
            .map(|(_, k)| k.clone())
        else {
            return false;
        };
        let runtime = self.runtime.clone();
        let requests = self.requests.clone();
        let _ = std::thread::Builder::new()
            .name("ext-shortcut".into())
            .spawn(move || {
                if let Err(e) = runtime.extensions().run_shortcut(&key) {
                    let _ = requests.try_send(UiRequest::Notify {
                        message: format!("Extension shortcut {key} failed: {e}"),
                        kind: NotifyKind::Error,
                    });
                }
            });
        true
    }

    /// Suggestions of extension autocomplete providers for the composer
    /// text and cursor (byte offset), merged by prefix.
    pub fn autocomplete(&self, text: &str, cursor: usize) -> Option<AutocompleteSuggestions> {
        let providers = self.mirror().autocomplete.clone();
        if providers.is_empty() {
            return None;
        }
        let (lines, row, column) = editor_position(text, cursor);
        let results: Vec<AutocompleteSuggestions> = providers
            .iter()
            .filter_map(|p| p.complete(&lines, row, column, false))
            .filter(|s| !s.items.is_empty())
            .collect();
        merge_autocomplete(results)
    }

    /// `/reload`: reload extensions and resources, then drop the UI state
    /// the old extension set registered.
    pub fn reload(&mut self, cx: &mut Context<Self>) -> Vec<String> {
        let mut lines = vec!["Reloaded extensions and runtime resources.".to_string()];
        lines.extend(self.runtime.reload());
        self.reset();
        self.load_shortcuts();
        self.sync_themes(cx);
        cx.notify();
        lines
    }

    fn reset(&mut self) {
        self.statuses.clear();
        self.widgets.clear();
        self.header = None;
        self.footer = None;
        self.working_message = None;
        self.working_visible = true;
        self.frames = None;
        self._animation = None;
        self.editor_component = None;
        let mut mirror = self.mirror();
        mirror.autocomplete.clear();
        mirror.editor_component = None;
    }

    /// Emit a card (panels report through the chrome).
    pub fn card(&mut self, title: String, lines: Vec<String>, error: bool, cx: &mut Context<Self>) {
        cx.emit(ChromeEvent::Card {
            title,
            lines,
            error,
            messages_before: None,
        });
    }

    // ----- rendering slots --------------------------------------------------------

    /// Extension header lines, shown under the header bar.
    pub fn render_header(&self, cx: &App) -> Option<AnyElement> {
        let lines = self.header.as_ref().filter(|l| !l.is_empty())?;
        Some(
            div()
                .w_full()
                .px_3()
                .pt_2()
                .child(line_block(lines, cx))
                .into_any_element(),
        )
    }

    /// Working indicator, widgets and the editor component, shown above the
    /// composer.
    pub fn render_above_composer(&self, running: bool, cx: &App) -> Option<AnyElement> {
        let theme = cx.theme();
        let working = running && self.working_visible;
        if !working && self.widgets.is_empty() && self.editor_component.is_none() {
            return None;
        }
        let mut col = v_flex().w_full().gap_1();
        if working {
            let text = self
                .working_message
                .clone()
                .unwrap_or_else(|| WORKING_TEXT.into());
            let indicator: AnyElement = match &self.frames {
                Some(frames) => div()
                    .font_family(theme.mono_font_family.clone())
                    .child(frames[self.frame % frames.len()].clone())
                    .into_any_element(),
                None => Spinner::new().small().into_any_element(),
            };
            col = col.child(
                h_flex()
                    .gap_2()
                    .items_center()
                    .text_xs()
                    .text_color(theme.muted_foreground)
                    .child(indicator)
                    .child(text),
            );
        }
        for lines in self.widgets.values() {
            col = col.child(line_block(lines, cx));
        }
        if let Some(editor) = &self.editor_component {
            col = col.child(editor.clone());
        }
        Some(col.into_any_element())
    }

    /// Status entries and footer lines, shown under the status bar.
    pub fn render_footer(&self, cx: &App) -> Option<AnyElement> {
        let status = status_line(&self.statuses);
        let footer = self.footer.as_ref().filter(|l| !l.is_empty());
        if status.is_empty() && footer.is_none() {
            return None;
        }
        let theme = cx.theme();
        Some(
            v_flex()
                .w_full()
                .gap_0p5()
                .text_xs()
                .text_color(theme.muted_foreground)
                .when(!status.is_empty(), |el| el.child(div().child(status)))
                .when_some(footer, |el, lines| {
                    el.children(
                        lines
                            .iter()
                            .map(|l| div().child(SharedString::from(l.clone()))),
                    )
                })
                .into_any_element(),
        )
    }
}

impl Render for ExtensionChrome {
    fn render(&mut self, _window: &mut Window, _cx: &mut Context<Self>) -> impl IntoElement {
        div()
    }
}

fn line_block(lines: &[String], cx: &App) -> Div {
    let theme = cx.theme();
    v_flex()
        .w_full()
        .px_2()
        .py_1()
        .rounded_md()
        .border_1()
        .border_color(theme.border)
        .bg(theme.muted.opacity(0.3))
        .text_xs()
        .font_family(theme.mono_font_family.clone())
        .children(lines.iter().map(|l| {
            div()
                .whitespace_nowrap()
                .child(SharedString::from(l.clone()))
        }))
}

/// Insert `text` at byte `cursor` of `value`; returns the text and the
/// cursor after the insertion.
pub fn splice(value: &str, cursor: usize, text: &str) -> (String, usize) {
    let mut cursor = cursor.min(value.len());
    while !value.is_char_boundary(cursor) {
        cursor -= 1;
    }
    let next = format!("{}{text}{}", &value[..cursor], &value[cursor..]);
    (next, cursor + text.len())
}

/// Editor lines and the cursor row / column (in characters) for
/// autocomplete providers.
pub fn editor_position(text: &str, cursor: usize) -> (Vec<String>, usize, usize) {
    let mut cursor = cursor.min(text.len());
    while !text.is_char_boundary(cursor) {
        cursor -= 1;
    }
    let before = &text[..cursor];
    let row = before.matches('\n').count();
    let line_start = before.rfind('\n').map(|i| i + 1).unwrap_or(0);
    let column = before[line_start..].chars().count();
    let lines = text.split('\n').map(str::to_string).collect();
    (lines, row, column)
}

/// The suggestion value inserted for an autocomplete item: replaces the
/// provider prefix before the cursor. Returns the start byte of the prefix.
pub fn prefix_start(text: &str, cursor: usize, prefix: &str) -> Option<usize> {
    let cursor = cursor.min(text.len());
    let start = cursor.checked_sub(prefix.len())?;
    (text.is_char_boundary(start) && &text[start..cursor] == prefix).then_some(start)
}

#[cfg(test)]
mod tests {
    use super::{editor_position, prefix_start, splice};

    #[test]
    fn splices_and_positions() {
        assert_eq!(splice("hello", 2, "XX"), ("heXXllo".to_string(), 4));
        assert_eq!(splice("hé", 2, "!"), ("h!é".to_string(), 2));
        assert_eq!(splice("ab", 99, "c"), ("abc".to_string(), 3));
        let (lines, row, col) = editor_position("one\ntwö x", 9);
        assert_eq!(lines, ["one", "twö x"]);
        assert_eq!((row, col), (1, 4));
        assert_eq!(prefix_start("say :sm", 7, ":sm"), Some(4));
        assert_eq!(prefix_start("say :sm", 7, "xx"), None);
        assert_eq!(prefix_start("a", 1, "abc"), None);
    }
}
