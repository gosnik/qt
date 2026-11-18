//! qt desktop application.

mod composer;
mod dialogs;
mod extensions;
mod index_panel;
mod llama_panel;
mod login;
mod mcp_panel;
mod model_picker;
mod ollama_panel;
mod run_status;
mod session_views;
mod sessions;
mod settings_panel;
mod subagent_panel;
mod theme;
mod transcript;
mod workspace;

use std::path::PathBuf;
use std::time::Duration;

use anyhow::{Context as _, Result};
use gpui_kit::*;
use qt_runtime::{Runtime, RuntimeOptions};

use crate::workspace::Workspace;

/// The composition root's runtime (index, MCP, memory, extensions,
/// subagents, llama.cpp), reachable from any view through the app context.
pub struct RuntimeGlobal(pub Runtime);

impl Global for RuntimeGlobal {}

/// How long quitting waits for memory capture to flush.
const SHUTDOWN_CAPTURE_TIMEOUT: Duration = Duration::from_secs(5);

const USAGE: &str = "Usage: qt [--cwd <dir> | <dir>]\n\nOpens the qt desktop app for a project directory (default: current directory).\nUse qt-cli for headless runs.\n";

fn parse_cwd() -> Result<Option<PathBuf>> {
    let mut args = std::env::args().skip(1);
    let mut cwd = None;
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "-h" | "--help" => {
                print!("{USAGE}");
                return Ok(None);
            }
            "--cwd" => {
                cwd = Some(PathBuf::from(
                    args.next().context("--cwd needs a directory")?,
                ))
            }
            other if other.starts_with('-') => anyhow::bail!("unknown option {other}\n\n{USAGE}"),
            other => cwd = Some(PathBuf::from(other)),
        }
    }
    let cwd = match cwd {
        Some(dir) => dir,
        None => std::env::current_dir()?,
    };
    anyhow::ensure!(cwd.is_dir(), "{} is not a directory", cwd.display());
    Ok(Some(cwd))
}

fn main() -> Result<()> {
    let Some(cwd) = parse_cwd()? else {
        return Ok(());
    };
    let runtime = Runtime::build(RuntimeOptions::interactive(cwd))?;
    for diagnostic in runtime.diagnostics() {
        eprintln!("qt: {diagnostic}");
    }
    runtime.start_background_services();
    let engine = runtime.engine().clone();
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
            if let Err(err) = gpui_kit::open_window(options, cx, move |window, cx| {
                cx.new(|cx| Workspace::new(engine, window, cx))
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
