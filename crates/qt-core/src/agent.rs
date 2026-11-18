//! The agent loop: stream a response, run its tool calls, repeat.

use async_channel::Sender;
use serde_json::Value;
use tokio_util::sync::CancellationToken;

use crate::message::{ContentBlock, Message, StopReason, ToolResultMessage, Usage, now_millis};
use crate::provider::{self, ProviderConfig, Request, StreamEvent, ThinkingLevel};
use crate::tools::{ToolContext, ToolRegistry};

/// Events emitted while the agent runs. Front ends render from these.
#[derive(Debug, Clone, PartialEq)]
pub enum AgentEvent {
    AgentStart,
    /// An assistant response started streaming.
    AssistantStart,
    /// Incremental content of the streaming assistant response.
    Stream(StreamEvent),
    /// A message was appended to the conversation (and persisted).
    MessageEnd(Message),
    ToolStart(ToolExecution),
    /// Latest output tail of a running tool.
    ToolProgress {
        id: String,
        output: String,
    },
    /// Non-fatal problem, e.g. a session write failure.
    Warning(String),
    /// The run finished; `usage` sums all responses of this run.
    AgentEnd {
        usage: Usage,
        aborted: bool,
    },
}

#[derive(Debug, Clone, PartialEq)]
pub struct ToolExecution {
    pub id: String,
    pub name: String,
    pub args: Value,
}

/// Everything fixed for one agent run.
#[derive(Clone)]
pub struct AgentParams {
    pub http: reqwest::Client,
    pub provider: ProviderConfig,
    pub model: String,
    pub system: String,
    pub tools: ToolRegistry,
    pub max_tokens: u32,
    pub thinking: ThinkingLevel,
    pub cwd: std::path::PathBuf,
}

/// Run until the model stops calling tools, an error occurs, or `cancel` fires.
///
/// `record` is called for every message appended to the conversation, in
/// order; it persists the message and returns an error description on failure.
pub async fn run_agent(
    params: AgentParams,
    mut history: Vec<Message>,
    new_messages: Vec<Message>,
    cancel: CancellationToken,
    tx: Sender<AgentEvent>,
    record: &mut (dyn FnMut(&Message) -> Result<(), String> + Send),
) {
    let emit = |event: AgentEvent| {
        let _ = tx.try_send(event);
    };
    let mut append = |history: &mut Vec<Message>, message: Message| {
        if let Err(err) = record(&message) {
            emit(AgentEvent::Warning(format!(
                "Could not save session: {err}"
            )));
        }
        history.push(message.clone());
        emit(AgentEvent::MessageEnd(message));
    };

    emit(AgentEvent::AgentStart);
    for message in new_messages {
        append(&mut history, message);
    }
    let specs = params.tools.specs();
    let mut usage = Usage::default();
    loop {
        if cancel.is_cancelled() {
            break;
        }
        let request = Request {
            model: params.model.clone(),
            system: params.system.clone(),
            messages: history.clone(),
            tools: specs.clone(),
            max_tokens: params.max_tokens,
            thinking: params.thinking,
        };
        emit(AgentEvent::AssistantStart);
        let mut on_stream = |event: StreamEvent| emit(AgentEvent::Stream(event));
        let assistant = provider::stream(
            &params.http,
            &params.provider,
            &request,
            &cancel,
            &mut on_stream,
        )
        .await;
        usage.add(&assistant.usage);
        let stop = assistant.stop_reason;
        let calls: Vec<(String, String, Value)> = assistant
            .tool_calls()
            .map(|(id, name, args)| (id.to_string(), name.to_string(), args.clone()))
            .collect();
        append(&mut history, Message::Assistant(assistant));
        if matches!(stop, StopReason::Error | StopReason::Aborted) || calls.is_empty() {
            break;
        }
        for (id, name, args) in calls {
            let result = if cancel.is_cancelled() {
                ToolResultMessage {
                    tool_call_id: id,
                    tool_name: name,
                    content: vec![ContentBlock::text("Skipped: aborted by user")],
                    is_error: true,
                    details: None,
                    timestamp: Some(now_millis()),
                }
            } else {
                emit(AgentEvent::ToolStart(ToolExecution {
                    id: id.clone(),
                    name: name.clone(),
                    args: args.clone(),
                }));
                let progress_tx = tx.clone();
                let progress_id = id.clone();
                let ctx = ToolContext::new(params.cwd.clone())
                    .with_cancel(cancel.child_token())
                    .with_progress(std::sync::Arc::new(move |output: String| {
                        let _ = progress_tx.try_send(AgentEvent::ToolProgress {
                            id: progress_id.clone(),
                            output,
                        });
                    }));
                let output = params.tools.execute(&name, args, ctx).await;
                ToolResultMessage {
                    tool_call_id: id,
                    tool_name: name,
                    content: output.content,
                    is_error: output.is_error,
                    details: output.details,
                    timestamp: Some(now_millis()),
                }
            };
            append(&mut history, Message::ToolResult(result));
        }
    }
    emit(AgentEvent::AgentEnd {
        usage,
        aborted: cancel.is_cancelled(),
    });
}
