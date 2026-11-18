//! OpenAI-compatible chat completions (OpenAI, OpenRouter, Ollama).

use std::collections::HashMap;

use anyhow::{Result, bail};
use futures::StreamExt as _;
use serde_json::{Value, json};
use tokio_util::sync::CancellationToken;

use super::{
    PartialMessage, ProviderConfig, ProviderKind, Request, StreamEvent, ThinkingLevel, http_error,
    sanitize_history,
};
use crate::message::{ContentBlock, Message, StopReason, Usage};
use crate::sse::SseParser;

pub(crate) fn endpoint(provider: &ProviderConfig) -> String {
    match provider.kind {
        ProviderKind::Ollama => format!("{}/v1/chat/completions", provider.base_url),
        _ => format!("{}/chat/completions", provider.base_url),
    }
}

pub(crate) fn authorize(
    builder: reqwest::RequestBuilder,
    provider: &ProviderConfig,
) -> reqwest::RequestBuilder {
    let builder = match &provider.api_key {
        Some(key) => builder.bearer_auth(key),
        None => builder,
    };
    if provider.kind == ProviderKind::OpenRouter {
        builder
            .header("X-Title", "qt")
            .header("HTTP-Referer", "https://github.com/qtbits/qt")
    } else {
        builder
    }
}

/// Build the JSON request body.
pub fn build_body(provider: &ProviderConfig, request: &Request) -> Value {
    let mut messages = vec![json!({ "role": "system", "content": request.system })];
    for message in sanitize_history(&request.messages) {
        convert_message(&message, &mut messages);
    }
    let mut body = json!({
        "model": request.model,
        "messages": messages,
        "stream": true,
        "stream_options": { "include_usage": true },
    });
    if !request.tools.is_empty() {
        body["tools"] = Value::Array(
            request
                .tools
                .iter()
                .map(|t| json!({ "type": "function", "function": { "name": t.name, "description": t.description, "parameters": t.parameters } }))
                .collect(),
        );
    }
    if request.thinking != ThinkingLevel::Off {
        let effort = request.thinking.as_str();
        match provider.kind {
            ProviderKind::OpenRouter => body["reasoning"] = json!({ "effort": effort }),
            _ => body["reasoning_effort"] = json!(effort),
        }
    }
    body
}

fn convert_message(message: &Message, out: &mut Vec<Value>) {
    match message {
        Message::User { content, .. } => {
            let blocks = content.blocks();
            out.push(json!({ "role": "user", "content": user_parts(&blocks) }));
        }
        Message::Assistant(a) => {
            let text = a.text();
            let calls: Vec<Value> = a
                .tool_calls()
                .map(|(id, name, args)| {
                    let arguments = match args {
                        Value::String(raw) => raw.clone(),
                        other => other.to_string(),
                    };
                    json!({ "id": id, "type": "function", "function": { "name": name, "arguments": arguments } })
                })
                .collect();
            // A turn with only reasoning has nothing replayable; some servers
            // (Ollama) reject assistant messages without content or calls.
            if text.is_empty() && calls.is_empty() {
                return;
            }
            let mut msg = json!({ "role": "assistant", "content": text });
            if !calls.is_empty() {
                msg["tool_calls"] = Value::Array(calls);
            }
            out.push(msg);
        }
        Message::ToolResult(r) => {
            let text = r.text();
            out.push(json!({
                "role": "tool",
                "tool_call_id": r.tool_call_id,
                "content": if text.is_empty() { "(no output)".to_string() } else { text },
            }));
            let images: Vec<ContentBlock> = r
                .content
                .iter()
                .filter(|b| matches!(b, ContentBlock::Image { .. }))
                .cloned()
                .collect();
            if !images.is_empty() {
                let mut blocks = vec![ContentBlock::text(format!(
                    "Image output from {}:",
                    r.tool_name
                ))];
                blocks.extend(images);
                out.push(json!({ "role": "user", "content": user_parts(&blocks) }));
            }
        }
    }
}

fn user_parts(blocks: &[ContentBlock]) -> Value {
    let only_text = blocks
        .iter()
        .all(|b| matches!(b, ContentBlock::Text { .. }));
    if only_text {
        return json!(crate::message::join_text(blocks));
    }
    Value::Array(
        blocks
            .iter()
            .filter_map(|b| match b {
                ContentBlock::Text { text } => Some(json!({ "type": "text", "text": text })),
                ContentBlock::Image { data, mime_type } => {
                    Some(json!({ "type": "image_url", "image_url": { "url": format!("data:{mime_type};base64,{data}") } }))
                }
                _ => None,
            })
            .collect(),
    )
}

/// Tracks the content index assigned to each streamed choice part.
#[derive(Default)]
struct ChunkState {
    text_index: Option<usize>,
    thinking_index: Option<usize>,
    /// Provider tool-call index -> content block index.
    tool_indices: HashMap<u64, usize>,
}

pub(crate) async fn stream(
    http: &reqwest::Client,
    provider: &ProviderConfig,
    request: &Request,
    cancel: &CancellationToken,
    partial: &mut PartialMessage,
    on_event: &mut (dyn FnMut(StreamEvent) + Send),
) -> Result<()> {
    if provider.needs_api_key() && provider.api_key.is_none() {
        bail!("No API key configured for {}", provider.label);
    }
    let body = build_body(provider, request);
    let send = authorize(http.post(endpoint(provider)), provider)
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
    let mut state = ChunkState::default();
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
        for event in &events {
            if event.data.trim() == "[DONE]" {
                return Ok(());
            }
            let value: Value = match serde_json::from_str(&event.data) {
                Ok(v) => v,
                Err(_) => continue,
            };
            apply_chunk(&value, &mut state, partial, on_event)?;
        }
        if ended {
            return Ok(());
        }
    }
}

/// Apply one parsed chunk to the partial message.
fn apply_chunk(
    value: &Value,
    state: &mut ChunkState,
    partial: &mut PartialMessage,
    on_event: &mut (dyn FnMut(StreamEvent) + Send),
) -> Result<()> {
    if let Some(err) = value.get("error") {
        let msg = err
            .get("message")
            .and_then(Value::as_str)
            .map(str::to_string)
            .unwrap_or_else(|| err.to_string());
        bail!("{msg}");
    }
    if let Some(id) = value.get("id").and_then(Value::as_str) {
        partial
            .message
            .response_id
            .get_or_insert_with(|| id.to_string());
    }
    if let Some(usage) = value.get("usage").filter(|u| u.is_object()) {
        partial.message.usage = parse_usage(usage);
    }
    let Some(choice) = value.get("choices").and_then(|c| c.get(0)) else {
        return Ok(());
    };
    if let Some(delta) = choice.get("delta") {
        for key in ["reasoning_content", "reasoning"] {
            if let Some(text) = delta
                .get(key)
                .and_then(Value::as_str)
                .filter(|t| !t.is_empty())
            {
                let index = match state.thinking_index {
                    Some(i) => i,
                    None => {
                        let i = partial.open(
                            ContentBlock::Thinking {
                                thinking: String::new(),
                                signature: None,
                                redacted: false,
                            },
                            on_event,
                        );
                        state.thinking_index = Some(i);
                        i
                    }
                };
                partial.push_thinking(index, text, on_event);
                break;
            }
        }
        if let Some(text) = delta
            .get("content")
            .and_then(Value::as_str)
            .filter(|t| !t.is_empty())
        {
            let index = match state.text_index {
                Some(i) => i,
                None => {
                    let i = partial.open(ContentBlock::text(""), on_event);
                    state.text_index = Some(i);
                    i
                }
            };
            partial.push_text(index, text, on_event);
        }
        if let Some(calls) = delta.get("tool_calls").and_then(Value::as_array) {
            for (position, call) in calls.iter().enumerate() {
                let key = call
                    .get("index")
                    .and_then(Value::as_u64)
                    .unwrap_or(position as u64);
                let function = call.get("function");
                let index = match state.tool_indices.get(&key) {
                    Some(i) => *i,
                    None => {
                        let id = call
                            .get("id")
                            .and_then(Value::as_str)
                            .filter(|s| !s.is_empty())
                            .map(str::to_string)
                            .unwrap_or_else(|| format!("call_{}", crate::util::short_id()));
                        let name = function
                            .and_then(|f| f.get("name"))
                            .and_then(Value::as_str)
                            .unwrap_or_default();
                        let i = partial.open(
                            ContentBlock::ToolCall {
                                id,
                                name: name.to_string(),
                                arguments: Value::Null,
                            },
                            on_event,
                        );
                        state.tool_indices.insert(key, i);
                        // Text after a tool call starts a new block.
                        state.text_index = None;
                        i
                    }
                };
                if let Some(args) = function.and_then(|f| f.get("arguments")) {
                    match args {
                        Value::String(s) => partial.push_tool_args(index, s, on_event),
                        Value::Object(_) => {
                            partial.push_tool_args(index, &args.to_string(), on_event)
                        }
                        _ => {}
                    }
                }
            }
        }
    }
    if let Some(reason) = choice.get("finish_reason").and_then(Value::as_str) {
        partial.set_stop(match reason {
            "tool_calls" | "function_call" => StopReason::ToolCall,
            "length" => StopReason::Length,
            "content_filter" => StopReason::Error,
            _ => StopReason::Stop,
        });
    }
    Ok(())
}

fn parse_usage(usage: &Value) -> Usage {
    let get = |v: &Value, k: &str| v.get(k).and_then(Value::as_u64).unwrap_or(0);
    let prompt = get(usage, "prompt_tokens");
    let completion = get(usage, "completion_tokens");
    let cached = usage
        .get("prompt_tokens_details")
        .map(|d| get(d, "cached_tokens"))
        .unwrap_or(0);
    let total = get(usage, "total_tokens");
    Usage {
        input_tokens: prompt.saturating_sub(cached),
        output_tokens: completion,
        cache_read_tokens: cached,
        cache_write_tokens: 0,
        total_tokens: if total > 0 {
            total
        } else {
            prompt + completion
        },
        total_cost_usd: usage.get("cost").and_then(Value::as_f64).unwrap_or(0.0),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::provider::ToolSpec;

    fn cfg() -> ProviderConfig {
        ProviderConfig {
            id: "openrouter".into(),
            label: "OpenRouter".into(),
            kind: ProviderKind::OpenRouter,
            base_url: "https://x".into(),
            api_key: Some("k".into()),
        }
    }

    #[test]
    fn applies_streamed_chunks() {
        let provider = cfg();
        let mut partial = PartialMessage::new(&provider, "m");
        let mut state = ChunkState::default();
        let mut events = Vec::new();
        let mut sink = |e: StreamEvent| events.push(e);
        let chunks = [
            r#"{"id":"r1","choices":[{"delta":{"reasoning":"think"}}]}"#,
            r#"{"choices":[{"delta":{"content":"Hel"}}]}"#,
            r#"{"choices":[{"delta":{"content":"lo"}}]}"#,
            r#"{"choices":[{"delta":{"tool_calls":[{"index":0,"id":"c1","function":{"name":"read","arguments":"{\"pa"}}]}}]}"#,
            r#"{"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"th\":\"x\"}"}}]}}]}"#,
            r#"{"choices":[{"delta":{},"finish_reason":"tool_calls"}],"usage":{"prompt_tokens":10,"completion_tokens":4,"prompt_tokens_details":{"cached_tokens":3},"cost":0.01}}"#,
        ];
        for c in chunks {
            apply_chunk(
                &serde_json::from_str(c).unwrap(),
                &mut state,
                &mut partial,
                &mut sink,
            )
            .unwrap();
        }
        let m = partial.finish();
        assert_eq!(m.stop_reason, StopReason::ToolCall);
        assert_eq!(m.text(), "Hello");
        assert_eq!(m.response_id.as_deref(), Some("r1"));
        assert_eq!(m.usage.input_tokens, 7);
        assert_eq!(m.usage.cache_read_tokens, 3);
        let (id, name, args) = m.tool_calls().next().unwrap();
        assert_eq!((id, name), ("c1", "read"));
        assert_eq!(args["path"], "x");
        assert!(matches!(m.content[0], ContentBlock::Thinking { .. }));
    }

    #[test]
    fn body_includes_tools_and_tool_results() {
        let provider = cfg();
        let call = Message::Assistant(crate::message::AssistantMessage {
            content: vec![ContentBlock::ToolCall {
                id: "c1".into(),
                name: "ls".into(),
                arguments: json!({}),
            }],
            stop_reason: StopReason::ToolCall,
            ..Default::default()
        });
        let result = Message::ToolResult(crate::message::ToolResultMessage {
            tool_call_id: "c1".into(),
            tool_name: "ls".into(),
            content: vec![ContentBlock::text("a/")],
            ..Default::default()
        });
        let request = Request {
            model: "m".into(),
            system: "sys".into(),
            messages: vec![Message::user_text("hi"), call, result],
            tools: vec![ToolSpec {
                name: "ls".into(),
                description: "d".into(),
                parameters: json!({"type":"object"}),
            }],
            max_tokens: 100,
            thinking: ThinkingLevel::High,
        };
        let body = build_body(&provider, &request);
        let msgs = body["messages"].as_array().unwrap();
        assert_eq!(msgs.len(), 4);
        assert_eq!(msgs[2]["tool_calls"][0]["function"]["arguments"], "{}");
        assert_eq!(msgs[3]["role"], "tool");
        assert_eq!(body["tools"][0]["function"]["name"], "ls");
        assert_eq!(body["reasoning"]["effort"], "high");
        assert_eq!(msgs[2]["content"], "");
    }

    #[test]
    fn thinking_only_turns_are_not_replayed() {
        let thinking_only = Message::Assistant(crate::message::AssistantMessage {
            content: vec![ContentBlock::Thinking {
                thinking: "hmm".into(),
                signature: None,
                redacted: false,
            }],
            stop_reason: StopReason::Stop,
            ..Default::default()
        });
        let request = Request {
            model: "m".into(),
            system: "s".into(),
            messages: vec![
                Message::user_text("a"),
                thinking_only,
                Message::user_text("b"),
            ],
            tools: vec![],
            max_tokens: 100,
            thinking: ThinkingLevel::Off,
        };
        let body = build_body(&cfg(), &request);
        let roles: Vec<&str> = body["messages"]
            .as_array()
            .unwrap()
            .iter()
            .map(|m| m["role"].as_str().unwrap())
            .collect();
        assert_eq!(roles, ["system", "user", "user"]);
        assert!(
            body["messages"]
                .as_array()
                .unwrap()
                .iter()
                .all(|m| !m["content"].is_null())
        );
    }
}
