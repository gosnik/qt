//! Initial-message processing: `@file` arguments (Pi
//! `processFileArguments`, archived `initial_message.cpp`) and `@path`
//! mentions typed in the composer.
//!
//! Text files become `<file name="<absolute path>">\n<content>\n</file>\n`
//! blocks (invalid UTF-8 replaced with U+FFFD); PNG, JPEG, GIF and WebP
//! files become image attachments, resized to provider limits when
//! `images.autoResize` is on, referenced by an empty `<file>` element or one
//! holding Pi's dimension note. Empty files are skipped.

use std::path::Path;

use base64::Engine as _;

use crate::images::{self, ImageFormat, ResizeOptions};
use crate::message::ContentBlock;

const IMAGE_OMITTED_NOTE: &str =
    "[Image omitted: could not be resized below the inline image size limit.]";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DiagnosticSeverity {
    Warning,
    Error,
}

/// A problem with one `@file` argument.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FileDiagnostic {
    pub severity: DiagnosticSeverity,
    pub path: String,
    pub message: String,
}

/// Result of [`process_file_arguments`].
#[derive(Debug, Clone, Default, PartialEq)]
pub struct ProcessedFiles {
    pub text: String,
    pub images: Vec<ContentBlock>,
    pub diagnostics: Vec<FileDiagnostic>,
}

impl ProcessedFiles {
    /// No error diagnostics.
    pub fn ok(&self) -> bool {
        !self
            .diagnostics
            .iter()
            .any(|d| d.severity == DiagnosticSeverity::Error)
    }
}

fn path_for_message(path: &Path) -> String {
    let text = path.display().to_string();
    if cfg!(windows) {
        text.replace('\\', "/")
    } else {
        text
    }
}

/// Image types accepted as attachments (sniffed from content).
fn attachment_format(bytes: &[u8]) -> Option<ImageFormat> {
    images::sniff_format(bytes).filter(|f| {
        matches!(
            f,
            ImageFormat::Png | ImageFormat::Jpeg | ImageFormat::Gif | ImageFormat::Webp
        )
    })
}

/// Read `@file` arguments relative to `cwd`.
pub fn process_file_arguments(
    file_args: &[String],
    cwd: &Path,
    auto_resize: bool,
) -> ProcessedFiles {
    let mut out = ProcessedFiles::default();
    for arg in file_args {
        let Some(resolved) = crate::policy::resolve_argument(cwd, arg) else {
            continue;
        };
        let path = resolved.canonical;
        let shown = path_for_message(&path);
        let error = |out: &mut ProcessedFiles, message: String| {
            out.diagnostics.push(FileDiagnostic {
                severity: DiagnosticSeverity::Error,
                path: shown.clone(),
                message,
            });
        };
        let metadata = match std::fs::metadata(&path) {
            Ok(m) => m,
            Err(_) => {
                error(&mut out, format!("File not found: {shown}"));
                continue;
            }
        };
        if !metadata.is_file() {
            error(&mut out, format!("File is not a regular file: {shown}"));
            continue;
        }
        if metadata.len() == 0 {
            continue;
        }
        let bytes = match std::fs::read(&path) {
            Ok(b) => b,
            Err(_) => {
                error(&mut out, format!("Could not read file: {shown}"));
                continue;
            }
        };
        let Some(format) = attachment_format(&bytes) else {
            out.text.push_str(&format!(
                "<file name=\"{shown}\">\n{}\n</file>\n",
                String::from_utf8_lossy(&bytes)
            ));
            continue;
        };
        let data = base64::engine::general_purpose::STANDARD.encode(&bytes);
        let mime = format.mime_type();
        if !auto_resize {
            out.images.push(ContentBlock::Image {
                data,
                mime_type: mime.into(),
            });
            out.text
                .push_str(&format!("<file name=\"{shown}\"></file>\n"));
            continue;
        }
        match images::resize_image(&data, mime, &ResizeOptions::default()) {
            Some(resized) => {
                let note = images::format_dimension_note(&resized).unwrap_or_default();
                out.images.push(ContentBlock::Image {
                    data: resized.data,
                    mime_type: resized.mime_type,
                });
                out.text
                    .push_str(&format!("<file name=\"{shown}\">{note}</file>\n"));
            }
            None => {
                let limits = ResizeOptions::default();
                out.diagnostics.push(FileDiagnostic {
                    severity: DiagnosticSeverity::Warning,
                    path: shown.clone(),
                    message: format!(
                        "Image omitted because it could not be resized to fit within the configured auto-resize limits ({}x{}, {} base64 bytes; observed {} bytes).",
                        limits.max_width,
                        limits.max_height,
                        limits.max_base64_bytes,
                        data.len()
                    ),
                });
                out.text.push_str(&format!(
                    "<file name=\"{shown}\">{IMAGE_OMITTED_NOTE}</file>\n"
                ));
            }
        }
    }
    out
}

/// Archived `build_initial_message`: stdin, then the file blocks, then the
/// first message, concatenated.
pub fn build_initial_message(
    stdin: Option<&str>,
    file_text: &str,
    message: Option<&str>,
) -> String {
    let mut combined = String::new();
    if let Some(stdin) = stdin {
        combined.push_str(stdin);
    }
    combined.push_str(file_text);
    if let Some(message) = message {
        combined.push_str(message);
    }
    combined
}

/// Split CLI message arguments into `@file` arguments and messages.
pub fn split_file_arguments(args: &[String]) -> (Vec<String>, Vec<String>) {
    let mut files = Vec::new();
    let mut messages = Vec::new();
    for arg in args {
        match arg.strip_prefix('@') {
            Some(path) if !path.is_empty() => files.push(path.to_string()),
            _ => messages.push(arg.clone()),
        }
    }
    (files, messages)
}

/// `@path` / `@"path with spaces"` mentions in composer text, in order and
/// deduplicated. A mention starts at the beginning of the text or after
/// whitespace.
pub fn extract_mentions(text: &str) -> Vec<String> {
    let mut mentions: Vec<String> = Vec::new();
    let chars: Vec<char> = text.chars().collect();
    let mut i = 0;
    while i < chars.len() {
        if chars[i] == '@' && (i == 0 || chars[i - 1].is_whitespace()) {
            let mut j = i + 1;
            let mention: String = if j < chars.len() && (chars[j] == '"' || chars[j] == '\'') {
                let quote = chars[j];
                j += 1;
                let start = j;
                while j < chars.len() && chars[j] != quote {
                    j += 1;
                }
                let value: String = chars[start..j].iter().collect();
                j += 1;
                value
            } else {
                let start = j;
                while j < chars.len() && !chars[j].is_whitespace() {
                    j += 1;
                }
                let raw: String = chars[start..j].iter().collect();
                raw.trim_end_matches([',', '.', ';', ':', ')', '!', '?'])
                    .to_string()
            };
            if !mention.is_empty() && !mentions.contains(&mention) {
                mentions.push(mention);
            }
            i = j;
            continue;
        }
        i += 1;
    }
    mentions
}

/// Attach the files named by `@` mentions in `text`: mentions that resolve
/// to existing files are processed like `@file` arguments and their blocks
/// appended after the text; other mentions (people, missing paths) are
/// left alone.
pub fn expand_mentions(text: &str, cwd: &Path, auto_resize: bool) -> ProcessedFiles {
    let existing: Vec<String> = extract_mentions(text)
        .into_iter()
        .filter(|m| crate::policy::resolve_argument(cwd, m).is_some_and(|r| r.canonical.is_file()))
        .collect();
    let mut files = process_file_arguments(&existing, cwd, auto_resize);
    let mut combined = text.to_string();
    if !files.text.is_empty() {
        if !combined.ends_with('\n') {
            combined.push_str("\n\n");
        }
        combined.push_str(files.text.trim_end_matches('\n'));
    }
    files.text = combined;
    files
}

#[cfg(test)]
mod tests {
    use super::*;

    fn png(width: u32, height: u32) -> Vec<u8> {
        let img = image::RgbImage::from_pixel(width, height, image::Rgb([1, 2, 3]));
        let mut out = std::io::Cursor::new(Vec::new());
        image::DynamicImage::ImageRgb8(img)
            .write_to(&mut out, image::ImageFormat::Png)
            .unwrap();
        out.into_inner()
    }

    #[test]
    fn files_become_blocks_images_and_diagnostics() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().canonicalize().unwrap();
        std::fs::write(root.join("a.txt"), b"hello \xff").unwrap();
        std::fs::write(root.join("empty.txt"), b"").unwrap();
        std::fs::write(root.join("big.png"), png(3000, 1000)).unwrap();
        std::fs::write(root.join("small.png"), png(4, 4)).unwrap();
        std::fs::create_dir(root.join("sub")).unwrap();
        let args: Vec<String> = [
            "@a.txt",
            "empty.txt",
            "big.png",
            "small.png",
            "sub",
            "missing",
        ]
        .iter()
        .map(|s| s.to_string())
        .collect();
        let out = process_file_arguments(&args, &root, true);
        let p = |n: &str| root.join(n).display().to_string();
        assert_eq!(
            out.text,
            format!(
                "<file name=\"{}\">\nhello \u{fffd}\n</file>\n<file name=\"{}\">[Image: original 3000x1000, displayed at 2000x667. Multiply coordinates by 1.50 to map to original image.]</file>\n<file name=\"{}\"></file>\n",
                p("a.txt"),
                p("big.png"),
                p("small.png")
            )
        );
        assert_eq!(out.images.len(), 2);
        assert!(!out.ok());
        let messages: Vec<&str> = out.diagnostics.iter().map(|d| d.message.as_str()).collect();
        assert_eq!(
            messages,
            [
                format!("File is not a regular file: {}", p("sub")),
                format!("File not found: {}", p("missing"))
            ]
        );
        let raw = process_file_arguments(&["big.png".into()], &root, false);
        assert_eq!(
            raw.text,
            format!("<file name=\"{}\"></file>\n", p("big.png"))
        );
        assert_eq!(
            build_initial_message(Some("in "), "<f/>", Some("go")),
            "in <f/>go"
        );
        assert_eq!(
            split_file_arguments(&["@x".into(), "hi".into(), "@".into()]),
            (
                vec!["x".to_string()],
                vec!["hi".to_string(), "@".to_string()]
            )
        );
    }

    #[test]
    fn mentions_are_extracted_and_expanded() {
        assert_eq!(
            extract_mentions("see @src/main.rs, and @\"my file.txt\" not a@b @src/main.rs"),
            ["src/main.rs", "my file.txt"]
        );
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().canonicalize().unwrap();
        std::fs::write(root.join("notes.md"), "N").unwrap();
        let out = expand_mentions("check @notes.md and @alice", &root, true);
        assert_eq!(
            out.text,
            format!(
                "check @notes.md and @alice\n\n<file name=\"{}\">\nN\n</file>",
                root.join("notes.md").display()
            )
        );
        assert!(out.diagnostics.is_empty());
    }
}
