//! `/memory dream [status|run|research <json>]` and the RPC `memory` action
//! `dream` (archived `interactive_mode.cpp` and `rpc_mode.cpp`) as library
//! functions. Front ends render the returned lines or JSON.

use serde_json::{Value, json};

use super::{DreamTrigger, DreamingService};
use crate::error::{Result, invalid};
use crate::research::{ManualEvidenceOrigin, parse_manual_research};

const USAGE: &str = "usage: /memory dream [status|run|research <json>]";
const RESEARCH_USAGE: &str = "usage: /memory dream research {\"dream_id\":\"...\",\"summary\":\"...\",\"source_uri\":\"...\"}";
const NOT_QUEUED_RPC: &str =
    "Dreaming cycle was not queued; enable Dreaming and verify its queue/model settings";

fn none_if_empty(value: &str) -> &str {
    if value.is_empty() { "none" } else { value }
}

/// Handles the text after `/memory dream` and returns transcript lines.
pub fn dream_command(service: &DreamingService, arguments: &str) -> Vec<String> {
    let arguments = arguments.trim_start();
    let (action, rest) = match arguments.find(char::is_whitespace) {
        Some(index) => (&arguments[..index], arguments[index..].trim_start()),
        None => (arguments, ""),
    };
    match action.to_ascii_lowercase().as_str() {
        "" | "status" => {
            let status = service.status();
            vec![
                "Memory Dreaming:".into(),
                format!("  enabled: {}", status.enabled),
                format!("  trigger: {}", status.trigger),
                format!("  idle duration: {}s", status.idle_seconds),
                format!("  idle for: {}s", status.idle_for_seconds),
                format!("  waiting for idle: {}", status.waiting_for_idle),
                format!(
                    "  state: {}",
                    if status.running { "running" } else { "idle" }
                ),
                format!("  pending: {}", status.pending_runs),
                format!("  completed: {}", status.completed_runs),
                format!("  failed: {}", status.failed_runs),
                format!("  last dream: {}", none_if_empty(&status.last_dream_id)),
                format!("  last review: {}", none_if_empty(&status.last_review_id)),
                format!("  last error: {}", none_if_empty(&status.last_error)),
            ]
        }
        "run" => vec![if service.enqueue(DreamTrigger::Manual, None) {
            "Memory Dreaming cycle queued in the background.".into()
        } else {
            "Memory Dreaming cycle was not queued; enable Dreaming and verify its queue/model settings."
                .into()
        }],
        "research" => {
            let stored = serde_json::from_str::<Value>(rest)
                .map_err(|error| crate::error::MemoryError::invalid(error.to_string()))
                .and_then(|input| parse_manual_research(&input, ManualEvidenceOrigin::Interactive))
                .and_then(|(dream_id, evidence)| service.add_research(&dream_id, evidence));
            match stored {
                Ok(stored) => vec![
                    format!(
                        "Dream research successor stored for review: {}",
                        stored.researched_memory_id.as_deref().unwrap_or("none")
                    ),
                    format!(
                        "Research review: {}",
                        stored.research_review_id.as_deref().unwrap_or("none")
                    ),
                ],
                Err(error) => vec![
                    format!("Unable to store dream research: {error}"),
                    RESEARCH_USAGE.into(),
                ],
            }
        }
        _ => vec![USAGE.into()],
    }
}

/// RPC `memory` / `dream`: `params.operation` is `status` (default), `run`
/// or `research` (evidence fields inline in `params`).
pub fn dream_rpc(service: &DreamingService, params: &Value) -> Result<Value> {
    let operation = params
        .get("operation")
        .and_then(Value::as_str)
        .unwrap_or("status");
    match operation {
        "status" => Ok(service.status().to_json()),
        "run" => {
            if service.enqueue(DreamTrigger::Manual, None) {
                Ok(json!({"queued": true}))
            } else {
                invalid(NOT_QUEUED_RPC)
            }
        }
        "research" => {
            let (dream_id, evidence) = parse_manual_research(params, ManualEvidenceOrigin::Rpc)?;
            let stored = service.add_research(&dream_id, evidence)?;
            Ok(json!({
                "dream_id": stored.id,
                "research_id": stored.researched_memory_id,
                "research_review_id": stored.research_review_id,
            }))
        }
        _ => invalid("memory dream operation must be status, run, or research"),
    }
}
