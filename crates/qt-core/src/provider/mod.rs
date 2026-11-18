//! Model providers.
//!
//! Two wire protocols cover the supported back ends:
//! - [`anthropic`]: the Anthropic Messages API.
//! - [`openai`]: OpenAI-compatible chat completions, used for OpenAI,
//!   OpenRouter and Ollama instances.
//!
//! Every stream call returns a complete [`AssistantMessage`]; transport and
//! API failures become `stopReason: "error"` messages and cancellation
//! becomes `stopReason: "aborted"` with whatever content had streamed.

pub mod anthropic;
pub mod models;
pub mod openai;

use std::collections::HashSet;
use std::fmt;

use serde_json::Value;
use tokio_util::sync::CancellationToken;

use crate::config::{AuthStore, Settings};
use crate::message::{AssistantMessage, ContentBlock, Message, StopReason, now_millis};

pub use models::{ModelInfo, list_models};

/// Wire protocol spoken by a provider.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProviderKind {
    Anthropic,
    OpenAi,
    OpenRouter,
    Ollama,
}

impl ProviderKind {
    /// The `api` string recorded on assistant messages (Pi naming).
    pub fn api_name(self) -> &'static str {
        match self {
            Self::Anthropic => "anthropic-messages",
            Self::OpenAi | Self::OpenRouter | Self::Ollama => "openai-completions",
        }
    }
}

/// A configured provider endpoint.
#[derive(Clone, PartialEq, Eq)]
pub struct ProviderConfig {
    pub id: String,
    pub label: String,
    pub kind: ProviderKind,
    pub base_url: String,
    pub api_key: Option<String>,
}

impl fmt::Debug for ProviderConfig {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("ProviderConfig")
            .field("id", &self.id)
            .field("kind", &self.kind)
            .field("base_url", &self.base_url)
            .field("api_key", &self.api_key.as_ref().map(|_| "<set>"))
            .finish()
    }
}

impl ProviderConfig {
    /// Whether requests can be made (local providers need no key).
    pub fn is_ready(&self) -> bool {
        self.kind == ProviderKind::Ollama || self.api_key.is_some()
    }

    pub fn needs_api_key(&self) -> bool {
        self.kind != ProviderKind::Ollama
    }
}

/// Providers that accept an API key from the user, in display order.
pub const KEYED_PROVIDERS: [(&str, &str); 3] = [
    ("anthropic", "Anthropic"),
    ("openai", "OpenAI"),
    ("openrouter", "OpenRouter"),
];

/// Build the provider list from settings and credentials.
pub fn configured_providers(settings: &Settings, auth: &AuthStore) -> Vec<ProviderConfig> {
    let base_override = |id: &str| {
        settings
            .raw()
            .get("providers")
            .and_then(|p| p.get(id))
            .and_then(|p| p.get("baseUrl"))
            .and_then(Value::as_str)
            .map(|s| s.trim_end_matches('/').to_string())
    };
    let mut providers = vec![
        ProviderConfig {
            id: "anthropic".into(),
            label: "Anthropic".into(),
            kind: ProviderKind::Anthropic,
            base_url: base_override("anthropic")
                .unwrap_or_else(|| "https://api.anthropic.com".into()),
            api_key: auth.api_key("anthropic"),
        },
        ProviderConfig {
            id: "openai".into(),
            label: "OpenAI".into(),
            kind: ProviderKind::OpenAi,
            base_url: base_override("openai").unwrap_or_else(|| "https://api.openai.com/v1".into()),
            api_key: auth.api_key("openai"),
        },
        ProviderConfig {
            id: "openrouter".into(),
            label: "OpenRouter".into(),
            kind: ProviderKind::OpenRouter,
            base_url: base_override("openrouter")
                .unwrap_or_else(|| "https://openrouter.ai/api/v1".into()),
            api_key: auth.api_key("openrouter"),
        },
    ];
    let mut instances = settings.ollama_instances();
    if instances.is_empty() {
        instances.push(crate::config::OllamaInstance {
            id: "local".into(),
            base_url: "http://127.0.0.1:11434".into(),
            models: Vec::new(),
        });
    }
    for (i, inst) in instances.iter().enumerate() {
        let id = if i == 0 {
            "ollama".to_string()
        } else {
            format!("ollama:{}", inst.id)
        };
        providers.push(ProviderConfig {
            label: if i == 0 {
                "Ollama".into()
            } else {
                format!("Ollama ({})", inst.id)
            },
            id,
            kind: ProviderKind::Ollama,
            base_url: inst.base_url.clone(),
            api_key: None,
        });
    }
    providers
}

/// Identifies a model on a provider.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct ModelRef {
    pub provider: String,
    pub id: String,
}

impl ModelRef {
    pub fn new(provider: impl Into<String>, id: impl Into<String>) -> Self {
        Self {
            provider: provider.into(),
            id: id.into(),
        }
    }

    /// Parse `provider/model`; the model part may itself contain slashes.
    pub fn parse(spec: &str) -> Option<Self> {
        let (provider, id) = spec.split_once('/')?;
        (!provider.is_empty() && !id.is_empty()).then(|| Self::new(provider, id))
    }
}

impl fmt::Display for ModelRef {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}/{}", self.provider, self.id)
    }
}

/// Reasoning effort requested from models that support it.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum ThinkingLevel {
    /// Leave the provider default in place.
    #[default]
    Off,
    Low,
    Medium,
    High,
}

impl ThinkingLevel {
    pub const ALL: [ThinkingLevel; 4] = [Self::Off, Self::Low, Self::Medium, Self::High];

    pub fn as_str(self) -> &'static str {
        match self {
            Self::Off => "off",
            Self::Low => "low",
            Self::Medium => "medium",
            Self::High => "high",
        }
    }

    pub fn parse(s: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|l| l.as_str() == s)
    }

    /// Anthropic extended-thinking budget.
    pub fn budget_tokens(self) -> Option<u32> {
        match self {
            Self::Off => None,
            Self::Low => Some(4_096),
            Self::Medium => Some(12_288),
            Self::High => Some(28_672),
        }
    }
}

/// JSON-schema description of a tool offered to the model.
#[derive(Debug, Clone, PartialEq)]
pub struct ToolSpec {
    pub name: String,
    pub description: String,
    pub parameters: Value,
}

/// Everything a provider needs for one completion.
///
/// Construct with `..Default::default()` so new fields stay source-compatible.
#[derive(Debug, Clone, Default)]
pub struct Request {
    pub model: String,
    pub system: String,
    pub messages: Vec<Message>,
    pub tools: Vec<ToolSpec>,
    pub max_tokens: u32,
    pub thinking: ThinkingLevel,
}

/// Incremental updates while an assistant message streams.
#[derive(Debug, Clone, PartialEq)]
pub enum StreamEvent {
    /// A new content block was opened at `index`.
    BlockStart {
        index: usize,
        block: ContentBlock,
    },
    TextDelta {
        index: usize,
        delta: String,
    },
    ThinkingDelta {
        index: usize,
        delta: String,
    },
    /// Raw JSON argument text for the tool call at `index`.
    ToolArgsDelta {
        index: usize,
        delta: String,
    },
}

/// Stream one assistant response.
pub async fn stream(
    http: &reqwest::Client,
    provider: &ProviderConfig,
    request: &Request,
    cancel: &CancellationToken,
    on_event: &mut (dyn FnMut(StreamEvent) + Send),
) -> AssistantMessage {
    let mut partial = PartialMessage::new(provider, &request.model);
    let result = match provider.kind {
        ProviderKind::Anthropic => {
            anthropic::stream(http, provider, request, cancel, &mut partial, on_event).await
        }
        _ => openai::stream(http, provider, request, cancel, &mut partial, on_event).await,
    };
    match result {
        Ok(()) if cancel.is_cancelled() => partial.finish_aborted(),
        Ok(()) => partial.finish(),
        Err(_) if cancel.is_cancelled() => partial.finish_aborted(),
        Err(err) => partial.finish_error(format!("{err:#}")),
    }
}

/// Accumulates streamed content into an [`AssistantMessage`].
#[derive(Debug)]
pub struct PartialMessage {
    pub message: AssistantMessage,
    /// Raw JSON argument text per content index.
    tool_args: Vec<(usize, String)>,
    stop: Option<StopReason>,
}

impl PartialMessage {
    pub fn new(provider: &ProviderConfig, model: &str) -> Self {
        Self {
            message: AssistantMessage {
                api: provider.kind.api_name().into(),
                provider: provider.id.clone(),
                model: model.to_string(),
                ..Default::default()
            },
            tool_args: Vec::new(),
            stop: None,
        }
    }

    pub fn open(
        &mut self,
        block: ContentBlock,
        on_event: &mut (dyn FnMut(StreamEvent) + Send),
    ) -> usize {
        let index = self.message.content.len();
        if matches!(block, ContentBlock::ToolCall { .. }) {
            self.tool_args.push((index, String::new()));
        }
        self.message.content.push(block.clone());
        on_event(StreamEvent::BlockStart { index, block });
        index
    }

    pub fn push_text(
        &mut self,
        index: usize,
        delta: &str,
        on_event: &mut (dyn FnMut(StreamEvent) + Send),
    ) {
        if delta.is_empty() {
            return;
        }
        if let Some(ContentBlock::Text { text }) = self.message.content.get_mut(index) {
            text.push_str(delta);
            on_event(StreamEvent::TextDelta {
                index,
                delta: delta.to_string(),
            });
        }
    }

    pub fn push_thinking(
        &mut self,
        index: usize,
        delta: &str,
        on_event: &mut (dyn FnMut(StreamEvent) + Send),
    ) {
        if delta.is_empty() {
            return;
        }
        if let Some(ContentBlock::Thinking { thinking, .. }) = self.message.content.get_mut(index) {
            thinking.push_str(delta);
            on_event(StreamEvent::ThinkingDelta {
                index,
                delta: delta.to_string(),
            });
        }
    }

    pub fn set_signature(&mut self, index: usize, sig: &str) {
        if let Some(ContentBlock::Thinking { signature, .. }) = self.message.content.get_mut(index)
        {
            signature.get_or_insert_with(String::new).push_str(sig);
        }
    }

    pub fn push_tool_args(
        &mut self,
        index: usize,
        delta: &str,
        on_event: &mut (dyn FnMut(StreamEvent) + Send),
    ) {
        if let Some((_, args)) = self.tool_args.iter_mut().find(|(i, _)| *i == index) {
            args.push_str(delta);
            on_event(StreamEvent::ToolArgsDelta {
                index,
                delta: delta.to_string(),
            });
        }
    }

    pub fn set_stop(&mut self, stop: StopReason) {
        self.stop = Some(stop);
    }

    fn finalize_tool_args(&mut self) {
        for (index, raw) in std::mem::take(&mut self.tool_args) {
            if let Some(ContentBlock::ToolCall { arguments, .. }) =
                self.message.content.get_mut(index)
            {
                *arguments = parse_tool_arguments(&raw);
            }
        }
    }

    pub fn finish(mut self) -> AssistantMessage {
        self.finalize_tool_args();
        let has_calls = self.message.tool_calls().next().is_some();
        self.message.stop_reason = match self.stop {
            Some(StopReason::Stop) | None if has_calls => StopReason::ToolCall,
            Some(stop) => stop,
            None => StopReason::Stop,
        };
        self.message.timestamp = Some(now_millis());
        self.message
    }

    pub fn finish_aborted(mut self) -> AssistantMessage {
        self.finalize_tool_args();
        self.message.stop_reason = StopReason::Aborted;
        self.message.timestamp = Some(now_millis());
        self.message
    }

    pub fn finish_error(mut self, error: String) -> AssistantMessage {
        self.finalize_tool_args();
        self.message.stop_reason = StopReason::Error;
        self.message.error_message = Some(error);
        self.message.timestamp = Some(now_millis());
        self.message
    }
}

/// Parse streamed tool arguments; invalid JSON is kept as a string so the
/// tool reports a validation error to the model instead of silently running.
pub fn parse_tool_arguments(raw: &str) -> Value {
    if raw.trim().is_empty() {
        return Value::Object(Default::default());
    }
    serde_json::from_str(raw).unwrap_or_else(|_| Value::String(raw.to_string()))
}

/// Make a history safe to send: drop tool calls from failed or aborted
/// assistant turns, drop tool results without a matching call, and drop
/// assistant messages left empty.
pub fn sanitize_history(messages: &[Message]) -> Vec<Message> {
    let mut out = Vec::with_capacity(messages.len());
    let mut open_calls: HashSet<String> = HashSet::new();
    for message in messages {
        match message {
            Message::Assistant(a) => {
                let failed = matches!(a.stop_reason, StopReason::Error | StopReason::Aborted);
                let content: Vec<ContentBlock> = a
                    .content
                    .iter()
                    .filter(|b| match b {
                        ContentBlock::ToolCall { .. } => !failed,
                        ContentBlock::Text { text } => !text.is_empty(),
                        ContentBlock::Thinking {
                            thinking, redacted, ..
                        } => !thinking.is_empty() || *redacted,
                        ContentBlock::Image { .. } => true,
                    })
                    .cloned()
                    .collect();
                if content.is_empty() {
                    continue;
                }
                open_calls.clear();
                for block in &content {
                    if let ContentBlock::ToolCall { id, .. } = block {
                        open_calls.insert(id.clone());
                    }
                }
                out.push(Message::Assistant(AssistantMessage {
                    content,
                    ..a.clone()
                }));
            }
            Message::ToolResult(r) => {
                if open_calls.remove(&r.tool_call_id) {
                    out.push(message.clone());
                }
            }
            Message::User { .. } => {
                open_calls.clear();
                out.push(message.clone());
            }
        }
    }
    out
}

/// Read an error body from a failed HTTP response into a short message.
pub(crate) async fn http_error(response: reqwest::Response) -> anyhow::Error {
    let status = response.status();
    let body = response.text().await.unwrap_or_default();
    let detail = serde_json::from_str::<Value>(&body)
        .ok()
        .and_then(|v| {
            v.get("error")
                .and_then(|e| {
                    e.get("message")
                        .and_then(Value::as_str)
                        .map(str::to_string)
                        .or_else(|| e.as_str().map(str::to_string))
                })
                .or_else(|| v.get("message").and_then(Value::as_str).map(str::to_string))
        })
        .unwrap_or_else(|| crate::util::shorten(body.trim(), 500));
    anyhow::anyhow!("HTTP {status}: {detail}")
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::message::ToolResultMessage;

    #[test]
    fn model_ref_parses_nested_ids() {
        let r = ModelRef::parse("openrouter/~deepseek/deepseek-flash-latest").unwrap();
        assert_eq!(r.provider, "openrouter");
        assert_eq!(r.id, "~deepseek/deepseek-flash-latest");
        assert!(ModelRef::parse("nomodel").is_none());
    }

    #[test]
    fn sanitize_drops_orphans() {
        let aborted = Message::Assistant(AssistantMessage {
            content: vec![ContentBlock::ToolCall {
                id: "a".into(),
                name: "read".into(),
                arguments: Value::Null,
            }],
            stop_reason: StopReason::Aborted,
            ..Default::default()
        });
        let orphan = Message::ToolResult(ToolResultMessage {
            tool_call_id: "a".into(),
            ..Default::default()
        });
        let good_call = Message::Assistant(AssistantMessage {
            content: vec![ContentBlock::ToolCall {
                id: "b".into(),
                name: "read".into(),
                arguments: Value::Null,
            }],
            stop_reason: StopReason::ToolCall,
            ..Default::default()
        });
        let good_result = Message::ToolResult(ToolResultMessage {
            tool_call_id: "b".into(),
            ..Default::default()
        });
        let out = sanitize_history(&[
            Message::user_text("hi"),
            aborted,
            orphan,
            good_call,
            good_result,
        ]);
        assert_eq!(out.len(), 3);
    }

    #[test]
    fn finish_infers_tool_call_stop() {
        let cfg = ProviderConfig {
            id: "x".into(),
            label: "x".into(),
            kind: ProviderKind::OpenAi,
            base_url: String::new(),
            api_key: None,
        };
        let mut p = PartialMessage::new(&cfg, "m");
        let mut sink = |_e: StreamEvent| {};
        let i = p.open(
            ContentBlock::ToolCall {
                id: "1".into(),
                name: "read".into(),
                arguments: Value::Null,
            },
            &mut sink,
        );
        p.push_tool_args(i, "{\"path\":", &mut sink);
        p.push_tool_args(i, "\"a\"}", &mut sink);
        let m = p.finish();
        assert_eq!(m.stop_reason, StopReason::ToolCall);
        let (_, _, args) = m.tool_calls().next().unwrap();
        assert_eq!(args["path"], "a");
    }
}
