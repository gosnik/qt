//! Caller-supplied research evidence (archived `/memory dream research
//! <json>` and the RPC `memory` `dream` action with `operation: "research"`).

use serde_json::Value;

use crate::error::{Result, invalid};
use crate::model::SourceType;
use crate::post_processing::DreamResearchEvidence;

/// Who supplied the evidence; recorded as `provenance.created_by`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ManualEvidenceOrigin {
    Interactive,
    Rpc,
}

impl ManualEvidenceOrigin {
    pub fn created_by(self) -> &'static str {
        match self {
            Self::Interactive => "user:interactive",
            Self::Rpc => "user:rpc",
        }
    }

    fn missing_fields_message(self) -> &'static str {
        match self {
            Self::Interactive => "research JSON requires string dream_id and summary",
            Self::Rpc => "memory dream research requires string dream_id and summary",
        }
    }
}

fn strings(value: &Value, key: &str) -> Vec<String> {
    value
        .get(key)
        .and_then(Value::as_array)
        .map(|items| {
            items
                .iter()
                .filter_map(Value::as_str)
                .map(str::to_string)
                .collect()
        })
        .unwrap_or_default()
}

fn text(value: &Value, key: &str, fallback: &str) -> String {
    value
        .get(key)
        .and_then(Value::as_str)
        .unwrap_or(fallback)
        .to_string()
}

fn number(value: &Value, key: &str, fallback: f64) -> f64 {
    value.get(key).and_then(Value::as_f64).unwrap_or(fallback)
}

/// Parses the evidence object: string `dream_id` and `summary` are required;
/// `source_memory_ids`, `tags`, `confidence` (0.7), `source_type`
/// (`document`), `source_uri`, `source_hash`, `source_span` and
/// `evidence_strength` (0.7) are optional. Returns the Dream ID and evidence.
pub fn parse_manual_research(
    input: &Value,
    origin: ManualEvidenceOrigin,
) -> Result<(String, DreamResearchEvidence)> {
    let dream_id = input.get("dream_id").and_then(Value::as_str);
    let summary = input.get("summary").and_then(Value::as_str);
    let (Some(dream_id), Some(summary)) = (dream_id, summary) else {
        return invalid(origin.missing_fields_message());
    };
    let mut evidence = DreamResearchEvidence {
        summary: summary.to_string(),
        source_memory_ids: strings(input, "source_memory_ids"),
        tags: strings(input, "tags"),
        confidence: number(input, "confidence", 0.7),
        ..Default::default()
    };
    evidence.provenance.source_type = SourceType::parse(&text(input, "source_type", "document"));
    evidence.provenance.source_uri = text(input, "source_uri", "");
    evidence.provenance.source_hash = text(input, "source_hash", "");
    evidence.provenance.source_span = text(input, "source_span", "");
    evidence.provenance.created_by = origin.created_by().into();
    evidence.provenance.evidence_strength = number(input, "evidence_strength", 0.7);
    Ok((dream_id.to_string(), evidence))
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn parses_defaults_and_optional_fields() {
        let (dream, evidence) = parse_manual_research(
            &json!({"dream_id": "d1", "summary": "s", "source_uri": "https://x"}),
            ManualEvidenceOrigin::Interactive,
        )
        .unwrap();
        assert_eq!(dream, "d1");
        assert_eq!(evidence.confidence, 0.7);
        assert_eq!(evidence.provenance.source_type, SourceType::Document);
        assert_eq!(evidence.provenance.created_by, "user:interactive");
        assert_eq!(evidence.provenance.evidence_strength, 0.7);
        assert_eq!(evidence.provenance.source_uri, "https://x");

        let (_, evidence) = parse_manual_research(
            &json!({"dream_id": "d1", "summary": "s", "source_memory_ids": ["a", 1, "b"],
                    "tags": ["t"], "confidence": 0.4, "source_type": "pr",
                    "evidence_strength": 0.9}),
            ManualEvidenceOrigin::Rpc,
        )
        .unwrap();
        assert_eq!(evidence.source_memory_ids, ["a", "b"]);
        assert_eq!(evidence.tags, ["t"]);
        assert_eq!(evidence.provenance.source_type, SourceType::PullRequest);
        assert_eq!(evidence.provenance.created_by, "user:rpc");
    }

    #[test]
    fn requires_string_dream_id_and_summary() {
        assert_eq!(
            parse_manual_research(
                &json!({"dream_id": 1, "summary": "s"}),
                ManualEvidenceOrigin::Interactive
            )
            .unwrap_err()
            .to_string(),
            "research JSON requires string dream_id and summary"
        );
        assert_eq!(
            parse_manual_research(&json!([]), ManualEvidenceOrigin::Rpc)
                .unwrap_err()
                .to_string(),
            "memory dream research requires string dream_id and summary"
        );
    }
}
