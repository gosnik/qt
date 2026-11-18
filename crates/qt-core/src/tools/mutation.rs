//! Mutation approval: `execute` runs file writes and commands immediately,
//! `queue` parks them in a [`MutationQueue`] until the user approves them.
//!
//! Follows the archived qt `ToolMutationQueue` (`archived/src/tools/models.cpp`)
//! and `require_mutation_approval` (`tool_validation.cpp`).

use std::collections::HashSet;
use std::sync::Mutex;

use serde::{Deserialize, Serialize};
use serde_json::{Value, json};

use super::{ToolContext, ToolOutput};

/// Whether mutating tool calls run immediately or wait for approval.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum MutationPolicy {
    #[default]
    Execute,
    Queue,
}

/// What a queued mutation would do.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum MutationKind {
    Command,
    FileAppend,
    FileEdit,
    FileWrite,
}

impl MutationKind {
    pub fn as_str(self) -> &'static str {
        match self {
            MutationKind::Command => "command",
            MutationKind::FileAppend => "file_append",
            MutationKind::FileEdit => "file_edit",
            MutationKind::FileWrite => "file_write",
        }
    }
}

/// A tool call waiting for approval.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct PendingMutation {
    pub id: String,
    pub kind: MutationKind,
    pub tool_name: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub call_id: Option<String>,
    pub summary: String,
    pub arguments: Value,
}

#[derive(Default)]
struct QueueState {
    pending: Vec<PendingMutation>,
    approved: HashSet<String>,
    next_id: u64,
}

/// Thread-safe queue of mutations awaiting approval.
#[derive(Default)]
pub struct MutationQueue {
    state: Mutex<QueueState>,
}

impl MutationQueue {
    pub fn new() -> Self {
        Self::default()
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, QueueState> {
        self.state.lock().unwrap_or_else(|e| e.into_inner())
    }

    /// Add a mutation, assigning `mutation-<n>` when it has no id.
    pub fn enqueue(&self, mut mutation: PendingMutation) -> String {
        let mut state = self.lock();
        if mutation.id.is_empty() {
            state.next_id += 1;
            mutation.id = format!("mutation-{}", state.next_id);
        }
        let id = mutation.id.clone();
        state.pending.push(mutation);
        id
    }

    /// Approve a pending mutation and return it so the caller can re-run the
    /// tool with [`ToolContext::with_approved_mutation`].
    pub fn approve(&self, id: &str) -> Option<PendingMutation> {
        let mut state = self.lock();
        let index = state.pending.iter().position(|m| m.id == id)?;
        let mutation = state.pending.remove(index);
        state.approved.insert(mutation.id.clone());
        Some(mutation)
    }

    /// Drop a pending mutation. Returns `false` for unknown ids.
    pub fn reject(&self, id: &str) -> bool {
        let mut state = self.lock();
        match state.pending.iter().position(|m| m.id == id) {
            Some(index) => {
                state.pending.remove(index);
                true
            }
            None => false,
        }
    }

    pub fn is_approved(&self, id: &str) -> bool {
        self.lock().approved.contains(id)
    }

    /// Consume an approval; each approval authorises one execution.
    pub fn take_approval(&self, id: &str) -> bool {
        self.lock().approved.remove(id)
    }

    pub fn find(&self, id: &str) -> Option<PendingMutation> {
        self.lock().pending.iter().find(|m| m.id == id).cloned()
    }

    pub fn pending(&self) -> Vec<PendingMutation> {
        self.lock().pending.clone()
    }

    pub fn len(&self) -> usize {
        self.lock().pending.len()
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

/// Gate a mutation. Returns `None` when the tool may proceed, otherwise the
/// result to hand back to the model (queued for approval, or refused).
pub(crate) fn require_approval(
    ctx: &ToolContext,
    kind: MutationKind,
    tool_name: &str,
    summary: String,
    arguments: &Value,
) -> Option<ToolOutput> {
    if ctx.mutations.policy == MutationPolicy::Execute {
        return None;
    }
    let Some(queue) = ctx.mutations.queue.as_ref() else {
        return Some(ToolOutput::error(
            "mutation queue is required by tool context",
        ));
    };
    if let Some(id) = &ctx.mutations.approved_id {
        if queue.take_approval(id) {
            return None;
        }
        return Some(
            ToolOutput::error("mutation approval was not granted").with_details(json!({
                "status": "permission_denied",
                "mutation_id": id,
            })),
        );
    }
    let id = queue.enqueue(PendingMutation {
        id: String::new(),
        kind,
        tool_name: tool_name.to_string(),
        call_id: ctx.call_id.clone(),
        summary: summary.clone(),
        arguments: arguments.clone(),
    });
    Some(
        ToolOutput::error(format!(
            "Pending approval: {summary}. The change was not applied; it is queued as {id} until the user approves it."
        ))
        .with_details(json!({
            "status": "pending_approval",
            "mutation_id": id,
            "queued": true,
            "kind": kind.as_str(),
            "tool_name": tool_name,
        })),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn pending(summary: &str) -> PendingMutation {
        PendingMutation {
            id: String::new(),
            kind: MutationKind::FileWrite,
            tool_name: "write".into(),
            call_id: None,
            summary: summary.into(),
            arguments: json!({}),
        }
    }

    #[test]
    fn queue_assigns_ids_and_approves_or_rejects() {
        let queue = MutationQueue::new();
        let a = queue.enqueue(pending("a"));
        let b = queue.enqueue(pending("b"));
        assert_eq!((a.as_str(), b.as_str()), ("mutation-1", "mutation-2"));
        assert_eq!(queue.len(), 2);
        assert_eq!(queue.approve(&a).unwrap().summary, "a");
        assert!(queue.is_approved(&a));
        assert!(queue.approve(&a).is_none());
        assert!(queue.reject(&b));
        assert!(!queue.reject(&b));
        assert!(queue.is_empty());
        assert!(queue.take_approval(&a));
        assert!(!queue.take_approval(&a));
    }
}
