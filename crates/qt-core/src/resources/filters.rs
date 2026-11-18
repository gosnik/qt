//! Settings resource filters (`!exclude`, `+force-include`, `-force-exclude`).
//!
//! Filter entries sit next to paths in `skillRoots`, `promptRoots` and
//! `themeRoots`. `!pattern` is a glob that disables matching resources,
//! `+path` re-enables an exact match and `-path` disables an exact match,
//! applied in that order. Patterns are tried against the resource path
//! relative to the agent home, the root's parent and the root itself, the
//! file name, and the full path; for a skill's `SKILL.md` the same forms of
//! its directory are tried too.

use std::path::Path;

use regex::Regex;

use super::fsutil::{file_name, generic, normalize, path_string, relative_to};

/// How resource paths are matched against filter patterns.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MatchKind {
    Generic,
    /// `SKILL.md` files also match by their directory.
    Skill,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Mode {
    Glob,
    Exact,
}

fn normalize_pattern(pattern: &str) -> String {
    let pattern = pattern.strip_prefix("./").unwrap_or(pattern);
    pattern.replace('\\', "/")
}

/// `**/` matches any directory prefix, `**` anything, `*` within a segment,
/// `?` one non-slash character.
pub fn glob_to_regex(pattern: &str) -> Option<Regex> {
    let chars: Vec<char> = pattern.chars().collect();
    let mut expr = String::from("^");
    let mut i = 0;
    while i < chars.len() {
        match chars[i] {
            '*' if chars.get(i + 1) == Some(&'*') => {
                if chars.get(i + 2) == Some(&'/') {
                    expr.push_str("(?:.*/)?");
                    i += 3;
                } else {
                    expr.push_str(".*");
                    i += 2;
                }
                continue;
            }
            '*' => expr.push_str("[^/]*"),
            '?' => expr.push_str("[^/]"),
            c => expr.push_str(&regex::escape(&c.to_string())),
        }
        i += 1;
    }
    expr.push('$');
    Regex::new(&expr).ok()
}

fn relative_candidate(path: &Path, base: &str) -> String {
    if base.is_empty() {
        return normalize_pattern(&generic(path));
    }
    normalize_pattern(&generic(&relative_to(path, Path::new(base))))
}

fn candidates(path: &Path, base: &str, kind: MatchKind) -> Vec<String> {
    let mut out = vec![
        relative_candidate(path, base),
        normalize_pattern(&file_name(path)),
        normalize_pattern(&path_string(path)),
    ];
    if kind == MatchKind::Skill
        && file_name(path) == "SKILL.md"
        && let Some(dir) = path.parent()
    {
        out.push(relative_candidate(dir, base));
        out.push(normalize_pattern(&file_name(dir)));
        out.push(normalize_pattern(&path_string(dir)));
    }
    out.sort();
    out.dedup();
    out
}

fn matches_one(path: &Path, base: &str, pattern: &str, kind: MatchKind, mode: Mode) -> bool {
    let pattern = normalize_pattern(pattern);
    if pattern.is_empty() {
        return false;
    }
    let regex = match mode {
        Mode::Glob => match glob_to_regex(&pattern) {
            Some(r) => Some(r),
            None => return false,
        },
        Mode::Exact => None,
    };
    candidates(path, base, kind)
        .iter()
        .any(|candidate| match &regex {
            Some(r) => r.is_match(candidate),
            None => *candidate == pattern,
        })
}

/// Does `path` match `pattern` (glob) relative to `base_directory`?
pub fn path_matches_filter(
    path: &Path,
    base_directory: &str,
    pattern: &str,
    kind: MatchKind,
) -> bool {
    matches_one(path, base_directory, pattern, kind, Mode::Glob)
}

fn matches_any(
    path: &Path,
    root: &Path,
    fallback_base: &Path,
    patterns: &[String],
    kind: MatchKind,
    mode: Mode,
) -> bool {
    if patterns.is_empty() {
        return false;
    }
    let mut bases: Vec<String> = Vec::new();
    if !fallback_base.as_os_str().is_empty() {
        bases.push(path_string(fallback_base));
    }
    if !root.as_os_str().is_empty() {
        if let Some(parent) = root.parent() {
            bases.push(path_string(parent));
        }
        bases.push(path_string(root));
    }
    if bases.is_empty() {
        bases.push(String::new());
    }
    bases.sort();
    bases.dedup();
    patterns.iter().any(|pattern| {
        bases
            .iter()
            .any(|base| matches_one(path, base, pattern, kind, mode))
    })
}

fn strip_prefix(filters: &[String], prefix: char) -> Vec<String> {
    filters
        .iter()
        .filter_map(|f| f.strip_prefix(prefix).map(str::to_string))
        .collect()
}

/// Is a resource enabled after applying `filters`?
pub fn is_enabled(
    path: &Path,
    root: &Path,
    fallback_base: &Path,
    filters: &[String],
    kind: MatchKind,
) -> bool {
    if filters.is_empty() {
        return true;
    }
    let path = normalize(path);
    let mut enabled = true;
    if matches_any(
        &path,
        root,
        fallback_base,
        &strip_prefix(filters, '!'),
        kind,
        Mode::Glob,
    ) {
        enabled = false;
    }
    if matches_any(
        &path,
        root,
        fallback_base,
        &strip_prefix(filters, '+'),
        kind,
        Mode::Exact,
    ) {
        enabled = true;
    }
    if matches_any(
        &path,
        root,
        fallback_base,
        &strip_prefix(filters, '-'),
        kind,
        Mode::Exact,
    ) {
        enabled = false;
    }
    enabled
}

/// Remove filtered-out records in place.
pub fn apply<T>(
    records: &mut Vec<T>,
    filters: &[String],
    fallback_base: &Path,
    kind: MatchKind,
    parts: impl Fn(&T) -> (&Path, &Path),
) {
    if filters.is_empty() {
        return;
    }
    records.retain(|record| {
        let (path, root) = parts(record);
        is_enabled(path, root, fallback_base, filters, kind)
    });
}

/// Is a settings array entry a filter rather than a path?
pub fn is_filter_entry(value: &str) -> bool {
    value.starts_with(['!', '+', '-'])
}

#[cfg(test)]
mod tests {
    use super::*;

    fn s(v: &[&str]) -> Vec<String> {
        v.iter().map(|x| x.to_string()).collect()
    }

    #[test]
    fn glob_semantics() {
        let r = glob_to_regex("**/foo/*.md").unwrap();
        assert!(r.is_match("foo/a.md"));
        assert!(r.is_match("x/y/foo/a.md"));
        assert!(!r.is_match("foo/b/a.md"));
        assert!(glob_to_regex("a?c").unwrap().is_match("abc"));
        assert!(glob_to_regex("a.c").unwrap().is_match("a.c"));
        assert!(!glob_to_regex("a.c").unwrap().is_match("abc"));
    }

    #[test]
    fn exclude_then_force_include_then_force_exclude() {
        let home = Path::new("/home/u/.qt");
        let root = Path::new("/home/u/.qt/skills");
        let a = Path::new("/home/u/.qt/skills/alpha/SKILL.md");
        let b = Path::new("/home/u/.qt/skills/beta/SKILL.md");
        let filters = s(&["!*", "+alpha"]);
        assert!(is_enabled(a, root, home, &filters, MatchKind::Skill));
        assert!(!is_enabled(b, root, home, &filters, MatchKind::Skill));
        let filters = s(&["+skills/alpha", "-alpha"]);
        assert!(!is_enabled(a, root, home, &filters, MatchKind::Skill));
        assert!(is_enabled(b, root, home, &filters, MatchKind::Skill));
        // Generic kind does not match by the SKILL.md directory.
        let filters = s(&["!alpha"]);
        assert!(is_enabled(a, root, home, &filters, MatchKind::Generic));
        assert!(!is_enabled(a, root, home, &filters, MatchKind::Skill));
        // Glob relative to the agent home.
        let filters = s(&["!skills/**"]);
        assert!(!is_enabled(a, root, home, &filters, MatchKind::Generic));
        assert!(is_filter_entry("!x") && is_filter_entry("+x") && is_filter_entry("-x"));
        assert!(!is_filter_entry("x"));
    }
}
