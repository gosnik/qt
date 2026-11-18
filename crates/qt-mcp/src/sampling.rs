//! Server-initiated `sampling/createMessage`.
//!
//! Servers may ask the client to run a model completion. The manager forwards such
//! requests to a [`SamplingHandler`]; [`ProviderSamplingHandler`] answers them with a
//! qt-core provider, applying the per-server `sampling` policy (`enabled`, `model`,
//! `allowed_models`, `max_tokens_cap`, `timeout`) as the archived qt did.

use std::sync::{Arc, RwLock};
use std::time::Duration;

use futures::future::BoxFuture;
use qt_core::message::{
    AssistantMessage, ContentBlock, Message, StopReason, ToolResultMessage, UserContent,
};
use qt_core::provider::{self, ProviderConfig, Request, ToolSpec};
use serde_json::{Value, json};
use tokio_util::sync::CancellationToken;

use crate::util::sanitize_mcp_error;

/// One `sampling/createMessage` request from a server.
#[derive(Debug, Clone, PartialEq)]
pub struct SamplingRequest {
    /// Configured name of the requesting server.
    pub server_name: String,
    /// The JSON-RPC `params` (`messages`, `systemPrompt`, `maxTokens`, ...).
    pub params: Value,
    /// The server's `sampling` policy object from its configuration.
    pub config: Value,
}

/// Answers sampling requests. The `Ok` value is the JSON-RPC `result`
/// (`{role, content, model, stopReason}`); `Err` becomes a `-32000` error.
pub trait SamplingHandler: Send + Sync {
    fn create_message(&self, request: SamplingRequest)
    -> BoxFuture<'static, Result<Value, String>>;
}

/// Shared, replaceable handler slot (the manager installs it after a model is chosen).
pub(crate) type SamplingSlot = Arc<RwLock<Option<Arc<dyn SamplingHandler>>>>;

pub(crate) fn current_handler(slot: &SamplingSlot) -> Option<Arc<dyn SamplingHandler>> {
    slot.read().unwrap_or_else(|e| e.into_inner()).clone()
}

/// Default `max_tokens_cap` when the policy sets none.
const DEFAULT_MAX_TOKENS_CAP: i64 = 4096;
/// Default sampling `timeout` (seconds).
const DEFAULT_SAMPLING_TIMEOUT_SECONDS: i64 = 30;

/// Runs sampling requests against one provider/model.
#[derive(Clone)]
pub struct ProviderSamplingHandler {
    http: reqwest::Client,
    provider: ProviderConfig,
    model: String,
}

impl ProviderSamplingHandler {
    pub fn new(http: reqwest::Client, provider: ProviderConfig, model: impl Into<String>) -> Self {
        Self {
            http,
            provider,
            model: model.into(),
        }
    }
}

impl SamplingHandler for ProviderSamplingHandler {
    fn create_message(
        &self,
        request: SamplingRequest,
    ) -> BoxFuture<'static, Result<Value, String>> {
        let this = self.clone();
        Box::pin(async move { this.run(request).await })
    }
}

impl ProviderSamplingHandler {
    async fn run(&self, sampling: SamplingRequest) -> Result<Value, String> {
        let config = &sampling.config;
        let params = &sampling.params;
        let requested = requested_model(config, params);
        if let (Some(allowed), Some(requested)) = (
            config.get("allowed_models").and_then(Value::as_array),
            requested.as_deref(),
        ) && !allowed.iter().any(|m| m.as_str() == Some(requested))
        {
            return Err(format!(
                "MCP sampling requested disallowed model: {requested}"
            ));
        }
        let request = build_request(&self.model, params, config);
        let timeout_seconds = config
            .get("timeout")
            .and_then(Value::as_i64)
            .unwrap_or(DEFAULT_SAMPLING_TIMEOUT_SECONDS)
            .max(0) as u64;
        let cancel = CancellationToken::new();
        let mut ignore_events = |_| {};
        let stream = provider::stream(
            &self.http,
            &self.provider,
            &request,
            &cancel,
            &mut ignore_events,
        );
        let message = match tokio::time::timeout(Duration::from_secs(timeout_seconds), stream).await
        {
            Ok(message) => message,
            Err(_) => {
                cancel.cancel();
                return Err("MCP sampling exceeded configured timeout".into());
            }
        };
        if message.stop_reason == StopReason::Error {
            return Err(sanitize_mcp_error(&format!(
                "MCP sampling failed: {}",
                message.error_message.as_deref().unwrap_or("unknown error")
            )));
        }
        Ok(sampling_result(&message, &self.model))
    }
}

/// `sampling.model`, else the first `modelPreferences.hints[].name`.
fn requested_model(config: &Value, params: &Value) -> Option<String> {
    if let Some(model) = config.get("model").and_then(Value::as_str) {
        return Some(model.to_string());
    }
    params
        .get("modelPreferences")
        .and_then(|p| p.get("hints"))
        .and_then(Value::as_array)?
        .iter()
        .find_map(|hint| hint.get("name").and_then(Value::as_str))
        .map(str::to_string)
}

/// Translate MCP sampling params into a provider request.
pub(crate) fn build_request(model: &str, params: &Value, config: &Value) -> Request {
    let mut system: Vec<String> = Vec::new();
    if let Some(prompt) = params.get("systemPrompt").and_then(Value::as_str) {
        system.push(prompt.to_string());
    }
    let mut messages = Vec::new();
    for message in params
        .get("messages")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .filter(|m| m.is_object())
    {
        convert_message(message, &mut system, &mut messages);
    }
    let cap = config
        .get("max_tokens_cap")
        .and_then(Value::as_i64)
        .unwrap_or(DEFAULT_MAX_TOKENS_CAP);
    let max_tokens = params
        .get("maxTokens")
        .and_then(Value::as_i64)
        .unwrap_or(cap)
        .min(cap)
        .clamp(1, i64::from(u32::MAX)) as u32;
    let tools = params
        .get("tools")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .filter_map(|tool| {
            let name = tool.get("name")?.as_str()?;
            Some(ToolSpec {
                name: name.to_string(),
                description: tool
                    .get("description")
                    .and_then(Value::as_str)
                    .unwrap_or_default()
                    .to_string(),
                parameters: tool
                    .get("inputSchema")
                    .cloned()
                    .unwrap_or_else(|| json!({"type": "object", "additionalProperties": true})),
            })
        })
        .collect();
    Request {
        model: model.to_string(),
        system: system.join("\n\n"),
        messages,
        tools,
        max_tokens,
        ..Default::default()
    }
}

fn content_items(content: Option<&Value>) -> Vec<&Value> {
    match content {
        Some(Value::Array(items)) => items.iter().collect(),
        Some(other) => vec![other],
        None => Vec::new(),
    }
}

fn block_text(item: &Value) -> Option<String> {
    match item {
        Value::String(s) => Some(s.clone()),
        Value::Object(o) if o.get("type").and_then(Value::as_str).unwrap_or("text") == "text" => {
            Some(
                o.get("text")
                    .and_then(Value::as_str)
                    .unwrap_or_default()
                    .to_string(),
            )
        }
        _ => None,
    }
}

fn image_block(item: &Value) -> Option<ContentBlock> {
    if item.get("type").and_then(Value::as_str) != Some("image") {
        return None;
    }
    let data = item.get("data").and_then(Value::as_str).unwrap_or_default();
    if data.is_empty() {
        return None;
    }
    let mime = item
        .get("mimeType")
        .or_else(|| item.get("media_type"))
        .and_then(Value::as_str)
        .unwrap_or("image/png");
    Some(ContentBlock::Image {
        data: data.to_string(),
        mime_type: mime.to_string(),
    })
}

fn convert_message(message: &Value, system: &mut Vec<String>, out: &mut Vec<Message>) {
    let role = message
        .get("role")
        .and_then(Value::as_str)
        .unwrap_or("user");
    let items = content_items(message.get("content"));
    match role {
        "system" | "developer" => {
            let text: Vec<String> = items.iter().filter_map(|i| block_text(i)).collect();
            if !text.is_empty() {
                system.push(text.join("\n"));
            }
        }
        "assistant" => {
            let mut content = Vec::new();
            for item in items {
                if let Some(text) = block_text(item) {
                    content.push(ContentBlock::text(text));
                } else if item.get("type").and_then(Value::as_str) == Some("tool_use") {
                    content.push(ContentBlock::ToolCall {
                        id: item
                            .get("id")
                            .and_then(Value::as_str)
                            .unwrap_or("mcp_sampling_tool")
                            .to_string(),
                        name: item
                            .get("name")
                            .and_then(Value::as_str)
                            .unwrap_or_default()
                            .to_string(),
                        arguments: item.get("input").cloned().unwrap_or_else(|| json!({})),
                    });
                }
            }
            out.push(Message::Assistant(AssistantMessage {
                content,
                stop_reason: StopReason::Stop,
                ..Default::default()
            }));
        }
        _ => {
            let mut blocks = Vec::new();
            for item in items {
                if item.get("type").and_then(Value::as_str) == Some("tool_result") {
                    if !blocks.is_empty() {
                        out.push(user_message(std::mem::take(&mut blocks)));
                    }
                    out.push(Message::ToolResult(tool_result(item)));
                } else if let Some(image) = image_block(item) {
                    blocks.push(image);
                } else if let Some(text) = block_text(item) {
                    blocks.push(ContentBlock::text(text));
                }
            }
            if !blocks.is_empty() {
                out.push(user_message(blocks));
            }
        }
    }
}

fn user_message(blocks: Vec<ContentBlock>) -> Message {
    Message::User {
        content: UserContent::Blocks(blocks),
        timestamp: None,
    }
}

fn tool_result(item: &Value) -> ToolResultMessage {
    let content = match item.get("content") {
        Some(Value::String(s)) => vec![ContentBlock::text(s.clone())],
        Some(Value::Array(blocks)) => blocks
            .iter()
            .filter_map(|b| image_block(b).or_else(|| block_text(b).map(ContentBlock::text)))
            .collect(),
        Some(other) => vec![ContentBlock::text(other.to_string())],
        None => Vec::new(),
    };
    ToolResultMessage {
        tool_call_id: item
            .get("toolUseId")
            .and_then(Value::as_str)
            .unwrap_or_default()
            .to_string(),
        tool_name: item
            .get("name")
            .and_then(Value::as_str)
            .unwrap_or_default()
            .to_string(),
        content,
        is_error: item
            .get("isError")
            .and_then(Value::as_bool)
            .unwrap_or(false),
        ..Default::default()
    }
}

/// Build the `CreateMessageResult` from the provider response.
pub(crate) fn sampling_result(message: &AssistantMessage, model: &str) -> Value {
    let calls: Vec<Value> = message
        .tool_calls()
        .map(
            |(id, name, input)| json!({"type": "tool_use", "id": id, "name": name, "input": input}),
        )
        .collect();
    let (content, stop_reason) = if calls.is_empty() {
        let stop = if message.stop_reason == StopReason::Length {
            "maxTokens"
        } else {
            "endTurn"
        };
        (
            json!({"type": "text", "text": sanitize_mcp_error(&message.text())}),
            stop,
        )
    } else {
        let stop = if message.stop_reason == StopReason::Length {
            "maxTokens"
        } else {
            "toolUse"
        };
        (Value::Array(calls), stop)
    };
    let model = if message.model.is_empty() {
        model
    } else {
        message.model.as_str()
    };
    json!({"role": "assistant", "content": content, "model": model, "stopReason": stop_reason})
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn builds_provider_request_from_sampling_params() {
        let params = json!({
            "systemPrompt": "Be brief.",
            "maxTokens": 99999,
            "messages": [
                {"role": "system", "content": {"type": "text", "text": "extra"}},
                {"role": "user", "content": {"type": "text", "text": "hi"}},
                {"role": "user", "content": [
                    {"type": "image", "data": "AAA", "mimeType": "image/jpeg"},
                    {"type": "image", "data": ""}
                ]},
                {"role": "assistant", "content": [
                    {"type": "text", "text": "calling"},
                    {"type": "tool_use", "id": "t1", "name": "look", "input": {"q": 1}}
                ]},
                {"role": "user", "content": [
                    {"type": "tool_result", "toolUseId": "t1", "content": [{"type": "text", "text": "seen"}], "isError": true},
                    "after"
                ]}
            ],
            "tools": [{"name": "look", "description": "Look", "inputSchema": {"type": "object"}}, {"nope": 1}]
        });
        let req = build_request("m", &params, &json!({"max_tokens_cap": 1000}));
        assert_eq!(req.model, "m");
        assert_eq!(req.system, "Be brief.\n\nextra");
        assert_eq!(req.max_tokens, 1000);
        assert_eq!(req.tools.len(), 1);
        assert_eq!(req.tools[0].name, "look");
        assert_eq!(req.messages.len(), 5);
        match &req.messages[1] {
            Message::User { content, .. } => assert_eq!(
                content.blocks(),
                [ContentBlock::Image {
                    data: "AAA".into(),
                    mime_type: "image/jpeg".into()
                }]
            ),
            other => panic!("unexpected {other:?}"),
        }
        match &req.messages[2] {
            Message::Assistant(a) => {
                assert_eq!(a.text(), "calling");
                assert_eq!(a.tool_calls().next().unwrap().0, "t1");
            }
            other => panic!("unexpected {other:?}"),
        }
        match &req.messages[3] {
            Message::ToolResult(r) => {
                assert_eq!(r.tool_call_id, "t1");
                assert!(r.is_error);
                assert_eq!(r.text(), "seen");
            }
            other => panic!("unexpected {other:?}"),
        }
        let small = build_request("m", &json!({"maxTokens": 10}), &json!({}));
        assert_eq!(small.max_tokens, 10);
        let default = build_request("m", &json!({}), &json!({}));
        assert_eq!(default.max_tokens, 4096);
    }

    #[test]
    fn result_shapes_follow_the_archived_qt() {
        let text = AssistantMessage {
            content: vec![ContentBlock::text("hello sk-secret")],
            model: "served".into(),
            ..Default::default()
        };
        assert_eq!(
            sampling_result(&text, "m"),
            json!({"role": "assistant", "content": {"type": "text", "text": "hello sk-[REDACTED]"}, "model": "served", "stopReason": "endTurn"})
        );
        let cut = AssistantMessage {
            content: vec![ContentBlock::text("par")],
            stop_reason: StopReason::Length,
            ..Default::default()
        };
        assert_eq!(sampling_result(&cut, "m")["stopReason"], "maxTokens");
        assert_eq!(sampling_result(&cut, "m")["model"], "m");
        let call = AssistantMessage {
            content: vec![ContentBlock::ToolCall {
                id: "c".into(),
                name: "n".into(),
                arguments: json!({"a": 1}),
            }],
            stop_reason: StopReason::ToolCall,
            ..Default::default()
        };
        assert_eq!(
            sampling_result(&call, "m")["content"],
            json!([{"type": "tool_use", "id": "c", "name": "n", "input": {"a": 1}}])
        );
        assert_eq!(sampling_result(&call, "m")["stopReason"], "toolUse");
    }

    #[test]
    fn requested_model_prefers_policy_then_hints() {
        assert_eq!(
            requested_model(&json!({"model": "a"}), &json!({})).as_deref(),
            Some("a")
        );
        assert_eq!(
            requested_model(
                &json!({}),
                &json!({"modelPreferences": {"hints": [{}, {"name": "b"}]}})
            )
            .as_deref(),
            Some("b")
        );
        assert_eq!(requested_model(&json!({}), &json!({})), None);
    }

    #[tokio::test]
    async fn disallowed_models_are_rejected_before_any_request() {
        let handler = ProviderSamplingHandler::new(
            reqwest::Client::new(),
            ProviderConfig {
                id: "ollama:local".into(),
                label: "Ollama".into(),
                kind: qt_core::provider::ProviderKind::OpenAiCompletions,
                base_url: "http://127.0.0.1:9/v1".into(),
                local: true,
                ..Default::default()
            },
            "m",
        );
        let err = handler
            .create_message(SamplingRequest {
                server_name: "s".into(),
                params: json!({"modelPreferences": {"hints": [{"name": "big"}]}}),
                config: json!({"allowed_models": ["small"]}),
            })
            .await
            .unwrap_err();
        assert_eq!(err, "MCP sampling requested disallowed model: big");
    }
}
