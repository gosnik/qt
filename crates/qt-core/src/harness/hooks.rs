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

/// Block a tool call (the reason becomes the error result the model sees)
/// or replace its arguments (Pi handlers mutate `event.input`).
#[derive(Debug, Clone, PartialEq, Default)]
pub struct ToolCallDecision {
    pub block: bool,
    pub reason: Option<String>,
    /// Replacement arguments for the call; later hooks see them.
    pub args: Option<Value>,
}

/// What the `tool_call` chain decided: blocked with a reason, or run with
/// the (possibly rewritten) arguments.
#[derive(Debug, Clone, PartialEq)]
pub enum ToolCallOutcome {
    Run(Value),
    Blocked(Option<String>),
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
        /// Pi `session_shutdown` reason (`quit`, `reload`, `new`, `resume`,
        /// `fork`).
        reason: String,
        /// The session being switched to, when the reason is a switch.
        target_session_file: Option<PathBuf>,
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

/// Where user input came from (Pi `InputSource`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InputSource {
    Interactive,
    Rpc,
    Extension,
}

impl InputSource {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Interactive => "interactive",
            Self::Rpc => "rpc",
            Self::Extension => "extension",
        }
    }
}

/// User input about to be submitted (Pi `input` event).
#[derive(Debug, Clone, PartialEq)]
pub struct InputEvent {
    pub text: String,
    pub images: Vec<ContentBlock>,
    pub source: InputSource,
}

/// Result of the `input` chain.
#[derive(Debug, Clone, PartialEq)]
pub enum InputOutcome {
    /// Submit this (possibly transformed) input.
    Continue {
        text: String,
        images: Vec<ContentBlock>,
    },
    /// A hook consumed the input; nothing is submitted.
    Handled,
}

/// A session switch (`new`, `resume`) is about to happen.
#[derive(Debug, Clone, PartialEq)]
pub struct SessionBeforeSwitchEvent {
    pub reason: String,
    pub target_session_file: Option<PathBuf>,
}

/// A fork is about to be created (`position`: `before` or `at`).
#[derive(Debug, Clone, PartialEq)]
pub struct SessionBeforeForkEvent {
    pub entry_id: String,
    pub position: String,
}

/// Cancel a fork or skip restoring the selected message into the editor.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct BeforeForkResult {
    pub cancel: bool,
    pub skip_conversation_restore: bool,
}

/// A tree navigation is about to happen (Pi `TreePreparation`).
#[derive(Debug, Clone, PartialEq)]
pub struct SessionBeforeTreeEvent {
    pub target_id: String,
    pub old_leaf_id: Option<String>,
    pub user_wants_summary: bool,
    pub custom_instructions: Option<String>,
    pub replace_instructions: bool,
    pub label: Option<String>,
    /// Entries that would be summarized.
    pub entries_to_summarize: Vec<SessionEntry>,
}

/// Overrides from `session_before_tree` handlers (unset fields are kept).
#[derive(Debug, Clone, PartialEq, Default)]
pub struct BeforeTreeResult {
    pub cancel: bool,
    /// A summary supplied by the hook: `{summary, details?}`.
    pub summary: Option<Value>,
    pub custom_instructions: Option<String>,
    pub replace_instructions: Option<bool>,
    pub label: Option<String>,
}

/// A model was selected (`source`: `set`, `cycle`, `restore`).
#[derive(Debug, Clone, PartialEq)]
pub struct ModelSelectEvent {
    pub model: ModelRef,
    pub previous: Option<ModelRef>,
    pub source: String,
}

/// The thinking level changed.
#[derive(Debug, Clone, PartialEq)]
pub struct ThinkingLevelSelectEvent {
    pub level: String,
    pub previous: String,
}

/// A user `!cmd` / `!!cmd` is about to run.
#[derive(Debug, Clone, PartialEq)]
pub struct UserBashEvent {
    pub command: String,
    pub exclude_from_context: bool,
    pub cwd: PathBuf,
}

/// Resources are being (re)discovered (`reason`: `startup` or `reload`).
#[derive(Debug, Clone, PartialEq)]
pub struct ResourcesDiscoverEvent {
    pub cwd: PathBuf,
    pub reason: String,
}

/// Extra resource paths contributed by `resources_discover` hooks.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct DiscoveredResourcePaths {
    pub skill_paths: Vec<String>,
    pub prompt_paths: Vec<String>,
    pub theme_paths: Vec<String>,
}

/// Instruction files (AGENTS.md, CLAUDE.md) were loaded into the prompt.
#[derive(Debug, Clone, PartialEq)]
pub struct InstructionsLoadedEvent {
    pub files: Vec<PathBuf>,
    pub reason: String,
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
/// A model-context augmentor that may fail; an error aborts the request
/// (fail-closed memory pre-handler).
pub type FallibleModelContextHook =
    Arc<dyn Fn(&ModelContextEvent) -> Result<Option<Request>, String> + Send + Sync>;
type InputHook = Arc<dyn Fn(&InputEvent) -> Result<Option<InputOutcome>, String> + Send + Sync>;
type BeforeSwitchHook = Arc<dyn Fn(&SessionBeforeSwitchEvent) -> bool + Send + Sync>;
type BeforeForkHook =
    Arc<dyn Fn(&SessionBeforeForkEvent) -> Option<BeforeForkResult> + Send + Sync>;
type BeforeTreeHook =
    Arc<dyn Fn(&SessionBeforeTreeEvent) -> Option<BeforeTreeResult> + Send + Sync>;
type ModelSelectHook = Arc<dyn Fn(&ModelSelectEvent) + Send + Sync>;
type ThinkingSelectHook = Arc<dyn Fn(&ThinkingLevelSelectEvent) + Send + Sync>;
type UserBashHook = Arc<dyn Fn(&UserBashEvent) + Send + Sync>;
type ResourcesDiscoverHook =
    Arc<dyn Fn(&ResourcesDiscoverEvent) -> Option<DiscoveredResourcePaths> + Send + Sync>;
type InstructionsLoadedHook = Arc<dyn Fn(&InstructionsLoadedEvent) + Send + Sync>;
type MessageEndHook = Arc<dyn Fn(&AgentMessage) -> Option<AgentMessage> + Send + Sync>;
type ConversationHook = Arc<dyn Fn(&crate::engine::Conversation) + Send + Sync>;

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
    fallible_model_context: Vec<(HookId, FallibleModelContextHook)>,
    input: Vec<(HookId, InputHook)>,
    before_switch: Vec<(HookId, BeforeSwitchHook)>,
    before_fork: Vec<(HookId, BeforeForkHook)>,
    before_tree: Vec<(HookId, BeforeTreeHook)>,
    model_select: Vec<(HookId, ModelSelectHook)>,
    thinking_select: Vec<(HookId, ThinkingSelectHook)>,
    user_bash: Vec<(HookId, UserBashHook)>,
    resources_discover: Vec<(HookId, ResourcesDiscoverHook)>,
    instructions_loaded: Vec<(HookId, InstructionsLoadedHook)>,
    message_end: Vec<(HookId, MessageEndHook)>,
    conversation: Vec<(HookId, ConversationHook)>,
}

fn remove_from<T>(list: &mut Vec<(HookId, T)>, id: HookId) -> usize {
    let before = list.len();
    list.retain(|(i, _)| *i != id);
    before - list.len()
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
    registrar!(
        /// Add provider-only content to each request; an `Err` aborts the
        /// request with that message (fail-closed augmentors).
        add_fallible_model_context_augmentor, fallible_model_context, FallibleModelContextHook
    );
    registrar!(
        /// Transform or consume user input before it is submitted (`input`).
        add_input, input, InputHook
    );
    registrar!(
        /// Veto a session switch (`session_before_switch`); `true` cancels.
        add_session_before_switch, before_switch, BeforeSwitchHook
    );
    registrar!(
        /// Veto a fork (`session_before_fork`).
        add_session_before_fork, before_fork, BeforeForkHook
    );
    registrar!(
        /// Veto or adjust a tree navigation (`session_before_tree`).
        add_session_before_tree, before_tree, BeforeTreeHook
    );
    registrar!(
        /// Observe model selection (`model_select`).
        add_model_select, model_select, ModelSelectHook
    );
    registrar!(
        /// Observe thinking level changes (`thinking_level_select`).
        add_thinking_level_select, thinking_select, ThinkingSelectHook
    );
    registrar!(
        /// Observe user `!cmd` commands (`user_bash`).
        add_user_bash, user_bash, UserBashHook
    );
    registrar!(
        /// Contribute skill, prompt and theme paths (`resources_discover`).
        add_resources_discover, resources_discover, ResourcesDiscoverHook
    );
    registrar!(
        /// Observe loaded instruction files (`instructions_loaded`).
        add_instructions_loaded, instructions_loaded, InstructionsLoadedHook
    );
    registrar!(
        /// Replace a finalized message before it is recorded (`message_end`);
        /// a replacement with a different role is ignored.
        add_message_end, message_end, MessageEndHook
    );
    registrar!(
        /// Observe every conversation the engine creates (new, resumed,
        /// forked, cloned, imported) so a composition root can bind
        /// per-conversation services to it.
        add_conversation_listener, conversation, ConversationHook
    );

    /// Remove a hook by id; returns whether it existed.
    pub fn remove(&self, id: HookId) -> bool {
        let mut guard = self.table.write().expect("hook table");
        let t = &mut *guard;
        let removed = remove_from(&mut t.before_agent_start, id)
            + remove_from(&mut t.context, id)
            + remove_from(&mut t.model_context, id)
            + remove_from(&mut t.before_provider_request, id)
            + remove_from(&mut t.after_provider_response, id)
            + remove_from(&mut t.tool_call, id)
            + remove_from(&mut t.tool_result, id)
            + remove_from(&mut t.before_compact, id)
            + remove_from(&mut t.session, id)
            + remove_from(&mut t.agent_event, id)
            + remove_from(&mut t.fallible_model_context, id)
            + remove_from(&mut t.input, id)
            + remove_from(&mut t.before_switch, id)
            + remove_from(&mut t.before_fork, id)
            + remove_from(&mut t.before_tree, id)
            + remove_from(&mut t.model_select, id)
            + remove_from(&mut t.thinking_select, id)
            + remove_from(&mut t.user_bash, id)
            + remove_from(&mut t.resources_discover, id)
            + remove_from(&mut t.instructions_loaded, id)
            + remove_from(&mut t.message_end, id)
            + remove_from(&mut t.conversation, id);
        removed > 0
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

    /// Whether any model-context augmentor is registered.
    pub fn has_model_context(&self) -> bool {
        let t = self.read();
        !t.model_context.is_empty() || !t.fallible_model_context.is_empty()
    }

    /// Run the model-context augmentors (infallible first, then fallible).
    /// An `Err` from a fallible augmentor aborts the request.
    pub fn run_model_context(
        &self,
        model: &ModelRef,
        request: Request,
        turn_index: u64,
        cancel: &CancellationToken,
    ) -> Result<Request, String> {
        let (hooks, fallible) = {
            let t = self.read();
            (
                snapshot(&t.model_context),
                snapshot(&t.fallible_model_context),
            )
        };
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
        for hook in fallible {
            if let Some(request) = hook(&event)? {
                event.request = request;
            }
        }
        Ok(event.request)
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
        match self.run_tool_call_chain(event) {
            ToolCallOutcome::Blocked(reason) => Some(ToolCallDecision {
                block: true,
                reason,
                args: None,
            }),
            ToolCallOutcome::Run(_) => None,
        }
    }

    /// Run the `tool_call` hooks in order: argument replacements are chained
    /// and the first block stops the chain.
    pub fn run_tool_call_chain(&self, event: &ToolCallEvent) -> ToolCallOutcome {
        let mut event = event.clone();
        for hook in snapshot(&self.read().tool_call) {
            if let Some(decision) = hook(&event) {
                if decision.block {
                    return ToolCallOutcome::Blocked(decision.reason);
                }
                if let Some(args) = decision.args {
                    event.args = args;
                }
            }
        }
        ToolCallOutcome::Run(event.args)
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

    /// Run the `input` chain. A failing hook stops it with its error.
    pub fn run_input(
        &self,
        text: &str,
        images: Vec<ContentBlock>,
        source: InputSource,
    ) -> Result<InputOutcome, String> {
        let mut event = InputEvent {
            text: text.to_string(),
            images,
            source,
        };
        for hook in snapshot(&self.read().input) {
            match hook(&event)? {
                None => {}
                Some(InputOutcome::Handled) => return Ok(InputOutcome::Handled),
                Some(InputOutcome::Continue { text, images }) => {
                    event.text = text;
                    event.images = images;
                }
            }
        }
        Ok(InputOutcome::Continue {
            text: event.text,
            images: event.images,
        })
    }

    /// `true` when a hook cancelled the switch.
    pub fn run_session_before_switch(&self, event: &SessionBeforeSwitchEvent) -> bool {
        snapshot(&self.read().before_switch)
            .into_iter()
            .any(|hook| hook(event))
    }

    /// The first cancelling result, else the merged skip flag.
    pub fn run_session_before_fork(&self, event: &SessionBeforeForkEvent) -> BeforeForkResult {
        let mut out = BeforeForkResult::default();
        for hook in snapshot(&self.read().before_fork) {
            if let Some(result) = hook(event) {
                out.skip_conversation_restore |= result.skip_conversation_restore;
                if result.cancel {
                    out.cancel = true;
                    break;
                }
            }
        }
        out
    }

    /// Later hooks override earlier fields; a cancel stops the chain.
    pub fn run_session_before_tree(&self, event: &SessionBeforeTreeEvent) -> BeforeTreeResult {
        let mut out = BeforeTreeResult::default();
        for hook in snapshot(&self.read().before_tree) {
            if let Some(result) = hook(event) {
                if result.summary.is_some() {
                    out.summary = result.summary;
                }
                if result.custom_instructions.is_some() {
                    out.custom_instructions = result.custom_instructions;
                }
                if result.replace_instructions.is_some() {
                    out.replace_instructions = result.replace_instructions;
                }
                if result.label.is_some() {
                    out.label = result.label;
                }
                if result.cancel {
                    out.cancel = true;
                    break;
                }
            }
        }
        out
    }

    pub fn emit_model_select(&self, event: &ModelSelectEvent) {
        for hook in snapshot(&self.read().model_select) {
            hook(event);
        }
    }

    pub fn emit_thinking_level_select(&self, event: &ThinkingLevelSelectEvent) {
        for hook in snapshot(&self.read().thinking_select) {
            hook(event);
        }
    }

    pub fn emit_user_bash(&self, event: &UserBashEvent) {
        for hook in snapshot(&self.read().user_bash) {
            hook(event);
        }
    }

    /// Paths contributed by every `resources_discover` hook, in order.
    pub fn run_resources_discover(
        &self,
        event: &ResourcesDiscoverEvent,
    ) -> DiscoveredResourcePaths {
        let mut out = DiscoveredResourcePaths::default();
        for hook in snapshot(&self.read().resources_discover) {
            if let Some(paths) = hook(event) {
                out.skill_paths.extend(paths.skill_paths);
                out.prompt_paths.extend(paths.prompt_paths);
                out.theme_paths.extend(paths.theme_paths);
            }
        }
        out
    }

    pub fn emit_instructions_loaded(&self, event: &InstructionsLoadedEvent) {
        for hook in snapshot(&self.read().instructions_loaded) {
            hook(event);
        }
    }

    /// Whether any `message_end` replacement hook is registered.
    pub fn has_message_end(&self) -> bool {
        !self.read().message_end.is_empty()
    }

    /// Apply `message_end` replacements; a replacement must keep the role.
    pub fn run_message_end(&self, message: AgentMessage) -> AgentMessage {
        let hooks = snapshot(&self.read().message_end);
        let mut current = message;
        for hook in hooks {
            if let Some(replacement) = hook(&current)
                && agent_message_role(&replacement) == agent_message_role(&current)
            {
                current = replacement;
            }
        }
        current
    }

    pub fn emit_conversation_created(&self, conversation: &crate::engine::Conversation) {
        for hook in snapshot(&self.read().conversation) {
            hook(conversation);
        }
    }

    pub fn emit_agent_event(&self, event: &AgentEvent) {
        let hooks = snapshot(&self.read().agent_event);
        for hook in hooks {
            hook(event);
        }
    }
}

/// Pi role name of an agent message.
pub fn agent_message_role(message: &AgentMessage) -> &'static str {
    match message {
        AgentMessage::Llm(Message::User { .. }) => "user",
        AgentMessage::Llm(Message::Assistant(_)) => "assistant",
        AgentMessage::Llm(Message::ToolResult(_)) => "toolResult",
        AgentMessage::Custom(_) => "custom",
        AgentMessage::BashExecution(_) => "bashExecution",
        AgentMessage::BranchSummary(_) => "branchSummary",
        AgentMessage::CompactionSummary(_) => "compactionSummary",
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
                args: None,
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

    #[test]
    fn lifecycle_hooks_chain_and_replace() {
        let hooks = Hooks::new();
        hooks.add_tool_call(Arc::new(|e: &ToolCallEvent| {
            Some(ToolCallDecision {
                args: Some(serde_json::json!({"path": format!("{}!", e.args["path"].as_str().unwrap_or(""))})),
                ..Default::default()
            })
        }));
        let event = ToolCallEvent {
            turn_index: 0,
            tool_call_id: "1".into(),
            tool_name: "read".into(),
            args: serde_json::json!({"path": "a"}),
        };
        assert_eq!(
            hooks.run_tool_call_chain(&event),
            ToolCallOutcome::Run(serde_json::json!({"path": "a!"}))
        );
        assert!(hooks.run_tool_call(&event).is_none());

        let input = hooks.add_input(Arc::new(|e: &InputEvent| {
            Ok(match e.text.as_str() {
                "eat" => Some(InputOutcome::Handled),
                "fail" => return Err("boom".into()),
                _ => Some(InputOutcome::Continue {
                    text: e.text.to_uppercase(),
                    images: e.images.clone(),
                }),
            })
        }));
        assert_eq!(
            hooks.run_input("hi", vec![], InputSource::Interactive),
            Ok(InputOutcome::Continue {
                text: "HI".into(),
                images: vec![]
            })
        );
        assert_eq!(
            hooks.run_input("eat", vec![], InputSource::Rpc),
            Ok(InputOutcome::Handled)
        );
        assert_eq!(
            hooks.run_input("fail", vec![], InputSource::Rpc),
            Err("boom".into())
        );
        assert!(hooks.remove(input));

        hooks.add_message_end(Arc::new(|m: &AgentMessage| match m {
            AgentMessage::Llm(Message::Assistant(a)) => {
                let mut a = a.clone();
                a.content = vec![ContentBlock::text("replaced")];
                Some(AgentMessage::Llm(Message::Assistant(a)))
            }
            AgentMessage::Llm(Message::User { .. }) => Some(AgentMessage::Llm(Message::Assistant(
                AssistantMessage::default(),
            ))),
            _ => None,
        }));
        let replaced = hooks.run_message_end(AgentMessage::Llm(Message::Assistant(
            AssistantMessage::default(),
        )));
        assert_eq!(
            replaced.as_assistant().unwrap().content,
            vec![ContentBlock::text("replaced")]
        );
        // A role change is ignored.
        let user = AgentMessage::Llm(Message::user_text("x"));
        assert_eq!(hooks.run_message_end(user.clone()), user);

        hooks.add_fallible_model_context_augmentor(Arc::new(|_e: &ModelContextEvent| {
            Err("memory unavailable".into())
        }));
        assert!(hooks.has_model_context());
        let err = hooks
            .run_model_context(
                &ModelRef::new("p", "m"),
                Request::default(),
                0,
                &CancellationToken::new(),
            )
            .unwrap_err();
        assert_eq!(err, "memory unavailable");

        hooks.add_session_before_fork(Arc::new(|_e: &SessionBeforeForkEvent| {
            Some(BeforeForkResult {
                cancel: false,
                skip_conversation_restore: true,
            })
        }));
        hooks.add_session_before_fork(Arc::new(|e: &SessionBeforeForkEvent| {
            (e.position == "at").then_some(BeforeForkResult {
                cancel: true,
                skip_conversation_restore: false,
            })
        }));
        let r = hooks.run_session_before_fork(&SessionBeforeForkEvent {
            entry_id: "e".into(),
            position: "before".into(),
        });
        assert!(!r.cancel && r.skip_conversation_restore);
        assert!(
            hooks
                .run_session_before_fork(&SessionBeforeForkEvent {
                    entry_id: "e".into(),
                    position: "at".into(),
                })
                .cancel
        );
        hooks.add_resources_discover(Arc::new(|e: &ResourcesDiscoverEvent| {
            Some(DiscoveredResourcePaths {
                skill_paths: vec![format!("{}/s", e.reason)],
                ..Default::default()
            })
        }));
        assert_eq!(
            hooks
                .run_resources_discover(&ResourcesDiscoverEvent {
                    cwd: PathBuf::from("/"),
                    reason: "startup".into(),
                })
                .skill_paths,
            ["startup/s"]
        );
    }
}
