use std::ops::Range;

use futures::future::BoxFuture;
use serde::Deserialize;
use serde_json::{Value, json};
use unicode_normalization::UnicodeNormalization as _;

use super::mutation::{MutationKind, require_approval};
use super::{Tool, ToolContext, ToolOutput, de_opt_bool, parse_args};

pub struct EditTool;

#[derive(Debug, Clone, Deserialize)]
pub struct Replacement {
    #[serde(rename = "oldText")]
    pub old_text: String,
    #[serde(rename = "newText")]
    pub new_text: String,
    /// Replace every non-overlapping occurrence instead of requiring one.
    #[serde(default, deserialize_with = "de_opt_bool")]
    pub replace_all: Option<bool>,
}

impl Replacement {
    pub fn new(old_text: impl Into<String>, new_text: impl Into<String>) -> Self {
        Self {
            old_text: old_text.into(),
            new_text: new_text.into(),
            replace_all: None,
        }
    }

    pub fn all(mut self) -> Self {
        self.replace_all = Some(true);
        self
    }
}

#[derive(Deserialize)]
struct Args {
    path: String,
    #[serde(default)]
    edits: Vec<Replacement>,
    /// Legacy single-edit form (normally folded into `edits` by the registry).
    #[serde(rename = "oldText", default)]
    old_text: Option<String>,
    #[serde(rename = "newText", default)]
    new_text: Option<String>,
    #[serde(default, deserialize_with = "de_opt_bool")]
    replace_all: Option<bool>,
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
                    "minItems": 1,
                    "description": "One or more targeted replacements. Each edit is matched against the original file, not incrementally. Do not include overlapping or nested edits. If two changes touch the same block or nearby lines, merge them into one edit instead.",
                    "items": {
                        "type": "object",
                        "properties": {
                            "oldText": { "type": "string", "description": "Exact text for one targeted replacement. It must be unique in the original file and must not overlap with any other edits[].oldText in the same call." },
                            "newText": { "type": "string", "description": "Replacement text for this targeted edit." },
                            "replace_all": { "type": "boolean", "description": "Replace every non-overlapping occurrence for this edit." }
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

    fn execute(&self, raw: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        Box::pin(async move {
            let mut args: Args = match parse_args("edit", raw.clone()) {
                Ok(a) => a,
                Err(e) => return e,
            };
            if let (Some(old_text), Some(new_text)) = (args.old_text.take(), args.new_text.take()) {
                args.edits.push(Replacement {
                    old_text,
                    new_text,
                    replace_all: None,
                });
            }
            if args.edits.is_empty() {
                return ToolOutput::error(
                    "Invalid arguments for edit: edits must contain at least one replacement",
                );
            }
            if let Some(all) = args.replace_all {
                for edit in &mut args.edits {
                    edit.replace_all.get_or_insert(all);
                }
            }
            let path = match ctx.resolve_path(&args.path) {
                Ok(r) => r.path,
                Err(e) => return e,
            };
            let shown = ctx.display(&path);
            let original = match tokio::fs::read(&path).await {
                Ok(bytes) => {
                    if bytes.contains(&0) {
                        return ToolOutput::error(format!(
                            "edit refuses to modify binary file {shown}"
                        ));
                    }
                    match String::from_utf8(bytes) {
                        Ok(s) => s,
                        Err(_) => {
                            return ToolOutput::error(format!("{shown} is not a UTF-8 text file"));
                        }
                    }
                }
                Err(e) => return ToolOutput::error(format!("Cannot read {shown}: {e}")),
            };
            let applied = match apply_edits_counted(&original, &args.edits, &shown) {
                Ok(s) => s,
                Err(msg) => return ToolOutput::error(msg),
            };
            if let Some(queued) = require_approval(
                &ctx,
                MutationKind::FileEdit,
                "edit",
                format!(
                    "edit {shown} replacing {} bytes with {} bytes across {} matches",
                    applied.old_bytes, applied.new_bytes, applied.matches
                ),
                &raw,
            ) {
                return queued;
            }
            let updated = applied.text;
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
                applied.matches
            ))
            .with_details(json!({
                "diff": diff,
                "firstChangedLine": first_changed_line,
                "path": path,
                "matchCount": applied.matches,
                "editCount": args.edits.len(),
                "usedFuzzyMatch": applied.fuzzy,
            }))
        })
    }
}

struct Applied {
    text: String,
    matches: usize,
    old_bytes: usize,
    new_bytes: usize,
    fuzzy: bool,
}

/// Apply all replacements against the original text.
///
/// Line endings and a UTF-8 BOM are normalised for matching and restored
/// afterwards. When an exact match is missing, a line-wise match is tried on
/// text normalised like Pi's `normalizeForFuzzyMatch` (NFKC, trailing
/// whitespace, typographic quotes, dashes and spaces); the original lines
/// are replaced. Each edit must match once unless `replace_all` is set.
pub fn apply_edits(original: &str, edits: &[Replacement], shown: &str) -> Result<String, String> {
    apply_edits_counted(original, edits, shown).map(|a| a.text)
}

fn apply_edits_counted(
    original: &str,
    edits: &[Replacement],
    shown: &str,
) -> Result<Applied, String> {
    let (bom, body) = match original.strip_prefix('\u{feff}') {
        Some(rest) => ("\u{feff}", rest),
        None => ("", original),
    };
    let crlf = detect_crlf(body);
    let content = normalize_to_lf(body);
    let single = edits.len() == 1;

    let mut ranges: Vec<(Range<usize>, String, usize)> = Vec::new();
    let mut old_bytes = 0;
    let mut new_bytes = 0;
    let mut fuzzy = false;
    for (i, edit) in edits.iter().enumerate() {
        let old = normalize_to_lf(&edit.old_text);
        let new = normalize_to_lf(&edit.new_text);
        if old.is_empty() {
            return Err(if single {
                format!("oldText must not be empty in {shown}.")
            } else {
                format!("edits[{i}].oldText must not be empty in {shown}.")
            });
        }
        let (found, used_fuzzy) = find_matches(&content, &old);
        fuzzy |= used_fuzzy;
        if found.is_empty() {
            return Err(if single {
                format!(
                    "Could not find the exact text in {shown}. The old text must match exactly including all whitespace and newlines."
                )
            } else {
                format!(
                    "Could not find edits[{i}] in {shown}. The oldText must match exactly including all whitespace and newlines."
                )
            });
        }
        let replace_all = edit.replace_all.unwrap_or(false);
        if found.len() > 1 && !replace_all {
            let n = found.len();
            return Err(if single {
                format!(
                    "Found {n} occurrences of the text in {shown}. The text must be unique. Please provide more context to make it unique."
                )
            } else {
                format!(
                    "Found {n} occurrences of edits[{i}] in {shown}. Each oldText must be unique. Please provide more context to make it unique."
                )
            });
        }
        for range in found {
            old_bytes += range.len();
            new_bytes += new.len();
            ranges.push((range, new.clone(), i));
        }
    }
    ranges.sort_by_key(|(r, _, _)| r.start);
    for pair in ranges.windows(2) {
        if pair[0].0.end > pair[1].0.start {
            return Err(format!(
                "edits[{}] and edits[{}] overlap in {shown}. Merge them into one edit or target disjoint regions.",
                pair[0].2, pair[1].2
            ));
        }
    }
    let mut result = content.clone();
    for (range, new, _) in ranges.iter().rev() {
        result.replace_range(range.clone(), new);
    }
    if result == content {
        return Err(if single {
            format!(
                "No changes made to {shown}. The replacement produced identical content. This might indicate an issue with special characters or the text not existing as expected."
            )
        } else {
            format!("No changes made to {shown}. The replacements produced identical content.")
        });
    }
    let result = if crlf {
        result.replace('\n', "\r\n")
    } else {
        result
    };
    Ok(Applied {
        text: format!("{bom}{result}"),
        matches: ranges.len(),
        old_bytes,
        new_bytes,
        fuzzy,
    })
}

/// Pi's `detectLineEnding`: CRLF when the first line break is CRLF.
fn detect_crlf(text: &str) -> bool {
    match (text.find("\r\n"), text.find('\n')) {
        (Some(crlf), Some(lf)) => crlf < lf,
        _ => false,
    }
}

fn normalize_to_lf(text: &str) -> String {
    text.replace("\r\n", "\n").replace('\r', "\n")
}

/// Non-overlapping exact matches, falling back to fuzzy line matches.
fn find_matches(haystack: &str, needle: &str) -> (Vec<Range<usize>>, bool) {
    let exact: Vec<Range<usize>> = haystack
        .match_indices(needle)
        .map(|(i, m)| i..i + m.len())
        .collect();
    if !exact.is_empty() {
        return (exact, false);
    }
    let mut fuzzy = fuzzy_line_matches(haystack, needle);
    // Keep the first of any overlapping windows.
    let mut kept: Vec<Range<usize>> = Vec::with_capacity(fuzzy.len());
    for range in fuzzy.drain(..) {
        if kept.last().is_none_or(|last| last.end <= range.start) {
            kept.push(range);
        }
    }
    let used = !kept.is_empty();
    (kept, used)
}

/// Pi's `normalizeForFuzzyMatch` applied to a single line.
pub(crate) fn normalize_line(line: &str) -> String {
    let nfkc: String = line.nfkc().collect();
    nfkc.trim_end()
        .chars()
        .map(|c| match c {
            '\u{2018}' | '\u{2019}' | '\u{201A}' | '\u{201B}' => '\'',
            '\u{201C}' | '\u{201D}' | '\u{201E}' | '\u{201F}' => '"',
            '\u{2010}'..='\u{2015}' | '\u{2212}' => '-',
            '\u{00A0}' | '\u{2002}'..='\u{200A}' | '\u{202F}' | '\u{205F}' | '\u{3000}' => ' ',
            other => other,
        })
        .collect::<String>()
        .trim_end()
        .to_string()
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
    // (start, end_without_newline, end_with_newline) byte offsets of each line.
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
        Replacement::new(o, n)
    }

    #[test]
    fn applies_multiple_disjoint_edits() {
        let out = apply_edits("a\nb\nc\nd\n", &[r("d", "D"), r("a", "A")], "f").unwrap();
        assert_eq!(out, "A\nb\nc\nD\n");
    }

    #[test]
    fn rejects_missing_ambiguous_overlapping_and_noop() {
        assert_eq!(
            apply_edits("abc", &[r("x", "y")], "f").unwrap_err(),
            "Could not find the exact text in f. The old text must match exactly including all whitespace and newlines."
        );
        assert_eq!(
            apply_edits("aa", &[r("a", "b")], "f").unwrap_err(),
            "Found 2 occurrences of the text in f. The text must be unique. Please provide more context to make it unique."
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
        assert_eq!(
            apply_edits("abc", &[r("", "b")], "f").unwrap_err(),
            "oldText must not be empty in f."
        );
        assert_eq!(
            apply_edits("abc", &[r("a", "1"), r("z", "2")], "f").unwrap_err(),
            "Could not find edits[1] in f. The oldText must match exactly including all whitespace and newlines."
        );
    }

    #[test]
    fn replace_all_replaces_every_occurrence() {
        let out = apply_edits("foo foo\nfoo\n", &[r("foo", "bar").all()], "f").unwrap();
        assert_eq!(out, "bar bar\nbar\n");
    }

    #[test]
    fn preserves_crlf_and_bom() {
        let out = apply_edits("\u{feff}one\r\ntwo\r\n", &[r("one\ntwo", "1\n2")], "f").unwrap();
        assert_eq!(out, "\u{feff}1\r\n2\r\n");
    }

    #[test]
    fn fuzzy_matches_trailing_whitespace_quotes_and_nfkc() {
        let out = apply_edits(
            "let s = \u{201C}x\u{201D};   \nnext\n",
            &[r("let s = \"x\";\n", "let s = 1;\n")],
            "f",
        )
        .unwrap();
        assert_eq!(out, "let s = 1;\nnext\n");
        // NFKC folds the "ﬁ" ligature and full-width letters.
        let out = apply_edits(
            "de\u{FB01}ne \u{FF21}\nrest\n",
            &[r("define A", "defined")],
            "f",
        )
        .unwrap();
        assert_eq!(out, "defined\nrest\n");
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
        // Legacy single-edit shape with top-level replace_all.
        std::fs::write(dir.path().join("g.txt"), "x x x\n").unwrap();
        let out = EditTool
            .execute(
                json!({"path": "g.txt", "oldText": "x", "newText": "y", "replace_all": true}),
                ToolContext::new(dir.path()),
            )
            .await;
        assert!(!out.is_error, "{}", out.text_content());
        assert_eq!(
            out.text_content(),
            "Successfully replaced 3 block(s) in g.txt."
        );
        std::fs::write(dir.path().join("bin"), [b'a', 0, b'b']).unwrap();
        let out = EditTool
            .execute(
                json!({"path": "bin", "edits": [{"oldText": "a", "newText": "c"}]}),
                ToolContext::new(dir.path()),
            )
            .await;
        assert_eq!(out.text_content(), "edit refuses to modify binary file bin");
    }
}
