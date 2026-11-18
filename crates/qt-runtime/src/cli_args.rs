//! Command-line parsing with the archived `parse_cli_args` semantics: value
//! options take the next argument whatever it looks like, repeatable options
//! accumulate, single-value options keep the last value, `@file` arguments
//! and messages are collected, unknown `--flags` are kept for extensions
//! and unknown short options are errors.
//!
//! `qt-cli` and the desktop app (`qt`) both parse with [`parse`] and build
//! their [`RuntimeOptions`] with [`runtime_options`], so every flag reaches
//! the runtime the same way in both front ends.

use std::path::{Path, PathBuf};

use qt_core::logging::LogLevel;
use qt_core::provider::ThinkingLevel;

use crate::options::{RunMode, RuntimeOptions};

/// Severity of a parse diagnostic.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Severity {
    Warning,
    Error,
}

/// Parsed command line.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct CliArgs {
    pub help: bool,
    pub version: bool,
    /// `text`, `json` or `rpc` (other values are ignored).
    pub mode: Option<String>,
    pub print: bool,
    pub continue_session: bool,
    pub resume: bool,
    pub provider: Option<String>,
    pub model: Option<String>,
    pub api_key: Option<String>,
    pub system_prompt: Option<String>,
    pub append_system_prompt: Vec<String>,
    pub no_session: bool,
    pub session: Option<String>,
    pub fork: Option<String>,
    pub session_dir: Option<String>,
    pub models: Vec<String>,
    pub no_tools: bool,
    pub no_builtin_tools: bool,
    pub no_memory: bool,
    pub tools: Option<Vec<String>>,
    pub thinking: Option<ThinkingLevel>,
    pub export: Option<String>,
    pub extensions: Vec<String>,
    pub no_extensions: bool,
    pub skills: Vec<String>,
    pub prompt_templates: Vec<String>,
    pub themes: Vec<String>,
    pub no_skills: bool,
    pub no_prompt_templates: bool,
    pub no_themes: bool,
    pub no_context_files: bool,
    pub local_model_dirs: Vec<String>,
    /// `--list-models` without a search term.
    pub list_models_all: bool,
    /// `--list-models <search>`.
    pub list_models: Option<String>,
    pub verbose: bool,
    pub offline: bool,
    pub log_files: Vec<String>,
    pub log_stderr: bool,
    pub log_level: Option<String>,
    pub messages: Vec<String>,
    /// `@file` arguments without the `@`.
    pub file_args: Vec<String>,
    /// Unknown `--flag[=value]` / `--flag value` arguments, in order, for
    /// extension flag resolution.
    pub unknown_flags: Vec<String>,
    pub diagnostics: Vec<(Severity, String)>,
}

fn split_csv(text: &str, keep_empty: bool) -> Vec<String> {
    text.split(',')
        .map(|s| s.trim().to_string())
        .filter(|s| keep_empty || !s.is_empty())
        .collect()
}

/// Options that take a value, with their canonical names.
fn value_option(arg: &str) -> Option<&'static str> {
    Some(match arg {
        "--mode" => "--mode",
        "--provider" => "--provider",
        "--model" => "--model",
        "--api-key" => "--api-key",
        "--system-prompt" => "--system-prompt",
        "--append-system-prompt" => "--append-system-prompt",
        "--session" => "--session",
        "--fork" => "--fork",
        "--session-dir" => "--session-dir",
        "--models" => "--models",
        "--tools" | "-t" => "--tools",
        "--thinking" => "--thinking",
        "--export" => "--export",
        "--extension" | "-e" => "--extension",
        "--skill" => "--skill",
        "--prompt-template" => "--prompt-template",
        "--theme" => "--theme",
        "--discover-local-models" => "--discover-local-models",
        "--log-file" => "--log-file",
        "--log-level" => "--log-level",
        _ => return None,
    })
}

/// Boolean flags with their canonical names.
fn flag_option(arg: &str) -> Option<&'static str> {
    Some(match arg {
        "--help" | "-h" => "--help",
        "--version" | "-v" => "--version",
        "--continue" | "-c" => "--continue",
        "--resume" | "-r" => "--resume",
        "--no-session" => "--no-session",
        "--no-tools" | "-nt" => "--no-tools",
        "--no-builtin-tools" | "-nbt" => "--no-builtin-tools",
        "--no-memory" => "--no-memory",
        "--print" | "-p" => "--print",
        "--no-extensions" | "-ne" => "--no-extensions",
        "--no-skills" | "-ns" => "--no-skills",
        "--no-prompt-templates" | "-np" => "--no-prompt-templates",
        "--no-themes" => "--no-themes",
        "--no-context-files" | "-nc" => "--no-context-files",
        "--verbose" => "--verbose",
        "--offline" => "--offline",
        "--log-stderr" => "--log-stderr",
        _ => return None,
    })
}

fn is_print_prompt_candidate(text: &str) -> bool {
    !text.starts_with('@') && (!text.starts_with('-') || text.starts_with("---"))
}

/// Parse `args` (without the program name).
pub fn parse(args: &[String]) -> CliArgs {
    let mut parsed = CliArgs::default();
    let mut i = 0;
    while i < args.len() {
        let arg = args[i].as_str();
        if arg == "--list-models" {
            match args.get(i + 1) {
                Some(next)
                    if !next.is_empty() && !next.starts_with('-') && !next.starts_with('@') =>
                {
                    parsed.list_models = Some(next.clone());
                    parsed.list_models_all = false;
                    i += 2;
                }
                _ => {
                    parsed.list_models_all = parsed.list_models.is_none();
                    i += 1;
                }
            }
            continue;
        }
        if let Some((name, inline)) = arg
            .split_once('=')
            .filter(|(name, _)| name.starts_with("--") && value_option(name).is_some())
        {
            apply_value(
                &mut parsed,
                value_option(name).unwrap_or(name),
                inline.to_string(),
            );
            i += 1;
            continue;
        }
        if let Some(name) = value_option(arg) {
            match args.get(i + 1) {
                Some(value) => {
                    apply_value(&mut parsed, name, value.clone());
                    i += 2;
                }
                None => {
                    // Archived: a value option without a value is an
                    // unknown flag (resolved against extension flags).
                    parsed.unknown_flags.push(name.to_string());
                    i += 1;
                }
            }
            continue;
        }
        if let Some(name) = flag_option(arg) {
            apply_flag(&mut parsed, name);
            if name == "--print"
                && let Some(next) = args.get(i + 1)
                && is_print_prompt_candidate(next)
            {
                parsed.messages.push(next.clone());
                i += 2;
                continue;
            }
            i += 1;
            continue;
        }
        if let Some(file) = arg.strip_prefix('@') {
            parsed.file_args.push(file.to_string());
        } else if arg.starts_with("--") && arg.len() > 2 {
            parsed.unknown_flags.push(arg.to_string());
            if !arg.contains('=')
                && let Some(next) = args.get(i + 1)
                && !next.is_empty()
                && !next.starts_with('-')
                && !next.starts_with('@')
            {
                parsed.unknown_flags.push(next.clone());
                i += 1;
            }
        } else if arg.len() > 1 && arg.starts_with('-') {
            parsed
                .diagnostics
                .push((Severity::Error, format!("Unknown option: {arg}")));
        } else {
            parsed.messages.push(arg.to_string());
        }
        i += 1;
    }
    parsed
}

fn apply_flag(parsed: &mut CliArgs, name: &str) {
    match name {
        "--help" => parsed.help = true,
        "--version" => parsed.version = true,
        "--continue" => parsed.continue_session = true,
        "--resume" => parsed.resume = true,
        "--no-session" => parsed.no_session = true,
        "--no-tools" => parsed.no_tools = true,
        "--no-builtin-tools" => parsed.no_builtin_tools = true,
        "--no-memory" => parsed.no_memory = true,
        "--print" => parsed.print = true,
        "--no-extensions" => parsed.no_extensions = true,
        "--no-skills" => parsed.no_skills = true,
        "--no-prompt-templates" => parsed.no_prompt_templates = true,
        "--no-themes" => parsed.no_themes = true,
        "--no-context-files" => parsed.no_context_files = true,
        "--verbose" => parsed.verbose = true,
        "--offline" => parsed.offline = true,
        "--log-stderr" => parsed.log_stderr = true,
        _ => {}
    }
}

fn apply_value(parsed: &mut CliArgs, name: &str, value: String) {
    match name {
        "--mode" => {
            if matches!(value.as_str(), "text" | "json" | "rpc") {
                parsed.mode = Some(value);
            }
        }
        "--provider" => parsed.provider = Some(value),
        "--model" => parsed.model = Some(value),
        "--api-key" => parsed.api_key = Some(value),
        "--system-prompt" => parsed.system_prompt = Some(value),
        "--append-system-prompt" => parsed.append_system_prompt.push(value),
        "--session" => parsed.session = Some(value),
        "--fork" => parsed.fork = Some(value),
        "--session-dir" => parsed.session_dir = Some(value),
        "--models" => parsed.models = split_csv(&value, true),
        "--tools" => parsed.tools = Some(split_csv(&value, false)),
        "--thinking" => match ThinkingLevel::parse(&value).filter(|_| value == value.trim().to_ascii_lowercase()) {
            Some(level) => parsed.thinking = Some(level),
            None => parsed.diagnostics.push((
                Severity::Warning,
                format!(
                    "Invalid thinking level \"{value}\". Valid values: off, minimal, low, medium, high, xhigh"
                ),
            )),
        },
        "--export" => parsed.export = Some(value),
        "--extension" => parsed.extensions.push(value),
        "--skill" => parsed.skills.push(value),
        "--prompt-template" => parsed.prompt_templates.push(value),
        "--theme" => parsed.themes.push(value),
        "--discover-local-models" => parsed.local_model_dirs.extend(split_csv(&value, false)),
        "--log-file" => parsed.log_files.push(value),
        "--log-level" => {
            if LogLevel::parse(&value).is_none() {
                parsed.diagnostics.push((
                    Severity::Warning,
                    format!(
                        "Invalid log level \"{value}\". Valid values: trace, debug, info, warn, error, critical, off"
                    ),
                ));
            }
            parsed.log_level = Some(value);
        }
        _ => {}
    }
}

/// Startup mode (archived `resolve_startup_mode`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StartupMode {
    Interactive,
    Print,
    Json,
    Rpc,
}

pub fn startup_mode(parsed: &CliArgs, stdin_is_tty: bool) -> StartupMode {
    match parsed.mode.as_deref() {
        Some("rpc") => StartupMode::Rpc,
        Some("json") => StartupMode::Json,
        _ if parsed.print || !stdin_is_tty => StartupMode::Print,
        _ => StartupMode::Interactive,
    }
}

impl CliArgs {
    /// The flags that only make sense for a headless run (`-p`, `--mode`,
    /// `--export`, `--list-models`), by canonical name, in a fixed order.
    pub fn print_only_flags(&self) -> Vec<&'static str> {
        let mut flags = Vec::new();
        if self.print {
            flags.push("--print");
        }
        if self.mode.is_some() {
            flags.push("--mode");
        }
        if self.export.is_some() {
            flags.push("--export");
        }
        if self.list_models.is_some() || self.list_models_all {
            flags.push("--list-models");
        }
        flags
    }

    /// Archived: `--api-key` needs a model to apply to.
    pub fn api_key_error(&self) -> Option<&'static str> {
        (self.api_key.is_some() && self.model.is_none() && self.models.is_empty()).then_some(
            "--api-key requires a model to be specified via --model, --provider/--model, or --models",
        )
    }

    /// Archived: `--fork` cannot be combined with another session choice.
    pub fn fork_error(&self) -> Option<String> {
        self.fork.as_ref()?;
        let mut conflicts = Vec::new();
        if self.session.is_some() {
            conflicts.push("--session");
        }
        if self.continue_session {
            conflicts.push("--continue");
        }
        if self.resume {
            conflicts.push("--resume");
        }
        if self.no_session {
            conflicts.push("--no-session");
        }
        (!conflicts.is_empty())
            .then(|| format!("--fork cannot be combined with {}", conflicts.join(", ")))
    }
}

/// `path` against `cwd` unless it is absolute.
pub fn resolve_path(cwd: &Path, path: &str) -> String {
    let p = Path::new(path);
    if p.is_absolute() {
        path.to_string()
    } else {
        cwd.join(p).to_string_lossy().into_owned()
    }
}

/// Runtime options from the parsed flags. Paths are resolved against `cwd`;
/// `agent_home` is the agent home root (`None`: `QT_CODING_AGENT_DIR` /
/// `~/.qt`).
pub fn runtime_options(
    parsed: &CliArgs,
    mode: RunMode,
    cwd: &Path,
    agent_home: Option<&Path>,
) -> RuntimeOptions {
    let paths = |values: &[String]| {
        values
            .iter()
            .map(|v| resolve_path(cwd, v))
            .collect::<Vec<_>>()
    };
    RuntimeOptions {
        cwd: cwd.to_path_buf(),
        agent_home: agent_home.map(Path::to_path_buf),
        mode,
        provider: parsed.provider.clone(),
        model: parsed.model.clone(),
        api_key: parsed.api_key.clone(),
        thinking: parsed.thinking,
        models: parsed
            .models
            .iter()
            .filter(|m| !m.is_empty())
            .cloned()
            .collect(),
        system_prompt: parsed.system_prompt.clone(),
        append_system_prompt: parsed.append_system_prompt.clone(),
        tools: parsed.tools.clone(),
        no_tools: parsed.no_tools,
        no_builtin_tools: parsed.no_builtin_tools,
        no_memory: parsed.no_memory,
        extensions: paths(&parsed.extensions),
        no_extensions: parsed.no_extensions,
        extension_flags: parsed.unknown_flags.clone(),
        skills: paths(&parsed.skills),
        no_skills: parsed.no_skills,
        prompt_templates: paths(&parsed.prompt_templates),
        no_prompt_templates: parsed.no_prompt_templates,
        themes: paths(&parsed.themes),
        no_themes: parsed.no_themes,
        no_context_files: parsed.no_context_files,
        discover_local_models: parsed.local_model_dirs.iter().map(PathBuf::from).collect(),
        offline: parsed.offline,
        session_dir: parsed
            .session_dir
            .as_ref()
            .map(|d| PathBuf::from(resolve_path(cwd, d))),
        no_session: parsed.no_session,
        verbose: parsed.verbose,
        startup_migrations: false,
        child_executable: None,
        web: None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn p(args: &[&str]) -> CliArgs {
        parse(&args.iter().map(|s| s.to_string()).collect::<Vec<_>>())
    }

    #[test]
    fn value_options_accept_dash_values_and_repeat() {
        let a = p(&[
            "--model",
            "openai/gpt-4o:high",
            "-t",
            "read, grep",
            "--tools",
            "ls",
            "-e",
            "a.rhai",
            "--extension=b.rhai",
            "--append-system-prompt",
            "-x",
            "--append-system-prompt",
            "y",
            "--models",
            "a,,b",
            "--discover-local-models",
            "m1,m2",
            "--thinking",
            "xhigh",
        ]);
        assert_eq!(a.model.as_deref(), Some("openai/gpt-4o:high"));
        assert_eq!(a.tools, Some(vec!["ls".to_string()]));
        assert_eq!(a.extensions, ["a.rhai", "b.rhai"]);
        assert_eq!(a.append_system_prompt, ["-x", "y"]);
        assert_eq!(a.models, ["a", "", "b"]);
        assert_eq!(a.local_model_dirs, ["m1", "m2"]);
        assert_eq!(a.thinking, Some(ThinkingLevel::XHigh));
        assert!(a.diagnostics.is_empty());
    }

    #[test]
    fn messages_files_flags_and_unknowns() {
        let a = p(&[
            "-p",
            "---literal",
            "@notes.md",
            "-nt",
            "-nbt",
            "-ne",
            "-ns",
            "-np",
            "-nc",
            "--plan",
            "--mode",
            "json",
            "--level=3",
            "--depth",
            "2",
            "-z",
            "hello",
        ]);
        assert!(a.print && a.no_tools && a.no_builtin_tools && a.no_extensions);
        assert!(a.no_skills && a.no_prompt_templates && a.no_context_files);
        assert_eq!(a.messages, ["---literal", "hello"]);
        assert_eq!(a.file_args, ["notes.md"]);
        assert_eq!(a.unknown_flags, ["--plan", "--level=3", "--depth", "2"]);
        assert_eq!(a.mode.as_deref(), Some("json"));
        assert_eq!(
            a.diagnostics,
            [(Severity::Error, "Unknown option: -z".to_string())]
        );
        assert_eq!(startup_mode(&a, true), StartupMode::Json);
    }

    #[test]
    fn list_models_search_and_invalid_values() {
        let a = p(&[
            "--list-models",
            "sonnet",
            "--thinking",
            "huge",
            "--log-level",
            "loud",
        ]);
        assert_eq!(a.list_models.as_deref(), Some("sonnet"));
        assert!(!a.list_models_all);
        assert_eq!(a.diagnostics.len(), 2);
        assert!(
            a.diagnostics[0]
                .1
                .starts_with("Invalid thinking level \"huge\"")
        );
        let b = p(&["--list-models", "--offline"]);
        assert!(b.list_models_all && b.offline);
        let c = p(&["--model"]);
        assert_eq!(c.unknown_flags, ["--model"]);
        assert_eq!(startup_mode(&p(&[]), false), StartupMode::Print);
        assert_eq!(startup_mode(&p(&[]), true), StartupMode::Interactive);
        assert_eq!(startup_mode(&p(&["--mode", "rpc"]), true), StartupMode::Rpc);
    }

    #[test]
    fn runtime_options_carry_every_flag() {
        let a = p(&[
            "--provider",
            "ollama",
            "--model",
            "qwen:high",
            "--api-key",
            "k",
            "--thinking",
            "low",
            "--models",
            "a,,b",
            "--system-prompt",
            "S",
            "--append-system-prompt",
            "A",
            "--tools",
            "read,ls",
            "-nt",
            "-nbt",
            "--no-memory",
            "-e",
            "ext.rhai",
            "-ne",
            "--skill",
            "/abs/skill",
            "-ns",
            "--prompt-template",
            "t",
            "-np",
            "--theme",
            "th",
            "--no-themes",
            "-nc",
            "--discover-local-models",
            "m",
            "--offline",
            "--session-dir",
            "s",
            "--no-session",
            "--verbose",
            "--plan",
        ]);
        let o = runtime_options(&a, RunMode::Interactive, Path::new("/w"), None);
        assert_eq!(o.mode, RunMode::Interactive);
        assert_eq!(o.cwd, Path::new("/w"));
        assert_eq!(o.agent_home, None);
        assert_eq!(o.provider.as_deref(), Some("ollama"));
        assert_eq!(o.model.as_deref(), Some("qwen:high"));
        assert_eq!(o.api_key.as_deref(), Some("k"));
        assert_eq!(o.thinking, Some(ThinkingLevel::Low));
        assert_eq!(o.models, ["a", "b"]);
        assert_eq!(o.system_prompt.as_deref(), Some("S"));
        assert_eq!(o.append_system_prompt, ["A"]);
        assert_eq!(o.tools, Some(vec!["read".to_string(), "ls".to_string()]));
        assert!(o.no_tools && o.no_builtin_tools && o.no_memory && o.no_extensions);
        assert!(o.no_skills && o.no_prompt_templates && o.no_themes && o.no_context_files);
        assert!(o.offline && o.no_session && o.verbose);
        assert_eq!(o.extensions, ["/w/ext.rhai"]);
        assert_eq!(o.skills, ["/abs/skill"]);
        assert_eq!(o.prompt_templates, ["/w/t"]);
        assert_eq!(o.themes, ["/w/th"]);
        assert_eq!(o.discover_local_models, [PathBuf::from("m")]);
        assert_eq!(o.session_dir, Some(PathBuf::from("/w/s")));
        assert_eq!(o.extension_flags, ["--plan"]);
    }

    #[test]
    fn startup_checks() {
        assert_eq!(
            p(&["-p", "--mode", "json", "--export", "x", "--list-models"]).print_only_flags(),
            ["--print", "--mode", "--export", "--list-models"]
        );
        assert!(p(&["hi"]).print_only_flags().is_empty());
        assert!(p(&["--api-key", "k"]).api_key_error().is_some());
        assert!(
            p(&["--api-key", "k", "--models", "a"])
                .api_key_error()
                .is_none()
        );
        assert_eq!(
            p(&["--fork", "x", "-c", "--no-session"])
                .fork_error()
                .unwrap(),
            "--fork cannot be combined with --continue, --no-session"
        );
        assert!(p(&["--fork", "x"]).fork_error().is_none());
    }
}
