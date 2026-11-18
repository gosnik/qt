//! Print and JSON modes (archived `run_print_or_json_mode`), `--export`,
//! `qt-cli memory` and the runtime options built from the flags.

use std::io::{IsTerminal as _, Read as _, Write as _};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Duration;

use async_channel::Receiver;
use serde_json::Value;

use qt_core::agent::AgentEvent;
use qt_core::config::AgentHome;
use qt_core::engine::{Conversation, Delivery};
use qt_core::events_json::{PiEventSerializer, session_header_json};
use qt_core::input::DiagnosticSeverity;
use qt_core::message::{ContentBlock, Message, StopReason};
use qt_runtime::{AttachOptions, RunMode, Runtime, RuntimeOptions};

use crate::args::{CliArgs, StartupMode};
use crate::resolve;
use crate::session::{self, StartupStop};

/// How long shutdown waits for memory capture of the finished runs.
const CAPTURE_FLUSH: Duration = Duration::from_secs(30);

/// Runtime options from the parsed flags.
pub fn runtime_options(
    parsed: &CliArgs,
    mode: StartupMode,
    cwd: &Path,
    home: &AgentHome,
) -> RuntimeOptions {
    let paths = |values: &[String]| values.iter().map(|v| resolve(cwd, v)).collect::<Vec<_>>();
    RuntimeOptions {
        cwd: cwd.to_path_buf(),
        agent_home: Some(home.root().to_path_buf()),
        mode: match mode {
            StartupMode::Json => RunMode::Json,
            StartupMode::Rpc => RunMode::Rpc,
            _ => RunMode::Print,
        },
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
            .map(|d| PathBuf::from(resolve(cwd, d))),
        no_session: parsed.no_session,
        startup_migrations: false,
        child_executable: None,
        web: None,
    }
}

/// Archived Dreaming-verifier guard: the verification child must run the
/// built-in llama provider without tools, extensions or skills.
pub fn check_memory_child(parsed: &CliArgs) -> Result<(), String> {
    if !std::env::var("QT_MEMORY_DREAM_VERIFY").is_ok_and(|v| !v.is_empty() && v != "0") {
        return Ok(());
    }
    let provider = parsed.provider.as_deref().or_else(|| {
        parsed
            .model
            .as_deref()
            .and_then(|m| m.split_once('/').map(|(p, _)| p))
    });
    let restricted = parsed.no_tools && parsed.no_extensions && parsed.no_skills;
    if provider != Some(qt_llama::PROVIDER_ID) || parsed.model.is_none() || !restricted {
        return Err(
            "Dream verification requires the built-in llama provider with no tools, extensions, or skills"
                .into(),
        );
    }
    Ok(())
}

/// `--export <session> [output]`.
pub fn export(input: &str, output: Option<&str>, cwd: &Path) -> i32 {
    let source = PathBuf::from(resolve(cwd, input));
    if !source.exists() {
        eprintln!("Error: File not found: {input}");
        return 1;
    }
    let session = match qt_core::session::Session::open(&source) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("Error: {e:#}");
            return 1;
        }
    };
    let html = qt_core::export::render_session_html(
        session.header(),
        session.entries(),
        &source.display().to_string(),
    );
    let target = match output {
        Some(o) => PathBuf::from(resolve(cwd, o)),
        None => cwd.join(qt_core::export::default_html_filename(&source)),
    };
    match qt_core::export::write_export(&target, &html) {
        Ok(()) => {
            println!("Exported to: {}", target.display());
            0
        }
        Err(e) => {
            eprintln!("Error: {e:#}");
            1
        }
    }
}

/// `qt-cli memory <args>` through the qt-memory command library.
pub fn memory_command(args: &[String], cwd: &Path, home: &AgentHome) -> i32 {
    let layered = qt_core::settings_layers::LayeredSettings::load(home, cwd);
    let mut warnings = Vec::new();
    let settings = qt_memory::settings::resolve_memory_settings(
        &Value::Object(layered.global.raw().clone()),
        (!layered.project.is_empty())
            .then(|| Value::Object(layered.project.clone()))
            .as_ref(),
    )
    .unwrap_or_else(|e| {
        warnings.push(format!("memory settings ignored: {e}"));
        qt_memory::settings::MemorySettings::default()
    });
    let index = qt_index::IndexManager::new(home.root());
    let code = settings.retrieval.code_enabled.then(|| {
        Arc::new(qt_runtime::code::IndexCodeSnapshots::new(
            index.service_for(cwd),
        )) as Arc<dyn qt_memory::code::CodeSnapshotProvider>
    });
    let effective = layered.effective();
    let llama = Arc::new(qt_llama::LlamaProviderBackend::new(home.clone()));
    let llama_settings = llama.effective_settings(&effective);
    let embedder = qt_runtime::memory::dense_embedder(&settings, &llama_settings, llama, effective);
    let environment = qt_memory::cli::MemoryCommandEnvironment {
        cwd: cwd.to_path_buf(),
        agent_home: home.root().to_path_buf(),
        settings,
        code,
        embedder,
        retrieval_warnings: warnings,
    };
    let output = qt_memory::cli::run_memory_command(args, &environment);
    print!("{}", output.stdout);
    eprint!("{}", output.stderr);
    output.exit_code
}

fn read_stdin() -> Option<String> {
    let mut content = String::new();
    std::io::stdin().read_to_string(&mut content).ok()?;
    let trimmed = content.trim();
    (!trimmed.is_empty()).then(|| trimmed.to_string())
}

/// Output of one run.
struct RunResult {
    ok: bool,
    error: Option<String>,
    final_text: String,
}

fn final_text(message: &qt_core::message::AssistantMessage) -> String {
    message
        .content
        .iter()
        .filter_map(|b| match b {
            ContentBlock::Text { text } => Some(text.as_str()),
            _ => None,
        })
        .collect::<Vec<_>>()
        .join("\n")
}

/// Consume a run's events, printing Pi JSON lines in JSON mode.
fn consume(
    runtime: &Runtime,
    conversation: &Conversation,
    rx: Receiver<AgentEvent>,
    json: bool,
) -> RunResult {
    let mut serializer = PiEventSerializer::new();
    if let Some(model) = conversation.model() {
        let api = runtime
            .engine()
            .provider(&model.provider)
            .map(|p| p.model_spec(&model.id).api.clone())
            .filter(|a| !a.is_empty())
            .or_else(|| {
                runtime
                    .engine()
                    .provider(&model.provider)
                    .map(|p| p.kind.api_name().to_string())
            })
            .unwrap_or_default();
        serializer.set_model(&model.provider, &model.id, &api);
    }
    let mut last: Option<qt_core::message::AssistantMessage> = None;
    let mut warnings = Vec::new();
    let mut stdout = std::io::stdout();
    runtime.engine().block_on(async {
        while let Ok(event) = rx.recv().await {
            if json {
                let lines = serializer.lines(&event);
                if !lines.is_empty() {
                    let _ = stdout.write_all(lines.as_bytes());
                    let _ = stdout.flush();
                }
            }
            match &event {
                AgentEvent::MessageEnd(Message::Assistant(a)) => last = Some(a.clone()),
                AgentEvent::Warning(w) => warnings.push(w.clone()),
                _ => {}
            }
        }
    });
    if !json {
        for w in &warnings {
            eprintln!("Warning: {w}");
        }
    }
    match last {
        Some(a) if matches!(a.stop_reason, StopReason::Error | StopReason::Aborted) => RunResult {
            ok: false,
            error: Some(
                a.error_message
                    .clone()
                    .unwrap_or_else(|| "Request was aborted".into()),
            ),
            final_text: String::new(),
        },
        Some(a) => RunResult {
            ok: true,
            error: None,
            final_text: final_text(&a),
        },
        None => RunResult {
            ok: false,
            error: warnings
                .first()
                .cloned()
                .or_else(|| Some("no response".into())),
            final_text: String::new(),
        },
    }
}

/// Print / JSON mode.
pub fn main(
    parsed: CliArgs,
    mode: StartupMode,
    stdin_is_tty: bool,
    options: RuntimeOptions,
) -> i32 {
    let runtime = match Runtime::build(options) {
        Ok(r) => r,
        Err(e) => {
            eprintln!("Error: {e:#}");
            return 1;
        }
    };
    for d in runtime.diagnostics() {
        tracing::warn!(target: "qt_cli", "{d}");
    }
    let code = run_with(&runtime, parsed, mode, stdin_is_tty);
    runtime.shutdown(CAPTURE_FLUSH);
    code
}

fn run_with(runtime: &Runtime, parsed: CliArgs, mode: StartupMode, stdin_is_tty: bool) -> i32 {
    let engine = runtime.engine();
    let interactive = mode == StartupMode::Interactive && std::io::stdout().is_terminal();
    let conversation = match session::open(engine, &parsed, runtime.startup_model(), interactive) {
        Ok(c) => c,
        Err(StartupStop::Message(text, code)) => {
            if !text.is_empty() {
                println!("{text}");
            }
            return code;
        }
        Err(StartupStop::Error(text)) => {
            eprintln!("{text}");
            return 1;
        }
    };
    runtime.apply_startup_selection(&conversation);
    let (sink_tx, sink_rx) = async_channel::unbounded::<Receiver<AgentEvent>>();
    runtime.attach(
        &conversation,
        AttachOptions {
            run_sink: Some(Arc::new(move |rx| {
                let _ = sink_tx.try_send(rx);
            })),
            deliver_subagent_events: false,
        },
    );
    runtime.connect_mcp();

    let stdin = if stdin_is_tty { None } else { read_stdin() };
    let json = mode == StartupMode::Json;
    if mode == StartupMode::Interactive
        && stdin.is_none()
        && parsed.messages.is_empty()
        && runtime.extra_messages().is_empty()
        && parsed.file_args.is_empty()
    {
        eprintln!(
            "qt-cli: no prompt given. The interactive interface is the qt desktop app (qt); run qt-cli -p \"<prompt>\" or pipe a prompt on stdin."
        );
        return 1;
    }
    if json {
        let header = session_header_json(&conversation.session_header());
        println!("{header}");
    }

    // The initial message is stdin + @files + the first message; the other
    // messages follow one by one (archived `prepare_initial_message`).
    let mut remaining: std::collections::VecDeque<String> = parsed
        .messages
        .iter()
        .chain(runtime.extra_messages())
        .cloned()
        .collect();
    let mut initial = Some(remaining.pop_front().unwrap_or_default())
        .filter(|first| !first.is_empty() || stdin.is_some() || !parsed.file_args.is_empty());
    let mut deferred = String::new();
    loop {
        let started = if let Some(first) = initial.take() {
            conversation.prompt_with_files(&first, &parsed.file_args, stdin.as_deref())
        } else if let Some(text) = remaining.pop_front() {
            conversation
                .prompt(&text, Vec::new())
                .map(|rx| (rx, Vec::new()))
        } else {
            break;
        };
        let (rx, diagnostics) = match started {
            Ok(v) => v,
            Err(e) => {
                eprintln!("qt: {e:#}");
                return 1;
            }
        };
        for d in diagnostics {
            let label = if d.severity == DiagnosticSeverity::Error {
                "Error"
            } else {
                "Warning"
            };
            eprintln!("{label}: {}", d.message);
        }
        let result = consume(runtime, &conversation, rx, json);
        drain_sink(runtime, &conversation, &sink_rx, json);
        if !result.ok {
            if !json && let Some(error) = result.error {
                eprintln!("error: {error}");
            }
            return 1;
        }
        deferred = result.final_text;
    }

    // Persistent workers: wait for their completions and run them as
    // follow-ups (JSON mode prints `subagent_event` frames).
    while let Some(coordinator) = runtime.subagents().coordinator() {
        let mut events = coordinator.take_events(usize::MAX);
        while events.is_empty() && coordinator.has_active_workers() {
            let waiter = coordinator.clone();
            if let Err(e) =
                engine.block_on(async move { waiter.wait("", Duration::from_millis(1000)).await })
            {
                eprintln!("qt: {e}");
                return 1;
            }
            events = coordinator.take_events(usize::MAX);
        }
        if events.is_empty() {
            break;
        }
        for event in events {
            if json {
                println!("{}", qt_subagents::delivery::event_frame(&event));
            }
            if !qt_subagents::delivery::is_completion(&event) {
                continue;
            }
            let message = qt_subagents::event_message(&event);
            match conversation.send_custom_message(message, Delivery::FollowUp, true) {
                Ok(Some(rx)) => {
                    let result = consume(runtime, &conversation, rx, json);
                    if !result.ok {
                        if !json && let Some(error) = result.error {
                            eprintln!("error: {error}");
                        }
                        return 1;
                    }
                    deferred = result.final_text;
                }
                Ok(None) => {}
                Err(e) => {
                    eprintln!("qt: {e:#}");
                    return 1;
                }
            }
        }
    }
    drain_sink(runtime, &conversation, &sink_rx, json);
    if !json && !deferred.is_empty() {
        println!("{deferred}");
    }
    0
}

/// Runs started by extensions while a prompt ran.
fn drain_sink(
    runtime: &Runtime,
    conversation: &Conversation,
    sink: &Receiver<Receiver<AgentEvent>>,
    json: bool,
) {
    while let Ok(rx) = sink.try_recv() {
        let _ = consume(runtime, conversation, rx, json);
    }
}
