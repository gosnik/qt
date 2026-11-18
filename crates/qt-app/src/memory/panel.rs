//! The `/memory` panel: the archived operation menu (status, settings,
//! dashboard, scan skills, dream, recall, remember, review, feedback,
//! forget) with a form per operation. Operations run the `/memory`
//! subcommands; their output shows here and in the transcript.

use std::path::{Path, PathBuf};

use gpui_kit::component::button::{Button, ButtonVariants as _};
use gpui_kit::component::input::{Input, InputState, Textarea, TextareaState};
use gpui_kit::component::spinner::Spinner;
use gpui_kit::component::{
    ActiveTheme as _, Disableable as _, Sizable as _, WindowExt as _, h_flex, v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::*;
use qt_memory::slash::{
    SlashMemoryResponse, dream_lines, memory_slash_command, resolve_skill_folder, status_lines,
};

use super::widgets::{self, chip, hint, section};
use super::{MemoryHost, SkillScan, disabled_line, skill_scan, start_skill_scan};

/// Panel sections (the archived menu entries that take input here).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Section {
    Status,
    Recall,
    Remember,
    Feedback,
    Forget,
    ScanSkills,
    Dream,
}

/// Archived menu: `(value, label, description)`.
const MENU: [(&str, &str, &str); 10] = [
    (
        "status",
        "status",
        "Show configuration, scope, pre-handler state, and statistics",
    ),
    (
        "settings",
        "settings",
        "Configure built-in memory for the next runtime",
    ),
    (
        "dashboard",
        "dashboard",
        "Open the scoped memory dashboard window",
    ),
    (
        "scan-skills",
        "scan skills folder",
        "Import SKILL.md packages as skill memories in the background",
    ),
    ("dream", "dream", "Run or inspect governed native Dreaming"),
    ("recall", "recall", "Recall governed memory for a query"),
    ("remember", "remember", "Create a reviewable hypothesis"),
    (
        "review",
        "review",
        "Open the scope-aware Memory Review Inbox",
    ),
    ("feedback", "feedback", "Record a procedural-memory outcome"),
    ("forget", "forget", "Request deletion of an in-scope memory"),
];

const OUTCOMES: [&str; 4] = ["success", "failure", "partial", "unused"];

pub struct MemoryPanel {
    host: MemoryHost,
    section: Section,
    output: Vec<String>,
    output_error: bool,
    busy: Option<String>,
    query: Entity<InputState>,
    content: Entity<TextareaState>,
    memory_id: Entity<InputState>,
    outcome: &'static str,
    research: Entity<TextareaState>,
    folder: PathBuf,
    folder_input: Entity<InputState>,
    folder_error: Option<String>,
    scan: Entity<SkillScan>,
    _scan_watch: Subscription,
    _task: Option<Task<()>>,
}

impl MemoryPanel {
    fn new(
        host: MemoryHost,
        section: Section,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let input = |placeholder: &'static str, window: &mut Window, cx: &mut Context<Self>| {
            cx.new(|cx| InputState::new(window, cx).placeholder(placeholder))
        };
        let context = host.slash_context();
        let folder = std::fs::canonicalize(&context.cwd).unwrap_or(context.cwd);
        let scan = skill_scan(cx);
        let watch = cx.observe(&scan, |_, _, cx| cx.notify());
        let folder_input = input("Folder to scan (absolute or relative)", window, cx);
        folder_input.update(cx, |s, cx| {
            s.set_value(folder.display().to_string(), window, cx)
        });
        let mut this = Self {
            query: input("What to recall, e.g. build commands", window, cx),
            content: cx.new(|cx| {
                TextareaState::new(window, cx)
                    .auto_grow(3, 10)
                    .placeholder("Content of the hypothesis to remember")
            }),
            memory_id: input("Memory ID, e.g. mem_…", window, cx),
            research: cx.new(|cx| {
                TextareaState::new(window, cx).auto_grow(3, 8).placeholder(
                    "{\"dream_id\":\"...\",\"summary\":\"...\",\"source_uri\":\"...\"}",
                )
            }),
            outcome: OUTCOMES[0],
            folder,
            folder_input,
            folder_error: None,
            scan,
            _scan_watch: watch,
            host,
            section,
            output: Vec::new(),
            output_error: false,
            busy: None,
            _task: None,
        };
        this.select(section, window, cx);
        this
    }

    fn select(&mut self, section: Section, _window: &mut Window, cx: &mut Context<Self>) {
        self.section = section;
        self.output_error = false;
        self.output = match section {
            Section::Status => status_lines(&self.host.slash_context()),
            Section::Dream => match self.host.memory() {
                Some(memory) => dream_lines(&memory, "status"),
                None => vec![disabled_line("using Dreaming")],
            },
            _ => Vec::new(),
        };
        cx.notify();
    }

    fn menu(&mut self, value: &str, window: &mut Window, cx: &mut Context<Self>) {
        let section = match value {
            "settings" => {
                super::settings::open(self.host.clone(), window, cx);
                return;
            }
            "dashboard" => {
                super::open_dashboard(&self.host, window, cx);
                return;
            }
            "review" => {
                super::open_inbox(&self.host, window, cx);
                return;
            }
            "scan-skills" => Section::ScanSkills,
            "dream" => Section::Dream,
            "recall" => Section::Recall,
            "remember" => Section::Remember,
            "feedback" => Section::Feedback,
            "forget" => Section::Forget,
            _ => Section::Status,
        };
        self.select(section, window, cx);
    }

    /// Runs `/memory <arguments>`; output goes to the panel and transcript.
    fn run(&mut self, arguments: String, window: &mut Window, cx: &mut Context<Self>) {
        if self.busy.is_some() {
            return;
        }
        let line = format!("/memory {arguments}");
        self.busy = Some(format!("Running {line}…"));
        let context = self.host.slash_context();
        let handle = self
            .host
            .runtime
            .engine()
            .spawn(async move { memory_slash_command(&context, &arguments).await });
        self._task = Some(cx.spawn_in(window, async move |this, cx| {
            let response = handle.await.unwrap_or_else(|error| SlashMemoryResponse {
                lines: vec![format!("/memory failed: {error}")],
                action: None,
            });
            let _ = this.update_in(cx, |this, window, cx| {
                this.busy = None;
                let error = response
                    .lines
                    .iter()
                    .any(|l| l.contains(" failed") || l.starts_with("usage:"));
                this.output = response.lines.clone();
                this.output_error = error;
                this.host.card(line, response.lines, error, cx);
                if let Some(action) = response.action {
                    super::perform_action(&this.host, action, window, cx);
                }
                if this.section == Section::Dream
                    && let Some(memory) = this.host.memory()
                {
                    this.output.push(String::new());
                    this.output.extend(dream_lines(&memory, "status"));
                }
                cx.notify();
            });
        }));
        cx.notify();
    }

    fn recall(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let query = self.query.read(cx).value().trim().to_string();
        if query.is_empty() {
            self.show_error("usage: /memory recall <query>", cx);
            return;
        }
        self.run(format!("recall {query}"), window, cx);
    }

    fn remember(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let content = self.content.read(cx).value().trim().to_string();
        if content.is_empty() {
            self.show_error("usage: /memory remember <content>", cx);
            return;
        }
        self.run(format!("remember {content}"), window, cx);
        self.content.update(cx, |s, cx| s.set_value("", window, cx));
    }

    fn id(&self, cx: &App) -> Option<String> {
        let id = self.memory_id.read(cx).value().trim().to_string();
        (!id.is_empty() && !id.contains(char::is_whitespace)).then_some(id)
    }

    fn show_error(&mut self, message: &str, cx: &mut Context<Self>) {
        self.output = vec![message.to_string()];
        self.output_error = true;
        cx.notify();
    }

    /// Feedback and forget mutate memory: both ask first (the archived
    /// commands required a trailing `confirm`).
    fn confirm(&mut self, forget: bool, window: &mut Window, cx: &mut Context<Self>) {
        let Some(id) = self.id(cx) else {
            self.show_error(
                if forget {
                    "usage: /memory forget <memory-id> confirm"
                } else {
                    "usage: /memory feedback <memory-id> <success|failure|partial|unused> confirm"
                },
                cx,
            );
            return;
        };
        let outcome = self.outcome;
        let this = cx.entity().downgrade();
        window.open_alert_dialog(cx, move |alert, _, _| {
            let this = this.clone();
            let id = id.clone();
            let (title, description, ok) = if forget {
                (
                    format!("Forget memory {id}?"),
                    "The memory is deleted from the store. Deletion also requires the normal mutation approval policy and cannot be undone.".to_string(),
                    "Forget",
                )
            } else {
                (
                    format!("Record {outcome} for {id}?"),
                    "The outcome is stored as procedural-memory feedback.".to_string(),
                    "Record",
                )
            };
            alert
                .confirm()
                .title(title)
                .description(description)
                .ok_text(ok)
                .on_ok(move |_, window, cx| {
                    let arguments = if forget {
                        format!("forget {id} confirm")
                    } else {
                        format!("feedback {id} {outcome} confirm")
                    };
                    let _ = this.update(cx, |this, cx| this.run(arguments, window, cx));
                    true
                })
        });
    }

    // ----- skill folder browser (archived `show_skill_folder_picker`) ------

    fn allowed(&self, path: &Path) -> bool {
        let context = self.host.slash_context();
        if context.allow_external_filesystem_access {
            return true;
        }
        let root = std::fs::canonicalize(&context.workspace_root).unwrap_or(context.workspace_root);
        path.starts_with(root)
    }

    fn browse(&mut self, path: PathBuf, window: &mut Window, cx: &mut Context<Self>) {
        match std::fs::canonicalize(&path) {
            Ok(path) if path.is_dir() && self.allowed(&path) => {
                self.folder_input.update(cx, |s, cx| {
                    s.set_value(path.display().to_string(), window, cx)
                });
                self.folder = path;
                self.folder_error = None;
            }
            _ => {
                self.folder_error = Some(
                    "Memory skill folder is unavailable or outside the allowed workspace.".into(),
                )
            }
        }
        cx.notify();
    }

    fn subfolders(&self) -> Vec<PathBuf> {
        let Ok(entries) = std::fs::read_dir(&self.folder) else {
            return Vec::new();
        };
        let mut folders: Vec<PathBuf> = entries
            .flatten()
            .filter(|entry| {
                entry
                    .file_type()
                    .is_ok_and(|kind| kind.is_dir() && !kind.is_symlink())
            })
            .filter_map(|entry| std::fs::canonicalize(entry.path()).ok())
            .filter(|path| self.allowed(path))
            .collect();
        folders.sort();
        folders
    }

    fn scan(&mut self, recursive: bool, window: &mut Window, cx: &mut Context<Self>) {
        let typed = self.folder_input.read(cx).value().trim().to_string();
        match resolve_skill_folder(&self.host.slash_context(), &typed) {
            Ok(folder) => {
                self.folder_error = None;
                self.host.card(
                    "/memory scan-skills",
                    vec![format!(
                        "Memory skill scan started: {} (recursive: {})",
                        folder.display(),
                        if recursive { "yes" } else { "no" }
                    )],
                    false,
                    cx,
                );
                start_skill_scan(&self.host, folder, recursive, cx);
            }
            Err(message) => self.folder_error = Some(message),
        }
        let _ = window;
        cx.notify();
    }

    // ----- rendering --------------------------------------------------------

    fn render_section(&mut self, window: &mut Window, cx: &mut Context<Self>) -> AnyElement {
        let busy = self.busy.is_some();
        let enabled = self.host.memory().is_some();
        let body = v_flex().gap_2();
        let body = match self.section {
            Section::Status => body
                .child(section("STATUS", cx))
                .child(h_flex().child(
                    Button::new("memory-status-refresh")
                        .label("Refresh")
                        .xsmall()
                        .ghost()
                        .on_click(cx.listener(|this, _, w, cx| this.select(Section::Status, w, cx))),
                )),
            Section::Recall => body
                .child(section("RECALL", cx))
                .child(hint("Recall governed memory for a query (the memory_recall tool).", cx))
                .child(
                    h_flex()
                        .gap_2()
                        .child(div().flex_1().child(Input::new(&self.query).small()))
                        .child(
                            Button::new("memory-recall")
                                .label("Recall")
                                .small()
                                .primary()
                                .disabled(busy || !enabled)
                                .on_click(cx.listener(|this, _, w, cx| this.recall(w, cx))),
                        ),
                ),
            Section::Remember => body
                .child(section("REMEMBER", cx))
                .child(hint(
                    "Stored as a reviewable hypothesis with source user:interactive.",
                    cx,
                ))
                .child(Textarea::new(&self.content))
                .child(h_flex().child(
                    Button::new("memory-remember")
                        .label("Remember")
                        .small()
                        .primary()
                        .disabled(busy || !enabled)
                        .on_click(cx.listener(|this, _, w, cx| this.remember(w, cx))),
                )),
            Section::Feedback => {
                let mut outcomes = h_flex().gap_1();
                for (i, outcome) in OUTCOMES.into_iter().enumerate() {
                    outcomes = outcomes.child(
                        chip(("memory-outcome", i), outcome, self.outcome == outcome).on_click(
                            cx.listener(move |this, _, _, cx| {
                                this.outcome = outcome;
                                cx.notify();
                            }),
                        ),
                    );
                }
                body.child(section("FEEDBACK", cx))
                    .child(hint("Record a procedural-memory outcome.", cx))
                    .child(Input::new(&self.memory_id).small())
                    .child(outcomes)
                    .child(h_flex().child(
                        Button::new("memory-feedback")
                            .label("Record feedback…")
                            .small()
                            .primary()
                            .disabled(busy || !enabled)
                            .on_click(cx.listener(|this, _, w, cx| this.confirm(false, w, cx))),
                    ))
            }
            Section::Forget => body
                .child(section("FORGET", cx))
                .child(hint(
                    "Request deletion of an in-scope memory. You are asked to confirm.",
                    cx,
                ))
                .child(Input::new(&self.memory_id).small())
                .child(h_flex().child(
                    Button::new("memory-forget")
                        .label("Forget…")
                        .small()
                        .danger()
                        .disabled(busy || !enabled)
                        .on_click(cx.listener(|this, _, w, cx| this.confirm(true, w, cx))),
                )),
            Section::Dream => body
                .child(section("DREAMING", cx))
                .child(
                    h_flex()
                        .gap_1()
                        .child(
                            Button::new("memory-dream-status")
                                .label("Status")
                                .small()
                                .disabled(busy)
                                .on_click(
                                    cx.listener(|this, _, w, cx| this.select(Section::Dream, w, cx)),
                                ),
                        )
                        .child(
                            Button::new("memory-dream-run")
                                .label("Run cycle")
                                .small()
                                .primary()
                                .disabled(busy || !enabled)
                                .on_click(cx.listener(|this, _, w, cx| {
                                    this.run("dream run".into(), w, cx)
                                })),
                        ),
                )
                .child(section("RESEARCH EVIDENCE", cx))
                .child(hint(
                    "Store an independently researched successor for a dream (JSON with dream_id and summary).",
                    cx,
                ))
                .child(Textarea::new(&self.research))
                .child(h_flex().child(
                    Button::new("memory-dream-research")
                        .label("Store research")
                        .small()
                        .disabled(busy || !enabled)
                        .on_click(cx.listener(|this, _, w, cx| {
                            let json = this.research.read(cx).value().trim().to_string();
                            this.run(format!("dream research {json}"), w, cx)
                        })),
                )),
            Section::ScanSkills => self.render_scan(window, cx),
        };
        body.into_any_element()
    }

    fn render_scan(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> Div {
        let theme = cx.theme().clone();
        let scan = self.scan.read(cx);
        let running = scan.running;
        let scan_lines = scan.lines.clone();
        let enabled = self.host.memory().is_some();
        let mut folders = v_flex()
            .id("memory-scan-folders")
            .gap_0p5()
            .flex_1()
            .min_h(px(120.))
            .overflow_y_scroll();
        if let Some(parent) = self.folder.parent().map(Path::to_path_buf)
            && self.allowed(&parent)
        {
            folders = folders.child(
                widgets::list_row(
                    "memory-scan-parent",
                    "..",
                    parent.display().to_string(),
                    false,
                    cx,
                )
                .on_click(cx.listener(move |this, _, w, cx| this.browse(parent.clone(), w, cx))),
            );
        }
        for (i, folder) in self.subfolders().into_iter().enumerate() {
            let name = folder
                .file_name()
                .map(|n| format!("{}/", n.to_string_lossy()))
                .unwrap_or_default();
            let detail = folder.display().to_string();
            folders = folders.child(
                widgets::list_row(("memory-scan-folder", i), name, detail, false, cx).on_click(
                    cx.listener(move |this, _, w, cx| this.browse(folder.clone(), w, cx)),
                ),
            );
        }
        v_flex()
            .size_full()
            .gap_2()
            .child(section("SCAN SKILLS FOLDER", cx))
            .when(!enabled, |el| {
                el.child(hint(disabled_line("scanning skills"), cx))
            })
            .child(Input::new(&self.folder_input).small())
            .child(
                h_flex()
                    .gap_1()
                    .child(
                        Button::new("memory-scan")
                            .label("Scan this folder")
                            .small()
                            .primary()
                            .tooltip("Only this folder's SKILL.md")
                            .disabled(running || !enabled)
                            .on_click(cx.listener(|this, _, w, cx| this.scan(false, w, cx))),
                    )
                    .child(
                        Button::new("memory-scan-recursive")
                            .label("Scan this folder + subfolders")
                            .small()
                            .tooltip("Discover nested skill packages recursively")
                            .disabled(running || !enabled)
                            .on_click(cx.listener(|this, _, w, cx| this.scan(true, w, cx))),
                    ),
            )
            .when_some(self.folder_error.clone(), |el, error| {
                el.child(div().text_xs().text_color(theme.danger).child(error))
            })
            .child(folders)
            .when(!scan_lines.is_empty(), |el| {
                el.child(section(
                    if running {
                        "SCAN PROGRESS"
                    } else {
                        "LAST SCAN"
                    },
                    cx,
                ))
                .child(widgets::output_box(
                    "memory-scan-output",
                    v_flex()
                        .gap_0p5()
                        .when(running, |el| {
                            el.child(
                                h_flex()
                                    .gap_2()
                                    .text_xs()
                                    .child(Spinner::new().small())
                                    .child("Scanning in the background…"),
                            )
                        })
                        .child(widgets::lines(&scan_lines, false, cx)),
                    160.,
                    cx,
                ))
            })
    }
}

impl Render for MemoryPanel {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        let current = match self.section {
            Section::Status => "status",
            Section::Recall => "recall",
            Section::Remember => "remember",
            Section::Feedback => "feedback",
            Section::Forget => "forget",
            Section::ScanSkills => "scan-skills",
            Section::Dream => "dream",
        };
        let mut menu = v_flex()
            .id("memory-menu")
            .gap_0p5()
            .w(px(230.))
            .flex_none()
            .h_full()
            .overflow_y_scroll();
        for (i, (value, label, description)) in MENU.into_iter().enumerate() {
            menu = menu.child(
                widgets::list_row(("memory-menu", i), label, description, value == current, cx)
                    .on_click(cx.listener(move |this, _, w, cx| this.menu(value, w, cx))),
            );
        }
        let disabled = self.host.memory().is_none();
        // The folder browser fills the panel and scrolls its own list; the
        // form sections scroll as a whole when the window is short.
        let fills = matches!(self.section, Section::ScanSkills);
        let section = div()
            .id("memory-section")
            .flex_1()
            .min_h_0()
            .when(!fills, |el| el.overflow_y_scroll())
            .when(fills, |el| el.flex().flex_col())
            .child(self.render_section(window, cx));
        let output = widgets::output_box(
            "memory-panel-output",
            v_flex()
                .gap_0p5()
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
                .child(widgets::lines(&self.output, self.output_error, cx)),
            420.,
            cx,
        );
        v_flex()
            .size_full()
            .gap_2()
            .when(disabled, |el| {
                el.child(
                    div()
                        .p_2()
                        .rounded_md()
                        .bg(theme.warning.opacity(0.12))
                        .text_xs()
                        .child(
                            "Built-in memory is disabled for this runtime. Enable memory.enabled in /memory settings and start a new runtime.",
                        ),
                )
            })
            .child(
                h_flex()
                    .flex_1()
                    .min_h_0()
                    .gap_4()
                    .items_stretch()
                    .child(menu)
                    .child(
                        v_flex()
                            .flex_1()
                            .min_w_0()
                            .min_h_0()
                            .gap_2()
                            .child(section)
                            .when(self.busy.is_some() || !self.output.is_empty(), |el| {
                                el.child(output)
                            }),
                    ),
            )
    }
}

/// Opens the `/memory` panel on `section`.
pub fn open(host: MemoryHost, section: Section, window: &mut Window, cx: &mut App) {
    let panel = cx.new(|cx| MemoryPanel::new(host, section, window, cx));
    window.open_dialog(cx, move |dialog, window, _| {
        crate::layout::fill_panel(dialog.title("Memory"), window, 900., panel.clone())
            .on_ok(|_, _, _| false)
    });
}
