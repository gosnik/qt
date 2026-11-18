//! Session persistence in Pi's v3 JSONL format.
//!
//! A session file starts with a header line
//! `{"type":"session","version":3,"id":…,"timestamp":…,"cwd":…,"parentSession":…}`
//! followed by entries linked through `id` / `parentId` into a tree. The
//! *leaf* is the current position; appending creates a child of the leaf and
//! branching moves the leaf to an earlier entry. The conversation context is
//! built from the path root → leaf, replaying compaction and branch summaries
//! (Pi `buildSessionContext`).
//!
//! Files written by Pi v1/v2 are migrated to v3 on open and rewritten. Like
//! Pi, nothing is written to disk until the session holds an assistant
//! message; the whole file is then written and later entries are appended.
//!
//! File naming follows the archived qt: sessions live directly in the session
//! directory as `<id>.jsonl` with ids `native-<micros>`, `fork-<parent>` and
//! `clone-<parent>`. Listing scans the directory recursively, so Pi's per-cwd
//! sub-directories are found too.

use std::collections::{HashMap, HashSet};
use std::fs;
use std::io::Write as _;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

use anyhow::{Context as _, Result, anyhow, bail};
use serde_json::{Map, Value, json};

use crate::config::{AgentHome, Settings};
use crate::message::{
    AgentMessage, BranchSummaryMessage, CompactionSummaryMessage, CustomMessage, Message,
    UserContent,
};
use crate::provider::ModelRef;

pub const SESSION_VERSION: u64 = 3;
/// Environment variable naming the session directory (below `--session-dir`).
pub const SESSION_DIR_ENV: &str = "PI_CODING_AGENT_SESSION_DIR";

/// First line of a session file.
#[derive(Debug, Clone, PartialEq)]
pub struct SessionHeader {
    pub version: u64,
    pub id: String,
    pub timestamp: String,
    pub cwd: String,
    /// Path of the session this one was forked or cloned from.
    pub parent_session: Option<String>,
    /// Other header fields (for example a persistent worker's `subagent`
    /// lineage), kept verbatim when the file is rewritten.
    pub extra: serde_json::Map<String, Value>,
}

impl SessionHeader {
    fn new(id: String, cwd: &Path, parent_session: Option<String>) -> Self {
        Self {
            version: SESSION_VERSION,
            id,
            timestamp: now_iso(),
            cwd: cwd.to_string_lossy().into_owned(),
            parent_session,
            extra: serde_json::Map::new(),
        }
    }

    pub fn to_value(&self) -> Value {
        let mut v = json!({
            "type": "session",
            "version": self.version,
            "id": self.id,
            "timestamp": self.timestamp,
            "cwd": self.cwd,
        });
        if let Some(parent) = &self.parent_session {
            v["parentSession"] = json!(parent);
        }
        if let Some(object) = v.as_object_mut() {
            for (key, value) in &self.extra {
                object.entry(key.clone()).or_insert_with(|| value.clone());
            }
        }
        v
    }

    fn from_value(v: &Value) -> Option<Self> {
        if v.get("type").and_then(Value::as_str) != Some("session") {
            return None;
        }
        Some(Self {
            version: v.get("version").and_then(Value::as_u64).unwrap_or(1),
            id: v.get("id").and_then(Value::as_str)?.to_string(),
            timestamp: str_field(v, "timestamp").unwrap_or_default(),
            cwd: str_field(v, "cwd").unwrap_or_default(),
            parent_session: str_field(v, "parentSession"),
            extra: v
                .as_object()
                .map(|object| {
                    object
                        .iter()
                        .filter(|(key, _)| {
                            !matches!(
                                key.as_str(),
                                "type" | "version" | "id" | "timestamp" | "cwd" | "parentSession"
                            )
                        })
                        .map(|(key, value)| (key.clone(), value.clone()))
                        .collect()
                })
                .unwrap_or_default(),
        })
    }
}

/// A `compaction` entry: everything before `first_kept_entry_id` is replaced
/// by `summary` in the replayed context.
#[derive(Debug, Clone, PartialEq)]
pub struct CompactionEntry {
    pub summary: String,
    pub first_kept_entry_id: String,
    pub tokens_before: u64,
    /// `{readFiles, modifiedFiles}` for qt/Pi compactions; free-form for hooks.
    pub details: Option<Value>,
    /// True when produced by a hook/extension instead of the built-in summarizer.
    pub from_hook: Option<bool>,
}

/// A `branch_summary` entry: a summary of an abandoned branch.
#[derive(Debug, Clone, PartialEq)]
pub struct BranchSummaryEntry {
    pub from_id: String,
    pub summary: String,
    pub details: Option<Value>,
    pub from_hook: Option<bool>,
}

/// A `custom_message` entry: extension content that reaches the model.
#[derive(Debug, Clone, PartialEq)]
pub struct CustomMessageEntry {
    pub custom_type: String,
    pub content: UserContent,
    pub details: Option<Value>,
    pub display: bool,
}

/// The typed payload of a session entry.
#[derive(Debug, Clone, PartialEq)]
pub enum EntryKind {
    Message(AgentMessage),
    ThinkingLevelChange {
        thinking_level: String,
    },
    ModelChange {
        provider: String,
        model_id: String,
    },
    Compaction(CompactionEntry),
    BranchSummary(BranchSummaryEntry),
    /// Extension state; never part of the model context.
    Custom {
        custom_type: String,
        data: Option<Value>,
    },
    CustomMessage(CustomMessageEntry),
    Label {
        target_id: String,
        label: Option<String>,
    },
    SessionInfo {
        name: Option<String>,
    },
    /// An entry type this version does not know (or one that failed
    /// validation); kept verbatim so rewrites do not lose it.
    Unknown(Value),
}

/// One line of a session file after the header.
#[derive(Debug, Clone, PartialEq)]
pub struct SessionEntry {
    pub id: String,
    pub parent_id: Option<String>,
    pub timestamp: String,
    pub kind: EntryKind,
}

impl SessionEntry {
    /// Pi `type` field.
    pub fn type_name(&self) -> &str {
        match &self.kind {
            EntryKind::Message(_) => "message",
            EntryKind::ThinkingLevelChange { .. } => "thinking_level_change",
            EntryKind::ModelChange { .. } => "model_change",
            EntryKind::Compaction(_) => "compaction",
            EntryKind::BranchSummary(_) => "branch_summary",
            EntryKind::Custom { .. } => "custom",
            EntryKind::CustomMessage(_) => "custom_message",
            EntryKind::Label { .. } => "label",
            EntryKind::SessionInfo { .. } => "session_info",
            EntryKind::Unknown(v) => v.get("type").and_then(Value::as_str).unwrap_or(""),
        }
    }

    pub fn message(&self) -> Option<&AgentMessage> {
        match &self.kind {
            EntryKind::Message(m) => Some(m),
            _ => None,
        }
    }

    pub fn is_user_message(&self) -> bool {
        self.message().is_some_and(AgentMessage::is_user)
    }

    /// Entry timestamp in milliseconds (0 when unparsable).
    pub fn timestamp_millis(&self) -> i64 {
        parse_iso_millis(&self.timestamp).unwrap_or(0)
    }

    /// The message this entry contributes to a replayed context, if any
    /// (`message`, `custom_message`, `branch_summary`; compactions are
    /// handled by the context builder).
    pub fn context_message(&self) -> Option<AgentMessage> {
        match &self.kind {
            EntryKind::Message(m) => Some(m.clone()),
            EntryKind::CustomMessage(c) => Some(AgentMessage::Custom(CustomMessage {
                custom_type: c.custom_type.clone(),
                content: c.content.clone(),
                display: c.display,
                details: c.details.clone(),
                timestamp: self.timestamp_millis(),
            })),
            EntryKind::BranchSummary(b) if !b.summary.is_empty() => {
                Some(AgentMessage::BranchSummary(BranchSummaryMessage {
                    summary: b.summary.clone(),
                    from_id: b.from_id.clone(),
                    timestamp: self.timestamp_millis(),
                }))
            }
            _ => None,
        }
    }

    /// Compaction summary as a replay message.
    pub fn compaction_message(&self) -> Option<AgentMessage> {
        match &self.kind {
            EntryKind::Compaction(c) => {
                Some(AgentMessage::CompactionSummary(CompactionSummaryMessage {
                    summary: c.summary.clone(),
                    tokens_before: c.tokens_before,
                    timestamp: self.timestamp_millis(),
                }))
            }
            _ => None,
        }
    }

    pub fn to_value(&self) -> Value {
        let mut map = Map::new();
        if let EntryKind::Unknown(raw) = &self.kind {
            return raw.clone();
        }
        map.insert("type".into(), json!(self.type_name()));
        map.insert("id".into(), json!(self.id));
        map.insert("parentId".into(), json!(self.parent_id));
        map.insert("timestamp".into(), json!(self.timestamp));
        let mut put = |k: &str, v: Value| {
            map.insert(k.into(), v);
        };
        match &self.kind {
            EntryKind::Message(m) => put("message", json!(m)),
            EntryKind::ThinkingLevelChange { thinking_level } => {
                put("thinkingLevel", json!(thinking_level))
            }
            EntryKind::ModelChange { provider, model_id } => {
                put("provider", json!(provider));
                put("modelId", json!(model_id));
            }
            EntryKind::Compaction(c) => {
                put("summary", json!(c.summary));
                put("firstKeptEntryId", json!(c.first_kept_entry_id));
                put("tokensBefore", json!(c.tokens_before));
                if let Some(d) = &c.details {
                    put("details", d.clone());
                }
                if let Some(h) = c.from_hook {
                    put("fromHook", json!(h));
                }
            }
            EntryKind::BranchSummary(b) => {
                put("fromId", json!(b.from_id));
                put("summary", json!(b.summary));
                if let Some(d) = &b.details {
                    put("details", d.clone());
                }
                if let Some(h) = b.from_hook {
                    put("fromHook", json!(h));
                }
            }
            EntryKind::Custom { custom_type, data } => {
                put("customType", json!(custom_type));
                if let Some(d) = data {
                    put("data", d.clone());
                }
            }
            EntryKind::CustomMessage(c) => {
                put("customType", json!(c.custom_type));
                put("content", json!(c.content));
                if let Some(d) = &c.details {
                    put("details", d.clone());
                }
                put("display", json!(c.display));
            }
            EntryKind::Label { target_id, label } => {
                put("targetId", json!(target_id));
                if let Some(l) = label {
                    put("label", json!(l));
                }
            }
            EntryKind::SessionInfo { name } => {
                if let Some(n) = name {
                    put("name", json!(n));
                }
            }
            EntryKind::Unknown(_) => unreachable!("handled above"),
        }
        Value::Object(map)
    }

    /// Parse and validate one entry. Unknown types become [`EntryKind::Unknown`];
    /// known types with invalid fields are an error.
    pub fn from_value(v: Value) -> Result<Self, String> {
        let id = v
            .get("id")
            .and_then(Value::as_str)
            .filter(|s| !s.is_empty())
            .ok_or("entry has no id")?
            .to_string();
        let parent_id = match v.get("parentId") {
            None | Some(Value::Null) => None,
            Some(Value::String(s)) => Some(s.clone()),
            Some(_) => return Err(format!("entry {id}: parentId must be a string or null")),
        };
        let timestamp = str_field(&v, "timestamp").unwrap_or_default();
        let kind_name = v.get("type").and_then(Value::as_str).unwrap_or("");
        let req = |key: &str| -> Result<String, String> {
            str_field(&v, key).ok_or_else(|| format!("entry {id}: {kind_name} needs `{key}`"))
        };
        let kind = match kind_name {
            "message" => {
                let raw = v
                    .get("message")
                    .cloned()
                    .ok_or_else(|| format!("entry {id}: message entry has no message"))?;
                EntryKind::Message(
                    serde_json::from_value(raw).map_err(|e| format!("entry {id}: {e}"))?,
                )
            }
            "thinking_level_change" => EntryKind::ThinkingLevelChange {
                thinking_level: req("thinkingLevel")?,
            },
            "model_change" => EntryKind::ModelChange {
                provider: req("provider")?,
                model_id: req("modelId")?,
            },
            "compaction" => EntryKind::Compaction(CompactionEntry {
                summary: req("summary")?,
                first_kept_entry_id: req("firstKeptEntryId")?,
                tokens_before: v.get("tokensBefore").and_then(Value::as_u64).unwrap_or(0),
                details: v.get("details").filter(|d| !d.is_null()).cloned(),
                from_hook: v.get("fromHook").and_then(Value::as_bool),
            }),
            "branch_summary" => EntryKind::BranchSummary(BranchSummaryEntry {
                from_id: str_field(&v, "fromId").unwrap_or_else(|| "root".into()),
                summary: req("summary")?,
                details: v.get("details").filter(|d| !d.is_null()).cloned(),
                from_hook: v.get("fromHook").and_then(Value::as_bool),
            }),
            "custom" => EntryKind::Custom {
                custom_type: req("customType")?,
                data: v.get("data").filter(|d| !d.is_null()).cloned(),
            },
            "custom_message" => EntryKind::CustomMessage(CustomMessageEntry {
                custom_type: req("customType")?,
                content: serde_json::from_value(v.get("content").cloned().unwrap_or(json!("")))
                    .map_err(|e| format!("entry {id}: {e}"))?,
                details: v.get("details").filter(|d| !d.is_null()).cloned(),
                display: v.get("display").and_then(Value::as_bool).unwrap_or(false),
            }),
            "label" => EntryKind::Label {
                target_id: req("targetId")?,
                label: str_field(&v, "label").filter(|l| !l.is_empty()),
            },
            "session_info" => EntryKind::SessionInfo {
                name: str_field(&v, "name"),
            },
            _ => EntryKind::Unknown(v.clone()),
        };
        Ok(Self {
            id,
            parent_id,
            timestamp,
            kind,
        })
    }
}

/// What the model sees for a leaf, plus the settings in effect there.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct SessionContext {
    pub messages: Vec<AgentMessage>,
    pub thinking_level: String,
    pub model: Option<ModelRef>,
}

/// Build the context for a root → leaf path (Pi `buildSessionContext`).
pub fn context_from_path(path: &[SessionEntry]) -> SessionContext {
    let mut thinking_level = "off".to_string();
    let mut model = None;
    let mut compaction: Option<usize> = None;
    for (i, entry) in path.iter().enumerate() {
        match &entry.kind {
            EntryKind::ThinkingLevelChange { thinking_level: t } => thinking_level = t.clone(),
            EntryKind::ModelChange { provider, model_id } => {
                model = Some(ModelRef::new(provider.clone(), model_id.clone()))
            }
            EntryKind::Message(AgentMessage::Llm(Message::Assistant(a))) => {
                model = Some(ModelRef::new(a.provider.clone(), a.model.clone()))
            }
            EntryKind::Compaction(_) => compaction = Some(i),
            _ => {}
        }
    }
    let mut messages = Vec::new();
    match compaction {
        Some(ci) => {
            let EntryKind::Compaction(c) = &path[ci].kind else {
                unreachable!()
            };
            messages.extend(path[ci].compaction_message());
            let mut found = false;
            for entry in &path[..ci] {
                if entry.id == c.first_kept_entry_id {
                    found = true;
                }
                if found {
                    messages.extend(entry.context_message());
                }
            }
            for entry in &path[ci + 1..] {
                messages.extend(entry.context_message());
            }
        }
        None => {
            for entry in path {
                messages.extend(entry.context_message());
            }
        }
    }
    SessionContext {
        messages,
        thinking_level,
        model,
    }
}

/// One node of [`SessionTree`].
#[derive(Debug, Clone, PartialEq)]
pub struct SessionTreeNode {
    pub entry: SessionEntry,
    /// Child entry ids, oldest first.
    pub children: Vec<String>,
    pub label: Option<String>,
    pub label_timestamp: Option<String>,
}

/// The session as a tree (Pi `getTree`), stored flat so very deep sessions
/// never recurse.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct SessionTree {
    /// Root ids (normally one; orphans with a missing parent are roots too).
    pub roots: Vec<String>,
    pub nodes: HashMap<String, SessionTreeNode>,
}

/// A row of a flattened tree, in depth-first display order.
#[derive(Debug, Clone, PartialEq)]
pub struct TreeRow {
    pub id: String,
    pub depth: usize,
    pub child_count: usize,
}

impl SessionTree {
    pub fn node(&self, id: &str) -> Option<&SessionTreeNode> {
        self.nodes.get(id)
    }

    /// Depth-first order with depths, children oldest first.
    pub fn flatten(&self) -> Vec<TreeRow> {
        let mut out = Vec::with_capacity(self.nodes.len());
        let mut stack: Vec<(&str, usize)> =
            self.roots.iter().rev().map(|r| (r.as_str(), 0)).collect();
        while let Some((id, depth)) = stack.pop() {
            let Some(node) = self.nodes.get(id) else {
                continue;
            };
            out.push(TreeRow {
                id: id.to_string(),
                depth,
                child_count: node.children.len(),
            });
            for child in node.children.iter().rev() {
                stack.push((child.as_str(), depth + 1));
            }
        }
        out
    }
}

/// An open session (file-backed or in memory). Cloning copies the in-memory
/// state only; both copies write to the same file until one is re-pathed.
#[derive(Debug, Clone)]
pub struct Session {
    path: PathBuf,
    dir: PathBuf,
    header: SessionHeader,
    entries: Vec<SessionEntry>,
    index: HashMap<String, usize>,
    labels: HashMap<String, (String, String)>,
    leaf: Option<String>,
    persist: bool,
    flushed: bool,
    has_assistant: bool,
    diagnostics: Vec<String>,
}

impl Session {
    /// A new session in `dir`; nothing is written until an assistant message arrives.
    pub fn create(dir: &Path, cwd: &Path) -> Self {
        Self::create_with(dir, cwd, new_session_id(), None)
    }

    /// A new session with an explicit id and parent session path.
    pub fn create_with(dir: &Path, cwd: &Path, id: String, parent_session: Option<String>) -> Self {
        let path = child_session_path(dir, &id);
        Self::fresh(path, dir.to_path_buf(), cwd, id, parent_session, true)
    }

    /// A session that is never written to disk (`--no-session`).
    pub fn in_memory(cwd: &Path) -> Self {
        let id = new_session_id();
        Self::fresh(PathBuf::new(), PathBuf::new(), cwd, id, None, false)
    }

    fn fresh(
        path: PathBuf,
        dir: PathBuf,
        cwd: &Path,
        id: String,
        parent_session: Option<String>,
        persist: bool,
    ) -> Self {
        Self {
            path,
            dir,
            header: SessionHeader::new(id, cwd, parent_session),
            entries: Vec::new(),
            index: HashMap::new(),
            labels: HashMap::new(),
            leaf: None,
            persist,
            flushed: false,
            has_assistant: false,
            diagnostics: Vec::new(),
        }
    }

    /// Open an existing session file, migrating v1/v2 files to v3.
    pub fn open(path: &Path) -> Result<Self> {
        Self::open_with(path, None)
    }

    /// Open with the working directory overridden (Pi `cwdOverride`).
    pub fn open_with(path: &Path, cwd_override: Option<&Path>) -> Result<Self> {
        let text =
            fs::read_to_string(path).with_context(|| format!("opening {}", path.display()))?;
        let mut diagnostics = Vec::new();
        let mut values = Vec::new();
        for (n, line) in text.lines().enumerate() {
            if line.trim().is_empty() {
                continue;
            }
            match serde_json::from_str::<Value>(line) {
                Ok(v) => values.push(v),
                Err(e) => diagnostics.push(format!("line {}: malformed JSON: {e}", n + 1)),
            }
        }
        let mut header = values
            .first()
            .and_then(SessionHeader::from_value)
            .ok_or_else(|| anyhow!("{} is not a session file (missing header)", path.display()))?;
        let migrated = migrate_to_current(&mut values);
        if migrated {
            header.version = SESSION_VERSION;
        }
        if let Some(cwd) = cwd_override {
            header.cwd = cwd.to_string_lossy().into_owned();
        }
        let mut entries = Vec::new();
        for value in values.into_iter().skip(1) {
            if value.get("type").and_then(Value::as_str) == Some("session") {
                diagnostics.push("extra session header ignored".into());
                continue;
            }
            let raw = value.clone();
            match SessionEntry::from_value(value) {
                Ok(e) => entries.push(e),
                Err(err) => {
                    diagnostics.push(err);
                    // Keep entries that still have an id so the tree stays intact.
                    if let Some(id) = raw.get("id").and_then(Value::as_str) {
                        entries.push(SessionEntry {
                            id: id.to_string(),
                            parent_id: str_field(&raw, "parentId"),
                            timestamp: str_field(&raw, "timestamp").unwrap_or_default(),
                            kind: EntryKind::Unknown(raw),
                        });
                    }
                }
            }
        }
        let dir = path.parent().map(Path::to_path_buf).unwrap_or_default();
        let mut session = Self {
            path: path.to_path_buf(),
            dir,
            header,
            entries,
            index: HashMap::new(),
            labels: HashMap::new(),
            leaf: None,
            persist: true,
            flushed: true,
            has_assistant: false,
            diagnostics,
        };
        session.rebuild_index();
        if migrated {
            session.rewrite()?;
        }
        Ok(session)
    }

    /// Open `path`, or start a new session that will be written there.
    pub fn open_or_create(path: &Path, cwd: &Path) -> Result<Self> {
        if path.exists() {
            return Self::open(path);
        }
        let dir = path.parent().map(Path::to_path_buf).unwrap_or_default();
        let id = path
            .file_stem()
            .and_then(|s| s.to_str())
            .map(str::to_string)
            .unwrap_or_else(new_session_id);
        Ok(Self::fresh(path.to_path_buf(), dir, cwd, id, None, true))
    }

    fn rebuild_index(&mut self) {
        self.index.clear();
        self.labels.clear();
        self.leaf = None;
        self.has_assistant = false;
        for (i, entry) in self.entries.iter().enumerate() {
            self.index.insert(entry.id.clone(), i);
            self.leaf = Some(entry.id.clone());
            if let EntryKind::Label { target_id, label } = &entry.kind {
                match label {
                    Some(l) => {
                        self.labels
                            .insert(target_id.clone(), (l.clone(), entry.timestamp.clone()));
                    }
                    None => {
                        self.labels.remove(target_id);
                    }
                }
            }
            if entry
                .message()
                .and_then(AgentMessage::as_assistant)
                .is_some()
            {
                self.has_assistant = true;
            }
        }
    }

    pub fn path(&self) -> &Path {
        &self.path
    }
    pub fn dir(&self) -> &Path {
        &self.dir
    }
    pub fn id(&self) -> &str {
        &self.header.id
    }
    pub fn cwd(&self) -> &Path {
        Path::new(&self.header.cwd)
    }
    pub fn header(&self) -> &SessionHeader {
        &self.header
    }
    pub fn parent_session(&self) -> Option<&str> {
        self.header.parent_session.as_deref()
    }
    pub fn is_persisted(&self) -> bool {
        self.persist
    }
    /// Problems found while loading (malformed lines, invalid entries).
    pub fn diagnostics(&self) -> &[String] {
        &self.diagnostics
    }
    pub fn entries(&self) -> &[SessionEntry] {
        &self.entries
    }
    pub fn entry(&self, id: &str) -> Option<&SessionEntry> {
        self.index.get(id).map(|&i| &self.entries[i])
    }
    pub fn leaf_id(&self) -> Option<&str> {
        self.leaf.as_deref()
    }
    pub fn leaf_entry(&self) -> Option<&SessionEntry> {
        self.leaf.as_deref().and_then(|id| self.entry(id))
    }
    pub fn children(&self, parent_id: &str) -> Vec<&SessionEntry> {
        self.entries
            .iter()
            .filter(|e| e.parent_id.as_deref() == Some(parent_id))
            .collect()
    }
    pub fn label(&self, id: &str) -> Option<&str> {
        self.labels.get(id).map(|(l, _)| l.as_str())
    }

    /// Entries from the root to `from` (default: the leaf), all types.
    pub fn branch_path(&self, from: Option<&str>) -> Vec<SessionEntry> {
        let mut path = Vec::new();
        let mut seen = HashSet::new();
        let mut cursor = from.or(self.leaf.as_deref()).and_then(|id| self.entry(id));
        while let Some(entry) = cursor {
            if !seen.insert(entry.id.as_str()) {
                break;
            }
            path.push(entry.clone());
            cursor = entry.parent_id.as_deref().and_then(|p| self.entry(p));
        }
        path.reverse();
        path
    }

    /// Context for the current leaf (empty after `reset_leaf`).
    pub fn build_context(&self) -> SessionContext {
        if self.leaf.is_none() {
            return SessionContext {
                thinking_level: "off".into(),
                ..Default::default()
            };
        }
        context_from_path(&self.branch_path(None))
    }

    /// Context as if the leaf were `leaf_id`.
    pub fn context_at(&self, leaf_id: &str) -> Result<SessionContext> {
        if self.entry(leaf_id).is_none() {
            bail!("Entry {leaf_id} not found");
        }
        Ok(context_from_path(&self.branch_path(Some(leaf_id))))
    }

    /// Messages on the path from the root to the current leaf.
    pub fn context(&self) -> Vec<AgentMessage> {
        self.build_context().messages
    }

    /// Latest session name (`session_info`); an empty name clears it.
    pub fn name(&self) -> Option<String> {
        self.entries.iter().rev().find_map(|e| match &e.kind {
            EntryKind::SessionInfo { name } => Some(
                name.as_deref()
                    .map(str::trim)
                    .filter(|n| !n.is_empty())
                    .map(str::to_string),
            ),
            _ => None,
        })?
    }

    /// The tree of all entries with resolved labels.
    pub fn tree(&self) -> SessionTree {
        let mut tree = SessionTree::default();
        for entry in &self.entries {
            let (label, label_timestamp) = match self.labels.get(&entry.id) {
                Some((l, t)) => (Some(l.clone()), Some(t.clone())),
                None => (None, None),
            };
            tree.nodes.insert(
                entry.id.clone(),
                SessionTreeNode {
                    entry: entry.clone(),
                    children: Vec::new(),
                    label,
                    label_timestamp,
                },
            );
        }
        for entry in &self.entries {
            match entry.parent_id.as_deref() {
                Some(p) if p != entry.id && tree.nodes.contains_key(p) => {
                    if let Some(parent) = tree.nodes.get_mut(p) {
                        parent.children.push(entry.id.clone());
                    }
                }
                _ => tree.roots.push(entry.id.clone()),
            }
        }
        let stamps: HashMap<String, i64> = self
            .entries
            .iter()
            .map(|e| (e.id.clone(), e.timestamp_millis()))
            .collect();
        for node in tree.nodes.values_mut() {
            node.children
                .sort_by_key(|c| stamps.get(c).copied().unwrap_or(0));
        }
        tree
    }

    // ----- appending -------------------------------------------------------

    fn generate_id(&self) -> String {
        for _ in 0..100 {
            let id = crate::util::short_id();
            if !self.index.contains_key(&id) {
                return id;
            }
        }
        format!("{}{}", crate::util::short_id(), crate::util::short_id())
    }

    fn append_kind(&mut self, kind: EntryKind) -> Result<String> {
        let id = self.generate_id();
        self.append_entry(SessionEntry {
            id: id.clone(),
            parent_id: self.leaf.clone(),
            timestamp: now_iso(),
            kind,
        })?;
        Ok(id)
    }

    /// Append a prepared entry and make it the leaf. The entry is kept in
    /// memory even if writing fails; the next write retries the whole file.
    pub fn append_entry(&mut self, entry: SessionEntry) -> Result<()> {
        if entry
            .message()
            .and_then(AgentMessage::as_assistant)
            .is_some()
        {
            self.has_assistant = true;
        }
        if let EntryKind::Label { target_id, label } = &entry.kind {
            match label {
                Some(l) => {
                    self.labels
                        .insert(target_id.clone(), (l.clone(), entry.timestamp.clone()));
                }
                None => {
                    self.labels.remove(target_id);
                }
            }
        }
        self.index.insert(entry.id.clone(), self.entries.len());
        self.leaf = Some(entry.id.clone());
        self.entries.push(entry);
        self.persist_last()
    }

    pub fn append_message(&mut self, message: &AgentMessage) -> Result<String> {
        self.append_kind(EntryKind::Message(message.clone()))
    }

    pub fn append_thinking_level_change(&mut self, level: &str) -> Result<String> {
        self.append_kind(EntryKind::ThinkingLevelChange {
            thinking_level: level.to_string(),
        })
    }

    pub fn append_model_change(&mut self, provider: &str, model_id: &str) -> Result<String> {
        self.append_kind(EntryKind::ModelChange {
            provider: provider.to_string(),
            model_id: model_id.to_string(),
        })
    }

    pub fn append_compaction(&mut self, compaction: CompactionEntry) -> Result<String> {
        self.append_kind(EntryKind::Compaction(compaction))
    }

    pub fn append_custom_entry(
        &mut self,
        custom_type: &str,
        data: Option<Value>,
    ) -> Result<String> {
        self.append_kind(EntryKind::Custom {
            custom_type: custom_type.to_string(),
            data,
        })
    }

    pub fn append_custom_message_entry(&mut self, message: &CustomMessage) -> Result<String> {
        self.append_kind(EntryKind::CustomMessage(CustomMessageEntry {
            custom_type: message.custom_type.clone(),
            content: message.content.clone(),
            details: message.details.clone(),
            display: message.display,
        }))
    }

    /// Set the display name; an empty name clears it.
    pub fn append_session_info(&mut self, name: &str) -> Result<String> {
        self.append_kind(EntryKind::SessionInfo {
            name: Some(name.trim().to_string()),
        })
    }

    /// Set (or with `None`/empty, clear) the label of `target_id`.
    pub fn append_label_change(&mut self, target_id: &str, label: Option<&str>) -> Result<String> {
        if !self.index.contains_key(target_id) {
            bail!("Entry {target_id} not found");
        }
        let label = label.map(str::trim).filter(|l| !l.is_empty());
        self.append_kind(EntryKind::Label {
            target_id: target_id.to_string(),
            label: label.map(str::to_string),
        })
    }

    /// Latest `custom` entry of `custom_type` on the current branch.
    pub fn latest_custom_on_branch(&self, custom_type: &str) -> Option<Option<Value>> {
        self.branch_path(None)
            .into_iter()
            .rev()
            .find_map(|e| match e.kind {
                EntryKind::Custom {
                    custom_type: t,
                    data,
                } if t == custom_type => Some(data),
                _ => None,
            })
    }

    // ----- branching -------------------------------------------------------

    /// Move the leaf to `id`; the next append starts a new branch there.
    pub fn branch(&mut self, id: &str) -> Result<()> {
        if !self.index.contains_key(id) {
            bail!("Entry {id} not found");
        }
        self.leaf = Some(id.to_string());
        Ok(())
    }

    /// Move the leaf before the first entry (the next append is a new root).
    pub fn reset_leaf(&mut self) {
        self.leaf = None;
    }

    /// Branch from `from` (or the root when `None`) and record a summary of
    /// the abandoned path there. Returns the summary entry id.
    pub fn branch_with_summary(
        &mut self,
        from: Option<&str>,
        summary: &str,
        details: Option<Value>,
        from_hook: Option<bool>,
    ) -> Result<String> {
        if let Some(id) = from
            && !self.index.contains_key(id)
        {
            bail!("Entry {id} not found");
        }
        self.leaf = from.map(str::to_string);
        self.append_kind(EntryKind::BranchSummary(BranchSummaryEntry {
            from_id: from.unwrap_or("root").to_string(),
            summary: summary.to_string(),
            details,
            from_hook,
        }))
    }

    /// Replace this session with a new one holding only the path root →
    /// `leaf_id` (labels on that path are re-created at its end). The new
    /// session's header points back at this file. Pi `createBranchedSession`.
    pub fn create_branched_session(&mut self, leaf_id: &str, new_id: String) -> Result<()> {
        let path = self.branch_path(Some(leaf_id));
        if path.is_empty() {
            bail!("Entry {leaf_id} not found");
        }
        let kept: Vec<SessionEntry> = path
            .into_iter()
            .filter(|e| !matches!(e.kind, EntryKind::Label { .. }))
            .collect();
        let ids: HashSet<&str> = kept.iter().map(|e| e.id.as_str()).collect();
        let mut labels: Vec<(String, String, String)> = self
            .labels
            .iter()
            .filter(|(target, _)| ids.contains(target.as_str()))
            .map(|(t, (l, ts))| (t.clone(), l.clone(), ts.clone()))
            .collect();
        labels.sort_by(|a, b| a.2.cmp(&b.2).then(a.0.cmp(&b.0)));
        let parent_session = self
            .persist
            .then(|| self.path.to_string_lossy().into_owned());
        let cwd = PathBuf::from(&self.header.cwd);
        self.header = SessionHeader::new(new_id.clone(), &cwd, parent_session);
        self.entries = kept;
        self.rebuild_index();
        for (target_id, label, ts) in labels {
            let id = self.generate_id();
            let entry = SessionEntry {
                id: id.clone(),
                parent_id: self.leaf.clone(),
                timestamp: ts,
                kind: EntryKind::Label {
                    target_id,
                    label: Some(label),
                },
            };
            self.index.insert(id.clone(), self.entries.len());
            self.leaf = Some(id);
            self.entries.push(entry);
        }
        self.rebuild_index();
        if self.persist {
            self.path = child_session_path(&self.dir, &new_id);
            self.flushed = false;
            if self.has_assistant {
                self.rewrite()?;
            }
        }
        Ok(())
    }

    /// Copy of every entry under a new id and file (archived qt `clone`).
    pub fn clone_to(&self, new_id: String) -> Result<Session> {
        let mut clone = Session {
            path: child_session_path(&self.dir, &new_id),
            dir: self.dir.clone(),
            header: SessionHeader::new(
                new_id,
                Path::new(&self.header.cwd),
                self.persist
                    .then(|| self.path.to_string_lossy().into_owned()),
            ),
            entries: self.entries.clone(),
            index: HashMap::new(),
            labels: HashMap::new(),
            leaf: None,
            persist: self.persist,
            flushed: false,
            has_assistant: false,
            diagnostics: Vec::new(),
        };
        clone.rebuild_index();
        clone.leaf = self.leaf.clone();
        if clone.persist && clone.has_assistant {
            clone.rewrite()?;
        }
        Ok(clone)
    }

    // ----- persistence -----------------------------------------------------

    fn persist_last(&mut self) -> Result<()> {
        if !self.persist {
            return Ok(());
        }
        if !self.has_assistant {
            self.flushed = false;
            return Ok(());
        }
        if !self.flushed {
            self.rewrite()?;
            return Ok(());
        }
        let Some(entry) = self.entries.last() else {
            return Ok(());
        };
        let line = entry.to_value().to_string();
        let result = (|| -> Result<()> {
            let mut file = fs::OpenOptions::new()
                .append(true)
                .open(&self.path)
                .with_context(|| format!("writing {}", self.path.display()))?;
            writeln!(file, "{line}")?;
            Ok(())
        })();
        if result.is_err() {
            self.flushed = false;
        }
        result
    }

    /// Write the whole file (header + entries) atomically.
    pub fn rewrite(&mut self) -> Result<()> {
        if !self.persist {
            return Ok(());
        }
        let mut text = self.header.to_value().to_string();
        text.push('\n');
        for entry in &self.entries {
            text.push_str(&entry.to_value().to_string());
            text.push('\n');
        }
        let dir = self.path.parent().unwrap_or(Path::new("."));
        fs::create_dir_all(dir).with_context(|| format!("creating {}", dir.display()))?;
        let tmp = dir.join(format!(
            ".{}.{}.tmp",
            self.path
                .file_name()
                .and_then(|n| n.to_str())
                .unwrap_or("session"),
            std::process::id()
        ));
        fs::write(&tmp, text).with_context(|| format!("writing {}", tmp.display()))?;
        fs::rename(&tmp, &self.path)
            .with_context(|| format!("replacing {}", self.path.display()))?;
        self.flushed = true;
        Ok(())
    }
}

// ----- migration -------------------------------------------------------------

/// Bring raw file values (header first) to the current version. Returns
/// whether anything changed. Pi `migrateToCurrentVersion`.
pub fn migrate_to_current(values: &mut [Value]) -> bool {
    let version = values
        .iter()
        .find(|v| v.get("type").and_then(Value::as_str) == Some("session"))
        .and_then(|h| h.get("version").and_then(Value::as_u64))
        .unwrap_or(1);
    if version >= SESSION_VERSION {
        return false;
    }
    if version < 2 {
        migrate_v1_to_v2(values);
    }
    migrate_v2_to_v3(values);
    true
}

fn migrate_v1_to_v2(values: &mut [Value]) {
    let mut ids: HashSet<String> = HashSet::new();
    let mut prev: Option<String> = None;
    let mut assigned: Vec<Option<String>> = Vec::with_capacity(values.len());
    for v in values.iter_mut() {
        if v.get("type").and_then(Value::as_str) == Some("session") {
            v["version"] = json!(2);
            assigned.push(None);
            continue;
        }
        let id = loop {
            let id = crate::util::short_id();
            if ids.insert(id.clone()) {
                break id;
            }
        };
        if let Some(obj) = v.as_object_mut() {
            obj.insert("id".into(), json!(id));
            obj.insert("parentId".into(), json!(prev));
        }
        prev = Some(id.clone());
        assigned.push(Some(id));
    }
    for v in values.iter_mut() {
        let is_compaction = v.get("type").and_then(Value::as_str) == Some("compaction");
        if !is_compaction {
            continue;
        }
        let Some(index) = v.get("firstKeptEntryIndex").and_then(Value::as_u64) else {
            continue;
        };
        let target = assigned.get(index as usize).cloned().flatten();
        if let Some(obj) = v.as_object_mut() {
            if let Some(target) = target {
                obj.insert("firstKeptEntryId".into(), json!(target));
            }
            obj.remove("firstKeptEntryIndex");
        }
    }
}

fn migrate_v2_to_v3(values: &mut [Value]) {
    for v in values.iter_mut() {
        if v.get("type").and_then(Value::as_str) == Some("session") {
            v["version"] = json!(3);
            continue;
        }
        if v.get("type").and_then(Value::as_str) == Some("message")
            && v.pointer("/message/role").and_then(Value::as_str) == Some("hookMessage")
        {
            v["message"]["role"] = json!("custom");
        }
    }
}

// ----- catalog ---------------------------------------------------------------

/// A session id in the archived qt format.
pub fn new_session_id() -> String {
    let micros = SystemTime::now()
        .duration_since(SystemTime::UNIX_EPOCH)
        .map(|d| d.as_micros())
        .unwrap_or_default();
    format!("native-{micros}")
}

/// `dir/<stem>.jsonl`, or `<stem>-2.jsonl`, … when taken. Unsafe filename
/// characters become `-` (archived qt `child_session_path`).
pub fn child_session_path(dir: &Path, id: &str) -> PathBuf {
    let stem: String = if id.is_empty() {
        "session".into()
    } else {
        id.chars()
            .map(|c| {
                if c.is_ascii_alphanumeric() || c == '-' || c == '_' {
                    c
                } else {
                    '-'
                }
            })
            .collect()
    };
    let mut candidate = dir.join(format!("{stem}.jsonl"));
    let mut suffix = 2;
    while candidate.exists() && suffix < 1000 {
        candidate = dir.join(format!("{stem}-{suffix}.jsonl"));
        suffix += 1;
    }
    candidate
}

/// Resolve the session directory, highest precedence first: `--session-dir`,
/// `$PI_CODING_AGENT_SESSION_DIR`, the `sessionDir` setting, `<agent home>/sessions`. `~` is expanded;
/// relative paths resolve against `cwd` (CLI/env) or the agent home (setting).
pub fn resolve_session_dir(
    cli: Option<&Path>,
    settings: &Settings,
    home: &AgentHome,
    cwd: &Path,
) -> PathBuf {
    let expand = |raw: &str, base: &Path| -> PathBuf {
        let path = if raw == "~" {
            dirs::home_dir().unwrap_or_default()
        } else if let Some(rest) = raw.strip_prefix("~/") {
            dirs::home_dir().unwrap_or_default().join(rest)
        } else {
            PathBuf::from(raw)
        };
        if path.is_absolute() {
            path
        } else {
            base.join(path)
        }
    };
    if let Some(cli) = cli.filter(|p| !p.as_os_str().is_empty()) {
        return expand(&cli.to_string_lossy(), cwd);
    }
    if let Some(env) = std::env::var_os(SESSION_DIR_ENV).filter(|v| !v.is_empty()) {
        return expand(&env.to_string_lossy(), cwd);
    }
    if let Some(dir) = settings
        .raw()
        .get("sessionDir")
        .and_then(Value::as_str)
        .filter(|s| !s.trim().is_empty())
    {
        return expand(dir.trim(), home.root());
    }
    home.sessions_dir()
}

/// Summary of a session file for pickers (Pi `SessionInfo`).
#[derive(Debug, Clone, PartialEq)]
pub struct SessionSummary {
    pub path: PathBuf,
    pub id: String,
    pub cwd: PathBuf,
    /// Display name from `session_info`.
    pub name: Option<String>,
    pub parent_session: Option<String>,
    pub created: String,
    /// Last user/assistant activity (falls back to header time, then mtime).
    pub modified: SystemTime,
    pub message_count: usize,
    pub first_message: String,
    pub all_messages_text: String,
    /// Name, else the first user message (whitespace collapsed, ≤ 80 chars).
    pub title: String,
}

fn session_files(dir: &Path) -> Vec<PathBuf> {
    let mut out = Vec::new();
    let mut stack = vec![dir.to_path_buf()];
    while let Some(d) = stack.pop() {
        let Ok(read) = fs::read_dir(&d) else {
            continue;
        };
        for entry in read.flatten() {
            let path = entry.path();
            let Ok(ft) = entry.file_type() else {
                continue;
            };
            if ft.is_dir() {
                stack.push(path);
            } else if ft.is_file() && path.extension().is_some_and(|x| x == "jsonl") {
                out.push(path);
            }
        }
    }
    out.sort();
    out
}

fn same_dir(a: &Path, b: &Path) -> bool {
    let norm = |p: &Path| p.canonicalize().unwrap_or_else(|_| p.to_path_buf());
    a == b || norm(a) == norm(b)
}

/// Sessions under `dir` (recursively) recorded for `cwd` (all when `None`),
/// most recently active first.
pub fn list_sessions(dir: &Path, cwd: Option<&Path>) -> Vec<SessionSummary> {
    let mut out: Vec<SessionSummary> = session_files(dir)
        .iter()
        .filter_map(|p| summarize(p))
        .filter(|s| cwd.is_none_or(|c| same_dir(&s.cwd, c)))
        .collect();
    out.sort_by_key(|s| std::cmp::Reverse(s.modified));
    out
}

fn message_text(message: &Value) -> String {
    match message.get("content") {
        Some(Value::String(s)) => s.clone(),
        Some(Value::Array(blocks)) => blocks
            .iter()
            .filter(|b| b.get("type").and_then(Value::as_str) == Some("text"))
            .filter_map(|b| b.get("text").and_then(Value::as_str))
            .collect::<Vec<_>>()
            .join(" "),
        _ => String::new(),
    }
}

/// Summarize one session file (Pi `buildSessionInfo`).
pub fn summarize(path: &Path) -> Option<SessionSummary> {
    let text = fs::read_to_string(path).ok()?;
    let mtime = fs::metadata(path).and_then(|m| m.modified()).ok()?;
    let mut lines = text.lines().filter(|l| !l.trim().is_empty());
    let header_value: Value = serde_json::from_str(lines.next()?).ok()?;
    let header = SessionHeader::from_value(&header_value)?;
    let mut name = None;
    let mut message_count = 0;
    let mut first_message = String::new();
    let mut all = Vec::new();
    let mut last_activity: Option<i64> = None;
    for line in lines {
        let Ok(v) = serde_json::from_str::<Value>(line) else {
            continue;
        };
        match v.get("type").and_then(Value::as_str) {
            Some("session_info") => {
                name = str_field(&v, "name")
                    .map(|n| n.trim().to_string())
                    .filter(|n| !n.is_empty());
            }
            Some("message") => {
                message_count += 1;
                let message = &v["message"];
                let role = message.get("role").and_then(Value::as_str).unwrap_or("");
                if role != "user" && role != "assistant" {
                    continue;
                }
                let stamp = message
                    .get("timestamp")
                    .and_then(Value::as_i64)
                    .or_else(|| str_field(&v, "timestamp").and_then(|t| parse_iso_millis(&t)));
                if let Some(t) = stamp {
                    last_activity = Some(last_activity.map_or(t, |l| l.max(t)));
                }
                let text = message_text(message);
                if text.is_empty() {
                    continue;
                }
                if first_message.is_empty() && role == "user" {
                    first_message = text.clone();
                }
                all.push(text);
            }
            _ => {}
        }
    }
    let modified = last_activity
        .filter(|t| *t > 0)
        .or_else(|| parse_iso_millis(&header.timestamp))
        .map(|ms| SystemTime::UNIX_EPOCH + std::time::Duration::from_millis(ms.max(0) as u64))
        .unwrap_or(mtime);
    let collapsed = first_message
        .split_whitespace()
        .collect::<Vec<_>>()
        .join(" ");
    let title = match &name {
        Some(n) => n.clone(),
        None if collapsed.is_empty() => "(no messages)".into(),
        None => crate::util::shorten(&collapsed, 80),
    };
    Some(SessionSummary {
        path: path.to_path_buf(),
        id: header.id,
        cwd: PathBuf::from(header.cwd),
        name,
        parent_session: header.parent_session,
        created: header.timestamp,
        modified,
        message_count,
        first_message: if first_message.is_empty() {
            "(no messages)".into()
        } else {
            first_message
        },
        all_messages_text: all.join(" "),
        title,
    })
}

/// Whether a `--session` argument names a file rather than an id prefix.
pub fn looks_like_path(arg: &str) -> bool {
    arg.contains('/') || arg.contains('\\') || arg.ends_with(".jsonl")
}

/// Resolve a session id prefix in `dir`. Matches recorded for
/// `preferred_cwd` win over other matches; an exact id beats prefixes.
pub fn resolve_id_prefix(
    dir: &Path,
    prefix: &str,
    preferred_cwd: Option<&Path>,
) -> Result<PathBuf> {
    if prefix.is_empty() {
        bail!("session id prefix is required");
    }
    let all = list_sessions(dir, None);
    let matches: Vec<&SessionSummary> = all.iter().filter(|s| s.id.starts_with(prefix)).collect();
    if let Some(exact) = matches.iter().find(|s| s.id == prefix) {
        return Ok(exact.path.clone());
    }
    let preferred: Vec<&SessionSummary> = matches
        .iter()
        .copied()
        .filter(|s| preferred_cwd.is_some_and(|c| same_dir(&s.cwd, c)))
        .collect();
    let candidates = if preferred.is_empty() {
        matches
    } else {
        preferred
    };
    match candidates.as_slice() {
        [] => bail!("No session found matching '{prefix}'"),
        [one] => Ok(one.path.clone()),
        many => {
            let mut sorted: Vec<&SessionSummary> = many.to_vec();
            sorted.sort_by(|a, b| a.path.cmp(&b.path));
            let ids: Vec<&str> = sorted.iter().map(|s| s.id.as_str()).collect();
            bail!(
                "Session prefix '{prefix}' is ambiguous; matches: {}",
                ids.join(" ")
            )
        }
    }
}

/// Most recently modified session file; sessions for `preferred_cwd` first
/// (archived qt `find_most_recent_session_catalog_path`).
pub fn most_recent_session(dir: &Path, preferred_cwd: Option<&Path>) -> Option<PathBuf> {
    let mut best: Option<(bool, SystemTime, PathBuf)> = None;
    for path in session_files(dir) {
        let Ok(text) = fs::read_to_string(&path) else {
            continue;
        };
        let Some(header) = text
            .lines()
            .next()
            .and_then(|l| serde_json::from_str::<Value>(l).ok())
            .and_then(|v| SessionHeader::from_value(&v))
        else {
            continue;
        };
        let preferred = preferred_cwd.is_some_and(|c| same_dir(Path::new(&header.cwd), c));
        let modified = fs::metadata(&path)
            .and_then(|m| m.modified())
            .unwrap_or(SystemTime::UNIX_EPOCH);
        let better = match &best {
            None => true,
            Some((bp, bm, bpath)) => {
                if preferred != *bp {
                    preferred
                } else if modified != *bm {
                    modified > *bm
                } else {
                    path < *bpath
                }
            }
        };
        if better {
            best = Some((preferred, modified, path));
        }
    }
    best.map(|(_, _, p)| p)
}

/// A session whose recorded working directory no longer exists (Pi `SessionCwdIssue`).
#[derive(Debug, Clone, PartialEq)]
pub struct SessionCwdIssue {
    pub session_file: PathBuf,
    pub session_cwd: String,
    pub fallback_cwd: PathBuf,
}

impl SessionCwdIssue {
    /// Pi `formatMissingSessionCwdError`.
    pub fn error_text(&self) -> String {
        format!(
            "Stored session working directory does not exist: {}\nSession file: {}\nCurrent working directory: {}",
            self.session_cwd,
            self.session_file.display(),
            self.fallback_cwd.display()
        )
    }

    /// Pi `formatMissingSessionCwdPrompt`.
    pub fn prompt_text(&self) -> String {
        format!(
            "cwd from session file does not exist\n{}\n\ncontinue in current cwd\n{}",
            self.session_cwd,
            self.fallback_cwd.display()
        )
    }
}

/// Check whether `path`'s stored cwd still exists.
pub fn missing_cwd_issue(path: &Path, fallback_cwd: &Path) -> Option<SessionCwdIssue> {
    let text = fs::read_to_string(path).ok()?;
    let header = text
        .lines()
        .next()
        .and_then(|l| serde_json::from_str::<Value>(l).ok())
        .and_then(|v| SessionHeader::from_value(&v))?;
    if header.cwd.is_empty() || Path::new(&header.cwd).exists() {
        return None;
    }
    Some(SessionCwdIssue {
        session_file: path.to_path_buf(),
        session_cwd: header.cwd,
        fallback_cwd: fallback_cwd.to_path_buf(),
    })
}

// ----- helpers ---------------------------------------------------------------

fn str_field(v: &Value, key: &str) -> Option<String> {
    v.get(key).and_then(Value::as_str).map(str::to_string)
}

pub(crate) fn now_iso() -> String {
    chrono::Utc::now().to_rfc3339_opts(chrono::SecondsFormat::Millis, true)
}

fn parse_iso_millis(s: &str) -> Option<i64> {
    chrono::DateTime::parse_from_rfc3339(s)
        .ok()
        .map(|d| d.timestamp_millis())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::message::{AssistantMessage, ContentBlock, StopReason, Usage};

    fn user(text: &str) -> AgentMessage {
        AgentMessage::Llm(Message::user_text(text))
    }

    fn assistant(text: &str) -> AgentMessage {
        AgentMessage::Llm(Message::Assistant(AssistantMessage {
            content: vec![ContentBlock::text(text)],
            provider: "p".into(),
            model: "m".into(),
            usage: Usage {
                input_tokens: 10,
                output_tokens: 5,
                ..Default::default()
            },
            stop_reason: StopReason::Stop,
            timestamp: Some(crate::message::now_millis()),
            ..Default::default()
        }))
    }

    fn texts(messages: &[AgentMessage]) -> Vec<String> {
        messages
            .iter()
            .map(|m| match m.to_llm() {
                Some(Message::User { content, .. }) => content.plain_text(),
                Some(Message::Assistant(a)) => a.text(),
                _ => String::new(),
            })
            .collect()
    }

    #[test]
    fn writes_only_after_assistant_and_round_trips() {
        let dir = tempfile::tempdir().unwrap();
        let cwd = dir.path().join("proj");
        let mut s = Session::create(dir.path(), &cwd);
        assert!(s.id().starts_with("native-"));
        s.append_message(&user("first   question\nhere")).unwrap();
        assert!(!s.path().exists(), "no file before an assistant message");
        s.append_message(&assistant("answer")).unwrap();
        assert!(s.path().exists());
        s.append_session_info("  My session ").unwrap();
        let reopened = Session::open(s.path()).unwrap();
        assert_eq!(reopened.id(), s.id());
        assert_eq!(reopened.cwd(), cwd);
        assert_eq!(reopened.context().len(), 2);
        assert_eq!(reopened.name().as_deref(), Some("My session"));
        let header: Value = serde_json::from_str(
            fs::read_to_string(s.path())
                .unwrap()
                .lines()
                .next()
                .unwrap(),
        )
        .unwrap();
        assert_eq!(header["version"], 3);
        assert!(header.get("parentSession").is_none());

        let listed = list_sessions(dir.path(), Some(&cwd));
        assert_eq!(listed.len(), 1);
        assert_eq!(listed[0].title, "My session");
        assert_eq!(listed[0].first_message, "first   question\nhere");
        assert_eq!(listed[0].message_count, 2);
        assert!(list_sessions(dir.path(), Some(Path::new("/elsewhere"))).is_empty());
    }

    #[test]
    fn rewrite_keeps_unknown_header_fields() {
        // A persistent worker's session file is pre-written with a
        // `subagent` lineage header; the child's first flush rewrites it.
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("worker.jsonl");
        fs::write(
            &path,
            concat!(
                r#"{"type":"session","version":3,"id":"w1","timestamp":"t","cwd":"/p","parentSession":"parent-1","subagent":{"childId":"/root/x","agent":"scout"}}"#,
                "\n",
            ),
        )
        .unwrap();
        let mut s = Session::open(&path).unwrap();
        s.append_message(&user("task")).unwrap();
        s.append_message(&assistant("done")).unwrap();
        let header: Value =
            serde_json::from_str(fs::read_to_string(&path).unwrap().lines().next().unwrap())
                .unwrap();
        assert_eq!(header["id"], "w1");
        assert_eq!(header["parentSession"], "parent-1");
        assert_eq!(header["subagent"]["childId"], "/root/x");
        assert_eq!(header["subagent"]["agent"], "scout");
    }

    #[test]
    fn context_follows_parent_chain_and_skips_other_entries() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("s.jsonl");
        fs::write(
            &path,
            concat!(
                r#"{"type":"session","version":3,"id":"s1","timestamp":"t","cwd":"/p"}"#, "\n",
                r#"{"type":"message","id":"a","parentId":null,"message":{"role":"user","content":"one"}}"#, "\n",
                r#"{"type":"message","id":"b","parentId":"a","message":{"role":"user","content":"abandoned branch"}}"#, "\n",
                r#"{"type":"model_change","id":"c","parentId":"a","provider":"x","modelId":"y"}"#, "\n",
                r#"{"type":"thinking_level_change","id":"e","parentId":"c","thinkingLevel":"high"}"#, "\n",
                r#"{"type":"future_kind","id":"f","parentId":"e","x":1}"#, "\n",
                "not json\n",
                r#"{"type":"message","id":"d","parentId":"f","message":{"role":"user","content":"two"}}"#, "\n",
            ),
        )
        .unwrap();
        let s = Session::open(&path).unwrap();
        let ctx = s.build_context();
        assert_eq!(texts(&ctx.messages), ["one", "two"]);
        assert_eq!(ctx.model, Some(ModelRef::new("x", "y")));
        assert_eq!(ctx.thinking_level, "high");
        assert_eq!(s.diagnostics().len(), 1);
        // Unknown entries survive a rewrite.
        let mut s = s;
        s.rewrite().unwrap();
        assert!(fs::read_to_string(&path).unwrap().contains("future_kind"));
        assert_eq!(
            texts(&s.context_at("b").unwrap().messages),
            ["one", "abandoned branch"]
        );
    }

    #[test]
    fn migrates_v1_and_v2_files() {
        let dir = tempfile::tempdir().unwrap();
        let v1 = dir.path().join("v1.jsonl");
        fs::write(
            &v1,
            concat!(
                r#"{"type":"session","id":"old","timestamp":"2024-01-01T00:00:00.000Z","cwd":"/p"}"#, "\n",
                r#"{"type":"message","timestamp":"2024-01-01T00:00:01.000Z","message":{"role":"user","content":"hello"}}"#, "\n",
                r#"{"type":"message","timestamp":"2024-01-01T00:00:02.000Z","message":{"role":"assistant","content":[{"type":"text","text":"hi"}],"provider":"p","model":"m","usage":{},"stopReason":"stop"}}"#, "\n",
                r#"{"type":"compaction","timestamp":"2024-01-01T00:00:03.000Z","summary":"sum","firstKeptEntryIndex":2,"tokensBefore":50}"#, "\n",
                r#"{"type":"message","timestamp":"2024-01-01T00:00:04.000Z","message":{"role":"hookMessage","customType":"x","content":"ext","display":true,"timestamp":1}}"#, "\n",
            ),
        )
        .unwrap();
        let s = Session::open(&v1).unwrap();
        assert_eq!(s.header().version, 3);
        let ids: Vec<&str> = s.entries().iter().map(|e| e.id.as_str()).collect();
        assert_eq!(s.entries()[1].parent_id.as_deref(), Some(ids[0]));
        let EntryKind::Compaction(c) = &s.entries()[2].kind else {
            panic!()
        };
        assert_eq!(c.first_kept_entry_id, ids[1]);
        assert_eq!(s.entries()[3].message().unwrap().role(), "custom");
        let ctx = s.build_context();
        assert_eq!(
            ctx.messages
                .iter()
                .map(AgentMessage::role)
                .collect::<Vec<_>>(),
            ["compactionSummary", "assistant", "custom"]
        );
        // The file was rewritten as v3.
        let rewritten = fs::read_to_string(&v1).unwrap();
        assert!(rewritten.lines().next().unwrap().contains(r#""version":3"#));
        assert!(!rewritten.contains("hookMessage"));
        assert!(!rewritten.contains("firstKeptEntryIndex"));

        assert!(Session::open(&dir.path().join("missing.jsonl")).is_err());
        let bad = dir.path().join("bad.jsonl");
        fs::write(&bad, "{\"type\":\"message\"}\n").unwrap();
        assert!(Session::open(&bad).is_err());
    }

    #[test]
    fn compaction_replay_keeps_entries_from_first_kept() {
        let mut s = Session::in_memory(Path::new("/p"));
        s.append_message(&user("u1")).unwrap();
        s.append_message(&assistant("a1")).unwrap();
        let keep = s.append_message(&user("u2")).unwrap();
        s.append_message(&assistant("a2")).unwrap();
        s.append_compaction(CompactionEntry {
            summary: "SUMMARY".into(),
            first_kept_entry_id: keep,
            tokens_before: 100,
            details: None,
            from_hook: None,
        })
        .unwrap();
        s.append_message(&user("u3")).unwrap();
        let ctx = s.build_context();
        let t = texts(&ctx.messages);
        assert!(t[0].contains("<summary>\nSUMMARY\n</summary>"));
        assert_eq!(&t[1..], ["u2", "a2", "u3"]);
    }

    #[test]
    fn tree_branching_labels_and_summaries() {
        let mut s = Session::in_memory(Path::new("/p"));
        let u1 = s.append_message(&user("u1")).unwrap();
        let a1 = s.append_message(&assistant("a1")).unwrap();
        let u2 = s.append_message(&user("u2")).unwrap();
        s.branch(&a1).unwrap();
        let u2b = s.append_message(&user("u2 alt")).unwrap();
        assert_eq!(texts(&s.context()), ["u1", "a1", "u2 alt"]);
        assert!(s.branch("zzz").is_err());

        s.append_label_change(&u2, Some("checkpoint")).unwrap();
        assert_eq!(s.label(&u2), Some("checkpoint"));
        assert!(s.append_label_change("nope", Some("x")).is_err());

        let tree = s.tree();
        assert_eq!(tree.roots, vec![u1.clone()]);
        assert_eq!(tree.node(&a1).unwrap().children.len(), 2);
        assert_eq!(tree.node(&u2).unwrap().label.as_deref(), Some("checkpoint"));
        let rows = tree.flatten();
        assert_eq!(rows[0].id, u1);
        assert_eq!(rows[1].depth, 1);
        assert_eq!(rows.len(), s.entries().len());

        let summary = s
            .branch_with_summary(Some(&u1), "explored", Some(json!({"readFiles":[]})), None)
            .unwrap();
        let ctx = s.build_context();
        assert_eq!(ctx.messages.len(), 2);
        assert_eq!(ctx.messages[1].role(), "branchSummary");
        assert_eq!(s.leaf_id(), Some(summary.as_str()));
        s.branch_with_summary(None, "from root", None, None)
            .unwrap();
        let EntryKind::BranchSummary(b) = &s.leaf_entry().unwrap().kind else {
            panic!()
        };
        assert_eq!(b.from_id, "root");
        assert_eq!(s.leaf_entry().unwrap().parent_id, None);

        s.reset_leaf();
        assert!(s.build_context().messages.is_empty());
        s.append_label_change(&u2, None).unwrap();
        assert_eq!(s.label(&u2), None);
        assert_eq!(u2b.len(), 8);
    }

    #[test]
    fn branched_session_copies_path_and_labels() {
        let dir = tempfile::tempdir().unwrap();
        let mut s = Session::create(dir.path(), Path::new("/p"));
        let u1 = s.append_message(&user("u1")).unwrap();
        let a1 = s.append_message(&assistant("a1")).unwrap();
        s.append_message(&user("u2")).unwrap();
        s.append_label_change(&u1, Some("start")).unwrap();
        let original = s.path().to_path_buf();
        s.create_branched_session(&a1, "fork-x".into()).unwrap();
        assert_eq!(s.id(), "fork-x");
        assert_eq!(s.parent_session(), Some(original.to_str().unwrap()));
        assert_eq!(texts(&s.context()), ["u1", "a1"]);
        assert_eq!(s.label(&u1), Some("start"));
        let reopened = Session::open(s.path()).unwrap();
        assert_eq!(reopened.entries().len(), 3);
        assert_eq!(reopened.parent_session(), Some(original.to_str().unwrap()));
        let clone = reopened.clone_to("clone-fork-x".into()).unwrap();
        assert_eq!(clone.entries().len(), 3);
        assert!(clone.path().ends_with("clone-fork-x.jsonl"));
        assert!(clone.path().exists());
    }

    #[test]
    fn catalog_resolves_prefixes_and_recent_sessions() {
        let dir = tempfile::tempdir().unwrap();
        let cwd_a = dir.path().join("a");
        let cwd_b = dir.path().join("b");
        fs::create_dir_all(&cwd_a).unwrap();
        fs::create_dir_all(&cwd_b).unwrap();
        let mk = |id: &str, cwd: &Path| {
            let mut s = Session::create_with(dir.path(), cwd, id.to_string(), None);
            s.append_message(&user("q")).unwrap();
            s.append_message(&assistant("a")).unwrap();
            s.path().to_path_buf()
        };
        let p1 = mk("native-111", &cwd_a);
        let p2 = mk("native-112", &cwd_b);
        let p3 = mk("fork-native-111", &cwd_a);
        assert_eq!(resolve_id_prefix(dir.path(), "fork", None).unwrap(), p3);
        assert_eq!(
            resolve_id_prefix(dir.path(), "native-11", Some(&cwd_b)).unwrap(),
            p2
        );
        let err = resolve_id_prefix(dir.path(), "native-11", None)
            .unwrap_err()
            .to_string();
        assert_eq!(
            err,
            "Session prefix 'native-11' is ambiguous; matches: native-111 native-112"
        );
        assert_eq!(
            resolve_id_prefix(dir.path(), "zzz", None)
                .unwrap_err()
                .to_string(),
            "No session found matching 'zzz'"
        );
        assert_eq!(
            resolve_id_prefix(dir.path(), "native-111", None).unwrap(),
            p1
        );
        assert!(most_recent_session(dir.path(), Some(&cwd_b)).unwrap() == p2);
        assert!(looks_like_path("x.jsonl") && looks_like_path("a/b") && !looks_like_path("abc"));

        let missing = dir.path().join("gone");
        let s = {
            let mut s = Session::create(dir.path(), &missing);
            s.append_message(&assistant("a")).unwrap();
            s
        };
        let issue = missing_cwd_issue(s.path(), &cwd_a).unwrap();
        assert!(
            issue
                .prompt_text()
                .starts_with("cwd from session file does not exist\n")
        );
        assert!(missing_cwd_issue(&p1, &cwd_a).is_none());
    }

    #[test]
    fn session_dir_precedence() {
        let dir = tempfile::tempdir().unwrap();
        let home = AgentHome::new(dir.path().join("home"));
        let cwd = dir.path().join("cwd");
        let mut settings = Settings::default();
        assert_eq!(
            resolve_session_dir(None, &settings, &home, &cwd),
            home.sessions_dir()
        );
        settings
            .raw_mut()
            .insert("sessionDir".into(), json!("custom"));
        // The env var outranks the setting only when set; tests avoid
        // mutating the process environment and check the setting path here.
        if std::env::var_os(SESSION_DIR_ENV).is_none() {
            assert_eq!(
                resolve_session_dir(None, &settings, &home, &cwd),
                home.root().join("custom")
            );
        }
        assert_eq!(
            resolve_session_dir(Some(Path::new("cli")), &settings, &home, &cwd),
            cwd.join("cli")
        );
    }
}
