//! Prompt history (`<agent home>/history.jsonl`, archived
//! `app/config/prompt_history.cpp`).
//!
//! One `{"prompt": "..."}` object per line, oldest first. Prompts are
//! trimmed, consecutive duplicates collapse, at most
//! `terminal.promptHistoryLimit` entries (default 500) and 8 MiB of prompt
//! text are kept. Every access holds an advisory lock on
//! `history.jsonl.lock`; rewrites go through a temporary file that is
//! fsynced and renamed into place, and both files are owner-only. A limit
//! of 0 deletes the history.

use std::collections::VecDeque;
use std::fs;
use std::io::Write as _;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

use anyhow::{Context as _, Result, anyhow, bail};
use serde_json::{Value, json};

use crate::config::AgentHome;

pub const DEFAULT_PROMPT_HISTORY_LIMIT: usize = 500;
pub const MAXIMUM_PROMPT_HISTORY_BYTES: usize = 8 * 1024 * 1024;
pub const PROMPT_HISTORY_FILENAME: &str = "history.jsonl";

/// Prompts loaded from disk, newest first.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct PromptHistorySnapshot {
    pub prompts: Vec<String>,
    pub file_exists: bool,
}

/// The persistent prompt history of an agent home.
#[derive(Debug, Clone)]
pub struct PromptHistoryStore {
    path: PathBuf,
    max_entries: usize,
}

struct ParsedHistory {
    oldest_first: VecDeque<String>,
    file_exists: bool,
    needs_rewrite: bool,
}

/// Exclusive advisory lock held for the duration of one operation.
struct HistoryLock {
    #[cfg(unix)]
    file: fs::File,
}

impl HistoryLock {
    fn acquire(history: &Path) -> Result<Self> {
        let dir = history.parent().context("history path has no parent")?;
        fs::create_dir_all(dir).with_context(|| format!("creating {}", dir.display()))?;
        #[cfg(unix)]
        {
            use std::os::unix::fs::{OpenOptionsExt as _, PermissionsExt as _};
            use std::os::unix::io::AsRawFd as _;
            let mut lock_path = history.as_os_str().to_owned();
            lock_path.push(".lock");
            let lock_path = PathBuf::from(lock_path);
            let file = fs::OpenOptions::new()
                .create(true)
                .truncate(false)
                .read(true)
                .write(true)
                .mode(0o600)
                .open(&lock_path)
                .with_context(|| {
                    format!("unable to open prompt history lock {}", lock_path.display())
                })?;
            fs::set_permissions(&lock_path, fs::Permissions::from_mode(0o600)).with_context(
                || {
                    format!(
                        "unable to secure prompt history lock {}",
                        lock_path.display()
                    )
                },
            )?;
            // SAFETY: `flock` on a valid open descriptor.
            if unsafe { libc::flock(file.as_raw_fd(), libc::LOCK_EX) } != 0 {
                return Err(anyhow!(
                    "unable to lock prompt history {}: {}",
                    history.display(),
                    std::io::Error::last_os_error()
                ));
            }
            Ok(Self { file })
        }
        #[cfg(not(unix))]
        {
            Ok(Self {})
        }
    }
}

impl Drop for HistoryLock {
    fn drop(&mut self) {
        #[cfg(unix)]
        {
            use std::os::unix::io::AsRawFd as _;
            // SAFETY: unlocking the descriptor this guard locked.
            unsafe {
                libc::flock(self.file.as_raw_fd(), libc::LOCK_UN);
            }
        }
    }
}

fn secure(path: &Path) -> Result<()> {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt as _;
        fs::set_permissions(path, fs::Permissions::from_mode(0o600))
            .with_context(|| format!("unable to secure prompt history {}", path.display()))?;
    }
    #[cfg(not(unix))]
    let _ = path;
    Ok(())
}

fn read_history(path: &Path, max_entries: usize) -> Result<ParsedHistory> {
    let content = match fs::read_to_string(path) {
        Ok(c) => c,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
            return Ok(ParsedHistory {
                oldest_first: VecDeque::new(),
                file_exists: false,
                needs_rewrite: false,
            });
        }
        Err(e) => return Err(e).with_context(|| path.display().to_string()),
    };
    let mut parsed = ParsedHistory {
        oldest_first: VecDeque::new(),
        file_exists: true,
        needs_rewrite: false,
    };
    let mut valid = 0usize;
    let mut retained_bytes = 0usize;
    for (index, line) in content.split('\n').enumerate() {
        let line = line.strip_suffix('\r').unwrap_or(line);
        if line.trim().is_empty() {
            continue;
        }
        let number = index + 1;
        let record: Value = serde_json::from_str(line).map_err(|e| {
            anyhow!(
                "invalid prompt history JSON at {}:{number}: {e}",
                path.display()
            )
        })?;
        let Some(raw) = record.get("prompt").and_then(Value::as_str) else {
            bail!(
                "invalid prompt history record at {}:{number}",
                path.display()
            );
        };
        let prompt = raw.trim();
        if prompt.is_empty() {
            parsed.needs_rewrite = true;
            continue;
        }
        if prompt.len() > MAXIMUM_PROMPT_HISTORY_BYTES {
            bail!(
                "prompt history entry exceeds the retained byte limit at {}:{number}",
                path.display()
            );
        }
        parsed.needs_rewrite |= prompt != raw;
        valid += 1;
        if max_entries > 0 {
            parsed.oldest_first.push_back(prompt.to_string());
            retained_bytes += prompt.len();
            if parsed.oldest_first.len() > max_entries {
                retained_bytes -= parsed.oldest_first.pop_front().map_or(0, |p| p.len());
            }
            while retained_bytes > MAXIMUM_PROMPT_HISTORY_BYTES {
                let Some(old) = parsed.oldest_first.pop_front() else {
                    break;
                };
                retained_bytes -= old.len();
                parsed.needs_rewrite = true;
            }
        }
    }
    parsed.needs_rewrite |= valid > max_entries;
    Ok(parsed)
}

fn write_history(path: &Path, oldest_first: &VecDeque<String>) -> Result<()> {
    static SEQUENCE: AtomicU64 = AtomicU64::new(0);
    let mut content = String::new();
    for prompt in oldest_first {
        content.push_str(&json!({ "prompt": prompt }).to_string());
        content.push('\n');
    }
    let mut tmp = path.as_os_str().to_owned();
    tmp.push(format!(
        ".tmp.{}.{}",
        std::process::id(),
        SEQUENCE.fetch_add(1, Ordering::Relaxed)
    ));
    let tmp = PathBuf::from(tmp);
    let result = (|| -> Result<()> {
        let mut options = fs::OpenOptions::new();
        options.write(true).create_new(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt as _;
            options.mode(0o600);
        }
        let mut file = options.open(&tmp).with_context(|| {
            format!(
                "unable to create prompt history temporary file {}",
                tmp.display()
            )
        })?;
        file.write_all(content.as_bytes()).with_context(|| {
            format!(
                "unable to write prompt history temporary file {}",
                tmp.display()
            )
        })?;
        file.sync_all().with_context(|| {
            format!(
                "unable to sync prompt history temporary file {}",
                tmp.display()
            )
        })?;
        drop(file);
        secure(&tmp)?;
        #[cfg(windows)]
        let _ = fs::remove_file(path);
        fs::rename(&tmp, path).with_context(|| path.display().to_string())?;
        secure(path)
    })();
    if result.is_err() {
        let _ = fs::remove_file(&tmp);
    }
    result
}

fn remove_history(path: &Path) -> Result<()> {
    match fs::remove_file(path) {
        Err(e) if e.kind() != std::io::ErrorKind::NotFound => {
            Err(e).with_context(|| path.display().to_string())
        }
        _ => Ok(()),
    }
}

impl PromptHistoryStore {
    pub fn new(home: &AgentHome, max_entries: usize) -> Self {
        Self {
            path: home.root().join(PROMPT_HISTORY_FILENAME),
            max_entries,
        }
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    pub fn max_entries(&self) -> usize {
        self.max_entries
    }

    /// Load the history (newest first), compacting the file when it holds
    /// blank, untrimmed or excess entries.
    pub fn load(&self) -> Result<PromptHistorySnapshot> {
        let _lock = HistoryLock::acquire(&self.path)?;
        if self.max_entries == 0 {
            let existed = self.path.exists();
            remove_history(&self.path)?;
            return Ok(PromptHistorySnapshot {
                prompts: Vec::new(),
                file_exists: existed,
            });
        }
        let parsed = read_history(&self.path, self.max_entries)?;
        if parsed.needs_rewrite {
            write_history(&self.path, &parsed.oldest_first)?;
        } else if parsed.file_exists {
            secure(&self.path)?;
        }
        Ok(PromptHistorySnapshot {
            prompts: parsed.oldest_first.into_iter().rev().collect(),
            file_exists: parsed.file_exists,
        })
    }

    /// Append one prompt (trimmed; skipped when empty or equal to the
    /// newest entry).
    pub fn append(&self, prompt: &str) -> Result<()> {
        let _lock = HistoryLock::acquire(&self.path)?;
        if self.max_entries == 0 {
            return remove_history(&self.path);
        }
        let mut entries = read_history(&self.path, self.max_entries)?.oldest_first;
        let normalized = prompt.trim();
        if normalized.len() > MAXIMUM_PROMPT_HISTORY_BYTES {
            bail!("prompt history entry exceeds the retained byte limit");
        }
        if !normalized.is_empty() && entries.back().map(String::as_str) != Some(normalized) {
            entries.push_back(normalized.to_string());
        }
        while entries.len() > self.max_entries {
            entries.pop_front();
        }
        let mut bytes: usize = entries.iter().map(String::len).sum();
        while bytes > MAXIMUM_PROMPT_HISTORY_BYTES {
            let Some(old) = entries.pop_front() else {
                break;
            };
            bytes -= old.len();
        }
        write_history(&self.path, &entries)
    }

    /// Replace the history with `newest_first` (normalised like appends).
    pub fn save(&self, newest_first: &[String]) -> Result<()> {
        let _lock = HistoryLock::acquire(&self.path)?;
        if self.max_entries == 0 {
            return remove_history(&self.path);
        }
        let mut normalized: Vec<String> = Vec::new();
        let mut bytes = 0usize;
        for candidate in newest_first {
            let prompt = candidate.trim();
            if prompt.is_empty() || normalized.last().map(String::as_str) == Some(prompt) {
                continue;
            }
            if prompt.len() > MAXIMUM_PROMPT_HISTORY_BYTES {
                bail!("prompt history entry exceeds the retained byte limit");
            }
            if bytes > MAXIMUM_PROMPT_HISTORY_BYTES - prompt.len() {
                continue;
            }
            bytes += prompt.len();
            normalized.push(prompt.to_string());
            if normalized.len() >= self.max_entries {
                break;
            }
        }
        let oldest_first: VecDeque<String> = normalized.into_iter().rev().collect();
        if oldest_first.is_empty() {
            return remove_history(&self.path);
        }
        write_history(&self.path, &oldest_first)
    }

    /// Delete the history.
    pub fn clear(&self) -> Result<()> {
        let _lock = HistoryLock::acquire(&self.path)?;
        remove_history(&self.path)
    }
}

/// A session's prompt history (header `promptHistory`, newest first) after
/// submitting `prompt`: trimmed, consecutive duplicates collapsed, at most
/// `limit` entries and [`MAXIMUM_PROMPT_HISTORY_BYTES`] of text.
pub fn push_session_prompt(history: &[String], prompt: &str, limit: usize) -> Vec<String> {
    if limit == 0 {
        return Vec::new();
    }
    let prompt = prompt.trim();
    let mut next: Vec<String> = Vec::with_capacity(history.len() + 1);
    if !prompt.is_empty() && history.first().map(String::as_str) != Some(prompt) {
        next.push(prompt.to_string());
    }
    let mut bytes = next.first().map_or(0, String::len);
    for entry in history {
        if next.len() >= limit || bytes + entry.len() > MAXIMUM_PROMPT_HISTORY_BYTES {
            break;
        }
        bytes += entry.len();
        next.push(entry.clone());
    }
    next
}

/// Up/Down navigation over a newest-first history list, keeping the
/// unsent draft.
#[derive(Debug, Clone, Default)]
pub struct HistoryCursor {
    entries: Vec<String>,
    position: Option<usize>,
    draft: String,
}

impl HistoryCursor {
    pub fn new(newest_first: Vec<String>) -> Self {
        Self {
            entries: newest_first,
            position: None,
            draft: String::new(),
        }
    }

    /// Record a submitted prompt as the newest entry and reset navigation.
    pub fn push(&mut self, prompt: &str) {
        let prompt = prompt.trim();
        if !prompt.is_empty() && self.entries.first().map(String::as_str) != Some(prompt) {
            self.entries.insert(0, prompt.to_string());
        }
        self.reset();
    }

    pub fn reset(&mut self) {
        self.position = None;
        self.draft.clear();
    }

    pub fn entries(&self) -> &[String] {
        &self.entries
    }

    /// Older entry (Up). `current` is the editor text, saved as the draft
    /// when navigation starts.
    pub fn older(&mut self, current: &str) -> Option<&str> {
        let next = match self.position {
            None => {
                self.draft = current.to_string();
                0
            }
            Some(p) => p + 1,
        };
        if next >= self.entries.len() {
            return None;
        }
        self.position = Some(next);
        Some(&self.entries[next])
    }

    /// Newer entry (Down); past the newest entry the draft comes back.
    pub fn newer(&mut self) -> Option<&str> {
        match self.position {
            None => None,
            Some(0) => {
                self.position = None;
                Some(&self.draft)
            }
            Some(p) => {
                self.position = Some(p - 1);
                Some(&self.entries[p - 1])
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn session_prompt_history_pushes_newest_first_within_limits() {
        let h = |v: &[&str]| v.iter().map(|s| s.to_string()).collect::<Vec<_>>();
        assert_eq!(push_session_prompt(&[], " /name x ", 3), h(&["/name x"]));
        assert_eq!(push_session_prompt(&h(&["a"]), "a", 3), h(&["a"]));
        assert_eq!(push_session_prompt(&h(&["a"]), "  ", 3), h(&["a"]));
        assert_eq!(
            push_session_prompt(&h(&["c", "b", "a"]), "!ls", 3),
            h(&["!ls", "c", "b"])
        );
        assert!(push_session_prompt(&h(&["a"]), "b", 0).is_empty());
        let big = "x".repeat(MAXIMUM_PROMPT_HISTORY_BYTES - 1);
        assert_eq!(push_session_prompt(&[big], "yy", 10), h(&["yy"]));
    }

    #[test]
    fn append_load_trim_dedupe_and_limit() {
        let dir = tempfile::tempdir().unwrap();
        let home = AgentHome::new(dir.path());
        let store = PromptHistoryStore::new(&home, 3);
        assert_eq!(store.load().unwrap(), PromptHistorySnapshot::default());
        for p in ["  one ", "two", "two", "", "three", "four"] {
            store.append(p).unwrap();
        }
        let snap = store.load().unwrap();
        assert_eq!(snap.prompts, ["four", "three", "two"]);
        assert!(snap.file_exists);
        let text = fs::read_to_string(store.path()).unwrap();
        assert_eq!(
            text,
            "{\"prompt\":\"two\"}\n{\"prompt\":\"three\"}\n{\"prompt\":\"four\"}\n"
        );
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt as _;
            let mode = fs::metadata(store.path()).unwrap().permissions().mode();
            assert_eq!(mode & 0o777, 0o600);
        }
        // A smaller limit compacts the file on load.
        let small = PromptHistoryStore::new(&home, 1);
        assert_eq!(small.load().unwrap().prompts, ["four"]);
        assert_eq!(fs::read_to_string(store.path()).unwrap().lines().count(), 1);
        small.save(&["b".into(), " ".into(), "a".into()]).unwrap();
        assert_eq!(small.load().unwrap().prompts, ["b"]);
        // Limit 0 deletes the file.
        let none = PromptHistoryStore::new(&home, 0);
        assert!(none.load().unwrap().file_exists);
        assert!(!store.path().exists());
    }

    #[test]
    fn invalid_records_are_reported_with_line_numbers() {
        let dir = tempfile::tempdir().unwrap();
        let home = AgentHome::new(dir.path());
        let store = PromptHistoryStore::new(&home, 10);
        fs::write(store.path(), "{\"prompt\":\"ok\"}\n{\"nope\":1}\n").unwrap();
        let err = store.load().unwrap_err().to_string();
        assert!(
            err.starts_with("invalid prompt history record at "),
            "{err}"
        );
        assert!(err.ends_with(":2"), "{err}");
        fs::write(store.path(), "not json\n").unwrap();
        assert!(
            store
                .load()
                .unwrap_err()
                .to_string()
                .starts_with("invalid prompt history JSON at ")
        );
    }

    #[test]
    fn cursor_walks_history_and_restores_draft() {
        let mut cursor = HistoryCursor::new(vec!["b".into(), "a".into()]);
        assert_eq!(cursor.newer(), None);
        assert_eq!(cursor.older("draft"), Some("b"));
        assert_eq!(cursor.older("ignored"), Some("a"));
        assert_eq!(cursor.older(""), None);
        assert_eq!(cursor.newer(), Some("b"));
        assert_eq!(cursor.newer(), Some("draft"));
        cursor.push("c");
        assert_eq!(cursor.entries(), ["c", "b", "a"]);
    }
}
