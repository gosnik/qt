//! Dense query retrieval with a bounded query-embedding LRU cache (archived
//! `dense_retrieval.cpp`) and the embedding-provider contract (archived
//! `RetrievalEmbedder` / `llama_retrieval.cpp`).

use std::collections::HashMap;
use std::io::Read as _;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::SystemTime;

use serde_json::json;

use super::{Cancellation, DenseIndexConfig, DenseSearchHit};
use crate::error::{MemoryError, Result};
use crate::model::{MemoryScope, RecallPolicy};
use crate::scope::scope_key;
use crate::store::MemoryStore;
use crate::text::{dump, stable_hash_hex};

const MAXIMUM_QUERY_BYTES: usize = 32768;

/// Embedding provider (the composition root binds qt-llama).
///
/// Calls are synchronous; implementations must stop using `cancelled` before
/// returning.
pub trait Embedder: Send + Sync {
    /// Establishes (or returns the cached) embedding-space contract.
    fn configure(&self, cancelled: Cancellation<'_>) -> Result<DenseIndexConfig>;
    /// Embeds `text` in the space `config` describes.
    fn embed(
        &self,
        config: &DenseIndexConfig,
        text: &str,
        cancelled: Cancellation<'_>,
    ) -> Result<Vec<f32>>;
    /// Fails when the configured model changed since `configure`.
    fn validate(&self) -> Result<()> {
        Ok(())
    }
}

/// Output of one embedding call of a local model.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct EmbeddingOutput {
    pub embedding: Vec<f32>,
    /// Pooling reported by the model (`mean`, `cls`, `last`, `none`).
    pub pooling: String,
    /// Whether the model returned normalised vectors.
    pub normalized: bool,
}

/// Embedding function of a local model: `(model_id, text, normalize)`.
pub type EmbedFn =
    Arc<dyn Fn(&str, &str, bool, Cancellation<'_>) -> Result<EmbeddingOutput> + Send + Sync>;

struct LocalState {
    config: Option<DenseIndexConfig>,
    size: u64,
    modified: Option<SystemTime>,
}

/// [`Embedder`] for a GGUF model file served by an in-process runtime: the
/// embedding space is pinned to the model bytes (archived
/// `bind_llama_retrieval`).
pub struct LocalModelEmbedder {
    model_id: String,
    path: PathBuf,
    query_prefix: String,
    document_prefix: String,
    normalize: bool,
    embed: EmbedFn,
    state: Mutex<LocalState>,
}

impl LocalModelEmbedder {
    pub fn new(
        model_id: impl Into<String>,
        path: impl Into<PathBuf>,
        query_prefix: impl Into<String>,
        document_prefix: impl Into<String>,
        normalize: bool,
        embed: EmbedFn,
    ) -> Result<Self> {
        let model_id = model_id.into();
        let path = path.into();
        if model_id.is_empty() || path.as_os_str().is_empty() {
            return Err(MemoryError::invalid(
                "dense retrieval requires a configured local embedding model",
            ));
        }
        Ok(Self {
            model_id,
            path,
            query_prefix: query_prefix.into(),
            document_prefix: document_prefix.into(),
            normalize,
            embed,
            state: Mutex::new(LocalState {
                config: None,
                size: 0,
                modified: None,
            }),
        })
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, LocalState> {
        self.state
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    fn check_source(&self, state: &LocalState) -> Result<()> {
        let metadata = std::fs::metadata(&self.path)?;
        if metadata.len() != state.size || metadata.modified().ok() != state.modified {
            return Err(MemoryError::storage(
                "embedding model changed; restart retrieval to rebuild its generation",
            ));
        }
        Ok(())
    }
}

/// `gguf-fnv1a64:<hex>:<bytes>` over the complete model file.
pub fn model_fingerprint(path: &std::path::Path, cancelled: Cancellation<'_>) -> Result<String> {
    let mut file = std::fs::File::open(path)
        .map_err(|_| MemoryError::storage("cannot read configured embedding model"))?;
    let mut buffer = vec![0u8; 65536];
    let mut hash: u64 = 14_695_981_039_346_656_037;
    let mut bytes: u64 = 0;
    loop {
        if cancelled() {
            return Err(MemoryError::storage("embedding initialization cancelled"));
        }
        let count = file
            .read(&mut buffer)
            .map_err(|_| MemoryError::storage("unable to fingerprint embedding model"))?;
        if count == 0 {
            break;
        }
        bytes += count as u64;
        for byte in &buffer[..count] {
            hash ^= u64::from(*byte);
            hash = hash.wrapping_mul(1_099_511_628_211);
        }
    }
    if bytes == 0 {
        return Err(MemoryError::storage(
            "unable to fingerprint embedding model",
        ));
    }
    Ok(format!("gguf-fnv1a64:{hash:016x}:{bytes}"))
}

/// `dense-v1-<hash>` of the complete embedding-space identity.
pub fn dense_generation(config: &DenseIndexConfig) -> String {
    let identity = json!([
        config.model_id,
        config.model_fingerprint,
        config.tokenizer_fingerprint,
        config.query_prefix,
        config.document_prefix,
        config.pooling,
        config.dimensions,
        config.normalize,
        "llama-embedding-v1"
    ]);
    format!("dense-v1-{}", stable_hash_hex(&dump(&identity)))
}

impl Embedder for LocalModelEmbedder {
    fn configure(&self, cancelled: Cancellation<'_>) -> Result<DenseIndexConfig> {
        let mut state = self.lock();
        if let Some(config) = &state.config {
            self.check_source(&state)?;
            return Ok(config.clone());
        }
        let metadata = std::fs::metadata(&self.path)?;
        state.size = metadata.len();
        state.modified = metadata.modified().ok();
        let fingerprint = model_fingerprint(&self.path, cancelled)?;
        let mut config = DenseIndexConfig {
            model_id: self.model_id.clone(),
            model_fingerprint: fingerprint.clone(),
            // GGUF carries tokenizer metadata; pinning all bytes covers it.
            tokenizer_fingerprint: fingerprint,
            query_prefix: self.query_prefix.clone(),
            document_prefix: self.document_prefix.clone(),
            normalize: self.normalize,
            ..Default::default()
        };
        self.check_source(&state)?;
        let output = (self.embed)(
            &config.model_id,
            &format!("{}memory retrieval", config.query_prefix),
            config.normalize,
            cancelled,
        )?;
        self.check_source(&state)?;
        if cancelled() {
            return Err(MemoryError::storage("embedding initialization cancelled"));
        }
        config.dimensions = output.embedding.len();
        config.pooling = output.pooling.clone();
        let mut norm = 0.0f64;
        for value in &output.embedding {
            if !value.is_finite() {
                return Err(MemoryError::storage(
                    "embedding provider returned non-finite values",
                ));
            }
            norm += f64::from(*value) * f64::from(*value);
        }
        if config.dimensions == 0
            || config.dimensions > 65536
            || norm <= 0.0
            || !matches!(config.pooling.as_str(), "mean" | "cls" | "last" | "none")
            || output.normalized != config.normalize
        {
            return Err(MemoryError::storage(
                "embedding provider did not establish a compatible vector contract",
            ));
        }
        config.generation = dense_generation(&config);
        state.config = Some(config.clone());
        Ok(config)
    }

    fn embed(
        &self,
        config: &DenseIndexConfig,
        text: &str,
        cancelled: Cancellation<'_>,
    ) -> Result<Vec<f32>> {
        {
            let state = self.lock();
            if state
                .config
                .as_ref()
                .is_none_or(|loaded| loaded.generation != config.generation)
            {
                return Err(MemoryError::storage(
                    "requested embedding space is not loaded",
                ));
            }
            self.check_source(&state)?;
        }
        let output = (self.embed)(&config.model_id, text, config.normalize, cancelled)?;
        self.check_source(&self.lock())?;
        if output.embedding.len() != config.dimensions
            || output.pooling != config.pooling
            || output.normalized != config.normalize
        {
            return Err(MemoryError::storage(
                "embedding provider changed its vector contract",
            ));
        }
        Ok(output.embedding)
    }

    fn validate(&self) -> Result<()> {
        let state = self.lock();
        if state.config.is_some() {
            self.check_source(&state)?;
        }
        Ok(())
    }
}

/// Query-side counters.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct DenseRetrievalStats {
    pub cache_hits: usize,
    pub embedding_failures: usize,
    pub cache_entries: usize,
    pub cache_bytes: usize,
    pub last_error: String,
}

struct CacheEntry {
    embedding: Vec<f32>,
    age: usize,
    bytes: usize,
}

struct CacheState {
    entries: HashMap<String, CacheEntry>,
    bytes: usize,
    next_age: usize,
    stats: DenseRetrievalStats,
}

/// Dense search over the active generation with a bounded LRU of query
/// embeddings keyed by scope, embedding space, policy and query.
pub struct DenseRetrievalService {
    store: Arc<MemoryStore>,
    embedder: Option<Arc<dyn Embedder>>,
    cache_capacity: usize,
    cache_byte_capacity: usize,
    state: Mutex<CacheState>,
}

fn config_key(config: &DenseIndexConfig) -> String {
    let mut key = String::new();
    for value in [
        &config.generation,
        &config.model_id,
        &config.model_fingerprint,
        &config.tokenizer_fingerprint,
        &config.query_prefix,
        &config.document_prefix,
        &config.pooling,
    ] {
        key += &format!("{}:{value}|", value.len());
    }
    key += &format!("{}:{}|", config.dimensions, u8::from(config.normalize));
    key
}

fn policy_ordinal(policy: RecallPolicy) -> u8 {
    match policy {
        RecallPolicy::Strict => 0,
        RecallPolicy::Balanced => 1,
        RecallPolicy::Broad => 2,
    }
}

impl DenseRetrievalService {
    pub fn new(
        store: Arc<MemoryStore>,
        embedder: Option<Arc<dyn Embedder>>,
        cache_capacity: usize,
    ) -> Self {
        Self::with_byte_capacity(store, embedder, cache_capacity, 32 * 1024 * 1024)
    }

    pub fn with_byte_capacity(
        store: Arc<MemoryStore>,
        embedder: Option<Arc<dyn Embedder>>,
        cache_capacity: usize,
        cache_byte_capacity: usize,
    ) -> Self {
        Self {
            store,
            embedder,
            cache_capacity: cache_capacity.min(1024),
            cache_byte_capacity: cache_byte_capacity.min(32 * 1024 * 1024),
            state: Mutex::new(CacheState {
                entries: HashMap::new(),
                bytes: 0,
                next_age: 0,
                stats: DenseRetrievalStats::default(),
            }),
        }
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, CacheState> {
        self.state
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    fn fail(&self, message: impl Into<String>) -> Result<Vec<DenseSearchHit>> {
        let mut state = self.lock();
        state.stats.embedding_failures += 1;
        state.stats.last_error = message.into();
        Ok(Vec::new())
    }

    /// Best chunk per owner, best first. Embedding failures are recorded in
    /// the stats and yield no hits; scope errors are returned.
    pub fn search(
        &self,
        scope: &MemoryScope,
        query: &str,
        policy: RecallPolicy,
        include_global_user: bool,
        limit: usize,
        cancelled: Cancellation<'_>,
    ) -> Result<Vec<DenseSearchHit>> {
        if cancelled() {
            return Ok(Vec::new());
        }
        if query.len() > MAXIMUM_QUERY_BYTES {
            self.lock().stats.last_error = "dense query exceeds 32768 bytes".into();
            return Ok(Vec::new());
        }
        let Some(config) = self
            .store
            .dense_index(scope, "", policy, include_global_user)?
        else {
            return Ok(Vec::new());
        };
        if limit == 0 {
            return Ok(Vec::new());
        }
        let key = format!(
            "{}|{}|policy:{}|global:{}|query:{}{query}",
            scope_key(scope)?,
            config_key(&config),
            policy_ordinal(policy),
            u8::from(include_global_user),
            config.query_prefix
        );
        let cached = {
            let mut state = self.lock();
            state.next_age += 1;
            let age = state.next_age;
            let hit = state.entries.get_mut(&key).map(|entry| {
                entry.age = age;
                entry.embedding.clone()
            });
            if hit.is_some() {
                state.stats.cache_hits += 1;
            }
            hit
        };
        let embedding = match cached {
            Some(embedding) => embedding,
            None => {
                let Some(embedder) = &self.embedder else {
                    return self.fail("dense embedding provider unavailable");
                };
                let embedding = match embedder.embed(
                    &config,
                    &format!("{}{query}", config.query_prefix),
                    cancelled,
                ) {
                    Ok(embedding) => embedding,
                    Err(error) => return self.fail(error.to_string()),
                };
                if embedding.len() != config.dimensions || embedding.is_empty() {
                    return self.fail("dense embedding provider returned the wrong dimension");
                }
                if embedding.iter().any(|value| !value.is_finite()) {
                    return self.fail("dense embedding provider returned a non-finite value");
                }
                let norm: f64 = embedding
                    .iter()
                    .map(|value| f64::from(*value) * f64::from(*value))
                    .sum();
                if norm <= 0.0 {
                    return self.fail("dense embedding provider returned a zero vector");
                }
                if cancelled() {
                    return Ok(Vec::new());
                }
                if self.cache_capacity != 0 && self.cache_byte_capacity != 0 {
                    let mut state = self.lock();
                    state.next_age += 1;
                    let bytes = key.len()
                        + 1
                        + embedding.len() * 4
                        + std::mem::size_of::<CacheEntry>()
                        + 48;
                    let age = state.next_age;
                    if let Some(prior) = state.entries.insert(
                        key.clone(),
                        CacheEntry {
                            embedding: embedding.clone(),
                            age,
                            bytes,
                        },
                    ) {
                        state.bytes -= prior.bytes;
                    }
                    state.bytes += bytes;
                    while state.entries.len() > self.cache_capacity
                        || state.bytes > self.cache_byte_capacity
                    {
                        let Some(oldest) = state
                            .entries
                            .iter()
                            .min_by_key(|(_, entry)| entry.age)
                            .map(|(key, _)| key.clone())
                        else {
                            break;
                        };
                        if let Some(removed) = state.entries.remove(&oldest) {
                            state.bytes -= removed.bytes;
                        }
                    }
                }
                embedding
            }
        };
        if cancelled() {
            return Ok(Vec::new());
        }
        self.store.search_dense(
            scope,
            &config.generation,
            &embedding,
            policy,
            include_global_user,
            limit,
            true,
        )
    }

    pub fn stats(&self) -> DenseRetrievalStats {
        let state = self.lock();
        let mut stats = state.stats.clone();
        stats.cache_entries = state.entries.len();
        stats.cache_bytes = state.bytes;
        stats
    }

    pub fn clear_cache(&self) {
        let mut state = self.lock();
        state.entries.clear();
        state.bytes = 0;
    }
}
