//! `!`/`+`/`-` include/exclude filters used by package entries and
//! `pi.extensions` declarations (archived loader and package manager globs).

use std::path::{Component, Path, PathBuf};

/// Backslashes to slashes and a leading `./` removed.
pub fn normalize_pattern(pattern: &str) -> String {
    let p = pattern.replace('\\', "/");
    p.strip_prefix("./").map(str::to_string).unwrap_or(p)
}

/// Archived glob: `*` and `?` do not cross `/`, `**` does (`**/` may match nothing).
pub fn glob_match(text: &str, pattern: &str) -> bool {
    let pattern = normalize_pattern(pattern);
    match_from(text.as_bytes(), 0, pattern.as_bytes(), 0)
}

fn match_from(text: &[u8], mut ti: usize, pattern: &[u8], mut pi: usize) -> bool {
    while pi < pattern.len() {
        if pattern[pi] == b'*' {
            if pattern.get(pi + 1) == Some(&b'*') {
                return match_after_starstar(text, ti, pattern, pi + 2);
            }
            pi += 1;
            let mut next = ti;
            loop {
                if next > ti && text[next - 1] == b'/' {
                    return false;
                }
                if match_from(text, next, pattern, pi) {
                    return true;
                }
                if next >= text.len() {
                    return false;
                }
                next += 1;
            }
        }
        if ti >= text.len() {
            return false;
        }
        if pattern[pi] == b'?' {
            if text[ti] == b'/' {
                return false;
            }
        } else if pattern[pi] != text[ti] {
            return false;
        }
        ti += 1;
        pi += 1;
    }
    ti == text.len()
}

fn match_after_starstar(text: &[u8], ti: usize, pattern: &[u8], pi: usize) -> bool {
    if pattern.get(pi) == Some(&b'/') && match_from(text, ti, pattern, pi + 1) {
        return true;
    }
    (ti..=text.len()).any(|next| match_from(text, next, pattern, pi))
}

/// `path` with `.`/`..` resolved lexically and `/` separators.
pub fn lexical_normal(path: &Path) -> PathBuf {
    let mut out = PathBuf::new();
    for c in path.components() {
        match c {
            Component::CurDir => {}
            Component::ParentDir => {
                if !out.pop() {
                    out.push("..");
                }
            }
            other => out.push(other.as_os_str()),
        }
    }
    out
}

pub fn display_path(path: &Path) -> String {
    lexical_normal(path).to_string_lossy().replace('\\', "/")
}

fn relative_to(path: &Path, base: &Path) -> String {
    match lexical_normal(path).strip_prefix(lexical_normal(base)) {
        Ok(rel) if !rel.as_os_str().is_empty() => display_path(rel),
        _ => path
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default(),
    }
}

fn candidates(path: &Path, base: &Path) -> [String; 4] {
    let relative = relative_to(path, base);
    let rooted = match base.file_name() {
        Some(name) => format!("{}/{relative}", name.to_string_lossy()),
        None => relative.clone(),
    };
    let name = path
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default();
    [relative, name, rooted, display_path(path)]
}

fn matches_glob(path: &Path, pattern: &str, base: &Path) -> bool {
    candidates(path, base)
        .iter()
        .any(|c| glob_match(c, pattern))
}

fn matches_exact(path: &Path, pattern: &str, base: &Path) -> bool {
    let normalized = normalize_pattern(pattern);
    candidates(path, base).contains(&normalized)
}

/// Pattern groups of a filter list.
#[derive(Default)]
struct Groups {
    includes: Vec<String>,
    excludes: Vec<String>,
    force_includes: Vec<String>,
    force_excludes: Vec<String>,
}

fn group(patterns: &[String]) -> Groups {
    let mut g = Groups::default();
    for p in patterns.iter().filter(|p| !p.is_empty()) {
        if let Some(rest) = p.strip_prefix('+') {
            g.force_includes.push(rest.into());
        } else if let Some(rest) = p.strip_prefix('-') {
            g.force_excludes.push(rest.into());
        } else if let Some(rest) = p.strip_prefix('!') {
            g.excludes.push(rest.into());
        } else {
            g.includes.push(p.clone());
        }
    }
    g
}

/// A pattern beginning with `!`, `+` or `-`.
pub fn is_override_pattern(pattern: &str) -> bool {
    pattern.starts_with(['!', '+', '-'])
}

/// Archived extension filtering: includes (globs) select, `!` globs remove,
/// `+` exact paths re-add, `-` exact paths remove. Order of `paths` is kept;
/// force-included paths are appended.
pub fn apply_filter_patterns(
    paths: Vec<PathBuf>,
    patterns: &[String],
    base: &Path,
) -> Vec<PathBuf> {
    if patterns.is_empty() {
        return paths;
    }
    let g = group(patterns);
    let mut filtered: Vec<PathBuf> = if g.includes.is_empty() {
        paths.clone()
    } else {
        paths
            .iter()
            .filter(|p| g.includes.iter().any(|i| matches_glob(p, i, base)))
            .cloned()
            .collect()
    };
    filtered.retain(|p| !g.excludes.iter().any(|e| matches_glob(p, e, base)));
    for p in &paths {
        if !filtered.contains(p) && g.force_includes.iter().any(|f| matches_exact(p, f, base)) {
            filtered.push(p.clone());
        }
    }
    filtered.retain(|p| !g.force_excludes.iter().any(|f| matches_exact(p, f, base)));
    filtered
}

fn resource_candidates(relative: &str, resource: &Path) -> Vec<String> {
    let mut c = vec![
        relative.to_string(),
        resource
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default(),
        display_path(resource),
    ];
    if resource.file_name().is_some_and(|n| n == "SKILL.md") {
        if let Some(parent_rel) = Path::new(relative).parent() {
            let s = display_path(parent_rel);
            if !s.is_empty() {
                c.push(s);
            }
        }
        if let Some(parent) = resource.parent() {
            if let Some(n) = parent.file_name() {
                c.push(n.to_string_lossy().into_owned());
            }
            c.push(display_path(parent));
        }
    }
    c.sort();
    c.dedup();
    c
}

/// Archived package-resource filtering (`resource_enabled_by_filters`).
/// `relative` is the resource path relative to its package root.
pub fn resource_enabled_by_filters(filters: &[String], relative: &str, resource: &Path) -> bool {
    if filters.is_empty() {
        return true;
    }
    let g = group(filters);
    let cands = resource_candidates(relative, resource);
    let glob_any = |list: &[String]| list.iter().any(|f| cands.iter().any(|c| glob_match(c, f)));
    let exact_any = |list: &[String]| {
        list.iter().any(|f| {
            let n = normalize_pattern(f);
            cands.contains(&n)
        })
    };
    let mut enabled = g.includes.is_empty() || glob_any(&g.includes);
    if enabled && glob_any(&g.excludes) {
        enabled = false;
    }
    if exact_any(&g.force_includes) {
        enabled = true;
    }
    if exact_any(&g.force_excludes) {
        enabled = false;
    }
    enabled
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn globs() {
        assert!(glob_match("a/b.ts", "a/*.ts"));
        assert!(!glob_match("a/b/c.ts", "a/*.ts"));
        assert!(glob_match("a/b/c.ts", "a/**/*.ts"));
        assert!(glob_match("a/c.ts", "a/**/*.ts"));
        assert!(glob_match("x.rhai", "./*.rhai"));
        assert!(glob_match("ab", "a?"));
        assert!(!glob_match("a/", "a?"));
        assert!(glob_match("anything/at/all", "**"));
    }

    #[test]
    fn extension_filters() {
        let base = Path::new("/pkg");
        let paths = vec![
            PathBuf::from("/pkg/ext/a.rhai"),
            PathBuf::from("/pkg/ext/b.rhai"),
            PathBuf::from("/pkg/other/c.ts"),
        ];
        let f = |p: &[&str]| {
            apply_filter_patterns(
                paths.clone(),
                &p.iter().map(|s| s.to_string()).collect::<Vec<_>>(),
                base,
            )
        };
        assert_eq!(f(&[]).len(), 3);
        assert_eq!(f(&["ext/*.rhai"]).len(), 2);
        assert_eq!(f(&["!*.ts"]).len(), 2);
        assert_eq!(f(&["ext/a.rhai", "+other/c.ts"]).len(), 2);
        assert_eq!(
            f(&["-ext/a.rhai"]),
            vec![paths[1].clone(), paths[2].clone()]
        );
        // rooted relative (base dir name) and absolute paths also match
        assert_eq!(f(&["pkg/ext/b.rhai"]), vec![paths[1].clone()]);
        assert_eq!(f(&["/pkg/other/c.ts"]), vec![paths[2].clone()]);
    }

    #[test]
    fn resource_filters() {
        let skill = Path::new("/pkg/skills/review/SKILL.md");
        let s = |v: &[&str]| v.iter().map(|x| x.to_string()).collect::<Vec<_>>();
        assert!(resource_enabled_by_filters(
            &[],
            "skills/review/SKILL.md",
            skill
        ));
        assert!(!resource_enabled_by_filters(
            &s(&["-skills/review"]),
            "skills/review/SKILL.md",
            skill
        ));
        assert!(resource_enabled_by_filters(
            &s(&["!review", "+review"]),
            "skills/review/SKILL.md",
            skill
        ));
        assert!(!resource_enabled_by_filters(
            &s(&["other/**"]),
            "skills/review/SKILL.md",
            skill
        ));
    }
}
