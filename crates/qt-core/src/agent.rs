//! The agent loop: stream a response, run its tool calls, deliver queued
//! steering and follow-up messages, repeat (Pi `agent-loop.ts`).
//!
//! The loop is stateless between runs. The conversation driver in
//! [`crate::engine`] supplies callbacks for persistence, queues and
//! turn-boundary updates, and wraps the loop with auto-retry, compaction and
//! goal continuation.

use std::sync::Arc;

use async_channel::Sender;
use serde_json::Value;
use tokio_util::sync::CancellationToken;

use crate::harness::compaction::CompactionResult;
use crate::harness::hooks::{
    CompactionReason, Hooks, ProviderResponseEvent, ToolCallEvent, ToolCallOutcome, ToolResultEvent,
};
use crate::message::{
    AgentMessage, AssistantMessage, Message, StopReason, ToolResultMessage, Usage, convert_to_llm,
    now_millis,
};
use crate::provider::{self, ModelRef, ProviderConfig, Request, StreamEvent, ThinkingLevel};
use crate::tools::{
    Tool, ToolContext, ToolEnvironment, ToolOutput, ToolRegistry, apply_output_guard,
    normalize_arguments,
};

/// Events emitted while the agent runs. Front ends render from these.
#[derive(Debug, Clone, PartialEq)]
pub enum AgentEvent {
    AgentStart,
    /// A turn (one assistant response plus its tool calls) begins.
    TurnStart,
    /// A turn ended: its assistant message and the tool results it produced.
    TurnEnd {
        message: AssistantMessage,
        tool_results: Vec<ToolResultMessage>,
    },
    /// An assistant response started streaming.
    AssistantStart,
    /// Incremental content of the streaming assistant response.
    Stream(StreamEvent),
    /// A user, assistant or tool-result message was appended (and persisted).
    MessageEnd(Message),
    /// A harness message (custom, bashExecution, summaries) was appended.
    CustomMessageEnd(AgentMessage),
    ToolStart(ToolExecution),
    /// Latest output tail of a running tool.
    ToolProgress {
        id: String,
        output: String,
    },
    /// The steering / follow-up queues changed.
    QueueUpdate {
        steering: Vec<String>,
        follow_up: Vec<String>,
    },
    CompactionStart {
        reason: CompactionReason,
    },
    CompactionEnd {
        reason: CompactionReason,
        result: Option<CompactionResult>,
        aborted: bool,
        /// The failed request is retried after this compaction.
        will_retry: bool,
        error: Option<String>,
    },
    /// A transient provider error will be retried after `delay_ms`.
    RetryStart {
        attempt: u32,
        max_attempts: u32,
        delay_ms: u64,
        error: String,
    },
    RetryEnd {
        success: bool,
        attempt: u32,
        final_error: Option<String>,
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

/// Sends events to the run's receiver and to registered event listeners.
#[derive(Clone)]
pub struct Emitter {
    tx: Sender<AgentEvent>,
    hooks: Hooks,
}

impl Emitter {
    pub fn new(tx: Sender<AgentEvent>, hooks: Hooks) -> Self {
        Self { tx, hooks }
    }

    pub fn emit(&self, event: AgentEvent) {
        self.hooks.emit_agent_event(&event);
        let _ = self.tx.try_send(event);
    }

    /// `MessageEnd` or `CustomMessageEnd` for `message`.
    pub fn message(&self, message: &AgentMessage) {
        match message {
            AgentMessage::Llm(m) => self.emit(AgentEvent::MessageEnd(m.clone())),
            other => self.emit(AgentEvent::CustomMessageEnd(other.clone())),
        }
    }
}

/// Everything fixed for one turn of the loop.
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
    pub hooks: Hooks,
    /// Workspace policy, mutation, bash, image and web options for tools.
    pub tool_env: ToolEnvironment,
}

impl AgentParams {
    pub fn model_ref(&self) -> ModelRef {
        ModelRef::new(self.provider.id.clone(), self.model.clone())
    }
}

/// Callbacks into the conversation that owns the run.
pub struct LoopCallbacks<'a> {
    /// Persist a message appended to the context; errors become warnings.
    pub record: &'a mut (dyn FnMut(&AgentMessage) -> Result<(), String> + Send),
    /// Steering messages due now.
    pub steering: &'a mut (dyn FnMut() -> Vec<AgentMessage> + Send),
    /// Follow-up messages due now (asked only when the agent would stop).
    pub follow_up: &'a mut (dyn FnMut() -> Vec<AgentMessage> + Send),
    /// Called after every turn (save point): flush deferred changes and
    /// return replacement parameters when model, thinking or tools changed.
    pub turn_end: &'a mut (dyn FnMut() -> Option<AgentParams> + Send),
}

/// What one loop run produced.
#[derive(Debug, Clone, Default)]
pub struct LoopOutcome {
    pub usage: Usage,
    pub last_assistant: Option<AssistantMessage>,
    pub turns: u64,
    pub tool_calls: usize,
}

/// Run until the model stops calling tools and no queued messages remain,
/// an error occurs, or `cancel` fires. `prompts` are appended first.
pub async fn run_loop(
    mut params: AgentParams,
    mut context: Vec<AgentMessage>,
    prompts: Vec<AgentMessage>,
    cancel: &CancellationToken,
    emit: &Emitter,
    cb: &mut LoopCallbacks<'_>,
) -> LoopOutcome {
    let mut outcome = LoopOutcome::default();
    let hooks = params.hooks.clone();
    // Assistant messages are finalized (and `message_end`-replaced) before
    // their tool calls are read; every other message is replaced here.
    let append =
        |context: &mut Vec<AgentMessage>, message: AgentMessage, cb: &mut LoopCallbacks<'_>| {
            let message = if hooks.has_message_end() && message.as_assistant().is_none() {
                hooks.run_message_end(message)
            } else {
                message
            };
            if let Err(err) = (cb.record)(&message) {
                emit.emit(AgentEvent::Warning(format!(
                    "Could not save session: {err}"
                )));
            }
            emit.message(&message);
            context.push(message);
        };
    for prompt in prompts {
        append(&mut context, prompt, cb);
    }
    let mut pending = (cb.steering)();
    'outer: loop {
        let mut has_more_tool_calls = true;
        while has_more_tool_calls || !pending.is_empty() {
            if cancel.is_cancelled() {
                break 'outer;
            }
            emit.emit(AgentEvent::TurnStart);
            for message in std::mem::take(&mut pending) {
                append(&mut context, message, cb);
            }
            let mut assistant =
                stream_response(&params, &context, outcome.turns, cancel, emit).await;
            if hooks.has_message_end()
                && let Some(replaced) = hooks
                    .run_message_end(AgentMessage::Llm(Message::Assistant(assistant.clone())))
                    .as_assistant()
            {
                assistant = replaced.clone();
            }
            let turn_message = assistant.clone();
            outcome.usage.add(&assistant.usage);
            let stop = assistant.stop_reason;
            let calls: Vec<(String, String, Value)> = assistant
                .tool_calls()
                .map(|(id, name, args)| (id.to_string(), name.to_string(), args.clone()))
                .collect();
            outcome.last_assistant = Some(assistant.clone());
            append(
                &mut context,
                AgentMessage::Llm(Message::Assistant(assistant)),
                cb,
            );
            if matches!(stop, StopReason::Error | StopReason::Aborted) {
                outcome.turns += 1;
                emit.emit(AgentEvent::TurnEnd {
                    message: turn_message,
                    tool_results: Vec::new(),
                });
                break 'outer;
            }
            has_more_tool_calls = false;
            let mut tool_results = Vec::new();
            if !calls.is_empty() {
                outcome.tool_calls += calls.len();
                let turn = outcome.turns;
                let sequential_sink = &mut |result: ToolResultMessage| {
                    tool_results.push(result.clone());
                    append(
                        &mut context,
                        AgentMessage::Llm(Message::ToolResult(result)),
                        cb,
                    );
                };
                execute_tools(&params, calls, turn, cancel, emit, sequential_sink).await;
                has_more_tool_calls = true;
            }
            outcome.turns += 1;
            emit.emit(AgentEvent::TurnEnd {
                message: turn_message,
                tool_results,
            });
            if let Some(next) = (cb.turn_end)() {
                params = next;
            }
            pending = (cb.steering)();
        }
        if cancel.is_cancelled() {
            break;
        }
        let follow_ups = (cb.follow_up)();
        if follow_ups.is_empty() {
            break;
        }
        pending = follow_ups;
    }
    outcome
}

async fn stream_response(
    params: &AgentParams,
    context: &[AgentMessage],
    turn: u64,
    cancel: &CancellationToken,
    emit: &Emitter,
) -> AssistantMessage {
    let model = params.model_ref();
    let messages = params.hooks.run_context(context.to_vec(), turn);
    let request = Request {
        model: params.model.clone(),
        system: params.system.clone(),
        messages: convert_to_llm(&messages),
        tools: params.tools.specs(),
        max_tokens: params.max_tokens,
        thinking: params.thinking,
        ..Default::default()
    };
    // Augmentors (memory recall, which may run an isolated child agent) are
    // synchronous and can take seconds, so they run on a blocking thread
    // instead of an async worker.
    let request = if params.hooks.has_model_context() {
        let hooks = params.hooks.clone();
        let hook_model = model.clone();
        let hook_cancel = cancel.clone();
        tokio::task::spawn_blocking(move || {
            hooks.run_model_context(&hook_model, request, turn, &hook_cancel)
        })
        .await
        .unwrap_or_else(|e| Err(format!("model context augmentor failed: {e}")))
    } else {
        Ok(request)
    };
    let request = match request {
        Ok(request) => request,
        Err(error) => {
            emit.emit(AgentEvent::AssistantStart);
            return provider::PartialMessage::new(&params.provider, &params.model)
                .finish_error(error);
        }
    };
    let request = params
        .hooks
        .run_before_provider_request(&model, request, turn);
    emit.emit(AgentEvent::AssistantStart);
    let mut on_stream = |event: StreamEvent| emit.emit(AgentEvent::Stream(event));
    let response = provider::stream(
        &params.http,
        &params.provider,
        &request,
        cancel,
        &mut on_stream,
    )
    .await;
    params
        .hooks
        .run_after_provider_response(&ProviderResponseEvent {
            model,
            request,
            response: response.clone(),
            turn_index: turn,
        });
    response
}

/// Execute `tool` the way [`ToolRegistry::execute`] does: archived argument
/// aliases are normalised and the output guard is applied.
async fn run_tool(tool: Arc<dyn Tool>, args: Value, ctx: ToolContext) -> ToolOutput {
    let args = normalize_arguments(tool.name(), args);
    let guard = ctx.output_guard;
    let mut output = tool.execute(args, ctx).await;
    apply_output_guard(&mut output, guard);
    output
}

struct PreparedCall {
    id: String,
    name: String,
    args: Value,
    tool: Option<Arc<dyn Tool>>,
    immediate: Option<ToolOutput>,
}

fn tool_context(
    params: &AgentParams,
    id: &str,
    cancel: &CancellationToken,
    emit: &Emitter,
) -> ToolContext {
    let progress_emit = emit.clone();
    let progress_id = id.to_string();
    params
        .tool_env
        .context(params.cwd.clone())
        .with_call_id(id)
        .with_cancel(cancel.child_token())
        .with_progress(Arc::new(move |output: String| {
            progress_emit.emit(AgentEvent::ToolProgress {
                id: progress_id.clone(),
                output,
            });
        }))
}

fn finish_call(
    params: &AgentParams,
    call: PreparedCall,
    output: ToolOutput,
    executed: bool,
    turn: u64,
) -> ToolResultMessage {
    let output = if executed {
        params.hooks.run_tool_result(ToolResultEvent {
            turn_index: turn,
            tool_call_id: call.id.clone(),
            tool_name: call.name.clone(),
            args: call.args.clone(),
            output,
        })
    } else {
        output
    };
    ToolResultMessage {
        tool_call_id: call.id,
        tool_name: call.name,
        content: output.content,
        is_error: output.is_error,
        details: output.details,
        timestamp: Some(now_millis()),
    }
}

/// Run one assistant message's tool calls. When every runnable call's tool
/// is [`Tool::parallel_safe`], they run concurrently and results are
/// committed in source order once all finish; otherwise they run one by one.
async fn execute_tools(
    params: &AgentParams,
    calls: Vec<(String, String, Value)>,
    turn: u64,
    cancel: &CancellationToken,
    emit: &Emitter,
    sink: &mut (dyn FnMut(ToolResultMessage) + Send),
) {
    let mut prepared = Vec::with_capacity(calls.len());
    for (id, name, mut args) in calls {
        let tool = params.tools.get(&name);
        let immediate = if tool.is_none() {
            Some(ToolOutput::error(format!("Tool {name} not found")))
        } else {
            match params.hooks.run_tool_call_chain(&ToolCallEvent {
                turn_index: turn,
                tool_call_id: id.clone(),
                tool_name: name.clone(),
                args: args.clone(),
            }) {
                ToolCallOutcome::Run(rewritten) => {
                    args = rewritten;
                    None
                }
                ToolCallOutcome::Blocked(reason) => Some(ToolOutput::error(
                    reason.unwrap_or_else(|| "Tool execution was blocked".into()),
                )),
            }
        };
        prepared.push(PreparedCall {
            id,
            name,
            args,
            tool,
            immediate,
        });
    }
    let runnable: Vec<&PreparedCall> = prepared.iter().filter(|p| p.immediate.is_none()).collect();
    let parallel = runnable.len() > 1
        && runnable
            .iter()
            .all(|p| p.tool.as_ref().is_some_and(|t| t.parallel_safe()));

    if parallel && !cancel.is_cancelled() {
        for call in &prepared {
            emit.emit(AgentEvent::ToolStart(ToolExecution {
                id: call.id.clone(),
                name: call.name.clone(),
                args: call.args.clone(),
            }));
        }
        let futures = prepared.iter().map(|call| {
            let run = match (&call.immediate, &call.tool) {
                (None, Some(tool)) => Some(run_tool(
                    tool.clone(),
                    call.args.clone(),
                    tool_context(params, &call.id, cancel, emit),
                )),
                _ => None,
            };
            async move {
                match run {
                    Some(fut) => Some(fut.await),
                    None => None,
                }
            }
        });
        let outputs = futures::future::join_all(futures).await;
        for (mut call, output) in prepared.into_iter().zip(outputs) {
            let (output, executed) = match output {
                Some(o) => (o, true),
                None => (call.immediate.take().unwrap_or_default(), false),
            };
            sink(finish_call(params, call, output, executed, turn));
        }
        return;
    }

    for mut call in prepared {
        if cancel.is_cancelled() {
            let output = ToolOutput::error("Skipped: aborted by user");
            sink(finish_call(params, call, output, false, turn));
            continue;
        }
        emit.emit(AgentEvent::ToolStart(ToolExecution {
            id: call.id.clone(),
            name: call.name.clone(),
            args: call.args.clone(),
        }));
        let (output, executed) = match (call.immediate.take(), call.tool.clone()) {
            (Some(output), _) => (output, false),
            (None, Some(tool)) => (
                run_tool(
                    tool,
                    call.args.clone(),
                    tool_context(params, &call.id, cancel, emit),
                )
                .await,
                true,
            ),
            (None, None) => (
                ToolOutput::error(format!("Tool {} not found", call.name)),
                false,
            ),
        };
        sink(finish_call(params, call, output, executed, turn));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::harness::hooks::{
        ModelContextEvent, ToolCallDecision, ToolCallEvent, ToolResultEvent, ToolResultPatch,
    };
    use crate::harness::test_server::{self, Reply};
    use crate::message::{ContentBlock, CustomMessage, UserContent};
    use crate::provider::ProviderKind;
    use futures::future::BoxFuture;
    use serde_json::json;
    use std::sync::Mutex;
    use std::time::Duration;

    /// Waits on a shared barrier: two calls only finish when run concurrently.
    struct BarrierTool {
        name: &'static str,
        barrier: Arc<tokio::sync::Barrier>,
        parallel: bool,
        log: Arc<Mutex<Vec<String>>>,
    }

    impl Tool for BarrierTool {
        fn name(&self) -> &'static str {
            self.name
        }
        fn description(&self) -> String {
            "test".into()
        }
        fn snippet(&self) -> &'static str {
            "test"
        }
        fn parameters(&self) -> Value {
            json!({"type": "object"})
        }
        fn parallel_safe(&self) -> bool {
            self.parallel
        }
        fn execute(&self, args: Value, _ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
            let barrier = self.barrier.clone();
            let log = self.log.clone();
            let parallel = self.parallel;
            let name = self.name;
            Box::pin(async move {
                log.lock().unwrap().push(format!("start {name}"));
                if parallel {
                    let waited = tokio::time::timeout(Duration::from_secs(5), barrier.wait()).await;
                    if waited.is_err() {
                        return ToolOutput::error("ran sequentially");
                    }
                }
                log.lock().unwrap().push(format!("end {name}"));
                ToolOutput::text(format!("{name}:{}", args["n"]))
            })
        }
    }

    fn tool(
        name: &'static str,
        barrier: &Arc<tokio::sync::Barrier>,
        parallel: bool,
        log: &Arc<Mutex<Vec<String>>>,
    ) -> Arc<dyn Tool> {
        Arc::new(BarrierTool {
            name,
            barrier: barrier.clone(),
            parallel,
            log: log.clone(),
        })
    }

    fn params(port: u16, tools: ToolRegistry, hooks: Hooks) -> AgentParams {
        AgentParams {
            http: reqwest::Client::new(),
            provider: ProviderConfig {
                id: "ollama:local".into(),
                label: "Ollama".into(),
                kind: ProviderKind::OpenAiCompletions,
                base_url: format!("http://127.0.0.1:{port}/v1"),
                compat: crate::ollama::compat(),
                local: true,
                // Loop tests check one response per request; HTTP retries are tested in provider::http.
                retry: crate::provider::http::RetryPolicy {
                    max_retries: 0,
                    ..Default::default()
                },
                ..Default::default()
            },
            model: "mock".into(),
            system: "sys".into(),
            tools,
            max_tokens: 1000,
            thinking: ThinkingLevel::Off,
            cwd: std::env::temp_dir(),
            hooks,
            tool_env: ToolEnvironment::confined(&std::env::temp_dir()),
        }
    }

    #[derive(Default)]
    struct Harness {
        recorded: Arc<Mutex<Vec<AgentMessage>>>,
        steering: Arc<Mutex<Vec<AgentMessage>>>,
        follow_up: Arc<Mutex<Vec<AgentMessage>>>,
    }

    impl Harness {
        async fn run(&self, params: AgentParams, prompt: &str) -> (LoopOutcome, Vec<AgentEvent>) {
            let (tx, rx) = async_channel::unbounded();
            let emit = Emitter::new(tx, params.hooks.clone());
            let recorded = self.recorded.clone();
            let mut record = move |m: &AgentMessage| -> Result<(), String> {
                recorded.lock().unwrap().push(m.clone());
                Ok(())
            };
            let steering = self.steering.clone();
            let mut steer = move || std::mem::take(&mut *steering.lock().unwrap());
            let follow = self.follow_up.clone();
            let mut follow_up = move || {
                let mut f = follow.lock().unwrap();
                if f.is_empty() {
                    Vec::new()
                } else {
                    vec![f.remove(0)]
                }
            };
            let mut turn_end = || None;
            let mut cb = LoopCallbacks {
                record: &mut record,
                steering: &mut steer,
                follow_up: &mut follow_up,
                turn_end: &mut turn_end,
            };
            let cancel = CancellationToken::new();
            let outcome = run_loop(
                params,
                Vec::new(),
                vec![AgentMessage::Llm(Message::user_text(prompt))],
                &cancel,
                &emit,
                &mut cb,
            )
            .await;
            drop(emit);
            let mut events = Vec::new();
            while let Ok(e) = rx.try_recv() {
                events.push(e);
            }
            (outcome, events)
        }

        fn roles(&self) -> Vec<&'static str> {
            self.recorded
                .lock()
                .unwrap()
                .iter()
                .map(AgentMessage::role)
                .collect()
        }

        fn results(&self) -> Vec<String> {
            self.recorded
                .lock()
                .unwrap()
                .iter()
                .filter_map(|m| match m {
                    AgentMessage::Llm(Message::ToolResult(r)) => Some(r.text()),
                    _ => None,
                })
                .collect()
        }
    }

    #[tokio::test]
    async fn parallel_safe_batches_run_concurrently_in_source_order() {
        let barrier = Arc::new(tokio::sync::Barrier::new(2));
        let log = Arc::new(Mutex::new(Vec::new()));
        let tools = ToolRegistry::empty()
            .with(tool("a", &barrier, true, &log))
            .with(tool("b", &barrier, true, &log));
        let (port, requests) = test_server::serve(vec![
            test_server::tool_calls(&[("1", "b", json!({"n": 1})), ("2", "a", json!({"n": 2}))]),
            test_server::text("done"),
        ])
        .await;
        let h = Harness::default();
        let (outcome, events) = h.run(params(port, tools, Hooks::new()), "go").await;
        assert_eq!(h.results(), ["b:1", "a:2"]);
        assert_eq!(outcome.tool_calls, 2);
        assert_eq!(
            h.roles(),
            ["user", "assistant", "toolResult", "toolResult", "assistant"]
        );
        let log = log.lock().unwrap().clone();
        assert!(log[..2].iter().all(|l| l.starts_with("start")), "{log:?}");
        let starts = events
            .iter()
            .filter(|e| matches!(e, AgentEvent::ToolStart(_)))
            .count();
        assert_eq!(starts, 2);
        let reqs = requests.lock().unwrap();
        let sent = reqs[1]["messages"].as_array().unwrap();
        let ids: Vec<_> = sent
            .iter()
            .filter_map(|m| m["tool_call_id"].as_str())
            .collect();
        assert_eq!(ids, ["1", "2"]);
    }

    #[tokio::test]
    async fn mixed_batches_run_sequentially_and_hooks_apply() {
        let barrier = Arc::new(tokio::sync::Barrier::new(2));
        let log = Arc::new(Mutex::new(Vec::new()));
        let tools = ToolRegistry::empty()
            .with(tool("seq", &barrier, false, &log))
            .with(tool("par", &barrier, false, &log))
            .with(tool("blocked", &barrier, false, &log));
        let hooks = Hooks::new();
        hooks.add_tool_call(Arc::new(|e: &ToolCallEvent| {
            (e.tool_name == "blocked").then_some(ToolCallDecision {
                args: None,
                block: true,
                reason: None,
            })
        }));
        hooks.add_tool_result(Arc::new(|e: &ToolResultEvent| {
            (e.tool_name == "par").then(|| ToolResultPatch {
                content: Some(vec![ContentBlock::text("rewritten")]),
                ..Default::default()
            })
        }));
        hooks.add_model_context_augmentor(Arc::new(|e: &ModelContextEvent| {
            let mut r = e.request.clone();
            r.messages.push(Message::user_text("augmented"));
            Some(r)
        }));
        let (port, requests) = test_server::serve(vec![
            test_server::tool_calls(&[
                ("1", "seq", json!({"n": 1})),
                ("2", "par", json!({"n": 2})),
                ("3", "blocked", json!({})),
                ("4", "missing", json!({})),
            ]),
            test_server::text("done"),
        ])
        .await;
        let h = Harness::default();
        h.run(params(port, tools, hooks), "go").await;
        assert_eq!(
            h.results(),
            [
                "seq:1",
                "rewritten",
                "Tool execution was blocked",
                "Tool missing not found"
            ]
        );
        assert_eq!(
            *log.lock().unwrap(),
            ["start seq", "end seq", "start par", "end par"]
        );
        let reqs = requests.lock().unwrap();
        let last = reqs[0]["messages"]
            .as_array()
            .unwrap()
            .last()
            .unwrap()
            .clone();
        assert_eq!(last["content"], "augmented");
        assert!(
            !h.recorded
                .lock()
                .unwrap()
                .iter()
                .any(|m| m.user_text().as_deref() == Some("augmented"))
        );
    }

    #[tokio::test]
    async fn hooks_rewrite_arguments_and_messages_and_can_fail_closed() {
        let barrier = Arc::new(tokio::sync::Barrier::new(1));
        let log = Arc::new(Mutex::new(Vec::new()));
        let tools = ToolRegistry::empty().with(tool("seq", &barrier, false, &log));
        let hooks = Hooks::new();
        hooks.add_tool_call(Arc::new(|_e: &ToolCallEvent| {
            Some(ToolCallDecision {
                args: Some(json!({"n": 5})),
                ..Default::default()
            })
        }));
        hooks.add_message_end(Arc::new(|m: &AgentMessage| {
            let a = m.as_assistant()?;
            if a.tool_calls().next().is_some() {
                return None;
            }
            let mut a = a.clone();
            a.content = vec![ContentBlock::text("replaced by hook")];
            Some(AgentMessage::Llm(Message::Assistant(a)))
        }));
        let (port, _requests) = test_server::serve(vec![
            test_server::tool_calls(&[("1", "seq", json!({"n": 1}))]),
            test_server::text("original"),
        ])
        .await;
        let h = Harness::default();
        let (outcome, _) = h.run(params(port, tools, hooks.clone()), "go").await;
        assert_eq!(h.results(), ["seq:5"]);
        let last = outcome.last_assistant.unwrap();
        assert_eq!(last.content, vec![ContentBlock::text("replaced by hook")]);
        assert!(h.recorded.lock().unwrap().iter().any(|m| {
            m.as_assistant()
                .is_some_and(|a| a.content == vec![ContentBlock::text("replaced by hook")])
        }));

        // A failing augmentor aborts the request before it is sent.
        let hooks = Hooks::new();
        hooks.add_fallible_model_context_augmentor(Arc::new(|_e: &ModelContextEvent| {
            Err("memory recall failed: store locked".into())
        }));
        let (port, requests) = test_server::serve(vec![test_server::text("unused")]).await;
        let h = Harness::default();
        let (outcome, _) = h
            .run(params(port, ToolRegistry::empty(), hooks), "go")
            .await;
        let last = outcome.last_assistant.unwrap();
        assert_eq!(last.stop_reason, StopReason::Error);
        assert_eq!(
            last.error_message.as_deref(),
            Some("memory recall failed: store locked")
        );
        assert!(requests.lock().unwrap().is_empty());
    }

    #[tokio::test]
    async fn steering_and_follow_ups_extend_the_run() {
        let (port, requests) = test_server::serve(vec![
            test_server::text("first"),
            test_server::text("second"),
        ])
        .await;
        let h = Harness::default();
        h.follow_up
            .lock()
            .unwrap()
            .push(AgentMessage::Custom(CustomMessage::new(
                "ext",
                UserContent::Text("later".into()),
                true,
            )));
        h.steering
            .lock()
            .unwrap()
            .push(AgentMessage::Llm(Message::user_text("steer now")));
        let (outcome, events) = h
            .run(params(port, ToolRegistry::empty(), Hooks::new()), "go")
            .await;
        assert_eq!(
            h.roles(),
            ["user", "user", "assistant", "custom", "assistant"]
        );
        assert_eq!(outcome.last_assistant.unwrap().text(), "second");
        assert!(
            events
                .iter()
                .any(|e| matches!(e, AgentEvent::CustomMessageEnd(_)))
        );
        let reqs = requests.lock().unwrap();
        assert_eq!(reqs.len(), 2);
        assert_eq!(reqs[0]["messages"].as_array().unwrap().len(), 3);
        let last = reqs[1]["messages"]
            .as_array()
            .unwrap()
            .last()
            .unwrap()
            .clone();
        assert_eq!(last["role"], "user");
    }

    #[tokio::test]
    async fn provider_errors_end_the_run() {
        let (port, _requests) = test_server::serve(vec![Reply::Status(
            503,
            r#"{"error":{"message":"overloaded"}}"#.into(),
        )])
        .await;
        let h = Harness::default();
        let (outcome, _) = h
            .run(params(port, ToolRegistry::empty(), Hooks::new()), "go")
            .await;
        let last = outcome.last_assistant.unwrap();
        assert_eq!(last.stop_reason, StopReason::Error);
        assert!(last.error_message.unwrap().contains("overloaded"));
    }
}
