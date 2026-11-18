//! qt desktop application.

// Release builds on Windows are GUI-subsystem executables (no console window
// when launched from the Start menu / MSI shortcut).
#![cfg_attr(all(windows, not(debug_assertions)), windows_subsystem = "windows")]

mod composer;
mod dialogs;
mod extensions;
mod index_panel;
mod llama_panel;
mod login;
mod mcp_panel;
mod memory;
mod model_picker;
mod ollama_panel;
mod run_status;
mod session_views;
mod sessions;
mod settings_panel;
mod startup;
mod subagent_panel;
mod theme;
mod transcript;
mod window_frame;
mod workspace;

use std::time::Duration;

use anyhow::Result;
use gpui_kit::*;
use qt_core::logging::{LoggingOptions, init_logging};
use qt_runtime::cli_args::{self, Severity};
use qt_runtime::initial_prompt::prepare_initial_prompt;
use qt_runtime::{RunMode, Runtime};

use crate::workspace::Workspace;

/// The composition root's runtime (index, MCP, memory, extensions,
/// subagents, llama.cpp), reachable from any view through the app context.
pub struct RuntimeGlobal(pub Runtime);

impl Global for RuntimeGlobal {}

/// How long quitting waits for memory capture to flush.
const SHUTDOWN_CAPTURE_TIMEOUT: Duration = Duration::from_secs(5);

fn main() -> Result<()> {
    let raw: Vec<String> = std::env::args().skip(1).collect();
    let logging = LoggingOptions::from_args(&raw);
    if let Err(e) = init_logging(&logging) {
        eprintln!("qt: cannot open log file: {e}");
    }
    let args = match startup::parse_args(raw) {
        Ok(args) => args,
        Err(err) => {
            eprintln!("Error: {err:#}");
            std::process::exit(1);
        }
    };
    let mut failed = false;
    for (severity, message) in &args.cli.diagnostics {
        match severity {
            Severity::Error => {
                eprintln!("Error: {message}");
                failed = true;
            }
            Severity::Warning => eprintln!("Warning: {message}"),
        }
    }
    if failed {
        std::process::exit(1);
    }
    if args.cli.version {
        println!("{}", env!("CARGO_PKG_VERSION"));
        return Ok(());
    }
    if args.cli.help {
        print!("{}", startup::USAGE);
        return Ok(());
    }
    if args.cli.offline || std::env::var("PI_OFFLINE").is_ok_and(|v| v == "1") {
        // SAFETY: single-threaded at this point; children inherit it.
        unsafe {
            std::env::set_var("PI_OFFLINE", "1");
            std::env::set_var("PI_SKIP_VERSION_CHECK", "1");
        }
    }
    let launch_dir = std::env::current_dir()?;
    let cwd = match &args.cwd {
        Some(dir) => launch_dir.join(dir),
        None => launch_dir.clone(),
    };
    anyhow::ensure!(cwd.is_dir(), "{} is not a directory", cwd.display());
    let stdin = startup::read_piped_stdin();
    // Paths on the command line are relative to where qt was started; the
    // session runs in the project directory.
    let mut options = cli_args::runtime_options(&args.cli, RunMode::Interactive, &launch_dir, None);
    options.cwd = cwd.clone();
    options.startup_migrations = true;
    let runtime = Runtime::build(options)?;
    for diagnostic in runtime.diagnostics() {
        eprintln!("qt: {diagnostic}");
    }
    let engine = runtime.engine().clone();
    let startup = match startup::resolve(&engine, &args.session) {
        Ok(action) => action,
        Err(err) => {
            eprintln!("{err:#}");
            std::process::exit(1);
        }
    };
    let messages: Vec<String> = args
        .cli
        .messages
        .iter()
        .chain(runtime.extra_messages())
        .cloned()
        .collect();
    let initial = match prepare_initial_prompt(
        &messages,
        &args.cli.file_args,
        stdin.as_deref(),
        &launch_dir,
        engine.settings().image_auto_resize(),
    ) {
        Ok((prompt, diagnostics)) => {
            for d in diagnostics {
                eprintln!("Warning: {}", d.message);
            }
            (!prompt.is_empty()).then_some(prompt)
        }
        Err(message) => {
            eprintln!("Error: {message}");
            std::process::exit(1);
        }
    };
    // The window cannot ask about a session's missing cwd before the first
    // prompt is sent, so refuse that combination (archived non-interactive
    // wording).
    if initial.is_some()
        && let startup::StartupAction::Open(path) = &startup
        && let Some(issue) = engine.session_cwd_issue(path)
    {
        eprintln!("{}", issue.error_text());
        std::process::exit(1);
    }
    runtime.start_background_services();
    let project = engine
        .cwd()
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_else(|| engine.cwd().display().to_string());

    gpui_kit::application()
        .with_assets(gpui_kit::assets::Assets)
        .run(move |cx: &mut App| {
            gpui_kit::init(cx);
            cx.set_global(RuntimeGlobal(runtime.clone()));
            workspace::bind_keys(cx);
            theme::init(&engine, cx);
            let bounds = Bounds::centered(None, size(px(1240.), px(860.)), cx);
            let options = WindowOptions {
                window_bounds: Some(WindowBounds::Windowed(bounds)),
                titlebar: Some(TitlebarOptions {
                    title: Some(format!("qt — {project}").into()),
                    ..Default::default()
                }),
                window_min_size: Some(size(px(640.), px(480.))),
                app_id: Some("qt".into()),
                ..Default::default()
            };
            let engine = engine.clone();
            let startup = startup.clone();
            let initial = initial.clone();
            let title = format!("qt — {project}");
            if let Err(err) = gpui_kit::open_window(options, cx, move |window, cx| {
                let startup = startup.clone();
                let initial = initial.clone();
                window_frame::framed(title, window, cx, |window, cx| {
                    cx.new(|cx| {
                        let workspace = Workspace::new(engine, window, cx);
                        cx.defer_in(window, move |this: &mut Workspace, window, cx| {
                            this.apply_startup(startup, window, cx);
                            // Command-line messages run like typed composer
                            // lines (slash commands, `!` commands, prompts).
                            if let Some(prompt) = initial {
                                this.submit_startup_lines(prompt.into_lines(), window, cx);
                            }
                        });
                        workspace
                    })
                })
            }) {
                eprintln!("qt: failed to open window: {err:#}");
                cx.quit();
            }
            cx.on_window_closed(|cx, _| {
                if cx.windows().is_empty() {
                    cx.quit();
                }
            })
            .detach();
            // Stop workers, MCP servers and the index, and flush memory
            // captures before the process exits.
            let quitting = runtime.clone();
            cx.on_app_quit(move |_| {
                let runtime = quitting.clone();
                async move { runtime.shutdown(SHUTDOWN_CAPTURE_TIMEOUT) }
            })
            .detach();
            cx.activate(true);
        });
    Ok(())
}
