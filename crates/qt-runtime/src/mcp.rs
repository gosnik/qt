//! MCP wiring: the manager built from discovered configs, its proxy and
//! resource tools as a dynamic [`ToolSource`], and sampling answered by the
//! active conversation's model.

use std::path::Path;
use std::sync::{Arc, Mutex, OnceLock};

use futures::future::BoxFuture;
use qt_core::engine::{Engine, ToolSource};
use qt_core::provider::ModelRef;
use qt_core::tools::Tool;
use qt_mcp::config::discover_server_configs;
use qt_mcp::{McpManager, ProviderSamplingHandler, SamplingHandler, SamplingRequest};
use serde_json::{Map, Value};

/// Discover MCP servers (settings layers, `mcp.json` files) and build a
/// manager; `None` when no server is configured.
pub fn build_manager(
    global: &Map<String, Value>,
    project: &Map<String, Value>,
    agent_home: &Path,
    workspace_root: &Path,
) -> Result<Option<Arc<McpManager>>, String> {
    let layers: Vec<&Map<String, Value>> = if project.is_empty() {
        vec![global]
    } else {
        vec![global, project]
    };
    let configs = discover_server_configs(&layers, agent_home, Some(workspace_root))?;
    Ok((!configs.is_empty()).then(|| McpManager::new(configs)))
}

/// Proxy (`mcp_<server>_<tool>`) and resource tools, re-read on every run so
/// reconnects and newly listed tools take effect.
pub struct McpToolSource(pub Arc<McpManager>);

impl ToolSource for McpToolSource {
    fn tools(&self) -> Vec<Arc<dyn Tool>> {
        let mut tools = self.0.proxy_tools();
        tools.extend(self.0.resource_tools());
        tools
    }
}

/// Answers `sampling/createMessage` with the model of the active
/// conversation (falling back to the default model).
pub struct EngineSamplingHandler {
    engine: Arc<OnceLock<Engine>>,
    current: Arc<Mutex<Option<ModelRef>>>,
}

impl EngineSamplingHandler {
    pub fn new(engine: Arc<OnceLock<Engine>>, current: Arc<Mutex<Option<ModelRef>>>) -> Self {
        Self { engine, current }
    }
}

impl SamplingHandler for EngineSamplingHandler {
    fn create_message(
        &self,
        request: SamplingRequest,
    ) -> BoxFuture<'static, Result<Value, String>> {
        let engine = self.engine.get().cloned();
        let model = self.current.lock().expect("model lock").clone();
        Box::pin(async move {
            let engine = engine.ok_or_else(|| "MCP sampling is not available yet".to_string())?;
            let model = model
                .or_else(|| engine.default_model())
                .ok_or_else(|| "MCP sampling requires a selected model".to_string())?;
            let provider = engine
                .provider(&model.provider)
                .filter(|p| p.is_ready())
                .ok_or_else(|| {
                    format!("MCP sampling provider is unavailable: {}", model.provider)
                })?;
            ProviderSamplingHandler::new(engine.http().clone(), provider, model.id)
                .create_message(request)
                .await
        })
    }
}
