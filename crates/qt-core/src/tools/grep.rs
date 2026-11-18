use std::path::{Path, PathBuf};

use futures::future::BoxFuture;
use globset::{GlobBuilder, GlobMatcher};
use regex::{Regex, RegexBuilder};
use serde::Deserialize;
use serde_json::{Value, json};
use tokio_util::sync::CancellationToken;

use super::{
    Tool, ToolContext, ToolOutput, de_opt_bool, de_opt_usize, extensions_for_type,
    matches_extension, parse_args,
};
use crate::util::{DEFAULT_MAX_BYTES, floor_char_boundary, shorten};

const DEFAULT_LIMIT: usize = 100;
const MAX_LINE_CHARS: usize = 500;

pub struct GrepTool;

#[derive(Deserialize)]
struct Args {
    pattern: String,
    #[serde(default)]
    path: Option<String>,
    #[serde(default)]
    glob: Option<String>,
    #[serde(rename = "type", default)]
    file_type: Option<String>,
    #[serde(rename = "ignoreCase", default, deserialize_with = "de_opt_bool")]
    ignore_case: Option<bool>,
    #[serde(default, deserialize_with = "de_opt_bool")]
    literal: Option<bool>,
    #[serde(default, deserialize_with = "de_opt_usize")]
    context: Option<usize>,
    #[serde(rename = "-A", default, deserialize_with = "de_opt_usize")]
    after: Option<usize>,
    #[serde(rename = "-B", default, deserialize_with = "de_opt_usize")]
    before: Option<usize>,
    #[serde(default, deserialize_with = "de_opt_usize")]
    limit: Option<usize>,
    #[serde(default, deserialize_with = "de_opt_usize")]
    head_limit: Option<usize>,
    #[serde(default, deserialize_with = "de_opt_usize")]
    offset: Option<usize>,
    #[serde(default)]
    output_mode: Option<String>,
    #[serde(default, deserialize_with = "de_opt_bool")]
    multiline: Option<bool>,
    #[serde(default, deserialize_with = "de_opt_bool")]
    hidden: Option<bool>,
    #[serde(default, deserialize_with = "de_opt_bool")]
    no_ignore: Option<bool>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum OutputMode {
    Content,
    FilesWithMatches,
    Count,
}

impl OutputMode {
    fn parse(value: Option<&str>) -> Result<Self, String> {
        match value.map(str::trim) {
            None | Some("") | Some("content") => Ok(OutputMode::Content),
            Some("files_with_matches") | Some("files_only") => Ok(OutputMode::FilesWithMatches),
            Some("count") => Ok(OutputMode::Count),
            Some(_) => Err(
                "grep output_mode must be content, files_with_matches, files_only, or count".into(),
            ),
        }
    }

    fn as_str(self) -> &'static str {
        match self {
            OutputMode::Content => "content",
            OutputMode::FilesWithMatches => "files_with_matches",
            OutputMode::Count => "count",
        }
    }
}

/// A glob filter applied to paths relative to the search root.
pub(crate) struct PathGlob {
    matcher: GlobMatcher,
    name_only: bool,
}

impl PathGlob {
    pub(crate) fn new(pattern: &str) -> Result<Self, String> {
        let glob = GlobBuilder::new(pattern)
            .literal_separator(true)
            .build()
            .map_err(|e| format!("Invalid glob pattern {pattern:?}: {e}"))?;
        Ok(Self {
            matcher: glob.compile_matcher(),
            name_only: !pattern.contains('/'),
        })
    }

    pub(crate) fn matches(&self, relative: &Path) -> bool {
        if self.matcher.is_match(relative) {
            return true;
        }
        self.name_only
            && relative
                .file_name()
                .is_some_and(|n| self.matcher.is_match(Path::new(n)))
    }
}

/// Directory walk options shared by `grep` and `find`.
#[derive(Debug, Clone, Copy, Default)]
pub(crate) struct WalkOptions {
    /// Include hidden files and directories (`.git` is still skipped).
    pub hidden: bool,
    /// Ignore `.gitignore`/`.ignore` rules.
    pub no_ignore: bool,
    /// Directory names never descended into.
    pub skip_dirs: &'static [&'static str],
}

/// Walk entries under `root` without following symlinks.
pub(crate) fn walker(root: &Path, options: WalkOptions) -> ignore::Walk {
    let respect = !options.no_ignore;
    let skip = options.skip_dirs;
    ignore::WalkBuilder::new(root)
        .hidden(!options.hidden)
        .ignore(respect)
        .git_ignore(respect)
        .git_global(respect)
        .git_exclude(respect)
        .parents(respect)
        .require_git(false)
        .follow_links(false)
        .sort_by_file_path(|a, b| a.cmp(b))
        .filter_entry(move |entry| {
            if entry.depth() == 0 || !entry.file_type().is_some_and(|t| t.is_dir()) {
                return true;
            }
            let name = entry.file_name().to_string_lossy();
            name != ".git" && !skip.contains(&name.as_ref())
        })
        .build()
}

impl Tool for GrepTool {
    fn name(&self) -> &'static str {
        "grep"
    }
    fn description(&self) -> String {
        format!(
            "Search file contents for a pattern. Returns matching lines with file paths and line numbers. Respects .gitignore. Output is truncated to {DEFAULT_LIMIT} matches or {}KB (whichever is hit first). Long lines are truncated to {MAX_LINE_CHARS} chars.",
            DEFAULT_MAX_BYTES / 1024
        )
    }
    fn snippet(&self) -> &'static str {
        "Search file contents for patterns (respects .gitignore)"
    }
    fn parameters(&self) -> Value {
        json!({
            "type": "object",
            "properties": {
                "pattern": { "type": "string", "description": "Search pattern (regex or literal string)" },
                "path": { "type": "string", "description": "Directory or file to search (default: current directory)" },
                "glob": { "type": "string", "description": "Filter files by glob pattern, e.g. '*.ts' or '**/*.spec.ts'" },
                "type": { "type": "string", "description": "Restrict to a common file type, e.g. 'rust', 'cpp', 'ts', 'py', or an extension" },
                "ignoreCase": { "type": "boolean", "description": "Case-insensitive search (default: false)" },
                "literal": { "type": "boolean", "description": "Treat pattern as literal string instead of regex (default: false)" },
                "context": { "type": "number", "description": "Number of lines to show before and after each match (default: 0)" },
                "limit": { "type": "number", "description": "Maximum number of matches to return (default: 100)" },
                "output_mode": { "type": "string", "enum": ["content", "files_with_matches", "count"], "description": "Output matching lines (default), only the paths of files with matches, or per-file match counts" },
                "head_limit": { "type": "number", "description": "Limit output entries (matches, files or counts); 0 means unlimited" },
                "offset": { "type": "number", "description": "Skip the first N output entries" },
                "multiline": { "type": "boolean", "description": "Let the pattern span lines ('.' matches newlines)" },
                "hidden": { "type": "boolean", "description": "Include hidden files and directories (default: false)" },
                "no_ignore": { "type": "boolean", "description": "Do not apply .gitignore rules (default: false)" }
            },
            "required": ["pattern"]
        })
    }
    fn parallel_safe(&self) -> bool {
        true
    }

    fn execute(&self, args: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        Box::pin(async move {
            let args: Args = match parse_args("grep", args) {
                Ok(a) => a,
                Err(e) => return e,
            };
            let root = match ctx.resolve_path(args.path.as_deref().unwrap_or(".")) {
                Ok(r) => r.path,
                Err(e) => return e,
            };
            let cancel = ctx.cancel.clone();
            tokio::task::spawn_blocking(move || search(args, root, &ctx, &cancel))
                .await
                .unwrap_or_else(|e| ToolOutput::error(format!("grep failed: {e}")))
        })
    }
}

/// One reported match: the lines it spans (0-based, inclusive).
struct Hit {
    first: usize,
    last: usize,
}

fn search(args: Args, root: PathBuf, ctx: &ToolContext, cancel: &CancellationToken) -> ToolOutput {
    if args.pattern.is_empty() {
        return ToolOutput::error("grep requires a non-empty search pattern");
    }
    let mode = match OutputMode::parse(args.output_mode.as_deref()) {
        Ok(m) => m,
        Err(e) => return ToolOutput::error(e),
    };
    let multiline = args.multiline.unwrap_or(false);
    let pattern = if args.literal.unwrap_or(false) {
        regex::escape(&args.pattern)
    } else {
        args.pattern.clone()
    };
    let regex: Regex = match RegexBuilder::new(&pattern)
        .case_insensitive(args.ignore_case.unwrap_or(false))
        .multi_line(multiline)
        .dot_matches_new_line(multiline)
        .build()
    {
        Ok(r) => r,
        Err(e) => return ToolOutput::error(format!("Invalid regex: {e}")),
    };
    let glob = match args.glob.as_deref().map(PathGlob::new).transpose() {
        Ok(g) => g,
        Err(e) => return ToolOutput::error(e),
    };
    let type_extensions = args.file_type.as_deref().map(extensions_for_type);
    if !root.exists() {
        return ToolOutput::error(format!("Path not found: {}", ctx.display(&root)));
    }
    let (limit, explicit_paging) = match (args.head_limit, args.limit) {
        (Some(0), _) => (usize::MAX, true),
        (Some(n), _) => (n, true),
        (None, Some(n)) if n > 0 => (n, false),
        _ => (DEFAULT_LIMIT, false),
    };
    let offset = args.offset.unwrap_or(0);
    let explicit_paging = explicit_paging || offset > 0;
    let context = args.context.unwrap_or(0);
    let before = args.before.unwrap_or(context);
    let after = args.after.unwrap_or(context);
    let base: PathBuf = if root.is_file() {
        root.parent().map(Path::to_path_buf).unwrap_or_default()
    } else {
        root.clone()
    };

    let walk = WalkOptions {
        hidden: args.hidden.unwrap_or(false),
        no_ignore: args.no_ignore.unwrap_or(false),
        skip_dirs: &[],
    };
    let mut out = String::new();
    let mut seen = 0usize;
    let mut emitted = 0usize;
    let mut total_matches = 0usize;
    let mut files_with_matches = 0usize;
    let mut limit_hit = false;
    let mut bytes_hit = false;
    'files: for entry in walker(&root, walk).flatten() {
        if cancel.is_cancelled() {
            return ToolOutput::error("grep aborted");
        }
        if !entry.file_type().is_some_and(|t| t.is_file()) {
            continue;
        }
        let path = entry.path();
        let relative = path.strip_prefix(&base).unwrap_or(path);
        if let Some(glob) = &glob
            && !glob.matches(relative)
        {
            continue;
        }
        if let Some(exts) = &type_extensions {
            let name = path
                .file_name()
                .map(|n| n.to_string_lossy())
                .unwrap_or_default();
            if !matches_extension(&name, exts) {
                continue;
            }
        }
        let Ok(bytes) = std::fs::read(path) else {
            continue;
        };
        if bytes.iter().take(8192).any(|b| *b == 0) {
            continue;
        }
        let text = String::from_utf8_lossy(&bytes);
        let lines: Vec<&str> = text.lines().collect();
        let (hits, count) = find_hits(&regex, &text, &lines, multiline);
        if hits.is_empty() {
            continue;
        }
        total_matches += count;
        files_with_matches += 1;
        let display = ctx.display(path);
        match mode {
            OutputMode::FilesWithMatches | OutputMode::Count => {
                seen += 1;
                if seen <= offset {
                    continue;
                }
                if emitted >= limit {
                    limit_hit = true;
                    break 'files;
                }
                emitted += 1;
                if mode == OutputMode::Count {
                    out.push_str(&format!("{display}:{count}\n"));
                } else {
                    out.push_str(&display);
                    out.push('\n');
                }
            }
            OutputMode::Content => {
                let mut last_printed: Option<usize> = None;
                for hit in &hits {
                    seen += 1;
                    if seen <= offset {
                        continue;
                    }
                    if emitted >= limit {
                        limit_hit = true;
                        break 'files;
                    }
                    emitted += 1;
                    let from = hit.first.saturating_sub(before);
                    let to = (hit.last + after).min(lines.len().saturating_sub(1));
                    if (before > 0 || after > 0)
                        && let Some(prev) = last_printed
                        && from > prev + 1
                    {
                        out.push_str("--\n");
                    }
                    for (j, line) in lines.iter().enumerate().take(to + 1).skip(from) {
                        if last_printed.is_some_and(|p| j <= p) {
                            continue;
                        }
                        let is_match = (hit.first..=hit.last).contains(&j)
                            || (!multiline && regex.is_match(line));
                        let sep = if is_match { ':' } else { '-' };
                        out.push_str(&format!(
                            "{display}{sep}{}{sep} {}\n",
                            j + 1,
                            shorten(line, MAX_LINE_CHARS)
                        ));
                        last_printed = Some(j);
                    }
                    if out.len() > DEFAULT_MAX_BYTES {
                        bytes_hit = true;
                        break 'files;
                    }
                }
            }
        }
        if out.len() > DEFAULT_MAX_BYTES {
            bytes_hit = true;
            break;
        }
    }
    let details = json!({
        "matches": total_matches,
        "files": files_with_matches,
        "outputMode": mode.as_str(),
        "offset": offset,
        "matchLimitReached": if limit_hit { json!(limit) } else { Value::Null },
    });
    if emitted == 0 {
        let text = if total_matches > 0 {
            format!("No matches found after offset {offset}")
        } else {
            "No matches found".to_string()
        };
        return ToolOutput::text(text).with_details(details);
    }
    let mut text = if out.len() > DEFAULT_MAX_BYTES {
        out[..floor_char_boundary(&out, DEFAULT_MAX_BYTES)].to_string()
    } else {
        out
    };
    while text.ends_with('\n') {
        text.pop();
    }
    if limit_hit {
        let next = offset + limit;
        text.push_str(&match (mode, explicit_paging) {
            (OutputMode::Content, false) => format!(
                "\n\n[{limit} matches limit reached. Use limit={} for more, or refine pattern]",
                limit * 2
            ),
            (OutputMode::Content, true) => format!(
                "\n\n[{limit} matches limit reached. Use offset={next} for more, or refine pattern]"
            ),
            _ => format!(
                "\n\n[{limit} results limit reached. Use offset={next} for more, or refine pattern]"
            ),
        });
    } else if bytes_hit {
        text.push_str(&format!(
            "\n\n[{}KB limit reached. Refine pattern or use glob]",
            DEFAULT_MAX_BYTES / 1024
        ));
    }
    ToolOutput::text(text).with_details(details)
}

/// Matches in one file and the match count used by `count` mode.
fn find_hits(regex: &Regex, text: &str, lines: &[&str], multiline: bool) -> (Vec<Hit>, usize) {
    if !multiline {
        let mut hits = Vec::new();
        let mut count = 0;
        for (i, line) in lines.iter().enumerate() {
            let n = regex.find_iter(line).count();
            if n > 0 {
                count += n;
                hits.push(Hit { first: i, last: i });
            }
        }
        return (hits, count);
    }
    // Byte offset at which each line starts.
    let mut starts = Vec::with_capacity(lines.len());
    let mut offset = 0;
    for piece in text.split_inclusive('\n') {
        starts.push(offset);
        offset += piece.len();
    }
    let line_of = |byte: usize| match starts.binary_search(&byte) {
        Ok(i) => i,
        Err(i) => i.saturating_sub(1),
    };
    let max_line = lines.len().saturating_sub(1);
    let hits: Vec<Hit> = regex
        .find_iter(text)
        .map(|m| {
            let first = line_of(m.start()).min(max_line);
            let end = if m.end() > m.start() {
                m.end() - 1
            } else {
                m.start()
            };
            let last = line_of(end).min(max_line).max(first);
            Hit { first, last }
        })
        .collect();
    let count = hits.len();
    (hits, count)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fixture() -> tempfile::TempDir {
        let dir = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(dir.path().join("src")).unwrap();
        std::fs::write(
            dir.path().join("src/a.rs"),
            "fn main() {}\nlet x = 1;\nfn other() {}\n",
        )
        .unwrap();
        std::fs::write(dir.path().join("b.txt"), "FN upper\n").unwrap();
        std::fs::write(dir.path().join(".gitignore"), "ignored.rs\n").unwrap();
        std::fs::write(dir.path().join("ignored.rs"), "fn hidden() {}\n").unwrap();
        std::fs::create_dir_all(dir.path().join(".config")).unwrap();
        std::fs::write(dir.path().join(".config/c.rs"), "fn dotted() {}\n").unwrap();
        dir
    }

    async fn grep(ctx: &ToolContext, args: Value) -> String {
        GrepTool.execute(args, ctx.clone()).await.text_content()
    }

    #[tokio::test]
    async fn searches_with_glob_case_and_gitignore() {
        let dir = fixture();
        let ctx = ToolContext::new(dir.path());
        assert_eq!(
            grep(&ctx, json!({"pattern": "^fn"})).await,
            "src/a.rs:1: fn main() {}\nsrc/a.rs:3: fn other() {}"
        );
        assert_eq!(
            grep(
                &ctx,
                json!({"pattern": "fn", "ignoreCase": true, "glob": "*.txt"})
            )
            .await,
            "b.txt:1: FN upper"
        );
        assert_eq!(
            grep(&ctx, json!({"pattern": "x = ", "context": 1})).await,
            "src/a.rs-1- fn main() {}\nsrc/a.rs:2: let x = 1;\nsrc/a.rs-3- fn other() {}"
        );
        let out = GrepTool
            .execute(json!({"pattern": "(", "literal": true}), ctx.clone())
            .await;
        assert!(!out.is_error);
        assert!(
            grep(&ctx, json!({"pattern": "fn", "limit": 1}))
                .await
                .contains("[1 matches limit reached. Use limit=2")
        );
        assert_eq!(
            grep(&ctx, json!({"pattern": "zzz"})).await,
            "No matches found"
        );
        let out = GrepTool
            .execute(json!({"pattern": "x", "path": "/"}), ctx)
            .await;
        assert!(out.is_error);
    }

    #[tokio::test]
    async fn output_modes_paging_type_hidden_and_no_ignore() {
        let dir = fixture();
        let ctx = ToolContext::new(dir.path());
        assert_eq!(
            grep(
                &ctx,
                json!({"pattern": "fn", "ignoreCase": true, "output_mode": "files_with_matches"})
            )
            .await,
            "b.txt\nsrc/a.rs"
        );
        assert_eq!(
            grep(&ctx, json!({"pattern": "fn", "output_mode": "count"})).await,
            "src/a.rs:2"
        );
        assert_eq!(
            grep(&ctx, json!({"pattern": "fn", "offset": 1, "head_limit": 1})).await,
            "src/a.rs:3: fn other() {}"
        );
        assert!(
            grep(&ctx, json!({"pattern": "fn", "head_limit": 1}))
                .await
                .ends_with("[1 matches limit reached. Use offset=1 for more, or refine pattern]")
        );
        assert_eq!(
            grep(&ctx, json!({"pattern": "fn", "ignoreCase": true, "output_mode": "files_only", "head_limit": 1})).await,
            "b.txt\n\n[1 results limit reached. Use offset=1 for more, or refine pattern]"
        );
        assert_eq!(
            grep(
                &ctx,
                json!({"pattern": "fn", "ignoreCase": true, "type": "rust"})
            )
            .await,
            "src/a.rs:1: fn main() {}\nsrc/a.rs:3: fn other() {}"
        );
        assert_eq!(
            grep(&ctx, json!({"pattern": "dotted", "hidden": true})).await,
            ".config/c.rs:1: fn dotted() {}"
        );
        assert_eq!(
            grep(&ctx, json!({"pattern": "hidden", "no_ignore": true})).await,
            "ignored.rs:1: fn hidden() {}"
        );
        let out = GrepTool
            .execute(json!({"pattern": "x", "output_mode": "nope"}), ctx.clone())
            .await;
        assert!(out.is_error);
        // Archived aliases arrive through the registry.
        let reg = super::super::ToolRegistry::coding();
        let out = reg
            .execute(
                "search_text",
                json!({"query": "upper", "include": "*.txt", "-i": true}),
                ctx,
            )
            .await;
        assert_eq!(out.text_content(), "b.txt:1: FN upper");
    }

    #[tokio::test]
    async fn multiline_matches_span_lines() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(
            dir.path().join("m.txt"),
            "start\nstruct A {\n    x: u8,\n}\nend\n",
        )
        .unwrap();
        let ctx = ToolContext::new(dir.path());
        assert_eq!(
            grep(
                &ctx,
                json!({"pattern": "struct A \\{.*?\\}", "multiline": true})
            )
            .await,
            "m.txt:2: struct A {\nm.txt:3:     x: u8,\nm.txt:4: }"
        );
        assert_eq!(
            grep(&ctx, json!({"pattern": "struct A \\{.*?\\}"})).await,
            "No matches found"
        );
        assert_eq!(
            grep(
                &ctx,
                json!({"pattern": "^end$", "multiline": true, "output_mode": "count"})
            )
            .await,
            "m.txt:1"
        );
    }
}
