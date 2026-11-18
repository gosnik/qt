//! Governed memory in the desktop app (archived `/memory` in
//! `interactive_mode.cpp` and the browser dashboard): the `/memory` panel,
//! the memory settings editor, the Review Inbox and the native dashboard
//! window. Subcommands run through `qt_memory::slash` on the runtime; views
//! use the `qt_memory::views` models.

mod dashboard;
mod graph;
mod inbox;
mod panel;
mod settings;
mod widgets;

use std::path::PathBuf;
use std::rc::Rc;
use std::sync::Arc;

use gpui_kit::component::WindowExt as _;
use gpui_kit::component::notification::Notification;
use gpui_kit::*;
use qt_memory::MemoryRuntime;
use qt_memory::settings::{MemorySettings, resolve_memory_settings};
use qt_memory::slash::{
    SlashMemoryAction, SlashMemoryContext, SlashMemoryResponse, memory_slash_command,
    run_skill_scan,
};
use qt_runtime::Runtime;
use serde_json::Value;

use crate::RuntimeGlobal;

pub use panel::Section;

/// Pushes a transcript card: `(title, lines, error)`.
pub type CardSink = Rc<dyn Fn(String, Vec<String>, bool, &mut App)>;

/// What the memory views need from the workspace.
#[derive(Clone)]
pub struct MemoryHost {
    pub runtime: Runtime,
    /// Transcript output (archived transcript lines).
    pub card: CardSink,
    /// `(provider/id, title)` of every discovered model, for the settings
    /// model pickers.
    pub models: Vec<(String, String)>,
}

impl MemoryHost {
    /// A host for the app's runtime.
    pub fn new(card: CardSink, models: Vec<(String, String)>, cx: &App) -> Self {
        Self {
            runtime: cx.global::<RuntimeGlobal>().0.clone(),
            card,
            models,
        }
    }

    /// The memory runtime, `None` when memory is disabled for this run.
    pub fn memory(&self) -> Option<Arc<MemoryRuntime>> {
        self.runtime.memory().map(|m| m.runtime().clone())
    }

    /// `/memory` inputs with the memory settings re-read from the settings
    /// files, so `status` shows saved changes as the archived app did.
    pub fn slash_context(&self) -> SlashMemoryContext {
        let mut context = self.runtime.memory_slash_context();
        if let Some(settings) = effective_memory_settings(&self.runtime) {
            context.settings = settings;
        }
        context
    }

    pub fn card(&self, title: impl Into<String>, lines: Vec<String>, error: bool, cx: &mut App) {
        if !lines.is_empty() {
            (self.card)(title.into(), lines, error, cx);
        }
    }

    /// Discovered models plus configured local llama.cpp models.
    pub fn model_choices(&self) -> Vec<(String, String)> {
        let mut models = self.models.clone();
        let settings = self.runtime.engine().settings();
        for model in self.runtime.llama().model_infos(&settings) {
            let id = format!("llama/{}", model.id);
            if !models.iter().any(|(value, _)| value == &id) {
                models.push((id, model.name.clone()));
            }
        }
        models
    }
}

/// Global and project memory settings as currently saved.
pub fn effective_memory_settings(runtime: &Runtime) -> Option<MemorySettings> {
    let engine = runtime.engine();
    let global = Value::Object(engine.global_settings().raw().clone());
    let project = engine.project_settings_layer();
    resolve_memory_settings(
        &global,
        (!project.is_empty())
            .then(|| Value::Object(project))
            .as_ref(),
    )
    .ok()
}

/// Runs blocking store work on the engine runtime's blocking pool so the UI
/// thread never waits on it.
pub fn blocking<T: Send + 'static>(
    runtime: &Runtime,
    work: impl FnOnce() -> T + Send + 'static,
) -> impl std::future::Future<Output = Result<T, String>> + 'static {
    let handle = runtime.engine().runtime_handle().spawn_blocking(work);
    async move { handle.await.map_err(|error| error.to_string()) }
}

/// The archived message for views that need an active memory runtime.
pub fn disabled_line(what: &str) -> String {
    format!(
        "Built-in memory is disabled. Enable it in /memory settings and start a new runtime before {what}."
    )
}

/// Runs `/memory <arguments>` from the composer or palette: text output
/// becomes a transcript card and view actions open their views.
pub fn run_slash(
    host: MemoryHost,
    line: String,
    arguments: String,
    window: &mut Window,
    cx: &mut App,
) {
    let context = host.slash_context();
    let handle = host
        .runtime
        .engine()
        .spawn(async move { memory_slash_command(&context, &arguments).await });
    window
        .spawn(cx, async move |cx| {
            let response = handle.await.unwrap_or_else(|error| SlashMemoryResponse {
                lines: vec![format!("/memory failed: {error}")],
                action: None,
            });
            let _ = cx.update(|window, cx| {
                host.card(line, response.lines, false, cx);
                if let Some(action) = response.action {
                    perform_action(&host, action, window, cx);
                }
            });
        })
        .detach();
}

/// Opens the view a `/memory` subcommand asked for.
pub fn perform_action(
    host: &MemoryHost,
    action: SlashMemoryAction,
    window: &mut Window,
    cx: &mut App,
) {
    match action {
        SlashMemoryAction::OpenMenu => panel::open(host.clone(), Section::Status, window, cx),
        SlashMemoryAction::OpenSettings => settings::open(host.clone(), window, cx),
        SlashMemoryAction::OpenReviewInbox => open_inbox(host, window, cx),
        SlashMemoryAction::OpenDashboard => open_dashboard(host, window, cx),
        SlashMemoryAction::PickSkillFolder => {
            if host.memory().is_none() {
                host.card(
                    "/memory scan-skills",
                    vec![disabled_line("scanning skills")],
                    false,
                    cx,
                );
                return;
            }
            panel::open(host.clone(), Section::ScanSkills, window, cx)
        }
        SlashMemoryAction::ScanSkills { folder, recursive } => {
            start_skill_scan(host, folder, recursive, cx)
        }
    }
}

/// Opens the `/memory` panel on `section` (sidebar entry).
pub fn open_panel(host: MemoryHost, section: Section, window: &mut Window, cx: &mut App) {
    panel::open(host, section, window, cx)
}

fn open_inbox(host: &MemoryHost, window: &mut Window, cx: &mut App) {
    if host.memory().is_none() {
        host.card(
            "/memory review",
            vec![disabled_line("reviewing memory")],
            false,
            cx,
        );
        return;
    }
    inbox::open(host.clone(), window, cx)
}

fn open_dashboard(host: &MemoryHost, window: &mut Window, cx: &mut App) {
    if host.memory().is_none() {
        host.card(
            "/memory dashboard",
            vec![disabled_line("opening the dashboard")],
            false,
            cx,
        );
        return;
    }
    if let Err(error) = dashboard::open(host.clone(), cx) {
        window.push_notification(
            Notification::error(format!("Memory dashboard failed to start: {error:#}")),
            cx,
        );
    }
}

/// Progress of the background skill scan (one at a time).
#[derive(Default)]
pub struct SkillScan {
    pub running: bool,
    pub folder: Option<PathBuf>,
    pub recursive: bool,
    pub lines: Vec<String>,
}

struct SkillScanGlobal(Entity<SkillScan>);

impl Global for SkillScanGlobal {}

/// The shared skill-scan state.
pub fn skill_scan(cx: &mut App) -> Entity<SkillScan> {
    if let Some(global) = cx.try_global::<SkillScanGlobal>() {
        return global.0.clone();
    }
    let entity = cx.new(|_| SkillScan::default());
    cx.set_global(SkillScanGlobal(entity.clone()));
    entity
}

enum ScanEvent {
    Progress(String),
    Done(Vec<String>),
}

/// Imports skills from `folder` on a background thread; progress lines go to
/// the shared state, the summary to the transcript (archived
/// `launch_skill_scan`).
pub fn start_skill_scan(host: &MemoryHost, folder: PathBuf, recursive: bool, cx: &mut App) {
    let Some(memory) = host.memory() else {
        host.card(
            "/memory scan-skills",
            vec![disabled_line("scanning skills")],
            false,
            cx,
        );
        return;
    };
    let state = skill_scan(cx);
    if state.read(cx).running {
        host.card(
            "/memory scan-skills",
            vec!["Memory skill scan is already running.".into()],
            true,
            cx,
        );
        return;
    }
    state.update(cx, |scan, cx| {
        scan.running = true;
        scan.folder = Some(folder.clone());
        scan.recursive = recursive;
        scan.lines = vec![format!(
            "Memory skill scan started: {} (recursive: {})",
            folder.display(),
            if recursive { "yes" } else { "no" }
        )];
        cx.notify();
    });
    let context = host.slash_context();
    let containment = (!context.allow_external_filesystem_access).then_some(context.workspace_root);
    let (sender, receiver) = async_channel::unbounded::<ScanEvent>();
    let progress_sender = sender.clone();
    let spawned = std::thread::Builder::new()
        .name("qt-memory-skill-scan".into())
        .spawn(move || {
            let summary = run_skill_scan(&memory, &folder, recursive, containment, move |line| {
                let _ = progress_sender.send_blocking(ScanEvent::Progress(line));
            });
            let _ = sender.send_blocking(ScanEvent::Done(summary));
        });
    if let Err(error) = spawned {
        state.update(cx, |scan, cx| {
            scan.running = false;
            scan.lines
                .push(format!("Memory skill scan failed: {error}"));
            cx.notify();
        });
        return;
    }
    let host = host.clone();
    cx.spawn(async move |cx| {
        while let Ok(event) = receiver.recv().await {
            let finished = matches!(event, ScanEvent::Done(_));
            cx.update(|cx| {
                state.update(cx, |scan, cx| {
                    match &event {
                        ScanEvent::Progress(line) => scan.lines.push(line.clone()),
                        ScanEvent::Done(summary) => {
                            scan.lines.extend(summary.iter().cloned());
                            scan.running = false;
                        }
                    }
                    cx.notify();
                });
                if let ScanEvent::Done(summary) = event {
                    let error = summary.iter().any(|line| line.contains("failed:"));
                    host.card("/memory scan-skills", summary, error, cx);
                }
            });
            if finished {
                break;
            }
        }
    })
    .detach();
}
