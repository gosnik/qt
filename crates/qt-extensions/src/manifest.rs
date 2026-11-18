//! `extension.json` manifest v1: decoding, validation and encoding (archived
//! `extensions/manifest.cpp`).

use serde_json::{Map, Value, json};

/// The only supported `manifest_version`.
pub const MANIFEST_VERSION: i64 = 1;
/// File name of a directory manifest.
pub const MANIFEST_FILE_NAME: &str = "extension.json";

const IDENTIFIER_MESSAGE: &str = "must use lowercase letters, digits, '.', '_' or '-'";

/// How an extension is executed.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Transport {
    /// A child process speaking line-delimited JSON-RPC 2.0 on stdio.
    #[default]
    SubprocessJsonRpc,
    /// A Pi TypeScript/JavaScript module; registrations come from a static
    /// scan and the module is never executed.
    ReferenceModule,
    /// A Rhai script run in-process (replaces the archived AngelScript host).
    Rhai,
}

impl Transport {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::SubprocessJsonRpc => "subprocess_jsonrpc",
            Self::ReferenceModule => "reference_module",
            Self::Rhai => "rhai",
        }
    }

    pub fn parse(text: &str) -> Option<Self> {
        match text {
            "subprocess_jsonrpc" => Some(Self::SubprocessJsonRpc),
            "reference_module" => Some(Self::ReferenceModule),
            "rhai" => Some(Self::Rhai),
            _ => None,
        }
    }
}

/// A slash command contributed by an extension.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct CommandContribution {
    pub name: String,
    pub title: Option<String>,
    pub description: Option<String>,
}

/// A model-callable tool contributed by an extension.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct ToolContribution {
    pub name: String,
    pub title: Option<String>,
    pub description: Option<String>,
    /// JSON schema of the arguments; tools without one accept any object.
    pub parameters: Option<Value>,
}

/// A model provider registration (Pi `registerProvider`).
#[derive(Debug, Clone, PartialEq)]
pub struct ProviderContribution {
    pub name: String,
    pub config: Value,
}

/// A CLI flag registration (Pi `registerFlag`).
#[derive(Debug, Clone, PartialEq)]
pub struct FlagContribution {
    pub name: String,
    pub description: Option<String>,
    /// `boolean` or `string`.
    pub flag_type: String,
    /// Default value; `Null` when unset.
    pub default: Value,
}

impl Default for FlagContribution {
    fn default() -> Self {
        Self {
            name: String::new(),
            description: None,
            flag_type: "boolean".into(),
            default: Value::Null,
        }
    }
}

/// A decoded `extension.json` (or the synthesized manifest of a script).
#[derive(Debug, Clone, PartialEq)]
pub struct ExtensionManifest {
    pub manifest_version: i64,
    pub name: String,
    pub display_name: Option<String>,
    pub transport: Transport,
    pub executable: String,
    pub entrypoint: Option<String>,
    pub args: Vec<String>,
    pub working_directory: Option<String>,
    pub commands: Vec<CommandContribution>,
    pub tools: Vec<ToolContribution>,
    pub providers: Vec<ProviderContribution>,
    pub flags: Vec<FlagContribution>,
    pub metadata: Value,
}

impl Default for ExtensionManifest {
    fn default() -> Self {
        Self {
            manifest_version: MANIFEST_VERSION,
            name: String::new(),
            display_name: None,
            transport: Transport::default(),
            executable: String::new(),
            entrypoint: None,
            args: Vec::new(),
            working_directory: None,
            commands: Vec::new(),
            tools: Vec::new(),
            providers: Vec::new(),
            flags: Vec::new(),
            metadata: Value::Object(Map::new()),
        }
    }
}

/// One validation problem, reported as `invalid_argument [field]: message`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ValidationIssue {
    pub field: String,
    pub message: String,
}

impl ValidationIssue {
    pub fn summary(&self) -> String {
        let mut out = String::from("invalid_argument");
        if !self.field.is_empty() {
            out.push_str(&format!(" [{}]", self.field));
        }
        if !self.message.is_empty() {
            out.push_str(&format!(": {}", self.message));
        }
        out
    }
}

/// Issues joined with `; ` (the archived loader diagnostic format).
pub fn join_issues(issues: &[ValidationIssue]) -> String {
    issues
        .iter()
        .map(ValidationIssue::summary)
        .collect::<Vec<_>>()
        .join("; ")
}

/// Lowercase ASCII letters/digits first, then letters, digits, `.`, `_`, `-`.
pub fn is_valid_extension_identifier(text: &str) -> bool {
    let mut chars = text.chars();
    let Some(first) = chars.next() else {
        return false;
    };
    let start = |c: char| c.is_ascii_lowercase() || c.is_ascii_digit();
    start(first) && chars.all(|c| start(c) || matches!(c, '.' | '_' | '-'))
}

fn issue(out: &mut Vec<ValidationIssue>, field: impl Into<String>, message: &str) {
    out.push(ValidationIssue {
        field: field.into(),
        message: message.into(),
    });
}

fn validate_named(
    items: impl Iterator<Item = (String, Option<String>)>,
    collection: &str,
    out: &mut Vec<ValidationIssue>,
) {
    let mut names = std::collections::HashSet::new();
    for (index, (name, title)) in items.enumerate() {
        let prefix = format!("{collection}[{index}]");
        if !is_valid_extension_identifier(&name) {
            issue(out, format!("{prefix}.name"), IDENTIFIER_MESSAGE);
        } else if !names.insert(name) {
            issue(
                out,
                format!("{prefix}.name"),
                "must be unique within the manifest",
            );
        }
        if title.as_deref() == Some("") {
            issue(
                out,
                format!("{prefix}.title"),
                "must not be empty when provided",
            );
        }
    }
}

/// Archived manifest validation rules.
pub fn validate_manifest(manifest: &ExtensionManifest) -> Vec<ValidationIssue> {
    let mut out = Vec::new();
    if manifest.manifest_version != MANIFEST_VERSION {
        issue(&mut out, "manifest_version", "unsupported manifest version");
    }
    if !is_valid_extension_identifier(&manifest.name) {
        issue(&mut out, "name", IDENTIFIER_MESSAGE);
    }
    if manifest.display_name.as_deref() == Some("") {
        issue(&mut out, "display_name", "must not be empty when provided");
    }
    let empty_entry = manifest.entrypoint.as_deref().is_none_or(str::is_empty);
    match manifest.transport {
        Transport::SubprocessJsonRpc if manifest.executable.is_empty() => {
            issue(&mut out, "executable", "must not be empty");
        }
        Transport::ReferenceModule | Transport::Rhai if empty_entry => {
            issue(&mut out, "entrypoint", "must not be empty");
        }
        _ => {}
    }
    for (index, arg) in manifest.args.iter().enumerate() {
        if arg.is_empty() {
            issue(&mut out, format!("args[{index}]"), "must not be empty");
        }
    }
    if manifest.working_directory.as_deref() == Some("") {
        issue(
            &mut out,
            "working_directory",
            "must not be empty when provided",
        );
    }
    if !manifest.metadata.is_object() {
        issue(&mut out, "metadata", "must be a JSON object");
    }
    validate_named(
        manifest
            .commands
            .iter()
            .map(|c| (c.name.clone(), c.title.clone())),
        "commands",
        &mut out,
    );
    validate_named(
        manifest
            .tools
            .iter()
            .map(|t| (t.name.clone(), t.title.clone())),
        "tools",
        &mut out,
    );
    for (index, tool) in manifest.tools.iter().enumerate() {
        if let Some(parameters) = &tool.parameters
            && !parameters.is_object()
        {
            issue(
                &mut out,
                format!("tools[{index}].parameters"),
                "must be a JSON object",
            );
        }
    }
    let mut flag_names = std::collections::HashSet::new();
    for (index, flag) in manifest.flags.iter().enumerate() {
        let prefix = format!("flags[{index}]");
        if !is_valid_extension_identifier(&flag.name) {
            issue(&mut out, format!("{prefix}.name"), IDENTIFIER_MESSAGE);
        } else if !flag_names.insert(flag.name.clone()) {
            issue(
                &mut out,
                format!("{prefix}.name"),
                "must be unique within the manifest",
            );
        }
        if flag.flag_type != "boolean" && flag.flag_type != "string" {
            issue(
                &mut out,
                format!("{prefix}.type"),
                "must be boolean or string",
            );
        }
        if !flag.default.is_null() {
            let matches = (flag.flag_type == "boolean" && flag.default.is_boolean())
                || (flag.flag_type == "string" && flag.default.is_string());
            if !matches {
                issue(
                    &mut out,
                    format!("{prefix}.default"),
                    "must match the declared flag type",
                );
            }
        }
    }
    let mut provider_names = std::collections::HashSet::new();
    for (index, provider) in manifest.providers.iter().enumerate() {
        let prefix = format!("providers[{index}]");
        if !is_valid_extension_identifier(&provider.name) {
            issue(&mut out, format!("{prefix}.name"), IDENTIFIER_MESSAGE);
        } else if !provider_names.insert(provider.name.clone()) {
            issue(
                &mut out,
                format!("{prefix}.name"),
                "must be unique within the manifest",
            );
        }
        if !provider.config.is_object() {
            issue(
                &mut out,
                format!("{prefix}.config"),
                "must be a JSON object",
            );
        }
    }
    out
}

fn required_string(json: &Map<String, Value>, key: &str) -> Result<String, String> {
    match json.get(key) {
        None => Err(format!("key '{key}' not found")),
        Some(Value::String(s)) => Ok(s.clone()),
        Some(other) => Err(format!(
            "'{key}' must be a string, got {}",
            type_name(other)
        )),
    }
}

fn optional_string(json: &Map<String, Value>, key: &str) -> Result<Option<String>, String> {
    match json.get(key) {
        None | Some(Value::Null) => Ok(None),
        Some(Value::String(s)) => Ok(Some(s.clone())),
        Some(other) => Err(format!(
            "'{key}' must be a string, got {}",
            type_name(other)
        )),
    }
}

fn type_name(value: &Value) -> &'static str {
    match value {
        Value::Null => "null",
        Value::Bool(_) => "boolean",
        Value::Number(_) => "number",
        Value::String(_) => "string",
        Value::Array(_) => "array",
        Value::Object(_) => "object",
    }
}

fn object<'a>(value: &'a Value, what: &str) -> Result<&'a Map<String, Value>, String> {
    value
        .as_object()
        .ok_or_else(|| format!("{what} must be an object, got {}", type_name(value)))
}

fn array<'a>(json: &'a Map<String, Value>, key: &str) -> Result<&'a [Value], String> {
    match json.get(key) {
        None | Some(Value::Null) => Ok(&[]),
        Some(Value::Array(items)) => Ok(items),
        Some(other) => Err(format!(
            "'{key}' must be an array, got {}",
            type_name(other)
        )),
    }
}

fn decode_command(value: &Value) -> Result<CommandContribution, String> {
    let json = object(value, "command contribution")?;
    Ok(CommandContribution {
        name: required_string(json, "name")?,
        title: optional_string(json, "title")?,
        description: optional_string(json, "description")?,
    })
}

fn decode_tool(value: &Value) -> Result<ToolContribution, String> {
    let json = object(value, "tool contribution")?;
    Ok(ToolContribution {
        name: required_string(json, "name")?,
        title: optional_string(json, "title")?,
        description: optional_string(json, "description")?,
        parameters: json.get("parameters").filter(|v| !v.is_null()).cloned(),
    })
}

fn decode_provider(value: &Value) -> Result<ProviderContribution, String> {
    let json = object(value, "provider contribution")?;
    Ok(ProviderContribution {
        name: required_string(json, "name")?,
        config: json.get("config").cloned().unwrap_or_else(|| json!({})),
    })
}

fn decode_flag(value: &Value) -> Result<FlagContribution, String> {
    let json = object(value, "flag contribution")?;
    Ok(FlagContribution {
        name: required_string(json, "name")?,
        description: optional_string(json, "description")?,
        flag_type: match json.get("type") {
            None => "boolean".into(),
            Some(Value::String(s)) => s.clone(),
            Some(other) => {
                return Err(format!("'type' must be a string, got {}", type_name(other)));
            }
        },
        default: json.get("default").cloned().unwrap_or(Value::Null),
    })
}

/// Decode a manifest document (no validation).
pub fn decode_manifest(value: &Value) -> Result<ExtensionManifest, String> {
    let json = object(value, "extension manifest")?;
    let manifest_version = match json.get("manifest_version") {
        None => MANIFEST_VERSION,
        Some(v) => v
            .as_i64()
            .ok_or_else(|| "'manifest_version' must be an integer".to_string())?,
    };
    let transport = match json.get("transport") {
        None => Transport::SubprocessJsonRpc,
        Some(Value::String(s)) => {
            Transport::parse(s).ok_or_else(|| "unsupported extension transport".to_string())?
        }
        Some(_) => return Err("unsupported extension transport".into()),
    };
    let args = array(json, "args")?
        .iter()
        .map(|v| {
            v.as_str()
                .map(str::to_string)
                .ok_or_else(|| "'args' entries must be strings".to_string())
        })
        .collect::<Result<Vec<_>, _>>()?;
    Ok(ExtensionManifest {
        manifest_version,
        name: required_string(json, "name")?,
        display_name: optional_string(json, "display_name")?,
        transport,
        executable: optional_string(json, "executable")?.unwrap_or_default(),
        entrypoint: optional_string(json, "entrypoint")?,
        args,
        working_directory: optional_string(json, "working_directory")?,
        commands: array(json, "commands")?
            .iter()
            .map(decode_command)
            .collect::<Result<_, _>>()?,
        tools: array(json, "tools")?
            .iter()
            .map(decode_tool)
            .collect::<Result<_, _>>()?,
        providers: array(json, "providers")?
            .iter()
            .map(decode_provider)
            .collect::<Result<_, _>>()?,
        flags: array(json, "flags")?
            .iter()
            .map(decode_flag)
            .collect::<Result<_, _>>()?,
        metadata: json.get("metadata").cloned().unwrap_or_else(|| json!({})),
    })
}

/// Parse and decode manifest text.
pub fn decode_manifest_str(text: &str) -> Result<ExtensionManifest, String> {
    let value: Value = serde_json::from_str(text).map_err(|e| e.to_string())?;
    decode_manifest(&value)
}

impl ExtensionManifest {
    /// The archived JSON encoding.
    pub fn to_json(&self) -> Value {
        let commands: Vec<Value> = self
            .commands
            .iter()
            .map(|c| {
                let mut v = json!({ "name": c.name });
                if let Some(t) = &c.title {
                    v["title"] = json!(t);
                }
                if let Some(d) = &c.description {
                    v["description"] = json!(d);
                }
                v
            })
            .collect();
        let tools: Vec<Value> = self
            .tools
            .iter()
            .map(|t| {
                let mut v = json!({ "name": t.name });
                if let Some(x) = &t.title {
                    v["title"] = json!(x);
                }
                if let Some(d) = &t.description {
                    v["description"] = json!(d);
                }
                if let Some(p) = &t.parameters {
                    v["parameters"] = p.clone();
                }
                v
            })
            .collect();
        let providers: Vec<Value> = self
            .providers
            .iter()
            .map(|p| json!({ "name": p.name, "config": p.config }))
            .collect();
        let flags: Vec<Value> = self
            .flags
            .iter()
            .map(|f| {
                let mut v = json!({ "name": f.name, "type": f.flag_type });
                if let Some(d) = &f.description {
                    v["description"] = json!(d);
                }
                if !f.default.is_null() {
                    v["default"] = f.default.clone();
                }
                v
            })
            .collect();
        let mut out = json!({
            "manifest_version": self.manifest_version,
            "name": self.name,
            "transport": self.transport.as_str(),
            "executable": self.executable,
            "args": self.args,
            "commands": commands,
            "tools": tools,
            "providers": providers,
            "flags": flags,
            "metadata": self.metadata,
        });
        if let Some(d) = &self.display_name {
            out["display_name"] = json!(d);
        }
        if let Some(e) = &self.entrypoint {
            out["entrypoint"] = json!(e);
        }
        if let Some(w) = &self.working_directory {
            out["working_directory"] = json!(w);
        }
        out
    }

    /// Lifecycle events this extension subscribes to (`metadata.lifecycle_handlers`
    /// or `lifecycleHandlers`, entries `{event}` or plain strings).
    pub fn handles_event(&self, event: &str) -> bool {
        ["lifecycle_handlers", "lifecycleHandlers"]
            .iter()
            .any(|field| {
                self.metadata
                    .get(field)
                    .and_then(Value::as_array)
                    .is_some_and(|handlers| {
                        handlers.iter().any(|h| match h {
                            Value::String(s) => s == event,
                            Value::Object(o) => {
                                o.get("event").and_then(Value::as_str) == Some(event)
                            }
                            _ => false,
                        })
                    })
            })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn identifiers() {
        assert!(is_valid_extension_identifier("my-ext.v2_x"));
        assert!(is_valid_extension_identifier("1abc"));
        assert!(!is_valid_extension_identifier(""));
        assert!(!is_valid_extension_identifier("-abc"));
        assert!(!is_valid_extension_identifier("Abc"));
        assert!(!is_valid_extension_identifier("a b"));
    }

    #[test]
    fn decodes_and_validates() {
        let m = decode_manifest_str(
            r#"{"name":"demo","executable":"bin/run","args":["--x"],
                "commands":[{"name":"hello","title":"Hello"}],
                "tools":[{"name":"t","parameters":{"type":"object"}}],
                "flags":[{"name":"verbose"},{"name":"greet","type":"string","default":"hi"}],
                "providers":[{"name":"p","config":{"baseUrl":"x"}}]}"#,
        )
        .unwrap();
        assert_eq!(m.transport, Transport::SubprocessJsonRpc);
        assert_eq!(m.flags[0].flag_type, "boolean");
        assert!(validate_manifest(&m).is_empty());
        let round = decode_manifest(&m.to_json()).unwrap();
        assert_eq!(round, m);
    }

    #[test]
    fn archived_issue_wording() {
        let m = ExtensionManifest {
            manifest_version: 2,
            name: "Bad".into(),
            display_name: Some(String::new()),
            args: vec![String::new()],
            commands: vec![
                CommandContribution {
                    name: "a".into(),
                    title: Some(String::new()),
                    ..Default::default()
                },
                CommandContribution {
                    name: "a".into(),
                    ..Default::default()
                },
            ],
            flags: vec![FlagContribution {
                name: "f".into(),
                flag_type: "number".into(),
                default: json!(1),
                description: None,
            }],
            providers: vec![ProviderContribution {
                name: "p".into(),
                config: json!([]),
            }],
            ..Default::default()
        };
        let summary = join_issues(&validate_manifest(&m));
        assert_eq!(
            summary,
            "invalid_argument [manifest_version]: unsupported manifest version; \
             invalid_argument [name]: must use lowercase letters, digits, '.', '_' or '-'; \
             invalid_argument [display_name]: must not be empty when provided; \
             invalid_argument [executable]: must not be empty; \
             invalid_argument [args[0]]: must not be empty; \
             invalid_argument [commands[0].title]: must not be empty when provided; \
             invalid_argument [commands[1].name]: must be unique within the manifest; \
             invalid_argument [flags[0].type]: must be boolean or string; \
             invalid_argument [flags[0].default]: must match the declared flag type; \
             invalid_argument [providers[0].config]: must be a JSON object"
        );
    }

    #[test]
    fn transports() {
        assert_eq!(
            decode_manifest_str(r#"{"name":"x","transport":"angelscript"}"#).unwrap_err(),
            "unsupported extension transport"
        );
        let m = decode_manifest_str(r#"{"name":"x","transport":"rhai"}"#).unwrap();
        assert_eq!(
            join_issues(&validate_manifest(&m)),
            "invalid_argument [entrypoint]: must not be empty"
        );
        assert!(decode_manifest_str(r#"{"transport":"rhai"}"#).is_err());
    }

    #[test]
    fn handles_event_metadata() {
        let m = ExtensionManifest {
            metadata: json!({"lifecycle_handlers":[{"event":"input"}],"lifecycleHandlers":["tool_call"]}),
            ..Default::default()
        };
        assert!(m.handles_event("input"));
        assert!(m.handles_event("tool_call"));
        assert!(!m.handles_event("context"));
    }
}
