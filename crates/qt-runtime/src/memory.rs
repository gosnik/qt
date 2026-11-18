//! Governed-memory wiring (archived `runtime_services.cpp` memory block):
//! the memory-task model plan, [`MemoryRuntimeOptions`] from `memory.*`
//! settings, the llama.cpp dense embedder, Dreaming with post-processing
//! through [`CaptureHooks`] / [`DreamingControl`], the `/context memory`
//! lines and the llama overrides applied inside memory child processes.

use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, OnceLock};

use qt_core::agent::AgentEvent;
use qt_core::harness::{HookId, Hooks};
use qt_core::message::{Message, convert_to_llm};
use qt_llama::LlamaProviderBackend;
use qt_llama::discovery::{
    DiscoveryOptions, MemoryTask, discover_models, select_model_for_memory_task,
};
use qt_llama::runtime::EmbeddingRequest;
use qt_llama::settings::LlamaSettings;
use qt_memory::code::CodeSnapshotProvider;
use qt_memory::dreaming::{
    ChildTask, DreamTrigger, DreamingConfig, DreamingService, DreamingWorker, IsolatedChild,
    ProcessChild,
};
use qt_memory::error::MemoryError;
use qt_memory::integration::{
    EpisodeBridge, MemoryHookIds, attach_memory, detach_memory, logical_input,
};
use qt_memory::isolated::ProcessAgentRunner;
use qt_memory::post_processing::PostProcessingService;
use qt_memory::research::{ManualEvidenceOrigin, parse_manual_research};
use qt_memory::retrieval::dense::{EmbedFn, EmbeddingOutput};
use qt_memory::retrieval::{
    Cancellation, DenseIndexConfig, Embedder, LocalModelEmbedder, RetrievalEngineOptions,
};
use qt_memory::runtime::{
    CaptureHooks, DreamResearchReceipt, DreamingControl, MemoryDreamingStatus,
};
use qt_memory::settings::MemorySettings;
use qt_memory::{
    MemoryCoordinatorSettings, MemoryRuntime, MemoryRuntimeOptions, MemoryScope, RecallPolicy,
};
use serde_json::{Value, json};

/// Local models selected for each memory task (archived
/// `MemoryTaskModelPlan`), as `llama/<id>` references.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MemoryTaskModelPlan {
    pub document_embedding: Option<String>,
    pub short_embedding: Option<String>,
    pub worthiness: Option<String>,
    pub extraction: Option<String>,
    pub recall_prehandler: Option<String>,
    pub context_compilation: Option<String>,
    /// Model files the children must discover (`--discover-local-models`).
    pub discovery_roots: Vec<String>,
    pub context_compilation_timeout_floor_ms: i64,
    pub context_compilation_input_token_limit: usize,
}

impl Default for MemoryTaskModelPlan {
    fn default() -> Self {
        Self {
            document_embedding: None,
            short_embedding: None,
            worthiness: None,
            extraction: None,
            recall_prehandler: None,
            context_compilation: None,
            discovery_roots: Vec::new(),
            context_compilation_timeout_floor_ms: 600_000,
            context_compilation_input_token_limit: 6000,
        }
    }
}

impl MemoryTaskModelPlan {
    fn all_resolved(&self) -> bool {
        self.document_embedding.is_some()
            && self.short_embedding.is_some()
            && self.worthiness.is_some()
            && self.extraction.is_some()
            && self.recall_prehandler.is_some()
            && self.context_compilation.is_some()
    }
}

fn normalize(path: &str) -> String {
    qt_llama::settings::lexically_normal(Path::new(path))
        .to_string_lossy()
        .replace('\\', "/")
}

/// Archived `resolve_memory_task_models`: configured readable GGUFs are
/// authoritative; when a task is unresolved, `<cwd>/models` and
/// `<agent_home>/models` are scanned too, and the files of discovered
/// pre-handler / compilation models become child discovery roots.
pub fn resolve_memory_task_models(
    llama: &LlamaSettings,
    cwd: &Path,
    agent_home: &Path,
) -> MemoryTaskModelPlan {
    let mut candidates = llama.clone();
    let mut plan = MemoryTaskModelPlan::default();
    let select_all = |candidates: &LlamaSettings, plan: &mut MemoryTaskModelPlan| {
        let select = |task| {
            select_model_for_memory_task(candidates, task).map(|m| format!("llama/{}", m.id))
        };
        plan.document_embedding = select(MemoryTask::DocumentEmbedding);
        plan.short_embedding = select(MemoryTask::ShortEmbedding);
        plan.worthiness = select(MemoryTask::Worthiness);
        plan.extraction = select(MemoryTask::Extraction);
        plan.recall_prehandler = select(MemoryTask::RecallPrehandler);
        plan.context_compilation = select(MemoryTask::ContextCompilation);
    };
    select_all(&candidates, &mut plan);
    if plan.all_resolved() {
        return plan;
    }
    let mut roots: Vec<PathBuf> = Vec::new();
    for root in [cwd.join("models"), agent_home.join("models")] {
        if root.is_dir() && !roots.contains(&root) {
            roots.push(root);
        }
    }
    if !roots.is_empty() {
        let discovered = discover_models(&DiscoveryOptions {
            roots,
            recursive: true,
        });
        let mut paths: Vec<String> = candidates
            .models
            .iter()
            .map(|m| normalize(&m.path))
            .collect();
        for model in &discovered.models {
            let path = normalize(&model.path);
            if !paths.contains(&path) {
                paths.push(path);
                candidates.models.push(model.to_settings());
            }
        }
    }
    select_all(&candidates, &mut plan);
    let configured: Vec<String> = llama.models.iter().map(|m| m.id.to_lowercase()).collect();
    for selected in [&plan.recall_prehandler, &plan.context_compilation]
        .into_iter()
        .flatten()
    {
        let id = selected
            .split_once('/')
            .map(|(_, id)| id)
            .unwrap_or(selected);
        if configured.contains(&id.to_lowercase()) {
            continue;
        }
        if let Some(model) = candidates
            .models
            .iter()
            .find(|m| m.id.eq_ignore_ascii_case(id))
            && !plan.discovery_roots.contains(&model.path)
        {
            plan.discovery_roots.push(model.path.clone());
        }
    }
    plan
}

/// An embedder that always fails with the archived configuration message
/// (dense retrieval enabled without a usable `embeddingModel`).
struct UnconfiguredEmbedder(String);

impl Embedder for UnconfiguredEmbedder {
    fn configure(&self, _cancelled: Cancellation<'_>) -> qt_memory::Result<DenseIndexConfig> {
        Err(MemoryError::invalid(self.0.clone()))
    }

    fn embed(
        &self,
        _config: &DenseIndexConfig,
        _text: &str,
        _cancelled: Cancellation<'_>,
    ) -> qt_memory::Result<Vec<f32>> {
        Err(MemoryError::invalid(self.0.clone()))
    }
}

/// The dense embedder for `memory.retrieval` (archived
/// `bind_llama_retrieval`): the configured `embeddingModel` served by the
/// in-process llama.cpp runtime. Calls check cancellation before embedding.
pub fn dense_embedder(
    memory: &MemorySettings,
    llama_settings: &LlamaSettings,
    backend: Arc<LlamaProviderBackend>,
    settings: qt_core::Settings,
) -> Option<Arc<dyn Embedder>> {
    let retrieval = &memory.retrieval;
    if !retrieval.dense_enabled {
        return None;
    }
    let Some(model_id) = retrieval.embedding_model.clone() else {
        return Some(Arc::new(UnconfiguredEmbedder(
            "dense retrieval requires memory.retrieval.embeddingModel".into(),
        )));
    };
    let Some(model) = llama_settings
        .models
        .iter()
        .find(|m| m.id == model_id)
        .cloned()
    else {
        return Some(Arc::new(UnconfiguredEmbedder(format!(
            "dense embeddingModel is not registered in llama.models: {model_id}"
        ))));
    };
    let embed: EmbedFn = Arc::new(
        move |model_id: &str, text: &str, normalize: bool, cancelled: Cancellation<'_>| {
            if cancelled() {
                return Err(MemoryError::storage("embedding cancelled"));
            }
            let mut request = EmbeddingRequest::new(model_id, text);
            request.normalize = normalize;
            request.queue_timeout = std::time::Duration::from_secs(30);
            let result = backend
                .embed(&settings, request)
                .map_err(|e| MemoryError::storage(e.to_string()))?;
            Ok(EmbeddingOutput {
                pooling: result
                    .metadata
                    .get("pooling")
                    .and_then(Value::as_str)
                    .unwrap_or("mean")
                    .to_string(),
                normalized: result
                    .metadata
                    .get("normalized")
                    .and_then(Value::as_bool)
                    .unwrap_or(normalize),
                embedding: result.embedding,
            })
        },
    );
    match LocalModelEmbedder::new(
        model.id.clone(),
        model.path.clone(),
        retrieval.query_prefix.clone(),
        retrieval.document_prefix.clone(),
        llama_settings.embeddings.normalize.unwrap_or(true),
        embed,
    ) {
        Ok(embedder) => Some(Arc::new(embedder)),
        Err(e) => Some(Arc::new(UnconfiguredEmbedder(e.to_string()))),
    }
}

/// Everything [`memory_runtime_options`] needs besides settings.
#[derive(Clone, Default)]
pub struct MemoryInputs {
    pub agent_home: PathBuf,
    pub cwd: PathBuf,
    pub plan: MemoryTaskModelPlan,
    pub code: Option<Arc<dyn CodeSnapshotProvider>>,
    pub embedder: Option<Arc<dyn Embedder>>,
    /// Executable of the pre-handler / consolidation children.
    pub child_executable: Option<PathBuf>,
    pub capture_hooks: Option<Arc<dyn CaptureHooks>>,
    /// Start the capture worker (and recover spooled episodes).
    pub background_processing: bool,
}

/// `MemoryRuntimeOptions` from resolved `memory.*` settings (archived
/// mapping, including the pre-handler timeout floor and input bound when a
/// local pre-handler model was selected automatically).
pub fn memory_runtime_options(
    settings: &MemorySettings,
    inputs: &MemoryInputs,
) -> MemoryRuntimeOptions {
    let retrieval = &settings.retrieval;
    let plan = &inputs.plan;
    let policy = RecallPolicy::from_setting(&settings.recall_policy);
    let code_enabled = retrieval.code_enabled && inputs.code.is_some();
    let mut coordinator = MemoryCoordinatorSettings {
        enabled: true,
        automatic_recall: settings.automatic_recall,
        recall_policy: policy,
        context_token_budget: settings.context_token_budget as usize,
        candidate_limit: settings.candidate_limit as usize,
        graph: qt_memory::cli::configured_graph(settings),
        dense_enabled: retrieval.dense_enabled,
        model: settings
            .prehandler
            .model
            .clone()
            .or_else(|| plan.recall_prehandler.clone())
            .unwrap_or_default(),
        model_task: "recall_prehandler".into(),
        thinking: settings.prehandler.thinking.clone(),
        timeout_ms: settings.prehandler.timeout_ms,
        fail_open: settings.prehandler.fail_open,
        max_model_input_tokens: (settings.prehandler.max_model_input_tokens as usize).min(
            if plan.recall_prehandler.is_some() {
                3000
            } else {
                12000
            },
        ),
        model_discovery_roots: plan.discovery_roots.clone(),
        ..Default::default()
    };
    if settings.prehandler.model.is_none() && plan.recall_prehandler.is_some() {
        coordinator.timeout_ms = coordinator.timeout_ms.max(120_000);
    }
    let mut options = MemoryRuntimeOptions {
        agent_home: inputs.agent_home.clone(),
        cwd: inputs.cwd.clone(),
        coordinator,
        automatic_capture: settings.automatic_capture,
        capture_post_process: settings.capture.post_process.clone(),
        max_evidence_characters: settings.capture.max_evidence_chars as usize,
        retrieval: RetrievalEngineOptions {
            enabled: retrieval.graph_enabled || retrieval.dense_enabled || retrieval.code_enabled,
            background_indexing: inputs.background_processing,
            dense_enabled: retrieval.dense_enabled,
            code_enabled,
            code_graph_enabled: code_enabled && retrieval.graph_enabled,
            graph_max_seeds: retrieval.max_seeds as usize,
            graph_max_depth: retrieval.max_depth as usize,
            graph_max_fanout: retrieval.max_fanout as usize,
            graph_max_edges: retrieval.max_edges as usize,
            graph_max_results: retrieval.max_graph_results as usize,
            code: if code_enabled {
                inputs.code.clone()
            } else {
                None
            },
            cache_entries: retrieval.query_cache_entries as usize,
            policy,
            include_global_user: false,
            embedder: inputs.embedder.clone(),
        },
        background_processing: inputs.background_processing,
        allow_dream_promotion: settings.dreaming.promotion_policy == "auto_validate",
        runner: inputs.child_executable.clone().map(|exe| {
            Arc::new(ProcessAgentRunner::new(exe))
                as Arc<dyn qt_memory::isolated::IsolatedAgentRunner>
        }),
        capture_hooks: inputs.capture_hooks.clone(),
        ..Default::default()
    };
    if let Some(model) = plan
        .context_compilation
        .clone()
        .filter(|m| *m != options.coordinator.model)
    {
        options.consolidation_model = model;
        options.consolidation_timeout_ms = plan.context_compilation_timeout_floor_ms;
        options.consolidation_max_model_input_tokens = plan.context_compilation_input_token_limit;
        options.consolidation_model_discovery_roots = plan.discovery_roots.clone();
    }
    options
}

/// Post-capture hooks: deterministic post-processing after each captured
/// episode (`capture.postProcess = turn_end`, archived limits 500 records /
/// 100 pairs) and capture counts for Dreaming triggers.
pub struct MemoryCaptureHooks {
    post: OnceLock<PostProcessingService>,
    dreaming: OnceLock<DreamingService>,
}

impl MemoryCaptureHooks {
    pub fn new() -> Self {
        Self {
            post: OnceLock::new(),
            dreaming: OnceLock::new(),
        }
    }

    fn bind(&self, post: PostProcessingService, dreaming: Option<DreamingService>) {
        let _ = self.post.set(post);
        if let Some(d) = dreaming {
            let _ = self.dreaming.set(d);
        }
    }
}

impl Default for MemoryCaptureHooks {
    fn default() -> Self {
        Self::new()
    }
}

impl CaptureHooks for MemoryCaptureHooks {
    fn post_process(&self, scope: &MemoryScope) -> qt_memory::Result<()> {
        match self.post.get() {
            Some(post) => post.process(scope, 500, 100).map(|_| ()),
            None => Ok(()),
        }
    }

    fn candidates_captured(
        &self,
        _scope: &MemoryScope,
        _turn: u64,
        extracted: usize,
        inserted: usize,
    ) {
        if let Some(dreaming) = self.dreaming.get() {
            dreaming.note_capture(extracted, inserted);
        }
    }
}

/// [`DreamingControl`] over a [`DreamingService`] (slash, CLI and RPC).
pub struct DreamingAdapter(pub DreamingService);

impl DreamingControl for DreamingAdapter {
    fn status(&self) -> MemoryDreamingStatus {
        let s = self.0.status();
        MemoryDreamingStatus {
            enabled: s.enabled,
            trigger: s.trigger,
            idle_seconds: s.idle_seconds as i64,
            idle_for_seconds: s.idle_for_seconds as i64,
            waiting_for_idle: s.waiting_for_idle,
            running: s.running,
            pending_runs: s.pending_runs,
            completed_runs: s.completed_runs as usize,
            failed_runs: s.failed_runs as usize,
            last_started_at: s.last_started_at,
            last_completed_at: s.last_completed_at,
            last_error: s.last_error,
            last_dream_id: s.last_dream_id,
            last_review_id: s.last_review_id,
        }
    }

    fn enqueue(&self, trigger: &str) -> bool {
        DreamTrigger::parse(trigger).is_some_and(|t| self.0.enqueue(t, None))
    }

    fn add_research(
        &self,
        dream_id: &str,
        params: &Value,
        created_by: &str,
    ) -> qt_memory::Result<DreamResearchReceipt> {
        let mut input = if params.is_object() {
            params.clone()
        } else {
            json!({})
        };
        input["dream_id"] = json!(dream_id);
        let origin = if created_by == ManualEvidenceOrigin::Interactive.created_by() {
            ManualEvidenceOrigin::Interactive
        } else {
            ManualEvidenceOrigin::Rpc
        };
        let (dream_id, evidence) = parse_manual_research(&input, origin)?;
        let result = self.0.add_research(&dream_id, evidence)?;
        Ok(DreamResearchReceipt {
            dream_id,
            research_id: result.researched_memory_id,
            research_review_id: result.research_review_id,
        })
    }
}

/// The memory subsystem of a runtime.
pub struct MemorySubsystem {
    pub settings: MemorySettings,
    pub plan: MemoryTaskModelPlan,
    runtime: Arc<MemoryRuntime>,
    dreaming: Option<DreamingService>,
    worker: Mutex<Option<DreamingWorker>>,
    attached: Mutex<Option<(Hooks, MemoryHookIds, Arc<EpisodeBridge>)>>,
    activity_hook: Mutex<Option<(Hooks, HookId)>>,
    diagnostics: Vec<String>,
}

impl MemorySubsystem {
    /// Open the memory runtime and, when `memory.dreaming.enabled`, the
    /// Dreaming service (children run `child_executable`).
    pub fn open(settings: MemorySettings, mut inputs: MemoryInputs) -> qt_memory::Result<Self> {
        let hooks = Arc::new(MemoryCaptureHooks::new());
        inputs.capture_hooks = Some(hooks.clone());
        let options = memory_runtime_options(&settings, &inputs);
        let runtime = Arc::new(MemoryRuntime::new(options)?);
        let dreaming = settings.dreaming.enabled.then(|| {
            let mut config = DreamingConfig::new(settings.dreaming.clone());
            config.consolidation_model = inputs.plan.context_compilation.clone();
            config.prehandler_model = settings
                .prehandler
                .model
                .clone()
                .or_else(|| inputs.plan.recall_prehandler.clone());
            config.discovery_roots = inputs
                .plan
                .discovery_roots
                .iter()
                .map(PathBuf::from)
                .collect();
            config.cwd = inputs.cwd.to_string_lossy().into_owned();
            config.agent_home = Some(inputs.agent_home.clone());
            let child = inputs
                .child_executable
                .clone()
                .map(|exe| Arc::new(ProcessChild::new(exe)) as Arc<dyn IsolatedChild>);
            DreamingService::new(Arc::clone(runtime.store()), config, runtime.scope(), child)
        });
        hooks.bind(
            PostProcessingService::new(Arc::clone(runtime.store())),
            dreaming.clone(),
        );
        if let Some(service) = &dreaming {
            runtime.set_dreaming(Arc::new(DreamingAdapter(service.clone())));
        }
        let mut diagnostics = Vec::new();
        if settings.retrieval.dense_enabled && inputs.embedder.is_none() {
            diagnostics.push("dense retrieval requires memory.retrieval.embeddingModel".into());
        }
        Ok(Self {
            settings,
            plan: inputs.plan,
            runtime,
            dreaming,
            worker: Mutex::new(None),
            attached: Mutex::new(None),
            activity_hook: Mutex::new(None),
            diagnostics,
        })
    }

    pub fn runtime(&self) -> &Arc<MemoryRuntime> {
        &self.runtime
    }

    pub fn dreaming(&self) -> Option<&DreamingService> {
        self.dreaming.as_ref()
    }

    pub fn diagnostics(&self) -> &[String] {
        &self.diagnostics
    }

    /// Register recall, capture and lifecycle hooks for `session_id`
    /// (replacing a previous attachment) plus Dreaming activity tracking.
    pub fn attach(&self, hooks: &Hooks, session_id: &str) {
        self.detach();
        let (bridge, ids) = attach_memory(hooks, Arc::clone(&self.runtime), session_id);
        *self.attached.lock().expect("memory lock") = Some((hooks.clone(), ids, bridge));
        if let Some(dreaming) = self.dreaming.clone() {
            let id =
                hooks.add_agent_event_listener(Arc::new(move |event: &AgentEvent| match event {
                    AgentEvent::AgentStart => dreaming.set_foreground_active(true),
                    AgentEvent::AgentEnd { .. } => dreaming.set_foreground_active(false),
                    AgentEvent::MessageEnd(_) => dreaming.note_activity(),
                    _ => {}
                }));
            *self.activity_hook.lock().expect("memory lock") = Some((hooks.clone(), id));
        }
    }

    /// Remove the hooks registered by [`Self::attach`].
    pub fn detach(&self) {
        if let Some((hooks, ids, _)) = self.attached.lock().expect("memory lock").take() {
            detach_memory(&hooks, &ids);
        }
        if let Some((hooks, id)) = self.activity_hook.lock().expect("memory lock").take() {
            hooks.remove(id);
        }
    }

    /// Start the Dreaming worker (interactive use only).
    pub fn start_background(&self) {
        let mut worker = self.worker.lock().expect("memory lock");
        if worker.is_none()
            && let Some(service) = &self.dreaming
        {
            *worker = Some(service.start_worker());
        }
    }

    /// `/context memory` lines for the conversation context `messages`.
    pub fn context_lines(&self, messages: &[Message]) -> Vec<String> {
        let (_, epoch) = logical_input(messages);
        let Some(augmentation) = self.runtime.coordinator().last_augmentation(epoch) else {
            return vec!["- no augmentation has been prepared in this projection".into()];
        };
        let c = &augmentation.contribution;
        let mut lines = vec![
            format!("- logical_input_sequence: {}", c.logical_input_sequence),
            format!("- memory_ids: {}", json!(c.memory_ids)),
            format!("- code_ids: {}", json!(c.code_ids)),
            format!("- block_ids: {}", json!(c.block_ids)),
            format!("- estimated_tokens: {}", c.estimated_tokens),
            format!("- omission_reason: {}", c.omission_reason),
            format!(
                "- isolated_agent: {}, deterministic_fallback: {}",
                augmentation.used_isolated_agent, augmentation.used_deterministic_fallback
            ),
        ];
        for w in &c.warnings {
            lines.push(format!(
                "- warning [{}] {} memory={} from={} to={}",
                w.code, w.message, w.memory_id, w.from_id, w.to_id
            ));
        }
        lines.push("memory_diagnostics:".into());
        if augmentation.diagnostics.is_empty() {
            lines.push("- none".into());
        } else {
            lines.extend(augmentation.diagnostics.iter().map(|d| format!("- {d}")));
        }
        lines.push("memory_context_markdown:".into());
        if c.context_markdown.is_empty() {
            lines.push("(empty)".into());
        } else {
            lines.extend(c.context_markdown.split('\n').map(str::to_string));
        }
        lines
    }

    /// Stop Dreaming and the capture worker, flushing pending captures for
    /// at most `timeout`.
    pub fn shutdown(&self, timeout: std::time::Duration) {
        self.detach();
        if let Some(worker) = self.worker.lock().expect("memory lock").take() {
            worker.stop();
        }
        self.runtime.wait_for_capture_idle(timeout);
        self.runtime.shutdown();
    }
}

/// `/context memory` lines for an agent context.
pub fn context_messages(messages: &[qt_core::AgentMessage]) -> Vec<Message> {
    convert_to_llm(messages)
}

/// The memory child task this process runs as, from the archived
/// environment markers (`QT_MEMORY_PREHANDLER`, `QT_MEMORY_DREAMING`,
/// `QT_MEMORY_DREAM_RESEARCH`, `QT_MEMORY_DREAM_VERIFY`).
pub fn memory_child_task() -> Option<ChildTask> {
    let flag = |name: &str| {
        std::env::var(name).is_ok_and(|v| {
            let v = v.trim().to_ascii_lowercase();
            !v.is_empty() && !matches!(v.as_str(), "0" | "false" | "no" | "off")
        })
    };
    if flag("QT_MEMORY_DREAM_VERIFY") {
        Some(ChildTask::DreamVerification)
    } else if flag("QT_MEMORY_DREAM_RESEARCH") {
        Some(ChildTask::DreamResearch)
    } else if flag("QT_MEMORY_DREAMING") {
        Some(ChildTask::Dreaming)
    } else if flag("QT_MEMORY_PREHANDLER") {
        Some(ChildTask::SemanticConsolidation)
    } else {
        None
    }
}

/// Archived llama overrides inside memory children: an 8192-token context,
/// up to 16 threads, the task temperature, seed 0, the task output bound,
/// no KV reuse and the task grammar; `QT_MEMORY_FORCE_CPU` disables GPU
/// offload.
pub fn apply_child_llama_overrides(
    settings: &mut LlamaSettings,
    task: Option<ChildTask>,
    force_cpu: bool,
) {
    if let Some(task) = task {
        settings.context.context_size = Some(i64::from(task.context_size()));
        let threads = std::thread::available_parallelism()
            .map(|n| n.get())
            .unwrap_or(8)
            .min(16) as i32;
        settings.context.threads = Some(threads);
        settings.context.batch_threads = Some(threads);
        settings.generation.temperature = Some(task.temperature());
        settings.generation.seed = Some(0);
        settings.generation.max_output_tokens = Some(i64::from(task.max_output_tokens()));
        settings.kv_cache.policy = Some("off".into());
        settings.grammar.default_grammar = task.grammar().map(str::to_string);
    }
    if force_cpu {
        settings.backend.gpu_layers = Some(0);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use qt_llama::ModelSettings;

    #[test]
    fn task_plan_prefers_recommended_models_and_adds_discovery_roots() {
        let dir = tempfile::tempdir().unwrap();
        let cwd = dir.path().join("work");
        let home = dir.path().join("home");
        std::fs::create_dir_all(&cwd).unwrap();
        std::fs::create_dir_all(&home).unwrap();
        let configured = dir.path().join("qwen2.5-3b-instruct-q4_k_m.gguf");
        std::fs::write(&configured, b"x").unwrap();
        let llama = LlamaSettings {
            models: vec![ModelSettings {
                id: "qwen2-5-3b-instruct-q4-k-m".into(),
                name: "Qwen".into(),
                path: configured.to_string_lossy().into_owned(),
                ..Default::default()
            }],
            ..Default::default()
        };
        let plan = resolve_memory_task_models(&llama, &cwd, &home);
        assert_eq!(
            plan.recall_prehandler.as_deref(),
            Some("llama/qwen2-5-3b-instruct-q4-k-m")
        );
        assert_eq!(plan.context_compilation, plan.recall_prehandler);
        assert!(plan.document_embedding.is_none());
        assert!(plan.discovery_roots.is_empty());
    }

    #[test]
    fn runtime_options_follow_archived_mapping() {
        let mut settings = MemorySettings {
            enabled: true,
            ..Default::default()
        };
        settings.prehandler.timeout_ms = 1000;
        let plan = MemoryTaskModelPlan {
            recall_prehandler: Some("llama/a".into()),
            context_compilation: Some("llama/b".into()),
            ..Default::default()
        };
        let options = memory_runtime_options(
            &settings,
            &MemoryInputs {
                plan,
                child_executable: Some(PathBuf::from("/bin/qt-cli")),
                ..Default::default()
            },
        );
        assert_eq!(options.coordinator.model, "llama/a");
        assert_eq!(options.coordinator.timeout_ms, 120_000);
        assert!(options.coordinator.max_model_input_tokens <= 3000);
        assert_eq!(options.consolidation_model, "llama/b");
        assert_eq!(options.consolidation_timeout_ms, 600_000);
        assert!(options.runner.is_some());
    }

    #[test]
    fn child_overrides_shrink_the_context_and_pin_the_grammar() {
        let mut llama = LlamaSettings::default();
        apply_child_llama_overrides(&mut llama, Some(ChildTask::Dreaming), true);
        assert_eq!(llama.context.context_size, Some(8192));
        assert_eq!(llama.generation.temperature, Some(0.2));
        assert_eq!(llama.generation.max_output_tokens, Some(1024));
        assert_eq!(llama.kv_cache.policy.as_deref(), Some("off"));
        assert!(
            llama
                .grammar
                .default_grammar
                .as_deref()
                .unwrap()
                .starts_with("root ::=")
        );
        assert_eq!(llama.backend.gpu_layers, Some(0));
        let mut verify = LlamaSettings::default();
        apply_child_llama_overrides(&mut verify, Some(ChildTask::DreamVerification), false);
        assert!(verify.grammar.default_grammar.is_none());
        assert_eq!(verify.generation.max_output_tokens, Some(1536));
    }
}
