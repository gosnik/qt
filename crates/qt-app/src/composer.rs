//! Composer helpers: image attachments (clipboard paste, dropped files) and
//! the external editor round-trip (`$VISUAL` / `$EDITOR`).

use std::path::{Path, PathBuf};
use std::process::Command;

use anyhow::{Context as _, Result, bail};
use base64::Engine as _;
use qt_core::images::{inspect, prepare_image, sniff_format};
use qt_core::message::ContentBlock;

/// Largest file read as an image attachment (before resizing).
pub const MAX_ATTACHMENT_BYTES: u64 = 50 * 1024 * 1024;

/// An image waiting to be sent with the next prompt.
#[derive(Debug, Clone, PartialEq)]
pub struct Attachment {
    /// Display name (file name, or `pasted-image-N.png`).
    pub name: String,
    pub mime_type: String,
    /// Base64 image data after optional resizing.
    pub data: String,
    pub width: u32,
    pub height: u32,
    /// Size of the (possibly resized) image in bytes.
    pub byte_size: usize,
    /// Pi's dimension note when the image was resized.
    pub note: Option<String>,
}

impl Attachment {
    /// Build from raw image bytes; `auto_resize` follows `images.autoResize`.
    pub fn from_bytes(name: impl Into<String>, bytes: &[u8], auto_resize: bool) -> Result<Self> {
        let Some(format) = sniff_format(bytes) else {
            bail!("not a supported image (PNG, JPEG, GIF, WebP or BMP)");
        };
        let data = base64::engine::general_purpose::STANDARD.encode(bytes);
        let (block, note) = prepare_image(&data, format.mime_type(), auto_resize);
        let ContentBlock::Image { data, mime_type } = block else {
            bail!("image preparation returned no image");
        };
        let decoded = base64::engine::general_purpose::STANDARD
            .decode(data.as_bytes())
            .unwrap_or_default();
        let (width, height) = inspect(&decoded)
            .map(|m| m.display_dimensions())
            .unwrap_or((0, 0));
        Ok(Self {
            name: name.into(),
            mime_type,
            data,
            width,
            height,
            byte_size: decoded.len(),
            note,
        })
    }

    /// Read an image file.
    pub fn from_path(path: &Path, auto_resize: bool) -> Result<Self> {
        let meta =
            std::fs::metadata(path).with_context(|| format!("reading {}", path.display()))?;
        if !meta.is_file() {
            bail!("{} is not a file", path.display());
        }
        if meta.len() > MAX_ATTACHMENT_BYTES {
            bail!("{} is larger than 50 MB", path.display());
        }
        let bytes = std::fs::read(path).with_context(|| format!("reading {}", path.display()))?;
        let name = path
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_else(|| path.display().to_string());
        Self::from_bytes(name, &bytes, auto_resize)
            .with_context(|| format!("{} is not an image", path.display()))
    }

    pub fn block(&self) -> ContentBlock {
        ContentBlock::Image {
            data: self.data.clone(),
            mime_type: self.mime_type.clone(),
        }
    }

    /// Chip caption: `name · 1200×800 · 312 KB`.
    pub fn caption(&self) -> String {
        let mut out = self.name.clone();
        if self.width > 0 && self.height > 0 {
            out.push_str(&format!(" · {}×{}", self.width, self.height));
        }
        out.push_str(&format!(" · {}", format_bytes(self.byte_size)));
        out
    }
}

pub fn format_bytes(n: usize) -> String {
    if n >= 1024 * 1024 {
        format!("{:.1} MB", n as f64 / (1024.0 * 1024.0))
    } else if n >= 1024 {
        format!("{} KB", n.div_ceil(1024))
    } else {
        format!("{n} B")
    }
}

/// Whether a path looks like an image by extension.
pub fn has_image_extension(path: &Path) -> bool {
    path.extension()
        .and_then(|e| e.to_str())
        .map(|e| e.to_ascii_lowercase())
        .is_some_and(|e| matches!(e.as_str(), "png" | "jpg" | "jpeg" | "gif" | "webp" | "bmp"))
}

/// The prompt text and image blocks sent for `text` with `attachments`.
/// Resize notes are appended to the text so the model can map coordinates.
pub fn prompt_payload(text: &str, attachments: &[Attachment]) -> (String, Vec<ContentBlock>) {
    let mut out = text.trim().to_string();
    for note in attachments.iter().filter_map(|a| a.note.as_deref()) {
        if !out.is_empty() {
            out.push('\n');
        }
        out.push_str(note);
    }
    (out, attachments.iter().map(Attachment::block).collect())
}

/// Text inserted into the composer for dropped non-image files.
pub fn dropped_paths_text(paths: &[PathBuf]) -> String {
    paths
        .iter()
        .map(|p| {
            let s = p.display().to_string();
            if s.contains(char::is_whitespace) {
                format!("\"{s}\"")
            } else {
                s
            }
        })
        .collect::<Vec<_>>()
        .join(" ")
}

/// The external editor command: `$VISUAL`, else `$EDITOR`.
pub fn editor_command(visual: Option<&str>, editor: Option<&str>) -> Option<Vec<String>> {
    let cmd = visual
        .filter(|v| !v.trim().is_empty())
        .or(editor.filter(|v| !v.trim().is_empty()))?;
    let parts: Vec<String> = cmd.split_whitespace().map(str::to_string).collect();
    (!parts.is_empty()).then_some(parts)
}

/// Edit `text` in the external editor (blocking). Returns the new text with
/// one trailing newline removed (Pi), or `None` when the editor failed.
pub fn run_external_editor(command: &[String], text: &str) -> Result<Option<String>> {
    let Some((program, args)) = command.split_first() else {
        bail!("No editor configured. Set $VISUAL or $EDITOR environment variable.");
    };
    let path = std::env::temp_dir().join(format!(
        "qt-editor-{}-{}.qt.md",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0)
    ));
    std::fs::write(&path, text).with_context(|| format!("writing {}", path.display()))?;
    let status = Command::new(program).args(args).arg(&path).status();
    let result = match status {
        Ok(status) if status.success() => {
            let edited = std::fs::read_to_string(&path)
                .with_context(|| format!("reading {}", path.display()))?;
            Ok(Some(
                edited.strip_suffix('\n').unwrap_or(&edited).to_string(),
            ))
        }
        Ok(_) => Ok(None),
        Err(err) => Err(anyhow::anyhow!("Failed to run {program}: {err}")),
    };
    let _ = std::fs::remove_file(&path);
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    use image::{DynamicImage, ImageFormat, RgbImage};

    fn png(width: u32, height: u32) -> Vec<u8> {
        let img = DynamicImage::ImageRgb8(RgbImage::from_pixel(
            width,
            height,
            image::Rgb([9, 99, 199]),
        ));
        let mut out = std::io::Cursor::new(Vec::new());
        img.write_to(&mut out, ImageFormat::Png).unwrap();
        out.into_inner()
    }

    #[test]
    fn attachments_are_sniffed_and_resized() {
        let small = Attachment::from_bytes("a.png", &png(40, 30), true).unwrap();
        assert_eq!(small.mime_type, "image/png");
        assert_eq!((small.width, small.height), (40, 30));
        assert!(small.note.is_none());
        assert_eq!(
            small.caption(),
            format!("a.png · 40×30 · {}", format_bytes(small.byte_size))
        );

        let big = Attachment::from_bytes("big.png", &png(2400, 1200), true).unwrap();
        assert!(big.width <= 2000 && big.height <= 2000);
        assert!(
            big.note
                .as_deref()
                .unwrap()
                .starts_with("[Image: original 2400x1200")
        );
        let kept = Attachment::from_bytes("big.png", &png(2400, 1200), false).unwrap();
        assert_eq!((kept.width, kept.height), (2400, 1200));
        assert!(kept.note.is_none());

        assert!(Attachment::from_bytes("x.txt", b"hello", true).is_err());
    }

    #[test]
    fn files_are_read_and_validated() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("shot.png");
        std::fs::write(&path, png(10, 10)).unwrap();
        let a = Attachment::from_path(&path, true).unwrap();
        assert_eq!(a.name, "shot.png");
        let text = dir.path().join("notes.txt");
        std::fs::write(&text, "x").unwrap();
        assert!(Attachment::from_path(&text, true).is_err());
        assert!(Attachment::from_path(dir.path(), true).is_err());
        assert!(has_image_extension(&path));
        assert!(!has_image_extension(&text));
    }

    #[test]
    fn payload_appends_resize_notes() {
        let mut a = Attachment::from_bytes("a.png", &png(4, 4), true).unwrap();
        let (text, blocks) = prompt_payload("  look  ", std::slice::from_ref(&a));
        assert_eq!(text, "look");
        assert_eq!(blocks.len(), 1);
        a.note = Some("[Image: note]".into());
        let (text, _) = prompt_payload("look", std::slice::from_ref(&a));
        assert_eq!(text, "look\n[Image: note]");
        let (text, _) = prompt_payload("", std::slice::from_ref(&a));
        assert_eq!(text, "[Image: note]");
    }

    #[test]
    fn editor_command_prefers_visual_and_splits_args() {
        assert_eq!(
            editor_command(Some("code --wait"), Some("vim")).unwrap(),
            ["code", "--wait"]
        );
        assert_eq!(editor_command(Some(" "), Some("nano")).unwrap(), ["nano"]);
        assert!(editor_command(None, None).is_none());
        assert_eq!(
            dropped_paths_text(&["/a/b.rs".into(), "/my dir/c.rs".into()]),
            "/a/b.rs \"/my dir/c.rs\""
        );
        assert_eq!(format_bytes(512), "512 B");
        assert_eq!(format_bytes(2048), "2 KB");
        assert_eq!(format_bytes(3 * 1024 * 1024), "3.0 MB");
    }

    #[cfg(unix)]
    #[test]
    fn external_editor_round_trip() {
        let dir = tempfile::tempdir().unwrap();
        let script = dir.path().join("ed.sh");
        std::fs::write(&script, "#!/bin/sh\nprintf 'edited\\n' >> \"$1\"\n").unwrap();
        use std::os::unix::fs::PermissionsExt as _;
        std::fs::set_permissions(&script, std::fs::Permissions::from_mode(0o755)).unwrap();
        let cmd = vec![script.display().to_string()];
        assert_eq!(
            run_external_editor(&cmd, "draft ").unwrap().as_deref(),
            Some("draft edited")
        );
        assert_eq!(run_external_editor(&["false".into()], "x").unwrap(), None);
        assert!(run_external_editor(&["/nonexistent/editor".into()], "x").is_err());
    }
}
