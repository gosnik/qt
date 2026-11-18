//! Agent tools backed by MCP servers.

use std::sync::Arc;

use futures::future::BoxFuture;
use qt_core::message::ContentBlock;
use qt_core::tools::{Tool, ToolContext, ToolOutput};
use serde_json::{Map, Value, json};

use crate::manager::{McpManager, RemoteTool};
use crate::util::{intern, pretty, sanitize_mcp_error};

/// Forwards calls to one remote tool. Offered to the model as `mcp_<server>_<tool>`
/// with the server's `inputSchema`.
pub struct McpProxyTool {
    remote: RemoteTool,
    manager: Arc<McpManager>,
    name: &'static str,
    snippet: &'static str,
}

impl McpProxyTool {
    pub fn new(remote: RemoteTool, manager: Arc<McpManager>) -> Self {
        let name = intern(&remote.registered_name);
        let first_line = remote.description.lines().next().unwrap_or_default();
        let snippet = intern(&format!(
            "MCP {}/{}: {}",
            remote.server_name,
            remote.remote_name,
            qt_core::util::shorten(first_line.trim(), 160)
        ));
        Self {
            remote,
            manager,
            name,
            snippet,
        }
    }

    pub fn remote(&self) -> &RemoteTool {
        &self.remote
    }
}

impl Tool for McpProxyTool {
    fn name(&self) -> &'static str {
        self.name
    }

    fn description(&self) -> String {
        format!(
            "MCP {}/{}: {}",
            self.remote.server_name, self.remote.remote_name, self.remote.description
        )
    }

    fn snippet(&self) -> &'static str {
        self.snippet
    }

    fn parameters(&self) -> Value {
        self.remote.input_schema.clone()
    }

    fn execute(&self, args: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        let manager = self.manager.clone();
        let remote = self.remote.clone();
        let name = self.name;
        Box::pin(async move {
            let args = match args {
                Value::Null => json!({}),
                Value::Object(map) => Value::Object(map),
                Value::String(raw) => {
                    return ToolOutput::error(format!(
                        "Invalid arguments for {name}: not valid JSON: {}",
                        qt_core::util::shorten(&raw, 200)
                    ));
                }
                _ => {
                    return ToolOutput::error(format!(
                        "Invalid arguments for {name}: expected a JSON object"
                    ));
                }
            };
            match manager
                .call_tool(
                    &remote.server_name,
                    &remote.remote_name,
                    args,
                    Some(&ctx.cancel),
                )
                .await
            {
                Ok(result) => call_result_output(&remote, result),
                Err(err) => ToolOutput::error(sanitize_mcp_error(&err.to_string())),
            }
        })
    }
}

/// Convert MCP content items into text plus image blocks.
fn render_content(items: &[Value]) -> (Vec<String>, Vec<ContentBlock>) {
    let mut texts = Vec::new();
    let mut images = Vec::new();
    for item in items {
        let kind = item.get("type").and_then(Value::as_str).unwrap_or_default();
        match kind {
            "text" => {
                if let Some(text) = item.get("text").and_then(Value::as_str) {
                    texts.push(text.to_string());
                }
            }
            "image" => {
                if let (Some(data), Some(mime)) = (
                    item.get("data").and_then(Value::as_str),
                    item.get("mimeType").and_then(Value::as_str),
                ) {
                    images.push(ContentBlock::Image {
                        data: data.to_string(),
                        mime_type: mime.to_string(),
                    });
                }
            }
            "resource" => {
                if let Some(text) = item
                    .get("resource")
                    .and_then(|r| r.get("text"))
                    .and_then(Value::as_str)
                {
                    texts.push(text.to_string());
                }
            }
            _ => {}
        }
    }
    (texts, images)
}

/// Build the tool output for a `CallToolResult`: text items joined by newlines (the
/// whole result as JSON when there are none), image items as image blocks.
pub(crate) fn call_result_output(remote: &RemoteTool, result: Value) -> ToolOutput {
    let items = result
        .get("content")
        .and_then(Value::as_array)
        .cloned()
        .unwrap_or_default();
    let (texts, images) = render_content(&items);
    let is_error = result
        .get("isError")
        .and_then(Value::as_bool)
        .unwrap_or(false);
    let text = if !texts.is_empty() {
        texts.join("\n")
    } else if images.is_empty() {
        if is_error && items.is_empty() {
            "MCP tool returned an error".to_string()
        } else {
            pretty(&result)
        }
    } else {
        String::new()
    };
    let mut content = Vec::new();
    if !text.is_empty() {
        content.push(ContentBlock::text(text));
    }
    content.extend(images);
    ToolOutput {
        content,
        is_error,
        details: Some(json!({
            "server": remote.server_name,
            "tool": remote.remote_name,
            "mcp": result,
        })),
    }
}

/// Validated `server`/`mcp_name`/... arguments.
struct ResourceArgs {
    server: Option<String>,
    cursor: Option<String>,
    uri: Option<String>,
}

fn parse_resource_args(tool: &str, args: Value, allowed: &[&str]) -> Result<ResourceArgs, String> {
    let object: Map<String, Value> = match args {
        Value::Null => Map::new(),
        Value::Object(map) => map,
        Value::String(raw) => {
            return Err(format!(
                "Invalid arguments for {tool}: not valid JSON: {}",
                qt_core::util::shorten(&raw, 200)
            ));
        }
        _ => {
            return Err(format!(
                "Invalid arguments for {tool}: expected a JSON object"
            ));
        }
    };
    if object.keys().any(|k| !allowed.contains(&k.as_str())) {
        let list = match allowed {
            [a, b, c] => format!("{a}, {b}, and {c}"),
            other => other.join(", "),
        };
        return Err(format!("{tool} accepts only {list} arguments"));
    }
    let string = |key: &str| -> Result<Option<String>, String> {
        match object.get(key) {
            None | Some(Value::Null) => Ok(None),
            Some(Value::String(s)) if s.is_empty() => Ok(None),
            Some(Value::String(s)) => Ok(Some(s.clone())),
            Some(_) => Err(format!("{tool} requires string {key}")),
        }
    };
    let server = string("server")?;
    let alias = string("mcp_name")?;
    if let (Some(a), Some(b)) = (&server, &alias)
        && a != b
    {
        return Err(format!("{tool} accepts server or mcp_name, not both"));
    }
    Ok(ResourceArgs {
        server: server.or(alias),
        cursor: if allowed.contains(&"cursor") {
            string("cursor")?
        } else {
            None
        },
        uri: if allowed.contains(&"uri") {
            string("uri")?
        } else {
            None
        },
    })
}

const LIST_DESCRIPTION: &str = "List MCP resources exposed by integrated MCP servers.";
const READ_DESCRIPTION: &str = "Read a resource from an integrated MCP server.";

/// `list_mcp_resources`: resources of every connected server, or one server's page.
pub struct ListMcpResourcesTool {
    manager: Arc<McpManager>,
}

impl ListMcpResourcesTool {
    pub fn new(manager: Arc<McpManager>) -> Self {
        Self { manager }
    }
}

impl Tool for ListMcpResourcesTool {
    fn name(&self) -> &'static str {
        "list_mcp_resources"
    }

    fn description(&self) -> String {
        LIST_DESCRIPTION.into()
    }

    fn snippet(&self) -> &'static str {
        LIST_DESCRIPTION
    }

    fn parameters(&self) -> Value {
        json!({
            "type": "object",
            "properties": {
                "server": {"type": "string", "description": "Optional MCP server name."},
                "mcp_name": {"type": "string", "description": "Crush-compatible alias for server."},
                "cursor": {"type": "string", "description": "Optional server-specific pagination cursor."}
            },
            "additionalProperties": false
        })
    }

    fn parallel_safe(&self) -> bool {
        true
    }

    fn execute(&self, args: Value, _ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        let manager = self.manager.clone();
        Box::pin(async move {
            let args = match parse_resource_args(
                "list_mcp_resources",
                args,
                &["server", "mcp_name", "cursor"],
            ) {
                Ok(args) => args,
                Err(message) => return ToolOutput::error(message),
            };
            if args.cursor.is_some() && args.server.is_none() {
                return ToolOutput::error("list_mcp_resources cursor requires a server");
            }
            let listing = match manager
                .list_resources(args.server.as_deref(), args.cursor.as_deref())
                .await
            {
                Ok(listing) => listing,
                Err(err) => return ToolOutput::error(sanitize_mcp_error(&err.to_string())),
            };
            ToolOutput::text(listing_text(&listing)).with_details(listing)
        })
    }
}

fn listing_text(listing: &Value) -> String {
    let resources = listing
        .get("resources")
        .and_then(Value::as_array)
        .cloned()
        .unwrap_or_default();
    let mut text = if resources.is_empty() {
        "No MCP resources available".to_string()
    } else {
        pretty(&Value::Array(resources))
    };
    if let Some(cursor) = listing.get("nextCursor").and_then(Value::as_str) {
        text.push_str(&format!(
            "\n\nMore resources available. Call list_mcp_resources again with cursor: {cursor}"
        ));
    }
    for error in listing
        .get("errors")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
    {
        text.push_str(&format!(
            "\n\nMCP server {} failed: {}",
            error["server"].as_str().unwrap_or_default(),
            error["error"].as_str().unwrap_or_default()
        ));
    }
    text
}

/// `read_mcp_resource`: `resources/read` on one server.
pub struct ReadMcpResourceTool {
    manager: Arc<McpManager>,
}

impl ReadMcpResourceTool {
    pub fn new(manager: Arc<McpManager>) -> Self {
        Self { manager }
    }
}

impl Tool for ReadMcpResourceTool {
    fn name(&self) -> &'static str {
        "read_mcp_resource"
    }

    fn description(&self) -> String {
        READ_DESCRIPTION.into()
    }

    fn snippet(&self) -> &'static str {
        READ_DESCRIPTION
    }

    fn parameters(&self) -> Value {
        json!({
            "type": "object",
            "properties": {
                "server": {"type": "string", "description": "MCP server name."},
                "mcp_name": {"type": "string", "description": "Crush-compatible alias for server."},
                "uri": {"type": "string", "description": "Resource URI to read."}
            },
            "required": ["server", "uri"],
            "additionalProperties": false
        })
    }

    fn parallel_safe(&self) -> bool {
        true
    }

    fn execute(&self, args: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        let manager = self.manager.clone();
        Box::pin(async move {
            let args = match parse_resource_args(
                "read_mcp_resource",
                args,
                &["server", "mcp_name", "uri"],
            ) {
                Ok(args) => args,
                Err(message) => return ToolOutput::error(message),
            };
            let (Some(server), Some(uri)) = (args.server, args.uri) else {
                return ToolOutput::error("read_mcp_resource requires non-empty server and uri");
            };
            match manager
                .read_resource(&server, &uri, Some(&ctx.cancel))
                .await
            {
                Ok(result) => read_result_output(&server, &uri, result),
                Err(err) => ToolOutput::error(sanitize_mcp_error(&err.to_string())),
            }
        })
    }
}

/// Text contents joined by newlines, image blobs as image blocks, other blobs
/// summarised; the whole payload as JSON when nothing is renderable.
pub(crate) fn read_result_output(server: &str, uri: &str, result: Value) -> ToolOutput {
    let contents = result
        .get("contents")
        .and_then(Value::as_array)
        .cloned()
        .unwrap_or_default();
    if contents.is_empty() {
        return ToolOutput::error(format!("MCP resource not found: {uri}"));
    }
    let mut texts = Vec::new();
    let mut images = Vec::new();
    for item in &contents {
        let item_uri = item.get("uri").and_then(Value::as_str).unwrap_or(uri);
        let mime = item.get("mimeType").and_then(Value::as_str);
        if let Some(text) = item.get("text").and_then(Value::as_str) {
            texts.push(text.to_string());
        } else if let Some(blob) = item.get("blob").and_then(Value::as_str) {
            match mime {
                Some(mime) if mime.starts_with("image/") => images.push(ContentBlock::Image {
                    data: blob.to_string(),
                    mime_type: mime.to_string(),
                }),
                _ => texts.push(format!(
                    "[binary resource {item_uri} ({}), {} bytes base64]",
                    mime.unwrap_or("application/octet-stream"),
                    blob.len()
                )),
            }
        }
    }
    let mut content = Vec::new();
    if !texts.is_empty() {
        content.push(ContentBlock::text(texts.join("\n")));
    } else if images.is_empty() {
        content.push(ContentBlock::text(pretty(&result)));
    }
    content.extend(images);
    ToolOutput {
        content,
        is_error: false,
        details: Some(json!({"server": server, "uri": uri, "contents": contents})),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn remote() -> RemoteTool {
        RemoteTool {
            server_name: "s".into(),
            remote_name: "t".into(),
            registered_name: "mcp_s_t".into(),
            description: "Does t".into(),
            input_schema: json!({"type": "object"}),
        }
    }

    #[test]
    fn call_results_render_text_images_and_errors() {
        let out = call_result_output(
            &remote(),
            json!({"content": [
                {"type": "text", "text": "a"},
                {"type": "image", "data": "QQ==", "mimeType": "image/png"},
                {"type": "resource", "resource": {"uri": "x", "text": "b"}},
                {"type": "audio", "data": "zz"}
            ]}),
        );
        assert!(!out.is_error);
        assert_eq!(out.text_content(), "a\nb");
        assert_eq!(out.content.len(), 2);
        assert_eq!(out.details.as_ref().unwrap()["server"], "s");

        let structured = json!({"content": [], "structuredContent": {"n": 1}});
        let out = call_result_output(&remote(), structured.clone());
        assert_eq!(
            out.text_content(),
            serde_json::to_string_pretty(&structured).unwrap()
        );

        let out = call_result_output(&remote(), json!({"isError": true}));
        assert!(out.is_error);
        assert_eq!(out.text_content(), "MCP tool returned an error");
        let out = call_result_output(
            &remote(),
            json!({"isError": true, "content": [{"type": "text", "text": "boom"}]}),
        );
        assert!(out.is_error);
        assert_eq!(out.text_content(), "boom");
    }

    #[test]
    fn resource_arguments_follow_archived_validation() {
        let list = ["server", "mcp_name", "cursor"];
        let err = |args: Value| {
            parse_resource_args("list_mcp_resources", args, &list)
                .err()
                .unwrap()
        };
        assert_eq!(
            err(json!({"bogus": 1})),
            "list_mcp_resources accepts only server, mcp_name, and cursor arguments"
        );
        assert_eq!(
            err(json!({"server": 1})),
            "list_mcp_resources requires string server"
        );
        assert_eq!(
            err(json!({"server": "a", "mcp_name": "b"})),
            "list_mcp_resources accepts server or mcp_name, not both"
        );
        assert!(err(Value::String("{x".into())).contains("not valid JSON"));
        let ok = parse_resource_args(
            "list_mcp_resources",
            json!({"mcp_name": "a", "cursor": ""}),
            &list,
        )
        .unwrap();
        assert_eq!(ok.server.as_deref(), Some("a"));
        assert_eq!(ok.cursor, None);
        let ok = parse_resource_args("list_mcp_resources", Value::Null, &list).unwrap();
        assert!(ok.server.is_none());
    }

    #[test]
    fn listing_and_read_rendering() {
        assert_eq!(
            listing_text(&json!({"resources": [], "errors": []})),
            "No MCP resources available"
        );
        let text = listing_text(&json!({
            "resources": [{"uri": "u", "server": "s"}],
            "nextCursor": "c2",
            "errors": [{"server": "t", "error": "down"}]
        }));
        assert!(text.starts_with("[\n  {\n    \"uri\": \"u\""), "{text}");
        assert!(text.contains("cursor: c2"));
        assert!(text.ends_with("MCP server t failed: down"));

        let out = read_result_output(
            "s",
            "file:///a",
            json!({"contents": [
                {"uri": "file:///a", "text": "hello"},
                {"uri": "file:///b", "blob": "AAAA", "mimeType": "application/pdf"},
                {"uri": "file:///c", "blob": "QQ==", "mimeType": "image/png"}
            ]}),
        );
        assert_eq!(
            out.text_content(),
            "hello\n[binary resource file:///b (application/pdf), 4 bytes base64]"
        );
        assert_eq!(out.content.len(), 2);
        let missing = read_result_output("s", "u", json!({"contents": []}));
        assert!(missing.is_error);
        assert_eq!(missing.text_content(), "MCP resource not found: u");
    }
}
