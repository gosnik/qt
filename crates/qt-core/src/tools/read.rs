use std::collections::HashMap;
use std::path::Path;
use std::sync::{Arc, Mutex};

use base64::Engine as _;
use futures::future::BoxFuture;
use serde::Deserialize;
use serde_json::{Value, json};

use super::{Tool, ToolContext, ToolOutput, de_opt_usize, parse_args};
use crate::message::ContentBlock;
use crate::util::{DEFAULT_MAX_BYTES, DEFAULT_MAX_LINES, human_bytes, truncate_head};

/// Inline image limit used when no resize hook is installed.
const MAX_IMAGE_BYTES: usize = 5 * 1024 * 1024;
const REPEATED_READ_WARN_COUNT: usize = 2;
const REPEATED_READ_BLOCK_COUNT: usize = 4;
const REPEATED_READ_MESSAGE: &str = "This unchanged file range has already been read.";
const REPEATED_READ_REFUSAL: &str = "read_text refused a duplicate unchanged read. Reuse the prior result or read a different offset/limit.";
const NON_VISION_NOTE: &str =
    "[Current model does not support images. The image will be omitted from this request.]";
const RESIZE_FAILED_NOTE: &str =
    "[Image omitted: could not be resized below the inline image size limit.]";

/// Image bytes handed to the resize hook.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ImageInput {
    pub bytes: Vec<u8>,
    pub mime_type: String,
}

/// Result of a successful resize.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ResizedImage {
    pub bytes: Vec<u8>,
    pub mime_type: String,
    /// Pi's dimension note, e.g. "[Image: original 4000x3000, displayed at …]".
    pub dimension_note: Option<String>,
}

/// Resize hook. Returns `None` when the image cannot be brought under the
/// inline size limit. Image processing itself is owned by the provider
/// layer (`images.autoResize`, 2000×2000 / 4.5 MiB); this module only calls it.
pub type ImageResizeFn = Arc<dyn Fn(&ImageInput) -> Option<ResizedImage> + Send + Sync>;

/// Image handling for `read`.
#[derive(Clone)]
pub struct ImageOptions {
    /// Settings `images.autoResize` (default `true`).
    pub auto_resize: bool,
    /// Resize implementation; without one, images over 5 MiB are omitted.
    pub resize: Option<ImageResizeFn>,
    /// `Some(false)` adds the "model does not support images" note.
    pub model_supports_images: Option<bool>,
}

impl Default for ImageOptions {
    fn default() -> Self {
        Self {
            auto_resize: true,
            resize: None,
            model_supports_images: None,
        }
    }
}

/// `read` with the archived repeated-read guard. The guard state is shared
/// by clones of the registry holding this tool.
#[derive(Default)]
pub struct ReadTool {
    reads: Mutex<HashMap<String, usize>>,
}

impl ReadTool {
    pub fn new() -> Self {
        Self::default()
    }

    fn count_read(&self, fingerprint: String) -> usize {
        let mut reads = self.reads.lock().unwrap_or_else(|e| e.into_inner());
        let count = reads.entry(fingerprint).or_insert(0);
        *count += 1;
        *count
    }
}

#[derive(Deserialize)]
struct Args {
    path: String,
    #[serde(default, deserialize_with = "de_opt_usize")]
    offset: Option<usize>,
    #[serde(default, deserialize_with = "de_opt_usize")]
    limit: Option<usize>,
}

/// Sniff the image type from magic bytes (png, jpeg, gif, webp).
pub(crate) fn sniff_image_mime(bytes: &[u8]) -> Option<&'static str> {
    if bytes.starts_with(&[0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A]) {
        Some("image/png")
    } else if bytes.starts_with(&[0xFF, 0xD8, 0xFF]) {
        Some("image/jpeg")
    } else if bytes.starts_with(b"GIF87a") || bytes.starts_with(b"GIF89a") {
        Some("image/gif")
    } else if bytes.len() >= 12 && &bytes[..4] == b"RIFF" && &bytes[8..12] == b"WEBP" {
        Some("image/webp")
    } else {
        None
    }
}

/// Image type implied by the file extension.
pub(crate) fn image_mime(path: &Path) -> Option<&'static str> {
    let ext = path.extension()?.to_str()?.to_ascii_lowercase();
    Some(match ext.as_str() {
        "png" => "image/png",
        "jpg" | "jpeg" => "image/jpeg",
        "gif" => "image/gif",
        "webp" => "image/webp",
        _ => return None,
    })
}

fn fingerprint(path: &Path, meta: &std::fs::Metadata, args: &Args) -> String {
    let modified = meta
        .modified()
        .ok()
        .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    format!(
        "{}|{:?}|{:?}|{}|{modified}",
        path.display(),
        args.offset,
        args.limit,
        meta.len()
    )
}

/// Up to five siblings whose names resemble the missing file name.
fn similar_paths(ctx: &ToolContext, missing: &Path) -> Vec<String> {
    let Some(parent) = missing.parent() else {
        return Vec::new();
    };
    let wanted = missing
        .file_name()
        .map(|n| n.to_string_lossy().to_lowercase())
        .unwrap_or_default();
    let Ok(entries) = std::fs::read_dir(parent) else {
        return Vec::new();
    };
    let mut names: Vec<_> = entries
        .flatten()
        .map(|e| e.path())
        .filter(|p| {
            let name = p
                .file_name()
                .map(|n| n.to_string_lossy().to_lowercase())
                .unwrap_or_default();
            (!wanted.is_empty() && name.contains(&wanted))
                || (!name.is_empty() && wanted.contains(&name))
        })
        .map(|p| ctx.display(&p))
        .collect();
    names.sort();
    names.truncate(5);
    names
}

fn directory_listing(dir: &Path) -> String {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return "(empty directory)".into();
    };
    let mut names: Vec<String> = entries
        .flatten()
        .map(|e| {
            let mut name = e.file_name().to_string_lossy().into_owned();
            if e.path().is_dir() {
                name.push('/');
            }
            name
        })
        .collect();
    if names.is_empty() {
        return "(empty directory)".into();
    }
    names.sort_by(|a, b| {
        a.to_lowercase()
            .cmp(&b.to_lowercase())
            .then_with(|| a.cmp(b))
    });
    names.join("\n")
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
    fn parallel_safe(&self) -> bool {
        true
    }

    fn execute(&self, args: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        let args: Args = match parse_args("read", args) {
            Ok(a) => a,
            Err(e) => return Box::pin(async move { e }),
        };
        let resolved = match ctx.resolve_read_path(&args.path) {
            Ok(r) => r,
            Err(e) => return Box::pin(async move { e }),
        };
        let path = resolved.path;
        let shown = ctx.display(&path);
        let meta = match std::fs::metadata(&path) {
            Ok(m) => m,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                let suggestions = similar_paths(&ctx, &path);
                let mut msg = format!("File not found: {shown}");
                if let Some(first) = suggestions.first() {
                    msg.push_str(&format!(". Did you mean {first}?"));
                }
                let out = ToolOutput::error(msg);
                return Box::pin(async move {
                    if suggestions.is_empty() {
                        out
                    } else {
                        out.with_details(json!({ "suggestions": suggestions }))
                    }
                });
            }
            Err(e) => {
                return Box::pin(
                    async move { ToolOutput::error(format!("Cannot read {shown}: {e}")) },
                );
            }
        };
        if meta.is_dir() {
            let listing = directory_listing(&path);
            return Box::pin(async move {
                ToolOutput::text(listing).with_details(json!({ "directory": true, "path": shown }))
            });
        }
        let repeat = self.count_read(fingerprint(&resolved.canonical, &meta, &args));
        if repeat >= REPEATED_READ_BLOCK_COUNT {
            return Box::pin(async move {
                ToolOutput::error(REPEATED_READ_REFUSAL)
                    .with_details(json!({ "repeatedRead": { "count": repeat, "blocked": true } }))
            });
        }
        Box::pin(async move {
            let bytes = match tokio::fs::read(&path).await {
                Ok(b) => b,
                Err(e) => return ToolOutput::error(format!("Cannot read {shown}: {e}")),
            };
            // Images are recognised by content, so a mislabelled file is read as text.
            let mut out = match sniff_image_mime(&bytes) {
                Some(mime) => read_image(bytes, mime, &ctx),
                None => read_text(
                    &String::from_utf8_lossy(&bytes),
                    args.offset,
                    args.limit,
                    &shown,
                ),
            };
            if repeat >= REPEATED_READ_WARN_COUNT && !out.is_error {
                out.content.push(ContentBlock::text(format!(
                    "\n\n[{REPEATED_READ_MESSAGE} Read {repeat} times; reuse the earlier result.]"
                )));
                let mut details = out.details.take().unwrap_or_else(|| json!({}));
                details["repeatedRead"] =
                    json!({ "count": repeat, "message": REPEATED_READ_MESSAGE });
                out.details = Some(details);
            }
            out
        })
    }
}

fn read_image(bytes: Vec<u8>, mime: &str, ctx: &ToolContext) -> ToolOutput {
    let model_note = (ctx.images.model_supports_images == Some(false)).then_some(NON_VISION_NOTE);
    let with_model_note = |mut note: String| {
        if let Some(extra) = model_note {
            note.push('\n');
            note.push_str(extra);
        }
        note
    };
    let input = ImageInput {
        bytes,
        mime_type: mime.to_string(),
    };
    let resized = match (&ctx.images.resize, ctx.images.auto_resize) {
        (Some(resize), true) => match resize(&input) {
            Some(r) => Some(r),
            None => {
                let note =
                    with_model_note(format!("Read image file [{mime}]\n{RESIZE_FAILED_NOTE}"));
                return ToolOutput::text(note).with_details(json!({ "imageOmitted": true }));
            }
        },
        _ => {
            if input.bytes.len() > MAX_IMAGE_BYTES {
                let note = with_model_note(format!(
                    "Read image file [{mime}]\n[Image omitted: {} exceeds the {} inline image limit.]",
                    human_bytes(input.bytes.len()),
                    human_bytes(MAX_IMAGE_BYTES)
                ));
                return ToolOutput::text(note).with_details(json!({ "imageOmitted": true }));
            }
            None
        }
    };
    let (data, mime_type, dimension_note) = match resized {
        Some(r) => (r.bytes, r.mime_type, r.dimension_note),
        None => (input.bytes, input.mime_type, None),
    };
    let mut note = format!("Read image file [{mime_type}]");
    if let Some(dim) = dimension_note {
        note.push('\n');
        note.push_str(&dim);
    }
    let note = with_model_note(note);
    ToolOutput {
        content: vec![
            ContentBlock::text(note),
            ContentBlock::Image {
                data: base64::engine::general_purpose::STANDARD.encode(data),
                mime_type,
            },
        ],
        ..Default::default()
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
        ))
        .with_details(json!({ "truncation": { "truncated": true, "truncatedBy": "bytes", "firstLineExceedsLimit": true } }));
    }
    let t = truncate_head(&selected, DEFAULT_MAX_LINES, DEFAULT_MAX_BYTES);
    let mut out = t.text;
    let last_shown = start - 1 + t.kept_lines;
    let mut details = None;
    if t.truncated {
        out.push_str(&format!(
            "\n\n[Showing lines {start}-{last_shown} of {total}. Use offset={} to continue.]",
            last_shown + 1
        ));
        details = Some(json!({ "truncation": {
            "truncated": true,
            "truncatedBy": if t.kept_lines >= DEFAULT_MAX_LINES { "lines" } else { "bytes" },
            "totalLines": total,
            "outputLines": t.kept_lines,
            "maxLines": DEFAULT_MAX_LINES,
            "maxBytes": DEFAULT_MAX_BYTES,
        }}));
    } else if end_exclusive < total {
        out.push_str(&format!(
            "\n\n[{} more lines in file. Use offset={} to continue.]",
            total - end_exclusive,
            end_exclusive + 1
        ));
    }
    let out = ToolOutput::text(out);
    match details {
        Some(d) => out.with_details(d),
        None => out,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const PNG: [u8; 8] = [0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A];

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
        let out = read_text(&text, None, None, "f");
        assert!(
            out.text_content()
                .ends_with("[Showing lines 1-2000 of 2500. Use offset=2001 to continue.]")
        );
        assert_eq!(out.details.unwrap()["truncation"]["truncatedBy"], "lines");
    }

    #[tokio::test]
    async fn reads_images_lists_dirs_and_suggests_paths() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(dir.path().join("x.png"), PNG).unwrap();
        std::fs::write(dir.path().join("notes.txt"), "hi").unwrap();
        std::fs::create_dir(dir.path().join("sub")).unwrap();
        let ctx = ToolContext::new(dir.path());
        let tool = ReadTool::new();
        let out = tool.execute(json!({"path": "x.png"}), ctx.clone()).await;
        assert_eq!(out.text_content(), "Read image file [image/png]");
        assert!(matches!(out.content[1], ContentBlock::Image { .. }));
        let out = tool.execute(json!({"path": "."}), ctx.clone()).await;
        assert_eq!(out.text_content(), "notes.txt\nsub/\nx.png");
        let out = tool.execute(json!({"path": "note"}), ctx.clone()).await;
        assert!(out.is_error);
        assert_eq!(
            out.text_content(),
            "File not found: note. Did you mean notes.txt?"
        );
        let out = tool.execute(json!({"path": "/etc/hostname"}), ctx).await;
        assert!(
            out.text_content()
                .starts_with("path escapes workspace_root")
        );
    }

    #[tokio::test]
    async fn resize_hook_and_model_note() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(dir.path().join("x.png"), PNG).unwrap();
        let resize: ImageResizeFn = Arc::new(|input: &ImageInput| {
            Some(ResizedImage {
                bytes: input.bytes[..4].to_vec(),
                mime_type: "image/jpeg".into(),
                dimension_note: Some("[Image resized]".into()),
            })
        });
        let ctx = ToolContext::new(dir.path()).with_image_options(ImageOptions {
            auto_resize: true,
            resize: Some(resize),
            model_supports_images: Some(false),
        });
        let out = ReadTool::new()
            .execute(json!({"path": "x.png"}), ctx.clone())
            .await;
        assert_eq!(
            out.text_content(),
            format!("Read image file [image/jpeg]\n[Image resized]\n{NON_VISION_NOTE}")
        );
        let failing: ImageResizeFn = Arc::new(|_: &ImageInput| None);
        let ctx = ctx.with_image_options(ImageOptions {
            resize: Some(failing),
            ..ImageOptions::default()
        });
        let out = ReadTool::new().execute(json!({"path": "x.png"}), ctx).await;
        assert_eq!(out.content.len(), 1);
        assert!(out.text_content().ends_with(RESIZE_FAILED_NOTE));
    }

    #[tokio::test]
    async fn repeated_reads_warn_then_block_until_the_file_changes() {
        let dir = tempfile::tempdir().unwrap();
        let file = dir.path().join("a.txt");
        std::fs::write(&file, "one\n").unwrap();
        let ctx = ToolContext::new(dir.path());
        let tool = ReadTool::new();
        let args = json!({"path": "a.txt"});
        let first = tool.execute(args.clone(), ctx.clone()).await;
        assert_eq!(first.text_content(), "one");
        let second = tool.execute(args.clone(), ctx.clone()).await;
        assert!(second.text_content().contains(REPEATED_READ_MESSAGE));
        assert_eq!(second.details.unwrap()["repeatedRead"]["count"], 2);
        let _third = tool.execute(args.clone(), ctx.clone()).await;
        let fourth = tool.execute(args.clone(), ctx.clone()).await;
        assert!(fourth.is_error);
        assert_eq!(fourth.text_content(), REPEATED_READ_REFUSAL);
        // A different range is a different fingerprint.
        let other = tool
            .execute(json!({"path": "a.txt", "offset": 1}), ctx.clone())
            .await;
        assert!(!other.is_error);
        // Changing the file resets the guard.
        std::fs::write(&file, "one\ntwo\n").unwrap();
        let changed = tool.execute(args, ctx).await;
        assert_eq!(changed.text_content(), "one\ntwo");
    }
}
