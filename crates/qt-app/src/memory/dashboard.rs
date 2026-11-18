//! Native memory dashboard window (replaces the archived ImGui/WASM
//! browser dashboard): Overview, Explore (search, filters, detail, history,
//! lifecycle and governed Dream review), Graph, Sources, Sessions/Agents,
//! Context pack builder, Capture, Operations and Logs/System.

use std::collections::HashMap;
use std::rc::Rc;
use std::sync::Arc;

use anyhow::Result;
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
use qt_memory::model::{
    MemoryEntry, MemoryStatus, MemoryType, SourceEvent, SourceRecord, SourceType,
};
use qt_memory::review::{ReviewAction, ReviewItem, ReviewMutationPreview};
use qt_memory::slash::dream_lines;
use qt_memory::views::dashboard::{
    self as data, CaptureWrite, ContextBuild, ContextBuildResult, DashboardSummary, GraphData,
    GroupRow, LifecycleAction, MemoryDetail, MemoryFilter, NodeLayout,
};
use qt_memory::views::review::{parse_confidence, parse_tags, receipt_lines, text_excerpt};

use super::MemoryHost;
use super::graph::{self, GraphView};
use super::widgets::{self, chip, hint, section};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Tab {
    Overview,
    Explore,
    Graph,
    Sources,
    Sessions,
    Context,
    Capture,
    Operations,
    Logs,
}

const TABS: [(Tab, &str, &str); 9] = [
    (Tab::Overview, "Overview", "Your memory at a glance"),
    (
        Tab::Explore,
        "Explore",
        "Search memories, details and history",
    ),
    (Tab::Graph, "Graph", "Typed memory relationships"),
    (Tab::Sources, "Sources", "Where memories came from"),
    (
        Tab::Sessions,
        "Sessions & Agents",
        "Memories by session or agent",
    ),
    (
        Tab::Context,
        "Context pack",
        "Preview what recall would inject",
    ),
    (
        Tab::Capture,
        "Capture",
        "Write memories and process raw text",
    ),
    (Tab::Operations, "Operations", "Retrieval and Dreaming"),
    (Tab::Logs, "Logs & System", "Audit log and service details"),
];

type DreamPreview = (ReviewItem, ReviewAction, ReviewMutationPreview);

pub struct MemoryDashboard {
    host: MemoryHost,
    memory: Arc<MemoryRuntime>,
    tab: Tab,
    banner: Option<(String, bool)>,
    busy: Option<String>,
    summary: Option<std::result::Result<DashboardSummary, String>>,
    // Explore
    search: Entity<InputState>,
    filter: MemoryFilter,
    entries: Vec<MemoryEntry>,
    selected: Option<String>,
    detail: Option<std::result::Result<MemoryDetail, String>>,
    dream_preview: Option<DreamPreview>,
    // Graph
    graph: GraphData,
    layout: HashMap<String, NodeLayout>,
    view: GraphView,
    drag: Option<Point<Pixels>>,
    edge_type: Entity<InputState>,
    // Sources
    sources: Vec<SourceRecord>,
    selected_source: Option<String>,
    source_events: Vec<SourceEvent>,
    // Sessions / agents
    by_agent: bool,
    group_memories: Vec<MemoryEntry>,
    groups: Vec<GroupRow>,
    selected_group: Option<String>,
    // Context
    context_query: Entity<InputState>,
    context_budget: Entity<InputState>,
    context_policy: &'static str,
    context_global: bool,
    context_result: Option<std::result::Result<ContextBuildResult, String>>,
    context_markdown: Entity<TextareaState>,
    // Capture
    capture_content: Entity<TextareaState>,
    capture_type: MemoryType,
    capture_status: MemoryStatus,
    capture_confidence: Entity<InputState>,
    capture_source_uri: Entity<InputState>,
    capture_tags: Entity<InputState>,
    raw_text: Entity<TextareaState>,
    raw_dry_run: bool,
    raw_lines: Vec<String>,
    // Operations / logs
    operation_lines: Vec<String>,
    log_lines: Vec<String>,
    _subscriptions: Vec<Subscription>,
}

const POLICIES: [(&str, &str); 4] = [
    ("", "configured"),
    ("strict", "strict"),
    ("balanced", "balanced"),
    ("broad", "broad"),
];

impl MemoryDashboard {
    fn new(
        host: MemoryHost,
        memory: Arc<MemoryRuntime>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let input = |placeholder: &'static str, window: &mut Window, cx: &mut Context<Self>| {
            cx.new(|cx| InputState::new(window, cx).placeholder(placeholder))
        };
        let search = input("Search memories (Enter to search)", window, cx);
        let context_query = input("Query or task to build context for", window, cx);
        let context_budget = input("Token budget (1–16000)", window, cx);
        context_budget.update(cx, |s, cx| s.set_value("900", window, cx));
        let capture_confidence = input("Confidence 0–1", window, cx);
        capture_confidence.update(cx, |s, cx| s.set_value("0.50", window, cx));
        let subscriptions = vec![
            cx.subscribe_in(&search, window, |this, _, event: &InputEvent, _, cx| {
                if let InputEvent::PressEnter { .. } = event {
                    this.search(cx);
                }
            }),
            cx.subscribe_in(
                &context_query,
                window,
                |this, _, event: &InputEvent, window, cx| {
                    if let InputEvent::PressEnter { .. } = event {
                        this.build_context(window, cx);
                    }
                },
            ),
        ];
        let mut this = Self {
            tab: Tab::Overview,
            banner: None,
            busy: None,
            summary: None,
            search,
            filter: MemoryFilter::default(),
            entries: Vec::new(),
            selected: None,
            detail: None,
            dream_preview: None,
            graph: GraphData::default(),
            layout: HashMap::new(),
            view: GraphView {
                zoom: 1.0,
                pan_x: graph::WIDTH * 0.5,
                pan_y: graph::HEIGHT * 0.5,
            },
            drag: None,
            edge_type: input("Edge type filter (blank: all)", window, cx),
            sources: Vec::new(),
            selected_source: None,
            source_events: Vec::new(),
            by_agent: false,
            group_memories: Vec::new(),
            groups: Vec::new(),
            selected_group: None,
            context_query,
            context_budget,
            context_policy: "",
            context_global: false,
            context_result: None,
            context_markdown: cx.new(|cx| TextareaState::new(window, cx)),
            capture_content: cx.new(|cx| {
                TextareaState::new(window, cx)
                    .auto_grow(4, 10)
                    .placeholder("A decision, constraint, or observation to remember")
            }),
            capture_type: MemoryType::Observation,
            capture_status: MemoryStatus::Hypothesis,
            capture_confidence,
            capture_source_uri: input("Source URI (optional)", window, cx),
            capture_tags: input("Tags, comma-separated", window, cx),
            raw_text: cx.new(|cx| {
                TextareaState::new(window, cx)
                    .auto_grow(5, 12)
                    .placeholder("Raw session text to extract memories from")
            }),
            raw_dry_run: true,
            raw_lines: Vec::new(),
            operation_lines: Vec::new(),
            log_lines: Vec::new(),
            _subscriptions: subscriptions,
            host,
            memory,
        };
        this.refresh_all();
        this
    }

    fn ok(&mut self, message: impl Into<String>) {
        self.banner = Some((message.into(), false));
    }

    fn err(&mut self, message: impl Into<String>) {
        self.banner = Some((message.into(), true));
    }

    fn refresh_all(&mut self) {
        self.summary = Some(data::summary(&self.memory).map_err(|e| e.to_string()));
        self.refresh_entries();
        self.refresh_graph();
        self.refresh_sources();
        self.refresh_groups();
        self.refresh_operations();
        self.refresh_logs();
    }

    fn refresh_entries(&mut self) {
        match data::memories(&self.memory, &self.filter) {
            Ok(entries) => self.entries = entries,
            Err(error) => self.err(format!("Memory search failed: {error}")),
        }
        if let Some(id) = self.selected.clone() {
            self.load_detail(&id);
        }
    }

    fn search(&mut self, cx: &mut Context<Self>) {
        self.filter.query = self.search.read(cx).value().to_string();
        self.refresh_entries();
        cx.notify();
    }

    fn load_detail(&mut self, id: &str) {
        self.selected = Some(id.to_string());
        self.dream_preview = None;
        self.detail = Some(data::memory_detail(&self.memory, id).map_err(|e| e.to_string()));
    }

    fn select_memory(&mut self, id: String, cx: &mut Context<Self>) {
        self.load_detail(&id);
        cx.notify();
    }

    fn refresh_graph(&mut self) {
        let edge_type = String::new();
        self.reload_graph(&edge_type);
    }

    fn reload_graph(&mut self, edge_type: &str) {
        match data::graph(&self.memory, &self.filter, edge_type, 250) {
            Ok(graph) => {
                self.layout = data::graph_layout(&graph);
                self.view = GraphView::fit(&self.layout);
                self.graph = graph;
            }
            Err(error) => self.err(format!("Graph failed: {error}")),
        }
    }

    fn refresh_sources(&mut self) {
        match data::sources(&self.memory) {
            Ok(sources) => self.sources = sources,
            Err(error) => self.err(format!("Sources failed: {error}")),
        }
        if self.selected_source.is_none() {
            self.selected_source = self.sources.first().map(|s| s.id.clone());
        }
        if let Some(id) = self.selected_source.clone() {
            self.source_events = data::source_events(&self.memory, &id).unwrap_or_default();
        }
    }

    fn refresh_groups(&mut self) {
        match data::memories(&self.memory, &MemoryFilter::default()) {
            Ok(memories) => {
                self.groups = data::group_rows(&memories, self.by_agent);
                self.group_memories = memories;
            }
            Err(error) => self.err(format!("Sessions failed: {error}")),
        }
        if self
            .selected_group
            .as_ref()
            .is_none_or(|id| !self.groups.iter().any(|g| &g.id == id))
        {
            self.selected_group = self.groups.first().map(|g| g.id.clone());
        }
    }

    fn refresh_operations(&mut self) {
        let mut lines = data::retrieval_lines(&self.memory);
        lines.push(String::new());
        lines.extend(dream_lines(&self.memory, "status"));
        self.operation_lines = lines;
    }

    fn refresh_logs(&mut self) {
        self.log_lines = data::audit_lines(&self.memory, 200)
            .unwrap_or_else(|error| vec![format!("Audit log unavailable: {error}")]);
    }

    // ----- memory actions ----------------------------------------------------

    fn lifecycle(&mut self, action: LifecycleAction, cx: &mut Context<Self>) {
        let Some(id) = self.selected.clone() else {
            return;
        };
        match data::transition(&self.memory, &id, action) {
            Ok(_) => {
                self.ok(action.done_text());
                self.refresh_all();
            }
            Err(error) => self.err(format!("Memory action failed: {error}")),
        }
        cx.notify();
    }

    fn preview_dream(&mut self, approve: bool, cx: &mut Context<Self>) {
        let Some(id) = self.selected.clone() else {
            return;
        };
        match data::preview_dream_review(&self.memory, &id, approve) {
            Ok(preview) => self.dream_preview = Some(preview),
            Err(error) => self.err(format!("Dream review preview failed: {error}")),
        }
        cx.notify();
    }

    fn apply_dream(&mut self, cx: &mut Context<Self>) {
        let Some((review, action, preview)) = self.dream_preview.take() else {
            return;
        };
        match data::apply_dream_review(&self.memory, &review.id, action, &preview) {
            Ok(result) => {
                let lines = receipt_lines(&review.id, &result);
                self.ok(lines.join(" "));
                self.host.card("/memory dashboard", lines, false, cx);
                self.refresh_all();
            }
            Err(error) => self.err(format!("Dream review failed: {error}")),
        }
        cx.notify();
    }

    fn forget(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let Some(id) = self.selected.clone() else {
            return;
        };
        let this = cx.entity().downgrade();
        window.open_alert_dialog(cx, move |alert, _, _| {
            let this = this.clone();
            let id = id.clone();
            alert
                .confirm()
                .title(format!("Forget memory {id}?"))
                .description("The memory is deleted from the store. Deletion also requires the normal mutation approval policy and cannot be undone.")
                .ok_text("Forget")
                .on_ok(move |_, window, cx| {
                    let id = id.clone();
                    let _ = this.update(cx, |this, cx| this.run_forget(id, window, cx));
                    true
                })
        });
    }

    fn run_forget(&mut self, id: String, window: &mut Window, cx: &mut Context<Self>) {
        let context = self.host.slash_context();
        let handle = self.host.runtime.engine().spawn(async move {
            qt_memory::slash::memory_slash_command(&context, &format!("forget {id} confirm")).await
        });
        cx.spawn_in(window, async move |this, cx| {
            let response = handle.await;
            let _ = this.update(cx, |this, cx| {
                match response {
                    Ok(response) => {
                        let failed = response.lines.iter().any(|l| l.contains("failed"));
                        if failed {
                            this.err(response.lines.join(" "));
                        } else {
                            this.ok("Forgotten memory.");
                            this.selected = None;
                            this.detail = None;
                        }
                        this.host.card("/memory forget", response.lines, failed, cx);
                    }
                    Err(error) => this.err(format!("Forget failed: {error}")),
                }
                this.refresh_all();
                cx.notify();
            });
        })
        .detach();
    }

    // ----- context, capture, operations ----------------------------------------

    /// Builds the context pack off the UI thread (recall may embed the
    /// query with a local model).
    fn build_context(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.busy.is_some() {
            return;
        }
        let budget = self.context_budget.read(cx).value().trim().parse::<usize>();
        let input = ContextBuild {
            query: self.context_query.read(cx).value().to_string(),
            recall_policy: self.context_policy.to_string(),
            budget_tokens: budget.unwrap_or(0),
            include_global_user: self.context_global,
            ..Default::default()
        };
        let memory = self.memory.clone();
        let work = super::blocking(&self.host.runtime, move || {
            data::build_context(&memory, &input).map_err(|e| e.to_string())
        });
        self.busy = Some("Building context pack…".into());
        cx.spawn_in(window, async move |this, cx| {
            let result = work.await.and_then(|result| result);
            let _ = this.update_in(cx, |this, window, cx| {
                this.busy = None;
                let markdown = match &result {
                    Ok(result) if result.markdown.is_empty() => "(empty)".to_string(),
                    Ok(result) => result.markdown.clone(),
                    Err(_) => String::new(),
                };
                this.context_markdown
                    .update(cx, |s, cx| s.set_value(markdown, window, cx));
                this.context_result = Some(result);
                cx.notify();
            });
        })
        .detach();
        cx.notify();
    }

    fn write_memory(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let content = self.capture_content.read(cx).value().trim().to_string();
        if content.is_empty() {
            self.err("Write failed: content is required");
            cx.notify();
            return;
        }
        let confidence = match parse_confidence(&self.capture_confidence.read(cx).value()) {
            Ok(value) => value,
            Err(error) => {
                self.err(error.replace("Invalid review edit: ", "Write failed: "));
                cx.notify();
                return;
            }
        };
        let input = CaptureWrite {
            content,
            memory_type: self.capture_type,
            status: self.capture_status,
            confidence,
            source_type: SourceType::Manual,
            source_uri: self.capture_source_uri.read(cx).value().trim().to_string(),
            tags: parse_tags(&self.capture_tags.read(cx).value()),
            ..Default::default()
        };
        match data::write_memory(&self.memory, &input) {
            Ok(entry) => {
                self.ok(format!("Memory written: {} ({})", entry.id, entry.status));
                self.capture_content
                    .update(cx, |s, cx| s.set_value("", window, cx));
                self.refresh_all();
            }
            Err(error) => self.err(format!("Write failed: {error}")),
        }
        cx.notify();
    }

    /// Extracts candidates off the UI thread (storing them when not a dry
    /// run).
    fn process_text(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.busy.is_some() {
            return;
        }
        let text = self.raw_text.read(cx).value().to_string();
        let tags = parse_tags(&self.capture_tags.read(cx).value());
        let uri = self.capture_source_uri.read(cx).value().trim().to_string();
        let dry_run = self.raw_dry_run;
        let memory = self.memory.clone();
        let work = super::blocking(&self.host.runtime, move || {
            data::process_text(&memory, &text, &uri, tags, dry_run).map_err(|e| e.to_string())
        });
        self.busy = Some("Processing raw text…".into());
        cx.spawn_in(window, async move |this, cx| {
            let result = work.await.and_then(|result| result);
            let _ = this.update(cx, |this, cx| {
                this.busy = None;
                match result {
                    Ok(result) => {
                        let mut lines = vec![format!(
                            "Processed raw text: {} candidate(s), {} stored{}",
                            result.candidates.len(),
                            result.stored_ids.len(),
                            if dry_run { " (dry run)" } else { "" }
                        )];
                        lines.extend(result.candidates.iter().map(|candidate| {
                            format!(
                                "  {} confidence={:.2} [{}] {}",
                                candidate.memory_type,
                                candidate.confidence,
                                candidate.tags.join(", "),
                                text_excerpt(&candidate.content, 100)
                            )
                        }));
                        if !result.stored_ids.is_empty() {
                            lines.push(format!("Stored: {}", result.stored_ids.join(", ")));
                        }
                        this.raw_lines = lines;
                        this.ok("Raw text processed.");
                        if !dry_run {
                            this.refresh_all();
                        }
                    }
                    Err(error) => this.err(format!("Processing failed: {error}")),
                }
                cx.notify();
            });
        })
        .detach();
        cx.notify();
    }

    fn rebuild_retrieval(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let this = cx.entity().downgrade();
        window.open_alert_dialog(cx, move |alert, _, _| {
            let this = this.clone();
            alert
                .confirm()
                .title("Rebuild retrieval indexes?")
                .description("Invalidates chunks and vectors of every memory in this scope and queues re-indexing jobs.")
                .ok_text("Rebuild")
                .on_ok(move |_, window, cx| {
                    let _ = this.update(cx, |this, cx| this.start_rebuild(window, cx));
                    true
                })
        });
    }

    fn start_rebuild(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.busy.is_some() {
            return;
        }
        let memory = self.memory.clone();
        let work = super::blocking(&self.host.runtime, move || {
            memory.rebuild_retrieval(None).map_err(|e| e.to_string())
        });
        self.busy = Some("Rebuilding retrieval indexes…".into());
        cx.spawn_in(window, async move |this, cx| {
            let result = work.await.and_then(|result| result);
            let _ = this.update(cx, |this, cx| {
                this.busy = None;
                match result {
                    Ok(report) => {
                        let lines = data::rebuild_lines(&report);
                        this.ok(lines.join(" "));
                        this.refresh_operations();
                    }
                    Err(error) => this.err(format!("Retrieval rebuild failed: {error}")),
                }
                cx.notify();
            });
        })
        .detach();
        cx.notify();
    }

    fn run_dream(&mut self, cx: &mut Context<Self>) {
        let lines = dream_lines(&self.memory, "run");
        let queued = lines
            .first()
            .is_some_and(|l| l.contains("queued in the background"));
        self.banner = Some((lines.join(" "), !queued));
        self.refresh_operations();
        cx.notify();
    }

    // ----- rendering ------------------------------------------------------------

    fn render_overview(&self, cx: &mut Context<Self>) -> Div {
        let theme = cx.theme().clone();
        let Some(summary) = &self.summary else {
            return v_flex();
        };
        let summary = match summary {
            Ok(summary) => summary,
            Err(error) => {
                return v_flex().child(div().text_color(theme.danger).child(error.clone()));
            }
        };
        let distribution =
            |title: &str, counts: &std::collections::BTreeMap<String, u64>, cx: &App| {
                let total: u64 = counts.values().sum();
                let mut card = v_flex()
                    .flex_1()
                    .p_3()
                    .gap_1()
                    .rounded_lg()
                    .border_1()
                    .border_color(cx.theme().border)
                    .child(section(title.to_uppercase(), cx));
                if counts.is_empty() {
                    card = card.child(hint("No memories yet.", cx));
                }
                for (name, count) in counts {
                    let share = if total == 0 {
                        0.0
                    } else {
                        *count as f32 / total as f32
                    };
                    card = card.child(
                        h_flex()
                            .gap_2()
                            .items_center()
                            .child(div().w(px(110.)).text_xs().child(name.clone()))
                            .child(
                                div()
                                    .flex_1()
                                    .h(px(8.))
                                    .rounded_full()
                                    .bg(cx.theme().muted)
                                    .child(
                                        div()
                                            .h_full()
                                            .rounded_full()
                                            .bg(cx.theme().primary)
                                            .w(relative(share)),
                                    ),
                            )
                            .child(div().w(px(40.)).text_xs().child(count.to_string())),
                    );
                }
                card
            };
        let mut urgent = v_flex().gap_1().child(section("REVIEW QUEUE", cx));
        if summary.urgent.is_empty() {
            urgent = urgent.child(hint("No urgent review signals in this scope.", cx));
        }
        for signal in &summary.urgent {
            urgent = urgent.child(
                h_flex()
                    .gap_2()
                    .child(chip(
                        SharedString::from(format!("urgent-{}", signal.kind)),
                        signal.kind,
                        true,
                    ))
                    .child(div().text_xs().child(format!(
                        "{} — {}",
                        signal.title,
                        signal.message()
                    ))),
            );
        }
        let stats = &summary.statistics;
        v_flex()
            .gap_3()
            .child(hint(
                format!(
                    "Scope: {}. See what is stored, what needs attention, and where it came from.",
                    if summary.project.is_empty() {
                        "current runtime"
                    } else {
                        &summary.project
                    }
                ),
                cx,
            ))
            .child(
                h_flex()
                    .gap_2()
                    .child(widgets::metric(
                        "Health",
                        summary.health_score.to_string(),
                        "Trust weighted",
                        summary.health_score < 70,
                        cx,
                    ))
                    .child(widgets::metric(
                        "Memories",
                        summary.memory_entries.to_string(),
                        "Indexed entries",
                        false,
                        cx,
                    ))
                    .child(widgets::metric(
                        "Conflicts",
                        summary.conflict_count.to_string(),
                        "Needs review",
                        summary.conflict_count > 0,
                        cx,
                    ))
                    .child(widgets::metric(
                        "Stale",
                        summary.stale_count.to_string(),
                        "Evidence drift",
                        summary.stale_count > 0,
                        cx,
                    ))
                    .child(widgets::metric(
                        "Pending reviews",
                        stats.pending_reviews.to_string(),
                        "Review Inbox",
                        stats.pending_reviews > 0,
                        cx,
                    )),
            )
            .child(
                h_flex()
                    .gap_2()
                    .items_start()
                    .child(distribution("Memory mix", &summary.memory_counts, cx))
                    .child(distribution("Trust states", &summary.trust_states, cx)),
            )
            .child(urgent)
            .child(section("STATISTICS", cx))
            .child(widgets::lines(
                &qt_memory::slash::statistics_lines(stats),
                false,
                cx,
            ))
    }

    fn render_detail(&self, cx: &mut Context<Self>) -> Div {
        let theme = cx.theme().clone();
        let Some(detail) = &self.detail else {
            return v_flex().gap_1().child(section("MEMORY DETAIL", cx)).child(hint(
                "Open a result in Explore or select a graph node to read its content and history.",
                cx,
            ));
        };
        let detail = match detail {
            Ok(detail) => detail,
            Err(error) => {
                return v_flex().child(
                    div()
                        .text_xs()
                        .text_color(theme.danger)
                        .child(error.clone()),
                );
            }
        };
        let entry = &detail.entry;
        let dream = data::requires_dream_review(entry);
        let mut rows = v_flex().gap_0p5();
        for (label, value) in data::memory_rows(entry) {
            rows = rows.child(widgets::row(label, value, cx));
        }
        let actions =
            h_flex()
                .gap_1()
                .flex_wrap()
                .child(
                    Button::new("dash-promote")
                        .label(if dream { "Review approval" } else { "Promote" })
                        .xsmall()
                        .primary()
                        .on_click(cx.listener(move |this, _, _, cx| {
                            if dream {
                                this.preview_dream(true, cx)
                            } else {
                                this.lifecycle(LifecycleAction::Promote, cx)
                            }
                        })),
                )
                .child(
                    Button::new("dash-validate")
                        .label("Validate")
                        .xsmall()
                        .disabled(dream)
                        .on_click(cx.listener(|this, _, _, cx| {
                            this.err(data::VALIDATE_REFUSAL);
                            cx.notify();
                        })),
                )
                .child(
                    Button::new("dash-deprecate")
                        .label("Deprecate")
                        .xsmall()
                        .disabled(dream)
                        .on_click(cx.listener(|this, _, _, cx| {
                            this.lifecycle(LifecycleAction::Deprecate, cx)
                        })),
                )
                .child(
                    Button::new("dash-reject")
                        .label(if dream { "Review rejection" } else { "Reject" })
                        .xsmall()
                        .on_click(cx.listener(move |this, _, _, cx| {
                            if dream {
                                this.preview_dream(false, cx)
                            } else {
                                this.lifecycle(LifecycleAction::Reject, cx)
                            }
                        })),
                )
                .child(
                    Button::new("dash-forget")
                        .label("Forget…")
                        .xsmall()
                        .danger()
                        .on_click(cx.listener(|this, _, w, cx| this.forget(w, cx))),
                );
        let history: Vec<String> = detail.revisions.iter().map(data::revision_line).collect();
        v_flex()
            .gap_2()
            .child(section("MEMORY DETAIL", cx))
            .child(actions)
            .when_some(
                self.dream_preview.as_ref(),
                |el, (review, action, preview)| {
                    let mut lines = vec![format!(
                        "Governed Dream review {} — {} preview{}",
                        review.id,
                        action.kind,
                        if preview.applicable {
                            ""
                        } else {
                            " (not applicable)"
                        }
                    )];
                    lines.extend(preview.will_do.iter().map(|v| format!("Will: {v}")));
                    lines.extend(preview.will_not_do.iter().map(|v| format!("Will not: {v}")));
                    lines.extend(preview.warnings.iter().map(|v| format!("Warning: {v}")));
                    lines.extend(preview.changes.iter().map(|c| {
                        format!("Change: {} {} {}", c.record_type, c.record_id, c.operation)
                    }));
                    let applicable = preview.applicable;
                    el.child(widgets::lines(&lines, false, cx)).child(
                        h_flex()
                            .gap_1()
                            .child(
                                Button::new("dash-dream-confirm")
                                    .label("Confirm review")
                                    .xsmall()
                                    .primary()
                                    .disabled(!applicable)
                                    .on_click(cx.listener(|this, _, _, cx| this.apply_dream(cx))),
                            )
                            .child(
                                Button::new("dash-dream-cancel")
                                    .label("Cancel")
                                    .xsmall()
                                    .ghost()
                                    .on_click(cx.listener(|this, _, _, cx| {
                                        this.dream_preview = None;
                                        cx.notify();
                                    })),
                            ),
                    )
                },
            )
            .child(
                div()
                    .p_2()
                    .rounded_md()
                    .bg(theme.secondary)
                    .text_sm()
                    .child(entry.content.clone()),
            )
            .child(rows)
            .child(section(
                format!("HISTORY ({} revisions)", detail.revisions.len()),
                cx,
            ))
            .child(if history.is_empty() {
                hint("No revisions recorded.", cx)
            } else {
                widgets::lines(&history, false, cx)
            })
    }

    fn render_explore(&self, cx: &mut Context<Self>) -> Div {
        let mut types = h_flex().gap_1().flex_wrap().child(hint("Type", cx)).child(
            chip("dash-type-all", "all", self.filter.memory_type.is_none()).on_click(cx.listener(
                |this, _, _, cx| {
                    this.filter.memory_type = None;
                    this.refresh_entries();
                    cx.notify();
                },
            )),
        );
        for (i, kind) in data::EXPLORER_TYPES.into_iter().enumerate() {
            types = types.child(
                chip(
                    ("dash-type", i),
                    kind.as_str(),
                    self.filter.memory_type == Some(kind),
                )
                .on_click(cx.listener(move |this, _, _, cx| {
                    this.filter.memory_type = Some(kind);
                    this.refresh_entries();
                    cx.notify();
                })),
            );
        }
        let mut statuses = h_flex()
            .gap_1()
            .flex_wrap()
            .child(hint("Status", cx))
            .child(
                chip("dash-status-all", "all", self.filter.status.is_none()).on_click(cx.listener(
                    |this, _, _, cx| {
                        this.filter.status = None;
                        this.refresh_entries();
                        cx.notify();
                    },
                )),
            );
        for (i, status) in data::EXPLORER_STATUSES.into_iter().enumerate() {
            statuses = statuses.child(
                chip(
                    ("dash-status", i),
                    status.as_str(),
                    self.filter.status == Some(status),
                )
                .on_click(cx.listener(move |this, _, _, cx| {
                    this.filter.status = Some(status);
                    this.refresh_entries();
                    cx.notify();
                })),
            );
        }
        let mut table = v_flex()
            .id("dash-memory-table")
            .gap_0p5()
            .max_h(px(470.))
            .overflow_y_scroll();
        if self.entries.is_empty() {
            table = table.child(hint(
                "No memories found. Try a different search or clear the filters. Use Capture to save your first memory.",
                cx,
            ));
        }
        for (index, entry) in self.entries.iter().enumerate() {
            let id = entry.id.clone();
            table = table.child(
                widgets::list_row(
                    ("dash-memory", index),
                    text_excerpt(&entry.content, 80),
                    format!(
                        "{} · {} · {} · confidence {:.2} · {} uses · {}",
                        entry.id,
                        entry.memory_type,
                        entry.status,
                        entry.confidence,
                        entry.access_count,
                        entry.tags.join(", ")
                    ),
                    self.selected.as_deref() == Some(entry.id.as_str()),
                    cx,
                )
                .on_click(cx.listener(move |this, _, _, cx| this.select_memory(id.clone(), cx))),
            );
        }
        h_flex()
            .gap_3()
            .items_start()
            .child(
                v_flex()
                    .flex_1()
                    .min_w_0()
                    .gap_2()
                    .child(
                        h_flex()
                            .gap_1()
                            .child(div().flex_1().child(Input::new(&self.search).small()))
                            .child(
                                Button::new("dash-search")
                                    .label("Search")
                                    .small()
                                    .primary()
                                    .on_click(cx.listener(|this, _, _, cx| this.search(cx))),
                            ),
                    )
                    .child(types)
                    .child(statuses)
                    .child(
                        Checkbox::new("dash-superseded")
                            .label("Include superseded")
                            .checked(self.filter.include_superseded)
                            .on_click(cx.listener(|this, checked: &bool, _, cx| {
                                this.filter.include_superseded = *checked;
                                this.refresh_entries();
                                cx.notify();
                            })),
                    )
                    .child(hint(
                        format!(
                            "{} results (limit {})",
                            self.entries.len(),
                            self.filter.limit
                        ),
                        cx,
                    ))
                    .child(table),
            )
            .child(
                div()
                    .id("dash-detail")
                    .w(px(420.))
                    .flex_none()
                    .max_h(px(620.))
                    .overflow_y_scroll()
                    .child(self.render_detail(cx)),
            )
    }

    fn render_graph(&self, cx: &mut Context<Self>) -> Div {
        let this = cx.entity().downgrade();
        let on_select: graph::SelectNode = Rc::new(move |id, _, cx| {
            let _ = this.update(cx, |this, cx| this.select_memory(id, cx));
        });
        let canvas = graph::render(
            &self.graph,
            &self.layout,
            self.view,
            self.selected.as_deref(),
            on_select,
            cx,
        );
        let interactive = div()
            .id("dash-graph")
            .on_scroll(cx.listener(|this, event: &ScrollEvent, _, cx| {
                let delta = event.delta.pixel_delta(px(20.)).y;
                let notches = f32::from(delta) / 20.0;
                this.view.zoom_by(notches * 0.08);
                cx.notify();
            }))
            .on_mouse_down(
                MouseButton::Right,
                cx.listener(|this, event: &MouseDownEvent, _, _| this.drag = Some(event.position)),
            )
            .on_mouse_up(
                MouseButton::Right,
                cx.listener(|this, _, _, _| this.drag = None),
            )
            .on_mouse_move(cx.listener(|this, event: &MouseMoveEvent, _, cx| {
                if let Some(last) = this.drag {
                    if event.pressed_button != Some(MouseButton::Right) {
                        this.drag = None;
                        return;
                    }
                    let delta = event.position - last;
                    this.view.pan_x += f32::from(delta.x);
                    this.view.pan_y += f32::from(delta.y);
                    this.drag = Some(event.position);
                    cx.notify();
                }
            }))
            .child(canvas);
        let mut edge_types: std::collections::BTreeMap<&str, usize> = Default::default();
        for edge in &self.graph.edges {
            *edge_types.entry(edge.edge_type.as_str()).or_insert(0) += 1;
        }
        let legend: Vec<String> = edge_types
            .iter()
            .map(|(kind, count)| format!("{kind}={count}"))
            .collect();
        h_flex()
            .gap_3()
            .items_start()
            .child(
                v_flex()
                    .w(px(graph::WIDTH))
                    .flex_none()
                    .gap_2()
                    .child(
                        h_flex()
                            .gap_1()
                            .child(div().w(px(260.)).child(Input::new(&self.edge_type).small()))
                            .child(
                                Button::new("dash-graph-load")
                                    .label("Load graph")
                                    .small()
                                    .primary()
                                    .on_click(cx.listener(|this, _, _, cx| {
                                        let edge_type = this.edge_type.read(cx).value().to_string();
                                        this.reload_graph(&edge_type);
                                        cx.notify();
                                    })),
                            )
                            .child(
                                Button::new("dash-graph-fit")
                                    .label("Fit")
                                    .small()
                                    .on_click(cx.listener(|this, _, _, cx| {
                                        this.view = GraphView::fit(&this.layout);
                                        cx.notify();
                                    })),
                            )
                            .child(
                                Button::new("dash-graph-zoom-in")
                                    .label("+")
                                    .small()
                                    .ghost()
                                    .on_click(cx.listener(|this, _, _, cx| {
                                        this.view.zoom_by(0.16);
                                        cx.notify();
                                    })),
                            )
                            .child(
                                Button::new("dash-graph-zoom-out")
                                    .label("−")
                                    .small()
                                    .ghost()
                                    .on_click(cx.listener(|this, _, _, cx| {
                                        this.view.zoom_by(-0.16);
                                        cx.notify();
                                    })),
                            ),
                    )
                    .child(hint(
                        format!(
                            "{} nodes · {} edges shown ({} returned) · edge types: {} · scroll to zoom, right-drag to pan, click a node for details",
                            self.graph.nodes.len(),
                            self.graph.edges.len(),
                            self.graph.total_edges,
                            if legend.is_empty() { "none".into() } else { legend.join(", ") }
                        ),
                        cx,
                    ))
                    .child(interactive),
            )
            .child(
                div()
                    .id("dash-graph-detail")
                    .flex_1()
                    .min_w(px(320.))
                    .max_h(px(620.))
                    .overflow_y_scroll()
                    .child(self.render_detail(cx)),
            )
    }

    fn render_sources(&self, cx: &mut Context<Self>) -> Div {
        let mut list = v_flex()
            .id("dash-sources")
            .gap_0p5()
            .w(px(420.))
            .flex_none()
            .max_h(px(600.))
            .overflow_y_scroll();
        if self.sources.is_empty() {
            list = list.child(hint("No sources recorded in this scope yet.", cx));
        }
        for (index, source) in self.sources.iter().enumerate() {
            let id = source.id.clone();
            list = list.child(
                widgets::list_row(
                    ("dash-source", index),
                    if source.display_name.is_empty() {
                        source.uri.clone()
                    } else {
                        source.display_name.clone()
                    },
                    format!(
                        "{} · trust {} · hash {} · {} ingests",
                        source.source_type,
                        source.trust_level,
                        source.hash_status,
                        source.ingest_count
                    ),
                    self.selected_source.as_deref() == Some(source.id.as_str()),
                    cx,
                )
                .on_click(cx.listener(move |this, _, _, cx| {
                    this.selected_source = Some(id.clone());
                    this.source_events = data::source_events(&this.memory, &id).unwrap_or_default();
                    cx.notify();
                })),
            );
        }
        let mut detail = v_flex().flex_1().min_w_0().gap_1();
        if let Some(source) = self
            .selected_source
            .as_ref()
            .and_then(|id| self.sources.iter().find(|s| &s.id == id))
        {
            detail = detail.child(section("SOURCE", cx));
            for (label, value) in data::source_rows(source) {
                detail = detail.child(widgets::row(label, value, cx));
            }
            let events: Vec<String> = self
                .source_events
                .iter()
                .map(data::source_event_line)
                .collect();
            detail = detail
                .child(section(format!("HISTORY ({})", events.len()), cx))
                .child(if events.is_empty() {
                    hint("No source events.", cx)
                } else {
                    widgets::lines(&events, false, cx)
                });
        }
        h_flex().gap_3().items_start().child(list).child(detail)
    }

    fn render_sessions(&self, cx: &mut Context<Self>) -> Div {
        let mut toggle = h_flex().gap_1();
        for (i, (agent, label)) in [(false, "Sessions"), (true, "Agents")]
            .into_iter()
            .enumerate()
        {
            toggle = toggle.child(
                chip(("dash-group-kind", i), label, self.by_agent == agent).on_click(cx.listener(
                    move |this, _, _, cx| {
                        this.by_agent = agent;
                        this.selected_group = None;
                        this.refresh_groups();
                        cx.notify();
                    },
                )),
            );
        }
        let mut list = v_flex().gap_0p5().w(px(360.)).flex_none();
        for (index, group) in self.groups.iter().enumerate() {
            let id = group.id.clone();
            list = list.child(
                widgets::list_row(
                    ("dash-group", index),
                    group.id.clone(),
                    format!(
                        "{} memories written · last active {}",
                        group.memories_written,
                        data::last_active_text(group.last_active)
                    ),
                    self.selected_group.as_deref() == Some(group.id.as_str()),
                    cx,
                )
                .on_click(cx.listener(move |this, _, _, cx| {
                    this.selected_group = Some(id.clone());
                    cx.notify();
                })),
            );
        }
        let members = self
            .selected_group
            .as_ref()
            .map(|id| data::group_members(&self.group_memories, self.by_agent, id))
            .unwrap_or_default();
        let mut detail = v_flex()
            .id("dash-group-members")
            .flex_1()
            .min_w_0()
            .gap_0p5()
            .max_h(px(560.))
            .overflow_y_scroll()
            .child(section(format!("MEMORIES ({})", members.len()), cx));
        for (index, memory) in members.iter().enumerate() {
            let id = memory.id.clone();
            detail = detail.child(
                widgets::list_row(
                    ("dash-group-memory", index),
                    text_excerpt(&memory.content, 90),
                    format!("{} · {} · {}", memory.id, memory.memory_type, memory.status),
                    false,
                    cx,
                )
                .on_click(cx.listener(move |this, _, _, cx| {
                    this.load_detail(&id);
                    this.tab = Tab::Explore;
                    cx.notify();
                })),
            );
        }
        v_flex()
            .gap_2()
            .child(toggle)
            .when(self.groups.is_empty(), |el| {
                el.child(hint("No memories in this scope yet.", cx))
            })
            .child(h_flex().gap_3().items_start().child(list).child(detail))
    }

    fn render_context(&self, cx: &mut Context<Self>) -> Div {
        let theme = cx.theme().clone();
        let mut policies = h_flex().gap_1().child(hint("Recall policy", cx));
        for (i, (value, label)) in POLICIES.into_iter().enumerate() {
            policies = policies.child(
                chip(("dash-policy", i), label, self.context_policy == value).on_click(
                    cx.listener(move |this, _, _, cx| {
                        this.context_policy = value;
                        cx.notify();
                    }),
                ),
            );
        }
        let mut body = v_flex()
            .gap_2()
            .child(hint(
                "Preview the context pack recall would inject for a query. Nothing is recorded as accessed.",
                cx,
            ))
            .child(
                h_flex()
                    .gap_1()
                    .child(div().flex_1().child(Input::new(&self.context_query).small()))
                    .child(div().w(px(160.)).child(Input::new(&self.context_budget).small()))
                    .child(
                        Button::new("dash-context-build")
                            .label("Build context")
                            .small()
                            .primary()
                            .on_click(cx.listener(|this, _, w, cx| this.build_context(w, cx))),
                    ),
            )
            .child(
                h_flex()
                    .gap_3()
                    .child(policies)
                    .child(
                        Checkbox::new("dash-context-global")
                            .label("Include global user memories")
                            .checked(self.context_global)
                            .on_click(cx.listener(|this, checked: &bool, _, cx| {
                                this.context_global = *checked;
                                cx.notify();
                            })),
                    ),
            );
        match &self.context_result {
            None => {}
            Some(Err(error)) => {
                body = body.child(
                    div()
                        .text_xs()
                        .text_color(theme.danger)
                        .child(error.clone()),
                )
            }
            Some(Ok(result)) => {
                let mut summary = vec![format!(
                    "Used {} of {} tokens · {} memories · {} blocks · {} code items · {} omitted · {} code candidates",
                    result.used_tokens,
                    result.budget_tokens,
                    result.memory_ids.len(),
                    result.block_ids.len(),
                    result.code_ids.len(),
                    result.omitted,
                    result.code_candidates
                )];
                summary.extend(result.warnings.iter().map(|w| format!("Warning: {w}")));
                body = body
                    .child(widgets::lines(&summary, false, cx))
                    .child(section("EXPLANATION", cx))
                    .child(if result.explanation.is_empty() {
                        hint("No items were eligible.", cx)
                    } else {
                        widgets::lines(&result.explanation, false, cx)
                    })
                    .child(section("RECALLED MEMORIES", cx))
                    .child(if result.memories.is_empty() {
                        hint("Nothing was recalled.", cx)
                    } else {
                        widgets::lines(&result.memories, false, cx)
                    })
                    .child(section("CODE CANDIDATES", cx))
                    .child(if result.code.is_empty() {
                        hint("No code was retrieved.", cx)
                    } else {
                        widgets::lines(&result.code, false, cx)
                    })
                    .child(section("CONTEXT MARKDOWN", cx))
                    .child(
                        Textarea::new(&self.context_markdown)
                            .readonly(true)
                            .h(px(260.))
                            .font_family(theme.mono_font_family.clone())
                            .text_xs(),
                    );
            }
        }
        body
    }

    fn render_capture(&self, cx: &mut Context<Self>) -> Div {
        let mut types = h_flex().gap_1().flex_wrap().child(hint("Type", cx));
        for (i, kind) in data::EXPLORER_TYPES.into_iter().enumerate() {
            types = types.child(
                chip(
                    ("dash-cap-type", i),
                    kind.as_str(),
                    self.capture_type == kind,
                )
                .on_click(cx.listener(move |this, _, _, cx| {
                    this.capture_type = kind;
                    cx.notify();
                })),
            );
        }
        let mut statuses = h_flex().gap_1().child(hint("Status", cx));
        for (i, status) in data::CAPTURE_STATUSES.into_iter().enumerate() {
            statuses = statuses.child(
                chip(
                    ("dash-cap-status", i),
                    status.as_str(),
                    self.capture_status == status,
                )
                .on_click(cx.listener(move |this, _, _, cx| {
                    this.capture_status = status;
                    cx.notify();
                })),
            );
        }
        v_flex()
            .gap_2()
            .child(section("STRUCTURED MEMORY", cx))
            .child(hint(
                "Save a decision, constraint, or observation with its source. New memories follow the current scope and are capped at candidate or hypothesis; promotion goes through review.",
                cx,
            ))
            .child(types)
            .child(
                h_flex()
                    .gap_3()
                    .child(statuses)
                    .child(hint("Confidence", cx))
                    .child(div().w(px(100.)).child(Input::new(&self.capture_confidence).small())),
            )
            .child(
                h_flex()
                    .gap_1()
                    .child(div().flex_1().child(Input::new(&self.capture_source_uri).small()))
                    .child(div().flex_1().child(Input::new(&self.capture_tags).small())),
            )
            .child(Textarea::new(&self.capture_content))
            .child(h_flex().child(
                Button::new("dash-write")
                    .label("Write memory")
                    .small()
                    .primary()
                    .on_click(cx.listener(|this, _, w, cx| this.write_memory(w, cx))),
            ))
            .child(section("RAW TEXT PROCESSING", cx))
            .child(hint(
                "Extracts signal-bearing lines as candidates (source dashboard:text, processed_text). Unchecking dry run stores them as candidate memories for review.",
                cx,
            ))
            .child(Textarea::new(&self.raw_text))
            .child(
                h_flex()
                    .gap_2()
                    .child(
                        Checkbox::new("dash-dry-run")
                            .label("Dry run")
                            .checked(self.raw_dry_run)
                            .on_click(cx.listener(|this, checked: &bool, _, cx| {
                                this.raw_dry_run = *checked;
                                cx.notify();
                            })),
                    )
                    .child(
                        Button::new("dash-process")
                            .label("Process raw text")
                            .small()
                            .on_click(cx.listener(|this, _, w, cx| this.process_text(w, cx))),
                    ),
            )
            .when(!self.raw_lines.is_empty(), |el| el.child(widgets::lines(&self.raw_lines, false, cx)))
    }

    fn render_operations(&self, cx: &mut Context<Self>) -> Div {
        v_flex()
            .gap_2()
            .child(
                h_flex()
                    .gap_1()
                    .child(
                        Button::new("dash-ops-refresh")
                            .label("Refresh")
                            .small()
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.refresh_operations();
                                cx.notify();
                            })),
                    )
                    .child(
                        Button::new("dash-rebuild")
                            .label("Rebuild retrieval…")
                            .small()
                            .on_click(cx.listener(|this, _, w, cx| this.rebuild_retrieval(w, cx))),
                    )
                    .child(
                        Button::new("dash-dream-run")
                            .label("Run Dreaming cycle")
                            .small()
                            .on_click(cx.listener(|this, _, _, cx| this.run_dream(cx))),
                    ),
            )
            .child(widgets::lines(&self.operation_lines, false, cx))
    }

    fn render_logs(&self, cx: &mut Context<Self>) -> Div {
        v_flex()
            .gap_2()
            .child(section("SYSTEM", cx))
            .child(widgets::lines(&data::system_lines(&self.memory), false, cx))
            .child(
                h_flex()
                    .gap_2()
                    .child(section(format!("AUDIT LOG ({})", self.log_lines.len()), cx))
                    .child(
                        Button::new("dash-logs-refresh")
                            .label("Refresh")
                            .xsmall()
                            .ghost()
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.refresh_logs();
                                cx.notify();
                            })),
                    ),
            )
            .child(if self.log_lines.is_empty() {
                hint("No audit events in this scope.", cx)
            } else {
                widgets::lines(&self.log_lines, false, cx)
            })
    }
}

impl Render for MemoryDashboard {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        let mut nav = v_flex()
            .w(px(210.))
            .flex_none()
            .h_full()
            .p_2()
            .gap_0p5()
            .bg(theme.sidebar)
            .border_r_1()
            .border_color(theme.sidebar_border)
            .child(
                div()
                    .px_2()
                    .py_1()
                    .text_sm()
                    .font_weight(FontWeight::SEMIBOLD)
                    .child("Memory dashboard"),
            )
            .child(hint(
                format!(
                    "Scope: {}",
                    qt_memory::views::review::scope_label(&self.memory.scope())
                ),
                cx,
            ));
        for (index, (tab, label, detail)) in TABS.into_iter().enumerate() {
            nav = nav.child(
                widgets::list_row(("dash-tab", index), label, detail, self.tab == tab, cx)
                    .on_click(cx.listener(move |this, _, _, cx| {
                        this.tab = tab;
                        cx.notify();
                    })),
            );
        }
        nav = nav.child(div().flex_1()).child(
            Button::new("dash-refresh-all")
                .label("Refresh all")
                .small()
                .on_click(cx.listener(|this, _, _, cx| {
                    this.refresh_all();
                    this.ok("Refreshed.");
                    cx.notify();
                })),
        );
        let content = match self.tab {
            Tab::Overview => self.render_overview(cx),
            Tab::Explore => self.render_explore(cx),
            Tab::Graph => self.render_graph(cx),
            Tab::Sources => self.render_sources(cx),
            Tab::Sessions => self.render_sessions(cx),
            Tab::Context => self.render_context(cx),
            Tab::Capture => self.render_capture(cx),
            Tab::Operations => self.render_operations(cx),
            Tab::Logs => self.render_logs(cx),
        };
        let title = TABS
            .iter()
            .find(|(tab, ..)| *tab == self.tab)
            .map(|(_, label, _)| *label)
            .unwrap_or_default();
        h_flex()
            .size_full()
            .bg(theme.background)
            .text_color(theme.foreground)
            .child(nav)
            .child(
                v_flex()
                    .id("dash-content")
                    .flex_1()
                    .min_w_0()
                    .h_full()
                    .p_4()
                    .gap_3()
                    .overflow_y_scroll()
                    .child(
                        div()
                            .text_lg()
                            .font_weight(FontWeight::SEMIBOLD)
                            .child(title),
                    )
                    .when_some(self.busy.clone(), |el, label| {
                        el.child(
                            h_flex()
                                .gap_2()
                                .items_center()
                                .text_xs()
                                .child(Spinner::new().small())
                                .child(label),
                        )
                    })
                    .when_some(self.banner.clone(), |el, (message, error)| {
                        el.child(
                            h_flex()
                                .p_2()
                                .gap_2()
                                .rounded_md()
                                .bg(if error {
                                    theme.danger.opacity(0.12)
                                } else {
                                    theme.success.opacity(0.12)
                                })
                                .child(div().flex_1().text_xs().child(message))
                                .child(
                                    Button::new("dash-banner-close")
                                        .label("Dismiss")
                                        .xsmall()
                                        .ghost()
                                        .on_click(cx.listener(|this, _, _, cx| {
                                            this.banner = None;
                                            cx.notify();
                                        })),
                                ),
                        )
                    })
                    .child(content),
            )
    }
}

struct DashboardWindow(AnyWindowHandle);

impl Global for DashboardWindow {}

/// Opens the dashboard window, or brings the open one to the front.
pub fn open(host: MemoryHost, cx: &mut App) -> Result<()> {
    if let Some(DashboardWindow(handle)) = cx
        .try_global::<DashboardWindow>()
        .map(|g| DashboardWindow(g.0))
        && handle
            .update(cx, |_, window, _| window.activate_window())
            .is_ok()
    {
        return Ok(());
    }
    let Some(memory) = host.memory() else {
        anyhow::bail!("built-in memory is inactive");
    };
    let project = host
        .runtime
        .engine()
        .cwd()
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default();
    let bounds = Bounds::centered(None, size(px(1320.), px(860.)), cx);
    let options = WindowOptions {
        window_bounds: Some(WindowBounds::Windowed(bounds)),
        titlebar: Some(TitlebarOptions {
            title: Some(format!("qt memory — {project}").into()),
            ..Default::default()
        }),
        window_min_size: Some(size(px(900.), px(600.))),
        app_id: Some("qt".into()),
        ..Default::default()
    };
    let title = format!("qt memory — {project}");
    let (handle, _) = gpui_kit::open_window(options, cx, move |window, cx| {
        crate::window_frame::framed(title, window, cx, |window, cx| {
            cx.new(|cx| MemoryDashboard::new(host, memory, window, cx))
        })
    })?;
    cx.set_global(DashboardWindow(handle));
    Ok(())
}
