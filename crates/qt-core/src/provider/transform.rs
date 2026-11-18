//! Cross-provider history transformation (Pi `transformMessages`).
//!
//! - Images are replaced with a placeholder for models without image input.
//! - Thinking from another model becomes plain text (or is dropped when
//!   empty or redacted); same-model thinking keeps its signature for replay.
//! - Tool-call ids from other models are normalised by the target API's rule
//!   and tool results are rewritten to match.
//! - Errored and aborted assistant turns are skipped, tool calls left without
//!   results get a synthetic error result, and results without a call are
//!   dropped.

use std::collections::{HashMap, HashSet};

use crate::message::{
    AssistantMessage, ContentBlock, Message, StopReason, ToolResultMessage, UserContent, now_millis,
};

pub const NON_VISION_USER_IMAGE_PLACEHOLDER: &str =
    "(image omitted: model does not support images)";
pub const NON_VISION_TOOL_IMAGE_PLACEHOLDER: &str =
    "(tool image omitted: model does not support images)";
pub const MISSING_TOOL_RESULT: &str = "No result provided";

/// The model a history is being prepared for.
#[derive(Debug, Clone, Copy)]
pub struct Target<'a> {
    pub provider: &'a str,
    pub api: &'a str,
    pub model: &'a str,
    pub images: bool,
}

impl Target<'_> {
    pub fn is_same_model(&self, message: &AssistantMessage) -> bool {
        message.provider == self.provider && message.api == self.api && message.model == self.model
    }
}

fn replace_images(blocks: &[ContentBlock], placeholder: &str) -> Vec<ContentBlock> {
    let mut out = Vec::with_capacity(blocks.len());
    let mut previous_was_placeholder = false;
    for block in blocks {
        match block {
            ContentBlock::Image { .. } => {
                if !previous_was_placeholder {
                    out.push(ContentBlock::text(placeholder));
                }
                previous_was_placeholder = true;
            }
            ContentBlock::Text { text } => {
                previous_was_placeholder = text == placeholder;
                out.push(block.clone());
            }
            other => {
                previous_was_placeholder = false;
                out.push(other.clone());
            }
        }
    }
    out
}

type Normalizer<'a> = &'a dyn Fn(&str, &AssistantMessage) -> String;

/// Prepare `messages` for `target`.
pub fn transform_messages(
    messages: &[Message],
    target: &Target<'_>,
    normalize_id: Option<Normalizer<'_>>,
) -> Vec<Message> {
    let mut id_map: HashMap<String, String> = HashMap::new();
    let mut transformed = Vec::with_capacity(messages.len());
    for message in messages {
        match message {
            Message::User { content, timestamp } => {
                let content = match content {
                    UserContent::Blocks(blocks) if !target.images => UserContent::Blocks(
                        replace_images(blocks, NON_VISION_USER_IMAGE_PLACEHOLDER),
                    ),
                    other => other.clone(),
                };
                transformed.push(Message::User {
                    content,
                    timestamp: *timestamp,
                });
            }
            Message::ToolResult(result) => {
                let mut result = result.clone();
                if !target.images {
                    result.content =
                        replace_images(&result.content, NON_VISION_TOOL_IMAGE_PLACEHOLDER);
                }
                if let Some(mapped) = id_map.get(&result.tool_call_id) {
                    result.tool_call_id = mapped.clone();
                }
                transformed.push(Message::ToolResult(result));
            }
            Message::Assistant(assistant) => {
                let same = target.is_same_model(assistant);
                let mut content = Vec::with_capacity(assistant.content.len());
                for block in &assistant.content {
                    match block {
                        ContentBlock::Thinking {
                            thinking,
                            signature,
                            redacted,
                        } => {
                            if *redacted {
                                if same {
                                    content.push(block.clone());
                                }
                            } else if same && signature.as_deref().is_some_and(|s| !s.is_empty()) {
                                content.push(block.clone());
                            } else if thinking.trim().is_empty() {
                            } else if same {
                                content.push(block.clone());
                            } else {
                                content.push(ContentBlock::text(thinking.clone()));
                            }
                        }
                        ContentBlock::ToolCall {
                            id,
                            name,
                            arguments,
                        } if !same => {
                            let new_id = normalize_id
                                .map(|f| f(id, assistant))
                                .unwrap_or_else(|| id.clone());
                            if new_id != *id {
                                id_map.insert(id.clone(), new_id.clone());
                            }
                            content.push(ContentBlock::ToolCall {
                                id: new_id,
                                name: name.clone(),
                                arguments: arguments.clone(),
                            });
                        }
                        other => content.push(other.clone()),
                    }
                }
                transformed.push(Message::Assistant(AssistantMessage {
                    content,
                    ..assistant.clone()
                }));
            }
        }
    }

    let mut out = Vec::with_capacity(transformed.len());
    let mut pending: Vec<(String, String)> = Vec::new();
    let mut answered: HashSet<String> = HashSet::new();
    let flush = |out: &mut Vec<Message>,
                 pending: &mut Vec<(String, String)>,
                 answered: &mut HashSet<String>| {
        for (id, name) in pending.drain(..) {
            if !answered.contains(&id) {
                out.push(Message::ToolResult(ToolResultMessage {
                    tool_call_id: id,
                    tool_name: name,
                    content: vec![ContentBlock::text(MISSING_TOOL_RESULT)],
                    is_error: true,
                    details: None,
                    timestamp: Some(now_millis()),
                }));
            }
        }
        answered.clear();
    };
    for message in transformed {
        match &message {
            Message::Assistant(a) => {
                flush(&mut out, &mut pending, &mut answered);
                if matches!(a.stop_reason, StopReason::Error | StopReason::Aborted) {
                    continue;
                }
                pending = a
                    .content
                    .iter()
                    .filter_map(|b| match b {
                        ContentBlock::ToolCall { id, name, .. } => Some((id.clone(), name.clone())),
                        _ => None,
                    })
                    .collect();
                out.push(message);
            }
            Message::ToolResult(r) => {
                if pending.iter().any(|(id, _)| *id == r.tool_call_id)
                    && answered.insert(r.tool_call_id.clone())
                {
                    out.push(message);
                }
            }
            Message::User { .. } => {
                flush(&mut out, &mut pending, &mut answered);
                out.push(message);
            }
        }
    }
    flush(&mut out, &mut pending, &mut answered);
    out
}

/// Replace characters outside `[a-zA-Z0-9_-]` with `_`.
pub fn sanitize_id(id: &str) -> String {
    id.chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() || c == '_' || c == '-' {
                c
            } else {
                '_'
            }
        })
        .collect()
}

/// Pi `shortHash`: two 32-bit FNV-style mixes rendered in base 36.
pub fn short_hash(value: &str) -> String {
    let mut h1: u32 = 0xdead_beef;
    let mut h2: u32 = 0x41c6_ce57;
    for unit in value.encode_utf16() {
        let ch = unit as u32;
        h1 = (h1 ^ ch).wrapping_mul(2_654_435_761);
        h2 = (h2 ^ ch).wrapping_mul(1_597_334_677);
    }
    h1 = (h1 ^ (h1 >> 16)).wrapping_mul(2_246_822_507)
        ^ (h2 ^ (h2 >> 13)).wrapping_mul(3_266_489_909);
    h2 = (h2 ^ (h2 >> 16)).wrapping_mul(2_246_822_507)
        ^ (h1 ^ (h1 >> 13)).wrapping_mul(3_266_489_909);
    format!("{}{}", base36(h2), base36(h1))
}

fn base36(mut value: u32) -> String {
    if value == 0 {
        return "0".into();
    }
    let digits = b"0123456789abcdefghijklmnopqrstuvwxyz";
    let mut out = Vec::new();
    while value > 0 {
        out.push(digits[(value % 36) as usize]);
        value /= 36;
    }
    out.reverse();
    String::from_utf8(out).expect("ascii")
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn assistant(
        provider: &str,
        model: &str,
        content: Vec<ContentBlock>,
        stop: StopReason,
    ) -> Message {
        Message::Assistant(AssistantMessage {
            content,
            api: "openai-responses".into(),
            provider: provider.into(),
            model: model.into(),
            stop_reason: stop,
            ..Default::default()
        })
    }

    fn result(id: &str) -> Message {
        Message::ToolResult(ToolResultMessage {
            tool_call_id: id.into(),
            tool_name: "read".into(),
            content: vec![
                ContentBlock::text("ok"),
                ContentBlock::Image {
                    data: "AAAA".into(),
                    mime_type: "image/png".into(),
                },
            ],
            ..Default::default()
        })
    }

    #[test]
    fn converts_foreign_thinking_and_normalises_ids() {
        let history = vec![
            Message::user_text("go"),
            assistant(
                "openai",
                "gpt-5",
                vec![
                    ContentBlock::Thinking {
                        thinking: "plan".into(),
                        signature: Some("{\"id\":\"rs_1\"}".into()),
                        redacted: false,
                    },
                    ContentBlock::Thinking {
                        thinking: String::new(),
                        signature: Some("secret".into()),
                        redacted: true,
                    },
                    ContentBlock::ToolCall {
                        id: "call|fc+1".into(),
                        name: "read".into(),
                        arguments: json!({}),
                    },
                ],
                StopReason::ToolCall,
            ),
            result("call|fc+1"),
        ];
        let target = Target {
            provider: "anthropic",
            api: "anthropic-messages",
            model: "claude",
            images: false,
        };
        let normalize = |id: &str, _: &AssistantMessage| sanitize_id(id);
        let out = transform_messages(&history, &target, Some(&normalize));
        let Message::Assistant(a) = &out[1] else {
            panic!()
        };
        assert_eq!(a.content[0], ContentBlock::text("plan"));
        assert_eq!(a.content.len(), 2);
        assert!(matches!(&a.content[1], ContentBlock::ToolCall { id, .. } if id == "call_fc_1"));
        let Message::ToolResult(r) = &out[2] else {
            panic!()
        };
        assert_eq!(r.tool_call_id, "call_fc_1");
        assert_eq!(
            r.content[1],
            ContentBlock::text(NON_VISION_TOOL_IMAGE_PLACEHOLDER)
        );
    }

    #[test]
    fn skips_failed_turns_and_synthesises_missing_results() {
        let call = |id: &str| ContentBlock::ToolCall {
            id: id.into(),
            name: "read".into(),
            arguments: json!({}),
        };
        let history = vec![
            Message::user_text("a"),
            assistant("p", "m", vec![call("x")], StopReason::Error),
            result("x"),
            assistant("p", "m", vec![call("y"), call("z")], StopReason::ToolCall),
            result("y"),
            Message::user_text("b"),
        ];
        let target = Target {
            provider: "p",
            api: "openai-responses",
            model: "m",
            images: true,
        };
        let out = transform_messages(&history, &target, None);
        let roles: Vec<&str> = out
            .iter()
            .map(|m| match m {
                Message::User { .. } => "user",
                Message::Assistant(_) => "assistant",
                Message::ToolResult(_) => "tool",
            })
            .collect();
        assert_eq!(roles, ["user", "assistant", "tool", "tool", "user"]);
        let Message::ToolResult(synthetic) = &out[3] else {
            panic!()
        };
        assert_eq!(synthetic.tool_call_id, "z");
        assert!(synthetic.is_error);
        assert_eq!(
            synthetic.content,
            vec![ContentBlock::text(MISSING_TOOL_RESULT)]
        );
    }

    #[test]
    fn short_hash_matches_pi() {
        // Golden values computed with Pi's shortHash (utils/hash.ts) under Node.
        assert_eq!(short_hash(""), "k4n83c7h0j2b");
        assert_eq!(short_hash("abc"), "y0biex7f9bbh");
        assert_eq!(short_hash("rs_123|x"), "1qy20rd105vc1d");
    }
}
