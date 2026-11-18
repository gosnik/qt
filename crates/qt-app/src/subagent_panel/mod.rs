//! `/subagent` menu: role definitions (user + built-ins, project roles on
//! request), a single tracked run of one role, persistent workers (spawn,
//! send, follow up, wait, interrupt) and the parallel / chain workflow
//! prompts, through the runtime's `subagent` tool and worker coordinator.

use std::sync::{Arc, Mutex};

use gpui_kit::component::button::{Button, ButtonVariants as _};
use gpui_kit::component::input::{Input, InputState, Textarea, TextareaState};
use gpui_kit::component::{
    ActiveTheme as _, Disableable as _, Sizable as _, WindowExt as _, h_flex, v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::*;
use qt_core::tools::{Tool as _, ToolOutput};
use qt_runtime::Runtime;
use qt_runtime::qt_subagents::commands::{
    CHAIN_PROMPT, PARALLEL_PROMPT, definition_lines, help_lines, tool_result_lines,
    tool_unavailable_lines, use_agent_arguments, use_agent_label,
};
use qt_runtime::qt_subagents::definitions::{SubagentDefinition, discover_subagents};
use serde_json::{Value, json};
use tokio_util::sync::CancellationToken;

use crate::index_panel::{PanelHooks, output_view, section};

/// One persistent worker row.
#[derive(Debug, Clone, PartialEq)]
pub struct WorkerRow {
    pub path: String,
    pub name: String,
    pub agent: String,
    pub status: String,
    pub mailbox: u64,
    pub last_error: Option<String>,
}

/// Workers of a `list_workers` result (`{children: [...]}`).
pub fn worker_rows(list: &Value) -> Vec<WorkerRow> {
    let text = |v: &Value, k: &str| v.get(k).and_then(Value::as_str).unwrap_or("").to_string();
    list.get("children")
        .and_then(Value::as_array)
        .map(|children| {
            children
                .iter()
                .map(|c| WorkerRow {
                    path: Some(text(c, "id"))
                        .filter(|s| !s.is_empty())
                        .unwrap_or_else(|| text(c, "path")),
                    name: text(c, "name"),
                    agent: text(c, "agent"),
                    status: text(c, "status"),
                    mailbox: c.get("mailbox_depth").and_then(Value::as_u64).unwrap_or(0),
                    last_error: c
                        .get("last_error")
                        .and_then(Value::as_str)
                        .map(str::to_string),
                })
                .collect()
        })
        .unwrap_or_default()
}

/// Tool arguments of a worker action on `target`.
pub fn worker_arguments(action: &str, target: &str, text: &str) -> Result<Value, String> {
    let mut value = json!({"action": action, "target": target});
    match action {
        "send" | "follow_up" => {
            let text = text.trim();
            if text.is_empty() {
                return Err(format!("{action} needs a message."));
            }
            value[if action == "send" { "message" } else { "task" }] = json!(text);
        }
        "wait" => value["timeout_ms"] = json!(1000),
        "interrupt" => {}
        other => return Err(format!("unknown worker action: {other}")),
    }
    Ok(value)
}

/// Arguments of a `spawn` from the panel fields.
pub fn spawn_arguments(agent: &str, task: &str, name: &str) -> Option<Value> {
    let task = task.trim();
    if task.is_empty() {
        return None;
    }
    let mut value = json!({"action": "spawn", "agent": agent, "task": task});
    if !name.trim().is_empty() {
        value["name"] = json!(name.trim());
    }
    Some(value)
}

/// Agent scopes offered by the panel.
pub const SCOPES: [&str; 3] = ["user", "project", "both"];

pub struct SubagentPanel {
    runtime: Runtime,
    hooks: PanelHooks,
    scope: &'static str,
    agents: Vec<SubagentDefinition>,
    diagnostics: Vec<String>,
    selected: usize,
    task: Entity<TextareaState>,
    worker_name: Entity<InputState>,
    message: Entity<InputState>,
    workers: Vec<WorkerRow>,
    title: String,
    output: Vec<String>,
    error: bool,
    busy: Option<String>,
    progress: Arc<Mutex<String>>,
    cancel: Option<CancellationToken>,
    _task: Option<Task<()>>,
    _ticker: Option<Task<()>>,
}

impl SubagentPanel {
    fn new(
        runtime: Runtime,
        hooks: PanelHooks,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let mut this = Self {
            task: cx.new(|cx| {
                TextareaState::new(window, cx)
                    .auto_grow(3, 8)
                    .placeholder("Task for the selected role")
            }),
            worker_name: cx.new(|cx| {
                InputState::new(window, cx).placeholder("Worker name (optional, for Spawn)")
            }),
            message: cx
                .new(|cx| InputState::new(window, cx).placeholder("Message or task for a worker")),
            runtime,
            hooks,
            scope: "user",
            agents: Vec::new(),
            diagnostics: Vec::new(),
            selected: 0,
            workers: Vec::new(),
            title: String::new(),
            output: Vec::new(),
            error: false,
            busy: None,
            progress: Arc::new(Mutex::new(String::new())),
            cancel: None,
            _task: None,
            _ticker: None,
        };
        this.discover();
        this.refresh_workers();
        this
    }

    fn agent_home(&self) -> std::path::PathBuf {
        self.runtime.agent_home().root().to_path_buf()
    }

    fn discover(&mut self) {
        let found = discover_subagents(&self.agent_home(), self.runtime.engine().cwd(), self.scope);
        self.agents = found.agents;
        self.diagnostics = found.diagnostics;
        self.selected = self.selected.min(self.agents.len().saturating_sub(1));
    }

    fn refresh_workers(&mut self) {
        self.workers = self
            .runtime
            .subagents()
            .coordinator()
            .and_then(|c| c.list().ok())
            .map(|v| worker_rows(&v))
            .unwrap_or_default();
    }

    fn current(&self) -> Option<&SubagentDefinition> {
        self.agents.get(self.selected)
    }

    fn tool_available(&self) -> bool {
        self.runtime.engine().tools().get("subagent").is_some()
    }

    /// Run the `subagent` tool with `arguments`; `card` posts the result to
    /// the transcript (also after the panel closed).
    fn run_tool(
        &mut self,
        arguments: Value,
        label: String,
        card: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if !self.tool_available() {
            self.error = true;
            self.output = tool_unavailable_lines();
            cx.notify();
            return;
        }
        if self.busy.is_some() {
            return;
        }
        let engine = self.runtime.engine().clone();
        let cancel = CancellationToken::new();
        let progress = self.progress.clone();
        *progress.lock().unwrap_or_else(|e| e.into_inner()) = String::new();
        let ctx = engine
            .tool_environment(None)
            .context(engine.cwd().to_path_buf())
            .with_cancel(cancel.clone())
            .with_progress(Arc::new(move |text: String| {
                *progress.lock().unwrap_or_else(|e| e.into_inner()) = text;
            }));
        let tool = self.runtime.subagents().current_tool();
        let handle = engine.spawn(async move { tool.execute(arguments, ctx).await });
        self.busy = Some(format!("{label}…"));
        self.cancel = Some(cancel);
        let hooks = self.hooks.clone();
        let this_weak = cx.entity().downgrade();
        self._ticker = Some(cx.spawn(async move |this, cx| {
            loop {
                cx.background_executor()
                    .timer(std::time::Duration::from_millis(500))
                    .await;
                if this.update(cx, |_, cx| cx.notify()).is_err() {
                    break;
                }
            }
        }));
        cx.spawn_in(window, async move |_, cx| {
            let output = handle
                .await
                .unwrap_or_else(|e| ToolOutput::error(format!("subagent tool failed: {e}")));
            let lines = tool_result_lines(&output);
            let error = output.is_error;
            let _ = cx.update(|window, cx| {
                if card {
                    (hooks.card)(label.clone(), lines.clone(), error, window, cx);
                }
                let _ = this_weak.update(cx, |this, cx| {
                    this.busy = None;
                    this.cancel = None;
                    this._ticker = None;
                    this.title = label;
                    this.error = error;
                    this.output = lines;
                    this.refresh_workers();
                    cx.notify();
                });
            });
        })
        .detach();
        cx.notify();
    }

    fn use_agent(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let Some(agent) = self.current().map(|a| a.name.clone()) else {
            return;
        };
        let task = self.task.read(cx).value().to_string();
        match use_agent_arguments(&agent, &task) {
            Some(args) => {
                let label = use_agent_label(&agent, &task);
                self.run_tool(args, label, true, window, cx)
            }
            None => {
                self.error = true;
                self.output = vec!["Enter a task for the subagent first.".into()];
                cx.notify();
            }
        }
    }

    fn spawn_worker(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let Some(agent) = self.current().map(|a| a.name.clone()) else {
            return;
        };
        let task = self.task.read(cx).value().to_string();
        let name = self.worker_name.read(cx).value().to_string();
        match spawn_arguments(&agent, &task, &name) {
            Some(args) => self.run_tool(
                args,
                format!("/subagent spawn {agent} {}", task.trim()),
                true,
                window,
                cx,
            ),
            None => {
                self.error = true;
                self.output = vec!["Enter a task for the worker first.".into()];
                cx.notify();
            }
        }
    }

    fn worker_action(
        &mut self,
        action: &'static str,
        target: String,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let text = self.message.read(cx).value().to_string();
        match worker_arguments(action, &target, &text) {
            Ok(args) => {
                let label = format!("/subagent {action} {target}");
                self.run_tool(args, label, action != "wait", window, cx);
                if matches!(action, "send" | "follow_up") {
                    self.message.update(cx, |s, cx| s.set_value("", window, cx));
                }
            }
            Err(e) => {
                self.error = true;
                self.output = vec![e];
                cx.notify();
            }
        }
    }

    fn prefill(&mut self, text: &'static str, window: &mut Window, cx: &mut Context<Self>) {
        window.close_dialog(cx);
        (self.hooks.prefill)(text.to_string(), window, cx);
    }
}

impl Render for SubagentPanel {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        let busy = self.busy.is_some();

        let mut scopes = h_flex().gap_1();
        for scope in SCOPES {
            let selected = self.scope == scope;
            scopes = scopes.child(
                Button::new(SharedString::from(format!("subagent-scope-{scope}")))
                    .label(scope)
                    .xsmall()
                    .when(selected, |b| b.primary())
                    .when(!selected, |b| b.ghost())
                    .on_click(cx.listener(move |this, _, _, cx| {
                        this.scope = scope;
                        this.discover();
                        cx.notify();
                    })),
            );
        }
        let mut roles = v_flex()
            .id("subagent-roles")
            .gap_0p5()
            .max_h(px(300.))
            .overflow_y_scroll();
        if self.agents.is_empty() {
            roles = roles.child(
                div()
                    .text_xs()
                    .text_color(theme.muted_foreground)
                    .child("No executable subagents discovered."),
            );
        }
        for (i, agent) in self.agents.iter().enumerate() {
            let selected = i == self.selected;
            roles = roles.child(
                v_flex()
                    .id(("subagent-role", i))
                    .px_2()
                    .py_1()
                    .rounded_md()
                    .cursor_pointer()
                    .when(selected, |el| el.bg(theme.list_active))
                    .when(!selected, |el| el.hover(|s| s.bg(theme.muted.opacity(0.5))))
                    .on_click(cx.listener(move |this, _, _, cx| {
                        this.selected = i;
                        cx.notify();
                    }))
                    .child(
                        div()
                            .text_sm()
                            .child(format!("{} [{}]", agent.name, agent.source)),
                    )
                    .child(
                        div()
                            .text_xs()
                            .text_color(theme.muted_foreground)
                            .child(qt_core::util::shorten(&agent.description, 90)),
                    ),
            );
        }
        let left = v_flex()
            .w(px(250.))
            .flex_none()
            .gap_2()
            .child(section("ROLES", cx))
            .child(scopes)
            .child(roles)
            .children(self.diagnostics.iter().map(|d| {
                div()
                    .text_xs()
                    .text_color(theme.warning)
                    .child(format!("warning: {d}"))
            }))
            .when(self.scope != "user", |el| {
                el.child(
                    div()
                        .text_xs()
                        .text_color(theme.muted_foreground)
                        .child("Project roles are repository-controlled and require explicit trust before execution."),
                )
            })
            .child(section("WORKFLOWS", cx))
            .child(
                h_flex()
                    .gap_1()
                    .flex_wrap()
                    .child(
                        Button::new("subagent-parallel")
                            .label("Parallel delegation")
                            .xsmall()
                            .ghost()
                            .on_click(cx.listener(|this, _, w, cx| {
                                this.prefill(PARALLEL_PROMPT, w, cx)
                            })),
                    )
                    .child(
                        Button::new("subagent-chain")
                            .label("Sequential chain")
                            .xsmall()
                            .ghost()
                            .on_click(cx.listener(|this, _, w, cx| this.prefill(CHAIN_PROMPT, w, cx))),
                    )
                    .child(
                        Button::new("subagent-help")
                            .label("Help")
                            .xsmall()
                            .ghost()
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.title = "/subagent help".into();
                                this.error = false;
                                this.output = help_lines(&this.agent_home());
                                cx.notify();
                            })),
                    ),
            );

        let mut right = v_flex().flex_1().min_w_0().gap_2();
        if let Some(agent) = self.current().cloned() {
            right = right
                .children(definition_lines(&agent).into_iter().map(|l| {
                    div()
                        .text_xs()
                        .text_color(theme.muted_foreground)
                        .child(SharedString::from(l))
                }))
                .child(Textarea::new(&self.task))
                .child(Input::new(&self.worker_name).small())
                .child(
                    h_flex()
                        .gap_1()
                        .child(
                            Button::new("subagent-run")
                                .label("Run once")
                                .small()
                                .primary()
                                .disabled(busy)
                                .on_click(cx.listener(|this, _, w, cx| this.use_agent(w, cx))),
                        )
                        .child(
                            Button::new("subagent-spawn")
                                .label("Spawn worker")
                                .small()
                                .disabled(busy)
                                .on_click(cx.listener(|this, _, w, cx| this.spawn_worker(w, cx))),
                        )
                        .when(busy, |el| {
                            el.child(
                                Button::new("subagent-cancel")
                                    .label("Cancel")
                                    .small()
                                    .ghost()
                                    .on_click(cx.listener(|this, _, _, cx| {
                                        if let Some(cancel) = &this.cancel {
                                            cancel.cancel();
                                        }
                                        cx.notify();
                                    })),
                            )
                        }),
                );
        }
        right = right
            .child(
                h_flex()
                    .gap_2()
                    .items_center()
                    .child(div().flex_1().child(section("WORKERS", cx)))
                    .child(
                        Button::new("subagent-workers-refresh")
                            .label("Refresh")
                            .xsmall()
                            .ghost()
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.refresh_workers();
                                cx.notify();
                            })),
                    ),
            )
            .child(Input::new(&self.message).small());
        let mut workers = v_flex()
            .id("subagent-workers")
            .gap_0p5()
            .max_h(px(180.))
            .overflow_y_scroll();
        if self.workers.is_empty() {
            workers = workers.child(
                div()
                    .text_xs()
                    .text_color(theme.muted_foreground)
                    .child("No persistent workers in this session."),
            );
        }
        for (i, worker) in self.workers.iter().enumerate() {
            let mut row = h_flex().gap_1().items_center().child(
                v_flex()
                    .flex_1()
                    .min_w_0()
                    .child(div().text_sm().child(worker.path.clone()))
                    .child(
                        div()
                            .text_xs()
                            .text_color(if worker.last_error.is_some() {
                                theme.danger
                            } else {
                                theme.muted_foreground
                            })
                            .child(format!(
                                "{} · {} · mailbox {}{}",
                                worker.agent,
                                worker.status,
                                worker.mailbox,
                                worker
                                    .last_error
                                    .as_deref()
                                    .map(|e| format!(" · {e}"))
                                    .unwrap_or_default()
                            )),
                    ),
            );
            for (j, (action, label)) in [
                ("send", "Send"),
                ("follow_up", "Follow up"),
                ("wait", "Wait"),
                ("interrupt", "Interrupt"),
            ]
            .into_iter()
            .enumerate()
            {
                let target = worker.path.clone();
                row = row.child(
                    Button::new(("subagent-worker", i * 4 + j))
                        .label(label)
                        .xsmall()
                        .ghost()
                        .disabled(busy)
                        .on_click(cx.listener(move |this, _, w, cx| {
                            this.worker_action(action, target.clone(), w, cx)
                        })),
                );
            }
            workers = workers.child(row);
        }
        right = right.child(workers);

        let progress = self
            .progress
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .clone();
        let busy_label = self.busy.as_ref().map(|b| {
            if progress.is_empty() {
                b.clone()
            } else {
                format!("{b}\n{progress}")
            }
        });
        v_flex()
            .gap_3()
            .child(h_flex().gap_4().items_start().child(left).child(right))
            .child(output_view(
                "subagent-output",
                &self.output,
                self.error,
                busy_label.as_deref(),
                260.,
                cx,
            ))
    }
}

/// Open the `/subagent` menu.
pub fn open(runtime: Runtime, hooks: PanelHooks, window: &mut Window, cx: &mut App) {
    let panel = cx.new(|cx| SubagentPanel::new(runtime, hooks, window, cx));
    window.open_dialog(cx, move |dialog, _, _| {
        dialog
            .title("Subagents · user + builtins")
            .w(px(900.))
            .on_ok(|_, _, _| false)
            .child(panel.clone())
    });
}

#[cfg(test)]
mod tests {
    use super::{spawn_arguments, worker_arguments, worker_rows};
    use serde_json::json;

    #[test]
    fn worker_rows_and_arguments() {
        let rows = worker_rows(&json!({"children": [
            {"id": "/root/a", "path": "/x/a/session.jsonl", "name": "a", "agent": "scout", "status": "idle", "mailbox_depth": 2},
            {"id": "/root/b", "name": "b", "agent": "worker", "status": "failed", "last_error": "boom"}
        ]}));
        assert_eq!(rows.len(), 2);
        assert_eq!(rows[0].mailbox, 2);
        assert_eq!(rows[1].last_error.as_deref(), Some("boom"));
        assert!(worker_rows(&json!({})).is_empty());
        assert_eq!(
            worker_arguments("send", "/root/a", " hi ").unwrap(),
            json!({"action": "send", "target": "/root/a", "message": "hi"})
        );
        assert_eq!(
            worker_arguments("follow_up", "/root/a", "go").unwrap()["task"],
            "go"
        );
        assert_eq!(
            worker_arguments("wait", "/root/a", "").unwrap()["timeout_ms"],
            1000
        );
        assert!(worker_arguments("send", "/root/a", " ").is_err());
        assert!(worker_arguments("explode", "/root/a", "").is_err());
        assert_eq!(
            spawn_arguments("scout", "map it", " w1 ").unwrap(),
            json!({"action": "spawn", "agent": "scout", "task": "map it", "name": "w1"})
        );
        assert_eq!(spawn_arguments("scout", " ", ""), None);
    }
}
