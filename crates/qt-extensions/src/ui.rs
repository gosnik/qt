//! `ctx.ui.*`: the presentation API extensions call (archived
//! `extension_ui_presenter.cpp` and the headless host in
//! `extension_runtime_binding.cpp`).
//!
//! Front ends implement [`ExtensionUi`]; every method has the headless
//! behaviour as its default (dialogs return their cancelled value,
//! notifications go to stderr, presentation setters do nothing), so
//! [`HeadlessUi`] is the empty implementation plus stderr notifications.
//! [`dispatch_ui`] validates the JSON arguments and calls the trait.

use std::sync::Arc;
use std::time::Duration;

use serde_json::{Value, json};

/// Calls one script handler of the extension that owns a component or
/// provider (`{type:"render",...}` / `{type:"input",...}` events).
pub type HandlerCallback = Arc<dyn Fn(Value) -> Result<Value, String> + Send + Sync>;

/// A scripted component (`ctx.ui.custom`, `setEditorComponent`). The front
/// end sends `{type:"render",width,height}` and `{type:"input",data}`; a
/// render returns `{lines:[...]}`, an input may return `{handled, done, value}`.
#[derive(Clone)]
pub struct ScriptComponent {
    pub handler: String,
    pub call: HandlerCallback,
}

impl std::fmt::Debug for ScriptComponent {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ScriptComponent")
            .field("handler", &self.handler)
            .finish_non_exhaustive()
    }
}

/// What a component returned for one event.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct ComponentResponse {
    pub lines: Option<Vec<String>>,
    pub handled: bool,
    /// The component finished; `value` is the `ctx.ui.custom` result.
    pub done: bool,
    pub value: Value,
}

impl ScriptComponent {
    pub fn render(&self, width: u32, height: u32) -> Result<Vec<String>, String> {
        let r = (self.call)(json!({ "type": "render", "width": width, "height": height }))?;
        Ok(decode_component(&r).lines.unwrap_or_default())
    }

    pub fn input(&self, data: &str) -> Result<ComponentResponse, String> {
        let r = (self.call)(json!({ "type": "input", "data": data }))?;
        Ok(decode_component(&r))
    }
}

/// Decode a component handler result.
pub fn decode_component(value: &Value) -> ComponentResponse {
    let lines = value.get("lines").and_then(Value::as_array).map(|a| {
        a.iter()
            .map(|l| {
                l.as_str()
                    .map(str::to_string)
                    .unwrap_or_else(|| l.to_string())
            })
            .collect()
    });
    let done = value.get("done").is_some_and(|d| match d {
        Value::Bool(b) => *b,
        Value::Null => false,
        _ => true,
    });
    ComponentResponse {
        lines,
        handled: value
            .get("handled")
            .and_then(Value::as_bool)
            .unwrap_or(false),
        done,
        value: match value.get("done") {
            Some(Value::Bool(_)) | None => value.get("value").cloned().unwrap_or(Value::Null),
            Some(other) => other.clone(),
        },
    }
}

/// One autocomplete suggestion.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AutocompleteItem {
    pub value: String,
    pub label: String,
    pub description: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct AutocompleteSuggestions {
    pub prefix: String,
    pub items: Vec<AutocompleteItem>,
}

/// An extension autocomplete provider (`ctx.ui.addAutocompleteProvider`).
#[derive(Clone)]
pub struct AutocompleteProvider {
    pub handler: String,
    pub call: HandlerCallback,
}

impl std::fmt::Debug for AutocompleteProvider {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("AutocompleteProvider")
            .field("handler", &self.handler)
            .finish_non_exhaustive()
    }
}

impl AutocompleteProvider {
    /// Suggestions for the editor state; failures and malformed results give none.
    pub fn complete(
        &self,
        lines: &[String],
        row: usize,
        column: usize,
        force: bool,
    ) -> Option<AutocompleteSuggestions> {
        let r =
            (self.call)(json!({ "lines": lines, "row": row, "column": column, "force": force }))
                .ok()?;
        decode_autocomplete(&r)
    }
}

pub fn decode_autocomplete(value: &Value) -> Option<AutocompleteSuggestions> {
    let items = value.get("items")?.as_array()?;
    Some(AutocompleteSuggestions {
        prefix: value
            .get("prefix")
            .and_then(Value::as_str)
            .unwrap_or_default()
            .into(),
        items: items
            .iter()
            .filter_map(|item| {
                let v = item.get("value")?.as_str()?.to_string();
                Some(AutocompleteItem {
                    label: item
                        .get("label")
                        .and_then(Value::as_str)
                        .map(str::to_string)
                        .unwrap_or_else(|| v.clone()),
                    description: item
                        .get("description")
                        .and_then(Value::as_str)
                        .map(str::to_string),
                    value: v,
                })
            })
            .collect(),
    })
}

/// Merge suggestions of several providers: items of providers whose prefix
/// equals the first provider's prefix are combined.
pub fn merge_autocomplete(
    results: Vec<AutocompleteSuggestions>,
) -> Option<AutocompleteSuggestions> {
    let mut iter = results.into_iter();
    let mut first = iter.next()?;
    for r in iter {
        if r.prefix == first.prefix {
            first.items.extend(r.items);
        }
    }
    Some(first)
}

/// Notification severity (`notifyType`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum NotifyKind {
    #[default]
    Info,
    Warning,
    Error,
    Success,
}

impl NotifyKind {
    pub fn parse(text: &str) -> Self {
        match text {
            "warning" | "warn" => Self::Warning,
            "error" => Self::Error,
            "success" => Self::Success,
            _ => Self::Info,
        }
    }
}

fn unsupported(method: &str) -> String {
    format!("not_implemented: {method} requires a typed in-process UI component or presenter")
}

/// Presentation surface for extensions. Every method defaults to headless
/// behaviour. Dialog methods block the calling (extension) thread until the
/// user answers; `timeout` cancels them.
pub trait ExtensionUi: Send + Sync {
    fn select(
        &self,
        _title: &str,
        _options: &[String],
        _timeout: Option<Duration>,
    ) -> Option<String> {
        None
    }
    fn confirm(&self, _title: &str, _message: &str, _timeout: Option<Duration>) -> bool {
        false
    }
    fn input(
        &self,
        _title: &str,
        _placeholder: &str,
        _timeout: Option<Duration>,
    ) -> Option<String> {
        None
    }
    fn editor(&self, _title: &str, _prefill: &str, _timeout: Option<Duration>) -> Option<String> {
        None
    }
    fn notify(&self, message: &str, _kind: NotifyKind) {
        eprintln!("{message}");
    }
    fn set_status(&self, _key: &str, _text: Option<&str>) {}
    fn set_working_message(&self, _message: Option<&str>) {}
    fn set_working_visible(&self, _visible: bool) {}
    fn set_working_indicator(&self, _frames: Option<&[String]>, _interval_ms: u64) {}
    fn set_hidden_thinking_label(&self, _label: Option<&str>) {}
    fn set_widget(&self, _key: &str, _lines: Option<&[String]>) {}
    fn set_header(&self, _lines: Option<&[String]>) {}
    fn set_footer(&self, _lines: Option<&[String]>) {}
    fn set_title(&self, _title: &str) {}
    fn tools_expanded(&self) -> bool {
        false
    }
    fn set_tools_expanded(&self, _expanded: bool) {}
    fn editor_text(&self) -> String {
        String::new()
    }
    fn set_editor_text(&self, _text: &str) {}
    fn paste_to_editor(&self, _text: &str) {}
    /// Available themes as `{name, ...}` objects.
    fn themes(&self) -> Vec<Value> {
        Vec::new()
    }
    fn theme(&self, _name: Option<&str>) -> Option<Value> {
        None
    }
    fn set_theme(&self, _name: &str) -> Result<(), String> {
        Ok(())
    }
    /// Show a scripted component until it reports `done`; `Null` when cancelled.
    fn custom(&self, _component: ScriptComponent) -> Result<Value, String> {
        Ok(Value::Null)
    }
    /// Replace (or with `None` restore) the prompt editor with a component.
    fn set_editor_component(&self, _component: Option<ScriptComponent>) {}
    /// Handler of the active editor component, if any.
    fn editor_component(&self) -> Option<String> {
        None
    }
    fn add_autocomplete_provider(&self, _provider: AutocompleteProvider) {}
    /// Terminal input interception (TUI only; GUI front ends report it unsupported).
    fn set_terminal_input_handler(&self, handler: Option<ScriptComponent>) -> Result<(), String> {
        match handler {
            None => Ok(()),
            Some(_) => Err(unsupported("ctx.ui.onTerminalInput")),
        }
    }
}

/// Headless front ends (print/JSON mode, background threads).
#[derive(Debug, Default, Clone, Copy)]
pub struct HeadlessUi;

impl ExtensionUi for HeadlessUi {}

fn required_string(args: &Value, field: &str) -> Result<String, String> {
    args.get(field)
        .and_then(Value::as_str)
        .map(str::to_string)
        .ok_or_else(|| format!("UI method requires string {field}"))
}

fn optional_text(args: &Value, field: &str) -> Result<Option<String>, String> {
    match args.get(field) {
        None | Some(Value::Null) => Ok(None),
        Some(Value::String(s)) => Ok(Some(s.clone())),
        Some(_) => Err(format!("UI method requires string {field}")),
    }
}

fn required_strings(args: &Value, field: &str) -> Result<Vec<String>, String> {
    let items = args
        .get(field)
        .and_then(Value::as_array)
        .ok_or_else(|| format!("UI method requires array {field}"))?;
    items
        .iter()
        .map(|v| {
            v.as_str()
                .map(str::to_string)
                .ok_or_else(|| format!("{field} must contain strings"))
        })
        .collect()
}

fn optional_strings(args: &Value, field: &str) -> Result<Option<Vec<String>>, String> {
    match args.get(field) {
        None | Some(Value::Null) => Ok(None),
        Some(_) => required_strings(args, field).map(Some),
    }
}

fn timeout(args: &Value) -> Result<Option<Duration>, String> {
    match args.get("timeout").and_then(Value::as_i64) {
        None => Ok(None),
        Some(t) if t < 0 => Err("UI timeout must not be negative".into()),
        Some(t) => Ok(Some(Duration::from_millis(t as u64))),
    }
}

/// Makes a callback that invokes `handler` in the calling extension.
pub type CallbackFactory<'a> = &'a dyn Fn(&str) -> HandlerCallback;

/// Run `ctx.ui.<method>`. `callbacks` builds handler callbacks for
/// components and providers; without it those methods are unsupported.
pub fn dispatch_ui(
    ui: &dyn ExtensionUi,
    method: &str,
    args: &Value,
    callbacks: Option<CallbackFactory<'_>>,
) -> Result<Value, String> {
    let component = |args: &Value| -> Result<Option<ScriptComponent>, String> {
        match args.get("handler") {
            None | Some(Value::Null) => Ok(None),
            Some(_) => {
                let handler = required_string(args, "handler")?;
                let factory = callbacks.ok_or_else(|| unsupported(method))?;
                Ok(Some(ScriptComponent {
                    call: factory(&handler),
                    handler,
                }))
            }
        }
    };
    match method {
        "ctx.ui.select" => {
            let title = required_string(args, "title")?;
            let options = required_strings(args, "options")?;
            Ok(ui
                .select(&title, &options, timeout(args)?)
                .map(Value::String)
                .unwrap_or(Value::Null))
        }
        "ctx.ui.confirm" => {
            let title = required_string(args, "title")?;
            let message = required_string(args, "message")?;
            Ok(json!(ui.confirm(&title, &message, timeout(args)?)))
        }
        "ctx.ui.input" => {
            let title = required_string(args, "title")?;
            let placeholder = optional_text(args, "placeholder")?.unwrap_or_default();
            Ok(ui
                .input(&title, &placeholder, timeout(args)?)
                .map(Value::String)
                .unwrap_or(Value::Null))
        }
        "ctx.ui.editor" => {
            let title = required_string(args, "title")?;
            let prefill = optional_text(args, "prefill")?.unwrap_or_default();
            Ok(ui
                .editor(&title, &prefill, timeout(args)?)
                .map(Value::String)
                .unwrap_or(Value::Null))
        }
        "ctx.ui.notify" => {
            let message = required_string(args, "message")?;
            let kind = args
                .get("notifyType")
                .and_then(Value::as_str)
                .map(NotifyKind::parse)
                .unwrap_or_default();
            ui.notify(&message, kind);
            Ok(Value::Null)
        }
        "ctx.ui.setStatus" => {
            let key = required_string(args, "statusKey")?;
            let text = optional_text(args, "statusText")?;
            ui.set_status(&key, text.as_deref());
            Ok(Value::Null)
        }
        "ctx.ui.setWorkingMessage" => {
            ui.set_working_message(optional_text(args, "message")?.as_deref());
            Ok(Value::Null)
        }
        "ctx.ui.setWorkingVisible" => {
            ui.set_working_visible(args.get("visible").and_then(Value::as_bool).unwrap_or(true));
            Ok(Value::Null)
        }
        "ctx.ui.setWorkingIndicator" => {
            let frames = optional_strings(args, "frames")?;
            let interval = args.get("intervalMs").and_then(Value::as_u64).unwrap_or(80);
            ui.set_working_indicator(frames.as_deref(), interval);
            Ok(Value::Null)
        }
        "ctx.ui.setHiddenThinkingLabel" => {
            ui.set_hidden_thinking_label(optional_text(args, "label")?.as_deref());
            Ok(Value::Null)
        }
        "ctx.ui.setWidget" => {
            let key = required_string(args, "key")?;
            let content = optional_strings(args, "content")?;
            ui.set_widget(&key, content.as_deref());
            Ok(Value::Null)
        }
        "ctx.ui.setHeader" | "ctx.ui.setFooter" => {
            let lines = match optional_strings(args, "lines")? {
                Some(l) => Some(l),
                None => optional_strings(args, "content")?,
            };
            if method == "ctx.ui.setHeader" {
                ui.set_header(lines.as_deref());
            } else {
                ui.set_footer(lines.as_deref());
            }
            Ok(Value::Null)
        }
        "ctx.ui.setTitle" => {
            ui.set_title(&required_string(args, "title")?);
            Ok(Value::Null)
        }
        "ctx.ui.getToolsExpanded" => Ok(json!(ui.tools_expanded())),
        "ctx.ui.setToolsExpanded" => {
            ui.set_tools_expanded(
                args.get("expanded")
                    .and_then(Value::as_bool)
                    .unwrap_or(false),
            );
            Ok(Value::Null)
        }
        "ctx.ui.getEditorText" => Ok(json!(ui.editor_text())),
        "ctx.ui.setEditorText" => {
            ui.set_editor_text(&required_string(args, "text")?);
            Ok(Value::Null)
        }
        "ctx.ui.pasteToEditor" => {
            ui.paste_to_editor(&required_string(args, "text")?);
            Ok(Value::Null)
        }
        "ctx.ui.getAllThemes" => Ok(Value::Array(ui.themes())),
        "ctx.ui.getTheme" => Ok(ui
            .theme(optional_text(args, "name")?.as_deref())
            .unwrap_or(Value::Null)),
        "ctx.ui.setTheme" => {
            ui.set_theme(&required_string(args, "name")?)?;
            Ok(Value::Null)
        }
        "ctx.ui.custom" => {
            let c =
                component(args)?.ok_or_else(|| "UI method requires string handler".to_string())?;
            ui.custom(c)
        }
        "ctx.ui.setEditorComponent" => {
            ui.set_editor_component(component(args)?);
            Ok(Value::Null)
        }
        "ctx.ui.getEditorComponent" => Ok(ui
            .editor_component()
            .map(|h| json!({ "handler": h }))
            .unwrap_or(Value::Null)),
        "ctx.ui.addAutocompleteProvider" => {
            let c =
                component(args)?.ok_or_else(|| "UI method requires string handler".to_string())?;
            ui.add_autocomplete_provider(AutocompleteProvider {
                handler: c.handler,
                call: c.call,
            });
            Ok(Value::Null)
        }
        "ctx.ui.onTerminalInput" => {
            ui.set_terminal_input_handler(component(args)?)?;
            Ok(Value::Null)
        }
        other => Err(format!("unknown extension host call: {other}")),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex;

    #[derive(Default)]
    struct Recording {
        calls: Mutex<Vec<String>>,
    }

    impl ExtensionUi for Recording {
        fn select(
            &self,
            title: &str,
            options: &[String],
            timeout: Option<Duration>,
        ) -> Option<String> {
            self.calls
                .lock()
                .unwrap()
                .push(format!("select {title} {options:?} {timeout:?}"));
            options.first().cloned()
        }
        fn notify(&self, message: &str, kind: NotifyKind) {
            self.calls
                .lock()
                .unwrap()
                .push(format!("notify {message} {kind:?}"));
        }
        fn set_status(&self, key: &str, text: Option<&str>) {
            self.calls
                .lock()
                .unwrap()
                .push(format!("status {key} {text:?}"));
        }
        fn custom(&self, component: ScriptComponent) -> Result<Value, String> {
            let lines = component.render(80, 24)?;
            let r = component.input("q")?;
            self.calls
                .lock()
                .unwrap()
                .push(format!("custom {lines:?} {}", r.done));
            Ok(r.value)
        }
    }

    #[test]
    fn headless_defaults() {
        let ui = HeadlessUi;
        let call = |m: &str, a: Value| dispatch_ui(&ui, m, &a, None);
        assert_eq!(
            call("ctx.ui.confirm", json!({"title":"t","message":"m"})).unwrap(),
            json!(false)
        );
        assert_eq!(
            call("ctx.ui.select", json!({"title":"t","options":["a"]})).unwrap(),
            Value::Null
        );
        assert_eq!(
            call("ctx.ui.input", json!({"title":"t"})).unwrap(),
            Value::Null
        );
        assert_eq!(
            call("ctx.ui.editor", json!({"title":"t","prefill":"x"})).unwrap(),
            Value::Null
        );
        assert_eq!(call("ctx.ui.getEditorText", json!({})).unwrap(), json!(""));
        assert_eq!(call("ctx.ui.getAllThemes", json!({})).unwrap(), json!([]));
        assert_eq!(
            call("ctx.ui.getToolsExpanded", json!({})).unwrap(),
            json!(false)
        );
        assert_eq!(
            call("ctx.ui.setHeader", json!({"lines":null})).unwrap(),
            Value::Null
        );
        assert_eq!(
            call("ctx.ui.onTerminalInput", json!({"handler":null})).unwrap(),
            Value::Null
        );
        assert_eq!(
            call("ctx.ui.custom", json!({"handler":"panel"})).unwrap_err(),
            "not_implemented: ctx.ui.custom requires a typed in-process UI component or presenter"
        );
        assert_eq!(
            call("ctx.ui.select", json!({"title":"t"})).unwrap_err(),
            "UI method requires array options"
        );
        assert_eq!(
            call("ctx.ui.select", json!({"title":"t","options":[1]})).unwrap_err(),
            "options must contain strings"
        );
        assert_eq!(
            call("ctx.ui.notify", json!({})).unwrap_err(),
            "UI method requires string message"
        );
        assert_eq!(
            call(
                "ctx.ui.confirm",
                json!({"title":"t","message":"m","timeout":-1})
            )
            .unwrap_err(),
            "UI timeout must not be negative"
        );
        assert_eq!(
            call("ctx.ui.nope", json!({})).unwrap_err(),
            "unknown extension host call: ctx.ui.nope"
        );
    }

    #[test]
    fn presenter_calls_and_components() {
        let ui = Recording::default();
        let factory = |handler: &str| -> HandlerCallback {
            let handler = handler.to_string();
            Arc::new(move |event: Value| {
                Ok(match event["type"].as_str() {
                    Some("render") => json!({"lines":[format!("{handler} {}", event["width"])]}),
                    _ => json!({"handled":true,"done":"picked"}),
                })
            })
        };
        let f: CallbackFactory<'_> = &factory;
        assert_eq!(
            dispatch_ui(
                &ui,
                "ctx.ui.select",
                &json!({"title":"Pick","options":["x","y"],"timeout":50}),
                Some(f)
            )
            .unwrap(),
            json!("x")
        );
        dispatch_ui(
            &ui,
            "ctx.ui.notify",
            &json!({"message":"hi","notifyType":"warning"}),
            Some(f),
        )
        .unwrap();
        dispatch_ui(
            &ui,
            "ctx.ui.setStatus",
            &json!({"statusKey":"k","statusText":null}),
            Some(f),
        )
        .unwrap();
        assert_eq!(
            dispatch_ui(&ui, "ctx.ui.custom", &json!({"handler":"panel"}), Some(f)).unwrap(),
            json!("picked")
        );
        let calls = ui.calls.lock().unwrap().clone();
        assert_eq!(
            calls,
            [
                "select Pick [\"x\", \"y\"] Some(50ms)",
                "notify hi Warning",
                "status k None",
                "custom [\"panel 80\"] true",
            ]
        );
    }

    #[test]
    fn autocomplete_decoding() {
        let s = decode_autocomplete(&json!({"prefix":"/x","items":[{"value":"a"},{"value":"b","label":"B","description":"d"},{"bad":1}]})).unwrap();
        assert_eq!(s.items.len(), 2);
        assert_eq!(s.items[0].label, "a");
        assert!(decode_autocomplete(&Value::Null).is_none());
        let merged = merge_autocomplete(vec![
            s.clone(),
            AutocompleteSuggestions {
                prefix: "/x".into(),
                items: vec![AutocompleteItem {
                    value: "c".into(),
                    label: "c".into(),
                    description: None,
                }],
            },
            AutocompleteSuggestions {
                prefix: "/y".into(),
                items: vec![AutocompleteItem {
                    value: "z".into(),
                    label: "z".into(),
                    description: None,
                }],
            },
        ])
        .unwrap();
        assert_eq!(merged.items.len(), 3);
    }
}
