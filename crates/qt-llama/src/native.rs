//! llama.cpp backend over the `qt-llama-sys` bindings.
//!
//! Only the runtime worker thread touches these objects. All raw pointers are
//! owned here and released in [`NativeBackend::unload`] / `Drop`.

use std::ffi::{CStr, CString, c_char, c_void};
use std::panic::AssertUnwindSafe;
use std::ptr;
use std::sync::Mutex;

use qt_llama_sys as sys;
use serde_json::{Value, json};

use crate::LlamaError;
use crate::offload::GpuMemory;
use crate::runtime::{
    EmbeddingRequest, EmbeddingResult, GenerateRequest, GenerateResult, GenerateStop,
    KvCacheOperation, KvCacheState, LlamaBackend, LoadRequest, RuntimeStatus,
};
use crate::sampling::SamplerStep;

// ---------------------------------------------------------------------------
// Process-wide backend initialisation
// ---------------------------------------------------------------------------

static BACKEND_REFS: Mutex<usize> = Mutex::new(0);

unsafe extern "C" fn quiet_log(
    _level: sys::ggml_log_level,
    _text: *const c_char,
    _data: *mut c_void,
) {
}

/// Reference-counted `llama_backend_init` / `llama_backend_free`.
struct BackendGuard;

impl BackendGuard {
    fn acquire() -> Self {
        let mut refs = BACKEND_REFS.lock().unwrap_or_else(|e| e.into_inner());
        if *refs == 0 {
            // SAFETY: plain C initialisation calls, serialised by the mutex.
            unsafe {
                if std::env::var_os("QT_LLAMA_LOG").is_none() {
                    sys::llama_log_set(Some(quiet_log), ptr::null_mut());
                }
                sys::llama_backend_init();
            }
        }
        *refs += 1;
        BackendGuard
    }
}

impl Drop for BackendGuard {
    fn drop(&mut self) {
        let mut refs = BACKEND_REFS.lock().unwrap_or_else(|e| e.into_inner());
        if *refs == 0 {
            return;
        }
        *refs -= 1;
        if *refs == 0 {
            // SAFETY: no llama objects remain (every owner holds a guard).
            unsafe { sys::llama_backend_free() };
        }
    }
}

// ---------------------------------------------------------------------------
// Callback trampolines
// ---------------------------------------------------------------------------

type CancelRef<'a> = &'a dyn Fn() -> bool;

unsafe extern "C" fn abort_trampoline(data: *mut c_void) -> bool {
    if data.is_null() {
        return false;
    }
    // SAFETY: `data` points at a `CancelRef` that outlives the callback
    // registration (see `AbortGuard`).
    let callback = unsafe { &*(data as *const CancelRef<'_>) };
    std::panic::catch_unwind(AssertUnwindSafe(callback)).unwrap_or(true)
}

unsafe extern "C" fn progress_trampoline(_progress: f32, data: *mut c_void) -> bool {
    if data.is_null() {
        return true;
    }
    // SAFETY: as above; valid for the duration of the load call.
    let callback = unsafe { &*(data as *const CancelRef<'_>) };
    !std::panic::catch_unwind(AssertUnwindSafe(callback)).unwrap_or(true)
}

/// Clears the context abort callback when dropped.
struct AbortGuard {
    context: *mut sys::llama_context,
}

impl AbortGuard {
    /// # Safety
    /// `context` must be valid and `callback` must outlive the guard.
    unsafe fn install(context: *mut sys::llama_context, callback: &CancelRef<'_>) -> Self {
        unsafe {
            sys::llama_set_abort_callback(
                context,
                Some(abort_trampoline),
                callback as *const CancelRef<'_> as *mut c_void,
            );
        }
        AbortGuard { context }
    }
}

impl Drop for AbortGuard {
    fn drop(&mut self) {
        // SAFETY: the context is alive while the guard exists.
        unsafe { sys::llama_set_abort_callback(self.context, None, ptr::null_mut()) };
    }
}

struct Sampler(*mut sys::llama_sampler);

impl Drop for Sampler {
    fn drop(&mut self) {
        if !self.0.is_null() {
            // SAFETY: owned chain, freed once.
            unsafe { sys::llama_sampler_free(self.0) };
        }
    }
}

struct Batch(sys::llama_batch);

impl Drop for Batch {
    fn drop(&mut self) {
        // SAFETY: allocated by llama_batch_init.
        unsafe { sys::llama_batch_free(self.0) };
    }
}

/// Emits complete UTF-8 sequences from a byte stream of token pieces.
#[derive(Default)]
pub(crate) struct Utf8Stream {
    pending: Vec<u8>,
}

impl Utf8Stream {
    pub(crate) fn push(&mut self, bytes: &[u8]) -> String {
        self.pending.extend_from_slice(bytes);
        match std::str::from_utf8(&self.pending) {
            Ok(text) => {
                let text = text.to_string();
                self.pending.clear();
                text
            }
            Err(err) => {
                let valid = err.valid_up_to();
                if err.error_len().is_some() {
                    // Invalid bytes: emit them lossily rather than stalling.
                    let text = String::from_utf8_lossy(&self.pending).into_owned();
                    self.pending.clear();
                    return text;
                }
                let text = String::from_utf8_lossy(&self.pending[..valid]).into_owned();
                self.pending.drain(..valid);
                text
            }
        }
    }

    pub(crate) fn finish(&mut self) -> String {
        let text = String::from_utf8_lossy(&self.pending).into_owned();
        self.pending.clear();
        text
    }
}

fn checked_u32(value: i64, field: &str) -> Result<u32, LlamaError> {
    u32::try_from(value)
        .map_err(|_| LlamaError::backend(format!("llama {field} exceeds uint32 range")))
}

fn split_mode(value: Option<&str>) -> sys::llama_split_mode {
    match value.map(|v| v.trim().to_ascii_lowercase()).as_deref() {
        Some("none") => sys::LLAMA_SPLIT_MODE_NONE,
        Some("row") => sys::LLAMA_SPLIT_MODE_ROW,
        _ => sys::LLAMA_SPLIT_MODE_LAYER,
    }
}

fn pooling(value: Option<&str>) -> sys::llama_pooling_type {
    match value.map(|v| v.trim().to_ascii_lowercase()).as_deref() {
        Some("none") => sys::LLAMA_POOLING_TYPE_NONE,
        Some("mean") => sys::LLAMA_POOLING_TYPE_MEAN,
        Some("cls") => sys::LLAMA_POOLING_TYPE_CLS,
        Some("last") => sys::LLAMA_POOLING_TYPE_LAST,
        Some("rank") => sys::LLAMA_POOLING_TYPE_RANK,
        _ => sys::LLAMA_POOLING_TYPE_UNSPECIFIED,
    }
}

fn pooling_name(value: sys::llama_pooling_type) -> &'static str {
    match value {
        sys::LLAMA_POOLING_TYPE_NONE => "none",
        sys::LLAMA_POOLING_TYPE_MEAN => "mean",
        sys::LLAMA_POOLING_TYPE_CLS => "cls",
        sys::LLAMA_POOLING_TYPE_LAST => "last",
        sys::LLAMA_POOLING_TYPE_RANK => "rank",
        _ => "unknown",
    }
}

fn c_string(ptr: *const c_char) -> String {
    if ptr.is_null() {
        return String::new();
    }
    // SAFETY: llama/ggml return NUL-terminated static strings.
    unsafe { CStr::from_ptr(ptr) }
        .to_string_lossy()
        .into_owned()
}

fn device_type_name(kind: sys::ggml_backend_dev_type) -> &'static str {
    match kind {
        sys::GGML_BACKEND_DEVICE_TYPE_CPU => "cpu",
        sys::GGML_BACKEND_DEVICE_TYPE_GPU => "gpu",
        sys::GGML_BACKEND_DEVICE_TYPE_IGPU => "igpu",
        sys::GGML_BACKEND_DEVICE_TYPE_ACCEL => "accelerator",
        sys::GGML_BACKEND_DEVICE_TYPE_META => "meta",
        _ => "unknown",
    }
}

/// L2-normalise in place (no-op for a zero vector).
pub(crate) fn normalize_embedding(embedding: &mut [f32]) {
    let norm_squared: f64 = embedding
        .iter()
        .map(|v| f64::from(*v) * f64::from(*v))
        .sum();
    if norm_squared <= 0.0 {
        return;
    }
    let scale = 1.0 / norm_squared.sqrt();
    for value in embedding {
        *value = (f64::from(*value) * scale) as f32;
    }
}

#[derive(Default, Clone, Copy)]
struct Breakdown {
    model: u64,
    context: u64,
    compute: u64,
}

impl Breakdown {
    fn add(&mut self, entry: &sys::qt_llama_memory_entry) {
        self.model += entry.model as u64;
        self.context += entry.context as u64;
        self.compute += entry.compute as u64;
    }

    fn to_json(self) -> Value {
        json!({
            "modelBytes": self.model,
            "contextBytes": self.context,
            "computeBytes": self.compute,
            "allocatedBytes": self.model + self.context + self.compute,
        })
    }
}

/// The llama.cpp model + context for one runtime instance.
pub struct NativeBackend {
    guard: Option<BackendGuard>,
    model: *mut sys::llama_model,
    context: *mut sys::llama_context,
    model_id: Option<String>,
    load_request: LoadRequest,
    tensor_split: Vec<f32>,
    /// Tokens currently held in the KV cache for sequence 0.
    cached_tokens: Vec<sys::llama_token>,
}

impl NativeBackend {
    pub fn new() -> Self {
        Self {
            guard: None,
            model: ptr::null_mut(),
            context: ptr::null_mut(),
            model_id: None,
            load_request: LoadRequest::default(),
            tensor_split: Vec::new(),
            cached_tokens: Vec::new(),
        }
    }

    fn vocab(&self) -> *const sys::llama_vocab {
        // SAFETY: called only with a loaded model.
        unsafe { sys::llama_model_get_vocab(self.model) }
    }

    fn tokenize(&self, text: &str) -> Result<Vec<sys::llama_token>, LlamaError> {
        let vocab = self.vocab();
        let len = i32::try_from(text.len())
            .map_err(|_| LlamaError::backend("llama tokenization overflow"))?;
        // SAFETY: valid vocab and buffer sizes.
        let count = unsafe {
            sys::llama_tokenize(
                vocab,
                text.as_ptr().cast(),
                len,
                ptr::null_mut(),
                0,
                true,
                true,
            )
        };
        if count == i32::MIN {
            return Err(LlamaError::backend("llama tokenization overflow"));
        }
        let count = count.unsigned_abs() as usize;
        let mut tokens = vec![0; count];
        if count == 0 {
            return Ok(tokens);
        }
        // SAFETY: `tokens` has room for `count` tokens.
        let actual = unsafe {
            sys::llama_tokenize(
                vocab,
                text.as_ptr().cast(),
                len,
                tokens.as_mut_ptr(),
                count as i32,
                true,
                true,
            )
        };
        if actual < 0 {
            return Err(LlamaError::backend("llama tokenization failed"));
        }
        tokens.truncate(actual as usize);
        Ok(tokens)
    }

    fn token_bytes(&self, token: sys::llama_token) -> Result<Vec<u8>, LlamaError> {
        let vocab = self.vocab();
        let mut buffer = vec![0u8; 128];
        // SAFETY: buffer length passed explicitly.
        let mut written = unsafe {
            sys::llama_token_to_piece(
                vocab,
                token,
                buffer.as_mut_ptr().cast(),
                buffer.len() as i32,
                0,
                false,
            )
        };
        if written < 0 {
            buffer = vec![0u8; written.unsigned_abs() as usize];
            // SAFETY: as above with the requested size.
            written = unsafe {
                sys::llama_token_to_piece(
                    vocab,
                    token,
                    buffer.as_mut_ptr().cast(),
                    buffer.len() as i32,
                    0,
                    false,
                )
            };
            if written < 0 {
                return Err(LlamaError::backend("llama token detokenization failed"));
            }
        }
        buffer.truncate(written as usize);
        Ok(buffer)
    }

    fn make_sampler(&self, plan: &[SamplerStep]) -> Result<Sampler, LlamaError> {
        // SAFETY: chain creation/addition with valid pointers; ownership of
        // each sampler moves into the chain.
        unsafe {
            let chain = Sampler(sys::llama_sampler_chain_init(
                sys::llama_sampler_chain_default_params(),
            ));
            if chain.0.is_null() {
                return Err(LlamaError::backend("failed to create llama sampler chain"));
            }
            for step in plan {
                let sampler = match step {
                    SamplerStep::Penalties {
                        last_n,
                        repeat_penalty,
                        frequency_penalty,
                        presence_penalty,
                    } => sys::llama_sampler_init_penalties(
                        *last_n,
                        *repeat_penalty as f32,
                        *frequency_penalty as f32,
                        *presence_penalty as f32,
                    ),
                    SamplerStep::TopK { k } => sys::llama_sampler_init_top_k(*k),
                    SamplerStep::TopP { p, min_keep } => {
                        sys::llama_sampler_init_top_p(*p as f32, (*min_keep).max(1) as usize)
                    }
                    SamplerStep::MinP { p, min_keep } => {
                        sys::llama_sampler_init_min_p(*p as f32, (*min_keep).max(1) as usize)
                    }
                    SamplerStep::Temperature { temperature } => {
                        sys::llama_sampler_init_temp(*temperature as f32)
                    }
                    SamplerStep::Greedy { .. } => sys::llama_sampler_init_greedy(),
                    SamplerStep::Dist { seed } => sys::llama_sampler_init_dist(*seed),
                    SamplerStep::Grammar { grammar, root, .. } => {
                        let grammar = CString::new(grammar.as_str()).map_err(|_| {
                            LlamaError::backend("failed to create llama sampler: grammar")
                        })?;
                        let root = CString::new(root.as_str()).map_err(|_| {
                            LlamaError::backend("failed to create llama sampler: grammar")
                        })?;
                        sys::llama_sampler_init_grammar(
                            self.vocab(),
                            grammar.as_ptr(),
                            root.as_ptr(),
                        )
                    }
                };
                if sampler.is_null() {
                    return Err(LlamaError::backend(format!(
                        "failed to create llama sampler: {}",
                        step.name()
                    )));
                }
                sys::llama_sampler_chain_add(chain.0, sampler);
            }
            Ok(chain)
        }
    }

    fn memory(&self) -> sys::llama_memory_t {
        // SAFETY: valid context.
        unsafe { sys::llama_get_memory(self.context) }
    }

    fn reset_cache(&mut self) {
        // SAFETY: valid context.
        unsafe { sys::llama_memory_clear(self.memory(), true) };
        self.cached_tokens.clear();
    }

    fn memory_status(&self) -> Value {
        let mut memory =
            json!({"devices": [], "host": Breakdown::default().to_json(), "buffers": []});
        if self.model.is_null() || self.context.is_null() {
            return memory;
        }
        // SAFETY: valid model/context; shim functions are bounds-checked.
        unsafe {
            let count = sys::qt_llama_model_n_devices(self.model).max(0);
            let devices: Vec<sys::ggml_backend_dev_t> = (0..count)
                .map(|i| sys::qt_llama_model_get_device(self.model, i))
                .filter(|d| !d.is_null())
                .collect();
            let total = sys::qt_llama_memory_breakdown(self.context, ptr::null_mut(), 0);
            let mut entries = vec![sys::qt_llama_memory_entry::default(); total];
            let filled =
                sys::qt_llama_memory_breakdown(self.context, entries.as_mut_ptr(), entries.len());
            entries.truncate(filled.min(total));

            let mut per_device = vec![Breakdown::default(); devices.len()];
            let mut host = Breakdown::default();
            let mut buffers = Vec::new();
            for entry in &entries {
                if sys::ggml_backend_buft_is_host(entry.buffer_type) {
                    host.add(entry);
                    continue;
                }
                let device = sys::ggml_backend_buft_get_device(entry.buffer_type);
                match devices.iter().position(|d| *d == device) {
                    Some(index) if !device.is_null() => per_device[index].add(entry),
                    _ => {
                        let mut b = Breakdown::default();
                        b.add(entry);
                        let mut json = b.to_json();
                        json["name"] =
                            json!(c_string(sys::ggml_backend_buft_name(entry.buffer_type)));
                        buffers.push(json);
                    }
                }
            }
            let mut device_json = Vec::new();
            for (index, device) in devices.iter().enumerate() {
                let mut free = 0usize;
                let mut total = 0usize;
                sys::ggml_backend_dev_memory(*device, &mut free, &mut total);
                let kind = sys::ggml_backend_dev_type(*device);
                let breakdown = per_device[index];
                let mut json = breakdown.to_json();
                let allocated = breakdown.model + breakdown.context + breakdown.compute;
                let used = (total as u64).saturating_sub(free as u64);
                json["index"] = json!(index);
                json["name"] = json!(c_string(sys::ggml_backend_dev_name(*device)));
                json["description"] = json!(c_string(sys::ggml_backend_dev_description(*device)));
                json["type"] = json!(device_type_name(kind));
                json["gpuMemory"] = json!(
                    kind == sys::GGML_BACKEND_DEVICE_TYPE_GPU
                        || kind == sys::GGML_BACKEND_DEVICE_TYPE_IGPU
                );
                json["freeBytes"] = json!(free as u64);
                json["totalBytes"] = json!(total as u64);
                json["usedBytes"] = json!(used);
                json["unaccountedBytes"] = json!(used.saturating_sub(allocated));
                device_json.push(json);
            }
            memory["devices"] = Value::Array(device_json);
            memory["host"] = host.to_json();
            memory["buffers"] = Value::Array(buffers);
        }
        memory
    }

    /// Decode `tokens` in `n_batch` chunks. Returns the llama status of the
    /// first failing chunk (0 on success).
    fn decode_tokens(&mut self, tokens: &mut [sys::llama_token]) -> i32 {
        // SAFETY: valid context; batch views into `tokens`.
        let batch_size = unsafe { sys::llama_n_batch(self.context) }.max(1) as usize;
        for chunk in tokens.chunks_mut(batch_size) {
            let status = unsafe {
                sys::llama_decode(
                    self.context,
                    sys::llama_batch_get_one(chunk.as_mut_ptr(), chunk.len() as i32),
                )
            };
            if status != 0 {
                return status;
            }
            self.cached_tokens.extend_from_slice(chunk);
        }
        0
    }
}

impl Default for NativeBackend {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for NativeBackend {
    fn drop(&mut self) {
        self.unload();
    }
}

impl LlamaBackend for NativeBackend {
    fn load(
        &mut self,
        request: &LoadRequest,
        cancelled: &dyn Fn() -> bool,
    ) -> Result<(), LlamaError> {
        if cancelled() {
            return Err(LlamaError::Cancelled);
        }
        self.unload();
        let guard = BackendGuard::acquire();
        let path = CString::new(request.model.path.as_str()).map_err(|_| {
            LlamaError::backend(format!(
                "failed to load llama model: {}",
                request.model.path
            ))
        })?;
        let callback: CancelRef<'_> = cancelled;

        // SAFETY: parameter structs come from llama defaults; pointers
        // (`tensor_split`, callback data, path) outlive the load calls.
        unsafe {
            let mut params = sys::llama_model_default_params();
            params.progress_callback = Some(progress_trampoline);
            params.progress_callback_user_data = &callback as *const CancelRef<'_> as *mut c_void;
            if let Some(layers) = request.backend.gpu_layers {
                params.n_gpu_layers = layers;
            }
            if let Some(main) = request.backend.main_gpu {
                params.main_gpu = main;
            }
            params.split_mode = split_mode(request.backend.split_mode.as_deref());
            if let Some(split) = &request.backend.tensor_split {
                // llama.cpp reads llama_max_devices() entries.
                let mut values: Vec<f32> = split.iter().map(|v| *v as f32).collect();
                values.resize(sys::llama_max_devices().max(values.len()), 0.0);
                self.tensor_split = values;
                params.tensor_split = self.tensor_split.as_ptr();
            }
            if let Some(mmap) = request.backend.use_mmap {
                params.use_mmap = mmap;
            }
            if let Some(mlock) = request.backend.use_mlock {
                params.use_mlock = mlock;
            }

            let model = sys::llama_model_load_from_file(path.as_ptr(), params);
            if cancelled() {
                if !model.is_null() {
                    sys::llama_model_free(model);
                }
                self.tensor_split.clear();
                return Err(LlamaError::Cancelled);
            }
            if model.is_null() {
                self.tensor_split.clear();
                return Err(LlamaError::backend(format!(
                    "failed to load llama model: {}",
                    request.model.path
                )));
            }

            let mut ctx_params = sys::llama_context_default_params();
            let build = || -> Result<sys::llama_context_params, LlamaError> {
                let mut p = ctx_params;
                if let Some(size) = request.context.context_size {
                    p.n_ctx = checked_u32(size, "contextSize")?;
                }
                if let Some(size) = request.context.batch_size {
                    p.n_batch = checked_u32(size, "batchSize")?;
                }
                if let Some(size) = request.context.ubatch_size {
                    p.n_ubatch = checked_u32(size, "ubatchSize")?;
                }
                Ok(p)
            };
            ctx_params = match build() {
                Ok(p) => p,
                Err(err) => {
                    sys::llama_model_free(model);
                    self.tensor_split.clear();
                    return Err(err);
                }
            };
            if let Some(threads) = request.context.threads {
                ctx_params.n_threads = threads;
            }
            if let Some(threads) = request.context.batch_threads {
                ctx_params.n_threads_batch = threads;
            }
            let embeddings = request.embeddings.enabled.unwrap_or(false);
            ctx_params.embeddings = embeddings;
            ctx_params.pooling_type = pooling(request.embeddings.pooling.as_deref());
            if embeddings
                && request.context.batch_size.is_none()
                && request.context.ubatch_size.is_none()
            {
                // Non-causal encoders need the whole input in one ubatch.
                let n = if ctx_params.n_ctx > 0 {
                    ctx_params.n_ctx
                } else {
                    sys::llama_model_n_ctx_train(model).max(0) as u32
                };
                if n > 0 {
                    ctx_params.n_batch = ctx_params.n_batch.max(n);
                    ctx_params.n_ubatch = ctx_params.n_ubatch.max(n);
                }
            }
            if let Some(offload) = request.backend.offload_kqv {
                ctx_params.offload_kqv = offload;
            }
            let context = sys::llama_init_from_model(model, ctx_params);
            if cancelled() {
                if !context.is_null() {
                    sys::llama_free(context);
                }
                sys::llama_model_free(model);
                self.tensor_split.clear();
                return Err(LlamaError::Cancelled);
            }
            if context.is_null() {
                sys::llama_model_free(model);
                self.tensor_split.clear();
                return Err(LlamaError::backend(format!(
                    "failed to create llama context for model: {}",
                    request.model.id
                )));
            }
            self.model = model;
            self.context = context;
        }
        self.guard = Some(guard);
        self.model_id = Some(request.model.id.clone());
        self.load_request = request.clone();
        self.cached_tokens.clear();
        Ok(())
    }

    fn unload(&mut self) {
        // SAFETY: pointers are owned and freed once (context before model).
        unsafe {
            if !self.context.is_null() {
                sys::llama_free(self.context);
            }
            if !self.model.is_null() {
                sys::llama_model_free(self.model);
            }
        }
        self.context = ptr::null_mut();
        self.model = ptr::null_mut();
        self.model_id = None;
        self.tensor_split.clear();
        self.cached_tokens.clear();
        self.load_request = LoadRequest::default();
        self.guard = None;
    }

    fn loaded_model_id(&self) -> Option<String> {
        self.model_id.clone()
    }

    fn loaded_embeddings_enabled(&self) -> bool {
        self.load_request.embeddings.enabled.unwrap_or(false)
    }

    fn gpu_memory(&self) -> Vec<GpuMemory> {
        let _guard = BackendGuard::acquire();
        let mut memory = Vec::new();
        // SAFETY: device registry enumeration after backend init.
        unsafe {
            let count = sys::ggml_backend_dev_count();
            for index in 0..count {
                let device = sys::ggml_backend_dev_get(index);
                if device.is_null() {
                    continue;
                }
                let kind = sys::ggml_backend_dev_type(device);
                if kind != sys::GGML_BACKEND_DEVICE_TYPE_GPU
                    && kind != sys::GGML_BACKEND_DEVICE_TYPE_IGPU
                {
                    continue;
                }
                let mut free = 0usize;
                let mut total = 0usize;
                sys::ggml_backend_dev_memory(device, &mut free, &mut total);
                memory.push(GpuMemory {
                    index: memory.len(),
                    free_bytes: free as u64,
                    total_bytes: total as u64,
                });
            }
        }
        memory
    }

    fn runtime_status(&self) -> RuntimeStatus {
        let _guard = BackendGuard::acquire();
        let mut status = RuntimeStatus {
            loaded_model_id: self.model_id.clone(),
            model_loaded: !self.model.is_null(),
            context_loaded: !self.context.is_null(),
            embeddings_enabled: self.loaded_embeddings_enabled(),
            sequence_position: self.cached_tokens.len() as i64,
            backend: self.load_request.backend.status_json(),
            memory: self.memory_status(),
            metadata: json!({
                "source": "llama-ext",
                "compiledBackends": crate::compiled_backends(),
            }),
            ..Default::default()
        };
        // SAFETY: simple queries on valid (or absent) objects.
        unsafe {
            status.gpu_offload_supported = sys::llama_supports_gpu_offload();
            status.max_devices = sys::llama_max_devices();
            if !self.model.is_null() {
                status.model_size_bytes = sys::llama_model_size(self.model);
            }
            if !self.context.is_null() {
                status.state_size_bytes = sys::llama_state_get_size(self.context) as u64;
                status.context_size = sys::llama_n_ctx(self.context);
            }
        }
        status
    }

    fn generate(
        &mut self,
        request: &GenerateRequest,
        on_token: &mut dyn FnMut(&str),
        cancelled: &dyn Fn() -> bool,
    ) -> Result<GenerateResult, LlamaError> {
        if self.model.is_null() || self.context.is_null() {
            return Err(LlamaError::backend("llama model is not loaded"));
        }
        let mut result = GenerateResult::default();
        if cancelled() {
            result.stop = GenerateStop::Aborted;
            return Ok(result);
        }
        let mut tokens = self.tokenize(&request.prompt)?;
        // SAFETY: valid context.
        let context_size = unsafe { sys::llama_n_ctx(self.context) };
        let max_output = request.max_output_tokens.max(0);
        if context_size > 0 && tokens.len() as i64 + max_output > i64::from(context_size) {
            return Err(LlamaError::backend(format!(
                "llama context overflow: prompt tokens {} plus max output {} exceed context {}",
                tokens.len(),
                max_output,
                context_size
            )));
        }

        let retain = request.kv_cache.retains();
        let mut reused = 0usize;
        if retain {
            let mut common = self
                .cached_tokens
                .iter()
                .zip(&tokens)
                .take_while(|(a, b)| a == b)
                .count();
            // At least one token must be decoded to produce logits.
            if common == tokens.len() && common > 0 {
                common -= 1;
            }
            if common < self.cached_tokens.len() {
                // SAFETY: valid memory handle.
                let removed =
                    unsafe { sys::llama_memory_seq_rm(self.memory(), 0, common as i32, -1) };
                if removed {
                    self.cached_tokens.truncate(common);
                } else {
                    self.reset_cache();
                    common = 0;
                }
            }
            reused = common;
        } else {
            self.reset_cache();
        }

        let callback: CancelRef<'_> = cancelled;
        // SAFETY: `callback` outlives the guard.
        let _abort = unsafe { AbortGuard::install(self.context, &callback) };

        let status = self.decode_tokens(&mut tokens[reused..]);
        if status == 2 {
            self.reset_cache();
            result.stop = GenerateStop::Aborted;
            return Ok(result);
        }
        if status != 0 {
            self.reset_cache();
            return Err(LlamaError::backend(format!(
                "llama prompt decode failed with status {status}"
            )));
        }

        let sampler = self.make_sampler(&request.sampler_plan)?;
        let vocab = self.vocab();
        let mut utf8 = Utf8Stream::default();
        let mut generated: i64 = 0;
        result.stop = GenerateStop::MaxTokens;
        while generated < max_output {
            if cancelled() {
                result.stop = GenerateStop::Aborted;
                break;
            }
            // SAFETY: valid sampler/context; logits exist for the last token.
            let mut token = unsafe { sys::llama_sampler_sample(sampler.0, self.context, -1) };
            // SAFETY: valid vocab.
            if unsafe { sys::llama_vocab_is_eog(vocab, token) } {
                result.stop = GenerateStop::EndTurn;
                break;
            }
            let piece = utf8.push(&self.token_bytes(token)?);
            if !piece.is_empty() {
                result.text.push_str(&piece);
                on_token(&piece);
            }
            // SAFETY: single-token batch over a local.
            let status =
                unsafe { sys::llama_decode(self.context, sys::llama_batch_get_one(&mut token, 1)) };
            generated += 1;
            if status == 2 {
                self.reset_cache();
                result.stop = GenerateStop::Aborted;
                break;
            }
            if status != 0 {
                self.reset_cache();
                return Err(LlamaError::backend(format!(
                    "llama token decode failed with status {status}"
                )));
            }
            self.cached_tokens.push(token);
        }
        let tail = utf8.finish();
        if !tail.is_empty() {
            result.text.push_str(&tail);
            on_token(&tail);
        }
        if !retain {
            self.reset_cache();
        }

        result.input_tokens = tokens.len() as u64;
        result.output_tokens = generated as u64;
        // SAFETY: static string.
        let system_info = c_string(unsafe { sys::llama_print_system_info() });
        result.metadata = json!({
            "contextSize": context_size,
            "model": request.model_id,
            "kvCache": {
                "policy": request.kv_cache.effective_policy(),
                "sequencePosition": self.cached_tokens.len(),
                "retained": retain,
                "reusedTokens": reused,
            },
            "systemInfo": system_info,
        });
        Ok(result)
    }

    fn embed(
        &mut self,
        request: &EmbeddingRequest,
        cancelled: &dyn Fn() -> bool,
    ) -> Result<EmbeddingResult, LlamaError> {
        if cancelled() {
            return Err(LlamaError::Cancelled);
        }
        if self.model.is_null() || self.context.is_null() {
            return Err(LlamaError::backend("llama model is not loaded"));
        }
        if !self.loaded_embeddings_enabled() {
            return Err(LlamaError::backend("llama embedding mode is not enabled"));
        }
        let tokens = self.tokenize(&request.text)?;
        if tokens.is_empty() {
            return Err(LlamaError::backend(
                "llama embedding text produced no tokens",
            ));
        }
        // SAFETY: valid context.
        let context_size = unsafe { sys::llama_n_ctx(self.context) };
        if context_size > 0 && tokens.len() > context_size as usize {
            return Err(LlamaError::backend(format!(
                "llama embedding input exceeds context {context_size}"
            )));
        }
        self.reset_cache();
        let callback: CancelRef<'_> = cancelled;
        // SAFETY: batch sized for `tokens`; callback outlives the guard;
        // returned embedding pointers are read before the next decode.
        unsafe {
            sys::llama_set_embeddings(self.context, true);
            let _abort = AbortGuard::install(self.context, &callback);
            let batch = Batch(sys::llama_batch_init(tokens.len() as i32, 0, 1));
            for (index, token) in tokens.iter().enumerate() {
                *batch.0.token.add(index) = *token;
                *batch.0.pos.add(index) = index as sys::llama_pos;
                *batch.0.n_seq_id.add(index) = 1;
                *(*batch.0.seq_id.add(index)) = 0;
                *batch.0.logits.add(index) = 1;
            }
            let mut b = batch.0;
            b.n_tokens = tokens.len() as i32;
            let encoder_only = sys::llama_model_has_encoder(self.model)
                && !sys::llama_model_has_decoder(self.model);
            let status = if encoder_only {
                sys::llama_encode(self.context, b)
            } else {
                sys::llama_decode(self.context, b)
            };
            if cancelled() {
                self.reset_cache();
                return Err(LlamaError::Cancelled);
            }
            if status != 0 {
                self.reset_cache();
                return Err(LlamaError::backend(format!(
                    "llama embedding decode failed with status {status}"
                )));
            }
            let pooling_type = sys::llama_pooling_type(self.context);
            let dimensions = sys::llama_model_n_embd_out(self.model).max(0) as usize;
            let raw = if pooling_type == sys::LLAMA_POOLING_TYPE_NONE {
                sys::llama_get_embeddings_ith(self.context, tokens.len() as i32 - 1)
            } else {
                sys::llama_get_embeddings_seq(self.context, 0)
            };
            if raw.is_null() {
                self.reset_cache();
                return Err(LlamaError::backend("failed to read llama embeddings"));
            }
            let mut embedding = std::slice::from_raw_parts(raw, dimensions).to_vec();
            self.reset_cache();
            if request.normalize {
                normalize_embedding(&mut embedding);
            }
            Ok(EmbeddingResult {
                embedding,
                input_tokens: tokens.len() as u64,
                metadata: json!({
                    "model": request.model_id,
                    "dimensions": dimensions,
                    "normalized": request.normalize,
                    "pooling": pooling_name(pooling_type),
                }),
            })
        }
    }

    fn kv_cache_control(&mut self, operation: &KvCacheOperation) -> KvCacheState {
        let mut state = KvCacheState::empty(operation.seq_id());
        state.loaded_model_id = self.model_id.clone();
        if self.context.is_null() {
            return state;
        }
        let memory = self.memory();
        // SAFETY: valid memory handle.
        unsafe {
            match *operation {
                KvCacheOperation::Inspect { .. } => {}
                KvCacheOperation::Clear => {
                    self.reset_cache();
                    state.mutated = true;
                }
                KvCacheOperation::Keep { seq_id } => {
                    sys::llama_memory_seq_keep(memory, seq_id);
                    if seq_id != 0 {
                        self.cached_tokens.clear();
                    }
                    state.mutated = true;
                }
                KvCacheOperation::Drop { seq_id, from, to } => {
                    state.mutated = sys::llama_memory_seq_rm(memory, seq_id, from, to);
                    if state.mutated && seq_id == 0 {
                        if from <= 0 && to < 0 {
                            self.cached_tokens.clear();
                        } else {
                            // A hole in the cached prefix: keep only what precedes it.
                            self.cached_tokens.truncate(from.max(0) as usize);
                            if to >= 0 {
                                sys::llama_memory_seq_rm(memory, 0, from.max(0), -1);
                            }
                        }
                    }
                }
                KvCacheOperation::Shift {
                    seq_id,
                    from,
                    to,
                    delta,
                } => {
                    if !sys::llama_memory_can_shift(memory) {
                        state.metadata["error"] = json!("llama memory does not support shifting");
                    } else {
                        sys::llama_memory_seq_add(memory, seq_id, from, to, delta);
                        if seq_id == 0 {
                            // Positions no longer match the cached prompt.
                            sys::llama_memory_clear(memory, true);
                            self.cached_tokens.clear();
                        }
                        state.mutated = true;
                    }
                }
            }
            state.position_min = sys::llama_memory_seq_pos_min(memory, operation.seq_id());
            state.position_max = sys::llama_memory_seq_pos_max(memory, operation.seq_id());
            state.can_shift = sys::llama_memory_can_shift(memory);
        }
        state.sequence_position = self.cached_tokens.len() as i64;
        state.metadata["loaded"] = json!(self.model_id.is_some());
        state
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn utf8_stream_joins_split_sequences() {
        let mut s = Utf8Stream::default();
        let bytes = "héllo €".as_bytes();
        let mut out = String::new();
        for b in bytes {
            out.push_str(&s.push(&[*b]));
        }
        out.push_str(&s.finish());
        assert_eq!(out, "héllo €");
        let mut bad = Utf8Stream::default();
        assert_eq!(bad.push(&[0xff, b'a']), "\u{fffd}a");
        assert_eq!(bad.push(&[0xe2]), "");
        assert_eq!(bad.finish(), "\u{fffd}");
    }

    #[test]
    fn normalizes_embeddings() {
        let mut v = vec![3.0, 4.0];
        normalize_embedding(&mut v);
        assert!((v[0] - 0.6).abs() < 1e-6 && (v[1] - 0.8).abs() < 1e-6);
        let mut zero = vec![0.0, 0.0];
        normalize_embedding(&mut zero);
        assert_eq!(zero, vec![0.0, 0.0]);
    }

    #[test]
    fn maps_settings_to_llama_enums() {
        assert_eq!(split_mode(Some(" Row ")), sys::LLAMA_SPLIT_MODE_ROW);
        assert_eq!(split_mode(None), sys::LLAMA_SPLIT_MODE_LAYER);
        assert_eq!(pooling(Some("cls")), sys::LLAMA_POOLING_TYPE_CLS);
        assert_eq!(pooling(None), sys::LLAMA_POOLING_TYPE_UNSPECIFIED);
        assert_eq!(pooling_name(sys::LLAMA_POOLING_TYPE_MEAN), "mean");
        assert!(checked_u32(-1, "contextSize").is_err());
    }
}
