//! Hook registry shared by every conversation of an [`Engine`](crate::Engine).
//!
//! Extensions (the Rhai host) and the memory subsystem register callbacks
//! here; the agent loop and the conversation driver invoke them at fixed
//! points. Hooks are synchronous and run on the engine's runtime threads, so
//! they must return quickly; long work belongs on the caller's own threads.
//!
//! Composition rules when several hooks are registered for one point:
//! - transforming hooks (`context`, `model_context`, `before_provider_request`,
//!   `tool_result`) are chained in registration order, each seeing the
//!   previous output;
//! - `before_agent_start` concatenates injected messages and chains the
//!   system prompt;
//! - `tool_call` and `before_compact` stop at the first hook that blocks,
//!   cancels or supplies a compaction;
//! - observers (`after_provider_response`, `session`, `agent_event`) all run.

use std::path::PathBuf;
use std::sync::{Arc, RwLock};

use serde_json::Value;
use tokio_util::sync::CancellationToken;

use super::compaction::{CompactionPreparation, CompactionResult};
use crate::agent::AgentEvent;
use crate::message::{AgentMessage, AssistantMessage, ContentBlock, CustomMessage, Message};
use crate::provider::{ModelRef, Request};
use crate::session::SessionEntry;
use crate::tools::ToolOutput;

/// Handle returned by every registration; pass it to [`Hooks::remove`].
pub type HookId = u64;

/// A prompt is about to start the agent.
#[derive(Debug, Clone)]
pub struct BeforeAgentStartEvent {
    pub prompt: Message,
    pub system_prompt: String,
    pub cwd: PathBuf,
}

/// Messages to add after the prompt and an optional system prompt for this run.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct BeforeAgentStartResult {
    pub messages: Vec<CustomMessage>,
    pub system_prompt: Option<String>,
}

/// The agent context about to be converted for the provider. Changes affect
/// this request only; the session keeps the original messages.
#[derive(Debug, Clone)]
pub struct ContextEvent {
    pub messages: Vec<AgentMessage>,
    pub turn_index: u64,
}

/// The provider request after context hooks; augmentors (e.g. memory recall)
/// may add provider-only content that is never persisted.
#[derive(Debug, Clone)]
pub struct ModelContextEvent {
    pub model: ModelRef,
    pub request: Request,
    pub turn_index: u64,
    pub cancel: CancellationToken,
}

/// Last chance to change the request (model, prompt, tools, limits).
#[derive(Debug, Clone)]
pub struct ProviderRequestEvent {
    pub model: ModelRef,
    pub request: Request,
    pub turn_index: u64,
}

/// A provider response finished (successfully or not).
#[derive(Debug, Clone)]
pub struct ProviderResponseEvent {
    pub model: ModelRef,
    pub request: Request,
    pub response: AssistantMessage,
    pub turn_index: u64,
}

/// A tool call is about to run.
#[derive(Debug, Clone)]
pub struct ToolCallEvent {
    pub turn_index: u64,
    pub tool_call_id: String,
    pub tool_name: String,
    pub args: Value,
}

/// Block a tool call; the reason becomes the error result the model sees.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct ToolCallDecision {
    pub block: bool,
    pub reason: Option<String>,
}

/// A tool call finished.
#[derive(Debug, Clone)]
pub struct ToolResultEvent {
    pub turn_index: u64,
    pub tool_call_id: String,
    pub tool_name: String,
    pub args: Value,
    pub output: ToolOutput,
}

/// Replacement fields for a tool result (unset fields are kept).
#[derive(Debug, Clone, PartialEq, Default)]
pub struct ToolResultPatch {
    pub content: Option<Vec<ContentBlock>>,
    pub details: Option<Value>,
    pub is_error: Option<bool>,
}

/// Why a compaction runs.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CompactionReason {
    Manual,
    Threshold,
    Overflow,
}

impl CompactionReason {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Manual => "manual",
            Self::Threshold => "threshold",
            Self::Overflow => "overflow",
        }
    }
}

/// A compaction is about to summarize.
#[derive(Debug, Clone)]
pub struct BeforeCompactEvent {
    pub reason: CompactionReason,
    pub preparation: CompactionPreparation,
    pub branch_entries: Vec<SessionEntry>,
    pub custom_instructions: Option<String>,
}

/// Cancel the compaction or supply its result (marked `fromHook`).
#[derive(Debug, Clone, PartialEq, Default)]
pub struct BeforeCompactResult {
    pub cancel: bool,
    pub compaction: Option<CompactionResult>,
}

/// Why a session became active.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SessionStartReason {
    Startup,
    New,
    Resume,
    Fork,
    Clone,
    Import,
}

/// Session lifecycle notifications.
#[derive(Debug, Clone, PartialEq)]
pub enum SessionEvent {
    Start {
        reason: SessionStartReason,
        session_id: String,
        session_file: Option<PathBuf>,
        previous_session_file: Option<PathBuf>,
    },
    Shutdown {
        session_id: String,
        session_file: Option<PathBuf>,
    },
    Compact {
        session_id: String,
        entry_id: String,
        result: CompactionResult,
        from_hook: bool,
    },
    Tree {
        session_id: String,
        old_leaf_id: Option<String>,
        new_leaf_id: Option<String>,
        summary_entry_id: Option<String>,
    },
    NameChanged {
        session_id: String,
        name: Option<String>,
    },
}

type BeforeAgentStartHook =
    Arc<dyn Fn(&BeforeAgentStartEvent) -> Option<BeforeAgentStartResult> + Send + Sync>;
type ContextHook = Arc<dyn Fn(&ContextEvent) -> Option<Vec<AgentMessage>> + Send + Sync>;
type ModelContextHook = Arc<dyn Fn(&ModelContextEvent) -> Option<Request> + Send + Sync>;
type ProviderRequestHook = Arc<dyn Fn(&ProviderRequestEvent) -> Option<Request> + Send + Sync>;
type ProviderResponseHook = Arc<dyn Fn(&ProviderResponseEvent) + Send + Sync>;
type ToolCallHook = Arc<dyn Fn(&ToolCallEvent) -> Option<ToolCallDecision> + Send + Sync>;
type ToolResultHook = Arc<dyn Fn(&ToolResultEvent) -> Option<ToolResultPatch> + Send + Sync>;
type BeforeCompactHook =
    Arc<dyn Fn(&BeforeCompactEvent) -> Option<BeforeCompactResult> + Send + Sync>;
type SessionHook = Arc<dyn Fn(&SessionEvent) + Send + Sync>;
type AgentEventHook = Arc<dyn Fn(&AgentEvent) + Send + Sync>;

#[derive(Default)]
struct HookTable {
    next_id: HookId,
    before_agent_start: Vec<(HookId, BeforeAgentStartHook)>,
    context: Vec<(HookId, ContextHook)>,
    model_context: Vec<(HookId, ModelContextHook)>,
    before_provider_request: Vec<(HookId, ProviderRequestHook)>,
    after_provider_response: Vec<(HookId, ProviderResponseHook)>,
    tool_call: Vec<(HookId, ToolCallHook)>,
    tool_result: Vec<(HookId, ToolResultHook)>,
    before_compact: Vec<(HookId, BeforeCompactHook)>,
    session: Vec<(HookId, SessionHook)>,
    agent_event: Vec<(HookId, AgentEventHook)>,
}

/// Cheaply cloneable handle to the shared hook table.
#[derive(Clone, Default)]
pub struct Hooks {
    table: Arc<RwLock<HookTable>>,
}

impl std::fmt::Debug for Hooks {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("Hooks")
    }
}

macro_rules! registrar {
    ($(#[$doc:meta])* $name:ident, $field:ident, $ty:ty) => {
        $(#[$doc])*
        pub fn $name(&self, hook: $ty) -> HookId {
            let mut table = self.table.write().expect("hook table");
            table.next_id += 1;
            let id = table.next_id;
            table.$field.push((id, hook));
            id
        }
    };
}

fn snapshot<T: Clone>(list: &[(HookId, T)]) -> Vec<T> {
    list.iter().map(|(_, h)| h.clone()).collect()
}

impl Hooks {
    pub fn new() -> Self {
        Self::default()
    }

    registrar!(
        /// Inject messages or replace the system prompt when a prompt starts.
        add_before_agent_start, before_agent_start, BeforeAgentStartHook
    );
    registrar!(
        /// Transform the agent context before each provider request.
        add_context, context, ContextHook
    );
    registrar!(
        /// Add provider-only content to each request (never persisted).
        add_model_context_augmentor, model_context, ModelContextHook
    );
    registrar!(
        /// Rewrite the provider request just before it is sent.
        add_before_provider_request, before_provider_request, ProviderRequestHook
    );
    registrar!(
        /// Observe each provider response.
        add_after_provider_response, after_provider_response, ProviderResponseHook
    );
    registrar!(
        /// Inspect (and optionally block) each tool call.
        add_tool_call, tool_call, ToolCallHook
    );
    registrar!(
        /// Rewrite each tool result.
        add_tool_result, tool_result, ToolResultHook
    );
    registrar!(
        /// Cancel a compaction or provide its summary.
        add_before_compact, before_compact, BeforeCompactHook
    );
    registrar!(
        /// Observe session lifecycle events.
        add_session_listener, session, SessionHook
    );
    registrar!(
        /// Observe every agent event of every conversation.
        add_agent_event_listener, agent_event, AgentEventHook
    );

    /// Remove a hook by id; returns whether it existed.
    pub fn remove(&self, id: HookId) -> bool {
        let mut t = self.table.write().expect("hook table");
        let before = t.before_agent_start.len()
            + t.context.len()
            + t.model_context.len()
            + t.before_provider_request.len()
            + t.after_provider_response.len()
            + t.tool_call.len()
            + t.tool_result.len()
            + t.before_compact.len()
            + t.session.len()
            + t.agent_event.len();
        t.before_agent_start.retain(|(i, _)| *i != id);
        t.context.retain(|(i, _)| *i != id);
        t.model_context.retain(|(i, _)| *i != id);
        t.before_provider_request.retain(|(i, _)| *i != id);
        t.after_provider_response.retain(|(i, _)| *i != id);
        t.tool_call.retain(|(i, _)| *i != id);
        t.tool_result.retain(|(i, _)| *i != id);
        t.before_compact.retain(|(i, _)| *i != id);
        t.session.retain(|(i, _)| *i != id);
        t.agent_event.retain(|(i, _)| *i != id);
        let after = t.before_agent_start.len()
            + t.context.len()
            + t.model_context.len()
            + t.before_provider_request.len()
            + t.after_provider_response.len()
            + t.tool_call.len()
            + t.tool_result.len()
            + t.before_compact.len()
            + t.session.len()
            + t.agent_event.len();
        after < before
    }

    fn read(&self) -> std::sync::RwLockReadGuard<'_, HookTable> {
        self.table.read().expect("hook table")
    }

    pub fn has_before_compact(&self) -> bool {
        !self.read().before_compact.is_empty()
    }

    pub fn run_before_agent_start(&self, event: &BeforeAgentStartEvent) -> BeforeAgentStartResult {
        let hooks = snapshot(&self.read().before_agent_start);
        let mut out = BeforeAgentStartResult::default();
        let mut event = event.clone();
        for hook in hooks {
            if let Some(result) = hook(&event) {
                out.messages.extend(result.messages);
                if let Some(prompt) = result.system_prompt {
                    event.system_prompt = prompt.clone();
                    out.system_prompt = Some(prompt);
                }
            }
        }
        out
    }

    pub fn run_context(&self, messages: Vec<AgentMessage>, turn_index: u64) -> Vec<AgentMessage> {
        let hooks = snapshot(&self.read().context);
        let mut event = ContextEvent {
            messages,
            turn_index,
        };
        for hook in hooks {
            if let Some(messages) = hook(&event) {
                event.messages = messages;
            }
        }
        event.messages
    }

    pub fn run_model_context(
        &self,
        model: &ModelRef,
        request: Request,
        turn_index: u64,
        cancel: &CancellationToken,
    ) -> Request {
        let hooks = snapshot(&self.read().model_context);
        let mut event = ModelContextEvent {
            model: model.clone(),
            request,
            turn_index,
            cancel: cancel.clone(),
        };
        for hook in hooks {
            if let Some(request) = hook(&event) {
                event.request = request;
            }
        }
        event.request
    }

    pub fn run_before_provider_request(
        &self,
        model: &ModelRef,
        request: Request,
        turn_index: u64,
    ) -> Request {
        let hooks = snapshot(&self.read().before_provider_request);
        let mut event = ProviderRequestEvent {
            model: model.clone(),
            request,
            turn_index,
        };
        for hook in hooks {
            if let Some(request) = hook(&event) {
                event.request = request;
            }
        }
        event.request
    }

    pub fn run_after_provider_response(&self, event: &ProviderResponseEvent) {
        for hook in snapshot(&self.read().after_provider_response) {
            hook(event);
        }
    }

    /// The first blocking decision, if any.
    pub fn run_tool_call(&self, event: &ToolCallEvent) -> Option<ToolCallDecision> {
        snapshot(&self.read().tool_call)
            .into_iter()
            .find_map(|hook| hook(event).filter(|d| d.block))
    }

    pub fn run_tool_result(&self, mut event: ToolResultEvent) -> ToolOutput {
        for hook in snapshot(&self.read().tool_result) {
            if let Some(patch) = hook(&event) {
                if let Some(content) = patch.content {
                    event.output.content = content;
                }
                if let Some(details) = patch.details {
                    event.output.details = Some(details);
                }
                if let Some(is_error) = patch.is_error {
                    event.output.is_error = is_error;
                }
            }
        }
        event.output
    }

    pub fn run_before_compact(&self, event: &BeforeCompactEvent) -> BeforeCompactResult {
        snapshot(&self.read().before_compact)
            .into_iter()
            .find_map(|hook| hook(event).filter(|r| r.cancel || r.compaction.is_some()))
            .unwrap_or_default()
    }

    pub fn emit_session(&self, event: &SessionEvent) {
        for hook in snapshot(&self.read().session) {
            hook(event);
        }
    }

    pub fn emit_agent_event(&self, event: &AgentEvent) {
        let hooks = snapshot(&self.read().agent_event);
        for hook in hooks {
            hook(event);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::message::UserContent;

    #[test]
    fn chains_and_blocks_in_registration_order() {
        let hooks = Hooks::new();
        let a = hooks.add_before_agent_start(Arc::new(|e: &BeforeAgentStartEvent| {
            Some(BeforeAgentStartResult {
                messages: vec![CustomMessage::new(
                    "mem",
                    UserContent::Text("recall".into()),
                    false,
                )],
                system_prompt: Some(format!("{} +a", e.system_prompt)),
            })
        }));
        hooks.add_before_agent_start(Arc::new(|e: &BeforeAgentStartEvent| {
            Some(BeforeAgentStartResult {
                messages: Vec::new(),
                system_prompt: Some(format!("{} +b", e.system_prompt)),
            })
        }));
        let result = hooks.run_before_agent_start(&BeforeAgentStartEvent {
            prompt: Message::user_text("hi"),
            system_prompt: "base".into(),
            cwd: PathBuf::from("/"),
        });
        assert_eq!(result.system_prompt.as_deref(), Some("base +a +b"));
        assert_eq!(result.messages.len(), 1);

        hooks.add_tool_call(Arc::new(|e: &ToolCallEvent| {
            (e.tool_name == "bash").then(|| ToolCallDecision {
                block: true,
                reason: Some("no shell".into()),
            })
        }));
        let call = |name: &str| ToolCallEvent {
            turn_index: 0,
            tool_call_id: "1".into(),
            tool_name: name.into(),
            args: Value::Null,
        };
        assert_eq!(
            hooks
                .run_tool_call(&call("bash"))
                .unwrap()
                .reason
                .as_deref(),
            Some("no shell")
        );
        assert!(hooks.run_tool_call(&call("read")).is_none());

        hooks.add_tool_result(Arc::new(|_e: &ToolResultEvent| {
            Some(ToolResultPatch {
                content: Some(vec![ContentBlock::text("redacted")]),
                ..Default::default()
            })
        }));
        let out = hooks.run_tool_result(ToolResultEvent {
            turn_index: 0,
            tool_call_id: "1".into(),
            tool_name: "read".into(),
            args: Value::Null,
            output: ToolOutput::text("secret"),
        });
        assert_eq!(out.text_content(), "redacted");

        assert!(hooks.remove(a));
        assert!(!hooks.remove(a));
        let result = hooks.run_before_agent_start(&BeforeAgentStartEvent {
            prompt: Message::user_text("hi"),
            system_prompt: "base".into(),
            cwd: PathBuf::from("/"),
        });
        assert_eq!(result.system_prompt.as_deref(), Some("base +b"));
        assert!(!hooks.has_before_compact());
    }
}
