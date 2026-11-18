//! Session views: the tree (`/tree`: branch navigation with an optional
//! branch summary, labels, fork) and the resume picker (`/resume`: search
//! across sessions, rename, import).

use std::path::PathBuf;
use std::rc::Rc;
use std::time::SystemTime;

use gpui_kit::component::button::{Button, ButtonVariants as _};
use gpui_kit::component::input::{Input, InputEvent, InputState};
use gpui_kit::component::spinner::Spinner;
use gpui_kit::component::{
    ActiveTheme as _, Disableable as _, IconName, Sizable as _, WindowExt as _, h_flex, v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::*;
use qt_core::session::SessionSummary;
use qt_core::{Conversation, Engine, NavigateOptions};
use serde_json::Value;

use crate::sessions::{TreeFilter, TreeViewRow, session_matches, tree_rows};

/// What a session view asks the workspace to do.
pub enum SessionAction {
    /// The conversation moved within its tree; reload the transcript and,
    /// when the composer is empty, seed it with `editor_text`.
    Navigated { editor_text: Option<String> },
    /// Labels changed (nothing to reload in the transcript).
    Labeled,
    /// Fork from the user message with this entry id.
    Fork(String),
    /// Resume this session file.
    Open(PathBuf),
    /// Import this JSONL file (after confirmation).
    Import(PathBuf),
    /// Another session was renamed.
    Renamed,
    /// Rename the live session.
    RenameCurrent(String),
}

pub type ActionHandler = Rc<dyn Fn(SessionAction, &mut Window, &mut App)>;

/// Pi's three summary choices for leaving a branch.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SummaryChoice {
    None,
    Summarize,
    Custom,
}

impl SummaryChoice {
    pub fn label(self) -> &'static str {
        match self {
            SummaryChoice::None => "No summary",
            SummaryChoice::Summarize => "Summarize",
            SummaryChoice::Custom => "Summarize with custom prompt",
        }
    }
}

/// The navigate options for a summary choice.
pub fn navigate_options(choice: SummaryChoice, custom: &str, label: &str) -> NavigateOptions {
    let custom = custom.trim();
    NavigateOptions {
        summarize: choice != SummaryChoice::None,
        custom_instructions: (choice == SummaryChoice::Custom && !custom.is_empty())
            .then(|| custom.to_string()),
        replace_instructions: false,
        label: Some(label.trim().to_string()).filter(|l| !l.is_empty()),
    }
}

/// `branchSummary.skipPrompt`: navigate without asking about a summary.
pub fn skip_summary_prompt(engine: &Engine) -> bool {
    engine
        .settings()
        .raw()
        .get("branchSummary")
        .and_then(|b| b.get("skipPrompt"))
        .and_then(Value::as_bool)
        .unwrap_or(false)
}

enum TreeMode {
    Browse,
    Choose {
        target: String,
        choice: SummaryChoice,
    },
    Summarizing,
    Label {
        target: String,
    },
}

pub struct TreePanel {
    conversation: Conversation,
    engine: Engine,
    filter: TreeFilter,
    search: Entity<InputState>,
    custom: Entity<InputState>,
    label: Entity<InputState>,
    rows: Vec<TreeViewRow>,
    selected: Option<String>,
    mode: TreeMode,
    message: Option<(bool, String)>,
    scroll: UniformListScrollHandle,
    on_action: ActionHandler,
    _task: Option<Task<()>>,
    _subscriptions: Vec<Subscription>,
}

impl TreePanel {
    pub fn new(
        engine: Engine,
        conversation: Conversation,
        on_action: ActionHandler,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let search = cx.new(|cx| InputState::new(window, cx).placeholder("Search entries"));
        let custom = cx
            .new(|cx| InputState::new(window, cx).placeholder("Custom summarization instructions"));
        let label = cx.new(|cx| InputState::new(window, cx).placeholder("Label (optional)"));
        let subscriptions = vec![cx.subscribe(&search, |this, _, e: &InputEvent, cx| {
            if matches!(e, InputEvent::Change) {
                this.refresh(cx);
            }
        })];
        let mut this = Self {
            selected: None,
            conversation,
            engine,
            filter: TreeFilter::Default,
            search,
            custom,
            label,
            rows: Vec::new(),
            mode: TreeMode::Browse,
            message: None,
            scroll: UniformListScrollHandle::new(),
            on_action,
            _task: None,
            _subscriptions: subscriptions,
        };
        this.refresh(cx);
        this.select_current();
        if let Some(i) = this.selected_index() {
            this.scroll.scroll_to_item(i, ScrollStrategy::Center);
        }
        this
    }

    fn refresh(&mut self, cx: &mut Context<Self>) {
        let query = self.search.read(cx).value().to_string();
        let leaf = self.conversation.leaf_id();
        self.rows = tree_rows(
            &self.conversation.tree(),
            leaf.as_deref(),
            self.filter,
            &query,
        );
        cx.notify();
    }

    /// Select the row standing for the current position.
    fn select_current(&mut self) {
        self.selected = self.rows.iter().find(|r| r.is_leaf).map(|r| r.id.clone());
    }

    fn selected_index(&self) -> Option<usize> {
        let id = self.selected.as_deref()?;
        self.rows.iter().position(|r| r.id == id)
    }

    fn selected_row(&self) -> Option<&TreeViewRow> {
        self.selected_index().map(|i| &self.rows[i])
    }

    fn begin_navigate(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let Some(target) = self.selected.clone() else {
            return;
        };
        if self.conversation.leaf_id().as_deref() == Some(target.as_str())
            || self.selected_row().is_some_and(|r| r.is_leaf)
        {
            self.message = Some((false, "Already at this point".into()));
            cx.notify();
            return;
        }
        if self.conversation.is_running() {
            self.message = Some((true, "Wait for the agent to finish first.".into()));
            cx.notify();
            return;
        }
        if skip_summary_prompt(&self.engine) {
            self.navigate(target, NavigateOptions::default(), window, cx);
            return;
        }
        self.custom.update(cx, |s, cx| s.set_value("", window, cx));
        self.label.update(cx, |s, cx| s.set_value("", window, cx));
        self.mode = TreeMode::Choose {
            target,
            choice: SummaryChoice::None,
        };
        cx.notify();
    }

    fn navigate(
        &mut self,
        target: String,
        options: NavigateOptions,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let summarizing = options.summarize;
        self.mode = if summarizing {
            TreeMode::Summarizing
        } else {
            TreeMode::Browse
        };
        self.message = None;
        let handle = self.conversation.navigate_tree(&target, options);
        let on_action = self.on_action.clone();
        self._task = Some(cx.spawn_in(window, async move |this, cx| {
            let result = handle.await;
            let _ = this.update_in(cx, |this, window, cx| {
                this.mode = TreeMode::Browse;
                match result {
                    Ok(Ok(nav)) if nav.cancelled => {
                        this.message = Some((false, "Branch summarization cancelled".into()));
                    }
                    Ok(Ok(nav)) => {
                        this.refresh(cx);
                        this.select_current();
                        this.message = Some((false, "Navigated to selected point".into()));
                        on_action(
                            SessionAction::Navigated {
                                editor_text: nav.editor_text,
                            },
                            window,
                            cx,
                        );
                        window.close_dialog(cx);
                    }
                    Ok(Err(err)) => this.message = Some((true, format!("{err:#}"))),
                    Err(err) => this.message = Some((true, format!("{err}"))),
                }
                this.refresh(cx);
            });
        }));
        cx.notify();
    }

    fn confirm_choice(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let TreeMode::Choose { target, choice } = &self.mode else {
            return;
        };
        let custom = self.custom.read(cx).value().to_string();
        if *choice == SummaryChoice::Custom && custom.trim().is_empty() {
            self.message = Some((true, "Enter custom summarization instructions.".into()));
            cx.notify();
            return;
        }
        let label = self.label.read(cx).value().to_string();
        let options = navigate_options(*choice, &custom, &label);
        let target = target.clone();
        self.navigate(target, options, window, cx);
    }

    pub fn is_summarizing(&self) -> bool {
        matches!(self.mode, TreeMode::Summarizing)
    }

    fn cancel_summary(&mut self, cx: &mut Context<Self>) {
        self.conversation.abort();
        cx.notify();
    }

    fn begin_label(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let Some(target) = self.selected.clone() else {
            return;
        };
        let current = self.conversation.label(&target).unwrap_or_default();
        self.label.update(cx, |s, cx| {
            s.set_value(current, window, cx);
            s.focus(window, cx);
        });
        self.mode = TreeMode::Label { target };
        cx.notify();
    }

    fn save_label(&mut self, clear: bool, window: &mut Window, cx: &mut Context<Self>) {
        let TreeMode::Label { target } = &self.mode else {
            return;
        };
        let value = self.label.read(cx).value().to_string();
        let label = (!clear).then_some(value.as_str());
        match self.conversation.set_label(target, label) {
            Ok(()) => {
                self.mode = TreeMode::Browse;
                self.message = None;
                (self.on_action)(SessionAction::Labeled, window, cx);
            }
            Err(err) => self.message = Some((true, format!("{err:#}"))),
        }
        self.refresh(cx);
    }

    fn render_row(&self, row: &TreeViewRow, index: usize, cx: &mut Context<Self>) -> AnyElement {
        let theme = cx.theme().clone();
        let selected = self.selected.as_deref() == Some(row.id.as_str());
        let id = row.id.clone();
        h_flex()
            .id(("tree-row", index))
            .w_full()
            .h(px(26.))
            .gap_1p5()
            .items_center()
            .pl(px(8. + row.depth as f32 * 14.))
            .pr_2()
            .rounded_md()
            .cursor_pointer()
            .when(selected, |el| el.bg(theme.list_active))
            .when(!selected, |el| el.hover(|s| s.bg(theme.muted.opacity(0.5))))
            .on_click(cx.listener(move |this, event: &ClickEvent, window, cx| {
                this.selected = Some(id.clone());
                if matches!(this.mode, TreeMode::Label { .. }) {
                    this.mode = TreeMode::Browse;
                }
                if event.click_count() >= 2 {
                    this.begin_navigate(window, cx);
                }
                cx.notify();
            }))
            .child(
                div()
                    .w(px(8.))
                    .h(px(8.))
                    .rounded_full()
                    .flex_none()
                    .when(row.on_active_path, |el| el.bg(theme.primary))
                    .when(!row.on_active_path, |el| {
                        el.border_1().border_color(theme.border)
                    }),
            )
            .child(
                div()
                    .flex_1()
                    .min_w_0()
                    .overflow_hidden()
                    .whitespace_nowrap()
                    .text_ellipsis()
                    .text_sm()
                    .text_color(if row.is_user || row.on_active_path {
                        theme.foreground
                    } else {
                        theme.muted_foreground
                    })
                    .child(SharedString::from(row.summary.clone())),
            )
            .when_some(row.label.clone(), |el, label| {
                el.child(
                    div()
                        .px_1p5()
                        .rounded_sm()
                        .bg(theme.info.opacity(0.2))
                        .text_xs()
                        .text_color(theme.info)
                        .child(label),
                )
            })
            .when(row.child_count > 1, |el| {
                el.child(
                    div()
                        .text_xs()
                        .text_color(theme.muted_foreground)
                        .child(format!("{} branches", row.child_count)),
                )
            })
            .when(row.is_leaf, |el| {
                el.child(div().text_xs().text_color(theme.primary).child("● current"))
            })
            .into_any_element()
    }

    fn render_footer(&self, cx: &mut Context<Self>) -> AnyElement {
        let theme = cx.theme().clone();
        match &self.mode {
            TreeMode::Browse => {
                let row = self.selected_row();
                let is_user = row.is_some_and(|r| r.is_user);
                let has = row.is_some();
                let fork_id = row.map(|r| r.id.clone());
                h_flex()
                    .gap_2()
                    .child(
                        Button::new("tree-navigate")
                            .label("Go to selected point")
                            .small()
                            .primary()
                            .disabled(!has)
                            .on_click(cx.listener(|this, _, w, cx| this.begin_navigate(w, cx))),
                    )
                    .child(
                        Button::new("tree-label")
                            .icon(IconName::Star)
                            .label("Label")
                            .small()
                            .disabled(!has)
                            .on_click(cx.listener(|this, _, w, cx| this.begin_label(w, cx))),
                    )
                    .child(
                        Button::new("tree-fork")
                            .label("Fork from here")
                            .small()
                            .disabled(!is_user)
                            .tooltip("New session ending before this message; the message is put back in the composer")
                            .on_click(cx.listener(move |this, _, window, cx| {
                                if let Some(id) = fork_id.clone() {
                                    (this.on_action)(SessionAction::Fork(id), window, cx);
                                    window.close_dialog(cx);
                                }
                            })),
                    )
                    .into_any_element()
            }
            TreeMode::Choose { choice, .. } => {
                let current = *choice;
                let mut options = h_flex().gap_1();
                for (i, c) in [
                    SummaryChoice::None,
                    SummaryChoice::Summarize,
                    SummaryChoice::Custom,
                ]
                .into_iter()
                .enumerate()
                {
                    options = options.child(
                        Button::new(("summary-choice", i))
                            .label(c.label())
                            .small()
                            .map(|b| if c == current { b.primary() } else { b.ghost() })
                            .on_click(cx.listener(move |this, _, _, cx| {
                                if let TreeMode::Choose { choice, .. } = &mut this.mode {
                                    *choice = c;
                                }
                                cx.notify();
                            })),
                    );
                }
                v_flex()
                    .gap_2()
                    .child(
                        div()
                            .text_sm()
                            .font_weight(FontWeight::SEMIBOLD)
                            .child("Summarize branch?"),
                    )
                    .child(options)
                    .when(current == SummaryChoice::Custom, |el| {
                        el.child(Input::new(&self.custom).small())
                    })
                    .child(Input::new(&self.label).small())
                    .child(
                        h_flex()
                            .gap_2()
                            .justify_end()
                            .child(
                                Button::new("choose-back")
                                    .label("Back")
                                    .small()
                                    .ghost()
                                    .on_click(cx.listener(|this, _, _, cx| {
                                        this.mode = TreeMode::Browse;
                                        cx.notify();
                                    })),
                            )
                            .child(
                                Button::new("choose-go")
                                    .label("Navigate")
                                    .small()
                                    .primary()
                                    .on_click(
                                        cx.listener(|this, _, w, cx| this.confirm_choice(w, cx)),
                                    ),
                            ),
                    )
                    .into_any_element()
            }
            TreeMode::Summarizing => h_flex()
                .gap_2()
                .items_center()
                .child(Spinner::new().small())
                .child(
                    div()
                        .flex_1()
                        .text_sm()
                        .text_color(theme.muted_foreground)
                        .child("Summarizing branch... (Esc to cancel)"),
                )
                .child(
                    Button::new("summary-cancel")
                        .label("Cancel")
                        .small()
                        .on_click(cx.listener(|this, _, _, cx| this.cancel_summary(cx))),
                )
                .into_any_element(),
            TreeMode::Label { .. } => h_flex()
                .gap_2()
                .child(div().flex_1().child(Input::new(&self.label).small()))
                .child(
                    Button::new("label-save")
                        .label("Save label")
                        .small()
                        .primary()
                        .on_click(cx.listener(|this, _, w, cx| this.save_label(false, w, cx))),
                )
                .child(
                    Button::new("label-clear")
                        .label("Clear")
                        .small()
                        .ghost()
                        .on_click(cx.listener(|this, _, w, cx| this.save_label(true, w, cx))),
                )
                .into_any_element(),
        }
    }
}

impl Render for TreePanel {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        let mut filters = h_flex().gap_1();
        for (i, f) in TreeFilter::ALL.into_iter().enumerate() {
            filters = filters.child(
                Button::new(("tree-filter", i))
                    .label(f.label())
                    .xsmall()
                    .map(|b| {
                        if f == self.filter {
                            b.primary()
                        } else {
                            b.ghost()
                        }
                    })
                    .on_click(cx.listener(move |this, _, _, cx| {
                        this.filter = f;
                        this.refresh(cx);
                        if this.selected_index().is_none() {
                            this.select_current();
                        }
                    })),
            );
        }
        let count = self.rows.len();
        let list = uniform_list(
            "session-tree",
            count,
            cx.processor(|this, range: std::ops::Range<usize>, _window, cx| {
                range
                    .filter_map(|i| this.rows.get(i).cloned().map(|r| (i, r)))
                    .map(|(i, row)| this.render_row(&row, i, cx))
                    .collect::<Vec<_>>()
            }),
        )
        .track_scroll(&self.scroll)
        .h(px(420.));
        v_flex()
            .gap_2()
            .on_key_down(cx.listener(|this, event: &KeyDownEvent, _, cx| {
                if event.keystroke.key == "escape" && matches!(this.mode, TreeMode::Summarizing) {
                    this.cancel_summary(cx);
                    cx.stop_propagation();
                }
            }))
            .child(
                h_flex()
                    .gap_2()
                    .items_center()
                    .child(filters)
                    .child(div().flex_1())
                    .child(div().w(px(220.)).child(Input::new(&self.search).small())),
            )
            .child(
                div()
                    .rounded_md()
                    .border_1()
                    .border_color(theme.border)
                    .p_1()
                    .child(if count == 0 {
                        div()
                            .p_4()
                            .text_sm()
                            .text_color(theme.muted_foreground)
                            .child("No entries in session")
                            .into_any_element()
                    } else {
                        list.into_any_element()
                    }),
            )
            .child(self.render_footer(cx))
            .when_some(self.message.clone(), |el, (error, text)| {
                el.child(
                    div()
                        .text_sm()
                        .text_color(if error {
                            theme.danger
                        } else {
                            theme.muted_foreground
                        })
                        .child(text),
                )
            })
    }
}

/// Open the session tree for `conversation`.
pub fn open_tree(
    engine: &Engine,
    conversation: &Conversation,
    on_action: ActionHandler,
    window: &mut Window,
    cx: &mut App,
) {
    let panel =
        cx.new(|cx| TreePanel::new(engine.clone(), conversation.clone(), on_action, window, cx));
    let conversation = conversation.clone();
    window.open_dialog(cx, move |dialog, _, cx| {
        // While a branch summary runs, Esc cancels it instead of closing.
        let busy = panel.read(cx).is_summarizing();
        let conversation = conversation.clone();
        let closing = panel.clone();
        dialog
            .title("Session tree")
            .on_ok(|_, _, _| false)
            .keyboard(!busy)
            .on_close(move |_, _, cx| {
                if closing.read(cx).is_summarizing() {
                    conversation.abort();
                }
            })
            .w(px(820.))
            .child(panel.clone())
    });
}

// ----- resume picker --------------------------------------------------------

pub struct SessionPicker {
    engine: Engine,
    current: PathBuf,
    all: bool,
    sessions: Vec<SessionSummary>,
    search: Entity<InputState>,
    rename: Entity<InputState>,
    import: Entity<InputState>,
    renaming: Option<PathBuf>,
    message: Option<(bool, String)>,
    on_action: ActionHandler,
    _subscriptions: Vec<Subscription>,
}

impl SessionPicker {
    pub fn new(
        engine: Engine,
        current: PathBuf,
        on_action: ActionHandler,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let search = cx.new(|cx| {
            InputState::new(window, cx).placeholder("Search name, messages, id or folder")
        });
        let rename = cx.new(|cx| InputState::new(window, cx).placeholder("Session name"));
        let import = cx.new(|cx| InputState::new(window, cx).placeholder("/path/to/session.jsonl"));
        let subscriptions = vec![
            cx.subscribe(&search, |_, _, e: &InputEvent, cx| {
                if matches!(e, InputEvent::Change) {
                    cx.notify();
                }
            }),
            cx.subscribe_in(&rename, window, |this, _, e: &InputEvent, window, cx| {
                if let InputEvent::PressEnter { .. } = e {
                    this.save_rename(window, cx);
                }
            }),
        ];
        search.update(cx, |s, cx| s.focus(window, cx));
        let mut this = Self {
            engine,
            current,
            all: false,
            sessions: Vec::new(),
            search,
            rename,
            import,
            renaming: None,
            message: None,
            on_action,
            _subscriptions: subscriptions,
        };
        this.reload();
        this
    }

    fn reload(&mut self) {
        self.sessions = if self.all {
            self.engine.all_sessions()
        } else {
            self.engine.sessions()
        };
    }

    fn begin_rename(
        &mut self,
        session: &SessionSummary,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.renaming = Some(session.path.clone());
        let name = session.name.clone().unwrap_or_default();
        self.rename.update(cx, |s, cx| {
            s.set_value(name, window, cx);
            s.focus(window, cx);
        });
        cx.notify();
    }

    fn save_rename(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let Some(path) = self.renaming.take() else {
            return;
        };
        let name = self.rename.read(cx).value().to_string();
        let result = if path == self.current {
            // The workspace renames the live conversation.
            Ok(None)
        } else {
            self.engine.open_conversation(&path).and_then(|c| {
                let r = c.set_name(&name);
                c.shutdown();
                r.map(|_| Some(()))
            })
        };
        match result {
            Ok(None) => (self.on_action)(SessionAction::RenameCurrent(name), window, cx),
            Ok(Some(())) => (self.on_action)(SessionAction::Renamed, window, cx),
            Err(err) => self.message = Some((true, format!("Could not rename: {err:#}"))),
        }
        self.reload();
        cx.notify();
    }

    fn browse_import(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let rx = cx.prompt_for_paths(PathPromptOptions {
            files: true,
            directories: false,
            multiple: false,
            prompt: Some("Import session".into()),
        });
        cx.spawn_in(window, async move |this, cx| {
            let picked = rx.await;
            let _ = this.update_in(cx, |this, window, cx| match picked {
                Ok(Ok(Some(paths))) => {
                    if let Some(path) = paths.into_iter().next() {
                        let text = path.display().to_string();
                        this.import
                            .update(cx, |s, cx| s.set_value(text, window, cx));
                    }
                }
                Ok(Err(err)) => {
                    this.message = Some((
                        true,
                        format!("File picker unavailable: {err:#}. Enter the path instead."),
                    ));
                    cx.notify();
                }
                _ => {}
            });
        })
        .detach();
    }

    fn import(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let text = self.import.read(cx).value().trim().to_string();
        if text.is_empty() {
            self.message = Some((
                true,
                "Usage: enter the path of a session .jsonl file".into(),
            ));
            cx.notify();
            return;
        }
        let path = qt_core::util::resolve_path(self.engine.cwd(), &text);
        (self.on_action)(SessionAction::Import(path), window, cx);
    }
}

pub fn relative_time(time: SystemTime) -> String {
    let secs = SystemTime::now()
        .duration_since(time)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    match secs {
        0..=59 => "just now".into(),
        60..=3_599 => format!("{} min ago", secs / 60),
        3_600..=86_399 => format!("{} h ago", secs / 3_600),
        86_400..=604_799 => format!("{} d ago", secs / 86_400),
        _ => chrono::DateTime::<chrono::Local>::from(time)
            .format("%Y-%m-%d")
            .to_string(),
    }
}

impl Render for SessionPicker {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        let query = self.search.read(cx).value().to_string();
        let shown: Vec<SessionSummary> = self
            .sessions
            .iter()
            .filter(|s| session_matches(s, &query))
            .cloned()
            .collect();
        let mut list = v_flex()
            .id("session-picker-list")
            .gap_0p5()
            .h(px(380.))
            .overflow_y_scroll();
        if shown.is_empty() {
            list = list.child(
                div()
                    .p_3()
                    .text_sm()
                    .text_color(theme.muted_foreground)
                    .child(if self.sessions.is_empty() {
                        "No sessions found."
                    } else {
                        "No sessions match the search."
                    }),
            );
        }
        for (i, session) in shown.into_iter().enumerate() {
            let current = session.path == self.current;
            let renaming = self.renaming.as_deref() == Some(session.path.as_path());
            let path = session.path.clone();
            let for_rename = session.clone();
            let mut meta = format!(
                "{} · {} messages",
                relative_time(session.modified),
                session.message_count
            );
            if self.all {
                meta.push_str(&format!(" · {}", session.cwd.display()));
            }
            let row = h_flex()
                .id(("session-pick", i))
                .gap_2()
                .items_center()
                .px_2()
                .py_1p5()
                .rounded_md()
                .cursor_pointer()
                .when(current, |el| el.bg(theme.list_active))
                .when(!current, |el| el.hover(|s| s.bg(theme.muted.opacity(0.5))))
                .on_click(cx.listener(move |this, _, window, cx| {
                    if this.renaming.is_none() {
                        (this.on_action)(SessionAction::Open(path.clone()), window, cx);
                    }
                }))
                .child(if renaming {
                    div()
                        .flex_1()
                        .child(Input::new(&self.rename).small())
                        .into_any_element()
                } else {
                    v_flex()
                        .flex_1()
                        .min_w_0()
                        .child(
                            div()
                                .text_sm()
                                .overflow_hidden()
                                .whitespace_nowrap()
                                .text_ellipsis()
                                .child(session.title.clone()),
                        )
                        .child(
                            div()
                                .text_xs()
                                .text_color(theme.muted_foreground)
                                .overflow_hidden()
                                .whitespace_nowrap()
                                .text_ellipsis()
                                .child(meta),
                        )
                        .into_any_element()
                })
                .child(if renaming {
                    Button::new(("rename-save", i))
                        .label("Save")
                        .xsmall()
                        .primary()
                        .on_click(cx.listener(|this, _, w, cx| this.save_rename(w, cx)))
                } else {
                    Button::new(("rename", i))
                        .icon(IconName::Replace)
                        .xsmall()
                        .ghost()
                        .tooltip("Rename")
                        .on_click(cx.listener(move |this, _, w, cx| {
                            cx.stop_propagation();
                            this.begin_rename(&for_rename, w, cx)
                        }))
                });
            list = list.child(row);
        }

        v_flex()
            .gap_2()
            .child(
                h_flex()
                    .gap_2()
                    .items_center()
                    .child(div().flex_1().child(Input::new(&self.search).small()))
                    .child(
                        Button::new("scope-folder")
                            .label("This folder")
                            .xsmall()
                            .map(|b| if self.all { b.ghost() } else { b.primary() })
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.all = false;
                                this.reload();
                                cx.notify();
                            })),
                    )
                    .child(
                        Button::new("scope-all")
                            .label("All sessions")
                            .xsmall()
                            .map(|b| if self.all { b.primary() } else { b.ghost() })
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.all = true;
                                this.reload();
                                cx.notify();
                            })),
                    ),
            )
            .child(
                div()
                    .rounded_md()
                    .border_1()
                    .border_color(theme.border)
                    .p_1()
                    .child(list),
            )
            .child(
                h_flex()
                    .gap_2()
                    .items_center()
                    .child(
                        div()
                            .text_xs()
                            .text_color(theme.muted_foreground)
                            .child("Import"),
                    )
                    .child(div().flex_1().child(Input::new(&self.import).small()))
                    .child(
                        Button::new("import-browse")
                            .label("Browse…")
                            .xsmall()
                            .ghost()
                            .on_click(cx.listener(|this, _, w, cx| this.browse_import(w, cx))),
                    )
                    .child(
                        Button::new("import-go")
                            .label("Import")
                            .xsmall()
                            .on_click(cx.listener(|this, _, w, cx| this.import(w, cx))),
                    ),
            )
            .when_some(self.message.clone(), |el, (error, text)| {
                el.child(
                    div()
                        .text_sm()
                        .text_color(if error {
                            theme.danger
                        } else {
                            theme.muted_foreground
                        })
                        .child(text),
                )
            })
    }
}

/// Open the resume picker.
pub fn open_picker(
    engine: &Engine,
    current: PathBuf,
    on_action: ActionHandler,
    window: &mut Window,
    cx: &mut App,
) {
    let panel = cx.new(|cx| SessionPicker::new(engine.clone(), current, on_action, window, cx));
    window.open_dialog(cx, move |dialog, _, _| {
        dialog
            .title("Resume session")
            .on_ok(|_, _, _| false)
            .w(px(720.))
            .child(panel.clone())
    });
}

#[cfg(test)]
mod tests {
    // Not `super::*`: the gpui glob import would shadow `#[test]`.
    use super::{SummaryChoice, navigate_options, skip_summary_prompt};
    use qt_core::{AgentHome, Engine};

    #[test]
    fn summary_choices_map_to_navigate_options() {
        let none = navigate_options(SummaryChoice::None, "ignored", "");
        assert!(!none.summarize && none.custom_instructions.is_none() && none.label.is_none());
        let plain = navigate_options(SummaryChoice::Summarize, "ignored", " mark ");
        assert!(plain.summarize && plain.custom_instructions.is_none());
        assert_eq!(plain.label.as_deref(), Some("mark"));
        let custom = navigate_options(SummaryChoice::Custom, " focus on tests ", "");
        assert_eq!(
            custom.custom_instructions.as_deref(),
            Some("focus on tests")
        );
        assert!(!custom.replace_instructions);
        assert_eq!(
            SummaryChoice::Custom.label(),
            "Summarize with custom prompt"
        );
    }

    #[test]
    fn skip_prompt_setting() {
        let dir = tempfile::tempdir().unwrap();
        let engine = Engine::new(AgentHome::new(dir.path().join("home")), dir.path()).unwrap();
        assert!(!skip_summary_prompt(&engine));
        engine
            .update_settings(|s| {
                s.raw_mut().insert(
                    "branchSummary".into(),
                    serde_json::json!({"skipPrompt": true}),
                );
            })
            .unwrap();
        assert!(skip_summary_prompt(&engine));
    }
}
