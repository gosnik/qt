//! Keyword extraction and transcript chunking for captured episodes
//! (archived `ingestion_service.cpp`).

use std::collections::HashSet;

use crate::error::{MemoryError, Result};
use crate::model::*;
use crate::service::MemoryService;
use crate::text::{infer_type, normalize_content, redact_secrets};

/// Lines containing one of these (case-insensitively) become candidates.
pub const SIGNALS: [&str; 17] = [
    "decision",
    "decided",
    "must ",
    "must not",
    "always ",
    "never ",
    "root cause",
    "fixed by",
    "regression",
    "procedure",
    "steps:",
    "preference",
    "i prefer",
    "correction",
    "actually",
    "todo",
    "next step",
];

/// One extracted candidate.
#[derive(Debug, Clone, PartialEq)]
pub struct IngestionCandidate {
    pub content: String,
    pub memory_type: MemoryType,
    pub tags: Vec<String>,
    pub confidence: f64,
    pub source_line: String,
}

/// Text to extract candidates from.
#[derive(Debug, Clone, PartialEq)]
pub struct IngestionRequest {
    pub content: String,
    pub source: String,
    pub provenance: Provenance,
    pub tags: Vec<String>,
    pub minimum_confidence: f64,
    pub fallback_to_raw: bool,
}

impl Default for IngestionRequest {
    fn default() -> Self {
        Self {
            content: String::new(),
            source: "process:text-block".into(),
            provenance: Provenance::default(),
            tags: Vec::new(),
            minimum_confidence: 0.6,
            fallback_to_raw: false,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct IngestionResult {
    pub candidates: Vec<IngestionCandidate>,
    pub stored_ids: Vec<String>,
}

fn is_space(ch: char) -> bool {
    matches!(ch, ' ' | '\t' | '\n' | '\u{0b}' | '\u{0c}' | '\r')
}

fn trim(value: &str) -> &str {
    value.trim_matches(is_space)
}

/// `std::getline` semantics: no trailing empty line after a final newline.
fn lines(value: &str) -> Vec<&str> {
    let mut parts: Vec<&str> = value.split('\n').collect();
    if value.ends_with('\n') || value.is_empty() {
        parts.pop();
    }
    parts
}

fn meaningful(line: &str) -> bool {
    if line.len() < 12 {
        return false;
    }
    let lower = line.to_ascii_lowercase();
    SIGNALS.iter().any(|signal| lower.contains(signal))
}

/// Lowercased ASCII alphanumerics; equal keys are duplicates.
pub fn canonical_key(value: &str) -> String {
    value
        .chars()
        .filter(char::is_ascii_alphanumeric)
        .map(|ch| ch.to_ascii_lowercase())
        .collect()
}

fn chunk_end(value: &str, offset: usize, maximum: usize) -> usize {
    let mut end = if maximum >= value.len() - offset {
        value.len()
    } else {
        offset + maximum
    };
    if end == value.len() {
        return end;
    }
    while end > offset && !value.is_char_boundary(end) {
        end -= 1;
    }
    if end != offset {
        return end;
    }
    let mut next = offset + 1;
    while next < value.len() && !value.is_char_boundary(next) {
        next += 1;
    }
    next
}

fn bounded(value: &str, maximum: usize) -> String {
    let end = chunk_end(value, 0, maximum);
    if end > maximum {
        "?".into()
    } else {
        value[..end].to_string()
    }
}

fn append_unit(chunks: &mut Vec<String>, current: &mut String, unit: &str, max: usize) {
    let cleaned = trim(unit);
    if cleaned.is_empty() {
        return;
    }
    if !current.is_empty() && current.len() + cleaned.len() + 2 > max {
        chunks.push(trim(current).to_string());
        current.clear();
    }
    if cleaned.len() <= max {
        if !current.is_empty() {
            current.push_str("\n\n");
        }
        current.push_str(cleaned);
        return;
    }
    if !current.is_empty() {
        chunks.push(trim(current).to_string());
        current.clear();
    }
    let mut offset = 0usize;
    while offset < cleaned.len() {
        let end = chunk_end(cleaned, offset, max);
        chunks.push(if end - offset > max {
            "?".into()
        } else {
            trim(&cleaned[offset..end]).to_string()
        });
        offset = end;
    }
}

/// Redacts and splits a transcript into chunks of at most `max_characters`
/// bytes along `### ` sections (or blank-line paragraphs), prefixing Codex
/// transcript chunks with their heading.
pub fn chunk_transcript(content: &str, max_characters: usize) -> Result<Vec<String>> {
    if max_characters == 0 {
        return Err(MemoryError::invalid(
            "transcript chunk maximum must be non-zero",
        ));
    }
    let mut redacted = content.to_string();
    redact_secrets(&mut redacted);
    let mut units: Vec<String> = Vec::new();
    let mut current = String::new();
    let mut saw_section = false;
    for line in lines(&redacted) {
        if line.starts_with("### ") {
            saw_section = true;
            let section = trim(&current);
            if !section.is_empty() {
                units.push(section.to_string());
            }
            current.clear();
        }
        current.push_str(line);
        current.push('\n');
    }
    let tail = trim(&current);
    if !tail.is_empty() {
        units.push(tail.to_string());
    }
    if !saw_section {
        units.clear();
        let mut paragraph = String::new();
        for line in lines(&redacted) {
            if trim(line).is_empty() {
                let completed = trim(&paragraph);
                if !completed.is_empty() {
                    units.push(completed.to_string());
                }
                paragraph.clear();
                continue;
            }
            if !paragraph.is_empty() {
                paragraph.push('\n');
            }
            paragraph.push_str(line);
        }
        let completed = trim(&paragraph);
        if !completed.is_empty() {
            units.push(completed.to_string());
        }
    }
    let mut chunks = Vec::new();
    let mut chunk = String::new();
    for unit in &units {
        append_unit(&mut chunks, &mut chunk, unit, max_characters);
    }
    if !trim(&chunk).is_empty() {
        chunks.push(trim(&chunk).to_string());
    }
    if chunks.is_empty() && !trim(&redacted).is_empty() {
        let mut chunk = String::new();
        append_unit(&mut chunks, &mut chunk, &redacted, max_characters);
        if !trim(&chunk).is_empty() {
            chunks.push(trim(&chunk).to_string());
        }
    }
    const HEADING: &str = "# Codex Session Transcript";
    if redacted.contains(HEADING) {
        let prefix = "# Codex Session Transcript\n\n## Events\n\n";
        let mut contextualized = Vec::new();
        for item in chunks {
            if item.contains(HEADING) || prefix.len() >= max_characters {
                contextualized.push(item);
                continue;
            }
            let available = max_characters - prefix.len();
            let mut offset = 0usize;
            while offset < item.len() {
                let end = chunk_end(&item, offset, available);
                contextualized.push(format!(
                    "{prefix}{}",
                    if end - offset > available {
                        "?"
                    } else {
                        trim(&item[offset..end])
                    }
                ));
                offset = end;
            }
        }
        chunks = contextualized;
    }
    Ok(chunks)
}

/// Extracts and optionally stores candidate memories from free text.
#[derive(Debug, Clone)]
pub struct MemoryIngestionService {
    memories: MemoryService,
}

impl MemoryIngestionService {
    pub fn new(memories: MemoryService) -> Self {
        Self { memories }
    }

    /// Signal-bearing lines (confidence 0.72, at most 1200 bytes), or the
    /// whole text as a 0.6 fallback when requested.
    pub fn extract(&self, request: &IngestionRequest) -> Result<Vec<IngestionCandidate>> {
        extract(request)
    }

    /// Extracts candidates and stores non-duplicates as `candidate` memories.
    pub fn ingest(
        &self,
        scope: &MemoryScope,
        request: &IngestionRequest,
        persist: bool,
    ) -> Result<IngestionResult> {
        let mut result = IngestionResult {
            candidates: extract(request)?,
            stored_ids: Vec::new(),
        };
        if !persist {
            return Ok(result);
        }
        for candidate in &result.candidates {
            let key = canonical_key(&candidate.content);
            let existing = MemoryQuery {
                text: candidate.content.clone(),
                limit: 10,
                policy: RecallPolicy::Strict,
                ..Default::default()
            };
            if self
                .memories
                .query(scope, &existing)?
                .iter()
                .any(|memory| canonical_key(&memory.content) == key)
            {
                continue;
            }
            result.stored_ids.push(self.memories.remember(
                scope,
                MemoryWrite {
                    memory_type: candidate.memory_type,
                    status: MemoryStatus::Candidate,
                    provenance: request.provenance.clone(),
                    source: request.source.clone(),
                    content: candidate.content.clone(),
                    tags: candidate.tags.clone(),
                    confidence: candidate.confidence,
                    quality_score: 0.0,
                },
            )?);
        }
        Ok(result)
    }
}

/// See [`MemoryIngestionService::extract`].
pub fn extract(request: &IngestionRequest) -> Result<Vec<IngestionCandidate>> {
    let mut result = Vec::new();
    let mut seen = HashSet::new();
    for chunk in chunk_transcript(&request.content, 6000)? {
        for line in lines(&chunk) {
            let line = normalize_content(line);
            if !meaningful(&line) {
                continue;
            }
            let content = bounded(&line, 1200);
            let memory_type = infer_type(&content, MemoryType::Observation);
            let mut tags = request.tags.clone();
            tags.push(format!("processed:{memory_type}"));
            let candidate = IngestionCandidate {
                memory_type,
                confidence: 0.72,
                source_line: bounded(&line, 200),
                tags,
                content,
            };
            if candidate.confidence >= request.minimum_confidence
                && seen.insert(canonical_key(&candidate.content))
            {
                result.push(candidate);
            }
        }
    }
    if result.is_empty() && request.fallback_to_raw {
        let content = normalize_content(&request.content);
        if !content.is_empty() && 0.6 >= request.minimum_confidence {
            let bounded_content = bounded(&content, 1200);
            let mut tags = request.tags.clone();
            tags.push("processed:fallback".into());
            result.push(IngestionCandidate {
                memory_type: infer_type(&bounded_content, MemoryType::Observation),
                content: bounded_content,
                confidence: 0.6,
                source_line: bounded(&content, 200),
                tags,
            });
        }
    }
    Ok(result)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn extracts_signal_lines_once() {
        let request = IngestionRequest {
            content: "## user\n\nWe decided to use SQLite for storage.\nhello\nWe decided to use SQLite for storage!\nRoot cause was a race.\n".into(),
            tags: vec!["episode".into()],
            ..Default::default()
        };
        let candidates = extract(&request).unwrap();
        assert_eq!(candidates.len(), 2);
        assert_eq!(
            candidates[0].content,
            "We decided to use SQLite for storage."
        );
        assert_eq!(candidates[0].memory_type, MemoryType::Decision);
        assert_eq!(candidates[0].tags, vec!["episode", "processed:decision"]);
        assert_eq!(candidates[1].memory_type, MemoryType::Bug);
        assert_eq!(candidates[0].confidence, 0.72);
        let fallback = extract(&IngestionRequest {
            content: "plain words".into(),
            fallback_to_raw: true,
            ..Default::default()
        })
        .unwrap();
        assert_eq!(fallback[0].tags, vec!["processed:fallback"]);
    }

    #[test]
    fn chunks_by_section_and_size() {
        let text = "### one\nalpha\n### two\nbeta\n";
        assert_eq!(
            chunk_transcript(text, 6000).unwrap(),
            vec!["### one\nalpha\n\n### two\nbeta"]
        );
        assert_eq!(
            chunk_transcript(text, 13).unwrap(),
            vec!["### one\nalpha", "### two\nbeta"]
        );
        assert_eq!(chunk_transcript("abcdef", 4).unwrap(), vec!["abcd", "ef"]);
        assert_eq!(
            chunk_transcript("p1\n\np2", 6000).unwrap(),
            vec!["p1\n\np2"]
        );
        assert!(chunk_transcript("x", 0).is_err());
        let codex =
            chunk_transcript("# Codex Session Transcript\n\nintro\n\nbody text", 60).unwrap();
        assert!(
            codex
                .iter()
                .all(|chunk| chunk.starts_with("# Codex Session Transcript"))
        );
    }
}
