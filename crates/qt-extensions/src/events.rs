//! Lifecycle event payloads and result decoding (archived
//! `extensions/contributions.cpp` decoders and merge rules).
//!
//! Every handler receives the event as a JSON object with a `type` field
//! (subprocess extensions receive it as the params of `qt.event.<type>`).
//! Handlers return `null` for "no change" or an object whose fields are
//! decoded here; malformed results are errors reported with the archived
//! wording and do not change the event.

use serde_json::{Map, Value, json};

use qt_core::harness::{CompactionPreparation, CompactionResult};
use qt_core::message::{AgentMessage, ContentBlock, CustomMessage, UserContent};

fn obj<'a>(
    result: &'a Value,
    context: &str,
    null_ok: bool,
) -> Result<Option<&'a Map<String, Value>>, String> {
    match result {
        Value::Null if null_ok => Ok(None),
        Value::Object(o) => Ok(Some(o)),
        _ => Err(format!(
            "{context} must be {}",
            if null_ok {
                "an object or null"
            } else {
                "a JSON object"
            }
        )),
    }
}

fn required_string(o: &Map<String, Value>, field: &str, context: &str) -> Result<String, String> {
    o.get(field)
        .and_then(Value::as_str)
        .map(str::to_string)
        .ok_or_else(|| format!("{context} field '{field}' must be a string"))
}

fn optional_bool(
    o: &Map<String, Value>,
    field: &str,
    context: &str,
) -> Result<Option<bool>, String> {
    match o.get(field) {
        None => Ok(None),
        Some(Value::Bool(b)) => Ok(Some(*b)),
        Some(_) => Err(format!("{context} field '{field}' must be a boolean")),
    }
}

/// `images` must be null or an array.
pub fn valid_images(images: &Value) -> bool {
    images.is_null() || images.is_array()
}

// ----- input ----------------------------------------------------------------

/// Where user input came from.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InputSource {
    Interactive,
    Rpc,
    Extension,
}

impl InputSource {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Interactive => "interactive",
            Self::Rpc => "rpc",
            Self::Extension => "extension",
        }
    }
}

/// Decoded `input` handler result.
#[derive(Debug, Clone, PartialEq)]
pub enum InputAction {
    Continue,
    Handled,
    Transform { text: String, images: Option<Value> },
}

pub fn decode_input_result(result: &Value) -> Result<InputAction, String> {
    let o = obj(result, "extension input event result", false)?.expect("object");
    let action = required_string(o, "action", "extension input event result")?;
    match action.as_str() {
        "continue" => Ok(InputAction::Continue),
        "handled" => Ok(InputAction::Handled),
        "transform" => {
            let text = required_string(o, "text", "extension input transform result")?;
            let images = match o.get("images") {
                None => None,
                Some(i) if valid_images(i) => Some(i.clone()),
                Some(_) => {
                    return Err(
                        "extension input transform result images must be null or a JSON array"
                            .into(),
                    );
                }
            };
            Ok(InputAction::Transform { text, images })
        }
        _ => Err(
            "extension input event result action must be continue, transform, or handled".into(),
        ),
    }
}

// ----- before_agent_start ---------------------------------------------------

#[derive(Debug, Clone, PartialEq, Default)]
pub struct BeforeAgentStartDecoded {
    pub message: Option<CustomMessage>,
    pub system_prompt: Option<String>,
}

fn decode_injected_message(value: &Value, context: &str) -> Result<CustomMessage, String> {
    let o = value
        .as_object()
        .ok_or_else(|| format!("{context} must be an object"))?;
    let custom_type = required_string(o, "customType", context)?;
    if custom_type.is_empty() {
        return Err(format!("{context} field 'customType' must not be empty"));
    }
    let content = required_string(o, "content", context)?;
    let display = optional_bool(o, "display", context)?.unwrap_or(true);
    let mut message = CustomMessage::new(custom_type, UserContent::Text(content), display);
    message.details = o.get("details").filter(|d| !d.is_null()).cloned();
    Ok(message)
}

pub fn decode_before_agent_start_result(result: &Value) -> Result<BeforeAgentStartDecoded, String> {
    let Some(o) = obj(result, "extension before_agent_start result", true)? else {
        return Ok(BeforeAgentStartDecoded::default());
    };
    let mut out = BeforeAgentStartDecoded::default();
    if let Some(m) = o.get("message").filter(|m| !m.is_null()) {
        out.message = Some(decode_injected_message(
            m,
            "extension before_agent_start result field 'message'",
        )?);
    }
    if let Some(p) = o.get("systemPrompt") {
        out.system_prompt = Some(p.as_str().map(str::to_string).ok_or_else(|| {
            "extension before_agent_start result field 'systemPrompt' must be a string".to_string()
        })?);
    }
    Ok(out)
}

// ----- tool_call / tool_result ------------------------------------------------

#[derive(Debug, Clone, PartialEq, Default)]
pub struct ToolCallDecoded {
    pub block: bool,
    pub reason: Option<String>,
    pub input: Option<Value>,
}

pub fn decode_tool_call_result(result: &Value) -> Result<ToolCallDecoded, String> {
    let Some(o) = obj(result, "extension tool_call result", true)? else {
        return Ok(ToolCallDecoded::default());
    };
    let context = "extension tool_call result";
    let mut out = ToolCallDecoded {
        block: optional_bool(o, "block", context)?.unwrap_or(false),
        ..Default::default()
    };
    match o.get("reason") {
        None | Some(Value::Null) => {}
        Some(Value::String(s)) => out.reason = Some(s.clone()),
        Some(_) => return Err(format!("{context} field 'reason' must be a string")),
    }
    match o.get("input") {
        None | Some(Value::Null) => {}
        Some(v @ Value::Object(_)) => out.input = Some(v.clone()),
        Some(_) => return Err(format!("{context} field 'input' must be an object")),
    }
    Ok(out)
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct ToolResultDecoded {
    pub content: Option<Vec<ContentBlock>>,
    pub details: Option<Value>,
    pub is_error: Option<bool>,
}

pub fn decode_tool_result_result(result: &Value) -> Result<ToolResultDecoded, String> {
    let Some(o) = obj(result, "extension tool_result result", true)? else {
        return Ok(ToolResultDecoded::default());
    };
    let context = "extension tool_result result";
    let mut out = ToolResultDecoded::default();
    if let Some(c) = o.get("content") {
        if !c.is_array() {
            return Err(format!("{context} field 'content' must be a JSON array"));
        }
        out.content = Some(
            serde_json::from_value(c.clone())
                .map_err(|e| format!("{context} field 'content' is invalid: {e}"))?,
        );
    }
    out.details = o.get("details").cloned();
    out.is_error = optional_bool(o, "isError", context)?;
    Ok(out)
}

// ----- context ---------------------------------------------------------------

/// `{messages: [...]}` replaces the context; anything else keeps it.
pub fn decode_context_result(result: &Value) -> Result<Option<Vec<AgentMessage>>, String> {
    let Some(messages) = result.as_object().and_then(|o| o.get("messages")) else {
        return Ok(None);
    };
    serde_json::from_value(messages.clone())
        .map(Some)
        .map_err(|e| format!("extension context hook returned invalid messages: {e}"))
}

// ----- before_provider_request -------------------------------------------------

#[derive(Debug, Clone, PartialEq, Default)]
pub struct ProviderRequestPatch {
    pub system_prompt: Option<String>,
    pub messages: Option<Vec<qt_core::message::Message>>,
    pub max_tokens: Option<u32>,
    pub temperature: Option<f64>,
}

pub fn decode_provider_request_result(result: &Value) -> Result<ProviderRequestPatch, String> {
    let Some(o) = obj(result, "extension before_provider_request result", true)? else {
        return Ok(ProviderRequestPatch::default());
    };
    let context = "extension before_provider_request result";
    let mut out = ProviderRequestPatch::default();
    if let Some(p) = o.get("systemPrompt") {
        out.system_prompt = Some(
            p.as_str()
                .ok_or_else(|| format!("{context} field 'systemPrompt' must be a string"))?
                .into(),
        );
    }
    if let Some(m) = o.get("messages") {
        out.messages = Some(
            serde_json::from_value(m.clone())
                .map_err(|e| format!("{context} field 'messages' is invalid: {e}"))?,
        );
    }
    if let Some(t) = o.get("maxTokens") {
        out.max_tokens = Some(
            t.as_u64()
                .and_then(|v| u32::try_from(v).ok())
                .ok_or_else(|| {
                    format!("{context} field 'maxTokens' must be a non-negative integer")
                })?,
        );
    }
    if let Some(t) = o.get("temperature") {
        out.temperature = Some(
            t.as_f64()
                .ok_or_else(|| format!("{context} field 'temperature' must be a number"))?,
        );
    }
    Ok(out)
}

// ----- session events --------------------------------------------------------

/// `{cancel}` results of `session_before_switch` (and the cancel part of others).
pub fn decode_cancel(result: &Value, event: &str) -> Result<bool, String> {
    let context = format!("extension {event} result");
    let Some(o) = obj(result, &context, true)? else {
        return Ok(false);
    };
    Ok(optional_bool(o, "cancel", &context)?.unwrap_or(false))
}

/// `session_before_fork`: `(cancel, skipConversationRestore)`.
pub fn decode_before_fork_result(result: &Value) -> Result<(bool, bool), String> {
    let context = "extension session_before_fork result";
    let Some(o) = obj(result, context, true)? else {
        return Ok((false, false));
    };
    Ok((
        optional_bool(o, "cancel", context)?.unwrap_or(false),
        optional_bool(o, "skipConversationRestore", context)?.unwrap_or(false),
    ))
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct BeforeCompactDecoded {
    pub cancel: bool,
    pub compaction: Option<CompactionResult>,
}

pub fn decode_before_compact_result(result: &Value) -> Result<BeforeCompactDecoded, String> {
    let context = "extension session_before_compact result";
    let Some(o) = obj(result, context, true)? else {
        return Ok(BeforeCompactDecoded::default());
    };
    let mut out = BeforeCompactDecoded {
        cancel: optional_bool(o, "cancel", context)?.unwrap_or(false),
        compaction: None,
    };
    if let Some(c) = o.get("compaction").filter(|c| !c.is_null()) {
        let c = c
            .as_object()
            .ok_or_else(|| format!("{context} field 'compaction' must be an object"))?;
        let inner = "extension session_before_compact compaction";
        let summary = required_string(c, "summary", inner)?;
        let first_kept_entry_id = required_string(c, "firstKeptEntryId", inner)?;
        let tokens_before = c.get("tokensBefore").and_then(Value::as_u64).ok_or_else(|| {
            "extension session_before_compact compaction field 'tokensBefore' must be a non-negative integer"
                .to_string()
        })?;
        out.compaction = Some(CompactionResult {
            summary,
            first_kept_entry_id,
            tokens_before,
            details: c.get("details").filter(|d| !d.is_null()).cloned(),
        });
    }
    Ok(out)
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct BeforeTreeDecoded {
    pub cancel: bool,
    pub summary: Option<Value>,
    pub custom_instructions: Option<String>,
    pub replace_instructions: Option<bool>,
    pub label: Option<String>,
}

pub fn decode_before_tree_result(result: &Value) -> Result<BeforeTreeDecoded, String> {
    let context = "extension session_before_tree result";
    let Some(o) = obj(result, context, true)? else {
        return Ok(BeforeTreeDecoded::default());
    };
    let mut out = BeforeTreeDecoded {
        cancel: optional_bool(o, "cancel", context)?.unwrap_or(false),
        replace_instructions: optional_bool(o, "replaceInstructions", context)?,
        ..Default::default()
    };
    if let Some(s) = o.get("summary").filter(|s| !s.is_null()) {
        let so = s
            .as_object()
            .ok_or_else(|| format!("{context} field 'summary' must be an object"))?;
        required_string(so, "summary", &format!("{context} field 'summary'"))?;
        out.summary = Some(s.clone());
    }
    for (field, slot) in [
        ("customInstructions", &mut out.custom_instructions),
        ("label", &mut out.label),
    ] {
        if let Some(v) = o.get(field) {
            *slot = Some(
                v.as_str()
                    .ok_or_else(|| format!("{context} field '{field}' must be a string"))?
                    .into(),
            );
        }
    }
    Ok(out)
}

/// `resources_discover`: skill, prompt and theme paths (relative paths are
/// resolved by the caller against the extension directory).
#[derive(Debug, Clone, PartialEq, Default)]
pub struct DiscoveredResources {
    pub skill_paths: Vec<String>,
    pub prompt_paths: Vec<String>,
    pub theme_paths: Vec<String>,
}

pub fn decode_resources_discover_result(result: &Value) -> Result<DiscoveredResources, String> {
    let Some(o) = obj(result, "extension resources_discover result", true)? else {
        return Ok(DiscoveredResources::default());
    };
    let list = |field: &str| -> Result<Vec<String>, String> {
        let prefix = format!("extension resources_discover result field '{field}'");
        match o.get(field) {
            None | Some(Value::Null) => Ok(Vec::new()),
            Some(Value::Array(items)) => items
                .iter()
                .map(|v| match v.as_str() {
                    Some("") => Err(format!("{prefix} must not contain empty paths")),
                    Some(s) => Ok(s.to_string()),
                    None => Err(format!("{prefix} must contain string paths")),
                })
                .collect(),
            Some(_) => Err(format!("{prefix} must be an array")),
        }
    };
    Ok(DiscoveredResources {
        skill_paths: list("skillPaths")?,
        prompt_paths: list("promptPaths")?,
        theme_paths: list("themePaths")?,
    })
}

// ----- payload helpers --------------------------------------------------------

/// `{type: event, ...fields}`.
pub fn event(event_type: &str, fields: Value) -> Value {
    let mut out = json!({ "type": event_type });
    if let (Some(target), Value::Object(fields)) = (out.as_object_mut(), fields) {
        target.extend(fields);
    }
    out
}

pub fn messages_json(messages: &[AgentMessage]) -> Value {
    serde_json::to_value(messages).unwrap_or_else(|_| json!([]))
}

pub fn compaction_result_json(result: &CompactionResult) -> Value {
    json!({
        "summary": result.summary,
        "firstKeptEntryId": result.first_kept_entry_id,
        "tokensBefore": result.tokens_before,
        "details": result.details,
    })
}

pub fn preparation_json(p: &CompactionPreparation) -> Value {
    json!({
        "firstKeptEntryId": p.first_kept_entry_id,
        "messagesToSummarize": messages_json(&p.messages_to_summarize),
        "turnPrefixMessages": messages_json(&p.turn_prefix_messages),
        "isSplitTurn": p.is_split_turn,
        "tokensBefore": p.tokens_before,
        "previousSummary": p.previous_summary,
        "fileOps": {
            "read": p.file_ops.read,
            "written": p.file_ops.written,
            "edited": p.file_ops.edited,
        },
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn input_results() {
        assert_eq!(
            decode_input_result(&json!({"action":"continue"})).unwrap(),
            InputAction::Continue
        );
        assert_eq!(
            decode_input_result(&json!({"action":"handled"})).unwrap(),
            InputAction::Handled
        );
        assert_eq!(
            decode_input_result(&json!({"action":"transform","text":"x","images":[]})).unwrap(),
            InputAction::Transform {
                text: "x".into(),
                images: Some(json!([]))
            }
        );
        assert_eq!(
            decode_input_result(&Value::Null).unwrap_err(),
            "extension input event result must be a JSON object"
        );
        assert_eq!(
            decode_input_result(&json!({})).unwrap_err(),
            "extension input event result field 'action' must be a string"
        );
        assert_eq!(
            decode_input_result(&json!({"action":"x"})).unwrap_err(),
            "extension input event result action must be continue, transform, or handled"
        );
        assert_eq!(
            decode_input_result(&json!({"action":"transform"})).unwrap_err(),
            "extension input transform result field 'text' must be a string"
        );
        assert_eq!(
            decode_input_result(&json!({"action":"transform","text":"","images":1})).unwrap_err(),
            "extension input transform result images must be null or a JSON array"
        );
    }

    #[test]
    fn before_agent_start_results() {
        let d = decode_before_agent_start_result(
            &json!({"message":{"customType":"note","content":"hi","details":{"a":1}},"systemPrompt":""}),
        )
        .unwrap();
        let m = d.message.unwrap();
        assert_eq!((m.custom_type.as_str(), m.display), ("note", true));
        assert_eq!(m.details, Some(json!({"a":1})));
        assert_eq!(d.system_prompt.as_deref(), Some(""));
        assert_eq!(
            decode_before_agent_start_result(&json!({"message":{"customType":"","content":"x"}}))
                .unwrap_err(),
            "extension before_agent_start result field 'message' field 'customType' must not be empty"
        );
        assert_eq!(
            decode_before_agent_start_result(
                &json!({"message":{"customType":"t","content":"x","display":1}})
            )
            .unwrap_err(),
            "extension before_agent_start result field 'message' field 'display' must be a boolean"
        );
        assert_eq!(
            decode_before_agent_start_result(&json!({"systemPrompt":1})).unwrap_err(),
            "extension before_agent_start result field 'systemPrompt' must be a string"
        );
        assert_eq!(
            decode_before_agent_start_result(&json!(3)).unwrap_err(),
            "extension before_agent_start result must be an object or null"
        );
    }

    #[test]
    fn tool_results() {
        let d = decode_tool_call_result(&json!({"block":true,"reason":"no"})).unwrap();
        assert!(d.block);
        assert_eq!(
            decode_tool_call_result(&json!({"block":"y"})).unwrap_err(),
            "extension tool_call result field 'block' must be a boolean"
        );
        assert_eq!(
            decode_tool_call_result(&json!({"input":[]})).unwrap_err(),
            "extension tool_call result field 'input' must be an object"
        );
        let r = decode_tool_result_result(
            &json!({"content":[{"type":"text","text":"x"}],"isError":true}),
        )
        .unwrap();
        assert_eq!(r.content.unwrap(), vec![ContentBlock::text("x")]);
        assert_eq!(r.is_error, Some(true));
        assert_eq!(
            decode_tool_result_result(&json!({"content":"x"})).unwrap_err(),
            "extension tool_result result field 'content' must be a JSON array"
        );
    }

    #[test]
    fn session_results() {
        assert!(decode_cancel(&json!({"cancel":true}), "session_before_switch").unwrap());
        assert_eq!(
            decode_cancel(&json!({"cancel":1}), "session_before_switch").unwrap_err(),
            "extension session_before_switch result field 'cancel' must be a boolean"
        );
        assert_eq!(
            decode_before_fork_result(&json!({"skipConversationRestore":true})).unwrap(),
            (false, true)
        );
        let c = decode_before_compact_result(
            &json!({"compaction":{"summary":"s","firstKeptEntryId":"e","tokensBefore":5}}),
        )
        .unwrap();
        assert_eq!(c.compaction.unwrap().tokens_before, 5);
        assert_eq!(
            decode_before_compact_result(
                &json!({"compaction":{"summary":"s","firstKeptEntryId":"e","tokensBefore":-1}})
            )
            .unwrap_err(),
            "extension session_before_compact compaction field 'tokensBefore' must be a non-negative integer"
        );
        let t =
            decode_before_tree_result(&json!({"label":"x","replaceInstructions":true})).unwrap();
        assert_eq!(t.label.as_deref(), Some("x"));
        let r = decode_resources_discover_result(&json!({"skillPaths":["a"],"themePaths":null}))
            .unwrap();
        assert_eq!(r.skill_paths, ["a"]);
        assert_eq!(
            decode_resources_discover_result(&json!({"promptPaths":[""]})).unwrap_err(),
            "extension resources_discover result field 'promptPaths' must not contain empty paths"
        );
        assert_eq!(decode_context_result(&json!(null)).unwrap(), None);
        let m = decode_context_result(&json!({"messages":[{"role":"user","content":"hi"}]}))
            .unwrap()
            .unwrap();
        assert!(m[0].is_user());
        assert_eq!(
            decode_provider_request_result(&json!({"maxTokens":-1})).unwrap_err(),
            "extension before_provider_request result field 'maxTokens' must be a non-negative integer"
        );
        assert_eq!(event("x", json!({"a":1})), json!({"type":"x","a":1}));
    }
}
