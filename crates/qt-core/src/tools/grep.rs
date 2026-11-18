use std::path::{Path, PathBuf};

use futures::future::BoxFuture;
use globset::{GlobBuilder, GlobMatcher};
use regex::{Regex, RegexBuilder};
use serde::Deserialize;
use serde_json::{Value, json};

use super::{Tool, ToolContext, ToolOutput, de_opt_usize, parse_args};
use crate::util::{DEFAULT_MAX_BYTES, resolve_path, shorten};

const DEFAULT_LIMIT: usize = 100;
const MAX_LINE_CHARS: usize = 500;

pub struct GrepTool;

#[derive(Deserialize)]
struct Args {
    pattern: String,
    #[serde(default)]
    path: Option<String>,
    #[serde(default)]
    glob: Option<String>,
    #[serde(rename = "ignoreCase", default)]
    ignore_case: Option<bool>,
    #[serde(default)]
    literal: Option<bool>,
    #[serde(default, deserialize_with = "de_opt_usize")]
    context: Option<usize>,
    #[serde(default, deserialize_with = "de_opt_usize")]
    limit: Option<usize>,
}

/// A glob filter applied to paths relative to the search root.
pub(crate) struct PathGlob {
    matcher: GlobMatcher,
    name_only: bool,
}

impl PathGlob {
    pub(crate) fn new(pattern: &str) -> Result<Self, String> {
        let glob = GlobBuilder::new(pattern)
            .literal_separator(true)
            .build()
            .map_err(|e| format!("Invalid glob pattern {pattern:?}: {e}"))?;
        Ok(Self {
            matcher: glob.compile_matcher(),
            name_only: !pattern.contains('/'),
        })
    }

    pub(crate) fn matches(&self, relative: &Path) -> bool {
        if self.matcher.is_match(relative) {
            return true;
        }
        self.name_only
            && relative
                .file_name()
                .is_some_and(|n| self.matcher.is_match(Path::new(n)))
    }
}

/// Walk files under `root`, honouring .gitignore and skipping hidden entries.
pub(crate) fn walker(root: &Path) -> ignore::Walk {
    ignore::WalkBuilder::new(root)
        .hidden(true)
        .git_ignore(true)
        .git_global(true)
        .git_exclude(true)
        .require_git(false)
        .follow_links(false)
        .sort_by_file_path(|a, b| a.cmp(b))
        .build()
}

impl Tool for GrepTool {
    fn name(&self) -> &'static str {
        "grep"
    }
    fn description(&self) -> String {
        format!(
            "Search file contents for a pattern. Returns matching lines with file paths and line numbers. Respects .gitignore. Output is truncated to {DEFAULT_LIMIT} matches or {}KB (whichever is hit first). Long lines are truncated to {MAX_LINE_CHARS} chars.",
            DEFAULT_MAX_BYTES / 1024
        )
    }
    fn snippet(&self) -> &'static str {
        "Search file contents for patterns (respects .gitignore)"
    }
    fn parameters(&self) -> Value {
        json!({
            "type": "object",
            "properties": {
                "pattern": { "type": "string", "description": "Search pattern (regex or literal string)" },
                "path": { "type": "string", "description": "Directory or file to search (default: current directory)" },
                "glob": { "type": "string", "description": "Filter files by glob pattern, e.g. '*.ts' or '**/*.spec.ts'" },
                "ignoreCase": { "type": "boolean", "description": "Case-insensitive search (default: false)" },
                "literal": { "type": "boolean", "description": "Treat pattern as literal string instead of regex (default: false)" },
                "context": { "type": "number", "description": "Number of lines to show before and after each match (default: 0)" },
                "limit": { "type": "number", "description": "Maximum number of matches to return (default: 100)" }
            },
            "required": ["pattern"]
        })
    }

    fn execute(&self, args: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        Box::pin(async move {
            let args: Args = match parse_args("grep", args) {
                Ok(a) => a,
                Err(e) => return e,
            };
            let cancel = ctx.cancel.clone();
            tokio::task::spawn_blocking(move || search(args, &ctx.cwd, &cancel))
                .await
                .unwrap_or_else(|e| ToolOutput::error(format!("grep failed: {e}")))
        })
    }
}

fn search(args: Args, cwd: &Path, cancel: &tokio_util::sync::CancellationToken) -> ToolOutput {
    let pattern = if args.literal.unwrap_or(false) {
        regex::escape(&args.pattern)
    } else {
        args.pattern.clone()
    };
    let regex: Regex = match RegexBuilder::new(&pattern)
        .case_insensitive(args.ignore_case.unwrap_or(false))
        .build()
    {
        Ok(r) => r,
        Err(e) => return ToolOutput::error(format!("Invalid regex: {e}")),
    };
    let glob = match args.glob.as_deref().map(PathGlob::new).transpose() {
        Ok(g) => g,
        Err(e) => return ToolOutput::error(e),
    };
    let root = resolve_path(cwd, args.path.as_deref().unwrap_or("."));
    if !root.exists() {
        return ToolOutput::error(format!("Path not found: {}", root.display()));
    }
    let limit = args.limit.filter(|l| *l > 0).unwrap_or(DEFAULT_LIMIT);
    let context = args.context.unwrap_or(0);
    let base: PathBuf = if root.is_file() {
        root.parent().map(Path::to_path_buf).unwrap_or_default()
    } else {
        root.clone()
    };

    let mut out = String::new();
    let mut matches = 0usize;
    let mut limit_hit = false;
    let mut bytes_hit = false;
    'files: for entry in walker(&root).flatten() {
        if cancel.is_cancelled() {
            return ToolOutput::error("grep aborted");
        }
        if !entry.file_type().is_some_and(|t| t.is_file()) {
            continue;
        }
        let path = entry.path();
        let relative = path.strip_prefix(&base).unwrap_or(path);
        if let Some(glob) = &glob
            && !glob.matches(relative)
        {
            continue;
        }
        let Ok(bytes) = std::fs::read(path) else {
            continue;
        };
        if bytes.iter().take(8192).any(|b| *b == 0) {
            continue;
        }
        let text = String::from_utf8_lossy(&bytes);
        let lines: Vec<&str> = text.lines().collect();
        let display = display_relative(cwd, path);
        let mut last_printed: Option<usize> = None;
        for (i, line) in lines.iter().enumerate() {
            if !regex.is_match(line) {
                continue;
            }
            if matches >= limit {
                limit_hit = true;
                break 'files;
            }
            matches += 1;
            let from = i.saturating_sub(context);
            let to = (i + context).min(lines.len().saturating_sub(1));
            if context > 0
                && let Some(prev) = last_printed
                && from > prev + 1
            {
                out.push_str("--\n");
            }
            for (j, ctx_line) in lines.iter().enumerate().take(to + 1).skip(from) {
                if last_printed.is_some_and(|p| j <= p) {
                    continue;
                }
                let sep = if j == i || regex.is_match(ctx_line) {
                    ':'
                } else {
                    '-'
                };
                out.push_str(&format!(
                    "{display}{sep}{}{sep} {}\n",
                    j + 1,
                    shorten(ctx_line, MAX_LINE_CHARS)
                ));
                last_printed = Some(j);
            }
            if out.len() > DEFAULT_MAX_BYTES {
                bytes_hit = true;
                break 'files;
            }
        }
    }
    if matches == 0 {
        return ToolOutput::text("No matches found");
    }
    let mut text = if out.len() > DEFAULT_MAX_BYTES {
        out[..crate::util::floor_char_boundary(&out, DEFAULT_MAX_BYTES)].to_string()
    } else {
        out
    };
    while text.ends_with('\n') {
        text.pop();
    }
    if limit_hit {
        text.push_str(&format!(
            "\n\n[{limit} matches limit reached. Use limit={} for more, or refine pattern]",
            limit * 2
        ));
    } else if bytes_hit {
        text.push_str(&format!(
            "\n\n[{}KB limit reached. Refine pattern or use glob]",
            DEFAULT_MAX_BYTES / 1024
        ));
    }
    ToolOutput::text(text).with_details(json!({ "matches": matches }))
}

/// Path relative to `cwd` with `/` separators on every platform.
fn display_relative(cwd: &Path, path: &Path) -> String {
    let shown = path
        .strip_prefix(cwd)
        .map(|p| p.display().to_string())
        .unwrap_or_else(|_| path.display().to_string());
    if cfg!(windows) {
        shown.replace('\\', "/")
    } else {
        shown
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fixture() -> tempfile::TempDir {
        let dir = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(dir.path().join("src")).unwrap();
        std::fs::write(
            dir.path().join("src/a.rs"),
            "fn main() {}\nlet x = 1;\nfn other() {}\n",
        )
        .unwrap();
        std::fs::write(dir.path().join("b.txt"), "FN upper\n").unwrap();
        std::fs::write(dir.path().join(".gitignore"), "ignored.rs\n").unwrap();
        std::fs::write(dir.path().join("ignored.rs"), "fn hidden() {}\n").unwrap();
        dir
    }

    #[tokio::test]
    async fn searches_with_glob_case_and_gitignore() {
        let dir = fixture();
        let ctx = ToolContext::new(dir.path());
        let out = GrepTool
            .execute(json!({"pattern": "^fn"}), ctx.clone())
            .await
            .text_content();
        assert_eq!(out, "src/a.rs:1: fn main() {}\nsrc/a.rs:3: fn other() {}");
        let out = GrepTool
            .execute(
                json!({"pattern": "fn", "ignoreCase": true, "glob": "*.txt"}),
                ctx.clone(),
            )
            .await;
        assert_eq!(out.text_content(), "b.txt:1: FN upper");
        let out = GrepTool
            .execute(json!({"pattern": "x = ", "context": 1}), ctx.clone())
            .await
            .text_content();
        assert_eq!(
            out,
            "src/a.rs-1- fn main() {}\nsrc/a.rs:2: let x = 1;\nsrc/a.rs-3- fn other() {}"
        );
        let out = GrepTool
            .execute(json!({"pattern": "(", "literal": true}), ctx.clone())
            .await;
        assert!(!out.is_error);
        let out = GrepTool
            .execute(json!({"pattern": "fn", "limit": 1}), ctx.clone())
            .await
            .text_content();
        assert!(out.contains("[1 matches limit reached"));
        let out = GrepTool
            .execute(json!({"pattern": "zzz"}), ctx)
            .await
            .text_content();
        assert_eq!(out, "No matches found");
    }
}
