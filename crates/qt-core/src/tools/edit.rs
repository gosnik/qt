use std::ops::Range;

use futures::future::BoxFuture;
use serde::Deserialize;
use serde_json::{Value, json};

use super::{Tool, ToolContext, ToolOutput, parse_args};
use crate::util::{display_path, resolve_path};

pub struct EditTool;

#[derive(Debug, Clone, Deserialize)]
pub struct Replacement {
    #[serde(rename = "oldText")]
    pub old_text: String,
    #[serde(rename = "newText")]
    pub new_text: String,
}

#[derive(Deserialize)]
struct Args {
    path: String,
    #[serde(default)]
    edits: Vec<Replacement>,
    /// Legacy single-edit form.
    #[serde(rename = "oldText", default)]
    old_text: Option<String>,
    #[serde(rename = "newText", default)]
    new_text: Option<String>,
}

impl Tool for EditTool {
    fn name(&self) -> &'static str {
        "edit"
    }
    fn description(&self) -> String {
        "Edit a single file using exact text replacement. Every edits[].oldText must match a unique, non-overlapping region of the original file. If two changes affect the same block or nearby lines, merge them into one edit instead of emitting overlapping edits. Do not include large unchanged regions just to connect distant changes.".into()
    }
    fn snippet(&self) -> &'static str {
        "Make precise file edits with exact text replacement, including multiple disjoint edits in one call"
    }
    fn guidelines(&self) -> &'static [&'static str] {
        &[
            "Use edit for precise changes (edits[].oldText must match exactly)",
            "When changing multiple separate locations in one file, use one edit call with multiple entries in edits[] instead of multiple edit calls",
            "Each edits[].oldText is matched against the original file, not after earlier edits are applied. Do not emit overlapping or nested edits. Merge nearby changes into one edit.",
            "Keep edits[].oldText as small as possible while still being unique in the file. Do not pad with large unchanged regions.",
        ]
    }
    fn parameters(&self) -> Value {
        json!({
            "type": "object",
            "properties": {
                "path": { "type": "string", "description": "Path to the file to edit (relative or absolute)" },
                "edits": {
                    "type": "array",
                    "description": "One or more targeted replacements. Each edit is matched against the original file, not incrementally. Do not include overlapping or nested edits. If two changes touch the same block or nearby lines, merge them into one edit instead.",
                    "items": {
                        "type": "object",
                        "properties": {
                            "oldText": { "type": "string", "description": "Exact text for one targeted replacement. It must be unique in the original file and must not overlap with any other edits[].oldText in the same call." },
                            "newText": { "type": "string", "description": "Replacement text for this targeted edit." }
                        },
                        "required": ["oldText", "newText"],
                        "additionalProperties": false
                    }
                }
            },
            "required": ["path", "edits"],
            "additionalProperties": false
        })
    }

    fn execute(&self, args: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        Box::pin(async move {
            let mut args: Args = match parse_args("edit", args) {
                Ok(a) => a,
                Err(e) => return e,
            };
            if let (Some(old_text), Some(new_text)) = (args.old_text.take(), args.new_text.take()) {
                args.edits.push(Replacement { old_text, new_text });
            }
            if args.edits.is_empty() {
                return ToolOutput::error(
                    "Invalid arguments for edit: edits must contain at least one replacement",
                );
            }
            let path = resolve_path(&ctx.cwd, &args.path);
            let shown = display_path(&ctx.cwd, &path);
            let original = match tokio::fs::read(&path).await {
                Ok(bytes) => match String::from_utf8(bytes) {
                    Ok(s) => s,
                    Err(_) => {
                        return ToolOutput::error(format!("{shown} is not a UTF-8 text file"));
                    }
                },
                Err(e) => return ToolOutput::error(format!("Cannot read {shown}: {e}")),
            };
            let updated = match apply_edits(&original, &args.edits, &shown) {
                Ok(s) => s,
                Err(msg) => return ToolOutput::error(msg),
            };
            if let Err(e) = tokio::fs::write(&path, updated.as_bytes()).await {
                return ToolOutput::error(format!("Cannot write {shown}: {e}"));
            }
            let diff = similar::TextDiff::from_lines(&original, &updated)
                .unified_diff()
                .context_radius(3)
                .header(&format!("a/{shown}"), &format!("b/{shown}"))
                .to_string();
            let first_changed_line = first_changed_line(&original, &updated);
            ToolOutput::text(format!(
                "Successfully replaced {} block(s) in {shown}.",
                args.edits.len()
            ))
            .with_details(
                json!({ "diff": diff, "firstChangedLine": first_changed_line, "path": path }),
            )
        })
    }
}

/// Apply all replacements against the original text.
///
/// Line endings and a UTF-8 BOM are normalised for matching and restored
/// afterwards. When an exact match is missing, a line-wise match that ignores
/// trailing whitespace and typographic quote/dash variants is tried; it must
/// also be unique.
pub fn apply_edits(original: &str, edits: &[Replacement], shown: &str) -> Result<String, String> {
    let (bom, body) = match original.strip_prefix('\u{feff}') {
        Some(rest) => ("\u{feff}", rest),
        None => ("", original),
    };
    let crlf = body.contains("\r\n");
    let content = if crlf {
        body.replace("\r\n", "\n")
    } else {
        body.to_string()
    };

    let mut ranges: Vec<(Range<usize>, String, usize)> = Vec::with_capacity(edits.len());
    for (i, edit) in edits.iter().enumerate() {
        let old = edit.old_text.replace("\r\n", "\n");
        let new = edit.new_text.replace("\r\n", "\n");
        if old.is_empty() {
            return Err(format!("edits[{i}].oldText must not be empty."));
        }
        let range = find_unique(&content, &old).map_err(|count| {
            if count == 0 {
                format!(
                    "Could not find edits[{i}].oldText in {shown}. The oldText must match exactly including all whitespace and newlines."
                )
            } else {
                format!(
                    "Found {count} occurrences of edits[{i}].oldText in {shown}. Each oldText must be unique. Please provide more context to make it unique."
                )
            }
        })?;
        ranges.push((range, new, i));
    }
    ranges.sort_by_key(|(r, _, _)| r.start);
    for pair in ranges.windows(2) {
        if pair[0].0.end > pair[1].0.start {
            return Err(format!(
                "edits[{}] and edits[{}] overlap in {shown}. Merge them into one edit.",
                pair[0].2, pair[1].2
            ));
        }
    }
    let mut result = content.clone();
    for (range, new, _) in ranges.iter().rev() {
        result.replace_range(range.clone(), new);
    }
    if result == content {
        return Err(format!(
            "No changes made to {shown}. The replacement produced identical content."
        ));
    }
    let result = if crlf {
        result.replace('\n', "\r\n")
    } else {
        result
    };
    Ok(format!("{bom}{result}"))
}

/// Byte range of the unique match of `needle`, or the match count on failure.
fn find_unique(haystack: &str, needle: &str) -> Result<Range<usize>, usize> {
    let exact: Vec<usize> = haystack.match_indices(needle).map(|(i, _)| i).collect();
    match exact.len() {
        1 => return Ok(exact[0]..exact[0] + needle.len()),
        n if n > 1 => return Err(n),
        _ => {}
    }
    let fuzzy = fuzzy_line_matches(haystack, needle);
    match fuzzy.len() {
        1 => Ok(fuzzy[0].clone()),
        n => Err(n),
    }
}

fn normalize_line(line: &str) -> String {
    line.trim_end()
        .chars()
        .map(|c| match c {
            '\u{2018}' | '\u{2019}' | '\u{201A}' | '\u{201B}' => '\'',
            '\u{201C}' | '\u{201D}' | '\u{201E}' | '\u{201F}' => '"',
            '\u{2010}'..='\u{2015}' | '\u{2212}' => '-',
            '\u{00A0}' | '\u{2002}'..='\u{200A}' | '\u{202F}' | '\u{205F}' | '\u{3000}' => ' ',
            other => other,
        })
        .collect()
}

/// Line-window matches of `needle` after per-line normalisation.
fn fuzzy_line_matches(haystack: &str, needle: &str) -> Vec<Range<usize>> {
    let ends_with_newline = needle.ends_with('\n');
    let needle_lines: Vec<String> = needle
        .strip_suffix('\n')
        .unwrap_or(needle)
        .split('\n')
        .map(normalize_line)
        .collect();
    // (start, end_without_newline) byte offsets of each haystack line.
    let mut spans = Vec::new();
    let mut start = 0;
    for line in haystack.split_inclusive('\n') {
        let content_end = start + line.strip_suffix('\n').map_or(line.len(), str::len);
        spans.push((start, content_end, start + line.len()));
        start += line.len();
    }
    let normalized: Vec<String> = spans
        .iter()
        .map(|(s, e, _)| normalize_line(&haystack[*s..*e]))
        .collect();
    let n = needle_lines.len();
    let mut found = Vec::new();
    if n == 0 || n > normalized.len() {
        return found;
    }
    for i in 0..=normalized.len() - n {
        if normalized[i..i + n] == needle_lines[..] {
            let begin = spans[i].0;
            let end = if ends_with_newline {
                spans[i + n - 1].2
            } else {
                spans[i + n - 1].1
            };
            found.push(begin..end);
        }
    }
    found
}

fn first_changed_line(a: &str, b: &str) -> usize {
    a.lines()
        .zip(b.lines())
        .position(|(x, y)| x != y)
        .unwrap_or_else(|| a.lines().count().min(b.lines().count()))
        + 1
}

#[cfg(test)]
mod tests {
    use super::*;

    fn r(o: &str, n: &str) -> Replacement {
        Replacement {
            old_text: o.into(),
            new_text: n.into(),
        }
    }

    #[test]
    fn applies_multiple_disjoint_edits() {
        let out = apply_edits("a\nb\nc\nd\n", &[r("d", "D"), r("a", "A")], "f").unwrap();
        assert_eq!(out, "A\nb\nc\nD\n");
    }

    #[test]
    fn rejects_missing_ambiguous_overlapping_and_noop() {
        assert!(
            apply_edits("abc", &[r("x", "y")], "f")
                .unwrap_err()
                .contains("Could not find")
        );
        assert!(
            apply_edits("aa", &[r("a", "b")], "f")
                .unwrap_err()
                .contains("Found 2 occurrences")
        );
        assert!(
            apply_edits("abcdef", &[r("abc", "1"), r("cde", "2")], "f")
                .unwrap_err()
                .contains("overlap")
        );
        assert!(
            apply_edits("abc", &[r("b", "b")], "f")
                .unwrap_err()
                .contains("No changes")
        );
        assert!(apply_edits("abc", &[r("", "b")], "f").is_err());
    }

    #[test]
    fn preserves_crlf_and_bom() {
        let out = apply_edits("\u{feff}one\r\ntwo\r\n", &[r("one\ntwo", "1\n2")], "f").unwrap();
        assert_eq!(out, "\u{feff}1\r\n2\r\n");
    }

    #[test]
    fn fuzzy_matches_trailing_whitespace_and_quotes() {
        let out = apply_edits(
            "let s = \u{201C}x\u{201D};   \nnext\n",
            &[r("let s = \"x\";\n", "let s = 1;\n")],
            "f",
        )
        .unwrap();
        assert_eq!(out, "let s = 1;\nnext\n");
    }

    #[tokio::test]
    async fn tool_writes_and_reports_diff() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(dir.path().join("f.txt"), "hello\nworld\n").unwrap();
        let out = EditTool
            .execute(
                json!({"path": "f.txt", "edits": [{"oldText": "world", "newText": "there"}]}),
                ToolContext::new(dir.path()),
            )
            .await;
        assert!(!out.is_error, "{}", out.text_content());
        assert_eq!(
            std::fs::read_to_string(dir.path().join("f.txt")).unwrap(),
            "hello\nthere\n"
        );
        let details = out.details.unwrap();
        assert!(details["diff"].as_str().unwrap().contains("+there"));
        assert_eq!(details["firstChangedLine"], 2);
        // Legacy single-edit shape.
        let out = EditTool
            .execute(
                json!({"path": "f.txt", "oldText": "hello", "newText": "hi"}),
                ToolContext::new(dir.path()),
            )
            .await;
        assert!(!out.is_error);
    }
}
