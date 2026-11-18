//! Composer autocomplete (archived `tui/widgets/autocomplete.cpp`, Pi
//! `CombinedAutocompleteProvider`): slash command names and arguments on the
//! first line, and `@` path mentions anywhere.
//!
//! Everything here is pure text and filesystem logic; the workspace owns
//! the popup and feeds it the composer text and cursor (UTF-8 offsets).

use std::path::{Path, PathBuf};

use qt_core::slash::{CompletionItem, SlashCommand};

/// Most path suggestions shown (archived `max_file_suggestions`).
pub const MAX_PATH_SUGGESTIONS: usize = 50;
/// Directory entries visited by one recursive `@` search.
const MAX_WALK_ENTRIES: usize = 20_000;
/// Characters that end a path token (archived `path_delimiters`).
const DELIMITERS: &[char] = &[' ', '\t', '"', '\'', '='];

/// What the text before the cursor asks to complete.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CompletionKind {
    /// `/na` on the first line: command names.
    SlashName,
    /// `/name args`: argument completions for `command`.
    SlashArgs { command: String },
    /// `@path` / `@"path with spaces`: workspace paths.
    Path { quoted: bool },
}

/// A completion request: the kind, the typed prefix and where it starts.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CompletionRequest {
    pub kind: CompletionKind,
    /// Text the completion replaces: the name without `/`, the argument
    /// text, or the raw path after `@` / `@"`.
    pub query: String,
    /// Byte offset where the replaced text starts (after `/` for names,
    /// at `@` for paths, at the arguments for slash arguments).
    pub start: usize,
}

/// One row of the popup.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Suggestion {
    /// Inserted text.
    pub value: String,
    /// Main label (`model`, `main.rs`, `src/`).
    pub label: String,
    /// Secondary text (argument hint and description, or the path).
    pub detail: Option<String>,
    /// A directory path completion: no trailing space, completion continues.
    pub directory: bool,
}

impl Suggestion {
    fn from_core(item: CompletionItem) -> Self {
        Self {
            value: item.value,
            label: item.label,
            detail: item.description,
            directory: false,
        }
    }
}

fn is_delimiter(c: char) -> bool {
    DELIMITERS.contains(&c)
}

fn token_start(text: &str, index: usize) -> bool {
    index == 0 || text[..index].chars().next_back().is_some_and(is_delimiter)
}

/// Start of an unclosed `"` in `text` (archived `find_unclosed_quote_start`).
fn unclosed_quote_start(text: &str) -> Option<usize> {
    let mut start = None;
    for (i, c) in text.char_indices() {
        if c == '"' {
            start = match start {
                None => Some(i),
                Some(_) => None,
            };
        }
    }
    start
}

/// The `@…` token ending at the end of `before` (archived `extract_at_prefix`).
/// Returns the offset of `@` and whether the path is quoted.
fn at_prefix(before: &str) -> Option<(usize, bool)> {
    if let Some(quote) = unclosed_quote_start(before)
        && quote > 0
        && before[..quote].ends_with('@')
        && token_start(before, quote - 1)
    {
        return Some((quote - 1, true));
    }
    let start = before
        .char_indices()
        .rev()
        .find(|(_, c)| is_delimiter(*c) || *c == '\n')
        .map(|(i, c)| i + c.len_utf8())
        .unwrap_or(0);
    before[start..].starts_with('@').then_some((start, false))
}

/// Work out what to complete for `text` with the cursor at byte `cursor`.
pub fn detect(text: &str, cursor: usize) -> Option<CompletionRequest> {
    let cursor = cursor.min(text.len());
    if !text.is_char_boundary(cursor) {
        return None;
    }
    let before = &text[..cursor];
    let line_start = before.rfind('\n').map(|i| i + 1).unwrap_or(0);
    let line = &before[line_start..];
    if let Some((at, quoted)) = at_prefix(line) {
        let skip = if quoted { 2 } else { 1 };
        return Some(CompletionRequest {
            kind: CompletionKind::Path { quoted },
            query: line[at + skip..].to_string(),
            start: line_start + at,
        });
    }
    if line_start != 0 {
        return None;
    }
    let offset = line.len() - line.trim_start_matches([' ', '\t']).len();
    let rest = line[offset..].strip_prefix('/')?;
    match rest.find([' ', '\t']) {
        None => {
            // `/usr/bin` is a path, not a command.
            if rest.contains('/') {
                return None;
            }
            Some(CompletionRequest {
                kind: CompletionKind::SlashName,
                query: rest.to_string(),
                start: offset + 1,
            })
        }
        Some(space) => {
            let command = rest[..space].to_string();
            if command.is_empty() {
                return None;
            }
            let args_start = offset + 1 + space + 1;
            Some(CompletionRequest {
                kind: CompletionKind::SlashArgs { command },
                query: line[args_start..].to_string(),
                start: args_start,
            })
        }
    }
}

/// Command name suggestions: case-insensitive substring matches, else a
/// fuzzy match (archived). The detail is `hint - description`.
pub fn command_suggestions(commands: &[SlashCommand], query: &str) -> Vec<Suggestion> {
    let item = |c: &SlashCommand| {
        let detail = match (&c.argument_hint, c.description.is_empty()) {
            (Some(hint), true) => Some(hint.clone()),
            (Some(hint), false) => Some(format!("{hint} - {}", c.description)),
            (None, true) => None,
            (None, false) => Some(c.description.clone()),
        };
        Suggestion {
            value: c.name.clone(),
            label: format!("/{}", c.name),
            detail,
            directory: false,
        }
    };
    if query.is_empty() {
        return commands.iter().map(item).collect();
    }
    let lower = query.to_lowercase();
    let mut out: Vec<Suggestion> = commands
        .iter()
        .filter(|c| c.name.to_lowercase().contains(&lower))
        .map(item)
        .collect();
    if out.is_empty() {
        out = commands
            .iter()
            .filter(|c| qt_core::model_scope::fuzzy_matches(query, &c.name))
            .map(item)
            .collect();
    } else {
        // Prefix matches first, keeping registry order within each group.
        out.sort_by_key(|s| !s.value.to_lowercase().starts_with(&lower));
    }
    out
}

/// Argument suggestions from the core providers.
pub fn argument_suggestions(items: Vec<CompletionItem>) -> Vec<Suggestion> {
    items.into_iter().map(Suggestion::from_core).collect()
}

/// Index of the row to preselect (archived `best_autocomplete_match_index`):
/// an exact value match, else the first prefix match, else the first row.
pub fn best_match(items: &[Suggestion], query: &str) -> usize {
    if query.is_empty() {
        return 0;
    }
    items
        .iter()
        .position(|s| s.value == query)
        .or_else(|| items.iter().position(|s| s.value.starts_with(query)))
        .unwrap_or(0)
}

/// `@path` value for `path` (archived `build_completion_value`).
pub fn completion_value(path: &str, quoted: bool) -> String {
    if quoted || path.contains(' ') {
        format!("@\"{path}\"")
    } else {
        format!("@{path}")
    }
}

fn expand_home(raw: &str) -> Option<PathBuf> {
    let home = || std::env::var_os("HOME").map(PathBuf::from);
    if raw == "~" {
        return home();
    }
    raw.strip_prefix("~/")
        .and_then(|rest| home().map(|h| h.join(rest)))
}

fn resolve_search_root(base: &Path, display_base: &str) -> PathBuf {
    if display_base.is_empty() {
        return base.to_path_buf();
    }
    if let Some(path) =
        expand_home(display_base.trim_end_matches('/')).or_else(|| expand_home(display_base))
    {
        return path;
    }
    let path = Path::new(display_base);
    if path.is_absolute() {
        path.to_path_buf()
    } else {
        base.join(path)
    }
}

/// Ordered subsequence match; returns the number of skipped characters.
fn fuzzy_gaps(query: &str, text: &str) -> Option<usize> {
    let mut gaps = 0;
    let mut chars = text.chars();
    let mut started = false;
    for q in query.chars().filter(|c| !c.is_whitespace()) {
        loop {
            let c = chars.next()?;
            if c == q {
                started = true;
                break;
            }
            if started {
                gaps += 1;
            }
        }
    }
    Some(gaps)
}

/// Archived `score_file_entry`: exact name 100, name prefix 80, name
/// substring 50, path substring 30, fuzzy path match up to 25; directories
/// get +10. A score of 0 drops the entry.
pub fn score_entry(display_path: &str, query: &str, directory: bool) -> i32 {
    if query.is_empty() {
        return if directory { 11 } else { 1 };
    }
    let query = query.to_lowercase();
    let path = display_path.to_lowercase();
    let name = path.rsplit('/').next().unwrap_or(&path);
    let mut score = if name == query {
        100
    } else if name.starts_with(&query) {
        80
    } else if name.contains(&query) {
        50
    } else if path.contains(&query) {
        30
    } else {
        match fuzzy_gaps(&query, &path) {
            Some(gaps) => (25 - gaps.min(24) as i32).max(1),
            None => 0,
        }
    };
    if directory && score > 0 {
        score += 10;
    }
    score
}

/// Recursive `@` path suggestions under `base` (archived
/// `get_recursive_at_suggestions`, using a gitignore-aware walk like the
/// archived `fd` search: hidden files included, `.git` excluded).
pub fn path_suggestions(base: &Path, raw: &str, quoted: bool, max: usize) -> Vec<Suggestion> {
    let query = raw.replace('\\', "/");
    let (display_base, tail) = match query.rfind('/') {
        Some(i) => (query[..=i].to_string(), query[i + 1..].to_string()),
        None => (String::new(), query.clone()),
    };
    let root = resolve_search_root(base, &display_base);
    if !root.is_dir() {
        return Vec::new();
    }
    let walker = ignore::WalkBuilder::new(&root)
        .hidden(false)
        .follow_links(true)
        .require_git(false)
        .filter_entry(|e| e.file_name() != std::ffi::OsStr::new(".git"))
        .build();
    let mut scored: Vec<(i32, String, usize, Suggestion)> = Vec::new();
    for (order, entry) in walker.flatten().take(MAX_WALK_ENTRIES).enumerate() {
        if entry.depth() == 0 {
            continue;
        }
        let Ok(relative) = entry.path().strip_prefix(&root) else {
            continue;
        };
        let relative = relative.to_string_lossy().replace('\\', "/");
        let directory = entry.path().is_dir();
        let display = format!("{display_base}{relative}");
        let score = score_entry(&display, &tail, directory);
        if score <= 0 {
            continue;
        }
        let name = entry.file_name().to_string_lossy().into_owned();
        let completion = if directory {
            format!("{display}/")
        } else {
            display.clone()
        };
        let label = if directory { format!("{name}/") } else { name };
        scored.push((
            score,
            label.to_lowercase(),
            order,
            Suggestion {
                value: completion_value(&completion, quoted),
                label,
                detail: Some(display),
                directory,
            },
        ));
    }
    scored.sort_by(|a, b| {
        b.0.cmp(&a.0)
            .then(b.3.directory.cmp(&a.3.directory))
            .then(a.1.cmp(&b.1))
            .then(a.2.cmp(&b.2))
    });
    scored.truncate(max);
    scored.into_iter().map(|(_, _, _, s)| s).collect()
}

/// The composer text and cursor after applying `item` to `request`
/// (archived `apply_completion`).
pub fn apply(
    text: &str,
    cursor: usize,
    request: &CompletionRequest,
    item: &Suggestion,
) -> (String, usize) {
    let cursor = cursor.min(text.len());
    let start = request.start.min(cursor);
    let before = &text[..start];
    let after = &text[cursor..];
    match &request.kind {
        CompletionKind::SlashName => {
            let out = format!("{before}{} {after}", item.value);
            (out, start + item.value.len() + 1)
        }
        CompletionKind::SlashArgs { .. } => {
            let out = format!("{before}{}{after}", item.value);
            (out, start + item.value.len())
        }
        CompletionKind::Path { quoted } => {
            let value_quoted = item.value.ends_with('"');
            let after = if *quoted && value_quoted && after.starts_with('"') {
                &after[1..]
            } else {
                after
            };
            let suffix = if item.directory { "" } else { " " };
            let out = format!("{before}{}{suffix}{after}", item.value);
            let mut offset = item.value.len();
            if item.directory && value_quoted {
                offset -= 1;
            }
            (out, start + offset + suffix.len())
        }
    }
}

/// Whether Enter on a highlighted suggestion should also submit the line
/// (Pi: slash command names do, paths and arguments do not).
pub fn submits_on_enter(kind: &CompletionKind) -> bool {
    matches!(kind, CompletionKind::SlashName)
}

/// `!cmd` / `!!cmd` (archived `parse_interactive_command`): the command and
/// whether it is excluded from the model context.
pub fn parse_bash(text: &str) -> Option<(String, bool)> {
    let trimmed = text.trim();
    let rest = trimmed.strip_prefix('!')?;
    let (exclude, rest) = match rest.strip_prefix('!') {
        Some(r) => (true, r),
        None => (false, rest),
    };
    let command = rest.trim();
    (!command.is_empty()).then(|| (command.to_string(), exclude))
}

#[cfg(test)]
mod tests {
    use super::*;
    use qt_core::slash::builtin_commands;

    fn req(kind: CompletionKind, query: &str, start: usize) -> CompletionRequest {
        CompletionRequest {
            kind,
            query: query.into(),
            start,
        }
    }

    #[test]
    fn detects_slash_names_arguments_and_paths() {
        assert_eq!(
            detect("/mo", 3),
            Some(req(CompletionKind::SlashName, "mo", 1))
        );
        assert_eq!(
            detect("  /", 3),
            Some(req(CompletionKind::SlashName, "", 3))
        );
        assert_eq!(
            detect("/model claude", 13),
            Some(req(
                CompletionKind::SlashArgs {
                    command: "model".into()
                },
                "claude",
                7
            ))
        );
        // Cursor inside the name completes only what precedes it.
        assert_eq!(
            detect("/model x", 3),
            Some(req(CompletionKind::SlashName, "mo", 1))
        );
        assert_eq!(detect("/usr/bin", 8), None);
        assert_eq!(detect("hello /mo", 9), None);
        assert_eq!(detect("line\n/mo", 8), None);
        assert_eq!(
            detect("look at @src/ma", 15),
            Some(req(CompletionKind::Path { quoted: false }, "src/ma", 8))
        );
        assert_eq!(
            detect("@", 1),
            Some(req(CompletionKind::Path { quoted: false }, "", 0))
        );
        assert_eq!(
            detect("see @\"my dir/fi", 15),
            Some(req(CompletionKind::Path { quoted: true }, "my dir/fi", 4))
        );
        assert_eq!(
            detect("a\n@x", 4),
            Some(req(CompletionKind::Path { quoted: false }, "x", 2))
        );
        // Mentions need a token boundary; e-mail addresses do not trigger.
        assert_eq!(detect("me@host", 7), None);
        assert_eq!(detect("@done ", 6), None);
        assert_eq!(
            detect("/model @", 8).unwrap().kind,
            CompletionKind::Path { quoted: false }
        );
        assert_eq!(detect("é", 1), None);
    }

    #[test]
    fn filters_commands_by_substring_then_fuzzy() {
        let commands = builtin_commands();
        let all = command_suggestions(&commands, "");
        assert_eq!(all.len(), commands.len());
        let model = command_suggestions(&commands, "mod");
        assert_eq!(model[0].value, "model");
        assert!(model.iter().any(|s| s.value == "scoped-models"));
        assert_eq!(
            model[0].detail.as_deref(),
            Some("[provider/model|search|refresh] - Select model")
        );
        assert_eq!(model[0].label, "/model");
        let upper = command_suggestions(&commands, "HELP");
        assert_eq!(upper[0].value, "help");
        // No substring: fall back to the fuzzy subsequence match.
        let fuzzy = command_suggestions(&commands, "cmpct");
        assert_eq!(fuzzy[0].value, "compact");
        assert!(command_suggestions(&commands, "zzzz").is_empty());
        assert_eq!(best_match(&model, "model"), 0);
        assert_eq!(best_match(&fuzzy, ""), 0);
    }

    #[test]
    fn applies_completions_like_the_archived_editor() {
        let r = req(CompletionKind::SlashName, "mo", 1);
        let item = Suggestion {
            value: "model".into(),
            label: "/model".into(),
            detail: None,
            directory: false,
        };
        assert_eq!(apply("/mo", 3, &r, &item), ("/model ".into(), 7));

        let r = req(
            CompletionKind::SlashArgs {
                command: "thinking".into(),
            },
            "hi",
            10,
        );
        let item = Suggestion {
            value: "high".into(),
            label: "high".into(),
            detail: None,
            directory: false,
        };
        assert_eq!(
            apply("/thinking hi", 12, &r, &item),
            ("/thinking high".into(), 14)
        );

        let file = Suggestion {
            value: "@src/main.rs".into(),
            label: "main.rs".into(),
            detail: None,
            directory: false,
        };
        let r = req(CompletionKind::Path { quoted: false }, "ma", 4);
        assert_eq!(
            apply("see @ma and", 7, &r, &file),
            ("see @src/main.rs  and".into(), 17)
        );

        let dir = Suggestion {
            value: "@\"my dir/\"".into(),
            label: "my dir/".into(),
            detail: None,
            directory: true,
        };
        let r = req(CompletionKind::Path { quoted: true }, "my", 0);
        let (text, cursor) = apply("@\"my\"", 4, &r, &dir);
        assert_eq!(text, "@\"my dir/\"");
        // The cursor stays inside the quotes so completion continues.
        assert_eq!(cursor, text.len() - 1);
        assert_eq!(
            detect(&text, cursor),
            Some(req(CompletionKind::Path { quoted: true }, "my dir/", 0))
        );
        assert!(submits_on_enter(&CompletionKind::SlashName));
        assert!(!submits_on_enter(&CompletionKind::Path { quoted: false }));
    }

    #[test]
    fn scores_and_walks_paths() {
        assert_eq!(score_entry("src/main.rs", "main.rs", false), 100);
        assert_eq!(score_entry("src/main.rs", "ma", false), 80);
        assert_eq!(score_entry("src/main.rs", "in.r", false), 50);
        assert_eq!(score_entry("src/main.rs", "src/m", false), 30);
        assert!(score_entry("src/main.rs", "smrs", false) > 0);
        assert_eq!(score_entry("src/main.rs", "zz", false), 0);
        assert_eq!(score_entry("src", "", true), 11);
        assert_eq!(score_entry("src", "sr", true), 90);

        let dir = tempfile::tempdir().unwrap();
        let root = dir.path();
        std::fs::create_dir_all(root.join("src/nested")).unwrap();
        std::fs::create_dir_all(root.join(".git/objects")).unwrap();
        std::fs::create_dir_all(root.join("target")).unwrap();
        std::fs::create_dir_all(root.join("my dir")).unwrap();
        std::fs::write(root.join(".gitignore"), "target/\n").unwrap();
        std::fs::write(root.join("src/main.rs"), "").unwrap();
        std::fs::write(root.join("src/nested/mod.rs"), "").unwrap();
        std::fs::write(root.join("target/main.rs"), "").unwrap();
        std::fs::write(root.join("my dir/notes.md"), "").unwrap();
        std::fs::write(root.join(".env"), "").unwrap();

        let found = path_suggestions(root, "main", false, 50);
        assert_eq!(found[0].value, "@src/main.rs");
        assert_eq!(found[0].label, "main.rs");
        assert!(!found.iter().any(|s| s.value.contains("target")));
        assert!(!found.iter().any(|s| s.value.contains(".git/")));

        let all = path_suggestions(root, "", false, 50);
        assert!(all[0].directory, "directories sort first: {all:?}");
        assert!(all.iter().any(|s| s.value == "@.env"));
        assert!(all.iter().any(|s| s.value == "@\"my dir/\""));
        assert_eq!(path_suggestions(root, "", false, 2).len(), 2);

        let nested = path_suggestions(root, "src/", false, 50);
        assert!(nested.iter().all(|s| s.value.starts_with("@src/")));
        assert!(
            nested
                .iter()
                .any(|s| s.value == "@src/nested/" && s.directory)
        );
        let quoted = path_suggestions(root, "my dir/no", true, 50);
        assert_eq!(quoted[0].value, "@\"my dir/notes.md\"");
        assert!(path_suggestions(root, "missing/", false, 50).is_empty());
    }

    #[test]
    fn parses_user_bash() {
        assert_eq!(parse_bash("!ls -la"), Some(("ls -la".into(), false)));
        assert_eq!(
            parse_bash("  !! git status "),
            Some(("git status".into(), true))
        );
        assert_eq!(parse_bash("!"), None);
        assert_eq!(parse_bash("!!  "), None);
        assert_eq!(parse_bash("ls"), None);
    }
}
