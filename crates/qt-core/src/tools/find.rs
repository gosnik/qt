use std::path::Path;

use futures::future::BoxFuture;
use serde::Deserialize;
use serde_json::{Value, json};

use super::grep::{PathGlob, walker};
use super::{Tool, ToolContext, ToolOutput, de_opt_usize, parse_args};
use crate::util::{DEFAULT_MAX_BYTES, resolve_path, truncate_head};

const DEFAULT_LIMIT: usize = 1000;

pub struct FindTool;

#[derive(Deserialize)]
struct Args {
    pattern: String,
    #[serde(default)]
    path: Option<String>,
    #[serde(default, deserialize_with = "de_opt_usize")]
    limit: Option<usize>,
}

impl Tool for FindTool {
    fn name(&self) -> &'static str {
        "find"
    }
    fn description(&self) -> String {
        format!(
            "Search for files by glob pattern. Returns matching file paths relative to the search directory. Respects .gitignore. Output is truncated to {DEFAULT_LIMIT} results or {}KB (whichever is hit first).",
            DEFAULT_MAX_BYTES / 1024
        )
    }
    fn snippet(&self) -> &'static str {
        "Find files by glob pattern (respects .gitignore)"
    }
    fn parameters(&self) -> Value {
        json!({
            "type": "object",
            "properties": {
                "pattern": { "type": "string", "description": "Glob pattern to match files, e.g. '*.ts', '**/*.json', or 'src/**/*.spec.ts'" },
                "path": { "type": "string", "description": "Directory to search in (default: current directory)" },
                "limit": { "type": "number", "description": "Maximum number of results (default: 1000)" }
            },
            "required": ["pattern"]
        })
    }

    fn execute(&self, args: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        Box::pin(async move {
            let args: Args = match parse_args("find", args) {
                Ok(a) => a,
                Err(e) => return e,
            };
            let cancel = ctx.cancel.clone();
            tokio::task::spawn_blocking(move || find(args, &ctx.cwd, &cancel))
                .await
                .unwrap_or_else(|e| ToolOutput::error(format!("find failed: {e}")))
        })
    }
}

fn find(args: Args, cwd: &Path, cancel: &tokio_util::sync::CancellationToken) -> ToolOutput {
    let glob = match PathGlob::new(&args.pattern) {
        Ok(g) => g,
        Err(e) => return ToolOutput::error(e),
    };
    let root = resolve_path(cwd, args.path.as_deref().unwrap_or("."));
    if !root.is_dir() {
        return ToolOutput::error(format!("Directory not found: {}", root.display()));
    }
    let limit = args.limit.filter(|l| *l > 0).unwrap_or(DEFAULT_LIMIT);
    let mut results = Vec::new();
    let mut limit_hit = false;
    for entry in walker(&root).flatten() {
        if cancel.is_cancelled() {
            return ToolOutput::error("find aborted");
        }
        let path = entry.path();
        let Ok(relative) = path.strip_prefix(&root) else {
            continue;
        };
        if relative.as_os_str().is_empty() || !glob.matches(relative) {
            continue;
        }
        if results.len() >= limit {
            limit_hit = true;
            break;
        }
        let mut shown = relative.to_string_lossy().replace('\\', "/");
        if entry.file_type().is_some_and(|t| t.is_dir()) {
            shown.push('/');
        }
        results.push(shown);
    }
    if results.is_empty() {
        return ToolOutput::text("No files found matching pattern");
    }
    let t = truncate_head(&results.join("\n"), usize::MAX, DEFAULT_MAX_BYTES);
    let mut text = t.text;
    if limit_hit {
        text.push_str(&format!(
            "\n\n[{limit} results limit reached. Use limit={} for more, or refine pattern]",
            limit * 2
        ));
    } else if t.truncated {
        text.push_str(&format!(
            "\n\n[{}KB limit reached. Refine pattern]",
            DEFAULT_MAX_BYTES / 1024
        ));
    }
    ToolOutput::text(text).with_details(json!({ "count": results.len() }))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn finds_by_name_and_path_globs() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(dir.path().join("src/nested")).unwrap();
        std::fs::write(dir.path().join("src/a.rs"), "").unwrap();
        std::fs::write(dir.path().join("src/nested/b.rs"), "").unwrap();
        std::fs::write(dir.path().join("c.md"), "").unwrap();
        let ctx = ToolContext::new(dir.path());
        let out = FindTool
            .execute(json!({"pattern": "*.rs"}), ctx.clone())
            .await
            .text_content();
        assert_eq!(out, "src/a.rs\nsrc/nested/b.rs");
        let out = FindTool
            .execute(json!({"pattern": "src/*.rs"}), ctx.clone())
            .await
            .text_content();
        assert_eq!(out, "src/a.rs");
        let out = FindTool
            .execute(json!({"pattern": "*.py"}), ctx)
            .await
            .text_content();
        assert_eq!(out, "No files found matching pattern");
    }
}
