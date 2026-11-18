//! Content normalisation, secret redaction, durable sanitisation and the JSON
//! encoding used for stored payloads.
//!
//! Stored JSON is written in the archived nlohmann form: compact, with object
//! keys in byte order. Keeping that form means rows written here and rows
//! written by the C++ binary compare equal, so the scrub pass does not report
//! formatting-only differences as changes.

use std::collections::HashMap;
use std::sync::LazyLock;

use regex::Regex;
use serde_json::{Map, Value};

use crate::model::MemoryType;

pub(crate) const MAXIMUM_DURABLE_TEXT: usize = 1024 * 1024;
pub(crate) const MAXIMUM_DURABLE_JSON: usize = 1024 * 1024;
pub(crate) const MAXIMUM_DURABLE_METADATA: usize = 32768;
pub(crate) const MAXIMUM_DURABLE_URI: usize = 4096;

/// FNV-1a 64-bit hash (archived `qt::foundation::stable_hash64`).
pub fn stable_hash64(value: &str) -> u64 {
    let mut hash: u64 = 14_695_981_039_346_656_037;
    for byte in value.bytes() {
        hash ^= u64::from(byte);
        hash = hash.wrapping_mul(1_099_511_628_211);
    }
    hash
}

/// Sixteen lowercase hex digits of [`stable_hash64`].
pub fn stable_hash_hex(value: &str) -> String {
    format!("{:016x}", stable_hash64(value))
}

/// Strips `\r`, collapses runs of three or more newlines to two and trims
/// surrounding whitespace.
pub fn normalize_content(content: &str) -> String {
    let mut out = String::with_capacity(content.len());
    let mut newline_run = 0usize;
    for ch in content.chars() {
        match ch {
            '\r' => {}
            '\n' => {
                if newline_run < 2 {
                    out.push(ch);
                }
                newline_run += 1;
            }
            _ => {
                newline_run = 0;
                out.push(ch);
            }
        }
    }
    out.trim_matches(|ch: char| ch.is_ascii_whitespace() || ch == '\u{0b}')
        .to_string()
}

struct Redactor {
    json_secret: Regex,
    patterns: Vec<Regex>,
}

static REDACTOR: LazyLock<Redactor> = LazyLock::new(|| {
    let compile = |pattern: &str| Regex::new(pattern).expect("static redaction pattern");
    Redactor {
        json_secret: compile(
            r#"(?i)("(?:password|secret|api[_-]?key|access[_-]?token|refresh[_-]?token|token|authorization)"\s*:\s*")([^"]*)(")"#,
        ),
        patterns: vec![
            compile(r"sk-(proj-)?[A-Za-z0-9_-]{20,}"),
            compile(r"sk-ant-[A-Za-z0-9_-]{20,}"),
            compile(r"ghp_[A-Za-z0-9]{20,}"),
            compile(r"github_pat_[A-Za-z0-9_]{22,}"),
            compile(r"(?i)Bearer\s+[A-Za-z0-9._~+/=-]{20,}"),
            compile(r"AKIA[0-9A-Z]{16}"),
            compile(r"(?i)(password|secret)\s*[:=]\s*[^\s]+"),
            compile(r#"/(?:home|Users|var|tmp|etc|workspace|root|mnt)/[^\s"'<>|]+"#),
            compile(r#"[A-Za-z]:\\[^\s"'<>|]+"#),
            compile(
                r"-----BEGIN (RSA |EC |DSA |OPENSSH )?PRIVATE KEY-----[\s\S]*?-----END (RSA |EC |DSA |OPENSSH )?PRIVATE KEY-----",
            ),
        ],
    }
});

/// Replaces credentials, private keys and absolute local paths with
/// `[REDACTED]`. Returns whether anything changed.
pub fn redact_secrets(content: &mut String) -> bool {
    let redactor = &*REDACTOR;
    let mut changed = false;
    let replaced = redactor
        .json_secret
        .replace_all(content, "${1}[REDACTED]${3}");
    if replaced != content.as_str() {
        *content = replaced.into_owned();
        changed = true;
    }
    for pattern in &redactor.patterns {
        let replaced = pattern.replace_all(content, "[REDACTED]");
        if replaced != content.as_str() {
            *content = replaced.into_owned();
            changed = true;
        }
    }
    changed
}

/// Redacted copy of `value`.
pub fn redacted(value: &str) -> String {
    let mut value = value.to_string();
    redact_secrets(&mut value);
    value
}

/// Infers a type from leading keywords; returns `fallback` otherwise.
pub fn infer_type(content: &str, fallback: MemoryType) -> MemoryType {
    let lower = content.to_ascii_lowercase();
    let starts = |prefix: &str| lower.starts_with(prefix);
    let has = |needle: &str| lower.contains(needle);
    if starts("todo") || starts("task") || has("next step") {
        return MemoryType::Todo;
    }
    if starts("bug") || starts("issue") || has("root cause") || has("fixed by") {
        return MemoryType::Bug;
    }
    if starts("decision:") || has("we decided") || has("decided to") {
        return MemoryType::Decision;
    }
    if starts("procedure:") || has("checklist") || has("steps:") {
        return MemoryType::Procedural;
    }
    if starts("constraint:") || has("must not") || has("must always") {
        return MemoryType::Constraint;
    }
    if starts("preference:") || has("i prefer") {
        return MemoryType::Preference;
    }
    fallback
}

/// Truncates to at most `limit` bytes on a character boundary.
pub(crate) fn truncate_bytes(value: &mut String, limit: usize) {
    if value.len() > limit {
        let mut end = limit;
        while !value.is_char_boundary(end) {
            end -= 1;
        }
        value.truncate(end);
    }
}

/// Redacts and bounds a durable text field.
pub(crate) fn sanitize_text(value: &str, limit: usize) -> String {
    let mut value = value.to_string();
    redact_secrets(&mut value);
    truncate_bytes(&mut value, limit);
    value
}

pub(crate) fn sanitize_meta(value: &str) -> String {
    sanitize_text(value, MAXIMUM_DURABLE_METADATA)
}

fn sensitive_json_key(key: &str) -> bool {
    let key = key.to_ascii_lowercase();
    [
        "password",
        "secret",
        "api_key",
        "apikey",
        "access_token",
        "refresh_token",
    ]
    .iter()
    .any(|needle| key.contains(needle))
        || key == "token"
        || key == "authorization"
}

fn sanitize_json_value(value: &mut Value) {
    match value {
        Value::Object(map) => {
            for (key, item) in map.iter_mut() {
                if sensitive_json_key(key) {
                    *item = Value::String("[REDACTED]".into());
                } else {
                    sanitize_json_value(item);
                }
            }
        }
        Value::Array(items) => items.iter_mut().for_each(sanitize_json_value),
        Value::String(text) => *text = sanitize_meta(text),
        _ => {}
    }
}

/// Redacts sensitive keys and string values of a stored JSON payload and
/// bounds its size. Unparseable input is wrapped as `_qt_unparsed`.
pub(crate) fn sanitize_json(value: &str, limit: usize) -> String {
    let Ok(mut parsed) = serde_json::from_str::<Value>(value) else {
        let wrapped = sanitize_text(&sanitize_text(value, limit / 2), limit / 2);
        return dump(&serde_json::json!({ "_qt_unparsed": wrapped }));
    };
    sanitize_json_value(&mut parsed);
    let encoded = dump(&parsed);
    if encoded.len() <= limit {
        return encoded;
    }
    dump(&serde_json::json!({ "_qt_truncated": true, "original_bytes": encoded.len() }))
}

pub(crate) fn sanitize_json_default(value: &str) -> String {
    sanitize_json(value, MAXIMUM_DURABLE_JSON)
}

/// Copy of `value` with every object's keys in byte order.
pub fn canonical(value: &Value) -> Value {
    match value {
        Value::Object(map) => {
            let mut keys: Vec<&String> = map.keys().collect();
            keys.sort();
            let mut sorted = Map::new();
            for key in keys {
                sorted.insert(key.clone(), canonical(&map[key]));
            }
            Value::Object(sorted)
        }
        Value::Array(items) => Value::Array(items.iter().map(canonical).collect()),
        other => other.clone(),
    }
}

/// Compact JSON with sorted keys (the archived nlohmann `dump()` form).
pub fn dump(value: &Value) -> String {
    canonical(value).to_string()
}

/// Replaces every string leaf equal to a mapped ID, consulting maps in order.
pub(crate) fn remap_json_ids(value: &mut Value, maps: &[&HashMap<String, String>]) {
    match value {
        Value::Object(map) => map.values_mut().for_each(|item| remap_json_ids(item, maps)),
        Value::Array(items) => items.iter_mut().for_each(|item| remap_json_ids(item, maps)),
        Value::String(text) => {
            if let Some(mapped) = maps.iter().find_map(|map| map.get(text.as_str())) {
                *text = mapped.clone();
            }
        }
        _ => {}
    }
}

pub(crate) fn remapped_json(value: &str, maps: &[&HashMap<String, String>]) -> String {
    match serde_json::from_str::<Value>(value) {
        Ok(mut parsed) => {
            remap_json_ids(&mut parsed, maps);
            sanitize_json_default(&dump(&parsed))
        }
        Err(_) => sanitize_json_default(value),
    }
}

/// Lowercases ASCII only, as the archived capture-receipt canonicaliser does.
pub(crate) fn ascii_lower(value: &str) -> String {
    value.to_ascii_lowercase()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn stable_hash_matches_fnv1a() {
        assert_eq!(stable_hash_hex(""), "cbf29ce484222325");
        assert_eq!(stable_hash_hex("a"), "af63dc4c8601ec8c");
    }

    #[test]
    fn normalizes_content() {
        assert_eq!(normalize_content("  a\r\n\n\n\nb  \n"), "a\n\nb");
    }

    #[test]
    fn redacts_credentials_and_paths() {
        let mut text = String::from(
            r#"{"api_key": "abc", "token":"t"} sk-abcdefghijklmnopqrstuvwxyz ghp_abcdefghijklmnopqrstuv Bearer abcdefghijklmnopqrstuvwxyz AKIAABCDEFGHIJKLMNOP password=hunter2 /home/alice/secret.txt C:\Users\bob\x.txt"#,
        );
        assert!(redact_secrets(&mut text));
        assert_eq!(
            text,
            r#"{"api_key": "[REDACTED]", "token":"[REDACTED]"} [REDACTED] [REDACTED] [REDACTED] [REDACTED] [REDACTED] [REDACTED] [REDACTED]"#
        );
        let mut key =
            String::from("x -----BEGIN RSA PRIVATE KEY-----\nabc\n-----END RSA PRIVATE KEY----- y");
        assert!(redact_secrets(&mut key));
        assert_eq!(key, "x [REDACTED] y");
        let mut clean = String::from("nothing here");
        assert!(!redact_secrets(&mut clean));
    }

    #[test]
    fn infers_types() {
        assert_eq!(
            infer_type("TODO: ship", MemoryType::Observation),
            MemoryType::Todo
        );
        assert_eq!(
            infer_type("the root cause was x", MemoryType::Observation),
            MemoryType::Bug
        );
        assert_eq!(
            infer_type("We decided to use Rust", MemoryType::Observation),
            MemoryType::Decision
        );
        assert_eq!(
            infer_type("Steps: a, b", MemoryType::Observation),
            MemoryType::Procedural
        );
        assert_eq!(
            infer_type("You must not push", MemoryType::Observation),
            MemoryType::Constraint
        );
        assert_eq!(
            infer_type("I prefer tabs", MemoryType::Observation),
            MemoryType::Preference
        );
        assert_eq!(
            infer_type("plain", MemoryType::Semantic),
            MemoryType::Semantic
        );
    }

    #[test]
    fn sanitize_json_matches_archived_shapes() {
        assert_eq!(
            sanitize_json_default(r#"{"z":1,"password":"x","a":{"path":"/home/u/f"}}"#),
            r#"{"a":{"path":"[REDACTED]"},"password":"[REDACTED]","z":1}"#
        );
        assert_eq!(
            sanitize_json_default("not json"),
            r#"{"_qt_unparsed":"not json"}"#
        );
        assert_eq!(
            sanitize_json(r#"{"a":"0123456789"}"#, 10),
            r#"{"_qt_truncated":true,"original_bytes":18}"#
        );
    }

    #[test]
    fn truncation_respects_char_boundaries() {
        let mut value = String::from("héllo");
        truncate_bytes(&mut value, 2);
        assert_eq!(value, "h");
    }
}
