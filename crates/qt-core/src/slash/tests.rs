use std::path::Path;

use serde_json::json;

use super::*;
use crate::config::AgentHome;
use crate::engine::{Conversation, Engine};
use crate::message::{AgentMessage, AssistantMessage, ContentBlock, Message, Usage};
use crate::provider::{ModelRef, ThinkingLevel};

struct Fixture {
    _dir: tempfile::TempDir,
    root: std::path::PathBuf,
    engine: Engine,
    conv: Conversation,
}

fn write(path: &Path, text: &str) {
    std::fs::create_dir_all(path.parent().unwrap()).unwrap();
    std::fs::write(path, text).unwrap();
}

fn fixture() -> Fixture {
    let dir = tempfile::tempdir().unwrap();
    let root = dir.path().canonicalize().unwrap();
    let work = root.join("work");
    std::fs::create_dir_all(work.join(".git")).unwrap();
    let home = AgentHome::new(root.join("home"));
    write(
        &home.settings_path(),
        r#"{"ollama":{"instances":[{"id":"local","baseUrl":"http://127.0.0.1:9","enabled":true}]}}"#,
    );
    write(
        &home.root().join("prompts/review.md"),
        "---\ndescription: Review code\nargument-hint: <file>\n---\nReview $1 carefully.",
    );
    write(&home.root().join("prompts/help.md"), "Shadowed help");
    write(
        &home.root().join("skills/pdf/SKILL.md"),
        "---\nname: pdf\ndescription: PDF tools\n---\nUse pdftotext.",
    );
    let engine = Engine::builder(home, &work)
        .changelog("# Changelog\n\n## 1.2.0\n- new\n\n## 1.1.0\n- old\n")
        .build()
        .unwrap();
    let conv = engine.new_conversation(Some(ModelRef::new("ollama:local", "mock")));
    Fixture {
        _dir: dir,
        root: work,
        engine,
        conv,
    }
}

fn lines(outcome: SlashOutcome) -> Vec<String> {
    match outcome {
        SlashOutcome::Output(lines) => lines,
        other => panic!("expected output, got {other:?}"),
    }
}

fn assistant(text: &str) -> AgentMessage {
    AgentMessage::Llm(Message::Assistant(AssistantMessage {
        content: vec![ContentBlock::text(text)],
        provider: "ollama:local".into(),
        model: "mock".into(),
        usage: Usage {
            input_tokens: 100,
            output_tokens: 20,
            total_cost_usd: 0.0125,
            ..Default::default()
        },
        ..Default::default()
    }))
}

#[test]
fn registry_order_precedence_and_diagnostics() {
    let f = fixture();
    f.engine.set_extension_commands(vec![
        SlashCommand::extension("deploy", "Deploy it", Some("/e/a.rhai".into())),
        SlashCommand::extension("deploy", "Deploy other", Some("/e/b.rhai".into())),
        SlashCommand::extension("status", "Extension status", None),
    ]);
    let registry = f.engine.slash_commands();
    let names: Vec<&str> = registry.commands.iter().map(|c| c.name.as_str()).collect();
    assert_eq!(&names[..3], ["deploy:1", "deploy:2", "status"]);
    assert_eq!(
        registry.find("/deploy:2").unwrap().target_name.as_deref(),
        Some("deploy")
    );
    // The extension took `/status`; the built-in is still listed after it but
    // `find` resolves to the extension (first match).
    assert_eq!(
        registry.find("status").unwrap().source,
        SlashCommandSource::Extension
    );
    assert!(names.contains(&"review"));
    assert!(names.contains(&"skill:pdf"));
    assert!(!names.contains(&"hotkeys"));
    assert_eq!(registry.diagnostics.len(), 1);
    assert!(registry.diagnostics[0].starts_with(
        "prompt command '/help' conflicts with an existing slash command and was skipped: "
    ));
    let builtins: Vec<String> = builtin_commands().into_iter().map(|c| c.name).collect();
    for name in [
        "help",
        "settings",
        "model",
        "scoped-models",
        "thinking",
        "export",
        "import",
        "share",
        "copy",
        "name",
        "session",
        "context",
        "status",
        "changelog",
        "new",
        "clone",
        "fork",
        "tree",
        "resume",
        "login",
        "logout",
        "tools",
        "extensions",
        "skills",
        "prompts",
        "themes",
        "reload",
        "quit",
        "goal",
        "follow-up",
        "compact",
        "llama",
        "ollama",
        "subagent",
        "index",
        "memory",
        "mcp",
    ] {
        assert!(builtins.iter().any(|b| b == name), "missing /{name}");
    }
}

#[test]
fn help_templates_skills_and_extensions_dispatch() {
    let f = fixture();
    let help = lines(dispatch(&f.conv, "/help mo").unwrap());
    assert_eq!(
        help,
        [
            "Available slash commands matching '/mo':",
            "/model [provider/model|search|refresh] [builtin] - Select model",
        ]
    );
    let none = lines(dispatch(&f.conv, "/help zz").unwrap());
    assert_eq!(none[1], "(no matching slash commands)");
    match dispatch(&f.conv, "/review src/lib.rs").unwrap() {
        SlashOutcome::Prompt(text) => assert_eq!(text, "Review src/lib.rs carefully."),
        other => panic!("{other:?}"),
    }
    match dispatch(&f.conv, "/skill:pdf convert a.pdf").unwrap() {
        SlashOutcome::Prompt(text) => assert_eq!(text, "Use pdftotext.\n\nUser: convert a.pdf"),
        other => panic!("{other:?}"),
    }
    f.engine
        .set_extension_commands(vec![SlashCommand::extension("hello", "Say hi", None)]);
    match dispatch(&f.conv, "/hello  world").unwrap() {
        SlashOutcome::Action(SlashAction::Extension {
            name,
            target,
            arguments,
            ..
        }) => assert_eq!(
            (name.as_str(), target.as_str(), arguments.as_str()),
            ("hello", "hello", "world")
        ),
        other => panic!("{other:?}"),
    }
    assert_eq!(
        dispatch(&f.conv, "/nope").unwrap_err().to_string(),
        "unknown slash command '/nope'"
    );
    assert_eq!(
        dispatch(&f.conv, "hello").unwrap_err().to_string(),
        "slash command input must start with '/'"
    );
    assert_eq!(
        parse_invocation("/  x").unwrap_err(),
        "slash command name is empty"
    );
    assert!(looks_like_command("/model x"));
    assert!(!looks_like_command("/usr/bin/env"));
}

#[test]
fn name_session_status_and_changelog_text() {
    let f = fixture();
    assert_eq!(
        lines(dispatch(&f.conv, "/name").unwrap()),
        ["Session name: (unnamed)"]
    );
    assert_eq!(
        lines(dispatch(&f.conv, "/name  Big refactor ").unwrap()),
        ["Session name set to Big refactor"]
    );
    f.conv
        .push_message_for_test(AgentMessage::Llm(Message::user_text("hi")));
    f.conv.push_message_for_test(assistant("Hello **there**"));
    let session = lines(dispatch(&f.conv, "/session").unwrap());
    assert_eq!(session[0], "name: Big refactor");
    assert_eq!(
        session[1],
        format!("file: {}", f.conv.session_path().display())
    );
    assert_eq!(session[2], format!("id: {}", f.conv.session_id()));
    assert_eq!(session[3], format!("cwd: {}", f.root.display()));
    assert_eq!(session[4], "model: ollama:local/mock");
    assert_eq!(session[5], "thinking: off");
    assert_eq!(
        session[6],
        "messages: user=1, assistant=1, tool_calls=0, tool_results=0, bash=0"
    );
    assert_eq!(session[7], "tokens: input=100, output=20, total=120");
    assert_eq!(session[8], "cost: $0.0125");
    let status = lines(dispatch(&f.conv, "/status").unwrap());
    assert_eq!(status[0], format!("cwd: {}", f.root.display()));
    assert_eq!(status[2], "model: ollama:local/mock");
    assert_eq!(status[4], "auth: unconfigured");
    assert_eq!(
        status[5],
        format!(
            "tools: {}, prompts: 2, skills: 1, extensions: 0",
            f.engine.tools().len()
        )
    );
    assert_eq!(
        lines(dispatch(&f.conv, "/changelog").unwrap()),
        ["## 1.2.0", "- new", ""]
    );
    assert_eq!(
        changelog_lines(None),
        ["No changelog entries are available."]
    );
}

#[test]
fn context_sections_and_unknown_section() {
    let f = fixture();
    f.conv
        .push_message_for_test(AgentMessage::Llm(Message::user_text("hi")));
    let all = lines(dispatch(&f.conv, "/context").unwrap());
    assert_eq!(
        all[0],
        "Context inspection (default is all available content)."
    );
    for heading in [
        "environment:",
        "resolved_session_messages: 1, text_bytes=2",
        "active_pending_messages: 0",
        "last_provider_payload: unavailable (no completed provider request in this runtime)",
        "runtime_diagnostics:",
        "instruction_load_hooks_planned: 0",
        "- memory runtime is unavailable",
        "model_usage: input=0, output=0, total=0",
        "model_context_usage: unavailable (no model context-window estimate)",
    ] {
        assert!(all.iter().any(|l| l == heading), "missing {heading}");
    }
    assert!(
        all.iter()
            .any(|l| l.starts_with("assembled_system_prompt ("))
    );
    assert!(
        all.iter()
            .any(|l| l == "- no project context sections loaded")
    );
    assert!(all.iter().any(|l| l.starts_with("tool read (active):")));
    assert!(all.iter().any(|l| l.starts_with("skill pdf ")));
    let env = lines(dispatch(&f.conv, "/context environment").unwrap());
    assert_eq!(env[2], "environment:");
    assert_eq!(env[3], format!("- cwd: {}", f.root.display()));
    assert_eq!(env[4], format!("- repository_root: {}", f.root.display()));
    assert!(!env.iter().any(|l| l.starts_with("assembled_system_prompt")));
    f.engine.set_memory_context(Some(std::sync::Arc::new(|| {
        vec!["- memory_ids: []".into()]
    })));
    let memory = lines(dispatch(&f.conv, "/context memory, usage").unwrap());
    assert!(memory.contains(&"- memory_ids: []".to_string()));
    assert_eq!(
        lines(dispatch(&f.conv, "/context bogus").unwrap()),
        [
            "Unknown /context section: bogus",
            "Use /context [all|environment|prompt|messages|resources|diagnostics|memory|usage]."
        ]
    );
}

#[test]
fn thinking_model_tools_and_actions() {
    let f = fixture();
    assert_eq!(
        dispatch(&f.conv, "/thinking turbo")
            .unwrap_err()
            .to_string(),
        "unsupported thinking level: turbo"
    );
    let set = lines(dispatch(&f.conv, "/thinking high").unwrap());
    assert_eq!(set.len(), 1);
    assert!(set[0].starts_with("Thinking level set to "));
    match dispatch(&f.conv, "/thinking").unwrap() {
        SlashOutcome::Action(SlashAction::SelectThinking { levels, .. }) => {
            assert!(!levels.is_empty())
        }
        other => panic!("{other:?}"),
    }
    f.engine.remember_models(&[crate::provider::ModelInfo {
        provider: "ollama:local".into(),
        id: "qwen3".into(),
        name: "qwen3".into(),
        ..Default::default()
    }]);
    assert_eq!(
        lines(dispatch(&f.conv, "/model ollama:local/qwen3").unwrap()),
        ["Model set to ollama:local/qwen3"]
    );
    assert_eq!(f.conv.model(), Some(ModelRef::new("ollama:local", "qwen3")));
    assert_eq!(
        f.engine.settings().default_model().as_deref(),
        Some("qwen3")
    );
    let completions = argument_completions(&f.engine, "model", "qwen");
    assert!(completions.iter().any(|c| c.value == "ollama:local/qwen3"));
    match dispatch(&f.conv, "/model gpt").unwrap() {
        SlashOutcome::Action(SlashAction::SelectModel { search }) => {
            assert_eq!(search.as_deref(), Some("gpt"))
        }
        other => panic!("{other:?}"),
    }
    match dispatch(&f.conv, "/tools ls").unwrap() {
        SlashOutcome::Action(SlashAction::SelectTools { tools, .. }) => {
            assert_eq!(tools.len(), 1);
            assert_eq!(tools[0].0, "ls");
            assert!(tools[0].2);
        }
        other => panic!("{other:?}"),
    }
    f.conv.set_tool_enabled("ls", false).unwrap();
    assert!(!f.conv.active_tools().contains(&"ls".to_string()));
    assert_eq!(
        lines(dispatch(&f.conv, "/tools zzz").unwrap()),
        ["No tools matched."]
    );
    match dispatch(&f.conv, "/settings skills pdf").unwrap() {
        SlashOutcome::Action(SlashAction::OpenPackageResources { kind, filter }) => {
            assert_eq!((kind.as_str(), filter.as_deref()), ("skills", Some("pdf")))
        }
        other => panic!("{other:?}"),
    }
    match dispatch(&f.conv, "/mcp reconnect fs").unwrap() {
        SlashOutcome::Action(SlashAction::Subsystem { command, arguments }) => {
            assert_eq!(
                (command.as_str(), arguments.as_str()),
                ("mcp", "reconnect fs")
            )
        }
        other => panic!("{other:?}"),
    }
    assert!(matches!(
        dispatch(&f.conv, "/quit").unwrap(),
        SlashOutcome::Action(SlashAction::Quit)
    ));
    assert_eq!(
        dispatch(&f.conv, "/follow-up").unwrap_err().to_string(),
        "follow-up requires message text"
    );
    assert_eq!(
        dispatch(&f.conv, "/follow-up later")
            .unwrap_err()
            .to_string(),
        "follow-up requires a running turn"
    );
    assert!(f.conv.thinking() != ThinkingLevel::Off);
    assert_eq!(
        argument_completions(&f.engine, "thinking", "h")
            .iter()
            .map(|c| c.value.as_str())
            .collect::<Vec<_>>(),
        ["high", "xhigh"]
    );
    assert_eq!(argument_completions(&f.engine, "ollama", "").len(), 12);
    assert_eq!(
        argument_completions(&f.engine, "ollama", "pull lo")[0].value,
        "pull local"
    );
    assert_eq!(
        argument_completions(&f.engine, "llama", "run ch")[0].value,
        "run chat"
    );
    assert_eq!(
        f.engine.slash_commands().command_completions("/co").len(),
        3
    );
}

#[test]
fn export_copy_new_clone_and_import() {
    let f = fixture();
    assert_eq!(
        lines(dispatch(&f.conv, "/copy").unwrap()),
        ["No assistant text is available yet."]
    );
    f.conv
        .push_message_for_test(AgentMessage::Llm(Message::user_text("hi")));
    f.conv.push_message_for_test(assistant("The answer"));
    match dispatch(&f.conv, "/copy").unwrap() {
        SlashOutcome::Copy { text, lines } => {
            assert_eq!(text, "The answer");
            assert_eq!(
                lines,
                ["Copied the last assistant message to the clipboard."]
            );
        }
        other => panic!("{other:?}"),
    }
    let html = lines(dispatch(&f.conv, "/export").unwrap());
    let expected = f
        .root
        .join(crate::export::default_html_filename(&f.conv.session_path()));
    assert_eq!(html, [format!("Exported HTML to {}", expected.display())]);
    assert!(
        std::fs::read_to_string(&expected)
            .unwrap()
            .contains("The answer")
    );
    assert_eq!(
        lines(dispatch(&f.conv, "/export out/s.jsonl").unwrap()),
        ["Exported JSONL to out/s.jsonl"]
    );
    let jsonl = std::fs::read_to_string(f.root.join("out/s.jsonl")).unwrap();
    assert_eq!(jsonl.lines().count(), 1 + f.conv.entries().len());
    assert_eq!(
        dispatch(&f.conv, "/export x.txt").unwrap_err().to_string(),
        "unsupported export path: x.txt"
    );
    match dispatch(&f.conv, "/clone").unwrap() {
        SlashOutcome::SwitchConversation {
            conversation,
            message,
        } => {
            assert_eq!(message, "Cloned to new session.");
            assert!(conversation.session_id().starts_with("clone-"));
        }
        other => panic!("{other:?}"),
    }
    match dispatch(&f.conv, "/new").unwrap() {
        SlashOutcome::SwitchConversation {
            message,
            conversation,
        } => {
            assert_eq!(message, "Started a new session.");
            assert!(conversation.entries().is_empty());
        }
        other => panic!("{other:?}"),
    }
    match dispatch(&f.conv, "/fork").unwrap() {
        SlashOutcome::Action(SlashAction::ForkFromMessage { messages }) => {
            assert_eq!(messages.len(), 1);
            assert_eq!(messages[0].1, "hi");
        }
        other => panic!("{other:?}"),
    }
    assert_eq!(
        dispatch(&f.conv, "/import").unwrap_err().to_string(),
        "Usage: /import <path.jsonl>"
    );
    match dispatch(&f.conv, "/import out/s.jsonl").unwrap() {
        SlashOutcome::SwitchConversation {
            message,
            conversation,
        } => {
            assert_eq!(message, "Imported session from out/s.jsonl.");
            assert_eq!(conversation.messages().len(), 2);
        }
        other => panic!("{other:?}"),
    }
    let ephemeral = f.engine.new_ephemeral_conversation(None);
    assert_eq!(
        dispatch(&ephemeral, "/export").unwrap_err().to_string(),
        "Cannot export an in-memory session"
    );
    assert_eq!(
        dispatch(&ephemeral, "/share").unwrap_err().to_string(),
        "Cannot share an in-memory session"
    );
    assert_eq!(
        dispatch(&ephemeral, "/fork").unwrap_err().to_string(),
        "Cannot fork session: no active session file"
    );
    let _ = json!({});
}

#[test]
fn busy_rules_follow_the_archived_list() {
    assert!(is_blocked_while_running("compact", ""));
    assert!(is_blocked_while_running("context", "all"));
    assert!(is_blocked_while_running("reload", ""));
    assert!(is_blocked_while_running("llama", "load x"));
    assert!(!is_blocked_while_running("llama", ""));
    assert!(!is_blocked_while_running("llama", "models"));
    assert!(!is_blocked_while_running("session", ""));
}
