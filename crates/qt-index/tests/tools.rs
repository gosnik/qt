//! End-to-end tests of the index tools over temporary projects.

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use qt_core::tools::{Tool, ToolContext, ToolOutput};
use qt_index::{IndexManager, index_tools};
use serde_json::{Value, json};

struct Fixture {
    _dir: tempfile::TempDir,
    root: PathBuf,
    tools: Vec<Arc<dyn Tool>>,
}

fn write(root: &Path, relative: &str, text: &str) {
    let path = root.join(relative);
    fs::create_dir_all(path.parent().unwrap()).unwrap();
    fs::write(path, text).unwrap();
}

fn fixture() -> Fixture {
    let dir = tempfile::tempdir().unwrap();
    let base = fs::canonicalize(dir.path()).unwrap();
    let root = base.join("project");
    write(
        &root,
        "CMakeLists.txt",
        "project(demo)\nadd_executable(demo src/main.cpp src/engine.cpp)\ntarget_link_libraries(demo PRIVATE m)\ntarget_include_directories(demo PRIVATE include)\n",
    );
    write(
        &root,
        "include/engine.h",
        "#pragma once\nclass Engine {\npublic:\n  void start();\n  int rpm() const { return 900; }\n};\n",
    );
    write(
        &root,
        "src/engine.cpp",
        "#include \"engine.h\"\n#include <vector>\n\nvoid Engine::start() {\n  ignite();\n  warmup(3);\n}\n\nvoid ignite() {\n  spark();\n}\n",
    );
    write(
        &root,
        "src/main.cpp",
        "#include \"engine.h\"\n\nint main() {\n  Engine engine;\n  engine.start();\n  return 0;\n}\n",
    );
    write(&root, "config/plan.toml", "notes = \"turbocharger\"\n");
    write(&root, ".gitignore", "out/\n");
    write(&root, "out/generated.cpp", "void generated() {}\n");
    let manager = Arc::new(IndexManager::new(base.join("agent-home")));
    let tools = index_tools(manager, |_| true);
    Fixture {
        _dir: dir,
        root,
        tools,
    }
}

impl Fixture {
    async fn call(&self, name: &str, args: Value) -> ToolOutput {
        let tool = self.tools.iter().find(|t| t.name() == name).expect("tool");
        tool.execute(args, ToolContext::new(&self.root)).await
    }

    async fn ok(&self, name: &str, args: Value) -> Value {
        let out = self.call(name, args).await;
        assert!(!out.is_error, "{name} failed: {}", out.text_content());
        serde_json::from_str(&out.text_content()).expect("json output")
    }
}

fn error_payload(out: &ToolOutput) -> (String, Value) {
    assert!(out.is_error);
    let text = out.text_content();
    match text.rsplit_once('\n') {
        Some((message, rest)) => match serde_json::from_str::<Value>(rest) {
            Ok(payload) => (message.to_string(), payload),
            Err(_) => (text.clone(), Value::Null),
        },
        None => (text, Value::Null),
    }
}

#[tokio::test(flavor = "multi_thread")]
async fn tool_set_and_schemas() {
    let f = fixture();
    let names: Vec<_> = f.tools.iter().map(|t| t.name()).collect();
    assert_eq!(names, qt_index::INDEX_TOOL_NAMES);
    for tool in &f.tools {
        assert!(tool.parallel_safe());
        assert!(!tool.snippet().is_empty());
        let schema = tool.parameters();
        assert_eq!(schema["additionalProperties"], false);
    }
    let symbol = f.tools.iter().find(|t| t.name() == "symbol").unwrap();
    assert_eq!(symbol.parameters()["properties"]["limit"]["maximum"], 100);
    assert!(symbol.guidelines()[0].starts_with("For C/C++ class, function, method"));
    let router = f.tools.iter().find(|t| t.name() == "index").unwrap();
    assert_eq!(router.parameters()["required"], json!(["action"]));
    let only_router = index_tools(Arc::new(IndexManager::new(f.root.join("h"))), |n| {
        n == "index"
    });
    assert!(only_router[0].guidelines()[0].contains("operation=code.symbol"));
}

#[tokio::test(flavor = "multi_thread")]
async fn symbol_finds_definitions_with_context() {
    let f = fixture();
    let out = f
        .ok(
            "symbol",
            json!({ "name": "start", "context": true, "after": 1 }),
        )
        .await;
    let symbols = out["symbols"].as_array().unwrap();
    let start = symbols
        .iter()
        .find(|s| s["qualified_name"] == "start" || s["name"] == "start")
        .expect("start");
    assert_eq!(start["file"], "src/engine.cpp");
    assert_eq!(start["line"], 4);
    assert_eq!(start["symbol_kind"], "function");
    let context = start["context"].as_array().unwrap();
    assert!(
        context
            .iter()
            .any(|l| l["target"] == true && l["text"] == "void Engine::start() {")
    );
    assert_eq!(out["index"]["generation"], 1);
    assert_eq!(out["index"]["stale"], false);
    assert_eq!(out["output_guard"]["truncated"], false);
    assert_eq!(out["query"]["context"], "1");

    let classes = f
        .ok("symbol", json!({ "name": "Engine", "file": "include/" }))
        .await;
    let class = &classes["symbols"][0];
    assert_eq!(class["symbol_kind"], "class");
    assert_eq!(class["methods"][0]["qualified_name"], "Engine::rpm");

    // Ignored and generated directories are not indexed.
    let none = f.ok("symbol", json!({ "name": "generated" })).await;
    assert_eq!(none["symbols"], json!([]));
}

#[tokio::test(flavor = "multi_thread")]
async fn invalid_arguments_are_retryable() {
    let f = fixture();
    let out = f
        .call("symbol", json!({ "name": "x", "limit": 1000, "bogus": 1 }))
        .await;
    let (message, payload) = error_payload(&out);
    assert!(message.starts_with("Validation failed for tool \"symbol\":\n"));
    assert!(
        out.text_content()
            .contains("  - limit: value is above maximum 100")
    );
    assert!(out.text_content().contains("  - bogus: unknown property"));
    assert_eq!(payload["code"], "invalid_arguments");
    assert_eq!(payload["tool"], "symbol");
    assert_eq!(payload["retryable"], true);
    assert_eq!(payload["expected_schema"]["required"], json!(["name"]));
    assert_eq!(out.details.as_ref().unwrap()["status"], "invalid_request");

    let out = f.call("symbol", Value::String("{bad".into())).await;
    assert!(out.text_content().contains("root: expected object"));

    let out = f
        .call("analyze_file", json!({ "file": "missing.cpp" }))
        .await;
    assert_eq!(
        out.text_content(),
        "analyze_file file does not exist: missing.cpp"
    );
    let out = f
        .call("build_context", json!({ "file": "../etc/passwd" }))
        .await;
    assert_eq!(
        out.text_content(),
        "build_context file must stay inside the project"
    );
    let out = f
        .call(
            "references",
            json!({ "symbol": "x", "direction": "sideways" }),
        )
        .await;
    assert!(
        out.text_content()
            .contains("direction: value must be one of")
    );
}

#[tokio::test(flavor = "multi_thread")]
async fn analyze_file_and_references() {
    let f = fixture();
    let out = f
        .ok("analyze_file", json!({ "file": "src/engine.cpp" }))
        .await;
    assert_eq!(out["call_graph_approximate"], true);
    assert_eq!(out["line_stats"]["total"], 11);
    assert_eq!(out["line_stats"]["blank"], 2);
    let functions: Vec<_> = out["functions"]
        .as_array()
        .unwrap()
        .iter()
        .map(|f| f["name"].clone())
        .collect();
    assert_eq!(functions, vec![json!("start"), json!("ignite")]);
    let outgoing: Vec<_> = out["outgoing_calls"]
        .as_array()
        .unwrap()
        .iter()
        .map(|e| e["callee"].as_str().unwrap().to_string())
        .collect();
    assert!(outgoing.contains(&"ignite".to_string()));
    assert!(outgoing.contains(&"warmup".to_string()));
    assert!(
        out["incoming_calls"]
            .as_array()
            .unwrap()
            .iter()
            .any(|e| e["caller"] == "main")
    );
    assert_eq!(out["query"]["limit"], "100");

    let refs = f.ok("references", json!({ "symbol": "ignite" })).await;
    assert_eq!(refs["approximate"], true);
    assert_eq!(
        refs["verification"],
        "Verify material call edges against current source."
    );
    assert_eq!(refs["direction"], "both");
    assert_eq!(refs["depth"], 3);
    let paths = refs["paths"].as_array().unwrap();
    assert!(
        paths
            .iter()
            .any(|p| p["direction"] == "outbound" && p["callee"] == "spark")
    );
    assert!(
        paths
            .iter()
            .any(|p| p["direction"] == "inbound" && p["caller"] == "start")
    );
    let inbound = f
        .ok(
            "references",
            json!({ "symbol": "spark", "direction": "inbound", "depth": 2 }),
        )
        .await;
    let callers: Vec<_> = inbound["paths"]
        .as_array()
        .unwrap()
        .iter()
        .map(|p| p["caller"].clone())
        .collect();
    assert_eq!(callers, vec![json!("ignite"), json!("start")]);
}

#[tokio::test(flavor = "multi_thread")]
async fn build_context_reports_flags_and_includes() {
    let f = fixture();
    let out = f
        .ok("build_context", json!({ "file": "src/engine.cpp" }))
        .await;
    assert_eq!(out["file"], "src/engine.cpp");
    let entries = out["compile_flags"]["entries"].as_array().unwrap();
    assert_eq!(entries.len(), 1);
    let entry = &entries[0];
    assert_eq!(entry["compiler"], "c++");
    assert_eq!(entry["standards"], json!(["-std=c++17"]));
    let includes: Vec<_> = entry["includes"]
        .as_array()
        .unwrap()
        .iter()
        .map(|v| v.as_str().unwrap())
        .collect();
    assert!(includes.contains(&"include"), "{includes:?}");
    assert!(includes.contains(&"src"));
    assert_eq!(entry["output"], ".index/objects/src/engine.cpp.o");
    assert_eq!(out["compile_flags"]["truncated"], false);
    let edges = out["include_graph"]["edges"].as_array().unwrap();
    assert_eq!(edges.len(), 2);
    assert_eq!(edges[0]["include"], "engine.h");
    assert_eq!(edges[0]["resolved"], "include/engine.h");
    assert_eq!(edges[0]["system"], false);
    assert_eq!(edges[1]["include"], "vector");
    assert_eq!(edges[1]["system"], true);
    assert_eq!(edges[1]["resolved"], "");
    assert!(f.root.join("compile_commands.json").is_file());

    let limited = f
        .ok(
            "build_context",
            json!({ "file": "src/engine.cpp", "limit": 1 }),
        )
        .await;
    assert_eq!(
        limited["include_graph"]["edges"].as_array().unwrap().len(),
        1
    );
    assert_eq!(limited["include_graph"]["truncated"], true);
}

#[tokio::test(flavor = "multi_thread")]
async fn search_index_plain_and_fts5() {
    let f = fixture();
    let out = f
        .ok("search_index", json!({ "query": "turbocharger" }))
        .await;
    assert_eq!(out["syntax"], "plain");
    assert_eq!(out["query"], "turbocharger");
    assert_eq!(out["limit"], 50);
    let matches = out["matches"].as_array().unwrap();
    assert_eq!(matches[0]["path"], "config/plan.toml");
    assert!(
        matches[0]["snippet"]
            .as_str()
            .unwrap()
            .contains("[turbocharger]")
    );

    let quoted = f
        .ok(
            "search_index",
            json!({ "query": "ignite \"spark", "limit": 5 }),
        )
        .await;
    assert!(quoted["matches"].as_array().is_some());

    let fts = f
        .ok(
            "search_index",
            json!({ "query": "ignite OR warmup", "syntax": "fts5" }),
        )
        .await;
    assert!(!fts["matches"].as_array().unwrap().is_empty());

    let bad = f
        .call(
            "search_index",
            json!({ "query": "\"unbalanced", "syntax": "fts5" }),
        )
        .await;
    assert!(bad.is_error);
    assert!(
        bad.text_content().starts_with("invalid FTS5 query: "),
        "{}",
        bad.text_content()
    );
    assert_eq!(bad.details.as_ref().unwrap()["status"], "invalid_request");
}

#[tokio::test(flavor = "multi_thread")]
async fn router_tree_and_calls() {
    let f = fixture();
    let tree = f
        .ok("index", json!({ "action": "tree", "category": "git" }))
        .await;
    let ops: Vec<_> = tree["categories"][0]["operations"]
        .as_array()
        .unwrap()
        .iter()
        .map(|o| o["operation"].as_str().unwrap().to_string())
        .collect();
    assert_eq!(
        ops,
        vec![
            "git.status",
            "git.log",
            "git.diff",
            "git.show",
            "git.blame",
            "git.branches"
        ]
    );
    assert_eq!(tree["usage"]["call"]["operation"], "fs.read");

    let functions = f
        .ok("index", json!({ "action": "call", "operation": "code.functions", "arguments": { "name": "ign" } }))
        .await;
    assert_eq!(functions["category"], "code");
    assert_eq!(functions["operation"], "code.functions");
    assert_eq!(
        functions["output"]["functions"][0]["qualified_name"],
        "ignite"
    );

    // Alias rewrites: find_symbol + pattern become code.symbol + name.
    let symbol = f
        .ok("index", json!({ "action": "call", "arguments": { "operation": "find_symbol", "pattern": "rpm" } }))
        .await;
    assert_eq!(symbol["operation"], "code.symbol");
    assert_eq!(
        symbol["output"]["symbols"][0]["qualified_name"],
        "Engine::rpm"
    );

    let read = f
        .ok("index", json!({ "action": "call", "category": "fs", "operation": "read", "arguments": { "file": "src/main.cpp", "start_line": 3, "max_lines": 2 } }))
        .await;
    let lines = read["output"]["lines"].as_array().unwrap();
    assert_eq!(lines.len(), 2);
    assert_eq!(lines[0]["text"], "int main() {");
    assert_eq!(read["output"]["truncated"], true);

    let list = f
        .ok(
            "index",
            json!({ "action": "call", "operation": "fs.list", "arguments": {} }),
        )
        .await;
    let names: Vec<_> = list["output"]["entries"]
        .as_array()
        .unwrap()
        .iter()
        .map(|e| e["name"].clone())
        .collect();
    assert!(names.contains(&json!("src")));
    let stat = f
        .ok("index", json!({ "action": "call", "operation": "fs.stat" }))
        .await;
    assert_eq!(stat["output"]["type"], "directory");

    let status = f
        .ok("index", json!({ "action": "call", "operation": "status", "arguments": { "include_tree_sitter": false } }))
        .await;
    assert_eq!(status["category"], "core");
    assert!(status["output"].get("tree_sitter").is_none());
    assert!(status["output"].get("build_metadata").is_none());
    // Without a running worker every scan-backed router call rescans.
    assert!(status["output"]["generation"].as_u64().unwrap() > 1);

    let graph = f
        .ok(
            "index",
            json!({ "action": "call", "operation": "build.target_graph" }),
        )
        .await;
    let nodes = graph["output"]["nodes"].as_array().unwrap();
    assert!(
        nodes
            .iter()
            .any(|n| n["name"] == "demo" && n["type"] == "executable")
    );
    let edges = graph["output"]["edges"].as_array().unwrap();
    assert!(
        edges
            .iter()
            .any(|e| e["from"] == "demo" && e["to"] == "m" && e["type"] == "links")
    );

    let summary = f
        .ok(
            "index",
            json!({ "action": "call", "operation": "project_summary" }),
        )
        .await;
    assert_eq!(summary["output"]["symbols"]["classes"], 1);
    assert_eq!(summary["output"]["architecture_nodes"][0]["name"], "Engine");

    let memory = f
        .ok("index", json!({ "action": "call", "operation": "memory_graph", "arguments": { "include_nodes": false } }))
        .await;
    assert!(memory["output"].get("nodes").is_none());
    assert!(Path::new(memory["output"]["artifact"].as_str().unwrap()).is_file());

    let snippet = f
        .ok("index", json!({ "action": "call", "operation": "code_snippet", "arguments": { "qualified_name": "ignite", "after": 1 } }))
        .await;
    assert_eq!(snippet["output"]["qualified_name"], "ignite");
    assert_eq!(snippet["output"]["lines"].as_array().unwrap().len(), 4);

    let ast = f
        .ok("index", json!({ "action": "call", "operation": "code.ast", "arguments": { "file": "src/main.cpp" } }))
        .await;
    assert!(
        ast["output"]["lines"][0]["text"]
            .as_str()
            .unwrap()
            .starts_with("(translation_unit")
    );
}

#[tokio::test(flavor = "multi_thread")]
async fn router_errors() {
    let f = fixture();
    let unknown = f
        .call(
            "index",
            json!({ "action": "call", "operation": "serial.list" }),
        )
        .await;
    assert_eq!(
        unknown.text_content(),
        "unknown index operation: serial.list"
    );

    let bad_args = f
        .call("index", json!({ "action": "call", "operation": "code.read_context", "arguments": { "file": "src/main.cpp", "lines": 3 } }))
        .await;
    let (message, payload) = error_payload(&bad_args);
    assert!(message.starts_with("Validation failed for tool \"code.read_context\":\n  - line: required property is missing\n  - lines: unknown property"));
    assert_eq!(payload["code"], "invalid_index_arguments");
    assert_eq!(payload["operation"], "code.read_context");
    assert_eq!(payload["retryable"], true);
    assert_eq!(
        payload["received_arguments"],
        json!({ "file": "src/main.cpp", "lines": 3 })
    );

    let missing_op = f.call("index", json!({ "action": "call" })).await;
    assert_eq!(
        missing_op.text_content(),
        "index requires a string operation argument"
    );

    let bad_action = f.call("index", json!({ "action": "run" })).await;
    assert!(
        bad_action
            .text_content()
            .contains("action: value must be one of [\"tree\",\"call\"]")
    );

    let not_object = f
        .call(
            "index",
            json!({ "action": "call", "operation": "status", "arguments": [1] }),
        )
        .await;
    assert!(
        not_object
            .text_content()
            .contains("arguments: expected object, received array")
    );

    let escape = f
        .call("index", json!({ "action": "call", "operation": "fs.read", "arguments": { "file": "../outside.txt" } }))
        .await;
    assert_eq!(escape.text_content(), "file must not contain '..'");
    assert_eq!(
        escape.details.as_ref().unwrap()["status"],
        "invalid_request"
    );

    let missing = f
        .call("index", json!({ "action": "call", "operation": "code_snippet", "arguments": { "qualified_name": "nothing_here" } }))
        .await;
    assert_eq!(missing.text_content(), "symbol not found: nothing_here");
    assert_eq!(
        missing.details.as_ref().unwrap()["status"],
        "execution_error"
    );
}

#[tokio::test(flavor = "multi_thread")]
async fn router_git_operations() {
    let f = fixture();
    let git = |args: &[&str]| {
        std::process::Command::new("git")
            .args(args)
            .current_dir(&f.root)
            .env("GIT_AUTHOR_NAME", "t")
            .env("GIT_AUTHOR_EMAIL", "t@example.invalid")
            .env("GIT_COMMITTER_NAME", "t")
            .env("GIT_COMMITTER_EMAIL", "t@example.invalid")
            .output()
            .unwrap()
    };
    if !git(&["init", "-q"]).status.success() {
        return;
    }
    git(&["add", "."]);
    git(&["commit", "-q", "-m", "initial import"]);
    let log = f
        .ok(
            "index",
            json!({ "action": "call", "operation": "git.log", "arguments": { "limit": 5 } }),
        )
        .await;
    let output = &log["output"];
    assert_eq!(output["exit_code"], 0);
    assert!(
        output["stdout"][0]
            .as_str()
            .unwrap()
            .ends_with("initial import")
    );
    assert_eq!(output["args"][0], "-C");
    assert!(
        output["args"]
            .as_array()
            .unwrap()
            .contains(&json!("--max-count=5"))
    );

    let blame = f
        .ok("index", json!({ "action": "call", "operation": "git.blame", "arguments": { "file": "src/main.cpp", "start_line": 3, "end_line": 4 } }))
        .await;
    assert_eq!(blame["output"]["stdout"].as_array().unwrap().len(), 2);

    let show = f
        .call(
            "index",
            json!({ "action": "call", "operation": "git.show", "arguments": { "rev": "--evil" } }),
        )
        .await;
    assert_eq!(
        show.text_content(),
        "git revision must not be empty or start with '-'"
    );
}

#[tokio::test(flavor = "multi_thread")]
async fn index_not_ready_when_scan_cannot_run() {
    let dir = tempfile::tempdir().unwrap();
    let root = fs::canonicalize(dir.path()).unwrap();
    write(&root, "project/a.cpp", "void a() {}\n");
    // The agent home is a file, so the state directory cannot be created.
    fs::write(root.join("home"), "not a directory").unwrap();
    let tools = index_tools(Arc::new(IndexManager::new(root.join("home"))), |n| {
        n == "symbol"
    });
    let out = tools[0]
        .execute(
            json!({ "name": "a" }),
            ToolContext::new(root.join("project")),
        )
        .await;
    let (message, payload) = error_payload(&out);
    assert_eq!(
        message,
        "project code index is still building; retry symbol"
    );
    assert_eq!(payload["code"], "index_not_ready");
    assert_eq!(payload["retryable"], true);
    assert_eq!(payload["scan_phase"], "error");
    assert!(
        payload["last_error"]
            .as_str()
            .unwrap()
            .contains("could not create index state directory")
    );
}

#[tokio::test(flavor = "multi_thread")]
async fn started_service_serves_tools() {
    let f = fixture();
    let manager = Arc::new(IndexManager::new(f.root.parent().unwrap().join("home2")));
    let service = manager.start(&f.root).unwrap();
    let tools = index_tools(manager.clone(), |_| true);
    let symbol = tools.iter().find(|t| t.name() == "symbol").unwrap();
    // The direct tools wait up to three seconds for the first generation.
    let mut out = symbol
        .execute(json!({ "name": "main" }), ToolContext::new(&f.root))
        .await;
    for _ in 0..50 {
        if !out.is_error {
            break;
        }
        assert!(
            out.text_content()
                .starts_with("project code index is still building")
        );
        out = symbol
            .execute(json!({ "name": "main" }), ToolContext::new(&f.root))
            .await;
    }
    assert!(!out.is_error, "{}", out.text_content());
    let value: Value = serde_json::from_str(&out.text_content()).unwrap();
    assert_eq!(value["symbols"][0]["name"], "main");
    assert!(service.status().running);
    manager.stop_all();
    assert!(!service.status().running);
}
