//! `complete_path`: complete a filesystem path prefix inside the workspace
//! (archived `complete_path_tool.cpp`).

use std::path::{Path, PathBuf};

use futures::future::BoxFuture;
use serde::Deserialize;
use serde_json::{Value, json};

use super::{Tool, ToolContext, ToolOutput, parse_args};

const MAX_MATCHES: usize = 200;

pub struct CompletePathTool;

#[derive(Deserialize)]
struct Args {
    path: String,
}

impl Tool for CompletePathTool {
    fn name(&self) -> &'static str {
        "complete_path"
    }
    fn description(&self) -> String {
        "Complete a path prefix permitted by the runtime filesystem policy. Returns the entries of the prefix's directory whose names start with the final component; directories end with '/'.".into()
    }
    fn snippet(&self) -> &'static str {
        "Complete a file or directory path prefix"
    }
    fn parameters(&self) -> Value {
        json!({
            "type": "object",
            "properties": {
                "path": { "type": "string", "description": "Relative or absolute filesystem path prefix to complete." }
            },
            "required": ["path"],
            "additionalProperties": false
        })
    }
    fn parallel_safe(&self) -> bool {
        true
    }

    fn execute(&self, args: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        Box::pin(async move {
            let args: Args = match parse_args("complete_path", args) {
                Ok(a) => a,
                Err(e) => return e,
            };
            complete(&args.path, &ctx)
        })
    }
}

fn complete(prefix: &str, ctx: &ToolContext) -> ToolOutput {
    let raw = prefix.strip_prefix('@').unwrap_or(prefix);
    let directory_prefix = raw.is_empty() || raw.ends_with('/') || raw.ends_with('\\');
    // Split the typed text into the directory part (kept verbatim in the
    // completions) and the file-name prefix.
    let (typed_parent, name_prefix) = if directory_prefix {
        (raw.to_string(), String::new())
    } else {
        match raw.rfind(['/', '\\']) {
            Some(i) => (raw[..=i].to_string(), raw[i + 1..].to_string()),
            None => (String::new(), raw.to_string()),
        }
    };
    let parent_arg = if typed_parent.is_empty() {
        ".".to_string()
    } else {
        typed_parent.clone()
    };
    let parent = match ctx.resolve_path(&parent_arg) {
        Ok(r) => r.path,
        Err(e) => return e,
    };
    let details_base = json!({
        "prefix": prefix,
        "parent": ctx.display(&parent),
    });
    let entries = match std::fs::read_dir(&parent) {
        Ok(entries) => entries,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
            return no_matches(details_base);
        }
        Err(_) if parent.exists() && !parent.is_dir() => {
            return ToolOutput::error(format!("{} is not a directory", ctx.display(&parent)));
        }
        Err(e) => return ToolOutput::error(format!("{}: {e}", ctx.display(&parent))),
    };
    let typed_parent = typed_parent.replace('\\', "/");
    let mut matches: Vec<(String, PathBuf, &'static str)> = entries
        .flatten()
        .filter_map(|entry| {
            let name = entry.file_name().to_string_lossy().into_owned();
            if !name.starts_with(&name_prefix) {
                return None;
            }
            let path = entry.path();
            let kind = kind_of(&path);
            let mut completion = format!("{typed_parent}{name}");
            if kind == "directory" {
                completion.push('/');
            }
            Some((completion, path, kind))
        })
        .collect();
    if matches.is_empty() {
        return no_matches(details_base);
    }
    matches.sort_by(|a, b| a.0.cmp(&b.0));
    let total = matches.len();
    matches.truncate(MAX_MATCHES);
    let mut text = matches
        .iter()
        .map(|(c, _, _)| c.as_str())
        .collect::<Vec<_>>()
        .join("\n");
    if total > MAX_MATCHES {
        text.push_str(&format!(
            "\n\n[{MAX_MATCHES} of {total} completions shown. Type a longer prefix to narrow them.]"
        ));
    }
    let mut details = details_base;
    details["count"] = json!(total);
    details["matches"] = json!(
        matches
            .iter()
            .map(|(completion, path, kind)| json!({
                "name": path.file_name().map(|n| n.to_string_lossy().into_owned()),
                "path": ctx.display(path),
                "kind": kind,
                "completion": completion,
            }))
            .collect::<Vec<_>>()
    );
    ToolOutput::text(text).with_details(details)
}

fn no_matches(mut details: Value) -> ToolOutput {
    details["count"] = json!(0);
    details["matches"] = json!([]);
    ToolOutput::text("No matching paths").with_details(details)
}

fn kind_of(path: &Path) -> &'static str {
    match std::fs::metadata(path) {
        Ok(m) if m.is_dir() => "directory",
        Ok(m) if m.is_file() => "file",
        _ => "other",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn completes_prefixes_and_directories() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(dir.path().join("src/tools")).unwrap();
        std::fs::write(dir.path().join("src/main.rs"), "").unwrap();
        std::fs::write(dir.path().join("src/mod.rs"), "").unwrap();
        std::fs::write(dir.path().join("README.md"), "").unwrap();
        let ctx = ToolContext::new(dir.path());
        let run = |path: &str| CompletePathTool.execute(json!({ "path": path }), ctx.clone());
        assert_eq!(run("src/m").await.text_content(), "src/main.rs\nsrc/mod.rs");
        assert_eq!(
            run("src/").await.text_content(),
            "src/main.rs\nsrc/mod.rs\nsrc/tools/"
        );
        assert_eq!(run("R").await.text_content(), "README.md");
        let out = run("sr").await;
        assert_eq!(out.text_content(), "src/");
        assert_eq!(out.details.unwrap()["matches"][0]["kind"], "directory");
        assert_eq!(run("nope/x").await.text_content(), "No matching paths");
        assert!(run("../").await.is_error);
    }
}
