//! Composition root for qt.
//!
//! [`Runtime::build`] turns [`RuntimeOptions`] (the CLI flags, or the
//! desktop app's defaults) into a `qt_core` [`Engine`] with every
//! subsystem wired in, as the archived `runtime_services.cpp` did:
//!
//! - the in-process llama.cpp provider ([`qt_llama::LlamaProviderBackend`])
//!   with `--discover-local-models` and the memory-child overrides;
//! - the project index tools and its background service (started only for
//!   interactive use) with a code snapshot for memory code recall;
//! - MCP servers from settings and `mcp.json` files, their proxy and
//!   resource tools and sampling through the active model;
//! - governed memory (recall augmentor, capture, memory tools, Dreaming,
//!   post-processing, semantic consolidation, dense embeddings);
//! - extensions (tools, hooks, commands, flags, providers, UI);
//! - subagents (the `subagent` tool, persistent workers and their delivery).
//!
//! The [`Runtime`] handle exposes each subsystem so front ends can build
//! panels on top of it, and [`Runtime::attach`] binds the per-conversation
//! parts (extension host, memory session, subagent coordinator).

pub mod code;
pub mod commands;
pub mod extensions;
pub mod index_commands;
pub mod llama_commands;
pub mod mcp;
pub mod mcp_commands;
pub mod memory;
pub mod options;
pub mod package_resources;
pub mod runtime;
pub mod subagents;

pub use options::{
    ModelArgument, RunMode, RuntimeOptions, parse_model_argument, resolve_model_argument,
};
pub use runtime::{AttachOptions, Runtime, changelog_markdown};
pub use subagents::RunSink;

// Subsystem crates, for front ends that build panels on the runtime.
pub use qt_extensions;
pub use qt_index;
pub use qt_llama;
pub use qt_mcp;
pub use qt_subagents;
