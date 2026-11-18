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
}
