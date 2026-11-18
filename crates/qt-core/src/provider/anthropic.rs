//! Anthropic Messages API.

use std::collections::HashMap;

use anyhow::{Result, bail};
use futures::StreamExt as _;
use serde_json::{Value, json};
use tokio_util::sync::CancellationToken;

use super::{PartialMessage, ProviderConfig, Request, StreamEvent, http_error, sanitize_history};
use crate::message::{ContentBlock, Message, StopReason};
use crate::sse::SseParser;

pub const API_VERSION: &str = "2023-06-01";

pub(crate) fn authorize(
    builder: reqwest::RequestBuilder,
    provider: &ProviderConfig,
) -> reqwest::RequestBuilder {
    let builder = builder.header("anthropic-version", API_VERSION);
    match &provider.api_key {
        Some(key) => builder.header("x-api-key", key),
        None => builder,
    }
}

/// Build the JSON request body.
pub fn build_body(request: &Request) -> Value {
    let mut messages: Vec<Value> = Vec::new();
    for message in sanitize_history(&request.messages) {
        match &message {
            Message::User { content, .. } => {
                messages
                    .push(json!({ "role": "user", "content": blocks_to_api(&content.blocks()) }));
            }
            Message::Assistant(a) => {
                let same_provider = a.provider == "anthropic";
                let content: Vec<Value> = a
                    .content
                    .iter()
                    .filter_map(|block| match block {
                        ContentBlock::Text { text } if !text.is_empty() => Some(json!({ "type": "text", "text": text })),
                        ContentBlock::Thinking { thinking, signature: Some(sig), redacted } if same_provider => {
                            if *redacted {
                                Some(json!({ "type": "redacted_thinking", "data": sig }))
                            } else {
                                Some(json!({ "type": "thinking", "thinking": thinking, "signature": sig }))
                            }
                        }
                        // Unsigned or foreign thinking cannot be replayed and is dropped.
                        ContentBlock::ToolCall { id, name, arguments } => Some(json!({
                            "type": "tool_use",
                            "id": id,
                            "name": name,
                            "input": if arguments.is_object() { arguments.clone() } else { json!({}) },
                        })),
                        _ => None,
                    })
                    .collect();
                if !content.is_empty() {
                    messages.push(json!({ "role": "assistant", "content": content }));
                }
            }
            Message::ToolResult(r) => {
                let mut inner = blocks_to_api(&r.content);
                if inner.is_empty() {
                    inner.push(json!({ "type": "text", "text": "(no output)" }));
                }
                let block = json!({
                    "type": "tool_result",
                    "tool_use_id": r.tool_call_id,
                    "content": inner,
                    "is_error": r.is_error,
                });
                // Consecutive tool results share one user message.
                let merge = messages.last().is_some_and(|last| {
                    last["role"] == "user"
                        && last["content"]
                            .as_array()
                            .is_some_and(|c| c.iter().all(|b| b["type"] == "tool_result"))
                });
                if merge {
                    messages.last_mut().expect("checked")["content"]
                        .as_array_mut()
                        .expect("array")
                        .push(block);
                } else {
                    messages.push(json!({ "role": "user", "content": [block] }));
                }
            }
        }
    }
    // Cache the conversation prefix up to the latest user turn.
    if let Some(last_user) = messages.iter_mut().rev().find(|m| m["role"] == "user")
        && let Some(last_block) = last_user["content"]
            .as_array_mut()
            .and_then(|c| c.last_mut())
    {
        last_block["cache_control"] = json!({ "type": "ephemeral" });
    }

    let mut body = json!({
        "model": request.model,
        "max_tokens": request.max_tokens,
        "system": [{ "type": "text", "text": request.system, "cache_control": { "type": "ephemeral" } }],
        "messages": messages,
        "stream": true,
    });
    if !request.tools.is_empty() {
        body["tools"] = Value::Array(
            request
                .tools
                .iter()
                .map(|t| json!({ "name": t.name, "description": t.description, "input_schema": t.parameters }))
                .collect(),
        );
    }
    if let Some(budget) = request.thinking.budget_tokens() {
        body["thinking"] = json!({ "type": "enabled", "budget_tokens": budget });
        body["max_tokens"] = json!(request.max_tokens.max(budget + 4_096));
    }
    body
}

fn blocks_to_api(blocks: &[ContentBlock]) -> Vec<Value> {
    blocks
        .iter()
        .filter_map(|b| match b {
            ContentBlock::Text { text } if !text.is_empty() => {
                Some(json!({ "type": "text", "text": text }))
            }
            ContentBlock::Image { data, mime_type } => Some(json!({
                "type": "image",
                "source": { "type": "base64", "media_type": mime_type, "data": data },
            })),
            _ => None,
        })
        .collect()
}

pub(crate) async fn stream(
    http: &reqwest::Client,
    provider: &ProviderConfig,
    request: &Request,
    cancel: &CancellationToken,
    partial: &mut PartialMessage,
    on_event: &mut (dyn FnMut(StreamEvent) + Send),
) -> Result<()> {
    if provider.api_key.is_none() {
        bail!("No API key configured for {}", provider.label);
    }
    let body = build_body(request);
    let send = authorize(
        http.post(format!("{}/v1/messages", provider.base_url)),
        provider,
    )
    .json(&body)
    .send();
    let response = tokio::select! {
        r = send => r?,
        _ = cancel.cancelled() => return Ok(()),
    };
    if !response.status().is_success() {
        return Err(http_error(response).await);
    }
    let mut bytes = response.bytes_stream();
    let mut parser = SseParser::new();
    let mut state = StreamState::default();
    loop {
        let chunk = tokio::select! {
            c = bytes.next() => c,
            _ = cancel.cancelled() => return Ok(()),
        };
        let ended = chunk.is_none();
        let events = match chunk {
            Some(chunk) => parser.push(&chunk?),
            None => parser.finish(),
        };
        for event in events {
            let Ok(value) = serde_json::from_str::<Value>(&event.data) else {
                continue;
            };
            if apply_event(&value, &mut state, partial, on_event)? {
                return Ok(());
            }
        }
        if ended {
            return Ok(());
        }
    }
}

#[derive(Default)]
struct StreamState {
    /// API block index -> content index.
    blocks: HashMap<u64, usize>,
}

/// Apply one event; returns `true` at `message_stop`.
fn apply_event(
    value: &Value,
    state: &mut StreamState,
    partial: &mut PartialMessage,
    on_event: &mut (dyn FnMut(StreamEvent) + Send),
) -> Result<bool> {
    let u = |v: &Value, k: &str| v.get(k).and_then(Value::as_u64);
    match value
        .get("type")
        .and_then(Value::as_str)
        .unwrap_or_default()
    {
        "message_start" => {
            let message = &value["message"];
            if let Some(id) = message.get("id").and_then(Value::as_str) {
                partial.message.response_id = Some(id.to_string());
            }
            let usage = &message["usage"];
            let usage_out = &mut partial.message.usage;
            usage_out.input_tokens = u(usage, "input_tokens").unwrap_or(0);
            usage_out.cache_read_tokens = u(usage, "cache_read_input_tokens").unwrap_or(0);
            usage_out.cache_write_tokens = u(usage, "cache_creation_input_tokens").unwrap_or(0);
            usage_out.output_tokens = u(usage, "output_tokens").unwrap_or(0);
        }
        "content_block_start" => {
            let api_index = u(value, "index").unwrap_or(0);
            let block = &value["content_block"];
            let content = match block
                .get("type")
                .and_then(Value::as_str)
                .unwrap_or_default()
            {
                "text" => Some(ContentBlock::text(
                    block
                        .get("text")
                        .and_then(Value::as_str)
                        .unwrap_or_default(),
                )),
                "thinking" => Some(ContentBlock::Thinking {
                    thinking: block
                        .get("thinking")
                        .and_then(Value::as_str)
                        .unwrap_or_default()
                        .to_string(),
                    signature: None,
                    redacted: false,
                }),
                "redacted_thinking" => Some(ContentBlock::Thinking {
                    thinking: String::new(),
                    signature: block
                        .get("data")
                        .and_then(Value::as_str)
                        .map(str::to_string),
                    redacted: true,
                }),
                "tool_use" => Some(ContentBlock::ToolCall {
                    id: block
                        .get("id")
                        .and_then(Value::as_str)
                        .unwrap_or_default()
                        .to_string(),
                    name: block
                        .get("name")
                        .and_then(Value::as_str)
                        .unwrap_or_default()
                        .to_string(),
                    arguments: Value::Null,
                }),
                _ => None,
            };
            if let Some(content) = content {
                let index = partial.open(content, on_event);
                state.blocks.insert(api_index, index);
            }
        }
        "content_block_delta" => {
            let Some(&index) = u(value, "index").and_then(|i| state.blocks.get(&i)) else {
                return Ok(false);
            };
            let delta = &value["delta"];
            let s = |k: &str| delta.get(k).and_then(Value::as_str).unwrap_or_default();
            match delta
                .get("type")
                .and_then(Value::as_str)
                .unwrap_or_default()
            {
                "text_delta" => partial.push_text(index, s("text"), on_event),
                "thinking_delta" => partial.push_thinking(index, s("thinking"), on_event),
                "signature_delta" => partial.set_signature(index, s("signature")),
                "input_json_delta" => partial.push_tool_args(index, s("partial_json"), on_event),
                _ => {}
            }
        }
        "message_delta" => {
            if let Some(reason) = value["delta"].get("stop_reason").and_then(Value::as_str) {
                partial.set_stop(match reason {
                    "tool_use" => StopReason::ToolCall,
                    "max_tokens" => StopReason::Length,
                    "refusal" => StopReason::Error,
                    _ => StopReason::Stop,
                });
            }
            let usage = &value["usage"];
            let out = &mut partial.message.usage;
            if let Some(v) = u(usage, "output_tokens") {
                out.output_tokens = v;
            }
            if let Some(v) = u(usage, "input_tokens") {
                out.input_tokens = v;
            }
            if let Some(v) = u(usage, "cache_read_input_tokens") {
                out.cache_read_tokens = v;
            }
            if let Some(v) = u(usage, "cache_creation_input_tokens") {
                out.cache_write_tokens = v;
            }
            out.total_tokens = out.input_tokens
                + out.output_tokens
                + out.cache_read_tokens
                + out.cache_write_tokens;
        }
        "message_stop" => return Ok(true),
        "error" => {
            let msg = value["error"]
                .get("message")
                .and_then(Value::as_str)
                .unwrap_or("stream error");
            bail!("{msg}");
        }
        _ => {}
    }
    Ok(false)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::message::{AssistantMessage, ToolResultMessage};
    use crate::provider::{ProviderKind, ThinkingLevel};

    #[test]
    fn applies_stream_events() {
        let provider = ProviderConfig {
            id: "anthropic".into(),
            label: "Anthropic".into(),
            kind: ProviderKind::Anthropic,
            base_url: "https://x".into(),
            api_key: Some("k".into()),
        };
        let mut partial = PartialMessage::new(&provider, "claude");
        let mut state = StreamState::default();
        let mut sink = |_e: StreamEvent| {};
        let events = [
            r#"{"type":"message_start","message":{"id":"msg_1","usage":{"input_tokens":12,"cache_read_input_tokens":4,"output_tokens":1}}}"#,
            r#"{"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":""}}"#,
            r#"{"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"hmm"}}"#,
            r#"{"type":"content_block_delta","index":0,"delta":{"type":"signature_delta","signature":"SIG"}}"#,
            r#"{"type":"content_block_start","index":1,"content_block":{"type":"text","text":""}}"#,
            r#"{"type":"content_block_delta","index":1,"delta":{"type":"text_delta","text":"Hi"}}"#,
            r#"{"type":"content_block_start","index":2,"content_block":{"type":"tool_use","id":"tu1","name":"bash","input":{}}}"#,
            r#"{"type":"content_block_delta","index":2,"delta":{"type":"input_json_delta","partial_json":"{\"command\":\"ls\"}"}}"#,
            r#"{"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"output_tokens":20}}"#,
        ];
        for e in events {
            assert!(
                !apply_event(
                    &serde_json::from_str(e).unwrap(),
                    &mut state,
                    &mut partial,
                    &mut sink
                )
                .unwrap()
            );
        }
        assert!(
            apply_event(
                &json!({"type":"message_stop"}),
                &mut state,
                &mut partial,
                &mut sink
            )
            .unwrap()
        );
        let m = partial.finish();
        assert_eq!(m.stop_reason, StopReason::ToolCall);
        assert_eq!(m.text(), "Hi");
        assert_eq!(m.usage.output_tokens, 20);
        assert_eq!(m.usage.total_tokens, 12 + 20 + 4);
        assert!(
            matches!(&m.content[0], ContentBlock::Thinking { signature: Some(s), .. } if s == "SIG")
        );
        assert_eq!(m.tool_calls().next().unwrap().2["command"], "ls");
    }

    #[test]
    fn merges_tool_results_and_marks_cache() {
        let call = Message::Assistant(AssistantMessage {
            provider: "anthropic".into(),
            content: vec![
                ContentBlock::ToolCall {
                    id: "a".into(),
                    name: "ls".into(),
                    arguments: json!({}),
                },
                ContentBlock::ToolCall {
                    id: "b".into(),
                    name: "ls".into(),
                    arguments: json!({}),
                },
            ],
            stop_reason: StopReason::ToolCall,
            ..Default::default()
        });
        let r = |id: &str| {
            Message::ToolResult(ToolResultMessage {
                tool_call_id: id.into(),
                content: vec![ContentBlock::text("ok")],
                ..Default::default()
            })
        };
        let request = Request {
            model: "m".into(),
            system: "s".into(),
            messages: vec![Message::user_text("go"), call, r("a"), r("b")],
            tools: vec![],
            max_tokens: 1000,
            thinking: ThinkingLevel::Low,
        };
        let body = build_body(&request);
        let msgs = body["messages"].as_array().unwrap();
        assert_eq!(msgs.len(), 3);
        assert_eq!(msgs[2]["content"].as_array().unwrap().len(), 2);
        assert_eq!(msgs[2]["content"][1]["cache_control"]["type"], "ephemeral");
        assert!(body["max_tokens"].as_u64().unwrap() > 4_096);
    }
}
