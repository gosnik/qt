//! Session statistics (Pi `getSessionStats` / `getContextUsage`) with a
//! per-model usage breakdown.

use std::path::PathBuf;

use super::compaction::{calculate_context_tokens, estimate_context_tokens};
use crate::message::{AgentMessage, ContentBlock, Message, StopReason, Usage};
use crate::provider::ModelRef;
use crate::session::{EntryKind, SessionEntry};

/// Usage of one model within the context.
#[derive(Debug, Clone, PartialEq)]
pub struct ModelUsage {
    pub model: ModelRef,
    pub responses: usize,
    pub usage: Usage,
}

/// How full the context window is. `tokens`/`percent` are unknown right
/// after a compaction until the next response reports usage.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ContextUsage {
    pub tokens: Option<u64>,
    pub context_window: u64,
    pub percent: Option<f64>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct SessionStats {
    pub session_file: Option<PathBuf>,
    pub session_id: String,
    pub user_messages: usize,
    pub assistant_messages: usize,
    pub tool_calls: usize,
    pub tool_results: usize,
    pub total_messages: usize,
    /// Summed usage; `total_tokens` is input + output + cache read + cache write.
    pub tokens: Usage,
    pub cost: f64,
    /// Per provider/model, in order of first use.
    pub per_model: Vec<ModelUsage>,
    pub context_usage: Option<ContextUsage>,
}

/// Counts and usage over the context messages.
pub fn compute_stats(
    messages: &[AgentMessage],
    session_file: Option<PathBuf>,
    session_id: String,
    context_usage: Option<ContextUsage>,
) -> SessionStats {
    let mut stats = SessionStats {
        session_file,
        session_id,
        user_messages: 0,
        assistant_messages: 0,
        tool_calls: 0,
        tool_results: 0,
        total_messages: messages.len(),
        tokens: Usage::default(),
        cost: 0.0,
        per_model: Vec::new(),
        context_usage,
    };
    for message in messages {
        match message {
            AgentMessage::Llm(Message::User { .. }) => stats.user_messages += 1,
            AgentMessage::Llm(Message::ToolResult(_)) => stats.tool_results += 1,
            AgentMessage::Llm(Message::Assistant(a)) => {
                stats.assistant_messages += 1;
                stats.tool_calls += a
                    .content
                    .iter()
                    .filter(|b| matches!(b, ContentBlock::ToolCall { .. }))
                    .count();
                stats.tokens.add(&a.usage);
                stats.cost += a.usage.total_cost_usd;
                let model = ModelRef::new(a.provider.clone(), a.model.clone());
                match stats.per_model.iter_mut().find(|m| m.model == model) {
                    Some(entry) => {
                        entry.responses += 1;
                        entry.usage.add(&a.usage);
                    }
                    None => stats.per_model.push(ModelUsage {
                        model,
                        responses: 1,
                        usage: a.usage,
                    }),
                }
            }
            _ => {}
        }
    }
    let t = &mut stats.tokens;
    t.total_tokens = t.input_tokens + t.output_tokens + t.cache_read_tokens + t.cache_write_tokens;
    stats
}

/// Pi `getContextUsage`: `None` without a known window.
pub fn context_usage(
    branch: &[SessionEntry],
    messages: &[AgentMessage],
    context_window: Option<u64>,
) -> Option<ContextUsage> {
    let window = context_window.filter(|w| *w > 0)?;
    if let Some(ci) = branch
        .iter()
        .rposition(|e| matches!(e.kind, EntryKind::Compaction(_)))
    {
        let post = branch[ci + 1..].iter().rev().find_map(|e| {
            e.message()
                .and_then(AgentMessage::as_assistant)
                .filter(|a| !matches!(a.stop_reason, StopReason::Aborted | StopReason::Error))
        });
        if !post.is_some_and(|a| calculate_context_tokens(&a.usage) > 0) {
            return Some(ContextUsage {
                tokens: None,
                context_window: window,
                percent: None,
            });
        }
    }
    let tokens = estimate_context_tokens(messages).tokens;
    Some(ContextUsage {
        tokens: Some(tokens),
        context_window: window,
        percent: Some(tokens as f64 / window as f64 * 100.0),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::message::AssistantMessage;

    fn reply(provider: &str, model: &str, input: u64, cost: f64) -> AgentMessage {
        AgentMessage::Llm(Message::Assistant(AssistantMessage {
            content: vec![ContentBlock::ToolCall {
                id: "1".into(),
                name: "read".into(),
                arguments: serde_json::Value::Null,
            }],
            provider: provider.into(),
            model: model.into(),
            usage: Usage {
                input_tokens: input,
                output_tokens: 10,
                total_cost_usd: cost,
                ..Default::default()
            },
            ..Default::default()
        }))
    }

    #[test]
    fn counts_and_per_model_usage() {
        let messages = vec![
            AgentMessage::Llm(Message::user_text("hi")),
            reply("a", "m1", 100, 0.5),
            reply("b", "m2", 50, 0.25),
            reply("a", "m1", 10, 0.0),
        ];
        let s = compute_stats(&messages, None, "id".into(), None);
        assert_eq!(
            (s.user_messages, s.assistant_messages, s.tool_calls),
            (1, 3, 3)
        );
        assert_eq!(s.tokens.input_tokens, 160);
        assert_eq!(s.tokens.total_tokens, 190);
        assert!((s.cost - 0.75).abs() < 1e-9);
        assert_eq!(s.per_model.len(), 2);
        assert_eq!(s.per_model[0].responses, 2);
        assert_eq!(s.per_model[0].usage.input_tokens, 110);

        let usage = context_usage(&[], &messages, Some(1_000)).unwrap();
        assert_eq!(usage.tokens, Some(20));
        assert!(context_usage(&[], &messages, None).is_none());
    }
}
