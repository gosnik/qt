use futures::future::BoxFuture;
use serde::Deserialize;
use serde_json::{Value, json};
use tokio::io::AsyncWriteExt as _;

use super::mutation::{MutationKind, require_approval};
use super::{Tool, ToolContext, ToolOutput, parse_args};

/// Archived `append_text`: append text to a file, creating it (and its
/// parent directories) when missing.
pub struct AppendTextTool;

#[derive(Deserialize)]
struct Args {
    path: String,
    text: String,
}

impl Tool for AppendTextTool {
    fn name(&self) -> &'static str {
        "append_text"
    }
    fn description(&self) -> String {
        "Append text to a path permitted by the runtime filesystem policy. Creates the file and parent directories when missing.".into()
    }
    fn snippet(&self) -> &'static str {
        "Append text to the end of a file"
    }
    fn parameters(&self) -> Value {
        json!({
            "type": "object",
            "properties": {
                "path": { "type": "string", "description": "Relative or absolute path to the text file to append to." },
                "text": { "type": "string", "description": "Text content to append to the file." }
            },
            "required": ["path", "text"],
            "additionalProperties": false
        })
    }

    fn execute(&self, raw: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        Box::pin(async move {
            let args: Args = match parse_args("append_text", raw.clone()) {
                Ok(a) => a,
                Err(e) => return e,
            };
            let path = match ctx.resolve_path(&args.path) {
                Ok(r) => r.path,
                Err(e) => return e,
            };
            let shown = ctx.display(&path);
            if let Some(queued) = require_approval(
                &ctx,
                MutationKind::FileAppend,
                "append_text",
                format!("append {} bytes to {shown}", args.text.len()),
                &raw,
            ) {
                return queued;
            }
            if let Some(parent) = path.parent()
                && let Err(e) = tokio::fs::create_dir_all(parent).await
            {
                return ToolOutput::error(format!("Cannot create directory for {shown}: {e}"));
            }
            let result = async {
                let mut file = tokio::fs::OpenOptions::new()
                    .create(true)
                    .append(true)
                    .open(&path)
                    .await?;
                file.write_all(args.text.as_bytes()).await?;
                file.flush().await
            }
            .await;
            match result {
                Ok(()) => ToolOutput::text(format!(
                    "Successfully appended {} bytes to {shown}",
                    args.text.len()
                ))
                .with_details(json!({ "path": path, "bytes": args.text.len(), "appended": true })),
                Err(e) => ToolOutput::error(format!("Cannot append to {shown}: {e}")),
            }
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn appends_and_creates() {
        let dir = tempfile::tempdir().unwrap();
        let ctx = ToolContext::new(dir.path());
        for chunk in ["a\n", "b\n"] {
            let out = AppendTextTool
                .execute(json!({"path": "log/x.txt", "text": chunk}), ctx.clone())
                .await;
            assert!(!out.is_error, "{}", out.text_content());
        }
        assert_eq!(
            std::fs::read_to_string(dir.path().join("log/x.txt")).unwrap(),
            "a\nb\n"
        );
        let out = AppendTextTool
            .execute(json!({"path": "../escape.txt", "text": "x"}), ctx)
            .await;
        assert!(out.is_error);
    }
}
