//! Filesystem and text helpers shared by the resource loaders.

use std::fs;
use std::path::{Component, Path, PathBuf};

use serde_json::Value;

/// Lexically normalise a path: drop `.` components and fold `..` into the
/// preceding normal component (like `std::filesystem::path::lexically_normal`).
pub fn normalize(path: &Path) -> PathBuf {
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
    if out.is_empty() {
        return PathBuf::from(".");
    }
    out.iter().collect()
}

/// Normalised path rendered with forward slashes (the archived qt's
/// `generic_string`).
pub fn path_string(path: &Path) -> String {
    let text = normalize(path).to_string_lossy().into_owned();
    if cfg!(windows) {
        text.replace('\\', "/")
    } else {
        text
    }
}

/// `fs::canonicalize`, falling back to the lexically normalised path.
pub fn canonical_or_normal(path: &Path) -> PathBuf {
    fs::canonicalize(path).unwrap_or_else(|_| normalize(path))
}

/// Expand `~`, `~/x` and `~x` (relative to `$HOME`) as the archived qt did.
pub fn expand_tilde(root: &str) -> PathBuf {
    if let Some(rest) = root.strip_prefix('~') {
        let home = std::env::var_os("HOME")
            .filter(|h| !h.is_empty())
            .map(PathBuf::from)
            .or_else(dirs::home_dir);
        if let Some(home) = home {
            if rest.is_empty() {
                return home;
            }
            let suffix = rest.strip_prefix('/').unwrap_or(rest);
            return home.join(suffix);
        }
    }
    PathBuf::from(root)
}

/// Resolve a user-supplied path: tilde expansion, then relative to `base`.
pub fn resolve_user_path(value: &str, base: &Path) -> PathBuf {
    let path = expand_tilde(value);
    let joined = if path.is_relative() {
        base.join(path)
    } else {
        path
    };
    normalize(&joined)
}

/// Lexical relative path from `base` to `path` (both should be absolute).
pub fn relative_to(path: &Path, base: &Path) -> PathBuf {
    let path = normalize(path);
    let base = normalize(base);
    let p: Vec<Component<'_>> = path.components().collect();
    let b: Vec<Component<'_>> = base.components().collect();
    let common = p.iter().zip(&b).take_while(|(x, y)| x == y).count();
    let mut out = PathBuf::new();
    for _ in common..b.len() {
        out.push("..");
    }
    for component in &p[common..] {
        out.push(component.as_os_str());
    }
    out
}

pub fn generic(path: &Path) -> String {
    let text = path.to_string_lossy().into_owned();
    if cfg!(windows) {
        text.replace('\\', "/")
    } else {
        text
    }
}

pub fn is_markdown(path: &Path) -> bool {
    path.extension().is_some_and(|e| e == "md")
}

pub fn is_json(path: &Path) -> bool {
    path.extension().is_some_and(|e| e == "json")
}

pub fn file_name(path: &Path) -> String {
    path.file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default()
}

/// Regular files in `dir` accepted by `keep`, sorted by file name.
pub fn list_files(dir: &Path, keep: fn(&Path) -> bool) -> Vec<PathBuf> {
    let Ok(entries) = fs::read_dir(dir) else {
        return Vec::new();
    };
    let mut files: Vec<PathBuf> = entries
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| keep(p) && p.is_file())
        .collect();
    files.sort_by_key(|p| file_name(p));
    files
}

/// Subdirectories of `dir`, sorted by full path.
pub fn list_subdirectories(dir: &Path) -> Vec<PathBuf> {
    let Ok(entries) = fs::read_dir(dir) else {
        return Vec::new();
    };
    let mut dirs: Vec<PathBuf> = entries
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| p.is_dir())
        .collect();
    dirs.sort_by_key(|p| generic(p));
    dirs
}

/// Convert `\r\n` and lone `\r` to `\n`.
pub fn normalize_newlines(text: &str) -> String {
    text.replace("\r\n", "\n").replace('\r', "\n")
}

/// Trim ASCII whitespace (C `isspace`).
pub fn trim(text: &str) -> &str {
    text.trim_matches(|c: char| c.is_ascii_whitespace() || c == '\x0b')
}

/// Strip one pair of matching surrounding quotes.
pub fn unquote(value: &str) -> &str {
    let bytes = value.as_bytes();
    if bytes.len() >= 2
        && ((bytes[0] == b'"' && bytes[bytes.len() - 1] == b'"')
            || (bytes[0] == b'\'' && bytes[bytes.len() - 1] == b'\''))
    {
        &value[1..value.len() - 1]
    } else {
        value
    }
}

/// Entries of `pi.<key>` in a `package.json`. `Err` carries the diagnostic.
pub fn read_pi_manifest_entries(package_json: &Path, key: &str) -> Result<Vec<String>, String> {
    let package_path = path_string(package_json);
    let content = fs::read_to_string(package_json)
        .map_err(|_| format!("failed to read package manifest: {package_path}"))?;
    let json: Value = serde_json::from_str(&content)
        .map_err(|e| format!("failed to decode package manifest: {package_path} ({e})"))?;
    let Some(entries) = json.get("pi").and_then(|pi| pi.get(key)) else {
        return Ok(Vec::new());
    };
    let Some(list) = entries.as_array() else {
        return Ok(Vec::new());
    };
    list.iter()
        .map(|v| {
            v.as_str().map(str::to_string).ok_or_else(|| {
                format!(
                    "failed to decode package manifest: {package_path} (pi.{key} entries must be strings)"
                )
            })
        })
        .collect()
}

/// Resolve a package manifest entry relative to the package directory.
pub fn resolve_manifest_entry(directory: &Path, entry: &str) -> PathBuf {
    let path = Path::new(entry);
    if path.is_absolute() {
        path.to_path_buf()
    } else {
        directory.join(path)
    }
}

/// Truthy environment flag (`1`, `true`, `yes`, `on`, case-insensitive).
pub fn env_flag_enabled(name: &str) -> bool {
    std::env::var(name)
        .map(|v| matches!(v.to_ascii_lowercase().as_str(), "1" | "true" | "yes" | "on"))
        .unwrap_or(false)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn normalize_and_relative() {
        assert_eq!(normalize(Path::new("/a/./b/../c")), PathBuf::from("/a/c"));
        assert_eq!(
            relative_to(Path::new("/a/b/c"), Path::new("/a")),
            PathBuf::from("b/c")
        );
        assert_eq!(
            relative_to(Path::new("/a/x"), Path::new("/a/b")),
            PathBuf::from("../x")
        );
        assert_eq!(unquote("\"x\""), "x");
        assert_eq!(unquote("'x"), "'x");
        assert_eq!(normalize_newlines("a\r\nb\rc"), "a\nb\nc");
    }
}
