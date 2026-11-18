//! Output guard: caps oversized strings and arrays in tool results before
//! they reach the model (archived `apply_output_guards`).

use serde_json::{Value, json};

use super::ToolOutput;
use crate::message::ContentBlock;
use crate::util::floor_char_boundary;

/// Limits applied to every tool result.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct OutputGuardOptions {
    pub max_string_bytes: usize,
    pub max_array_items: usize,
}

impl Default for OutputGuardOptions {
    fn default() -> Self {
        Self {
            max_string_bytes: 64 * 1024,
            max_array_items: 1000,
        }
    }
}

/// Truncate text blocks and `details` values that exceed the limits.
///
/// Image data is left alone (images have their own size handling). When
/// anything was cut, `details.outputGuard` records each truncated field.
pub fn apply_output_guard(output: &mut ToolOutput, options: OutputGuardOptions) {
    let mut fields = Vec::new();
    for (index, block) in output.content.iter_mut().enumerate() {
        if let ContentBlock::Text { text, .. } = block
            && text.len() > options.max_string_bytes
        {
            let original = text.len();
            let cut = floor_char_boundary(text, options.max_string_bytes);
            text.truncate(cut);
            text.push_str(&format!(
                "\n\n[Output truncated by the output guard: showing {cut} of {original} bytes]"
            ));
            fields.push(json!({
                "path": format!("/content/{index}"),
                "type": "string",
                "original_bytes": original,
                "preview_bytes": cut,
            }));
        }
    }
    if let Some(details) = output.details.as_mut() {
        guard_value(details, "", options, &mut fields);
    }
    if fields.is_empty() {
        return;
    }
    let record = json!({
        "truncated": true,
        "max_string_bytes": options.max_string_bytes,
        "max_array_items": options.max_array_items,
        "fields": fields,
    });
    match output.details.as_mut() {
        Some(Value::Object(map)) => {
            map.insert("outputGuard".into(), record);
        }
        Some(other) => {
            let previous = other.take();
            *other = json!({ "value": previous, "outputGuard": record });
        }
        None => output.details = Some(json!({ "outputGuard": record })),
    }
}

fn child(path: &str, key: &str) -> String {
    format!("{path}/{key}")
}

fn guard_value(
    value: &mut Value,
    path: &str,
    options: OutputGuardOptions,
    fields: &mut Vec<Value>,
) {
    match value {
        Value::String(text) if text.len() > options.max_string_bytes => {
            let original = text.len();
            let cut = floor_char_boundary(text, options.max_string_bytes);
            text.truncate(cut);
            fields.push(json!({
                "path": if path.is_empty() { "/" } else { path },
                "type": "string",
                "original_bytes": original,
                "preview_bytes": cut,
            }));
        }
        Value::Array(items) => {
            let original = items.len();
            if original > options.max_array_items {
                items.truncate(options.max_array_items);
                fields.push(json!({
                    "path": if path.is_empty() { "/" } else { path },
                    "type": "array",
                    "original_items": original,
                    "preview_items": options.max_array_items,
                }));
            }
            for (index, item) in items.iter_mut().enumerate() {
                guard_value(item, &child(path, &index.to_string()), options, fields);
            }
        }
        Value::Object(map) => {
            for (key, item) in map.iter_mut() {
                guard_value(item, &child(path, key), options, fields);
            }
        }
        _ => {}
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn truncates_text_strings_and_arrays() {
        let options = OutputGuardOptions {
            max_string_bytes: 4,
            max_array_items: 2,
        };
        let mut out = ToolOutput::text("héllo world").with_details(json!({
            "list": [1, 2, 3],
            "nested": { "s": "abcdefgh" },
        }));
        apply_output_guard(&mut out, options);
        assert!(out.text_content().starts_with("hél\n\n[Output truncated"));
        let details = out.details.unwrap();
        assert_eq!(details["list"], json!([1, 2]));
        assert_eq!(details["nested"]["s"], "abcd");
        let fields = details["outputGuard"]["fields"].as_array().unwrap();
        assert_eq!(fields.len(), 3);
        assert_eq!(fields[1]["path"], "/list");

        let mut small = ToolOutput::text("ok");
        apply_output_guard(&mut small, OutputGuardOptions::default());
        assert!(small.details.is_none());
    }
}
