//! `/llama` (archived `interactive_mode.cpp` llama handler and
//! `llama_slash_commands.cpp`): status, models, selection, load/unload,
//! policy, grammar, KV cache, GPU settings, embeddings, prompt, tagged runs
//! and GGUF discovery, shared by the desktop `/llama` panel and the slash
//! command. Every function here may block on the llama worker thread, so
//! front ends call them off their UI thread.

use std::path::PathBuf;

use qt_core::message::Message;
use qt_core::provider::{ModelRef, Request};
use qt_llama::discovery::default_roots;
use qt_llama::provider::make_generate_request;
use qt_llama::{
    DiscoveredModel, EmbeddingRequest, GenerateOptions, InstanceTag, KvCacheOperation,
    KvCacheState, LlamaRuntime, LlamaSettings, PROVIDER_ID, RuntimeStatus,
};
use serde_json::{Map, Value, json};

use crate::runtime::Runtime;

/// The archived subcommand list: `(name, argument hint, description)`.
pub const LLAMA_SUBCOMMANDS: &[(&str, &str, &str)] = &[
    (
        "status",
        "",
        "Show llama runtime, model, KV cache, GPU, grammar, and embedding status.",
    ),
    (
        "models",
        "",
        "List configured local llama models and mark the selected model.",
    ),
    (
        "select",
        "<model-id>",
        "Select the local llama model used by llama commands.",
    ),
    (
        "load",
        "[model-id]",
        "Load the selected or requested llama model now.",
    ),
    ("unload", "", "Release llama model/context resources."),
    (
        "reload",
        "",
        "Reload llama settings, catalog entries, and runtime state.",
    ),
    (
        "policy",
        "[temperature=<n>] [topP=<n>]",
        "Show or update llama generation policy overrides.",
    ),
    (
        "grammar",
        "<show|list|default|off> ...",
        "Show, list, disable, or select the default grammar.",
    ),
    (
        "kv",
        "<show|clear|keep|drop|shift> ...",
        "Inspect or mutate the llama KV cache.",
    ),
    (
        "gpu",
        "[gpuLayers=<n>] [mainGpu=<n>] [splitMode=<mode>]",
        "Show or update llama GPU/backend offload settings.",
    ),
    (
        "embeddings",
        "<text>",
        "Generate a llama embedding for text when supported by the model.",
    ),
    (
        "prompt",
        "<text>",
        "Run text through the selected llama model.",
    ),
    (
        "run",
        "<embed|rank|instruct|chat> <text>",
        "Run text through a tagged llama instance.",
    ),
    (
        "discover",
        "[dirs]",
        "Discover local GGUF models and add them to settings.",
    ),
];

/// Instance tags in the archived status order.
pub const INSTANCE_TAGS: [InstanceTag; 4] = [
    InstanceTag::Embed,
    InstanceTag::Rank,
    InstanceTag::Instruct,
    InstanceTag::Chat,
];

fn split_first(text: &str) -> (&str, &str) {
    let text = text.trim();
    match text.find(char::is_whitespace) {
        Some(i) => (&text[..i], text[i..].trim()),
        None => (text, ""),
    }
}

/// `1.50 GiB` style sizes (archived `format_bytes`).
pub fn format_bytes(bytes: u64) -> String {
    const UNITS: [&str; 5] = ["B", "KiB", "MiB", "GiB", "TiB"];
    let mut value = bytes as f64;
    let mut unit = 0;
    while value >= 1024.0 && unit + 1 < UNITS.len() {
        value /= 1024.0;
        unit += 1;
    }
    if unit == 0 {
        format!("{bytes} B")
    } else {
        format!("{value:.2} {}", UNITS[unit])
    }
}

fn opt<T: ToString>(value: &Option<T>, fallback: &str) -> String {
    value
        .as_ref()
        .map(ToString::to_string)
        .unwrap_or_else(|| fallback.to_string())
}

fn json_bytes(value: &Value, key: &str) -> u64 {
    value.get(key).and_then(Value::as_u64).unwrap_or(0)
}

/// The `kv_state:` line.
pub fn kv_state_line(state: &KvCacheState) -> String {
    format!(
        "kv_state: seq={}, min={}, max={}, position={}, canShift={}",
        state.seq_id,
        state.position_min,
        state.position_max,
        state.sequence_position,
        state.can_shift
    )
}

/// The selected llama model: `llama.selectedModel`, else the conversation
/// model when it is a llama model.
pub fn selected_model(settings: &LlamaSettings, current: Option<&ModelRef>) -> Option<String> {
    settings
        .selected_model
        .clone()
        .filter(|s| !s.is_empty())
        .or_else(|| {
            current
                .filter(|m| m.provider == PROVIDER_ID)
                .map(|m| m.id.clone())
        })
}

/// What `/llama status` needs from the runtime (absent until it started).
#[derive(Debug, Clone, Default)]
pub struct RuntimeSnapshot {
    pub loaded: Option<String>,
    pub kv: Option<KvCacheState>,
    pub status: Option<RuntimeStatus>,
    pub instances: Vec<(InstanceTag, RuntimeStatus)>,
}

impl RuntimeSnapshot {
    /// Query `runtime` (blocking on its worker).
    pub fn capture(runtime: &LlamaRuntime) -> Self {
        Self {
            loaded: runtime.loaded_model_id(InstanceTag::Chat).ok().flatten(),
            kv: runtime
                .kv_cache_control(KvCacheOperation::Inspect { seq_id: 0 }, InstanceTag::Chat)
                .ok(),
            status: runtime.runtime_status(InstanceTag::Chat).ok(),
            instances: INSTANCE_TAGS
                .iter()
                .filter_map(|tag| runtime.runtime_status(*tag).ok().map(|s| (*tag, s)))
                .collect(),
        }
    }
}

/// `/llama status` lines (archived layout).
pub fn status_lines(
    settings: &LlamaSettings,
    selected: Option<&str>,
    current: Option<&ModelRef>,
    runtime: Option<&RuntimeSnapshot>,
) -> Vec<String> {
    let generation = selected
        .and_then(|id| settings.model(id))
        .map(|m| settings.generation.merged(&m.generation))
        .unwrap_or_else(|| settings.generation.clone());
    let loaded = match runtime {
        None => "unavailable until llama provider runtime is initialized".to_string(),
        Some(r) => r.loaded.clone().unwrap_or_else(|| "(not loaded)".into()),
    };
    let mut lines = vec![
        format!(
            "llama provider: {}",
            if settings.models.is_empty() {
                "no configured models"
            } else {
                "configured"
            }
        ),
        format!(
            "native: {}",
            if qt_llama::native_available() {
                let backends = qt_llama::compiled_backends();
                if backends.is_empty() {
                    "llama.cpp (CPU)".to_string()
                } else {
                    format!("llama.cpp ({})", backends.join(", "))
                }
            } else {
                "llama.cpp is not built into this binary".to_string()
            }
        ),
        format!(
            "selected: {}",
            selected
                .map(|id| format!("{PROVIDER_ID}/{id}"))
                .unwrap_or_else(|| "(not selected)".into())
        ),
        format!(
            "top_level_model: {}",
            current
                .map(|m| format!("{}/{}", m.provider, m.id))
                .unwrap_or_else(|| "(none)".into())
        ),
        format!("loaded: {loaded}"),
        format!(
            "context: {}",
            opt(&settings.context.context_size, "llama.cpp default")
        ),
        format!(
            "policy: temperature={}, topK={}, topP={}, minP={}",
            opt(&generation.temperature, "default"),
            opt(&generation.top_k, "default"),
            opt(&generation.top_p, "default"),
            opt(&generation.min_p, "default")
        ),
        format!(
            "kv_cache: {}",
            settings
                .kv_cache
                .policy
                .clone()
                .unwrap_or_else(|| "session".into())
        ),
        format!(
            "gpu: gpuLayers={}, mainGpu={}, splitMode={}",
            opt(&settings.backend.gpu_layers, "default"),
            opt(&settings.backend.main_gpu, "default"),
            opt(&settings.backend.split_mode, "default")
        ),
        format!(
            "grammar: root={}, default={}",
            settings
                .grammar
                .root
                .clone()
                .unwrap_or_else(|| "root".into()),
            settings
                .grammar
                .default_grammar
                .clone()
                .unwrap_or_else(|| "(none)".into())
        ),
        format!(
            "embeddings: enabled={}, pooling={}",
            opt(&settings.embeddings.enabled, "false"),
            opt(&settings.embeddings.pooling, "default")
        ),
    ];
    let Some(runtime) = runtime else {
        return lines;
    };
    if let Some(kv) = &runtime.kv {
        lines.push(kv_state_line(kv));
    }
    if let Some(status) = &runtime.status {
        lines.push(format!(
            "runtime: modelLoaded={}, contextLoaded={}, contextSize={}, sequencePosition={}, embeddings={}",
            status.model_loaded,
            status.context_loaded,
            status.context_size,
            status.sequence_position,
            status.embeddings_enabled
        ));
        let host = status.memory.get("host").cloned().unwrap_or(Value::Null);
        lines.push(format!(
            "memory: model={}, state={}, host={}",
            format_bytes(status.model_size_bytes),
            format_bytes(status.state_size_bytes),
            format_bytes(json_bytes(&host, "allocatedBytes"))
        ));
        lines.push(format!(
            "backend: gpuOffloadSupported={}, maxDevices={}",
            status.gpu_offload_supported, status.max_devices
        ));
        lines.extend(device_lines(status));
        if let Some(offload) = status.metadata.get("offload") {
            lines.push(format!("offload: {offload}"));
        }
    }
    lines.push("instances:".into());
    for (tag, status) in &runtime.instances {
        lines.push(format!(
            "- {}: {}, contextLoaded={}, embeddings={}, sequencePosition={}",
            tag.as_str(),
            status
                .loaded_model_id
                .clone()
                .unwrap_or_else(|| "(not loaded)".into()),
            status.context_loaded,
            status.embeddings_enabled,
            status.sequence_position
        ));
    }
    lines
}

/// `gpu_memory:` / `device_memory:` lines of a status.
pub fn device_lines(status: &RuntimeStatus) -> Vec<String> {
    let devices = status
        .memory
        .get("devices")
        .and_then(Value::as_array)
        .cloned()
        .unwrap_or_default();
    if devices.is_empty() {
        return vec!["gpu_memory: no accelerator device allocations reported".into()];
    }
    devices
        .iter()
        .map(|d| {
            let text = |k: &str, f: &str| d.get(k).and_then(Value::as_str).unwrap_or(f).to_string();
            format!(
                "{}{} type={} total={}, free={}, used={}, llama={} (model={}, context={}, compute={})",
                if d.get("gpuMemory").and_then(Value::as_bool).unwrap_or(false) {
                    "gpu_memory: "
                } else {
                    "device_memory: "
                },
                text("name", "device"),
                text("type", "unknown"),
                format_bytes(json_bytes(d, "totalBytes")),
                format_bytes(json_bytes(d, "freeBytes")),
                format_bytes(json_bytes(d, "usedBytes")),
                format_bytes(json_bytes(d, "allocatedBytes")),
                format_bytes(json_bytes(d, "modelBytes")),
                format_bytes(json_bytes(d, "contextBytes")),
                format_bytes(json_bytes(d, "computeBytes"))
            )
        })
        .collect()
}

/// `/llama models` lines.
pub fn model_lines(settings: &LlamaSettings, selected: Option<&str>) -> Vec<String> {
    if settings.models.is_empty() {
        return vec!["No llama models are configured.".into()];
    }
    settings
        .models
        .iter()
        .map(|m| {
            format!(
                "{} {} - {} ({})",
                if selected == Some(m.id.as_str()) {
                    "*"
                } else {
                    "-"
                },
                m.id,
                if m.name.is_empty() { &m.id } else { &m.name },
                m.path
            )
        })
        .collect()
}

/// Lines describing discovered models.
pub fn discovery_lines(models: &[DiscoveredModel], notes: &[String]) -> Vec<String> {
    let mut lines = vec![format!("Discovered {} local model(s).", models.len())];
    for m in models {
        let mut line = format!(
            "  llama/{} [{}] {} ({})",
            m.id,
            m.capability.as_str(),
            m.quantization,
            m.path
        );
        if !m.best_usage.is_empty() {
            line.push_str(&format!(" - {}", m.best_usage));
        }
        lines.push(line);
    }
    lines.extend(notes.iter().cloned());
    lines
}

/// Parse `/llama kv ...` (archived usage messages).
pub fn parse_kv(arguments: &str) -> Result<(String, KvCacheOperation), String> {
    let (verb, rest) = split_first(arguments);
    let verb = if verb.is_empty() {
        "status".to_string()
    } else {
        verb.to_ascii_lowercase()
    };
    let nums: Vec<&str> = rest.split_whitespace().collect();
    let int = |s: &str| s.parse::<i32>().ok();
    let op = match verb.as_str() {
        "status" | "show" => KvCacheOperation::Inspect { seq_id: 0 },
        "clear" => KvCacheOperation::Clear,
        "keep" => KvCacheOperation::Keep {
            seq_id: nums
                .first()
                .and_then(|s| int(s))
                .filter(|_| nums.len() == 1)
                .ok_or("Usage: /llama kv keep <seq-id>")?,
        },
        "drop" => {
            let usage = "Usage: /llama kv drop <seq-id> [from] [to]";
            if nums.is_empty() || nums.len() > 3 {
                return Err(usage.into());
            }
            let seq_id = int(nums[0]).ok_or(usage)?;
            let from = nums.get(1).map(|s| int(s).ok_or(usage)).transpose()?;
            let to = nums.get(2).map(|s| int(s).ok_or(usage)).transpose()?;
            KvCacheOperation::Drop {
                seq_id,
                from: from.unwrap_or(-1),
                to: to.unwrap_or(-1),
            }
        }
        "shift" => {
            let usage = "Usage: /llama kv shift <seq-id> <from> <to> <delta>";
            let values: Vec<i32> = nums.iter().filter_map(|s| int(s)).collect();
            if nums.len() != 4 || values.len() != 4 {
                return Err(usage.into());
            }
            KvCacheOperation::Shift {
                seq_id: values[0],
                from: values[1],
                to: values[2],
                delta: values[3],
            }
        }
        other => return Err(format!("Unsupported llama KV command: {other}")),
    };
    let verb = if verb == "show" {
        "status".into()
    } else {
        verb
    };
    Ok((verb, op))
}

/// Lines after a KV operation.
pub fn kv_lines(verb: &str, state: &KvCacheState) -> Vec<String> {
    vec![
        if verb == "status" {
            "Llama KV cache status.".to_string()
        } else {
            format!("Llama KV cache updated: {verb}")
        },
        kv_state_line(state),
    ]
}

fn parse_bool(value: &str) -> Option<bool> {
    match value.to_ascii_lowercase().as_str() {
        "1" | "true" | "yes" | "on" => Some(true),
        "0" | "false" | "no" | "off" => Some(false),
        _ => None,
    }
}

/// Parse `key=value` GPU settings into `settings.llama.backend` keys.
pub fn parse_gpu(arguments: &str) -> Result<Map<String, Value>, String> {
    let usage = "Usage: /llama gpu [gpuLayers=<n>] [mainGpu=<n>] [splitMode=<none|layer|row>] [offloadKqv=<bool>]";
    let mut out = Map::new();
    for token in arguments.split_whitespace() {
        let (key, value) = token
            .split_once('=')
            .filter(|(k, v)| !k.is_empty() && !v.is_empty())
            .ok_or(usage)?;
        let parsed = match key {
            "gpuLayers" => json!(value.parse::<i32>().map_err(|_| usage)?),
            "mainGpu" => json!(value.parse::<i32>().ok().filter(|v| *v >= 0).ok_or(usage)?),
            "splitMode" if matches!(value, "none" | "layer" | "row") => json!(value),
            "offloadKqv" => json!(parse_bool(value).ok_or(usage)?),
            _ => return Err(usage.into()),
        };
        out.insert(key.to_string(), parsed);
    }
    if out.is_empty() {
        return Err(usage.into());
    }
    Ok(out)
}

/// Parse `key=value` generation policy settings into `settings.llama.generation` keys.
pub fn parse_policy(arguments: &str) -> Result<Map<String, Value>, String> {
    let usage = "Usage: /llama policy [temperature=<n>] [topK=<n>] [topP=<n>] [minP=<n>] [seed=<n>] [maxOutputTokens=<n>]";
    let mut out = Map::new();
    for token in arguments.split_whitespace() {
        let (key, value) = token.split_once('=').ok_or(usage)?;
        let parsed = match key {
            "temperature" | "topP" | "minP" | "repeatPenalty" => {
                json!(value.parse::<f64>().map_err(|_| usage)?)
            }
            "topK" | "seed" | "maxOutputTokens" => {
                json!(value.parse::<i64>().map_err(|_| usage)?)
            }
            _ => return Err(usage.into()),
        };
        out.insert(key.to_string(), parsed);
    }
    if out.is_empty() {
        return Err(usage.into());
    }
    Ok(out)
}

/// Archived embedding summary lines.
pub fn embedding_lines(header: &[&str], embedding: &[f32], input_tokens: u64) -> Vec<String> {
    let preview: Vec<String> = embedding.iter().take(3).map(|v| v.to_string()).collect();
    let mut lines: Vec<String> = header.iter().map(|s| s.to_string()).collect();
    lines.push(format!("dimensions: {}", embedding.len()));
    lines.push(format!("input_tokens: {input_tokens}"));
    lines.push(format!(
        "preview: [{}{}",
        preview.join(", "),
        if embedding.len() > 3 { ", ...]" } else { "]" }
    ));
    lines
}

fn set_llama_section(settings: &mut qt_core::Settings, section: &str, values: &Map<String, Value>) {
    let raw = settings.raw_mut();
    let llama = raw
        .entry("llama")
        .or_insert_with(|| Value::Object(Map::new()));
    if !llama.is_object() {
        *llama = Value::Object(Map::new());
    }
    let target = llama
        .as_object_mut()
        .expect("llama object")
        .entry(section)
        .or_insert_with(|| Value::Object(Map::new()));
    if !target.is_object() {
        *target = Value::Object(Map::new());
    }
    let object = target.as_object_mut().expect("section object");
    for (k, v) in values {
        if v.is_null() {
            object.remove(k);
        } else {
            object.insert(k.clone(), v.clone());
        }
    }
}

impl Runtime {
    /// Effective llama settings for the engine's current settings.
    pub fn llama_settings(&self) -> LlamaSettings {
        self.llama().effective_settings(&self.engine().settings())
    }

    /// The selected llama model id.
    pub fn llama_selected_model(&self) -> Option<String> {
        let current = self.active_conversation().and_then(|c| c.model());
        selected_model(&self.llama_settings(), current.as_ref())
    }

    /// Persist `llama.selectedModel` (archived `/llama select`).
    pub fn select_llama_model(&self, id: &str) -> Result<String, String> {
        let settings = self.llama_settings();
        let model = settings
            .models
            .iter()
            .find(|m| m.id == id || m.id.eq_ignore_ascii_case(id))
            .ok_or_else(|| format!("llama model not found: {id}"))?;
        let chosen = model.id.clone();
        self.engine()
            .update_settings(|s| {
                let mut values = Map::new();
                values.insert("selectedModel".into(), json!(chosen));
                let raw = s.raw_mut();
                let llama = raw
                    .entry("llama")
                    .or_insert_with(|| Value::Object(Map::new()));
                if let Some(o) = llama.as_object_mut() {
                    o.extend(values);
                } else {
                    *llama = json!({"selectedModel": chosen});
                }
            })
            .map_err(|e| format!("{e:#}"))?;
        Ok(format!("Llama model set to llama/{}", model.id))
    }

    fn require_selection(&self, unavailable: &str) -> Result<String, Vec<String>> {
        self.llama_selected_model()
            .ok_or_else(|| vec![unavailable.to_string()])
    }

    /// Discover GGUF models under `dirs` (default roots when empty), persist
    /// and register them.
    pub fn llama_discover(&self, dirs: &[PathBuf]) -> Vec<String> {
        let engine = self.engine();
        let roots: Vec<PathBuf> = if dirs.is_empty() {
            default_roots(engine.cwd(), self.agent_home().root(), &[])
        } else {
            dirs.iter()
                .map(|p| {
                    if p.is_absolute() {
                        p.clone()
                    } else {
                        engine.cwd().join(p)
                    }
                })
                .collect()
        };
        if roots.is_empty() {
            return vec![
                "No discovery directories found (<project>/models, <agent home>/models).".into(),
                "Pass directories to scan, for example /llama discover ~/models.".into(),
            ];
        }
        let (found, notes) =
            self.llama()
                .discover_and_register(&engine.settings(), engine.cwd(), &roots);
        discovery_lines(&found.models, &notes)
    }

    /// `/llama <arguments>` as transcript lines. Blocks on the llama worker.
    pub fn llama_command(&self, arguments: &str) -> Vec<String> {
        match self.llama_command_inner(arguments) {
            Ok(lines) | Err(lines) => lines,
        }
    }

    fn llama_command_inner(&self, arguments: &str) -> Result<Vec<String>, Vec<String>> {
        let (first, rest) = split_first(arguments);
        let sub = if first.is_empty() {
            "status".to_string()
        } else {
            first.to_ascii_lowercase()
        };
        let engine = self.engine();
        let settings = self.llama_settings();
        let current = self.active_conversation().and_then(|c| c.model());
        let selected = selected_model(&settings, current.as_ref());
        let runtime = || self.llama().runtime(&engine.settings());
        let fail = |e: String| vec![e];
        match sub.as_str() {
            "status" => {
                let snapshot = self
                    .llama()
                    .current_runtime()
                    .map(|r| RuntimeSnapshot::capture(&r));
                let mut lines = status_lines(
                    &settings,
                    selected.as_deref(),
                    current.as_ref(),
                    snapshot.as_ref(),
                );
                lines.extend(self.llama().diagnostics());
                Ok(lines)
            }
            "models" => Ok(model_lines(&settings, selected.as_deref())),
            "select" => {
                if rest.is_empty() {
                    return Err(fail("Usage: /llama select <model-id>".into()));
                }
                self.select_llama_model(rest).map(|l| vec![l]).map_err(fail)
            }
            "load" => {
                let id = if rest.is_empty() {
                    self.require_selection(
                        "Llama load is unavailable until a llama model is selected.",
                    )?
                } else {
                    rest.to_string()
                };
                runtime()
                    .load_model(&id, InstanceTag::Chat)
                    .map(|_| vec![format!("Loaded llama/{id}.")])
                    .map_err(|e| fail(format!("Llama load failed: {e}")))
            }
            "unload" => match self.llama().current_runtime() {
                None => Ok(vec!["No llama runtime is running.".into()]),
                Some(r) => r
                    .unload()
                    .map(|_| vec!["Released llama model and context resources.".into()])
                    .map_err(|e| fail(format!("Llama unload failed: {e}"))),
            },
            "reload" => {
                engine.reload_settings();
                let reloaded = self.llama_settings();
                let mut lines = vec![format!(
                    "Reloaded llama settings: {} model(s).",
                    reloaded.models.len()
                )];
                lines.extend(reloaded.validate());
                lines.extend(self.llama().diagnostics());
                Ok(lines)
            }
            "policy" => {
                if rest.is_empty() || rest.eq_ignore_ascii_case("show") {
                    let g = selected
                        .as_deref()
                        .and_then(|id| settings.model(id))
                        .map(|m| settings.generation.merged(&m.generation))
                        .unwrap_or_else(|| settings.generation.clone());
                    return Ok(vec![
                        format!(
                            "policy: temperature={}, topK={}, topP={}, minP={}",
                            opt(&g.temperature, "default"),
                            opt(&g.top_k, "default"),
                            opt(&g.top_p, "default"),
                            opt(&g.min_p, "default")
                        ),
                        format!(
                            "seed={}, maxOutputTokens={}, repeatPenalty={}",
                            opt(&g.seed, "default"),
                            opt(&g.max_output_tokens, "default"),
                            opt(&g.repeat_penalty, "default")
                        ),
                    ]);
                }
                let values = parse_policy(rest).map_err(fail)?;
                engine
                    .update_settings(|s| set_llama_section(s, "generation", &values))
                    .map_err(|e| fail(format!("{e:#}")))?;
                Ok(vec![format!(
                    "Llama generation policy updated: {}",
                    Value::Object(values)
                )])
            }
            "grammar" => {
                let (verb, value) = split_first(rest);
                let root = settings
                    .grammar
                    .root
                    .clone()
                    .unwrap_or_else(|| "root".into());
                match verb.to_ascii_lowercase().as_str() {
                    "" | "status" | "show" => Ok(vec![format!(
                        "grammar: root={root}, default={}",
                        settings
                            .grammar
                            .default_grammar
                            .clone()
                            .unwrap_or_else(|| "(none)".into())
                    )]),
                    "list" => {
                        let files = settings.grammar.files.clone().unwrap_or_default();
                        if files.is_empty() {
                            return Ok(vec!["No llama grammar files are configured.".into()]);
                        }
                        Ok(files
                            .iter()
                            .map(|(name, path)| format!("- {name}: {path}"))
                            .collect())
                    }
                    "off" => {
                        let mut values = Map::new();
                        values.insert("default".into(), Value::Null);
                        engine
                            .update_settings(|s| set_llama_section(s, "grammar", &values))
                            .map_err(|e| fail(format!("{e:#}")))?;
                        Ok(vec![
                            "Llama grammar constraints disabled for subsequent turns.".into(),
                        ])
                    }
                    "default" | "use" => {
                        if value.is_empty() {
                            return Err(fail("Usage: /llama grammar default <name>".into()));
                        }
                        let files = settings.grammar.files.clone().unwrap_or_default();
                        if !files.contains_key(value) {
                            return Err(fail(format!("llama grammar not found: {value}")));
                        }
                        let mut values = Map::new();
                        values.insert("default".into(), json!(value));
                        engine
                            .update_settings(|s| set_llama_section(s, "grammar", &values))
                            .map_err(|e| fail(format!("{e:#}")))?;
                        Ok(vec![
                            format!("Llama grammar {value} configured for subsequent turns."),
                            format!("root: {root}"),
                        ])
                    }
                    other => Err(fail(format!(
                        "Unsupported llama grammar command: {other}. Usage: /llama grammar [show|list|default <name>|off]"
                    ))),
                }
            }
            "kv" => {
                let Some(r) = self.llama().current_runtime() else {
                    return Ok(vec![
                        "Llama KV cache is unavailable until the runtime starts with a llama model."
                            .into(),
                    ]);
                };
                let (verb, op) = parse_kv(rest).map_err(fail)?;
                r.kv_cache_control(op, InstanceTag::Chat)
                    .map(|state| kv_lines(&verb, &state))
                    .map_err(|e| fail(format!("Llama KV cache failed: {e}")))
            }
            "gpu" => {
                if rest.is_empty() || rest.eq_ignore_ascii_case("status") {
                    let b = &settings.backend;
                    let mut lines = vec![format!(
                        "gpu_applied: gpuLayers={}, mainGpu={}, splitMode={}, offloadKqv={}",
                        opt(&b.gpu_layers, "default"),
                        opt(&b.main_gpu, "default"),
                        opt(&b.split_mode, "default"),
                        opt(&b.offload_kqv, "default")
                    )];
                    let backends = qt_llama::compiled_backends();
                    lines.push(format!(
                        "compiled_backends: {}",
                        if backends.is_empty() {
                            "cpu".to_string()
                        } else {
                            backends.join(", ")
                        }
                    ));
                    if let Some(r) = self.llama().current_runtime()
                        && let Ok(status) = r.runtime_status(InstanceTag::Chat)
                    {
                        lines.extend(device_lines(&status));
                        if let Some(offload) = status.metadata.get("offload") {
                            lines.push(format!("offload: {offload}"));
                        }
                    }
                    return Ok(lines);
                }
                let values = parse_gpu(rest).map_err(fail)?;
                engine
                    .update_settings(|s| set_llama_section(s, "backend", &values))
                    .map_err(|e| fail(format!("{e:#}")))?;
                Ok(vec![
                    format!("Llama GPU settings saved: {}", Value::Object(values)),
                    "The llama runtime restarts with them on next use.".into(),
                ])
            }
            "embeddings" => {
                let id = self.require_selection(
                    "Llama embeddings are unavailable until a llama model is selected and loaded.",
                )?;
                if rest.is_empty() {
                    return Err(fail("Usage: /llama embeddings <text>".into()));
                }
                let request = EmbeddingRequest {
                    queue_timeout: std::time::Duration::from_secs(120),
                    ..EmbeddingRequest::new(id, rest)
                };
                runtime()
                    .embed(request, InstanceTag::Embed)
                    .map(|e| {
                        embedding_lines(
                            &["Llama embedding generated."],
                            &e.embedding,
                            e.input_tokens,
                        )
                    })
                    .map_err(|e| fail(format!("Llama embedding failed: {e}")))
            }
            "prompt" => {
                let id = self.require_selection(
                    "Llama prompt is unavailable until a llama model is selected.",
                )?;
                if rest.is_empty() {
                    return Err(fail("Usage: /llama prompt <text>".into()));
                }
                let r = runtime();
                let _ = r.kv_cache_control(KvCacheOperation::Clear, InstanceTag::Chat);
                self.llama_generate(&r, &settings, &id, rest, InstanceTag::Chat)
                    .map(|text| vec![text])
                    .map_err(|e| fail(format!("Llama prompt failed: {e}")))
            }
            "run" => {
                let id = self.require_selection(
                    "Llama run is unavailable until a llama model is selected.",
                )?;
                let usage = "Usage: /llama run <embed|rank|instruct|chat> <text>";
                let (tag, text) = split_first(rest);
                let tag = InstanceTag::parse(&tag.to_ascii_lowercase())
                    .ok_or_else(|| fail(usage.into()))?;
                if text.is_empty() {
                    return Err(fail(usage.into()));
                }
                let r = runtime();
                if tag == InstanceTag::Embed {
                    let request = EmbeddingRequest {
                        queue_timeout: std::time::Duration::from_secs(120),
                        ..EmbeddingRequest::new(id, text)
                    };
                    return r
                        .embed(request, tag)
                        .map(|e| {
                            embedding_lines(
                                &["Llama embed run complete.", "instance: embed"],
                                &e.embedding,
                                e.input_tokens,
                            )
                        })
                        .map_err(|e| fail(format!("Llama embed run failed: {e}")));
                }
                let _ = r.kv_cache_control(KvCacheOperation::Clear, tag);
                self.llama_generate(&r, &settings, &id, text, tag)
                    .map(|out| vec![format!("instance: {}", tag.as_str()), out])
                    .map_err(|e| fail(format!("Llama {} run failed: {e}", tag.as_str())))
            }
            "discover" => {
                let dirs: Vec<PathBuf> = rest
                    .split(|c: char| c == ',' || c.is_whitespace())
                    .filter(|w| !w.is_empty())
                    .map(PathBuf::from)
                    .collect();
                Ok(self.llama_discover(&dirs))
            }
            other => Err(vec![
                format!("unknown llama command: {other}"),
                format!(
                    "usage: /llama [{}]",
                    LLAMA_SUBCOMMANDS
                        .iter()
                        .map(|(n, _, _)| *n)
                        .collect::<Vec<_>>()
                        .join("|")
                ),
            ]),
        }
    }

    fn llama_generate(
        &self,
        runtime: &LlamaRuntime,
        settings: &LlamaSettings,
        model: &str,
        text: &str,
        tag: InstanceTag,
    ) -> Result<String, String> {
        let request = Request {
            model: model.to_string(),
            messages: vec![Message::user_text(text)],
            ..Default::default()
        };
        let generate = make_generate_request(settings, &request, &GenerateOptions::default())
            .map_err(|e| e.to_string())?;
        let result = runtime.generate(generate, tag).map_err(|e| e.to_string())?;
        let text = result.text.trim().to_string();
        Ok(if text.is_empty() {
            "(llama returned no text)".into()
        } else {
            text
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use qt_llama::ModelSettings;

    fn settings() -> LlamaSettings {
        LlamaSettings {
            models: vec![
                ModelSettings {
                    id: "qwen".into(),
                    name: "Qwen".into(),
                    path: "/m/qwen.gguf".into(),
                    ..Default::default()
                },
                ModelSettings {
                    id: "embed".into(),
                    path: "/m/e.gguf".into(),
                    ..Default::default()
                },
            ],
            ..Default::default()
        }
    }

    #[test]
    fn status_models_and_selection() {
        let s = settings();
        let current = ModelRef::new("llama", "embed");
        assert_eq!(selected_model(&s, Some(&current)).as_deref(), Some("embed"));
        let mut chosen = s.clone();
        chosen.selected_model = Some("qwen".into());
        assert_eq!(
            selected_model(&chosen, Some(&current)).as_deref(),
            Some("qwen")
        );
        assert_eq!(
            selected_model(&s, Some(&ModelRef::new("openai", "x"))),
            None
        );

        let lines = status_lines(&s, Some("qwen"), Some(&current), None);
        assert_eq!(lines[0], "llama provider: configured");
        assert_eq!(lines[2], "selected: llama/qwen");
        assert_eq!(lines[3], "top_level_model: llama/embed");
        assert_eq!(
            lines[4],
            "loaded: unavailable until llama provider runtime is initialized"
        );
        assert_eq!(lines[5], "context: llama.cpp default");
        assert_eq!(lines[7], "kv_cache: session");
        assert_eq!(
            lines.last().unwrap(),
            "embeddings: enabled=false, pooling=default"
        );

        let status = RuntimeStatus {
            memory: json!({"devices": [{"name": "CUDA0", "type": "gpu", "gpuMemory": true, "totalBytes": 2048u64}]}),
            ..Default::default()
        };
        let snapshot = RuntimeSnapshot {
            loaded: None,
            kv: Some(KvCacheState::empty(0)),
            status: Some(status),
            instances: vec![(InstanceTag::Chat, RuntimeStatus::default())],
        };
        let lines = status_lines(&s, None, None, Some(&snapshot));
        assert!(lines.contains(&"loaded: (not loaded)".to_string()));
        assert!(
            lines.contains(
                &"kv_state: seq=0, min=-1, max=-1, position=0, canShift=false".to_string()
            )
        );
        assert!(
            lines
                .iter()
                .any(|l| l.starts_with("gpu_memory: CUDA0 type=gpu total=2.00 KiB"))
        );
        assert_eq!(
            lines.last().unwrap(),
            "- chat: (not loaded), contextLoaded=false, embeddings=false, sequencePosition=0"
        );

        assert_eq!(
            model_lines(&s, Some("qwen")),
            [
                "* qwen - Qwen (/m/qwen.gguf)",
                "- embed - embed (/m/e.gguf)"
            ]
        );
        assert_eq!(
            model_lines(&LlamaSettings::default(), None),
            ["No llama models are configured."]
        );
    }

    #[test]
    fn parses_kv_gpu_and_policy() {
        assert_eq!(
            parse_kv("").unwrap(),
            ("status".into(), KvCacheOperation::Inspect { seq_id: 0 })
        );
        assert_eq!(parse_kv("clear").unwrap().1, KvCacheOperation::Clear);
        assert_eq!(
            parse_kv("drop 1 4").unwrap().1,
            KvCacheOperation::Drop {
                seq_id: 1,
                from: 4,
                to: -1
            }
        );
        assert_eq!(
            parse_kv("shift 0 1 2 3").unwrap().1,
            KvCacheOperation::Shift {
                seq_id: 0,
                from: 1,
                to: 2,
                delta: 3
            }
        );
        assert_eq!(
            parse_kv("keep x").unwrap_err(),
            "Usage: /llama kv keep <seq-id>"
        );
        assert_eq!(
            parse_kv("shift 1 2").unwrap_err(),
            "Usage: /llama kv shift <seq-id> <from> <to> <delta>"
        );
        assert_eq!(
            parse_kv("nope").unwrap_err(),
            "Unsupported llama KV command: nope"
        );
        assert_eq!(
            kv_lines("clear", &KvCacheState::empty(0))[0],
            "Llama KV cache updated: clear"
        );

        let gpu = parse_gpu("gpuLayers=20 splitMode=row offloadKqv=off").unwrap();
        assert_eq!(
            Value::Object(gpu),
            json!({"gpuLayers": 20, "splitMode": "row", "offloadKqv": false})
        );
        assert!(parse_gpu("mainGpu=-1").is_err());
        assert!(parse_gpu("").is_err());
        let policy = parse_policy("temperature=0.2 topK=40").unwrap();
        assert_eq!(
            Value::Object(policy),
            json!({"temperature": 0.2, "topK": 40})
        );
        assert!(parse_policy("bogus=1").is_err());

        assert_eq!(format_bytes(512), "512 B");
        assert_eq!(format_bytes(1536), "1.50 KiB");
        assert_eq!(
            embedding_lines(&["Llama embedding generated."], &[0.5, 0.25, 1.0, 2.0], 3),
            [
                "Llama embedding generated.",
                "dimensions: 4",
                "input_tokens: 3",
                "preview: [0.5, 0.25, 1, ...]"
            ]
        );
    }

    #[test]
    fn settings_sections_are_merged() {
        let mut s = qt_core::Settings::default();
        let mut values = Map::new();
        values.insert("gpuLayers".into(), json!(10));
        set_llama_section(&mut s, "backend", &values);
        values.insert("gpuLayers".into(), Value::Null);
        values.insert("mainGpu".into(), json!(1));
        set_llama_section(&mut s, "backend", &values);
        assert_eq!(s.raw()["llama"], json!({"backend": {"mainGpu": 1}}));
    }
}
