//! `/llama` commands against the real in-process llama.cpp.
//!
//! Uses `qwen2.5-3b-instruct-q4_k_m.gguf` (else `qwen2.5-0.5b-instruct-q8_0.gguf`,
//! which rarely follows the tool protocol, so the tool-call assertions need
//! the 3B model) from `QT_LLAMA_TEST_MODELS` (a directory, default
//! `<workspace>/models`); skips with a message when llama.cpp is not linked
//! or no model is present. The test takes about a minute on the CPU
//! (`gpuLayers: 0`), so it only runs when `QT_LLAMA_COMMAND_TESTS=1`; with
//! Vulkan builds set `GGML_VK_VISIBLE_DEVICES=` to keep llama.cpp off a busy
//! GPU. Run with `--nocapture` to see the transcripts.

use std::path::PathBuf;
use std::time::Duration;

use qt_runtime::{RunMode, Runtime, RuntimeOptions};
use serde_json::json;

const MODELS: [&str; 2] = [
    "qwen2.5-3b-instruct-q4_k_m.gguf",
    "qwen2.5-0.5b-instruct-q8_0.gguf",
];

fn model_path() -> Option<PathBuf> {
    if std::env::var_os("QT_LLAMA_COMMAND_TESTS").is_none_or(|v| v != "1") {
        eprintln!("skipping: set QT_LLAMA_COMMAND_TESTS=1 to run");
        return None;
    }
    if !qt_llama::native_available() {
        eprintln!("skipping: llama.cpp is not linked in this build");
        return None;
    }
    let dir = std::env::var_os("QT_LLAMA_TEST_MODELS")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../models"));
    let found = MODELS.iter().map(|m| dir.join(m)).find(|p| p.is_file());
    if found.is_none() {
        eprintln!("skipping: no test model in {}", dir.display());
    }
    found
}

fn run(runtime: &Runtime, command: &str) -> Vec<String> {
    let lines = runtime.llama_command(command);
    eprintln!("> /llama {command}");
    for line in &lines {
        eprintln!("  {line}");
    }
    lines
}

#[test]
fn grammar_overrides_and_instruct_tool_loop() {
    let Some(model) = model_path() else {
        return;
    };
    let dir = tempfile::tempdir().unwrap();
    let home = dir.path().join("home");
    let work = dir.path().join("work");
    std::fs::create_dir_all(&home).unwrap();
    std::fs::create_dir_all(work.join("grammars")).unwrap();
    std::fs::write(work.join("notes.txt"), "the secret word is banana\n").unwrap();
    std::fs::write(
        work.join("grammars/fruit.gbnf"),
        "start ::= \"banana\" | \"cherry\"\n",
    )
    .unwrap();
    std::fs::write(dir.path().join("outside.gbnf"), "root ::= \"x\"\n").unwrap();
    std::fs::write(
        home.join("settings.json"),
        json!({"llama": {
            "backend": {"gpuLayers": 0},
            "context": {"contextSize": 4096},
            "selectedModel": "qwen",
            "generation": {"temperature": 0.0, "seed": 1},
            "models": [{"id": "qwen", "name": "qwen", "path": model.to_string_lossy()}]
        }})
        .to_string(),
    )
    .unwrap();
    let runtime = Runtime::build(RuntimeOptions {
        agent_home: Some(home.clone()),
        mode: RunMode::Print,
        child_executable: Some("/nonexistent/qt-cli".into()),
        no_memory: true,
        // The prompt carries every offered tool schema; three keep it small.
        tools: Some(vec!["read".into(), "ls".into(), "grep".into()]),
        ..RuntimeOptions::new(&work)
    })
    .unwrap();

    assert_eq!(
        run(&runtime, "grammar")[1],
        "runtime_override: null",
        "no override at start"
    );
    assert_eq!(
        run(&runtime, r#"grammar inline root ::= "yes" | "no""#),
        [
            "Llama inline grammar configured for subsequent turns.",
            "root: root"
        ]
    );
    assert_eq!(
        run(&runtime, "grammar show")[1],
        r#"runtime_override: {"inline":"root ::= \"yes\" | \"no\"","root":"root"}"#
    );
    let answer = run(&runtime, "prompt Is the sky blue? Answer yes or no.");
    assert!(
        answer == ["yes"] || answer == ["no"],
        "inline grammar constrains the prompt: {answer:?}"
    );
    let status = run(&runtime, "status");
    assert!(status.contains(
        &r#"grammar_override: {"inline":"root ::= \"yes\" | \"no\"","root":"root"}"#.to_string()
    ));

    let canonical_work = work.canonicalize().unwrap();
    assert_eq!(
        run(&runtime, "grammar file grammars/fruit.gbnf start"),
        [
            "Llama file grammar configured for subsequent turns.".to_string(),
            format!(
                "path: {}",
                canonical_work.join("grammars/fruit.gbnf").display()
            ),
        ]
    );
    let fruit = run(&runtime, "run chat Name a yellow fruit.");
    assert_eq!(fruit[0], "instance: chat");
    assert!(
        fruit[1] == "banana" || fruit[1] == "cherry",
        "file grammar constrains tagged runs: {fruit:?}"
    );
    assert_eq!(
        run(&runtime, "grammar file ../outside.gbnf"),
        [
            "llama grammar path must be under cwd, workspace root, agent home, or lib/llama.cpp/grammars"
        ]
    );
    assert_eq!(
        run(&runtime, "grammar file"),
        ["Usage: /llama grammar file <path> [root]"]
    );
    assert_eq!(
        run(&runtime, "grammar inline"),
        ["Usage: /llama grammar inline <gbnf>"]
    );
    assert_eq!(
        run(&runtime, "grammar off"),
        ["Llama grammar constraints disabled for subsequent turns."]
    );
    assert_eq!(
        run(&runtime, "grammar")[1],
        r#"runtime_override: {"enabled":false}"#
    );

    assert_eq!(
        run(&runtime, "gpu useMmap=maybe"),
        ["useMmap must be a boolean"]
    );

    // One context at a time keeps the test within small GPUs' memory.
    assert_eq!(
        run(&runtime, "unload"),
        ["Released llama model and context resources."]
    );
    let instruct = run(
        &runtime,
        "run instruct What is the secret word in notes.txt? Read the file first.",
    );
    assert_eq!(instruct[0], "instance: instruct");
    let tool_count = runtime.engine().tools().len();
    assert_eq!(
        instruct.last().unwrap(),
        &format!("tools_available: {tool_count}")
    );
    if !model.ends_with(MODELS[0]) {
        runtime.shutdown(Duration::from_secs(5));
        return;
    }
    assert!(
        instruct.iter().any(|l| l.starts_with("tool_call: read ")),
        "the instruct run calls the read tool: {instruct:?}"
    );
    assert!(instruct.iter().any(|l| l == "tool_result: success"));
    assert!(
        instruct.iter().any(|l| l.contains("banana")),
        "the file content reaches the transcript: {instruct:?}"
    );

    runtime.shutdown(Duration::from_secs(5));
}
