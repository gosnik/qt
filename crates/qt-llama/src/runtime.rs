//! llama runtime: one worker thread owns every llama.cpp object.
//!
//! All model work (load, generate, embed, KV cache operations, status) is
//! queued onto a single worker thread, as in the archived `LlamaProvider`.
//! Instances are keyed by [`InstanceTag`] so an embedding model and a chat
//! model can stay loaded side by side. Model loading follows the offload plan
//! ([`crate::offload`]) and retries smaller GPU layer counts down to CPU.

use std::collections::{BTreeMap, VecDeque};
use std::panic::AssertUnwindSafe;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::mpsc;
use std::sync::{Arc, Condvar, Mutex};
use std::thread::{JoinHandle, ThreadId};
use std::time::{Duration, Instant};

use serde_json::{Value, json};

use crate::LlamaError;
use crate::offload::{GpuMemory, OffloadPlanInput, plan_offload};
use crate::sampling::SamplerStep;
use crate::settings::{
    BackendSettings, ContextSettings, EmbeddingSettings, GenerationSettings, KvCacheSettings,
    LlamaSettings, ModelSettings,
};

/// Independent model instances kept by the runtime.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum InstanceTag {
    Embed,
    Rank,
    Instruct,
    Chat,
}

impl InstanceTag {
    pub fn parse(value: &str) -> Option<Self> {
        Some(match value {
            "embed" => Self::Embed,
            "rank" => Self::Rank,
            "instruct" => Self::Instruct,
            "chat" => Self::Chat,
            _ => return None,
        })
    }

    pub fn as_str(self) -> &'static str {
        match self {
            Self::Embed => "embed",
            Self::Rank => "rank",
            Self::Instruct => "instruct",
            Self::Chat => "chat",
        }
    }
}

/// Everything needed to load a model and create its context.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct LoadRequest {
    pub model: ModelSettings,
    pub backend: BackendSettings,
    pub context: ContextSettings,
    pub embeddings: EmbeddingSettings,
}

/// One generation call.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct GenerateRequest {
    pub model_id: String,
    pub prompt: String,
    pub generation: GenerationSettings,
    pub kv_cache: KvCacheSettings,
    pub sampler_plan: Vec<SamplerStep>,
    pub max_output_tokens: i64,
    pub metadata: Value,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum GenerateStop {
    /// The model produced an end-of-generation token.
    #[default]
    EndTurn,
    MaxTokens,
    Aborted,
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct GenerateResult {
    pub text: String,
    pub input_tokens: u64,
    pub output_tokens: u64,
    pub stop: GenerateStop,
    pub metadata: Value,
}

/// Cooperative cancellation predicate shared with the caller.
pub type CancelFn = Arc<dyn Fn() -> bool + Send + Sync>;

/// An embedding call.
#[derive(Clone)]
pub struct EmbeddingRequest {
    pub model_id: String,
    pub text: String,
    /// Default normalisation; `llama.embeddings.normalize` overrides it.
    pub normalize: bool,
    pub metadata: Value,
    /// Polled while queued and during decode; latched once it returns true.
    pub cancelled: Option<CancelFn>,
    /// Bounds the time spent waiting in the queue (not loading or decoding).
    pub queue_timeout: Duration,
}

impl EmbeddingRequest {
    pub fn new(model_id: impl Into<String>, text: impl Into<String>) -> Self {
        Self {
            model_id: model_id.into(),
            text: text.into(),
            normalize: true,
            metadata: json!({}),
            cancelled: None,
            queue_timeout: Duration::from_millis(250),
        }
    }
}

impl std::fmt::Debug for EmbeddingRequest {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("EmbeddingRequest")
            .field("model_id", &self.model_id)
            .field("text_len", &self.text.len())
            .field("normalize", &self.normalize)
            .field("queue_timeout", &self.queue_timeout)
            .finish()
    }
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct EmbeddingResult {
    pub embedding: Vec<f32>,
    pub input_tokens: u64,
    pub metadata: Value,
}

/// KV cache operations on sequence `seq_id` (positions `[from, to)`, `-1`
/// meaning open-ended).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum KvCacheOperation {
    Inspect {
        seq_id: i32,
    },
    Clear,
    Keep {
        seq_id: i32,
    },
    Drop {
        seq_id: i32,
        from: i32,
        to: i32,
    },
    Shift {
        seq_id: i32,
        from: i32,
        to: i32,
        delta: i32,
    },
}

impl KvCacheOperation {
    pub fn seq_id(&self) -> i32 {
        match *self {
            Self::Inspect { seq_id }
            | Self::Keep { seq_id }
            | Self::Drop { seq_id, .. }
            | Self::Shift { seq_id, .. } => seq_id,
            Self::Clear => 0,
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct KvCacheState {
    pub loaded_model_id: Option<String>,
    pub seq_id: i32,
    pub position_min: i32,
    pub position_max: i32,
    pub sequence_position: i64,
    pub can_shift: bool,
    pub mutated: bool,
    pub metadata: Value,
}

impl KvCacheState {
    pub fn empty(seq_id: i32) -> Self {
        Self {
            loaded_model_id: None,
            seq_id,
            position_min: -1,
            position_max: -1,
            sequence_position: 0,
            can_shift: false,
            mutated: false,
            metadata: json!({}),
        }
    }

    pub fn to_json(&self) -> Value {
        json!({
            "loadedModelId": self.loaded_model_id,
            "seqId": self.seq_id,
            "positionMin": self.position_min,
            "positionMax": self.position_max,
            "sequencePosition": self.sequence_position,
            "canShift": self.can_shift,
            "mutated": self.mutated,
            "metadata": self.metadata,
        })
    }
}

/// Runtime status for one instance (`/llama status`).
#[derive(Debug, Clone, PartialEq)]
pub struct RuntimeStatus {
    pub loaded_model_id: Option<String>,
    pub model_loaded: bool,
    pub context_loaded: bool,
    pub embeddings_enabled: bool,
    pub model_size_bytes: u64,
    pub state_size_bytes: u64,
    pub context_size: u32,
    pub sequence_position: i64,
    pub gpu_offload_supported: bool,
    pub max_devices: usize,
    pub backend: Value,
    pub memory: Value,
    pub metadata: Value,
}

impl Default for RuntimeStatus {
    fn default() -> Self {
        Self {
            loaded_model_id: None,
            model_loaded: false,
            context_loaded: false,
            embeddings_enabled: false,
            model_size_bytes: 0,
            state_size_bytes: 0,
            context_size: 0,
            sequence_position: 0,
            gpu_offload_supported: false,
            max_devices: 0,
            backend: json!({}),
            memory: json!({}),
            metadata: json!({}),
        }
    }
}

impl RuntimeStatus {
    pub fn to_json(&self) -> Value {
        json!({
            "loadedModelId": self.loaded_model_id,
            "modelLoaded": self.model_loaded,
            "contextLoaded": self.context_loaded,
            "embeddingsEnabled": self.embeddings_enabled,
            "modelSizeBytes": self.model_size_bytes,
            "stateSizeBytes": self.state_size_bytes,
            "contextSize": self.context_size,
            "sequencePosition": self.sequence_position,
            "gpuOffloadSupported": self.gpu_offload_supported,
            "maxDevices": self.max_devices,
            "backend": self.backend,
            "memory": self.memory,
            "metadata": self.metadata,
        })
    }
}

/// A llama.cpp model/context holder. The runtime calls it only from its
/// worker thread; implementations need not be thread-safe.
pub trait LlamaBackend {
    /// Load `request.model`, replacing any loaded model. `cancelled` is
    /// polled at load progress checkpoints.
    fn load(
        &mut self,
        request: &LoadRequest,
        cancelled: &dyn Fn() -> bool,
    ) -> Result<(), LlamaError>;
    fn unload(&mut self);
    fn loaded_model_id(&self) -> Option<String>;
    fn loaded_embeddings_enabled(&self) -> bool;
    /// Free/total memory of GPU devices (for offload planning).
    fn gpu_memory(&self) -> Vec<GpuMemory> {
        Vec::new()
    }
    fn runtime_status(&self) -> RuntimeStatus;
    /// Generate, calling `on_token` with UTF-8 text as it is produced.
    fn generate(
        &mut self,
        request: &GenerateRequest,
        on_token: &mut dyn FnMut(&str),
        cancelled: &dyn Fn() -> bool,
    ) -> Result<GenerateResult, LlamaError>;
    fn embed(
        &mut self,
        request: &EmbeddingRequest,
        cancelled: &dyn Fn() -> bool,
    ) -> Result<EmbeddingResult, LlamaError>;
    fn kv_cache_control(&mut self, operation: &KvCacheOperation) -> KvCacheState;
}

/// Creates a backend for a new instance (called on the worker thread).
pub type BackendFactory = Arc<dyn Fn() -> Box<dyn LlamaBackend> + Send + Sync>;

/// Backend used when this build does not link llama.cpp.
#[cfg_attr(llama_native, allow(dead_code))]
struct UnavailableBackend;

impl LlamaBackend for UnavailableBackend {
    fn load(
        &mut self,
        _request: &LoadRequest,
        _cancelled: &dyn Fn() -> bool,
    ) -> Result<(), LlamaError> {
        Err(LlamaError::Unavailable(
            qt_llama_sys::UNAVAILABLE_REASON.to_string(),
        ))
    }
    fn unload(&mut self) {}
    fn loaded_model_id(&self) -> Option<String> {
        None
    }
    fn loaded_embeddings_enabled(&self) -> bool {
        false
    }
    fn runtime_status(&self) -> RuntimeStatus {
        RuntimeStatus {
            metadata: json!({"source": "unavailable", "reason": qt_llama_sys::UNAVAILABLE_REASON}),
            ..Default::default()
        }
    }
    fn generate(
        &mut self,
        _request: &GenerateRequest,
        _on_token: &mut dyn FnMut(&str),
        _cancelled: &dyn Fn() -> bool,
    ) -> Result<GenerateResult, LlamaError> {
        Err(LlamaError::Unavailable(
            qt_llama_sys::UNAVAILABLE_REASON.to_string(),
        ))
    }
    fn embed(
        &mut self,
        _request: &EmbeddingRequest,
        _cancelled: &dyn Fn() -> bool,
    ) -> Result<EmbeddingResult, LlamaError> {
        Err(LlamaError::Unavailable(
            qt_llama_sys::UNAVAILABLE_REASON.to_string(),
        ))
    }
    fn kv_cache_control(&mut self, operation: &KvCacheOperation) -> KvCacheState {
        KvCacheState::empty(operation.seq_id())
    }
}

/// The backend factory for this build (native llama.cpp when linked).
pub fn default_backend_factory() -> BackendFactory {
    #[cfg(llama_native)]
    {
        Arc::new(|| Box::new(crate::native::NativeBackend::new()) as Box<dyn LlamaBackend>)
    }
    #[cfg(not(llama_native))]
    {
        Arc::new(|| Box::new(UnavailableBackend) as Box<dyn LlamaBackend>)
    }
}

/// Latches the first `true` from a cancellation predicate and serialises
/// evaluation (archived `LlamaCancellation`).
struct Latch {
    callback: Option<CancelFn>,
    stopped: Mutex<bool>,
}

impl Latch {
    fn new(callback: Option<CancelFn>) -> Self {
        Self {
            callback,
            stopped: Mutex::new(false),
        }
    }

    fn poll(&self) -> bool {
        let mut stopped = self.stopped.lock().unwrap_or_else(|e| e.into_inner());
        if *stopped {
            return true;
        }
        if let Some(callback) = &self.callback {
            *stopped = std::panic::catch_unwind(AssertUnwindSafe(|| callback())).unwrap_or(true);
        }
        *stopped
    }
}

/// State owned by the worker thread.
pub struct WorkerState {
    settings: Arc<LlamaSettings>,
    factory: BackendFactory,
    backends: BTreeMap<InstanceTag, Box<dyn LlamaBackend>>,
    offload_status: BTreeMap<InstanceTag, Value>,
    automatic_gpu_offload: bool,
}

impl WorkerState {
    fn backend(&mut self, tag: InstanceTag) -> &mut dyn LlamaBackend {
        let factory = self.factory.clone();
        self.backends
            .entry(tag)
            .or_insert_with(|| factory())
            .as_mut()
    }

    fn require_model(&self, id: &str) -> Result<ModelSettings, LlamaError> {
        self.settings
            .model(id)
            .cloned()
            .ok_or_else(|| LlamaError::UnknownModel(id.to_string()))
    }

    /// Load `model` into `tag` unless already loaded in the requested mode,
    /// following the offload plan.
    fn ensure_loaded(
        &mut self,
        model: &ModelSettings,
        tag: InstanceTag,
        embeddings_required: bool,
        cancelled: &dyn Fn() -> bool,
    ) -> Result<(), LlamaError> {
        if cancelled() {
            return Err(LlamaError::Cancelled);
        }
        let settings = self.settings.clone();
        let automatic = self.automatic_gpu_offload;
        let factory = self.factory.clone();
        let backend = self
            .backends
            .entry(tag)
            .or_insert_with(|| factory())
            .as_mut();
        // Embedding capability and embedding execution mode are distinct:
        // generation always needs a logits context.
        if backend.loaded_model_id().as_deref() == Some(model.id.as_str())
            && backend.loaded_embeddings_enabled() == embeddings_required
        {
            return Ok(());
        }
        let mut request = LoadRequest {
            model: model.clone(),
            backend: settings.backend.clone(),
            context: settings.context.clone(),
            embeddings: settings.embeddings.clone(),
        };
        if request.context.context_size.is_none() {
            request.context.context_size = model.context_window;
        }
        request.embeddings.enabled = Some(embeddings_required);

        let mut devices = if automatic {
            backend.gpu_memory()
        } else {
            Vec::new()
        };
        let split_none = settings
            .backend
            .split_mode
            .as_deref()
            .is_some_and(|m| m.trim().eq_ignore_ascii_case("none"));
        if split_none && !devices.is_empty() {
            let main = usize::try_from(settings.backend.main_gpu.unwrap_or(0)).unwrap_or(0);
            let device = devices
                .iter()
                .find(|d| d.index == main)
                .copied()
                .unwrap_or(devices[0]);
            devices = vec![device];
        }
        let model_bytes = std::fs::metadata(&model.path).map(|m| m.len()).unwrap_or(0);
        let plan = plan_offload(&OffloadPlanInput {
            configured_gpu_layers: if automatic {
                None
            } else {
                settings.backend.gpu_layers
            },
            block_count: model.block_count,
            model_size_bytes: model_bytes,
            devices,
        });
        let mut status = plan.status_json(model_bytes);

        let mut last_error = None;
        let mut load_started = false;
        for &layers in &plan.gpu_layer_attempts {
            request.backend.gpu_layers = Some(layers);
            if cancelled() {
                if load_started {
                    backend.unload();
                }
                status["loadCancelled"] = json!(true);
                self.offload_status.insert(tag, status);
                return Err(LlamaError::Cancelled);
            }
            if let Some(attempts) = status["attemptedGpuLayers"].as_array_mut() {
                attempts.push(json!(layers));
            }
            load_started = true;
            match backend.load(&request, cancelled) {
                Ok(()) if !cancelled() => {
                    status["selectedGpuLayers"] = json!(layers);
                    status["cpuFallback"] = json!(
                        layers == 0 && plan.gpu_layer_attempts.first().is_some_and(|f| *f > 0)
                    );
                    self.offload_status.insert(tag, status);
                    return Ok(());
                }
                Ok(()) | Err(LlamaError::Cancelled) => {
                    backend.unload();
                    status["loadCancelled"] = json!(true);
                    self.offload_status.insert(tag, status);
                    return Err(LlamaError::Cancelled);
                }
                Err(err @ LlamaError::Unavailable(_)) => {
                    status["loadFailed"] = json!(true);
                    self.offload_status.insert(tag, status);
                    return Err(err);
                }
                Err(err) => {
                    // A cancellation must never become another GPU/CPU retry.
                    if cancelled() {
                        backend.unload();
                        status["loadCancelled"] = json!(true);
                        self.offload_status.insert(tag, status);
                        return Err(LlamaError::Cancelled);
                    }
                    last_error = Some(err);
                }
            }
        }
        status["loadFailed"] = json!(true);
        self.offload_status.insert(tag, status);
        Err(last_error.unwrap_or_else(|| {
            LlamaError::backend(format!("failed to load llama model: {}", model.path))
        }))
    }

    /// Generate with the `chat` instance.
    pub(crate) fn generate(
        &mut self,
        tag: InstanceTag,
        request: &GenerateRequest,
        on_token: &mut dyn FnMut(&str),
        cancelled: &dyn Fn() -> bool,
    ) -> Result<GenerateResult, LlamaError> {
        let model = self.require_model(&request.model_id)?;
        self.ensure_loaded(&model, tag, false, &|| false)?;
        if cancelled() {
            return Ok(GenerateResult {
                stop: GenerateStop::Aborted,
                ..Default::default()
            });
        }
        self.backend(tag).generate(request, on_token, cancelled)
    }
}

type Job = Box<dyn FnOnce(&mut WorkerState) + Send>;

struct QueuedJob {
    id: u64,
    run: Job,
}

#[derive(Default)]
struct QueueState {
    jobs: VecDeque<QueuedJob>,
    stopping: bool,
}

struct Shared {
    queue: Mutex<QueueState>,
    ready: Condvar,
}

impl Shared {
    fn lock(&self) -> std::sync::MutexGuard<'_, QueueState> {
        self.queue.lock().unwrap_or_else(|e| e.into_inner())
    }
}

/// The llama runtime. Dropping it unloads all models and joins the worker.
pub struct LlamaRuntime {
    settings: Arc<LlamaSettings>,
    shared: Arc<Shared>,
    worker: Option<JoinHandle<()>>,
    worker_id: ThreadId,
    next_job: AtomicU64,
}

impl std::fmt::Debug for LlamaRuntime {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("LlamaRuntime")
            .field("models", &self.settings.models.len())
            .finish()
    }
}

impl LlamaRuntime {
    /// A runtime using llama.cpp (or an unavailable backend in builds
    /// without it).
    pub fn new(settings: LlamaSettings) -> Self {
        Self::with_backend_factory(settings, default_backend_factory())
    }

    /// A runtime with a custom backend factory (tests, alternative engines).
    pub fn with_backend_factory(settings: LlamaSettings, factory: BackendFactory) -> Self {
        let settings = Arc::new(settings);
        let shared = Arc::new(Shared {
            queue: Mutex::new(QueueState::default()),
            ready: Condvar::new(),
        });
        let worker_settings = settings.clone();
        let worker_shared = shared.clone();
        let worker = std::thread::Builder::new()
            .name("qt-llama".into())
            .spawn(move || {
                // Backends are created and used only on this thread.
                let mut state = WorkerState {
                    automatic_gpu_offload: worker_settings.backend.gpu_layers.is_none(),
                    settings: worker_settings,
                    factory,
                    backends: BTreeMap::new(),
                    offload_status: BTreeMap::new(),
                };
                worker_loop(&worker_shared, &mut state)
            })
            .expect("spawn llama worker thread");
        let worker_id = worker.thread().id();
        Self {
            settings,
            shared,
            worker: Some(worker),
            worker_id,
            next_job: AtomicU64::new(1),
        }
    }

    pub fn settings(&self) -> &LlamaSettings {
        &self.settings
    }

    /// Queue `f` on the worker; returns the job id and its result channel.
    pub(crate) fn enqueue<T, F>(&self, f: F) -> Result<(u64, mpsc::Receiver<T>), LlamaError>
    where
        T: Send + 'static,
        F: FnOnce(&mut WorkerState) -> T + Send + 'static,
    {
        if std::thread::current().id() == self.worker_id {
            return Err(LlamaError::backend(
                "llama runtime called from its own worker thread",
            ));
        }
        let (tx, rx) = mpsc::channel();
        let id = self.next_job.fetch_add(1, Ordering::Relaxed);
        {
            let mut queue = self.shared.lock();
            if queue.stopping {
                return Err(LlamaError::ShuttingDown);
            }
            queue.jobs.push_back(QueuedJob {
                id,
                run: Box::new(move |state| {
                    let _ = tx.send(f(state));
                }),
            });
        }
        self.shared.ready.notify_one();
        Ok((id, rx))
    }

    fn call<T, F>(&self, f: F) -> Result<T, LlamaError>
    where
        T: Send + 'static,
        F: FnOnce(&mut WorkerState) -> T + Send + 'static,
    {
        let (_, rx) = self.enqueue(f)?;
        rx.recv()
            .map_err(|_| LlamaError::backend("llama worker job failed"))
    }

    /// Remove a job that has not started yet; true when it was removed.
    fn cancel_pending(&self, id: u64) -> bool {
        let mut queue = self.shared.lock();
        match queue.jobs.iter().position(|job| job.id == id) {
            Some(index) => {
                queue.jobs.remove(index);
                true
            }
            None => false,
        }
    }

    fn is_pending(&self, id: u64) -> bool {
        self.shared.lock().jobs.iter().any(|job| job.id == id)
    }

    /// Load a configured model into an instance (`embed` loads in embedding
    /// mode).
    pub fn load_model(&self, model_id: &str, tag: InstanceTag) -> Result<(), LlamaError> {
        let id = model_id.to_string();
        self.call(move |state| {
            let model = state.require_model(&id)?;
            state.ensure_loaded(&model, tag, tag == InstanceTag::Embed, &|| false)
        })?
    }

    /// Generate without streaming on an instance (archived `run_tagged`).
    /// Build `request` with [`crate::provider::make_generate_request`].
    pub fn generate(
        &self,
        request: GenerateRequest,
        tag: InstanceTag,
    ) -> Result<GenerateResult, LlamaError> {
        self.call(move |state| state.generate(tag, &request, &mut |_| {}, &|| false))?
    }

    /// Unload every instance.
    pub fn unload(&self) -> Result<(), LlamaError> {
        self.call(|state| {
            for backend in state.backends.values_mut() {
                backend.unload();
            }
        })
    }

    pub fn loaded_model_id(&self, tag: InstanceTag) -> Result<Option<String>, LlamaError> {
        self.call(move |state| state.backends.get(&tag).and_then(|b| b.loaded_model_id()))
    }

    /// Status of an instance, with its last offload decision.
    pub fn runtime_status(&self, tag: InstanceTag) -> Result<RuntimeStatus, LlamaError> {
        self.call(move |state| {
            let mut status = state
                .backends
                .get(&tag)
                .map(|b| b.runtime_status())
                .unwrap_or_default();
            if !status.metadata.is_object() {
                status.metadata = json!({});
            }
            status.metadata["tag"] = json!(tag.as_str());
            if let Some(offload) = state.offload_status.get(&tag) {
                status.metadata["offload"] = offload.clone();
            }
            status
        })
    }

    /// Apply a KV cache operation to an instance.
    pub fn kv_cache_control(
        &self,
        operation: KvCacheOperation,
        tag: InstanceTag,
    ) -> Result<KvCacheState, LlamaError> {
        self.call(move |state| {
            let mut result = state.backend(tag).kv_cache_control(&operation);
            if !result.metadata.is_object() {
                result.metadata = json!({});
            }
            result.metadata["tag"] = json!(tag.as_str());
            result
        })
    }

    /// Embed text. Waits at most `queue_timeout` in the queue; cancellation
    /// is honoured while queued, during load and during decode. A queued
    /// call removed by cancellation or timeout never reaches the backend.
    pub fn embed(
        &self,
        request: EmbeddingRequest,
        tag: InstanceTag,
    ) -> Result<EmbeddingResult, LlamaError> {
        let latch = Arc::new(Latch::new(request.cancelled.clone()));
        if latch.poll() {
            return Err(LlamaError::Cancelled);
        }
        let queued_at = Instant::now();
        let timeout = request.queue_timeout;
        let worker_latch = latch.clone();
        let normalize_override = self.settings.embeddings.normalize;
        let (id, rx) = self.enqueue(move |state| -> Result<EmbeddingResult, LlamaError> {
            let cancelled = || worker_latch.poll();
            if cancelled() {
                return Err(LlamaError::Cancelled);
            }
            // The worker may dequeue between waiter polls; enforce the
            // deadline before loading or decoding.
            if queued_at.elapsed() >= timeout {
                return Err(LlamaError::QueueTimeout);
            }
            let model = state.require_model(&request.model_id)?;
            state.ensure_loaded(&model, tag, true, &cancelled)?;
            let mut resolved = request;
            resolved.normalize = normalize_override.unwrap_or(resolved.normalize);
            let mut result = state.backend(tag).embed(&resolved, &cancelled)?;
            if !result.metadata.is_object() {
                result.metadata = json!({});
            }
            result.metadata["tag"] = json!(tag.as_str());
            Ok(result)
        })?;
        let lost = || LlamaError::backend("llama worker job failed");
        loop {
            match rx.recv_timeout(Duration::from_millis(10)) {
                Ok(result) => return result,
                Err(mpsc::RecvTimeoutError::Disconnected) => return Err(lost()),
                Err(mpsc::RecvTimeoutError::Timeout) => {}
            }
            let cancelled = latch.poll();
            let timed_out = queued_at.elapsed() >= timeout;
            if cancelled || timed_out {
                if self.cancel_pending(id) {
                    return Err(if cancelled {
                        LlamaError::Cancelled
                    } else {
                        LlamaError::QueueTimeout
                    });
                }
                if cancelled {
                    // Active: wait so callbacks borrowing caller state finish.
                    let _ = rx.recv();
                    return Err(LlamaError::Cancelled);
                }
            }
            if !self.is_pending(id) {
                // Running: queue timeout no longer applies.
                return rx.recv().map_err(|_| lost())?;
            }
        }
    }

    fn stop_worker(&mut self) {
        {
            let mut queue = self.shared.lock();
            if queue.stopping {
                return;
            }
            queue.jobs.push_back(QueuedJob {
                id: 0,
                run: Box::new(|state: &mut WorkerState| {
                    for backend in state.backends.values_mut() {
                        backend.unload();
                    }
                }),
            });
            queue.stopping = true;
        }
        self.shared.ready.notify_one();
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

impl Drop for LlamaRuntime {
    fn drop(&mut self) {
        self.stop_worker();
    }
}

fn worker_loop(shared: &Shared, state: &mut WorkerState) {
    loop {
        let job = {
            let mut queue = shared.lock();
            loop {
                if let Some(job) = queue.jobs.pop_front() {
                    break job;
                }
                if queue.stopping {
                    return;
                }
                queue = shared.ready.wait(queue).unwrap_or_else(|e| e.into_inner());
            }
        };
        // A panicking job drops its result sender; the caller sees an error
        // and the worker keeps serving.
        let _ = std::panic::catch_unwind(AssertUnwindSafe(|| (job.run)(state)));
    }
}

/// Shared cancellation flag for generation jobs.
#[derive(Debug, Clone, Default)]
pub struct CancelFlag(Arc<AtomicBool>);

impl CancelFlag {
    pub fn cancel(&self) {
        self.0.store(true, Ordering::SeqCst);
    }
    pub fn is_cancelled(&self) -> bool {
        self.0.load(Ordering::SeqCst)
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use std::sync::atomic::AtomicUsize;

    /// Scripted backend recording calls.
    #[derive(Default)]
    pub struct MockLog {
        pub loads: Mutex<Vec<(String, i32, bool)>>,
        pub embeds: AtomicUsize,
        pub prompts: Mutex<Vec<String>>,
    }

    pub struct MockBackend {
        pub log: Arc<MockLog>,
        /// Loads fail while gpu_layers is above this value.
        pub max_gpu_layers: i32,
        pub gpus: Vec<GpuMemory>,
        pub reply: Vec<String>,
        pub embed_delay: Duration,
        loaded: Option<(String, bool)>,
    }

    impl MockBackend {
        pub fn new(log: Arc<MockLog>) -> Self {
            Self {
                log,
                max_gpu_layers: i32::MAX,
                gpus: Vec::new(),
                reply: vec!["Hello".into(), " world".into()],
                embed_delay: Duration::ZERO,
                loaded: None,
            }
        }
    }

    impl LlamaBackend for MockBackend {
        fn load(
            &mut self,
            request: &LoadRequest,
            cancelled: &dyn Fn() -> bool,
        ) -> Result<(), LlamaError> {
            let layers = request.backend.gpu_layers.unwrap_or(-1);
            let embeddings = request.embeddings.enabled.unwrap_or(false);
            self.log
                .loads
                .lock()
                .unwrap()
                .push((request.model.id.clone(), layers, embeddings));
            if cancelled() {
                return Err(LlamaError::Cancelled);
            }
            if layers > self.max_gpu_layers {
                return Err(LlamaError::backend(format!(
                    "failed to create llama context for model: {}",
                    request.model.id
                )));
            }
            self.loaded = Some((request.model.id.clone(), embeddings));
            Ok(())
        }
        fn unload(&mut self) {
            self.loaded = None;
        }
        fn loaded_model_id(&self) -> Option<String> {
            self.loaded.as_ref().map(|(id, _)| id.clone())
        }
        fn loaded_embeddings_enabled(&self) -> bool {
            self.loaded.as_ref().is_some_and(|(_, e)| *e)
        }
        fn gpu_memory(&self) -> Vec<GpuMemory> {
            self.gpus.clone()
        }
        fn runtime_status(&self) -> RuntimeStatus {
            RuntimeStatus {
                loaded_model_id: self.loaded_model_id(),
                model_loaded: self.loaded.is_some(),
                ..Default::default()
            }
        }
        fn generate(
            &mut self,
            request: &GenerateRequest,
            on_token: &mut dyn FnMut(&str),
            cancelled: &dyn Fn() -> bool,
        ) -> Result<GenerateResult, LlamaError> {
            self.log
                .prompts
                .lock()
                .unwrap()
                .push(request.prompt.clone());
            let mut result = GenerateResult {
                input_tokens: request.prompt.len() as u64,
                ..Default::default()
            };
            for piece in &self.reply {
                if cancelled() {
                    result.stop = GenerateStop::Aborted;
                    return Ok(result);
                }
                if result.output_tokens as i64 >= request.max_output_tokens {
                    result.stop = GenerateStop::MaxTokens;
                    return Ok(result);
                }
                on_token(piece);
                result.text.push_str(piece);
                result.output_tokens += 1;
            }
            result.metadata = json!({"model": request.model_id});
            Ok(result)
        }
        fn embed(
            &mut self,
            request: &EmbeddingRequest,
            cancelled: &dyn Fn() -> bool,
        ) -> Result<EmbeddingResult, LlamaError> {
            let start = Instant::now();
            while start.elapsed() < self.embed_delay {
                if cancelled() {
                    return Err(LlamaError::Cancelled);
                }
                std::thread::sleep(Duration::from_millis(2));
            }
            self.log.embeds.fetch_add(1, Ordering::SeqCst);
            let mut v = vec![3.0f32, 4.0];
            if request.normalize {
                v = vec![0.6, 0.8];
            }
            Ok(EmbeddingResult {
                embedding: v,
                input_tokens: request.text.split_whitespace().count() as u64,
                metadata: json!({"model": request.model_id}),
            })
        }
        fn kv_cache_control(&mut self, operation: &KvCacheOperation) -> KvCacheState {
            let mut state = KvCacheState::empty(operation.seq_id());
            state.loaded_model_id = self.loaded_model_id();
            state.mutated = !matches!(operation, KvCacheOperation::Inspect { .. });
            state
        }
    }

    pub fn model_file(dir: &std::path::Path, id: &str, bytes: usize) -> ModelSettings {
        let path = dir.join(format!("{id}.gguf"));
        std::fs::write(&path, vec![0u8; bytes]).unwrap();
        ModelSettings {
            id: id.into(),
            name: id.into(),
            path: path.to_string_lossy().into_owned(),
            block_count: Some(7),
            ..Default::default()
        }
    }

    pub fn runtime_with(
        settings: LlamaSettings,
        configure: impl Fn(&mut MockBackend) + Send + Sync + 'static,
    ) -> (LlamaRuntime, Arc<MockLog>) {
        let log = Arc::new(MockLog::default());
        let factory_log = log.clone();
        let runtime = LlamaRuntime::with_backend_factory(
            settings,
            Arc::new(move || {
                let mut backend = MockBackend::new(factory_log.clone());
                configure(&mut backend);
                Box::new(backend) as Box<dyn LlamaBackend>
            }),
        );
        (runtime, log)
    }

    #[test]
    fn tags_round_trip() {
        for tag in [
            InstanceTag::Embed,
            InstanceTag::Rank,
            InstanceTag::Instruct,
            InstanceTag::Chat,
        ] {
            assert_eq!(InstanceTag::parse(tag.as_str()), Some(tag));
        }
        assert_eq!(InstanceTag::parse("x"), None);
    }

    #[test]
    fn automatic_offload_retries_down_to_cpu() {
        let dir = tempfile::tempdir().unwrap();
        let settings = LlamaSettings {
            models: vec![model_file(dir.path(), "m", 4096)],
            ..Default::default()
        };
        let (runtime, log) = runtime_with(settings, |b| {
            b.max_gpu_layers = 0;
            b.gpus = vec![GpuMemory {
                index: 0,
                free_bytes: 2 * 1024 * 1024 * 1024,
                total_bytes: 4 * 1024 * 1024 * 1024,
            }];
        });
        runtime.load_model("m", InstanceTag::Chat).unwrap();
        let layers: Vec<i32> = log.loads.lock().unwrap().iter().map(|l| l.1).collect();
        // 7 blocks → 8 layers: 999, 6, 4, 2, 0.
        assert_eq!(layers, vec![999, 6, 4, 2, 0]);
        let status = runtime.runtime_status(InstanceTag::Chat).unwrap();
        assert_eq!(status.loaded_model_id.as_deref(), Some("m"));
        let offload = &status.metadata["offload"];
        assert_eq!(offload["policy"], "automatic");
        assert_eq!(offload["selectedGpuLayers"], 0);
        assert_eq!(offload["cpuFallback"], true);
        assert_eq!(offload["attemptedGpuLayers"], json!([999, 6, 4, 2, 0]));
        assert_eq!(status.metadata["tag"], "chat");

        // Already loaded in the same mode: no reload.
        runtime.load_model("m", InstanceTag::Chat).unwrap();
        assert_eq!(log.loads.lock().unwrap().len(), 5);
        assert_eq!(
            runtime.load_model("nope", InstanceTag::Chat).unwrap_err(),
            LlamaError::UnknownModel("nope".into())
        );
    }

    #[test]
    fn configured_layers_and_failure_status() {
        let dir = tempfile::tempdir().unwrap();
        let mut settings = LlamaSettings {
            models: vec![model_file(dir.path(), "m", 16)],
            ..Default::default()
        };
        settings.backend.gpu_layers = Some(12);
        let (runtime, log) = runtime_with(settings, |b| b.max_gpu_layers = -1);
        let err = runtime.load_model("m", InstanceTag::Chat).unwrap_err();
        assert_eq!(
            err.to_string(),
            "failed to create llama context for model: m"
        );
        let layers: Vec<i32> = log.loads.lock().unwrap().iter().map(|l| l.1).collect();
        assert_eq!(layers, vec![12, 0]);
        let status = runtime.runtime_status(InstanceTag::Chat).unwrap();
        assert_eq!(status.metadata["offload"]["policy"], "configured");
        assert_eq!(status.metadata["offload"]["loadFailed"], true);
        assert!(!status.model_loaded);
    }

    #[test]
    fn embeddings_reload_in_embedding_mode_and_report_tag() {
        let dir = tempfile::tempdir().unwrap();
        let mut settings = LlamaSettings {
            models: vec![model_file(dir.path(), "e", 16)],
            ..Default::default()
        };
        settings.embeddings.normalize = Some(false);
        let (runtime, log) = runtime_with(settings, |_| {});
        runtime.load_model("e", InstanceTag::Chat).unwrap();
        let result = runtime
            .embed(EmbeddingRequest::new("e", "two words"), InstanceTag::Embed)
            .unwrap();
        // settings.embeddings.normalize=false overrides the request default.
        assert_eq!(result.embedding, vec![3.0, 4.0]);
        assert_eq!(result.metadata["tag"], "embed");
        let loads = log.loads.lock().unwrap().clone();
        assert_eq!(loads.last().unwrap(), &("e".to_string(), 0, true));
        assert_eq!(
            runtime
                .loaded_model_id(InstanceTag::Embed)
                .unwrap()
                .as_deref(),
            Some("e")
        );
        assert_eq!(runtime.loaded_model_id(InstanceTag::Rank).unwrap(), None);
        runtime.unload().unwrap();
        assert_eq!(runtime.loaded_model_id(InstanceTag::Embed).unwrap(), None);
    }

    #[test]
    fn embedding_queue_timeout_and_cancellation() {
        let dir = tempfile::tempdir().unwrap();
        let settings = LlamaSettings {
            models: vec![model_file(dir.path(), "e", 16)],
            ..Default::default()
        };
        let (runtime, log) = runtime_with(settings, |b| b.embed_delay = Duration::from_millis(300));
        let runtime = Arc::new(runtime);

        // Occupy the worker with a slow embedding.
        let busy = {
            let runtime = runtime.clone();
            std::thread::spawn(move || {
                let mut request = EmbeddingRequest::new("e", "slow");
                request.queue_timeout = Duration::from_secs(5);
                runtime.embed(request, InstanceTag::Embed)
            })
        };
        std::thread::sleep(Duration::from_millis(50));

        let mut quick = EmbeddingRequest::new("e", "queued");
        quick.queue_timeout = Duration::from_millis(30);
        assert_eq!(
            runtime.embed(quick, InstanceTag::Embed).unwrap_err(),
            LlamaError::QueueTimeout
        );

        let flag = Arc::new(AtomicBool::new(false));
        let mut cancelled = EmbeddingRequest::new("e", "queued");
        cancelled.queue_timeout = Duration::from_secs(5);
        let f = flag.clone();
        cancelled.cancelled = Some(Arc::new(move || f.load(Ordering::SeqCst)));
        let setter = {
            let flag = flag.clone();
            std::thread::spawn(move || {
                std::thread::sleep(Duration::from_millis(30));
                flag.store(true, Ordering::SeqCst);
            })
        };
        assert_eq!(
            runtime.embed(cancelled, InstanceTag::Embed).unwrap_err(),
            LlamaError::Cancelled
        );
        setter.join().unwrap();
        assert!(busy.join().unwrap().is_ok());
        // Only the slow request reached the backend.
        assert_eq!(log.embeds.load(Ordering::SeqCst), 1);

        let mut zero = EmbeddingRequest::new("e", "x");
        zero.queue_timeout = Duration::ZERO;
        assert_eq!(
            runtime.embed(zero, InstanceTag::Embed).unwrap_err(),
            LlamaError::QueueTimeout
        );

        let mut pre = EmbeddingRequest::new("e", "x");
        pre.cancelled = Some(Arc::new(|| true));
        assert_eq!(
            runtime.embed(pre, InstanceTag::Embed).unwrap_err(),
            LlamaError::Cancelled
        );
    }

    #[test]
    fn active_embedding_cancellation_is_cooperative() {
        let dir = tempfile::tempdir().unwrap();
        let settings = LlamaSettings {
            models: vec![model_file(dir.path(), "e", 16)],
            ..Default::default()
        };
        let (runtime, log) = runtime_with(settings, |b| b.embed_delay = Duration::from_secs(10));
        let flag = Arc::new(AtomicBool::new(false));
        let f = flag.clone();
        let mut request = EmbeddingRequest::new("e", "x");
        request.cancelled = Some(Arc::new(move || f.load(Ordering::SeqCst)));
        let setter = std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(100));
            flag.store(true, Ordering::SeqCst);
        });
        let started = Instant::now();
        assert_eq!(
            runtime.embed(request, InstanceTag::Embed).unwrap_err(),
            LlamaError::Cancelled
        );
        assert!(started.elapsed() < Duration::from_secs(5));
        setter.join().unwrap();
        assert_eq!(log.embeds.load(Ordering::SeqCst), 0);
    }

    #[test]
    fn kv_operations_are_tagged() {
        let (runtime, _) = runtime_with(LlamaSettings::default(), |_| {});
        let state = runtime
            .kv_cache_control(KvCacheOperation::Inspect { seq_id: 3 }, InstanceTag::Chat)
            .unwrap();
        assert_eq!(state.seq_id, 3);
        assert!(!state.mutated);
        assert_eq!(state.metadata["tag"], "chat");
        let cleared = runtime
            .kv_cache_control(KvCacheOperation::Clear, InstanceTag::Instruct)
            .unwrap();
        assert!(cleared.mutated);
        assert_eq!(cleared.to_json()["metadata"]["tag"], "instruct");
    }

    #[test]
    fn unavailable_backend_reports_reason() {
        let mut backend = UnavailableBackend;
        let err = backend
            .load(&LoadRequest::default(), &|| false)
            .unwrap_err();
        assert!(matches!(err, LlamaError::Unavailable(_)));
        assert_eq!(backend.runtime_status().metadata["source"], "unavailable");
    }

    #[test]
    fn worker_survives_panicking_job() {
        let (runtime, _) = runtime_with(LlamaSettings::default(), |_| {});
        let err = runtime.call(|_| -> u8 { panic!("boom") }).unwrap_err();
        assert_eq!(err.to_string(), "llama worker job failed");
        assert_eq!(runtime.loaded_model_id(InstanceTag::Chat).unwrap(), None);
    }
}
