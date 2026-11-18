//! Anthropic Messages API (Pi `anthropic-messages`).
//!
//! Supports API keys (`x-api-key`), Claude Pro/Max OAuth tokens
//! (`sk-ant-oat…`, sent as Bearer with the Claude Code identity, betas and
//! tool-name casing), adaptive thinking for Opus/Sonnet 4.6+, budget thinking
//! for older models, prompt caching with optional 1 h TTL, and the
//! fine-grained tool streaming / interleaved thinking betas.

use std::collections::{BTreeMap, HashMap};

use anyhow::{Result, bail};
use futures::StreamExt as _;
use serde_json::{Value, json};
use tokio_util::sync::CancellationToken;

use super::compat::{self, AnthropicCompat};
use super::http::{self, Sent};
use super::transform::{Target, sanitize_id, transform_messages};
use super::{
    CacheRetention, PartialMessage, ProviderConfig, Request, StreamEvent, ThinkingLevel,
    effective_max_tokens, sanitize_text, with_headers,
};
use crate::catalog::ModelSpec;
use crate::message::{ContentBlock, Message, StopReason, UserContent};
use crate::sse::SseParser;

pub const API_VERSION: &str = "2023-06-01";
pub const CLAUDE_CODE_VERSION: &str = "2.1.75";
pub const CLAUDE_CODE_IDENTITY: &str = "You are Claude Code, Anthropic's official CLI for Claude.";
pub const FINE_GRAINED_TOOL_STREAMING_BETA: &str = "fine-grained-tool-streaming-2025-05-14";
pub const INTERLEAVED_THINKING_BETA: &str = "interleaved-thinking-2025-05-14";

/// Claude Code 2.x tool names (canonical casing).
const CLAUDE_CODE_TOOLS: &[&str] = &[
    "Read",
    "Write",
    "Edit",
    "Bash",
    "Grep",
    "Glob",
    "AskUserQuestion",
    "EnterPlanMode",
    "ExitPlanMode",
    "KillShell",
    "NotebookEdit",
    "Skill",
    "Task",
    "TaskOutput",
    "TodoWrite",
    "WebFetch",
    "WebSearch",
];

/// Claude Code casing for a tool name when it matches one case-insensitively.
pub fn to_claude_code_name(name: &str) -> String {
    CLAUDE_CODE_TOOLS
        .iter()
        .find(|t| t.eq_ignore_ascii_case(name))
        .map(|t| t.to_string())
        .unwrap_or_else(|| name.to_string())
}

/// Map a streamed tool name back to the request's tool casing.
pub fn from_claude_code_name(name: &str, tools: &[super::ToolSpec]) -> String {
    tools
        .iter()
        .find(|t| t.name.eq_ignore_ascii_case(name))
        .map(|t| t.name.clone())
        .unwrap_or_else(|| name.to_string())
}

/// Claude Pro/Max OAuth access tokens.
pub fn is_oauth_token(key: &str) -> bool {
    key.contains("sk-ant-oat")
}

/// Opus 4.6+/Sonnet 4.6 use adaptive thinking.
pub fn supports_adaptive_thinking(model_id: &str) -> bool {
    [
        "opus-4-6",
        "opus-4.6",
        "opus-4-7",
        "opus-4.7",
        "sonnet-4-6",
        "sonnet-4.6",
    ]
    .iter()
    .any(|needle| model_id.contains(needle))
}

/// Adaptive-thinking effort for a level (Pi `mapThinkingLevelToEffort`).
pub fn effort_for(model: &ModelSpec, level: ThinkingLevel) -> String {
    if let Some(Some(mapped)) = model.mapped_level(level) {
        return mapped.to_string();
    }
    match level {
        ThinkingLevel::Minimal | ThinkingLevel::Low => "low",
        ThinkingLevel::Medium => "medium",
        _ => "high",
    }
    .to_string()
}

/// Pi `adjustMaxTokensForThinking`: (max_tokens, thinking budget).
pub fn adjust_max_tokens_for_thinking(
    base: u32,
    model_max: u32,
    level: ThinkingLevel,
) -> (u32, u32) {
    let mut budget = level.budget_tokens().unwrap_or(0);
    let model_max = if model_max == 0 { u32::MAX } else { model_max };
    let max_tokens = base.saturating_add(budget).min(model_max);
    if max_tokens <= budget {
        budget = max_tokens.saturating_sub(1_024);
    }
    (max_tokens, budget)
}

/// Authorise a non-streaming request (model listing).
pub(crate) fn authorize(
    builder: reqwest::RequestBuilder,
    provider: &ProviderConfig,
) -> reqwest::RequestBuilder {
    let builder = builder.header("anthropic-version", API_VERSION);
    match &provider.api_key {
        Some(key) if is_oauth_token(key) => builder
            .bearer_auth(key)
            .header("anthropic-beta", "oauth-2025-04-20"),
        Some(key) => builder.header("x-api-key", key),
        None => builder,
    }
}

/// A fully built request.
#[derive(Debug, Clone, PartialEq)]
pub struct Built {
    pub url: String,
    pub headers: BTreeMap<String, String>,
    pub body: Value,
    pub oauth: bool,
}

fn cache_control(compat: &AnthropicCompat, retention: CacheRetention) -> Option<Value> {
    match retention {
        CacheRetention::None => None,
        CacheRetention::Long if compat.supports_long_cache_retention => {
            Some(json!({ "type": "ephemeral", "ttl": "1h" }))
        }
        _ => Some(json!({ "type": "ephemeral" })),
    }
}

/// Build the URL, headers and body for one request.
pub fn build_request(
    provider: &ProviderConfig,
    model: &ModelSpec,
    request: &Request,
) -> Result<Built> {
    let key = provider.api_key.clone().unwrap_or_default();
    let compat = compat::anthropic(provider, model);
    let retention = CacheRetention::resolve(request.cache_retention);
    let cache = cache_control(&compat, retention);
    let cloudflare = model.provider == "cloudflare-ai-gateway";
    let oauth = !cloudflare && is_oauth_token(&key);
    let level = super::request_level(model, request.thinking);
    let adaptive = supports_adaptive_thinking(&model.id);

    let mut betas: Vec<&str> = Vec::new();
    if !request.tools.is_empty() && !compat.supports_eager_tool_input_streaming {
        betas.push(FINE_GRAINED_TOOL_STREAMING_BETA);
    }
    if !adaptive {
        betas.push(INTERLEAVED_THINKING_BETA);
    }
    let mut headers = BTreeMap::new();
    headers.insert("accept".to_string(), "application/json".to_string());
    headers.insert(
        "anthropic-dangerous-direct-browser-access".to_string(),
        "true".to_string(),
    );
    headers.insert("anthropic-version".to_string(), API_VERSION.to_string());
    headers.insert("content-type".to_string(), "application/json".to_string());
    if cloudflare {
        headers.insert("cf-aig-authorization".into(), format!("Bearer {key}"));
        if !betas.is_empty() {
            headers.insert("anthropic-beta".into(), betas.join(","));
        }
    } else if oauth {
        let mut all = vec!["claude-code-20250219", "oauth-2025-04-20"];
        all.extend(betas.iter().copied());
        headers.insert("anthropic-beta".into(), all.join(","));
        headers.insert("authorization".into(), format!("Bearer {key}"));
        headers.insert(
            "user-agent".into(),
            format!("claude-cli/{CLAUDE_CODE_VERSION}"),
        );
        headers.insert("x-app".into(), "cli".into());
    } else {
        if !betas.is_empty() {
            headers.insert("anthropic-beta".into(), betas.join(","));
        }
        if !key.is_empty() {
            headers.insert("x-api-key".into(), key.clone());
        }
        if let Some(session) = &request.session_id
            && retention != CacheRetention::None
            && compat.send_session_affinity_headers
        {
            headers.insert("x-session-affinity".into(), session.clone());
        }
    }
    if (provider.auth_header || model.auth_header) && !key.is_empty() {
        headers.insert("authorization".into(), format!("Bearer {key}"));
    }
    headers.extend(provider.headers.clone());
    headers.extend(model.headers.clone());
    headers.extend(request.headers.clone());

    let base_max = effective_max_tokens(request, model);
    let mut body = json!({
        "model": model.id,
        "messages": convert_messages(&request.messages, provider, model, oauth, cache.as_ref()),
        "max_tokens": base_max,
        "stream": true,
    });
    let mut system = Vec::new();
    if oauth {
        let mut identity = json!({ "type": "text", "text": CLAUDE_CODE_IDENTITY });
        if let Some(c) = &cache {
            identity["cache_control"] = c.clone();
        }
        system.push(identity);
    }
    if !request.system.is_empty() {
        let mut block = json!({ "type": "text", "text": sanitize_text(&request.system) });
        if let Some(c) = &cache {
            block["cache_control"] = c.clone();
        }
        system.push(block);
    }
    if !system.is_empty() {
        body["system"] = Value::Array(system);
    }
    let thinking_enabled = level != ThinkingLevel::Off;
    if let Some(t) = request.temperature
        && !thinking_enabled
    {
        body["temperature"] = json!(t);
    }
    if !request.tools.is_empty() {
        let tool_cache = cache
            .as_ref()
            .filter(|_| compat.supports_cache_control_on_tools);
        let count = request.tools.len();
        let tools: Vec<Value> = request
            .tools
            .iter()
            .enumerate()
            .map(|(i, tool)| {
                let mut t = json!({
                    "name": if oauth { to_claude_code_name(&tool.name) } else { tool.name.clone() },
                    "description": tool.description,
                });
                if compat.supports_eager_tool_input_streaming {
                    t["eager_input_streaming"] = json!(true);
                }
                t["input_schema"] = json!({
                    "type": "object",
                    "properties": tool.parameters.get("properties").cloned().unwrap_or_else(|| json!({})),
                    "required": tool.parameters.get("required").cloned().unwrap_or_else(|| json!([])),
                });
                if i + 1 == count
                    && let Some(c) = tool_cache
                {
                    t["cache_control"] = c.clone();
                }
                t
            })
            .collect();
        body["tools"] = Value::Array(tools);
    }
    if model.reasoning {
        if thinking_enabled {
            if adaptive {
                body["thinking"] = json!({ "type": "adaptive", "display": "summarized" });
                body["output_config"] = json!({ "effort": effort_for(model, level) });
            } else {
                let model_max = u32::try_from(model.max_tokens).unwrap_or(u32::MAX);
                let (max_tokens, budget) =
                    adjust_max_tokens_for_thinking(base_max, model_max, level);
                body["max_tokens"] = json!(max_tokens);
                body["thinking"] = json!({
                    "type": "enabled",
                    "budget_tokens": if budget == 0 { 1_024 } else { budget },
                    "display": "summarized",
                });
            }
        } else {
            body["thinking"] = json!({ "type": "disabled" });
        }
    }

    let base =
        super::resolve_url_placeholders(model.base_url.trim_end_matches('/'), &model.provider)?;
    Ok(Built {
        url: format!("{base}/v1/messages"),
        headers,
        body,
        oauth,
    })
}

/// Text-only content joins into a string; with images a block list.
fn result_content(blocks: &[ContentBlock]) -> Value {
    let has_images = blocks
        .iter()
        .any(|b| matches!(b, ContentBlock::Image { .. }));
    if !has_images {
        let texts: Vec<&str> = blocks
            .iter()
            .filter_map(|b| match b {
                ContentBlock::Text { text } => Some(text.as_str()),
                _ => None,
            })
            .collect();
        return json!(sanitize_text(&texts.join("\n")));
    }
    let mut out: Vec<Value> = blocks
        .iter()
        .filter_map(|b| match b {
            ContentBlock::Text { text } => {
                Some(json!({ "type": "text", "text": sanitize_text(text) }))
            }
            ContentBlock::Image { data, mime_type } => Some(json!({
                "type": "image",
                "source": { "type": "base64", "media_type": mime_type, "data": data },
            })),
            _ => None,
        })
        .collect();
    if !out.iter().any(|b| b["type"] == "text") {
        out.insert(0, json!({ "type": "text", "text": "(see attached image)" }));
    }
    Value::Array(out)
}

fn convert_messages(
    messages: &[Message],
    provider: &ProviderConfig,
    model: &ModelSpec,
    oauth: bool,
    cache: Option<&Value>,
) -> Vec<Value> {
    let target = Target {
        provider: &provider.id,
        api: "anthropic-messages",
        model: &model.id,
        images: model.supports_images(),
    };
    let normalize = |id: &str, _: &crate::message::AssistantMessage| {
        sanitize_id(id).chars().take(64).collect::<String>()
    };
    let transformed = transform_messages(messages, &target, Some(&normalize));
    let mut out: Vec<Value> = Vec::new();
    let mut i = 0;
    while i < transformed.len() {
        match &transformed[i] {
            Message::User { content, .. } => match content {
                UserContent::Text(text) => {
                    if !text.trim().is_empty() {
                        out.push(json!({ "role": "user", "content": sanitize_text(text) }));
                    }
                }
                UserContent::Blocks(blocks) => {
                    let blocks: Vec<Value> = blocks
                        .iter()
                        .filter_map(|b| match b {
                            ContentBlock::Text { text } if !text.trim().is_empty() => {
                                Some(json!({ "type": "text", "text": sanitize_text(text) }))
                            }
                            ContentBlock::Image { data, mime_type } => Some(json!({
                                "type": "image",
                                "source": { "type": "base64", "media_type": mime_type, "data": data },
                            })),
                            _ => None,
                        })
                        .collect();
                    if !blocks.is_empty() {
                        out.push(json!({ "role": "user", "content": blocks }));
                    }
                }
            },
            Message::Assistant(a) => {
                let mut blocks = Vec::new();
                for block in &a.content {
                    match block {
                        ContentBlock::Text { text } if !text.trim().is_empty() => {
                            blocks.push(json!({ "type": "text", "text": sanitize_text(text) }));
                        }
                        ContentBlock::Thinking {
                            thinking,
                            signature,
                            redacted,
                        } => {
                            if *redacted {
                                blocks.push(json!({
                                    "type": "redacted_thinking",
                                    "data": signature.clone().unwrap_or_default(),
                                }));
                            } else if thinking.trim().is_empty() {
                            } else if let Some(sig) = signature.as_deref().filter(|s| !s.trim().is_empty()) {
                                blocks.push(json!({
                                    "type": "thinking",
                                    "thinking": sanitize_text(thinking),
                                    "signature": sig,
                                }));
                            } else {
                                blocks.push(json!({ "type": "text", "text": sanitize_text(thinking) }));
                            }
                        }
                        ContentBlock::ToolCall {
                            id,
                            name,
                            arguments,
                        } => blocks.push(json!({
                            "type": "tool_use",
                            "id": id,
                            "name": if oauth { to_claude_code_name(name) } else { name.clone() },
                            "input": if arguments.is_object() { arguments.clone() } else { json!({}) },
                        })),
                        _ => {}
                    }
                }
                if !blocks.is_empty() {
                    out.push(json!({ "role": "assistant", "content": blocks }));
                }
            }
            Message::ToolResult(_) => {
                let mut results = Vec::new();
                while let Some(Message::ToolResult(r)) = transformed.get(i) {
                    results.push(json!({
                        "type": "tool_result",
                        "tool_use_id": r.tool_call_id,
                        "content": result_content(&r.content),
                        "is_error": r.is_error,
                    }));
                    i += 1;
                }
                out.push(json!({ "role": "user", "content": results }));
                continue;
            }
        }
        i += 1;
    }
    if let Some(cache) = cache
        && let Some(last) = out.last_mut()
        && last["role"] == "user"
    {
        if let Some(text) = last["content"].as_str().map(str::to_string) {
            last["content"] = json!([{ "type": "text", "text": text, "cache_control": cache }]);
        } else if let Some(block) = last["content"].as_array_mut().and_then(|c| c.last_mut())
            && matches!(
                block["type"].as_str(),
                Some("text" | "image" | "tool_result")
            )
        {
            block["cache_control"] = cache.clone();
        }
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
    if provider.api_key.is_none() && !provider.local {
        bail!("No API key configured for {}", provider.label);
    }
    let built = build_request(provider, model, request)?;
    let body = serde_json::to_vec(&built.body)?;
    let make = || with_headers(http_client.post(&built.url), &built.headers).body(body.clone());
    let response = match http::send(&make, &provider.retry, cancel, None).await? {
        Sent::Ok(r) => r,
        Sent::Failed(f) => bail!(f.message()),
        Sent::Cancelled => return Ok(()),
    };
    let mut bytes = response.bytes_stream();
    let mut parser = SseParser::new();
    let mut state = StreamState {
        oauth: built.oauth,
        tools: request.tools.clone(),
        ..Default::default()
    };
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
    oauth: bool,
    tools: Vec<super::ToolSpec>,
}

fn map_stop_reason(reason: &str) -> Result<StopReason> {
    Ok(match reason {
        "end_turn" | "pause_turn" | "stop_sequence" => StopReason::Stop,
        "max_tokens" => StopReason::Length,
        "tool_use" => StopReason::ToolCall,
        "refusal" | "sensitive" => StopReason::Error,
        other => bail!("Unhandled stop reason: {other}"),
    })
}

/// Apply one event; returns `true` at `message_stop`.
fn apply_event(
    value: &Value,
    state: &mut StreamState,
    partial: &mut PartialMessage,
    on_event: &mut (dyn FnMut(StreamEvent) + Send),
) -> Result<bool> {
    let u = |v: &Value, k: &str| v.get(k).and_then(Value::as_u64);
    let s = |v: &Value, k: &str| {
        v.get(k)
            .and_then(Value::as_str)
            .unwrap_or_default()
            .to_string()
    };
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
            let out = &mut partial.message.usage;
            out.input_tokens = u(usage, "input_tokens").unwrap_or(0);
            out.output_tokens = u(usage, "output_tokens").unwrap_or(0);
            out.cache_read_tokens = u(usage, "cache_read_input_tokens").unwrap_or(0);
            out.cache_write_tokens = u(usage, "cache_creation_input_tokens").unwrap_or(0);
            out.total_tokens = out.input_tokens
                + out.output_tokens
                + out.cache_read_tokens
                + out.cache_write_tokens;
        }
        "content_block_start" => {
            let api_index = u(value, "index").unwrap_or(0);
            let block = &value["content_block"];
            let content = match block
                .get("type")
                .and_then(Value::as_str)
                .unwrap_or_default()
            {
                "text" => Some(ContentBlock::text("")),
                "thinking" => Some(ContentBlock::Thinking {
                    thinking: String::new(),
                    signature: None,
                    redacted: false,
                }),
                "redacted_thinking" => Some(ContentBlock::Thinking {
                    thinking: "[Reasoning redacted]".into(),
                    signature: block
                        .get("data")
                        .and_then(Value::as_str)
                        .map(str::to_string),
                    redacted: true,
                }),
                "tool_use" => {
                    let name = s(block, "name");
                    Some(ContentBlock::ToolCall {
                        id: s(block, "id"),
                        name: if state.oauth {
                            from_claude_code_name(&name, &state.tools)
                        } else {
                            name
                        },
                        arguments: Value::Null,
                    })
                }
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
            match delta
                .get("type")
                .and_then(Value::as_str)
                .unwrap_or_default()
            {
                "text_delta" => partial.push_text(index, &s(delta, "text"), on_event),
                "thinking_delta" => partial.push_thinking(index, &s(delta, "thinking"), on_event),
                "signature_delta" => partial.set_signature(index, &s(delta, "signature")),
                "input_json_delta" => {
                    partial.push_tool_args(index, &s(delta, "partial_json"), on_event)
                }
                _ => {}
            }
        }
        "message_delta" => {
            if let Some(reason) = value["delta"].get("stop_reason").and_then(Value::as_str) {
                partial.set_stop(map_stop_reason(reason)?);
            }
            let usage = &value["usage"];
            let out = &mut partial.message.usage;
            if let Some(v) = u(usage, "input_tokens") {
                out.input_tokens = v;
            }
            if let Some(v) = u(usage, "output_tokens") {
                out.output_tokens = v;
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
    use crate::catalog::ModelCatalog;
    use crate::message::{AssistantMessage, ToolResultMessage};
    use crate::provider::test_server::{MockResponse, serve};
    use crate::provider::{ProviderKind, ToolSpec};

    fn provider(key: &str) -> ProviderConfig {
        ProviderConfig {
            id: "anthropic".into(),
            label: "Anthropic".into(),
            kind: ProviderKind::AnthropicMessages,
            base_url: "https://api.anthropic.com".into(),
            api_key: Some(key.into()),
            ..Default::default()
        }
    }

    fn model(id: &str) -> ModelSpec {
        ModelCatalog::builtin()
            .find_model("anthropic", id)
            .cloned()
            .unwrap_or_else(|| {
                let mut m = ModelSpec::fallback(
                    "anthropic",
                    "anthropic-messages",
                    "https://api.anthropic.com",
                    id,
                );
                m.max_tokens = 64_000;
                m
            })
    }

    fn tool(name: &str) -> ToolSpec {
        ToolSpec {
            name: name.into(),
            description: "d".into(),
            parameters: json!({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]}),
        }
    }

    #[test]
    fn applies_stream_events() {
        let provider = provider("k");
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
    fn unknown_stop_reason_is_an_error() {
        let provider = provider("k");
        let mut partial = PartialMessage::new(&provider, "claude");
        let mut state = StreamState::default();
        let err = apply_event(
            &json!({"type":"message_delta","delta":{"stop_reason":"weird"},"usage":{}}),
            &mut state,
            &mut partial,
            &mut |_| {},
        )
        .unwrap_err();
        assert_eq!(err.to_string(), "Unhandled stop reason: weird");
    }

    #[test]
    fn api_key_request_has_cache_and_adaptive_thinking() {
        let call = Message::Assistant(AssistantMessage {
            provider: "anthropic".into(),
            api: "anthropic-messages".into(),
            model: "claude-opus-4-7".into(),
            content: vec![
                ContentBlock::ToolCall {
                    id: "a".into(),
                    name: "read".into(),
                    arguments: json!({"path":"x"}),
                },
                ContentBlock::ToolCall {
                    id: "b".into(),
                    name: "read".into(),
                    arguments: json!({"path":"y"}),
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
            model: "claude-opus-4-7".into(),
            system: "sys".into(),
            messages: vec![Message::user_text("go"), call, r("a"), r("b")],
            tools: vec![tool("read")],
            thinking: ThinkingLevel::XHigh,
            session_id: Some("s1".into()),
            cache_retention: Some(CacheRetention::Long),
            ..Default::default()
        };
        let built =
            build_request(&provider("sk-ant-api"), &model("claude-opus-4-7"), &request).unwrap();
        assert_eq!(built.url, "https://api.anthropic.com/v1/messages");
        assert_eq!(built.headers["x-api-key"], "sk-ant-api");
        assert!(!built.headers.contains_key("anthropic-beta"));
        let body = &built.body;
        assert_eq!(
            body["thinking"],
            json!({"type":"adaptive","display":"summarized"})
        );
        assert_eq!(body["output_config"]["effort"], "xhigh");
        assert_eq!(body["max_tokens"], 32_000);
        assert_eq!(
            body["system"][0]["cache_control"],
            json!({"type":"ephemeral","ttl":"1h"})
        );
        assert_eq!(body["tools"][0]["eager_input_streaming"], true);
        assert_eq!(body["tools"][0]["input_schema"]["required"][0], "path");
        assert_eq!(body["tools"][0]["cache_control"]["ttl"], "1h");
        let msgs = body["messages"].as_array().unwrap();
        assert_eq!(msgs.len(), 3);
        assert_eq!(msgs[2]["content"].as_array().unwrap().len(), 2);
        assert_eq!(msgs[2]["content"][0]["content"], "ok");
        assert_eq!(msgs[2]["content"][1]["cache_control"]["type"], "ephemeral");
    }

    #[test]
    fn budget_thinking_for_older_models_and_interleaved_beta() {
        let request = Request {
            model: "claude-haiku-4-5".into(),
            system: String::new(),
            messages: vec![Message::user_text("hi")],
            thinking: ThinkingLevel::Medium,
            temperature: Some(0.5),
            ..Default::default()
        };
        let built = build_request(&provider("k"), &model("claude-haiku-4-5"), &request).unwrap();
        assert_eq!(built.headers["anthropic-beta"], INTERLEAVED_THINKING_BETA);
        assert_eq!(built.body["max_tokens"], 32_000 + 8_192);
        assert_eq!(built.body["thinking"]["budget_tokens"], 8_192);
        assert!(built.body.get("temperature").is_none());
        assert!(built.body.get("system").is_none());
        // A plain string user message carries cache_control after conversion.
        assert_eq!(
            built.body["messages"][0]["content"][0]["cache_control"]["type"],
            "ephemeral"
        );

        let off = Request {
            thinking: ThinkingLevel::Off,
            ..request
        };
        let built = build_request(&provider("k"), &model("claude-haiku-4-5"), &off).unwrap();
        assert_eq!(built.body["thinking"], json!({"type":"disabled"}));
        assert_eq!(built.body["temperature"], 0.5);
        assert_eq!(
            adjust_max_tokens_for_thinking(4_000, 6_000, ThinkingLevel::High),
            (6_000, 4_976)
        );
    }

    #[test]
    fn oauth_uses_claude_code_identity_and_tool_names() {
        let history = vec![
            Message::user_text("list"),
            Message::Assistant(AssistantMessage {
                provider: "anthropic".into(),
                api: "anthropic-messages".into(),
                model: "claude-sonnet-4-6".into(),
                content: vec![ContentBlock::ToolCall {
                    id: "t1".into(),
                    name: "read".into(),
                    arguments: json!({"path":"a"}),
                }],
                stop_reason: StopReason::ToolCall,
                ..Default::default()
            }),
            Message::ToolResult(ToolResultMessage {
                tool_call_id: "t1".into(),
                tool_name: "read".into(),
                content: vec![ContentBlock::Image {
                    data: "AA".into(),
                    mime_type: "image/png".into(),
                }],
                ..Default::default()
            }),
        ];
        let request = Request {
            model: "claude-sonnet-4-6".into(),
            system: "be brief".into(),
            messages: history,
            tools: vec![tool("read"), tool("my_tool")],
            cache_retention: Some(CacheRetention::None),
            ..Default::default()
        };
        let built = build_request(
            &provider("sk-ant-oat01-abc"),
            &model("claude-sonnet-4-6"),
            &request,
        )
        .unwrap();
        assert!(built.oauth);
        assert_eq!(built.headers["authorization"], "Bearer sk-ant-oat01-abc");
        assert_eq!(
            built.headers["anthropic-beta"],
            "claude-code-20250219,oauth-2025-04-20"
        );
        assert_eq!(built.headers["user-agent"], "claude-cli/2.1.75");
        assert_eq!(built.headers["x-app"], "cli");
        assert!(!built.headers.contains_key("x-api-key"));
        assert_eq!(built.body["system"][0]["text"], CLAUDE_CODE_IDENTITY);
        assert_eq!(built.body["system"][1]["text"], "be brief");
        assert!(built.body["system"][0].get("cache_control").is_none());
        assert_eq!(built.body["tools"][0]["name"], "Read");
        assert_eq!(built.body["tools"][1]["name"], "my_tool");
        assert_eq!(built.body["messages"][1]["content"][0]["name"], "Read");
        let result = &built.body["messages"][2]["content"][0]["content"];
        assert_eq!(result[0]["text"], "(see attached image)");
        assert_eq!(result[1]["type"], "image");
        assert_eq!(from_claude_code_name("Read", &request.tools), "read");
    }

    #[test]
    fn fireworks_uses_fine_grained_beta_and_affinity() {
        let catalog = ModelCatalog::builtin();
        let m = catalog
            .find_model("fireworks", "accounts/fireworks/models/kimi-k2p6")
            .unwrap();
        let p = ProviderConfig {
            id: "fireworks".into(),
            api_key: Some("fw".into()),
            ..Default::default()
        };
        let request = Request {
            model: m.id.clone(),
            messages: vec![Message::user_text("x")],
            tools: vec![tool("read")],
            session_id: Some("sess".into()),
            ..Default::default()
        };
        let built = build_request(&p, m, &request).unwrap();
        assert_eq!(
            built.headers["anthropic-beta"],
            format!("{FINE_GRAINED_TOOL_STREAMING_BETA},{INTERLEAVED_THINKING_BETA}")
        );
        assert_eq!(built.headers["x-session-affinity"], "sess");
        assert!(
            built.body["tools"][0]
                .get("eager_input_streaming")
                .is_none()
        );
        assert!(built.body["tools"][0].get("cache_control").is_none());
        assert_eq!(built.url, "https://api.fireworks.ai/inference/v1/messages");
    }

    #[tokio::test]
    async fn streams_from_server_with_retry() {
        let sse = [
            r#"{"type":"message_start","message":{"id":"m1","usage":{"input_tokens":1000000,"output_tokens":0}}}"#,
            r#"{"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}}"#,
            r#"{"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"done"}}"#,
            r#"{"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":1000000}}"#,
            r#"{"type":"message_stop"}"#,
        ]
        .iter()
        .map(|e| format!("event: x\ndata: {e}\n\n"))
        .collect::<String>();
        let (base, requests) = serve(vec![
            MockResponse::json(529, r#"{"error":{"message":"overloaded"}}"#)
                .with_header("retry-after", "0"),
            MockResponse::sse(sse),
        ])
        .await;
        let mut p = provider("k");
        p.base_url = base.clone();
        p.models = std::sync::Arc::new(vec![ModelSpec {
            base_url: base,
            ..model("claude-haiku-4-5")
        }]);
        let request = Request {
            model: "claude-haiku-4-5".into(),
            messages: vec![Message::user_text("hi")],
            ..Default::default()
        };
        let msg = crate::provider::stream(
            &reqwest::Client::new(),
            &p,
            &request,
            &CancellationToken::new(),
            &mut |_| {},
        )
        .await;
        assert_eq!(msg.error_message, None);
        assert_eq!(msg.text(), "done");
        assert_eq!(msg.api, "anthropic-messages");
        assert!((msg.usage.total_cost_usd - 6.0).abs() < 1e-9);
        let requests = requests.lock().unwrap();
        assert_eq!(requests.len(), 2);
        assert_eq!(requests[1].path, "/v1/messages");
        assert_eq!(requests[1].headers["x-api-key"], "k");
    }
}
