//! [`Runtime`]: the built engine plus every subsystem handle.

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::Duration;

use anyhow::{Context as _, Result, anyhow, bail};
use serde_json::Value;

use qt_core::config::AgentHome;
use qt_core::engine::{Conversation, Engine, ModelRefresh};
use qt_core::harness::Hooks;
use qt_core::harness::hooks::ModelSelectEvent;
use qt_core::migrations::{StartupMigrationResult, run_startup_migrations};
use qt_core::model_scope::ScopedModel;
use qt_core::policy::WorkspacePolicy;
use qt_core::provider::{ModelInfo, ModelRef, ThinkingLevel};
use qt_core::resources::ResourceOptions;
use qt_core::settings_layers::LayeredSettings;
use qt_core::tools::{Tool, ToolRegistry};
use qt_extensions::{ConversationHost, DiscoveryOptions, ExtensionUi, Extensions, SandboxPolicy};
use qt_index::{BackgroundService, IndexManager, index_tools};
use qt_llama::LlamaProviderBackend;
use qt_mcp::McpManager;
use qt_memory::code::CodeSnapshotProvider;
use qt_memory::settings::{MemorySettings, resolve_memory_settings};

use crate::code::IndexCodeSnapshots;
use crate::extensions::{ExtensionProviderBackend, extension_infos, extension_slash_commands};
use crate::mcp::{EngineSamplingHandler, McpToolSource, build_manager};
use crate::memory::{
    MemoryInputs, MemorySubsystem, apply_child_llama_overrides, dense_embedder, memory_child_task,
    resolve_memory_task_models,
};
use crate::options::{RunMode, RuntimeOptions, resolve_model_argument};
use crate::subagents::{RunSink, SubagentSubsystem};

/// Interval of the interactive OpenAI / OpenRouter catalog refresh (Pi).
const MODEL_REFRESH_INTERVAL: Duration = Duration::from_secs(300);

/// Where the archived binary read `/changelog` from (relative to the cwd).
const CHANGELOG_PATH: &str = "reference/pi/packages/coding-agent/CHANGELOG.md";

/// The changelog shown by `/changelog`: `reference/pi/packages/coding-agent/
/// CHANGELOG.md` under `cwd`, as the archived binary read it.
pub fn changelog_markdown(cwd: &Path) -> Option<String> {
    std::fs::read_to_string(cwd.join(CHANGELOG_PATH)).ok()
}

/// How [`Runtime::attach`] binds a conversation.
#[derive(Clone, Default)]
pub struct AttachOptions {
    /// Receives the event streams of runs that extensions or worker
    /// completions start in this conversation.
    pub run_sink: Option<RunSink>,
    /// Deliver persistent-worker completions as follow-ups automatically
    /// (interactive). Print / JSON modes poll the coordinator instead.
    pub deliver_subagent_events: bool,
}

struct Inner {
    engine: Engine,
    options: RuntimeOptions,
    home: AgentHome,
    migrations: StartupMigrationResult,
    llama: Arc<LlamaProviderBackend>,
    index: Arc<IndexManager>,
    index_service: Arc<BackgroundService>,
    code: Arc<IndexCodeSnapshots>,
    mcp: Option<Arc<McpManager>>,
    mcp_connected: AtomicBool,
    memory: Option<Arc<MemorySubsystem>>,
    memory_settings: MemorySettings,
    extensions: Extensions,
    subagents: SubagentSubsystem,
    diagnostics: Mutex<Vec<String>>,
    extra_messages: Vec<String>,
    selected: Option<(ModelRef, Option<ThinkingLevel>)>,
    scope: Vec<ScopedModel>,
    active: Arc<Mutex<Option<Conversation>>>,
    current_model: Arc<Mutex<Option<ModelRef>>>,
    refresh: Mutex<Option<ModelRefresh>>,
    background: AtomicBool,
}

/// A built qt runtime. Cheap to clone.
#[derive(Clone)]
pub struct Runtime {
    inner: Arc<Inner>,
}

impl std::fmt::Debug for Runtime {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Runtime")
            .field("cwd", &self.inner.engine.cwd())
            .field("mode", &self.inner.options.mode)
            .finish_non_exhaustive()
    }
}

fn child_executable(options: &RuntimeOptions) -> PathBuf {
    options
        .child_executable
        .clone()
        .unwrap_or_else(qt_subagents::runner::resolve_child_executable)
}

fn env_flag(name: &str) -> bool {
    std::env::var(name).is_ok_and(|v| {
        let v = v.trim().to_ascii_lowercase();
        !v.is_empty() && !matches!(v.as_str(), "0" | "false" | "no" | "off")
    })
}

impl Runtime {
    /// Build the engine and every subsystem.
    pub fn build(options: RuntimeOptions) -> Result<Self> {
        let home = options
            .agent_home
            .clone()
            .map(AgentHome::new)
            .unwrap_or_else(AgentHome::from_env);
        let cwd = options
            .cwd
            .canonicalize()
            .unwrap_or_else(|_| options.cwd.clone());
        let migrations = if options.startup_migrations {
            run_startup_migrations(&cwd, &home)
        } else {
            StartupMigrationResult::default()
        };
        let mut diagnostics: Vec<String> = Vec::new();
        let layered = LayeredSettings::load(&home, &cwd);
        let effective = layered.effective();
        let workspace_root = WorkspacePolicy::from_settings(
            &cwd,
            layered.global.raw(),
            (!layered.project.is_empty()).then_some(&layered.project),
        )
        .map(|p| p.root().to_path_buf())
        .unwrap_or_else(|_| cwd.clone());
        let engine_cell: Arc<OnceLock<Engine>> = Arc::new(OnceLock::new());
        let hooks = Hooks::new();
        let current_model: Arc<Mutex<Option<ModelRef>>> = Arc::new(Mutex::new(None));
        let tracked = current_model.clone();
        hooks.add_model_select(Arc::new(move |e: &ModelSelectEvent| {
            *tracked.lock().expect("model lock") = Some(e.model.clone());
        }));

        // Extensions first: their hooks see `resources_discover` at startup.
        let extensions = Extensions::load(DiscoveryOptions {
            cwd: cwd.clone(),
            workspace_root: Some(workspace_root.clone()),
            agent_home: home.root().to_path_buf(),
            cli_paths: options.extensions.clone(),
            include_discovered: !options.no_extensions,
            policy: SandboxPolicy::default(),
        });
        diagnostics.extend(extensions.diagnostics());
        let rest = extensions
            .apply_cli_flags(&options.extension_flags)
            .map_err(|e| anyhow!(e))?;
        if let Some(flag) = rest.iter().find(|a| a.starts_with('-')) {
            let name = flag.split('=').next().unwrap_or(flag);
            bail!("Unknown option: {name}");
        }
        // Values the CLI parser attached to what turned out to be boolean
        // extension flags are messages after all.
        let extra_messages = rest;
        extensions.install_hooks(&hooks);

        // llama.cpp, with the archived overrides inside memory children.
        let child_task = memory_child_task();
        let force_cpu = env_flag("QT_MEMORY_FORCE_CPU");
        let mut llama = LlamaProviderBackend::new(home.clone());
        if child_task.is_some() || force_cpu {
            llama = llama.with_settings_override(Arc::new(move |s| {
                apply_child_llama_overrides(s, child_task, force_cpu)
            }));
        }
        let llama = Arc::new(llama);
        if !options.discover_local_models.is_empty() {
            let roots: Vec<PathBuf> = options
                .discover_local_models
                .iter()
                .map(|p| {
                    if p.is_absolute() {
                        p.clone()
                    } else {
                        cwd.join(p)
                    }
                })
                .collect();
            let (_, notes) = llama.discover_and_register(&effective, &cwd, &roots);
            diagnostics.extend(notes);
        }

        // Project index: background scans only for interactive use.
        let index = Arc::new(IndexManager::new(home.root()));
        let index_service = if options.mode.is_interactive() {
            match index.start(&workspace_root) {
                Ok(service) => service,
                Err(e) => {
                    diagnostics.push(format!("index service could not start: {e}"));
                    index.service_for(&workspace_root)
                }
            }
        } else {
            index.service_for(&workspace_root)
        };
        let code = Arc::new(IndexCodeSnapshots::new(index_service.clone()));
        let child_exe = child_executable(&options);

        // Memory.
        let memory_settings = match resolve_memory_settings(
            &Value::Object(layered.global.raw().clone()),
            (!layered.project.is_empty())
                .then(|| Value::Object(layered.project.clone()))
                .as_ref(),
        ) {
            Ok(s) => s,
            Err(e) => {
                diagnostics.push(format!("memory settings ignored: {e}"));
                MemorySettings::default()
            }
        };
        let memory = if memory_settings.enabled && !options.no_memory && child_task.is_none() {
            let llama_settings = llama.effective_settings(&effective);
            let plan = resolve_memory_task_models(&llama_settings, &cwd, home.root());
            let inputs = MemoryInputs {
                agent_home: home.root().to_path_buf(),
                cwd: cwd.clone(),
                plan,
                code: Some(code.clone() as Arc<dyn CodeSnapshotProvider>),
                embedder: dense_embedder(
                    &memory_settings,
                    &llama_settings,
                    llama.clone(),
                    effective.clone(),
                ),
                child_executable: Some(child_exe.clone()),
                capture_hooks: None,
                background_processing: true,
            };
            match MemorySubsystem::open(memory_settings.clone(), inputs) {
                Ok(m) => {
                    diagnostics.extend(m.diagnostics().iter().cloned());
                    Some(Arc::new(m))
                }
                Err(e) if !memory_settings.prehandler.fail_open => {
                    bail!("unable to initialize built-in memory: {e}")
                }
                Err(e) => {
                    diagnostics.push(format!(
                        "built-in memory disabled after initialization error: {e}"
                    ));
                    None
                }
            }
        } else {
            None
        };

        // Built-in tools: the core set plus index, memory and subagent tools
        // (the archived registry), or only the restricted research tools in a
        // Dreaming research child.
        let subagents = SubagentSubsystem::new(home.root().to_path_buf(), Some(child_exe.clone()));
        let builtin = if child_task == Some(qt_memory::dreaming::ChildTask::DreamResearch) {
            qt_memory::research::research_registry()
        } else {
            let mut registry = ToolRegistry::builtin();
            for tool in index_tools(index.clone(), |_| true) {
                registry = registry.with(tool);
            }
            if let Some(memory) = &memory {
                for tool in qt_memory::tools::memory_tools(memory.runtime(), |_| true) {
                    registry = registry.with(tool);
                }
            }
            registry.with(subagents.tool())
        };

        // MCP.
        let mcp = match build_manager(
            layered.global.raw(),
            &layered.project,
            home.root(),
            &workspace_root,
        ) {
            Ok(m) => m,
            Err(e) => {
                diagnostics.push(format!("MCP configuration ignored: {e}"));
                None
            }
        };
        if let Some(manager) = &mcp {
            manager.set_sampling_handler(Some(Arc::new(EngineSamplingHandler::new(
                engine_cell.clone(),
                current_model.clone(),
            ))));
        }

        let resource_options = ResourceOptions {
            skill_paths: options.skills.clone(),
            prompt_template_paths: options.prompt_templates.clone(),
            theme_paths: options.themes.clone(),
            no_skills: options.no_skills,
            no_prompt_templates: options.no_prompt_templates,
            no_themes: options.no_themes,
            system_prompt: options.system_prompt.clone(),
            append_system_prompt: options.append_system_prompt.clone(),
            ..ResourceOptions::new(&cwd)
        };
        let ext_for_tools = extensions.clone();
        let mut builder = Engine::builder(home.clone(), &cwd)
            .builtin_tools(builtin)
            .tool_source(Arc::new(move || ext_for_tools.tools()))
            .tool_selection(options.tool_selection())
            .provider_backend(llama.clone())
            .provider_backend(Arc::new(ExtensionProviderBackend::new(
                extensions.clone(),
                home.clone(),
                engine_cell.clone(),
            )))
            .hooks(hooks)
            .resource_options(resource_options)
            .no_context_files(options.no_context_files)
            .offline(options.offline)
            .session_dir(options.session_dir.clone())
            .ephemeral_sessions(options.no_session)
            .startup_migrations(false);
        if let Some(manager) = &mcp {
            builder = builder.tool_source(Arc::new(McpToolSource(manager.clone())));
        }
        if let Some(web) = options.web.clone() {
            builder = builder.web_options(web);
        }
        if let Some(changelog) = changelog_markdown(&cwd) {
            builder = builder.changelog(changelog);
        }
        let engine = builder.build()?;
        let _ = engine_cell.set(engine.clone());
        diagnostics.extend(engine.settings_diagnostics());
        diagnostics.extend(llama.diagnostics());

        engine.set_extension_commands(extension_slash_commands(&extensions));
        engine.set_extensions_info(extension_infos(&extensions));
        let lister_engine = engine.clone();
        extensions.set_command_lister(Arc::new(move || {
            lister_engine
                .slash_commands()
                .commands
                .iter()
                .map(|c| {
                    (
                        c.name.clone(),
                        c.description.clone(),
                        c.source.as_str().to_string(),
                    )
                })
                .collect()
        }));

        let active: Arc<Mutex<Option<Conversation>>> = Arc::new(Mutex::new(None));
        if let Some(memory) = &memory {
            let memory = memory.clone();
            let active = active.clone();
            engine.set_memory_context(Some(Arc::new(move || {
                let conversation = active.lock().expect("active lock").clone();
                let messages = conversation
                    .map(|c| crate::memory::context_messages(&c.agent_messages()))
                    .unwrap_or_default();
                memory.context_lines(&messages)
            })));
        }

        if let Some(key) = options.api_key.as_deref().filter(|k| !k.is_empty()) {
            let provider = options
                .provider
                .clone()
                .or_else(|| {
                    options.model.as_deref().and_then(|m| {
                        crate::options::parse_model_argument(None, m, |p| engine.provider(p).is_some())
                            .provider
                    })
                })
                .context("--api-key requires a model to be specified via --model, --provider/--model, or --models")?;
            engine.set_runtime_api_key(&provider, key);
        }

        let providers = engine.providers();
        let mut scope = Vec::new();
        if !options.models.is_empty() {
            let available: Vec<ModelInfo> = providers
                .iter()
                .filter(|p| p.is_ready())
                .flat_map(|p| p.models.iter().map(|m| ModelInfo::from_spec(&p.id, m)))
                .collect();
            let resolution = qt_core::model_scope::resolve_model_scope(&options.models, &available);
            diagnostics.extend(resolution.warnings);
            scope = resolution.models;
        }
        let selected = match options.model.as_deref() {
            Some(model) => Some(
                resolve_model_argument(&providers, options.provider.as_deref(), model)
                    .map_err(|e| anyhow!(e))?,
            ),
            None => match options.provider.as_deref() {
                Some(provider) => {
                    let config = providers
                        .iter()
                        .find(|p| p.id.eq_ignore_ascii_case(provider))
                        .ok_or_else(|| anyhow!("unknown provider selection: {provider}"))?;
                    let default = engine
                        .default_model()
                        .filter(|m| m.provider == config.id)
                        .or_else(|| {
                            config
                                .models
                                .first()
                                .map(|m| ModelRef::new(config.id.clone(), m.id.clone()))
                        })
                        .ok_or_else(|| anyhow!("provider has no models: {provider}"))?;
                    Some((default, None))
                }
                None => scope.first().map(|s| (s.model.clone(), s.thinking)),
            },
        };

        let runtime = Runtime {
            inner: Arc::new(Inner {
                engine,
                home,
                migrations,
                llama,
                index,
                index_service,
                code,
                mcp,
                mcp_connected: AtomicBool::new(false),
                memory,
                memory_settings,
                extensions,
                subagents,
                diagnostics: Mutex::new(diagnostics),
                extra_messages,
                selected,
                scope,
                active,
                current_model,
                refresh: Mutex::new(None),
                background: AtomicBool::new(false),
                options,
            }),
        };
        if runtime.inner.options.mode.is_interactive() {
            // The desktop app creates conversations itself; bind each new one
            // (weakly, so the engine's hooks do not keep the runtime alive).
            let weak = Arc::downgrade(&runtime.inner);
            runtime
                .inner
                .engine
                .hooks()
                .add_conversation_listener(Arc::new(move |conversation: &Conversation| {
                    if let Some(inner) = weak.upgrade() {
                        Runtime { inner }.attach(
                            conversation,
                            AttachOptions {
                                run_sink: None,
                                deliver_subagent_events: true,
                            },
                        );
                    }
                }));
        }
        Ok(runtime)
    }

    // ----- accessors ---------------------------------------------------------

    pub fn engine(&self) -> &Engine {
        &self.inner.engine
    }
    pub fn options(&self) -> &RuntimeOptions {
        &self.inner.options
    }
    pub fn mode(&self) -> RunMode {
        self.inner.options.mode
    }
    pub fn agent_home(&self) -> &AgentHome {
        &self.inner.home
    }
    /// What the startup migrations changed.
    pub fn migration_result(&self) -> &StartupMigrationResult {
        &self.inner.migrations
    }
    /// The llama.cpp backend (`/llama` panel, model discovery).
    pub fn llama(&self) -> &Arc<LlamaProviderBackend> {
        &self.inner.llama
    }
    /// The project index manager (`/index` panel).
    pub fn index(&self) -> &Arc<IndexManager> {
        &self.inner.index
    }
    /// The index service of the workspace root.
    pub fn index_service(&self) -> &Arc<BackgroundService> {
        &self.inner.index_service
    }
    /// Code snapshots for memory code recall.
    pub fn code_snapshots(&self) -> Arc<dyn CodeSnapshotProvider> {
        self.inner.code.clone()
    }
    /// The MCP manager, when servers are configured (`/mcp` panel).
    pub fn mcp(&self) -> Option<&Arc<McpManager>> {
        self.inner.mcp.as_ref()
    }
    /// Governed memory, when enabled (`/memory` panel).
    pub fn memory(&self) -> Option<&Arc<MemorySubsystem>> {
        self.inner.memory.as_ref()
    }
    /// The resolved `memory.*` settings (also when memory is disabled).
    pub fn memory_settings(&self) -> &MemorySettings {
        &self.inner.memory_settings
    }
    /// Loaded extensions (`/extensions`, commands, UI).
    pub fn extensions(&self) -> &Extensions {
        &self.inner.extensions
    }
    /// Subagent tool and persistent workers.
    pub fn subagents(&self) -> &SubagentSubsystem {
        &self.inner.subagents
    }
    /// Startup diagnostics (extensions, settings, llama, memory, MCP).
    pub fn diagnostics(&self) -> Vec<String> {
        self.inner
            .diagnostics
            .lock()
            .expect("diagnostics lock")
            .clone()
    }
    /// Arguments left over after extension flags were resolved (a value the
    /// CLI attached to a boolean extension flag): further messages.
    pub fn extra_messages(&self) -> &[String] {
        &self.inner.extra_messages
    }

    /// The model chosen by `--provider` / `--model` / `--models`, with the
    /// thinking level from a `:level` suffix or the scope.
    pub fn selected_model(&self) -> Option<(ModelRef, Option<ThinkingLevel>)> {
        self.inner.selected.clone()
    }
    /// The `--models` scope (Ctrl+P cycling in the archived TUI).
    pub fn model_scope(&self) -> &[ScopedModel] {
        &self.inner.scope
    }
    /// The conversation bound by [`Self::attach`].
    pub fn active_conversation(&self) -> Option<Conversation> {
        self.inner.active.lock().expect("active lock").clone()
    }

    /// Present extension `ctx.ui.*` calls with `ui` (headless by default).
    pub fn set_extension_ui(&self, ui: Arc<dyn ExtensionUi>) {
        self.inner.extensions.set_ui(ui);
    }

    // ----- conversations -----------------------------------------------------

    /// The model a new conversation starts with: the selected model, else
    /// the settings default.
    pub fn startup_model(&self) -> Option<ModelRef> {
        self.inner
            .selected
            .as_ref()
            .map(|(m, _)| m.clone())
            .or_else(|| self.inner.engine.default_model())
    }

    /// Apply `--model` / `--thinking` to a conversation: the selected model
    /// replaces the session's, and the thinking level (flag, then `:level`
    /// suffix) is clamped to the model.
    pub fn apply_startup_selection(&self, conversation: &Conversation) {
        if let Some((model, _)) = &self.inner.selected {
            conversation.select_model(model.clone(), "restore");
        }
        let level = self
            .inner
            .options
            .thinking
            .or_else(|| self.inner.selected.as_ref().and_then(|(_, t)| *t));
        if let Some(level) = level {
            let clamped = conversation
                .model()
                .and_then(|m| {
                    self.inner
                        .engine
                        .provider(&m.provider)
                        .map(|p| p.model_spec(&m.id).clamp_thinking(level))
                })
                .unwrap_or(level);
            conversation.set_thinking(clamped);
        }
        *self.inner.current_model.lock().expect("model lock") = conversation.model();
    }

    /// Bind `conversation` as the active one: the extension host, the
    /// memory session and the subagent coordinator follow it.
    pub fn attach(&self, conversation: &Conversation, options: AttachOptions) {
        *self.inner.active.lock().expect("active lock") = Some(conversation.clone());
        *self.inner.current_model.lock().expect("model lock") = conversation.model();
        let mut host = ConversationHost::new(conversation.clone());
        if let Some(sink) = options.run_sink.clone() {
            host = host.with_run_sink(sink);
        }
        self.inner.extensions.set_host(Some(Arc::new(host)));
        if let Some(memory) = &self.inner.memory {
            memory.attach(self.inner.engine.hooks(), &conversation.session_id());
        }
        let delivery = if options.deliver_subagent_events {
            Some(options.run_sink.unwrap_or_else(|| Arc::new(|_rx| {})))
        } else {
            None
        };
        self.inner
            .subagents
            .bind(&self.inner.engine, conversation, delivery);
    }

    /// Start the interactive background services: the Dreaming worker, the
    /// periodic model refresh and MCP connections (idempotent).
    pub fn start_background_services(&self) {
        if self.inner.background.swap(true, Ordering::SeqCst) {
            return;
        }
        if let Some(memory) = &self.inner.memory {
            memory.start_background();
        }
        if !self.inner.engine.is_offline() {
            *self.inner.refresh.lock().expect("refresh lock") = Some(
                self.inner
                    .engine
                    .start_model_refresh(MODEL_REFRESH_INTERVAL),
            );
        }
        let runtime = self.clone();
        self.inner
            .engine
            .spawn(async move { runtime.connect_mcp_async().await });
    }

    /// Listings of the periodic model refresh (interactive).
    pub fn model_refresh(&self) -> Option<async_channel::Receiver<qt_core::engine::ModelListing>> {
        self.inner
            .refresh
            .lock()
            .expect("refresh lock")
            .as_ref()
            .map(|r| r.listings.clone())
    }

    async fn connect_mcp_async(&self) {
        let Some(manager) = self.inner.mcp.clone() else {
            return;
        };
        if self.inner.mcp_connected.swap(true, Ordering::SeqCst) {
            return;
        }
        let notes = manager.connect_all().await;
        self.inner
            .diagnostics
            .lock()
            .expect("diagnostics lock")
            .extend(notes);
    }

    /// Connect every MCP server and wait (print / JSON modes).
    pub fn connect_mcp(&self) {
        let runtime = self.clone();
        self.inner
            .engine
            .block_on(async move { runtime.connect_mcp_async().await });
    }

    /// Run an extension slash command (`SlashAction::Extension`). Commands
    /// may open dialogs, so GUI front ends call this off the UI thread.
    pub fn run_extension_command(&self, name: &str, arguments: &str) -> Result<Vec<String>> {
        self.inner
            .extensions
            .run_command(name, arguments)
            .map(|o| o.lines)
            .map_err(|e| anyhow!(e))
    }

    /// Reload extensions and resources (`/reload`).
    pub fn reload(&self) -> Vec<String> {
        let mut lines = Vec::new();
        let result = self.inner.extensions.reload();
        lines.extend(result.diagnostics.iter().cloned());
        self.inner
            .engine
            .set_extension_commands(extension_slash_commands(&self.inner.extensions));
        self.inner
            .engine
            .set_extensions_info(extension_infos(&self.inner.extensions));
        lines.extend(self.inner.engine.reload_resources());
        lines
    }

    /// Stop the active conversation and every subsystem, waiting at most
    /// `capture_timeout` for memory capture to finish.
    pub fn shutdown(&self, capture_timeout: Duration) {
        if let Some(conversation) = self.active_conversation() {
            conversation.shutdown();
        }
        let runtime = self.clone();
        self.inner.engine.block_on(async move {
            runtime.inner.subagents.shutdown().await;
            if let Some(mcp) = &runtime.inner.mcp {
                mcp.shutdown().await;
            }
        });
        *self.inner.refresh.lock().expect("refresh lock") = None;
        if let Some(memory) = &self.inner.memory {
            memory.shutdown(capture_timeout);
        }
        self.inner.extensions.uninstall_hooks();
        self.inner.index.stop_all();
    }

    /// Tools that would be offered to a new conversation, by name.
    pub fn tool_names(&self) -> Vec<String> {
        self.inner
            .engine
            .tools()
            .iter()
            .map(|t: &Arc<dyn Tool>| t.name().to_string())
            .collect()
    }
}
