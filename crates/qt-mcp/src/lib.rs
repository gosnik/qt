//! Model Context Protocol client for qt.
//!
//! Ports the archived C++ qt MCP integration (`archived/src/tools/mcp_client.cpp`,
//! `mcp_resource_tool.cpp`, the `mcpServers` settings parsing and the provider-backed
//! sampling callback):
//!
//! - [`config`]: discovery and merge of `mcp.json` files and `mcpServers` settings.
//! - [`client`]: one JSON-RPC 2.0 session per server (protocol `2025-03-26`) over stdio,
//!   streamable HTTP or the legacy HTTP+SSE transport.
//! - [`manager`]: connects every configured server, tracks status for a `/mcp` panel and
//!   exposes tools and resources.
//! - [`tools`]: `mcp_<server>_<tool>` proxy tools plus `list_mcp_resources` and
//!   `read_mcp_resource`, implementing [`qt_core::tools::Tool`].
//! - [`sampling`]: server-initiated `sampling/createMessage` through a pluggable
//!   [`SamplingHandler`], with a provider-backed implementation.

pub mod client;
pub mod config;
pub mod error;
pub mod manager;
pub mod sampling;
pub mod tools;
mod transport;
pub mod util;

pub use client::{InitializeInfo, McpClient};
pub use config::{McpServerConfig, TransportKind};
pub use error::McpError;
pub use manager::{McpManager, McpOptions, RemoteTool, ServerSnapshot, ServerStatus};
pub use sampling::{ProviderSamplingHandler, SamplingHandler, SamplingRequest};
pub use tools::{ListMcpResourcesTool, McpProxyTool, ReadMcpResourceTool};
pub use util::{registered_tool_name, sanitize_mcp_error};

/// MCP protocol revision announced in `initialize` and the `mcp-protocol-version` header.
pub const PROTOCOL_VERSION: &str = "2025-03-26";
