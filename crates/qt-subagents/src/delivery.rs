//! Delivering worker events to the parent conversation.
//!
//! The archived runtime turned every `subagent_complete` event into a parent
//! follow-up: a `custom` message with `customType: "subagent_event"`, the
//! event JSON as its text and the event as `details`, shown in the
//! transcript. Once delivered the event is acknowledged so it is not replayed
//! after a restart. JSON and RPC streams print `{"type":"subagent_event",
//! "subagent": <event>}` frames.

use std::sync::{Arc, Weak};

use async_channel::Receiver;
use serde_json::{Value, json};

use qt_core::agent::AgentEvent;
use qt_core::engine::{Conversation, Delivery};
use qt_core::message::{CustomMessage, UserContent};

use crate::coordinator::SubagentCoordinator;

/// `customType` of delivered worker events.
pub const EVENT_CUSTOM_TYPE: &str = "subagent_event";

/// The follow-up message carrying `event` to the parent model.
pub fn event_message(event: &Value) -> CustomMessage {
    let mut message = CustomMessage::new(
        EVENT_CUSTOM_TYPE,
        UserContent::Text(event.to_string()),
        true,
    );
    message.details = Some(event.clone());
    message
}

/// The JSON / RPC stream frame for `event`.
pub fn event_frame(event: &Value) -> Value {
    json!({ "type": EVENT_CUSTOM_TYPE, "subagent": event })
}

/// One transcript line for `event` (`subagent: /root/x completed`).
pub fn transcript_line(event: &Value) -> String {
    format!(
        "subagent: {} {}",
        event.get("child_id").and_then(Value::as_str).unwrap_or(""),
        event
            .get("status")
            .and_then(Value::as_str)
            .unwrap_or("updated")
    )
}

/// Whether `event` is a completion that wakes the parent.
pub fn is_completion(event: &Value) -> bool {
    event.get("type").and_then(Value::as_str) == Some("subagent_complete")
}

/// Hands one message to the parent; returns whether it was accepted (the
/// event is then acknowledged).
pub type DeliverFn = Arc<dyn Fn(CustomMessage) -> bool + Send + Sync>;

fn deliver_event(coordinator: &SubagentCoordinator, deliver: &DeliverFn, event: &Value) {
    if !is_completion(event) {
        return;
    }
    if deliver(event_message(event)) {
        let child = event.get("child_id").and_then(Value::as_str).unwrap_or("");
        let id = event.get("event_id").and_then(Value::as_u64).unwrap_or(0);
        let _ = coordinator.acknowledge_event(child, id);
    }
}

/// Route completion events (pending ones first) to `deliver`, acknowledging
/// each accepted event.
pub fn attach_delivery(coordinator: &Arc<SubagentCoordinator>, deliver: DeliverFn) {
    let weak: Weak<SubagentCoordinator> = Arc::downgrade(coordinator);
    let sink_deliver = deliver.clone();
    coordinator.set_event_sink(Arc::new(move |event: &Value| {
        if let Some(owner) = weak.upgrade() {
            deliver_event(&owner, &sink_deliver, event);
        }
    }));
    for event in coordinator.pending_events() {
        deliver_event(coordinator, &deliver, &event);
    }
}

/// Deliver into `conversation`: queued as a follow-up while it runs; when
/// idle a run starts with the message (its events go to `on_run`). Without a
/// usable model the message is appended to the context instead.
pub fn conversation_delivery(
    conversation: Conversation,
    on_run: Arc<dyn Fn(Receiver<AgentEvent>) + Send + Sync>,
) -> DeliverFn {
    Arc::new(move |message: CustomMessage| {
        match conversation.send_custom_message(message.clone(), Delivery::FollowUp, true) {
            Ok(Some(events)) => {
                on_run(events);
                true
            }
            Ok(None) => true,
            Err(_) => conversation
                .send_custom_message(message, Delivery::FollowUp, false)
                .is_ok(),
        }
    })
}

/// Wire a coordinator to a conversation: forks snapshot its context and
/// completions arrive as follow-up `subagent_event` messages.
pub fn attach_conversation(
    coordinator: &Arc<SubagentCoordinator>,
    conversation: &Conversation,
    on_run: Arc<dyn Fn(Receiver<AgentEvent>) + Send + Sync>,
) {
    let snapshot_source = conversation.clone();
    coordinator.set_context_snapshot(Arc::new(move || snapshot_source.agent_messages()));
    attach_delivery(
        coordinator,
        conversation_delivery(conversation.clone(), on_run),
    );
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn message_shape() {
        let event = json!({"type": "subagent_complete", "child_id": "/root/a", "event_id": 2, "status": "completed"});
        let message = event_message(&event);
        assert_eq!(message.custom_type, "subagent_event");
        assert!(message.display);
        assert_eq!(message.details.as_ref(), Some(&event));
        assert_eq!(
            serde_json::from_str::<Value>(&message.content.plain_text()).unwrap(),
            event
        );
        assert_eq!(event_frame(&event)["subagent"], event);
        assert_eq!(transcript_line(&event), "subagent: /root/a completed");
        assert!(is_completion(&event));
        assert!(!is_completion(&json!({"type": "subagent_start"})));
    }
}
