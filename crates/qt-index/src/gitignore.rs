//! `.gitignore` matching as implemented by the archived index.
//!
//! Every `.gitignore` under the root (outside skipped scan directories) is
//! loaded. Rules are applied from the root down; within a file later rules
//! win. Patterns support `*`, `**` (treated like `*`) and `?`; a pattern
//! without `/` matches any path segment, a pattern with `/` is anchored to the
//! `.gitignore` directory, and a trailing `/` makes the rule match the
//! directory and everything beneath it.
//!
//! Unlike the archived matcher, rules are re-read on every load instead of
//! being cached for the life of the process, so edits to `.gitignore` take
//! effect at the next scan.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use crate::util::{
    generic, lexically_normal, path_has_prefix, should_skip_scan_directory, trim_ascii,
    weakly_canonical,
};

#[derive(Debug, Clone)]
struct Rule {
    base: PathBuf,
    pattern: String,
    negated: bool,
    directory_only: bool,
    basename_only: bool,
}

#[derive(Debug, Clone)]
pub struct GitIgnoreMatcher {
    root: PathBuf,
    rules_by_base: BTreeMap<PathBuf, Vec<Rule>>,
}

fn glob_matches(pattern: &[u8], text: &[u8]) -> bool {
    let mut p = 0usize;
    let mut t = 0usize;
    let mut star: Option<usize> = None;
    let mut star_t = 0usize;
    while t < text.len() {
        if p < pattern.len() && (pattern[p] == b'?' || pattern[p] == text[t]) {
            p += 1;
            t += 1;
        } else if p < pattern.len() && pattern[p] == b'*' {
            if p + 1 < pattern.len() && pattern[p + 1] == b'*' {
                p += 1;
            }
            star = Some(p);
            p += 1;
            star_t = t;
        } else if let Some(s) = star {
            p = s + 1;
            star_t += 1;
            t = star_t;
        } else {
            return false;
        }
    }
    while p < pattern.len() && pattern[p] == b'*' {
        p += 1;
    }
    p == pattern.len()
}

fn basename_rule_matches(pattern: &str, relative: &str) -> bool {
    relative
        .split('/')
        .any(|segment| glob_matches(pattern.as_bytes(), segment.as_bytes()))
}

fn path_rule_matches(pattern: &str, relative: &str) -> bool {
    let p = pattern.as_bytes();
    let r = relative.as_bytes();
    glob_matches(p, r)
        || (r.len() > p.len() && r[p.len()] == b'/' && glob_matches(p, &r[..p.len()]))
}

fn directory_rule_matches(pattern: &str, relative: &str) -> bool {
    if path_rule_matches(pattern, relative) {
        return true;
    }
    let r = relative.as_bytes();
    r.iter()
        .enumerate()
        .filter(|(_, b)| **b == b'/')
        .any(|(slash, _)| glob_matches(pattern.as_bytes(), &r[..slash]))
}

fn relative_text(absolute: &Path, base: &Path) -> String {
    let absolute_text = generic(absolute);
    let mut base_text = generic(base);
    if absolute_text == base_text {
        return String::new();
    }
    if !base_text.is_empty() && !base_text.ends_with('/') {
        base_text.push('/');
    }
    match absolute_text.strip_prefix(&base_text) {
        Some(rest) => rest.to_string(),
        None => String::new(),
    }
}

fn unescape(pattern: &str) -> String {
    let mut result = String::with_capacity(pattern.len());
    let mut escaped = false;
    for ch in pattern.chars() {
        if escaped {
            result.push(ch);
            escaped = false;
        } else if ch == '\\' {
            escaped = true;
        } else {
            result.push(ch);
        }
    }
    if escaped {
        result.push('\\');
    }
    result
}

impl GitIgnoreMatcher {
    /// Load every `.gitignore` below `root`.
    pub fn load(root: &Path) -> Self {
        let root = weakly_canonical(root);
        let mut matcher = Self {
            root: root.clone(),
            rules_by_base: BTreeMap::new(),
        };
        let root_gitignore = root.join(".gitignore");
        if root_gitignore.is_file() {
            matcher.load_file(&root_gitignore);
        }
        let mut nested = Vec::new();
        let _ = crate::util::walk(&root, |path, is_dir, _| {
            if is_dir {
                return Ok(!should_skip_scan_directory(path));
            }
            if path.file_name().is_some_and(|n| n == ".gitignore")
                && path != root_gitignore
                && path.is_file()
            {
                nested.push(path.to_path_buf());
            }
            Ok(true)
        });
        for file in nested {
            matcher.load_file(&file);
        }
        matcher
    }

    /// A matcher with no rules.
    pub fn empty(root: &Path) -> Self {
        Self {
            root: root.to_path_buf(),
            rules_by_base: BTreeMap::new(),
        }
    }

    fn load_file(&mut self, gitignore: &Path) {
        let Ok(bytes) = std::fs::read(gitignore) else {
            return;
        };
        let text = String::from_utf8_lossy(&bytes);
        let base = lexically_normal(gitignore.parent().unwrap_or(Path::new("")));
        for raw in crate::util::getlines(&text) {
            let raw = raw.strip_suffix('\r').unwrap_or(raw);
            let mut line = trim_ascii(raw).to_string();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let mut negated = false;
            if let Some(rest) = line.strip_prefix('!') {
                negated = true;
                line = rest.to_string();
            }
            if line.is_empty() {
                continue;
            }
            let mut directory_only = false;
            if line.ends_with('/') {
                directory_only = true;
                line.pop();
            }
            if let Some(rest) = line.strip_prefix('/') {
                line = rest.to_string();
            }
            let basename_only = !line.contains('/');
            let pattern = unescape(&line);
            if pattern.is_empty() {
                continue;
            }
            self.rules_by_base
                .entry(base.clone())
                .or_default()
                .push(Rule {
                    base: base.clone(),
                    pattern,
                    negated,
                    directory_only,
                    basename_only,
                });
        }
    }

    /// Whether `path` (absolute, or relative to the root) is ignored.
    pub fn ignores(&self, path: &Path) -> bool {
        let absolute = if path.is_absolute() {
            lexically_normal(path)
        } else {
            lexically_normal(&self.root.join(path))
        };
        let mut bases = Vec::new();
        let mut dir = Some(absolute.as_path());
        while let Some(current) = dir {
            if current.as_os_str().is_empty() || !path_has_prefix(current, &self.root) {
                break;
            }
            bases.push(current.to_path_buf());
            if current == self.root {
                break;
            }
            dir = current.parent();
        }
        bases.reverse();

        let mut ignored = false;
        for base in bases {
            let Some(rules) = self.rules_by_base.get(&base) else {
                continue;
            };
            for rule in rules {
                let text = relative_text(&absolute, &rule.base);
                if text.is_empty() {
                    continue;
                }
                let matches = if rule.directory_only {
                    directory_rule_matches(&rule.pattern, &text)
                } else if rule.basename_only {
                    basename_rule_matches(&rule.pattern, &text)
                } else {
                    path_rule_matches(&rule.pattern, &text)
                };
                if matches {
                    ignored = !rule.negated;
                }
            }
        }
        ignored
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    fn setup(gitignore: &str) -> (tempfile::TempDir, PathBuf, GitIgnoreMatcher) {
        let dir = tempfile::tempdir().unwrap();
        let root = fs::canonicalize(dir.path()).unwrap();
        fs::write(root.join(".gitignore"), gitignore).unwrap();
        let matcher = GitIgnoreMatcher::load(&root);
        (dir, root, matcher)
    }

    #[test]
    fn basename_and_anchored_patterns() {
        let (_d, root, m) = setup("*.o\n/out\ndocs/*.tmp\n# comment\n\n");
        assert!(m.ignores(&root.join("a/b/c.o")));
        assert!(m.ignores(&root.join("out")));
        assert!(m.ignores(&root.join("out/x.c")));
        assert!(m.ignores(&root.join("docs/a.tmp")));
        assert!(!m.ignores(&root.join("src/docs/a.tmp")));
        assert!(!m.ignores(&root.join("main.c")));
    }

    #[test]
    fn negation_and_directory_rules() {
        let (_d, root, m) = setup("gen/\n*.log\n!keep.log\n");
        assert!(m.ignores(&root.join("gen")));
        assert!(m.ignores(&root.join("gen/deep/file.c")));
        assert!(m.ignores(&root.join("x.log")));
        assert!(!m.ignores(&root.join("keep.log")));
    }

    #[test]
    fn nested_gitignore_applies_below_its_directory() {
        let (_d, root, _) = setup("");
        fs::create_dir_all(root.join("sub")).unwrap();
        fs::write(root.join("sub/.gitignore"), "local.txt\n").unwrap();
        let m = GitIgnoreMatcher::load(&root);
        assert!(m.ignores(&root.join("sub/local.txt")));
        assert!(!m.ignores(&root.join("local.txt")));
    }

    #[test]
    fn glob_semantics() {
        assert!(glob_matches(b"a*c", b"abbc"));
        assert!(glob_matches(b"a**c", b"a/b/c"));
        assert!(glob_matches(b"a?c", b"abc"));
        assert!(!glob_matches(b"a?c", b"ac"));
    }
}
