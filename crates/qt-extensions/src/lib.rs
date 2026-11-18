//! Extensions for qt (PLAN milestone 10).
//!
//! - [`loader`]: discovery (`-e` paths, `<workspace>/.pi/extensions`,
//!   `<agent_home>/extensions`, settings packages) and loading of
//!   `extension.json` manifests, `package.json` `pi.extensions`, Rhai
//!   scripts and Pi TypeScript/JavaScript modules (static scan only).
//! - [`script`]: the sandboxed Rhai runtime with the registration API.
//! - [`runtime`]: [`Extensions`], which owns the loaded set, dispatches host
//!   calls (`pi.*`, `ctx.*`, `ctx.ui.*`), runs commands and tools and routes
//!   lifecycle events through `qt_core` hooks.
//! - [`host`] / [`ui`]: the session and presentation traits behind host
//!   calls, with a `Conversation` adapter and a headless UI.
//! - [`subprocess`] / [`jsonrpc`]: line-delimited JSON-RPC 2.0 extensions.
//! - [`packages`]: `qt install/remove/update/list` and `qt config`.

pub mod events;
pub mod filters;
pub mod host;
pub mod jsonrpc;
pub mod loader;
pub mod manifest;
pub mod packages;
pub mod reference;
pub mod registration;
pub mod runtime;
pub mod script;
pub mod subprocess;
pub mod tool;
pub mod ui;

pub use events::InputSource;
pub use host::{ConversationHost, ExtensionHost};
pub use loader::{
    DiscoveryOptions, ExtensionOrigin, LoadResult, LoadedExtension, discover_extensions,
};
pub use runtime::{CommandOutcome, ExtensionCommand, Extensions, InputOutcome};
pub use script::{SandboxPolicy, ScriptRuntime};
pub use tool::ExtensionTool;
pub use ui::{ExtensionUi, HeadlessUi};
