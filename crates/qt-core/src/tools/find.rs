use std::path::{Path, PathBuf};

use futures::future::BoxFuture;
use serde::Deserialize;
use serde_json::{Value, json};
use tokio_util::sync::CancellationToken;

use super::grep::{PathGlob, WalkOptions, walker};
use super::{
    Tool, ToolContext, ToolOutput, de_opt_bool, de_opt_usize, extensions_for_type,
    matches_extension, parse_args,
};
use crate::util::{DEFAULT_MAX_BYTES, truncate_head};

const DEFAULT_LIMIT: usize = 1000;
/// Directories `find` never descends into (archived `should_skip_directory`).
const SKIPPED_DIRS: &[&str] = &["node_modules"];

pub struct FindTool;

#[derive(Deserialize)]
struct Args {
    pattern: String,
    #[serde(default)]
    path: Option<String>,
    #[serde(default, deserialize_with = "de_opt_usize")]
    limit: Option<usize>,
    #[serde(rename = "type", default)]
    file_type: Option<String>,
    #[serde(default, deserialize_with = "de_opt_bool")]
    directories: Option<bool>,
    #[serde(default, deserialize_with = "de_opt_bool")]
    hidden: Option<bool>,
    #[serde(default, deserialize_with = "de_opt_bool")]
    no_ignore: Option<bool>,
}

/// What a `type` argument selects.
enum TypeFilter {
    Files,
    Directories,
    Extensions(Vec<String>),
}

impl TypeFilter {
    fn parse(kind: &str) -> Self {
        match kind.trim().to_ascii_lowercase().as_str() {
            "f" | "file" | "files" => TypeFilter::Files,
            "d" | "dir" | "directory" | "directories" => TypeFilter::Directories,
            other => TypeFilter::Extensions(extensions_for_type(other)),
        }
    }
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
                "limit": { "type": "number", "description": "Maximum number of results (default: 1000)" },
                "type": { "type": "string", "description": "'f' for files, 'd' for directories, or a file type such as 'rust', 'cpp', 'ts', 'py'" },
                "directories": { "type": "boolean", "description": "Match directories as well as files (default: false)" },
                "hidden": { "type": "boolean", "description": "Include hidden files and directories (default: false)" },
                "no_ignore": { "type": "boolean", "description": "Do not apply .gitignore rules (default: false)" }
            },
            "required": ["pattern"]
        })
    }
    fn parallel_safe(&self) -> bool {
        true
    }

    fn execute(&self, args: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        Box::pin(async move {
            let args: Args = match parse_args("find", args) {
                Ok(a) => a,
                Err(e) => return e,
            };
            let root = match ctx.resolve_path(args.path.as_deref().unwrap_or(".")) {
                Ok(r) => r.path,
                Err(e) => return e,
            };
            let cancel = ctx.cancel.clone();
            tokio::task::spawn_blocking(move || find(args, root, &ctx, &cancel))
                .await
                .unwrap_or_else(|e| ToolOutput::error(format!("find failed: {e}")))
        })
    }
}

fn find(args: Args, root: PathBuf, ctx: &ToolContext, cancel: &CancellationToken) -> ToolOutput {
    if args.pattern.trim().is_empty() {
        return ToolOutput::error("find requires a non-empty pattern argument");
    }
    let glob = match PathGlob::new(&args.pattern) {
        Ok(g) => g,
        Err(e) => return ToolOutput::error(e),
    };
    if !root.exists() {
        return ToolOutput::error(format!("Path not found: {}", ctx.display(&root)));
    }
    if !root.is_dir() {
        return ToolOutput::error(format!("Directory not found: {}", ctx.display(&root)));
    }
    let filter = args.file_type.as_deref().map(TypeFilter::parse);
    let (want_files, want_dirs) = match &filter {
        Some(TypeFilter::Directories) => (false, true),
        Some(TypeFilter::Files) => (true, args.directories.unwrap_or(false)),
        Some(TypeFilter::Extensions(_)) => (true, false),
        None => (true, args.directories.unwrap_or(false)),
    };
    let limit = args.limit.filter(|l| *l > 0).unwrap_or(DEFAULT_LIMIT);
    let walk = WalkOptions {
        hidden: args.hidden.unwrap_or(false),
        no_ignore: args.no_ignore.unwrap_or(false),
        skip_dirs: SKIPPED_DIRS,
    };
    let mut results = Vec::new();
    let mut limit_hit = false;
    for entry in walker(&root, walk).flatten() {
        if cancel.is_cancelled() {
            return ToolOutput::error("find aborted");
        }
        let path = entry.path();
        let Ok(relative) = path.strip_prefix(&root) else {
            continue;
        };
        if relative.as_os_str().is_empty() {
            continue;
        }
        let is_dir = entry.file_type().is_some_and(|t| t.is_dir());
        let is_file = entry.file_type().is_some_and(|t| t.is_file());
        if (is_dir && !want_dirs) || (is_file && !want_files) || (!is_dir && !is_file) {
            continue;
        }
        if let Some(TypeFilter::Extensions(exts)) = &filter
            && !matches_extension(&file_name(path), exts)
        {
            continue;
        }
        if !glob.matches(relative) {
            continue;
        }
        if results.len() >= limit {
            limit_hit = true;
            break;
        }
        let mut shown = relative.to_string_lossy().replace('\\', "/");
        if is_dir {
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
    ToolOutput::text(text).with_details(json!({
        "count": results.len(),
        "resultLimitReached": if limit_hit { json!(limit) } else { Value::Null },
    }))
}

fn file_name(path: &Path) -> String {
    path.file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::*;

    async fn find(ctx: &ToolContext, args: Value) -> String {
        FindTool.execute(args, ctx.clone()).await.text_content()
    }

    #[tokio::test]
    async fn finds_by_name_path_type_and_directories() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(dir.path().join("src/nested")).unwrap();
        std::fs::create_dir_all(dir.path().join("node_modules/pkg")).unwrap();
        std::fs::create_dir_all(dir.path().join(".hidden")).unwrap();
        std::fs::write(dir.path().join("src/a.rs"), "").unwrap();
        std::fs::write(dir.path().join("src/nested/b.rs"), "").unwrap();
        std::fs::write(dir.path().join("node_modules/pkg/c.rs"), "").unwrap();
        std::fs::write(dir.path().join(".hidden/d.rs"), "").unwrap();
        std::fs::write(dir.path().join("c.md"), "").unwrap();
        let ctx = ToolContext::new(dir.path());
        assert_eq!(
            find(&ctx, json!({"pattern": "*.rs"})).await,
            "src/a.rs\nsrc/nested/b.rs"
        );
        assert_eq!(find(&ctx, json!({"pattern": "src/*.rs"})).await, "src/a.rs");
        assert_eq!(
            find(&ctx, json!({"pattern": "*.py"})).await,
            "No files found matching pattern"
        );
        assert_eq!(
            find(&ctx, json!({"pattern": "*", "type": "d"})).await,
            "src/\nsrc/nested/"
        );
        assert_eq!(
            find(&ctx, json!({"pattern": "*", "type": "markdown"})).await,
            "c.md"
        );
        assert_eq!(
            find(&ctx, json!({"pattern": "n*", "directories": true})).await,
            "src/nested/"
        );
        assert_eq!(
            find(&ctx, json!({"pattern": "d.rs", "hidden": true})).await,
            ".hidden/d.rs"
        );
        assert!(
            find(&ctx, json!({"pattern": "*.rs", "limit": 1}))
                .await
                .ends_with("[1 results limit reached. Use limit=2 for more, or refine pattern]")
        );
        let out = FindTool
            .execute(json!({"pattern": "*", "path": "/"}), ctx)
            .await;
        assert!(
            out.text_content()
                .starts_with("path escapes workspace_root")
        );
    }
}
