//! The YAML subset accepted for `settings.yaml` / `settings.yml`.
//!
//! The archived qt read YAML settings with its indentation-based config
//! parser (`foundation/config.cpp`): nested `key: value` maps, `- item`
//! sequences, `#` comments and quoted scalars. This module parses the same
//! subset into JSON. Scalars are typed (booleans `true/false/yes/no/on/off`,
//! integers, floats, `null`/`~`) except for keys whose values are always
//! strings in qt settings.

use serde_json::{Map, Number, Value};

/// Keys whose scalar values stay strings even when they look like numbers
/// or booleans (model ids, paths, modes).
const STRING_KEYS: &[&str] = &[
    "cwd",
    "workspaceRoot",
    "sessionDir",
    "agentHome",
    "defaultProvider",
    "defaultModel",
    "defaultThinkingLevel",
    "theme",
    "steeringMode",
    "followUpMode",
    "transport",
    "shellPath",
    "shellCommandPrefix",
    "id",
    "name",
    "baseUrl",
    "apiKey",
    "model",
];

/// Keys holding string arrays (each item stays a string).
const STRING_ARRAY_KEYS: &[&str] = &[
    "promptRoots",
    "skillRoots",
    "themeRoots",
    "prompts",
    "skills",
    "themes",
    "npmCommand",
    "enabledModels",
    "toolEntryCompactionEnabledTools",
];

#[derive(Debug, Clone)]
struct Line {
    number: usize,
    indent: usize,
    text: String,
}

fn strip_comment(line: &str) -> &str {
    let mut quote: Option<char> = None;
    let mut previous_space = true;
    for (index, ch) in line.char_indices() {
        match quote {
            Some(q) if ch == q => quote = None,
            Some(_) => {}
            None if ch == '"' || ch == '\'' => quote = Some(ch),
            None if ch == '#' && previous_space => return &line[..index],
            None => {}
        }
        previous_space = ch.is_whitespace();
    }
    line
}

fn collect_lines(content: &str) -> Result<Vec<Line>, String> {
    let mut lines = Vec::new();
    for (index, raw) in content.lines().enumerate() {
        let raw = raw.strip_suffix('\r').unwrap_or(raw);
        if raw.contains('\t') && raw.trim_start_matches(' ').starts_with('\t') {
            return Err(format!(
                "line {}: tabs are not allowed for indentation",
                index + 1
            ));
        }
        let stripped = strip_comment(raw).trim_end();
        if stripped.trim().is_empty() || stripped.trim() == "---" {
            continue;
        }
        let indent = stripped.len() - stripped.trim_start_matches(' ').len();
        lines.push(Line {
            number: index + 1,
            indent,
            text: stripped[indent..].to_string(),
        });
    }
    Ok(lines)
}

fn unquote(value: &str) -> Option<String> {
    let v = value.trim();
    if v.len() >= 2
        && ((v.starts_with('"') && v.ends_with('"')) || (v.starts_with('\'') && v.ends_with('\'')))
    {
        let inner = &v[1..v.len() - 1];
        if v.starts_with('"') {
            let mut out = String::new();
            let mut chars = inner.chars();
            while let Some(c) = chars.next() {
                if c == '\\' {
                    match chars.next() {
                        Some('n') => out.push('\n'),
                        Some('t') => out.push('\t'),
                        Some('"') => out.push('"'),
                        Some('\\') => out.push('\\'),
                        Some(other) => {
                            out.push('\\');
                            out.push(other);
                        }
                        None => out.push('\\'),
                    }
                } else {
                    out.push(c);
                }
            }
            return Some(out);
        }
        return Some(inner.replace("''", "'"));
    }
    None
}

fn typed_scalar(value: &str, as_string: bool) -> Value {
    if let Some(s) = unquote(value) {
        return Value::String(s);
    }
    let v = value.trim();
    if as_string {
        return Value::String(v.to_string());
    }
    if v.starts_with('[') && v.ends_with(']') {
        let inner = &v[1..v.len() - 1];
        if inner.trim().is_empty() {
            return Value::Array(Vec::new());
        }
        return Value::Array(
            split_flow(inner)
                .iter()
                .map(|item| typed_scalar(item, false))
                .collect(),
        );
    }
    if v == "{}" {
        return Value::Object(Map::new());
    }
    match v.to_ascii_lowercase().as_str() {
        "true" | "yes" | "on" => return Value::Bool(true),
        "false" | "no" | "off" => return Value::Bool(false),
        "null" | "~" => return Value::Null,
        _ => {}
    }
    if let Ok(i) = v.parse::<i64>() {
        return Value::Number(i.into());
    }
    if let Ok(f) = v.parse::<f64>()
        && f.is_finite()
        && v.chars().any(|c| c.is_ascii_digit())
        && let Some(n) = Number::from_f64(f)
    {
        return Value::Number(n);
    }
    Value::String(v.to_string())
}

/// Split a flow sequence body on commas outside quotes.
fn split_flow(text: &str) -> Vec<String> {
    let mut items = Vec::new();
    let mut current = String::new();
    let mut quote: Option<char> = None;
    for ch in text.chars() {
        match quote {
            Some(q) if ch == q => {
                quote = None;
                current.push(ch);
            }
            Some(_) => current.push(ch),
            None if ch == '"' || ch == '\'' => {
                quote = Some(ch);
                current.push(ch);
            }
            None if ch == ',' => items.push(std::mem::take(&mut current).trim().to_string()),
            None => current.push(ch),
        }
    }
    if !current.trim().is_empty() {
        items.push(current.trim().to_string());
    }
    items
}

/// `key: rest` split at the first `:` followed by a space or end of line
/// (outside quotes).
fn split_key(text: &str) -> Option<(String, String)> {
    let mut quote: Option<char> = None;
    let bytes: Vec<char> = text.chars().collect();
    let mut offset = 0usize;
    for (i, ch) in bytes.iter().enumerate() {
        match quote {
            Some(q) if *ch == q => quote = None,
            Some(_) => {}
            None if *ch == '"' || *ch == '\'' => quote = Some(*ch),
            None if *ch == ':' && (i + 1 == bytes.len() || bytes[i + 1] == ' ') => {
                let key = text[..offset].trim();
                let key = unquote(key).unwrap_or_else(|| key.to_string());
                if key.is_empty() {
                    return None;
                }
                return Some((key, text[offset + 1..].trim().to_string()));
            }
            None => {}
        }
        offset += ch.len_utf8();
    }
    None
}

fn line_error(line: &Line, message: &str) -> String {
    format!("line {}: {message}", line.number)
}

fn parse_block(
    lines: &[Line],
    index: &mut usize,
    indent: usize,
    key: Option<&str>,
) -> Result<Value, String> {
    let Some(first) = lines.get(*index) else {
        return Ok(Value::Null);
    };
    if first.text == "-" || first.text.starts_with("- ") {
        parse_sequence(lines, index, indent, key)
    } else {
        parse_map(lines, index, indent).map(Value::Object)
    }
}

fn parse_sequence(
    lines: &[Line],
    index: &mut usize,
    indent: usize,
    key: Option<&str>,
) -> Result<Value, String> {
    let as_string = key.is_some_and(|k| STRING_ARRAY_KEYS.contains(&k));
    let mut items = Vec::new();
    while let Some(line) = lines.get(*index) {
        if line.indent < indent {
            break;
        }
        if line.indent > indent {
            return Err(line_error(line, "unexpected indentation"));
        }
        if !(line.text == "-" || line.text.starts_with("- ")) {
            break;
        }
        let rest = line.text[1..].trim().to_string();
        *index += 1;
        if rest.is_empty() {
            match lines.get(*index) {
                Some(next) if next.indent > indent => {
                    let child_indent = next.indent;
                    items.push(parse_block(lines, index, child_indent, None)?);
                }
                _ => items.push(Value::Null),
            }
            continue;
        }
        if unquote(&rest).is_none()
            && let Some((k, v)) = split_key(&rest)
        {
            // `- key: value` starts an inline map; following keys align with `key`.
            let item_indent = indent + 2;
            let mut map = Map::new();
            insert_entry(lines, index, item_indent, &mut map, k, v, line)?;
            if let Some(next) = lines.get(*index)
                && next.indent == item_indent
                && !next.text.starts_with("- ")
            {
                let rest_map = parse_map(lines, index, item_indent)?;
                map.extend(rest_map);
            }
            items.push(Value::Object(map));
            continue;
        }
        items.push(typed_scalar(&rest, as_string));
    }
    Ok(Value::Array(items))
}

fn insert_entry(
    lines: &[Line],
    index: &mut usize,
    indent: usize,
    map: &mut Map<String, Value>,
    key: String,
    rest: String,
    line: &Line,
) -> Result<(), String> {
    if map.contains_key(&key) {
        return Err(line_error(line, &format!("duplicate key: {key}")));
    }
    let value = if rest.is_empty() {
        match lines.get(*index) {
            Some(next)
                if next.indent > indent
                    || (next.indent == indent
                        && (next.text == "-" || next.text.starts_with("- "))) =>
            {
                let child_indent = next.indent;
                parse_block(lines, index, child_indent, Some(&key))?
            }
            _ => Value::Null,
        }
    } else if rest == "|" || rest == ">" || rest == "|-" || rest == ">-" {
        let mut parts = Vec::new();
        while let Some(next) = lines.get(*index) {
            if next.indent <= indent {
                break;
            }
            parts.push(next.text.clone());
            *index += 1;
        }
        let joined = if rest.starts_with('|') {
            parts.join("\n")
        } else {
            parts.join(" ")
        };
        Value::String(joined)
    } else {
        let as_string = STRING_KEYS.contains(&key.as_str());
        let as_string_items = STRING_ARRAY_KEYS.contains(&key.as_str());
        let value = typed_scalar(&rest, as_string);
        match value {
            Value::Array(items) if as_string_items => Value::Array(
                items
                    .into_iter()
                    .map(|v| match v {
                        Value::String(s) => Value::String(s),
                        other => Value::String(other.to_string()),
                    })
                    .collect(),
            ),
            other => other,
        }
    };
    map.insert(key, value);
    Ok(())
}

fn parse_map(
    lines: &[Line],
    index: &mut usize,
    indent: usize,
) -> Result<Map<String, Value>, String> {
    let mut map = Map::new();
    while let Some(line) = lines.get(*index) {
        if line.indent < indent {
            break;
        }
        if line.indent > indent {
            return Err(line_error(line, "unexpected indentation"));
        }
        if line.text == "-" || line.text.starts_with("- ") {
            break;
        }
        let Some((key, rest)) = split_key(&line.text) else {
            return Err(line_error(line, "expected 'key: value'"));
        };
        *index += 1;
        insert_entry(lines, index, indent, &mut map, key, rest, line)?;
    }
    Ok(map)
}

/// Parse a YAML settings document into a JSON object.
pub fn parse_yaml_object(content: &str) -> Result<Map<String, Value>, String> {
    let lines = collect_lines(content)?;
    let mut index = 0;
    if lines.is_empty() {
        return Ok(Map::new());
    }
    let indent = lines[0].indent;
    let root = parse_map(&lines, &mut index, indent)?;
    if let Some(line) = lines.get(index) {
        return Err(line_error(line, "could not parse line"));
    }
    Ok(root)
}

/// Parse `settings.yaml` with the archived restrictions (`llama` settings
/// must live in `settings.json`).
pub fn parse_yaml_settings(content: &str) -> Result<Map<String, Value>, String> {
    let map = parse_yaml_object(content)?;
    if map.contains_key("llama") {
        return Err("llama YAML settings are not supported yet; use settings.json".into());
    }
    Ok(map)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn parses_maps_sequences_and_typed_scalars() {
        let doc = r#"
# global settings
defaultProvider: openai
defaultModel: "o3"
allowExternalFilesystemAccess: yes
maxOutputTokens: 8192
skillRoots:
  - ~/skills   # comment
  - "./more skills"
enabledModels: [anthropic/*, openai/gpt-5]
images:
  autoResize: off
retry:
  provider:
    timeoutMs: 1000
ollama:
  instances:
    - id: local
      baseUrl: http://127.0.0.1:11434
      enabled: true
    - id: gpu
      baseUrl: "http://box:11434"
theme: 1984
"#;
        let map = parse_yaml_settings(doc).unwrap();
        assert_eq!(
            Value::Object(map),
            json!({
                "defaultProvider": "openai",
                "defaultModel": "o3",
                "allowExternalFilesystemAccess": true,
                "maxOutputTokens": 8192,
                "skillRoots": ["~/skills", "./more skills"],
                "enabledModels": ["anthropic/*", "openai/gpt-5"],
                "images": {"autoResize": false},
                "retry": {"provider": {"timeoutMs": 1000}},
                "ollama": {"instances": [
                    {"id": "local", "baseUrl": "http://127.0.0.1:11434", "enabled": true},
                    {"id": "gpu", "baseUrl": "http://box:11434"}
                ]},
                "theme": "1984"
            })
        );
    }

    #[test]
    fn rejects_llama_and_malformed_documents() {
        assert_eq!(
            parse_yaml_settings("llama:\n  backend: cpu\n").unwrap_err(),
            "llama YAML settings are not supported yet; use settings.json"
        );
        assert!(
            parse_yaml_object("a: 1\n    b: 2\n")
                .unwrap_err()
                .contains("line 2")
        );
        assert!(parse_yaml_object("just text\n").is_err());
        assert!(
            parse_yaml_object("a: 1\na: 2\n")
                .unwrap_err()
                .contains("duplicate")
        );
        assert!(parse_yaml_object("").unwrap().is_empty());
    }
}
