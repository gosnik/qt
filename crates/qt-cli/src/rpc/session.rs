//! The RPC session: the active conversation and the handler of every
//! command (archived `rpc_mode.cpp` handlers, Pi `rpc-mode.ts`
//! `handleCommand`).

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, RwLock};
use std::time::{Duration, Instant};

use async_channel::Receiver;
use serde_json::{Map, Value, json};

use qt_core::agent::AgentEvent;
use qt_core::engine::{Conversation, Engine};
use qt_core::events_json::message_json;
use qt_core::harness::compaction::CompactionResult;
use qt_core::harness::hooks::{InputOutcome, InputSource};
use qt_core::message::ContentBlock;
use qt_core::model_scope::ScopedModel;
use qt_core::provider::{ModelRef, ThinkingLevel};
use qt_core::slash::SlashCommandSource;
use qt_core::tools::Tool as _;
use qt_core::{QueueMode, SessionStats};
use qt_extensions::ExtensionUi as _;
use qt_extensions::ui::NotifyKind;
use qt_runtime::subagents::RunSink;
use qt_runtime::{AttachOptions, Runtime};
use qt_subagents::delivery::{conversation_delivery, event_frame, event_message, is_completion};

use super::models;
use super::output::{EventStream, RpcOutput, RunPump};
use super::protocol::{self, Command, Request, RpcError, StreamingBehavior};
use super::ui::RpcExtensionUi;

/// How long `abort`, `compact` and session switches wait for a run to stop.
const STOP_TIMEOUT: Duration = Duration::from_secs(30);

type Outcome = Result<Option<Value>, RpcError>;

/// Shared state of one RPC process.
pub struct RpcSession {
    runtime: Runtime,
    conversation: Arc<RwLock<Conversation>>,
    output: RpcOutput,
    events: Arc<EventStream>,
    pump: RunPump,
    ui: Arc<RpcExtensionUi>,
    tasks: Arc<AtomicUsize>,
}

fn api_of(engine: &Engine, model: &ModelRef) -> String {
    engine
        .provider(&model.provider)
        .map(|p| {
            let api = p.model_spec(&model.id).api;
            if api.is_empty() {
                p.kind.api_name().to_string()
            } else {
                api
            }
        })
        .unwrap_or_default()
}

/// `extension <event> hook failed: <name>: <error>` from the extension
/// reporter, as `(event, name, error)`.
fn hook_failure(message: &str) -> Option<(&str, &str, &str)> {
    let rest = message.strip_prefix("extension ")?;
    let (event, rest) = rest.split_once(" hook failed: ")?;
    let (name, error) = rest.split_once(": ")?;
    Some((event, name, error))
}

fn compaction_json(result: &CompactionResult) -> Value {
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

fn tokens_json(usage: &qt_core::message::Usage) -> Value {
    json!({
        "input": usage.input_tokens,
        "output": usage.output_tokens,
        "cacheRead": usage.cache_read_tokens,
        "cacheWrite": usage.cache_write_tokens,
        "total": usage.input_tokens + usage.output_tokens + usage.cache_read_tokens + usage.cache_write_tokens,
    })
}

/// Pi `SessionStats` plus the archived per-model breakdown.
pub fn stats_json(stats: &SessionStats) -> Value {
    let mut models = Map::new();
    for m in &stats.per_model {
        models.insert(
            format!("{}/{}", m.model.provider, m.model.id),
            json!({
                "assistantMessages": m.responses,
                "tokens": tokens_json(&m.usage),
                "cost": m.usage.total_cost_usd,
            }),
        );
    }
    let mut out = json!({
        "sessionFile": stats.session_file.as_ref().map(|p| p.display().to_string()),
        "sessionId": stats.session_id,
        "userMessages": stats.user_messages,
        "assistantMessages": stats.assistant_messages,
        "toolCalls": stats.tool_calls,
        "toolResults": stats.tool_results,
        "totalMessages": stats.total_messages,
        "tokens": tokens_json(&stats.tokens),
        "cost": stats.cost,
        "models": models,
    });
    if let Some(usage) = stats.context_usage {
        out["contextUsage"] = json!({
            "tokens": usage.tokens,
            "contextWindow": usage.context_window,
            "percent": usage.percent,
        });
    }
    out
}

/// Pi `SourceInfo` of a command's file: `user` under the agent home,
/// `project` under the working directory, else `temporary`.
fn source_info(path: Option<&Path>, source: &str, cwd: &Path, home: &Path) -> Value {
    let scope = match path {
        Some(p) if p.starts_with(home) => "user",
        Some(p) if p.starts_with(cwd) => "project",
        _ => "temporary",
    };
    json!({
        "path": path.map(|p| p.display().to_string()).unwrap_or_default(),
        "source": source,
        "scope": scope,
        "origin": "top-level",
    })
}

fn wait_until(timeout: Duration, mut done: impl FnMut() -> bool) -> bool {
    let deadline = Instant::now() + timeout;
    while !done() {
        if Instant::now() >= deadline {
            return false;
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    true
}

impl RpcSession {
    /// Bind `conversation` and route extension UI, extension errors, run
    /// events and worker events to `output`.
    pub fn start(runtime: Runtime, conversation: Conversation, output: RpcOutput) -> Arc<Self> {
        let events = Arc::new(EventStream::new(output.clone()));
        let ui = Arc::new(RpcExtensionUi::new(output.clone()));
        let slot = Arc::new(RwLock::new(conversation.clone()));
        let model_slot = slot.clone();
        let engine = runtime.engine().clone();
        let pump = RunPump::start(
            events.clone(),
            Arc::new(move || {
                let model = model_slot.read().expect("conversation lock").model()?;
                let api = api_of(&engine, &model);
                Some((model.provider, model.id, api))
            }),
        );
        let session = Arc::new(Self {
            runtime,
            conversation: slot,
            output,
            events,
            pump,
            ui,
            tasks: Arc::new(AtomicUsize::new(0)),
        });
        session.runtime.set_extension_ui(session.ui.clone());
        let reporter_events = session.events.clone();
        let extensions = session.runtime.extensions().clone();
        session.runtime.extensions().set_reporter(Arc::new(
            move |message: &str| match hook_failure(message) {
                Some((event, name, error)) => {
                    let path = extensions
                        .summaries()
                        .into_iter()
                        .find(|s| s.name == name)
                        .map(|s| s.path.display().to_string())
                        .unwrap_or_else(|| name.to_string());
                    reporter_events.emit_value(&json!({
                        "type": "extension_error",
                        "extensionPath": path,
                        "event": event,
                        "error": error,
                    }));
                }
                None => eprintln!("{message}"),
            },
        ));
        session.attach(&conversation);
        session
    }

    pub fn conversation(&self) -> Conversation {
        self.conversation.read().expect("conversation lock").clone()
    }

    pub fn ui(&self) -> &Arc<RpcExtensionUi> {
        &self.ui
    }

    fn engine(&self) -> &Engine {
        self.runtime.engine()
    }

    fn sink(&self) -> RunSink {
        let pump = self.pump.clone();
        Arc::new(move |run: Receiver<AgentEvent>| pump.submit(run))
    }

    /// Make `conversation` the active one.
    fn attach(&self, conversation: &Conversation) {
        *self.conversation.write().expect("conversation lock") = conversation.clone();
        self.runtime.attach(
            conversation,
            AttachOptions {
                run_sink: Some(self.sink()),
                deliver_subagent_events: false,
            },
        );
        self.wire_subagents(conversation);
    }

    /// Persistent-worker events become `subagent_event` frames; completions
    /// are also delivered to the parent as follow-ups (runs they start are
    /// streamed) and then acknowledged.
    fn wire_subagents(&self, conversation: &Conversation) {
        let Some(coordinator) = self.runtime.subagents().coordinator() else {
            return;
        };
        let events = self.events.clone();
        let deliver = conversation_delivery(conversation.clone(), self.sink());
        let weak = Arc::downgrade(&coordinator);
        let handle = Arc::new(move |event: &Value| {
            events.emit_value(&event_frame(event));
            let accepted = !is_completion(event) || deliver(event_message(event));
            if accepted && let Some(owner) = weak.upgrade() {
                let child = event.get("child_id").and_then(Value::as_str).unwrap_or("");
                let id = event.get("event_id").and_then(Value::as_u64).unwrap_or(0);
                let _ = owner.acknowledge_event(child, id);
            }
        });
        let sink = handle.clone();
        coordinator.set_event_sink(Arc::new(move |event: &Value| sink(event)));
        for event in coordinator.pending_events() {
            handle(&event);
        }
    }

    // ----- output ------------------------------------------------------------

    pub fn respond(&self, id: Option<&str>, command: &str, outcome: Outcome) {
        let value = match outcome {
            Ok(data) => protocol::success(id, command, data),
            Err(error) => protocol::failure(id, command, &error),
        };
        self.output.write(&value);
    }

    fn emit_queue(&self, conversation: &Conversation) {
        let queue = conversation.queue();
        self.events.emit_value(&json!({
            "type": "queue_update",
            "steering": queue.steering,
            "followUp": queue.follow_up,
        }));
    }

    /// Run `task` on its own thread, counted until it finishes.
    fn background(&self, task: impl FnOnce() + Send + 'static) {
        self.tasks.fetch_add(1, Ordering::SeqCst);
        let tasks = self.tasks.clone();
        std::thread::spawn(move || {
            task();
            tasks.fetch_sub(1, Ordering::SeqCst);
        });
    }

    /// Wait until no command, run or event stream is still in progress.
    pub fn wait_quiescent(&self) {
        while self.tasks.load(Ordering::SeqCst) > 0
            || self.conversation().is_running()
            || self.pump.pending() > 0
        {
            std::thread::sleep(Duration::from_millis(10));
        }
    }

    /// Abort the active run and wait for its events to be written.
    fn stop_run(&self, conversation: &Conversation) {
        if conversation.is_running() {
            conversation.abort();
            wait_until(STOP_TIMEOUT, || !conversation.is_running());
        }
        self.pump.wait_idle(STOP_TIMEOUT);
    }

    // ----- dispatch ----------------------------------------------------------

    /// Commands answered without waiting for earlier commands (`abort`,
    /// `abort_bash`, `abort_retry`); others are returned for the worker.
    pub fn handle_interrupt(self: &Arc<Self>, request: Request) -> Option<Request> {
        let id = request.id.clone();
        match request.command {
            Command::Abort => {
                let session = self.clone();
                self.background(move || {
                    let conversation = session.conversation();
                    let was_running = conversation.is_running();
                    conversation.clear_queue();
                    session.stop_run(&conversation);
                    session.respond(id.as_deref(), "abort", Ok(None));
                    if !was_running {
                        session.emit_queue(&conversation);
                    }
                });
                None
            }
            Command::AbortBash => {
                self.conversation().abort_bash();
                self.respond(id.as_deref(), "abort_bash", Ok(None));
                None
            }
            Command::AbortRetry => {
                if self.events.is_retrying() {
                    self.conversation().abort();
                }
                self.respond(id.as_deref(), "abort_retry", Ok(None));
                None
            }
            _ => Some(request),
        }
    }

    /// Handle one command in order.
    pub fn handle(self: &Arc<Self>, request: Request) {
        let Request { id, kind, command } = request;
        let id_ref = id.as_deref();
        let outcome = match command {
            Command::Prompt {
                message,
                images,
                streaming,
            } => return self.prompt(id, message, images, streaming),
            Command::Steer { message, images } => return self.queue(id, kind, message, images),
            Command::FollowUp { message, images } => {
                return self.queue(id, kind, message, images);
            }
            Command::Abort | Command::AbortBash | Command::AbortRetry => {
                let request = Request { id, kind, command };
                if let Some(request) = self.handle_interrupt(request) {
                    self.respond(
                        request.id.as_deref(),
                        kind,
                        Err(RpcError::internal("unexpected interrupt command")),
                    );
                }
                return;
            }
            Command::SetSessionName { name } => return self.set_session_name(id, name),
            Command::GetState => Ok(Some(self.state())),
            Command::GetMessages => Ok(Some(json!({
                "messages": self
                    .conversation()
                    .agent_messages()
                    .iter()
                    .map(message_json)
                    .collect::<Vec<_>>(),
            }))),
            Command::SetModel { provider, model_id } => self.set_model(&provider, &model_id),
            Command::CycleModel => self.cycle_model(),
            Command::GetAvailableModels => Ok(Some(json!({
                "models": models::available_models(self.engine(), true)
                    .iter()
                    .map(|m| models::model_json(self.engine(), &m.model_ref()))
                    .collect::<Vec<_>>(),
            }))),
            Command::SetThinkingLevel { level } => {
                self.set_thinking_level(level);
                Ok(None)
            }
            Command::CycleThinkingLevel => Ok(Some(self.cycle_thinking_level())),
            Command::SetSteeringMode { mode } => self.set_queue_mode(mode, false),
            Command::SetFollowUpMode { mode } => self.set_queue_mode(mode, true),
            Command::Compact {
                custom_instructions,
            } => self.compact(custom_instructions.as_deref()),
            Command::SetAutoCompaction { enabled } => self
                .engine()
                .set_auto_compaction(enabled)
                .map(|()| None)
                .map_err(RpcError::from),
            Command::SetAutoRetry { enabled } => self
                .engine()
                .set_auto_retry(enabled)
                .map(|()| None)
                .map_err(RpcError::from),
            Command::Bash { command } => self.bash(&command),
            Command::NewSession { parent_session } => self.new_session(parent_session),
            Command::SwitchSession { session_path } => self.switch_session(&session_path),
            Command::Fork { entry_id } => self.fork(&entry_id),
            Command::Clone => self.clone_session(),
            Command::GetSessionStats => Ok(Some(stats_json(&self.conversation().stats()))),
            Command::GetForkMessages => Ok(Some(json!({
                "messages": self
                    .conversation()
                    .user_messages_for_forking()
                    .into_iter()
                    .map(|(entry_id, text)| json!({"entryId": entry_id, "text": text}))
                    .collect::<Vec<_>>(),
            }))),
            Command::ExportHtml { output_path } => self.export_html(output_path.as_deref()),
            Command::GetLastAssistantText => Ok(Some(
                json!({"text": self.conversation().last_assistant_text()}),
            )),
            Command::GetCommands => Ok(Some(self.commands())),
            Command::Memory {
                action,
                params,
                confirm,
            } => self.memory(action, params, confirm),
            Command::Subagent { arguments, .. } => self.subagent(id_ref, arguments),
        };
        self.respond(id_ref, kind, outcome);
    }

    // ----- prompting -----------------------------------------------------------

    /// `(invocation name, registered name, arguments)` when `text` invokes
    /// an extension command.
    fn extension_command(&self, text: &str) -> Option<(String, String, String)> {
        crate::prompting::extension_command(self.engine(), text)
            .map(|c| (c.name, c.target, c.arguments))
    }

    /// Expand `/template args` and `/skill:name args`; other text is kept.
    fn expand(&self, text: &str) -> String {
        crate::prompting::expand(self.engine(), text)
    }

    /// Run an extension command (Pi `_tryExecuteExtensionCommand`): output
    /// lines become `notify` requests, a failure an `extension_error` event.
    fn run_extension_command(&self, name: &str, target: &str, arguments: &str) {
        match self.runtime.run_extension_command(target, arguments) {
            Ok(lines) => {
                for line in crate::prompting::command_output(target, lines) {
                    self.ui.notify(&line, NotifyKind::Info);
                }
            }
            Err(error) => self.events.emit_value(&json!({
                "type": "extension_error",
                "extensionPath": format!("command:{name}"),
                "event": "command",
                "error": format!("{error:#}"),
            })),
        }
    }

    /// `prompt`: extension commands run on their own thread (they may open
    /// dialogs) and are acknowledged when done; other input goes through
    /// the `input` hooks and template expansion, then starts a run, or is
    /// queued per `streamingBehavior` while one is active. The response is
    /// written before the run's events.
    fn prompt(
        self: &Arc<Self>,
        id: Option<String>,
        message: String,
        images: Vec<ContentBlock>,
        streaming: Option<StreamingBehavior>,
    ) {
        if let Some((name, target, arguments)) = self.extension_command(&message) {
            let session = self.clone();
            self.background(move || {
                session.run_extension_command(&name, &target, &arguments);
                session.respond(id.as_deref(), "prompt", Ok(None));
            });
            return;
        }
        match self.start_prompt(&message, images, streaming) {
            Ok(run) => {
                self.respond(id.as_deref(), "prompt", Ok(None));
                if let Some(run) = run {
                    self.pump.submit(run);
                }
            }
            Err(error) => self.respond(id.as_deref(), "prompt", Err(error)),
        }
    }

    fn start_prompt(
        &self,
        message: &str,
        images: Vec<ContentBlock>,
        streaming: Option<StreamingBehavior>,
    ) -> Result<Option<Receiver<AgentEvent>>, RpcError> {
        let (text, images) = match self
            .engine()
            .process_input(message, images, InputSource::Rpc)?
        {
            InputOutcome::Handled => return Ok(None),
            InputOutcome::Continue { text, images } => (text, images),
        };
        let text = self.expand(&text);
        let conversation = self.conversation();
        if conversation.is_running() {
            return self
                .queue_while_running(&conversation, &text, images, streaming)
                .map(|()| None);
        }
        match conversation.prompt(&text, images.clone()) {
            Ok(run) => Ok(Some(run)),
            Err(_) if conversation.is_running() && streaming.is_some() => self
                .queue_while_running(&conversation, &text, images, streaming)
                .map(|()| None),
            Err(error) => Err(error.into()),
        }
    }

    fn queue_while_running(
        &self,
        conversation: &Conversation,
        text: &str,
        images: Vec<ContentBlock>,
        streaming: Option<StreamingBehavior>,
    ) -> Result<(), RpcError> {
        match streaming {
            None => Err(RpcError::internal(
                "Agent is already processing. Specify streamingBehavior ('steer' or 'followUp') to queue the message.",
            )),
            Some(StreamingBehavior::Steer) => Ok(conversation.steer(text, images)?),
            Some(StreamingBehavior::FollowUp) => Ok(conversation.follow_up(text, images)?),
        }
    }

    /// `steer` / `follow_up`: queue a message (delivered by the active run,
    /// or with the next prompt when idle). Extension commands cannot be
    /// queued.
    fn queue(&self, id: Option<String>, kind: &str, message: String, images: Vec<ContentBlock>) {
        let conversation = self.conversation();
        let outcome = (|| {
            if let Some((name, _, _)) = self.extension_command(&message) {
                return Err(RpcError::internal(format!(
                    "Extension command \"/{name}\" cannot be queued. Use prompt() or execute the command when not streaming."
                )));
            }
            let text = self.expand(&message);
            let running = conversation.is_running();
            if kind == "follow_up" {
                conversation.follow_up(&text, images)?;
            } else {
                conversation.steer(&text, images)?;
            }
            Ok(running)
        })();
        match outcome {
            Ok(running) => {
                self.respond(id.as_deref(), kind, Ok(None));
                // A running agent reports the queue itself.
                if !running {
                    self.emit_queue(&conversation);
                }
            }
            Err(error) => self.respond(id.as_deref(), kind, Err(error)),
        }
    }

    // ----- state and models ----------------------------------------------------

    fn state(&self) -> Value {
        let conversation = self.conversation();
        let engine = self.engine();
        let harness = engine.harness_settings();
        let compacting = self.events.is_compacting();
        let mut out = Map::new();
        out.insert(
            "model".into(),
            conversation
                .model()
                .map(|m| models::model_json(engine, &m))
                .unwrap_or(Value::Null),
        );
        out.insert(
            "thinkingLevel".into(),
            json!(conversation.thinking().as_str()),
        );
        out.insert(
            "isStreaming".into(),
            json!(conversation.is_running() && !compacting),
        );
        out.insert("isCompacting".into(), json!(compacting));
        out.insert("steeringMode".into(), json!(harness.steering_mode.as_str()));
        out.insert(
            "followUpMode".into(),
            json!(harness.follow_up_mode.as_str()),
        );
        if conversation.is_persisted() {
            out.insert(
                "sessionFile".into(),
                json!(conversation.session_path().display().to_string()),
            );
        }
        out.insert("sessionId".into(), json!(conversation.session_id()));
        if let Some(name) = conversation.name() {
            out.insert("sessionName".into(), json!(name));
        }
        out.insert(
            "autoCompactionEnabled".into(),
            json!(harness.compaction.enabled),
        );
        out.insert(
            "messageCount".into(),
            json!(conversation.agent_messages().len()),
        );
        out.insert(
            "pendingMessageCount".into(),
            json!(conversation.queued_messages().len()),
        );
        Value::Object(out)
    }

    fn set_model(&self, provider: &str, model_id: &str) -> Outcome {
        let engine = self.engine();
        let Some(info) = models::find_available(engine, provider, model_id) else {
            return Err(RpcError::internal(format!(
                "Model not found: {provider}/{model_id}"
            )));
        };
        let model = info.model_ref();
        let conversation = self.conversation();
        conversation.select_model(model.clone(), "set");
        conversation.set_thinking(models::clamp_thinking(
            engine,
            &model,
            conversation.thinking(),
        ));
        engine.set_default_model(&model)?;
        Ok(Some(models::model_json(engine, &model)))
    }

    /// Pi `cycleModel`: through the `--models` / `enabledModels` scope when
    /// there is one, else every available model.
    fn cycle_model(&self) -> Outcome {
        let engine = self.engine();
        let available = models::available_models(engine, false);
        let mut scope: Vec<ScopedModel> = self.runtime.model_scope().to_vec();
        if scope.is_empty() {
            scope = engine.scoped_models(&available).models;
        }
        let scoped = !scope.is_empty();
        let candidates: Vec<(ModelRef, Option<ThinkingLevel>)> = if scoped {
            scope.into_iter().map(|s| (s.model, s.thinking)).collect()
        } else {
            available.iter().map(|m| (m.model_ref(), None)).collect()
        };
        if candidates.len() <= 1 {
            return Ok(Some(Value::Null));
        }
        let conversation = self.conversation();
        let current = conversation.model();
        let next = current
            .as_ref()
            .and_then(|c| {
                candidates
                    .iter()
                    .position(|(m, _)| m.provider == c.provider && m.id == c.id)
            })
            .map(|i| (i + 1) % candidates.len())
            .unwrap_or(0);
        let (model, thinking) = candidates[next].clone();
        conversation.select_model(model.clone(), "cycle");
        let level = models::clamp_thinking(
            engine,
            &model,
            thinking.unwrap_or_else(|| conversation.thinking()),
        );
        conversation.set_thinking(level);
        if !scoped {
            engine.set_default_model(&model)?;
        }
        Ok(Some(json!({
            "model": models::model_json(engine, &model),
            "thinkingLevel": level.as_str(),
            "isScoped": scoped,
        })))
    }

    fn set_thinking_level(&self, level: ThinkingLevel) {
        let conversation = self.conversation();
        let level = match conversation.model() {
            Some(model) => models::clamp_thinking(self.engine(), &model, level),
            None => level,
        };
        conversation.set_thinking(level);
    }

    fn cycle_thinking_level(&self) -> Value {
        let conversation = self.conversation();
        let Some(model) = conversation.model() else {
            return Value::Null;
        };
        match models::next_thinking_level(self.engine(), &model, conversation.thinking()) {
            Some(level) => {
                conversation.set_thinking(level);
                json!({"level": level.as_str()})
            }
            None => Value::Null,
        }
    }

    fn set_queue_mode(&self, mode: QueueMode, follow_up: bool) -> Outcome {
        let conversation = self.conversation();
        if follow_up {
            conversation.set_follow_up_mode(mode)?;
        } else {
            conversation.set_steering_mode(mode)?;
        }
        Ok(None)
    }

    // ----- compaction and bash ---------------------------------------------------

    /// Manual compaction (Pi `compact`): a running agent is aborted first;
    /// `compaction_start` / `compaction_end` are streamed and the result is
    /// the response data.
    fn compact(&self, instructions: Option<&str>) -> Outcome {
        let conversation = self.conversation();
        self.stop_run(&conversation);
        let run = conversation.compact(instructions)?;
        let mut result = None;
        let mut failure = None;
        while let Ok(event) = run.recv_blocking() {
            self.events.emit(&event);
            if let AgentEvent::CompactionEnd {
                result: done,
                aborted,
                error,
                ..
            } = &event
            {
                result = done.clone();
                failure = if *aborted {
                    Some("Compaction cancelled".to_string())
                } else {
                    error.clone()
                };
            }
        }
        match result {
            Some(result) => Ok(Some(compaction_json(&result))),
            None => Err(RpcError::internal(
                failure.unwrap_or_else(|| "Compaction failed".into()),
            )),
        }
    }

    /// `bash` (Pi `executeBash`): run a user command, recorded as a
    /// `bashExecution` message.
    fn bash(&self, command: &str) -> Outcome {
        let conversation = self.conversation();
        let bash = conversation.run_bash(command, false)?;
        let message = self
            .engine()
            .block_on(bash.result)
            .map_err(|e| RpcError::internal(e.to_string()))?;
        let mut out = json!({
            "output": message.output,
            "exitCode": message.exit_code,
            "cancelled": message.cancelled,
            "truncated": message.truncated,
        });
        if let Some(path) = &message.full_output_path {
            out["fullOutputPath"] = json!(path);
        }
        Ok(Some(out))
    }

    // ----- sessions ------------------------------------------------------------

    /// Replace the active conversation: the old one is shut down with
    /// `reason` and the new one bound.
    fn replace(&self, old: &Conversation, new: &Conversation, reason: &str) {
        let target = new.is_persisted().then(|| new.session_path());
        old.shutdown_with(reason, target.as_deref());
        self.attach(new);
    }

    fn previous_file(conversation: &Conversation) -> Option<PathBuf> {
        conversation
            .is_persisted()
            .then(|| conversation.session_path())
    }

    fn new_session(&self, parent_session: Option<String>) -> Outcome {
        let engine = self.engine();
        if !engine.allow_session_switch("new", None) {
            return Ok(Some(json!({"cancelled": true})));
        }
        let old = self.conversation();
        self.stop_run(&old);
        let new = engine.new_conversation_with_parent(
            old.model(),
            parent_session,
            Self::previous_file(&old),
        );
        new.set_thinking(old.thinking());
        self.replace(&old, &new, "new");
        Ok(Some(json!({"cancelled": false})))
    }

    fn switch_session(&self, session_path: &str) -> Outcome {
        let engine = self.engine();
        let path = crate::resolve(engine.cwd(), session_path);
        let path = PathBuf::from(path);
        if !path.exists() {
            return Err(RpcError::internal(format!(
                "Session file not found: {}",
                path.display()
            )));
        }
        if !engine.allow_session_switch("resume", Some(&path)) {
            return Ok(Some(json!({"cancelled": true})));
        }
        let old = self.conversation();
        self.stop_run(&old);
        let new = engine.switch_conversation(&path, Self::previous_file(&old))?;
        self.replace(&old, &new, "resume");
        Ok(Some(json!({"cancelled": false})))
    }

    fn fork(&self, entry_id: &str) -> Outcome {
        let old = self.conversation();
        match old.fork(entry_id, false) {
            Ok(fork) => {
                self.replace(&old, &fork.conversation, "fork");
                Ok(Some(json!({
                    "text": fork.selected_text.unwrap_or_default(),
                    "cancelled": false,
                })))
            }
            Err(error) if error.to_string() == "Fork cancelled by an extension" => {
                Ok(Some(json!({"cancelled": true})))
            }
            Err(error) => Err(error.into()),
        }
    }

    fn clone_session(&self) -> Outcome {
        let old = self.conversation();
        if old.leaf_id().is_none() {
            return Err(RpcError::internal(
                "Cannot clone session: no current entry selected",
            ));
        }
        let new = old.clone_session()?;
        self.replace(&old, &new, "fork");
        Ok(Some(json!({"cancelled": false})))
    }

    fn export_html(&self, output_path: Option<&str>) -> Outcome {
        let conversation = self.conversation();
        if !conversation.is_persisted() {
            return Err(RpcError::internal(
                "Cannot export HTML: no active session file",
            ));
        }
        let target = output_path.map(|p| PathBuf::from(crate::resolve(self.engine().cwd(), p)));
        let path = conversation.export_html(target.as_deref())?;
        Ok(Some(json!({"path": path.display().to_string()})))
    }

    fn set_session_name(&self, id: Option<String>, name: String) {
        let name = name.trim().to_string();
        if name.is_empty() {
            return self.respond(
                id.as_deref(),
                "set_session_name",
                Err(RpcError::internal("Session name cannot be empty")),
            );
        }
        let conversation = self.conversation();
        match conversation.set_name(&name) {
            Ok(()) => {
                self.respond(id.as_deref(), "set_session_name", Ok(None));
                self.events.emit_value(&json!({
                    "type": "session_info_changed",
                    "name": conversation.name(),
                }));
            }
            Err(error) => self.respond(id.as_deref(), "set_session_name", Err(error.into())),
        }
    }

    // ----- commands, memory, subagents -------------------------------------------

    /// Extension, prompt-template and skill commands (Pi `get_commands`).
    fn commands(&self) -> Value {
        let engine = self.engine();
        let registry = engine.slash_commands();
        let cwd = engine.cwd();
        let home = engine.home().root();
        let mut commands = Vec::new();
        for source in [
            SlashCommandSource::Extension,
            SlashCommandSource::Prompt,
            SlashCommandSource::Skill,
        ] {
            for command in registry.commands.iter().filter(|c| c.source == source) {
                let mut entry = json!({
                    "name": command.name,
                    "source": source.as_str(),
                    "sourceInfo": source_info(command.source_path.as_deref(), source.as_str(), cwd, home),
                });
                if !command.description.is_empty() {
                    entry["description"] = json!(command.description);
                }
                commands.push(entry);
            }
        }
        json!({"commands": commands, "diagnostics": registry.diagnostics})
    }

    fn memory(&self, action: String, params: Value, confirm: bool) -> Outcome {
        let context = self.runtime.memory_rpc_context();
        let command = qt_memory::rpc::RpcMemoryCommand {
            action,
            params,
            confirm,
        };
        qt_memory::rpc::handle_memory_rpc(&context, &command)
            .map(Some)
            .map_err(|e| RpcError::with_code(e.code, e.message))
    }

    /// Persistent-worker actions through the `subagent` tool (archived RPC
    /// `subagent` handler).
    fn subagent(&self, id: Option<&str>, arguments: Value) -> Outcome {
        let conversation = self.conversation();
        if !conversation.active_tools().iter().any(|t| t == "subagent") {
            return Err(RpcError::invalid(
                "subagent tool is disabled for this runtime",
            ));
        }
        let engine = self.engine();
        let tool = self.runtime.subagents().current_tool();
        let mut context = engine
            .tool_environment(None)
            .context(engine.cwd().to_path_buf());
        context.call_id = id.map(str::to_string);
        let output = engine.block_on(tool.execute(arguments, context));
        let text: String = output
            .content
            .iter()
            .filter_map(|b| match b {
                ContentBlock::Text { text } => Some(text.as_str()),
                _ => None,
            })
            .collect::<Vec<_>>()
            .join("\n");
        if output.is_error {
            return Err(RpcError::internal(if text.is_empty() {
                "subagent request failed".to_string()
            } else {
                text
            }));
        }
        Ok(Some(match output.details {
            Some(details) => details,
            None => serde_json::from_str(&text).unwrap_or(Value::String(text)),
        }))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hook_failures_are_parsed() {
        assert_eq!(
            hook_failure("extension tool_call hook failed: guard: field 'block' must be a boolean"),
            Some(("tool_call", "guard", "field 'block' must be a boolean"))
        );
        assert_eq!(hook_failure("[guard] stderr line"), None);
    }

    #[test]
    fn source_scopes() {
        let home = Path::new("/h");
        let cwd = Path::new("/w");
        assert_eq!(
            source_info(Some(Path::new("/h/prompts/a.md")), "prompt", cwd, home)["scope"],
            "user"
        );
        assert_eq!(
            source_info(Some(Path::new("/w/.pi/x.rhai")), "extension", cwd, home)["scope"],
            "project"
        );
        assert_eq!(
            source_info(None, "skill", cwd, home),
            json!({"path": "", "source": "skill", "scope": "temporary", "origin": "top-level"})
        );
    }

    #[test]
    fn stats_shape() {
        let stats = SessionStats {
            session_file: None,
            session_id: "s".into(),
            user_messages: 1,
            assistant_messages: 1,
            tool_calls: 0,
            tool_results: 0,
            total_messages: 2,
            tokens: qt_core::message::Usage {
                input_tokens: 10,
                output_tokens: 2,
                ..Default::default()
            },
            cost: 0.5,
            per_model: vec![qt_core::harness::stats::ModelUsage {
                model: ModelRef::new("p", "m"),
                responses: 1,
                usage: qt_core::message::Usage {
                    input_tokens: 10,
                    output_tokens: 2,
                    ..Default::default()
                },
            }],
            context_usage: Some(qt_core::harness::stats::ContextUsage {
                tokens: None,
                context_window: 100,
                percent: None,
            }),
        };
        let json = stats_json(&stats);
        assert_eq!(json["sessionFile"], Value::Null);
        assert_eq!(json["tokens"]["total"], 12);
        assert_eq!(json["models"]["p/m"]["assistantMessages"], 1);
        assert_eq!(
            json["contextUsage"],
            json!({"tokens": null, "contextWindow": 100, "percent": null})
        );
    }
}
