//! Extension UI over RPC (Pi `rpc-mode.ts` `createExtensionUIContext`).
//!
//! Dialogs (`select`, `confirm`, `input`, `editor`) emit
//! `{type: "extension_ui_request", id, method, ...}` and block the calling
//! extension thread until the client answers with an
//! `{type: "extension_ui_response", id, value | confirmed | cancelled}`
//! frame. A dialog with a `timeout` resolves to its default (`undefined` /
//! `false`) when it expires, as does every dialog once the input stream
//! closes. Fire-and-forget methods (`notify`, `setStatus`, `setWidget`,
//! `setTitle`, `set_editor_text`) emit a request with a fresh id and expect
//! no answer; terminal-only features are no-ops.

use std::collections::{HashMap, HashSet};
use std::sync::Mutex;
use std::sync::mpsc;
use std::time::Duration;

use serde_json::{Map, Value, json};

use qt_extensions::ExtensionUi;
use qt_extensions::ui::NotifyKind;

use super::output::RpcOutput;

/// A random RFC 4122 version 4 UUID (Pi `crypto.randomUUID()`).
pub fn random_uuid() -> String {
    let mut bytes: [u8; 16] = rand::random();
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    let hex: String = bytes.iter().map(|b| format!("{b:02x}")).collect();
    format!(
        "{}-{}-{}-{}-{}",
        &hex[0..8],
        &hex[8..12],
        &hex[12..16],
        &hex[16..20],
        &hex[20..32]
    )
}

#[derive(Default)]
struct Pending {
    waiting: HashMap<String, mpsc::Sender<Map<String, Value>>>,
    closed: bool,
}

/// [`ExtensionUi`] that talks to the RPC client.
pub struct RpcExtensionUi {
    output: RpcOutput,
    pending: Mutex<Pending>,
    answered: Mutex<HashSet<String>>,
}

fn kind_name(kind: NotifyKind) -> &'static str {
    match kind {
        NotifyKind::Info => "info",
        NotifyKind::Warning => "warning",
        NotifyKind::Error => "error",
        NotifyKind::Success => "success",
    }
}

fn cancelled(response: &Map<String, Value>) -> bool {
    response.get("cancelled").and_then(Value::as_bool) == Some(true)
}

fn value_of(response: Option<Map<String, Value>>) -> Option<String> {
    let response = response?;
    if cancelled(&response) {
        return None;
    }
    response
        .get("value")
        .and_then(Value::as_str)
        .map(str::to_string)
}

impl RpcExtensionUi {
    pub fn new(output: RpcOutput) -> Self {
        Self {
            output,
            pending: Mutex::new(Pending::default()),
            answered: Mutex::new(HashSet::new()),
        }
    }

    fn request(&self, id: &str, method: &str, fields: Value) -> Value {
        let mut out = Map::new();
        out.insert("type".into(), json!("extension_ui_request"));
        out.insert("id".into(), json!(id));
        out.insert("method".into(), json!(method));
        if let Value::Object(fields) = fields {
            out.extend(fields);
        }
        Value::Object(out)
    }

    /// Emit a request that expects no answer.
    fn fire(&self, method: &str, fields: Value) {
        self.output
            .write(&self.request(&random_uuid(), method, fields));
    }

    /// Emit a dialog request and wait for its answer; `None` on timeout,
    /// after [`Self::close`] or when the output is gone.
    fn dialog(
        &self,
        method: &str,
        fields: Value,
        timeout: Option<Duration>,
    ) -> Option<Map<String, Value>> {
        let id = random_uuid();
        let (tx, rx) = mpsc::channel();
        {
            let mut pending = self.pending.lock().expect("ui lock");
            if pending.closed || self.output.is_broken() {
                return None;
            }
            pending.waiting.insert(id.clone(), tx);
        }
        self.output.write(&self.request(&id, method, fields));
        let answer = match timeout.filter(|t| !t.is_zero()) {
            Some(timeout) => rx.recv_timeout(timeout).ok(),
            None => rx.recv().ok(),
        };
        if answer.is_none() {
            self.pending.lock().expect("ui lock").waiting.remove(&id);
        }
        answer
    }

    /// Route an `extension_ui_response`. Answers for unknown (expired or
    /// fire-and-forget) ids are ignored; a second answer for the same id is
    /// an error (archived `duplicate extension ui response id`).
    pub fn resolve(&self, id: &str, payload: Map<String, Value>) -> Result<(), String> {
        if !self
            .answered
            .lock()
            .expect("ui lock")
            .insert(id.to_string())
        {
            return Err(format!("duplicate extension ui response id: {id}"));
        }
        let waiting = self.pending.lock().expect("ui lock").waiting.remove(id);
        if let Some(tx) = waiting {
            let _ = tx.send(payload);
        }
        Ok(())
    }

    /// Resolve every open dialog with its default and refuse new ones
    /// (input closed).
    pub fn close(&self) {
        let mut pending = self.pending.lock().expect("ui lock");
        pending.closed = true;
        pending.waiting.clear();
    }

    /// Dialogs waiting for an answer.
    #[cfg(test)]
    pub fn open_dialogs(&self) -> usize {
        self.pending.lock().expect("ui lock").waiting.len()
    }
}

fn with_timeout(mut fields: Value, timeout: Option<Duration>) -> Value {
    if let Some(t) = timeout.filter(|t| !t.is_zero()) {
        fields["timeout"] = json!(t.as_millis() as u64);
    }
    fields
}

impl ExtensionUi for RpcExtensionUi {
    fn select(&self, title: &str, options: &[String], timeout: Option<Duration>) -> Option<String> {
        value_of(self.dialog(
            "select",
            with_timeout(json!({"title": title, "options": options}), timeout),
            timeout,
        ))
    }

    fn confirm(&self, title: &str, message: &str, timeout: Option<Duration>) -> bool {
        let Some(response) = self.dialog(
            "confirm",
            with_timeout(json!({"title": title, "message": message}), timeout),
            timeout,
        ) else {
            return false;
        };
        !cancelled(&response)
            && response
                .get("confirmed")
                .and_then(Value::as_bool)
                .unwrap_or(false)
    }

    fn input(&self, title: &str, placeholder: &str, timeout: Option<Duration>) -> Option<String> {
        let mut fields = json!({"title": title});
        if !placeholder.is_empty() {
            fields["placeholder"] = json!(placeholder);
        }
        value_of(self.dialog("input", with_timeout(fields, timeout), timeout))
    }

    fn editor(&self, title: &str, prefill: &str, timeout: Option<Duration>) -> Option<String> {
        let mut fields = json!({"title": title});
        if !prefill.is_empty() {
            fields["prefill"] = json!(prefill);
        }
        value_of(self.dialog("editor", fields, timeout))
    }

    fn notify(&self, message: &str, kind: NotifyKind) {
        self.fire(
            "notify",
            json!({"message": message, "notifyType": kind_name(kind)}),
        );
    }

    fn set_status(&self, key: &str, text: Option<&str>) {
        let mut fields = json!({"statusKey": key});
        if let Some(text) = text {
            fields["statusText"] = json!(text);
        }
        self.fire("setStatus", fields);
    }

    fn set_widget(&self, key: &str, lines: Option<&[String]>) {
        let mut fields = json!({"widgetKey": key});
        if let Some(lines) = lines {
            fields["widgetLines"] = json!(lines);
        }
        self.fire("setWidget", fields);
    }

    fn set_title(&self, title: &str) {
        self.fire("setTitle", json!({"title": title}));
    }

    fn set_editor_text(&self, text: &str) {
        self.fire("set_editor_text", json!({"text": text}));
    }

    fn paste_to_editor(&self, text: &str) {
        self.set_editor_text(text);
    }

    fn set_theme(&self, _name: &str) -> Result<(), String> {
        Err("Theme switching not supported in RPC mode".into())
    }
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;
    use std::time::Instant;

    use super::*;
    use crate::rpc::output::tests::Captured;

    fn ui() -> (Arc<RpcExtensionUi>, Captured) {
        let captured = Captured::default();
        (
            Arc::new(RpcExtensionUi::new(RpcOutput::new(Box::new(
                captured.clone(),
            )))),
            captured,
        )
    }

    /// Wait for the next request and return it.
    fn next_request(captured: &Captured, index: usize) -> Value {
        let deadline = Instant::now() + Duration::from_secs(5);
        loop {
            if let Some(line) = captured.lines().get(index) {
                return line.clone();
            }
            assert!(Instant::now() < deadline, "no request");
            std::thread::sleep(Duration::from_millis(2));
        }
    }

    fn payload(value: Value) -> Map<String, Value> {
        value.as_object().unwrap().clone()
    }

    #[test]
    fn uuids_are_version_4() {
        let id = random_uuid();
        assert_eq!(id.len(), 36);
        assert_eq!(&id[14..15], "4");
        assert!(matches!(&id[19..20], "8" | "9" | "a" | "b"));
        assert_ne!(id, random_uuid());
    }

    #[test]
    fn dialogs_round_trip() {
        let (ui, captured) = ui();
        let worker = {
            let ui = ui.clone();
            std::thread::spawn(move || {
                (
                    ui.select("Pick", &["a".into(), "b".into()], None),
                    ui.confirm("Sure?", "Really", Some(Duration::from_secs(30))),
                    ui.input("Name", "", None),
                    ui.editor("Edit", "draft", None),
                )
            })
        };
        let select = next_request(&captured, 0);
        assert_eq!(select["method"], "select");
        assert_eq!(select["options"], json!(["a", "b"]));
        assert!(select.get("timeout").is_none());
        ui.resolve(
            select["id"].as_str().unwrap(),
            payload(json!({"value": "b"})),
        )
        .unwrap();
        let confirm = next_request(&captured, 1);
        assert_eq!(confirm["message"], "Really");
        assert_eq!(confirm["timeout"], 30000);
        ui.resolve(
            confirm["id"].as_str().unwrap(),
            payload(json!({"confirmed": true})),
        )
        .unwrap();
        let input = next_request(&captured, 2);
        assert!(input.get("placeholder").is_none());
        ui.resolve(
            input["id"].as_str().unwrap(),
            payload(json!({"cancelled": true})),
        )
        .unwrap();
        let editor = next_request(&captured, 3);
        assert_eq!(editor["prefill"], "draft");
        ui.resolve(
            editor["id"].as_str().unwrap(),
            payload(json!({"value": ""})),
        )
        .unwrap();
        let (select, confirm, input, editor) = worker.join().unwrap();
        assert_eq!(select.as_deref(), Some("b"));
        assert!(confirm);
        assert_eq!(input, None);
        assert_eq!(editor.as_deref(), Some(""));
        // A second answer for the same id is rejected.
        let first = captured.lines()[0]["id"].as_str().unwrap().to_string();
        assert_eq!(
            ui.resolve(&first, Map::new()).unwrap_err(),
            format!("duplicate extension ui response id: {first}")
        );
        // Unknown ids are ignored.
        assert!(ui.resolve("nobody", Map::new()).is_ok());
    }

    #[test]
    fn timeouts_and_close_give_defaults() {
        let (ui, captured) = ui();
        let started = Instant::now();
        assert!(!ui.confirm("t", "m", Some(Duration::from_millis(50))));
        assert!(started.elapsed() >= Duration::from_millis(50));
        assert_eq!(ui.open_dialogs(), 0);
        let waiting = {
            let ui = ui.clone();
            std::thread::spawn(move || ui.select("s", &[], None))
        };
        next_request(&captured, 1);
        ui.close();
        assert_eq!(waiting.join().unwrap(), None);
        // Closed: dialogs return immediately without a request.
        assert_eq!(ui.input("i", "p", None), None);
        assert_eq!(captured.lines().len(), 2);
    }

    #[test]
    fn fire_and_forget_methods() {
        let (ui, captured) = ui();
        ui.notify("hello", NotifyKind::Warning);
        ui.set_status("k", Some("busy"));
        ui.set_status("k", None);
        ui.set_widget("w", Some(&["l1".to_string()]));
        ui.set_title("T");
        ui.paste_to_editor("pasted");
        ui.set_working_message(Some("ignored"));
        assert!(ui.set_theme("dark").is_err());
        assert_eq!(ui.editor_text(), "");
        let lines = captured.lines();
        let methods: Vec<&str> = lines
            .iter()
            .map(|l| l["method"].as_str().unwrap())
            .collect();
        assert_eq!(
            methods,
            [
                "notify",
                "setStatus",
                "setStatus",
                "setWidget",
                "setTitle",
                "set_editor_text"
            ]
        );
        assert_eq!(lines[0]["notifyType"], "warning");
        assert_eq!(lines[1]["statusText"], "busy");
        assert!(lines[2].get("statusText").is_none());
        assert_eq!(lines[3]["widgetLines"], json!(["l1"]));
        assert_eq!(lines[5]["text"], "pasted");
        assert!(lines.iter().all(|l| l["type"] == "extension_ui_request"));
    }
}
