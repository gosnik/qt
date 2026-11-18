use std::path::Path;

use futures::future::BoxFuture;
use serde::Deserialize;
use serde_json::{Value, json};

use super::grep::PathGlob;
use super::{Tool, ToolContext, ToolOutput, de_opt_bool, de_opt_usize, parse_args};
use crate::util::{DEFAULT_MAX_BYTES, truncate_head};

const DEFAULT_LIMIT: usize = 500;

pub struct LsTool;

#[derive(Deserialize)]
struct Args {
    #[serde(default)]
    path: Option<String>,
    #[serde(default, deserialize_with = "de_opt_usize")]
    limit: Option<usize>,
    #[serde(default, deserialize_with = "de_opt_bool")]
    recursive: Option<bool>,
    #[serde(default, deserialize_with = "de_opt_usize")]
    depth: Option<usize>,
    #[serde(default)]
    ignore: Option<Vec<String>>,
}

struct Entry {
    /// Path relative to the listed directory, `/`-separated.
    name: String,
    is_dir: bool,
}

impl Tool for LsTool {
    fn name(&self) -> &'static str {
        "ls"
    }
    fn description(&self) -> String {
        format!(
            "List directory contents. Returns entries sorted alphabetically, with '/' suffix for directories. Includes dotfiles. Output is truncated to {DEFAULT_LIMIT} entries or {}KB (whichever is hit first).",
            DEFAULT_MAX_BYTES / 1024
        )
    }
    fn snippet(&self) -> &'static str {
        "List directory contents"
    }
    fn parameters(&self) -> Value {
        json!({
            "type": "object",
            "properties": {
                "path": { "type": "string", "description": "Directory to list (default: current directory)" },
                "limit": { "type": "number", "description": "Maximum number of entries to return (default: 500)" },
                "recursive": { "type": "boolean", "description": "List the whole tree instead of only direct children" },
                "depth": { "type": "number", "description": "Maximum depth below the directory when recursive (0 = direct children only); implies recursive" },
                "ignore": { "type": "array", "items": { "type": "string" }, "description": "Glob patterns of entries to leave out, e.g. ['target', '*.log']" }
            }
        })
    }
    fn parallel_safe(&self) -> bool {
        true
    }

    fn execute(&self, args: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        Box::pin(async move {
            let args: Args = match parse_args("ls", args) {
                Ok(a) => a,
                Err(e) => return e,
            };
            let dir = match ctx.resolve_path(args.path.as_deref().unwrap_or(".")) {
                Ok(r) => r.path,
                Err(e) => return e,
            };
            tokio::task::spawn_blocking(move || list(args, &dir, &ctx))
                .await
                .unwrap_or_else(|e| ToolOutput::error(format!("ls failed: {e}")))
        })
    }
}

fn list(args: Args, dir: &Path, ctx: &ToolContext) -> ToolOutput {
    let shown = ctx.display(dir);
    let limit = args.limit.filter(|l| *l > 0).unwrap_or(DEFAULT_LIMIT);
    let ignore: Vec<PathGlob> = match args
        .ignore
        .unwrap_or_default()
        .iter()
        .map(|p| PathGlob::new(p))
        .collect::<Result<_, _>>()
    {
        Ok(globs) => globs,
        Err(e) => return ToolOutput::error(e),
    };
    let recursive = args.recursive.unwrap_or(false) || args.depth.is_some();
    let max_depth = if recursive { args.depth } else { Some(0) };
    let meta = match std::fs::metadata(dir) {
        Ok(m) => m,
        Err(e) => return ToolOutput::error(format!("Cannot list {shown}: {e}")),
    };
    if !meta.is_dir() {
        return ToolOutput::error(format!("Not a directory: {shown}"));
    }
    let mut entries = Vec::new();
    if let Err(e) = collect(dir, "", 0, max_depth, &ignore, &ctx.cancel, &mut entries) {
        return ToolOutput::error(format!("Cannot list {shown}: {e}"));
    }
    if ctx.cancel.is_cancelled() {
        return ToolOutput::error("ls aborted");
    }
    if entries.is_empty() {
        return ToolOutput::text("(empty directory)");
    }
    entries.sort_by(|a, b| {
        a.name
            .to_lowercase()
            .cmp(&b.name.to_lowercase())
            .then_with(|| a.name.cmp(&b.name))
    });
    let total = entries.len();
    entries.truncate(limit);
    let lines: Vec<String> = entries
        .iter()
        .map(|e| {
            if e.is_dir {
                format!("{}/", e.name)
            } else {
                e.name.clone()
            }
        })
        .collect();
    let t = truncate_head(&lines.join("\n"), usize::MAX, DEFAULT_MAX_BYTES);
    let mut text = t.text;
    if total > limit {
        text.push_str(&format!(
            "\n\n[{limit} entries limit reached. Use limit={} for more]",
            limit * 2
        ));
    } else if t.truncated {
        text.push_str(&format!(
            "\n\n[{}KB limit reached]",
            DEFAULT_MAX_BYTES / 1024
        ));
    }
    ToolOutput::text(text).with_details(json!({
        "count": total.min(limit),
        "recursive": recursive,
        "depth": args.depth,
        "entryLimitReached": if total > limit { json!(limit) } else { Value::Null },
    }))
}

/// Gather entries below `dir`; symlinked directories are listed but not
/// followed, and `.git` contents are not listed.
fn collect(
    dir: &Path,
    prefix: &str,
    depth: usize,
    max_depth: Option<usize>,
    ignore: &[PathGlob],
    cancel: &tokio_util::sync::CancellationToken,
    out: &mut Vec<Entry>,
) -> std::io::Result<()> {
    // A generous cap so a huge tree cannot exhaust memory before sorting.
    const MAX_COLLECTED: usize = 100_000;
    for entry in std::fs::read_dir(dir)? {
        if cancel.is_cancelled() || out.len() >= MAX_COLLECTED {
            return Ok(());
        }
        let Ok(entry) = entry else { continue };
        let name = entry.file_name().to_string_lossy().into_owned();
        let relative = if prefix.is_empty() {
            name.clone()
        } else {
            format!("{prefix}/{name}")
        };
        if ignore
            .iter()
            .any(|g| g.matches(Path::new(&relative)) || g.matches(Path::new(&name)))
        {
            continue;
        }
        let file_type = entry.file_type()?;
        let is_dir = if file_type.is_symlink() {
            std::fs::metadata(entry.path())
                .map(|m| m.is_dir())
                .unwrap_or(false)
        } else {
            file_type.is_dir()
        };
        out.push(Entry {
            name: relative.clone(),
            is_dir,
        });
        let descend = is_dir
            && !file_type.is_symlink()
            && name != ".git"
            && max_depth.is_none_or(|max| depth < max);
        if descend {
            // Unreadable subdirectories are listed without their contents.
            let _ = collect(
                &entry.path(),
                &relative,
                depth + 1,
                max_depth,
                ignore,
                cancel,
                out,
            );
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn lists_sorted_with_dir_suffix() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::create_dir(dir.path().join("Sub")).unwrap();
        std::fs::write(dir.path().join("b.txt"), "").unwrap();
        std::fs::write(dir.path().join(".hidden"), "").unwrap();
        let out = LsTool
            .execute(json!({}), ToolContext::new(dir.path()))
            .await;
        assert_eq!(out.text_content(), ".hidden\nb.txt\nSub/");
        let out = LsTool
            .execute(json!({"limit": 1}), ToolContext::new(dir.path()))
            .await;
        assert!(out.text_content().contains("[1 entries limit reached"));
    }

    #[tokio::test]
    async fn recursive_depth_and_ignore() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(dir.path().join("a/b/c")).unwrap();
        std::fs::create_dir_all(dir.path().join("target/debug")).unwrap();
        std::fs::write(dir.path().join("a/x.log"), "").unwrap();
        std::fs::write(dir.path().join("a/b/y.rs"), "").unwrap();
        let ctx = ToolContext::new(dir.path());
        let out = LsTool
            .execute(
                json!({"recursive": true, "ignore": ["target", "*.log"]}),
                ctx.clone(),
            )
            .await;
        assert_eq!(out.text_content(), "a/\na/b/\na/b/c/\na/b/y.rs");
        let out = LsTool
            .execute(json!({"depth": 1, "ignore": ["target"]}), ctx.clone())
            .await;
        assert_eq!(out.text_content(), "a/\na/b/\na/x.log");
        let out = LsTool
            .execute(json!({"path": "a/x.log"}), ctx.clone())
            .await;
        assert_eq!(out.text_content(), "Not a directory: a/x.log");
        let out = LsTool.execute(json!({"path": ".."}), ctx).await;
        assert!(out.is_error);
    }
}
