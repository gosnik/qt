use base64::Engine as _;
use futures::future::BoxFuture;
use serde::Deserialize;
use serde_json::{Value, json};

use super::{Tool, ToolContext, ToolOutput, de_opt_usize, parse_args};
use crate::message::ContentBlock;
use crate::util::{
    DEFAULT_MAX_BYTES, DEFAULT_MAX_LINES, display_path, human_bytes, resolve_path, truncate_head,
};

const MAX_IMAGE_BYTES: u64 = 5 * 1024 * 1024;

pub struct ReadTool;

#[derive(Deserialize)]
struct Args {
    path: String,
    #[serde(default, deserialize_with = "de_opt_usize")]
    offset: Option<usize>,
    #[serde(default, deserialize_with = "de_opt_usize")]
    limit: Option<usize>,
}

pub(crate) fn image_mime(path: &std::path::Path) -> Option<&'static str> {
    let ext = path.extension()?.to_str()?.to_ascii_lowercase();
    Some(match ext.as_str() {
        "png" => "image/png",
        "jpg" | "jpeg" => "image/jpeg",
        "gif" => "image/gif",
        "webp" => "image/webp",
        _ => return None,
    })
}

impl Tool for ReadTool {
    fn name(&self) -> &'static str {
        "read"
    }
    fn description(&self) -> String {
        format!(
            "Read the contents of a file. Supports text files and images (jpg, png, gif, webp). Images are sent as attachments. For text files, output is truncated to {DEFAULT_MAX_LINES} lines or {}KB (whichever is hit first). Use offset/limit for large files. When you need the full file, continue with offset until complete.",
            DEFAULT_MAX_BYTES / 1024
        )
    }
    fn snippet(&self) -> &'static str {
        "Read file contents"
    }
    fn guidelines(&self) -> &'static [&'static str] {
        &["Use read to examine files instead of cat or sed."]
    }
    fn parameters(&self) -> Value {
        json!({
            "type": "object",
            "properties": {
                "path": { "type": "string", "description": "Path to the file to read (relative or absolute)" },
                "offset": { "type": "number", "description": "Line number to start reading from (1-indexed)" },
                "limit": { "type": "number", "description": "Maximum number of lines to read" }
            },
            "required": ["path"]
        })
    }

    fn execute(&self, args: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        Box::pin(async move {
            let args: Args = match parse_args("read", args) {
                Ok(a) => a,
                Err(e) => return e,
            };
            let path = resolve_path(&ctx.cwd, &args.path);
            let shown = display_path(&ctx.cwd, &path);
            let meta = match tokio::fs::metadata(&path).await {
                Ok(m) => m,
                Err(e) => return ToolOutput::error(format!("Cannot read {shown}: {e}")),
            };
            if meta.is_dir() {
                return ToolOutput::error(format!(
                    "{shown} is a directory. Use ls to list directory contents."
                ));
            }
            if let Some(mime) = image_mime(&path) {
                if meta.len() > MAX_IMAGE_BYTES {
                    return ToolOutput::error(format!(
                        "Image {shown} is {}, larger than the {} limit.",
                        human_bytes(meta.len() as usize),
                        human_bytes(MAX_IMAGE_BYTES as usize)
                    ));
                }
                return match tokio::fs::read(&path).await {
                    Ok(bytes) => ToolOutput {
                        content: vec![
                            ContentBlock::text(format!("Read image file [{mime}]")),
                            ContentBlock::Image {
                                data: base64::engine::general_purpose::STANDARD.encode(bytes),
                                mime_type: mime.into(),
                            },
                        ],
                        ..Default::default()
                    },
                    Err(e) => ToolOutput::error(format!("Cannot read {shown}: {e}")),
                };
            }
            let bytes = match tokio::fs::read(&path).await {
                Ok(b) => b,
                Err(e) => return ToolOutput::error(format!("Cannot read {shown}: {e}")),
            };
            read_text(
                &String::from_utf8_lossy(&bytes),
                args.offset,
                args.limit,
                &shown,
            )
        })
    }
}

fn read_text(text: &str, offset: Option<usize>, limit: Option<usize>, shown: &str) -> ToolOutput {
    let lines: Vec<&str> = if text.is_empty() {
        Vec::new()
    } else {
        text.strip_suffix('\n')
            .unwrap_or(text)
            .split('\n')
            .collect()
    };
    let total = lines.len();
    let start = offset.unwrap_or(1).max(1);
    if total == 0 {
        return ToolOutput::text("(empty file)");
    }
    if start > total {
        return ToolOutput::error(format!(
            "Offset {start} is beyond end of file ({total} lines total)"
        ));
    }
    let end_exclusive = match limit {
        Some(limit) => (start - 1 + limit.max(1)).min(total),
        None => total,
    };
    let selected = lines[start - 1..end_exclusive].join("\n");
    let first_len = lines[start - 1].len();
    if first_len > DEFAULT_MAX_BYTES {
        return ToolOutput::text(format!(
            "[Line {start} is {}, exceeds {}KB limit. Use bash: sed -n '{start}p' {shown} | head -c {DEFAULT_MAX_BYTES}]",
            human_bytes(first_len),
            DEFAULT_MAX_BYTES / 1024
        ));
    }
    let t = truncate_head(&selected, DEFAULT_MAX_LINES, DEFAULT_MAX_BYTES);
    let mut out = t.text;
    let last_shown = start - 1 + t.kept_lines;
    if t.truncated {
        out.push_str(&format!(
            "\n\n[Showing lines {start}-{last_shown} of {total}. Use offset={} to continue.]",
            last_shown + 1
        ));
    } else if end_exclusive < total {
        out.push_str(&format!(
            "\n\n[{} more lines in file. Use offset={} to continue.]",
            total - end_exclusive,
            end_exclusive + 1
        ));
    }
    ToolOutput::text(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn offsets_and_limits() {
        let text = "a\nb\nc\nd\n";
        assert_eq!(
            read_text(text, None, None, "f").text_content(),
            "a\nb\nc\nd"
        );
        let out = read_text(text, Some(2), Some(2), "f").text_content();
        assert_eq!(
            out,
            "b\nc\n\n[1 more lines in file. Use offset=4 to continue.]"
        );
        assert!(read_text(text, Some(9), None, "f").is_error);
        assert_eq!(
            read_text("", None, None, "f").text_content(),
            "(empty file)"
        );
    }

    #[test]
    fn truncates_long_files() {
        let text: String = (1..=2500).map(|i| format!("{i}\n")).collect();
        let out = read_text(&text, None, None, "f").text_content();
        assert!(out.ends_with("[Showing lines 1-2000 of 2500. Use offset=2001 to continue.]"));
    }

    #[tokio::test]
    async fn reads_images_and_rejects_dirs() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(dir.path().join("x.png"), [137u8, 80, 78, 71]).unwrap();
        let ctx = ToolContext::new(dir.path());
        let out = ReadTool
            .execute(json!({"path": "x.png"}), ctx.clone())
            .await;
        assert!(matches!(out.content[1], ContentBlock::Image { .. }));
        let out = ReadTool.execute(json!({"path": "."}), ctx).await;
        assert!(out.is_error);
    }
}
