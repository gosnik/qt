//! RPC wire protocol (archived `rpc_protocol.cpp`, Pi `rpc-types.ts`):
//! command decoding with the archived validation messages and the response
//! envelopes `{id?, type: "response", command, success, data?}` /
//! `{..., success: false, error, error_code, errorCode}`.

use serde_json::{Map, Value, json};

use qt_core::QueueMode;
use qt_core::message::ContentBlock;
use qt_core::provider::ThinkingLevel;

/// `streamingBehavior` of a `prompt` sent while the agent runs.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StreamingBehavior {
    Steer,
    FollowUp,
}

/// One decoded command.
#[derive(Debug, Clone, PartialEq)]
pub enum Command {
    Prompt {
        message: String,
        images: Vec<ContentBlock>,
        streaming: Option<StreamingBehavior>,
    },
    Steer {
        message: String,
        images: Vec<ContentBlock>,
    },
    FollowUp {
        message: String,
        images: Vec<ContentBlock>,
    },
    Abort,
    GetState,
    GetMessages,
    SetModel {
        provider: String,
        model_id: String,
    },
    CycleModel,
    GetAvailableModels,
    SetThinkingLevel {
        level: ThinkingLevel,
    },
    CycleThinkingLevel,
    SetSteeringMode {
        mode: QueueMode,
    },
    SetFollowUpMode {
        mode: QueueMode,
    },
    Compact {
        custom_instructions: Option<String>,
    },
    SetAutoCompaction {
        enabled: bool,
    },
    SetAutoRetry {
        enabled: bool,
    },
    AbortRetry,
    Bash {
        command: String,
    },
    AbortBash,
    NewSession {
        parent_session: Option<String>,
    },
    SwitchSession {
        session_path: String,
    },
    Fork {
        entry_id: String,
    },
    Clone,
    GetSessionStats,
    GetForkMessages,
    ExportHtml {
        output_path: Option<String>,
    },
    GetLastAssistantText,
    SetSessionName {
        name: String,
    },
    GetCommands,
    Memory {
        action: String,
        params: Value,
        confirm: bool,
    },
    Subagent {
        action: String,
        arguments: Value,
    },
}

/// Every command type, in the archived `RpcCommandKind` order.
pub const COMMAND_TYPES: &[&str] = &[
    "prompt",
    "steer",
    "follow_up",
    "abort",
    "get_state",
    "get_messages",
    "set_model",
    "cycle_model",
    "get_available_models",
    "set_thinking_level",
    "cycle_thinking_level",
    "set_steering_mode",
    "set_follow_up_mode",
    "compact",
    "set_auto_compaction",
    "set_auto_retry",
    "abort_retry",
    "bash",
    "abort_bash",
    "new_session",
    "switch_session",
    "fork",
    "clone",
    "get_session_stats",
    "get_fork_messages",
    "export_html",
    "get_last_assistant_text",
    "set_session_name",
    "get_commands",
    "memory",
    "subagent",
];

/// Event types a client must not send (archived `RpcEventKind`).
const EVENT_TYPES: &[&str] = &[
    "agent_start",
    "agent_end",
    "turn_start",
    "turn_end",
    "message_start",
    "message_update",
    "tool_execution_start",
    "tool_execution_update",
    "tool_execution_end",
    "queue_update",
    "compaction_start",
    "compaction_end",
    "auto_retry_start",
    "auto_retry_end",
    "extension_error",
    "extension_ui_request",
    "message_end",
];

/// Archived memory actions.
const MEMORY_ACTIONS: &[&str] = &[
    "status",
    "dream",
    "recall",
    "explain",
    "remember",
    "review",
    "feedback",
    "forget",
    "retrieval_rebuild",
];

/// Archived persistent-worker actions of the `subagent` command.
const SUBAGENT_ACTIONS: &[&str] = &[
    "spawn",
    "send",
    "follow_up",
    "wait",
    "interrupt",
    "list_workers",
];

/// A command with its correlation id.
#[derive(Debug, Clone, PartialEq)]
pub struct Request {
    pub id: Option<String>,
    /// The command `type` (`steer`, `set_auto_retry`, ...).
    pub kind: &'static str,
    pub command: Command,
}

/// One input line.
#[derive(Debug, Clone, PartialEq)]
pub enum Frame {
    Command(Request),
    /// `extension_ui_response`: the answer to an `extension_ui_request`
    /// (`value`, `confirmed`, `cancelled` and any future fields).
    UiResponse {
        id: String,
        payload: Map<String, Value>,
    },
}

/// A line that could not be decoded; answered with an error response.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FrameError {
    pub id: Option<String>,
    pub command: String,
    pub message: String,
}

impl FrameError {
    fn invalid(message: impl Into<String>) -> Self {
        Self {
            id: None,
            command: "invalid".into(),
            message: message.into(),
        }
    }
}

/// A failed command: the message and its machine-readable code.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RpcError {
    pub code: String,
    pub message: String,
}

impl RpcError {
    /// The archived default classification.
    pub fn internal(message: impl Into<String>) -> Self {
        Self {
            code: "internal_error".into(),
            message: message.into(),
        }
    }

    pub fn invalid(message: impl Into<String>) -> Self {
        Self {
            code: "invalid_argument".into(),
            message: message.into(),
        }
    }

    pub fn with_code(code: impl Into<String>, message: impl Into<String>) -> Self {
        Self {
            code: code.into(),
            message: message.into(),
        }
    }
}

impl From<anyhow::Error> for RpcError {
    fn from(error: anyhow::Error) -> Self {
        Self::internal(format!("{error:#}"))
    }
}

/// The success envelope; `data` is omitted when `None`.
pub fn success(id: Option<&str>, command: &str, data: Option<Value>) -> Value {
    let mut out = Map::new();
    if let Some(id) = id {
        out.insert("id".into(), json!(id));
    }
    out.insert("type".into(), json!("response"));
    out.insert("command".into(), json!(command));
    out.insert("success".into(), json!(true));
    if let Some(data) = data {
        out.insert("data".into(), data);
    }
    Value::Object(out)
}

/// The error envelope. A message of the form `rpc_error:<code>:<text>`
/// carries its own code (archived `encode_failure_response`). The code is
/// sent as `error_code` (archived wire name) and `errorCode`.
pub fn failure(id: Option<&str>, command: &str, error: &RpcError) -> Value {
    let mut code = error.code.clone();
    let mut message = error.message.clone();
    if let Some(rest) = message.strip_prefix("rpc_error:")
        && let Some((embedded, text)) = rest.split_once(':')
        && !embedded.is_empty()
    {
        code = embedded.to_string();
        message = text.to_string();
    }
    let mut out = Map::new();
    if let Some(id) = id {
        out.insert("id".into(), json!(id));
    }
    out.insert("type".into(), json!("response"));
    out.insert("command".into(), json!(command));
    out.insert("success".into(), json!(false));
    out.insert("error".into(), json!(message));
    out.insert("error_code".into(), json!(code));
    out.insert("errorCode".into(), json!(code));
    Value::Object(out)
}

fn command_type(text: &str) -> Option<&'static str> {
    COMMAND_TYPES.iter().copied().find(|t| *t == text)
}

/// Decode one input line (without its LF).
pub fn decode_line(line: &str) -> Result<Frame, FrameError> {
    let payload = line.strip_suffix('\r').unwrap_or(line);
    if payload.is_empty() {
        return Err(parse_error("rpc frame payload must not be empty"));
    }
    if payload.contains('\r') {
        return Err(parse_error(
            "rpc frame must contain exactly one LF-delimited record",
        ));
    }
    let value: Value = serde_json::from_str(payload).map_err(|e| parse_error(&e.to_string()))?;
    decode_frame(&value)
}

fn parse_error(message: &str) -> FrameError {
    FrameError {
        id: None,
        command: "parse".into(),
        message: format!("Failed to parse command: {message}"),
    }
}

/// Decode a parsed frame (archived `RpcServer::process_line`).
pub fn decode_frame(value: &Value) -> Result<Frame, FrameError> {
    let Some(object) = value.as_object() else {
        return Err(FrameError::invalid("rpc frame must be a JSON object"));
    };
    let Some(kind) = object.get("type") else {
        return Err(FrameError::invalid("rpc frame must contain type"));
    };
    let Some(kind) = kind.as_str() else {
        return Err(FrameError::invalid("rpc frame type must be a string"));
    };
    if kind == "extension_ui_response" {
        let id = required_non_empty(object, "id").map_err(FrameError::invalid)?;
        let payload = object
            .iter()
            .filter(|(k, _)| !matches!(k.as_str(), "type" | "id" | "method"))
            .map(|(k, v)| (k.clone(), v.clone()))
            .collect();
        return Ok(Frame::UiResponse { id, payload });
    }
    if let Some(kind) = command_type(kind) {
        let id_hint = object.get("id").and_then(Value::as_str).map(str::to_string);
        return decode_command(kind, object)
            .map(Frame::Command)
            .map_err(|message| FrameError {
                id: id_hint,
                command: kind.into(),
                message,
            });
    }
    if kind == "response" || EVENT_TYPES.contains(&kind) {
        return Err(FrameError::invalid(format!(
            "rpc server accepts command frames only, received {kind}"
        )));
    }
    Err(FrameError::invalid("unsupported rpc frame type"))
}

type Fields = Map<String, Value>;

fn required_string(object: &Fields, field: &str) -> Result<String, String> {
    match object.get(field) {
        None => Err(format!("key '{field}' not found")),
        Some(Value::String(s)) => Ok(s.clone()),
        Some(_) => Err(format!("{field} must be a string")),
    }
}

fn required_non_empty(object: &Fields, field: &str) -> Result<String, String> {
    let value = required_string(object, field)?;
    if value.is_empty() {
        return Err(format!("{field} must not be empty"));
    }
    Ok(value)
}

fn optional_string(object: &Fields, field: &str) -> Result<Option<String>, String> {
    match object.get(field) {
        None | Some(Value::Null) => Ok(None),
        Some(Value::String(s)) => Ok(Some(s.clone())),
        Some(_) => Err(format!("{field} must be a string")),
    }
}

fn required_bool(object: &Fields, field: &str) -> Result<bool, String> {
    match object.get(field) {
        None => Err(format!("key '{field}' not found")),
        Some(Value::Bool(b)) => Ok(*b),
        Some(_) => Err(format!("{field} must be a boolean")),
    }
}

/// Images of a prompt: `{type: "image", data, mimeType}` objects; entries
/// without data are skipped and the MIME type defaults to `image/png`
/// (archived `rpc_prompt_message`).
fn images(object: &Fields) -> Result<Vec<ContentBlock>, String> {
    let list = match object.get("images") {
        None | Some(Value::Null) => return Ok(Vec::new()),
        Some(Value::Array(list)) => list,
        Some(_) => return Err("images must be a JSON array".into()),
    };
    Ok(list
        .iter()
        .filter_map(Value::as_object)
        .filter_map(|image| {
            let data = image.get("data").and_then(Value::as_str)?;
            if data.is_empty() {
                return None;
            }
            let mime = image
                .get("mimeType")
                .and_then(Value::as_str)
                .filter(|m| !m.is_empty())
                .unwrap_or("image/png");
            Some(ContentBlock::Image {
                data: data.to_string(),
                mime_type: mime.to_string(),
            })
        })
        .collect())
}

fn queue_mode(object: &Fields) -> Result<QueueMode, String> {
    let mode = required_string(object, "mode")?;
    match mode.as_str() {
        "all" => Ok(QueueMode::All),
        "one-at-a-time" => Ok(QueueMode::OneAtATime),
        _ => Err("queue mode must equal 'all' or 'one-at-a-time'".into()),
    }
}

fn decode_command(kind: &'static str, object: &Fields) -> Result<Request, String> {
    let id = match object.get("id") {
        None | Some(Value::Null) => None,
        Some(Value::String(s)) => Some(s.clone()),
        Some(_) => return Err("rpc id must be a string when present".into()),
    };
    let command = match kind {
        "prompt" | "steer" | "follow_up" => {
            let message = required_string(object, "message")?;
            let images = images(object)?;
            if message.is_empty() && images.is_empty() {
                return Err("prompt command message must not be empty".into());
            }
            let streaming = match object.get("streamingBehavior") {
                None | Some(Value::Null) => None,
                Some(_) if kind != "prompt" => {
                    return Err("streamingBehavior is only supported on prompt commands".into());
                }
                Some(Value::String(s)) if s == "steer" => Some(StreamingBehavior::Steer),
                Some(Value::String(s)) if s == "followUp" => Some(StreamingBehavior::FollowUp),
                Some(_) => return Err("unsupported prompt streamingBehavior".into()),
            };
            match kind {
                "prompt" => Command::Prompt {
                    message,
                    images,
                    streaming,
                },
                "steer" => Command::Steer { message, images },
                _ => Command::FollowUp { message, images },
            }
        }
        "abort" => Command::Abort,
        "get_state" => Command::GetState,
        "get_messages" => Command::GetMessages,
        "set_model" => {
            let provider = required_string(object, "provider")?;
            let model_id = required_string(object, "modelId")?;
            if provider.is_empty() || model_id.is_empty() {
                return Err("set_model command provider and modelId must not be empty".into());
            }
            Command::SetModel { provider, model_id }
        }
        "cycle_model" => Command::CycleModel,
        "get_available_models" => Command::GetAvailableModels,
        "set_thinking_level" => {
            let level = required_string(object, "level")?;
            let level = ThinkingLevel::parse(&level).ok_or_else(|| {
                "thinking level must be off, minimal, low, medium, high, or xhigh".to_string()
            })?;
            Command::SetThinkingLevel { level }
        }
        "cycle_thinking_level" => Command::CycleThinkingLevel,
        "set_steering_mode" => Command::SetSteeringMode {
            mode: queue_mode(object)?,
        },
        "set_follow_up_mode" => Command::SetFollowUpMode {
            mode: queue_mode(object)?,
        },
        "compact" => {
            let custom_instructions = optional_string(object, "customInstructions")?;
            if custom_instructions.as_deref() == Some("") {
                return Err("compact customInstructions must not be empty".into());
            }
            Command::Compact {
                custom_instructions,
            }
        }
        "set_auto_compaction" => Command::SetAutoCompaction {
            enabled: required_bool(object, "enabled")?,
        },
        "set_auto_retry" => Command::SetAutoRetry {
            enabled: required_bool(object, "enabled")?,
        },
        "abort_retry" => Command::AbortRetry,
        "bash" => {
            let command = required_string(object, "command")?;
            if command.is_empty() {
                return Err("bash command must not be empty".into());
            }
            Command::Bash { command }
        }
        "abort_bash" => Command::AbortBash,
        "new_session" => {
            let parent_session = optional_string(object, "parentSession")?;
            if parent_session.as_deref() == Some("") {
                return Err("new_session parentSession must not be empty".into());
            }
            Command::NewSession { parent_session }
        }
        "switch_session" => {
            let session_path = required_string(object, "sessionPath")?;
            if session_path.is_empty() {
                return Err("switch_session sessionPath must not be empty".into());
            }
            Command::SwitchSession { session_path }
        }
        "fork" => {
            let entry_id = required_string(object, "entryId")?;
            if entry_id.is_empty() {
                return Err("fork entryId must not be empty".into());
            }
            Command::Fork { entry_id }
        }
        "clone" => Command::Clone,
        "get_session_stats" => Command::GetSessionStats,
        "get_fork_messages" => Command::GetForkMessages,
        "export_html" => {
            let output_path = optional_string(object, "outputPath")?;
            if output_path.as_deref() == Some("") {
                return Err("export_html outputPath must not be empty".into());
            }
            Command::ExportHtml { output_path }
        }
        "get_last_assistant_text" => Command::GetLastAssistantText,
        "set_session_name" => {
            let name = required_string(object, "name")?;
            if name.is_empty() {
                return Err("set_session_name name must not be empty".into());
            }
            Command::SetSessionName { name }
        }
        "get_commands" => Command::GetCommands,
        "memory" => {
            let action = required_string(object, "action")?;
            if !MEMORY_ACTIONS.contains(&action.as_str()) {
                return Err("memory command action is unsupported".into());
            }
            let params = object.get("params").cloned().unwrap_or_else(|| json!({}));
            if !params.is_object() {
                return Err("memory command params must be an object".into());
            }
            let confirm = match object.get("confirm") {
                None => false,
                Some(Value::Bool(b)) => *b,
                Some(_) => return Err("confirm must be a boolean".into()),
            };
            Command::Memory {
                action,
                params,
                confirm,
            }
        }
        "subagent" => {
            let action = required_non_empty(object, "action")?;
            let mut arguments = object
                .get("arguments")
                .cloned()
                .unwrap_or_else(|| json!({}));
            let Some(fields) = arguments.as_object_mut() else {
                return Err("subagent command arguments must be an object".into());
            };
            if !SUBAGENT_ACTIONS.contains(&action.as_str()) {
                return Err("subagent command action is unsupported".into());
            }
            fields.insert("action".into(), json!(action));
            Command::Subagent { action, arguments }
        }
        other => return Err(format!("unsupported rpc command type: {other}")),
    };
    Ok(Request { id, kind, command })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn command(line: &str) -> Request {
        match decode_line(line).unwrap() {
            Frame::Command(r) => r,
            other => panic!("not a command: {other:?}"),
        }
    }

    fn error(line: &str) -> FrameError {
        decode_line(line).unwrap_err()
    }

    #[test]
    fn decodes_every_command_type() {
        let samples = [
            r#"{"type":"prompt","message":"hi"}"#,
            r#"{"type":"steer","message":"hi"}"#,
            r#"{"type":"follow_up","message":"hi"}"#,
            r#"{"type":"abort"}"#,
            r#"{"type":"get_state"}"#,
            r#"{"type":"get_messages"}"#,
            r#"{"type":"set_model","provider":"p","modelId":"m"}"#,
            r#"{"type":"cycle_model"}"#,
            r#"{"type":"get_available_models"}"#,
            r#"{"type":"set_thinking_level","level":"high"}"#,
            r#"{"type":"cycle_thinking_level"}"#,
            r#"{"type":"set_steering_mode","mode":"all"}"#,
            r#"{"type":"set_follow_up_mode","mode":"one-at-a-time"}"#,
            r#"{"type":"compact"}"#,
            r#"{"type":"set_auto_compaction","enabled":true}"#,
            r#"{"type":"set_auto_retry","enabled":false}"#,
            r#"{"type":"abort_retry"}"#,
            r#"{"type":"bash","command":"ls"}"#,
            r#"{"type":"abort_bash"}"#,
            r#"{"type":"new_session"}"#,
            r#"{"type":"switch_session","sessionPath":"/s.jsonl"}"#,
            r#"{"type":"fork","entryId":"e1"}"#,
            r#"{"type":"clone"}"#,
            r#"{"type":"get_session_stats"}"#,
            r#"{"type":"get_fork_messages"}"#,
            r#"{"type":"export_html"}"#,
            r#"{"type":"get_last_assistant_text"}"#,
            r#"{"type":"set_session_name","name":"n"}"#,
            r#"{"type":"get_commands"}"#,
            r#"{"type":"memory","action":"status"}"#,
            r#"{"type":"subagent","action":"list_workers"}"#,
        ];
        assert_eq!(samples.len(), COMMAND_TYPES.len());
        for (sample, kind) in samples.iter().zip(COMMAND_TYPES) {
            assert_eq!(command(sample).kind, *kind, "{sample}");
        }
    }

    #[test]
    fn prompt_fields_and_images() {
        let r = command(
            r#"{"id":"1","type":"prompt","message":"look","streamingBehavior":"followUp","images":[{"type":"image","data":"AAA"},{"type":"image","data":"","mimeType":"image/jpeg"},7]}"#,
        );
        assert_eq!(r.id.as_deref(), Some("1"));
        assert_eq!(
            r.command,
            Command::Prompt {
                message: "look".into(),
                images: vec![ContentBlock::Image {
                    data: "AAA".into(),
                    mime_type: "image/png".into()
                }],
                streaming: Some(StreamingBehavior::FollowUp),
            }
        );
        let e = error(r#"{"type":"steer","message":"x","streamingBehavior":"steer"}"#);
        assert_eq!(
            e.message,
            "streamingBehavior is only supported on prompt commands"
        );
        assert_eq!(e.command, "steer");
        let e = error(r#"{"id":"7","type":"prompt","message":""}"#);
        assert_eq!(e.message, "prompt command message must not be empty");
        assert_eq!(e.id.as_deref(), Some("7"));
        assert_eq!(
            error(r#"{"type":"prompt","message":"x","streamingBehavior":"later"}"#).message,
            "unsupported prompt streamingBehavior"
        );
        assert_eq!(
            error(r#"{"type":"prompt"}"#).message,
            "key 'message' not found"
        );
    }

    #[test]
    fn archived_validation_messages() {
        let cases = [
            (
                r#"{"type":"set_model","provider":"","modelId":"m"}"#,
                "set_model command provider and modelId must not be empty",
            ),
            (
                r#"{"type":"set_thinking_level","level":"max"}"#,
                "thinking level must be off, minimal, low, medium, high, or xhigh",
            ),
            (
                r#"{"type":"set_steering_mode","mode":"some"}"#,
                "queue mode must equal 'all' or 'one-at-a-time'",
            ),
            (
                r#"{"type":"compact","customInstructions":""}"#,
                "compact customInstructions must not be empty",
            ),
            (
                r#"{"type":"set_auto_retry","enabled":"yes"}"#,
                "enabled must be a boolean",
            ),
            (
                r#"{"type":"bash","command":""}"#,
                "bash command must not be empty",
            ),
            (
                r#"{"type":"new_session","parentSession":""}"#,
                "new_session parentSession must not be empty",
            ),
            (
                r#"{"type":"switch_session","sessionPath":""}"#,
                "switch_session sessionPath must not be empty",
            ),
            (
                r#"{"type":"fork","entryId":""}"#,
                "fork entryId must not be empty",
            ),
            (
                r#"{"type":"export_html","outputPath":""}"#,
                "export_html outputPath must not be empty",
            ),
            (
                r#"{"type":"set_session_name","name":""}"#,
                "set_session_name name must not be empty",
            ),
            (
                r#"{"type":"memory","action":"nap"}"#,
                "memory command action is unsupported",
            ),
            (
                r#"{"type":"memory","action":"status","params":[]}"#,
                "memory command params must be an object",
            ),
            (
                r#"{"type":"subagent","action":"run"}"#,
                "subagent command action is unsupported",
            ),
            (
                r#"{"type":"subagent","action":"spawn","arguments":1}"#,
                "subagent command arguments must be an object",
            ),
            (
                r#"{"type":"subagent","action":""}"#,
                "action must not be empty",
            ),
            (
                r#"{"id":5,"type":"get_state"}"#,
                "rpc id must be a string when present",
            ),
        ];
        for (line, message) in cases {
            assert_eq!(error(line).message, message, "{line}");
        }
    }

    #[test]
    fn subagent_arguments_carry_the_action() {
        let r = command(r#"{"type":"subagent","action":"wait","arguments":{"target":"/root/a"}}"#);
        assert_eq!(
            r.command,
            Command::Subagent {
                action: "wait".into(),
                arguments: json!({"target": "/root/a", "action": "wait"}),
            }
        );
    }

    #[test]
    fn frame_errors() {
        let e = error("not json");
        assert_eq!(e.command, "parse");
        assert!(e.message.starts_with("Failed to parse command: "), "{e:?}");
        assert_eq!(
            error("").message,
            "Failed to parse command: rpc frame payload must not be empty"
        );
        assert_eq!(error("[1]").message, "rpc frame must be a JSON object");
        assert_eq!(error("{}").message, "rpc frame must contain type");
        assert_eq!(
            error(r#"{"type":"dance"}"#).message,
            "unsupported rpc frame type"
        );
        assert_eq!(
            error(r#"{"type":"agent_start"}"#).message,
            "rpc server accepts command frames only, received agent_start"
        );
        assert_eq!(
            error(r#"{"type":"response","command":"x","success":true}"#).message,
            "rpc server accepts command frames only, received response"
        );
        assert_eq!(error("{}").command, "invalid");
        // CRLF is accepted.
        assert_eq!(command("{\"type\":\"abort\"}\r").kind, "abort");
    }

    #[test]
    fn ui_responses_keep_their_payload() {
        let frame = decode_line(
            r#"{"type":"extension_ui_response","id":"u1","value":"a","method":"x","future":1}"#,
        )
        .unwrap();
        let Frame::UiResponse { id, payload } = frame else {
            panic!()
        };
        assert_eq!(id, "u1");
        assert_eq!(Value::Object(payload), json!({"value": "a", "future": 1}));
        assert_eq!(
            error(r#"{"type":"extension_ui_response","id":""}"#).message,
            "id must not be empty"
        );
    }

    #[test]
    fn envelopes() {
        assert_eq!(
            success(Some("1"), "get_state", Some(json!({"a": 1}))),
            json!({"id": "1", "type": "response", "command": "get_state", "success": true, "data": {"a": 1}})
        );
        assert_eq!(
            success(None, "abort", None),
            json!({"type": "response", "command": "abort", "success": true})
        );
        assert_eq!(
            failure(None, "prompt", &RpcError::internal("boom")),
            json!({"type": "response", "command": "prompt", "success": false, "error": "boom", "error_code": "internal_error", "errorCode": "internal_error"})
        );
        let coded = failure(
            Some("2"),
            "memory",
            &RpcError::invalid("rpc_error:review_not_found:review r1 not found"),
        );
        assert_eq!(coded["error"], "review r1 not found");
        assert_eq!(coded["error_code"], "review_not_found");
        assert_eq!(coded["errorCode"], "review_not_found");
    }
}
