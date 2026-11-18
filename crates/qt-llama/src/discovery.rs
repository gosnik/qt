//! Local GGUF model discovery and classification.
//!
//! Ports `llama_model_discovery.cpp`: candidate collection, GGUF metadata
//! inspection, quantisation naming (including the PrismML `Q2_0` = 41 file
//! type), capability classification with usage notes and runtime hints,
//! memory-task model selection, and persisting discovered models into
//! `settings.json`.

use std::collections::{BTreeMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

use regex::Regex;
use serde_json::{Map, Value, json};

use crate::LlamaError;
use crate::gguf::{self, GgufMetadata};
use crate::settings::{
    GenerationSettings, LlamaSettings, ModelSettings, PROVIDER_ID, lexically_normal,
    weakly_canonical, zero_cost_metadata,
};

/// What a discovered model is good for.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum ModelCapability {
    Embedding,
    Reranker,
    SentenceHandler,
    Chat,
    #[default]
    Other,
}

impl ModelCapability {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Embedding => "embedding",
            Self::Reranker => "reranker",
            Self::SentenceHandler => "sentence_handler",
            Self::Chat => "chat",
            Self::Other => "other",
        }
    }

    /// Generative models stream and can call tools.
    pub fn is_generative(self) -> bool {
        !matches!(self, Self::Embedding | Self::Reranker)
    }
}

/// Memory subsystem tasks that pick a local model.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MemoryTask {
    DocumentEmbedding,
    ShortEmbedding,
    Worthiness,
    Extraction,
    RecallPrehandler,
    ContextCompilation,
}

impl MemoryTask {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::DocumentEmbedding => "document_embedding",
            Self::ShortEmbedding => "short_embedding",
            Self::Worthiness => "worthiness",
            Self::Extraction => "extraction",
            Self::RecallPrehandler => "recall_prehandler",
            Self::ContextCompilation => "context_compilation",
        }
    }
}

#[derive(Debug, Clone)]
pub struct DiscoveryOptions {
    pub roots: Vec<PathBuf>,
    pub recursive: bool,
}

impl Default for DiscoveryOptions {
    fn default() -> Self {
        Self {
            roots: Vec::new(),
            recursive: true,
        }
    }
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct DiscoveredModel {
    pub id: String,
    pub name: String,
    pub path: String,
    pub format: String,
    pub capability: ModelCapability,
    pub best_usage: String,
    pub quantization: String,
    pub quantization_preference: String,
    pub block_count: Option<i64>,
    pub context_length: Option<i64>,
    pub embedding_length: Option<i64>,
    pub gguf_file_type: Option<i64>,
    pub gguf_metadata: GgufMetadata,
    pub matched_patterns: Vec<String>,
    pub usage_notes: Vec<String>,
    pub warnings: Vec<String>,
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct DiscoveryResult {
    pub models: Vec<DiscoveredModel>,
    pub diagnostics: Vec<String>,
}

/// The default discovery roots: `<cwd>/models`, `<agent_home>/models`, then
/// `extra` directories (relative ones resolve from `cwd`). Only existing
/// directories are kept for the two defaults; duplicates are removed.
pub fn default_roots(cwd: &Path, agent_home: &Path, extra: &[PathBuf]) -> Vec<PathBuf> {
    let mut roots: Vec<PathBuf> = Vec::new();
    let push = |path: PathBuf, roots: &mut Vec<PathBuf>| {
        let normal = lexically_normal(&path);
        if !roots.contains(&normal) {
            roots.push(normal);
        }
    };
    for root in [cwd.join("models"), agent_home.join("models")] {
        if root.is_dir() {
            push(root, &mut roots);
        }
    }
    for dir in extra {
        let expanded = crate::settings::expand_user_path(&dir.to_string_lossy());
        let path = if expanded.is_relative() {
            cwd.join(expanded)
        } else {
            expanded
        };
        push(path, &mut roots);
    }
    roots
}

fn contains(haystack: &str, needle: &str) -> bool {
    haystack.contains(needle)
}

fn append_unique(values: &mut Vec<String>, value: &str) {
    if !value.is_empty() && !values.iter().any(|v| v == value) {
        values.push(value.to_string());
    }
}

/// Lowercase ASCII slug of `value` (`local-model` when empty).
pub fn slugify(value: &str) -> String {
    let mut slug = String::new();
    let mut last_dash = false;
    for ch in value.chars() {
        if ch.is_ascii_alphanumeric() {
            slug.push(ch.to_ascii_lowercase());
            last_dash = false;
        } else if !last_dash && !slug.is_empty() {
            slug.push('-');
            last_dash = true;
        }
    }
    while slug.ends_with('-') {
        slug.pop();
    }
    if slug.is_empty() {
        "local-model".to_string()
    } else {
        slug
    }
}

/// Quantisation name for a GGUF `general.file_type` (llama ftype).
pub fn quantization_from_file_type(file_type: i64) -> Option<&'static str> {
    Some(match file_type {
        0 => "F32",
        1 => "F16",
        2 => "Q4_0",
        3 => "Q4_1",
        7 => "Q8_0",
        8 => "Q5_0",
        9 => "Q5_1",
        10 => "Q2_K",
        11 => "Q3_K_S",
        12 => "Q3_K_M",
        13 => "Q3_K_L",
        14 => "Q4_K_S",
        15 => "Q4_K_M",
        16 => "Q5_K_S",
        17 => "Q5_K_M",
        18 => "Q6_K",
        32 => "BF16",
        36 => "TQ1_0",
        37 => "TQ2_0",
        40 => "Q1_0",
        41 => "Q2_0",
        _ => return None,
    })
}

/// Quantisation name guessed from a file name corpus.
pub fn quantization_from_filename(corpus: &str) -> Option<String> {
    const NAMES: [&str; 25] = [
        "q8_0", "q6_k", "q5_k_m", "q5_k_s", "q4_k_m", "q4_k_s", "q4_0", "q4_1", "q3_k_l", "q3_k_m",
        "q3_k_s", "tq1_0", "tq2_0", "q1_0", "q2_0", "q2_k", "iq4_nl", "iq4_xs", "iq3_m", "iq3_s",
        "iq2_m", "iq2_s", "f16", "bf16", "f32",
    ];
    let normalized = corpus.replace(['-', '.'], "_");
    NAMES
        .iter()
        .find(|q| normalized.contains(*q))
        .map(|q| q.to_ascii_uppercase())
}

fn model_size_regex() -> &'static Regex {
    static RE: OnceLock<Regex> = OnceLock::new();
    RE.get_or_init(|| Regex::new(r"(?i)(\d+(?:\.\d+)?)([bm])").expect("valid regex"))
}

/// Parameter count in billions guessed from the corpus (`0.5b`, `135m`).
pub fn model_size_billions(corpus: &str) -> Option<f64> {
    let captures = model_size_regex().captures(corpus)?;
    let amount: f64 = captures.get(1)?.as_str().parse().ok()?;
    let unit = captures.get(2)?.as_str().to_ascii_lowercase();
    Some(if unit == "m" { amount / 1000.0 } else { amount })
}

pub fn embedding_quantization_preference(quantization: &str) -> &'static str {
    match quantization {
        "F16" | "BF16" | "F32" => "best_quality_more_ram",
        "Q8_0" => "recommended_default",
        "Q6_K" => "good_quality",
        "Q5_K_M" => "acceptable",
        "Q4_K_M" => "usable_if_constrained",
        "" => "unknown",
        _ => "avoid_unless_memory_is_extremely_constrained",
    }
}

pub fn sentence_handler_quantization_preference(
    quantization: &str,
    size_b: Option<f64>,
) -> &'static str {
    match quantization {
        "Q4_K_M" | "Q5_K_M" => "recommended_speed_quality_balance",
        "Q8_0" => {
            if size_b.is_some_and(|s| s <= 1.0) {
                "high_quality_for_tiny_model"
            } else {
                "high_quality_more_ram"
            }
        }
        "F16" | "BF16" | "F32" => "best_quality_more_ram",
        "" => "unknown",
        _ => "usable",
    }
}

fn first_matching_usage(
    corpus: &str,
    rules: &[(&str, &str)],
    matched: &mut Vec<String>,
) -> Option<String> {
    rules
        .iter()
        .find(|(pattern, _)| contains(corpus, pattern))
        .map(|(pattern, usage)| {
            append_unique(matched, pattern);
            usage.to_string()
        })
}

fn matches_any(corpus: &str, patterns: &[&str], matched: &mut Vec<String>) -> bool {
    let mut found = false;
    for pattern in patterns {
        if contains(corpus, pattern) {
            append_unique(matched, pattern);
            found = true;
        }
    }
    found
}

/// Classify a model from its lowercase text corpus and metadata. Returns the
/// capability and best usage; matched patterns are appended to `matched`.
pub fn classify_capability(
    corpus: &str,
    metadata: &GgufMetadata,
    matched: &mut Vec<String>,
) -> (ModelCapability, String) {
    const RERANKER: [(&str, &str); 6] = [
        ("bge-reranker-base", "rerank_memory_search_results"),
        ("bge-reranker-large", "rerank_memory_search_results"),
        ("jina-reranker", "rerank_memory_search_results"),
        ("reranker", "rerank_memory_search_results"),
        ("re-ranker", "rerank_memory_search_results"),
        ("cross-encoder", "rerank_memory_search_results"),
    ];
    if let Some(usage) = first_matching_usage(corpus, &RERANKER, matched) {
        return (ModelCapability::Reranker, usage);
    }
    const EMBEDDING: [(&str, &str); 17] = [
        ("bge-small-en-v1.5", "default_sentence_embedding"),
        ("all-minilm-l6-v2", "fast_short_sentence_embedding"),
        ("all-minilm", "fast_short_sentence_embedding"),
        ("mxbai-embed-xsmall-v1", "ultra_light_embedding"),
        ("nomic-embed-text-v1.5", "paragraph_or_document_embedding"),
        ("jina-embeddings-v2", "long_context_embedding"),
        ("snowflake-arctic-embed", "general_embedding"),
        ("gte-small", "general_embedding"),
        ("e5-small", "query_document_embedding"),
        ("sentence-transformers", "general_embedding"),
        ("nomic-embed", "paragraph_or_document_embedding"),
        ("jina-embeddings", "long_context_embedding"),
        ("mxbai-embed", "general_embedding"),
        ("bge", "general_embedding"),
        ("minilm", "fast_short_sentence_embedding"),
        ("e5", "query_document_embedding"),
        ("gte", "general_embedding"),
    ];
    if let Some(usage) = first_matching_usage(corpus, &EMBEDDING, matched) {
        return (ModelCapability::Embedding, usage);
    }
    const HIGH_CAPACITY: [(&str, &str); 2] = [
        (
            "ternary-bonsai-27b",
            "memory_context_compilation_and_consolidation_review",
        ),
        (
            "ternary bonsai 27b",
            "memory_context_compilation_and_consolidation_review",
        ),
    ];
    if let Some(usage) = first_matching_usage(corpus, &HIGH_CAPACITY, matched) {
        return (ModelCapability::Chat, usage);
    }
    const SENTENCE: [(&str, &str); 8] = [
        ("qwen3-0.6b", "local_sentence_classification_and_extraction"),
        (
            "qwen2.5-0.5b-instruct",
            "local_sentence_classification_and_extraction",
        ),
        (
            "qwen2-0.5b-instruct",
            "local_sentence_classification_and_extraction",
        ),
        ("smollm2-360m-instruct", "ultra_light_sentence_handler"),
        ("smollm2-1.7b-instruct", "better_local_summary_and_rewrite"),
        ("tinyllama-1.1b-chat", "lightweight_chat_or_rewrite"),
        ("phi-3-mini", "higher_quality_local_reasoning"),
        ("gemma-2-2b", "higher_quality_local_rewrite"),
    ];
    if let Some(usage) = first_matching_usage(corpus, &SENTENCE, matched) {
        return (ModelCapability::SentenceHandler, usage);
    }
    if matches_any(corpus, &["smol", "smollm", "tinyllama"], matched) {
        let usage = if contains(corpus, "smollm2-1.7b") {
            "better_local_summary_and_rewrite"
        } else {
            "ultra_light_sentence_handler"
        };
        return (ModelCapability::SentenceHandler, usage.to_string());
    }
    let local_llm = ["qwen", "phi", "gemma", "llama"]
        .iter()
        .any(|m| contains(corpus, m));
    let instruction = contains(corpus, "instruct") || contains(corpus, "chat");
    if local_llm && instruction && model_size_billions(corpus).is_some_and(|s| s <= 3.0) {
        append_unique(matched, "small-local-instruct");
        return (
            ModelCapability::SentenceHandler,
            "local_sentence_classification_and_extraction".to_string(),
        );
    }
    if matches_any(
        corpus,
        &[
            "qwen", "qwen2", "qwen2.5", "qwen3", "phi", "gemma", "llama", "instruct", "chat",
        ],
        matched,
    ) {
        return (ModelCapability::Chat, "local_chat".to_string());
    }
    // Decoder models expose an embedding width too; only encoder
    // architectures count as embedding models from metadata alone.
    let architecture = metadata
        .string("general.architecture")
        .unwrap_or_default()
        .to_ascii_lowercase();
    if architecture == "bert" || architecture == "nomic-bert" {
        append_unique(matched, &architecture);
        return (ModelCapability::Embedding, "general_embedding".to_string());
    }
    (ModelCapability::Other, "local_llama_model".to_string())
}

pub fn usage_notes_for(capability: ModelCapability) -> Vec<String> {
    let notes: &[&str] = match capability {
        ModelCapability::Embedding => &[
            "Use for sentence, memory chunk, and query vectors.",
            "Prefer this over chat models for primary memory search.",
            "Hybrid retrieval hint: 0.55 vector, 0.25 lexical, 0.15 graph, 0.05 recency_or_importance.",
        ],
        ModelCapability::Reranker => &[
            "Use after initial vector/ngram retrieval.",
            "Retrieve top 20-100 candidates, rerank, then keep top 5-20.",
        ],
        ModelCapability::SentenceHandler => &[
            "Use for memory worthiness, entity extraction, canonical rewrites, contradiction checks, graph edge suggestions, and context summaries.",
            "Do not use as the primary vector search model.",
        ],
        ModelCapability::Chat => {
            &["Use for local chat or rewrite tasks when a generative model is needed."]
        }
        ModelCapability::Other => {
            &["GGUF model with no known memory-system specialization inferred."]
        }
    };
    notes.iter().map(|s| s.to_string()).collect()
}

pub fn runtime_hints(capability: ModelCapability) -> Value {
    match capability {
        ModelCapability::Embedding => json!({
            "hybridScore": {
                "vectorSimilarity": 0.55,
                "ngramOrBm25": 0.25,
                "graphProximity": 0.15,
                "recencyOrImportance": 0.05
            },
            "preferredForPrimaryMemorySearch": true
        }),
        ModelCapability::Reranker => json!({
            "initialCandidatesMin": 20,
            "initialCandidatesMax": 100,
            "finalContextMin": 5,
            "finalContextMax": 20
        }),
        ModelCapability::SentenceHandler => json!({
            "primaryEmbeddingSearch": false,
            "tasks": [
                "memory_worthiness_classification",
                "entity_extraction",
                "canonical_memory_rewrite",
                "contradiction_detection",
                "graph_edge_suggestion",
                "context_compression"
            ]
        }),
        ModelCapability::Chat | ModelCapability::Other => json!({}),
    }
}

fn infer_context_length(metadata: &GgufMetadata) -> Option<i64> {
    [
        "llama.context_length",
        "bert.context_length",
        "nomic-bert.context_length",
        "qwen2.context_length",
        "qwen3.context_length",
        "qwen35.context_length",
    ]
    .iter()
    .find_map(|key| metadata.int(key))
}

fn infer_block_count(metadata: &GgufMetadata) -> Option<i64> {
    if let Some(arch) = metadata.string("general.architecture")
        && let Some(value) = metadata.int(&format!("{arch}.block_count"))
    {
        return Some(value);
    }
    metadata
        .fields
        .iter()
        .filter(|(k, _)| k.ends_with(".block_count"))
        .find_map(|(k, _)| metadata.int(k))
}

fn text_corpus(path: &Path, metadata: &GgufMetadata) -> String {
    let name = |p: Option<&Path>| {
        p.and_then(Path::file_name)
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default()
    };
    let parent = path.parent();
    let mut corpus = format!(
        "{} {} {}",
        name(Some(path)),
        name(parent),
        name(parent.and_then(Path::parent))
    );
    for (key, value) in &metadata.fields {
        corpus.push(' ');
        corpus.push_str(key);
        if let Some(text) = value.as_str() {
            corpus.push(' ');
            corpus.push_str(text);
        }
    }
    corpus.to_lowercase()
}

/// Inspect one GGUF file.
pub fn inspect_model_file(path: &Path) -> DiscoveredModel {
    let mut model = DiscoveredModel {
        path: lexically_normal(path).to_string_lossy().into_owned(),
        format: "gguf".to_string(),
        ..Default::default()
    };
    match gguf::read_metadata(path) {
        Ok(metadata) => model.gguf_metadata = metadata,
        Err(err) => model
            .warnings
            .push(format!("GGUF metadata parse failed: {err}")),
    }
    let stem = path
        .file_stem()
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_default();
    model.name = model.gguf_metadata.string("general.name").unwrap_or(stem);
    let corpus = text_corpus(path, &model.gguf_metadata);
    model.block_count = infer_block_count(&model.gguf_metadata);
    model.context_length = infer_context_length(&model.gguf_metadata);
    model.embedding_length = model.gguf_metadata.int("embedding.length");
    model.gguf_file_type = model.gguf_metadata.int("general.file_type");
    model.quantization = model
        .gguf_file_type
        .and_then(quantization_from_file_type)
        .map(str::to_string)
        .or_else(|| quantization_from_filename(&corpus))
        .unwrap_or_default();
    let (capability, usage) =
        classify_capability(&corpus, &model.gguf_metadata, &mut model.matched_patterns);
    model.capability = capability;
    model.best_usage = usage;
    model.quantization_preference = match capability {
        ModelCapability::Embedding => embedding_quantization_preference(&model.quantization),
        ModelCapability::SentenceHandler => sentence_handler_quantization_preference(
            &model.quantization,
            model_size_billions(&corpus),
        ),
        ModelCapability::Reranker => "precision_over_latency",
        _ if model.quantization.is_empty() => "unknown",
        _ => "model_dependent",
    }
    .to_string();
    model.usage_notes = usage_notes_for(capability);
    model
}

fn is_gguf(path: &Path) -> bool {
    path.extension()
        .is_some_and(|e| e.to_string_lossy().eq_ignore_ascii_case("gguf"))
}

fn scan_dir(root: &Path, recursive: bool, paths: &mut Vec<PathBuf>) -> std::io::Result<()> {
    let mut stack = vec![root.to_path_buf()];
    let mut first = true;
    while let Some(dir) = stack.pop() {
        let entries = match std::fs::read_dir(&dir) {
            Ok(entries) => entries,
            Err(err) if first => return Err(err),
            // Permission-denied subdirectories are skipped.
            Err(_) => continue,
        };
        first = false;
        for entry in entries {
            let entry = entry?;
            let path = entry.path();
            let file_type = entry.file_type()?;
            if file_type.is_dir() {
                if recursive {
                    stack.push(path);
                }
                continue;
            }
            let is_file = if file_type.is_symlink() {
                std::fs::metadata(&path).is_ok_and(|m| m.is_file())
            } else {
                file_type.is_file()
            };
            if is_file && is_gguf(&path) {
                paths.push(path);
            }
        }
    }
    Ok(())
}

fn collect_candidates(options: &DiscoveryOptions, diagnostics: &mut Vec<String>) -> Vec<PathBuf> {
    let mut paths = Vec::new();
    for root in &options.roots {
        let text = root.to_string_lossy();
        let trimmed = text.trim();
        if trimmed.is_empty() {
            diagnostics.push("local model discovery skipped empty root".into());
            continue;
        }
        let root = PathBuf::from(trimmed);
        let display = root.to_string_lossy().into_owned();
        let Ok(metadata) = std::fs::metadata(&root) else {
            diagnostics.push(format!(
                "local model discovery path does not exist: {display}"
            ));
            continue;
        };
        if metadata.is_file() {
            if is_gguf(&root) {
                paths.push(root);
            }
            continue;
        }
        if !metadata.is_dir() {
            diagnostics.push(format!(
                "local model discovery path is not a file or directory: {display}"
            ));
            continue;
        }
        if let Err(err) = scan_dir(&root, options.recursive, &mut paths) {
            let verb = if options.recursive {
                "could not fully scan"
            } else {
                "could not scan"
            };
            diagnostics.push(format!("local model discovery {verb} {display}: {err}"));
        }
    }
    paths.sort_by(|a, b| a.to_string_lossy().cmp(&b.to_string_lossy()));
    paths.dedup();
    paths
}

fn assign_unique_ids(models: &mut [DiscoveredModel]) {
    let mut used = HashSet::new();
    for model in models {
        let stem = Path::new(&model.path)
            .file_stem()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_default();
        let base = slugify(&stem);
        let mut candidate = base.clone();
        let mut suffix = 2;
        while !used.insert(candidate.to_ascii_lowercase()) {
            candidate = format!("{base}-{suffix}");
            suffix += 1;
        }
        model.id = candidate;
    }
}

/// Discover GGUF models under the configured roots.
pub fn discover_models(options: &DiscoveryOptions) -> DiscoveryResult {
    let mut result = DiscoveryResult::default();
    for path in collect_candidates(options, &mut result.diagnostics) {
        let model = inspect_model_file(&path);
        for warning in &model.warnings {
            result
                .diagnostics
                .push(format!("{}: {warning}", model.path));
        }
        result.models.push(model);
    }
    assign_unique_ids(&mut result.models);
    result
}

impl DiscoveredModel {
    /// Settings entry for this model.
    pub fn to_settings(&self) -> ModelSettings {
        ModelSettings {
            id: self.id.clone(),
            name: self.name.clone(),
            path: self.path.clone(),
            block_count: self.block_count,
            context_window: self.context_length,
            supports_embeddings: Some(self.capability == ModelCapability::Embedding),
            ..Default::default()
        }
    }

    pub fn to_json(&self) -> Value {
        let mut m = Map::new();
        m.insert("id".into(), json!(self.id));
        m.insert("name".into(), json!(self.name));
        m.insert("path".into(), json!(self.path));
        m.insert("format".into(), json!(self.format));
        m.insert("capability".into(), json!(self.capability.as_str()));
        m.insert("bestUsage".into(), json!(self.best_usage));
        m.insert("matchedPatterns".into(), json!(self.matched_patterns));
        m.insert("usageNotes".into(), json!(self.usage_notes));
        m.insert("warnings".into(), json!(self.warnings));
        self.optional_fields(&mut m);
        Value::Object(m)
    }

    fn optional_fields(&self, m: &mut Map<String, Value>) {
        if !self.quantization.is_empty() {
            m.insert("quantization".into(), json!(self.quantization));
        }
        if !self.quantization_preference.is_empty() {
            m.insert(
                "quantizationPreference".into(),
                json!(self.quantization_preference),
            );
        }
        if let Some(v) = self.context_length {
            m.insert("contextLength".into(), json!(v));
        }
        if let Some(v) = self.block_count {
            m.insert("blockCount".into(), json!(v));
        }
        if let Some(v) = self.embedding_length {
            m.insert("embeddingLength".into(), json!(v));
        }
        if let Some(v) = self.gguf_file_type {
            m.insert("ggufFileType".into(), json!(v));
        }
        if !self.gguf_metadata.fields.is_empty() {
            m.insert("ggufMetadata".into(), self.gguf_metadata.to_json());
        }
    }

    /// The `localDiscovery` catalog metadata block.
    pub fn discovery_metadata(&self) -> Value {
        let mut m = Map::new();
        m.insert("source".into(), json!("cli.localModelDiscovery"));
        m.insert("capability".into(), json!(self.capability.as_str()));
        m.insert("bestUsage".into(), json!(self.best_usage));
        m.insert("format".into(), json!(self.format));
        m.insert("path".into(), json!(self.path));
        m.insert("matchedPatterns".into(), json!(self.matched_patterns));
        m.insert("usageNotes".into(), json!(self.usage_notes));
        m.insert("runtimeHints".into(), runtime_hints(self.capability));
        self.optional_fields(&mut m);
        if !self.warnings.is_empty() {
            m.insert("warnings".into(), json!(self.warnings));
        }
        Value::Object(m)
    }

    /// Catalog record metadata (archived `model_catalog_from_llama_discovery`).
    pub fn catalog_metadata(&self, settings: Option<&LlamaSettings>) -> Value {
        let settings_model = settings
            .and_then(|s| {
                s.models
                    .iter()
                    .find(|m| m.id.eq_ignore_ascii_case(&self.id))
                    .cloned()
            })
            .unwrap_or_else(|| self.to_settings());
        let generation = settings
            .map(|s| s.generation.merged(&settings_model.generation).to_json())
            .unwrap_or_else(|| GenerationSettings::default().to_json());
        let mut metadata = json!({
            "input": ["text"],
            "cost": zero_cost_metadata(),
            "path": self.path,
            "local": true,
            "direct": true,
            "format": self.format,
            "localDiscovery": self.discovery_metadata(),
            "llama": {"model": settings_model.to_json(), "generation": generation},
        });
        if let Some(v) = self.context_length {
            metadata["contextWindow"] = json!(v);
        }
        if self.capability == ModelCapability::Embedding {
            metadata["supportsEmbeddings"] = json!(true);
        }
        metadata
    }

    /// Model picker entry.
    pub fn model_info(&self) -> qt_core::ModelInfo {
        qt_core::ModelInfo {
            provider: PROVIDER_ID.to_string(),
            id: self.id.clone(),
            name: self.name.clone(),
            context_window: self
                .context_length
                .and_then(|v| u64::try_from(v).ok())
                .filter(|v| *v > 0),
            ..Default::default()
        }
    }
}

impl DiscoveryResult {
    pub fn to_json(&self) -> Value {
        json!({
            "models": self.models.iter().map(DiscoveredModel::to_json).collect::<Vec<_>>(),
            "diagnostics": self.diagnostics,
        })
    }
}

/// Pick a configured, readable GGUF for a memory task, preferring the
/// quantised task models the archived qt recommends.
pub fn select_model_for_memory_task(
    settings: &LlamaSettings,
    task: MemoryTask,
) -> Option<&ModelSettings> {
    let preference = |model: &ModelSettings| -> i32 {
        let corpus = format!("{} {} {}", model.id, model.name, model.path).to_lowercase();
        let has = |v: &str| corpus.contains(v);
        let qwen05 = has("qwen2.5-0.5b-instruct") || has("qwen2-5-0-5b-instruct");
        let qwen3b = has("qwen2.5-3b-instruct") || has("qwen2-5-3b-instruct");
        let smol = has("smollm2-135m-instruct");
        let mut score = match task {
            MemoryTask::DocumentEmbedding if has("nomic-embed-text-v1.5") => 500,
            MemoryTask::DocumentEmbedding if has("bge-small-en-v1.5") => 400,
            MemoryTask::ShortEmbedding if has("bge-small-en-v1.5") => 500,
            MemoryTask::ShortEmbedding if has("nomic-embed-text-v1.5") => 400,
            MemoryTask::Worthiness if smol => 500,
            MemoryTask::Worthiness if qwen05 => 400,
            MemoryTask::Extraction if qwen05 => 500,
            MemoryTask::Extraction if qwen3b => 400,
            MemoryTask::RecallPrehandler if qwen3b => 600,
            MemoryTask::RecallPrehandler if qwen05 => 450,
            MemoryTask::RecallPrehandler if smol => 300,
            MemoryTask::ContextCompilation
                if has("ternary-bonsai-27b") || has("ternary bonsai 27b") =>
            {
                1100
            }
            MemoryTask::ContextCompilation if qwen3b => 600,
            MemoryTask::ContextCompilation if qwen05 => 450,
            MemoryTask::ContextCompilation if smol => 300,
            _ => -1,
        };
        if score < 0 {
            return score;
        }
        if has("q8_0") || has("q8-0") {
            score += 30;
        }
        if has("q4_k_m") || has("q4-k-m") {
            score += if task == MemoryTask::ContextCompilation {
                40
            } else {
                20
            };
        }
        if has("f16") {
            score += 5;
        }
        score
    };
    let mut selected: Option<&ModelSettings> = None;
    let mut selected_score = -1;
    for model in &settings.models {
        if model.path.is_empty() || !Path::new(&model.path).is_file() {
            continue;
        }
        let score = preference(model);
        if score > selected_score
            || (score == selected_score && selected.is_some_and(|s| model.id < s.id))
        {
            selected = Some(model);
            selected_score = score;
        }
    }
    selected
}

/// Outcome of [`persist_discovered_models`].
#[derive(Debug, Clone, Default, PartialEq)]
pub struct PersistedDiscovery {
    pub settings_path: PathBuf,
    pub added: usize,
    pub skipped: usize,
    pub diagnostics: Vec<String>,
}

fn resolve_from(base: &Path, path: &str) -> PathBuf {
    let expanded = crate::settings::expand_user_path(path);
    let joined = if expanded.is_relative() {
        base.join(expanded)
    } else {
        expanded
    };
    weakly_canonical(&joined)
}

fn unique_model_id(id: &str, used: &mut HashSet<String>) -> String {
    let base = if id.is_empty() { "local-model" } else { id };
    let mut candidate = base.to_string();
    let mut suffix = 2;
    while !used.insert(candidate.to_ascii_lowercase()) {
        candidate = format!("{base}-{suffix}");
        suffix += 1;
    }
    candidate
}

/// Append discovered models to `llama.models` in `settings_path`, skipping
/// paths already configured and de-duplicating ids (archived
/// `persist_discovered_llama_models`). Relative discovered paths resolve
/// from `cwd`.
pub fn persist_discovered_models(
    settings_path: &Path,
    cwd: &Path,
    discovery: &DiscoveryResult,
) -> Result<PersistedDiscovery, LlamaError> {
    let mut result = PersistedDiscovery {
        settings_path: settings_path.to_path_buf(),
        ..Default::default()
    };
    if discovery.models.is_empty() {
        return Ok(result);
    }
    let settings_dir = settings_path.parent().unwrap_or(Path::new("."));
    let mut document: Value = match std::fs::read_to_string(settings_path) {
        Ok(text) => serde_json::from_str(&text).map_err(|err| {
            LlamaError::InvalidSettings(format!(
                "invalid settings JSON in {}: {err}",
                settings_path.display()
            ))
        })?,
        Err(err) if err.kind() == std::io::ErrorKind::NotFound => json!({}),
        Err(err) => {
            return Err(LlamaError::InvalidSettings(format!(
                "settings file could not be read: {}: {err}",
                settings_path.display()
            )));
        }
    };
    let root = document.as_object_mut().ok_or_else(|| {
        LlamaError::InvalidSettings("settings document must be a JSON object".into())
    })?;
    let llama = root.entry("llama").or_insert_with(|| json!({}));
    if llama.is_null() {
        *llama = json!({});
    }
    let llama = llama.as_object_mut().ok_or_else(|| {
        LlamaError::InvalidSettings("settings llama field must be an object".into())
    })?;
    let models = llama.entry("models").or_insert_with(|| json!([]));
    if models.is_null() {
        *models = json!([]);
    }
    let models = models.as_array_mut().ok_or_else(|| {
        LlamaError::InvalidSettings("settings llama.models field must be an array".into())
    })?;

    let mut used_ids = HashSet::new();
    let mut used_paths = HashSet::new();
    for item in models.iter() {
        if let Some(id) = item.get("id").and_then(Value::as_str) {
            used_ids.insert(id.to_ascii_lowercase());
        }
        if let Some(path) = item.get("path").and_then(Value::as_str) {
            used_paths.insert(
                resolve_from(settings_dir, path)
                    .to_string_lossy()
                    .to_ascii_lowercase(),
            );
        }
    }
    for discovered in &discovery.models {
        let mut model = discovered.to_settings();
        model.path = resolve_from(cwd, &model.path)
            .to_string_lossy()
            .into_owned();
        let key = model.path.to_ascii_lowercase();
        if used_paths.contains(&key) {
            result.skipped += 1;
            result.diagnostics.push(format!(
                "local model discovery skipped duplicate llama model path: {}",
                model.path
            ));
            continue;
        }
        model.id = unique_model_id(&model.id, &mut used_ids);
        used_paths.insert(key);
        models.push(model.to_json());
        result.added += 1;
    }
    if result.added == 0 {
        return Ok(result);
    }
    qt_core::config::write_json_atomic(settings_path, &document, false)
        .map_err(|err| LlamaError::InvalidSettings(format!("{err:#}")))?;
    result.diagnostics.push(format!(
        "saved {} discovered llama model(s) to {}",
        result.added,
        settings_path.display()
    ));
    Ok(result)
}

/// Group discovered models by capability (for listings).
pub fn by_capability(result: &DiscoveryResult) -> BTreeMap<&'static str, Vec<&DiscoveredModel>> {
    let mut map: BTreeMap<&'static str, Vec<&DiscoveredModel>> = BTreeMap::new();
    for model in &result.models {
        map.entry(model.capability.as_str())
            .or_default()
            .push(model);
    }
    map
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::gguf::testing::{Kv, gguf};

    fn classify(corpus: &str) -> (ModelCapability, String, Vec<String>) {
        let mut matched = Vec::new();
        let (c, u) = classify_capability(corpus, &GgufMetadata::default(), &mut matched);
        (c, u, matched)
    }

    #[test]
    fn quantization_names_include_prism_q2_0() {
        assert_eq!(quantization_from_file_type(41), Some("Q2_0"));
        assert_eq!(quantization_from_file_type(40), Some("Q1_0"));
        assert_eq!(quantization_from_file_type(15), Some("Q4_K_M"));
        assert_eq!(quantization_from_file_type(99), None);
        assert_eq!(
            quantization_from_filename("ternary-bonsai-27b-q2_0.gguf").as_deref(),
            Some("Q2_0")
        );
        assert_eq!(
            quantization_from_filename("model.q4-k-m.gguf").as_deref(),
            Some("Q4_K_M")
        );
        assert_eq!(quantization_from_filename("plain.gguf"), None);
    }

    #[test]
    fn classifies_like_archived() {
        assert_eq!(
            classify("bge-reranker-base.gguf").0,
            ModelCapability::Reranker
        );
        let (c, u, m) = classify("bge-small-en-v1.5-q8_0.gguf");
        assert_eq!(
            (c, u.as_str()),
            (ModelCapability::Embedding, "default_sentence_embedding")
        );
        assert_eq!(m, vec!["bge-small-en-v1.5"]);
        assert_eq!(
            classify("ternary-bonsai-27b-q2_0.gguf").1,
            "memory_context_compilation_and_consolidation_review"
        );
        assert_eq!(
            classify("qwen2.5-0.5b-instruct-q4_k_m.gguf").0,
            ModelCapability::SentenceHandler
        );
        let (c, u, _) = classify("smollm2-135m-instruct-q8_0.gguf");
        assert_eq!(
            (c, u.as_str()),
            (
                ModelCapability::SentenceHandler,
                "ultra_light_sentence_handler"
            )
        );
        let (c, u, m) = classify("qwen2.5-3b-instruct-q4_k_m.gguf");
        assert_eq!(
            (c, u.as_str()),
            (
                ModelCapability::SentenceHandler,
                "local_sentence_classification_and_extraction"
            )
        );
        assert!(m.contains(&"small-local-instruct".to_string()));
        assert_eq!(
            classify("qwen2.5-14b-instruct.gguf").0,
            ModelCapability::Chat
        );
        assert_eq!(classify("mystery.gguf").0, ModelCapability::Other);
        let mut meta = GgufMetadata::default();
        meta.fields
            .insert("general.architecture".into(), json!("nomic-bert"));
        let mut matched = Vec::new();
        assert_eq!(
            classify_capability("mystery.gguf", &meta, &mut matched).0,
            ModelCapability::Embedding
        );
        assert_eq!(model_size_billions("smollm2-135m"), Some(0.135));
        assert_eq!(model_size_billions("qwen2.5-0.5b"), Some(0.5));
    }

    #[test]
    fn slugs_and_preferences() {
        assert_eq!(slugify("Qwen2.5 0.5B__Instruct!"), "qwen2-5-0-5b-instruct");
        assert_eq!(slugify("---"), "local-model");
        assert_eq!(
            embedding_quantization_preference("Q8_0"),
            "recommended_default"
        );
        assert_eq!(
            embedding_quantization_preference("Q2_K"),
            "avoid_unless_memory_is_extremely_constrained"
        );
        assert_eq!(
            sentence_handler_quantization_preference("Q8_0", Some(0.5)),
            "high_quality_for_tiny_model"
        );
        assert_eq!(
            sentence_handler_quantization_preference("Q8_0", Some(3.0)),
            "high_quality_more_ram"
        );
    }

    #[test]
    fn discovers_and_persists_models() {
        let dir = tempfile::tempdir().unwrap();
        let models = dir.path().join("models");
        std::fs::create_dir_all(models.join("sub")).unwrap();
        std::fs::write(
            models.join("qwen2.5-0.5b-instruct-q4_k_m.gguf"),
            gguf(&[
                ("general.architecture", Kv::Str("qwen2")),
                ("general.name", Kv::Str("Qwen2.5 0.5B Instruct")),
                ("general.file_type", Kv::U32(15)),
                ("qwen2.block_count", Kv::U32(24)),
                ("qwen2.context_length", Kv::U32(32768)),
            ]),
        )
        .unwrap();
        std::fs::write(models.join("sub/Broken.GGUF"), b"nope").unwrap();
        std::fs::write(models.join("readme.txt"), b"x").unwrap();
        let roots = default_roots(
            dir.path(),
            &dir.path().join("home"),
            &[PathBuf::from("missing")],
        );
        assert_eq!(roots, vec![models.clone(), dir.path().join("missing")]);
        let result = discover_models(&DiscoveryOptions {
            roots,
            recursive: true,
        });
        assert_eq!(result.models.len(), 2);
        let qwen = &result.models[0];
        assert_eq!(qwen.id, "qwen2-5-0-5b-instruct-q4-k-m");
        assert_eq!(qwen.name, "Qwen2.5 0.5B Instruct");
        assert_eq!(qwen.quantization, "Q4_K_M");
        assert_eq!(qwen.block_count, Some(24));
        assert_eq!(qwen.context_length, Some(32768));
        assert_eq!(qwen.capability, ModelCapability::SentenceHandler);
        assert_eq!(
            qwen.quantization_preference,
            "recommended_speed_quality_balance"
        );
        let broken = &result.models[1];
        assert_eq!(broken.id, "broken");
        assert_eq!(
            broken.warnings,
            vec!["GGUF metadata parse failed: missing GGUF magic"]
        );
        assert!(
            result
                .diagnostics
                .iter()
                .any(|d| d.contains("path does not exist"))
        );
        assert!(
            result
                .diagnostics
                .iter()
                .any(|d| d.ends_with("GGUF metadata parse failed: missing GGUF magic"))
        );

        let meta = qwen.catalog_metadata(None);
        assert_eq!(meta["localDiscovery"]["capability"], "sentence_handler");
        assert_eq!(meta["contextWindow"], 32768);
        assert_eq!(meta["llama"]["model"]["blockCount"], 24);

        let non_recursive = discover_models(&DiscoveryOptions {
            roots: vec![models.clone()],
            recursive: false,
        });
        assert_eq!(non_recursive.models.len(), 1);

        let settings_path = dir.path().join("home/settings.json");
        std::fs::create_dir_all(settings_path.parent().unwrap()).unwrap();
        std::fs::write(&settings_path, r#"{"theme": "dark", "llama": {"models": [{"id": "broken", "name": "x", "path": "/elsewhere.gguf"}]}}"#).unwrap();
        let persisted = persist_discovered_models(&settings_path, dir.path(), &result).unwrap();
        assert_eq!((persisted.added, persisted.skipped), (2, 0));
        let again = persist_discovered_models(&settings_path, dir.path(), &result).unwrap();
        assert_eq!((again.added, again.skipped), (0, 2));
        let doc: Value =
            serde_json::from_str(&std::fs::read_to_string(&settings_path).unwrap()).unwrap();
        assert_eq!(doc["theme"], "dark");
        let ids: Vec<&str> = doc["llama"]["models"]
            .as_array()
            .unwrap()
            .iter()
            .map(|m| m["id"].as_str().unwrap())
            .collect();
        assert_eq!(
            ids,
            vec!["broken", "qwen2-5-0-5b-instruct-q4-k-m", "broken-2"]
        );
        let loaded = LlamaSettings::from_document(&doc, settings_path.parent().unwrap()).unwrap();
        assert_eq!(
            select_model_for_memory_task(&loaded, MemoryTask::Extraction).map(|m| m.id.as_str()),
            Some("qwen2-5-0-5b-instruct-q4-k-m")
        );
        assert!(select_model_for_memory_task(&loaded, MemoryTask::DocumentEmbedding).is_none());
    }

    #[test]
    fn memory_task_scores_prefer_quantised_task_models() {
        let dir = tempfile::tempdir().unwrap();
        let mk = |name: &str| {
            let p = dir.path().join(name);
            std::fs::write(&p, b"GGUF").unwrap();
            ModelSettings {
                id: slugify(name),
                name: name.into(),
                path: p.to_string_lossy().into_owned(),
                ..Default::default()
            }
        };
        let settings = LlamaSettings {
            models: vec![
                mk("bge-small-en-v1.5-f16.gguf"),
                mk("bge-small-en-v1.5-q8_0.gguf"),
                mk("nomic-embed-text-v1.5.Q8_0.gguf"),
                mk("qwen2.5-3b-instruct-q4_k_m.gguf"),
                mk("smollm2-135m-instruct-q8_0.gguf"),
                mk("Ternary-Bonsai-27B-Q2_0.gguf"),
            ],
            ..Default::default()
        };
        let pick = |task| {
            select_model_for_memory_task(&settings, task)
                .unwrap()
                .name
                .clone()
        };
        assert_eq!(
            pick(MemoryTask::ShortEmbedding),
            "bge-small-en-v1.5-q8_0.gguf"
        );
        assert_eq!(
            pick(MemoryTask::DocumentEmbedding),
            "nomic-embed-text-v1.5.Q8_0.gguf"
        );
        assert_eq!(
            pick(MemoryTask::Worthiness),
            "smollm2-135m-instruct-q8_0.gguf"
        );
        assert_eq!(
            pick(MemoryTask::RecallPrehandler),
            "qwen2.5-3b-instruct-q4_k_m.gguf"
        );
        assert_eq!(
            pick(MemoryTask::ContextCompilation),
            "Ternary-Bonsai-27B-Q2_0.gguf"
        );
    }
}
