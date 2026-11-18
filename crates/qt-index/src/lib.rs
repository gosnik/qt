//! Project index for qt.
//!
//! A port of the archived C++ `index` subsystem: a background service that
//! scans the project (gitignore-aware), derives build metadata from CMake,
//! Make, Ninja, qmake, waf and PlatformIO projects, writes
//! `compile_commands.json`, builds a heuristic C/C++ code index and
//! tree-sitter C++ metadata, and persists everything to SQLite with an FTS5
//! table under `<agent_home>/index/<hash>/index.sqlite3`.
//!
//! The model reaches it through six tools ([`tools::index_tools`]): `symbol`,
//! `analyze_file`, `references`, `build_context`, `search_index` and the
//! grouped `index` router (core, code, fs, git and build operations).
//!
//! ```no_run
//! use std::sync::Arc;
//! use qt_index::{IndexManager, index_tools};
//!
//! let manager = Arc::new(IndexManager::new("/home/me/.qt"));
//! manager.start(std::path::Path::new("/home/me/project")).unwrap();
//! let tools = index_tools(manager.clone(), |_| true);
//! assert_eq!(tools.len(), 6);
//! ```

pub mod build;
pub mod cancel;
pub mod code_index;
pub mod compdb;
pub mod gitignore;
pub mod ops;
pub mod scan;
pub mod schema;
pub mod service;
pub mod store;
pub mod tools;
pub mod tree_sitter_meta;
pub mod util;

pub use service::{
    BackgroundService, IndexManager, ServiceOptions, ServiceStatus, state_directory_for,
};
pub use tools::{DirectIndexTool, INDEX_TOOL_NAMES, IndexRouterTool, index_tools};
