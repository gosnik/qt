//! The archived llama prompt and tool-calling protocol.
//!
//! The conversation is serialised as an OpenAI-compatible chat completion
//! request JSON inside a plain-text prompt (no chat template). With tools,
//! two controller messages are prepended and, when the user asks for
//! file/search/model work, a third forces `tool_choice: "required"`. The
//! model's reply is scanned for the first balanced JSON object, which is
//! interpreted as `tool_calls`, `function_call`, `tool_call` or a single
//! `{"<tool name>": {...}}` object.
//!
//! JSON is printed with sorted keys and two-space indentation, matching the
//! archived `nlohmann::json::dump(2)` output byte for byte.

use std::collections::hash_map::DefaultHasher;
use std::hash::{Hash, Hasher};

use qt_core::message::{ContentBlock, Message, UserContent};
use qt_core::provider::{Request, ToolSpec};
use serde_json::{Map, Value, json};

const CONTROLLER_SYSTEM: &str = "You are a local model controller. You must either answer directly or request a tool call using valid JSON arguments.";

const CONTROLLER_DEVELOPER: &str = "Filesystem operations must be performed with tools. Never invent file listings or file contents. For model discovery, call list_directory first, then classify discovered models. For search, grep, or find-text requests, call search_text. For user text like 'search <term> <path>', call search_text with arguments {\"query\":\"<term>\",\"path\":\"<path>\",\"literal\":true}. For reading a named file, call read_text. For listing a directory, call list_directory. When a tool is needed, return an assistant message with OpenAI-style tool_calls only. Do not include markdown, prose, or role labels around the tool call.";

const FORCE_TOOL_DEVELOPER: &str = "The current user request requires one of the available tools. Return only an OpenAI-style tool_calls JSON object. Do not answer directly before the tool result is available. Include all required tool arguments from the user request.";

const REQUEST_HEADER: &str = "OpenAI-compatible chat completion request JSON:\n";

const FORCED_INSTRUCTION: &str = "This request requires a tool. Return exactly one JSON object shaped like {\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"tool_name\",\"arguments\":\"{}\"}}]} and no prose.\n";

const OPTIONAL_INSTRUCTION: &str = "If calling a tool, return exactly one JSON object shaped like {\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"tool_name\",\"arguments\":\"{}\"}}]}. If no tool is needed, return plain assistant text.\n";

/// Words in user messages that force a tool call when tools are offered.
pub const TOOL_INTENT_TOKENS: [&str; 13] = [
    "file",
    "files",
    "directory",
    "directories",
    "folder",
    "folders",
    "list",
    "read",
    "search",
    "find",
    "grep",
    "model",
    "models",
];

/// Recursively sort object keys (nlohmann's default ordered map).
pub fn sorted_json(value: &Value) -> Value {
    match value {
        Value::Object(map) => {
            let mut keys: Vec<&String> = map.keys().collect();
            keys.sort();
            let mut out = Map::new();
            for key in keys {
                out.insert(key.clone(), sorted_json(&map[key]));
            }
            Value::Object(out)
        }
        Value::Array(items) => Value::Array(items.iter().map(sorted_json).collect()),
        other => other.clone(),
    }
}

/// Compact dump with sorted keys.
pub fn dump(value: &Value) -> String {
    serde_json::to_string(&sorted_json(value)).unwrap_or_default()
}

/// Two-space indented dump with sorted keys.
pub fn dump_pretty(value: &Value) -> String {
    serde_json::to_string_pretty(&sorted_json(value)).unwrap_or_default()
}

fn block_text(block: &ContentBlock) -> String {
    match block {
        ContentBlock::Text { text } => text.clone(),
        ContentBlock::Thinking { thinking, .. } => thinking.clone(),
        ContentBlock::Image { mime_type, .. } => format!("[image:{mime_type}]"),
        ContentBlock::ToolCall {
            id,
            name,
            arguments,
        } => format!(
            "[tool_call name={name} id={id} arguments={}]",
            dump(arguments)
        ),
    }
}

fn blocks_text(blocks: &[ContentBlock]) -> String {
    blocks.iter().map(block_text).collect::<Vec<_>>().join("\n")
}

fn user_blocks(content: &UserContent) -> Vec<ContentBlock> {
    content.blocks()
}

/// One message in the request JSON.
pub fn message_json(message: &Message) -> Value {
    match message {
        Message::User { content, .. } => json!({
            "role": "user",
            "content": blocks_text(&user_blocks(content)),
        }),
        Message::Assistant(a) => json!({
            "role": "assistant",
            "content": blocks_text(&a.content),
        }),
        Message::ToolResult(r) => {
            let mut result = Value::String(r.text());
            if r.is_error {
                result = json!({"result": result, "is_error": true});
            }
            json!({
                "role": "tool",
                "name": r.tool_name,
                "tool_call_id": r.tool_call_id,
                "content": dump(&result),
            })
        }
    }
}

fn tool_json(tool: &ToolSpec) -> Value {
    json!({
        "type": "function",
        "function": {
            "name": tool.name,
            "description": tool.description,
            "parameters": tool.parameters,
        },
    })
}

fn user_intent_requires_tool(content: &UserContent) -> bool {
    let mut text = String::new();
    for block in user_blocks(content) {
        text.push('\n');
        text.push_str(&block_text(&block));
    }
    let normalized = text.to_lowercase();
    TOOL_INTENT_TOKENS.iter().any(|t| normalized.contains(t))
}

/// Whether any user message asks for work that needs a tool.
pub fn requires_tool_choice(request: &Request) -> bool {
    !request.tools.is_empty()
        && request.messages.iter().any(|m| match m {
            Message::User { content, .. } => user_intent_requires_tool(content),
            _ => false,
        })
}

fn request_messages(request: &Request) -> Vec<Value> {
    let mut out = Vec::new();
    if !request.system.is_empty() {
        out.push(json!({"role": "system", "content": request.system}));
    }
    out.extend(request.messages.iter().map(message_json));
    out
}

/// Serialise a request into the archived plain-text prompt.
pub fn serialize_prompt(request: &Request) -> String {
    let mut out = String::new();
    if request.tools.is_empty() {
        let payload = json!({"messages": request_messages(request)});
        out.push_str(REQUEST_HEADER);
        out.push_str(&dump_pretty(&payload));
        out.push_str("\n\nReturn the assistant response text.\n");
    } else {
        let force = requires_tool_choice(request);
        let mut messages = vec![
            json!({"role": "system", "content": CONTROLLER_SYSTEM}),
            json!({"role": "developer", "content": CONTROLLER_DEVELOPER}),
        ];
        if force {
            messages.push(json!({"role": "developer", "content": FORCE_TOOL_DEVELOPER}));
        }
        messages.extend(request_messages(request));
        let payload = json!({
            "messages": messages,
            "tools": request.tools.iter().map(tool_json).collect::<Vec<_>>(),
            "tool_choice": if force { "required" } else { "auto" },
            "parallel_tool_calls": false,
        });
        out.push_str(REQUEST_HEADER);
        out.push_str(&dump_pretty(&payload));
        out.push_str("\n\nReturn the assistant response. ");
        out.push_str(if force {
            FORCED_INSTRUCTION
        } else {
            OPTIONAL_INSTRUCTION
        });
    }
    out.push_str("assistant: ");
    out
}

/// The first balanced, parseable JSON object in `text`.
pub fn parse_first_json_object(text: &str) -> Option<Value> {
    let bytes = text.as_bytes();
    for start in 0..bytes.len() {
        if bytes[start] != b'{' {
            continue;
        }
        let mut depth = 0usize;
        let mut in_string = false;
        let mut escaped = false;
        for index in start..bytes.len() {
            let ch = bytes[index];
            if in_string {
                if escaped {
                    escaped = false;
                } else if ch == b'\\' {
                    escaped = true;
                } else if ch == b'"' {
                    in_string = false;
                }
                continue;
            }
            match ch {
                b'"' => in_string = true,
                b'{' => depth += 1,
                b'}' => {
                    if depth == 0 {
                        break;
                    }
                    depth -= 1;
                    if depth == 0 {
                        match serde_json::from_str::<Value>(&text[start..=index]) {
                            Ok(value) => return Some(value),
                            Err(_) => break,
                        }
                    }
                }
                _ => {}
            }
        }
    }
    None
}

/// A parsed tool call.
#[derive(Debug, Clone, PartialEq)]
pub struct ParsedToolCall {
    pub id: String,
    pub name: String,
    pub arguments: Value,
}

fn parse_arguments(arguments: Option<&Value>) -> Option<Value> {
    let arguments = match arguments {
        None | Some(Value::Null) => return Some(json!({})),
        Some(Value::String(text)) => serde_json::from_str::<Value>(text).ok()?,
        Some(other) => other.clone(),
    };
    arguments.is_object().then_some(arguments)
}

fn fallback_id(raw_text: &str) -> String {
    let mut hasher = DefaultHasher::new();
    raw_text.hash(&mut hasher);
    format!("llama_{}", hasher.finish())
}

fn str_field<'a>(value: &'a Value, key: &str) -> &'a str {
    value.get(key).and_then(Value::as_str).unwrap_or("")
}

fn make_call(
    id: &str,
    name: &str,
    arguments: Option<&Value>,
    raw_text: &str,
) -> Option<ParsedToolCall> {
    if name.is_empty() {
        return None;
    }
    let arguments = parse_arguments(arguments)?;
    Some(ParsedToolCall {
        id: if id.is_empty() {
            fallback_id(raw_text)
        } else {
            id.to_string()
        },
        name: name.to_string(),
        arguments,
    })
}

/// Interpret a parsed JSON object as a tool call.
pub fn tool_call_from_json(
    parsed: &Value,
    tool_names: &[&str],
    raw_text: &str,
) -> Option<ParsedToolCall> {
    let object = parsed.as_object()?;
    if let Some(first) = object
        .get("tool_calls")
        .and_then(Value::as_array)
        .and_then(|calls| calls.first())
        .filter(|first| first.is_object())
    {
        let id = str_field(first, "id");
        if let Some(function) = first.get("function").filter(|f| f.is_object()) {
            return make_call(
                id,
                str_field(function, "name"),
                function.get("arguments"),
                raw_text,
            );
        }
        return make_call(
            id,
            str_field(first, "name"),
            first.get("arguments"),
            raw_text,
        );
    }
    if let Some(call) = object.get("function_call").filter(|f| f.is_object()) {
        return make_call("", str_field(call, "name"), call.get("arguments"), raw_text);
    }
    if let Some(call) = object.get("tool_call").filter(|f| f.is_object()) {
        return make_call(
            str_field(call, "id"),
            str_field(call, "name"),
            call.get("arguments"),
            raw_text,
        );
    }
    if !tool_names.is_empty() && object.len() == 1 {
        let (key, value) = object.iter().next()?;
        if tool_names.contains(&key.as_str()) {
            return make_call("", key, Some(value), raw_text);
        }
    }
    None
}

/// Parse the generated text as a tool call when it contains one.
pub fn parse_tool_call(text: &str, tools: &[ToolSpec]) -> Option<ParsedToolCall> {
    if text.is_empty() {
        return None;
    }
    let parsed = parse_first_json_object(text.trim())?;
    let names: Vec<&str> = tools
        .iter()
        .map(|t| t.name.as_str())
        .filter(|n| !n.is_empty())
        .collect();
    tool_call_from_json(&parsed, &names, text)
}

#[cfg(test)]
mod tests {
    use super::*;
    use qt_core::message::{AssistantMessage, ToolResultMessage};

    fn read_tool() -> ToolSpec {
        ToolSpec {
            name: "read".into(),
            description: "Read a file".into(),
            parameters: json!({"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]}),
        }
    }

    #[test]
    fn plain_prompt_golden() {
        let request = Request {
            system: "Be brief.".into(),
            messages: vec![Message::User {
                content: UserContent::Text("Hello".into()),
                timestamp: None,
            }],
            ..Default::default()
        };
        let expected = "OpenAI-compatible chat completion request JSON:\n{\n  \"messages\": [\n    {\n      \"content\": \"Be brief.\",\n      \"role\": \"system\"\n    },\n    {\n      \"content\": \"Hello\",\n      \"role\": \"user\"\n    }\n  ]\n}\n\nReturn the assistant response text.\nassistant: ";
        assert_eq!(serialize_prompt(&request), expected);
    }

    #[test]
    fn tool_prompt_golden_with_forcing() {
        let request = Request {
            messages: vec![
                Message::User {
                    content: UserContent::Text("Read the file a.txt".into()),
                    timestamp: None,
                },
                Message::Assistant(AssistantMessage {
                    content: vec![
                        ContentBlock::text("ok"),
                        ContentBlock::ToolCall {
                            id: "c1".into(),
                            name: "read".into(),
                            arguments: json!({"path": "a.txt", "limit": 2}),
                        },
                    ],
                    ..Default::default()
                }),
                Message::ToolResult(ToolResultMessage {
                    tool_call_id: "c1".into(),
                    tool_name: "read".into(),
                    content: vec![ContentBlock::text("hi")],
                    ..Default::default()
                }),
            ],
            tools: vec![read_tool()],
            ..Default::default()
        };
        assert!(requires_tool_choice(&request));
        let prompt = serialize_prompt(&request);
        let expected = format!(
            "OpenAI-compatible chat completion request JSON:\n{{\n  \"messages\": [\n    {{\n      \"content\": \"{sys}\",\n      \"role\": \"system\"\n    }},\n    {{\n      \"content\": {dev},\n      \"role\": \"developer\"\n    }},\n    {{\n      \"content\": \"{force}\",\n      \"role\": \"developer\"\n    }},\n    {{\n      \"content\": \"Read the file a.txt\",\n      \"role\": \"user\"\n    }},\n    {{\n      \"content\": \"ok\\n[tool_call name=read id=c1 arguments={{\\\"limit\\\":2,\\\"path\\\":\\\"a.txt\\\"}}]\",\n      \"role\": \"assistant\"\n    }},\n    {{\n      \"content\": \"\\\"hi\\\"\",\n      \"name\": \"read\",\n      \"role\": \"tool\",\n      \"tool_call_id\": \"c1\"\n    }}\n  ],\n  \"parallel_tool_calls\": false,\n  \"tool_choice\": \"required\",\n  \"tools\": [\n    {{\n      \"function\": {{\n        \"description\": \"Read a file\",\n        \"name\": \"read\",\n        \"parameters\": {{\n          \"properties\": {{\n            \"path\": {{\n              \"type\": \"string\"\n            }}\n          }},\n          \"required\": [\n            \"path\"\n          ],\n          \"type\": \"object\"\n        }}\n      }},\n      \"type\": \"function\"\n    }}\n  ]\n}}\n\nReturn the assistant response. {forced}assistant: ",
            sys = CONTROLLER_SYSTEM,
            dev = serde_json::to_string(CONTROLLER_DEVELOPER).unwrap(),
            force = FORCE_TOOL_DEVELOPER,
            forced = FORCED_INSTRUCTION,
        );
        assert_eq!(prompt, expected);
    }

    #[test]
    fn tool_choice_is_auto_without_intent() {
        let request = Request {
            messages: vec![Message::User {
                content: UserContent::Text("Say hello".into()),
                timestamp: None,
            }],
            tools: vec![read_tool()],
            ..Default::default()
        };
        assert!(!requires_tool_choice(&request));
        let prompt = serialize_prompt(&request);
        assert!(prompt.contains("\"tool_choice\": \"auto\""));
        assert!(prompt.ends_with(&format!(
            "Return the assistant response. {OPTIONAL_INSTRUCTION}assistant: "
        )));
        assert!(!prompt.contains(FORCE_TOOL_DEVELOPER));
        let no_tools = Request {
            tools: vec![],
            ..request
        };
        assert!(!requires_tool_choice(&no_tools));
    }

    #[test]
    fn finds_first_json_object() {
        assert_eq!(
            parse_first_json_object("Sure: {\"a\": \"}{\", \"b\": {\"c\": 1}} trailing {\"d\":2}"),
            Some(json!({"a": "}{", "b": {"c": 1}}))
        );
        assert_eq!(
            parse_first_json_object("{bad} {\"ok\": true}"),
            Some(json!({"ok": true}))
        );
        assert_eq!(parse_first_json_object("no json"), None);
        assert_eq!(parse_first_json_object("{\"open\": 1"), None);
        assert_eq!(
            parse_first_json_object("{\"s\": \"\\\"}\"}"),
            Some(json!({"s": "\"}"}))
        );
    }

    #[test]
    fn parses_tool_call_shapes() {
        let tools = vec![read_tool()];
        let call = parse_tool_call(
            r#"{"tool_calls":[{"id":"call_1","type":"function","function":{"name":"read","arguments":"{\"path\":\"a\"}"}}]}"#,
            &tools,
        )
        .unwrap();
        assert_eq!(
            call,
            ParsedToolCall {
                id: "call_1".into(),
                name: "read".into(),
                arguments: json!({"path": "a"})
            }
        );

        let flat = parse_tool_call(
            r#"{"tool_calls":[{"name":"read","arguments":{"path":"b"}}]}"#,
            &tools,
        )
        .unwrap();
        assert_eq!(flat.arguments, json!({"path": "b"}));
        assert!(flat.id.starts_with("llama_"));

        let fc = parse_tool_call(
            r#"x {"function_call":{"name":"read","arguments":null}}"#,
            &tools,
        )
        .unwrap();
        assert_eq!(fc.arguments, json!({}));

        let tc = parse_tool_call(
            r#"{"tool_call":{"id":"t","name":"read","arguments":{"path":"c"}}}"#,
            &tools,
        )
        .unwrap();
        assert_eq!(tc.id, "t");

        let by_name = parse_tool_call(r#"{"read": {"path": "d"}}"#, &tools).unwrap();
        assert_eq!(
            (by_name.name.as_str(), by_name.arguments.clone()),
            ("read", json!({"path": "d"}))
        );

        assert!(parse_tool_call(r#"{"write": {"path": "d"}}"#, &tools).is_none());
        assert!(
            parse_tool_call(
                r#"{"tool_calls":[{"function":{"name":"read","arguments":"[1]"}}]}"#,
                &tools
            )
            .is_none()
        );
        assert!(
            parse_tool_call(
                r#"{"tool_calls":[{"function":{"name":"","arguments":"{}"}}]}"#,
                &tools
            )
            .is_none()
        );
        assert!(parse_tool_call("plain text", &tools).is_none());
        assert!(parse_tool_call("", &tools).is_none());
        // The fallback id is deterministic for the same text.
        let again = parse_tool_call(
            r#"{"tool_calls":[{"name":"read","arguments":{"path":"b"}}]}"#,
            &tools,
        )
        .unwrap();
        assert_eq!(again.id, flat.id);
    }

    #[test]
    fn error_tool_results_are_marked() {
        let message = Message::ToolResult(ToolResultMessage {
            tool_call_id: "x".into(),
            tool_name: "bash".into(),
            content: vec![ContentBlock::text("boom")],
            is_error: true,
            ..Default::default()
        });
        assert_eq!(
            message_json(&message)["content"],
            "{\"is_error\":true,\"result\":\"boom\"}"
        );
        let image = Message::User {
            content: UserContent::Blocks(vec![
                ContentBlock::text("look"),
                ContentBlock::Image {
                    data: "AAAA".into(),
                    mime_type: "image/png".into(),
                },
            ]),
            timestamp: None,
        };
        assert_eq!(message_json(&image)["content"], "look\n[image:image/png]");
    }
}
