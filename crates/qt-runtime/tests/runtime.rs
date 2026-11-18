//! Composition-root checks that do not need a model server.

use std::path::Path;
use std::time::Duration;

use qt_core::slash::SlashCommandSource;
use qt_runtime::{AttachOptions, RunMode, Runtime, RuntimeOptions};
use serde_json::json;

fn setup(
    settings: serde_json::Value,
) -> (tempfile::TempDir, std::path::PathBuf, std::path::PathBuf) {
    let dir = tempfile::tempdir().unwrap();
    let home = dir.path().join("home");
    let work = dir.path().join("work");
    std::fs::create_dir_all(&home).unwrap();
    std::fs::create_dir_all(&work).unwrap();
    std::fs::write(home.join("settings.json"), settings.to_string()).unwrap();
    (dir, home, work)
}

fn options(home: &Path, work: &Path, mode: RunMode) -> RuntimeOptions {
    RuntimeOptions {
        agent_home: Some(home.to_path_buf()),
        mode,
        child_executable: Some("/nonexistent/qt-cli".into()),
        ..RuntimeOptions::new(work)
    }
}

const SCRIPT: &str = r#"
fn activate() {
    registerCommand("hello", "Hello", "Say hello");
    registerTool(#{ name: "echo_ext", description: "Echo" });
    registerFlag("plan", "boolean", false, "Plan mode");
}
fn command_hello(args) { #{ text: "hello " + args.args } }
fn tool_echo_ext(args) { "ok" }
"#;

#[test]
fn builds_every_subsystem_and_binds_conversations() {
    let (_dir, home, work) = setup(json!({
        "memory": {"enabled": true},
        "ollama": {"instances": [{"id": "mock", "baseUrl": "http://127.0.0.1:1", "models": [{"name": "m"}]}]},
    }));
    std::fs::write(work.join("ext.rhai"), SCRIPT).unwrap();
    let mut opts = options(&home, &work, RunMode::Print);
    opts.extensions = vec![work.join("ext.rhai").to_string_lossy().into_owned()];
    opts.extension_flags = vec!["--plan".into(), "message".into()];
    opts.model = Some("ollama:mock/m:high".into());
    let runtime = Runtime::build(opts).unwrap();
    assert_eq!(runtime.extra_messages(), ["message"]);
    assert_eq!(runtime.extensions().flag_value("plan"), json!(true));

    let names = runtime.tool_names();
    for expected in [
        "read",
        "index",
        "symbol",
        "subagent",
        "memory_recall",
        "echo_ext",
    ] {
        assert!(
            names.iter().any(|n| n == expected),
            "missing {expected}: {names:?}"
        );
    }
    let registry = runtime.engine().slash_commands();
    let hello = registry
        .find("hello")
        .expect("extension command registered");
    assert_eq!(hello.source, SlashCommandSource::Extension);
    assert_eq!(
        runtime.run_extension_command("hello", "you").unwrap(),
        ["hello you"]
    );
    assert_eq!(runtime.engine().extensions_info().len(), 1);

    let (model, thinking) = runtime.selected_model().unwrap();
    assert_eq!(model.to_string(), "ollama:mock/m");
    assert_eq!(thinking, Some(qt_core::provider::ThinkingLevel::High));

    let conversation = runtime
        .engine()
        .new_ephemeral_conversation(runtime.startup_model());
    runtime.apply_startup_selection(&conversation);
    runtime.attach(&conversation, AttachOptions::default());
    assert!(runtime.subagents().coordinator().is_some());
    assert_eq!(
        runtime.active_conversation().unwrap().session_id(),
        conversation.session_id()
    );
    // /context memory comes from the memory runtime.
    let lines = runtime.engine().memory_context_lines().unwrap();
    assert_eq!(
        lines,
        ["- no augmentation has been prepared in this projection"]
    );
    runtime.shutdown(Duration::from_secs(5));
}

#[test]
fn interactive_runtimes_bind_new_conversations_and_respect_flags() {
    let (_dir, home, work) = setup(json!({}));
    let mut opts = options(&home, &work, RunMode::Interactive);
    opts.no_builtin_tools = true;
    opts.no_memory = true;
    let runtime = Runtime::build(opts).unwrap();
    assert!(runtime.memory().is_none());
    assert!(
        runtime.tool_names().is_empty(),
        "{:?}",
        runtime.tool_names()
    );
    let conversation = runtime.engine().new_ephemeral_conversation(None);
    assert_eq!(
        runtime.active_conversation().unwrap().session_id(),
        conversation.session_id(),
        "the conversation listener attached it"
    );
    runtime.start_background_services();
    runtime.start_background_services();
    assert!(runtime.index_service().status().running);
    runtime.shutdown(Duration::from_secs(5));

    let mut bad = options(&home, &work, RunMode::Print);
    bad.extension_flags = vec!["--nope".into()];
    assert_eq!(
        Runtime::build(bad).unwrap_err().to_string(),
        "Unknown option: --nope"
    );
    let mut bad = options(&home, &work, RunMode::Print);
    bad.model = Some("nothing-like-this".into());
    assert!(
        Runtime::build(bad)
            .unwrap_err()
            .to_string()
            .starts_with("unknown model selection: nothing-like-this")
    );
}
