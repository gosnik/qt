//! Memory scope parity across front ends: a memory remembered over RPC in
//! one session is counted by another RPC session (the runtime the desktop
//! app uses) and by the `qt memory` subcommands, all started from different
//! directories of the same checkout.

use std::io::{BufRead, BufReader, Write};
use std::path::Path;
use std::process::{Command, Stdio};

use serde_json::{Value, json};

struct Env {
    _dir: tempfile::TempDir,
    home: std::path::PathBuf,
    user_home: std::path::PathBuf,
    root: std::path::PathBuf,
}

fn env() -> Env {
    let dir = tempfile::tempdir().unwrap();
    let home = dir.path().join("agent");
    let user_home = dir.path().join("user");
    let root = dir.path().join("checkout");
    for d in [&home, &user_home, &root.join("src"), &root.join(".git")] {
        std::fs::create_dir_all(d).unwrap();
    }
    std::fs::write(root.join(".git/HEAD"), "ref: refs/heads/main\n").unwrap();
    // No model is ever called: the provider points at a closed port.
    let settings = json!({
        "ollama": {"instances": [{"id": "mock", "baseUrl": "http://127.0.0.1:9"}]},
        "defaultProvider": "ollama:mock",
        "defaultModel": "m",
        "memory": {"enabled": true},
    });
    std::fs::write(home.join("settings.json"), settings.to_string()).unwrap();
    Env {
        _dir: dir,
        home,
        user_home,
        root,
    }
}

fn command(env: &Env, cwd: &Path) -> Command {
    let mut command = Command::new(env!("CARGO_BIN_EXE_qt-cli"));
    command
        .current_dir(cwd)
        .env("QT_CODING_AGENT_DIR", &env.home)
        .env("HOME", &env.user_home)
        .env_remove("PI_CODING_AGENT_SESSION_DIR")
        .env_remove("QT_SUBAGENT_DEPTH")
        .env_remove("QT_SUBAGENT_CHILD");
    command
}

/// Runs one `qt-cli --mode rpc` process over `commands` and returns the
/// response data by request id.
fn rpc(env: &Env, cwd: &Path, commands: &[Value]) -> Vec<Value> {
    let mut child = command(env, cwd)
        .args(["--mode", "rpc"])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .expect("spawn qt-cli");
    let mut stdin = child.stdin.take().unwrap();
    let stdout = BufReader::new(child.stdout.take().unwrap());
    let mut lines = stdout.lines();
    let mut out = Vec::new();
    for (index, command) in commands.iter().enumerate() {
        let id = format!("req-{index}");
        let mut command = command.clone();
        command["id"] = json!(id);
        writeln!(stdin, "{command}").unwrap();
        stdin.flush().unwrap();
        let response = loop {
            let line = lines.next().expect("rpc output closed").unwrap();
            let value: Value = serde_json::from_str(&line).unwrap();
            if value["type"] == "response" && value["id"] == id.as_str() {
                break value;
            }
        };
        assert_eq!(response["success"], true, "{response}");
        out.push(response["data"].clone());
    }
    drop(stdin);
    assert!(child.wait().unwrap().success());
    out
}

fn memory_cli(env: &Env, cwd: &Path, args: &[&str]) -> Value {
    let output = command(env, cwd)
        .arg("memory")
        .args(args)
        .arg("--json")
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    serde_json::from_slice(&output.stdout).unwrap()
}

#[test]
fn rpc_memories_are_visible_to_other_sessions_and_the_memory_cli() {
    let env = env();
    let src = env.root.join("src");
    let first = rpc(
        &env,
        &src,
        &[
            json!({"type": "memory", "action": "remember", "confirm": true,
                   "params": {"content": "Deploys always use blue-green switching."}}),
            json!({"type": "memory", "action": "status"}),
        ],
    );
    let id = first[0]["id"].as_str().unwrap().to_string();
    assert_eq!(first[1]["scope_memory_count"], 1);
    let rpc_scope = first[1]["scope"].clone();

    // A second RPC session (the runtime the desktop app runs) started at
    // the checkout root counts and recalls it.
    let second = rpc(
        &env,
        &env.root,
        &[
            json!({"type": "memory", "action": "status"}),
            json!({"type": "memory", "action": "recall",
                   "params": {"query": "blue-green deploys"}}),
        ],
    );
    assert_eq!(second[0]["scope_memory_count"], 1, "{}", second[0]);
    for key in ["user", "project", "repository", "branch"] {
        assert_eq!(second[0]["scope"][key], rpc_scope[key], "{key}");
    }
    assert!(second[1].to_string().contains(&id), "{}", second[1]);

    // `qt memory` from either directory derives the same scope.
    for cwd in [&env.root, &src] {
        let status = memory_cli(&env, cwd, &["status"]);
        assert_eq!(status["scope_memory_count"], 1, "{status}");
        for key in ["user", "project", "repository", "branch"] {
            assert_eq!(status["scope"][key], rpc_scope[key], "{key}");
        }
        assert_eq!(status["scope"]["session"], "");
        let recall = memory_cli(&env, cwd, &["recall", "blue-green deploys"]);
        assert!(recall.to_string().contains(&id), "{recall}");
    }

    // The stored row carries no session dimension.
    let db = rusqlite::Connection::open(env.home.join("memory/memory.sqlite3")).unwrap();
    let (session, agent): (String, String) = db
        .query_row(
            "SELECT session_id,agent_id FROM memories WHERE id=?;",
            [&id],
            |row| Ok((row.get(0)?, row.get(1)?)),
        )
        .unwrap();
    assert_eq!((session.as_str(), agent.as_str()), ("", ""));
}
