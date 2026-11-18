//! Scripted stand-in for `qt-cli --mode json` used by the integration tests.
//!
//! The task is the last `Task: ...` argument, or stdin for persistent
//! workers. Keywords in the task select the behaviour:
//! - `fail`: print `agent_start`, write `boom` to stderr and exit 3;
//! - `noend`: exit 0 without an `agent_end` event;
//! - `sleep:<ms>`: sleep before answering;
//! - `garbage`: print a non-JSON line first.
//!
//! Otherwise it answers `done: <task>`. With `QT_MOCK_CHILD_LOG` set, each
//! invocation appends `{args, cwd, env, stdin}` to that file.

use std::io::{Read as _, Write as _};

use serde_json::{Value, json};

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut stdin = String::new();
    if args.iter().all(|a| !a.starts_with("Task: ")) {
        let _ = std::io::stdin().read_to_string(&mut stdin);
    }
    let task = args
        .iter()
        .rev()
        .find_map(|a| a.strip_prefix("Task: "))
        .map(str::to_string)
        .unwrap_or_else(|| stdin.trim_end().to_string());

    if let Ok(log) = std::env::var("QT_MOCK_CHILD_LOG") {
        let env: serde_json::Map<String, Value> = std::env::vars()
            .filter(|(k, _)| k.starts_with("QT_SUBAGENT") || k == "QT_CODING_AGENT_DIR")
            .map(|(k, v)| (k, json!(v)))
            .collect();
        let record = json!({
            "args": args,
            "cwd": std::env::current_dir().map(|d| d.to_string_lossy().into_owned()).unwrap_or_default(),
            "env": env,
            "stdin": stdin,
        });
        if let Ok(mut file) = std::fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(log)
        {
            let _ = writeln!(file, "{record}");
        }
    }

    let mut out = std::io::stdout().lock();
    let _ = writeln!(out, "{}", json!({"type": "agent_start"}));
    let _ = out.flush();
    if task.contains("garbage") {
        let _ = writeln!(out, "this is not json");
    }
    if let Some(ms) = task
        .split_whitespace()
        .find_map(|w| w.strip_prefix("sleep:"))
        .and_then(|ms| ms.parse::<u64>().ok())
    {
        std::thread::sleep(std::time::Duration::from_millis(ms));
    }
    if task.contains("fail") {
        eprint!("boom");
        std::process::exit(3);
    }
    if task.contains("noend") {
        return;
    }
    let answer = format!("done: {task}");
    let assistant = json!({
        "role": "assistant",
        "content": [{"type": "text", "text": answer}],
        "stopReason": "stop"
    });
    let _ = writeln!(
        out,
        "{}",
        json!({"type": "message_end", "message": assistant})
    );
    let _ = writeln!(
        out,
        "{}",
        json!({
            "type": "agent_end",
            "messages": [
                {"role": "user", "content": [{"type": "text", "text": task}]},
                assistant
            ]
        })
    );
}
