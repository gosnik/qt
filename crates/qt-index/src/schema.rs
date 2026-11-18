//! JSON-schema argument validation with lenient coercion, as the archived
//! `validate_tool_arguments` implemented it.
//!
//! Values are coerced to the schema type when unambiguous (`"3"` → `3`,
//! `"true"` → `true`, `1` → `true`, numbers → strings, `null` → zero/empty).
//! Errors are collected per path and rendered as:
//!
//! ```text
//! Validation failed for tool "<name>":
//!   - <path>: <problem>
//!
//! Received arguments:
//! <pretty JSON>
//! ```

use serde_json::{Number, Value};

fn schema_types(schema: &Value) -> Vec<&str> {
    match schema.get("type") {
        Some(Value::String(t)) => vec![t.as_str()],
        Some(Value::Array(items)) => items.iter().filter_map(Value::as_str).collect(),
        _ => Vec::new(),
    }
}

fn is_integer(value: &Value) -> bool {
    value.as_i64().is_some() || value.as_u64().is_some()
}

fn matches_type(value: &Value, kind: &str) -> bool {
    match kind {
        "object" => value.is_object(),
        "array" => value.is_array(),
        "string" => value.is_string(),
        "boolean" => value.is_boolean(),
        "integer" => is_integer(value),
        "number" => value.is_number(),
        "null" => value.is_null(),
        _ => true,
    }
}

fn parse_full_integer(text: &str) -> Option<i64> {
    if text.is_empty() || text.starts_with(char::is_whitespace) {
        return None;
    }
    text.parse::<i64>().ok()
}

fn parse_full_number(text: &str) -> Option<f64> {
    if text.is_empty() || text.starts_with(char::is_whitespace) {
        return None;
    }
    text.parse::<f64>().ok().filter(|n| n.is_finite())
}

fn number_value(n: f64) -> Value {
    if n.fract() == 0.0 && n.abs() < 9.0e15 {
        Value::Number(Number::from(n as i64))
    } else {
        Number::from_f64(n)
            .map(Value::Number)
            .unwrap_or(Value::Null)
    }
}

fn coerce_type(value: &Value, kind: &str) -> Option<Value> {
    if matches_type(value, kind) {
        return Some(value.clone());
    }
    match kind {
        "number" => match value {
            Value::String(s) => parse_full_number(s).map(number_value),
            Value::Bool(b) => Some(Value::from(i64::from(*b))),
            Value::Null => Some(Value::from(0)),
            _ => None,
        },
        "integer" => match value {
            Value::String(s) => parse_full_integer(s).map(Value::from),
            Value::Number(n) => n
                .as_f64()
                .filter(|f| f.floor() == *f && f.abs() < 9.2e18)
                .map(|f| Value::from(f as i64)),
            Value::Bool(b) => Some(Value::from(i64::from(*b))),
            Value::Null => Some(Value::from(0)),
            _ => None,
        },
        "boolean" => match value {
            Value::String(s) if s == "true" => Some(Value::Bool(true)),
            Value::String(s) if s == "false" => Some(Value::Bool(false)),
            Value::Number(n) if n.as_i64() == Some(1) => Some(Value::Bool(true)),
            Value::Number(n) if n.as_i64() == Some(0) => Some(Value::Bool(false)),
            _ => None,
        },
        "string" => match value {
            Value::Null => Some(Value::String(String::new())),
            Value::Bool(b) => Some(Value::String(b.to_string())),
            Value::Number(n) => Some(Value::String(n.to_string())),
            _ => None,
        },
        "null" => match value {
            Value::String(s) if s.is_empty() => Some(Value::Null),
            Value::Number(n) if n.as_i64() == Some(0) => Some(Value::Null),
            Value::Bool(false) => Some(Value::Null),
            _ => None,
        },
        _ => None,
    }
}

fn coerce(value: &Value, schema: &Value) -> Option<Value> {
    let types = schema_types(schema);
    if types.is_empty() || types.iter().any(|t| matches_type(value, t)) {
        return Some(value.clone());
    }
    types.iter().find_map(|t| coerce_type(value, t))
}

fn type_label(schema: &Value) -> String {
    let types = schema_types(schema);
    if types.is_empty() {
        "valid value".into()
    } else {
        types.join(" or ")
    }
}

fn value_label(value: &Value) -> &'static str {
    match value {
        Value::Object(_) => "object",
        Value::Array(_) => "array",
        Value::String(_) => "string",
        Value::Bool(_) => "boolean",
        Value::Number(n) if n.is_i64() || n.is_u64() => "integer",
        Value::Number(_) => "number",
        Value::Null => "null",
    }
}

fn child_path(path: &str, child: &str) -> String {
    if path.is_empty() || path == "root" {
        child.to_string()
    } else {
        format!("{path}.{child}")
    }
}

fn validate_value(schema: &Value, value: &mut Value, path: &str, errors: &mut Vec<String>) {
    if !schema.is_object() {
        return;
    }
    let display = if path.is_empty() { "root" } else { path };
    let Some(coerced) = coerce(value, schema) else {
        errors.push(format!(
            "{display}: expected {}, received {}",
            type_label(schema),
            value_label(value)
        ));
        return;
    };
    *value = coerced;

    if let Some(Value::Array(options)) = schema.get("enum")
        && !options.contains(value)
    {
        errors.push(format!(
            "{display}: value must be one of {}",
            serde_json::to_string(options).unwrap_or_default()
        ));
    }

    if let Value::Object(object) = value {
        if let Some(Value::Array(required)) = schema.get("required") {
            for name in required.iter().filter_map(Value::as_str) {
                if !object.contains_key(name) {
                    errors.push(format!(
                        "{}: required property is missing",
                        child_path(display, name)
                    ));
                }
            }
        }
        if let Some(Value::Object(properties)) = schema.get("properties") {
            let additional = schema
                .get("additionalProperties")
                .and_then(Value::as_bool)
                .unwrap_or(true);
            for (key, item) in object.iter_mut() {
                match properties.get(key) {
                    Some(property) => {
                        validate_value(property, item, &child_path(display, key), errors)
                    }
                    None if !additional => {
                        errors.push(format!("{}: unknown property", child_path(display, key)));
                    }
                    None => {}
                }
            }
        }
    }

    if let Value::Array(items) = value {
        if let Some(min) = schema.get("minItems").and_then(Value::as_i64)
            && min >= 0
            && (items.len() as i64) < min
        {
            errors.push(format!("{display}: expected at least {min} items"));
        }
        if let Some(max) = schema.get("maxItems").and_then(Value::as_i64)
            && max >= 0
            && (items.len() as i64) > max
        {
            errors.push(format!("{display}: expected at most {max} items"));
        }
        if let Some(item_schema) = schema.get("items").filter(|s| s.is_object()) {
            for (index, item) in items.iter_mut().enumerate() {
                validate_value(item_schema, item, &format!("{display}[{index}]"), errors);
            }
        }
    }

    if let Value::String(text) = value
        && let Some(min) = schema.get("minLength").and_then(Value::as_i64)
        && min >= 0
        && (text.len() as i64) < min
    {
        errors.push(format!("{display}: expected at least {min} characters"));
    }

    if let Some(number) = value.as_f64() {
        if let Some(minimum) = schema.get("minimum").filter(|m| m.is_number())
            && number < minimum.as_f64().unwrap_or(f64::MIN)
        {
            errors.push(format!("{display}: value is below minimum {minimum}"));
        }
        if let Some(maximum) = schema.get("maximum").filter(|m| m.is_number())
            && number > maximum.as_f64().unwrap_or(f64::MAX)
        {
            errors.push(format!("{display}: value is above maximum {maximum}"));
        }
    }
}

fn failure(tool: &str, problems: &[String], arguments: &Value) -> String {
    let mut message = format!("Validation failed for tool \"{tool}\":");
    for problem in problems {
        message.push_str("\n  - ");
        message.push_str(problem);
    }
    message.push_str("\n\nReceived arguments:\n");
    message.push_str(&serde_json::to_string_pretty(arguments).unwrap_or_default());
    message
}

/// Validate and coerce `arguments` against `schema`. Returns the coerced
/// arguments or the model-facing error message.
pub fn validate_tool_arguments(
    tool: &str,
    schema: &Value,
    arguments: &Value,
) -> Result<Value, String> {
    if !arguments.is_object() {
        return Err(failure(tool, &["root: expected object".into()], arguments));
    }
    if !schema.is_object() {
        return Err(failure(
            tool,
            &["root: expected JSON schema object".into()],
            arguments,
        ));
    }
    let mut result = arguments.clone();
    let mut errors = Vec::new();
    validate_value(schema, &mut result, "root", &mut errors);
    if errors.is_empty() {
        Ok(result)
    } else {
        Err(failure(tool, &errors, arguments))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn schema() -> Value {
        json!({
            "type": "object",
            "properties": {
                "name": { "type": "string", "minLength": 1 },
                "limit": { "type": "integer", "minimum": 1, "maximum": 100 },
                "context": { "type": "boolean" },
                "direction": { "type": "string", "enum": ["inbound", "outbound", "both"] }
            },
            "required": ["name"],
            "additionalProperties": false
        })
    }

    #[test]
    fn coerces_scalars() {
        let out = validate_tool_arguments(
            "symbol",
            &schema(),
            &json!({"name": 5, "limit": "7", "context": "true"}),
        )
        .unwrap();
        assert_eq!(out, json!({"name": "5", "limit": 7, "context": true}));
        let out = validate_tool_arguments(
            "symbol",
            &schema(),
            &json!({"name": "x", "limit": 3.0, "context": 0}),
        )
        .unwrap();
        assert_eq!(out["limit"], 3);
        assert_eq!(out["context"], false);
    }

    #[test]
    fn reports_every_problem() {
        let err = validate_tool_arguments(
            "references",
            &schema(),
            &json!({"limit": 0, "direction": "up", "extra": 1, "context": "maybe"}),
        )
        .unwrap_err();
        assert!(err.starts_with("Validation failed for tool \"references\":\n"));
        assert!(err.contains("  - name: required property is missing"));
        assert!(err.contains("  - limit: value is below minimum 1"));
        assert!(
            err.contains("  - direction: value must be one of [\"inbound\",\"outbound\",\"both\"]")
        );
        assert!(err.contains("  - extra: unknown property"));
        assert!(err.contains("  - context: expected boolean, received string"));
        assert!(err.contains("\n\nReceived arguments:\n{"));
        let err = validate_tool_arguments("symbol", &schema(), &json!({"name": ""})).unwrap_err();
        assert!(err.contains("name: expected at least 1 characters"));
        let err = validate_tool_arguments("symbol", &schema(), &json!([1])).unwrap_err();
        assert!(err.contains("root: expected object"));
    }
}
