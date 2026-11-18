//! `qt-cli`: the headless qt front end (archived `main.cpp` /
//! `cli_startup.cpp`): print, JSON and RPC modes, `--list-models`, `--export`,
//! session selection and the `install`/`remove`/`update`/`list`/`config`/
//! `memory` subcommands. The interactive interface is the desktop app.

mod args;
mod help;
mod list_models;
mod prompting;
mod rpc;
mod run;
mod session;

use std::io::{IsTerminal as _, Write as _};
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use qt_core::config::AgentHome;
use qt_core::logging::{LoggingOptions, init_logging};

use crate::args::{Severity, StartupMode};

fn main() -> ExitCode {
    let raw: Vec<String> = std::env::args().skip(1).collect();
    // SAFETY: single-threaded at this point; children inherit the marker.
    unsafe { std::env::set_var("PI_CODING_AGENT", "true") };
    let logging = LoggingOptions::from_args(&raw);
    if let Err(e) = init_logging(&logging) {
        eprintln!("qt-cli: cannot open log file: {e}");
    }
    tracing::info!(target: "qt_cli", argc = raw.len(), "process_start");
    let code = run(&raw);
    tracing::info!(target: "qt_cli", code, "process_exit");
    let _ = std::io::stdout().flush();
    ExitCode::from(code.clamp(0, 255) as u8)
}

fn current_dir() -> PathBuf {
    std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."))
}

fn resolve(cwd: &Path, path: &str) -> String {
    qt_runtime::cli_args::resolve_path(cwd, path)
}

fn run(raw: &[String]) -> i32 {
    let cwd = current_dir();
    let home = AgentHome::from_env();
    if raw.first().map(String::as_str) == Some("memory") {
        return run::memory_command(&raw[1..], &cwd, &home);
    }
    {
        let stdout = &mut std::io::stdout();
        let stderr = &mut std::io::stderr();
        if let Some(code) = qt_extensions::packages::run_package_command(
            raw,
            &cwd,
            home.root(),
            qt_extensions::packages::PackageRunOptions::default(),
            stdout,
            stderr,
        ) {
            return code;
        }
    }
    let mut parsed = args::parse(raw);
    if parsed.offline || std::env::var("PI_OFFLINE").is_ok_and(|v| v == "1") {
        parsed.offline = true;
        // SAFETY: no other threads read the environment concurrently here.
        unsafe {
            std::env::set_var("PI_OFFLINE", "1");
            std::env::set_var("PI_SKIP_VERSION_CHECK", "1");
        }
    }
    let mut failed = false;
    for (severity, message) in &parsed.diagnostics {
        match severity {
            Severity::Error => {
                eprintln!("Error: {message}");
                failed = true;
            }
            Severity::Warning => eprintln!("Warning: {message}"),
        }
    }
    if failed {
        return 1;
    }
    if parsed.version {
        println!("{}", env!("CARGO_PKG_VERSION"));
        return 0;
    }
    let stdin_is_tty = std::io::stdin().is_terminal();
    if parsed.help {
        if args::startup_mode(&parsed, stdin_is_tty) == StartupMode::Interactive {
            print!("{}", help::HELP);
        } else {
            eprint!("{}", help::HELP);
        }
        return 0;
    }
    let migrations = qt_core::migrations::run_startup_migrations(&cwd, &home);
    tracing::debug!(target: "qt_cli", ?migrations, "startup_migrations");
    if let Some(export) = &parsed.export {
        return run::export(export, parsed.messages.first().map(String::as_str), &cwd);
    }
    if parsed.list_models.is_some() || parsed.list_models_all {
        return list_models::run(&parsed, &cwd, &home);
    }
    let mode = args::startup_mode(&parsed, stdin_is_tty);
    if mode == StartupMode::Rpc && !parsed.file_args.is_empty() {
        eprintln!("Error: @file arguments are not supported in RPC mode");
        return 1;
    }
    if let Some(error) = parsed.fork_error() {
        eprintln!("Error: {error}");
        return 1;
    }
    if let Some(error) = parsed.api_key_error() {
        eprintln!("Error: {error}");
        return 1;
    }
    if let Err(message) = run::check_memory_child(&parsed) {
        eprintln!("Error: {message}");
        return 1;
    }
    let options = run::runtime_options(&parsed, mode, &cwd, &home);
    if mode == StartupMode::Rpc {
        return rpc::run(parsed, options);
    }
    run::main(parsed, mode, stdin_is_tty, options)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn relative_paths_resolve_against_cwd() {
        assert_eq!(resolve(Path::new("/w"), "a/b"), "/w/a/b");
        assert_eq!(resolve(Path::new("/w"), "/x"), "/x");
    }
}
