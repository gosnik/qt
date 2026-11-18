use futures::future::BoxFuture;
use serde::Deserialize;
use serde_json::{Value, json};

use super::mutation::{MutationKind, require_approval};
use super::{Tool, ToolContext, ToolOutput, parse_args};

pub struct WriteTool;

#[derive(Deserialize)]
struct Args {
    path: String,
    content: String,
}

impl Tool for WriteTool {
    fn name(&self) -> &'static str {
        "write"
    }
    fn description(&self) -> String {
        "Write content to a file. Creates the file if it doesn't exist, overwrites if it does. Automatically creates parent directories.".into()
    }
    fn snippet(&self) -> &'static str {
        "Create or overwrite files"
    }
    fn guidelines(&self) -> &'static [&'static str] {
        &["Use write only for new files or complete rewrites."]
    }
    fn parameters(&self) -> Value {
        json!({
            "type": "object",
            "properties": {
                "path": { "type": "string", "description": "Path to the file to write (relative or absolute)" },
                "content": { "type": "string", "description": "Content to write to the file" }
            },
            "required": ["path", "content"]
        })
    }

    fn execute(&self, raw: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        Box::pin(async move {
            let args: Args = match parse_args("write", raw.clone()) {
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
                MutationKind::FileWrite,
                "write",
                format!("write {} bytes to {shown}", args.content.len()),
                &raw,
            ) {
                return queued;
            }
            if let Some(parent) = path.parent()
                && let Err(e) = tokio::fs::create_dir_all(parent).await
            {
                return ToolOutput::error(format!("Cannot create directory for {shown}: {e}"));
            }
            let existed = tokio::fs::try_exists(&path).await.unwrap_or(false);
            match tokio::fs::write(&path, args.content.as_bytes()).await {
                Ok(()) => ToolOutput::text(format!(
                    "Successfully wrote {} bytes to {shown}",
                    args.content.len()
                ))
                .with_details(
                    json!({ "path": path, "created": !existed, "bytes": args.content.len() }),
                ),
                Err(e) => ToolOutput::error(format!("Cannot write {shown}: {e}")),
            }
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn writes_with_parent_dirs_inside_the_workspace_only() {
        let dir = tempfile::tempdir().unwrap();
        let ctx = ToolContext::new(dir.path());
        let out = WriteTool
            .execute(
                json!({"path": "a/b/c.txt", "content": "hello"}),
                ctx.clone(),
            )
            .await;
        assert!(!out.is_error, "{}", out.text_content());
        assert_eq!(
            out.text_content(),
            "Successfully wrote 5 bytes to a/b/c.txt"
        );
        assert_eq!(
            std::fs::read_to_string(dir.path().join("a/b/c.txt")).unwrap(),
            "hello"
        );
        let outside = tempfile::tempdir().unwrap();
        let target = outside.path().join("x.txt");
        let out = WriteTool
            .execute(
                json!({"path": target.to_str().unwrap(), "content": "x"}),
                ctx,
            )
            .await;
        assert!(out.is_error);
        assert!(
            out.text_content()
                .starts_with("path escapes workspace_root")
        );
        assert!(!target.exists());
    }
}
