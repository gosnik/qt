//! The conversation transcript: a virtualised list of user turns, streamed
//! markdown, thinking blocks, tool-call cards, compaction / branch
//! summaries and custom messages.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::rc::Rc;

use gpui_kit::component::button::{Button, ButtonVariants as _};
use gpui_kit::component::message_scroller::{MessageScroller, MessageScrollerState};
use gpui_kit::component::spinner::Spinner;
use gpui_kit::component::text::{TextView, TextViewState};
use gpui_kit::component::{ActiveTheme as _, Icon, IconName, Sizable as _, h_flex, v_flex};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::*;
use qt_core::AgentEvent;
use qt_core::message::{
    AgentMessage, AssistantMessage, BashExecutionMessage, ContentBlock, CustomMessage, Message,
    StopReason, ToolResultMessage, UserContent,
};
use qt_core::provider::StreamEvent;
use serde_json::Value;

use crate::theme::QtPalette;

/// Lines of tool output shown while a card is collapsed.
const PREVIEW_LINES: usize = 6;
/// Lines of tool output shown while a card is expanded.
const EXPANDED_LINES: usize = 600;
const MAX_CONTENT_WIDTH: f32 = 920.;

#[derive(Clone, Copy, PartialEq, Eq)]
enum ToolStatus {
    Pending,
    Running,
    Done,
    Failed,
}

struct ToolItem {
    name: String,
    args: Value,
    status: ToolStatus,
    output: String,
    diff: Option<String>,
    images: usize,
    expanded: bool,
}

/// Which kind of harness summary an item shows.
#[derive(Clone, Copy, PartialEq, Eq)]
enum SummaryKind {
    Compaction { tokens_before: u64 },
    Branch,
}

enum Item {
    User {
        text: SharedString,
        images: usize,
        /// Session entry id, when known (enables "fork from here").
        entry_id: Option<String>,
    },
    Text(Entity<TextViewState>),
    Thinking {
        state: Entity<TextViewState>,
        expanded: bool,
        streaming: bool,
    },
    Tool(ToolItem),
    Summary {
        kind: SummaryKind,
        state: Entity<TextViewState>,
        expanded: bool,
    },
    Custom {
        custom_type: SharedString,
        state: Entity<TextViewState>,
    },
    Bash(BashExecutionMessage),
    /// A user `!command` that is still running.
    BashLive {
        id: u64,
        command: String,
        exclude: bool,
        output: String,
    },
    /// Slash command output (`/help`, `/session`, ...).
    Card {
        title: SharedString,
        state: Entity<TextViewState>,
        error: bool,
    },
    Error(SharedString),
    Notice(SharedString),
}

/// Events the workspace reacts to.
pub enum TranscriptEvent {
    /// Fork a new session before the user message with this entry id.
    Fork(String),
}

impl EventEmitter<TranscriptEvent> for Transcript {}

/// Renders displayed custom messages (extension message renderers):
/// `Some(lines)` replaces the default rendering.
pub type CustomRenderer = Rc<dyn Fn(&CustomMessage) -> Option<Vec<String>>>;

pub struct Transcript {
    items: Vec<Item>,
    scroller: Entity<MessageScrollerState>,
    /// Content index of the streaming assistant message -> item index.
    stream_items: HashMap<usize, usize>,
    /// Tool call id -> item index.
    tool_items: HashMap<String, usize>,
    cwd: PathBuf,
    hide_thinking: bool,
    next_bash_id: u64,
    _observers: Vec<Subscription>,
    custom_renderer: Option<CustomRenderer>,
    /// Extension `setHiddenThinkingLabel`.
    hidden_thinking_label: Option<SharedString>,
    /// Extension `setToolsExpanded`: tool cards start expanded.
    tools_expanded: bool,
}

impl Transcript {
    pub fn new(cwd: &Path, cx: &mut Context<Self>) -> Self {
        Self {
            items: Vec::new(),
            scroller: cx.new(|cx| MessageScrollerState::new(0, cx)),
            stream_items: HashMap::new(),
            tool_items: HashMap::new(),
            cwd: cwd.to_path_buf(),
            hide_thinking: false,
            next_bash_id: 0,
            _observers: Vec::new(),
            custom_renderer: None,
            hidden_thinking_label: None,
            tools_expanded: false,
        }
    }

    /// Render displayed custom messages with `renderer` first.
    pub fn set_custom_renderer(&mut self, renderer: Option<CustomRenderer>) {
        self.custom_renderer = renderer;
    }

    /// Label of collapsed or hidden thinking (`None` restores the default).
    pub fn set_hidden_thinking_label(&mut self, label: Option<String>, cx: &mut Context<Self>) {
        self.hidden_thinking_label = label.map(SharedString::from);
        cx.notify();
    }

    pub fn tools_expanded(&self) -> bool {
        self.tools_expanded
    }

    /// Expand (or collapse) every tool card; new cards follow.
    pub fn set_tools_expanded(&mut self, expanded: bool, cx: &mut Context<Self>) {
        self.tools_expanded = expanded;
        for item in &mut self.items {
            if let Item::Tool(tool) = item {
                tool.expanded = expanded || tool.status == ToolStatus::Failed;
            }
        }
        let count = self.items.len();
        self.scroller
            .update(cx, |s, cx| s.remeasure_items(0..count, cx));
        cx.notify();
    }

    /// Show slash command output as a card at the end of the transcript.
    pub fn push_card(
        &mut self,
        title: impl Into<SharedString>,
        lines: &[String],
        error: bool,
        cx: &mut Context<Self>,
    ) {
        let state = self.markdown_state(&card_markdown(lines), cx);
        self.items.push(Item::Card {
            title: title.into(),
            state,
            error,
        });
        self.appended(1, cx);
        self.scroller.update(cx, |s, cx| s.scroll_to_end(cx));
    }

    /// Add a running `!command` card; returns its id for updates.
    pub fn begin_bash(&mut self, command: &str, exclude: bool, cx: &mut Context<Self>) -> u64 {
        self.next_bash_id += 1;
        let id = self.next_bash_id;
        self.items.push(Item::BashLive {
            id,
            command: command.to_string(),
            exclude,
            output: String::new(),
        });
        self.appended(1, cx);
        self.scroller.update(cx, |s, cx| s.scroll_to_end(cx));
        id
    }

    fn bash_index(&self, wanted: u64) -> Option<usize> {
        self.items
            .iter()
            .position(|item| matches!(item, Item::BashLive { id, .. } if *id == wanted))
    }

    /// Replace the live output tail of a running `!command`.
    pub fn update_bash(&mut self, id: u64, tail: String, cx: &mut Context<Self>) {
        let index = self.bash_index(id);
        if let Some(Item::BashLive { output, .. }) = index.and_then(|i| self.items.get_mut(i)) {
            *output = tail;
        }
        self.changed(index, cx);
    }

    /// Turn a live `!command` card into its recorded result.
    pub fn finish_bash(&mut self, id: u64, message: BashExecutionMessage, cx: &mut Context<Self>) {
        if let Some(index) = self.bash_index(id) {
            self.items[index] = Item::Bash(message);
            self.changed(Some(index), cx);
        }
    }

    pub fn is_empty(&self) -> bool {
        self.items.is_empty()
    }

    /// `hideThinkingBlock`: show thinking as a one-line marker only.
    pub fn set_hide_thinking(&mut self, hide: bool, cx: &mut Context<Self>) {
        if self.hide_thinking != hide {
            self.hide_thinking = hide;
            let count = self.items.len();
            self.scroller
                .update(cx, |s, cx| s.remeasure_items(0..count, cx));
            cx.notify();
        }
    }

    /// Replace the transcript with a stored context. `user_ids` holds the
    /// session entry id of each user message (see
    /// [`crate::sessions::user_entry_ids`]).
    pub fn load(
        &mut self,
        messages: &[AgentMessage],
        user_ids: &[Option<String>],
        cx: &mut Context<Self>,
    ) {
        self.items.clear();
        self.stream_items.clear();
        self.tool_items.clear();
        self._observers.clear();
        for (index, message) in messages.iter().enumerate() {
            let entry_id = user_ids.get(index).cloned().flatten();
            match message {
                AgentMessage::Llm(Message::User { content, .. }) => {
                    self.push_user(content, entry_id)
                }
                AgentMessage::Llm(Message::Assistant(a)) => {
                    for block in &a.content {
                        match block {
                            ContentBlock::Text { text } if !text.trim().is_empty() => {
                                let state = self.markdown_state(text, cx);
                                self.items.push(Item::Text(state));
                            }
                            ContentBlock::Thinking { thinking, .. }
                                if !thinking.trim().is_empty() =>
                            {
                                let state = self.markdown_state(thinking, cx);
                                self.items.push(Item::Thinking {
                                    state,
                                    expanded: false,
                                    streaming: false,
                                });
                            }
                            ContentBlock::ToolCall {
                                id,
                                name,
                                arguments,
                            } => self.push_tool(id, name, arguments.clone()),
                            _ => {}
                        }
                    }
                    self.push_stop_notice(a);
                }
                AgentMessage::Llm(Message::ToolResult(r)) => self.apply_tool_result(r),
                other => self.push_harness(other, cx),
            }
        }
        // Calls left without a result belong to an interrupted run.
        for item in &mut self.items {
            if let Item::Tool(tool) = item
                && matches!(tool.status, ToolStatus::Pending | ToolStatus::Running)
            {
                tool.status = ToolStatus::Failed;
                tool.output = "No result recorded (run was interrupted).".into();
            }
        }
        let count = self.items.len();
        self.scroller.update(cx, |s, cx| {
            s.reset(count, cx);
            s.scroll_to_end(cx);
        });
        cx.notify();
    }

    /// Assign entry ids to user messages from the end (after a run, when
    /// the live user messages have been persisted).
    pub fn set_user_entry_ids(&mut self, ids: &[String], cx: &mut Context<Self>) {
        let mut ids = ids.to_vec();
        for item in self.items.iter_mut().rev() {
            if let Item::User { entry_id, .. } = item {
                match ids.pop() {
                    Some(id) => *entry_id = Some(id),
                    None => break,
                }
            }
        }
        cx.notify();
    }

    pub fn apply_event(&mut self, event: &AgentEvent, cx: &mut Context<Self>) {
        match event {
            AgentEvent::AgentStart
            | AgentEvent::TurnStart
            | AgentEvent::TurnEnd { .. }
            | AgentEvent::Warning(_)
            | AgentEvent::AgentEnd { .. }
            | AgentEvent::QueueUpdate { .. }
            | AgentEvent::CompactionStart { .. }
            | AgentEvent::CompactionEnd { .. }
            | AgentEvent::RetryStart { .. }
            | AgentEvent::RetryEnd { .. } => {}
            AgentEvent::CustomMessageEnd(message) => {
                let before = self.items.len();
                self.push_harness(message, cx);
                let added = self.items.len() - before;
                if added > 0 {
                    self.appended(added, cx);
                    self.scroller.update(cx, |s, cx| s.scroll_to_end(cx));
                }
            }
            AgentEvent::AssistantStart => self.stream_items.clear(),
            AgentEvent::Stream(stream) => self.apply_stream(stream, cx),
            AgentEvent::MessageEnd(Message::User { content, .. }) => {
                self.push_user(content, None);
                self.appended(1, cx);
                self.scroller.update(cx, |s, cx| s.scroll_to_end(cx));
            }
            AgentEvent::MessageEnd(Message::Assistant(a)) => self.finish_assistant(a, cx),
            AgentEvent::MessageEnd(Message::ToolResult(r)) => {
                self.apply_tool_result(r);
                self.changed(self.tool_items.get(&r.tool_call_id).copied(), cx);
            }
            AgentEvent::ToolStart(exec) => {
                let index = self.tool_items.get(&exec.id).copied();
                if let Some(Item::Tool(tool)) = index.and_then(|i| self.items.get_mut(i)) {
                    tool.status = ToolStatus::Running;
                    tool.args = exec.args.clone();
                }
                self.changed(index, cx);
            }
            AgentEvent::ToolProgress { id, output } => {
                let index = self.tool_items.get(id).copied();
                if let Some(Item::Tool(tool)) = index.and_then(|i| self.items.get_mut(i)) {
                    tool.output = output.clone();
                }
                self.changed(index, cx);
            }
        }
    }

    /// Harness messages: summaries, displayed custom messages, user bash.
    fn push_harness(&mut self, message: &AgentMessage, cx: &mut Context<Self>) {
        match message {
            AgentMessage::CompactionSummary(c) => {
                let state = self.markdown_state(&c.summary, cx);
                self.items.push(Item::Summary {
                    kind: SummaryKind::Compaction {
                        tokens_before: c.tokens_before,
                    },
                    state,
                    expanded: false,
                });
            }
            AgentMessage::BranchSummary(b) => {
                let state = self.markdown_state(&b.summary, cx);
                self.items.push(Item::Summary {
                    kind: SummaryKind::Branch,
                    state,
                    expanded: false,
                });
            }
            AgentMessage::Custom(c) if c.display => {
                let rendered = self.custom_renderer.as_ref().and_then(|r| r(c));
                let text = match rendered {
                    Some(lines) => card_markdown(&lines),
                    None => c.content.plain_text(),
                };
                let state = self.markdown_state(&text, cx);
                self.items.push(Item::Custom {
                    custom_type: c.custom_type.clone().into(),
                    state,
                });
            }
            AgentMessage::BashExecution(b) => self.items.push(Item::Bash(b.clone())),
            AgentMessage::Custom(_) | AgentMessage::Llm(_) => {}
        }
    }

    fn apply_stream(&mut self, event: &StreamEvent, cx: &mut Context<Self>) {
        match event {
            StreamEvent::BlockStart { index, block } => {
                let item = match block {
                    ContentBlock::Text { text } => Item::Text(self.markdown_state(text, cx)),
                    ContentBlock::Thinking { thinking, .. } => Item::Thinking {
                        state: self.markdown_state(thinking, cx),
                        expanded: false,
                        streaming: true,
                    },
                    ContentBlock::ToolCall {
                        id,
                        name,
                        arguments,
                    } => {
                        self.push_tool(id, name, arguments.clone());
                        self.stream_items.insert(*index, self.items.len() - 1);
                        self.appended(1, cx);
                        return;
                    }
                    ContentBlock::Image { .. } => return,
                };
                self.items.push(item);
                self.stream_items.insert(*index, self.items.len() - 1);
                self.appended(1, cx);
            }
            StreamEvent::TextDelta { index, delta }
            | StreamEvent::ThinkingDelta { index, delta } => {
                let item_index = self.stream_items.get(index).copied();
                let state = match item_index.and_then(|i| self.items.get(i)) {
                    Some(Item::Text(state)) | Some(Item::Thinking { state, .. }) => state.clone(),
                    _ => return,
                };
                state.update(cx, |s, cx| s.push_str(delta, cx));
                self.changed(item_index, cx);
            }
            StreamEvent::ToolArgsDelta { .. } => {}
        }
    }

    fn finish_assistant(&mut self, message: &AssistantMessage, cx: &mut Context<Self>) {
        let before = self.items.len();
        for (index, block) in message.content.iter().enumerate() {
            let Some(&item_index) = self.stream_items.get(&index) else {
                continue;
            };
            match (self.items.get_mut(item_index), block) {
                (Some(Item::Tool(tool)), ContentBlock::ToolCall { arguments, .. }) => {
                    tool.args = arguments.clone()
                }
                (Some(Item::Thinking { streaming, .. }), _) => *streaming = false,
                _ => {}
            }
        }
        self.stream_items.clear();
        self.push_stop_notice(message);
        let added = self.items.len() - before;
        if added > 0 {
            self.appended(added, cx);
        }
        cx.notify();
    }

    fn push_stop_notice(&mut self, message: &AssistantMessage) {
        match message.stop_reason {
            StopReason::Error => self.items.push(Item::Error(
                message
                    .error_message
                    .clone()
                    .unwrap_or_else(|| "The request failed.".into())
                    .into(),
            )),
            StopReason::Aborted => self.items.push(Item::Notice("Stopped.".into())),
            StopReason::Length => self
                .items
                .push(Item::Notice("Response hit the output token limit.".into())),
            StopReason::Stop | StopReason::ToolCall => {}
        }
    }

    fn push_user(&mut self, content: &UserContent, entry_id: Option<String>) {
        let images = match content {
            UserContent::Blocks(blocks) => blocks
                .iter()
                .filter(|b| matches!(b, ContentBlock::Image { .. }))
                .count(),
            UserContent::Text(_) => 0,
        };
        self.items.push(Item::User {
            text: content.plain_text().into(),
            images,
            entry_id,
        });
    }

    fn push_tool(&mut self, id: &str, name: &str, args: Value) {
        self.items.push(Item::Tool(ToolItem {
            name: name.to_string(),
            args,
            status: ToolStatus::Pending,
            output: String::new(),
            diff: None,
            images: 0,
            expanded: self.tools_expanded,
        }));
        self.tool_items.insert(id.to_string(), self.items.len() - 1);
    }

    fn apply_tool_result(&mut self, result: &ToolResultMessage) {
        let Some(&index) = self.tool_items.get(&result.tool_call_id) else {
            return;
        };
        let Some(Item::Tool(tool)) = self.items.get_mut(index) else {
            return;
        };
        tool.status = if result.is_error {
            ToolStatus::Failed
        } else {
            ToolStatus::Done
        };
        tool.output = result.text();
        tool.images = result
            .content
            .iter()
            .filter(|b| matches!(b, ContentBlock::Image { .. }))
            .count();
        tool.diff = result
            .details
            .as_ref()
            .and_then(|d| d.get("diff"))
            .and_then(Value::as_str)
            .filter(|d| !d.is_empty())
            .map(str::to_string);
        tool.expanded = result.is_error || self.tools_expanded;
    }

    fn markdown_state(&mut self, text: &str, cx: &mut Context<Self>) -> Entity<TextViewState> {
        let state = cx.new(|cx| TextViewState::markdown(text, cx).selectable(true));
        self.observe_text(&state, cx);
        state
    }

    /// Re-measure the row when its markdown finishes (re)parsing.
    fn observe_text(&mut self, state: &Entity<TextViewState>, cx: &mut Context<Self>) {
        let index = self.items.len();
        let subscription = cx.observe(state, move |this, _, cx| this.changed(Some(index), cx));
        self._observers.push(subscription);
    }

    fn appended(&mut self, count: usize, cx: &mut Context<Self>) {
        self.scroller.update(cx, |s, cx| {
            s.append(count, cx);
        });
        cx.notify();
    }

    fn changed(&mut self, index: Option<usize>, cx: &mut Context<Self>) {
        if let Some(i) = index.filter(|i| *i < self.items.len()) {
            self.scroller.update(cx, |s, cx| {
                s.remeasure_items(i..i + 1, cx);
            });
        }
        cx.notify();
    }

    fn toggle(&mut self, index: usize, cx: &mut Context<Self>) {
        match self.items.get_mut(index) {
            Some(Item::Tool(tool)) => tool.expanded = !tool.expanded,
            Some(Item::Thinking { expanded, .. }) => *expanded = !*expanded,
            Some(Item::Summary { expanded, .. }) => *expanded = !*expanded,
            _ => return,
        }
        self.changed(Some(index), cx);
    }

    fn render_item(
        &mut self,
        index: usize,
        _window: &mut Window,
        cx: &mut Context<Self>,
    ) -> AnyElement {
        let theme = cx.theme().clone();
        let palette = QtPalette::get(cx);
        let body: AnyElement = match &self.items[index] {
            Item::User {
                text,
                images,
                entry_id,
            } => {
                let mut lines = v_flex().gap_0p5().text_sm();
                for line in text.split('\n') {
                    lines = lines.child(
                        div()
                            .min_h(px(18.))
                            .child(SharedString::from(line.to_string())),
                    );
                }
                let fork = entry_id.clone().map(|id| {
                    Button::new(("fork", index))
                        .icon(IconName::Undo2)
                        .xsmall()
                        .ghost()
                        .tooltip("Fork a new session from this message")
                        .on_click(cx.listener(move |_, _, _, cx| {
                            cx.emit(TranscriptEvent::Fork(id.clone()))
                        }))
                });
                h_flex()
                    .group("user-message")
                    .w_full()
                    .justify_end()
                    .items_start()
                    .gap_1()
                    .when_some(fork, |el, fork| {
                        el.child(
                            div()
                                .invisible()
                                .group_hover("user-message", |s| s.visible())
                                .child(fork),
                        )
                    })
                    .child(
                        v_flex()
                            .max_w(relative(0.85))
                            .gap_1()
                            .px_4()
                            .py_2p5()
                            .rounded_lg()
                            .bg(palette.user_bg)
                            .text_color(palette.user_fg)
                            .child(lines)
                            .when(*images > 0, |el| {
                                el.child(
                                    div()
                                        .text_xs()
                                        .text_color(theme.muted_foreground)
                                        .child(format!("{images} image(s) attached")),
                                )
                            }),
                    )
                    .into_any_element()
            }
            Item::Text(state) => div()
                .w_full()
                .text_sm()
                .child(TextView::new(state).selectable(true))
                .into_any_element(),
            Item::Thinking {
                state,
                expanded,
                streaming,
            } => {
                let hidden = self.hide_thinking;
                let expanded = *expanded && !hidden;
                let label: SharedString = match &self.hidden_thinking_label {
                    Some(custom) if hidden || !expanded => custom.clone(),
                    _ if *streaming || hidden => "Thinking…".into(),
                    _ => "Thought process".into(),
                };
                v_flex()
                    .w_full()
                    .gap_1()
                    .child(
                        h_flex()
                            .id(("thinking", index))
                            .gap_1p5()
                            .items_center()
                            .text_xs()
                            .text_color(palette.thinking_text)
                            .when(!hidden, |el| {
                                el.cursor_pointer()
                                    .hover(|s| s.text_color(theme.foreground))
                                    .on_click(
                                        cx.listener(move |this, _, _, cx| this.toggle(index, cx)),
                                    )
                                    .child(
                                        Icon::new(if expanded {
                                            IconName::ChevronDown
                                        } else {
                                            IconName::ChevronRight
                                        })
                                        .xsmall(),
                                    )
                            })
                            .child(label),
                    )
                    .when(expanded, |el| {
                        el.child(
                            div()
                                .pl_4()
                                .ml_1()
                                .border_l_2()
                                .border_color(theme.border)
                                .text_xs()
                                .text_color(palette.thinking_text)
                                .child(TextView::new(state).selectable(true)),
                        )
                    })
                    .into_any_element()
            }
            Item::Tool(tool) => self.render_tool(tool, index, &theme, &palette, cx),
            Item::Summary {
                kind,
                state,
                expanded,
            } => {
                let expanded = *expanded;
                let (tag, line) = match kind {
                    SummaryKind::Compaction { tokens_before } => (
                        "[compaction]",
                        format!("Compacted from {} tokens", group_thousands(*tokens_before)),
                    ),
                    SummaryKind::Branch => ("[branch]", "Branch summary".to_string()),
                };
                v_flex()
                    .w_full()
                    .gap_1()
                    .px_3()
                    .py_2()
                    .rounded_md()
                    .bg(palette.custom_bg)
                    .child(
                        h_flex()
                            .id(("summary", index))
                            .gap_2()
                            .items_center()
                            .cursor_pointer()
                            .on_click(cx.listener(move |this, _, _, cx| this.toggle(index, cx)))
                            .child(
                                div()
                                    .text_xs()
                                    .font_weight(FontWeight::BOLD)
                                    .text_color(palette.custom_label)
                                    .child(tag),
                            )
                            .child(
                                div()
                                    .flex_1()
                                    .text_sm()
                                    .text_color(palette.custom_fg)
                                    .child(line),
                            )
                            .child(div().text_xs().text_color(palette.dim).child(if expanded {
                                "click to collapse"
                            } else {
                                "click to expand"
                            })),
                    )
                    .when(expanded, |el| {
                        el.child(
                            div()
                                .text_sm()
                                .text_color(palette.custom_fg)
                                .child(TextView::new(state).selectable(true)),
                        )
                    })
                    .into_any_element()
            }
            Item::Custom { custom_type, state } => v_flex()
                .w_full()
                .gap_1()
                .px_3()
                .py_2()
                .rounded_md()
                .bg(palette.custom_bg)
                .child(
                    div()
                        .text_xs()
                        .font_weight(FontWeight::BOLD)
                        .text_color(palette.custom_label)
                        .child(format!("[{custom_type}]")),
                )
                .child(
                    div()
                        .text_sm()
                        .text_color(palette.custom_fg)
                        .child(TextView::new(state).selectable(true)),
                )
                .into_any_element(),
            Item::Bash(bash) => {
                let mut status = Vec::new();
                if bash.cancelled {
                    status.push("(command cancelled)".to_string());
                } else if let Some(code) = bash.exit_code.filter(|c| *c != 0) {
                    status.push(format!("Command exited with code {code}"));
                }
                if bash.truncated
                    && let Some(path) = &bash.full_output_path
                {
                    status.push(format!("Output truncated. Full output: {path}"));
                }
                if bash.exclude_from_context {
                    status.push("not sent to the model".into());
                }
                render_bash(
                    &bash.command,
                    &bash.output,
                    &status,
                    false,
                    &theme,
                    &palette,
                )
            }
            Item::BashLive {
                command,
                exclude,
                output,
                ..
            } => {
                let mut status = vec!["Running… (Esc to cancel)".to_string()];
                if *exclude {
                    status.push("not sent to the model".into());
                }
                render_bash(command, output, &status, true, &theme, &palette)
            }
            Item::Card {
                title,
                state,
                error,
            } => v_flex()
                .w_full()
                .gap_1()
                .px_3()
                .py_2()
                .rounded_md()
                .border_1()
                .border_color(if *error { theme.danger } else { theme.border })
                .bg(theme.muted.opacity(0.3))
                .child(
                    h_flex()
                        .gap_2()
                        .items_center()
                        .text_xs()
                        .font_weight(FontWeight::SEMIBOLD)
                        .text_color(if *error {
                            theme.danger
                        } else {
                            theme.muted_foreground
                        })
                        .child(
                            Icon::new(if *error {
                                IconName::TriangleAlert
                            } else {
                                IconName::Info
                            })
                            .xsmall(),
                        )
                        .child(
                            div()
                                .flex_1()
                                .min_w_0()
                                .overflow_hidden()
                                .whitespace_nowrap()
                                .text_ellipsis()
                                .child(title.clone()),
                        ),
                )
                .child(div().text_sm().child(TextView::new(state).selectable(true)))
                .into_any_element(),
            Item::Error(message) => h_flex()
                .w_full()
                .gap_2()
                .items_start()
                .px_3()
                .py_2()
                .rounded_md()
                .border_1()
                .border_color(theme.danger)
                .text_sm()
                .text_color(theme.danger)
                .child(Icon::new(IconName::TriangleAlert).small())
                .child(div().flex_1().child(message.clone()))
                .into_any_element(),
            Item::Notice(text) => h_flex()
                .w_full()
                .justify_center()
                .text_xs()
                .text_color(theme.muted_foreground)
                .child(text.clone())
                .into_any_element(),
        };
        h_flex()
            .w_full()
            .justify_center()
            .px_6()
            .py_1p5()
            .child(div().w_full().max_w(px(MAX_CONTENT_WIDTH)).child(body))
            .into_any_element()
    }

    fn render_tool(
        &self,
        tool: &ToolItem,
        index: usize,
        theme: &component::Theme,
        palette: &QtPalette,
        cx: &mut Context<Self>,
    ) -> AnyElement {
        let (icon, color, bg) = match tool.status {
            ToolStatus::Pending | ToolStatus::Running => (
                IconName::LoaderCircle,
                theme.muted_foreground,
                palette.tool_pending_bg,
            ),
            ToolStatus::Done => (
                IconName::CircleCheck,
                palette.success,
                palette.tool_success_bg,
            ),
            ToolStatus::Failed => (IconName::CircleX, palette.error, palette.tool_error_bg),
        };
        let summary = tool_summary(&tool.name, &tool.args, &self.cwd);
        let running = matches!(tool.status, ToolStatus::Pending | ToolStatus::Running);
        let show_body =
            tool.expanded || running || tool.diff.is_some() || tool.status == ToolStatus::Failed;
        let header = h_flex()
            .id(("tool", index))
            .w_full()
            .gap_2()
            .items_center()
            .px_3()
            .py_1p5()
            .bg(bg)
            .cursor_pointer()
            .hover(|s| s.bg(theme.muted))
            .on_click(cx.listener(move |this, _, _, cx| this.toggle(index, cx)))
            .child(Icon::new(icon).small().text_color(color))
            .child(
                div()
                    .font_family(theme.mono_font_family.clone())
                    .text_sm()
                    .font_weight(FontWeight::SEMIBOLD)
                    .text_color(palette.tool_title)
                    .child(tool.name.clone()),
            )
            .child(
                div()
                    .flex_1()
                    .min_w_0()
                    .overflow_hidden()
                    .whitespace_nowrap()
                    .text_ellipsis()
                    .font_family(theme.mono_font_family.clone())
                    .text_xs()
                    .text_color(theme.muted_foreground)
                    .child(summary),
            )
            .child(
                Icon::new(if tool.expanded {
                    IconName::ChevronDown
                } else {
                    IconName::ChevronRight
                })
                .xsmall()
                .text_color(theme.muted_foreground),
            );

        let body = if !show_body || (tool.output.trim().is_empty() && tool.diff.is_none()) {
            None
        } else if let Some(diff) = &tool.diff {
            Some(self.render_diff(diff, tool.expanded, theme, palette))
        } else {
            let tail = tool.name == "bash";
            let limit = if tool.expanded {
                EXPANDED_LINES
            } else {
                PREVIEW_LINES
            };
            Some(render_output(
                &tool.output,
                limit,
                tail,
                tool.images,
                theme,
                palette.tool_output,
            ))
        };

        v_flex()
            .w_full()
            .rounded_md()
            .border_1()
            .border_color(if tool.status == ToolStatus::Failed {
                theme.danger.opacity(0.5)
            } else {
                theme.border
            })
            .overflow_hidden()
            .child(header)
            .children(body.map(|b| div().border_t_1().border_color(theme.border).child(b)))
            .into_any_element()
    }

    fn render_diff(
        &self,
        diff: &str,
        expanded: bool,
        theme: &component::Theme,
        palette: &QtPalette,
    ) -> AnyElement {
        let lines: Vec<&str> = diff
            .lines()
            .filter(|l| !l.starts_with("---") && !l.starts_with("+++"))
            .collect();
        let limit = if expanded { EXPANDED_LINES } else { 40 };
        let mut col = v_flex()
            .w_full()
            .py_1()
            .font_family(theme.mono_font_family.clone())
            .text_xs();
        for line in lines.iter().take(limit) {
            let (bg, fg) = if line.starts_with('+') {
                (palette.diff_added.opacity(0.14), theme.foreground)
            } else if line.starts_with('-') {
                (palette.diff_removed.opacity(0.14), theme.foreground)
            } else if line.starts_with("@@") {
                (theme.transparent, theme.info)
            } else {
                (theme.transparent, palette.diff_context)
            };
            col = col.child(
                div()
                    .w_full()
                    .px_3()
                    .bg(bg)
                    .text_color(fg)
                    .child(SharedString::from(line.to_string())),
            );
        }
        if lines.len() > limit {
            col = col.child(
                div()
                    .px_3()
                    .text_color(theme.muted_foreground)
                    .child(format!("… {} more lines", lines.len() - limit)),
            );
        }
        col.into_any_element()
    }
}

/// `12345` → `12,345` (Pi uses `toLocaleString()` for token counts).
pub fn group_thousands(n: u64) -> String {
    let digits = n.to_string();
    let mut out = String::with_capacity(digits.len() + digits.len() / 3);
    for (i, ch) in digits.chars().enumerate() {
        if i > 0 && (digits.len() - i).is_multiple_of(3) {
            out.push(',');
        }
        out.push(ch);
    }
    out
}

fn render_output(
    output: &str,
    limit: usize,
    tail: bool,
    images: usize,
    theme: &component::Theme,
    color: Hsla,
) -> AnyElement {
    let lines: Vec<&str> = output.lines().collect();
    let hidden = lines.len().saturating_sub(limit);
    let shown: &[&str] = if tail {
        &lines[hidden..]
    } else {
        &lines[..lines.len().min(limit)]
    };
    let mut col = v_flex()
        .w_full()
        .px_3()
        .py_1p5()
        .font_family(theme.mono_font_family.clone())
        .text_xs()
        .text_color(color);
    let more = |n: usize| {
        div()
            .italic()
            .child(format!("… {n} more lines (click header to expand)"))
    };
    if tail && hidden > 0 {
        col = col.child(more(hidden));
    }
    for line in shown {
        col = col.child(
            div()
                .min_h(px(16.))
                .child(SharedString::from(qt_core::util::shorten(line, 400))),
        );
    }
    if !tail && hidden > 0 {
        col = col.child(more(hidden));
    }
    if images > 0 {
        col = col.child(div().child(format!("[{images} image(s) returned]")));
    }
    col.into_any_element()
}

/// Card body: the output lines in a fenced block that no line can close.
pub fn card_markdown(lines: &[String]) -> String {
    let body = lines.join("\n");
    let mut longest = 0;
    let mut run = 0;
    for c in body.chars() {
        if c == '`' {
            run += 1;
            longest = longest.max(run);
        } else {
            run = 0;
        }
    }
    let fence = "`".repeat(longest.max(2) + 1);
    format!("{fence}text\n{body}\n{fence}")
}

fn render_bash(
    command: &str,
    output: &str,
    status: &[String],
    running: bool,
    theme: &component::Theme,
    palette: &QtPalette,
) -> AnyElement {
    let shown = match (output.is_empty(), running) {
        (false, _) => output,
        (true, true) => "",
        (true, false) => "(no output)",
    };
    v_flex()
        .w_full()
        .rounded_md()
        .border_1()
        .border_color(theme.border)
        .overflow_hidden()
        .child(
            h_flex()
                .px_3()
                .py_1p5()
                .gap_2()
                .font_family(theme.mono_font_family.clone())
                .text_sm()
                .child(Icon::new(IconName::SquareTerminal).small())
                .child(div().flex_1().child(format!("$ {command}")))
                .when(running, |el| el.child(Spinner::new().xsmall())),
        )
        .child(
            div()
                .border_t_1()
                .border_color(theme.border)
                .child(render_output(
                    shown,
                    PREVIEW_LINES * 2,
                    true,
                    0,
                    theme,
                    palette.tool_output,
                )),
        )
        .when(!status.is_empty(), |el| {
            el.child(
                div()
                    .px_3()
                    .pb_1p5()
                    .text_xs()
                    .text_color(palette.warning)
                    .child(status.join(" · ")),
            )
        })
        .into_any_element()
}

/// Short description of a tool call for its card header.
fn tool_summary(name: &str, args: &Value, cwd: &Path) -> SharedString {
    let s = |k: &str| args.get(k).and_then(Value::as_str).unwrap_or_default();
    let path = |k: &str| {
        let p = s(k);
        Path::new(p)
            .strip_prefix(cwd)
            .map(|r| r.display().to_string())
            .unwrap_or_else(|_| p.to_string())
    };
    let text = match name {
        "bash" => format!("$ {}", s("command").lines().next().unwrap_or_default()),
        "read" => {
            let mut t = path("path");
            if let Some(o) = args.get("offset").and_then(Value::as_u64) {
                t.push_str(&format!(":{o}"));
            }
            t
        }
        "edit" => {
            let n = args
                .get("edits")
                .and_then(Value::as_array)
                .map_or(1, Vec::len);
            format!(
                "{} ({n} edit{})",
                path("path"),
                if n == 1 { "" } else { "s" }
            )
        }
        "write" => path("path"),
        "ls" => {
            let p = path("path");
            if p.is_empty() { ".".into() } else { p }
        }
        "grep" | "find" => {
            let p = path("path");
            if p.is_empty() {
                s("pattern").to_string()
            } else {
                format!("{} in {p}", s("pattern"))
            }
        }
        _ => match args {
            Value::Null => String::new(),
            other => other.to_string(),
        },
    };
    qt_core::util::shorten(&text, 200).into()
}

impl Render for Transcript {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let this = cx.entity().downgrade();
        MessageScroller::new(
            "transcript",
            self.scroller.clone(),
            move |index, window, cx| {
                this.update(cx, |this, cx| {
                    if index < this.items.len() {
                        this.render_item(index, window, cx)
                    } else {
                        div().into_any_element()
                    }
                })
                .unwrap_or_else(|_| div().into_any_element())
            },
        )
        .jump_button(true)
        .size_full()
    }
}

#[cfg(test)]
mod tests {
    // Not `super::*`: the gpui glob import would shadow `#[test]`.
    use super::{card_markdown, group_thousands, tool_summary};
    use serde_json::json;
    use std::path::Path;

    #[test]
    fn summaries_are_relative_and_compact() {
        let cwd = Path::new("/work");
        assert_eq!(
            tool_summary("read", &json!({"path": "/work/src/a.rs", "offset": 5}), cwd).as_ref(),
            "src/a.rs:5"
        );
        assert_eq!(
            tool_summary("bash", &json!({"command": "ls\nmore"}), cwd).as_ref(),
            "$ ls"
        );
        assert_eq!(
            tool_summary("edit", &json!({"path": "x", "edits": [{}, {}]}), cwd).as_ref(),
            "x (2 edits)"
        );
        assert_eq!(
            tool_summary("grep", &json!({"pattern": "fn", "path": "src"}), cwd).as_ref(),
            "fn in src"
        );
        assert_eq!(tool_summary("ls", &json!({}), cwd).as_ref(), ".");
    }

    #[test]
    fn card_fences_cannot_be_closed_by_output() {
        assert_eq!(
            card_markdown(&["a".into(), "b".into()]),
            "```text\na\nb\n```"
        );
        assert_eq!(
            card_markdown(&["```x```".into()]),
            "````text\n```x```\n````"
        );
    }

    #[test]
    fn thousands_are_grouped() {
        assert_eq!(group_thousands(0), "0");
        assert_eq!(group_thousands(999), "999");
        assert_eq!(group_thousands(1_000), "1,000");
        assert_eq!(group_thousands(123_456_789), "123,456,789");
    }
}
