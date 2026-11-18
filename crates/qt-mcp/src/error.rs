//! Error type shared by the MCP transports, client and manager.

/// Failure of an MCP operation. Messages are already sanitized for display to the
/// model or the user (see [`crate::sanitize_mcp_error`]).
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum McpError {
    /// The server configuration cannot be used (missing command or url, bad header).
    #[error("{0}")]
    Config(String),
    /// The connection failed or was lost. Retried with backoff by the manager.
    #[error("{0}")]
    Transport(String),
    /// No response arrived within the configured timeout.
    #[error("{0}")]
    Timeout(String),
    /// The server answered with a JSON-RPC error object.
    #[error("{0}")]
    Rpc(String),
    /// The server sent something that is not a valid MCP response.
    #[error("{0}")]
    Protocol(String),
    /// The caller aborted the request.
    #[error("{0}")]
    Cancelled(String),
    /// The named server is not configured.
    #[error("MCP server not found: {0}")]
    NotFound(String),
}

impl McpError {
    /// Whether the manager should reconnect and try again.
    pub fn is_retryable(&self) -> bool {
        matches!(self, Self::Transport(_))
    }
}
