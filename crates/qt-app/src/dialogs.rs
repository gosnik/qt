//! Small dialogs: manual compaction, goal controls, session rename and the
//! fork message picker.

use std::rc::Rc;

use async_channel::Receiver;
use gpui_kit::component::button::{Button, ButtonVariants as _};
use gpui_kit::component::input::{Input, InputEvent, InputState};
use gpui_kit::component::{
    ActiveTheme as _, Disableable as _, Sizable as _, WindowExt as _, h_flex, v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::*;
use qt_core::harness::goals::GOAL_USAGE;
use qt_core::{AgentEvent, Conversation};

use crate::run_status::{goal_controls, goal_line, parse_budget};
use crate::sessions::first_line_truncated;

/// Receives the event stream of a run started from a dialog.
pub type RunHandler = Rc<dyn Fn(Receiver<AgentEvent>, &mut Window, &mut App)>;

/// Ask for optional compaction instructions, then call `on_compact`.
pub fn open_compact(
    on_compact: impl Fn(Option<String>, &mut Window, &mut App) + 'static,
    window: &mut Window,
    cx: &mut App,
) {
    let input = cx.new(|cx| {
        InputState::new(window, cx)
            .placeholder("Optional instructions, e.g. focus on the open bugs")
    });
    input.update(cx, |s, cx| s.focus(window, cx));
    let on_compact = Rc::new(on_compact);
    window.open_dialog(cx, move |dialog, _, cx| {
        let input_ok = input.clone();
        let run = on_compact.clone();
        dialog
            .title("Compact context")
            .w(px(520.))
            .child(
                v_flex()
                    .gap_2()
                    .child(
                        div()
                            .text_sm()
                            .text_color(cx.theme().muted_foreground)
                            .child(
                                "Older messages are replaced by a summary; recent turns are kept.",
                            ),
                    )
                    .child(Input::new(&input)),
            )
            .footer(
                h_flex()
                    .w_full()
                    .justify_end()
                    .gap_2()
                    .child(
                        Button::new("compact-cancel")
                            .label("Cancel")
                            .on_click(|_, window, cx| window.close_dialog(cx)),
                    )
                    .child(
                        Button::new("compact-go")
                            .label("Compact")
                            .primary()
                            .on_click(move |_, window, cx| {
                                let text = input_ok.read(cx).value().trim().to_string();
                                window.close_dialog(cx);
                                run(Some(text).filter(|t| !t.is_empty()), window, cx);
                            }),
                    ),
            )
    });
}

/// Rename the session (`/name`); empty clears the name.
pub fn open_rename(
    current: Option<String>,
    on_rename: impl Fn(String, &mut Window, &mut App) + 'static,
    window: &mut Window,
    cx: &mut App,
) {
    let input = cx.new(|cx| {
        InputState::new(window, cx)
            .placeholder("Session name")
            .default_value(current.unwrap_or_default())
    });
    input.update(cx, |s, cx| s.focus(window, cx));
    let on_rename = Rc::new(on_rename);
    window.open_dialog(cx, move |dialog, _, _| {
        let input_ok = input.clone();
        let run = on_rename.clone();
        dialog
            .title("Rename session")
            .w(px(460.))
            .child(Input::new(&input))
            .footer(
                h_flex()
                    .w_full()
                    .justify_end()
                    .gap_2()
                    .child(
                        Button::new("rename-cancel")
                            .label("Cancel")
                            .on_click(|_, window, cx| window.close_dialog(cx)),
                    )
                    .child(Button::new("rename-ok").label("Save").primary().on_click(
                        move |_, window, cx| {
                            let text = input_ok.read(cx).value().to_string();
                            window.close_dialog(cx);
                            run(text, window, cx);
                        },
                    )),
            )
    });
}

/// Pick a user message to fork from (`/fork`).
pub fn open_fork_picker(
    messages: Vec<(String, String)>,
    on_pick: impl Fn(String, &mut Window, &mut App) + 'static,
    window: &mut Window,
    cx: &mut App,
) {
    let on_pick = Rc::new(on_pick);
    window.open_dialog(cx, move |dialog, _, cx| {
        let theme = cx.theme().clone();
        let mut list = v_flex()
            .id("fork-list")
            .gap_0p5()
            .max_h(px(420.))
            .overflow_y_scroll();
        if messages.is_empty() {
            list = list.child(
                div()
                    .text_sm()
                    .text_color(theme.muted_foreground)
                    .child("No messages to fork from"),
            );
        }
        for (i, (id, text)) in messages.iter().enumerate().rev() {
            let id = id.clone();
            let pick = on_pick.clone();
            list = list.child(
                div()
                    .id(("fork-item", i))
                    .px_2()
                    .py_1p5()
                    .rounded_md()
                    .cursor_pointer()
                    .hover(|s| s.bg(theme.muted.opacity(0.6)))
                    .text_sm()
                    .on_click(move |_, window, cx| {
                        window.close_dialog(cx);
                        pick(id.clone(), window, cx);
                    })
                    .child(first_line_truncated(text, 100)),
            );
        }
        dialog
            .title("Fork from a message")
            .w(px(620.))
            .child(
                v_flex()
                    .gap_2()
                    .child(
                        div()
                            .text_xs()
                            .text_color(theme.muted_foreground)
                            .child("The new session ends before the selected message, which is put back in the composer."),
                    )
                    .child(list),
            )
    });
}

/// Goal controls (`/goal`).
pub struct GoalPanel {
    conversation: Conversation,
    objective: Entity<InputState>,
    budget: Entity<InputState>,
    lines: Vec<String>,
    error: bool,
    on_run: RunHandler,
    on_changed: Rc<dyn Fn(&mut Window, &mut App)>,
    _subscriptions: Vec<Subscription>,
}

impl GoalPanel {
    pub fn new(
        conversation: Conversation,
        on_run: RunHandler,
        on_changed: Rc<dyn Fn(&mut Window, &mut App)>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let goal = conversation.goal();
        let objective = cx.new(|cx| {
            InputState::new(window, cx)
                .placeholder("Objective, e.g. make every test in crates/core pass")
                .default_value(
                    goal.as_ref()
                        .map(|g| g.objective.clone())
                        .unwrap_or_default(),
                )
        });
        let budget = cx.new(|cx| {
            InputState::new(window, cx)
                .placeholder("Token budget (optional, e.g. 200k)")
                .default_value(
                    goal.as_ref()
                        .and_then(|g| g.token_budget)
                        .map(|b| b.to_string())
                        .unwrap_or_default(),
                )
        });
        objective.update(cx, |s, cx| s.focus(window, cx));
        let subscriptions =
            vec![
                cx.subscribe_in(&objective, window, |this, _, e: &InputEvent, window, cx| {
                    if let InputEvent::PressEnter { .. } = e {
                        this.set_goal(window, cx);
                    }
                }),
            ];
        Self {
            conversation,
            objective,
            budget,
            lines: Vec::new(),
            error: false,
            on_run,
            on_changed,
            _subscriptions: subscriptions,
        }
    }

    fn command(&mut self, args: &str, window: &mut Window, cx: &mut Context<Self>) {
        match self.conversation.goal_command(args) {
            Ok((output, rx)) => {
                self.lines = output.lines;
                self.error = false;
                if let Some(rx) = rx {
                    (self.on_run)(rx, window, cx);
                }
            }
            Err(err) => {
                self.lines = vec![format!("{err:#}")];
                self.error = true;
            }
        }
        (self.on_changed)(window, cx);
        cx.notify();
    }

    fn set_goal(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let objective = self.objective.read(cx).value().trim().to_string();
        if objective.is_empty() {
            self.lines = vec![GOAL_USAGE.to_string()];
            self.error = true;
            cx.notify();
            return;
        }
        let budget = match parse_budget(&self.budget.read(cx).value()) {
            Ok(b) => b,
            Err(e) => {
                self.lines = vec![e];
                self.error = true;
                cx.notify();
                return;
            }
        };
        let current = self.conversation.goal();
        let same_budget = current.as_ref().is_some_and(|g| g.token_budget == budget);
        if current.as_ref().is_some_and(|g| g.is_active()) && same_budget {
            // Edit the active goal's objective (steers a running agent).
            if current.is_some_and(|g| g.objective == objective) {
                self.lines = vec!["Goal unchanged.".into()];
                self.error = false;
                cx.notify();
                return;
            }
            self.command(&objective, window, cx);
            return;
        }
        match self.conversation.set_goal(&objective, budget) {
            Ok(rx) => {
                self.lines = vec![
                    "Goal set.".into(),
                    format!("Objective: {objective}"),
                    if rx.is_some() {
                        "Starting autonomous goal execution...".into()
                    } else {
                        "The goal continues when the current run ends.".into()
                    },
                ];
                self.error = false;
                if let Some(rx) = rx {
                    (self.on_run)(rx, window, cx);
                }
            }
            Err(err) => {
                self.lines = vec![format!("{err:#}")];
                self.error = true;
            }
        }
        (self.on_changed)(window, cx);
        cx.notify();
    }
}

impl Render for GoalPanel {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        let goal = self.conversation.goal();
        let (can_pause, can_resume) = goal
            .as_ref()
            .map(|g| goal_controls(g.status))
            .unwrap_or((false, false));
        v_flex()
            .gap_2()
            .child(
                div()
                    .text_sm()
                    .text_color(theme.muted_foreground)
                    .child(match &goal {
                        Some(g) => goal_line(g),
                        None => "No active goal.".to_string(),
                    }),
            )
            .child(Input::new(&self.objective))
            .child(Input::new(&self.budget).small())
            .child(
                h_flex()
                    .gap_2()
                    .child(
                        Button::new("goal-set")
                            .label(if goal.as_ref().is_some_and(|g| g.is_active()) {
                                "Update goal"
                            } else {
                                "Set goal"
                            })
                            .small()
                            .primary()
                            .on_click(cx.listener(|this, _, w, cx| this.set_goal(w, cx))),
                    )
                    .child(
                        Button::new("goal-pause")
                            .label("Pause")
                            .small()
                            .disabled(!can_pause)
                            .on_click(cx.listener(|this, _, w, cx| this.command("pause", w, cx))),
                    )
                    .child(
                        Button::new("goal-resume")
                            .label("Resume")
                            .small()
                            .disabled(!can_resume)
                            .on_click(cx.listener(|this, _, w, cx| this.command("resume", w, cx))),
                    )
                    .child(
                        Button::new("goal-status")
                            .label("Status")
                            .small()
                            .ghost()
                            .on_click(cx.listener(|this, _, w, cx| this.command("", w, cx))),
                    )
                    .child(div().flex_1())
                    .child(
                        Button::new("goal-clear")
                            .label("Clear")
                            .small()
                            .ghost()
                            .disabled(goal.is_none())
                            .on_click(cx.listener(|this, _, w, cx| this.command("clear", w, cx))),
                    ),
            )
            .when(!self.lines.is_empty(), |el| {
                el.child(
                    v_flex()
                        .gap_0p5()
                        .p_2()
                        .rounded_md()
                        .bg(theme.muted.opacity(0.5))
                        .children(self.lines.iter().map(|l| {
                            div()
                                .text_xs()
                                .text_color(if self.error {
                                    theme.danger
                                } else {
                                    theme.foreground
                                })
                                .child(SharedString::from(l.clone()))
                        })),
                )
            })
    }
}

pub fn open_goal(
    conversation: &Conversation,
    on_run: RunHandler,
    on_changed: Rc<dyn Fn(&mut Window, &mut App)>,
    window: &mut Window,
    cx: &mut App,
) {
    let panel = cx.new(|cx| GoalPanel::new(conversation.clone(), on_run, on_changed, window, cx));
    window.open_dialog(cx, move |dialog, _, _| {
        dialog.title("Goal").w(px(600.)).child(panel.clone())
    });
}
