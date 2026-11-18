//! `/index` panel: project index status and rescans, symbol search, file
//! analysis, references, full-text search, build context, router tree and
//! arbitrary router operations, on top of `qt-index` through the runtime.
//!
//! [`PanelHooks`] and [`output_view`] are shared by the subsystem panels
//! (`/index`, `/mcp`, `/llama`, `/subagent`).

use std::rc::Rc;
use std::time::{Duration, SystemTime};

use gpui_kit::component::button::{Button, ButtonVariants as _};
use gpui_kit::component::checkbox::Checkbox;
use gpui_kit::component::input::{Input, InputState};
use gpui_kit::component::spinner::Spinner;
use gpui_kit::component::{
    ActiveTheme as _, Disableable as _, Sizable as _, WindowExt as _, h_flex, v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::*;
use qt_core::provider::ModelRef;
use qt_runtime::Runtime;
use qt_runtime::index_commands::{
    INDEX_CATEGORIES, call_request, file_arguments, index_result_lines, parse_json_object,
    references_arguments, search_arguments, symbol_arguments, tree_request,
};
use qt_runtime::qt_index::service::ServiceStatus;
use serde_json::Value;

/// Shows lines as a transcript card: `(title, lines, is_error)`.
pub type CardHook = Rc<dyn Fn(String, Vec<String>, bool, &mut Window, &mut App)>;
/// Receives a value on the UI thread.
pub type ValueHook<T> = Rc<dyn Fn(T, &mut Window, &mut App)>;

/// What panels may ask of the workspace.
#[derive(Clone)]
pub struct PanelHooks {
    pub card: CardHook,
    /// Put text into the composer (cursor at the end).
    pub prefill: ValueHook<String>,
    /// Model lists changed: refresh the model picker.
    pub models_changed: crate::dialogs::UiCallback,
    /// Use this model for the conversation.
    pub choose_model: ValueHook<ModelRef>,
}

/// Lines shown at most in an output view (the rest is summarised).
pub const MAX_OUTPUT_LINES: usize = 600;

/// The trimmed output of a panel operation.
pub fn visible_lines(lines: &[String]) -> Vec<String> {
    if lines.len() <= MAX_OUTPUT_LINES {
        return lines.to_vec();
    }
    let mut out = lines[..MAX_OUTPUT_LINES].to_vec();
    out.push(format!("… {} more lines", lines.len() - MAX_OUTPUT_LINES));
    out
}

/// How an [`output_view`] takes vertical space in a panel whose root is
/// `size_full()`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum OutputSize {
    /// The output is the panel's main area: it fills the free space (at
    /// least [`OUTPUT_MIN_HEIGHT`]).
    Fill,
    /// The output sits under the panel's main area: as tall as its content,
    /// at most this share of the panel height.
    Share(f32),
}

/// Smallest height of a filling output view.
pub const OUTPUT_MIN_HEIGHT: f32 = 120.;

/// A scrollable monospace output area with an optional busy row.
pub fn output_view(
    id: &'static str,
    lines: &[String],
    error: bool,
    busy: Option<&str>,
    size: OutputSize,
    cx: &App,
) -> AnyElement {
    let theme = cx.theme();
    if lines.is_empty() && busy.is_none() {
        return div().into_any_element();
    }
    v_flex()
        .id(id)
        .w_full()
        .gap_0p5()
        .p_2()
        .rounded_md()
        .border_1()
        .border_color(theme.border)
        .map(|el| match size {
            OutputSize::Fill => el.flex_1().min_h(px(OUTPUT_MIN_HEIGHT)),
            OutputSize::Share(share) => el.flex_shrink_1().min_h_0().max_h(relative(share)),
        })
        .overflow_y_scroll()
        .when_some(busy.map(str::to_string), |el, label| {
            el.child(
                h_flex()
                    .gap_2()
                    .items_center()
                    .text_xs()
                    .child(Spinner::new().small())
                    .child(label),
            )
        })
        .children(visible_lines(lines).into_iter().map(|line| {
            div()
                .text_xs()
                .font_family(theme.mono_font_family.clone())
                .text_color(if error {
                    theme.danger
                } else {
                    theme.foreground
                })
                .child(SharedString::from(line))
        }))
        .into_any_element()
}

/// A section heading.
pub fn section(title: &'static str, cx: &App) -> Div {
    div()
        .text_xs()
        .font_weight(FontWeight::SEMIBOLD)
        .text_color(cx.theme().muted_foreground)
        .child(title)
}

/// Unix seconds as `N s ago` / `N min ago`.
pub fn ago(time: Option<SystemTime>, now: SystemTime) -> String {
    let Some(time) = time else {
        return "never".into();
    };
    let secs = now.duration_since(time).unwrap_or(Duration::ZERO).as_secs();
    match secs {
        0..=59 => format!("{secs} s ago"),
        60..=3599 => format!("{} min ago", secs / 60),
        _ => format!("{} h ago", secs / 3600),
    }
}

/// Summary rows of an index status.
pub fn status_rows(status: &ServiceStatus, now: SystemTime) -> Vec<(&'static str, String)> {
    let scan = if status.scan_active {
        format!(
            "scanning · {} · {}%",
            if status.scan_phase.is_empty() {
                "starting"
            } else {
                &status.scan_phase
            },
            status.scan_progress_percent
        )
    } else if status.generation == 0 {
        "not scanned yet".into()
    } else {
        format!("idle · last scan {}", ago(status.last_scan_finished, now))
    };
    let mut rows = vec![
        (
            "service",
            if status.running { "running" } else { "stopped" }.to_string(),
        ),
        ("scan", scan),
        ("generation", status.generation.to_string()),
        ("files", status.files.to_string()),
        ("compile commands", status.compile_commands.to_string()),
        (
            "full-text search",
            if status.sqlite_fts_available {
                "available"
            } else {
                "unavailable"
            }
            .to_string(),
        ),
    ];
    if !status.build_metadata_summary.is_empty() {
        rows.push(("build", status.build_metadata_summary.clone()));
    }
    if status.index_stale {
        rows.push((
            "snapshot",
            "stale (loaded from disk, rescan pending)".into(),
        ));
    }
    if !status.last_error.is_empty() {
        rows.push(("last error", status.last_error.clone()));
    }
    rows.push(("database", status.sqlite_database.display().to_string()));
    rows
}

/// Query modes of the panel.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum QueryMode {
    Symbol,
    Analyze,
    References,
    Search,
    BuildContext,
    Tree,
    Call,
}

impl QueryMode {
    pub const ALL: [QueryMode; 7] = [
        Self::Symbol,
        Self::Analyze,
        Self::References,
        Self::Search,
        Self::BuildContext,
        Self::Tree,
        Self::Call,
    ];

    pub fn label(self) -> &'static str {
        match self {
            Self::Symbol => "Symbol",
            Self::Analyze => "Analyze file",
            Self::References => "References",
            Self::Search => "Search",
            Self::BuildContext => "Build context",
            Self::Tree => "Operations",
            Self::Call => "Call",
        }
    }

    /// Placeholders of the primary and secondary fields.
    pub fn placeholders(self) -> (&'static str, Option<&'static str>) {
        match self {
            Self::Symbol => (
                "C/C++ identifier, e.g. ToolRegistry::prepare",
                Some("File filter (optional)"),
            ),
            Self::Analyze | Self::BuildContext => {
                ("Project-relative file, e.g. src/main.cpp", None)
            }
            Self::References => ("Function or method name", None),
            Self::Search => ("Search terms", None),
            Self::Tree => ("Category (core, code, fs, git, build) or empty", None),
            Self::Call => (
                "Operation, e.g. fs.read",
                Some("JSON arguments, e.g. {\"file\":\"README.md\"}"),
            ),
        }
    }

    /// `(tool, arguments, label)` for the fields, or the problem.
    pub fn request(
        self,
        primary: &str,
        secondary: &str,
        flag: bool,
        direction: &str,
    ) -> Result<(&'static str, Value, String), String> {
        let primary_trimmed = primary.trim();
        let missing = || format!("{} needs a value.", self.label());
        match self {
            Self::Symbol => symbol_arguments(primary, secondary, "", flag)
                .map(|a| ("symbol", a, format!("symbol {primary_trimmed}")))
                .ok_or_else(missing),
            Self::Analyze => file_arguments(primary)
                .map(|a| ("analyze_file", a, format!("analyze_file {primary_trimmed}")))
                .ok_or_else(missing),
            Self::BuildContext => file_arguments(primary)
                .map(|a| {
                    (
                        "build_context",
                        a,
                        format!("build_context {primary_trimmed}"),
                    )
                })
                .ok_or_else(missing),
            Self::References => references_arguments(primary, direction, None)
                .map(|a| {
                    (
                        "references",
                        a,
                        format!("references {primary_trimmed} ({direction})"),
                    )
                })
                .ok_or_else(missing),
            Self::Search => search_arguments(primary, flag)
                .map(|a| ("search_index", a, format!("search_index {primary_trimmed}")))
                .ok_or_else(missing),
            Self::Tree => {
                let category = primary_trimmed.to_ascii_lowercase();
                if !category.is_empty() && !INDEX_CATEGORIES.iter().any(|(c, _)| *c == category) {
                    return Err(format!("unknown index category: {category}"));
                }
                let label = if category.is_empty() {
                    "index tree".to_string()
                } else {
                    format!("index tree: {category}")
                };
                Ok(("index", tree_request(Some(&category), flag), label))
            }
            Self::Call => {
                if primary_trimmed.is_empty() {
                    return Err(missing());
                }
                let args = parse_json_object(secondary)
                    .ok_or_else(|| "index arguments must be a JSON object.".to_string())?;
                Ok((
                    "index",
                    call_request(primary_trimmed, args),
                    format!("index call: {primary_trimmed}"),
                ))
            }
        }
    }

    /// Label of the checkbox, when the mode has one.
    pub fn flag_label(self) -> Option<&'static str> {
        match self {
            Self::Symbol => Some("Include source context"),
            Self::Search => Some("FTS5 syntax"),
            Self::Tree => Some("Include schemas"),
            _ => None,
        }
    }
}

pub struct IndexPanel {
    runtime: Runtime,
    hooks: PanelHooks,
    status: ServiceStatus,
    mode: QueryMode,
    primary: Entity<InputState>,
    secondary: Entity<InputState>,
    flag: bool,
    direction: &'static str,
    title: String,
    output: Vec<String>,
    error: bool,
    busy: Option<String>,
    _task: Option<Task<()>>,
    _refresh: Task<()>,
}

impl IndexPanel {
    fn new(
        runtime: Runtime,
        hooks: PanelHooks,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let (p, s) = QueryMode::Symbol.placeholders();
        let primary = cx.new(|cx| InputState::new(window, cx).placeholder(p));
        let secondary = cx.new(|cx| InputState::new(window, cx).placeholder(s.unwrap_or("")));
        primary.update(cx, |s, cx| s.focus(window, cx));
        // Status refreshes while the panel is open (scans report progress).
        let refresh = cx.spawn(async move |this, cx| {
            loop {
                cx.background_executor().timer(Duration::from_secs(1)).await;
                let alive = this
                    .update(cx, |this, cx| {
                        let next = this.runtime.index_service().status();
                        if next != this.status {
                            this.status = next;
                            cx.notify();
                        }
                    })
                    .is_ok();
                if !alive {
                    break;
                }
            }
        });
        Self {
            status: runtime.index_service().status(),
            runtime,
            hooks,
            mode: QueryMode::Symbol,
            primary,
            secondary,
            flag: false,
            direction: "both",
            title: String::new(),
            output: Vec::new(),
            error: false,
            busy: None,
            _task: None,
            _refresh: refresh,
        }
    }

    fn set_mode(&mut self, mode: QueryMode, window: &mut Window, cx: &mut Context<Self>) {
        self.mode = mode;
        self.flag = false;
        let (p, s) = mode.placeholders();
        self.primary.update(cx, |state, cx| {
            state.set_placeholder(p, window, cx);
            state.focus(window, cx);
        });
        self.secondary.update(cx, |state, cx| {
            state.set_placeholder(s.unwrap_or(""), window, cx)
        });
        cx.notify();
    }

    fn invoke(
        &mut self,
        tool: &'static str,
        arguments: Value,
        label: String,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.busy.is_some() {
            return;
        }
        self.busy = Some(format!("{label}…"));
        let runtime = self.runtime.clone();
        let handle = self
            .runtime
            .engine()
            .spawn(async move { runtime.run_index_tool(tool, arguments).await });
        self._task = Some(cx.spawn_in(window, async move |this, cx| {
            let result = handle.await;
            let _ = this.update(cx, |this, cx| {
                this.busy = None;
                match result {
                    Ok(output) => {
                        this.error = output.is_error;
                        this.output = index_result_lines(&label, &output);
                    }
                    Err(e) => {
                        this.error = true;
                        this.output = vec![format!("{label} failed: {e}")];
                    }
                }
                this.title = label;
                this.status = this.runtime.index_service().status();
                cx.notify();
            });
        }));
        cx.notify();
    }

    fn run_query(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let primary = self.primary.read(cx).value().to_string();
        let secondary = self.secondary.read(cx).value().to_string();
        match self
            .mode
            .request(&primary, &secondary, self.flag, self.direction)
        {
            Ok((tool, args, label)) => self.invoke(tool, args, label, window, cx),
            Err(e) => {
                self.error = true;
                self.output = vec![e];
                cx.notify();
            }
        }
    }

    fn rescan(&mut self, cx: &mut Context<Self>) {
        self.runtime.index_service().request_scan();
        self.error = false;
        self.title = "index rescan".into();
        self.output = vec!["Project index scan requested.".into()];
        self.status = self.runtime.index_service().status();
        cx.notify();
    }
}

impl Render for IndexPanel {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        let busy = self.busy.is_some();
        let now = SystemTime::now();
        let mut status = v_flex().gap_0p5().text_xs();
        status = status.child(
            div().text_sm().font_weight(FontWeight::SEMIBOLD).child(
                self.runtime
                    .index_service()
                    .project_root()
                    .display()
                    .to_string(),
            ),
        );
        for (label, value) in status_rows(&self.status, now) {
            status = status.child(
                h_flex()
                    .gap_2()
                    .child(
                        div()
                            .w(px(120.))
                            .flex_none()
                            .text_color(theme.muted_foreground)
                            .child(label),
                    )
                    .child(div().flex_1().min_w_0().child(value)),
            );
        }
        let actions = h_flex()
            .gap_1()
            .flex_wrap()
            .child(
                Button::new("index-rescan")
                    .label("Rescan")
                    .small()
                    .disabled(self.status.scan_active)
                    .on_click(cx.listener(|this, _, _, cx| this.rescan(cx))),
            )
            .child(
                Button::new("index-status-json")
                    .label("Status JSON")
                    .small()
                    .ghost()
                    .disabled(busy)
                    .on_click(cx.listener(|this, _, w, cx| {
                        this.invoke(
                            "index",
                            call_request(
                                "status",
                                serde_json::json!({"include_build_metadata": true, "include_tree_sitter": true}),
                            ),
                            "index status".into(),
                            w,
                            cx,
                        )
                    })),
            )
            .child(
                Button::new("index-summary")
                    .label("Project summary")
                    .small()
                    .ghost()
                    .disabled(busy)
                    .on_click(cx.listener(|this, _, w, cx| {
                        this.invoke(
                            "index",
                            call_request("project_summary", serde_json::json!({})),
                            "index summary".into(),
                            w,
                            cx,
                        )
                    })),
            );

        let mut modes = h_flex().gap_1().flex_wrap();
        for mode in QueryMode::ALL {
            let selected = mode == self.mode;
            modes = modes.child(
                Button::new(SharedString::from(format!("index-mode-{}", mode.label())))
                    .label(mode.label())
                    .xsmall()
                    .when(selected, |b| b.primary())
                    .when(!selected, |b| b.ghost())
                    .on_click(cx.listener(move |this, _, w, cx| this.set_mode(mode, w, cx))),
            );
        }
        let (_, secondary) = self.mode.placeholders();
        let mut form = v_flex()
            .gap_1()
            .child(Input::new(&self.primary).small())
            .when(secondary.is_some(), |el| {
                el.child(Input::new(&self.secondary).small())
            });
        if self.mode == QueryMode::References {
            let mut dirs = h_flex()
                .gap_1()
                .items_center()
                .text_xs()
                .child("Direction:");
            for direction in ["both", "inbound", "outbound"] {
                let selected = self.direction == direction;
                dirs = dirs.child(
                    Button::new(SharedString::from(format!("index-dir-{direction}")))
                        .label(direction)
                        .xsmall()
                        .when(selected, |b| b.primary())
                        .when(!selected, |b| b.ghost())
                        .on_click(cx.listener(move |this, _, _, cx| {
                            this.direction = direction;
                            cx.notify();
                        })),
                );
            }
            form = form.child(dirs);
        }
        let mut run_row = h_flex().gap_2().items_center();
        if let Some(label) = self.mode.flag_label() {
            run_row = run_row.child(
                Checkbox::new("index-flag")
                    .label(label)
                    .checked(self.flag)
                    .on_click(cx.listener(|this, checked: &bool, _, cx| {
                        this.flag = *checked;
                        cx.notify();
                    })),
            );
        }
        run_row = run_row
            .child(div().flex_1())
            .when(!self.output.is_empty(), |el| {
                el.child(
                    Button::new("index-to-transcript")
                        .label("Copy to transcript")
                        .small()
                        .ghost()
                        .on_click(cx.listener(|this, _, w, cx| {
                            (this.hooks.card)(
                                this.title.clone(),
                                this.output.clone(),
                                this.error,
                                w,
                                cx,
                            )
                        })),
                )
            })
            .child(
                Button::new("index-run")
                    .label("Run")
                    .small()
                    .primary()
                    .disabled(busy)
                    .on_click(cx.listener(|this, _, w, cx| this.run_query(w, cx))),
            );

        // The status and query form keep their height and scroll when the
        // window is short; the output fills the rest.
        let top = v_flex()
            .id("index-form")
            .flex_shrink_1()
            .min_h_0()
            .overflow_y_scroll()
            .gap_3()
            .child(section("STATUS", cx))
            .child(status)
            .child(actions)
            .child(section("QUERY", cx))
            .child(modes)
            .child(form)
            .child(run_row);
        v_flex().size_full().gap_3().child(top).child(output_view(
            "index-output",
            &self.output,
            self.error,
            self.busy.as_deref(),
            OutputSize::Fill,
            cx,
        ))
    }
}

/// Open the `/index` panel.
pub fn open(runtime: Runtime, hooks: PanelHooks, window: &mut Window, cx: &mut App) {
    let panel = cx.new(|cx| IndexPanel::new(runtime, hooks, window, cx));
    window.open_dialog(cx, move |dialog, window, _| {
        crate::layout::fill_panel(dialog.title("Project index"), window, 820., panel.clone())
            .on_ok(|_, _, _| false)
    });
}

#[cfg(test)]
mod tests {
    use super::{QueryMode, ago, status_rows, visible_lines};
    use qt_runtime::qt_index::service::ServiceStatus;
    use serde_json::json;
    use std::time::{Duration, SystemTime};

    #[test]
    fn requests_from_fields() {
        let (tool, args, label) = QueryMode::Symbol
            .request("Foo", "src/a.cpp", true, "both")
            .unwrap();
        assert_eq!(tool, "symbol");
        assert_eq!(
            args,
            json!({"name": "Foo", "file": "src/a.cpp", "context": true})
        );
        assert_eq!(label, "symbol Foo");
        assert_eq!(
            QueryMode::References
                .request("f", "", false, "inbound")
                .unwrap()
                .1,
            json!({"symbol": "f", "direction": "inbound"})
        );
        assert_eq!(
            QueryMode::Search
                .request("a b", "", true, "both")
                .unwrap()
                .1,
            json!({"query": "a b", "syntax": "fts5"})
        );
        assert_eq!(
            QueryMode::Tree.request("Code", "", true, "both").unwrap().1,
            json!({"action": "tree", "include_schemas": true, "category": "code"})
        );
        assert_eq!(
            QueryMode::Tree
                .request("serial", "", false, "both")
                .unwrap_err(),
            "unknown index category: serial"
        );
        assert_eq!(
            QueryMode::Call
                .request("fs.read", "{\"file\":\"x\"}", false, "both")
                .unwrap()
                .1,
            json!({"action": "call", "operation": "fs.read", "arguments": {"file": "x"}})
        );
        assert_eq!(
            QueryMode::Call
                .request("fs.read", "[]", false, "both")
                .unwrap_err(),
            "index arguments must be a JSON object."
        );
        assert_eq!(
            QueryMode::Analyze
                .request(" ", "", false, "both")
                .unwrap_err(),
            "Analyze file needs a value."
        );
    }

    #[test]
    fn status_and_output_helpers() {
        let now = SystemTime::now();
        assert_eq!(ago(None, now), "never");
        assert_eq!(ago(Some(now - Duration::from_secs(5)), now), "5 s ago");
        assert_eq!(ago(Some(now - Duration::from_secs(180)), now), "3 min ago");
        let status = ServiceStatus {
            running: true,
            scan_active: true,
            scan_phase: "files".into(),
            scan_progress_percent: 40,
            last_error: "boom".into(),
            ..Default::default()
        };
        let rows = status_rows(&status, now);
        assert_eq!(rows[0], ("service", "running".to_string()));
        assert_eq!(rows[1], ("scan", "scanning · files · 40%".to_string()));
        assert!(rows.iter().any(|(k, v)| *k == "last error" && v == "boom"));
        let lines: Vec<String> = (0..700).map(|i| i.to_string()).collect();
        let shown = visible_lines(&lines);
        assert_eq!(shown.len(), 601);
        assert_eq!(shown.last().unwrap(), "… 100 more lines");
    }
}
