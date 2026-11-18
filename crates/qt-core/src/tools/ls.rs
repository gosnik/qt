use futures::future::BoxFuture;
use serde::Deserialize;
use serde_json::{Value, json};

use super::{Tool, ToolContext, ToolOutput, de_opt_usize, parse_args};
use crate::util::{DEFAULT_MAX_BYTES, display_path, resolve_path, truncate_head};

const DEFAULT_LIMIT: usize = 500;

pub struct LsTool;

#[derive(Deserialize)]
struct Args {
    #[serde(default)]
    path: Option<String>,
    #[serde(default, deserialize_with = "de_opt_usize")]
    limit: Option<usize>,
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
                "limit": { "type": "number", "description": "Maximum number of entries to return (default: 500)" }
            }
        })
    }

    fn execute(&self, args: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        Box::pin(async move {
            let args: Args = match parse_args("ls", args) {
                Ok(a) => a,
                Err(e) => return e,
            };
            let dir = resolve_path(&ctx.cwd, args.path.as_deref().unwrap_or("."));
            let shown = display_path(&ctx.cwd, &dir);
            let limit = args.limit.filter(|l| *l > 0).unwrap_or(DEFAULT_LIMIT);
            let mut reader = match tokio::fs::read_dir(&dir).await {
                Ok(r) => r,
                Err(e) => return ToolOutput::error(format!("Cannot list {shown}: {e}")),
            };
            let mut entries = Vec::new();
            while let Ok(Some(entry)) = reader.next_entry().await {
                let mut name = entry.file_name().to_string_lossy().into_owned();
                let is_dir = match entry.file_type().await {
                    Ok(t) if t.is_symlink() => tokio::fs::metadata(entry.path())
                        .await
                        .map(|m| m.is_dir())
                        .unwrap_or(false),
                    Ok(t) => t.is_dir(),
                    Err(_) => false,
                };
                if is_dir {
                    name.push('/');
                }
                entries.push(name);
            }
            if entries.is_empty() {
                return ToolOutput::text("(empty directory)");
            }
            entries.sort_by(|a, b| {
                a.to_lowercase()
                    .cmp(&b.to_lowercase())
                    .then_with(|| a.cmp(b))
            });
            let total = entries.len();
            entries.truncate(limit);
            let t = truncate_head(&entries.join("\n"), usize::MAX, DEFAULT_MAX_BYTES);
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
            ToolOutput::text(text)
        })
    }
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
}
