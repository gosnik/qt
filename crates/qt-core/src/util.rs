//! Small shared helpers: path resolution, output truncation, ids.

use std::path::{Path, PathBuf};

/// Maximum lines kept by tool output truncation (matches Pi).
pub const DEFAULT_MAX_LINES: usize = 2000;
/// Maximum bytes kept by tool output truncation (matches Pi).
pub const DEFAULT_MAX_BYTES: usize = 50 * 1024;

/// Expand a leading `~` and resolve `path` against `cwd`.
pub fn resolve_path(cwd: &Path, path: &str) -> PathBuf {
    let trimmed = path.trim();
    let expanded = if trimmed == "~" {
        dirs::home_dir().unwrap_or_else(|| PathBuf::from(trimmed))
    } else if let Some(rest) = trimmed.strip_prefix("~/") {
        dirs::home_dir()
            .map(|home| home.join(rest))
            .unwrap_or_else(|| PathBuf::from(trimmed))
    } else {
        PathBuf::from(trimmed)
    };
    if expanded.is_absolute() {
        expanded
    } else {
        cwd.join(expanded)
    }
}

/// Display `path` relative to `cwd` when it lives underneath it.
pub fn display_path(cwd: &Path, path: &Path) -> String {
    match path.strip_prefix(cwd) {
        Ok(rel) if !rel.as_os_str().is_empty() => rel.display().to_string(),
        _ => path.display().to_string(),
    }
}

/// Result of truncating a block of text.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Truncated {
    pub text: String,
    pub truncated: bool,
    /// Total number of lines in the input.
    pub total_lines: usize,
    /// Number of lines kept in `text`.
    pub kept_lines: usize,
}

/// Keep the first lines of `text` within the line and byte limits.
pub fn truncate_head(text: &str, max_lines: usize, max_bytes: usize) -> Truncated {
    let lines: Vec<&str> = split_lines(text);
    let total_lines = lines.len();
    let mut out = String::new();
    let mut kept = 0;
    for line in &lines {
        if kept >= max_lines {
            break;
        }
        let extra = line.len() + usize::from(kept > 0);
        if out.len() + extra > max_bytes {
            break;
        }
        if kept > 0 {
            out.push('\n');
        }
        out.push_str(line);
        kept += 1;
    }
    Truncated {
        truncated: kept < total_lines,
        text: out,
        total_lines,
        kept_lines: kept,
    }
}

/// Keep the last lines of `text` within the line and byte limits.
pub fn truncate_tail(text: &str, max_lines: usize, max_bytes: usize) -> Truncated {
    let lines: Vec<&str> = split_lines(text);
    let total_lines = lines.len();
    let mut kept: Vec<&str> = Vec::new();
    let mut bytes = 0;
    let mut cut_line = false;
    for line in lines.iter().rev() {
        if kept.len() >= max_lines {
            break;
        }
        let extra = line.len() + usize::from(!kept.is_empty());
        if bytes + extra > max_bytes {
            // A single oversized final line still yields its tail so the
            // caller never receives empty output for non-empty input.
            if kept.is_empty() {
                let start = floor_char_boundary(line, line.len().saturating_sub(max_bytes));
                kept.push(&line[start..]);
                cut_line = true;
            }
            break;
        }
        bytes += extra;
        kept.push(line);
    }
    kept.reverse();
    let kept_lines = kept.len();
    Truncated {
        text: kept.join("\n"),
        truncated: cut_line || kept_lines < total_lines,
        total_lines,
        kept_lines,
    }
}

fn split_lines(text: &str) -> Vec<&str> {
    if text.is_empty() {
        return Vec::new();
    }
    let trimmed = text.strip_suffix('\n').unwrap_or(text);
    trimmed
        .split('\n')
        .map(|l| l.strip_suffix('\r').unwrap_or(l))
        .collect()
}

/// Largest char boundary `<= index`.
pub fn floor_char_boundary(s: &str, index: usize) -> usize {
    if index >= s.len() {
        return s.len();
    }
    let mut i = index;
    while !s.is_char_boundary(i) {
        i -= 1;
    }
    i
}

/// Shorten a single line for display, appending an ellipsis.
pub fn shorten(s: &str, max_chars: usize) -> String {
    if s.chars().count() <= max_chars {
        return s.to_string();
    }
    let mut out: String = s.chars().take(max_chars.saturating_sub(1)).collect();
    out.push('…');
    out
}

/// Pi-style 8 hex character entry id.
pub fn short_id() -> String {
    format!("{:08x}", rand::random::<u32>())
}

pub fn human_bytes(bytes: usize) -> String {
    if bytes >= 1024 * 1024 {
        format!("{:.1}MB", bytes as f64 / (1024.0 * 1024.0))
    } else if bytes >= 1024 {
        format!("{:.1}KB", bytes as f64 / 1024.0)
    } else {
        format!("{bytes}B")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn head_truncates_by_lines_and_bytes() {
        let t = truncate_head("a\nb\nc\n", 2, 100);
        assert_eq!(t.text, "a\nb");
        assert!(t.truncated);
        assert_eq!(t.total_lines, 3);
        let t = truncate_head("aaaa\nbbbb", 10, 6);
        assert_eq!(t.text, "aaaa");
        assert!(t.truncated);
        let t = truncate_head("x\ny", 10, 100);
        assert!(!t.truncated);
    }

    #[test]
    fn tail_keeps_last_lines() {
        let t = truncate_tail("1\n2\n3\n4", 2, 100);
        assert_eq!(t.text, "3\n4");
        assert!(t.truncated);
        let t = truncate_tail(&"x".repeat(10), 5, 4);
        assert_eq!(t.text, "xxxx");
        assert!(t.truncated);
    }

    #[test]
    fn resolves_relative_paths() {
        let cwd = Path::new("/work");
        assert_eq!(
            resolve_path(cwd, "src/a.rs"),
            PathBuf::from("/work/src/a.rs")
        );
        assert_eq!(resolve_path(cwd, "/etc/x"), PathBuf::from("/etc/x"));
    }
}
