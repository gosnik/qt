//! Conversation message model.
//!
//! The serialized shape follows the Pi session format (`role` tagged messages
//! with `text` / `thinking` / `toolCall` / `image` content blocks) so session
//! files written by the archived C++ qt and by Pi can be resumed.

use serde::{Deserialize, Serialize};
use serde_json::Value;

/// One block of message content.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "type")]
pub enum ContentBlock {
    #[serde(rename = "text")]
    Text { text: String },
    #[serde(rename = "thinking")]
    Thinking {
        thinking: String,
        #[serde(
            rename = "thinkingSignature",
            default,
            skip_serializing_if = "Option::is_none"
        )]
        signature: Option<String>,
        #[serde(default, skip_serializing_if = "std::ops::Not::not")]
        redacted: bool,
    },
    #[serde(rename = "toolCall")]
    ToolCall {
        id: String,
        name: String,
        #[serde(default)]
        arguments: Value,
    },
    #[serde(rename = "image")]
    Image {
        data: String,
        #[serde(rename = "mimeType")]
        mime_type: String,
    },
}

impl ContentBlock {
    pub fn text(text: impl Into<String>) -> Self {
        Self::Text { text: text.into() }
    }
}

/// User content is either a bare string or a list of blocks.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(untagged)]
pub enum UserContent {
    Text(String),
    Blocks(Vec<ContentBlock>),
}

impl UserContent {
    pub fn blocks(&self) -> Vec<ContentBlock> {
        match self {
            Self::Text(text) => vec![ContentBlock::text(text.clone())],
            Self::Blocks(blocks) => blocks.clone(),
        }
    }

    /// Concatenated text of all text blocks.
    pub fn plain_text(&self) -> String {
        match self {
            Self::Text(text) => text.clone(),
            Self::Blocks(blocks) => join_text(blocks),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
pub enum StopReason {
    #[default]
    #[serde(rename = "stop")]
    Stop,
    #[serde(rename = "length")]
    Length,
    #[serde(rename = "tool_call", alias = "toolUse")]
    ToolCall,
    #[serde(rename = "error")]
    Error,
    #[serde(rename = "aborted")]
    Aborted,
}

/// Token usage and cost for one assistant response.
///
/// Written with the snake_case field names used by the archived C++ qt and
/// read with Pi's camelCase names as aliases.
#[derive(Debug, Clone, Copy, PartialEq, Default, Serialize, Deserialize)]
#[serde(default)]
pub struct Usage {
    #[serde(alias = "input")]
    pub input_tokens: u64,
    #[serde(alias = "output")]
    pub output_tokens: u64,
    #[serde(alias = "cacheRead")]
    pub cache_read_tokens: u64,
    #[serde(alias = "cacheWrite")]
    pub cache_write_tokens: u64,
    #[serde(alias = "totalTokens")]
    pub total_tokens: u64,
    pub total_cost_usd: f64,
}

impl Usage {
    pub fn add(&mut self, other: &Usage) {
        self.input_tokens += other.input_tokens;
        self.output_tokens += other.output_tokens;
        self.cache_read_tokens += other.cache_read_tokens;
        self.cache_write_tokens += other.cache_write_tokens;
        self.total_tokens += other.total_tokens;
        self.total_cost_usd += other.total_cost_usd;
    }

    /// Tokens occupying the context window after this response.
    pub fn context_tokens(&self) -> u64 {
        if self.total_tokens > 0 {
            self.total_tokens
        } else {
            self.input_tokens
                + self.output_tokens
                + self.cache_read_tokens
                + self.cache_write_tokens
        }
    }
}

#[derive(Debug, Clone, PartialEq, Default, Serialize, Deserialize)]
pub struct AssistantMessage {
    #[serde(default)]
    pub content: Vec<ContentBlock>,
    #[serde(default)]
    pub api: String,
    #[serde(default)]
    pub provider: String,
    #[serde(default)]
    pub model: String,
    #[serde(default)]
    pub usage: Usage,
    #[serde(rename = "stopReason", default)]
    pub stop_reason: StopReason,
    #[serde(
        rename = "errorMessage",
        default,
        skip_serializing_if = "Option::is_none"
    )]
    pub error_message: Option<String>,
    #[serde(
        rename = "responseId",
        default,
        skip_serializing_if = "Option::is_none"
    )]
    pub response_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub timestamp: Option<i64>,
}

impl AssistantMessage {
    pub fn text(&self) -> String {
        join_text(&self.content)
    }

    pub fn tool_calls(&self) -> impl Iterator<Item = (&str, &str, &Value)> {
        self.content.iter().filter_map(|block| match block {
            ContentBlock::ToolCall {
                id,
                name,
                arguments,
            } => Some((id.as_str(), name.as_str(), arguments)),
            _ => None,
        })
    }
}

#[derive(Debug, Clone, PartialEq, Default, Serialize, Deserialize)]
pub struct ToolResultMessage {
    #[serde(rename = "toolCallId")]
    pub tool_call_id: String,
    #[serde(rename = "toolName", default)]
    pub tool_name: String,
    #[serde(default)]
    pub content: Vec<ContentBlock>,
    #[serde(rename = "isError", default)]
    pub is_error: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub details: Option<Value>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub timestamp: Option<i64>,
}

impl ToolResultMessage {
    pub fn text(&self) -> String {
        join_text(&self.content)
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "role")]
pub enum Message {
    #[serde(rename = "user")]
    User {
        content: UserContent,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        timestamp: Option<i64>,
    },
    #[serde(rename = "assistant")]
    Assistant(AssistantMessage),
    #[serde(rename = "toolResult")]
    ToolResult(ToolResultMessage),
}

impl Message {
    pub fn user_text(text: impl Into<String>) -> Self {
        Self::User {
            content: UserContent::Text(text.into()),
            timestamp: Some(now_millis()),
        }
    }
}

/// Prefix and suffix wrapped around a compaction summary when it is replayed
/// to the model (Pi `COMPACTION_SUMMARY_PREFIX` / `_SUFFIX`).
pub const COMPACTION_SUMMARY_PREFIX: &str = "The conversation history before this point was compacted into the following summary:\n\n<summary>\n";
pub const COMPACTION_SUMMARY_SUFFIX: &str = "\n</summary>";
/// Prefix and suffix wrapped around a branch summary (Pi `BRANCH_SUMMARY_PREFIX` / `_SUFFIX`).
pub const BRANCH_SUMMARY_PREFIX: &str =
    "The following is a summary of a branch that this conversation came back from:\n\n<summary>\n";
pub const BRANCH_SUMMARY_SUFFIX: &str = "</summary>";

/// A message injected by an extension or the harness (Pi role `custom`).
/// It reaches the model as a user message.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct CustomMessage {
    #[serde(rename = "customType")]
    pub custom_type: String,
    pub content: UserContent,
    #[serde(default)]
    pub display: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub details: Option<Value>,
    #[serde(default)]
    pub timestamp: i64,
}

impl CustomMessage {
    pub fn new(custom_type: impl Into<String>, content: UserContent, display: bool) -> Self {
        Self {
            custom_type: custom_type.into(),
            content,
            display,
            details: None,
            timestamp: now_millis(),
        }
    }
}

/// A user-run shell command (`!cmd`, Pi role `bashExecution`).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct BashExecutionMessage {
    pub command: String,
    #[serde(default)]
    pub output: String,
    #[serde(rename = "exitCode", default)]
    pub exit_code: Option<i64>,
    #[serde(default)]
    pub cancelled: bool,
    #[serde(default)]
    pub truncated: bool,
    #[serde(
        rename = "fullOutputPath",
        default,
        skip_serializing_if = "Option::is_none"
    )]
    pub full_output_path: Option<String>,
    #[serde(default)]
    pub timestamp: i64,
    /// `!!cmd`: recorded in the session but never sent to the model.
    #[serde(
        rename = "excludeFromContext",
        default,
        skip_serializing_if = "std::ops::Not::not"
    )]
    pub exclude_from_context: bool,
}

impl BashExecutionMessage {
    /// The user-message text the model sees (Pi `bashExecutionToText`).
    pub fn to_text(&self) -> String {
        let mut text = format!("Ran `{}`\n", self.command);
        if self.output.is_empty() {
            text.push_str("(no output)");
        } else {
            text.push_str(&format!("```\n{}\n```", self.output));
        }
        if self.cancelled {
            text.push_str("\n\n(command cancelled)");
        } else if let Some(code) = self.exit_code.filter(|c| *c != 0) {
            text.push_str(&format!("\n\nCommand exited with code {code}"));
        }
        if self.truncated
            && let Some(path) = &self.full_output_path
        {
            text.push_str(&format!("\n\n[Output truncated. Full output: {path}]"));
        }
        text
    }
}

/// Replay of a `branch_summary` session entry.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct BranchSummaryMessage {
    pub summary: String,
    #[serde(rename = "fromId")]
    pub from_id: String,
    #[serde(default)]
    pub timestamp: i64,
}

/// Replay of a `compaction` session entry.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct CompactionSummaryMessage {
    pub summary: String,
    #[serde(rename = "tokensBefore", default)]
    pub tokens_before: u64,
    #[serde(default)]
    pub timestamp: i64,
}

/// Any message held in the agent context: the three LLM roles plus the
/// harness roles that are converted to user messages at the provider boundary.
#[derive(Debug, Clone, PartialEq)]
pub enum AgentMessage {
    Llm(Message),
    Custom(CustomMessage),
    BashExecution(BashExecutionMessage),
    BranchSummary(BranchSummaryMessage),
    CompactionSummary(CompactionSummaryMessage),
}

#[derive(Serialize, Deserialize)]
#[serde(tag = "role")]
enum HarnessRole {
    #[serde(rename = "custom", alias = "hookMessage")]
    Custom(CustomMessage),
    #[serde(rename = "bashExecution")]
    BashExecution(BashExecutionMessage),
    #[serde(rename = "branchSummary")]
    BranchSummary(BranchSummaryMessage),
    #[serde(rename = "compactionSummary")]
    CompactionSummary(CompactionSummaryMessage),
}

impl Serialize for AgentMessage {
    fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        match self {
            Self::Llm(m) => m.serialize(serializer),
            Self::Custom(m) => HarnessRole::Custom(m.clone()).serialize(serializer),
            Self::BashExecution(m) => HarnessRole::BashExecution(m.clone()).serialize(serializer),
            Self::BranchSummary(m) => HarnessRole::BranchSummary(m.clone()).serialize(serializer),
            Self::CompactionSummary(m) => {
                HarnessRole::CompactionSummary(m.clone()).serialize(serializer)
            }
        }
    }
}

impl<'de> Deserialize<'de> for AgentMessage {
    fn deserialize<D: serde::Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        use serde::de::Error as _;
        let value = Value::deserialize(deserializer)?;
        let role = value.get("role").and_then(Value::as_str).unwrap_or("");
        match role {
            "user" | "assistant" | "toolResult" => serde_json::from_value::<Message>(value)
                .map(Self::Llm)
                .map_err(D::Error::custom),
            _ => match serde_json::from_value::<HarnessRole>(value).map_err(D::Error::custom)? {
                HarnessRole::Custom(m) => Ok(Self::Custom(m)),
                HarnessRole::BashExecution(m) => Ok(Self::BashExecution(m)),
                HarnessRole::BranchSummary(m) => Ok(Self::BranchSummary(m)),
                HarnessRole::CompactionSummary(m) => Ok(Self::CompactionSummary(m)),
            },
        }
    }
}

impl From<Message> for AgentMessage {
    fn from(message: Message) -> Self {
        Self::Llm(message)
    }
}

impl AgentMessage {
    /// Pi role name.
    pub fn role(&self) -> &'static str {
        match self {
            Self::Llm(Message::User { .. }) => "user",
            Self::Llm(Message::Assistant(_)) => "assistant",
            Self::Llm(Message::ToolResult(_)) => "toolResult",
            Self::Custom(_) => "custom",
            Self::BashExecution(_) => "bashExecution",
            Self::BranchSummary(_) => "branchSummary",
            Self::CompactionSummary(_) => "compactionSummary",
        }
    }

    pub fn as_llm(&self) -> Option<&Message> {
        match self {
            Self::Llm(m) => Some(m),
            _ => None,
        }
    }

    pub fn as_assistant(&self) -> Option<&AssistantMessage> {
        match self {
            Self::Llm(Message::Assistant(a)) => Some(a),
            _ => None,
        }
    }

    pub fn is_user(&self) -> bool {
        matches!(self, Self::Llm(Message::User { .. }))
    }

    /// Millisecond timestamp, when known.
    pub fn timestamp(&self) -> Option<i64> {
        match self {
            Self::Llm(Message::User { timestamp, .. }) => *timestamp,
            Self::Llm(Message::Assistant(a)) => a.timestamp,
            Self::Llm(Message::ToolResult(r)) => r.timestamp,
            Self::Custom(m) => Some(m.timestamp),
            Self::BashExecution(m) => Some(m.timestamp),
            Self::BranchSummary(m) => Some(m.timestamp),
            Self::CompactionSummary(m) => Some(m.timestamp),
        }
    }

    /// Plain text of a user-like message (user, custom), used for queue matching.
    pub fn user_text(&self) -> Option<String> {
        match self {
            Self::Llm(Message::User { content, .. }) => Some(content.plain_text()),
            Self::Custom(m) => Some(m.content.plain_text()),
            _ => None,
        }
    }

    /// The message as the model sees it (Pi `convertToLlm` for one message).
    pub fn to_llm(&self) -> Option<Message> {
        let user = |text: String, timestamp: i64| Message::User {
            content: UserContent::Blocks(vec![ContentBlock::text(text)]),
            timestamp: Some(timestamp),
        };
        match self {
            Self::Llm(m) => Some(m.clone()),
            Self::Custom(m) => Some(Message::User {
                content: UserContent::Blocks(m.content.blocks()),
                timestamp: Some(m.timestamp),
            }),
            Self::BashExecution(m) if m.exclude_from_context => None,
            Self::BashExecution(m) => Some(user(m.to_text(), m.timestamp)),
            Self::BranchSummary(m) => Some(user(
                format!(
                    "{BRANCH_SUMMARY_PREFIX}{}{BRANCH_SUMMARY_SUFFIX}",
                    m.summary
                ),
                m.timestamp,
            )),
            Self::CompactionSummary(m) => Some(user(
                format!(
                    "{COMPACTION_SUMMARY_PREFIX}{}{COMPACTION_SUMMARY_SUFFIX}",
                    m.summary
                ),
                m.timestamp,
            )),
        }
    }
}

/// Convert an agent context to provider messages (Pi `convertToLlm`).
pub fn convert_to_llm(messages: &[AgentMessage]) -> Vec<Message> {
    messages.iter().filter_map(AgentMessage::to_llm).collect()
}

pub fn join_text(blocks: &[ContentBlock]) -> String {
    let mut out = String::new();
    for block in blocks {
        if let ContentBlock::Text { text } = block {
            if !out.is_empty() && !out.ends_with('\n') {
                out.push('\n');
            }
            out.push_str(text);
        }
    }
    out
}

pub fn now_millis() -> i64 {
    chrono::Utc::now().timestamp_millis()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_pi_and_qt_shapes() {
        let user: Message = serde_json::from_str(r#"{"role":"user","content":"hi"}"#).unwrap();
        assert!(
            matches!(user, Message::User { content: UserContent::Text(ref t), .. } if t == "hi")
        );

        let assistant: Message = serde_json::from_str(
            r#"{"role":"assistant","content":[{"type":"thinking","thinking":"hm","thinkingSignature":"sig"},
                {"type":"toolCall","id":"c1","name":"read","arguments":{"path":"a"}}],
                "api":"openai-completions","provider":"openrouter","model":"m",
                "usage":{"input":10,"output":5,"cacheRead":1,"cacheWrite":0,"totalTokens":16},
                "stopReason":"toolUse"}"#,
        )
        .unwrap();
        let Message::Assistant(a) = assistant else {
            panic!()
        };
        assert_eq!(a.stop_reason, StopReason::ToolCall);
        assert_eq!(a.usage.input_tokens, 10);
        assert_eq!(a.tool_calls().count(), 1);

        let result: Message = serde_json::from_str(
            r#"{"role":"toolResult","toolCallId":"c1","toolName":"read","content":[{"type":"text","text":"x"}],"isError":false}"#,
        )
        .unwrap();
        let round = serde_json::to_string(&result).unwrap();
        assert!(round.contains(r#""toolCallId":"c1""#));
    }

    #[test]
    fn harness_roles_round_trip_and_convert() {
        let legacy: AgentMessage = serde_json::from_str(
            r#"{"role":"hookMessage","customType":"x","content":"injected","display":true,"timestamp":5}"#,
        )
        .unwrap();
        assert_eq!(legacy.role(), "custom");
        let json = serde_json::to_value(&legacy).unwrap();
        assert_eq!(json["role"], "custom");
        assert_eq!(json["customType"], "x");

        let bash: AgentMessage = serde_json::from_str(
            r#"{"role":"bashExecution","command":"ls","output":"a","exitCode":2,"cancelled":false,"truncated":true,"fullOutputPath":"/tmp/o","timestamp":1}"#,
        )
        .unwrap();
        let Some(Message::User { content, .. }) = bash.to_llm() else {
            panic!()
        };
        assert_eq!(
            content.plain_text(),
            "Ran `ls`\n```\na\n```\n\nCommand exited with code 2\n\n[Output truncated. Full output: /tmp/o]"
        );
        let hidden = AgentMessage::BashExecution(BashExecutionMessage {
            command: "x".into(),
            output: String::new(),
            exit_code: None,
            cancelled: true,
            truncated: false,
            full_output_path: None,
            timestamp: 0,
            exclude_from_context: true,
        });
        assert!(hidden.to_llm().is_none());

        let compaction = AgentMessage::CompactionSummary(CompactionSummaryMessage {
            summary: "S".into(),
            tokens_before: 10,
            timestamp: 0,
        });
        let text = match compaction.to_llm() {
            Some(Message::User { content, .. }) => content.plain_text(),
            _ => panic!(),
        };
        assert_eq!(
            text,
            "The conversation history before this point was compacted into the following summary:\n\n<summary>\nS\n</summary>"
        );
        let branch = AgentMessage::BranchSummary(BranchSummaryMessage {
            summary: "B".into(),
            from_id: "root".into(),
            timestamp: 0,
        });
        let back: AgentMessage =
            serde_json::from_value(serde_json::to_value(&branch).unwrap()).unwrap();
        assert_eq!(back, branch);
        assert_eq!(convert_to_llm(&[branch, hidden]).len(), 1);
    }
}
