//! Static scan of Pi TypeScript/JavaScript extension modules. qt never runs
//! JavaScript; like the archived loader it reads `pi.registerCommand`,
//! `pi.registerTool`, `pi.registerFlag`, `pi.registerProvider`,
//! `pi.unregisterProvider` and `pi.on` calls so the contributions can be
//! listed, and reports runtime APIs that only work in Pi.

use std::collections::HashMap;

use serde_json::{Map, Value, json};

use crate::manifest::{
    CommandContribution, ExtensionManifest, FlagContribution, ProviderContribution,
    ToolContribution,
};

/// A use of an API the native host does not provide for reference modules.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ApiUseDiagnostic {
    pub api: String,
    pub message: String,
    pub row: usize,
    pub column: usize,
}

/// Runtime APIs that need the Pi JavaScript host (archived compatibility table).
pub const NOT_IMPLEMENTED_REFERENCE_APIS: &[&str] = &[
    "pi.sendMessage",
    "pi.sendUserMessage",
    "pi.appendEntry",
    "pi.setSessionName",
    "pi.getSessionName",
    "pi.setLabel",
    "pi.getCommands",
    "pi.registerMessageRenderer",
    "pi.registerShortcut",
    "pi.exec",
    "pi.getActiveTools",
    "pi.getAllTools",
    "pi.setActiveTools",
    "pi.setModel",
    "pi.getThinkingLevel",
    "pi.setThinkingLevel",
    "ctx.ui.select",
    "ctx.ui.confirm",
    "ctx.ui.input",
    "ctx.ui.editor",
    "ctx.ui.notify",
    "ctx.ui.custom",
    "ctx.ui.onTerminalInput",
    "ctx.ui.setStatus",
    "ctx.ui.setWorkingMessage",
    "ctx.ui.setWorkingVisible",
    "ctx.ui.setWorkingIndicator",
    "ctx.ui.setHiddenThinkingLabel",
    "ctx.ui.setWidget",
    "ctx.ui.setFooter",
    "ctx.ui.setHeader",
    "ctx.ui.setTitle",
    "ctx.ui.pasteToEditor",
    "ctx.ui.setEditorText",
    "ctx.ui.getEditorText",
    "ctx.ui.addAutocompleteProvider",
    "ctx.ui.setEditorComponent",
    "ctx.ui.getEditorComponent",
    "ctx.ui.getAllThemes",
    "ctx.ui.getTheme",
    "ctx.ui.setTheme",
    "ctx.ui.getToolsExpanded",
    "ctx.ui.setToolsExpanded",
];

fn is_ident_start(c: u8) -> bool {
    c.is_ascii_alphabetic() || c == b'_' || c == b'$'
}

fn is_ident_char(c: u8) -> bool {
    c.is_ascii_alphanumeric() || c == b'_' || c == b'$'
}

fn skip_ws(text: &[u8], offset: &mut usize) {
    while *offset < text.len() && text[*offset].is_ascii_whitespace() {
        *offset += 1;
    }
}

fn string_literal(text: &[u8], offset: &mut usize) -> Option<String> {
    skip_ws(text, offset);
    let quote = *text.get(*offset)?;
    if !matches!(quote, b'"' | b'\'' | b'`') {
        return None;
    }
    *offset += 1;
    let mut value = Vec::new();
    let mut escaped = false;
    while *offset < text.len() {
        let c = text[*offset];
        *offset += 1;
        if escaped {
            value.push(c);
            escaped = false;
        } else if c == b'\\' {
            escaped = true;
        } else if c == quote {
            return Some(String::from_utf8_lossy(&value).into_owned());
        } else {
            value.push(c);
        }
    }
    None
}

/// Body between `open` at `open_offset` and its matching `close`.
fn balanced(text: &[u8], open_offset: usize, open: u8, close: u8) -> Option<&[u8]> {
    if text.get(open_offset) != Some(&open) {
        return None;
    }
    let mut depth = 1usize;
    let mut quote = 0u8;
    let mut escaped = false;
    for (i, &c) in text.iter().enumerate().skip(open_offset + 1) {
        if quote != 0 {
            if escaped {
                escaped = false;
            } else if c == b'\\' {
                escaped = true;
            } else if c == quote {
                quote = 0;
            }
            continue;
        }
        if matches!(c, b'"' | b'\'' | b'`') {
            quote = c;
        } else if c == open {
            depth += 1;
        } else if c == close {
            depth -= 1;
            if depth == 0 {
                return Some(&text[open_offset + 1..i]);
            }
        }
    }
    None
}

fn identifier(text: &[u8], offset: &mut usize) -> Option<String> {
    skip_ws(text, offset);
    if *offset >= text.len() || !is_ident_start(text[*offset]) {
        return None;
    }
    let start = *offset;
    *offset += 1;
    while *offset < text.len() && is_ident_char(text[*offset]) {
        *offset += 1;
    }
    Some(String::from_utf8_lossy(&text[start..*offset]).into_owned())
}

fn first_object_argument(text: &[u8], mut offset: usize) -> Option<&[u8]> {
    skip_ws(text, &mut offset);
    if text.get(offset) == Some(&b'{') {
        return balanced(text, offset, b'{', b'}');
    }
    identifier(text, &mut offset)?;
    skip_ws(text, &mut offset);
    let body = balanced(text, offset, b'(', b')')?;
    let mut b = 0;
    skip_ws(body, &mut b);
    balanced(body, b, b'{', b'}')
}

fn object_definitions(text: &[u8]) -> HashMap<String, Vec<u8>> {
    let mut defs = HashMap::new();
    for offset in 0..text.len() {
        let keyword: &[u8] = if text[offset..].starts_with(b"const") {
            b"const"
        } else if text[offset..].starts_with(b"let") {
            b"let"
        } else if text[offset..].starts_with(b"var") {
            b"var"
        } else {
            continue;
        };
        let after = offset + keyword.len();
        if after < text.len() && is_ident_char(text[after]) {
            continue;
        }
        let mut cursor = after;
        let Some(name) = identifier(text, &mut cursor) else {
            continue;
        };
        while cursor < text.len() && !matches!(text[cursor], b'=' | b';' | b'\n') {
            cursor += 1;
        }
        if text.get(cursor) != Some(&b'=') {
            continue;
        }
        if let Some(object) = first_object_argument(text, cursor + 1) {
            defs.insert(name, object.to_vec());
        }
    }
    defs
}

fn object_string_property(body: &[u8], property: &str) -> Option<String> {
    let mut depth = 0i32;
    let mut quote = 0u8;
    let mut escaped = false;
    for offset in 0..body.len() {
        let c = body[offset];
        if quote != 0 {
            if escaped {
                escaped = false;
            } else if c == b'\\' {
                escaped = true;
            } else if c == quote {
                quote = 0;
            }
            continue;
        }
        if matches!(c, b'"' | b'\'' | b'`') {
            if depth == 0 && c != b'`' {
                let mut cursor = offset;
                if let Some(key) = string_literal(body, &mut cursor)
                    && key == property
                {
                    skip_ws(body, &mut cursor);
                    if body.get(cursor) == Some(&b':') {
                        cursor += 1;
                        return string_literal(body, &mut cursor);
                    }
                }
            }
            quote = c;
            continue;
        }
        if matches!(c, b'{' | b'[' | b'(') {
            depth += 1;
            continue;
        }
        if matches!(c, b'}' | b']' | b')') {
            depth -= 1;
            continue;
        }
        if depth != 0 || !is_ident_start(c) || (offset > 0 && is_ident_char(body[offset - 1])) {
            continue;
        }
        let mut cursor = offset;
        if identifier(body, &mut cursor).as_deref() != Some(property) {
            continue;
        }
        skip_ws(body, &mut cursor);
        if body.get(cursor) != Some(&b':') {
            continue;
        }
        cursor += 1;
        return string_literal(body, &mut cursor);
    }
    None
}

fn object_key(body: &[u8], mut offset: usize) -> Option<(String, usize)> {
    skip_ws(body, &mut offset);
    let c = *body.get(offset)?;
    let key = if matches!(c, b'"' | b'\'' | b'`') {
        string_literal(body, &mut offset)?
    } else if is_ident_start(c) {
        identifier(body, &mut offset)?
    } else {
        return None;
    };
    skip_ws(body, &mut offset);
    (body.get(offset) == Some(&b':')).then_some((key, offset + 1))
}

fn skip_value(body: &[u8], mut offset: usize) -> usize {
    skip_ws(body, &mut offset);
    let Some(&c) = body.get(offset) else {
        return offset;
    };
    if matches!(c, b'"' | b'\'' | b'`') {
        let mut o = offset;
        if string_literal(body, &mut o).is_some() {
            return o;
        }
        return body.len();
    }
    for (open, close) in [(b'{', b'}'), (b'[', b']'), (b'(', b')')] {
        if c == open
            && let Some(nested) = balanced(body, offset, open, close)
        {
            return offset + nested.len() + 2;
        }
    }
    while offset < body.len() && !matches!(body[offset], b',' | b'}' | b']') {
        offset += 1;
    }
    offset
}

fn literal_value(body: &[u8], offset: &mut usize) -> Option<Value> {
    skip_ws(body, offset);
    let c = *body.get(*offset)?;
    if matches!(c, b'"' | b'\'' | b'`') {
        return string_literal(body, offset).map(Value::String);
    }
    if c == b'{' {
        let nested = balanced(body, *offset, b'{', b'}')?;
        *offset += nested.len() + 2;
        return Some(object_literal(nested));
    }
    if c == b'[' {
        let nested = balanced(body, *offset, b'[', b']')?;
        *offset += nested.len() + 2;
        return Some(array_literal(nested));
    }
    for (word, value) in [
        (&b"true"[..], json!(true)),
        (&b"false"[..], json!(false)),
        (&b"null"[..], Value::Null),
    ] {
        if body[*offset..].starts_with(word) {
            *offset += word.len();
            return Some(value);
        }
    }
    let start = *offset;
    let mut o = *offset;
    if body[o] == b'-' {
        o += 1;
    }
    while o < body.len() && body[o].is_ascii_digit() {
        o += 1;
    }
    let mut fraction = false;
    if o < body.len() && body[o] == b'.' {
        let fs = o;
        o += 1;
        while o < body.len() && body[o].is_ascii_digit() {
            o += 1;
        }
        fraction = o > fs + 1;
        if !fraction {
            o = fs;
        }
    }
    if o > start && (body[start] != b'-' || o > start + 1) {
        let text = std::str::from_utf8(&body[start..o]).ok()?;
        let v = if fraction {
            json!(text.parse::<f64>().ok()?)
        } else {
            json!(text.parse::<i64>().ok()?)
        };
        *offset = o;
        return Some(v);
    }
    None
}

fn array_literal(body: &[u8]) -> Value {
    let mut out = Vec::new();
    let mut offset = 0;
    while offset < body.len() {
        skip_ws(body, &mut offset);
        if body.get(offset) == Some(&b',') {
            offset += 1;
            continue;
        }
        if offset >= body.len() {
            break;
        }
        let mut v = offset;
        if let Some(value) = literal_value(body, &mut v) {
            out.push(value);
            offset = v;
        } else {
            offset = skip_value(body, offset).max(offset + 1);
        }
        skip_ws(body, &mut offset);
        if body.get(offset) == Some(&b',') {
            offset += 1;
        }
    }
    Value::Array(out)
}

/// A TypeScript object literal as JSON; values that are not literals are
/// dropped, except the provider callbacks `streamSimple`, `getToken` and
/// `modifyModels`, which become `true`.
fn object_literal(body: &[u8]) -> Value {
    let mut out = Map::new();
    let mut offset = 0;
    while offset < body.len() {
        skip_ws(body, &mut offset);
        if body.get(offset) == Some(&b',') {
            offset += 1;
            continue;
        }
        let Some((key, value_offset)) = object_key(body, offset) else {
            break;
        };
        let mut v = value_offset;
        if let Some(value) = literal_value(body, &mut v) {
            out.insert(key, value);
            offset = v;
        } else {
            if matches!(key.as_str(), "streamSimple" | "getToken" | "modifyModels") {
                out.insert(key, json!(true));
            }
            offset = skip_value(body, value_offset);
        }
        skip_ws(body, &mut offset);
        if body.get(offset) == Some(&b',') {
            offset += 1;
        }
    }
    Value::Object(out)
}

fn resolve_object<'a>(body: &'a [u8], defs: &'a HashMap<String, Vec<u8>>) -> Option<&'a [u8]> {
    let mut offset = 0;
    skip_ws(body, &mut offset);
    if body.get(offset) == Some(&b'{') {
        return balanced(body, offset, b'{', b'}');
    }
    let name = identifier(body, &mut offset)?;
    defs.get(&name).map(Vec::as_slice)
}

fn call_body<'a>(text: &'a [u8], offset: usize, name: &str) -> Option<&'a [u8]> {
    let mut o = offset + name.len();
    skip_ws(text, &mut o);
    balanced(text, o, b'(', b')')
}

/// `(first string argument, rest after the next comma)`.
fn name_and_rest(body: &[u8]) -> Option<(String, Option<&[u8]>)> {
    let mut cursor = 0;
    let name = string_literal(body, &mut cursor)?;
    while cursor < body.len() && body[cursor] != b',' {
        cursor += 1;
    }
    let rest = (cursor < body.len()).then(|| &body[cursor + 1..]);
    Some((name, rest))
}

fn push_metadata(manifest: &mut ExtensionManifest, key: &str, item: Value) {
    if !manifest.metadata.is_object() {
        manifest.metadata = json!({});
    }
    let entry = &mut manifest.metadata[key];
    if !entry.is_array() {
        *entry = json!([]);
    }
    if let Some(list) = entry.as_array_mut() {
        list.push(item);
    }
}

/// Add the registrations found in `source` to `manifest`.
pub fn scan_module_registrations(manifest: &mut ExtensionManifest, source: &str) {
    let text = source.as_bytes();
    let defs = object_definitions(text);
    for offset in 0..text.len() {
        let rest = &text[offset..];
        if rest.starts_with(b"pi.unregisterProvider") {
            if let Some(body) = call_body(text, offset, "pi.unregisterProvider") {
                let mut c = 0;
                if let Some(name) = string_literal(body, &mut c) {
                    push_metadata(
                        manifest,
                        "provider_unregistrations",
                        json!({ "name": name }),
                    );
                }
            }
        } else if rest.starts_with(b"pi.registerCommand") {
            let Some(body) = call_body(text, offset, "pi.registerCommand") else {
                continue;
            };
            let Some((name, options)) = name_and_rest(body) else {
                continue;
            };
            let mut command = CommandContribution {
                name,
                ..Default::default()
            };
            if let Some(object) = options.and_then(|o| resolve_object(o, &defs)) {
                command.title = object_string_property(object, "title")
                    .or_else(|| object_string_property(object, "label"));
                command.description = object_string_property(object, "description");
            }
            manifest.commands.push(command);
        } else if rest.starts_with(b"pi.registerFlag") {
            let Some(body) = call_body(text, offset, "pi.registerFlag") else {
                continue;
            };
            let Some((name, options)) = name_and_rest(body) else {
                continue;
            };
            let mut flag = FlagContribution {
                name,
                ..Default::default()
            };
            if let Some(object) = options.and_then(|o| resolve_object(o, &defs)) {
                flag.description = object_string_property(object, "description");
                let parsed = object_literal(object);
                if let Some(t) = parsed.get("type").and_then(Value::as_str) {
                    flag.flag_type = t.into();
                }
                if let Some(d) = parsed.get("default") {
                    flag.default = d.clone();
                }
            }
            manifest.flags.push(flag);
        } else if rest.starts_with(b"pi.registerTool") {
            let Some(body) = call_body(text, offset, "pi.registerTool") else {
                continue;
            };
            let Some(object) = resolve_object(body, &defs) else {
                continue;
            };
            let Some(name) = object_string_property(object, "name") else {
                continue;
            };
            manifest.tools.push(ToolContribution {
                name,
                title: object_string_property(object, "title")
                    .or_else(|| object_string_property(object, "label")),
                description: object_string_property(object, "description"),
                parameters: None,
            });
        } else if rest.starts_with(b"pi.registerProvider") {
            let Some(body) = call_body(text, offset, "pi.registerProvider") else {
                continue;
            };
            let Some((name, Some(options))) = name_and_rest(body) else {
                continue;
            };
            if let Some(object) = resolve_object(options, &defs) {
                manifest.providers.push(ProviderContribution {
                    name,
                    config: object_literal(object),
                });
            }
        } else if rest.starts_with(b"pi.on") {
            if rest.get(5).is_some_and(|c| is_ident_char(*c)) {
                continue;
            }
            if let Some(body) = call_body(text, offset, "pi.on") {
                let mut c = 0;
                if let Some(event) = string_literal(body, &mut c) {
                    push_metadata(manifest, "lifecycle_handlers", json!({ "event": event }));
                }
            }
        }
    }
}

fn is_api_call_at(text: &[u8], offset: usize, api: &str) -> bool {
    if !text[offset..].starts_with(api.as_bytes()) {
        return false;
    }
    if offset > 0 && (is_ident_char(text[offset - 1]) || text[offset - 1] == b'.') {
        return false;
    }
    let after = offset + api.len();
    if after < text.len() && is_ident_char(text[after]) {
        return false;
    }
    let mut o = after;
    skip_ws(text, &mut o);
    text.get(o) == Some(&b'(')
}

/// Calls to [`NOT_IMPLEMENTED_REFERENCE_APIS`] outside comments and strings.
pub fn find_not_implemented_api_uses(source: &str) -> Vec<ApiUseDiagnostic> {
    let text = source.as_bytes();
    let mut out = Vec::new();
    let (mut row, mut column) = (1usize, 1usize);
    let advance = |c: u8, row: &mut usize, column: &mut usize| {
        if c == b'\n' {
            *row += 1;
            *column = 1;
        } else {
            *column += 1;
        }
    };
    let (mut line_comment, mut block_comment) = (false, false);
    let (mut quote, mut escaped) = (0u8, false);
    let mut offset = 0;
    while offset < text.len() {
        let c = text[offset];
        if line_comment {
            line_comment = c != b'\n';
        } else if block_comment {
            if c == b'*' && text.get(offset + 1) == Some(&b'/') {
                advance(c, &mut row, &mut column);
                offset += 1;
                block_comment = false;
            }
        } else if quote != 0 {
            if escaped {
                escaped = false;
            } else if c == b'\\' {
                escaped = true;
            } else if c == quote {
                quote = 0;
            }
        } else if c == b'/' && matches!(text.get(offset + 1), Some(b'/') | Some(b'*')) {
            if text[offset + 1] == b'/' {
                line_comment = true;
            } else {
                block_comment = true;
            }
            advance(c, &mut row, &mut column);
            offset += 1;
        } else if matches!(c, b'"' | b'\'' | b'`') {
            quote = c;
            escaped = false;
        } else {
            for api in NOT_IMPLEMENTED_REFERENCE_APIS {
                if is_api_call_at(text, offset, api) {
                    out.push(ApiUseDiagnostic {
                        api: (*api).into(),
                        message: format!(
                            "not_implemented: extension API {api} is not implemented by the native extension host"
                        ),
                        row,
                        column,
                    });
                }
            }
        }
        advance(text[offset], &mut row, &mut column);
        offset += 1;
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    const SOURCE: &str = r#"
import type { ExtensionAPI } from "@mariozechner/pi-coding-agent";
const toolDef = { name: "lookup", label: "Lookup", description: "Find things", execute: async () => {} };
export default function (pi: ExtensionAPI) {
  pi.registerCommand("hello", { description: "Say hi", handler: async (args, ctx) => {
    ctx.ui.notify("hi", "info"); // ctx.ui.select("no") in a comment
  }});
  pi.registerTool(toolDef);
  pi.registerFlag("plan", { description: "Plan mode", type: "boolean", default: false });
  pi.registerProvider("corp", { baseUrl: "https://x", apiKey: "K", models: [{ id: "m", contextWindow: 1000 }], streamSimple: myStream });
  pi.unregisterProvider("old");
  pi.on("tool_call", async (event) => { return undefined; });
  pi.onSomething("x");
  const s = "pi.sendMessage(";
}
"#;

    #[test]
    fn scans_registrations() {
        let mut m = ExtensionManifest::default();
        scan_module_registrations(&mut m, SOURCE);
        assert_eq!(m.commands.len(), 1);
        assert_eq!(m.commands[0].name, "hello");
        assert_eq!(m.commands[0].description.as_deref(), Some("Say hi"));
        assert_eq!(m.tools[0].name, "lookup");
        assert_eq!(m.tools[0].title.as_deref(), Some("Lookup"));
        assert_eq!(m.flags[0].flag_type, "boolean");
        assert_eq!(m.flags[0].default, json!(false));
        assert_eq!(
            m.providers[0].config,
            json!({"baseUrl":"https://x","apiKey":"K","models":[{"id":"m","contextWindow":1000}],"streamSimple":true})
        );
        assert_eq!(
            m.metadata["provider_unregistrations"],
            json!([{"name":"old"}])
        );
        assert_eq!(
            m.metadata["lifecycle_handlers"],
            json!([{"event":"tool_call"}])
        );
    }

    #[test]
    fn reports_unsupported_apis_outside_comments_and_strings() {
        let d = find_not_implemented_api_uses(SOURCE);
        assert_eq!(d.len(), 1);
        assert_eq!(d[0].api, "ctx.ui.notify");
        assert_eq!((d[0].row, d[0].column), (6, 5));
        assert_eq!(
            d[0].message,
            "not_implemented: extension API ctx.ui.notify is not implemented by the native extension host"
        );
    }
}
