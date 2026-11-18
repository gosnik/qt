//! Wiring between a [`MemoryRuntime`] and the qt-core harness hooks
//! (archived `attach_rpc_memory`): the provider-only model-context
//! augmentor, capture of finished runs from agent events, foreground/idle
//! tracking and lifecycle session events.

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

use qt_core::agent::AgentEvent;
use qt_core::harness::hooks::ModelContextEvent;
use qt_core::harness::{HookId, Hooks, SessionEvent};
use qt_core::message::{ContentBlock, Message, StopReason};
use qt_core::provider::Request;
use serde_json::json;

use crate::coordinator::MemoryAugmentation;
use crate::prehandler::PrehandlerModel;
use crate::runtime::{CompletedEpisodeMetadata, MemoryRuntime};
use crate::text::{dump, stable_hash64};

/// Inserts `message` immediately before the newest user message (or appends
/// when there is none).
pub fn insert_before_current_user(messages: &mut Vec<Message>, message: Message) {
    let position = messages
        .iter()
        .rposition(|item| matches!(item, Message::User { .. }))
        .unwrap_or(messages.len());
    messages.insert(position, message);
}

/// Logical input of a request: the number of user messages, and an epoch
/// identifying the earlier inputs (changes when history is rewritten).
pub fn logical_input(messages: &[Message]) -> (u64, u64) {
    let users: Vec<String> = messages
        .iter()
        .filter_map(|message| match message {
            Message::User { content, .. } => Some(content.plain_text()),
            _ => None,
        })
        .collect();
    let earlier = users.len().saturating_sub(1);
    let epoch = stable_hash64(&dump(&json!(users[..earlier])));
    (users.len() as u64, epoch)
}

/// Applies the coordinator to one provider request; `None` leaves it unchanged
/// (a failed augmentation also proceeds without memory: fail-open).
pub fn augment_request(
    runtime: &MemoryRuntime,
    event: &ModelContextEvent,
    run: u64,
) -> Option<Request> {
    try_augment_request(runtime, event, run).ok().flatten()
}

/// [`augment_request`] reporting failures, for `memory.prehandler.failOpen =
/// false`: the request is aborted with `memory recall failed: <error>`.
pub fn try_augment_request(
    runtime: &MemoryRuntime,
    event: &ModelContextEvent,
    run: u64,
) -> std::result::Result<Option<Request>, String> {
    let (sequence, epoch) = logical_input(&event.request.messages);
    if sequence == 0 {
        return Ok(None);
    }
    let cancel = event.cancel.clone();
    let model = PrehandlerModel {
        provider: event.model.provider.clone(),
        api: String::new(),
        name: event.model.id.clone(),
    };
    let augmentation: MemoryAugmentation = runtime
        .coordinator()
        .augment(
            epoch,
            sequence,
            &model,
            &event.request.messages,
            Some(Arc::new(move || cancel.is_cancelled())),
        )
        .map_err(|error| format!("memory recall failed: {error}"))?;
    runtime.note_recall(run, &augmentation.contribution.memory_ids);
    let Some(message) = augmentation.message else {
        return Ok(None);
    };
    let mut request = event.request.clone();
    insert_before_current_user(&mut request.messages, message);
    Ok(Some(request))
}

#[derive(Default)]
struct RunState {
    messages: Vec<Message>,
    tool_requests: usize,
    tool_errors: usize,
}

/// Collects one run's messages from agent events and hands finished runs to
/// the runtime.
pub struct EpisodeBridge {
    runtime: Arc<MemoryRuntime>,
    session_id: String,
    run: AtomicU64,
    episodes: AtomicU64,
    lifecycle: AtomicU64,
    state: Mutex<RunState>,
}

impl EpisodeBridge {
    pub fn new(runtime: Arc<MemoryRuntime>, session_id: impl Into<String>) -> Self {
        Self {
            runtime,
            session_id: session_id.into(),
            run: AtomicU64::new(0),
            episodes: AtomicU64::new(0),
            lifecycle: AtomicU64::new(0),
            state: Mutex::new(RunState::default()),
        }
    }

    /// Run counter used as the turn index for recall attribution.
    pub fn current_run(&self) -> u64 {
        self.run.load(Ordering::SeqCst)
    }

    fn lifecycle_event(&self, event_type: &str, mut payload: serde_json::Value) {
        let sequence = self.lifecycle.fetch_add(1, Ordering::SeqCst) + 1;
        payload["sequence"] = json!(sequence);
        self.runtime.record_lifecycle_event(
            event_type,
            "qt:harness",
            &dump(&payload),
            &format!("{}:rpc:{event_type}:{sequence}", self.session_id),
        );
    }

    pub fn on_agent_event(&self, event: &AgentEvent) {
        match event {
            AgentEvent::AgentStart => {
                self.run.fetch_add(1, Ordering::SeqCst);
                *self.state.lock().unwrap_or_else(|p| p.into_inner()) = RunState::default();
                self.runtime.set_foreground_active(true);
            }
            AgentEvent::MessageEnd(message) => {
                let mut state = self.state.lock().unwrap_or_else(|p| p.into_inner());
                match message {
                    Message::Assistant(assistant) => {
                        state.tool_requests += assistant
                            .content
                            .iter()
                            .filter(|block| matches!(block, ContentBlock::ToolCall { .. }))
                            .count();
                    }
                    Message::ToolResult(result) if result.is_error => state.tool_errors += 1,
                    _ => {}
                }
                state.messages.push(message.clone());
            }
            AgentEvent::CompactionStart { reason } => {
                self.runtime.set_foreground_active(true);
                self.lifecycle_event("compaction_start", json!({"reason": reason.as_str()}));
            }
            AgentEvent::CompactionEnd {
                reason,
                aborted,
                will_retry,
                error,
                ..
            } => {
                self.runtime.set_foreground_active(false);
                self.lifecycle_event(
                    "compaction_end",
                    json!({
                        "reason": reason.as_str(), "aborted": aborted, "will_retry": will_retry,
                        "error": error.clone().unwrap_or_default(),
                    }),
                );
            }
            AgentEvent::AgentEnd { usage, aborted } => {
                self.runtime.set_foreground_active(false);
                let state =
                    std::mem::take(&mut *self.state.lock().unwrap_or_else(|p| p.into_inner()));
                let last = state
                    .messages
                    .iter()
                    .rev()
                    .find_map(|message| match message {
                        Message::Assistant(assistant) => Some(assistant),
                        _ => None,
                    });
                let failed =
                    last.is_some_and(|assistant| assistant.stop_reason == StopReason::Error);
                let metadata = CompletedEpisodeMetadata {
                    ok: !aborted && !failed,
                    turn_index: self.current_run(),
                    tool_request_count: state.tool_requests,
                    tool_error_count: state.tool_errors,
                    error: last
                        .and_then(|assistant| assistant.error_message.clone())
                        .unwrap_or_default(),
                    model_provider: last
                        .map(|assistant| assistant.provider.clone())
                        .unwrap_or_default(),
                    model_id: last
                        .map(|assistant| assistant.model.clone())
                        .unwrap_or_default(),
                    input_tokens: usage.input_tokens as i64,
                    output_tokens: usage.output_tokens as i64,
                    recalled_memory_ids: Vec::new(),
                };
                if *aborted {
                    self.lifecycle_event(
                        "agent_abort",
                        json!({"outcome_kind": "aborted", "cleared_steer_count": 0, "cleared_follow_up_count": 0}),
                    );
                }
                if state.messages.is_empty() {
                    self.lifecycle_event(
                        "agent_end",
                        json!({
                            "outcome_kind": if metadata.tool_error_count != 0 { "tool_failure" }
                                else if metadata.ok { "completed" } else { "provider_failure" },
                            "ok": metadata.ok, "turn_index": metadata.turn_index,
                            "tool_request_count": metadata.tool_request_count,
                            "tool_error_count": metadata.tool_error_count, "error": metadata.error,
                        }),
                    );
                    return;
                }
                let episode = self.episodes.fetch_add(1, Ordering::SeqCst) + 1;
                self.runtime.enqueue_completed_episode(
                    &state.messages,
                    &format!("qt-session://{}/rpc-episode/{episode}", self.session_id),
                    metadata,
                );
            }
            _ => {}
        }
    }

    pub fn on_session_event(&self, event: &SessionEvent) {
        match event {
            SessionEvent::Start {
                reason,
                session_id,
                previous_session_file,
                ..
            } => {
                self.runtime.set_session_context(session_id, session_id);
                self.lifecycle_event(
                    "session_start",
                    json!({
                        "reason": format!("{reason:?}").to_ascii_lowercase(),
                        "previous_session_file": previous_session_file
                            .as_ref().map(|path| path.to_string_lossy().into_owned()).unwrap_or_default(),
                    }),
                );
            }
            SessionEvent::Shutdown { session_file, .. } => self.lifecycle_event(
                "session_shutdown",
                json!({
                    "reason": "shutdown",
                    "target_session_file": session_file
                        .as_ref().map(|path| path.to_string_lossy().into_owned()).unwrap_or_default(),
                }),
            ),
            SessionEvent::Tree {
                old_leaf_id,
                new_leaf_id,
                ..
            } => self.lifecycle_event(
                "session_tree",
                json!({
                    "new_leaf_id": new_leaf_id.clone().unwrap_or_default(),
                    "old_leaf_id": old_leaf_id.clone().unwrap_or_default(),
                    "from_extension": false,
                }),
            ),
            _ => {}
        }
    }
}

/// Registered hook ids; pass them to [`detach_memory`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MemoryHookIds(pub Vec<HookId>);

/// Registers memory on an engine's hooks: the model-context augmentor
/// (when automatic recall is enabled), the capture bridge and lifecycle
/// listeners. Returns the bridge and hook ids.
pub fn attach_memory(
    hooks: &Hooks,
    runtime: Arc<MemoryRuntime>,
    session_id: &str,
) -> (Arc<EpisodeBridge>, MemoryHookIds) {
    runtime.set_session_context(session_id, session_id);
    let bridge = Arc::new(EpisodeBridge::new(Arc::clone(&runtime), session_id));
    let mut ids = Vec::new();
    let settings = runtime.coordinator().settings();
    if settings.enabled && settings.automatic_recall {
        let augment_runtime = Arc::clone(&runtime);
        let augment_bridge = Arc::clone(&bridge);
        if settings.fail_open {
            ids.push(hooks.add_model_context_augmentor(Arc::new(
                move |event: &ModelContextEvent| {
                    augment_request(&augment_runtime, event, augment_bridge.current_run())
                },
            )));
        } else {
            ids.push(hooks.add_fallible_model_context_augmentor(Arc::new(
                move |event: &ModelContextEvent| {
                    try_augment_request(&augment_runtime, event, augment_bridge.current_run())
                },
            )));
        }
    }
    let agent_bridge = Arc::clone(&bridge);
    ids.push(
        hooks.add_agent_event_listener(Arc::new(move |event: &AgentEvent| {
            agent_bridge.on_agent_event(event);
        })),
    );
    let session_bridge = Arc::clone(&bridge);
    ids.push(
        hooks.add_session_listener(Arc::new(move |event: &SessionEvent| {
            session_bridge.on_session_event(event);
        })),
    );
    (bridge, MemoryHookIds(ids))
}

/// Removes hooks registered by [`attach_memory`].
pub fn detach_memory(hooks: &Hooks, ids: &MemoryHookIds) {
    for id in &ids.0 {
        hooks.remove(*id);
    }
}
