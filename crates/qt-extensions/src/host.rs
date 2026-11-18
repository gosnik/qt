//! The session side of the host API (`pi.*` / `ctx.*`), as a trait so front
//! ends and tests can supply it, plus [`ConversationHost`], the adapter over
//! a `qt_core` [`Conversation`] (archived `extension_host_api.cpp`).

use std::path::PathBuf;
use std::sync::Arc;
use std::time::{Duration, Instant};

use async_channel::Receiver;
use serde_json::{Value, json};

use qt_core::agent::AgentEvent;
use qt_core::catalog::{ModelCatalog, ModelSpec};
use qt_core::engine::{Conversation, Delivery};
use qt_core::harness::{CompactionResult, ContextUsage};
use qt_core::message::{AgentMessage, ContentBlock, CustomMessage, UserContent};
use qt_core::provider::{ModelRef, ThinkingLevel, ToolSpec};

/// Session operations behind the extension host API. Errors are the
/// messages scripts see as exceptions.
pub trait ExtensionHost: Send + Sync {
    fn cwd(&self) -> PathBuf;
    fn session_name(&self) -> Option<String>;
    fn set_session_name(&self, name: &str) -> Result<(), String>;
    /// Persist extension state as a `custom` entry.
    fn append_entry(&self, custom_type: &str, data: Value) -> Result<(), String>;
    fn set_label(&self, entry_id: &str, label: Option<&str>) -> Result<(), String>;
    /// Append (idle) or queue (running) a custom message; `trigger_turn`
    /// starts a run with it when idle.
    fn send_message(
        &self,
        message: CustomMessage,
        delivery: Delivery,
        trigger_turn: bool,
    ) -> Result<(), String>;
    /// Submit (idle) or queue (running, steer / follow-up) a user message.
    fn send_user_message(
        &self,
        content: Vec<ContentBlock>,
        delivery: Delivery,
    ) -> Result<(), String>;
    fn active_tools(&self) -> Vec<String>;
    fn all_tools(&self) -> Vec<ToolSpec>;
    fn set_active_tools(&self, names: &[String]) -> Result<(), String>;
    fn thinking_level(&self) -> String;
    /// Set the level (already validated), clamped to the model.
    fn set_thinking_level(&self, level: ThinkingLevel) -> Result<(), String>;
    /// Select a model; `Ok(false)` when it cannot be used.
    fn set_model(&self, provider: &str, id: &str) -> Result<bool, String>;
    /// Public metadata of the current model (see [`model_json`]).
    fn model(&self) -> Option<Value>;
    fn find_model(&self, provider: &str, id: &str) -> Option<Value>;
    /// `{providers, models}` without credentials or headers.
    fn model_catalog(&self) -> Value;
    fn system_prompt(&self) -> String;
    fn context_usage(&self) -> Option<ContextUsage>;
    fn context_messages(&self) -> Vec<AgentMessage>;
    /// Compact an idle session and wait for the result.
    fn compact(&self, custom_instructions: Option<&str>) -> Result<CompactionResult, String>;
    fn is_idle(&self) -> bool;
    fn has_pending_messages(&self) -> bool;
    fn abort(&self);
}

/// The archived public model shape: identity, capabilities and an allowlist
/// of metadata (context window, max tokens, input, cost, thinking map).
pub fn model_json(model: &ModelSpec) -> Value {
    let mut metadata = json!({});
    if model.context_window > 0 {
        metadata["contextWindow"] = json!(model.context_window);
    }
    if model.max_tokens > 0 {
        metadata["maxTokens"] = json!(model.max_tokens);
    }
    if !model.input.is_empty() {
        metadata["input"] = json!(model.input);
    }
    if !model.cost.is_zero() {
        metadata["cost"] = model.cost.to_json();
    }
    if !model.thinking_level_map.is_empty() {
        metadata["thinkingLevelMap"] = json!(model.thinking_level_map);
    }
    let mut out = json!({
        "provider": model.provider,
        "id": model.id,
        "name": model.display_name(),
        "api": model.api,
        "reasoning": model.reasoning,
        "capabilities": {
            "streaming": true,
            "toolCalls": true,
            "vision": model.supports_images(),
            "reasoning": model.reasoning,
            "sessionCaching": model.api.starts_with("anthropic") || model.api.contains("responses"),
        },
        "metadata": metadata.clone(),
    });
    for key in [
        "contextWindow",
        "maxTokens",
        "input",
        "cost",
        "thinkingLevelMap",
    ] {
        if let Some(v) = metadata.get(key) {
            out[key] = v.clone();
        }
    }
    out
}

/// `{providers, models}` of a catalog in the public shape.
pub fn catalog_json(catalog: &ModelCatalog) -> Value {
    let providers: Vec<Value> = catalog
        .providers
        .iter()
        .filter(|p| p.unsupported.is_none())
        .map(|p| {
            json!({
                "id": p.id,
                "name": p.name,
                "api": p.api,
                "capabilities": {"streaming": true, "toolCalls": true},
                "metadata": {},
            })
        })
        .collect();
    let models: Vec<Value> = catalog
        .models
        .iter()
        .filter(|m| {
            catalog
                .find_provider(&m.provider)
                .is_none_or(|p| p.unsupported.is_none())
        })
        .map(model_json)
        .collect();
    json!({ "providers": providers, "models": models })
}

/// `{tokens, contextWindow, percent}`; `null` without a context window.
pub fn usage_json(usage: Option<ContextUsage>) -> Value {
    match usage {
        None => Value::Null,
        Some(u) => json!({
            "tokens": u.tokens,
            "contextWindow": u.context_window,
            "percent": u.percent,
        }),
    }
}

/// Message content: a string or `text` / `image` (`mimeType` + base64 `data`) blocks.
pub fn parse_content(value: &Value) -> Result<Vec<ContentBlock>, String> {
    match value {
        Value::String(s) => Ok(vec![ContentBlock::text(s)]),
        Value::Array(blocks) => blocks
            .iter()
            .map(|b| {
                let invalid = || "extension message content block is invalid".to_string();
                match b.get("type").and_then(Value::as_str) {
                    Some("text") => Ok(ContentBlock::text(
                        b.get("text").and_then(Value::as_str).ok_or_else(invalid)?,
                    )),
                    Some("image") => {
                        let data = b.get("data").and_then(Value::as_str).ok_or_else(invalid)?;
                        let mime = b
                            .get("mimeType")
                            .or_else(|| b.get("mime_type"))
                            .and_then(Value::as_str)
                            .ok_or_else(invalid)?;
                        Ok(ContentBlock::Image {
                            data: data.into(),
                            mime_type: mime.into(),
                        })
                    }
                    _ => Err(invalid()),
                }
            })
            .collect(),
        _ => Err("extension message content must be a string or array".into()),
    }
}

/// Content blocks as `UserContent` (plain text when there is one text block).
pub fn user_content(blocks: Vec<ContentBlock>) -> UserContent {
    match blocks.as_slice() {
        [ContentBlock::Text { text }] => UserContent::Text(text.clone()),
        _ => UserContent::Blocks(blocks),
    }
}

/// Output of `pi.exec`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ExecOutput {
    pub stdout: String,
    pub stderr: String,
    pub code: i32,
    pub killed: bool,
}

/// Run `command args...` (no shell) in `cwd` with a timeout.
pub fn exec(
    command: &str,
    args: &[String],
    cwd: &std::path::Path,
    timeout: Duration,
) -> Result<ExecOutput, String> {
    use std::io::Read;
    let mut child = std::process::Command::new(command)
        .args(args)
        .current_dir(cwd)
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .map_err(|e| format!("{command}: {e}"))?;
    let read = |mut r: Box<dyn Read + Send>| {
        std::thread::spawn(move || {
            let mut buf = Vec::new();
            let _ = r.read_to_end(&mut buf);
            String::from_utf8_lossy(&buf).into_owned()
        })
    };
    let out = read(Box::new(child.stdout.take().expect("piped stdout")));
    let err = read(Box::new(child.stderr.take().expect("piped stderr")));
    let started = Instant::now();
    let mut killed = false;
    let status = loop {
        if let Some(status) = child.try_wait().map_err(|e| e.to_string())? {
            break status;
        }
        if started.elapsed() >= timeout {
            let _ = child.kill();
            killed = true;
            break child.wait().map_err(|e| e.to_string())?;
        }
        std::thread::sleep(Duration::from_millis(5));
    };
    Ok(ExecOutput {
        stdout: out.join().unwrap_or_default(),
        stderr: err.join().unwrap_or_default(),
        code: status.code().unwrap_or(-1),
        killed,
    })
}

/// Receives the event stream of runs an extension starts (prompts, triggered
/// custom messages) so the front end can render them.
pub type RunSink = Arc<dyn Fn(Receiver<AgentEvent>) + Send + Sync>;

/// [`ExtensionHost`] over a `qt_core` conversation.
pub struct ConversationHost {
    conversation: Conversation,
    catalog: Arc<ModelCatalog>,
    run_sink: Option<RunSink>,
}

impl ConversationHost {
    /// The catalog is loaded from the engine's agent home and settings.
    pub fn new(conversation: Conversation) -> Self {
        let engine = conversation.engine();
        let (catalog, _) = ModelCatalog::load(engine.home(), &engine.settings());
        Self::with_catalog(conversation, Arc::new(catalog))
    }

    pub fn with_catalog(conversation: Conversation, catalog: Arc<ModelCatalog>) -> Self {
        Self {
            conversation,
            catalog,
            run_sink: None,
        }
    }

    /// Where event streams of extension-started runs go (dropped otherwise;
    /// the run still completes and is persisted).
    pub fn with_run_sink(mut self, sink: RunSink) -> Self {
        self.run_sink = Some(sink);
        self
    }

    pub fn conversation(&self) -> &Conversation {
        &self.conversation
    }

    fn deliver(&self, rx: Receiver<AgentEvent>) {
        if let Some(sink) = &self.run_sink {
            sink(rx);
        }
    }

    fn spec(&self, model: &ModelRef) -> ModelSpec {
        self.catalog
            .find_model(&model.provider, &model.id)
            .cloned()
            .unwrap_or_else(|| ModelSpec {
                provider: model.provider.clone(),
                id: model.id.clone(),
                context_window: self
                    .conversation
                    .engine()
                    .context_window(model)
                    .unwrap_or(0),
                input: vec!["text".into()],
                ..Default::default()
            })
    }
}

impl ExtensionHost for ConversationHost {
    fn cwd(&self) -> PathBuf {
        self.conversation.engine().cwd().to_path_buf()
    }

    fn session_name(&self) -> Option<String> {
        self.conversation.name()
    }

    fn set_session_name(&self, name: &str) -> Result<(), String> {
        self.conversation.set_name(name).map_err(|e| e.to_string())
    }

    fn append_entry(&self, custom_type: &str, data: Value) -> Result<(), String> {
        self.conversation
            .append_custom_entry(custom_type, (!data.is_null()).then_some(data))
            .map_err(|e| e.to_string())
    }

    fn set_label(&self, entry_id: &str, label: Option<&str>) -> Result<(), String> {
        self.conversation
            .set_label(entry_id, label)
            .map_err(|e| e.to_string())
    }

    fn send_message(
        &self,
        message: CustomMessage,
        delivery: Delivery,
        trigger_turn: bool,
    ) -> Result<(), String> {
        if let Some(rx) = self
            .conversation
            .send_custom_message(message, delivery, trigger_turn)
            .map_err(|e| e.to_string())?
        {
            self.deliver(rx);
        }
        Ok(())
    }

    fn send_user_message(
        &self,
        content: Vec<ContentBlock>,
        delivery: Delivery,
    ) -> Result<(), String> {
        let mut text = String::new();
        let mut images = Vec::new();
        for block in content {
            match block {
                ContentBlock::Text { text: t } => {
                    if !text.is_empty() {
                        text.push('\n');
                    }
                    text.push_str(&t);
                }
                image @ ContentBlock::Image { .. } => images.push(image),
                _ => {}
            }
        }
        if self.conversation.is_running() {
            return match delivery {
                Delivery::FollowUp => self.conversation.follow_up(&text, images),
                _ => self.conversation.steer(&text, images),
            }
            .map_err(|e| e.to_string());
        }
        let rx = self
            .conversation
            .prompt(&text, images)
            .map_err(|e| e.to_string())?;
        self.deliver(rx);
        Ok(())
    }

    fn active_tools(&self) -> Vec<String> {
        self.conversation.active_tools()
    }

    fn all_tools(&self) -> Vec<ToolSpec> {
        self.conversation.engine().tools().specs()
    }

    fn set_active_tools(&self, names: &[String]) -> Result<(), String> {
        self.conversation
            .set_active_tools(names)
            .map_err(|e| e.to_string())
    }

    fn thinking_level(&self) -> String {
        self.conversation.thinking().as_str().into()
    }

    fn set_thinking_level(&self, level: ThinkingLevel) -> Result<(), String> {
        let level = match self.conversation.model() {
            Some(m) => self.spec(&m).clamp_thinking(level),
            None => level,
        };
        self.conversation.set_thinking(level);
        Ok(())
    }

    fn set_model(&self, provider: &str, id: &str) -> Result<bool, String> {
        let engine = self.conversation.engine();
        let Some(config) = engine.provider(provider) else {
            return Err(format!("Unknown provider {provider}"));
        };
        if !config.is_ready() {
            return Ok(false);
        }
        self.conversation.set_model(ModelRef::new(provider, id));
        Ok(true)
    }

    fn model(&self) -> Option<Value> {
        self.conversation
            .model()
            .map(|m| model_json(&self.spec(&m)))
    }

    fn find_model(&self, provider: &str, id: &str) -> Option<Value> {
        self.catalog.find_model(provider, id).map(model_json)
    }

    fn model_catalog(&self) -> Value {
        catalog_json(&self.catalog)
    }

    fn system_prompt(&self) -> String {
        self.conversation.system_prompt()
    }

    fn context_usage(&self) -> Option<ContextUsage> {
        self.conversation.context_usage()
    }

    fn context_messages(&self) -> Vec<AgentMessage> {
        self.conversation.agent_messages()
    }

    fn compact(&self, custom_instructions: Option<&str>) -> Result<CompactionResult, String> {
        if tokio::runtime::Handle::try_current().is_ok() {
            return Err("ctx.compact cannot wait on the engine runtime thread".into());
        }
        let rx = self
            .conversation
            .compact(custom_instructions)
            .map_err(|e| e.to_string())?;
        let mut outcome: Option<Result<CompactionResult, String>> = None;
        while let Ok(event) = rx.recv_blocking() {
            if let AgentEvent::CompactionEnd {
                result,
                aborted,
                error,
                ..
            } = event
            {
                outcome = Some(match (result, error) {
                    (Some(r), _) => Ok(r),
                    (None, Some(e)) => Err(e),
                    (None, None) if aborted => Err("Compaction aborted".into()),
                    (None, None) => Err("Compaction did not produce a result".into()),
                });
            }
        }
        outcome.unwrap_or_else(|| Err("Compaction did not produce a result".into()))
    }

    fn is_idle(&self) -> bool {
        !self.conversation.is_running()
    }

    fn has_pending_messages(&self) -> bool {
        !self.conversation.queue().is_empty()
    }

    fn abort(&self) {
        self.conversation.abort();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn content_parsing() {
        assert_eq!(
            parse_content(&json!("hi")).unwrap(),
            vec![ContentBlock::text("hi")]
        );
        let blocks = parse_content(&json!([{"type":"text","text":"a"},{"type":"image","mime_type":"image/png","data":"AA=="}])).unwrap();
        assert_eq!(blocks.len(), 2);
        assert!(matches!(user_content(blocks), UserContent::Blocks(_)));
        assert_eq!(
            parse_content(&json!([{"type":"video"}])).unwrap_err(),
            "extension message content block is invalid"
        );
        assert_eq!(
            parse_content(&json!(1)).unwrap_err(),
            "extension message content must be a string or array"
        );
    }

    #[test]
    fn model_shape_has_no_credentials() {
        let spec = ModelSpec {
            provider: "p".into(),
            id: "m".into(),
            api: "openai-completions".into(),
            context_window: 1000,
            api_key: Some("SECRET".into()),
            headers: [("x-key".to_string(), "SECRET".to_string())].into(),
            input: vec!["text".into(), "image".into()],
            ..Default::default()
        };
        let v = model_json(&spec);
        assert_eq!(v["name"], json!("m"));
        assert_eq!(v["contextWindow"], json!(1000));
        assert_eq!(v["capabilities"]["vision"], json!(true));
        assert!(!v.to_string().contains("SECRET"));
        assert!(
            !catalog_json(ModelCatalog::builtin())
                .to_string()
                .to_lowercase()
                .contains("apikey")
        );
    }

    #[cfg(unix)]
    #[test]
    fn exec_runs_argv_with_timeout() {
        let dir = std::env::temp_dir();
        let r = exec(
            "sh",
            &["-c".into(), "echo out; echo err >&2; exit 4".into()],
            &dir,
            Duration::from_secs(10),
        )
        .unwrap();
        assert_eq!(
            (r.stdout.as_str(), r.stderr.as_str(), r.code, r.killed),
            ("out\n", "err\n", 4, false)
        );
        let r = exec("sleep", &["5".into()], &dir, Duration::from_millis(50)).unwrap();
        assert!(r.killed);
    }
}
