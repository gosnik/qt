//! Pi JSON event lines (`--mode json`, RPC events, subagent children).
//!
//! [`PiEventSerializer`] turns the [`AgentEvent`] stream of a conversation
//! into Pi's `AgentSessionEvent` objects (`reference/pi/packages/agent`
//! `AgentEvent`, coding-agent `AgentSessionEvent`): `agent_start`,
//! `turn_start`, `message_start` / `message_update` (with the
//! `assistantMessageEvent` and the partial message) / `message_end`,
//! `tool_execution_start` / `_update` / `_end`, `turn_end`, `agent_end`,
//! `queue_update`, `compaction_start` / `_end` and `auto_retry_start` /
//! `_end`. Messages use Pi's shapes (camelCase usage with a `cost` object,
//! `stopReason: "toolUse"`). The session header line is
//! [`session_header_json`].

use serde_json::{Map, Value, json};

use crate::agent::AgentEvent;
use crate::harness::compaction::CompactionResult;
use crate::message::{AgentMessage, AssistantMessage, ContentBlock, Message, StopReason, Usage};
use crate::provider::StreamEvent;
use crate::session::SessionHeader;

/// The session header line Pi prints first in JSON mode.
pub fn session_header_json(header: &SessionHeader) -> Value {
    header.to_value()
}

fn usage_json(usage: &Usage) -> Value {
    json!({
        "input": usage.input_tokens,
        "output": usage.output_tokens,
        "cacheRead": usage.cache_read_tokens,
        "cacheWrite": usage.cache_write_tokens,
        "totalTokens": if usage.total_tokens > 0 {
            usage.total_tokens
        } else {
            usage.input_tokens + usage.output_tokens + usage.cache_read_tokens + usage.cache_write_tokens
        },
        "cost": {
            "input": 0.0,
            "output": 0.0,
            "cacheRead": 0.0,
            "cacheWrite": 0.0,
            "total": usage.total_cost_usd,
        },
    })
}

fn stop_reason_json(stop: StopReason) -> &'static str {
    match stop {
        StopReason::Stop => "stop",
        StopReason::Length => "length",
        StopReason::ToolCall => "toolUse",
        StopReason::Error => "error",
        StopReason::Aborted => "aborted",
    }
}

/// An assistant message in Pi's JSON shape.
pub fn assistant_json(message: &AssistantMessage) -> Value {
    let mut v = json!({
        "role": "assistant",
        "content": message.content,
        "api": message.api,
        "provider": message.provider,
        "model": message.model,
        "usage": usage_json(&message.usage),
        "stopReason": stop_reason_json(message.stop_reason),
        "timestamp": message.timestamp.unwrap_or_else(crate::message::now_millis),
    });
    if let Some(error) = &message.error_message {
        v["errorMessage"] = json!(error);
    }
    if let Some(id) = &message.response_id {
        v["responseId"] = json!(id);
    }
    v
}

/// Any agent message in Pi's JSON shape.
pub fn message_json(message: &AgentMessage) -> Value {
    match message {
        AgentMessage::Llm(Message::Assistant(a)) => assistant_json(a),
        other => serde_json::to_value(other).unwrap_or(Value::Null),
    }
}

fn compaction_result_json(result: &CompactionResult) -> Value {
    let mut v = json!({
        "summary": result.summary,
        "firstKeptEntryId": result.first_kept_entry_id,
        "tokensBefore": result.tokens_before,
    });
    if let Some(details) = &result.details {
        v["details"] = details.clone();
    }
    v
}

fn tool_call_json(block: &ContentBlock) -> Value {
    match block {
        ContentBlock::ToolCall {
            id,
            name,
            arguments,
        } => json!({"type": "toolCall", "id": id, "name": name, "arguments": arguments}),
        _ => Value::Null,
    }
}

/// Stateful converter from [`AgentEvent`]s to Pi JSON event objects.
#[derive(Debug, Default)]
pub struct PiEventSerializer {
    api: String,
    provider: String,
    model: String,
    /// The streaming assistant message, when one is open.
    partial: Option<AssistantMessage>,
    /// Content index of the block that has not been closed yet.
    open_block: Option<usize>,
    /// Raw tool argument text per content index.
    tool_args: Map<String, Value>,
    /// `(name, args)` per tool call id, for tool execution events.
    tools: std::collections::HashMap<String, (String, Value)>,
    /// Messages of the current run, for `agent_end`.
    run_messages: Vec<Value>,
}

impl PiEventSerializer {
    pub fn new() -> Self {
        Self::default()
    }

    /// Provider, model and API recorded on streaming partial messages.
    pub fn set_model(&mut self, provider: &str, model: &str, api: &str) {
        self.provider = provider.into();
        self.model = model.into();
        self.api = api.into();
    }

    fn partial_json(&self) -> Value {
        match &self.partial {
            Some(p) => assistant_json(p),
            None => Value::Null,
        }
    }

    fn update(&self, mut event: Value) -> Value {
        let partial = self.partial_json();
        event["partial"] = partial.clone();
        json!({
            "type": "message_update",
            "message": partial,
            "assistantMessageEvent": event,
        })
    }

    fn start_partial(&mut self) -> Value {
        self.partial = Some(AssistantMessage {
            api: self.api.clone(),
            provider: self.provider.clone(),
            model: self.model.clone(),
            timestamp: Some(crate::message::now_millis()),
            ..Default::default()
        });
        self.open_block = None;
        self.tool_args = Map::new();
        json!({"type": "message_start", "message": self.partial_json()})
    }

    /// Close the open block with its `*_end` event.
    fn close_block(&mut self, out: &mut Vec<Value>) {
        let Some(index) = self.open_block.take() else {
            return;
        };
        let Some(block) = self
            .partial
            .as_ref()
            .and_then(|p| p.content.get(index))
            .cloned()
        else {
            return;
        };
        let event = match &block {
            ContentBlock::Text { text } => {
                json!({"type": "text_end", "contentIndex": index, "content": text})
            }
            ContentBlock::Thinking { thinking, .. } => {
                json!({"type": "thinking_end", "contentIndex": index, "content": thinking})
            }
            ContentBlock::ToolCall { .. } => {
                json!({"type": "toolcall_end", "contentIndex": index, "toolCall": tool_call_json(&block)})
            }
            ContentBlock::Image { .. } => return,
        };
        out.push(self.update(event));
    }

    fn message_pair(&mut self, message: Value) -> Vec<Value> {
        self.run_messages.push(message.clone());
        vec![
            json!({"type": "message_start", "message": message}),
            json!({"type": "message_end", "message": message}),
        ]
    }

    fn stream(&mut self, event: &StreamEvent) -> Vec<Value> {
        let mut out = Vec::new();
        if self.partial.is_none() {
            out.push(self.start_partial());
        }
        match event {
            StreamEvent::BlockStart { index, block } => {
                self.close_block(&mut out);
                let mut block = block.clone();
                if let ContentBlock::ToolCall { arguments, .. } = &mut block {
                    *arguments = json!({});
                }
                if let Some(p) = self.partial.as_mut() {
                    while p.content.len() < *index {
                        p.content.push(ContentBlock::text(""));
                    }
                    if p.content.len() == *index {
                        p.content.push(block.clone());
                    } else {
                        p.content[*index] = block.clone();
                    }
                }
                self.open_block = Some(*index);
                let kind = match block {
                    ContentBlock::Text { .. } => "text_start",
                    ContentBlock::Thinking { .. } => "thinking_start",
                    ContentBlock::ToolCall { .. } => "toolcall_start",
                    ContentBlock::Image { .. } => return out,
                };
                out.push(self.update(json!({"type": kind, "contentIndex": index})));
            }
            StreamEvent::TextDelta { index, delta } => {
                if let Some(ContentBlock::Text { text }) = self
                    .partial
                    .as_mut()
                    .and_then(|p| p.content.get_mut(*index))
                {
                    text.push_str(delta);
                }
                out.push(
                    self.update(
                        json!({"type": "text_delta", "contentIndex": index, "delta": delta}),
                    ),
                );
            }
            StreamEvent::ThinkingDelta { index, delta } => {
                if let Some(ContentBlock::Thinking { thinking, .. }) = self
                    .partial
                    .as_mut()
                    .and_then(|p| p.content.get_mut(*index))
                {
                    thinking.push_str(delta);
                }
                out.push(self.update(
                    json!({"type": "thinking_delta", "contentIndex": index, "delta": delta}),
                ));
            }
            StreamEvent::ToolArgsDelta { index, delta } => {
                let key = index.to_string();
                let raw = format!(
                    "{}{delta}",
                    self.tool_args
                        .get(&key)
                        .and_then(Value::as_str)
                        .unwrap_or_default()
                );
                if let Some(ContentBlock::ToolCall { arguments, .. }) = self
                    .partial
                    .as_mut()
                    .and_then(|p| p.content.get_mut(*index))
                    && let Ok(parsed) = serde_json::from_str::<Value>(&raw)
                {
                    *arguments = parsed;
                }
                self.tool_args.insert(key, Value::String(raw));
                out.push(self.update(
                    json!({"type": "toolcall_delta", "contentIndex": index, "delta": delta}),
                ));
            }
        }
        out
    }

    /// Pi JSON objects for one event (possibly none).
    pub fn serialize(&mut self, event: &AgentEvent) -> Vec<Value> {
        match event {
            AgentEvent::AgentStart => {
                self.run_messages.clear();
                vec![json!({"type": "agent_start"})]
            }
            AgentEvent::TurnStart => vec![json!({"type": "turn_start"})],
            AgentEvent::AssistantStart => vec![self.start_partial()],
            AgentEvent::Stream(stream) => self.stream(stream),
            AgentEvent::MessageEnd(Message::Assistant(a)) => {
                let mut out = Vec::new();
                if self.partial.is_none() {
                    out.push(self.start_partial());
                }
                if let Some(p) = self.partial.as_mut() {
                    // The final content is authoritative for the closing event.
                    p.content = a.content.clone();
                }
                self.close_block(&mut out);
                self.partial = None;
                let message = assistant_json(a);
                self.run_messages.push(message.clone());
                out.push(json!({"type": "message_end", "message": message}));
                out
            }
            AgentEvent::MessageEnd(Message::ToolResult(r)) => {
                let mut out = vec![json!({
                    "type": "tool_execution_end",
                    "toolCallId": r.tool_call_id,
                    "toolName": r.tool_name,
                    "result": {
                        "content": r.content,
                        "details": r.details.clone().unwrap_or_else(|| json!({})),
                    },
                    "isError": r.is_error,
                })];
                self.tools.remove(&r.tool_call_id);
                out.extend(self.message_pair(message_json(&AgentMessage::Llm(
                    Message::ToolResult(r.clone()),
                ))));
                out
            }
            AgentEvent::MessageEnd(m) => {
                self.message_pair(message_json(&AgentMessage::Llm(m.clone())))
            }
            AgentEvent::CustomMessageEnd(m) => self.message_pair(message_json(m)),
            AgentEvent::ToolStart(exec) => {
                self.tools
                    .insert(exec.id.clone(), (exec.name.clone(), exec.args.clone()));
                vec![json!({
                    "type": "tool_execution_start",
                    "toolCallId": exec.id,
                    "toolName": exec.name,
                    "args": exec.args,
                })]
            }
            AgentEvent::ToolProgress { id, output } => {
                let (name, args) = self
                    .tools
                    .get(id)
                    .cloned()
                    .unwrap_or_else(|| (String::new(), json!({})));
                vec![json!({
                    "type": "tool_execution_update",
                    "toolCallId": id,
                    "toolName": name,
                    "args": args,
                    "partialResult": {
                        "content": [{"type": "text", "text": output}],
                        "details": {},
                    },
                })]
            }
            AgentEvent::TurnEnd {
                message,
                tool_results,
            } => vec![json!({
                "type": "turn_end",
                "message": assistant_json(message),
                "toolResults": tool_results
                    .iter()
                    .map(|r| message_json(&AgentMessage::Llm(Message::ToolResult(r.clone()))))
                    .collect::<Vec<_>>(),
            })],
            AgentEvent::QueueUpdate {
                steering,
                follow_up,
            } => vec![json!({"type": "queue_update", "steering": steering, "followUp": follow_up})],
            AgentEvent::CompactionStart { reason } => {
                vec![json!({"type": "compaction_start", "reason": reason.as_str()})]
            }
            AgentEvent::CompactionEnd {
                reason,
                result,
                aborted,
                will_retry,
                error,
            } => {
                let mut v = json!({
                    "type": "compaction_end",
                    "reason": reason.as_str(),
                    "result": result.as_ref().map(compaction_result_json),
                    "aborted": aborted,
                    "willRetry": will_retry,
                });
                if let Some(e) = error {
                    v["errorMessage"] = json!(e);
                }
                vec![v]
            }
            AgentEvent::RetryStart {
                attempt,
                max_attempts,
                delay_ms,
                error,
            } => vec![json!({
                "type": "auto_retry_start",
                "attempt": attempt,
                "maxAttempts": max_attempts,
                "delayMs": delay_ms,
                "errorMessage": error,
            })],
            AgentEvent::RetryEnd {
                success,
                attempt,
                final_error,
            } => {
                let mut v =
                    json!({"type": "auto_retry_end", "success": success, "attempt": attempt});
                if let Some(e) = final_error {
                    v["finalError"] = json!(e);
                }
                vec![v]
            }
            AgentEvent::Warning(_) => Vec::new(),
            AgentEvent::AgentEnd { .. } => {
                self.partial = None;
                vec![json!({
                    "type": "agent_end",
                    "messages": std::mem::take(&mut self.run_messages),
                })]
            }
        }
    }

    /// [`Self::serialize`] rendered as JSONL lines (each ends with `\n`).
    pub fn lines(&mut self, event: &AgentEvent) -> String {
        self.serialize(event)
            .into_iter()
            .map(|v| format!("{v}\n"))
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::agent::ToolExecution;
    use crate::harness::hooks::CompactionReason;
    use crate::message::ToolResultMessage;

    fn types(values: &[Value]) -> Vec<String> {
        values
            .iter()
            .map(|v| {
                let t = v["type"].as_str().unwrap().to_string();
                match v["assistantMessageEvent"]["type"].as_str() {
                    Some(inner) => format!("{t}:{inner}"),
                    None => t,
                }
            })
            .collect()
    }

    #[test]
    fn streams_turns_tools_and_end_in_pi_shapes() {
        let mut s = PiEventSerializer::new();
        s.set_model("openai", "gpt-5", "openai-responses");
        let call = ContentBlock::ToolCall {
            id: "c1".into(),
            name: "read".into(),
            arguments: json!({"path": "a"}),
        };
        let assistant = AssistantMessage {
            content: vec![
                ContentBlock::Thinking {
                    thinking: "hm".into(),
                    signature: None,
                    redacted: false,
                },
                ContentBlock::text("Hi"),
                call.clone(),
            ],
            provider: "openai".into(),
            model: "gpt-5".into(),
            api: "openai-responses".into(),
            stop_reason: StopReason::ToolCall,
            usage: Usage {
                input_tokens: 10,
                output_tokens: 2,
                total_cost_usd: 0.5,
                ..Default::default()
            },
            timestamp: Some(1),
            ..Default::default()
        };
        let result = ToolResultMessage {
            tool_call_id: "c1".into(),
            tool_name: "read".into(),
            content: vec![ContentBlock::text("data")],
            ..Default::default()
        };
        let events = vec![
            AgentEvent::AgentStart,
            AgentEvent::TurnStart,
            AgentEvent::MessageEnd(Message::user_text("go")),
            AgentEvent::AssistantStart,
            AgentEvent::Stream(StreamEvent::BlockStart {
                index: 0,
                block: ContentBlock::Thinking {
                    thinking: String::new(),
                    signature: None,
                    redacted: false,
                },
            }),
            AgentEvent::Stream(StreamEvent::ThinkingDelta {
                index: 0,
                delta: "hm".into(),
            }),
            AgentEvent::Stream(StreamEvent::BlockStart {
                index: 1,
                block: ContentBlock::text(""),
            }),
            AgentEvent::Stream(StreamEvent::TextDelta {
                index: 1,
                delta: "Hi".into(),
            }),
            AgentEvent::Stream(StreamEvent::BlockStart {
                index: 2,
                block: call.clone(),
            }),
            AgentEvent::Stream(StreamEvent::ToolArgsDelta {
                index: 2,
                delta: "{\"path\":\"a\"}".into(),
            }),
            AgentEvent::MessageEnd(Message::Assistant(assistant.clone())),
            AgentEvent::ToolStart(ToolExecution {
                id: "c1".into(),
                name: "read".into(),
                args: json!({"path": "a"}),
            }),
            AgentEvent::ToolProgress {
                id: "c1".into(),
                output: "partial".into(),
            },
            AgentEvent::MessageEnd(Message::ToolResult(result.clone())),
            AgentEvent::TurnEnd {
                message: assistant.clone(),
                tool_results: vec![result],
            },
            AgentEvent::QueueUpdate {
                steering: vec!["s".into()],
                follow_up: vec![],
            },
            AgentEvent::CompactionStart {
                reason: CompactionReason::Threshold,
            },
            AgentEvent::RetryEnd {
                success: false,
                attempt: 2,
                final_error: Some("boom".into()),
            },
            AgentEvent::Warning("ignored".into()),
            AgentEvent::AgentEnd {
                usage: Usage::default(),
                aborted: false,
            },
        ];
        let out: Vec<Value> = events.iter().flat_map(|e| s.serialize(e)).collect();
        assert_eq!(
            types(&out),
            [
                "agent_start",
                "turn_start",
                "message_start",
                "message_end",
                "message_start",
                "message_update:thinking_start",
                "message_update:thinking_delta",
                "message_update:thinking_end",
                "message_update:text_start",
                "message_update:text_delta",
                "message_update:text_end",
                "message_update:toolcall_start",
                "message_update:toolcall_delta",
                "message_update:toolcall_end",
                "message_end",
                "tool_execution_start",
                "tool_execution_update",
                "tool_execution_end",
                "message_start",
                "message_end",
                "turn_end",
                "queue_update",
                "compaction_start",
                "auto_retry_end",
                "agent_end",
            ]
        );
        let text_delta = &out[9];
        assert_eq!(text_delta["assistantMessageEvent"]["delta"], "Hi");
        assert_eq!(text_delta["assistantMessageEvent"]["contentIndex"], 1);
        assert_eq!(text_delta["message"]["content"][1]["text"], "Hi");
        assert_eq!(
            text_delta["assistantMessageEvent"]["partial"]["provider"],
            "openai"
        );
        assert_eq!(
            out[13]["assistantMessageEvent"]["toolCall"]["arguments"]["path"],
            "a"
        );
        let end = &out[14]["message"];
        assert_eq!(end["stopReason"], "toolUse");
        assert_eq!(end["usage"]["input"], 10);
        assert_eq!(end["usage"]["totalTokens"], 12);
        assert_eq!(end["usage"]["cost"]["total"], 0.5);
        assert_eq!(out[16]["toolName"], "read");
        assert_eq!(out[16]["partialResult"]["content"][0]["text"], "partial");
        assert_eq!(out[17]["result"]["content"][0]["text"], "data");
        assert_eq!(out[17]["isError"], false);
        assert_eq!(out[20]["toolResults"][0]["role"], "toolResult");
        assert_eq!(out[21]["followUp"], json!([]));
        assert_eq!(out[22]["reason"], "threshold");
        assert_eq!(out[23]["finalError"], "boom");
        assert_eq!(out[24]["messages"].as_array().unwrap().len(), 3);
        let header = SessionHeader {
            version: 3,
            id: "x".into(),
            timestamp: "t".into(),
            cwd: "/c".into(),
            parent_session: None,
        };
        assert_eq!(session_header_json(&header)["type"], "session");
        assert!(s.lines(&AgentEvent::TurnStart).ends_with("}\n"));
    }

    #[test]
    fn errors_without_streaming_still_open_the_message() {
        let mut s = PiEventSerializer::new();
        let failed = AssistantMessage {
            stop_reason: StopReason::Error,
            error_message: Some("nope".into()),
            ..Default::default()
        };
        let out = s.serialize(&AgentEvent::MessageEnd(Message::Assistant(failed)));
        assert_eq!(types(&out), ["message_start", "message_end"]);
        assert_eq!(out[1]["message"]["errorMessage"], "nope");
        assert_eq!(out[1]["message"]["stopReason"], "error");
    }
}
