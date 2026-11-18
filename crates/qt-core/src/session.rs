//! Session persistence in Pi's v3 JSONL format.
//!
//! A session file starts with a header line
//! `{"type":"session","version":3,"id":…,"timestamp":…,"cwd":…}` followed by
//! entries linked through `id` / `parentId`. The conversation context is the
//! chain of `message` entries from the last entry back to the root; entries of
//! other types are preserved on disk but not replayed.

use std::fs::{self, OpenOptions};
use std::io::{BufRead as _, BufReader, Write as _};
use std::path::{Path, PathBuf};
use std::time::SystemTime;

use anyhow::{Context as _, Result};
use serde_json::{Value, json};

use crate::config::AgentHome;
use crate::message::{Message, UserContent};
use crate::util::short_id;

pub const SESSION_VERSION: u64 = 3;

#[derive(Debug, Clone)]
struct Entry {
    id: Option<String>,
    parent_id: Option<String>,
    message: Option<Message>,
}

/// An open session file.
#[derive(Debug)]
pub struct Session {
    path: PathBuf,
    id: String,
    cwd: PathBuf,
    entries: Vec<Entry>,
    leaf: Option<String>,
    header_written: bool,
}

impl Session {
    /// A new session; nothing is written until the first entry is appended.
    pub fn create(home: &AgentHome, cwd: &Path) -> Self {
        let micros = SystemTime::now()
            .duration_since(SystemTime::UNIX_EPOCH)
            .map(|d| d.as_micros())
            .unwrap_or_default();
        let id = format!("native-{micros}");
        Self {
            path: home.sessions_dir().join(format!("{id}.jsonl")),
            id,
            cwd: cwd.to_path_buf(),
            entries: Vec::new(),
            leaf: None,
            header_written: false,
        }
    }

    pub fn open(path: &Path) -> Result<Self> {
        let file = fs::File::open(path).with_context(|| format!("opening {}", path.display()))?;
        let mut id = path
            .file_stem()
            .and_then(|s| s.to_str())
            .unwrap_or_default()
            .to_string();
        let mut cwd = PathBuf::new();
        let mut entries = Vec::new();
        for (n, line) in BufReader::new(file).lines().enumerate() {
            let line = line?;
            if line.trim().is_empty() {
                continue;
            }
            let Ok(value) = serde_json::from_str::<Value>(&line) else {
                continue;
            };
            if n == 0 && value.get("type").and_then(Value::as_str) == Some("session") {
                if let Some(v) = value.get("id").and_then(Value::as_str) {
                    id = v.to_string();
                }
                if let Some(v) = value.get("cwd").and_then(Value::as_str) {
                    cwd = PathBuf::from(v);
                }
                continue;
            }
            let message = if value.get("type").and_then(Value::as_str) == Some("message") {
                value
                    .get("message")
                    .cloned()
                    .and_then(|m| serde_json::from_value::<Message>(m).ok())
            } else {
                None
            };
            entries.push(Entry {
                id: value.get("id").and_then(Value::as_str).map(str::to_string),
                parent_id: value
                    .get("parentId")
                    .and_then(Value::as_str)
                    .map(str::to_string),
                message,
            });
        }
        let leaf = entries.iter().rev().find_map(|e| e.id.clone());
        Ok(Self {
            path: path.to_path_buf(),
            id,
            cwd,
            entries,
            leaf,
            header_written: true,
        })
    }

    pub fn path(&self) -> &Path {
        &self.path
    }
    pub fn id(&self) -> &str {
        &self.id
    }
    pub fn cwd(&self) -> &Path {
        &self.cwd
    }

    /// Messages on the path from the root to the current leaf.
    pub fn context(&self) -> Vec<Message> {
        let mut chain = Vec::new();
        let mut cursor = self.leaf.clone();
        let mut guard = 0;
        while let Some(id) = cursor {
            guard += 1;
            if guard > self.entries.len() + 1 {
                break;
            }
            let Some(entry) = self
                .entries
                .iter()
                .rev()
                .find(|e| e.id.as_deref() == Some(&id))
            else {
                break;
            };
            if let Some(m) = &entry.message {
                chain.push(m.clone());
            }
            cursor = entry.parent_id.clone();
        }
        chain.reverse();
        chain
    }

    pub fn append(&mut self, message: &Message) -> Result<()> {
        let id = short_id();
        let entry = json!({
            "type": "message",
            "id": id,
            "parentId": self.leaf,
            "timestamp": chrono::Utc::now().to_rfc3339(),
            "message": message,
        });
        self.write_line(&entry)?;
        self.entries.push(Entry {
            id: Some(id.clone()),
            parent_id: self.leaf.clone(),
            message: Some(message.clone()),
        });
        self.leaf = Some(id);
        Ok(())
    }

    fn write_line(&mut self, value: &Value) -> Result<()> {
        if let Some(dir) = self.path.parent() {
            fs::create_dir_all(dir).with_context(|| format!("creating {}", dir.display()))?;
        }
        let mut file = OpenOptions::new()
            .create(true)
            .append(true)
            .open(&self.path)
            .with_context(|| format!("writing {}", self.path.display()))?;
        if !self.header_written {
            let header = json!({
                "type": "session",
                "version": SESSION_VERSION,
                "id": self.id,
                "timestamp": chrono::Utc::now().to_rfc3339(),
                "cwd": self.cwd,
            });
            writeln!(file, "{header}")?;
            self.header_written = true;
        }
        writeln!(file, "{value}")?;
        Ok(())
    }
}

/// Summary of a session file for pickers.
#[derive(Debug, Clone, PartialEq)]
pub struct SessionSummary {
    pub path: PathBuf,
    pub id: String,
    pub cwd: PathBuf,
    pub title: String,
    pub modified: SystemTime,
}

/// Sessions recorded for `cwd` (all sessions when `None`), newest first.
pub fn list_sessions(home: &AgentHome, cwd: Option<&Path>) -> Vec<SessionSummary> {
    let Ok(dir) = fs::read_dir(home.sessions_dir()) else {
        return Vec::new();
    };
    let mut out: Vec<SessionSummary> = dir
        .flatten()
        .filter(|e| e.path().extension().is_some_and(|x| x == "jsonl"))
        .filter_map(|e| summarize(&e.path()))
        .filter(|s| cwd.is_none_or(|c| s.cwd == c))
        .collect();
    out.sort_by_key(|s| std::cmp::Reverse(s.modified));
    out
}

fn summarize(path: &Path) -> Option<SessionSummary> {
    let modified = fs::metadata(path).and_then(|m| m.modified()).ok()?;
    let file = fs::File::open(path).ok()?;
    let mut lines = BufReader::new(file).lines();
    let header: Value = serde_json::from_str(&lines.next()?.ok()?).ok()?;
    if header.get("type").and_then(Value::as_str) != Some("session") {
        return None;
    }
    let mut title = String::new();
    for line in lines.take(200).map_while(Result::ok) {
        let Ok(v) = serde_json::from_str::<Value>(&line) else {
            continue;
        };
        if v.pointer("/message/role").and_then(Value::as_str) == Some("user")
            && let Ok(content) =
                serde_json::from_value::<UserContent>(v["message"]["content"].clone())
        {
            title = content
                .plain_text()
                .split_whitespace()
                .collect::<Vec<_>>()
                .join(" ");
            if !title.is_empty() {
                break;
            }
        }
    }
    Some(SessionSummary {
        path: path.to_path_buf(),
        id: header
            .get("id")
            .and_then(Value::as_str)
            .unwrap_or_default()
            .to_string(),
        cwd: PathBuf::from(
            header
                .get("cwd")
                .and_then(Value::as_str)
                .unwrap_or_default(),
        ),
        title: if title.is_empty() {
            "(no messages)".into()
        } else {
            crate::util::shorten(&title, 80)
        },
        modified,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::message::{AssistantMessage, ContentBlock};

    #[test]
    fn writes_header_lazily_and_round_trips() {
        let dir = tempfile::tempdir().unwrap();
        let home = AgentHome::new(dir.path());
        let cwd = dir.path().join("proj");
        let mut s = Session::create(&home, &cwd);
        assert!(!s.path().exists());
        s.append(&Message::user_text("first   question\nhere"))
            .unwrap();
        s.append(&Message::Assistant(AssistantMessage {
            content: vec![ContentBlock::text("answer")],
            ..Default::default()
        }))
        .unwrap();
        let reopened = Session::open(s.path()).unwrap();
        assert_eq!(reopened.id(), s.id());
        assert_eq!(reopened.cwd(), cwd);
        assert_eq!(reopened.context().len(), 2);

        let listed = list_sessions(&home, Some(&cwd));
        assert_eq!(listed.len(), 1);
        assert_eq!(listed[0].title, "first question here");
        assert!(list_sessions(&home, Some(Path::new("/elsewhere"))).is_empty());
    }

    #[test]
    fn context_follows_parent_chain_and_skips_other_entries() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("s.jsonl");
        std::fs::write(
            &path,
            concat!(
                r#"{"type":"session","version":3,"id":"s1","timestamp":"t","cwd":"/p"}"#, "\n",
                r#"{"type":"message","id":"a","parentId":null,"message":{"role":"user","content":"one"}}"#, "\n",
                r#"{"type":"message","id":"b","parentId":"a","message":{"role":"user","content":"abandoned branch"}}"#, "\n",
                r#"{"type":"model_change","id":"c","parentId":"a","provider":"x"}"#, "\n",
                r#"{"type":"message","id":"d","parentId":"c","message":{"role":"user","content":"two"}}"#, "\n",
            ),
        )
        .unwrap();
        let s = Session::open(&path).unwrap();
        let texts: Vec<String> = s
            .context()
            .iter()
            .map(|m| match m {
                Message::User { content, .. } => content.plain_text(),
                _ => String::new(),
            })
            .collect();
        assert_eq!(texts, ["one", "two"]);
    }
}
