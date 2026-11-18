//! Citation validation of a research child's run (archived
//! `validate_dream_research_output`).
//!
//! A cited URL is accepted only when it was opened: a successful `web_run`
//! open/click reference (references carrying a `query` are search hits and
//! only count as discovered), or a successful 2xx `web_fetch` of a URL that a
//! earlier `web_search` returned. More than three tool calls, any other tool,
//! or a final answer outside the strict schema rejects the whole run.

use std::collections::{BTreeMap, BTreeSet};

use serde_json::{Value, json};

use crate::dreaming::child::{ChildEvent, ChildOutcome};
use crate::text::{dump, normalize_content, redact_secrets, stable_hash_hex};

use super::tools::RESEARCH_MAX_TOOL_CALLS;

/// Maximum bytes of opened source text kept per citation.
pub const EVIDENCE_BYTES_PER_SOURCE: usize = 4096;
/// Maximum summary bytes.
pub const MAX_SUMMARY_BYTES: usize = 4096;
/// Maximum cited URLs.
pub const MAX_SOURCE_URLS: usize = 8;

/// Research output the parent accepted.
#[derive(Debug, Clone, PartialEq)]
pub struct ValidatedResearch {
    pub summary: String,
    /// Distinct cited URLs in answer order.
    pub source_urls: Vec<String>,
    pub confidence: f64,
    /// Hash of every successful tool result's details.
    pub evidence_hash: String,
    /// `[{url, content}]` with bounded opened text, for the verifier.
    pub documents: Value,
}

/// The first 4096 bytes of `content` on a character boundary.
pub fn bounded_evidence(content: &str) -> String {
    let mut end = content.len().min(EVIDENCE_BYTES_PER_SOURCE);
    while !content.is_char_boundary(end) {
        end -= 1;
    }
    content[..end].to_string()
}

const RESEARCH_TOOLS: [&str; 3] = ["web_run", "web_search", "web_fetch"];

/// Validates a completed research run. `Err` carries the archived rejection
/// code (`non_web_tool_used`, `web_tool_not_used`, `unverified_source_url`…).
pub fn validate_research_output(outcome: &ChildOutcome) -> Result<ValidatedResearch, String> {
    let mut tool_calls = 0usize;
    let mut discovered: BTreeSet<String> = BTreeSet::new();
    let mut opened: BTreeSet<String> = BTreeSet::new();
    let mut opened_content: BTreeMap<String, String> = BTreeMap::new();
    let mut evidence: Vec<Value> = Vec::new();
    for event in &outcome.events {
        match event {
            ChildEvent::ToolStart { tool_name } => {
                tool_calls += 1;
                if !RESEARCH_TOOLS.contains(&tool_name.as_str()) {
                    return Err("non_web_tool_used".into());
                }
            }
            ChildEvent::ToolEnd {
                tool_name,
                is_error,
                details,
            } => {
                if !RESEARCH_TOOLS.contains(&tool_name.as_str()) {
                    return Err("non_web_tool_used".into());
                }
                let Some(details) = details.as_ref().filter(|_| !*is_error) else {
                    continue;
                };
                evidence.push(details.clone());
                match tool_name.as_str() {
                    "web_search" => {
                        for result in details
                            .get("results")
                            .and_then(Value::as_array)
                            .into_iter()
                            .flatten()
                        {
                            if let Some(url) = result.get("url").and_then(Value::as_str) {
                                discovered.insert(url.to_string());
                            }
                        }
                    }
                    "web_fetch" => {
                        let status = details.get("status").and_then(Value::as_i64).unwrap_or(0);
                        if let Some(url) = details.get("url").and_then(Value::as_str)
                            && (200..300).contains(&status)
                            && discovered.contains(url)
                        {
                            opened.insert(url.to_string());
                            if let Some(content) = details.get("content").and_then(Value::as_str) {
                                opened_content.insert(url.to_string(), bounded_evidence(content));
                            }
                        }
                    }
                    _ => {
                        for reference in details
                            .get("references")
                            .and_then(Value::as_array)
                            .into_iter()
                            .flatten()
                        {
                            let Some(url) = reference.get("url").and_then(Value::as_str) else {
                                continue;
                            };
                            discovered.insert(url.to_string());
                            if reference.get("query").is_some() {
                                continue;
                            }
                            opened.insert(url.to_string());
                            if let Some(content) = reference.get("content").and_then(Value::as_str)
                            {
                                opened_content.insert(url.to_string(), bounded_evidence(content));
                            }
                        }
                    }
                }
            }
        }
    }
    if tool_calls == 0 {
        return Err("web_tool_not_used".into());
    }
    if tool_calls > RESEARCH_MAX_TOOL_CALLS {
        return Err("web_tool_budget_exceeded".into());
    }
    let parsed: Value =
        serde_json::from_str(&outcome.final_output).map_err(|_| "invalid_json".to_string())?;
    let object = parsed
        .as_object()
        .filter(|object| {
            object.len() == 4
                && object.get("version").and_then(Value::as_i64) == Some(1)
                && object.get("summary").is_some_and(Value::is_string)
                && object.get("source_urls").is_some_and(Value::is_array)
                && object.get("confidence").is_some_and(Value::is_number)
        })
        .ok_or("schema_mismatch")?;
    let summary = normalize_content(object["summary"].as_str().unwrap_or_default());
    let confidence = object["confidence"].as_f64().unwrap_or(f64::NAN);
    if summary.is_empty() || summary.len() > MAX_SUMMARY_BYTES {
        return Err("invalid_summary".into());
    }
    if redact_secrets(&mut summary.clone()) {
        return Err("sensitive_content".into());
    }
    if !confidence.is_finite() || !(0.0..=1.0).contains(&confidence) {
        return Err("invalid_confidence".into());
    }
    let urls = object["source_urls"]
        .as_array()
        .map(Vec::as_slice)
        .unwrap_or_default();
    if urls.is_empty() || urls.len() > MAX_SOURCE_URLS {
        return Err("invalid_source_count".into());
    }
    let mut source_urls: Vec<String> = Vec::new();
    for value in urls {
        let url = value.as_str().ok_or("invalid_source_url")?;
        if !(url.starts_with("https://") || url.starts_with("http://")) || !opened.contains(url) {
            return Err("unverified_source_url".into());
        }
        if !source_urls.iter().any(|seen| seen == url) {
            source_urls.push(url.to_string());
        }
    }
    if source_urls.is_empty() {
        return Err("no_verified_sources".into());
    }
    let documents: Vec<Value> = source_urls
        .iter()
        .map(|url| {
            json!({
                "url": url,
                "content": opened_content.get(url).cloned().unwrap_or_default(),
            })
        })
        .collect();
    Ok(ValidatedResearch {
        summary,
        source_urls,
        confidence,
        evidence_hash: stable_hash_hex(&dump(&Value::Array(evidence))),
        documents: Value::Array(documents),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn start(name: &str) -> ChildEvent {
        ChildEvent::ToolStart {
            tool_name: name.into(),
        }
    }

    fn end(name: &str, details: Value) -> ChildEvent {
        ChildEvent::ToolEnd {
            tool_name: name.into(),
            is_error: false,
            details: Some(details),
        }
    }

    fn answer(urls: &[&str]) -> String {
        json!({"version": 1, "summary": "Public docs support it.", "source_urls": urls, "confidence": 0.6})
            .to_string()
    }

    fn outcome(events: Vec<ChildEvent>, output: String) -> ChildOutcome {
        let mut outcome = ChildOutcome::completed(output);
        outcome.events = events;
        outcome
    }

    #[test]
    fn accepts_fetched_search_results_and_opened_web_run_refs() {
        let events = vec![
            start("web_search"),
            end(
                "web_search",
                json!({"results": [{"title": "A", "url": "https://a.example/doc"}]}),
            ),
            start("web_fetch"),
            end(
                "web_fetch",
                json!({"url": "https://a.example/doc", "status": 200, "content": "A says the pattern holds."}),
            ),
            start("web_run"),
            end(
                "web_run",
                json!({"references": [
                    {"ref_id": "turn0search0", "url": "https://b.example/x", "query": "q"},
                    {"ref_id": "turn0fetch0", "url": "https://c.example/y", "content": "C text"}
                ]}),
            ),
        ];
        let result = validate_research_output(&outcome(
            events,
            answer(&[
                "https://a.example/doc",
                "https://c.example/y",
                "https://a.example/doc",
            ]),
        ))
        .unwrap();
        assert_eq!(
            result.source_urls,
            ["https://a.example/doc", "https://c.example/y"]
        );
        assert_eq!(
            result.documents,
            json!([
                {"url": "https://a.example/doc", "content": "A says the pattern holds."},
                {"url": "https://c.example/y", "content": "C text"}
            ])
        );
        assert_eq!(result.evidence_hash.len(), 16);
        assert_eq!(result.confidence, 0.6);
    }

    #[test]
    fn rejects_unopened_undiscovered_and_disallowed_sources() {
        let search = || {
            vec![
                start("web_search"),
                end(
                    "web_search",
                    json!({"results": [{"url": "https://a.example/doc"}]}),
                ),
            ]
        };
        let reject = |events: Vec<ChildEvent>, output: String| {
            validate_research_output(&outcome(events, output)).unwrap_err()
        };
        assert_eq!(
            reject(search(), answer(&["https://a.example/doc"])),
            "unverified_source_url"
        );
        let mut undiscovered = vec![start("web_fetch")];
        undiscovered.push(end(
            "web_fetch",
            json!({"url": "https://z.example", "status": 200}),
        ));
        assert_eq!(
            reject(undiscovered, answer(&["https://z.example"])),
            "unverified_source_url"
        );
        let mut failed = search();
        failed.push(start("web_fetch"));
        failed.push(end(
            "web_fetch",
            json!({"url": "https://a.example/doc", "status": 404}),
        ));
        assert_eq!(
            reject(failed, answer(&["https://a.example/doc"])),
            "unverified_source_url"
        );
        let mut errored = search();
        errored.push(start("web_fetch"));
        errored.push(ChildEvent::ToolEnd {
            tool_name: "web_fetch".into(),
            is_error: true,
            details: Some(json!({"url": "https://a.example/doc", "status": 200})),
        });
        assert_eq!(
            reject(errored, answer(&["https://a.example/doc"])),
            "unverified_source_url"
        );
        assert_eq!(
            reject(vec![start("bash")], answer(&[])),
            "non_web_tool_used"
        );
        assert_eq!(reject(Vec::new(), answer(&[])), "web_tool_not_used");
        assert_eq!(
            reject(vec![start("web_run"); 4], answer(&[])),
            "web_tool_budget_exceeded"
        );
        assert_eq!(reject(search(), "prose".into()), "invalid_json");
        assert_eq!(
            reject(
                search(),
                json!({"version": 1, "summary": "s", "source_urls": [], "confidence": 0.5, "x": 1})
                    .to_string()
            ),
            "schema_mismatch"
        );
        assert_eq!(
            reject(
                search(),
                json!({"version": 1, "summary": "  ", "source_urls": [], "confidence": 0.5})
                    .to_string()
            ),
            "invalid_summary"
        );
        assert_eq!(
            reject(search(), json!({"version": 1, "summary": "key sk-abcdefghijklmnopqrstuvwxyz012", "source_urls": [], "confidence": 0.5}).to_string()),
            "sensitive_content"
        );
        assert_eq!(
            reject(
                search(),
                json!({"version": 1, "summary": "s", "source_urls": [], "confidence": 2})
                    .to_string()
            ),
            "invalid_confidence"
        );
        assert_eq!(reject(search(), answer(&[])), "invalid_source_count");
        assert_eq!(
            reject(
                search(),
                json!({"version": 1, "summary": "s", "source_urls": [1], "confidence": 0.5})
                    .to_string()
            ),
            "invalid_source_url"
        );
    }

    #[test]
    fn evidence_is_bounded_on_char_boundaries() {
        let text = "é".repeat(3000);
        let bounded = bounded_evidence(&text);
        assert!(bounded.len() <= 4096);
        assert_eq!(bounded.len(), 4096);
        let odd = format!("a{}", "é".repeat(3000));
        assert_eq!(bounded_evidence(&odd).len(), 4095);
    }
}
