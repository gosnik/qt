//! Sliding read-back over a session file's current branch.
//!
//! A front end that shows a long conversation keeps only a window of recent
//! entries in memory and reads older ones back from the session JSONL file as
//! the user scrolls up. [`BranchLog::open`] scans the file once, recording for
//! each entry on the branch (root → leaf) the byte span of its line and
//! whether it starts a turn (a user message). [`BranchLog::read`] then parses
//! only the requested entries. Because it walks the branch rather than the
//! replayed context, history from before a compaction stays readable, with the
//! compaction entry in its chronological place.

use std::collections::HashMap;
use std::fs::File;
use std::io::{BufRead, BufReader, Read, Seek, SeekFrom};
use std::ops::Range;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, bail};
use serde::Deserialize;
use serde_json::Value;

use crate::session::SessionEntry;

/// Byte span of one branch entry's line in the session file.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct Span {
    offset: u64,
    len: u32,
    user: bool,
}

/// The current branch of a session file, read back on demand.
#[derive(Debug, Clone)]
pub struct BranchLog {
    path: PathBuf,
    spans: Vec<Span>,
}

#[derive(Deserialize)]
struct Role {
    role: Option<String>,
}

#[derive(Deserialize)]
struct Head {
    #[serde(rename = "type")]
    kind: Option<String>,
    id: Option<String>,
    #[serde(rename = "parentId")]
    parent_id: Option<String>,
    version: Option<u64>,
    message: Option<Role>,
}

impl BranchLog {
    /// Index the branch ending at `leaf` (the last entry when `None`).
    /// Fails for files older than session format v3, which have no entry
    /// ids; callers fall back to the in-memory session then.
    pub fn open(path: &Path, leaf: Option<&str>) -> Result<Self> {
        let file = File::open(path).with_context(|| format!("opening {}", path.display()))?;
        let mut reader = BufReader::new(file);
        let mut by_id: HashMap<String, (Span, Option<String>)> = HashMap::new();
        let mut last: Option<String> = None;
        let mut offset = 0u64;
        let mut line = String::new();
        let mut first = true;
        loop {
            line.clear();
            let read = reader.read_line(&mut line)?;
            if read == 0 {
                break;
            }
            let start = offset;
            offset += read as u64;
            let trimmed = line.trim_end_matches(['\n', '\r']);
            if trimmed.trim().is_empty() {
                continue;
            }
            let Ok(head) = serde_json::from_str::<Head>(trimmed) else {
                continue;
            };
            if first {
                first = false;
                if head.kind.as_deref() != Some("session") {
                    bail!("{} is not a session file", path.display());
                }
                if head.version.unwrap_or(1) < 3 {
                    bail!("{} predates session format v3", path.display());
                }
                continue;
            }
            if head.kind.as_deref() == Some("session") {
                continue;
            }
            let Some(id) = head.id else { continue };
            let user = head.kind.as_deref() == Some("message")
                && head.message.and_then(|m| m.role).as_deref() == Some("user");
            let span = Span {
                offset: start,
                len: trimmed.len() as u32,
                user,
            };
            last = Some(id.clone());
            by_id.insert(id, (span, head.parent_id));
        }
        let mut spans = Vec::new();
        let mut cursor = leaf.map(str::to_string).or(last);
        while let Some(id) = cursor {
            let Some((span, parent)) = by_id.remove(&id) else {
                break;
            };
            spans.push(span);
            cursor = parent;
        }
        spans.reverse();
        Ok(Self {
            path: path.to_path_buf(),
            spans,
        })
    }

    /// Number of entries on the branch.
    pub fn len(&self) -> usize {
        self.spans.len()
    }

    pub fn is_empty(&self) -> bool {
        self.spans.is_empty()
    }

    /// Start index for a chunk of about `want` entries ending before `end`,
    /// moved back to the nearest user message so a turn is never split
    /// (0 when there is none).
    pub fn chunk_start(&self, end: usize, want: usize) -> usize {
        let end = end.min(self.spans.len());
        let mut start = end.saturating_sub(want.max(1));
        while start > 0 && !self.spans[start].user {
            start -= 1;
        }
        start
    }

    /// End index for a chunk of about `want` entries starting at `start`,
    /// moved forward to the next user message so a turn is never split
    /// (the branch length when there is none).
    pub fn chunk_end(&self, start: usize, want: usize) -> usize {
        let len = self.spans.len();
        let mut end = start.saturating_add(want.max(1)).min(len);
        while end < len && !self.spans[end].user {
            end += 1;
        }
        end
    }

    /// Parse the entries in `range` from the file. Lines that no longer
    /// parse are skipped.
    pub fn read(&self, range: Range<usize>) -> Result<Vec<SessionEntry>> {
        let range = range.start.min(self.spans.len())..range.end.min(self.spans.len());
        let mut file =
            File::open(&self.path).with_context(|| format!("opening {}", self.path.display()))?;
        let mut out = Vec::with_capacity(range.len());
        let mut buf = Vec::new();
        for span in &self.spans[range] {
            file.seek(SeekFrom::Start(span.offset))?;
            buf.resize(span.len as usize, 0);
            file.read_exact(&mut buf)?;
            let Ok(value) = serde_json::from_slice::<Value>(&buf) else {
                continue;
            };
            if let Ok(entry) = SessionEntry::from_value(value) {
                out.push(entry);
            }
        }
        Ok(out)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    use std::io::Write;

    fn write(lines: &[Value]) -> tempfile::NamedTempFile {
        let mut f = tempfile::NamedTempFile::new().unwrap();
        for l in lines {
            writeln!(f, "{l}").unwrap();
        }
        f
    }

    fn msg(id: &str, parent: Option<&str>, role: &str, text: &str) -> Value {
        let message = if role == "user" {
            json!({"role": "user", "content": text, "timestamp": 1})
        } else {
            json!({"role": "assistant", "content": [{"type": "text", "text": text}],
                   "api": "x", "provider": "p", "model": "m",
                   "usage": {"input": 0, "output": 0, "cacheRead": 0, "cacheWrite": 0,
                             "totalTokens": 0, "cost": {"input": 0, "output": 0,
                             "cacheRead": 0, "cacheWrite": 0, "total": 0}},
                   "stopReason": "stop", "timestamp": 1})
        };
        json!({"type": "message", "id": id, "parentId": parent,
               "timestamp": "2026-01-01T00:00:00.000Z", "message": message})
    }

    fn header() -> Value {
        json!({"type": "session", "version": 3, "id": "s", "timestamp": "2026-01-01T00:00:00.000Z", "cwd": "/"})
    }

    #[test]
    fn walks_the_current_branch_and_reads_chunks_back() {
        // a(u) b(a) c(u) d(a) and a side branch c2 from b; leaf d.
        let f = write(&[
            header(),
            msg("a", None, "user", "one"),
            msg("b", Some("a"), "assistant", "two"),
            msg("c2", Some("b"), "user", "side"),
            msg("c", Some("b"), "user", "three"),
            msg("d", Some("c"), "assistant", "four"),
        ]);
        let log = BranchLog::open(f.path(), Some("d")).unwrap();
        assert_eq!(log.len(), 4);
        let ids: Vec<String> = log.read(0..4).unwrap().into_iter().map(|e| e.id).collect();
        assert_eq!(ids, ["a", "b", "c", "d"]);
        // A chunk ending before `d` that would start at `b` snaps back to
        // the user message `a`; one starting at `c` is kept.
        assert_eq!(log.chunk_start(3, 2), 0);
        assert_eq!(log.chunk_start(4, 2), 2);
        assert_eq!(log.chunk_end(0, 1), 2);
        assert_eq!(log.chunk_end(2, 1), 4);
        let tail: Vec<String> = log.read(2..4).unwrap().into_iter().map(|e| e.id).collect();
        assert_eq!(tail, ["c", "d"]);
        // Without a leaf the last entry in the file is the leaf.
        let log = BranchLog::open(f.path(), None).unwrap();
        assert_eq!(log.len(), 4);
    }

    #[test]
    fn old_formats_are_refused() {
        let f = write(&[json!({"type": "session", "id": "s", "timestamp": "t", "cwd": "/"})]);
        assert!(BranchLog::open(f.path(), None).is_err());
    }
}
