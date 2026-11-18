//! Extension dialogs (`ctx.ui.select/confirm/input/editor`) and scripted
//! components (`ctx.ui.custom`, `setEditorComponent`) as GPUI views.
//!
//! Every dialog answers exactly once: a button, Enter, or — when the view
//! is dropped because the dialog closed in any other way (Escape, the close
//! button, a timeout) — cancellation.

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;
use std::time::{Duration, Instant};

use gpui_kit::component::button::{Button, ButtonVariants as _};
use gpui_kit::component::input::{Input, InputState, Textarea, TextareaState};
use gpui_kit::component::{ActiveTheme as _, Sizable as _, WindowExt as _, h_flex, v_flex};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::*;
use qt_runtime::qt_extensions::ui::ScriptComponent;

use super::bridge::{
    COMPONENT_COLUMNS, COMPONENT_ROWS, ComponentEvent, DialogAnswer, DialogKind, DialogRequest,
};
use super::keys::{Pressed, input_data};

/// The pressed key of a GPUI keystroke.
pub fn pressed(keystroke: &Keystroke) -> Pressed {
    Pressed {
        control: keystroke.modifiers.control,
        alt: keystroke.modifiers.alt,
        shift: keystroke.modifiers.shift,
        platform: keystroke.modifiers.platform,
        key: keystroke.key.clone(),
        key_char: keystroke.key_char.clone(),
    }
}

/// Seconds left before `deadline`, rounded up.
pub fn seconds_left(deadline: Instant, now: Instant) -> u64 {
    let left = deadline.saturating_duration_since(now);
    left.as_secs() + u64::from(left.subsec_nanos() > 0)
}

/// One open extension dialog.
pub struct ExtDialog {
    title: String,
    kind: DialogKind,
    deadline: Option<Instant>,
    reply: Option<mpsc::SyncSender<Option<DialogAnswer>>>,
    expired: Arc<AtomicBool>,
    input: Option<Entity<InputState>>,
    editor: Option<Entity<TextareaState>>,
    selected: usize,
    focus: FocusHandle,
}

impl Drop for ExtDialog {
    fn drop(&mut self) {
        self.answer(None);
    }
}

impl ExtDialog {
    fn new(request: DialogRequest, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let input = match &request.kind {
            DialogKind::Input { placeholder } => {
                let placeholder = placeholder.clone();
                let state = cx.new(|cx| InputState::new(window, cx).placeholder(placeholder));
                state.update(cx, |s, cx| s.focus(window, cx));
                Some(state)
            }
            _ => None,
        };
        let editor = match &request.kind {
            DialogKind::Editor { prefill } => {
                let prefill = prefill.clone();
                let state = cx.new(|cx| TextareaState::new(window, cx).auto_grow(6, 18));
                state.update(cx, |s, cx| {
                    s.set_value(prefill, window, cx);
                    s.focus(window, cx);
                });
                Some(state)
            }
            _ => None,
        };
        let focus = cx.focus_handle();
        if input.is_none() && editor.is_none() {
            window.focus(&focus, cx);
        }
        Self {
            title: request.title,
            kind: request.kind,
            deadline: request.timeout.map(|t| Instant::now() + t),
            reply: Some(request.reply),
            expired: request.expired,
            input,
            editor,
            selected: 0,
            focus,
        }
    }

    /// The dialog was answered (it closed itself).
    pub fn answered(&self) -> bool {
        self.reply.is_none()
    }

    /// Still open although the extension stopped waiting (timeout).
    pub fn expired(&self) -> bool {
        self.reply.is_some()
            && (self.expired.load(Ordering::SeqCst)
                || self.deadline.is_some_and(|d| Instant::now() >= d))
    }

    /// Cancel an expired dialog (the caller closes it).
    pub fn expire(&mut self) {
        self.answer(None);
    }

    /// What should hold the keyboard focus.
    pub fn focus_target(&self, cx: &App) -> FocusHandle {
        if let Some(input) = &self.input {
            return input.read(cx).focus_handle(cx);
        }
        if let Some(editor) = &self.editor {
            return editor.read(cx).focus_handle(cx);
        }
        self.focus.clone()
    }

    fn answer(&mut self, answer: Option<DialogAnswer>) {
        if let Some(reply) = self.reply.take() {
            let _ = reply.try_send(answer);
        }
    }

    fn submit(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.reply.is_none() {
            return;
        }
        let answer = match &self.kind {
            DialogKind::Select { options } => {
                options.get(self.selected).cloned().map(DialogAnswer::Text)
            }
            DialogKind::Confirm { .. } => Some(DialogAnswer::Confirmed(true)),
            DialogKind::Input { .. } => self
                .input
                .as_ref()
                .map(|i| DialogAnswer::Text(i.read(cx).value().to_string())),
            DialogKind::Editor { .. } => self
                .editor
                .as_ref()
                .map(|e| DialogAnswer::Text(e.read(cx).value().to_string())),
        };
        self.answer(answer);
        window.close_dialog(cx);
    }

    fn cancel(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.reply.is_none() {
            return;
        }
        let answer = matches!(self.kind, DialogKind::Confirm { .. })
            .then_some(DialogAnswer::Confirmed(false));
        self.answer(answer);
        window.close_dialog(cx);
    }

    fn on_key(&mut self, event: &KeyDownEvent, window: &mut Window, cx: &mut Context<Self>) {
        let key = event.keystroke.key.as_str();
        match (&self.kind, key) {
            (DialogKind::Select { options }, "up") if !options.is_empty() => {
                self.selected = (self.selected + options.len() - 1) % options.len();
                cx.notify();
            }
            (DialogKind::Select { options }, "down") if !options.is_empty() => {
                self.selected = (self.selected + 1) % options.len();
                cx.notify();
            }
            (DialogKind::Select { .. } | DialogKind::Confirm { .. }, "enter") => {
                self.submit(window, cx)
            }
            (DialogKind::Input { .. }, "enter") => self.submit(window, cx),
            (DialogKind::Editor { .. }, "enter") if event.keystroke.modifiers.control => {
                self.submit(window, cx)
            }
            _ => return,
        }
        cx.stop_propagation();
    }
}

impl Render for ExtDialog {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        let countdown = self
            .deadline
            .map(|d| format!("Closes in {}s", seconds_left(d, Instant::now())));
        let mut body = v_flex()
            .id("ext-dialog")
            .track_focus(&self.focus)
            .capture_key_down(cx.listener(Self::on_key))
            .gap_2()
            .child(
                div()
                    .text_sm()
                    .font_weight(FontWeight::SEMIBOLD)
                    .child(self.title.clone()),
            );
        match &self.kind {
            DialogKind::Select { options } => {
                let mut list = v_flex()
                    .id("ext-select-list")
                    .gap_0p5()
                    .max_h(px(360.))
                    .overflow_y_scroll();
                for (i, option) in options.iter().enumerate() {
                    let selected = i == self.selected;
                    list = list.child(
                        div()
                            .id(("ext-select", i))
                            .px_2()
                            .py_1()
                            .rounded_md()
                            .text_sm()
                            .cursor_pointer()
                            .when(selected, |el| el.bg(theme.list_active))
                            .when(!selected, |el| el.hover(|s| s.bg(theme.muted.opacity(0.5))))
                            .on_click(cx.listener(move |this, _, window, cx| {
                                this.selected = i;
                                this.submit(window, cx);
                            }))
                            .child(SharedString::from(option.clone())),
                    );
                }
                if options.is_empty() {
                    list = list.child(
                        div()
                            .text_xs()
                            .text_color(theme.muted_foreground)
                            .child("No options."),
                    );
                }
                body = body.child(list);
            }
            DialogKind::Confirm { message } => {
                body = body.child(div().text_sm().child(SharedString::from(message.clone())));
            }
            DialogKind::Input { .. } => {
                if let Some(input) = &self.input {
                    body = body.child(Input::new(input));
                }
            }
            DialogKind::Editor { .. } => {
                if let Some(editor) = &self.editor {
                    body = body.child(Textarea::new(editor)).child(
                        div()
                            .text_xs()
                            .text_color(theme.muted_foreground)
                            .child("Ctrl+Enter submits"),
                    );
                }
            }
        }
        let (ok, cancel) = match &self.kind {
            DialogKind::Confirm { .. } => ("Yes", "No"),
            DialogKind::Select { .. } => ("Choose", "Cancel"),
            _ => ("Submit", "Cancel"),
        };
        body.child(
            h_flex()
                .gap_2()
                .items_center()
                .child(
                    div()
                        .flex_1()
                        .text_xs()
                        .text_color(theme.muted_foreground)
                        .children(countdown),
                )
                .child(
                    Button::new("ext-dialog-cancel")
                        .label(cancel)
                        .small()
                        .on_click(cx.listener(|this, _, window, cx| this.cancel(window, cx))),
                )
                .child(
                    Button::new("ext-dialog-ok")
                        .label(ok)
                        .small()
                        .primary()
                        .on_click(cx.listener(|this, _, window, cx| this.submit(window, cx))),
                ),
        )
    }
}

/// Open `request` as a dialog; the returned view reports when it is done.
pub fn open_dialog(request: DialogRequest, window: &mut Window, cx: &mut App) -> Entity<ExtDialog> {
    let wide = matches!(request.kind, DialogKind::Editor { .. });
    let view = cx.new(|cx| ExtDialog::new(request, window, cx));
    let content = view.clone();
    window.open_dialog(cx, move |dialog, _, _| {
        let submit = content.downgrade();
        dialog
            .w(px(if wide { 720. } else { 520. }))
            // Enter (the dialog's confirm binding) answers; `submit` closes.
            .on_ok(move |_, window, cx| {
                let _ = submit.update(cx, |d, cx| d.submit(window, cx));
                false
            })
            .child(content.clone())
    });
    // The dialog takes focus when it opens; hand it to the content after.
    let target = view.read(cx).focus_target(cx);
    window.defer(cx, move |window, cx| window.focus(&target, cx));
    view
}

/// Terminal-like rendering of component lines.
fn lines_view(lines: &[String], cx: &App) -> Div {
    let theme = cx.theme();
    v_flex()
        .w_full()
        .p_2()
        .rounded_md()
        .bg(theme.muted.opacity(0.4))
        .font_family(theme.mono_font_family.clone())
        .text_xs()
        .children(lines.iter().map(|l| {
            div()
                .whitespace_nowrap()
                .min_h(px(14.))
                .child(SharedString::from(l.clone()))
        }))
}

/// A running `ctx.ui.custom` component: shows its lines and forwards keys
/// to the extension thread that owns it.
pub struct ComponentView {
    title: String,
    lines: Vec<String>,
    events: mpsc::Sender<ComponentEvent>,
    focus: FocusHandle,
    closed: bool,
}

impl Drop for ComponentView {
    fn drop(&mut self) {
        if !self.closed {
            let _ = self.events.send(ComponentEvent::Cancel);
        }
    }
}

impl ComponentView {
    pub fn set_lines(&mut self, lines: Vec<String>, cx: &mut Context<Self>) {
        self.lines = lines;
        cx.notify();
    }

    /// The extension finished; closing the dialog must not cancel it again.
    pub fn mark_closed(&mut self) {
        self.closed = true;
    }

    fn on_key(&mut self, event: &KeyDownEvent, _: &mut Window, cx: &mut Context<Self>) {
        if let Some(data) = input_data(&pressed(&event.keystroke)) {
            let _ = self.events.send(ComponentEvent::Input(data));
            cx.stop_propagation();
        }
    }
}

impl Render for ComponentView {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let muted = cx.theme().muted_foreground;
        v_flex()
            .id("ext-component")
            .track_focus(&self.focus)
            .capture_key_down(cx.listener(Self::on_key))
            .gap_2()
            .child(
                div()
                    .text_xs()
                    .text_color(muted)
                    .child(format!("{} · keys go to the extension", self.title)),
            )
            .child(lines_view(&self.lines, cx))
            .child(
                h_flex().justify_end().child(
                    Button::new("ext-component-close")
                        .label("Close")
                        .small()
                        .on_click(|_, window, cx| window.close_dialog(cx)),
                ),
            )
    }
}

/// Open a component dialog (Escape goes to the component, not the dialog).
pub fn open_component(
    id: u64,
    title: String,
    lines: Vec<String>,
    events: mpsc::Sender<ComponentEvent>,
    window: &mut Window,
    cx: &mut App,
) -> Entity<ComponentView> {
    let view = cx.new(|cx| ComponentView {
        title: format!("{title} #{id}"),
        lines,
        events,
        focus: cx.focus_handle(),
        closed: false,
    });
    let focus = view.read(cx).focus.clone();
    let content = view.clone();
    window.open_dialog(cx, move |dialog, _, _| {
        dialog
            .w(px(760.))
            .keyboard(false)
            .on_ok(|_, _, _| false)
            .child(content.clone())
    });
    window.defer(cx, move |window, cx| window.focus(&focus, cx));
    view
}

/// `setEditorComponent`: a persistent component shown above the composer.
/// It renders and handles input on the UI thread (no handler is running
/// then); a component that reports `done` is removed.
pub struct EditorComponentView {
    component: ScriptComponent,
    lines: Vec<String>,
    error: Option<String>,
    focus: FocusHandle,
    done: bool,
}

impl EditorComponentView {
    pub fn new(component: ScriptComponent, cx: &mut Context<Self>) -> Self {
        let mut view = Self {
            component,
            lines: Vec::new(),
            error: None,
            focus: cx.focus_handle(),
            done: false,
        };
        view.rerender();
        view
    }

    pub fn is_done(&self) -> bool {
        self.done
    }

    pub fn focus_handle(&self) -> FocusHandle {
        self.focus.clone()
    }

    fn rerender(&mut self) {
        match self.component.render(COMPONENT_COLUMNS, COMPONENT_ROWS / 3) {
            Ok(lines) => {
                self.lines = lines;
                self.error = None;
            }
            Err(e) => self.error = Some(e),
        }
    }

    fn on_key(&mut self, event: &KeyDownEvent, _: &mut Window, cx: &mut Context<Self>) {
        let Some(data) = input_data(&pressed(&event.keystroke)) else {
            return;
        };
        match self.component.input(&data) {
            Ok(response) => {
                if response.done {
                    self.done = true;
                } else if let Some(lines) = response.lines {
                    self.lines = lines;
                } else {
                    self.rerender();
                }
                if response.handled || response.done {
                    cx.stop_propagation();
                }
            }
            Err(e) => self.error = Some(e),
        }
        cx.notify();
    }
}

impl Render for EditorComponentView {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        let focused = self.focus.is_focused(_window);
        v_flex()
            .id("ext-editor-component")
            .track_focus(&self.focus)
            .capture_key_down(cx.listener(Self::on_key))
            .on_click(cx.listener(|this, _, window, cx| window.focus(&this.focus, cx)))
            .gap_1()
            .p_1()
            .rounded_md()
            .border_1()
            .border_color(if focused { theme.ring } else { theme.border })
            .child(lines_view(&self.lines, cx))
            .when_some(self.error.clone(), |el, e| {
                el.child(div().text_xs().text_color(theme.danger).child(e))
            })
    }
}

/// Close-polling interval for dialogs with timeouts.
pub const DIALOG_POLL: Duration = Duration::from_millis(250);

#[cfg(test)]
mod tests {
    use super::seconds_left;
    use std::time::{Duration, Instant};

    #[test]
    fn countdown_rounds_up() {
        let now = Instant::now();
        assert_eq!(seconds_left(now + Duration::from_millis(1500), now), 2);
        assert_eq!(seconds_left(now + Duration::from_secs(3), now), 3);
        assert_eq!(seconds_left(now, now + Duration::from_secs(1)), 0);
    }
}
