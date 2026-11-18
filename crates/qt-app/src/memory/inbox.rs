//! Memory Review Inbox (archived `ReviewInboxController`): landing summary,
//! queue with filters (status, kind, priority, scope, tags, ordering), item
//! detail with the acceptance preview, Source Inspector, every typed action
//! with its inputs, preview → confirm → apply, history with receipts,
//! batches run as background jobs, and the persisted view state / queue
//! position.

use std::collections::BTreeSet;
use std::path::PathBuf;
use std::sync::Arc;
use std::time::Duration;

use gpui_kit::component::button::{Button, ButtonVariants as _};
use gpui_kit::component::checkbox::Checkbox;
use gpui_kit::component::input::{Input, InputEvent, InputState, Textarea, TextareaState};
use gpui_kit::component::spinner::Spinner;
use gpui_kit::component::{
    ActiveTheme as _, Disableable as _, Sizable as _, WindowExt as _, h_flex, v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::*;
use qt_memory::MemoryRuntime;
use qt_memory::model::{MemoryScope, MemoryStatus, MemoryType, ReviewStatus, now_ms};
use qt_memory::review::*;
use qt_memory::slash::status_lines;
use qt_memory::views::review::{self as view, ActionInput, EditField, StatusFilter};

use super::MemoryHost;
use super::widgets::{self, chip, hint, section};

/// What the right-hand pane shows.
enum Pane {
    Empty(String),
    Detail,
    Evidence {
        path: Option<PathBuf>,
        file: Option<Result<String, String>>,
    },
    Action(Draft),
    Confirm {
        action: ReviewAction,
        lines: Vec<String>,
    },
    History {
        filter: &'static str,
        records: Vec<ReviewActionRecord>,
        selected: Option<usize>,
    },
    Batch(BatchState),
}

/// An action whose inputs are being collected.
struct Draft {
    action: ReviewAction,
    inputs: Vec<ActionInput>,
    step: usize,
    /// Free-text edit field being entered.
    field: Option<EditField>,
    error: Option<String>,
}

#[derive(Default)]
struct BatchState {
    query: ReviewQuery,
    items: Vec<ReviewItem>,
    selected: BTreeSet<String>,
    summary: Vec<String>,
    preview: Option<(ReviewActionKind, ReviewBatchPreview, Vec<ReviewBatchItem>)>,
    preview_lines: Vec<String>,
    job: Option<ReviewBatchJob>,
}

pub struct ReviewInboxView {
    host: MemoryHost,
    memory: Arc<MemoryRuntime>,
    focus: FocusHandle,
    landing: Vec<String>,
    restored: Option<ReviewViewState>,
    queue_title: String,
    query: ReviewQuery,
    filter: ReviewQuery,
    items: Vec<ReviewItem>,
    cards: Vec<(String, String)>,
    position: usize,
    detail: Option<HydratedReview>,
    detail_lines: Vec<String>,
    accept_lines: Vec<String>,
    footer_lines: Vec<String>,
    pane: Pane,
    notice: Vec<String>,
    notice_error: bool,
    content: Entity<TextareaState>,
    text: Entity<InputState>,
    note: Entity<InputState>,
    tags: Entity<InputState>,
    show_filters: bool,
    _task: Option<Task<()>>,
    _subscriptions: Vec<Subscription>,
}

const ORDERINGS: [(bool, &str); 2] = [(false, "newest first"), (true, "oldest first")];
const SCOPES: [(ReviewScopeSelector, &str); 3] = [
    (ReviewScopeSelector::All, "all scopes"),
    (ReviewScopeSelector::Exact, "exact"),
    (ReviewScopeSelector::Durable, "durable"),
];

impl ReviewInboxView {
    fn new(
        host: MemoryHost,
        memory: Arc<MemoryRuntime>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let restored = memory.reviews().load_review_view_state().ok().flatten();
        let mut this = Self {
            focus: cx.focus_handle(),
            landing: Vec::new(),
            restored,
            queue_title: "Pending reviews".into(),
            query: ReviewQuery::default(),
            filter: ReviewQuery::default(),
            items: Vec::new(),
            cards: Vec::new(),
            position: 0,
            detail: None,
            detail_lines: Vec::new(),
            accept_lines: Vec::new(),
            footer_lines: Vec::new(),
            pane: Pane::Empty(String::new()),
            notice: Vec::new(),
            notice_error: false,
            content: cx.new(|cx| TextareaState::new(window, cx).auto_grow(3, 10)),
            text: cx.new(|cx| InputState::new(window, cx)),
            note: cx.new(|cx| InputState::new(window, cx).placeholder("Optional audit note")),
            show_filters: false,
            tags: cx.new(|cx| {
                InputState::new(window, cx).placeholder("Tags, comma-separated (blank clears)")
            }),
            host,
            memory,
            _task: None,
            _subscriptions: Vec::new(),
        };
        // Enter in the evidence or note input continues the action.
        for input in [this.text.clone(), this.note.clone()] {
            let subscription =
                cx.subscribe_in(&input, window, |this, _, event: &InputEvent, window, cx| {
                    if let InputEvent::PressEnter { .. } = event
                        && matches!(this.pane, Pane::Action(ref draft) if draft.field.is_none())
                    {
                        this.advance(window, cx);
                    }
                });
            this._subscriptions.push(subscription);
        }
        this.refresh_landing();
        if let Some(state) = this.restored.clone() {
            this.filter = state.query.clone();
            let tags = state.query.tags.join(", ");
            this.tags.update(cx, |s, cx| s.set_value(tags, window, cx));
            this.position = state.queue_offset;
            this.load_queue("Resumed review queue", state.query, true, cx);
            this.select_id(&state.selected_review_id);
        } else {
            this.load_queue("Pending reviews", ReviewQuery::default(), false, cx);
        }
        this
    }

    fn inbox(&self) -> &ReviewInbox {
        self.memory.reviews()
    }

    fn scope(&self) -> MemoryScope {
        self.inbox().scope()
    }

    fn refresh_landing(&mut self) {
        self.landing = view::landing_lines(self.inbox(), now_ms()).unwrap_or_else(|error| {
            vec![format!("Unable to open the Memory Review Inbox: {error}")]
        });
    }

    fn say(&mut self, lines: Vec<String>, error: bool, cx: &mut App) {
        self.notice = lines.clone();
        self.notice_error = error;
        self.host.card("/memory review", lines, error, cx);
    }

    fn persist_view(&mut self) {
        let Some(item) = self.items.get(self.position) else {
            return;
        };
        let state = ReviewViewState {
            query: self.query.clone(),
            selected_review_id: item.id.clone(),
            queue_offset: self.position,
            updated_at: 0,
        };
        let _ = self.inbox().save_review_view_state(state.clone());
        self.restored = Some(state);
    }

    /// Loads a queue (archived `show_queue`).
    fn load_queue(
        &mut self,
        title: &str,
        query: ReviewQuery,
        preserve_position: bool,
        cx: &mut App,
    ) {
        let items = match self.inbox().review_items(&query) {
            Ok(items) => items,
            Err(error) => {
                self.say(
                    vec![format!("Unable to load memory reviews: {error}")],
                    true,
                    cx,
                );
                return;
            }
        };
        if !preserve_position || self.queue_title != title {
            self.position = 0;
        }
        self.queue_title = title.to_string();
        self.query = query;
        self.items = items;
        let now = now_ms();
        let total = self.items.len();
        self.cards = self
            .items
            .iter()
            .enumerate()
            .map(|(index, item)| {
                let detail = self
                    .memory
                    .reviews()
                    .review_detail(&item.id)
                    .map_err(|e| e.to_string());
                view::queue_card(
                    index,
                    total,
                    item,
                    detail.as_ref().map_err(Clone::clone),
                    now,
                )
            })
            .collect();
        if self.items.is_empty() {
            self.detail = None;
            self.pane = Pane::Empty(title.to_string());
            return;
        }
        self.position = self.position.min(self.items.len() - 1);
        self.persist_view();
        self.load_detail();
    }

    fn select_id(&mut self, id: &str) {
        if let Some(index) = self.items.iter().position(|item| item.id == id) {
            self.position = index;
            self.persist_view();
            self.load_detail();
        }
    }

    fn select(&mut self, index: usize, window: &mut Window, cx: &mut Context<Self>) {
        self.position = index;
        self.persist_view();
        self.load_detail();
        window.focus(&self.focus, cx);
        cx.notify();
    }

    /// Hydrates the selected review (archived `show_detail`).
    fn load_detail(&mut self) {
        let Some(item) = self.items.get(self.position) else {
            return;
        };
        let scope = self.scope();
        match self.inbox().review_detail(&item.id) {
            Ok(detail) => {
                self.detail_lines =
                    view::detail_lines(&detail, &self.queue_title, &scope, now_ms());
                let preview = self.inbox().preview_review(
                    &detail.item.id,
                    view::acceptance_preview_action(detail.candidate.kind),
                );
                self.accept_lines = view::acceptance_preview_lines(
                    preview.as_ref().map_err(|e| e.to_string()),
                    detail.subject.as_ref().map(|s| &s.scope).unwrap_or(&scope),
                );
                self.footer_lines = view::detail_footer_lines(&detail);
                self.detail = Some(detail);
                self.pane = Pane::Detail;
            }
            Err(error) => {
                self.detail = None;
                self.pane = Pane::Empty(format!("Unable to open review detail: {error}"));
            }
        }
    }

    /// Archived `show_next`: the following item, wrapping around.
    fn next(&mut self, cx: &mut Context<Self>) {
        let current = self.items.get(self.position).map(|item| item.id.clone());
        let title = self.queue_title.clone();
        self.load_queue(&title, self.query.clone(), true, cx);
        if self.items.is_empty() {
            cx.notify();
            return;
        }
        self.position =
            match current.and_then(|id| self.items.iter().position(|item| item.id == id)) {
                Some(index) => (index + 1) % self.items.len(),
                None => self.position.min(self.items.len() - 1),
            };
        self.persist_view();
        self.load_detail();
        cx.notify();
    }

    fn return_to_queue(&mut self, cx: &mut Context<Self>) {
        let title = self.queue_title.clone();
        self.load_queue(&title, self.query.clone(), true, cx);
        self.refresh_landing();
        cx.notify();
    }

    // ----- landing navigation ---------------------------------------------

    fn landing(&mut self, value: &str, window: &mut Window, cx: &mut Context<Self>) {
        match value {
            "next" => self.load_queue(
                "Review next",
                ReviewQuery {
                    limit: 1,
                    ..Default::default()
                },
                false,
                cx,
            ),
            "pending" => self.load_queue("Pending reviews", ReviewQuery::default(), false, cx),
            "deferred" => self.load_queue(
                "Deferred reviews",
                ReviewQuery {
                    statuses: vec![ReviewStatus::Deferred],
                    include_due_deferred: true,
                    ..Default::default()
                },
                false,
                cx,
            ),
            "dream" => match view::latest_dream(self.inbox()) {
                Ok(Some((query, position, id))) => {
                    self.load_queue("Pending Dream reviews", query, false, cx);
                    self.position = position;
                    self.select_id(&id);
                }
                Ok(None) => {
                    self.items.clear();
                    self.cards.clear();
                    self.pane = Pane::Empty("Latest Dream review".into());
                }
                Err(error) => self.say(
                    vec![format!("Unable to load the latest Dream review: {error}")],
                    true,
                    cx,
                ),
            },
            "history" => self.open_history("all"),
            "batch" => self.open_batch(cx),
            "resume" => {
                if let Some(state) = self.restored.clone() {
                    self.position = state.queue_offset;
                    self.queue_title = "Resumed review queue".into();
                    self.load_queue("Resumed review queue", state.query, true, cx);
                    self.select_id(&state.selected_review_id);
                }
            }
            "clear_filters" => {
                let _ = self.inbox().clear_review_view_state();
                self.restored = None;
                self.filter = ReviewQuery::default();
                self.tags.update(cx, |s, cx| s.set_value("", window, cx));
                self.load_queue("Pending reviews", ReviewQuery::default(), false, cx);
            }
            "status" => {
                let lines = status_lines(&self.host.slash_context());
                self.host.card("/memory status", lines, false, cx);
                self.notice = vec!["Memory status was added to the transcript.".into()];
                self.notice_error = false;
            }
            _ => {}
        }
        self.refresh_landing();
        cx.notify();
    }

    fn apply_filters(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let tags = self.tags.read(cx).value().to_string();
        self.filter.tags = view::parse_tags(&tags);
        let query = self.filter.clone();
        self.load_queue("Filtered reviews", query, false, cx);
        let _ = window;
        cx.notify();
    }

    // ----- actions ------------------------------------------------------------

    fn begin(&mut self, kind: ReviewActionKind, window: &mut Window, cx: &mut Context<Self>) {
        // Reload so the action targets the current revision.
        self.load_detail();
        let Some(detail) = self.detail.clone() else {
            return;
        };
        let action = match view::base_action(&detail, kind, &self.scope()) {
            Ok(action) => action,
            Err(message) => {
                self.say(vec![message], true, cx);
                cx.notify();
                return;
            }
        };
        let inputs = view::action_inputs(&detail, kind);
        let initial = view::content_initial(&detail, kind);
        self.content
            .update(cx, |s, cx| s.set_value(initial, window, cx));
        self.text.update(cx, |s, cx| s.set_value("", window, cx));
        self.note.update(cx, |s, cx| s.set_value("", window, cx));
        self.pane = Pane::Action(Draft {
            action,
            inputs,
            step: 0,
            field: None,
            error: None,
        });
        cx.notify();
    }

    /// Advances the draft past its current input; after the note it
    /// previews.
    fn advance(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let Pane::Action(draft) = &mut self.pane else {
            return;
        };
        let Some(input) = draft.inputs.get(draft.step).cloned() else {
            return;
        };
        match input {
            ActionInput::Content { .. } => {
                let content = self.content.read(cx).value().to_string();
                view::apply_content(&mut draft.action, &content);
            }
            ActionInput::Evidence { .. } => {
                let uri = self.text.read(cx).value().trim().to_string();
                if uri.is_empty() {
                    draft.error = Some("Independent evidence is required.".into());
                    cx.notify();
                    return;
                }
                draft.action.evidence_uri = uri;
                self.text.update(cx, |s, cx| s.set_value("", window, cx));
            }
            ActionInput::Note => {
                draft.action.note = self.note.read(cx).value().trim().to_string();
            }
            ActionInput::EditFields | ActionInput::DeferChoice | ActionInput::TargetChoice => {}
        }
        draft.step += 1;
        draft.error = None;
        draft.field = None;
        if draft.step >= draft.inputs.len() {
            let action = draft.action.clone();
            self.preview(action, cx);
        }
        cx.notify();
    }

    fn set_draft(
        &mut self,
        change: impl FnOnce(&mut ReviewAction) -> Result<(), String>,
        cx: &mut Context<Self>,
    ) {
        if let Pane::Action(draft) = &mut self.pane {
            draft.error = change(&mut draft.action).err();
            cx.notify();
        }
    }

    /// Archived `show_confirmation`.
    fn preview(&mut self, mut action: ReviewAction, cx: &mut Context<Self>) {
        let Some(detail) = self.detail.clone() else {
            return;
        };
        let id = detail.item.id.clone();
        match self.inbox().preview_review(&id, action.clone()) {
            Ok(preview) if !preview.applicable => {
                self.say(vec![view::not_applicable_line(&preview)], true, cx);
                self.pane = Pane::Detail;
            }
            Ok(preview) => {
                view::confirm_action(&mut action, &preview);
                let lines = view::confirmation_lines(
                    &id,
                    &action,
                    &preview,
                    detail.subject.as_ref().map(|s| &s.scope),
                    &self.scope(),
                );
                self.pane = Pane::Confirm { action, lines };
            }
            Err(error) => {
                self.say(
                    vec![format!("Unable to preview review action: {error}")],
                    true,
                    cx,
                );
                self.pane = Pane::Detail;
            }
        }
    }

    /// Archived `apply`: the receipt goes to the transcript.
    fn apply(&mut self, action: ReviewAction, cx: &mut Context<Self>) {
        let Some(id) = self.detail.as_ref().map(|d| d.item.id.clone()) else {
            return;
        };
        match self.inbox().apply_review_action(&id, action) {
            Ok(result) => {
                let lines = view::receipt_lines(&id, &result);
                let error = !result.error.is_empty() || !result.failed_ids.is_empty();
                self.say(lines, error, cx);
                self.return_to_queue(cx);
            }
            Err(error) => {
                self.say(
                    vec![format!("Memory review was not changed: {error}")],
                    true,
                    cx,
                );
                self.load_detail();
            }
        }
        cx.notify();
    }

    fn shortcut(&mut self, key: &str, window: &mut Window, cx: &mut Context<Self>) {
        if !matches!(self.pane, Pane::Detail) || !self.focus.is_focused(window) {
            return;
        }
        let kind = match key {
            "a" => ReviewActionKind::Approve,
            "e" => ReviewActionKind::EditAndApprove,
            "r" => ReviewActionKind::Reject,
            "d" => ReviewActionKind::Defer,
            "s" => {
                self.next(cx);
                return;
            }
            "up" if self.position > 0 => {
                self.select(self.position - 1, window, cx);
                return;
            }
            "down" if self.position + 1 < self.items.len() => {
                self.select(self.position + 1, window, cx);
                return;
            }
            _ => return,
        };
        self.begin(kind, window, cx);
    }

    // ----- evidence ---------------------------------------------------------

    fn open_evidence(&mut self, cx: &mut Context<Self>) {
        let Some(detail) = &self.detail else {
            return;
        };
        let context = self.host.slash_context();
        let access = view::EvidenceAccess {
            cwd: context.cwd,
            workspace_root: context.workspace_root,
            allow_external_filesystem_access: context.allow_external_filesystem_access,
        };
        let path = view::source_evidence_path(&detail.item.provenance.source_uri, &access);
        self.pane = Pane::Evidence { path, file: None };
        cx.notify();
    }

    fn read_evidence(&mut self, cx: &mut Context<Self>) {
        if let Pane::Evidence {
            path: Some(path),
            file,
        } = &mut self.pane
        {
            *file = Some(view::read_source_evidence(path).map_err(|e| e.to_string()));
            cx.notify();
        }
    }

    // ----- history ------------------------------------------------------------

    fn open_history(&mut self, filter: &'static str) {
        let records = self.inbox().review_history(None, 200).unwrap_or_default();
        self.pane = Pane::History {
            filter,
            records,
            selected: None,
        };
    }

    // ----- batches ------------------------------------------------------------

    fn open_batch(&mut self, cx: &mut App) {
        let mut query = self
            .restored
            .as_ref()
            .map(|state| state.query.clone())
            .unwrap_or_default();
        query.statuses = vec![ReviewStatus::Pending];
        let items = match self.inbox().review_items(&query) {
            Ok(items) => items,
            Err(error) => {
                self.say(
                    vec![format!("Unable to open batch review selection: {error}")],
                    true,
                    cx,
                );
                return;
            }
        };
        let mut state = BatchState {
            query,
            items,
            ..Default::default()
        };
        state.summary = view::batch_summary_lines(self.inbox(), &state.items, &state.selected);
        self.pane = Pane::Batch(state);
    }

    fn toggle_batch(&mut self, id: String, cx: &mut Context<Self>) {
        let memory = self.memory.clone();
        if let Pane::Batch(state) = &mut self.pane {
            if !state.selected.remove(&id) {
                state.selected.insert(id);
            }
            state.preview = None;
            state.preview_lines.clear();
            state.summary =
                view::batch_summary_lines(memory.reviews(), &state.items, &state.selected);
            cx.notify();
        }
    }

    fn preview_batch(&mut self, kind: ReviewActionKind, cx: &mut Context<Self>) {
        let memory = self.memory.clone();
        let Pane::Batch(state) = &mut self.pane else {
            return;
        };
        if state.selected.is_empty() {
            state.preview_lines = vec!["Batch review requires at least one selected item.".into()];
            cx.notify();
            return;
        }
        let mut items = view::batch_items(&state.selected, kind);
        match memory.reviews().preview_review_batch(&items) {
            Ok(preview) => {
                state.preview_lines =
                    view::batch_preview_lines(kind, &preview, &state.query, items.len());
                if preview.applicable {
                    view::confirm_batch(&mut items, &preview);
                    state.preview = Some((kind, preview, items));
                } else {
                    state.preview = None;
                }
            }
            Err(error) => {
                state.preview = None;
                state.preview_lines = vec![format!("Batch preview failed: {error}")];
            }
        }
        cx.notify();
    }

    /// Applies the previewed batch as a background job and polls it.
    fn apply_batch(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let memory = self.memory.clone();
        let Pane::Batch(state) = &mut self.pane else {
            return;
        };
        let Some((_, preview, items)) = state.preview.take() else {
            return;
        };
        let total = items.len();
        let job = match memory
            .reviews()
            .start_review_batch(items, preview.idempotency_token.clone())
        {
            Ok(job) => job,
            Err(error) => {
                let line = format!("Memory review batch failed: {error}");
                state.preview_lines = vec![line.clone()];
                self.say(vec![line], true, cx);
                cx.notify();
                return;
            }
        };
        state.preview_lines = vec![format!("Memory review batch progress: 0/{total}")];
        state.job = Some(job.clone());
        let id = job.id.clone();
        let executor = cx.background_executor().clone();
        self._task = Some(cx.spawn_in(window, async move |this, cx| {
            loop {
                executor.timer(Duration::from_millis(150)).await;
                let job = memory.reviews().review_batch_job(&id);
                let finished = this
                    .update(cx, |this, cx| this.batch_progress(job, total, cx))
                    .unwrap_or(true);
                if finished {
                    break;
                }
            }
        }));
        cx.notify();
    }

    /// Returns whether the job is finished.
    fn batch_progress(
        &mut self,
        job: qt_memory::Result<ReviewBatchJob>,
        total: usize,
        cx: &mut Context<Self>,
    ) -> bool {
        let memory = self.memory.clone();
        let Pane::Batch(state) = &mut self.pane else {
            return true;
        };
        let job = match job {
            Ok(job) => job,
            Err(error) => {
                let line = format!("Memory review batch failed: {error}");
                state.preview_lines = vec![line.clone()];
                state.job = None;
                self.say(vec![line], true, cx);
                cx.notify();
                return true;
            }
        };
        let done = matches!(
            job.status,
            ReviewBatchJobStatus::Completed | ReviewBatchJobStatus::Failed
        );
        state.preview_lines = vec![format!(
            "Memory review batch progress: {}/{total}",
            job.processed_count
        )];
        if done {
            let mut lines = state.preview_lines.clone();
            match &job.result {
                Some(result) => {
                    let (result_lines, failed) = view::batch_result(result, total);
                    lines.extend(result_lines);
                    state.selected = failed;
                }
                None => lines.push(format!(
                    "Memory review batch failed: {}",
                    if job.error.is_empty() {
                        "unknown error"
                    } else {
                        &job.error
                    }
                )),
            }
            state.job = None;
            if let Ok(items) = memory.reviews().review_items(&state.query) {
                state.items = items;
            }
            state.summary =
                view::batch_summary_lines(memory.reviews(), &state.items, &state.selected);
            state.preview_lines = lines.clone();
            let error = job.status == ReviewBatchJobStatus::Failed
                || job.result.as_ref().is_some_and(|r| r.failed_count > 0);
            self.say(lines, error, cx);
            self.refresh_landing();
        } else {
            state.job = Some(job);
        }
        cx.notify();
        done
    }

    // ----- rendering --------------------------------------------------------

    fn render_nav(&self, cx: &mut Context<Self>) -> Div {
        let mut nav = h_flex().gap_1().flex_wrap();
        let mut entries: Vec<(&'static str, &'static str, &'static str)> = Vec::new();
        if self
            .restored
            .as_ref()
            .is_some_and(|state| !state.selected_review_id.is_empty())
        {
            entries.push(("resume", "Resume previous review", "Saved queue position"));
            if self
                .restored
                .as_ref()
                .is_some_and(|state| view::has_active_filters(&state.query))
            {
                entries.push((
                    "clear_filters",
                    "Clear saved filters",
                    "Return to the complete pending queue",
                ));
            }
        }
        entries.extend([
            ("next", "Review next", "Highest-priority pending item"),
            (
                "pending",
                "Browse pending",
                "All pending reviews in ranked order",
            ),
            (
                "batch",
                "Batch pending reviews",
                "Select safe homogeneous work",
            ),
            (
                "dream",
                "Review latest Dream",
                "Newest pending Dreaming result",
            ),
            ("deferred", "Deferred", "Deferred reviews and due work"),
            (
                "history",
                "History",
                "Accepted, rejected, and deferred decisions",
            ),
            (
                "status",
                "Memory status",
                "Show memory health in the transcript",
            ),
        ]);
        for (value, label, tooltip) in entries {
            nav = nav.child(
                Button::new(SharedString::from(format!("inbox-nav-{value}")))
                    .label(label)
                    .xsmall()
                    .ghost()
                    .tooltip(tooltip)
                    .on_click(cx.listener(move |this, _, w, cx| this.landing(value, w, cx))),
            );
        }
        nav
    }

    fn render_filters(&self, cx: &mut Context<Self>) -> Div {
        let mut statuses = h_flex().gap_1().flex_wrap().child(hint("Status", cx));
        let current_status = StatusFilter::of(&self.filter);
        for (i, preset) in StatusFilter::ALL.into_iter().enumerate() {
            statuses = statuses.child(
                chip(
                    ("inbox-status", i),
                    preset.label(),
                    current_status == Some(preset),
                )
                .on_click(cx.listener(move |this, _, _, cx| {
                    this.filter.statuses = preset.statuses();
                    cx.notify();
                })),
            );
        }
        let mut priorities = h_flex().gap_1().flex_wrap().child(hint("Priority", cx));
        for (i, (value, label)) in view::PRIORITY_FILTERS.into_iter().enumerate() {
            let active = if value.is_empty() {
                self.filter.priorities.is_empty()
            } else {
                self.filter.priorities == [value]
            };
            priorities = priorities.child(chip(("inbox-priority", i), label, active).on_click(
                cx.listener(move |this, _, _, cx| {
                    this.filter.priorities = if value.is_empty() {
                        Vec::new()
                    } else {
                        vec![value.to_string()]
                    };
                    cx.notify();
                }),
            ));
        }
        let kinds: Vec<(String, u64)> = self
            .inbox()
            .review_statistics()
            .map(|stats| stats.by_kind.into_iter().collect())
            .unwrap_or_default();
        let mut kind_row = h_flex().gap_1().flex_wrap().child(hint("Kind", cx)).child(
            chip("inbox-kind-any", "Any kind", self.filter.kinds.is_empty()).on_click(cx.listener(
                |this, _, _, cx| {
                    this.filter.kinds.clear();
                    cx.notify();
                },
            )),
        );
        for (i, (kind, count)) in kinds.into_iter().enumerate() {
            let active = self.filter.kinds == [kind.clone()];
            let label = format!("{kind} ({count})");
            kind_row = kind_row.child(chip(("inbox-kind", i), label, active).on_click(
                cx.listener(move |this, _, _, cx| {
                    this.filter.kinds = vec![kind.clone()];
                    cx.notify();
                }),
            ));
        }
        let mut scope_row = h_flex().gap_1().flex_wrap().child(hint("Scope", cx));
        for (i, (scope, label)) in SCOPES.into_iter().enumerate() {
            scope_row = scope_row.child(
                chip(("inbox-scope", i), label, self.filter.scope == scope).on_click(cx.listener(
                    move |this, _, _, cx| {
                        this.filter.scope = scope;
                        cx.notify();
                    },
                )),
            );
        }
        for (i, (oldest, label)) in ORDERINGS.into_iter().enumerate() {
            scope_row = scope_row.child(
                chip(
                    ("inbox-order", i),
                    label,
                    self.filter.oldest_first == oldest,
                )
                .on_click(cx.listener(move |this, _, _, cx| {
                    this.filter.oldest_first = oldest;
                    cx.notify();
                })),
            );
        }
        v_flex()
            .gap_1()
            .child(statuses)
            .child(priorities)
            .child(kind_row)
            .child(scope_row)
            .child(
                h_flex()
                    .gap_1()
                    .child(div().w(px(320.)).child(Input::new(&self.tags).small()))
                    .child(
                        Button::new("inbox-apply-filters")
                            .label("Apply filters")
                            .xsmall()
                            .primary()
                            .on_click(cx.listener(|this, _, w, cx| this.apply_filters(w, cx))),
                    )
                    .child(
                        Button::new("inbox-clear-filters")
                            .label("Clear all filters")
                            .xsmall()
                            .ghost()
                            .on_click(
                                cx.listener(|this, _, w, cx| this.landing("clear_filters", w, cx)),
                            ),
                    ),
            )
    }

    fn render_queue(&self, cx: &mut Context<Self>) -> Stateful<Div> {
        let mut list = v_flex()
            .id("inbox-queue")
            .gap_0p5()
            .w(px(330.))
            .flex_none()
            .max_h(px(430.))
            .overflow_y_scroll()
            .child(section(
                format!(
                    "{} · {}",
                    self.queue_title.to_uppercase(),
                    view::query_filter_label(&self.query)
                ),
                cx,
            ));
        for (index, (label, description)) in self.cards.iter().enumerate() {
            list = list.child(
                widgets::list_row(
                    ("inbox-card", index),
                    label.clone(),
                    description.clone(),
                    index == self.position && !matches!(self.pane, Pane::Batch(_)),
                    cx,
                )
                .on_click(cx.listener(move |this, _, w, cx| this.select(index, w, cx))),
            );
        }
        list
    }

    fn render_detail(&self, cx: &mut Context<Self>) -> Div {
        let Some(detail) = &self.detail else {
            return v_flex();
        };
        let mut actions = h_flex().gap_1().flex_wrap();
        for (i, choice) in view::action_choices(detail).into_iter().enumerate() {
            let kind = choice.kind;
            actions = actions.child(
                Button::new(("inbox-action", i))
                    .label(choice.label)
                    .xsmall()
                    .when(i == 0, |b| b.primary())
                    .tooltip(choice.description)
                    .on_click(cx.listener(move |this, _, w, cx| this.begin(kind, w, cx))),
            );
        }
        actions = actions
            .child(
                Button::new("inbox-inspect")
                    .label("Inspect sources")
                    .xsmall()
                    .outline()
                    .tooltip(format!(
                        "{} sources, {} graph edges",
                        detail.sources.len(),
                        detail.edges.len()
                    ))
                    .on_click(cx.listener(|this, _, _, cx| this.open_evidence(cx))),
            )
            .child(
                Button::new("inbox-next")
                    .label("Next [S]")
                    .xsmall()
                    .ghost()
                    .tooltip("Skip to the next item without mutation")
                    .on_click(cx.listener(|this, _, _, cx| this.next(cx))),
            );
        let mut body = self.detail_lines.clone();
        body.extend(self.accept_lines.iter().cloned());
        body.extend(self.footer_lines.iter().cloned());
        // Outcomes first so they stay visible above long previews.
        v_flex()
            .gap_2()
            .child(actions)
            .child(hint(
                "Keys: A approve  E edit  R reject  D defer  S next  ↑/↓ move",
                cx,
            ))
            .child(widgets::lines(&body, false, cx))
    }

    fn render_action(&self, cx: &mut Context<Self>) -> Div {
        let theme = cx.theme().clone();
        let Pane::Action(draft) = &self.pane else {
            return v_flex();
        };
        let Some(detail) = &self.detail else {
            return v_flex();
        };
        let cancel = Button::new("inbox-action-cancel")
            .label("Back")
            .xsmall()
            .ghost()
            .on_click(cx.listener(|this, _, _, cx| {
                this.pane = Pane::Detail;
                cx.notify();
            }));
        let next = |label: &'static str| {
            Button::new("inbox-action-next")
                .label(label)
                .xsmall()
                .primary()
                .on_click(cx.listener(|this, _, w, cx| this.advance(w, cx)))
        };
        let mut body =
            v_flex()
                .gap_2()
                .child(
                    div()
                        .text_sm()
                        .font_weight(FontWeight::SEMIBOLD)
                        .child(format!(
                            "{} · step {}/{}",
                            draft.action.kind,
                            draft.step + 1,
                            draft.inputs.len()
                        )),
                );
        match draft.inputs.get(draft.step) {
            Some(ActionInput::Content {
                title,
                prompt,
                hint: text,
            }) => {
                body = body
                    .child(section(title.to_uppercase(), cx))
                    .child(hint(format!("{prompt}: {text}"), cx))
                    .child(Textarea::new(&self.content))
                    .child(h_flex().gap_1().child(next("Continue")).child(cancel));
            }
            Some(ActionInput::EditFields) => {
                body = body
                    .child(section("EDIT SUPPORTED FIELDS", cx))
                    .child(hint(
                        "Content is staged. Scope narrowing remains a separate explicit governed action.",
                        cx,
                    ));
                for (i, spec) in view::edit_fields(detail.candidate.kind)
                    .into_iter()
                    .enumerate()
                {
                    let field = spec.field;
                    let value = view::edit_field_value(&draft.action, field);
                    let mut row = v_flex().gap_1().child(
                        h_flex()
                            .gap_2()
                            .child(div().text_xs().w(px(130.)).child(spec.label))
                            .child(hint(format!("{} · now: {value}", spec.description), cx)),
                    );
                    match field {
                        EditField::Type | EditField::Status => {
                            let options: Vec<String> = if field == EditField::Type {
                                view::EDIT_TYPES.iter().map(MemoryType::to_string).collect()
                            } else {
                                view::EDIT_STATUSES
                                    .iter()
                                    .map(MemoryStatus::to_string)
                                    .collect()
                            };
                            let mut chips = h_flex().gap_1().flex_wrap();
                            for (j, option) in options.into_iter().enumerate() {
                                let active = value == option;
                                chips = chips.child(
                                    chip(
                                        SharedString::from(format!("inbox-edit-{i}-{j}")),
                                        option.clone(),
                                        active,
                                    )
                                    .on_click(cx.listener(
                                        move |this, _, _, cx| {
                                            let option = option.clone();
                                            this.set_draft(
                                                |action| {
                                                    view::apply_edit_field(action, field, &option)
                                                },
                                                cx,
                                            )
                                        },
                                    )),
                                );
                            }
                            row = row.child(chips);
                        }
                        _ => {
                            row = row.child(
                                h_flex().child(
                                    Button::new(("inbox-edit-field", i))
                                        .label(format!("Edit {}", field.id()))
                                        .xsmall()
                                        .outline()
                                        .tooltip(field.hint())
                                        .on_click(cx.listener(move |this, _, w, cx| {
                                            this.text.update(cx, |s, cx| s.set_value("", w, cx));
                                            if let Pane::Action(draft) = &mut this.pane {
                                                draft.field = Some(field);
                                            }
                                            cx.notify();
                                        })),
                                ),
                            );
                        }
                    }
                    body = body.child(row);
                }
                if let Some(field) = draft.field {
                    body = body.child(
                        h_flex()
                            .gap_1()
                            .child(hint(format!("{}: {}", field.id(), field.hint()), cx))
                            .child(div().w(px(260.)).child(Input::new(&self.text).small()))
                            .child(
                                Button::new("inbox-edit-set")
                                    .label("Set")
                                    .xsmall()
                                    .on_click(cx.listener(move |this, _, _, cx| {
                                        let value = this.text.read(cx).value().to_string();
                                        this.set_draft(
                                            |action| view::apply_edit_field(action, field, &value),
                                            cx,
                                        );
                                        if let Pane::Action(draft) = &mut this.pane
                                            && draft.error.is_none()
                                        {
                                            draft.field = None;
                                        }
                                    })),
                            ),
                    );
                }
                body = body.child(
                    h_flex()
                        .gap_1()
                        .child(next("Preview edited approval"))
                        .child(cancel),
                );
            }
            Some(ActionInput::Evidence { title }) => {
                body = body
                    .child(section(title.to_uppercase(), cx))
                    .child(hint("URI: independent evidence is required", cx))
                    .child(Input::new(&self.text).small())
                    .child(h_flex().gap_1().child(next("Continue")).child(cancel));
            }
            Some(ActionInput::DeferChoice) => {
                let mut choices = h_flex().gap_1().flex_wrap();
                for (i, (seconds, label, description)) in
                    view::DEFER_CHOICES.into_iter().enumerate()
                {
                    choices = choices.child(
                        Button::new(("inbox-defer", i))
                            .label(label)
                            .xsmall()
                            .tooltip(description)
                            .on_click(cx.listener(move |this, _, w, cx| {
                                if let Pane::Action(draft) = &mut this.pane {
                                    draft.action.deferred_until = Some(now_ms() + seconds * 1000);
                                }
                                this.advance(w, cx)
                            })),
                    );
                }
                body = body
                    .child(section("DEFER REVIEW", cx))
                    .child(hint("Choose when it should resurface.", cx))
                    .child(choices.child(cancel));
            }
            Some(ActionInput::TargetChoice) => {
                let mut list = v_flex().gap_0p5();
                for (i, target) in view::target_choices(detail).into_iter().enumerate() {
                    let id = target.id.clone();
                    list = list.child(
                        widgets::list_row(
                            ("inbox-target", i),
                            format!("{}  ({})", target.label, target.id),
                            target.description,
                            false,
                            cx,
                        )
                        .on_click(cx.listener(move |this, _, w, cx| {
                            if let Pane::Action(draft) = &mut this.pane {
                                draft.action.target_id = id.clone();
                            }
                            this.advance(w, cx)
                        })),
                    );
                }
                body = body
                    .child(section("CHOOSE THE SURVIVING MEMORY", cx))
                    .child(hint("Other reviewed memories will point to it.", cx))
                    .child(list)
                    .child(h_flex().child(cancel));
            }
            Some(ActionInput::Note) => {
                body = body
                    .child(section("REVIEW NOTE", cx))
                    .child(hint(
                        "Optional audit note; Preview shows the exact changes.",
                        cx,
                    ))
                    .child(Input::new(&self.note).small())
                    .child(h_flex().gap_1().child(next("Preview")).child(cancel));
            }
            None => {}
        }
        body.when_some(draft.error.clone(), |el, error| {
            el.child(div().text_xs().text_color(theme.danger).child(error))
        })
    }

    fn render_pane(&mut self, cx: &mut Context<Self>) -> AnyElement {
        let theme = cx.theme().clone();
        match &self.pane {
            Pane::Empty(title) => v_flex()
                .gap_1()
                .child(div().text_sm().child(title.clone()))
                .child(hint("No review is required for this view.", cx))
                .child(hint(
                    "Memory status explains queue health and settings.",
                    cx,
                ))
                .into_any_element(),
            Pane::Detail => self.render_detail(cx).into_any_element(),
            Pane::Action(_) => self.render_action(cx).into_any_element(),
            Pane::Confirm { action, lines } => {
                let action = action.clone();
                v_flex()
                    .gap_2()
                    .child(
                        h_flex()
                            .gap_1()
                            .child(
                                Button::new("inbox-confirm")
                                    .label("Confirm")
                                    .xsmall()
                                    .primary()
                                    .tooltip("Apply this exact preview atomically")
                                    .on_click(cx.listener(move |this, _, _, cx| {
                                        this.apply(action.clone(), cx)
                                    })),
                            )
                            .child(
                                Button::new("inbox-confirm-cancel")
                                    .label("Cancel")
                                    .xsmall()
                                    .ghost()
                                    .tooltip("Return without changes")
                                    .on_click(cx.listener(|this, _, _, cx| {
                                        this.pane = Pane::Detail;
                                        cx.notify();
                                    })),
                            ),
                    )
                    .child(widgets::lines(lines, false, cx))
                    .into_any_element()
            }
            Pane::Evidence { path, file } => {
                let mut lines = self
                    .detail
                    .as_ref()
                    .map(view::evidence_lines)
                    .unwrap_or_default();
                let uri_set = self
                    .detail
                    .as_ref()
                    .is_some_and(|d| !d.item.provenance.source_uri.is_empty());
                if path.is_none() && uri_set {
                    lines.push(
                        "BLOCKED: source evidence is not a permitted local workspace path".into(),
                    );
                }
                let mut body = v_flex()
                    .gap_2()
                    .child(widgets::lines(&lines, false, cx))
                    .child(
                        h_flex()
                            .gap_1()
                            .when_some(path.clone(), |el, path| {
                                el.child(
                                    Button::new("inbox-open-evidence")
                                        .label("Open source evidence")
                                        .xsmall()
                                        .tooltip(path.display().to_string())
                                        .on_click(
                                            cx.listener(|this, _, _, cx| this.read_evidence(cx)),
                                        ),
                                )
                            })
                            .child(
                                Button::new("inbox-evidence-back")
                                    .label("Back to review")
                                    .xsmall()
                                    .ghost()
                                    .on_click(cx.listener(|this, _, _, cx| {
                                        this.pane = Pane::Detail;
                                        cx.notify();
                                    })),
                            ),
                    );
                if let (Some(path), Some(file)) = (path, file) {
                    body = body
                        .child(section("SOURCE EVIDENCE (READ-ONLY)", cx))
                        .child(hint(path.display().to_string(), cx));
                    body = match file {
                        Ok(text) => body.child(
                            div()
                                .p_2()
                                .rounded_md()
                                .bg(theme.secondary)
                                .text_xs()
                                .font_family(theme.mono_font_family.clone())
                                .child(text.clone()),
                        ),
                        Err(error) => body.child(
                            div()
                                .text_xs()
                                .text_color(theme.danger)
                                .child(error.clone()),
                        ),
                    };
                }
                body.into_any_element()
            }
            Pane::History {
                filter,
                records,
                selected,
            } => {
                let filter = *filter;
                let mut chips = h_flex().gap_1().flex_wrap();
                for (i, (value, label)) in view::HISTORY_FILTERS.into_iter().enumerate() {
                    chips = chips.child(
                        chip(("inbox-history-filter", i), label, filter == value).on_click(
                            cx.listener(move |this, _, _, cx| {
                                this.open_history(value);
                                cx.notify();
                            }),
                        ),
                    );
                }
                let mut list = v_flex().gap_0p5();
                let mut shown = 0;
                for (index, record) in records.iter().enumerate() {
                    if !view::history_matches(record, filter) {
                        continue;
                    }
                    shown += 1;
                    let (title, description) = view::history_row(record);
                    list = list.child(
                        widgets::list_row(
                            ("inbox-history", index),
                            title,
                            description,
                            *selected == Some(index),
                            cx,
                        )
                        .on_click(cx.listener(move |this, _, _, cx| {
                            if let Pane::History { selected, .. } = &mut this.pane {
                                *selected = Some(index);
                            }
                            cx.notify();
                        })),
                    );
                }
                let receipt = selected
                    .and_then(|index| records.get(index))
                    .map(view::history_receipt_lines);
                v_flex()
                    .gap_2()
                    .child(section("REVIEW HISTORY", cx))
                    .child(hint(
                        "Accepted, rejected, deferred, reopened, failed, and compensated actions with linked receipts.",
                        cx,
                    ))
                    .child(chips)
                    .when(shown == 0, |el| el.child(hint("No recorded actions match.", cx)))
                    .child(list)
                    .when_some(receipt, |el, lines| el.child(widgets::lines(&lines, false, cx)))
                    .into_any_element()
            }
            Pane::Batch(state) => {
                let running = state.job.is_some();
                let mut list = v_flex().gap_0p5();
                for (index, item) in state.items.iter().enumerate() {
                    let id = item.id.clone();
                    let checked = state.selected.contains(&item.id);
                    list = list.child(
                        h_flex()
                            .gap_2()
                            .items_start()
                            .child(
                                Checkbox::new(("inbox-batch-check", index))
                                    .checked(checked)
                                    .disabled(running)
                                    .on_click(cx.listener(move |this, _, _, cx| {
                                        this.toggle_batch(id.clone(), cx)
                                    })),
                            )
                            .child(
                                v_flex()
                                    .child(
                                        div()
                                            .text_xs()
                                            .child(format!("{}  {}", item.kind, item.priority)),
                                    )
                                    .child(hint(view::text_excerpt(&item.reason, 90), cx)),
                            ),
                    );
                }
                let can_confirm = state.preview.is_some() && !running;
                v_flex()
                    .gap_2()
                    .child(widgets::lines(&state.summary, false, cx))
                    .child(
                        h_flex()
                            .gap_1()
                            .child(
                                Button::new("inbox-batch-approve")
                                    .label("Approve selected")
                                    .xsmall()
                                    .disabled(running)
                                    .tooltip("Runtime rejects unsafe approval batches")
                                    .on_click(cx.listener(|this, _, _, cx| {
                                        this.preview_batch(ReviewActionKind::Approve, cx)
                                    })),
                            )
                            .child(
                                Button::new("inbox-batch-reject")
                                    .label("Reject selected")
                                    .xsmall()
                                    .disabled(running)
                                    .tooltip("Preserve audit history without deletion")
                                    .on_click(cx.listener(|this, _, _, cx| {
                                        this.preview_batch(ReviewActionKind::Reject, cx)
                                    })),
                            )
                            .child(
                                Button::new("inbox-batch-clear")
                                    .label("Clear selection")
                                    .xsmall()
                                    .ghost()
                                    .disabled(running)
                                    .on_click(cx.listener(|this, _, _, cx| {
                                        let memory = this.memory.clone();
                                        if let Pane::Batch(state) = &mut this.pane {
                                            state.selected.clear();
                                            state.preview = None;
                                            state.preview_lines.clear();
                                            state.summary = view::batch_summary_lines(
                                                memory.reviews(),
                                                &state.items,
                                                &state.selected,
                                            );
                                        }
                                        cx.notify();
                                    })),
                            ),
                    )
                    .when(!state.preview_lines.is_empty(), |el| {
                        el.child(
                            h_flex()
                                .gap_2()
                                .when(running, |el| el.child(Spinner::new().small()))
                                .child(widgets::lines(&state.preview_lines, false, cx)),
                        )
                    })
                    .when(can_confirm, |el| {
                        el.child(
                            h_flex().child(
                                Button::new("inbox-batch-confirm")
                                    .label("Confirm batch")
                                    .xsmall()
                                    .primary()
                                    .tooltip("Apply the exact aggregate preview")
                                    .on_click(
                                        cx.listener(|this, _, w, cx| this.apply_batch(w, cx)),
                                    ),
                            ),
                        )
                    })
                    .child(list)
                    .into_any_element()
            }
        }
    }
}

impl Render for ReviewInboxView {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        let pane = self.render_pane(cx);
        v_flex()
            .track_focus(&self.focus)
            .on_key_down(cx.listener(|this, event: &KeyDownEvent, window, cx| {
                let key = event.keystroke.key.to_ascii_lowercase();
                if !event.keystroke.modifiers.modified() {
                    this.shortcut(&key, window, cx);
                }
            }))
            .gap_2()
            .child(widgets::lines(&self.landing, false, cx))
            .child(
                self.render_nav(cx).child(
                    chip(
                        "inbox-nav-filters",
                        format!(
                            "Filters: {}",
                            if view::has_active_filters(&self.filter) {
                                view::query_filter_label(&self.filter)
                            } else {
                                "none".into()
                            }
                        ),
                        self.show_filters,
                    )
                    .tooltip("Combine status, category, priority, scope, tags, and ordering")
                    .on_click(cx.listener(|this, _, _, cx| {
                        this.show_filters = !this.show_filters;
                        cx.notify();
                    })),
                ),
            )
            .when(self.show_filters, |el| el.child(self.render_filters(cx)))
            .when(!self.notice.is_empty(), |el| {
                el.child(
                    div()
                        .p_2()
                        .rounded_md()
                        .bg(if self.notice_error {
                            theme.danger.opacity(0.1)
                        } else {
                            theme.success.opacity(0.1)
                        })
                        .child(widgets::lines(&self.notice, self.notice_error, cx)),
                )
            })
            .child(
                h_flex()
                    .gap_3()
                    .items_start()
                    .when(
                        !matches!(self.pane, Pane::Batch(_) | Pane::History { .. }),
                        |el| el.child(self.render_queue(cx)),
                    )
                    .child(
                        div()
                            .id("inbox-pane")
                            .flex_1()
                            .min_w_0()
                            .max_h(px(430.))
                            .overflow_y_scroll()
                            .child(pane),
                    ),
            )
    }
}

/// Opens the Review Inbox for the runtime's memory.
pub fn open(host: MemoryHost, window: &mut Window, cx: &mut App) {
    let Some(memory) = host.memory() else {
        return;
    };
    let view = cx.new(|cx| ReviewInboxView::new(host, memory, window, cx));
    view.update(cx, |view, cx| window.focus(&view.focus, cx));
    window.open_dialog(cx, move |dialog, _, _| {
        dialog
            .title("Memory Review Inbox")
            .w(px(1180.))
            .on_ok(|_, _, _| false)
            .child(view.clone())
    });
}
