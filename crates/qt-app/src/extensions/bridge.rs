//! The thread-safe half of the GPUI `ExtensionUi`: extension threads send
//! [`UiRequest`]s to the window and block on replies for dialogs.
//!
//! Extensions call `ctx.ui.*` from their own threads (commands run off the
//! UI thread, hooks run on the engine's runtime). Dialog calls wait for the
//! user's answer or their timeout; a call made on the UI thread itself
//! cannot wait (the window could never answer), so it returns the
//! cancelled value and the window shows a notification instead.

use std::collections::BTreeMap;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;
use std::sync::{Arc, Mutex};
use std::thread::ThreadId;
use std::time::Duration;

use qt_runtime::qt_extensions::ExtensionUi;
use qt_runtime::qt_extensions::ui::{AutocompleteProvider, NotifyKind, ScriptComponent};
use serde_json::{Value, json};

/// A modal question for the user.
#[derive(Debug, Clone, PartialEq)]
pub enum DialogKind {
    Select { options: Vec<String> },
    Confirm { message: String },
    Input { placeholder: String },
    Editor { prefill: String },
}

/// The user's answer: `None` is cancellation (Escape, Close, timeout).
#[derive(Debug, Clone, PartialEq)]
pub enum DialogAnswer {
    Text(String),
    Confirmed(bool),
}

/// A dialog waiting for an answer.
pub struct DialogRequest {
    pub title: String,
    pub kind: DialogKind,
    pub timeout: Option<Duration>,
    /// Receives exactly one answer (the first wins).
    pub reply: mpsc::SyncSender<Option<DialogAnswer>>,
    /// Set by the extension thread when it stopped waiting (timeout).
    pub expired: Arc<AtomicBool>,
}

/// Events sent from the window to a running `ctx.ui.custom` component.
#[derive(Debug, Clone, PartialEq)]
pub enum ComponentEvent {
    Input(String),
    Cancel,
}

/// One `ctx.ui.custom` session: the window shows `lines` and forwards key
/// input; the extension thread renders and answers.
pub struct ComponentSession {
    pub id: u64,
    pub title: String,
    pub events: mpsc::Sender<ComponentEvent>,
}

/// Everything extension threads ask the window to do.
pub enum UiRequest {
    Dialog(DialogRequest),
    Notify {
        message: String,
        kind: NotifyKind,
    },
    Status {
        key: String,
        text: Option<String>,
    },
    WorkingMessage(Option<String>),
    WorkingVisible(bool),
    WorkingIndicator {
        frames: Option<Vec<String>>,
        interval_ms: u64,
    },
    HiddenThinkingLabel(Option<String>),
    Widget {
        key: String,
        lines: Option<Vec<String>>,
    },
    Header(Option<Vec<String>>),
    Footer(Option<Vec<String>>),
    Title(String),
    ToolsExpanded(bool),
    SetEditorText(String),
    PasteToEditor(String),
    SetTheme(String),
    ComponentOpen {
        session: ComponentSession,
        lines: Vec<String>,
    },
    ComponentUpdate {
        id: u64,
        lines: Vec<String>,
    },
    ComponentClose {
        id: u64,
    },
    EditorComponent(Option<ScriptComponent>),
}

/// State the window mirrors for synchronous getters.
#[derive(Debug, Clone, Default)]
pub struct Mirror {
    pub editor_text: String,
    pub tools_expanded: bool,
    /// Theme names available to `getAllThemes` / `setTheme`.
    pub themes: Vec<String>,
    pub applied_theme: String,
    pub editor_component: Option<String>,
    pub autocomplete: Vec<AutocompleteProvider>,
}

/// Width and height (columns, rows) used to render components.
pub const COMPONENT_COLUMNS: u32 = 100;
pub const COMPONENT_ROWS: u32 = 24;

/// The `ExtensionUi` registered with the runtime.
pub struct GpuiExtensionUi {
    tx: async_channel::Sender<UiRequest>,
    mirror: Arc<Mutex<Mirror>>,
    ui_thread: ThreadId,
    next_component: std::sync::atomic::AtomicU64,
}

impl GpuiExtensionUi {
    /// `ui_thread` is the GPUI main thread (calls from it never block).
    pub fn new(
        tx: async_channel::Sender<UiRequest>,
        mirror: Arc<Mutex<Mirror>>,
        ui_thread: ThreadId,
    ) -> Self {
        Self {
            tx,
            mirror,
            ui_thread,
            next_component: std::sync::atomic::AtomicU64::new(1),
        }
    }

    fn on_ui_thread(&self) -> bool {
        std::thread::current().id() == self.ui_thread
    }

    fn send(&self, request: UiRequest) {
        let _ = self.tx.try_send(request);
    }

    fn mirror(&self) -> std::sync::MutexGuard<'_, Mirror> {
        self.mirror.lock().unwrap_or_else(|e| e.into_inner())
    }

    /// Ask the window and wait for the answer (or the timeout).
    fn ask(
        &self,
        title: &str,
        kind: DialogKind,
        timeout: Option<Duration>,
    ) -> Option<DialogAnswer> {
        if self.on_ui_thread() {
            self.send(UiRequest::Notify {
                message: format!(
                    "Extension dialog \"{title}\" was cancelled: dialogs cannot block the window thread."
                ),
                kind: NotifyKind::Warning,
            });
            return None;
        }
        if timeout == Some(Duration::ZERO) {
            return None;
        }
        let (reply, answers) = mpsc::sync_channel(1);
        let expired = Arc::new(AtomicBool::new(false));
        self.send(UiRequest::Dialog(DialogRequest {
            title: title.to_string(),
            kind,
            timeout,
            reply,
            expired: expired.clone(),
        }));
        let answer = match timeout {
            Some(t) => answers.recv_timeout(t).ok().flatten(),
            None => answers.recv().ok().flatten(),
        };
        if answer.is_none() {
            expired.store(true, Ordering::SeqCst);
        }
        answer
    }
}

/// `{name, current}` for `getTheme` / `getAllThemes`.
pub fn theme_json(name: &str, applied: &str) -> Value {
    json!({ "name": name, "current": name == applied })
}

impl ExtensionUi for GpuiExtensionUi {
    fn select(&self, title: &str, options: &[String], timeout: Option<Duration>) -> Option<String> {
        match self.ask(
            title,
            DialogKind::Select {
                options: options.to_vec(),
            },
            timeout,
        ) {
            Some(DialogAnswer::Text(choice)) if options.contains(&choice) => Some(choice),
            _ => None,
        }
    }

    fn confirm(&self, title: &str, message: &str, timeout: Option<Duration>) -> bool {
        matches!(
            self.ask(
                title,
                DialogKind::Confirm {
                    message: message.to_string(),
                },
                timeout,
            ),
            Some(DialogAnswer::Confirmed(true))
        )
    }

    fn input(&self, title: &str, placeholder: &str, timeout: Option<Duration>) -> Option<String> {
        match self.ask(
            title,
            DialogKind::Input {
                placeholder: placeholder.to_string(),
            },
            timeout,
        ) {
            Some(DialogAnswer::Text(text)) => Some(text),
            _ => None,
        }
    }

    fn editor(&self, title: &str, prefill: &str, timeout: Option<Duration>) -> Option<String> {
        match self.ask(
            title,
            DialogKind::Editor {
                prefill: prefill.to_string(),
            },
            timeout,
        ) {
            Some(DialogAnswer::Text(text)) => Some(text),
            _ => None,
        }
    }

    fn notify(&self, message: &str, kind: NotifyKind) {
        self.send(UiRequest::Notify {
            message: message.to_string(),
            kind,
        });
    }

    fn set_status(&self, key: &str, text: Option<&str>) {
        self.send(UiRequest::Status {
            key: key.to_string(),
            text: text.map(str::to_string),
        });
    }

    fn set_working_message(&self, message: Option<&str>) {
        self.send(UiRequest::WorkingMessage(message.map(str::to_string)));
    }

    fn set_working_visible(&self, visible: bool) {
        self.send(UiRequest::WorkingVisible(visible));
    }

    fn set_working_indicator(&self, frames: Option<&[String]>, interval_ms: u64) {
        self.send(UiRequest::WorkingIndicator {
            frames: frames.map(<[String]>::to_vec),
            interval_ms,
        });
    }

    fn set_hidden_thinking_label(&self, label: Option<&str>) {
        self.send(UiRequest::HiddenThinkingLabel(label.map(str::to_string)));
    }

    fn set_widget(&self, key: &str, lines: Option<&[String]>) {
        self.send(UiRequest::Widget {
            key: key.to_string(),
            lines: lines.map(<[String]>::to_vec),
        });
    }

    fn set_header(&self, lines: Option<&[String]>) {
        self.send(UiRequest::Header(lines.map(<[String]>::to_vec)));
    }

    fn set_footer(&self, lines: Option<&[String]>) {
        self.send(UiRequest::Footer(lines.map(<[String]>::to_vec)));
    }

    fn set_title(&self, title: &str) {
        self.send(UiRequest::Title(title.to_string()));
    }

    fn tools_expanded(&self) -> bool {
        self.mirror().tools_expanded
    }

    fn set_tools_expanded(&self, expanded: bool) {
        self.mirror().tools_expanded = expanded;
        self.send(UiRequest::ToolsExpanded(expanded));
    }

    fn editor_text(&self) -> String {
        self.mirror().editor_text.clone()
    }

    fn set_editor_text(&self, text: &str) {
        self.mirror().editor_text = text.to_string();
        self.send(UiRequest::SetEditorText(text.to_string()));
    }

    fn paste_to_editor(&self, text: &str) {
        self.send(UiRequest::PasteToEditor(text.to_string()));
    }

    fn themes(&self) -> Vec<Value> {
        let mirror = self.mirror();
        mirror
            .themes
            .iter()
            .map(|n| theme_json(n, &mirror.applied_theme))
            .collect()
    }

    fn theme(&self, name: Option<&str>) -> Option<Value> {
        let mirror = self.mirror();
        let name = name.unwrap_or(&mirror.applied_theme);
        mirror
            .themes
            .iter()
            .find(|t| t.eq_ignore_ascii_case(name))
            .map(|t| theme_json(t, &mirror.applied_theme))
    }

    fn set_theme(&self, name: &str) -> Result<(), String> {
        let found = self
            .mirror()
            .themes
            .iter()
            .find(|t| t.eq_ignore_ascii_case(name))
            .cloned()
            .ok_or_else(|| format!("theme not found: {name}"))?;
        self.mirror().applied_theme = found.clone();
        self.send(UiRequest::SetTheme(found));
        Ok(())
    }

    fn custom(&self, component: ScriptComponent) -> Result<Value, String> {
        if self.on_ui_thread() {
            self.notify(
                "Extension component was cancelled: components cannot block the window thread.",
                NotifyKind::Warning,
            );
            return Ok(Value::Null);
        }
        let id = self
            .next_component
            .fetch_add(1, std::sync::atomic::Ordering::SeqCst);
        let (events, inbox) = mpsc::channel();
        let lines = component.render(COMPONENT_COLUMNS, COMPONENT_ROWS)?;
        self.send(UiRequest::ComponentOpen {
            session: ComponentSession {
                id,
                title: component.handler.clone(),
                events,
            },
            lines,
        });
        let result = run_component(&component, &inbox, |lines| {
            self.send(UiRequest::ComponentUpdate { id, lines });
        });
        self.send(UiRequest::ComponentClose { id });
        result
    }

    fn set_editor_component(&self, component: Option<ScriptComponent>) {
        self.mirror().editor_component = component.as_ref().map(|c| c.handler.clone());
        self.send(UiRequest::EditorComponent(component));
    }

    fn editor_component(&self) -> Option<String> {
        self.mirror().editor_component.clone()
    }

    fn add_autocomplete_provider(&self, provider: AutocompleteProvider) {
        let mut mirror = self.mirror();
        mirror
            .autocomplete
            .retain(|p| p.handler != provider.handler);
        mirror.autocomplete.push(provider);
    }
}

/// Drive a component on the calling (extension) thread until it reports
/// `done` (its value) or the window cancels it (`null`). `update` receives
/// re-rendered lines.
pub fn run_component(
    component: &ScriptComponent,
    inbox: &mpsc::Receiver<ComponentEvent>,
    mut update: impl FnMut(Vec<String>),
) -> Result<Value, String> {
    loop {
        match inbox.recv() {
            Ok(ComponentEvent::Input(data)) => {
                let response = component.input(&data)?;
                if response.done {
                    return Ok(response.value);
                }
                let lines = match response.lines {
                    Some(lines) => lines,
                    None => component.render(COMPONENT_COLUMNS, COMPONENT_ROWS)?,
                };
                update(lines);
            }
            Ok(ComponentEvent::Cancel) | Err(_) => return Ok(Value::Null),
        }
    }
}

/// Status entries in display order (by key, as Pi's footer sorts them).
pub fn status_line(statuses: &BTreeMap<String, String>) -> String {
    statuses
        .values()
        .filter(|t| !t.is_empty())
        .cloned()
        .collect::<Vec<_>>()
        .join("  ·  ")
}

#[cfg(test)]
mod tests {
    use super::*;
    use qt_runtime::qt_extensions::ui::HandlerCallback;

    fn ui() -> (
        GpuiExtensionUi,
        async_channel::Receiver<UiRequest>,
        Arc<Mutex<Mirror>>,
    ) {
        let (tx, rx) = async_channel::unbounded();
        let mirror = Arc::new(Mutex::new(Mirror {
            themes: vec!["dark".into(), "light".into()],
            applied_theme: "dark".into(),
            ..Default::default()
        }));
        // A thread id that is not the test thread: dialogs may block.
        let other = std::thread::spawn(|| std::thread::current().id())
            .join()
            .unwrap();
        (GpuiExtensionUi::new(tx, mirror.clone(), other), rx, mirror)
    }

    fn answer_next(rx: async_channel::Receiver<UiRequest>, answer: Option<DialogAnswer>) {
        std::thread::spawn(move || {
            if let Ok(UiRequest::Dialog(d)) = rx.recv_blocking() {
                let _ = d.reply.send(answer);
            }
        });
    }

    #[test]
    fn dialogs_answer_and_cancel() {
        let (ui, rx, _) = ui();
        answer_next(rx.clone(), Some(DialogAnswer::Text("b".into())));
        assert_eq!(
            ui.select("Pick", &["a".into(), "b".into()], None),
            Some("b".into())
        );
        answer_next(rx.clone(), Some(DialogAnswer::Text("zzz".into())));
        assert_eq!(ui.select("Pick", &["a".into()], None), None);
        answer_next(rx.clone(), Some(DialogAnswer::Confirmed(true)));
        assert!(ui.confirm("Sure?", "really", None));
        answer_next(rx.clone(), None);
        assert!(!ui.confirm("Sure?", "really", None));
        answer_next(rx.clone(), Some(DialogAnswer::Text(String::new())));
        assert_eq!(ui.input("Name", "", None), Some(String::new()));
        answer_next(rx.clone(), Some(DialogAnswer::Text("edited".into())));
        assert_eq!(ui.editor("Edit", "x", None), Some("edited".into()));
    }

    #[test]
    fn timeouts_expire_the_dialog() {
        let (ui, rx, _) = ui();
        let started = std::time::Instant::now();
        assert_eq!(ui.input("Slow", "", Some(Duration::from_millis(30))), None);
        assert!(started.elapsed() >= Duration::from_millis(30));
        let Ok(UiRequest::Dialog(d)) = rx.try_recv() else {
            panic!("dialog request expected")
        };
        assert!(d.expired.load(Ordering::SeqCst));
        assert_eq!(d.timeout, Some(Duration::from_millis(30)));
        assert!(!ui.confirm("Zero", "m", Some(Duration::ZERO)));
        assert!(rx.try_recv().is_err(), "zero timeout opens nothing");
    }

    #[test]
    fn ui_thread_calls_never_block() {
        let (tx, rx) = async_channel::unbounded();
        let mirror = Arc::new(Mutex::new(Mirror::default()));
        let ui = GpuiExtensionUi::new(tx, mirror, std::thread::current().id());
        assert_eq!(ui.select("t", &["a".into()], None), None);
        assert!(matches!(
            rx.try_recv(),
            Ok(UiRequest::Notify {
                kind: NotifyKind::Warning,
                ..
            })
        ));
    }

    #[test]
    fn mirrored_getters_and_themes() {
        let (ui, rx, mirror) = ui();
        ui.set_editor_text("draft");
        assert_eq!(ui.editor_text(), "draft");
        assert!(matches!(rx.try_recv(), Ok(UiRequest::SetEditorText(t)) if t == "draft"));
        ui.set_tools_expanded(true);
        assert!(ui.tools_expanded());
        assert_eq!(ui.themes().len(), 2);
        assert_eq!(
            ui.theme(None).unwrap(),
            json!({"name": "dark", "current": true})
        );
        assert_eq!(ui.theme(Some("LIGHT")).unwrap()["name"], "light");
        assert_eq!(ui.set_theme("nope").unwrap_err(), "theme not found: nope");
        ui.set_theme("light").unwrap();
        assert_eq!(mirror.lock().unwrap().applied_theme, "light");
        let call: HandlerCallback = Arc::new(|_| Ok(Value::Null));
        ui.add_autocomplete_provider(AutocompleteProvider {
            handler: "complete".into(),
            call: call.clone(),
        });
        ui.add_autocomplete_provider(AutocompleteProvider {
            handler: "complete".into(),
            call,
        });
        assert_eq!(mirror.lock().unwrap().autocomplete.len(), 1);
        ui.set_editor_component(None);
        assert_eq!(ui.editor_component(), None);
    }

    #[test]
    fn components_render_until_done_or_cancelled() {
        let call: HandlerCallback = Arc::new(|event: Value| {
            Ok(match event["type"].as_str() {
                Some("render") => json!({"lines": ["count: 0"]}),
                _ if event["data"] == "q" => json!({"done": "finished"}),
                _ => json!({"handled": true, "lines": [format!("typed {}", event["data"])]}),
            })
        });
        let component = ScriptComponent {
            handler: "panel".into(),
            call,
        };
        let (tx, inbox) = mpsc::channel();
        tx.send(ComponentEvent::Input("a".into())).unwrap();
        tx.send(ComponentEvent::Input("q".into())).unwrap();
        let mut updates = Vec::new();
        let value = run_component(&component, &inbox, |l| updates.push(l)).unwrap();
        assert_eq!(value, json!("finished"));
        assert_eq!(updates, [vec!["typed \"a\"".to_string()]]);
        tx.send(ComponentEvent::Cancel).unwrap();
        assert_eq!(
            run_component(&component, &inbox, |_| {}).unwrap(),
            Value::Null
        );
        drop(tx);
        assert_eq!(
            run_component(&component, &inbox, |_| {}).unwrap(),
            Value::Null
        );

        let mut statuses = BTreeMap::new();
        statuses.insert("b".to_string(), "second".to_string());
        statuses.insert("a".to_string(), "first".to_string());
        statuses.insert("c".to_string(), String::new());
        assert_eq!(status_line(&statuses), "first  ·  second");
    }
}
