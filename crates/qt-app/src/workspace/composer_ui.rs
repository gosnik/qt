//! Composer behaviour on top of the plain textarea: the autocomplete popup
//! (`/` commands and arguments, `@` paths), prompt history on Up/Down,
//! `!cmd` user bash, `@` mention expansion on send.

use gpui_kit::component::input::{Enter, Escape, IndentInline, MoveDown, MoveUp};
use gpui_kit::component::notification::Notification;
use gpui_kit::component::{ActiveTheme as _, WindowExt as _, h_flex, v_flex};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::*;
use qt_core::ContentBlock;
use qt_core::input::{expand_mentions, extract_mentions};
use qt_core::message::BashExecutionMessage;
use qt_core::slash::argument_completions;

use super::completion::{
    self, CompletionKind, CompletionRequest, MAX_PATH_SUGGESTIONS, Suggestion, apply,
    argument_suggestions, best_match, command_suggestions, detect, submits_on_enter,
};
use super::history::{HistoryStep, PromptHistory, cursor_lines};
use super::{RunKind, Workspace};

/// Rows shown before the popup scrolls.
const POPUP_MAX_HEIGHT: f32 = 300.;
/// Recently shown `!command` results, to skip the copy a run re-emits.
const SHOWN_BASH_MEMORY: usize = 8;

/// The open autocomplete popup.
pub(super) struct CompletionPopup {
    pub request: CompletionRequest,
    pub items: Vec<Suggestion>,
    pub selected: usize,
    pub scroll: ScrollHandle,
}

/// A running user `!command`.
pub(super) struct BashRun {
    pub _task: Task<()>,
}

fn same_kind(a: &CompletionKind, b: &CompletionKind) -> bool {
    std::mem::discriminant(a) == std::mem::discriminant(b)
}

impl Workspace {
    pub(super) fn composer_cursor(&self, cx: &App) -> usize {
        self.composer.read(cx).cursor()
    }

    /// Replace the composer text and put the cursor at the end.
    pub(super) fn set_composer_end(
        &mut self,
        text: &str,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let text = text.to_string();
        let end = text.len();
        self.composer.update(cx, |state, cx| {
            state.set_value(text, window, cx);
            state.set_selected_range(end..end, cx);
            state.focus(window, cx);
        });
    }

    pub(super) fn clear_composer(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.completion = None;
        self.composer
            .update(cx, |state, cx| state.set_value("", window, cx));
        self.history.reset();
    }

    /// Every edit: end history browsing when the user changed the text and
    /// refresh the autocomplete popup.
    pub(super) fn on_composer_changed(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let text = self.composer_text(cx);
        self.history.on_text_changed(&text);
        self.refresh_completion(window, cx);
    }

    // ----- autocomplete -----------------------------------------------------------

    pub(super) fn refresh_completion(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let text = self.composer_text(cx);
        let cursor = self.composer_cursor(cx);
        self.completion_generation += 1;
        let Some(request) = detect(&text, cursor) else {
            // Extension autocomplete providers complete anything else.
            match self.extension_completion(&text, cursor, None, cx) {
                Some((request, items)) => self.show_completion(request, items, cx),
                None => self.close_completion(cx),
            }
            return;
        };
        match &request.kind {
            CompletionKind::SlashName => {
                let mut items =
                    command_suggestions(&self.engine.slash_commands().commands, &request.query);
                self.extension_completion(&text, cursor, Some((&request, &mut items)), cx);
                self.show_completion(request, items, cx);
            }
            CompletionKind::SlashArgs { command } => {
                let known = self.engine.slash_commands().find(command).is_some();
                let mut items = if known {
                    argument_suggestions(argument_completions(
                        &self.engine,
                        command,
                        &request.query,
                    ))
                } else {
                    Vec::new()
                };
                self.extension_completion(&text, cursor, Some((&request, &mut items)), cx);
                self.show_completion(request, items, cx);
            }
            CompletionKind::Extension => self.close_completion(cx),
            CompletionKind::Path { quoted } => {
                let generation = self.completion_generation;
                let cwd = self.engine.cwd().to_path_buf();
                let query = request.query.clone();
                let quoted = *quoted;
                let search = cx.background_executor().spawn(async move {
                    completion::path_suggestions(&cwd, &query, quoted, MAX_PATH_SUGGESTIONS)
                });
                self._completion_task = Some(cx.spawn_in(window, async move |this, cx| {
                    let items = search.await;
                    let _ = this.update(cx, |this, cx| {
                        if this.completion_generation == generation {
                            this.show_completion(request, items, cx);
                        }
                    });
                }));
            }
        }
    }

    fn show_completion(
        &mut self,
        request: CompletionRequest,
        items: Vec<Suggestion>,
        cx: &mut Context<Self>,
    ) {
        if items.is_empty() {
            self.close_completion(cx);
            return;
        }
        let selected = match &request.kind {
            CompletionKind::Path { .. } => 0,
            _ => best_match(&items, &request.query),
        };
        let scroll = ScrollHandle::new();
        scroll.scroll_to_item(selected);
        self.completion = Some(CompletionPopup {
            request,
            items,
            selected,
            scroll,
        });
        cx.notify();
    }

    fn close_completion(&mut self, cx: &mut Context<Self>) {
        if self.completion.take().is_some() {
            cx.notify();
        }
    }

    fn move_completion(&mut self, delta: isize, cx: &mut Context<Self>) {
        if let Some(popup) = self.completion.as_mut() {
            let len = popup.items.len() as isize;
            popup.selected = (popup.selected as isize + delta).rem_euclid(len) as usize;
            popup.scroll.scroll_to_item(popup.selected);
            cx.notify();
        }
    }

    /// Insert the highlighted suggestion; returns what kind it completed.
    pub(super) fn accept_completion(
        &mut self,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Option<CompletionKind> {
        let popup = self.completion.take()?;
        let item = popup.items.get(popup.selected)?.clone();
        let text = self.composer_text(cx);
        let cursor = self.composer_cursor(cx);
        // The cursor may have moved since the popup opened.
        let request = detect(&text, cursor)
            .filter(|r| same_kind(&r.kind, &popup.request.kind))
            .unwrap_or(popup.request);
        let (next, next_cursor) = apply(&text, cursor, &request, &item);
        self.composer.update(cx, |state, cx| {
            state.set_value(next.clone(), window, cx);
            state.set_selected_range(next_cursor..next_cursor, cx);
            state.focus(window, cx);
        });
        self.history.on_text_changed(&next);
        // Directories and command names continue completing.
        if item.directory || request.kind == CompletionKind::SlashName {
            self.refresh_completion(window, cx);
        }
        cx.notify();
        Some(request.kind)
    }

    fn pick_completion(&mut self, index: usize, window: &mut Window, cx: &mut Context<Self>) {
        if let Some(popup) = self.completion.as_mut() {
            popup.selected = index.min(popup.items.len().saturating_sub(1));
        }
        self.accept_completion(window, cx);
    }

    // ----- key handling (capture phase, before the textarea) ----------------------

    pub(super) fn capture_up(&mut self, _: &MoveUp, window: &mut Window, cx: &mut Context<Self>) {
        if self.completion.is_some() {
            self.move_completion(-1, cx);
            cx.stop_propagation();
            return;
        }
        let text = self.composer_text(cx);
        let (first, _) = cursor_lines(&text, self.composer_cursor(cx));
        if let HistoryStep::Show(entry) = self.history.up(&text, first) {
            self.set_composer_end(&entry, window, cx);
            cx.stop_propagation();
        }
    }

    pub(super) fn capture_down(
        &mut self,
        _: &MoveDown,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.completion.is_some() {
            self.move_completion(1, cx);
            cx.stop_propagation();
            return;
        }
        let text = self.composer_text(cx);
        let (_, last) = cursor_lines(&text, self.composer_cursor(cx));
        if let HistoryStep::Show(entry) = self.history.down(last) {
            self.set_composer_end(&entry, window, cx);
            cx.stop_propagation();
        }
    }

    pub(super) fn capture_tab(
        &mut self,
        _: &IndentInline,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.completion.is_some() {
            self.accept_completion(window, cx);
            cx.stop_propagation();
        }
    }

    pub(super) fn capture_enter(
        &mut self,
        action: &Enter,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.completion.is_none() || action.shift || action.secondary {
            return;
        }
        cx.stop_propagation();
        if let Some(kind) = self.accept_completion(window, cx)
            && submits_on_enter(&kind)
        {
            self.submit(false, window, cx);
        }
    }

    pub(super) fn capture_escape(&mut self, _: &Escape, _: &mut Window, cx: &mut Context<Self>) {
        if self.completion.is_some() {
            self.close_completion(cx);
            cx.stop_propagation();
        }
    }

    // ----- prompt history ----------------------------------------------------------

    /// Load the history for the current conversation.
    pub(super) fn reload_history(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let (history, warning) = PromptHistory::load(
            &self.engine.prompt_history(),
            &self.conversation.prompt_history(),
        );
        self.history = history;
        if let Some(warning) = warning {
            window.push_notification(Notification::warning(warning), cx);
        }
    }

    /// Record a submitted line in the session history and `history.jsonl`.
    pub(super) fn history_record(
        &mut self,
        text: &str,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let text = text.trim();
        if text.is_empty() {
            return;
        }
        self.history.push(text);
        let store = self.engine.prompt_history();
        if store.max_entries() > 0
            && let Err(err) = store.append(text)
        {
            window.push_notification(
                Notification::warning(format!("Could not save prompt history: {err:#}")),
                cx,
            );
        }
    }

    // ----- sending -------------------------------------------------------------------

    /// Attach the files named by `@` mentions (existing files only).
    fn with_mentions(
        &self,
        text: &str,
        mut images: Vec<ContentBlock>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> (String, Vec<ContentBlock>) {
        if extract_mentions(text).is_empty() {
            return (text.to_string(), images);
        }
        let auto_resize = self.engine.settings().image_auto_resize();
        let files = expand_mentions(text, self.engine.cwd(), auto_resize);
        for diagnostic in &files.diagnostics {
            window.push_notification(Notification::warning(diagnostic.message.clone()), cx);
        }
        images.extend(files.images);
        (files.text, images)
    }

    /// Send prompt text: start a run when idle, steer (or queue a follow-up)
    /// while running. Returns false when nothing was sent.
    pub(super) fn send_text(
        &mut self,
        text: &str,
        images: Vec<ContentBlock>,
        follow_up: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> bool {
        if self.running == Some(RunKind::Compaction) {
            window.push_notification(
                Notification::info("Compaction in progress — your message stays in the composer."),
                cx,
            );
            return false;
        }
        let Some((text, images)) = self.input_hooks(text, images, window, cx) else {
            // An extension handled the input (or rejected it with a notice).
            return true;
        };
        let (text, images) = self.with_mentions(&text, images, window, cx);
        if self.running == Some(RunKind::Agent) {
            let result = if follow_up {
                self.conversation.follow_up(&text, images)
            } else {
                self.conversation.steer(&text, images)
            };
            if let Err(err) = result {
                window.push_notification(Notification::error(format!("{err:#}")), cx);
                return false;
            }
            return true;
        }
        self.start(&text, images, window, cx)
    }

    // ----- user bash -----------------------------------------------------------------

    /// Run `!command` (`!!command` excluded from the context) with a live
    /// output card. Only one runs at a time; Esc cancels it.
    pub(super) fn run_user_bash(
        &mut self,
        command: &str,
        exclude: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> bool {
        let run = match self.conversation.run_bash(command, exclude) {
            Ok(run) => run,
            Err(err) => {
                window.push_notification(Notification::error(format!("{err:#}")), cx);
                return false;
            }
        };
        let id = self
            .transcript
            .update(cx, |t, cx| t.begin_bash(command, exclude, cx));
        let output = run.output;
        let result = run.result;
        let task = cx.spawn_in(window, async move |this, cx| {
            while let Ok(tail) = output.recv().await {
                if this
                    .update(cx, |this, cx| {
                        this.transcript
                            .update(cx, |t, cx| t.update_bash(id, tail, cx))
                    })
                    .is_err()
                {
                    return;
                }
            }
            let message = result.await;
            let _ = this.update_in(cx, |this, window, cx| {
                this.bash = None;
                match message {
                    Ok(message) => this.bash_finished(id, message, cx),
                    Err(err) => window.push_notification(
                        Notification::error(format!("Bash command failed: {err}")),
                        cx,
                    ),
                }
                cx.notify();
            });
        });
        self.bash = Some(BashRun { _task: task });
        cx.notify();
        true
    }

    fn bash_finished(&mut self, id: u64, message: BashExecutionMessage, cx: &mut Context<Self>) {
        self.shown_bash.push(message.clone());
        if self.shown_bash.len() > SHOWN_BASH_MEMORY {
            self.shown_bash.remove(0);
        }
        self.transcript
            .update(cx, |t, cx| t.finish_bash(id, message, cx));
    }

    /// A `!command` result recorded at the end of a run that this view
    /// already shows (the core holds results while the agent runs).
    pub(super) fn take_shown_bash(&mut self, message: &BashExecutionMessage) -> bool {
        match self.shown_bash.iter().position(|m| m == message) {
            Some(i) => {
                self.shown_bash.remove(i);
                true
            }
            None => false,
        }
    }

    // ----- rendering --------------------------------------------------------------------

    pub(super) fn render_completion(&self, cx: &mut Context<Self>) -> Option<AnyElement> {
        let popup = self.completion.as_ref()?;
        let theme = cx.theme().clone();
        let heading = match &popup.request.kind {
            CompletionKind::SlashName => "Commands".to_string(),
            CompletionKind::SlashArgs { command } => format!("/{command} arguments"),
            CompletionKind::Path { .. } => "Files".to_string(),
            CompletionKind::Extension => "Suggestions".to_string(),
        };
        let mono = matches!(popup.request.kind, CompletionKind::Path { .. });
        let mut list = v_flex()
            .id("completion-list")
            .track_scroll(&popup.scroll)
            .max_h(px(POPUP_MAX_HEIGHT))
            .overflow_y_scroll();
        for (i, item) in popup.items.iter().enumerate() {
            let selected = i == popup.selected;
            list = list.child(
                h_flex()
                    .id(("completion", i))
                    .gap_3()
                    .px_2()
                    .py_1()
                    .rounded_md()
                    .cursor_pointer()
                    .when(selected, |el| el.bg(theme.list_active))
                    .when(!selected, |el| el.hover(|s| s.bg(theme.muted.opacity(0.6))))
                    .on_click(
                        cx.listener(move |this, _, window, cx| this.pick_completion(i, window, cx)),
                    )
                    .child(
                        div()
                            .flex_none()
                            .max_w(px(320.))
                            .overflow_hidden()
                            .whitespace_nowrap()
                            .text_ellipsis()
                            .text_sm()
                            .when(mono, |el| el.font_family(theme.mono_font_family.clone()))
                            .child(item.label.clone()),
                    )
                    .when_some(item.detail.clone(), |el, detail| {
                        el.child(
                            div()
                                .flex_1()
                                .min_w_0()
                                .overflow_hidden()
                                .whitespace_nowrap()
                                .text_ellipsis()
                                .text_xs()
                                .text_color(theme.muted_foreground)
                                .child(detail),
                        )
                    }),
            );
        }
        Some(
            v_flex()
                .w_full()
                .p_1()
                .gap_0p5()
                .rounded_lg()
                .border_1()
                .border_color(theme.border)
                .bg(theme.popover)
                .shadow_lg()
                .child(
                    h_flex()
                        .px_2()
                        .text_xs()
                        .text_color(theme.muted_foreground)
                        .child(div().flex_1().child(heading))
                        .child("↑/↓ select · Tab/Enter insert · Esc close"),
                )
                .child(list)
                .into_any_element(),
        )
    }
}
