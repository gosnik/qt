//! In-process llama.cpp provider for qt.
//!
//! Ports the archived C++ qt llama integration:
//! - [`settings`]: `settings.llama` parsing, merging, validation, path
//!   resolution and serialisation.
//! - [`gguf`] and [`discovery`]: GGUF metadata reader, local model discovery
//!   (`<cwd>/models`, `<agent_home>/models`, extra directories),
//!   quantisation names and model classification.
//! - [`offload`]: GPU layer offload planning with VRAM headroom and
//!   100/75/50/25 %/CPU fallback.
//! - [`sampling`]: effective generation settings, sampler chain plan and GBNF
//!   grammar resolution (inline, alias, file).
//! - [`prompt`]: the archived prompt/tool-calling protocol (OpenAI-style
//!   request JSON in the prompt, `tool_choice` forcing heuristic, first JSON
//!   object tool-call parsing).
//! - [`runtime`]: a single worker thread owning llama.cpp state, with model
//!   lifecycle, generation, embeddings (queue timeout, cancellation), KV
//!   cache operations and runtime status.
//! - [`provider`]: [`provider::stream`], shaped like
//!   `qt_core::provider::stream`.
//!
//! llama.cpp itself is linked through `qt-llama-sys`. When that crate was
//! built without the llama.cpp sources, everything except model execution
//! still works and model operations fail with [`LlamaError::Unavailable`].

pub mod backend;
pub mod discovery;
pub mod gguf;
#[cfg(llama_native)]
mod native;
pub mod offload;
pub mod prompt;
pub mod provider;
pub mod runtime;
pub mod sampling;
pub mod settings;

pub use backend::LlamaProviderBackend;
pub use discovery::{
    DiscoveredModel, DiscoveryOptions, DiscoveryResult, MemoryTask, ModelCapability,
    discover_models,
};
pub use offload::{GpuMemory, OffloadPlan, OffloadPlanInput, plan_offload};
pub use provider::{GenerateOptions, stream, stream_with_options};
pub use runtime::{
    EmbeddingRequest, EmbeddingResult, InstanceTag, KvCacheOperation, KvCacheState, LlamaBackend,
    LlamaRuntime, RuntimeStatus,
};
pub use settings::{LlamaSettings, ModelSettings, PROVIDER_API, PROVIDER_ID};

/// Errors from the llama runtime. Messages follow the archived C++ qt.
#[derive(Debug, Clone, PartialEq, thiserror::Error)]
pub enum LlamaError {
    #[error("{0}")]
    InvalidSettings(String),
    #[error("unknown llama model: {0}")]
    UnknownModel(String),
    #[error("llama embedding cancelled")]
    Cancelled,
    #[error("llama embedding queue timeout")]
    QueueTimeout,
    #[error("llama provider is shutting down")]
    ShuttingDown,
    #[error("llama.cpp is not available in this build: {0}")]
    Unavailable(String),
    #[error("{0}")]
    Backend(String),
}

impl LlamaError {
    pub(crate) fn backend(message: impl Into<String>) -> Self {
        Self::Backend(message.into())
    }
}

/// Whether this build links llama.cpp.
pub fn native_available() -> bool {
    cfg!(llama_native)
}

/// GPU backends compiled into llama.cpp (empty for CPU-only builds).
pub fn compiled_backends() -> Vec<&'static str> {
    qt_llama_sys::BACKENDS
        .split(',')
        .filter(|s| !s.is_empty())
        .collect()
}
