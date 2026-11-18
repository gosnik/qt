//! `clipboard`: copy text or an image to the system clipboard through the
//! platform command (archived `clipboard_tool.cpp`). Without `execute` the
//! tool only reports the command plan.

use std::ffi::OsString;
use std::io::Write as _;
use std::path::PathBuf;
use std::process::Stdio;
use std::time::Duration;

use futures::future::BoxFuture;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};

use super::read::image_mime;
use super::{Tool, ToolContext, ToolOutput, de_opt_bool, parse_args};

const MAX_TEXT_BYTES: usize = 1024 * 1024;
const MAX_IMAGE_BYTES: u64 = 16 * 1024 * 1024;
const COPY_TIMEOUT: Duration = Duration::from_secs(3);

/// One candidate clipboard command.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct ClipboardCommandPlan {
    pub name: String,
    pub executable: String,
    pub arguments: Vec<String>,
    pub available: bool,
    pub reason: String,
}

impl ClipboardCommandPlan {
    pub fn command_line(&self) -> String {
        std::iter::once(self.executable.as_str())
            .chain(self.arguments.iter().map(String::as_str))
            .collect::<Vec<_>>()
            .join(" ")
    }
}

fn preferred_commands() -> Vec<(&'static str, &'static [&'static str])> {
    const PBCOPY: (&str, &[&str]) = ("pbcopy", &[]);
    const WL_COPY: (&str, &[&str]) = ("wl-copy", &[]);
    const XCLIP: (&str, &[&str]) = ("xclip", &["-selection", "clipboard"]);
    const CLIP: (&str, &[&str]) = ("clip.exe", &[]);
    if cfg!(target_os = "macos") {
        vec![PBCOPY, WL_COPY, XCLIP, CLIP]
    } else if cfg!(windows) {
        vec![CLIP, PBCOPY, WL_COPY, XCLIP]
    } else {
        vec![WL_COPY, XCLIP, PBCOPY, CLIP]
    }
}

/// Candidate commands in platform preference order, marking which exist.
pub fn plan_clipboard_copy_commands(available: &[String]) -> Vec<ClipboardCommandPlan> {
    preferred_commands()
        .into_iter()
        .map(|(exe, args)| {
            let is_available = available.iter().any(|a| a == exe);
            ClipboardCommandPlan {
                name: exe.to_string(),
                executable: exe.to_string(),
                arguments: args.iter().map(|s| s.to_string()).collect(),
                available: is_available,
                reason: if is_available {
                    "available"
                } else {
                    "executable not found"
                }
                .into(),
            }
        })
        .collect()
}

/// The first available command.
pub fn select_clipboard_copy_command(available: &[String]) -> Option<ClipboardCommandPlan> {
    plan_clipboard_copy_commands(available)
        .into_iter()
        .find(|p| p.available)
}

/// `clipboard` tool. `search_path` overrides `PATH` for command lookup.
#[derive(Default)]
pub struct ClipboardTool {
    search_path: Option<OsString>,
}

impl ClipboardTool {
    pub fn new() -> Self {
        Self::default()
    }

    /// Look up clipboard commands in `path` instead of `$PATH`.
    pub fn with_search_path(path: impl Into<OsString>) -> Self {
        Self {
            search_path: Some(path.into()),
        }
    }

    fn search_dirs(&self) -> Vec<PathBuf> {
        let path = self
            .search_path
            .clone()
            .or_else(|| std::env::var_os("PATH"))
            .unwrap_or_default();
        std::env::split_paths(&path).collect()
    }

    fn locate(&self, executable: &str) -> Option<PathBuf> {
        self.search_dirs()
            .into_iter()
            .map(|dir| dir.join(executable))
            .find(|p| p.is_file())
    }

    fn available_commands(&self) -> Vec<String> {
        preferred_commands()
            .into_iter()
            .map(|(exe, _)| exe)
            .filter(|exe| self.locate(exe).is_some())
            .map(str::to_string)
            .collect()
    }
}

#[derive(Deserialize)]
struct Args {
    #[serde(default)]
    text: Option<String>,
    #[serde(default)]
    image_path: Option<String>,
    #[serde(default, deserialize_with = "de_opt_bool")]
    execute: Option<bool>,
}

enum Payload {
    Text(String),
    Image {
        path: PathBuf,
        shown: String,
        mime: &'static str,
        bytes: u64,
    },
}

impl Tool for ClipboardTool {
    fn name(&self) -> &'static str {
        "clipboard"
    }
    fn description(&self) -> String {
        "Copy text or image bytes to the platform clipboard, or return the command plan. Set execute to true to actually copy; by default only the plan is returned.".into()
    }
    fn snippet(&self) -> &'static str {
        "Copy text or an image to the system clipboard"
    }
    fn parameters(&self) -> Value {
        json!({
            "type": "object",
            "properties": {
                "text": { "type": "string", "description": "Text to copy to the clipboard." },
                "image_path": { "type": "string", "description": "Image file (png, jpg, gif, webp) to copy to the clipboard." },
                "execute": { "type": "boolean", "description": "When true, run the selected clipboard command. Defaults to false (plan only)." }
            },
            "additionalProperties": false
        })
    }

    fn execute(&self, args: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        let args: Args = match parse_args("clipboard", args) {
            Ok(a) => a,
            Err(e) => return Box::pin(async move { e }),
        };
        let available = self.available_commands();
        let selected = select_clipboard_copy_command(&available);
        let executable = selected.as_ref().and_then(|s| self.locate(&s.executable));
        Box::pin(async move {
            let payload = match payload(&args, &ctx) {
                Ok(p) => p,
                Err(e) => return ToolOutput::error(e),
            };
            let plans = plan_clipboard_copy_commands(&available);
            let (kind, stdin_bytes, mime) = match &payload {
                Payload::Text(t) => ("text", t.len() as u64, "text/plain"),
                Payload::Image { bytes, mime, .. } => ("image", *bytes, *mime),
            };
            let mut details = json!({
                "operation": "copy",
                "valid": selected.is_some(),
                "payload": { "kind": kind, "mime_type": mime, "stdin_bytes": stdin_bytes },
                "available_commands": available,
                "candidates": plans,
            });
            let Some(mut plan) = selected else {
                return ToolOutput::error("no supported clipboard command is available")
                    .with_details(details);
            };
            if let Payload::Image { mime, .. } = &payload {
                match plan.executable.as_str() {
                    "xclip" => plan.arguments.extend(["-t".to_string(), mime.to_string()]),
                    "wl-copy" => plan
                        .arguments
                        .extend(["--type".to_string(), mime.to_string()]),
                    _ => {}
                }
            }
            details["plan"] = json!(plan);
            details["command"] = json!(plan.command_line());
            if !args.execute.unwrap_or(false) {
                return ToolOutput::text(format!(
                    "Clipboard copy plan: {} ({stdin_bytes} bytes of {kind}). Not executed; pass execute=true to copy.",
                    plan.command_line()
                ))
                .with_details(details);
            }
            let input = match &payload {
                Payload::Text(t) => t.clone().into_bytes(),
                Payload::Image { path, shown, .. } => match tokio::fs::read(path).await {
                    Ok(b) => b,
                    Err(e) => return ToolOutput::error(format!("Cannot read {shown}: {e}")),
                },
            };
            let program = executable.unwrap_or_else(|| PathBuf::from(&plan.executable));
            let arguments = plan.arguments.clone();
            let result =
                tokio::task::spawn_blocking(move || run_copy(&program, &arguments, &input))
                    .await
                    .unwrap_or_else(|e| Err(format!("clipboard copy failed: {e}")));
            match result {
                Ok(()) => {
                    details["copied"] = json!(true);
                    ToolOutput::text(format!(
                        "Copied {stdin_bytes} bytes of {kind} to the clipboard with {}",
                        plan.executable
                    ))
                    .with_details(details)
                }
                Err(e) => ToolOutput::error(e).with_details(details),
            }
        })
    }
}

fn payload(args: &Args, ctx: &ToolContext) -> Result<Payload, String> {
    // An empty argument yields to a non-empty one (archived behaviour).
    let full_text = args.text.as_deref().is_some_and(|t| !t.is_empty());
    let full_image = args
        .image_path
        .as_deref()
        .is_some_and(|p| !p.trim().is_empty());
    let use_text = args.text.is_some() && (args.image_path.is_none() || full_text || !full_image);
    let use_image = args.image_path.is_some() && (args.text.is_none() || full_image || !full_text);
    match (use_text, use_image) {
        (true, false) => {
            let text = args.text.clone().unwrap_or_default();
            if text.len() > MAX_TEXT_BYTES {
                return Err("clipboard text exceeds the 1048576 byte limit".into());
            }
            Ok(Payload::Text(text))
        }
        (false, true) => {
            let raw = args.image_path.as_deref().unwrap_or_default();
            let resolved = ctx
                .resolve_path(raw)
                .map_err(|_| "clipboard image path escapes workspace_root".to_string())?;
            let shown = ctx.display(&resolved.path);
            let mime = image_mime(&resolved.path).ok_or_else(|| {
                "clipboard image_path must end with .png, .jpg, .jpeg, .gif, or .webp".to_string()
            })?;
            let meta = std::fs::metadata(&resolved.path)
                .ok()
                .filter(|m| m.is_file())
                .ok_or_else(|| {
                    "clipboard image_path must point to a readable regular file".to_string()
                })?;
            if meta.len() > MAX_IMAGE_BYTES {
                return Err("clipboard image_path exceeds the 16777216 byte limit".into());
            }
            Ok(Payload::Image {
                path: resolved.path,
                shown,
                mime,
                bytes: meta.len(),
            })
        }
        _ => Err("clipboard requires exactly one of text or image_path".into()),
    }
}

fn run_copy(program: &std::path::Path, arguments: &[String], input: &[u8]) -> Result<(), String> {
    let mut child = std::process::Command::new(program)
        .args(arguments)
        .stdin(Stdio::piped())
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| format!("clipboard copy failed: {e}"))?;
    if let Some(mut stdin) = child.stdin.take() {
        stdin
            .write_all(input)
            .map_err(|e| format!("clipboard copy failed: {e}"))?;
    }
    let started = std::time::Instant::now();
    loop {
        match child.try_wait() {
            Ok(Some(status)) => {
                if status.success() {
                    return Ok(());
                }
                let mut stderr = String::new();
                if let Some(mut pipe) = child.stderr.take() {
                    use std::io::Read as _;
                    let _ = pipe.read_to_string(&mut stderr);
                }
                let stderr = stderr.trim();
                return Err(if stderr.is_empty() {
                    "clipboard copy failed".into()
                } else {
                    stderr.to_string()
                });
            }
            Ok(None) if started.elapsed() > COPY_TIMEOUT => {
                // wl-copy and xclip fork a server that keeps serving the selection;
                // the foreground process normally exits immediately.
                let _ = child.kill();
                let _ = child.wait();
                return Err("clipboard copy timed out".into());
            }
            Ok(None) => std::thread::sleep(Duration::from_millis(20)),
            Err(e) => return Err(format!("clipboard copy failed: {e}")),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn plans_follow_platform_preference() {
        let available = vec!["xclip".to_string(), "pbcopy".to_string()];
        let selected = select_clipboard_copy_command(&available).unwrap();
        // macOS prefers pbcopy; Windows prefers clip.exe, then pbcopy.
        if cfg!(target_os = "macos") || cfg!(windows) {
            assert_eq!(selected.executable, "pbcopy");
        } else {
            assert_eq!(selected.command_line(), "xclip -selection clipboard");
        }
        assert!(select_clipboard_copy_command(&[]).is_none());
        assert_eq!(plan_clipboard_copy_commands(&[]).len(), 4);
    }

    #[tokio::test]
    async fn plans_without_executing_and_reports_missing_commands() {
        let dir = tempfile::tempdir().unwrap();
        let empty = tempfile::tempdir().unwrap();
        let tool = ClipboardTool::with_search_path(empty.path());
        let out = tool
            .execute(json!({"text": "hi"}), ToolContext::new(dir.path()))
            .await;
        assert!(out.is_error);
        assert_eq!(
            out.text_content(),
            "no supported clipboard command is available"
        );
        let out = tool.execute(json!({}), ToolContext::new(dir.path())).await;
        assert_eq!(
            out.text_content(),
            "clipboard requires exactly one of text or image_path"
        );
    }

    #[cfg(unix)]
    #[tokio::test]
    async fn executes_the_selected_command_with_stdin() {
        let dir = tempfile::tempdir().unwrap();
        let bin = tempfile::tempdir().unwrap();
        let sink = dir.path().join("sink.txt");
        let name = if cfg!(target_os = "macos") {
            "pbcopy"
        } else {
            "wl-copy"
        };
        let script = bin.path().join(name);
        crate::test_support::write_script(
            &script,
            &format!(
                "#!/bin/sh\ncat > '{}'\necho \"$@\" >> '{}'\n",
                sink.display(),
                sink.display()
            ),
        );
        let tool = ClipboardTool::with_search_path(bin.path());
        let ctx = ToolContext::new(dir.path());

        let out = tool.execute(json!({"text": "copied!"}), ctx.clone()).await;
        assert!(!out.is_error, "{}", out.text_content());
        assert!(out.text_content().contains("Not executed"));
        assert!(!sink.exists());

        let out = tool
            .execute(json!({"text": "copied!", "execute": true}), ctx.clone())
            .await;
        assert!(!out.is_error, "{}", out.text_content());
        assert_eq!(
            out.text_content(),
            format!("Copied 7 bytes of text to the clipboard with {name}")
        );
        assert!(
            std::fs::read_to_string(&sink)
                .unwrap()
                .starts_with("copied!")
        );

        std::fs::write(dir.path().join("pic.png"), [0x89, b'P', b'N', b'G']).unwrap();
        let out = tool
            .execute(
                json!({"image_path": "pic.png", "execute": true}),
                ctx.clone(),
            )
            .await;
        assert!(!out.is_error, "{}", out.text_content());
        if name == "wl-copy" {
            let written = String::from_utf8_lossy(&std::fs::read(&sink).unwrap()).into_owned();
            assert!(written.contains("--type image/png"));
        }
        let out = tool
            .execute(json!({"image_path": "notes.txt"}), ctx.clone())
            .await;
        assert!(out.text_content().contains("must end with"));
        let out = tool.execute(json!({"image_path": "/etc/x.png"}), ctx).await;
        assert_eq!(
            out.text_content(),
            "clipboard image path escapes workspace_root"
        );
    }
}
