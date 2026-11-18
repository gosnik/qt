//! Extension tools as `qt_core` tools (archived `ExtensionContributionTool`).

use std::sync::Arc;

use futures::future::BoxFuture;
use serde_json::{Value, json};

use qt_core::message::ContentBlock;
use qt_core::tools::{Tool, ToolContext, ToolOutput};

use crate::filters::display_path;
use crate::manifest::ToolContribution;
use crate::runtime::Extensions;

/// A tool contributed by an extension. Rhai handlers receive the arguments
/// object; subprocess extensions receive `qt.tool.invoke {name, arguments}`.
/// A result with a `content` block array (Pi's tool result shape) becomes
/// those blocks with `details`; a string becomes text; anything else is
/// returned as compact JSON text with the value as details.
pub struct ExtensionTool {
    extensions: Extensions,
    name: &'static str,
    description: String,
    parameters: Value,
}

impl ExtensionTool {
    pub fn new(extensions: Extensions, contribution: &ToolContribution) -> Self {
        // Tool names are `'static` in qt-core; the few bytes per extension
        // tool (and per reload) are leaked deliberately.
        let name: &'static str = Box::leak(contribution.name.clone().into_boxed_str());
        let description = contribution
            .description
            .clone()
            .or_else(|| contribution.title.clone())
            .unwrap_or_else(|| format!("Extension tool {}", contribution.name));
        let parameters = contribution
            .parameters
            .clone()
            .unwrap_or_else(|| json!({ "type": "object", "additionalProperties": true }));
        Self {
            extensions,
            name,
            description,
            parameters,
        }
    }
}

/// Convert a tool handler result to a tool output.
pub fn tool_output(value: Value) -> ToolOutput {
    if let Some(content) = value.get("content").filter(|c| c.is_array())
        && let Ok(blocks) = serde_json::from_value::<Vec<ContentBlock>>(content.clone())
    {
        return ToolOutput {
            content: blocks,
            is_error: value
                .get("isError")
                .and_then(Value::as_bool)
                .unwrap_or(false),
            details: value.get("details").cloned().filter(|d| !d.is_null()),
        };
    }
    match value {
        Value::String(text) => ToolOutput::text(text),
        Value::Object(_) => ToolOutput::text(value.to_string()).with_details(value),
        other => ToolOutput::text(other.to_string()).with_details(json!({ "result": other })),
    }
}

impl Tool for ExtensionTool {
    fn name(&self) -> &'static str {
        self.name
    }

    fn description(&self) -> String {
        self.description.clone()
    }

    fn snippet(&self) -> &'static str {
        "Extension-provided tool"
    }

    fn parameters(&self) -> Value {
        self.parameters.clone()
    }

    fn execute(&self, args: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        let extensions = self.extensions.clone();
        let name = self.name;
        Box::pin(async move {
            let args = if args.is_object() { args } else { json!({}) };
            let cancel = ctx.cancel.clone();
            let ext = extensions.clone();
            let run = move || ext.run_tool(name, &args, Some(&cancel));
            let result = match tokio::runtime::Handle::try_current() {
                Ok(_) => tokio::task::spawn_blocking(run).await.unwrap_or_else(|e| {
                    Err(format!("extension tool '{name}' execution failed: {e}"))
                }),
                Err(_) => run(),
            };
            match result {
                Ok(value) => tool_output(value),
                Err(message) => {
                    let code = if message.starts_with("not_implemented:") {
                        "not_implemented"
                    } else {
                        "execution_error"
                    };
                    let extension = extensions
                        .tool_extension_path(name)
                        .map(|p| display_path(&p))
                        .unwrap_or_default();
                    ToolOutput::error(message.clone()).with_details(json!({
                        "code": code,
                        "tool": name,
                        "extension": extension,
                        "message": message,
                    }))
                }
            }
        })
    }
}

impl Extensions {
    /// Every extension tool as a `qt_core` tool.
    pub fn tools(&self) -> Vec<Arc<dyn Tool>> {
        self.snapshot()
            .extensions
            .iter()
            .flat_map(|e| e.manifest.tools.clone())
            .map(|t| Arc::new(ExtensionTool::new(self.clone(), &t)) as Arc<dyn Tool>)
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn output_shapes() {
        let o = tool_output(json!({"content":[{"type":"text","text":"hi"}],"details":{"n":1}}));
        assert_eq!(o.text_content(), "hi");
        assert_eq!(o.details, Some(json!({"n":1})));
        assert_eq!(tool_output(json!("plain")).text_content(), "plain");
        let o = tool_output(json!({"a":1}));
        assert_eq!(o.text_content(), "{\"a\":1}");
        assert_eq!(o.details, Some(json!({"a":1})));
        let o = tool_output(json!(5));
        assert_eq!(o.details, Some(json!({"result":5})));
    }
}
