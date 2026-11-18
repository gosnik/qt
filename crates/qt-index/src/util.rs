//! Path and string helpers shared by the scanner, build emulators and handlers.
//!
//! These mirror the archived `index/path_util` and `index/string_util` helpers,
//! including `std::filesystem` lexical normalisation semantics.

use std::fs;
use std::path::{Component, Path, PathBuf};

/// ASCII lower-casing (non-ASCII bytes are kept).
pub fn lowercase_ascii(value: &str) -> String {
    value.to_ascii_lowercase()
}

/// Trim ASCII whitespace from both ends.
pub fn trim_ascii(value: &str) -> &str {
    value.trim_matches(|c: char| c.is_ascii_whitespace())
}

/// Split a command line into words, honouring single/double quotes and
/// backslash escapes (quotes are removed).
pub fn split_shell_like(text: &str) -> Vec<String> {
    let mut tokens = Vec::new();
    let mut current = String::new();
    let mut quote: Option<char> = None;
    let mut escaped = false;
    for ch in text.chars() {
        if escaped {
            current.push(ch);
            escaped = false;
            continue;
        }
        if ch == '\\' {
            escaped = true;
            continue;
        }
        if let Some(q) = quote {
            if ch == q {
                quote = None;
            } else {
                current.push(ch);
            }
            continue;
        }
        if ch == '"' || ch == '\'' {
            quote = Some(ch);
            continue;
        }
        if ch.is_ascii_whitespace() {
            if !current.is_empty() {
                tokens.push(std::mem::take(&mut current));
            }
            continue;
        }
        current.push(ch);
    }
    if !current.is_empty() {
        tokens.push(current);
    }
    tokens
}

/// Lexically normalise a path (`std::filesystem::path::lexically_normal`):
/// removes `.` components and resolves `..` against preceding names.
pub fn lexically_normal(path: &Path) -> PathBuf {
    let mut out: Vec<Component<'_>> = Vec::new();
    for component in path.components() {
        match component {
            Component::CurDir => {}
            Component::ParentDir => match out.last() {
                Some(Component::Normal(_)) => {
                    out.pop();
                }
                Some(Component::RootDir) | Some(Component::Prefix(_)) => {}
                _ => out.push(component),
            },
            other => out.push(other),
        }
    }
    let mut result = PathBuf::new();
    for component in out {
        result.push(component.as_os_str());
    }
    result
}

/// `std::filesystem::weakly_canonical`: canonicalise the longest existing
/// prefix and append the remaining components lexically normalised.
pub fn weakly_canonical(path: &Path) -> PathBuf {
    if let Ok(canonical) = fs::canonicalize(path) {
        return canonical;
    }
    let absolute = if path.is_absolute() {
        path.to_path_buf()
    } else {
        std::env::current_dir()
            .map(|cwd| cwd.join(path))
            .unwrap_or_else(|_| path.to_path_buf())
    };
    let normal = lexically_normal(&absolute);
    let mut existing = normal.clone();
    let mut rest: Vec<std::ffi::OsString> = Vec::new();
    while !existing.as_os_str().is_empty() {
        if let Ok(canonical) = fs::canonicalize(&existing) {
            let mut result = canonical;
            for part in rest.iter().rev() {
                result.push(part);
            }
            return lexically_normal(&result);
        }
        match (existing.file_name(), existing.parent()) {
            (Some(name), Some(parent)) => {
                rest.push(name.to_os_string());
                existing = parent.to_path_buf();
            }
            _ => break,
        }
    }
    normal
}

/// The archived `normalize_path(base, path)`: join relative paths onto `base`
/// and resolve with `weakly_canonical` (falling back to lexical normalisation).
pub fn normalize_path(base: &Path, path: &str) -> PathBuf {
    let path = Path::new(path);
    let absolute = if path.is_absolute() {
        path.to_path_buf()
    } else {
        base.join(path)
    };
    weakly_canonical(&absolute)
}

/// Component-wise prefix test (`/a/b` is a prefix of `/a/b/c`, not of `/a/bc`).
pub fn path_has_prefix(path: &Path, prefix: &Path) -> bool {
    let mut path_it = path.components();
    for prefix_part in prefix.components() {
        match path_it.next() {
            Some(part) if part == prefix_part => {}
            _ => return false,
        }
    }
    true
}

/// `path` with `/` separators, as `generic_string()` returns it.
pub fn generic(path: &Path) -> String {
    let text = path.to_string_lossy();
    if std::path::MAIN_SEPARATOR == '/' {
        text.into_owned()
    } else {
        text.replace(std::path::MAIN_SEPARATOR, "/")
    }
}

/// Lossy `string()` of a path.
pub fn path_text(path: &Path) -> String {
    path.to_string_lossy().into_owned()
}

/// Lower-cased extension including the dot (`.cpp`), or empty.
pub fn extension_lower(path: &Path) -> String {
    match path.extension() {
        Some(ext) => format!(".{}", ext.to_string_lossy().to_ascii_lowercase()),
        None => String::new(),
    }
}

/// Extension including the dot as written (`.CPP`), or empty.
pub fn extension_text(path: &Path) -> String {
    match path.extension() {
        Some(ext) => format!(".{}", ext.to_string_lossy()),
        None => String::new(),
    }
}

/// File name as text, or empty.
pub fn file_name(path: &Path) -> String {
    path.file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default()
}

pub fn is_nix_store_path(path: &Path) -> bool {
    let mut it = path.components();
    matches!(it.next(), Some(Component::RootDir))
        && matches!(it.next(), Some(Component::Normal(n)) if n == "nix")
        && matches!(it.next(), Some(Component::Normal(n)) if n == "store")
}

pub fn is_ignored_dependency_path(path: &Path) -> bool {
    if is_nix_store_path(path) {
        return true;
    }
    path.components().any(|part| {
        let name = part.as_os_str().to_string_lossy();
        name == "node_modules"
            || name == "target"
            || name == ".git"
            || name == ".pio"
            || name.starts_with(".waf3-")
            || name == "venv"
            || name == ".venv"
            || name == "site-packages"
            || name == "__pycache__"
    })
}

pub fn is_ignored_inferred_translation_unit_path(path: &Path) -> bool {
    path.components().any(|part| {
        let name = part.as_os_str().to_string_lossy();
        name == "venv" || name == ".venv" || name == "site-packages" || name == "__pycache__"
    })
}

pub fn should_skip_scan_directory(path: &Path) -> bool {
    matches!(
        file_name(path).as_str(),
        ".git"
            | ".hg"
            | ".svn"
            | ".pio"
            | ".idea"
            | ".vscode"
            | "build"
            | "builds"
            | "cmake-build-debug"
            | "cmake-build-release"
            | "__pycache__"
            | "node_modules"
            | "third_party"
            | "vendor"
            | "external"
    )
}

pub fn should_skip_search_directory(path: &Path) -> bool {
    matches!(
        file_name(path).as_str(),
        ".git"
            | ".pio"
            | ".idea"
            | "build"
            | "builds"
            | "cmake-build-debug"
            | "cmake-build-release"
            | "third_party"
            | "vendor"
            | "external"
    )
}

/// Read a file as text (lossy UTF-8); empty when unreadable.
pub fn read_text_file(path: &Path) -> String {
    fs::read(path)
        .map(|bytes| String::from_utf8_lossy(&bytes).into_owned())
        .unwrap_or_default()
}

/// Split text into lines the way `std::getline` does: `\n` separated, the
/// final empty segment after a trailing newline is not a line. A trailing
/// `\r` is kept, as the archived code did.
pub fn getlines(text: &str) -> impl Iterator<Item = &str> {
    let trimmed = text.strip_suffix('\n').unwrap_or(text);
    let empty = text.is_empty();
    trimmed.split('\n').filter(move |_| !empty)
}

pub fn is_dir(path: &Path) -> bool {
    path.is_dir()
}

pub fn is_regular_file(path: &Path) -> bool {
    fs::metadata(path).map(|m| m.is_file()).unwrap_or(false)
}

/// FNV-1a 64-bit hash.
pub fn fnv1a64(bytes: &[u8]) -> u64 {
    let mut hash: u64 = 0xcbf2_9ce4_8422_2325;
    for byte in bytes {
        hash ^= u64::from(*byte);
        hash = hash.wrapping_mul(0x0000_0100_0000_01b3);
    }
    hash
}

/// Directory entries sorted by name (deterministic replacement for
/// `std::filesystem::directory_iterator`).
pub fn sorted_dir_entries(dir: &Path) -> Vec<fs::DirEntry> {
    let mut entries: Vec<fs::DirEntry> = match fs::read_dir(dir) {
        Ok(reader) => reader.filter_map(Result::ok).collect(),
        Err(_) => Vec::new(),
    };
    entries.sort_by_key(|e| e.file_name());
    entries
}

/// Whether a directory entry is a directory, following symlinks like
/// `directory_entry::is_directory`.
pub fn entry_is_dir(entry: &fs::DirEntry) -> bool {
    match entry.file_type() {
        Ok(t) if t.is_symlink() => entry.path().is_dir(),
        Ok(t) => t.is_dir(),
        Err(_) => false,
    }
}

pub fn entry_is_file(entry: &fs::DirEntry) -> bool {
    match entry.file_type() {
        Ok(t) if t.is_symlink() => is_regular_file(&entry.path()),
        Ok(t) => t.is_file(),
        Err(_) => false,
    }
}

/// `std::filesystem::relative(path, base)` for normalised absolute paths:
/// the remainder after `base`, or `None` when `path` is outside it.
pub fn relative_to(path: &Path, base: &Path) -> Option<PathBuf> {
    path.strip_prefix(base).ok().map(Path::to_path_buf)
}

/// Depth-first pre-order directory walk with the ability to skip recursion,
/// like `recursive_directory_iterator` with `disable_recursion_pending`.
/// `visit(path, is_dir, depth)` returns `false` to skip descending into a
/// directory. Symlinked directories are not followed (matching the default
/// `directory_options`). Entries are visited in name order.
pub fn walk<F>(root: &Path, mut visit: F) -> Result<(), crate::cancel::Cancelled>
where
    F: FnMut(&Path, bool, usize) -> Result<bool, crate::cancel::Cancelled>,
{
    fn recurse<F>(dir: &Path, depth: usize, visit: &mut F) -> Result<(), crate::cancel::Cancelled>
    where
        F: FnMut(&Path, bool, usize) -> Result<bool, crate::cancel::Cancelled>,
    {
        for entry in sorted_dir_entries(dir) {
            let path = entry.path();
            let file_type = match entry.file_type() {
                Ok(t) => t,
                Err(_) => continue,
            };
            let is_dir = if file_type.is_symlink() {
                path.is_dir()
            } else {
                file_type.is_dir()
            };
            let descend = visit(&path, is_dir, depth)?;
            if is_dir && descend && !file_type.is_symlink() {
                recurse(&path, depth + 1, visit)?;
            }
        }
        Ok(())
    }
    recurse(root, 0, &mut visit)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn shell_split_handles_quotes_and_escapes() {
        assert_eq!(
            split_shell_like(r#"cc -DNAME="a b" 'x y' a\ b"#),
            vec!["cc", "-DNAME=a b", "x y", "a b"]
        );
        assert!(split_shell_like("   ").is_empty());
    }

    #[test]
    fn lexical_normalisation() {
        assert_eq!(
            lexically_normal(Path::new("a/./b/../c")),
            PathBuf::from("a/c")
        );
        assert_eq!(
            lexically_normal(Path::new("/a/../../b")),
            PathBuf::from("/b")
        );
        assert_eq!(lexically_normal(Path::new("../a")), PathBuf::from("../a"));
    }

    #[test]
    fn prefix_is_component_wise() {
        assert!(path_has_prefix(Path::new("/a/b/c"), Path::new("/a/b")));
        assert!(!path_has_prefix(Path::new("/a/bc"), Path::new("/a/b")));
        assert!(is_nix_store_path(Path::new("/nix/store/x")));
        assert!(!is_nix_store_path(Path::new("nix/store/x")));
    }

    #[test]
    fn getlines_matches_getline() {
        assert_eq!(getlines("a\nb\n").collect::<Vec<_>>(), vec!["a", "b"]);
        assert_eq!(getlines("a\n\nb").collect::<Vec<_>>(), vec!["a", "", "b"]);
        assert_eq!(getlines("").count(), 0);
        assert_eq!(getlines("\n").collect::<Vec<_>>(), vec![""]);
    }

    #[test]
    fn weakly_canonical_handles_missing_tail() {
        let dir = tempfile::tempdir().unwrap();
        let root = fs::canonicalize(dir.path()).unwrap();
        let missing = root.join("x/../y/z");
        assert_eq!(weakly_canonical(&missing), root.join("y/z"));
    }
}
