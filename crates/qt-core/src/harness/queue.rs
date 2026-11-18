//! Steering, follow-up and next-turn message queues.
//!
//! - *Steering* messages are delivered after the current turn's tool calls
//!   finish, before the next model response.
//! - *Follow-up* messages are delivered only when the agent would otherwise
//!   stop (no tool calls, no steering).
//! - *Next-turn* messages ride along with the next user prompt.
//!
//! `QueueMode::OneAtATime` delivers one message per check (Pi default),
//! `QueueMode::All` drains the whole queue.

use std::collections::VecDeque;

use super::settings::QueueMode;
use crate::message::{AgentMessage, CustomMessage};

/// A queued message and the text shown for it in queue updates.
#[derive(Debug, Clone, PartialEq)]
pub struct QueuedMessage {
    pub text: String,
    pub message: AgentMessage,
}

/// Snapshot of what is waiting (Pi `queue_update`).
#[derive(Debug, Clone, PartialEq, Default)]
pub struct QueueSnapshot {
    pub steering: Vec<String>,
    pub follow_up: Vec<String>,
}

impl QueueSnapshot {
    pub fn is_empty(&self) -> bool {
        self.steering.is_empty() && self.follow_up.is_empty()
    }
}

#[derive(Debug, Clone, Default)]
pub struct MessageQueues {
    steering: VecDeque<QueuedMessage>,
    follow_up: VecDeque<QueuedMessage>,
    next_turn: Vec<CustomMessage>,
    pub steering_mode: QueueMode,
    pub follow_up_mode: QueueMode,
}

fn drain(queue: &mut VecDeque<QueuedMessage>, mode: QueueMode) -> Vec<AgentMessage> {
    match mode {
        QueueMode::All => queue.drain(..).map(|q| q.message).collect(),
        QueueMode::OneAtATime => queue.pop_front().map(|q| q.message).into_iter().collect(),
    }
}

impl MessageQueues {
    pub fn new(steering_mode: QueueMode, follow_up_mode: QueueMode) -> Self {
        Self {
            steering_mode,
            follow_up_mode,
            ..Default::default()
        }
    }

    pub fn push_steering(&mut self, text: String, message: AgentMessage) {
        self.steering.push_back(QueuedMessage { text, message });
    }

    pub fn push_follow_up(&mut self, text: String, message: AgentMessage) {
        self.follow_up.push_back(QueuedMessage { text, message });
    }

    pub fn push_next_turn(&mut self, message: CustomMessage) {
        self.next_turn.push(message);
    }

    /// Steering messages due now (per `steering_mode`).
    pub fn take_steering(&mut self) -> Vec<AgentMessage> {
        drain(&mut self.steering, self.steering_mode)
    }

    /// Follow-up messages due now (per `follow_up_mode`).
    pub fn take_follow_up(&mut self) -> Vec<AgentMessage> {
        drain(&mut self.follow_up, self.follow_up_mode)
    }

    /// All pending next-turn messages.
    pub fn take_next_turn(&mut self) -> Vec<CustomMessage> {
        std::mem::take(&mut self.next_turn)
    }

    pub fn has_queued(&self) -> bool {
        !self.steering.is_empty() || !self.follow_up.is_empty()
    }

    pub fn snapshot(&self) -> QueueSnapshot {
        QueueSnapshot {
            steering: self.steering.iter().map(|q| q.text.clone()).collect(),
            follow_up: self.follow_up.iter().map(|q| q.text.clone()).collect(),
        }
    }

    /// Drop steering and follow-up messages, returning their texts.
    pub fn clear(&mut self) -> QueueSnapshot {
        let snapshot = self.snapshot();
        self.steering.clear();
        self.follow_up.clear();
        snapshot
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::message::Message;

    fn q(text: &str) -> (String, AgentMessage) {
        (
            text.to_string(),
            AgentMessage::Llm(Message::user_text(text)),
        )
    }

    #[test]
    fn modes_control_delivery() {
        let mut queues = MessageQueues::default();
        for t in ["a", "b"] {
            let (text, m) = q(t);
            queues.push_steering(text, m);
        }
        assert_eq!(queues.snapshot().steering, ["a", "b"]);
        assert_eq!(queues.take_steering().len(), 1);
        assert_eq!(queues.snapshot().steering, ["b"]);
        queues.steering_mode = QueueMode::All;
        let (text, m) = q("c");
        queues.push_steering(text, m);
        assert_eq!(queues.take_steering().len(), 2);
        assert!(queues.take_steering().is_empty());

        let (text, m) = q("f");
        queues.push_follow_up(text, m);
        assert!(queues.has_queued());
        assert_eq!(queues.clear().follow_up, ["f"]);
        assert!(!queues.has_queued());

        queues.push_next_turn(CustomMessage::new(
            "x",
            crate::message::UserContent::Text("ctx".into()),
            false,
        ));
        assert_eq!(queues.take_next_turn().len(), 1);
        assert!(queues.take_next_turn().is_empty());
    }
}
