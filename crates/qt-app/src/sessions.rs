//! Pure helpers for the session views: tree rows, entry summaries, session
//! search and the user-message ↔ entry-id alignment of the transcript.

use std::collections::HashSet;

use qt_core::message::{AgentMessage, ContentBlock, Message};
use qt_core::session::{EntryKind, SessionEntry, SessionSummary, SessionTree};

/// Archived `first_line_truncated`: first line, at most `limit` chars.
pub fn first_line_truncated(text: &str, limit: usize) -> String {
    let line = text.lines().next().unwrap_or_default().trim_end();
    if line.chars().count() > limit {
        let kept: String = line.chars().take(limit.saturating_sub(3)).collect();
        format!("{kept}...")
    } else {
        line.to_string()
    }
}

/// Archived `entry_summary_text`: one line describing an entry.
pub fn entry_summary(entry: &SessionEntry) -> String {
    let t = |s: &str| first_line_truncated(s, 72);
    match &entry.kind {
        EntryKind::Message(m) => match m {
            AgentMessage::Llm(Message::User { content, .. }) => {
                let images = content
                    .blocks()
                    .iter()
                    .filter(|b| matches!(b, ContentBlock::Image { .. }))
                    .count();
                let mut text = format!("user: {}", t(&content.plain_text()));
                if images > 0 {
                    text.push_str(&format!(" [{images} image(s)]"));
                }
                text
            }
            AgentMessage::Llm(Message::Assistant(a)) => {
                let text = a.text();
                if text.trim().is_empty() {
                    let tools: Vec<&str> = a.tool_calls().map(|(_, name, _)| name).collect();
                    if tools.is_empty() {
                        "assistant: ".to_string()
                    } else {
                        format!("assistant: [{}]", tools.join(", "))
                    }
                } else {
                    format!("assistant: {}", t(&text))
                }
            }
            AgentMessage::Llm(Message::ToolResult(r)) => format!("tool: {}", r.tool_name),
            AgentMessage::Custom(c) => format!("custom: {}", t(&c.content.plain_text())),
            AgentMessage::BashExecution(b) => format!("bash: {}", t(&b.command)),
            AgentMessage::BranchSummary(b) => format!("branch summary: {}", t(&b.summary)),
            AgentMessage::CompactionSummary(c) => format!("compaction: {}", t(&c.summary)),
        },
        EntryKind::CustomMessage(c) => format!("custom: {}", t(&c.content.plain_text())),
        EntryKind::BranchSummary(b) => format!("branch summary: {}", t(&b.summary)),
        EntryKind::Compaction(c) => format!("compaction: {}", t(&c.summary)),
        EntryKind::ModelChange { provider, model_id } => format!("model: {provider}/{model_id}"),
        EntryKind::ThinkingLevelChange { thinking_level } => format!("thinking: {thinking_level}"),
        EntryKind::SessionInfo { name } => format!(
            "session info: {}",
            name.as_deref()
                .filter(|n| !n.is_empty())
                .unwrap_or("(unnamed)")
        ),
        EntryKind::Label { label, .. } => format!("label: {}", label.as_deref().unwrap_or("")),
        EntryKind::Custom { custom_type, .. } => format!("custom entry: {custom_type}"),
        EntryKind::Unknown(_) => format!("entry: {}", entry.type_name()),
    }
}

/// Which rows the tree view shows (Pi tree filter modes).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum TreeFilter {
    /// Messages, summaries and compactions; metadata entries hidden.
    #[default]
    Default,
    /// Like default, without tool results.
    NoTools,
    UserOnly,
    LabeledOnly,
    All,
}

impl TreeFilter {
    pub const ALL: [TreeFilter; 5] = [
        TreeFilter::Default,
        TreeFilter::NoTools,
        TreeFilter::UserOnly,
        TreeFilter::LabeledOnly,
        TreeFilter::All,
    ];

    pub fn label(self) -> &'static str {
        match self {
            TreeFilter::Default => "Default",
            TreeFilter::NoTools => "No tools",
            TreeFilter::UserOnly => "User only",
            TreeFilter::LabeledOnly => "Labeled",
            TreeFilter::All => "All",
        }
    }

    fn keeps(self, entry: &SessionEntry, labeled: bool) -> bool {
        let is_meta = matches!(
            entry.kind,
            EntryKind::ModelChange { .. }
                | EntryKind::ThinkingLevelChange { .. }
                | EntryKind::SessionInfo { .. }
                | EntryKind::Label { .. }
                | EntryKind::Custom { .. }
                | EntryKind::Unknown(_)
        );
        let is_tool = matches!(
            entry.kind,
            EntryKind::Message(AgentMessage::Llm(Message::ToolResult(_)))
        );
        match self {
            TreeFilter::All => true,
            TreeFilter::Default => !is_meta,
            TreeFilter::NoTools => !is_meta && !is_tool,
            TreeFilter::UserOnly => entry.is_user_message(),
            TreeFilter::LabeledOnly => labeled,
        }
    }
}

/// One visible row of the tree view.
#[derive(Debug, Clone, PartialEq)]
pub struct TreeViewRow {
    pub id: String,
    pub depth: usize,
    pub summary: String,
    pub label: Option<String>,
    pub is_user: bool,
    /// On the path root → current leaf.
    pub on_active_path: bool,
    pub is_leaf: bool,
    /// Children in the full tree (a branch point when > 1).
    pub child_count: usize,
}

/// Visible rows: depth-first, filtered and searched. Depth counts only
/// visible ancestors that start a new branch level, so hidden metadata
/// entries do not indent their children.
pub fn tree_rows(
    tree: &SessionTree,
    leaf: Option<&str>,
    filter: TreeFilter,
    query: &str,
) -> Vec<TreeViewRow> {
    let mut active: HashSet<String> = HashSet::new();
    let mut cursor = leaf.map(str::to_string);
    while let Some(id) = cursor {
        if !active.insert(id.clone()) {
            break;
        }
        cursor = tree.node(&id).and_then(|n| n.entry.parent_id.clone());
    }
    let words: Vec<String> = query.split_whitespace().map(str::to_lowercase).collect();
    let mut out = Vec::new();
    for row in tree.flatten() {
        let Some(node) = tree.node(&row.id) else {
            continue;
        };
        if !filter.keeps(&node.entry, node.label.is_some()) {
            continue;
        }
        let summary = entry_summary(&node.entry);
        if !words.is_empty() {
            let hay = format!(
                "{} {} {}",
                summary.to_lowercase(),
                node.label.as_deref().unwrap_or("").to_lowercase(),
                row.id.to_lowercase()
            );
            if !words.iter().all(|w| hay.contains(w)) {
                continue;
            }
        }
        out.push(TreeViewRow {
            id: row.id.clone(),
            depth: branch_depth(tree, &row.id),
            summary,
            label: node.label.clone(),
            is_user: node.entry.is_user_message(),
            on_active_path: active.contains(&row.id),
            is_leaf: leaf == Some(row.id.as_str()),
            child_count: row.child_count,
        });
    }
    // A hidden leaf (label, session info …): its nearest visible ancestor
    // stands for the current position.
    if leaf.is_some()
        && !out.iter().any(|r| r.is_leaf)
        && let Some(last) = out.iter_mut().rev().find(|r| r.on_active_path)
    {
        last.is_leaf = true;
    }
    out
}

/// Number of branch points (nodes with several children) above `id`, plus
/// one for every non-first child taken. A linear session stays flat.
fn branch_depth(tree: &SessionTree, id: &str) -> usize {
    let mut depth = 0;
    let mut current = id.to_string();
    let mut guard = 0;
    while let Some(parent) = tree
        .node(&current)
        .and_then(|n| n.entry.parent_id.clone())
        .filter(|p| tree.node(p).is_some())
    {
        guard += 1;
        if guard > tree.nodes.len() {
            break;
        }
        if tree.node(&parent).is_some_and(|p| p.children.len() > 1) {
            depth += 1;
        }
        current = parent;
    }
    depth
}

/// Whether a session matches every word of `query` (name, first message,
/// all message text, id or working directory).
pub fn session_matches(session: &SessionSummary, query: &str) -> bool {
    let words: Vec<String> = query.split_whitespace().map(str::to_lowercase).collect();
    if words.is_empty() {
        return true;
    }
    let hay = format!(
        "{}\n{}\n{}\n{}\n{}\n{}",
        session.name.as_deref().unwrap_or(""),
        session.title,
        session.first_message,
        session.all_messages_text,
        session.id,
        session.cwd.display()
    )
    .to_lowercase();
    words.iter().all(|w| hay.contains(w.as_str()))
}

#[cfg(test)]
mod tests {
    use super::*;
    use qt_core::message::{AssistantMessage, UserContent};
    use qt_core::session::{SessionTree, SessionTreeNode};
    use std::collections::HashMap;

    fn entry(id: &str, parent: Option<&str>, kind: EntryKind) -> SessionEntry {
        SessionEntry {
            id: id.into(),
            parent_id: parent.map(str::to_string),
            timestamp: "2026-01-01T00:00:00.000Z".into(),
            kind,
        }
    }

    fn user(text: &str) -> EntryKind {
        EntryKind::Message(AgentMessage::Llm(Message::user_text(text)))
    }

    fn assistant(text: &str) -> EntryKind {
        EntryKind::Message(AgentMessage::Llm(Message::Assistant(AssistantMessage {
            content: vec![ContentBlock::text(text)],
            ..Default::default()
        })))
    }

    fn tree(entries: Vec<SessionEntry>, labels: &[(&str, &str)]) -> SessionTree {
        let mut t = SessionTree {
            roots: Vec::new(),
            nodes: HashMap::new(),
        };
        for e in &entries {
            t.nodes.insert(
                e.id.clone(),
                SessionTreeNode {
                    entry: e.clone(),
                    children: Vec::new(),
                    label: labels
                        .iter()
                        .find(|(id, _)| *id == e.id)
                        .map(|(_, l)| l.to_string()),
                    label_timestamp: None,
                },
            );
        }
        for e in &entries {
            match &e.parent_id {
                Some(p) => t.nodes.get_mut(p).unwrap().children.push(e.id.clone()),
                None => t.roots.push(e.id.clone()),
            }
        }
        t
    }

    #[test]
    fn summaries_follow_archived_wording() {
        assert_eq!(first_line_truncated("abc\ndef", 72), "abc");
        assert_eq!(first_line_truncated(&"x".repeat(80), 10), "xxxxxxx...");
        assert_eq!(
            entry_summary(&entry("a", None, user("hello\nworld"))),
            "user: hello"
        );
        assert_eq!(
            entry_summary(&entry("a", None, assistant("done"))),
            "assistant: done"
        );
        assert_eq!(
            entry_summary(&entry(
                "m",
                None,
                EntryKind::ModelChange {
                    provider: "anthropic".into(),
                    model_id: "claude".into()
                }
            )),
            "model: anthropic/claude"
        );
        assert_eq!(
            entry_summary(&entry("s", None, EntryKind::SessionInfo { name: None })),
            "session info: (unnamed)"
        );
        let img = EntryKind::Message(AgentMessage::Llm(Message::User {
            content: UserContent::Blocks(vec![
                ContentBlock::text("see"),
                ContentBlock::Image {
                    data: String::new(),
                    mime_type: "image/png".into(),
                },
            ]),
            timestamp: None,
        }));
        assert_eq!(
            entry_summary(&entry("i", None, img)),
            "user: see [1 image(s)]"
        );
    }

    #[test]
    fn rows_mark_the_active_path_and_filter() {
        // u1 -> a1 -> u2 -> a2 (leaf)
        //            \-> u3 -> a3
        let t = tree(
            vec![
                entry("u1", None, user("first")),
                entry(
                    "m",
                    Some("u1"),
                    EntryKind::ThinkingLevelChange {
                        thinking_level: "high".into(),
                    },
                ),
                entry("a1", Some("m"), assistant("one")),
                entry("u2", Some("a1"), user("second")),
                entry("a2", Some("u2"), assistant("two")),
                entry("u3", Some("a1"), user("other")),
                entry("a3", Some("u3"), assistant("three")),
            ],
            &[("u3", "alt")],
        );
        let rows = tree_rows(&t, Some("a2"), TreeFilter::Default, "");
        let ids: Vec<&str> = rows.iter().map(|r| r.id.as_str()).collect();
        assert_eq!(ids, ["u1", "a1", "u2", "a2", "u3", "a3"]);
        assert!(rows[3].is_leaf && rows[3].on_active_path);
        assert!(!rows[4].on_active_path);
        assert_eq!(rows[0].depth, 0);
        assert_eq!(rows[2].depth, 1);
        assert_eq!(rows[4].label.as_deref(), Some("alt"));

        let all = tree_rows(&t, Some("a2"), TreeFilter::All, "");
        assert_eq!(all.len(), 7);
        let users = tree_rows(&t, None, TreeFilter::UserOnly, "");
        assert_eq!(users.len(), 3);
        assert!(users.iter().all(|r| r.is_user));
        let labeled = tree_rows(&t, None, TreeFilter::LabeledOnly, "");
        assert_eq!(labeled.len(), 1);
        let searched = tree_rows(&t, None, TreeFilter::Default, "ALT");
        assert_eq!(searched.len(), 1);
        let searched = tree_rows(&t, None, TreeFilter::Default, "assistant two");
        assert_eq!(searched[0].id, "a2");

        // The leaf is a hidden metadata entry: its visible ancestor is current.
        let rows = tree_rows(&t, Some("m"), TreeFilter::Default, "");
        let current: Vec<&str> = rows
            .iter()
            .filter(|r| r.is_leaf)
            .map(|r| r.id.as_str())
            .collect();
        assert_eq!(current, ["u1"]);
    }

    #[test]
    fn session_search_matches_all_words() {
        let s = SessionSummary {
            path: "/s/a.jsonl".into(),
            id: "abc123".into(),
            cwd: "/work/proj".into(),
            name: Some("Refactor".into()),
            parent_session: None,
            created: String::new(),
            modified: std::time::SystemTime::UNIX_EPOCH,
            message_count: 2,
            first_message: "fix the parser".into(),
            all_messages_text: "fix the parser and the lexer".into(),
            title: "Refactor".into(),
        };
        assert!(session_matches(&s, ""));
        assert!(session_matches(&s, "refactor lexer"));
        assert!(session_matches(&s, "ABC1"));
        assert!(session_matches(&s, "proj"));
        assert!(!session_matches(&s, "lexer compiler"));
    }
}
