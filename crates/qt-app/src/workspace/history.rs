//! Composer prompt history: the global `history.jsonl` merged with the
//! session's own prompts (archived `restore_prompt_history`) and Up/Down
//! navigation that keeps the unsent draft (archived `Editor`).

use qt_core::history::{HistoryCursor, MAXIMUM_PROMPT_HISTORY_BYTES, PromptHistoryStore};

/// Session prompts first, then global prompts not already present, within
/// `limit` entries and the archived 8 MiB budget. Both lists are newest first.
pub fn merge_history(session: &[String], global: &[String], limit: usize) -> Vec<String> {
    if limit == 0 {
        return Vec::new();
    }
    let mut merged: Vec<String> = Vec::new();
    let mut bytes = 0usize;
    for prompt in session.iter().chain(global) {
        let prompt = prompt.trim();
        if prompt.is_empty() || merged.iter().any(|p| p == prompt) {
            continue;
        }
        if merged.len() >= limit || bytes + prompt.len() > MAXIMUM_PROMPT_HISTORY_BYTES {
            break;
        }
        bytes += prompt.len();
        merged.push(prompt.to_string());
    }
    merged
}

/// What an Up/Down key press should do.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum HistoryStep {
    /// Replace the composer text (cursor to the end).
    Show(String),
    /// Let the editor move the cursor.
    PassThrough,
}

/// Prompt history for one conversation.
#[derive(Debug, Default)]
pub struct PromptHistory {
    cursor: HistoryCursor,
    /// How far back navigation is (0 = editing the draft).
    depth: usize,
    /// Text last put into the composer by navigation; any other text means
    /// the user edited it and browsing ends.
    shown: Option<String>,
    limit: usize,
}

impl PromptHistory {
    pub fn new(entries: Vec<String>, limit: usize) -> Self {
        Self {
            cursor: HistoryCursor::new(entries),
            depth: 0,
            shown: None,
            limit,
        }
    }

    /// Load the global history and merge the session's prompts.
    /// Returns the history and a warning when the file could not be read.
    pub fn load(store: &PromptHistoryStore, session: &[String]) -> (Self, Option<String>) {
        let limit = store.max_entries();
        match store.load() {
            Ok(snapshot) => (
                Self::new(merge_history(session, &snapshot.prompts, limit), limit),
                None,
            ),
            Err(err) => (
                Self::new(merge_history(session, &[], limit), limit),
                Some(format!("Could not load prompt history: {err:#}")),
            ),
        }
    }

    #[cfg(test)]
    pub fn entries(&self) -> &[String] {
        self.cursor.entries()
    }

    #[cfg(test)]
    pub fn is_browsing(&self) -> bool {
        self.depth > 0
    }

    /// Record a submitted line and stop browsing.
    pub fn push(&mut self, prompt: &str) {
        if self.limit > 0 {
            self.cursor.push(prompt);
            if self.cursor.entries().len() > self.limit {
                let keep: Vec<String> = self.cursor.entries()[..self.limit].to_vec();
                self.cursor = HistoryCursor::new(keep);
            }
        }
        self.reset();
    }

    /// Stop browsing (the user edited the text).
    pub fn reset(&mut self) {
        self.cursor.reset();
        self.depth = 0;
        self.shown = None;
    }

    /// Called on every composer change: an edit that is not the text we
    /// showed ends browsing.
    pub fn on_text_changed(&mut self, text: &str) {
        if self.depth > 0 && self.shown.as_deref() != Some(text) {
            self.reset();
        }
    }

    /// Up: navigate when the composer is empty, or on the first line while
    /// browsing (archived editor).
    pub fn up(&mut self, text: &str, on_first_line: bool) -> HistoryStep {
        let navigate = text.is_empty() || (on_first_line && self.depth > 0);
        if !navigate {
            return HistoryStep::PassThrough;
        }
        match self.cursor.older(text) {
            Some(entry) => {
                let entry = entry.to_string();
                self.depth += 1;
                self.shown = Some(entry.clone());
                HistoryStep::Show(entry)
            }
            None if text.is_empty() => HistoryStep::PassThrough,
            // At the oldest entry: stay put.
            None => HistoryStep::Show(text.to_string()),
        }
    }

    /// Down: on the last line while browsing, go newer; past the newest
    /// entry the draft comes back.
    pub fn down(&mut self, on_last_line: bool) -> HistoryStep {
        if !on_last_line || self.depth == 0 {
            return HistoryStep::PassThrough;
        }
        match self.cursor.newer() {
            Some(entry) => {
                let entry = entry.to_string();
                self.depth -= 1;
                self.shown = (self.depth > 0).then(|| entry.clone());
                HistoryStep::Show(entry)
            }
            None => HistoryStep::PassThrough,
        }
    }
}

/// Whether byte `cursor` is on the first / last line of `text`.
pub fn cursor_lines(text: &str, cursor: usize) -> (bool, bool) {
    let cursor = cursor.min(text.len());
    (
        !text[..cursor].contains('\n'),
        !text[cursor..].contains('\n'),
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use qt_core::AgentHome;

    fn s(items: &[&str]) -> Vec<String> {
        items.iter().map(|i| i.to_string()).collect()
    }

    #[test]
    fn merges_session_then_global_within_limits() {
        assert_eq!(
            merge_history(&s(&["b", "a"]), &s(&["c", "a", " ", "d"]), 10),
            s(&["b", "a", "c", "d"])
        );
        assert_eq!(
            merge_history(&s(&["b", "a"]), &s(&["c"]), 2),
            s(&["b", "a"])
        );
        assert!(merge_history(&s(&["a"]), &s(&["b"]), 0).is_empty());
    }

    #[test]
    fn up_and_down_browse_and_restore_the_draft() {
        let mut h = PromptHistory::new(s(&["newest", "older", "oldest"]), 10);
        // Text in the composer and not browsing: the editor moves the cursor.
        assert_eq!(h.up("draft", true), HistoryStep::PassThrough);
        assert_eq!(h.down(true), HistoryStep::PassThrough);
        // Empty composer starts browsing.
        assert_eq!(h.up("", true), HistoryStep::Show("newest".into()));
        assert!(h.is_browsing());
        h.on_text_changed("newest");
        assert!(h.is_browsing());
        assert_eq!(h.up("newest", true), HistoryStep::Show("older".into()));
        assert_eq!(h.up("older", true), HistoryStep::Show("oldest".into()));
        assert_eq!(h.up("oldest", true), HistoryStep::Show("oldest".into()));
        // Not on the first line: move the cursor instead.
        assert_eq!(h.up("oldest", false), HistoryStep::PassThrough);
        assert_eq!(h.down(false), HistoryStep::PassThrough);
        assert_eq!(h.down(true), HistoryStep::Show("older".into()));
        assert_eq!(h.down(true), HistoryStep::Show("newest".into()));
        assert_eq!(h.down(true), HistoryStep::Show(String::new()));
        assert!(!h.is_browsing());
        assert_eq!(h.down(true), HistoryStep::PassThrough);
    }

    #[test]
    fn editing_ends_browsing_and_push_records() {
        let mut h = PromptHistory::new(s(&["one"]), 2);
        assert_eq!(h.up("", true), HistoryStep::Show("one".into()));
        h.on_text_changed("one!");
        assert!(!h.is_browsing());
        h.push("two");
        h.push("two");
        h.push(" three ");
        assert_eq!(h.entries(), s(&["three", "two"]));
        assert_eq!(h.up("", true), HistoryStep::Show("three".into()));
        let mut empty = PromptHistory::new(Vec::new(), 0);
        empty.push("x");
        assert!(empty.entries().is_empty());
        assert_eq!(empty.up("", true), HistoryStep::PassThrough);
    }

    #[test]
    fn loads_global_history_and_cursor_lines() {
        let dir = tempfile::tempdir().unwrap();
        let store = PromptHistoryStore::new(&AgentHome::new(dir.path()), 500);
        store.append("global one").unwrap();
        store.append("shared").unwrap();
        let (h, warning) = PromptHistory::load(&store, &s(&["session", "shared"]));
        assert!(warning.is_none());
        assert_eq!(h.entries(), s(&["session", "shared", "global one"]));
        assert_eq!(cursor_lines("a\nb", 0), (true, false));
        assert_eq!(cursor_lines("a\nb", 3), (false, true));
        assert_eq!(cursor_lines("", 0), (true, true));
    }
}
