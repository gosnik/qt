//! `/tools` (active tool toggles and the mutation approval queue) and the
//! `/context` inspector.

use std::collections::HashSet;
use std::rc::Rc;
use std::time::Duration;

use gpui_kit::component::button::{Button, ButtonVariants as _};
use gpui_kit::component::checkbox::Checkbox;
use gpui_kit::component::input::{Input, InputEvent, InputState, Textarea, TextareaState};
use gpui_kit::component::notification::Notification;
use gpui_kit::component::{
    ActiveTheme as _, Disableable as _, Sizable as _, WindowExt as _, h_flex, v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::*;
use qt_core::slash::{CONTEXT_SECTIONS, context_lines};
use qt_core::tools::{MutationPolicy, PendingMutation};
use qt_core::{Conversation, Engine};

/// Reports an approved mutation's result: `(title, lines, is_error)`.
pub type ResultHandler = Rc<dyn Fn(String, Vec<String>, bool, &mut Window, &mut App)>;

/// One line describing a pending mutation.
pub fn mutation_line(m: &PendingMutation) -> String {
    format!("{} · {} · {}", m.id, m.kind.as_str(), m.summary)
}

/// Arguments preview: pretty JSON, at most `max_lines` lines.
pub fn arguments_preview(m: &PendingMutation, max_lines: usize) -> String {
    let text = serde_json::to_string_pretty(&m.arguments).unwrap_or_default();
    let lines: Vec<&str> = text.lines().collect();
    if lines.len() <= max_lines {
        return text;
    }
    let mut out = lines[..max_lines].join("\n");
    out.push_str(&format!("\n… {} more lines", lines.len() - max_lines));
    out
}

/// Tool names matching the filter (substring, case-insensitive).
pub fn tool_matches(name: &str, filter: &str) -> bool {
    let filter = filter.trim().to_lowercase();
    filter.is_empty() || name.to_lowercase().contains(&filter)
}

pub struct ToolsPanel {
    engine: Engine,
    conversation: Conversation,
    filter: Entity<InputState>,
    busy: HashSet<String>,
    results: Vec<(String, bool)>,
    on_result: ResultHandler,
    _subscription: Subscription,
    _refresh: Task<()>,
}

impl ToolsPanel {
    fn new(
        engine: Engine,
        conversation: Conversation,
        filter: Option<String>,
        on_result: ResultHandler,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let input = cx.new(|cx| {
            InputState::new(window, cx)
                .placeholder("Filter tools")
                .default_value(filter.unwrap_or_default())
        });
        let subscription = cx.subscribe(&input, |_, _, event: &InputEvent, cx| {
            if matches!(event, InputEvent::Change) {
                cx.notify();
            }
        });
        // Mutations are queued by running tool calls; refresh while open.
        let refresh = cx.spawn(async move |this, cx| {
            loop {
                cx.background_executor().timer(Duration::from_secs(1)).await;
                if this.update(cx, |_, cx| cx.notify()).is_err() {
                    break;
                }
            }
        });
        Self {
            engine,
            conversation,
            filter: input,
            busy: HashSet::new(),
            results: Vec::new(),
            on_result,
            _subscription: subscription,
            _refresh: refresh,
        }
    }

    fn toggle(&mut self, name: &str, enabled: bool, window: &mut Window, cx: &mut Context<Self>) {
        if let Err(err) = self.conversation.set_tool_enabled(name, enabled) {
            window.push_notification(Notification::error(format!("{err:#}")), cx);
        }
        cx.notify();
    }

    fn set_all(&mut self, enabled: bool, window: &mut Window, cx: &mut Context<Self>) {
        let names: Vec<String> = if enabled {
            self.conversation
                .tool_set()
                .iter()
                .map(|t| t.name().to_string())
                .collect()
        } else {
            Vec::new()
        };
        if let Err(err) = self.conversation.set_active_tools(&names) {
            window.push_notification(Notification::error(format!("{err:#}")), cx);
        }
        cx.notify();
    }

    fn approve(&mut self, id: String, window: &mut Window, cx: &mut Context<Self>) {
        let handle = match self.engine.execute_approved_mutation(&id) {
            Ok(handle) => handle,
            Err(err) => {
                window.push_notification(Notification::error(format!("{err:#}")), cx);
                return;
            }
        };
        self.busy.insert(id.clone());
        cx.notify();
        cx.spawn_in(window, async move |this, cx| {
            let (text, is_error) = match handle.await {
                Ok(output) => (output.text_content(), output.is_error),
                Err(err) => (format!("mutation task failed: {err}"), true),
            };
            let _ = this.update_in(cx, |this, window, cx| {
                this.busy.remove(&id);
                let headline = if is_error {
                    format!("{id} failed")
                } else {
                    format!("{id} applied")
                };
                this.results.insert(0, (headline.clone(), is_error));
                this.results.truncate(8);
                let lines: Vec<String> = text.lines().map(str::to_string).collect();
                (this.on_result)(format!("Approved {id}"), lines, is_error, window, cx);
                cx.notify();
            });
        })
        .detach();
    }

    fn reject(&mut self, id: &str, cx: &mut Context<Self>) {
        if self.engine.mutation_queue().reject(id) {
            self.results.insert(0, (format!("{id} rejected"), false));
            self.results.truncate(8);
        }
        cx.notify();
    }

    fn render_tools(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        let filter = self.filter.read(cx).value().to_string();
        let active = self.conversation.active_tools();
        let tools = self.conversation.tool_set();
        let mut list = v_flex()
            .id("tool-list")
            .gap_1()
            .max_h(px(300.))
            .overflow_y_scroll();
        let mut shown = 0;
        for (i, tool) in tools.iter().enumerate() {
            let name = tool.name().to_string();
            if !tool_matches(&name, &filter) {
                continue;
            }
            shown += 1;
            let checked = active.contains(&name);
            let description = tool.description();
            let first = description.lines().next().unwrap_or_default().to_string();
            list = list.child(
                h_flex()
                    .gap_2()
                    .items_start()
                    .child(
                        Checkbox::new(("tool", i))
                            .checked(checked)
                            .label(name.clone())
                            .on_click(cx.listener(move |this, value: &bool, window, cx| {
                                this.toggle(&name, *value, window, cx)
                            })),
                    )
                    .child(
                        div()
                            .flex_1()
                            .min_w_0()
                            .overflow_hidden()
                            .whitespace_nowrap()
                            .text_ellipsis()
                            .text_xs()
                            .text_color(theme.muted_foreground)
                            .child(first),
                    ),
            );
        }
        if shown == 0 {
            list = list.child(
                div()
                    .text_sm()
                    .text_color(theme.muted_foreground)
                    .child("No tools matched."),
            );
        }
        v_flex()
            .gap_2()
            .child(
                h_flex()
                    .gap_2()
                    .items_center()
                    .child(
                        div()
                            .flex_1()
                            .text_sm()
                            .font_weight(FontWeight::SEMIBOLD)
                            .child(format!(
                                "Tools ({} of {} active)",
                                active.len(),
                                tools.len()
                            )),
                    )
                    .child(
                        Button::new("tools-all")
                            .label("Enable all")
                            .xsmall()
                            .ghost()
                            .on_click(cx.listener(|this, _, w, cx| this.set_all(true, w, cx))),
                    )
                    .child(
                        Button::new("tools-none")
                            .label("Disable all")
                            .xsmall()
                            .ghost()
                            .on_click(cx.listener(|this, _, w, cx| this.set_all(false, w, cx))),
                    ),
            )
            .child(Input::new(&self.filter).small())
            .child(list)
            .child(
                div().text_xs().text_color(theme.muted_foreground).child(
                    "Changes are saved to the session and apply from the next model request.",
                ),
            )
    }

    fn render_queue(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        let policy = self.engine.mutation_policy();
        let pending = self.engine.mutation_queue().pending();
        let mut col = v_flex().gap_2().child(
            div()
                .text_sm()
                .font_weight(FontWeight::SEMIBOLD)
                .child(format!("Pending approvals ({})", pending.len())),
        );
        if policy == MutationPolicy::Execute {
            col = col.child(
                div()
                    .text_xs()
                    .text_color(theme.muted_foreground)
                    .child("Mutation policy is execute: file changes and commands run immediately. Start qt with the queue mutation policy to approve them here first."),
            );
        }
        if pending.is_empty() && policy == MutationPolicy::Queue {
            col = col.child(
                div()
                    .text_xs()
                    .text_color(theme.muted_foreground)
                    .child("No mutations are waiting for approval."),
            );
        }
        let mut list = v_flex()
            .id("mutation-list")
            .gap_2()
            .max_h(px(280.))
            .overflow_y_scroll();
        for (i, m) in pending.iter().enumerate() {
            let busy = self.busy.contains(&m.id);
            let approve_id = m.id.clone();
            let reject_id = m.id.clone();
            list = list.child(
                v_flex()
                    .gap_1()
                    .p_2()
                    .rounded_md()
                    .border_1()
                    .border_color(theme.border)
                    .child(
                        h_flex()
                            .gap_2()
                            .items_center()
                            .child(div().flex_1().min_w_0().text_sm().child(mutation_line(m)))
                            .child(
                                Button::new(("approve", i))
                                    .label("Approve")
                                    .xsmall()
                                    .primary()
                                    .disabled(busy)
                                    .on_click(cx.listener(move |this, _, w, cx| {
                                        this.approve(approve_id.clone(), w, cx)
                                    })),
                            )
                            .child(
                                Button::new(("reject", i))
                                    .label("Reject")
                                    .xsmall()
                                    .danger()
                                    .disabled(busy)
                                    .on_click(cx.listener(move |this, _, _, cx| {
                                        this.reject(&reject_id, cx)
                                    })),
                            ),
                    )
                    .child(
                        div()
                            .font_family(theme.mono_font_family.clone())
                            .text_xs()
                            .text_color(theme.muted_foreground)
                            .child(arguments_preview(m, 12)),
                    ),
            );
        }
        col = col.child(list);
        for (text, error) in &self.results {
            col = col.child(
                div()
                    .text_xs()
                    .text_color(if *error {
                        theme.danger
                    } else {
                        theme.muted_foreground
                    })
                    .child(text.clone()),
            );
        }
        col
    }
}

impl Render for ToolsPanel {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        v_flex()
            .gap_3()
            .child(self.render_tools(cx))
            .child(div().h(px(1.)).bg(theme.border))
            .child(self.render_queue(cx))
    }
}

/// Open the `/tools` panel.
pub fn open_tools(
    engine: &Engine,
    conversation: &Conversation,
    filter: Option<String>,
    on_result: ResultHandler,
    window: &mut Window,
    cx: &mut App,
) {
    let panel = cx.new(|cx| {
        ToolsPanel::new(
            engine.clone(),
            conversation.clone(),
            filter,
            on_result,
            window,
            cx,
        )
    });
    window.open_dialog(cx, move |dialog, _, _| {
        dialog
            .title("Tools")
            .w(px(680.))
            .on_ok(|_, _, _| false)
            .child(panel.clone())
    });
}

/// Sections offered by the inspector: `all` first.
pub fn inspector_sections() -> Vec<&'static str> {
    let mut sections = vec!["all"];
    sections.extend_from_slice(CONTEXT_SECTIONS);
    sections
}

pub struct ContextInspector {
    conversation: Conversation,
    section: &'static str,
    text: Entity<TextareaState>,
}

impl ContextInspector {
    fn new(
        conversation: Conversation,
        section: &'static str,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let text = cx.new(|cx| TextareaState::new(window, cx));
        let mut this = Self {
            conversation,
            section,
            text,
        };
        this.show(section, window, cx);
        this
    }

    fn show(&mut self, section: &'static str, window: &mut Window, cx: &mut Context<Self>) {
        self.section = section;
        let body = context_lines(&self.conversation, section).join("\n");
        self.text
            .update(cx, |state, cx| state.set_value(body, window, cx));
        cx.notify();
    }

    fn copy(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let text = self.text.read(cx).value().to_string();
        cx.write_to_clipboard(ClipboardItem::new_string(text));
        window.push_notification(Notification::info("Copied context to the clipboard"), cx);
    }
}

impl Render for ContextInspector {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let mut tabs = h_flex().gap_1().flex_wrap();
        for (i, section) in inspector_sections().into_iter().enumerate() {
            let active = section == self.section;
            tabs = tabs.child(
                Button::new(("context-section", i))
                    .label(section)
                    .xsmall()
                    .when(active, |b| b.primary())
                    .when(!active, |b| b.ghost())
                    .on_click(
                        cx.listener(move |this, _, window, cx| this.show(section, window, cx)),
                    ),
            );
        }
        v_flex()
            .gap_2()
            .child(
                h_flex()
                    .gap_2()
                    .items_center()
                    .child(div().flex_1().child(tabs))
                    .child(
                        Button::new("context-refresh")
                            .label("Refresh")
                            .xsmall()
                            .ghost()
                            .on_click(cx.listener(|this, _, window, cx| {
                                let section = this.section;
                                this.show(section, window, cx)
                            })),
                    )
                    .child(
                        Button::new("context-copy")
                            .label("Copy")
                            .xsmall()
                            .ghost()
                            .on_click(cx.listener(|this, _, w, cx| this.copy(w, cx))),
                    ),
            )
            .child(
                Textarea::new(&self.text)
                    .readonly(true)
                    .h(px(520.))
                    .font_family(cx.theme().mono_font_family.clone())
                    .text_xs(),
            )
    }
}

/// Open the `/context` inspector at `section` (`all` when unknown).
pub fn open_context(conversation: &Conversation, section: &str, window: &mut Window, cx: &mut App) {
    let section = inspector_sections()
        .into_iter()
        .find(|s| *s == section.trim().to_ascii_lowercase())
        .unwrap_or("all");
    let conversation = conversation.clone();
    let view = cx.new(|cx| ContextInspector::new(conversation, section, window, cx));
    window.open_dialog(cx, move |dialog, _, _| {
        dialog
            .title("Context inspector")
            .w(px(920.))
            .on_ok(|_, _, _| false)
            .child(view.clone())
    });
}

#[cfg(test)]
mod tests {
    use super::{arguments_preview, inspector_sections, mutation_line, tool_matches};
    use qt_core::tools::{MutationKind, PendingMutation};
    use serde_json::json;

    #[test]
    fn describes_mutations_and_filters_tools() {
        let m = PendingMutation {
            id: "mutation-1".into(),
            kind: MutationKind::FileWrite,
            tool_name: "write".into(),
            call_id: None,
            summary: "write src/a.rs".into(),
            arguments: json!({"path": "src/a.rs", "content": "a\nb"}),
        };
        assert_eq!(
            mutation_line(&m),
            "mutation-1 · file_write · write src/a.rs"
        );
        assert!(arguments_preview(&m, 12).contains("\"path\": \"src/a.rs\""));
        assert!(arguments_preview(&m, 2).ends_with("… 2 more lines"));
        assert!(tool_matches("web_fetch", "WEB"));
        assert!(tool_matches("read", " "));
        assert!(!tool_matches("read", "bash"));
        assert_eq!(inspector_sections()[0], "all");
        assert!(inspector_sections().contains(&"usage"));
    }
}
