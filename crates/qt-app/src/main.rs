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
mod workspace;

use std::time::Duration;

use anyhow::Result;
use gpui_kit::*;
use qt_runtime::{Runtime, RuntimeOptions};

use crate::workspace::Workspace;

/// The composition root's runtime (index, MCP, memory, extensions,
/// subagents, llama.cpp), reachable from any view through the app context.
pub struct RuntimeGlobal(pub Runtime);

impl Global for RuntimeGlobal {}

/// How long quitting waits for memory capture to flush.
const SHUTDOWN_CAPTURE_TIMEOUT: Duration = Duration::from_secs(5);

fn main() -> Result<()> {
    let args = startup::parse_args(std::env::args().skip(1))?;
    if args.help {
        print!("{}", startup::USAGE);
        return Ok(());
    }
    let cwd = match &args.cwd {
        Some(dir) => dir.clone(),
        None => std::env::current_dir()?,
    };
    anyhow::ensure!(cwd.is_dir(), "{} is not a directory", cwd.display());
    let mut options = RuntimeOptions::interactive(cwd.clone());
    options.no_session = args.no_session;
    options.session_dir = args.session_dir.as_ref().map(|dir| cwd.join(dir));
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
            if let Err(err) = gpui_kit::open_window(options, cx, move |window, cx| {
                let startup = startup.clone();
                cx.new(|cx| {
                    let workspace = Workspace::new(engine, window, cx);
                    cx.defer_in(window, move |this: &mut Workspace, window, cx| {
                        this.apply_startup(startup, window, cx)
                    });
                    workspace
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
