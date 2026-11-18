//! OpenAI-compatible chat completions (Pi `openai-completions`).
//!
//! Used for OpenAI-compatible hosts (OpenRouter, Groq, Together, Z.ai,
//! Cloudflare, OpenCode, Ollama, custom `models.json` providers). Request
//! shape differences are driven by [`super::compat::CompletionsCompat`].

use std::collections::{BTreeMap, HashMap};

use anyhow::{Result, bail};
use futures::StreamExt as _;
use serde_json::{Value, json};
use tokio_util::sync::CancellationToken;

use super::compat::{self, CompletionsCompat, ThinkingFormat};
use super::http::{self, Sent};
use super::transform::{Target, transform_messages};
use super::{
    CacheRetention, PartialMessage, ProviderConfig, Request, StreamEvent, ThinkingLevel,
    effective_max_tokens, sanitize_text, with_headers,
};
use crate::catalog::ModelSpec;
use crate::message::{AssistantMessage, ContentBlock, Message, StopReason, Usage, UserContent};
use crate::sse::SseParser;

/// Referer and title sent to OpenRouter (attribution).
pub const OPENROUTER_REFERER: &str = "https://github.com/qtbits/qt";
pub const OPENROUTER_TITLE: &str = "qt";
pub const OPENROUTER_CATEGORIES: &str = "cli-agent";
const REASONING_FIELDS: [&str; 3] = ["reasoning_content", "reasoning", "reasoning_text"];
const TOOL_RESULT_BRIDGE: &str = "I have processed the tool results.";

pub(crate) fn authorize(
    builder: reqwest::RequestBuilder,
    provider: &ProviderConfig,
) -> reqwest::RequestBuilder {
    match provider.api_key.as_deref().filter(|k| !k.is_empty()) {
        Some(key) => builder.bearer_auth(key),
        None => builder,
    }
}

fn is_openrouter(provider: &ProviderConfig, model: &ModelSpec) -> bool {
    provider.id == "openrouter"
        || model.provider == "openrouter"
        || model.base_url.contains("openrouter.ai")
}

/// A fully built request.
#[derive(Debug, Clone, PartialEq)]
pub struct Built {
    pub url: String,
    pub headers: BTreeMap<String, String>,
    pub body: Value,
}

/// Build the URL, headers and body for one request.
pub fn build_request(
    provider: &ProviderConfig,
    model: &ModelSpec,
    request: &Request,
) -> Result<Built> {
    let compat = compat::completions(provider, model);
    let retention = CacheRetention::resolve(request.cache_retention);
    let key = provider.api_key.clone().unwrap_or_default();
    let mut headers = BTreeMap::new();
    headers.insert("content-type".to_string(), "application/json".to_string());
    headers.insert("accept".to_string(), "text/event-stream".to_string());
    if model.provider == "cloudflare-ai-gateway" {
        headers.insert("cf-aig-authorization".into(), format!("Bearer {key}"));
    } else if !key.is_empty() {
        headers.insert("authorization".into(), format!("Bearer {key}"));
    }
    if let Some(session) = &request.session_id
        && retention != CacheRetention::None
        && compat.send_session_affinity_headers
    {
        headers.insert("session_id".into(), session.clone());
        headers.insert("x-client-request-id".into(), session.clone());
        headers.insert("x-session-affinity".into(), session.clone());
    }
    if is_openrouter(provider, model) && provider.attribution() {
        headers.insert("HTTP-Referer".into(), OPENROUTER_REFERER.into());
        headers.insert("X-Title".into(), OPENROUTER_TITLE.into());
        headers.insert("X-OpenRouter-Title".into(), OPENROUTER_TITLE.into());
        headers.insert(
            "X-OpenRouter-Categories".into(),
            OPENROUTER_CATEGORIES.into(),
        );
    }
    headers.extend(provider.headers.clone());
    headers.extend(model.headers.clone());
    headers.extend(request.headers.clone());

    let base =
        super::resolve_url_placeholders(model.base_url.trim_end_matches('/'), &model.provider)?;
    Ok(Built {
        url: format!("{base}/chat/completions"),
        headers,
        body: build_body(provider, model, request, &compat, retention),
    })
}

fn has_tool_history(messages: &[Message]) -> bool {
    messages.iter().any(|m| match m {
        Message::ToolResult(_) => true,
        Message::Assistant(a) => a.tool_calls().next().is_some(),
        Message::User { .. } => false,
    })
}

/// Build the JSON request body.
pub fn build_body(
    provider: &ProviderConfig,
    model: &ModelSpec,
    request: &Request,
    compat: &CompletionsCompat,
    retention: CacheRetention,
) -> Value {
    let mut messages = convert_messages(provider, model, request, compat);
    let long = retention == CacheRetention::Long && compat.supports_long_cache_retention;
    let mut body = json!({
        "model": model.id,
        "messages": [],
        "stream": true,
    });
    if let Some(session) = &request.session_id
        && ((model.base_url.contains("api.openai.com") && retention != CacheRetention::None)
            || long)
    {
        body["prompt_cache_key"] = json!(session);
    }
    if long {
        body["prompt_cache_retention"] = json!("24h");
    }
    if compat.supports_usage_in_streaming {
        body["stream_options"] = json!({ "include_usage": true });
    }
    if compat.supports_store {
        body["store"] = json!(false);
    }
    body[compat.max_tokens_field.as_str()] = json!(effective_max_tokens(request, model));
    if let Some(t) = request.temperature {
        body["temperature"] = json!(t);
    }
    let mut tools: Option<Vec<Value>> = None;
    if !request.tools.is_empty() {
        tools = Some(
            request
                .tools
                .iter()
                .map(|t| {
                    let mut function = json!({
                        "name": t.name,
                        "description": t.description,
                        "parameters": t.parameters,
                    });
                    if compat.supports_strict_mode {
                        function["strict"] = json!(false);
                    }
                    json!({ "type": "function", "function": function })
                })
                .collect(),
        );
        if compat.zai_tool_stream {
            body["tool_stream"] = json!(true);
        }
    } else if has_tool_history(&request.messages) {
        tools = Some(Vec::new());
    }
    if compat.cache_control_anthropic && retention != CacheRetention::None {
        let mut cache = json!({ "type": "ephemeral" });
        if long {
            cache["ttl"] = json!("1h");
        }
        apply_anthropic_cache_control(&mut messages, tools.as_mut(), &cache);
    }
    body["messages"] = Value::Array(messages);
    if let Some(tools) = tools {
        body["tools"] = Value::Array(tools);
    }

    let level = super::request_level(model, request.thinking);
    let effort = (level != ThinkingLevel::Off).then(|| model.effort_for(level));
    if model.reasoning {
        match compat.thinking_format {
            ThinkingFormat::Zai | ThinkingFormat::Qwen => {
                body["enable_thinking"] = json!(effort.is_some());
            }
            ThinkingFormat::QwenChatTemplate => {
                body["chat_template_kwargs"] =
                    json!({ "enable_thinking": effort.is_some(), "preserve_thinking": true });
            }
            ThinkingFormat::DeepSeek => {
                body["thinking"] =
                    json!({ "type": if effort.is_some() { "enabled" } else { "disabled" } });
                if let Some(e) = &effort {
                    body["reasoning_effort"] = json!(e);
                }
            }
            ThinkingFormat::OpenRouter => match &effort {
                Some(e) => body["reasoning"] = json!({ "effort": e }),
                None => match model.mapped_level(ThinkingLevel::Off) {
                    Some(None) => {}
                    Some(Some(off)) => body["reasoning"] = json!({ "effort": off }),
                    None => body["reasoning"] = json!({ "effort": "none" }),
                },
            },
            ThinkingFormat::Together => {
                body["reasoning"] = json!({ "enabled": effort.is_some() });
                if let Some(e) = &effort
                    && compat.supports_reasoning_effort
                {
                    body["reasoning_effort"] = json!(e);
                }
            }
            ThinkingFormat::OpenAi if compat.supports_reasoning_effort => match &effort {
                Some(e) => body["reasoning_effort"] = json!(e),
                None => {
                    if let Some(Some(off)) = model.mapped_level(ThinkingLevel::Off) {
                        body["reasoning_effort"] = json!(off);
                    }
                }
            },
            ThinkingFormat::OpenAi => {}
        }
    }
    if model.base_url.contains("openrouter.ai")
        && let Some(routing) = &compat.open_router_routing
    {
        body["provider"] = routing.clone();
    }
    if model.base_url.contains("ai-gateway.vercel.sh")
        && let Some(routing) = &compat.vercel_gateway_routing
    {
        let mut gateway = serde_json::Map::new();
        for key in ["only", "order"] {
            if let Some(v) = routing.get(key) {
                gateway.insert(key.into(), v.clone());
            }
        }
        if !gateway.is_empty() {
            body["providerOptions"] = json!({ "gateway": gateway });
        }
    }
    body
}

fn add_cache_control(message: &mut Value, cache: &Value) -> bool {
    match &message["content"] {
        Value::String(text) if !text.is_empty() => {
            let text = text.clone();
            message["content"] = json!([{ "type": "text", "text": text, "cache_control": cache }]);
            true
        }
        Value::Array(_) => {
            let parts = message["content"].as_array_mut().expect("array");
            match parts.iter_mut().rev().find(|p| p["type"] == "text") {
                Some(part) => {
                    part["cache_control"] = cache.clone();
                    true
                }
                None => false,
            }
        }
        _ => false,
    }
}

fn apply_anthropic_cache_control(
    messages: &mut [Value],
    tools: Option<&mut Vec<Value>>,
    cache: &Value,
) {
    if let Some(system) = messages
        .iter_mut()
        .find(|m| m["role"] == "system" || m["role"] == "developer")
    {
        add_cache_control(system, cache);
    }
    if let Some(last) = tools.and_then(|t| t.last_mut()) {
        last["cache_control"] = cache.clone();
    }
    for message in messages.iter_mut().rev() {
        if (message["role"] == "user" || message["role"] == "assistant")
            && add_cache_control(message, cache)
        {
            break;
        }
    }
}

fn image_part(data: &str, mime_type: &str) -> Value {
    json!({ "type": "image_url", "image_url": { "url": format!("data:{mime_type};base64,{data}") } })
}

/// Pi `convertMessages` for chat completions.
pub fn convert_messages(
    provider: &ProviderConfig,
    model: &ModelSpec,
    request: &Request,
    compat: &CompletionsCompat,
) -> Vec<Value> {
    let target = Target {
        provider: &provider.id,
        api: "openai-completions",
        model: &model.id,
        images: model.supports_images(),
    };
    let openai = model.provider == "openai";
    let normalize = move |id: &str, _: &AssistantMessage| -> String {
        if let Some((call_id, _)) = id.split_once('|') {
            return super::transform::sanitize_id(call_id)
                .chars()
                .take(40)
                .collect();
        }
        if openai && id.len() > 40 {
            return id.chars().take(40).collect();
        }
        id.to_string()
    };
    let transformed = transform_messages(&request.messages, &target, Some(&normalize));

    let mut out = Vec::new();
    if !request.system.is_empty() {
        let role = if model.reasoning && compat.supports_developer_role {
            "developer"
        } else {
            "system"
        };
        out.push(json!({ "role": role, "content": sanitize_text(&request.system) }));
    }
    let mut last_role: Option<&str> = None;
    let mut i = 0;
    while i < transformed.len() {
        let message = &transformed[i];
        if compat.requires_assistant_after_tool_result
            && last_role == Some("toolResult")
            && matches!(message, Message::User { .. })
        {
            out.push(json!({ "role": "assistant", "content": TOOL_RESULT_BRIDGE }));
        }
        match message {
            Message::User { content, .. } => {
                match content {
                    UserContent::Text(text) => {
                        out.push(json!({ "role": "user", "content": sanitize_text(text) }));
                    }
                    UserContent::Blocks(blocks) => {
                        let parts: Vec<Value> = blocks
                            .iter()
                            .filter_map(|b| match b {
                                ContentBlock::Text { text } => {
                                    Some(json!({ "type": "text", "text": sanitize_text(text) }))
                                }
                                ContentBlock::Image { data, mime_type } => {
                                    Some(image_part(data, mime_type))
                                }
                                _ => None,
                            })
                            .collect();
                        if parts.is_empty() {
                            i += 1;
                            continue;
                        }
                        out.push(json!({ "role": "user", "content": parts }));
                    }
                }
                last_role = Some("user");
            }
            Message::Assistant(a) => {
                let mut msg = json!({
                    "role": "assistant",
                    "content": if compat.requires_assistant_after_tool_result { json!("") } else { Value::Null },
                });
                let text_parts: Vec<String> = a
                    .content
                    .iter()
                    .filter_map(|b| match b {
                        ContentBlock::Text { text } if !text.trim().is_empty() => {
                            Some(sanitize_text(text))
                        }
                        _ => None,
                    })
                    .collect();
                let text = text_parts.concat();
                let thinking: Vec<(&str, Option<&str>)> = a
                    .content
                    .iter()
                    .filter_map(|b| match b {
                        ContentBlock::Thinking {
                            thinking,
                            signature,
                            ..
                        } if !thinking.trim().is_empty() => {
                            Some((thinking.as_str(), signature.as_deref()))
                        }
                        _ => None,
                    })
                    .collect();
                if !thinking.is_empty() {
                    if compat.requires_thinking_as_text {
                        let joined: Vec<String> =
                            thinking.iter().map(|(t, _)| sanitize_text(t)).collect();
                        let mut parts =
                            vec![json!({ "type": "text", "text": joined.join("\n\n") })];
                        parts.extend(
                            text_parts
                                .iter()
                                .map(|t| json!({ "type": "text", "text": t })),
                        );
                        msg["content"] = Value::Array(parts);
                    } else {
                        if !text.is_empty() {
                            msg["content"] = json!(text);
                        }
                        if let Some(field) = thinking[0].1.filter(|s| REASONING_FIELDS.contains(s))
                        {
                            let joined: Vec<&str> = thinking.iter().map(|(t, _)| *t).collect();
                            msg[field] = json!(joined.join("\n"));
                        }
                    }
                } else if !text.is_empty() {
                    msg["content"] = json!(text);
                }
                let calls: Vec<Value> = a
                    .tool_calls()
                    .map(|(id, name, args)| {
                        let arguments = match args {
                            Value::String(raw) => raw.clone(),
                            Value::Null => "{}".to_string(),
                            other => other.to_string(),
                        };
                        json!({ "id": id, "type": "function", "function": { "name": name, "arguments": arguments } })
                    })
                    .collect();
                let has_calls = !calls.is_empty();
                if has_calls {
                    msg["tool_calls"] = Value::Array(calls);
                }
                if compat.requires_reasoning_content_on_assistant_messages
                    && model.reasoning
                    && msg.get("reasoning_content").is_none()
                {
                    msg["reasoning_content"] = json!("");
                }
                let has_content = match &msg["content"] {
                    Value::String(s) => !s.is_empty(),
                    Value::Array(a) => !a.is_empty(),
                    _ => false,
                };
                if has_content || has_calls {
                    out.push(msg);
                }
                last_role = Some("assistant");
            }
            Message::ToolResult(_) => {
                let mut images = Vec::new();
                while let Some(Message::ToolResult(r)) = transformed.get(i) {
                    let text = r
                        .content
                        .iter()
                        .filter_map(|b| match b {
                            ContentBlock::Text { text } => Some(text.as_str()),
                            _ => None,
                        })
                        .collect::<Vec<_>>()
                        .join("\n");
                    let mut msg = json!({
                        "role": "tool",
                        "content": sanitize_text(if text.is_empty() { "(see attached image)" } else { &text }),
                        "tool_call_id": r.tool_call_id,
                    });
                    if compat.requires_tool_result_name && !r.tool_name.is_empty() {
                        msg["name"] = json!(r.tool_name);
                    }
                    out.push(msg);
                    if model.supports_images() {
                        for block in &r.content {
                            if let ContentBlock::Image { data, mime_type } = block {
                                images.push(image_part(data, mime_type));
                            }
                        }
                    }
                    i += 1;
                }
                if images.is_empty() {
                    last_role = Some("toolResult");
                } else {
                    if compat.requires_assistant_after_tool_result {
                        out.push(json!({ "role": "assistant", "content": TOOL_RESULT_BRIDGE }));
                    }
                    let mut parts = vec![
                        json!({ "type": "text", "text": "Attached image(s) from tool result:" }),
                    ];
                    parts.extend(images);
                    out.push(json!({ "role": "user", "content": parts }));
                    last_role = Some("user");
                }
                continue;
            }
        }
        i += 1;
    }
    out
}

pub(crate) async fn stream(
    http_client: &reqwest::Client,
    provider: &ProviderConfig,
    model: &ModelSpec,
    request: &Request,
    cancel: &CancellationToken,
    partial: &mut PartialMessage,
    on_event: &mut (dyn FnMut(StreamEvent) + Send),
) -> Result<()> {
    if provider.needs_api_key() && provider.api_key.is_none() {
        bail!("No API key configured for {}", provider.label);
    }
    let built = build_request(provider, model, request)?;
    let body = serde_json::to_vec(&built.body)?;
    let make = || with_headers(http_client.post(&built.url), &built.headers).body(body.clone());
    let response = match http::send(&make, &provider.retry, cancel, None).await? {
        Sent::Ok(r) => r,
        Sent::Failed(f) => bail!(openrouter_error(&f.body).unwrap_or_else(|| f.message())),
        Sent::Cancelled => return Ok(()),
    };
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
            let Ok(value) = serde_json::from_str::<Value>(&event.data) else {
                continue;
            };
            apply_chunk(&value, &mut state, partial, on_event)?;
        }
        if ended {
            return Ok(());
        }
    }
}

/// `error.message` plus OpenRouter's `error.metadata.raw` detail.
fn openrouter_error(body: &str) -> Option<String> {
    let value: Value = serde_json::from_str(body).ok()?;
    let error = value.get("error")?;
    let message = error.get("message")?.as_str()?;
    let raw = error
        .get("metadata")
        .and_then(|m| m.get("raw"))
        .and_then(Value::as_str)?;
    Some(format!("{message}\n{raw}"))
}

/// Tracks the content index assigned to each streamed choice part.
#[derive(Default)]
struct ChunkState {
    text_index: Option<usize>,
    thinking_index: Option<usize>,
    /// Provider tool-call index -> content block index.
    tool_by_index: HashMap<u64, usize>,
    tool_by_id: HashMap<String, usize>,
}

fn map_finish_reason(reason: &str) -> (StopReason, Option<String>) {
    match reason {
        "stop" | "end" => (StopReason::Stop, None),
        "length" => (StopReason::Length, None),
        "function_call" | "tool_calls" => (StopReason::ToolCall, None),
        other => (
            StopReason::Error,
            Some(format!("Provider finish_reason: {other}")),
        ),
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
        let mut msg = err
            .get("message")
            .and_then(Value::as_str)
            .map(str::to_string)
            .unwrap_or_else(|| err.to_string());
        if let Some(raw) = err
            .get("metadata")
            .and_then(|m| m.get("raw"))
            .and_then(Value::as_str)
        {
            msg.push('\n');
            msg.push_str(raw);
        }
        bail!("{msg}");
    }
    if let Some(id) = value.get("id").and_then(Value::as_str) {
        partial
            .message
            .response_id
            .get_or_insert_with(|| id.to_string());
    }
    if let Some(usage) = value.get("usage").filter(|u| u.is_object()) {
        apply_usage(usage, partial);
    }
    let Some(choice) = value.get("choices").and_then(|c| c.get(0)) else {
        return Ok(());
    };
    if value.get("usage").is_none_or(Value::is_null)
        && let Some(usage) = choice.get("usage").filter(|u| u.is_object())
    {
        apply_usage(usage, partial);
    }
    if let Some(reason) = choice.get("finish_reason").and_then(Value::as_str) {
        match map_finish_reason(reason) {
            (_, Some(error)) => partial.set_stop_error(error),
            (stop, None) => partial.set_stop(stop),
        }
    }
    let Some(delta) = choice.get("delta") else {
        return Ok(());
    };
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
    if let Some((field, text)) = REASONING_FIELDS.iter().find_map(|f| {
        delta
            .get(*f)
            .and_then(Value::as_str)
            .filter(|t| !t.is_empty())
            .map(|t| (*f, t))
    }) {
        let index = match state.thinking_index {
            Some(i) => i,
            None => {
                let i = partial.open(
                    ContentBlock::Thinking {
                        thinking: String::new(),
                        signature: Some(field.to_string()),
                        redacted: false,
                    },
                    on_event,
                );
                state.thinking_index = Some(i);
                i
            }
        };
        partial.push_thinking(index, text, on_event);
    }
    if let Some(calls) = delta.get("tool_calls").and_then(Value::as_array) {
        for call in calls {
            let stream_index = call.get("index").and_then(Value::as_u64);
            let call_id = call
                .get("id")
                .and_then(Value::as_str)
                .filter(|s| !s.is_empty());
            let function = call.get("function");
            let existing = stream_index
                .and_then(|i| state.tool_by_index.get(&i).copied())
                .or_else(|| call_id.and_then(|id| state.tool_by_id.get(id).copied()));
            let index = match existing {
                Some(i) => i,
                None => {
                    let name = function
                        .and_then(|f| f.get("name"))
                        .and_then(Value::as_str)
                        .unwrap_or_default();
                    partial.open(
                        ContentBlock::ToolCall {
                            id: call_id.unwrap_or_default().to_string(),
                            name: name.to_string(),
                            arguments: Value::Null,
                        },
                        on_event,
                    )
                }
            };
            if let Some(si) = stream_index {
                state.tool_by_index.insert(si, index);
            }
            if let Some(id) = call_id {
                state.tool_by_id.insert(id.to_string(), index);
            }
            if let Some(ContentBlock::ToolCall { id, name, .. }) =
                partial.message.content.get_mut(index)
            {
                if id.is_empty()
                    && let Some(cid) = call_id
                {
                    *id = cid.to_string();
                }
                if name.is_empty()
                    && let Some(n) = function.and_then(|f| f.get("name")).and_then(Value::as_str)
                {
                    *name = n.to_string();
                }
            }
            match function.and_then(|f| f.get("arguments")) {
                Some(Value::String(s)) => partial.push_tool_args(index, s, on_event),
                Some(args @ Value::Object(_)) => {
                    partial.push_tool_args(index, &args.to_string(), on_event)
                }
                _ => {}
            }
        }
    }
    Ok(())
}

/// Pi `parseChunkUsage`: cache reads exclude this request's cache writes.
fn apply_usage(raw: &Value, partial: &mut PartialMessage) {
    let get = |v: &Value, k: &str| v.get(k).and_then(Value::as_u64);
    let prompt = get(raw, "prompt_tokens").unwrap_or(0);
    let details = raw.get("prompt_tokens_details");
    let reported_cached = details
        .and_then(|d| get(d, "cached_tokens"))
        .or_else(|| get(raw, "prompt_cache_hit_tokens"))
        .unwrap_or(0);
    let cache_write = details
        .and_then(|d| get(d, "cache_write_tokens"))
        .unwrap_or(0);
    let cache_read = if cache_write > 0 {
        reported_cached.saturating_sub(cache_write)
    } else {
        reported_cached
    };
    let input = prompt.saturating_sub(cache_read + cache_write);
    let output = get(raw, "completion_tokens").unwrap_or(0);
    partial.message.usage = Usage {
        input_tokens: input,
        output_tokens: output,
        cache_read_tokens: cache_read,
        cache_write_tokens: cache_write,
        total_tokens: input + output + cache_read + cache_write,
        total_cost_usd: 0.0,
    };
    if let Some(cost) = raw.get("cost").and_then(Value::as_f64) {
        partial.reported_cost = Some(cost);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::catalog::ModelCatalog;
    use crate::message::ToolResultMessage;
    use crate::provider::test_server::{MockResponse, serve, sse_data};
    use crate::provider::{ProviderKind, ToolSpec};

    fn openrouter() -> ProviderConfig {
        ProviderConfig {
            id: "openrouter".into(),
            label: "OpenRouter".into(),
            kind: ProviderKind::OpenAiCompletions,
            base_url: "https://openrouter.ai/api/v1".into(),
            api_key: Some("k".into()),
            ..Default::default()
        }
    }

    fn spec(provider: &str, id: &str, url: &str) -> ModelSpec {
        ModelSpec::fallback(provider, "openai-completions", url, id)
    }

    fn tool() -> ToolSpec {
        ToolSpec {
            name: "ls".into(),
            description: "d".into(),
            parameters: json!({"type":"object"}),
        }
    }

    #[test]
    fn applies_streamed_chunks() {
        let provider = openrouter();
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
        assert!(
            matches!(&m.content[0], ContentBlock::Thinking { signature: Some(s), .. } if s == "reasoning")
        );
    }

    #[test]
    fn content_filter_becomes_error_and_usage_excludes_writes() {
        let provider = openrouter();
        let mut partial = PartialMessage::new(&provider, "m");
        let mut state = ChunkState::default();
        apply_chunk(
            &json!({"choices":[{"delta":{"content":"x"},"finish_reason":"content_filter"}],
                    "usage":{"prompt_tokens":100,"completion_tokens":5,"prompt_tokens_details":{"cached_tokens":50,"cache_write_tokens":20}}}),
            &mut state,
            &mut partial,
            &mut |_| {},
        )
        .unwrap();
        assert_eq!(partial.message.usage.cache_read_tokens, 30);
        assert_eq!(partial.message.usage.cache_write_tokens, 20);
        assert_eq!(partial.message.usage.input_tokens, 50);
        let m = partial.finish();
        assert_eq!(m.stop_reason, StopReason::Error);
        assert_eq!(
            m.error_message.as_deref(),
            Some("Provider finish_reason: content_filter")
        );
    }

    #[test]
    fn body_includes_tools_reasoning_and_tool_results() {
        let provider = openrouter();
        let call = Message::Assistant(AssistantMessage {
            provider: "openrouter".into(),
            api: "openai-completions".into(),
            model: "m".into(),
            content: vec![
                ContentBlock::Thinking {
                    thinking: "plan".into(),
                    signature: Some("reasoning".into()),
                    redacted: false,
                },
                ContentBlock::ToolCall {
                    id: "c1".into(),
                    name: "ls".into(),
                    arguments: json!({}),
                },
            ],
            stop_reason: StopReason::ToolCall,
            ..Default::default()
        });
        let result = Message::ToolResult(ToolResultMessage {
            tool_call_id: "c1".into(),
            tool_name: "ls".into(),
            content: vec![ContentBlock::text("a/")],
            ..Default::default()
        });
        let request = Request {
            model: "m".into(),
            system: "sys".into(),
            messages: vec![Message::user_text("hi"), call, result],
            tools: vec![tool()],
            max_tokens: 100,
            thinking: ThinkingLevel::High,
            ..Default::default()
        };
        let built = build_request(
            &provider,
            &spec("openrouter", "m", "https://openrouter.ai/api/v1"),
            &request,
        )
        .unwrap();
        let body = &built.body;
        let msgs = body["messages"].as_array().unwrap();
        assert_eq!(msgs.len(), 4);
        assert_eq!(msgs[0]["role"], "developer");
        assert_eq!(msgs[2]["tool_calls"][0]["function"]["arguments"], "{}");
        assert_eq!(msgs[2]["reasoning"], "plan");
        assert!(msgs[2]["content"].is_null());
        assert_eq!(msgs[3]["role"], "tool");
        assert_eq!(body["tools"][0]["function"]["strict"], false);
        assert_eq!(body["reasoning"]["effort"], "high");
        assert_eq!(body["max_completion_tokens"], 100);
        assert_eq!(body["store"], false);
        assert_eq!(body["stream_options"]["include_usage"], true);
        assert_eq!(built.url, "https://openrouter.ai/api/v1/chat/completions");
        assert_eq!(built.headers["authorization"], "Bearer k");
        assert_eq!(built.headers["X-OpenRouter-Title"], "qt");
        assert_eq!(built.headers["X-OpenRouter-Categories"], "cli-agent");
        assert_eq!(built.headers["HTTP-Referer"], OPENROUTER_REFERER);

        let off = Request {
            thinking: ThinkingLevel::Off,
            ..request.clone()
        };
        let built = build_request(
            &provider,
            &spec("openrouter", "m", "https://openrouter.ai/api/v1"),
            &off,
        )
        .unwrap();
        assert_eq!(built.body["reasoning"]["effort"], "none");

        let mut quiet = provider.clone();
        quiet.compat.insert("attribution".into(), json!(false));
        let built = build_request(
            &quiet,
            &spec("openrouter", "m", "https://openrouter.ai/api/v1"),
            &request,
        )
        .unwrap();
        assert!(!built.headers.contains_key("X-OpenRouter-Title"));
    }

    #[test]
    fn compat_matrix_shapes_messages() {
        let catalog = ModelCatalog::builtin();
        let together = catalog
            .find_model("together", "deepseek-ai/DeepSeek-V4-Pro")
            .unwrap();
        let provider = ProviderConfig {
            id: "together".into(),
            api_key: Some("t".into()),
            compat:
                json!({ "requiresToolResultName": true, "requiresAssistantAfterToolResult": true })
                    .as_object()
                    .cloned()
                    .unwrap(),
            ..Default::default()
        };
        let call = Message::Assistant(AssistantMessage {
            provider: "together".into(),
            api: "openai-completions".into(),
            model: together.id.clone(),
            content: vec![ContentBlock::ToolCall {
                id: "c1".into(),
                name: "ls".into(),
                arguments: json!({}),
            }],
            stop_reason: StopReason::ToolCall,
            ..Default::default()
        });
        let result = Message::ToolResult(ToolResultMessage {
            tool_call_id: "c1".into(),
            tool_name: "ls".into(),
            content: vec![ContentBlock::text("x")],
            ..Default::default()
        });
        let request = Request {
            model: together.id.clone(),
            system: "sys".into(),
            messages: vec![
                Message::user_text("a"),
                call,
                result,
                Message::user_text("b"),
            ],
            thinking: ThinkingLevel::Low,
            ..Default::default()
        };
        let built = build_request(&provider, together, &request).unwrap();
        let body = &built.body;
        let msgs = body["messages"].as_array().unwrap();
        assert_eq!(msgs[0]["role"], "system");
        assert_eq!(msgs[3]["name"], "ls");
        assert_eq!(
            msgs[4],
            json!({"role":"assistant","content":TOOL_RESULT_BRIDGE})
        );
        assert_eq!(msgs[2]["content"], "");
        assert!(body.get("store").is_none());
        assert_eq!(body["max_tokens"], 32_000);
        // Low clamps to high, the only supported level.
        assert_eq!(body["reasoning"], json!({"enabled": true}));
        assert_eq!(body["reasoning_effort"], "high");
        assert_eq!(body["tools"], json!([]));

        let zai = catalog.find_model("zai", "glm-5").unwrap();
        let p = ProviderConfig {
            id: "zai".into(),
            api_key: Some("z".into()),
            ..Default::default()
        };
        let req = Request {
            model: "glm-5".into(),
            tools: vec![tool()],
            messages: vec![Message::user_text("x")],
            ..Default::default()
        };
        let built = build_request(&p, zai, &req).unwrap();
        assert_eq!(built.body["enable_thinking"], false);
        assert_eq!(built.body["tool_stream"], true);
        assert_eq!(
            built.url,
            "https://api.z.ai/api/coding/paas/v4/chat/completions"
        );

        let deepseek = spec("deepseek", "deepseek-reasoner", "https://api.deepseek.com");
        let p = ProviderConfig {
            id: "deepseek".into(),
            ..Default::default()
        };
        let req = Request {
            model: deepseek.id.clone(),
            thinking: ThinkingLevel::Medium,
            messages: vec![
                Message::user_text("x"),
                Message::Assistant(AssistantMessage {
                    content: vec![ContentBlock::text("y")],
                    ..Default::default()
                }),
            ],
            ..Default::default()
        };
        let body = build_request(&p, &deepseek, &req).unwrap().body;
        assert_eq!(body["thinking"]["type"], "enabled");
        assert_eq!(body["reasoning_effort"], "medium");
        assert_eq!(body["messages"][1]["reasoning_content"], "");
    }

    #[test]
    fn anthropic_cache_control_and_routing_for_openrouter() {
        let mut model = spec(
            "openrouter",
            "anthropic/claude-sonnet-4",
            "https://openrouter.ai/api/v1",
        );
        model.compat = json!({ "openRouterRouting": { "only": ["anthropic"] } })
            .as_object()
            .cloned()
            .unwrap();
        let request = Request {
            model: model.id.clone(),
            system: "sys".into(),
            messages: vec![Message::user_text("hello")],
            tools: vec![tool()],
            session_id: Some("s".into()),
            cache_retention: Some(CacheRetention::Long),
            ..Default::default()
        };
        let body = build_request(&openrouter(), &model, &request).unwrap().body;
        assert_eq!(
            body["messages"][0]["content"][0]["cache_control"],
            json!({"type":"ephemeral","ttl":"1h"})
        );
        assert_eq!(
            body["messages"][1]["content"][0]["cache_control"]["type"],
            "ephemeral"
        );
        assert_eq!(body["tools"][0]["cache_control"]["type"], "ephemeral");
        assert_eq!(body["provider"]["only"][0], "anthropic");
        assert_eq!(body["prompt_cache_key"], "s");
        assert_eq!(body["prompt_cache_retention"], "24h");
    }

    #[test]
    fn responses_ids_are_shortened_and_images_follow_tool_results() {
        let mut model = spec("openai", "gpt-4o", "https://api.openai.com/v1");
        model.reasoning = false;
        let provider = ProviderConfig {
            id: "openai".into(),
            api_key: Some("k".into()),
            ..Default::default()
        };
        let long_id = format!("call_abc|fc_{}", "x".repeat(300));
        let call = Message::Assistant(AssistantMessage {
            provider: "openai-codex".into(),
            api: "openai-codex-responses".into(),
            model: "gpt-5".into(),
            content: vec![ContentBlock::ToolCall {
                id: long_id.clone(),
                name: "read".into(),
                arguments: json!({}),
            }],
            stop_reason: StopReason::ToolCall,
            ..Default::default()
        });
        let result = Message::ToolResult(ToolResultMessage {
            tool_call_id: long_id,
            tool_name: "read".into(),
            content: vec![ContentBlock::Image {
                data: "AA".into(),
                mime_type: "image/png".into(),
            }],
            ..Default::default()
        });
        let request = Request {
            model: "gpt-4o".into(),
            system: "s".into(),
            messages: vec![Message::user_text("x"), call, result],
            session_id: Some("sess".into()),
            ..Default::default()
        };
        let body = build_request(&provider, &model, &request).unwrap().body;
        let msgs = body["messages"].as_array().unwrap();
        assert_eq!(msgs[0]["role"], "system");
        assert_eq!(msgs[2]["tool_calls"][0]["id"], "call_abc");
        assert_eq!(msgs[3]["tool_call_id"], "call_abc");
        assert_eq!(msgs[3]["content"], "(see attached image)");
        assert_eq!(
            msgs[4]["content"][0]["text"],
            "Attached image(s) from tool result:"
        );
        assert_eq!(msgs[4]["content"][1]["type"], "image_url");
        assert_eq!(body["prompt_cache_key"], "sess");
        assert!(body.get("prompt_cache_retention").is_none());
    }

    #[test]
    fn thinking_only_turns_are_not_replayed() {
        let thinking_only = Message::Assistant(AssistantMessage {
            provider: "openrouter".into(),
            api: "openai-completions".into(),
            model: "m".into(),
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
            ..Default::default()
        };
        let mut model = spec("openrouter", "m", "https://openrouter.ai/api/v1");
        model.reasoning = false;
        let body = build_request(&openrouter(), &model, &request).unwrap().body;
        let roles: Vec<&str> = body["messages"]
            .as_array()
            .unwrap()
            .iter()
            .map(|m| m["role"].as_str().unwrap())
            .collect();
        assert_eq!(roles, ["system", "user", "user"]);
    }

    #[tokio::test]
    async fn streams_against_mock_server_with_cost() {
        let body = format!(
            "{}data: [DONE]\n\n",
            sse_data(&[
                json!({"id":"x","choices":[{"delta":{"content":"ok"}}]}),
                json!({"choices":[{"delta":{},"finish_reason":"stop"}],"usage":{"prompt_tokens":1000000,"completion_tokens":0,"cost":0.5}}),
            ])
        );
        let (base, requests) = serve(vec![
            MockResponse::json(500, r#"{"error":{"message":"boom"}}"#),
            MockResponse::sse(body.clone()),
            MockResponse::json(400, r#"{"error":{"message":"Provider returned error","metadata":{"raw":"upstream detail"}}}"#),
        ])
        .await;
        let provider = ProviderConfig {
            id: "custom".into(),
            api_key: Some("k".into()),
            base_url: base.clone(),
            retry: crate::provider::RetryPolicy {
                max_retries: 1,
                ..Default::default()
            },
            ..Default::default()
        };
        let request = Request {
            model: "m".into(),
            messages: vec![Message::user_text("x")],
            ..Default::default()
        };
        let http = reqwest::Client::new();
        let cancel = CancellationToken::new();
        let msg = crate::provider::stream(&http, &provider, &request, &cancel, &mut |_| {}).await;
        assert_eq!(msg.text(), "ok");
        assert_eq!(msg.stop_reason, StopReason::Stop);
        assert_eq!(msg.usage.total_cost_usd, 0.5);
        assert_eq!(requests.lock().unwrap().len(), 2);
        assert_eq!(requests.lock().unwrap()[1].path, "/chat/completions");

        let msg = crate::provider::stream(&http, &provider, &request, &cancel, &mut |_| {}).await;
        assert_eq!(msg.stop_reason, StopReason::Error);
        assert_eq!(
            msg.error_message.as_deref(),
            Some("Provider returned error\nupstream detail")
        );
    }
}
