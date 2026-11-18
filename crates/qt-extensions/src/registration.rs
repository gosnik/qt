//! What a script extension registers during `activate()`, with the archived
//! validation rules and messages (`angelscript_host.cpp` `add_registration_call`).

use serde_json::{Value, json};

use crate::manifest::{
    CommandContribution, FlagContribution, ProviderContribution, ToolContribution,
    is_valid_extension_identifier,
};

/// Every lifecycle event an extension may subscribe to (archived list).
pub const LIFECYCLE_EVENTS: &[&str] = &[
    "resources_discover",
    "instructions_loaded",
    "session_start",
    "session_before_switch",
    "session_before_fork",
    "session_before_compact",
    "session_compact",
    "session_shutdown",
    "session_before_tree",
    "session_tree",
    "context",
    "before_provider_request",
    "after_provider_response",
    "before_agent_start",
    "agent_start",
    "agent_end",
    "turn_start",
    "turn_end",
    "message_start",
    "message_update",
    "message_end",
    "tool_execution_start",
    "tool_execution_update",
    "tool_execution_end",
    "model_select",
    "thinking_level_select",
    "tool_call",
    "tool_result",
    "user_bash",
    "input",
];

/// Resource types an extension can contribute.
pub const RESOURCE_TYPES: &[&str] = &["skill", "prompt", "theme"];

pub fn is_valid_lifecycle_event(event: &str) -> bool {
    LIFECYCLE_EVENTS.contains(&event)
}

/// A script function name: ASCII letter or `_`, then letters, digits, `_`.
pub fn is_valid_handler_identifier(text: &str) -> bool {
    let mut chars = text.chars();
    match chars.next() {
        Some(c) if c.is_ascii_alphabetic() || c == '_' => {}
        _ => return false,
    }
    chars.all(|c| c.is_ascii_alphanumeric() || c == '_')
}

/// Relative path without `..` segments or a drive/root prefix.
pub fn is_safe_resource_path(path: &str) -> bool {
    if path.is_empty() || path.starts_with('/') || path.starts_with('\\') {
        return false;
    }
    let bytes = path.as_bytes();
    if bytes.len() >= 2 && bytes[0].is_ascii_alphabetic() && bytes[1] == b':' {
        return false;
    }
    !path.split(['/', '\\']).any(|segment| segment == "..")
}

/// Handler function name for a contribution: `<prefix><name>` with every
/// character other than ASCII letters, digits and `_` replaced by `_`.
pub fn handler_name(prefix: &str, name: &str) -> String {
    let mut out = String::from(prefix);
    out.extend(name.chars().map(|c| {
        if c.is_ascii_alphanumeric() || c == '_' {
            c
        } else {
            '_'
        }
    }));
    out
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ResourceRegistration {
    /// `skill`, `prompt` or `theme`.
    pub kind: String,
    /// Path relative to the extension directory.
    pub path: String,
    pub name: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LifecycleHandler {
    pub event: String,
    pub handler: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ShortcutRegistration {
    /// Key sequence such as `ctrl+x`.
    pub key: String,
    pub handler: String,
    pub description: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MessageRendererRegistration {
    pub custom_type: String,
    pub handler: String,
}

/// Everything an extension registered.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct Registration {
    pub commands: Vec<CommandContribution>,
    pub tools: Vec<ToolContribution>,
    /// Explicit handler names for tools registered with a `handler` field.
    pub tool_handlers: Vec<(String, String)>,
    pub resources: Vec<ResourceRegistration>,
    pub lifecycle_handlers: Vec<LifecycleHandler>,
    pub providers: Vec<ProviderContribution>,
    pub flags: Vec<FlagContribution>,
    pub shortcuts: Vec<ShortcutRegistration>,
    pub message_renderers: Vec<MessageRendererRegistration>,
}

fn non_empty(value: Option<String>) -> Option<String> {
    value.filter(|v| !v.is_empty())
}

impl Registration {
    pub fn add_command(
        &mut self,
        name: &str,
        title: Option<String>,
        description: Option<String>,
    ) -> Result<(), String> {
        if !is_valid_extension_identifier(name) {
            return Err(format!("invalid extension contribution name: {name}"));
        }
        self.commands.push(CommandContribution {
            name: name.into(),
            title: non_empty(title),
            description: non_empty(description),
        });
        Ok(())
    }

    pub fn add_tool(
        &mut self,
        name: &str,
        title: Option<String>,
        description: Option<String>,
        parameters: Option<Value>,
        handler: Option<String>,
    ) -> Result<(), String> {
        if !is_valid_extension_identifier(name) {
            return Err(format!("invalid extension contribution name: {name}"));
        }
        if let Some(p) = &parameters
            && !p.is_object()
        {
            return Err("extension tool parameters must be an object".into());
        }
        if let Some(h) = &handler {
            if !is_valid_handler_identifier(h) {
                return Err(format!("invalid extension tool handler: {h}"));
            }
            self.tool_handlers.push((name.into(), h.clone()));
        }
        self.tools.push(ToolContribution {
            name: name.into(),
            title: non_empty(title),
            description: non_empty(description),
            parameters,
        });
        Ok(())
    }

    pub fn add_resource(
        &mut self,
        kind: &str,
        path: &str,
        name: Option<String>,
    ) -> Result<(), String> {
        if !RESOURCE_TYPES.contains(&kind) {
            return Err(format!("invalid extension resource type: {kind}"));
        }
        if path.is_empty() {
            return Err("extension resource path must not be empty".into());
        }
        if !is_safe_resource_path(path) {
            return Err("extension resource path must stay within the extension directory".into());
        }
        self.resources.push(ResourceRegistration {
            kind: kind.into(),
            path: path.into(),
            name: non_empty(name),
        });
        Ok(())
    }

    pub fn add_lifecycle_handler(&mut self, event: &str, handler: &str) -> Result<(), String> {
        if !is_valid_lifecycle_event(event) {
            return Err(format!("invalid extension lifecycle event: {event}"));
        }
        if !is_valid_handler_identifier(handler) {
            return Err(format!("invalid extension lifecycle handler: {handler}"));
        }
        self.lifecycle_handlers.push(LifecycleHandler {
            event: event.into(),
            handler: handler.into(),
        });
        Ok(())
    }

    pub fn add_provider(&mut self, name: &str, config: Value) -> Result<(), String> {
        if !is_valid_extension_identifier(name) {
            return Err(format!("invalid extension provider name: {name}"));
        }
        if !config.is_object() {
            return Err("extension provider config must be an object".into());
        }
        self.providers.push(ProviderContribution {
            name: name.into(),
            config,
        });
        Ok(())
    }

    /// `flag_type`/`default` are `None` for the one-argument form (a boolean
    /// flag without default).
    pub fn add_flag(
        &mut self,
        name: &str,
        flag_type: Option<&str>,
        default: Value,
        description: Option<String>,
    ) -> Result<(), String> {
        if !is_valid_extension_identifier(name) {
            return Err("qt_register_flag expects a name".into());
        }
        let flag_type = flag_type.unwrap_or("boolean");
        if flag_type != "boolean" && flag_type != "string" {
            return Err("extension flag type must be boolean or string".into());
        }
        if !default.is_null()
            && !((flag_type == "boolean" && default.is_boolean())
                || (flag_type == "string" && default.is_string()))
        {
            return Err("extension flag default type mismatch".into());
        }
        self.flags.push(FlagContribution {
            name: name.into(),
            description: non_empty(description),
            flag_type: flag_type.into(),
            default,
        });
        Ok(())
    }

    pub fn add_shortcut(
        &mut self,
        key: &str,
        handler: &str,
        description: Option<String>,
    ) -> Result<(), String> {
        if key.is_empty() || !is_valid_handler_identifier(handler) {
            return Err("qt_register_shortcut expects a key and handler".into());
        }
        self.shortcuts.push(ShortcutRegistration {
            key: key.into(),
            handler: handler.into(),
            description: non_empty(description),
        });
        Ok(())
    }

    pub fn add_message_renderer(&mut self, custom_type: &str, handler: &str) -> Result<(), String> {
        if custom_type.is_empty() || !is_valid_handler_identifier(handler) {
            return Err("qt_register_message_renderer expects custom type and handler".into());
        }
        self.message_renderers.push(MessageRendererRegistration {
            custom_type: custom_type.into(),
            handler: handler.into(),
        });
        Ok(())
    }

    /// Handler names subscribed to `event`, in registration order.
    pub fn handlers_for(&self, event: &str) -> impl Iterator<Item = &str> {
        self.lifecycle_handlers
            .iter()
            .filter(move |h| h.event == event)
            .map(|h| h.handler.as_str())
    }

    /// The function that runs tool `name`.
    pub fn tool_handler(&self, name: &str) -> String {
        self.tool_handlers
            .iter()
            .find(|(tool, _)| tool == name)
            .map(|(_, h)| h.clone())
            .unwrap_or_else(|| handler_name("tool_", name))
    }

    /// Shortcuts, handlers, resources and renderers as manifest metadata.
    pub fn metadata_into(&self, metadata: &mut Value) {
        if !metadata.is_object() {
            *metadata = json!({});
        }
        if !self.shortcuts.is_empty() {
            metadata["shortcuts"] = self
                .shortcuts
                .iter()
                .map(|s| {
                    json!({
                        "key": s.key,
                        "handler": s.handler,
                        "description": s.description.clone().unwrap_or_default(),
                    })
                })
                .collect();
        }
        if !self.lifecycle_handlers.is_empty() {
            metadata["lifecycle_handlers"] = self
                .lifecycle_handlers
                .iter()
                .map(|h| json!({ "event": h.event, "handler": h.handler }))
                .collect();
        }
        if !self.resources.is_empty() {
            metadata["resources"] = self
                .resources
                .iter()
                .map(|r| {
                    let mut item = json!({ "type": r.kind, "path": r.path });
                    if let Some(n) = &r.name {
                        item["name"] = json!(n);
                    }
                    item
                })
                .collect();
        }
        if !self.message_renderers.is_empty() {
            metadata["message_renderers"] = self
                .message_renderers
                .iter()
                .map(|r| json!({ "customType": r.custom_type, "handler": r.handler }))
                .collect();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn handler_names_and_paths() {
        assert_eq!(handler_name("command_", "my-cmd.x"), "command_my_cmd_x");
        assert!(is_valid_handler_identifier("_on_input2"));
        assert!(!is_valid_handler_identifier("2x"));
        assert!(!is_valid_handler_identifier("a-b"));
        assert!(is_safe_resource_path("skills/a/SKILL.md"));
        assert!(!is_safe_resource_path("../x"));
        assert!(!is_safe_resource_path("a/../../x"));
        assert!(!is_safe_resource_path("/abs"));
        assert!(!is_safe_resource_path("C:\\x"));
    }

    #[test]
    fn archived_messages() {
        let mut r = Registration::default();
        assert_eq!(
            r.add_lifecycle_handler("nope", "h").unwrap_err(),
            "invalid extension lifecycle event: nope"
        );
        assert_eq!(
            r.add_lifecycle_handler("input", "1h").unwrap_err(),
            "invalid extension lifecycle handler: 1h"
        );
        assert_eq!(
            r.add_command("Bad", None, None).unwrap_err(),
            "invalid extension contribution name: Bad"
        );
        assert_eq!(
            r.add_resource("agent", "x", None).unwrap_err(),
            "invalid extension resource type: agent"
        );
        assert_eq!(
            r.add_resource("skill", "../x", None).unwrap_err(),
            "extension resource path must stay within the extension directory"
        );
        assert_eq!(
            r.add_flag("f", Some("number"), Value::Null, None)
                .unwrap_err(),
            "extension flag type must be boolean or string"
        );
        assert_eq!(
            r.add_flag("f", Some("string"), json!(true), None)
                .unwrap_err(),
            "extension flag default type mismatch"
        );
        assert_eq!(
            r.add_provider("p", json!(1)).unwrap_err(),
            "extension provider config must be an object"
        );
        assert_eq!(
            r.add_shortcut("", "h", None).unwrap_err(),
            "qt_register_shortcut expects a key and handler"
        );
        assert_eq!(
            r.add_message_renderer("t", "bad-name").unwrap_err(),
            "qt_register_message_renderer expects custom type and handler"
        );
        r.add_command("hello", Some(String::new()), Some("d".into()))
            .unwrap();
        assert_eq!(r.commands[0].title, None);
        r.add_tool("t", None, None, None, Some("run_t".into()))
            .unwrap();
        assert_eq!(r.tool_handler("t"), "run_t");
        assert_eq!(r.tool_handler("other-tool"), "tool_other_tool");
    }
}
