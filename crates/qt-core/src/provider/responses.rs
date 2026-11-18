//! OpenAI Responses API family (Pi `openai-responses`,
//! `openai-codex-responses`, `azure-openai-responses`).
//!
//! Reasoning items are replayed from the thinking signature (the serialized
//! reasoning item, including `encrypted_content`), tool-call ids carry
//! `call_id|item_id`, and ids from other providers are normalised.

use std::collections::{BTreeMap, HashSet};
use std::sync::OnceLock;

use anyhow::{Result, anyhow, bail};
use futures::StreamExt as _;
use serde_json::{Value, json};
use tokio_util::sync::CancellationToken;

use super::compat;
use super::http::{self, Sent};
use super::transform::{Target, short_hash, transform_messages};
use super::{
    CacheRetention, PartialMessage, ProviderConfig, ProviderKind, Request, StreamEvent,
    ThinkingLevel, effective_max_tokens, sanitize_text, with_headers,
};
use crate::catalog::{ModelSpec, resolve_codex_service_tier, service_tier_multiplier};
use crate::message::{AssistantMessage, ContentBlock, Message, StopReason, UserContent};
use crate::sse::SseParser;

pub const DEFAULT_CODEX_BASE_URL: &str = "https://chatgpt.com/backend-api";
pub const DEFAULT_AZURE_API_VERSION: &str = "v1";
pub const DEFAULT_CODEX_INSTRUCTIONS: &str = "You are a helpful assistant.";
const OPENAI_TOOL_CALL_PROVIDERS: &[&str] = &["openai", "openai-codex", "opencode"];
const AZURE_TOOL_CALL_PROVIDERS: &[&str] = &[
    "openai",
    "openai-codex",
    "opencode",
    "azure-openai-responses",
];

/// `pi/<version> (<platform>; native/rust; <arch>)`, the archived qt format.
pub fn pi_user_agent() -> String {
    let platform = match std::env::consts::OS {
        "macos" => "darwin",
        "windows" => "win32",
        other => other,
    };
    let arch = match std::env::consts::ARCH {
        "x86_64" => "x64",
        "aarch64" => "arm64",
        "x86" => "ia32",
        _ => "unknown",
    };
    format!(
        "pi/{} ({platform}; native/rust; {arch})",
        env!("CARGO_PKG_VERSION")
    )
}

/// Sanitise one id part: `[a-zA-Z0-9_-]`, at most 64 chars, no trailing `_`.
fn normalize_id_part(part: &str) -> String {
    let sanitized: String = super::transform::sanitize_id(part)
        .chars()
        .take(64)
        .collect();
    sanitized.trim_end_matches('_').to_string()
}

fn foreign_item_id(item_id: &str) -> String {
    format!("fc_{}", short_hash(item_id))
        .chars()
        .take(64)
        .collect()
}

/// Pi Responses tool-call id normalisation for history from other models.
pub fn normalize_tool_call_id(
    id: &str,
    target_provider: &str,
    target_api: &str,
    source: &AssistantMessage,
    allowed: &[&str],
) -> String {
    if !allowed.contains(&target_provider) {
        return normalize_id_part(id);
    }
    let Some((call_id, item_id)) = id.split_once('|') else {
        return normalize_id_part(id);
    };
    let item_id = item_id.split('|').next().unwrap_or_default();
    let foreign = source.provider != target_provider || source.api != target_api;
    let mut item = if foreign {
        foreign_item_id(item_id)
    } else {
        normalize_id_part(item_id)
    };
    if !item.starts_with("fc_") {
        item = normalize_id_part(&format!("fc_{item}"));
    }
    format!("{}|{item}", normalize_id_part(call_id))
}

fn input_image(data: &str, mime_type: &str) -> Value {
    json!({ "type": "input_image", "detail": "auto", "image_url": format!("data:{mime_type};base64,{data}") })
}

/// Pi `convertResponsesMessages`.
pub fn convert_messages(
    provider: &ProviderConfig,
    model: &ModelSpec,
    kind: ProviderKind,
    request: &Request,
    include_system: bool,
) -> Vec<Value> {
    let api = kind.api_name();
    let allowed = if kind == ProviderKind::AzureOpenAiResponses {
        AZURE_TOOL_CALL_PROVIDERS
    } else {
        OPENAI_TOOL_CALL_PROVIDERS
    };
    let target = Target {
        provider: &provider.id,
        api,
        model: &model.id,
        images: model.supports_images(),
    };
    let provider_id = provider.id.clone();
    let normalize = move |id: &str, source: &AssistantMessage| {
        normalize_tool_call_id(id, &provider_id, api, source, allowed)
    };
    let transformed = transform_messages(&request.messages, &target, Some(&normalize));

    let mut out = Vec::new();
    if include_system && !request.system.is_empty() {
        let role = if model.reasoning {
            "developer"
        } else {
            "system"
        };
        out.push(json!({ "role": role, "content": sanitize_text(&request.system) }));
    }
    for (index, message) in transformed.iter().enumerate() {
        match message {
            Message::User { content, .. } => {
                let parts: Vec<Value> = match content {
                    UserContent::Text(text) => {
                        vec![json!({ "type": "input_text", "text": sanitize_text(text) })]
                    }
                    UserContent::Blocks(blocks) => blocks
                        .iter()
                        .filter_map(|b| match b {
                            ContentBlock::Text { text } => {
                                Some(json!({ "type": "input_text", "text": sanitize_text(text) }))
                            }
                            ContentBlock::Image { data, mime_type } => {
                                Some(input_image(data, mime_type))
                            }
                            _ => None,
                        })
                        .collect(),
                };
                if !parts.is_empty() {
                    out.push(json!({ "role": "user", "content": parts }));
                }
            }
            Message::Assistant(a) => {
                let different_model =
                    a.model != model.id && a.provider == provider.id && a.api == api;
                for block in &a.content {
                    match block {
                        ContentBlock::Thinking { signature: Some(sig), .. } => {
                            if let Ok(item) = serde_json::from_str::<Value>(sig)
                                && item.is_object()
                            {
                                out.push(item);
                            }
                        }
                        ContentBlock::Text { text } => out.push(json!({
                            "type": "message",
                            "role": "assistant",
                            "content": [{ "type": "output_text", "text": sanitize_text(text), "annotations": [] }],
                            "status": "completed",
                            "id": format!("msg_{index}"),
                        })),
                        ContentBlock::ToolCall {
                            id,
                            name,
                            arguments,
                        } => {
                            let (call_id, item_id) = match id.split_once('|') {
                                Some((c, i)) => (c, Some(i)),
                                None => (id.as_str(), None),
                            };
                            let mut call = json!({
                                "type": "function_call",
                                "call_id": call_id,
                                "name": name,
                                "arguments": match arguments {
                                    Value::String(raw) => raw.clone(),
                                    Value::Null => "{}".to_string(),
                                    other => other.to_string(),
                                },
                            });
                            if let Some(item) = item_id
                                && !(different_model && item.starts_with("fc_"))
                            {
                                call["id"] = json!(item);
                            }
                            out.push(call);
                        }
                        _ => {}
                    }
                }
            }
            Message::ToolResult(r) => {
                let text = r
                    .content
                    .iter()
                    .filter_map(|b| match b {
                        ContentBlock::Text { text } => Some(text.as_str()),
                        _ => None,
                    })
                    .collect::<Vec<_>>()
                    .join("\n");
                let has_images = r
                    .content
                    .iter()
                    .any(|b| matches!(b, ContentBlock::Image { .. }));
                let call_id = r.tool_call_id.split('|').next().unwrap_or_default();
                let output = if has_images && model.supports_images() {
                    let mut parts = Vec::new();
                    if !text.is_empty() {
                        parts.push(json!({ "type": "input_text", "text": sanitize_text(&text) }));
                    }
                    for block in &r.content {
                        if let ContentBlock::Image { data, mime_type } = block {
                            parts.push(input_image(data, mime_type));
                        }
                    }
                    Value::Array(parts)
                } else {
                    json!(sanitize_text(if text.is_empty() {
                        "(see attached image)"
                    } else {
                        &text
                    }))
                };
                out.push(
                    json!({ "type": "function_call_output", "call_id": call_id, "output": output }),
                );
            }
        }
    }
    out
}

fn convert_tools(request: &Request, strict: Value) -> Vec<Value> {
    request
        .tools
        .iter()
        .map(|t| {
            json!({
                "type": "function",
                "name": t.name,
                "description": t.description,
                "parameters": t.parameters,
                "strict": strict,
            })
        })
        .collect()
}

/// Reasoning options shared by OpenAI and Azure (Pi `buildParams`).
fn apply_reasoning(body: &mut Value, model: &ModelSpec, request: &Request, skip_off: bool) {
    if !model.reasoning {
        return;
    }
    let level = super::request_level(model, request.thinking);
    let effort = (level != ThinkingLevel::Off).then(|| model.effort_for(level));
    if effort.is_some() || request.reasoning_summary.is_some() {
        body["reasoning"] = json!({
            "effort": effort.unwrap_or_else(|| "medium".into()),
            "summary": request.reasoning_summary.clone().unwrap_or_else(|| "auto".into()),
        });
        body["include"] = json!(["reasoning.encrypted_content"]);
    } else if !skip_off {
        match model.mapped_level(ThinkingLevel::Off) {
            Some(None) => {}
            Some(Some(off)) => body["reasoning"] = json!({ "effort": off }),
            None => body["reasoning"] = json!({ "effort": "none" }),
        }
    }
}

/// A fully built request.
#[derive(Debug, Clone, PartialEq)]
pub struct Built {
    pub url: String,
    pub headers: BTreeMap<String, String>,
    pub body: Value,
}

/// `chatgpt-account-id` from an OpenAI access token.
fn codex_account_id(provider: &ProviderConfig, token: &str) -> Result<String> {
    crate::auth::jwt_account_id(token)
        .or_else(|| {
            provider
                .oauth
                .as_ref()
                .and_then(|b| b.credentials.account_id.clone())
        })
        .ok_or_else(|| anyhow!("Failed to extract accountId from token"))
}

/// Codex endpoint for a base URL (Pi `resolveCodexUrl`).
pub fn resolve_codex_url(base_url: &str) -> String {
    let raw = if base_url.trim().is_empty() {
        DEFAULT_CODEX_BASE_URL
    } else {
        base_url.trim()
    };
    let normalized = raw.trim_end_matches('/');
    if normalized.ends_with("/codex/responses") {
        normalized.to_string()
    } else if normalized.ends_with("/codex") {
        format!("{normalized}/responses")
    } else {
        format!("{normalized}/codex/responses")
    }
}

/// Normalise an Azure base URL so Azure hosts end in `/openai/v1`.
pub fn normalize_azure_base_url(base_url: &str) -> Result<String> {
    let trimmed = base_url.trim().trim_end_matches('/');
    let mut url = reqwest::Url::parse(trimmed)
        .map_err(|_| anyhow!("Invalid Azure OpenAI base URL: {base_url}"))?;
    let host = url.host_str().unwrap_or_default().to_string();
    let azure_host =
        host.ends_with(".openai.azure.com") || host.ends_with(".cognitiveservices.azure.com");
    let path = url.path().trim_end_matches('/').to_string();
    if azure_host && (path.is_empty() || path == "/openai") {
        url.set_path("/openai/v1");
        url.set_query(None);
    }
    Ok(url.to_string().trim_end_matches('/').to_string())
}

fn env(name: &str) -> Option<String> {
    std::env::var(name)
        .ok()
        .map(|v| v.trim().to_string())
        .filter(|v| !v.is_empty())
}

/// Azure base URL and API version (Pi `resolveAzureConfig`).
fn azure_config(model: &ModelSpec) -> Result<(String, String)> {
    let version =
        env("AZURE_OPENAI_API_VERSION").unwrap_or_else(|| DEFAULT_AZURE_API_VERSION.into());
    let base = env("AZURE_OPENAI_BASE_URL")
        .or_else(|| {
            env("AZURE_OPENAI_RESOURCE_NAME")
                .map(|name| format!("https://{name}.openai.azure.com/openai/v1"))
        })
        .or_else(|| (!model.base_url.trim().is_empty()).then(|| model.base_url.clone()))
        .ok_or_else(|| {
            anyhow!(
                "Azure OpenAI base URL is required. Set AZURE_OPENAI_BASE_URL or AZURE_OPENAI_RESOURCE_NAME, or pass azureBaseUrl, azureResourceName, or model.baseUrl."
            )
        })?;
    Ok((normalize_azure_base_url(&base)?, version))
}

/// Deployment for a model from `AZURE_OPENAI_DEPLOYMENT_NAME_MAP` (`model=deployment,...`).
pub fn azure_deployment_name(model_id: &str, map: Option<&str>) -> String {
    map.into_iter()
        .flat_map(|m| m.split(','))
        .filter_map(|entry| entry.trim().split_once('='))
        .find(|(m, d)| m.trim() == model_id && !d.trim().is_empty())
        .map(|(_, d)| d.trim().to_string())
        .unwrap_or_else(|| model_id.to_string())
}

/// Build the URL, headers and body for one request.
pub fn build_request(
    provider: &ProviderConfig,
    model: &ModelSpec,
    kind: ProviderKind,
    request: &Request,
) -> Result<Built> {
    let key = provider.api_key.clone().unwrap_or_default();
    let retention = CacheRetention::resolve(request.cache_retention);
    let session = request
        .session_id
        .clone()
        .filter(|_| retention != CacheRetention::None);
    let mut headers = BTreeMap::new();
    headers.insert("content-type".to_string(), "application/json".to_string());
    headers.insert("accept".to_string(), "text/event-stream".to_string());
    let (url, body) = match kind {
        ProviderKind::OpenAiCodexResponses => {
            let account = codex_account_id(provider, &key)?;
            headers.insert("authorization".into(), format!("Bearer {key}"));
            headers.insert("chatgpt-account-id".into(), account);
            headers.insert("originator".into(), "pi".into());
            headers.insert("user-agent".into(), pi_user_agent());
            headers.insert("OpenAI-Beta".into(), "responses=experimental".into());
            if let Some(s) = &request.session_id {
                headers.insert("session_id".into(), s.clone());
                headers.insert("x-client-request-id".into(), s.clone());
            }
            let mut input = convert_messages(provider, model, kind, request, false);
            normalize_codex_input_ids(&mut input);
            let mut body = json!({
                "model": model.id,
                "store": false,
                "stream": true,
                "instructions": if request.system.is_empty() { DEFAULT_CODEX_INSTRUCTIONS.to_string() } else { sanitize_text(&request.system) },
                "input": input,
                "text": { "verbosity": request.text_verbosity.clone().unwrap_or_else(|| "low".into()) },
                "include": ["reasoning.encrypted_content"],
                "tool_choice": "auto",
                "parallel_tool_calls": true,
            });
            if let Some(s) = &request.session_id {
                body["prompt_cache_key"] = json!(s);
            }
            if let Some(t) = request.temperature {
                body["temperature"] = json!(t);
            }
            if let Some(tier) = &request.service_tier {
                body["service_tier"] = json!(tier);
            }
            if !request.tools.is_empty() {
                body["tools"] = Value::Array(convert_tools(request, Value::Null));
            }
            let level = super::request_level(model, request.thinking);
            if level != ThinkingLevel::Off
                && let Some(effort) = match model.mapped_level(level) {
                    Some(None) => None,
                    Some(Some(v)) => Some(v.to_string()),
                    None => Some(level.as_str().to_string()),
                }
            {
                body["reasoning"] = json!({
                    "effort": effort,
                    "summary": request.reasoning_summary.clone().unwrap_or_else(|| "auto".into()),
                });
            }
            (resolve_codex_url(&model.base_url), body)
        }
        ProviderKind::AzureOpenAiResponses => {
            let (base, version) = azure_config(model)?;
            if !key.is_empty() {
                headers.insert("api-key".into(), key.clone());
            }
            let deployment = azure_deployment_name(
                &model.id,
                std::env::var("AZURE_OPENAI_DEPLOYMENT_NAME_MAP")
                    .ok()
                    .as_deref(),
            );
            let mut body = json!({
                "model": deployment,
                "input": convert_messages(provider, model, kind, request, true),
                "stream": true,
            });
            if let Some(s) = &request.session_id {
                body["prompt_cache_key"] = json!(s);
            }
            body["max_output_tokens"] = json!(effective_max_tokens(request, model));
            if let Some(t) = request.temperature {
                body["temperature"] = json!(t);
            }
            if !request.tools.is_empty() {
                body["tools"] = Value::Array(convert_tools(request, json!(false)));
            }
            apply_reasoning(&mut body, model, request, false);
            (format!("{base}/responses?api-version={version}"), body)
        }
        _ => {
            let compat = compat::responses(provider, model);
            if model.provider == "cloudflare-ai-gateway" {
                headers.insert("cf-aig-authorization".into(), format!("Bearer {key}"));
            } else if !key.is_empty() {
                headers.insert("authorization".into(), format!("Bearer {key}"));
            }
            if let Some(s) = &session {
                if compat.send_session_id_header {
                    headers.insert("session_id".into(), s.clone());
                }
                headers.insert("x-client-request-id".into(), s.clone());
            }
            let mut body = json!({
                "model": model.id,
                "input": convert_messages(provider, model, kind, request, true),
                "stream": true,
                "store": false,
            });
            if let Some(s) = &session {
                body["prompt_cache_key"] = json!(s);
            }
            if retention == CacheRetention::Long && compat.supports_long_cache_retention {
                body["prompt_cache_retention"] = json!("24h");
            }
            body["max_output_tokens"] = json!(effective_max_tokens(request, model));
            if let Some(t) = request.temperature {
                body["temperature"] = json!(t);
            }
            if let Some(tier) = &request.service_tier {
                body["service_tier"] = json!(tier);
            }
            if !request.tools.is_empty() {
                body["tools"] = Value::Array(convert_tools(request, json!(false)));
            }
            apply_reasoning(
                &mut body,
                model,
                request,
                model.provider == "github-copilot",
            );
            let base = super::resolve_url_placeholders(
                model.base_url.trim_end_matches('/'),
                &model.provider,
            )?;
            (format!("{base}/responses"), body)
        }
    };
    if (provider.auth_header || model.auth_header) && !key.is_empty() {
        headers.insert("authorization".into(), format!("Bearer {key}"));
    }
    headers.extend(provider.headers.clone());
    headers.extend(model.headers.clone());
    headers.extend(request.headers.clone());
    Ok(Built { url, headers, body })
}

/// Codex rejects message/function-call ids without their usual prefixes.
fn normalize_codex_input_ids(input: &mut [Value]) {
    for item in input {
        let Some(id) = item.get("id").and_then(Value::as_str).map(str::to_string) else {
            continue;
        };
        match item.get("type").and_then(Value::as_str) {
            Some("message") if !id.starts_with("msg") => {
                item["id"] = json!(format!("msg_{}", short_hash(&id)))
            }
            Some("function_call") if !id.starts_with("fc") => {
                item["id"] = json!(format!("fc_{}", short_hash(&id)))
            }
            _ => {}
        }
    }
}

fn codex_retryable(status: u16, body: &str) -> bool {
    static RE: OnceLock<regex::Regex> = OnceLock::new();
    let re = RE.get_or_init(|| {
        regex::Regex::new(r"(?i)rate.?limit|overloaded|service.?unavailable|upstream.?connect|connection.?refused")
            .expect("valid regex")
    });
    matches!(status, 429 | 500 | 502 | 503 | 504) || re.is_match(body)
}

/// Pi `parseErrorResponse` for the Codex endpoint.
pub fn codex_error_message(status: u16, body: &str) -> String {
    let mut message = if body.is_empty() {
        "Request failed".to_string()
    } else {
        body.to_string()
    };
    if let Ok(parsed) = serde_json::from_str::<Value>(body)
        && let Some(err) = parsed.get("error").filter(|e| e.is_object())
    {
        let code = err
            .get("code")
            .or_else(|| err.get("type"))
            .and_then(Value::as_str)
            .unwrap_or_default()
            .to_ascii_lowercase();
        let mut friendly = None;
        if code.contains("usage_limit_reached")
            || code.contains("usage_not_included")
            || code.contains("rate_limit_exceeded")
            || status == 429
        {
            let plan = err
                .get("plan_type")
                .and_then(Value::as_str)
                .map(|p| format!(" ({} plan)", p.to_lowercase()))
                .unwrap_or_default();
            let when = err
                .get("resets_at")
                .and_then(Value::as_f64)
                .map(|resets| {
                    let mins = ((resets * 1000.0 - crate::message::now_millis() as f64) / 60_000.0)
                        .round()
                        .max(0.0);
                    format!(" Try again in ~{mins} min.")
                })
                .unwrap_or_default();
            friendly = Some(
                format!("You have hit your ChatGPT usage limit{plan}.{when}")
                    .trim()
                    .to_string(),
            );
        }
        if let Some(f) = friendly {
            return f;
        }
        if let Some(m) = err.get("message").and_then(Value::as_str) {
            message = m.to_string();
        }
    }
    message
}

#[allow(clippy::too_many_arguments)]
pub(crate) async fn stream(
    http_client: &reqwest::Client,
    provider: &ProviderConfig,
    model: &ModelSpec,
    kind: ProviderKind,
    request: &Request,
    cancel: &CancellationToken,
    partial: &mut PartialMessage,
    on_event: &mut (dyn FnMut(StreamEvent) + Send),
) -> Result<()> {
    if provider.api_key.is_none() && !provider.local {
        if kind == ProviderKind::OpenAiCodexResponses {
            bail!("No API key for provider: {}", provider.id);
        }
        bail!("No API key configured for {}", provider.label);
    }
    let built = build_request(provider, model, kind, request)?;
    let body = serde_json::to_vec(&built.body)?;
    let make = || with_headers(http_client.post(&built.url), &built.headers).body(body.clone());
    let codex = kind == ProviderKind::OpenAiCodexResponses;
    let retry_check: Option<&http::RetryCheck> = if codex { Some(&codex_retryable) } else { None };
    let response = match http::send(&make, &provider.retry, cancel, retry_check).await? {
        Sent::Ok(r) => r,
        Sent::Failed(f) if codex => bail!(codex_error_message(f.status, &f.body)),
        Sent::Failed(f) => bail!(f.message()),
        Sent::Cancelled => return Ok(()),
    };
    let mut bytes = response.bytes_stream();
    let mut parser = SseParser::new();
    let mut state = StreamState {
        codex,
        model_id: model.id.clone(),
        request_tier: request.service_tier.clone(),
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
            let data = event.data.trim();
            if data.is_empty() || data == "[DONE]" {
                continue;
            }
            let value: Value = match serde_json::from_str(data) {
                Ok(v) => v,
                Err(e) if codex => bail!("Invalid Codex SSE JSON: {e}"),
                Err(_) => continue,
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

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum BlockKind {
    Thinking,
    Text,
    Tool,
}

#[derive(Default)]
struct StreamState {
    codex: bool,
    model_id: String,
    request_tier: Option<String>,
    /// The output item currently streaming (mutated as parts arrive).
    item: Option<Value>,
    block: Option<(BlockKind, usize)>,
    tool_indices: HashSet<usize>,
}

fn set_thinking(partial: &mut PartialMessage, index: usize, text: String, signature: String) {
    if let Some(ContentBlock::Thinking {
        thinking,
        signature: sig,
        ..
    }) = partial.message.content.get_mut(index)
    {
        *thinking = text;
        *sig = Some(signature);
    }
}

fn set_text(partial: &mut PartialMessage, index: usize, value: String) {
    if let Some(ContentBlock::Text { text }) = partial.message.content.get_mut(index) {
        *text = value;
    }
}

fn last_part_mut<'a>(item: &'a mut Value, key: &str) -> Option<&'a mut Value> {
    item.get_mut(key)?.as_array_mut()?.last_mut()
}

fn append_part_text(part: &mut Value, key: &str, delta: &str) {
    let current = part.get(key).and_then(Value::as_str).unwrap_or_default();
    part[key] = json!(format!("{current}{delta}"));
}

fn map_status(status: Option<&str>) -> StopReason {
    match status {
        Some("incomplete") => StopReason::Length,
        Some("failed" | "cancelled") => StopReason::Error,
        _ => StopReason::Stop,
    }
}

/// Apply one event; returns `true` once the response completed.
fn apply_event(
    event: &Value,
    state: &mut StreamState,
    partial: &mut PartialMessage,
    on_event: &mut (dyn FnMut(StreamEvent) + Send),
) -> Result<bool> {
    let kind = event
        .get("type")
        .and_then(Value::as_str)
        .unwrap_or_default();
    let delta = event
        .get("delta")
        .and_then(Value::as_str)
        .unwrap_or_default();
    match kind {
        "response.created" => {
            if let Some(id) = event["response"].get("id").and_then(Value::as_str) {
                partial.message.response_id = Some(id.to_string());
            }
        }
        "response.output_item.added" => {
            let item = event["item"].clone();
            match item.get("type").and_then(Value::as_str) {
                Some("reasoning") => {
                    let i = partial.open(
                        ContentBlock::Thinking {
                            thinking: String::new(),
                            signature: None,
                            redacted: false,
                        },
                        on_event,
                    );
                    state.block = Some((BlockKind::Thinking, i));
                    state.item = Some(item);
                }
                Some("message") => {
                    let i = partial.open(ContentBlock::text(""), on_event);
                    state.block = Some((BlockKind::Text, i));
                    state.item = Some(item);
                }
                Some("function_call") => {
                    let s = |k: &str| {
                        item.get(k)
                            .and_then(Value::as_str)
                            .unwrap_or_default()
                            .to_string()
                    };
                    let i = partial.open(
                        ContentBlock::ToolCall {
                            id: format!("{}|{}", s("call_id"), s("id")),
                            name: s("name"),
                            arguments: Value::Null,
                        },
                        on_event,
                    );
                    let initial = s("arguments");
                    if !initial.is_empty() {
                        partial.set_tool_args(i, &initial);
                    }
                    state.tool_indices.insert(i);
                    state.block = Some((BlockKind::Tool, i));
                    state.item = Some(item);
                }
                _ => {}
            }
        }
        "response.reasoning_summary_part.added" => {
            if let Some(item) = state.item.as_mut().filter(|i| i["type"] == "reasoning") {
                if !item.get("summary").is_some_and(Value::is_array) {
                    item["summary"] = json!([]);
                }
                item["summary"]
                    .as_array_mut()
                    .expect("array")
                    .push(event["part"].clone());
            }
        }
        "response.reasoning_summary_text.delta" | "response.reasoning_summary_part.done" => {
            let text = if kind.ends_with(".done") {
                "\n\n"
            } else {
                delta
            };
            if let (Some(item), Some((BlockKind::Thinking, i))) = (state.item.as_mut(), state.block)
                && item["type"] == "reasoning"
                && let Some(part) = last_part_mut(item, "summary")
            {
                append_part_text(part, "text", text);
                partial.push_thinking(i, text, on_event);
            }
        }
        "response.reasoning_text.delta" => {
            if let (Some(item), Some((BlockKind::Thinking, i))) = (&state.item, state.block)
                && item["type"] == "reasoning"
            {
                partial.push_thinking(i, delta, on_event);
            }
        }
        "response.content_part.added" => {
            if let Some(item) = state.item.as_mut().filter(|i| i["type"] == "message") {
                let part = &event["part"];
                if matches!(part["type"].as_str(), Some("output_text" | "refusal")) {
                    if !item.get("content").is_some_and(Value::is_array) {
                        item["content"] = json!([]);
                    }
                    item["content"]
                        .as_array_mut()
                        .expect("array")
                        .push(part.clone());
                }
            }
        }
        "response.output_text.delta" | "response.refusal.delta" => {
            let (part_type, key) = if kind == "response.output_text.delta" {
                ("output_text", "text")
            } else {
                ("refusal", "refusal")
            };
            if let (Some(item), Some((BlockKind::Text, i))) = (state.item.as_mut(), state.block)
                && item["type"] == "message"
                && let Some(part) = last_part_mut(item, "content")
                && part["type"] == part_type
            {
                append_part_text(part, key, delta);
                partial.push_text(i, delta, on_event);
            }
        }
        "response.function_call_arguments.delta" => {
            if let Some((BlockKind::Tool, i)) = state.block {
                partial.push_tool_args(i, delta, on_event);
            }
        }
        "response.function_call_arguments.done" => {
            if let Some((BlockKind::Tool, i)) = state.block {
                let full = event
                    .get("arguments")
                    .and_then(Value::as_str)
                    .unwrap_or_default();
                let previous = partial.tool_args(i).unwrap_or_default().to_string();
                if let Some(rest) = full.strip_prefix(previous.as_str()) {
                    partial.push_tool_args(i, rest, on_event);
                } else {
                    partial.set_tool_args(i, full);
                }
            }
        }
        "response.output_item.done" => {
            let item = &event["item"];
            match (item.get("type").and_then(Value::as_str), state.block) {
                (Some("reasoning"), Some((BlockKind::Thinking, i))) => {
                    let join = |key: &str| {
                        item.get(key)
                            .and_then(Value::as_array)
                            .map(|parts| {
                                parts
                                    .iter()
                                    .filter_map(|p| p.get("text").and_then(Value::as_str))
                                    .collect::<Vec<_>>()
                                    .join("\n\n")
                            })
                            .unwrap_or_default()
                    };
                    let current = match partial.message.content.get(i) {
                        Some(ContentBlock::Thinking { thinking, .. }) => thinking.clone(),
                        _ => String::new(),
                    };
                    let summary = join("summary");
                    let content = join("content");
                    let text = if !summary.is_empty() {
                        summary
                    } else if !content.is_empty() {
                        content
                    } else {
                        current
                    };
                    set_thinking(partial, i, text, item.to_string());
                    state.block = None;
                    state.item = None;
                }
                (Some("message"), Some((BlockKind::Text, i))) => {
                    let text: String = item
                        .get("content")
                        .and_then(Value::as_array)
                        .map(|parts| {
                            parts
                                .iter()
                                .filter_map(|p| match p["type"].as_str() {
                                    Some("output_text") => p.get("text").and_then(Value::as_str),
                                    _ => p.get("refusal").and_then(Value::as_str),
                                })
                                .collect()
                        })
                        .unwrap_or_default();
                    set_text(partial, i, text);
                    state.block = None;
                    state.item = None;
                }
                (Some("function_call"), block) => {
                    let s = |k: &str| {
                        item.get(k)
                            .and_then(Value::as_str)
                            .unwrap_or_default()
                            .to_string()
                    };
                    match block {
                        Some((BlockKind::Tool, i)) => {
                            if partial.tool_args(i).is_none_or(str::is_empty) {
                                let args = s("arguments");
                                partial
                                    .set_tool_args(i, if args.is_empty() { "{}" } else { &args });
                            }
                        }
                        _ => {
                            let i = partial.open(
                                ContentBlock::ToolCall {
                                    id: format!("{}|{}", s("call_id"), s("id")),
                                    name: s("name"),
                                    arguments: Value::Null,
                                },
                                on_event,
                            );
                            let args = s("arguments");
                            partial.set_tool_args(i, if args.is_empty() { "{}" } else { &args });
                            state.tool_indices.insert(i);
                        }
                    }
                    state.block = None;
                    state.item = None;
                }
                _ => {}
            }
        }
        "response.completed" | "response.done" | "response.incomplete" => {
            if !state.codex && kind != "response.completed" {
                return Ok(false);
            }
            let response = &event["response"];
            if let Some(id) = response.get("id").and_then(Value::as_str) {
                partial.message.response_id = Some(id.to_string());
            }
            if let Some(usage) = response.get("usage").filter(|u| u.is_object()) {
                let get = |v: &Value, k: &str| v.get(k).and_then(Value::as_u64).unwrap_or(0);
                let cached = usage
                    .get("input_tokens_details")
                    .map(|d| get(d, "cached_tokens"))
                    .unwrap_or(0);
                let out = &mut partial.message.usage;
                out.input_tokens = get(usage, "input_tokens").saturating_sub(cached);
                out.output_tokens = get(usage, "output_tokens");
                out.cache_read_tokens = cached;
                out.cache_write_tokens = 0;
                out.total_tokens = get(usage, "total_tokens");
            }
            let response_tier = response.get("service_tier").and_then(Value::as_str);
            let tier = if state.codex {
                resolve_codex_service_tier(response_tier, state.request_tier.as_deref())
            } else {
                response_tier.or(state.request_tier.as_deref())
            };
            partial.cost_multiplier = service_tier_multiplier(&state.model_id, tier);
            let status = response.get("status").and_then(Value::as_str);
            let mut stop = map_status(status);
            if stop == StopReason::Stop && !state.tool_indices.is_empty() {
                stop = StopReason::ToolCall;
            }
            partial.set_stop(stop);
            return Ok(true);
        }
        "error" => {
            let code = event
                .get("code")
                .and_then(Value::as_str)
                .unwrap_or_default();
            let message = event
                .get("message")
                .and_then(Value::as_str)
                .unwrap_or_default();
            if state.codex {
                let detail = if !message.is_empty() {
                    message.to_string()
                } else if !code.is_empty() {
                    code.to_string()
                } else {
                    event.to_string()
                };
                bail!("Codex error: {detail}");
            }
            let code = event.get("code").map(|c| match c {
                Value::String(s) => s.clone(),
                Value::Null => "null".into(),
                other => other.to_string(),
            });
            bail!(
                "Error Code {}: {message}",
                code.unwrap_or_else(|| "undefined".into())
            );
        }
        "response.failed" => {
            let response = &event["response"];
            let error = response.get("error").filter(|e| e.is_object());
            if state.codex {
                bail!(
                    "{}",
                    error
                        .and_then(|e| e.get("message"))
                        .and_then(Value::as_str)
                        .unwrap_or("Codex response failed")
                );
            }
            let message = match error {
                Some(e) => format!(
                    "{}: {}",
                    e.get("code").and_then(Value::as_str).unwrap_or("unknown"),
                    e.get("message")
                        .and_then(Value::as_str)
                        .unwrap_or("no message")
                ),
                None => match response
                    .get("incomplete_details")
                    .and_then(|d| d.get("reason"))
                    .and_then(Value::as_str)
                {
                    Some(reason) => format!("incomplete: {reason}"),
                    None => "Unknown error (no error details in response)".into(),
                },
            };
            bail!("{message}");
        }
        _ => {}
    }
    Ok(false)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::catalog::ModelCatalog;
    use crate::message::ToolResultMessage;
    use crate::provider::ToolSpec;
    use crate::provider::test_server::{MockResponse, serve, sse_data};
    use base64::Engine as _;

    fn openai() -> ProviderConfig {
        ProviderConfig {
            id: "openai".into(),
            label: "OpenAI".into(),
            kind: ProviderKind::OpenAiResponses,
            base_url: "https://api.openai.com/v1".into(),
            api_key: Some("sk".into()),
            ..Default::default()
        }
    }

    fn model(provider: &str, id: &str) -> ModelSpec {
        ModelCatalog::builtin()
            .find_model(provider, id)
            .unwrap()
            .clone()
    }

    fn jwt(account: &str) -> String {
        let engine = base64::engine::general_purpose::URL_SAFE_NO_PAD;
        let payload = json!({ "https://api.openai.com/auth": { "chatgpt_account_id": account } });
        format!(
            "{}.{}.sig",
            engine.encode(b"{\"alg\":\"none\"}"),
            engine.encode(payload.to_string())
        )
    }

    fn tool() -> ToolSpec {
        ToolSpec {
            name: "read".into(),
            description: "d".into(),
            parameters: json!({"type":"object"}),
        }
    }

    fn history() -> Vec<Message> {
        let reasoning =
            json!({"type":"reasoning","id":"rs_1","summary":[],"encrypted_content":"ENC"});
        vec![
            Message::user_text("hi"),
            Message::Assistant(AssistantMessage {
                provider: "openai".into(),
                api: "openai-responses".into(),
                model: "gpt-5".into(),
                content: vec![
                    ContentBlock::Thinking {
                        thinking: "t".into(),
                        signature: Some(reasoning.to_string()),
                        redacted: false,
                    },
                    ContentBlock::text("Looking"),
                    ContentBlock::ToolCall {
                        id: "call_1|fc_1".into(),
                        name: "read".into(),
                        arguments: json!({"path":"a"}),
                    },
                ],
                stop_reason: StopReason::ToolCall,
                ..Default::default()
            }),
            Message::ToolResult(ToolResultMessage {
                tool_call_id: "call_1|fc_1".into(),
                tool_name: "read".into(),
                content: vec![ContentBlock::text("data")],
                ..Default::default()
            }),
        ]
    }

    #[test]
    fn openai_body_replays_reasoning_and_item_ids() {
        let request = Request {
            model: "gpt-5".into(),
            system: "sys".into(),
            messages: history(),
            tools: vec![tool()],
            thinking: ThinkingLevel::High,
            session_id: Some("sess".into()),
            cache_retention: Some(CacheRetention::Long),
            service_tier: Some("flex".into()),
            ..Default::default()
        };
        let built = build_request(
            &openai(),
            &model("openai", "gpt-5"),
            ProviderKind::OpenAiResponses,
            &request,
        )
        .unwrap();
        assert_eq!(built.url, "https://api.openai.com/v1/responses");
        assert_eq!(built.headers["session_id"], "sess");
        assert_eq!(built.headers["x-client-request-id"], "sess");
        assert_eq!(built.headers["authorization"], "Bearer sk");
        let body = &built.body;
        assert_eq!(body["store"], false);
        assert_eq!(body["prompt_cache_key"], "sess");
        assert_eq!(body["prompt_cache_retention"], "24h");
        assert_eq!(body["service_tier"], "flex");
        assert_eq!(body["reasoning"], json!({"effort":"high","summary":"auto"}));
        assert_eq!(body["include"][0], "reasoning.encrypted_content");
        assert_eq!(body["tools"][0]["strict"], false);
        let input = body["input"].as_array().unwrap();
        assert_eq!(input[0]["role"], "developer");
        assert_eq!(input[1]["content"][0]["type"], "input_text");
        assert_eq!(input[2]["encrypted_content"], "ENC");
        assert_eq!(input[3]["type"], "message");
        assert_eq!(input[3]["id"], "msg_1");
        assert_eq!(
            input[4],
            json!({"type":"function_call","call_id":"call_1","name":"read","arguments":"{\"path\":\"a\"}","id":"fc_1"})
        );
        assert_eq!(
            input[5],
            json!({"type":"function_call_output","call_id":"call_1","output":"data"})
        );

        // A different model of the same provider drops fc_ item ids; reasoning
        // and thinking from a different model are converted to text.
        let other = Request {
            model: "gpt-5-mini".into(),
            thinking: ThinkingLevel::Off,
            ..request.clone()
        };
        let built = build_request(
            &openai(),
            &model("openai", "gpt-5-mini"),
            ProviderKind::OpenAiResponses,
            &other,
        )
        .unwrap();
        let input = built.body["input"].as_array().unwrap();
        assert_eq!(input[2]["type"], "message");
        assert_eq!(input[2]["content"][0]["text"], "t");
        let call = input.iter().find(|i| i["type"] == "function_call").unwrap();
        assert!(call.get("id").is_none());
        // gpt-5-mini maps off to null: no reasoning field at all.
        assert!(built.body.get("reasoning").is_none());

        let gpt51 = Request {
            model: "gpt-5.1".into(),
            thinking: ThinkingLevel::Off,
            ..request
        };
        let built = build_request(
            &openai(),
            &model("openai", "gpt-5.1"),
            ProviderKind::OpenAiResponses,
            &gpt51,
        )
        .unwrap();
        assert_eq!(built.body["reasoning"], json!({"effort":"none"}));
    }

    #[test]
    fn foreign_tool_call_ids_are_normalised() {
        let source = AssistantMessage {
            provider: "anthropic".into(),
            api: "anthropic-messages".into(),
            ..Default::default()
        };
        let id = normalize_tool_call_id(
            "toolu_01|item+/=",
            "openai",
            "openai-responses",
            &source,
            OPENAI_TOOL_CALL_PROVIDERS,
        );
        assert_eq!(id, format!("toolu_01|fc_{}", short_hash("item+/=")));
        let same = AssistantMessage {
            provider: "openai".into(),
            api: "openai-responses".into(),
            ..Default::default()
        };
        assert_eq!(
            normalize_tool_call_id(
                "c|rs_9",
                "openai",
                "openai-responses",
                &same,
                OPENAI_TOOL_CALL_PROVIDERS
            ),
            "c|fc_rs_9"
        );
        assert_eq!(
            normalize_tool_call_id(
                "a.b__",
                "zai",
                "openai-responses",
                &same,
                OPENAI_TOOL_CALL_PROVIDERS
            ),
            "a_b"
        );
    }

    #[test]
    fn codex_body_and_headers() {
        let token = jwt("acct-123");
        let provider = ProviderConfig {
            id: "openai-codex".into(),
            kind: ProviderKind::OpenAiCodexResponses,
            api_key: Some(token.clone()),
            ..Default::default()
        };
        let m = model("openai-codex", "gpt-5.4");
        let request = Request {
            model: m.id.clone(),
            system: "sys prompt".into(),
            messages: history(),
            tools: vec![tool()],
            thinking: ThinkingLevel::Minimal,
            session_id: Some("s".into()),
            text_verbosity: Some("medium".into()),
            ..Default::default()
        };
        let built =
            build_request(&provider, &m, ProviderKind::OpenAiCodexResponses, &request).unwrap();
        assert_eq!(built.url, "https://chatgpt.com/backend-api/codex/responses");
        assert_eq!(built.headers["chatgpt-account-id"], "acct-123");
        assert_eq!(built.headers["originator"], "pi");
        assert_eq!(built.headers["OpenAI-Beta"], "responses=experimental");
        assert_eq!(built.headers["session_id"], "s");
        assert!(built.headers["user-agent"].starts_with("pi/"));
        let body = &built.body;
        assert_eq!(body["instructions"], "sys prompt");
        assert_eq!(body["text"]["verbosity"], "medium");
        assert_eq!(body["tool_choice"], "auto");
        assert_eq!(body["parallel_tool_calls"], true);
        assert_eq!(body["prompt_cache_key"], "s");
        assert!(body["tools"][0]["strict"].is_null());
        assert_eq!(body["reasoning"], json!({"effort":"low","summary":"auto"}));
        assert!(body.get("max_output_tokens").is_none());
        assert_ne!(body["input"][0]["role"], "developer");

        let no_system = Request {
            system: String::new(),
            thinking: ThinkingLevel::Off,
            ..request
        };
        let built = build_request(
            &provider,
            &m,
            ProviderKind::OpenAiCodexResponses,
            &no_system,
        )
        .unwrap();
        assert_eq!(built.body["instructions"], DEFAULT_CODEX_INSTRUCTIONS);
        assert!(built.body.get("reasoning").is_none());

        let bad = ProviderConfig {
            api_key: Some("not-a-jwt".into()),
            ..provider
        };
        let err =
            build_request(&bad, &m, ProviderKind::OpenAiCodexResponses, &no_system).unwrap_err();
        assert_eq!(err.to_string(), "Failed to extract accountId from token");
        assert_eq!(
            resolve_codex_url("https://x/codex/"),
            "https://x/codex/responses"
        );
        assert_eq!(
            resolve_codex_url(""),
            "https://chatgpt.com/backend-api/codex/responses"
        );
    }

    #[test]
    fn codex_friendly_errors() {
        let msg = codex_error_message(
            429,
            r#"{"error":{"code":"usage_limit_reached","plan_type":"PLUS","message":"x"}}"#,
        );
        assert_eq!(msg, "You have hit your ChatGPT usage limit (plus plan).");
        assert_eq!(
            codex_error_message(400, r#"{"error":{"message":"bad"}}"#),
            "bad"
        );
        assert_eq!(codex_error_message(500, "oops"), "oops");
        assert!(codex_retryable(400, "Upstream connect error"));
        assert!(!codex_retryable(400, "bad request"));
    }

    #[test]
    fn azure_endpoint_and_deployments() {
        assert_eq!(
            normalize_azure_base_url("https://res.openai.azure.com/").unwrap(),
            "https://res.openai.azure.com/openai/v1"
        );
        assert_eq!(
            normalize_azure_base_url("https://res.cognitiveservices.azure.com/openai").unwrap(),
            "https://res.cognitiveservices.azure.com/openai/v1"
        );
        assert_eq!(
            normalize_azure_base_url("https://proxy.example/v1").unwrap(),
            "https://proxy.example/v1"
        );
        assert!(normalize_azure_base_url("not a url").is_err());
        assert_eq!(
            azure_deployment_name("gpt-5", Some("gpt-4o=prod4o, gpt-5 = prod5")),
            "prod5"
        );
        assert_eq!(azure_deployment_name("gpt-5", None), "gpt-5");

        let mut m = model("azure-openai-responses", "gpt-5");
        m.base_url = "https://res.openai.azure.com".into();
        let provider = ProviderConfig {
            id: "azure-openai-responses".into(),
            api_key: Some("az".into()),
            ..Default::default()
        };
        let request = Request {
            model: "gpt-5".into(),
            messages: vec![Message::user_text("x")],
            session_id: Some("s".into()),
            ..Default::default()
        };
        if std::env::var_os("AZURE_OPENAI_BASE_URL").is_none()
            && std::env::var_os("AZURE_OPENAI_RESOURCE_NAME").is_none()
        {
            let built =
                build_request(&provider, &m, ProviderKind::AzureOpenAiResponses, &request).unwrap();
            assert!(
                built
                    .url
                    .starts_with("https://res.openai.azure.com/openai/v1/responses?api-version=")
            );
            assert_eq!(built.headers["api-key"], "az");
            assert!(built.body.get("store").is_none());
            assert_eq!(built.body["prompt_cache_key"], "s");
        }
    }

    #[test]
    fn processes_stream_events() {
        let provider = openai();
        let mut partial = PartialMessage::new(&provider, "gpt-5");
        let mut state = StreamState {
            model_id: "gpt-5".into(),
            request_tier: Some("priority".into()),
            ..Default::default()
        };
        let events = vec![
            json!({"type":"response.created","response":{"id":"resp_1"}}),
            json!({"type":"response.output_item.added","item":{"type":"reasoning","id":"rs_1","summary":[]}}),
            json!({"type":"response.reasoning_summary_part.added","part":{"type":"summary_text","text":""}}),
            json!({"type":"response.reasoning_summary_text.delta","delta":"Think"}),
            json!({"type":"response.reasoning_summary_part.done"}),
            json!({"type":"response.output_item.done","item":{"type":"reasoning","id":"rs_1","summary":[{"type":"summary_text","text":"Think"}],"encrypted_content":"E"}}),
            json!({"type":"response.output_item.added","item":{"type":"message","id":"msg_1","content":[]}}),
            json!({"type":"response.content_part.added","part":{"type":"output_text","text":""}}),
            json!({"type":"response.output_text.delta","delta":"Hi"}),
            json!({"type":"response.output_item.done","item":{"type":"message","id":"msg_1","content":[{"type":"output_text","text":"Hi!"}]}}),
            json!({"type":"response.output_item.added","item":{"type":"function_call","id":"fc_1","call_id":"call_1","name":"read","arguments":""}}),
            json!({"type":"response.function_call_arguments.delta","delta":"{\"path\""}),
            json!({"type":"response.function_call_arguments.done","arguments":"{\"path\":\"x\"}"}),
            json!({"type":"response.output_item.done","item":{"type":"function_call","id":"fc_1","call_id":"call_1","name":"read","arguments":"{\"path\":\"x\"}"}}),
        ];
        for e in &events {
            assert!(!apply_event(e, &mut state, &mut partial, &mut |_| {}).unwrap());
        }
        let done = json!({"type":"response.completed","response":{"id":"resp_1","status":"completed","usage":{"input_tokens":1000000,"output_tokens":0,"total_tokens":1000000,"input_tokens_details":{"cached_tokens":0}}}});
        assert!(apply_event(&done, &mut state, &mut partial, &mut |_| {}).unwrap());
        partial.apply_cost(&model("openai", "gpt-5"));
        let m = partial.finish();
        assert_eq!(m.stop_reason, StopReason::ToolCall);
        assert_eq!(m.text(), "Hi!");
        assert_eq!(m.response_id.as_deref(), Some("resp_1"));
        let ContentBlock::Thinking {
            thinking,
            signature,
            ..
        } = &m.content[0]
        else {
            panic!()
        };
        assert_eq!(thinking, "Think");
        assert_eq!(
            serde_json::from_str::<Value>(signature.as_ref().unwrap()).unwrap()["encrypted_content"],
            "E"
        );
        let (id, _, args) = m.tool_calls().next().unwrap();
        assert_eq!(id, "call_1|fc_1");
        assert_eq!(args["path"], "x");
        // priority doubles the 1.25 $/M input price
        assert!((m.usage.total_cost_usd - 2.5).abs() < 1e-9);
    }

    #[test]
    fn failure_events_become_errors() {
        let provider = openai();
        let mut partial = PartialMessage::new(&provider, "m");
        let mut state = StreamState::default();
        let err = apply_event(&json!({"type":"response.failed","response":{"error":{"code":"server_error","message":"boom"}}}), &mut state, &mut partial, &mut |_| {}).unwrap_err();
        assert_eq!(err.to_string(), "server_error: boom");
        let err = apply_event(
            &json!({"type":"error","code":"rate_limit","message":"slow"}),
            &mut state,
            &mut partial,
            &mut |_| {},
        )
        .unwrap_err();
        assert_eq!(err.to_string(), "Error Code rate_limit: slow");
        let mut codex = StreamState {
            codex: true,
            ..Default::default()
        };
        let err = apply_event(
            &json!({"type":"error","code":"x"}),
            &mut codex,
            &mut partial,
            &mut |_| {},
        )
        .unwrap_err();
        assert_eq!(err.to_string(), "Codex error: x");
    }

    #[tokio::test]
    async fn codex_stream_against_mock_server() {
        let body = sse_data(&[
            json!({"type":"response.output_item.added","item":{"type":"message","id":"m","content":[]}}),
            json!({"type":"response.content_part.added","part":{"type":"output_text","text":""}}),
            json!({"type":"response.output_text.delta","delta":"ok"}),
            json!({"type":"response.output_item.done","item":{"type":"message","id":"m","content":[{"type":"output_text","text":"ok"}]}}),
            json!({"type":"response.done","response":{"id":"r","status":"completed","service_tier":"default","usage":{"input_tokens":10,"output_tokens":2,"total_tokens":12}}}),
        ]);
        let (base, requests) = serve(vec![
            MockResponse::json(
                400,
                r#"{"error":{"message":"service unavailable, try later"}}"#,
            ),
            MockResponse::sse(body),
        ])
        .await;
        let mut m = model("openai-codex", "gpt-5.5");
        m.base_url = base.clone();
        let provider = ProviderConfig {
            id: "openai-codex".into(),
            kind: ProviderKind::OpenAiCodexResponses,
            api_key: Some(jwt("acc")),
            ..Default::default()
        };
        let request = Request {
            model: m.id.clone(),
            messages: vec![Message::user_text("x")],
            model_spec: Some(m),
            service_tier: Some("priority".into()),
            ..Default::default()
        };
        let msg = crate::provider::stream(
            &reqwest::Client::new(),
            &provider,
            &request,
            &CancellationToken::new(),
            &mut |_| {},
        )
        .await;
        assert_eq!(msg.error_message, None);
        assert_eq!(msg.text(), "ok");
        assert_eq!(msg.api, "openai-codex-responses");
        assert_eq!(msg.usage.total_tokens, 12);
        // gpt-5.5 priority pricing: 2.5 x (10*5 + 2*30)/1e6
        assert!((msg.usage.total_cost_usd - 2.5 * 110.0 / 1e6).abs() < 1e-12);
        let requests = requests.lock().unwrap();
        assert_eq!(requests.len(), 2);
        assert_eq!(requests[1].path, "/codex/responses");
        assert_eq!(requests[1].headers["chatgpt-account-id"], "acc");
    }
}
