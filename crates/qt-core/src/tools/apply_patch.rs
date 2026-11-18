//! `apply_patch`: Codex-style multi-file patch envelope (archived
//! `apply_patch_tool.cpp`), applied atomically with exact, unique context
//! matching per hunk.

use std::collections::BTreeMap;
use std::path::PathBuf;

use futures::future::BoxFuture;
use serde::Deserialize;
use serde_json::{Value, json};

use super::mutation::{MutationKind, require_approval};
use super::{Tool, ToolContext, ToolOutput, parse_args};

const BEGIN: &str = "*** Begin Patch";
const END: &str = "*** End Patch";
const ADD: &str = "*** Add File: ";
const DELETE: &str = "*** Delete File: ";
const UPDATE: &str = "*** Update File: ";
const MOVE: &str = "*** Move to: ";
const END_OF_FILE: &str = "*** End of File";

pub struct ApplyPatchTool;

/// One change block inside an `Update File` section.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Hunk {
    /// Text after `@@`, used to locate the hunk.
    pub anchor: Option<String>,
    /// Context and removed lines, in order.
    pub old_lines: Vec<String>,
    /// Context and added lines, in order.
    pub new_lines: Vec<String>,
    /// The hunk must end at the end of the file (`*** End of File`).
    pub end_of_file: bool,
}

/// A parsed file operation.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PatchOperation {
    Add {
        path: String,
        content: String,
    },
    Delete {
        path: String,
    },
    Update {
        path: String,
        move_to: Option<String>,
        hunks: Vec<Hunk>,
    },
}

fn is_file_header(line: &str) -> bool {
    line.starts_with(ADD) || line.starts_with(DELETE) || line.starts_with(UPDATE) || line == END
}

fn header_path(line: &str, prefix: &str) -> Result<String, String> {
    let path = line[prefix.len()..].trim();
    if path.is_empty() {
        return Err("apply_patch requires non-empty file paths".into());
    }
    Ok(path.to_string())
}

/// Parse a `*** Begin Patch` … `*** End Patch` envelope.
pub fn parse_patch(text: &str) -> Result<Vec<PatchOperation>, String> {
    let mut lines: Vec<&str> = text
        .split('\n')
        .map(|l| l.strip_suffix('\r').unwrap_or(l))
        .collect();
    while lines.last().is_some_and(|l| l.trim().is_empty()) {
        lines.pop();
    }
    while lines.first().is_some_and(|l| l.trim().is_empty()) {
        lines.remove(0);
    }
    if lines.len() < 2 || lines[0].trim_end() != BEGIN || lines[lines.len() - 1].trim_end() != END {
        return Err(
            "apply_patch input must start with *** Begin Patch and end with *** End Patch".into(),
        );
    }
    let body = &lines[1..lines.len() - 1];
    let mut ops = Vec::new();
    let mut i = 0;
    while i < body.len() {
        let line = body[i].trim_end();
        i += 1;
        if line.starts_with(ADD) {
            let path = header_path(line, ADD)?;
            let mut content = String::new();
            while i < body.len() && !is_file_header(body[i]) {
                let Some(rest) = body[i].strip_prefix('+') else {
                    return Err("apply_patch add hunks require + lines".into());
                };
                content.push_str(rest);
                content.push('\n');
                i += 1;
            }
            ops.push(PatchOperation::Add { path, content });
        } else if line.starts_with(DELETE) {
            let path = header_path(line, DELETE)?;
            ops.push(PatchOperation::Delete { path });
        } else if line.starts_with(UPDATE) {
            let path = header_path(line, UPDATE)?;
            let mut move_to = None;
            if i < body.len() && body[i].starts_with(MOVE) {
                move_to = Some(header_path(body[i].trim_end(), MOVE)?);
                i += 1;
            }
            let mut hunks: Vec<Hunk> = Vec::new();
            let mut current: Option<Hunk> = None;
            while i < body.len() && !is_file_header(body[i]) {
                let raw = body[i];
                i += 1;
                if let Some(rest) = raw.strip_prefix("@@") {
                    if let Some(h) = current.take() {
                        hunks.push(h);
                    }
                    let anchor = rest.trim();
                    let anchor = anchor.strip_suffix("@@").map(str::trim).unwrap_or(anchor);
                    current = Some(Hunk {
                        anchor: (!anchor.is_empty()).then(|| anchor.to_string()),
                        ..Hunk::default()
                    });
                    continue;
                }
                if raw.trim_end() == END_OF_FILE {
                    current.get_or_insert_with(Hunk::default).end_of_file = true;
                    continue;
                }
                let hunk = current.get_or_insert_with(Hunk::default);
                let (marker, text) = match raw.chars().next() {
                    // A bare empty line is an empty context line.
                    None => (' ', ""),
                    Some(c) => (c, &raw[c.len_utf8()..]),
                };
                match marker {
                    ' ' => {
                        hunk.old_lines.push(text.to_string());
                        hunk.new_lines.push(text.to_string());
                    }
                    '-' => hunk.old_lines.push(text.to_string()),
                    '+' => hunk.new_lines.push(text.to_string()),
                    _ => {
                        return Err(
                            "apply_patch update hunks require space, -, or + prefixes".into()
                        );
                    }
                }
            }
            if let Some(h) = current.take() {
                hunks.push(h);
            }
            hunks.retain(|h| !(h.old_lines.is_empty() && h.new_lines.is_empty()));
            if hunks.is_empty() && move_to.is_none() {
                return Err(format!("apply_patch update for {path} has no hunks"));
            }
            ops.push(PatchOperation::Update {
                path,
                move_to,
                hunks,
            });
        } else if line.trim().is_empty() {
            continue;
        } else {
            return Err("apply_patch expected Add File, Delete File, or Update File hunk".into());
        }
    }
    if ops.is_empty() {
        return Err("apply_patch requires at least one hunk".into());
    }
    Ok(ops)
}

/// File text split into lines, remembering the line-ending style.
struct FileLines {
    lines: Vec<String>,
    crlf: bool,
    trailing_newline: bool,
}

impl FileLines {
    fn parse(text: &str) -> Self {
        let crlf = text.contains("\r\n");
        let normalized = text.replace("\r\n", "\n");
        let trailing_newline = normalized.ends_with('\n');
        let body = normalized.strip_suffix('\n').unwrap_or(&normalized);
        let lines = if normalized.is_empty() {
            Vec::new()
        } else {
            body.split('\n').map(str::to_string).collect()
        };
        Self {
            lines,
            crlf,
            trailing_newline: trailing_newline || normalized.is_empty(),
        }
    }

    fn render(&self) -> String {
        if self.lines.is_empty() {
            return String::new();
        }
        let sep = if self.crlf { "\r\n" } else { "\n" };
        let mut out = self.lines.join(sep);
        if self.trailing_newline {
            out.push_str(sep);
        }
        out
    }
}

fn apply_hunks(text: &str, hunks: &[Hunk], shown: &str) -> Result<String, String> {
    let mut file = FileLines::parse(text);
    let mut cursor = 0usize;
    for (index, hunk) in hunks.iter().enumerate() {
        let lines = &file.lines;
        let mut start = cursor;
        if let Some(anchor) = &hunk.anchor {
            let found = (cursor..lines.len()).find(|&i| lines[i].trim() == anchor.trim());
            match found {
                Some(i) => start = i,
                None => {
                    return Err(format!("apply_patch could not find @@ {anchor} in {shown}"));
                }
            }
        }
        let position = if hunk.old_lines.is_empty() {
            if hunk.end_of_file {
                lines.len()
            } else if hunk.anchor.is_some() {
                start + 1
            } else {
                return Err("apply_patch update hunks require context or removed lines".into());
            }
        } else {
            let n = hunk.old_lines.len();
            let candidates: Vec<usize> = (start..=lines.len().saturating_sub(n))
                .filter(|&p| p + n <= lines.len())
                .filter(|&p| lines[p..p + n] == hunk.old_lines[..])
                .filter(|&p| !hunk.end_of_file || p + n == lines.len())
                .collect();
            if candidates.len() != 1 {
                return Err(format!(
                    "apply_patch update hunk {} matched {} locations in {shown}",
                    index + 1,
                    candidates.len()
                ));
            }
            candidates[0]
        };
        let end = position + hunk.old_lines.len();
        file.lines
            .splice(position..end, hunk.new_lines.iter().cloned());
        cursor = position + hunk.new_lines.len();
    }
    if !file.lines.is_empty() && text.is_empty() {
        file.trailing_newline = true;
    }
    Ok(file.render())
}

#[derive(Deserialize)]
struct Args {
    #[serde(default)]
    patch: Option<String>,
    #[serde(rename = "patchText", default)]
    patch_text: Option<String>,
}

/// Planned state of one file: `None` deletes it.
struct Planned {
    shown: String,
    content: Option<String>,
    original: Option<String>,
    operation: &'static str,
}

impl Tool for ApplyPatchTool {
    fn name(&self) -> &'static str {
        "apply_patch"
    }
    fn description(&self) -> String {
        "Apply a validated multi-file patch using the Codex-style patch envelope. The patch starts with *** Begin Patch and ends with *** End Patch; each file section is *** Add File: <path> (lines prefixed with +), *** Delete File: <path>, or *** Update File: <path> (optionally followed by *** Move to: <path>) with @@ hunks whose lines start with space (context), - (remove) or + (add). Context must match the file exactly and identify a unique location. All changes are applied together or not at all.".into()
    }
    fn snippet(&self) -> &'static str {
        "Apply a multi-file patch (Codex *** Begin Patch envelope)"
    }
    fn parameters(&self) -> Value {
        json!({
            "type": "object",
            "properties": {
                "patch": { "type": "string", "description": "Patch text beginning with *** Begin Patch." }
            },
            "required": ["patch"],
            "additionalProperties": false
        })
    }

    fn execute(&self, raw: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        Box::pin(async move {
            let args: Args = match parse_args("apply_patch", raw.clone()) {
                Ok(a) => a,
                Err(e) => return e,
            };
            let patch = match (args.patch, args.patch_text) {
                (Some(_), Some(_)) => {
                    return ToolOutput::error("apply_patch accepts patch or patchText, not both");
                }
                (None, None) => {
                    return ToolOutput::error("apply_patch requires a patch or patchText argument");
                }
                (Some(p), None) | (None, Some(p)) => p,
            };
            let ops = match parse_patch(&patch) {
                Ok(ops) => ops,
                Err(e) => return ToolOutput::error(e),
            };
            let plan = match plan(&ops, &ctx) {
                Ok(p) => p,
                Err(e) => return ToolOutput::error(e),
            };
            if let Some(queued) = require_approval(
                &ctx,
                MutationKind::FileEdit,
                "apply_patch",
                format!("apply patch touching {} files", plan.len()),
                &raw,
            ) {
                return queued;
            }
            commit(plan).await
        })
    }
}

fn plan(ops: &[PatchOperation], ctx: &ToolContext) -> Result<BTreeMap<PathBuf, Planned>, String> {
    let mut planned: BTreeMap<PathBuf, Planned> = BTreeMap::new();
    let resolve = |raw: &str| -> Result<(PathBuf, String), String> {
        let path = ctx.resolve_path(raw).map_err(|e| e.text_content())?.path;
        let shown = ctx.display(&path);
        Ok((path, shown))
    };
    // Current content of `path`, honouring earlier operations in this patch.
    let current = |planned: &BTreeMap<PathBuf, Planned>, path: &PathBuf| -> Option<String> {
        match planned.get(path) {
            Some(p) => p.content.clone(),
            None => std::fs::read_to_string(path).ok(),
        }
    };
    for op in ops {
        match op {
            PatchOperation::Add { path, content } => {
                let (path, shown) = resolve(path)?;
                if current(&planned, &path).is_some()
                    || (!planned.contains_key(&path) && path.exists())
                {
                    return Err(format!("apply_patch add file already exists: {shown}"));
                }
                let original = planned.get(&path).and_then(|p| p.original.clone());
                planned.insert(
                    path,
                    Planned {
                        shown,
                        content: Some(content.clone()),
                        original,
                        operation: "add",
                    },
                );
            }
            PatchOperation::Delete { path } => {
                let (path, shown) = resolve(path)?;
                let Some(existing) = current(&planned, &path) else {
                    return Err(format!("apply_patch delete file not found: {shown}"));
                };
                let original = planned
                    .get(&path)
                    .map(|p| p.original.clone())
                    .unwrap_or(Some(existing));
                planned.insert(
                    path,
                    Planned {
                        shown,
                        content: None,
                        original,
                        operation: "delete",
                    },
                );
            }
            PatchOperation::Update {
                path,
                move_to,
                hunks,
            } => {
                let (path, shown) = resolve(path)?;
                let Some(existing) = current(&planned, &path) else {
                    return Err(format!("apply_patch update file not found: {shown}"));
                };
                let updated = apply_hunks(&existing, hunks, &shown)?;
                let original = planned
                    .get(&path)
                    .map(|p| p.original.clone())
                    .unwrap_or_else(|| Some(existing.clone()));
                match move_to {
                    Some(dest) => {
                        let (dest, dest_shown) = resolve(dest)?;
                        if dest != path
                            && (current(&planned, &dest).is_some()
                                || (!planned.contains_key(&dest) && dest.exists()))
                        {
                            return Err(format!(
                                "apply_patch move destination already exists: {dest_shown}"
                            ));
                        }
                        planned.insert(
                            path,
                            Planned {
                                shown,
                                content: None,
                                original,
                                operation: "delete",
                            },
                        );
                        planned.insert(
                            dest,
                            Planned {
                                shown: dest_shown,
                                content: Some(updated),
                                original: None,
                                operation: "add",
                            },
                        );
                    }
                    None => {
                        let operation = planned
                            .get(&path)
                            .map(|p| p.operation)
                            .filter(|op| *op == "add")
                            .unwrap_or("update");
                        planned.insert(
                            path,
                            Planned {
                                shown,
                                content: Some(updated),
                                original,
                                operation,
                            },
                        );
                    }
                }
            }
        }
    }
    Ok(planned)
}

async fn commit(plan: BTreeMap<PathBuf, Planned>) -> ToolOutput {
    let mut summary = Vec::new();
    let mut files = Vec::new();
    let mut diff = String::new();
    for (path, entry) in &plan {
        match &entry.content {
            Some(content) => {
                if let Some(parent) = path.parent()
                    && let Err(e) = tokio::fs::create_dir_all(parent).await
                {
                    return ToolOutput::error(format!(
                        "Cannot create directory for {}: {e}",
                        entry.shown
                    ));
                }
                if let Err(e) = tokio::fs::write(path, content.as_bytes()).await {
                    return ToolOutput::error(format!("Cannot write {}: {e}", entry.shown));
                }
            }
            None => {
                if let Err(e) = tokio::fs::remove_file(path).await {
                    return ToolOutput::error(format!("Cannot delete {}: {e}", entry.shown));
                }
            }
        }
        let letter = match entry.operation {
            "add" => "A",
            "delete" => "D",
            _ => "M",
        };
        summary.push(format!("{letter} {}", entry.shown));
        files.push(json!({ "path": entry.shown, "operation": entry.operation }));
        let before = entry.original.as_deref().unwrap_or("");
        let after = entry.content.as_deref().unwrap_or("");
        diff.push_str(
            &similar::TextDiff::from_lines(before, after)
                .unified_diff()
                .context_radius(3)
                .header(&format!("a/{}", entry.shown), &format!("b/{}", entry.shown))
                .to_string(),
        );
    }
    ToolOutput::text(format!(
        "Success. Updated the following files:\n{}",
        summary.join("\n")
    ))
    .with_details(json!({
        "applied": true,
        "file_count": files.len(),
        "files": files,
        "diff": diff,
    }))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_envelope_sections() {
        let ops = parse_patch(
            "*** Begin Patch\n*** Add File: a.txt\n+one\n+two\n*** Delete File: b.txt\n*** Update File: c.txt\n*** Move to: d.txt\n@@ fn main\n ctx\n-old\n+new\n*** End of File\n*** End Patch\n",
        )
        .unwrap();
        assert_eq!(
            ops[0],
            PatchOperation::Add {
                path: "a.txt".into(),
                content: "one\ntwo\n".into()
            }
        );
        assert_eq!(
            ops[1],
            PatchOperation::Delete {
                path: "b.txt".into()
            }
        );
        let PatchOperation::Update { move_to, hunks, .. } = &ops[2] else {
            panic!("expected update");
        };
        assert_eq!(move_to.as_deref(), Some("d.txt"));
        assert_eq!(hunks[0].anchor.as_deref(), Some("fn main"));
        assert_eq!(hunks[0].old_lines, ["ctx", "old"]);
        assert_eq!(hunks[0].new_lines, ["ctx", "new"]);
        assert!(hunks[0].end_of_file);

        assert!(
            parse_patch("no envelope")
                .unwrap_err()
                .contains("*** Begin Patch")
        );
        assert!(
            parse_patch("*** Begin Patch\n*** Add File: a\nraw\n*** End Patch")
                .unwrap_err()
                .contains("+ lines")
        );
        assert!(
            parse_patch("*** Begin Patch\n*** Update File: a\n@@\n!bad\n*** End Patch")
                .unwrap_err()
                .contains("prefixes")
        );
        assert!(parse_patch("*** Begin Patch\n*** End Patch").is_err());
    }

    #[test]
    fn hunks_need_exact_unique_context() {
        let text = "a\nb\nc\nb\n";
        assert_eq!(
            apply_hunks(
                text,
                &[Hunk {
                    old_lines: vec!["a".into(), "b".into()],
                    new_lines: vec!["a".into(), "B".into()],
                    ..Hunk::default()
                }],
                "f"
            )
            .unwrap(),
            "a\nB\nc\nb\n"
        );
        let err = apply_hunks(
            text,
            &[Hunk {
                old_lines: vec!["b".into()],
                new_lines: vec!["x".into()],
                ..Hunk::default()
            }],
            "f",
        )
        .unwrap_err();
        assert_eq!(err, "apply_patch update hunk 1 matched 2 locations in f");
        // Hunks apply in order: the second `b` is unique after the first hunk.
        let out = apply_hunks(
            text,
            &[
                Hunk {
                    old_lines: vec!["a".into()],
                    new_lines: vec!["A".into()],
                    ..Hunk::default()
                },
                Hunk {
                    old_lines: vec!["c".into(), "b".into()],
                    new_lines: vec!["c".into(), "D".into()],
                    end_of_file: true,
                    ..Hunk::default()
                },
            ],
            "f",
        )
        .unwrap();
        assert_eq!(out, "A\nb\nc\nD\n");
        // CRLF files keep their line endings.
        let out = apply_hunks(
            "x\r\ny\r\n",
            &[Hunk {
                old_lines: vec!["y".into()],
                new_lines: vec!["z".into()],
                ..Hunk::default()
            }],
            "f",
        )
        .unwrap();
        assert_eq!(out, "x\r\nz\r\n");
    }

    #[tokio::test]
    async fn applies_atomically_with_alias_and_policy() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(dir.path().join("keep.txt"), "one\ntwo\nthree\n").unwrap();
        std::fs::write(dir.path().join("gone.txt"), "bye\n").unwrap();
        std::fs::write(dir.path().join("old.txt"), "x\n").unwrap();
        let ctx = ToolContext::new(dir.path());
        let patch = "*** Begin Patch\n*** Update File: keep.txt\n@@\n one\n-two\n+TWO\n*** Add File: new/file.txt\n+hello\n*** Delete File: gone.txt\n*** Update File: old.txt\n*** Move to: moved.txt\n@@\n-x\n+y\n*** End Patch";
        let out = ApplyPatchTool
            .execute(json!({"patchText": patch}), ctx.clone())
            .await;
        assert!(!out.is_error, "{}", out.text_content());
        assert_eq!(
            out.text_content(),
            "Success. Updated the following files:\nD gone.txt\nM keep.txt\nA moved.txt\nA new/file.txt\nD old.txt"
        );
        assert_eq!(
            std::fs::read_to_string(dir.path().join("keep.txt")).unwrap(),
            "one\nTWO\nthree\n"
        );
        assert_eq!(
            std::fs::read_to_string(dir.path().join("new/file.txt")).unwrap(),
            "hello\n"
        );
        assert_eq!(
            std::fs::read_to_string(dir.path().join("moved.txt")).unwrap(),
            "y\n"
        );
        assert!(!dir.path().join("gone.txt").exists());
        assert!(!dir.path().join("old.txt").exists());
        assert!(
            out.details.unwrap()["diff"]
                .as_str()
                .unwrap()
                .contains("+TWO")
        );

        // A failing hunk leaves every file untouched.
        let patch = "*** Begin Patch\n*** Add File: other.txt\n+x\n*** Update File: keep.txt\n@@\n-missing\n+y\n*** End Patch";
        let out = ApplyPatchTool
            .execute(json!({"patch": patch}), ctx.clone())
            .await;
        assert!(out.is_error);
        assert!(!dir.path().join("other.txt").exists());

        let patch = "*** Begin Patch\n*** Add File: ../escape.txt\n+x\n*** End Patch";
        let out = ApplyPatchTool
            .execute(json!({"patch": patch}), ctx.clone())
            .await;
        assert!(
            out.text_content()
                .starts_with("path escapes workspace_root")
        );
        let out = ApplyPatchTool
            .execute(json!({"patch": "a", "patchText": "b"}), ctx.clone())
            .await;
        assert_eq!(
            out.text_content(),
            "apply_patch accepts patch or patchText, not both"
        );
        let patch = "*** Begin Patch\n*** Add File: keep.txt\n+x\n*** End Patch";
        let out = ApplyPatchTool.execute(json!({"patch": patch}), ctx).await;
        assert_eq!(
            out.text_content(),
            "apply_patch add file already exists: keep.txt"
        );
    }
}
