//! End-to-end tests against real GGUF models.
//!
//! Models are looked up in `QT_LLAMA_TEST_MODELS` (a directory), falling back
//! to `<workspace>/models`. Tests skip (with a message) when llama.cpp is not
//! linked or the model files are missing:
//! - `SmolLM2-135M-Instruct-Q4_K_M.gguf` (generation, KV reuse, grammar)
//! - `bge-small-en-v1.5-q8_0.gguf` (embeddings)
//! - `Ternary-Bonsai-27B-Q2_0.gguf` (only its GGUF header is read)

// Each test runs on its own single-threaded runtime; the std mutex only
// serialises model-heavy tests, so holding it across awaits is intended.
#![allow(clippy::await_holding_lock)]

use std::path::PathBuf;
use std::sync::Mutex;

use qt_core::message::{Message, StopReason, UserContent};
use qt_core::provider::Request;
use qt_llama::discovery::{DiscoveryOptions, ModelCapability, discover_models, inspect_model_file};
use qt_llama::provider::make_generate_request;
use qt_llama::runtime::KvCacheOperation;
use qt_llama::settings::ModelSettings;
use qt_llama::{EmbeddingRequest, GenerateOptions, InstanceTag, LlamaRuntime, LlamaSettings};
use serde_json::json;
use tokio_util::sync::CancellationToken;

/// llama.cpp runs one heavy model at a time in tests to bound memory.
static SERIAL: Mutex<()> = Mutex::new(());

fn models_dir() -> PathBuf {
    std::env::var_os("QT_LLAMA_TEST_MODELS")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../models"))
}

fn model(file: &str, id: &str) -> Option<ModelSettings> {
    if !qt_llama::native_available() {
        eprintln!("skipping: llama.cpp is not linked in this build");
        return None;
    }
    let path = models_dir().join(file);
    if !path.is_file() {
        eprintln!("skipping: {} not found", path.display());
        return None;
    }
    Some(ModelSettings {
        id: id.into(),
        name: id.into(),
        path: path.to_string_lossy().into_owned(),
        context_window: Some(2048),
        ..Default::default()
    })
}

fn chat_settings() -> Option<LlamaSettings> {
    let model = model("SmolLM2-135M-Instruct-Q4_K_M.gguf", "smol")?;
    let mut settings = LlamaSettings {
        models: vec![model],
        models_configured: true,
        ..Default::default()
    };
    settings.generation.temperature = Some(0.0);
    settings.generation.max_output_tokens = Some(24);
    Some(settings)
}

fn user(text: &str) -> Request {
    Request {
        model: "smol".into(),
        messages: vec![Message::User {
            content: UserContent::Text(text.into()),
            timestamp: None,
        }],
        max_tokens: 16_384,
        ..Default::default()
    }
}

#[tokio::test]
async fn streams_greedy_text_and_reuses_kv_cache() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let Some(settings) = chat_settings() else {
        return;
    };
    let runtime = LlamaRuntime::new(settings);
    let request = user("What is the capital of France? Answer in one word.");

    let mut deltas = String::new();
    let message = qt_llama::stream(
        &runtime,
        &request,
        &CancellationToken::new(),
        &mut |event| {
            if let qt_core::provider::StreamEvent::TextDelta { delta, .. } = event {
                deltas.push_str(&delta);
            }
        },
    )
    .await;
    assert!(
        matches!(message.stop_reason, StopReason::Stop | StopReason::Length),
        "{message:?}"
    );
    assert!(!message.text().is_empty());
    assert_eq!(deltas, message.text());
    assert!(message.usage.input_tokens > 0);
    assert!(message.usage.output_tokens > 0 && message.usage.output_tokens <= 24);

    // Greedy decoding is deterministic, and the second call reuses the
    // cached prompt prefix.
    let generate =
        make_generate_request(runtime.settings(), &request, &GenerateOptions::default()).unwrap();
    let again = runtime.generate(generate, InstanceTag::Chat).unwrap();
    assert_eq!(again.text, message.text());
    assert!(again.metadata["kvCache"]["reusedTokens"].as_u64().unwrap() > 0);
    assert_eq!(again.metadata["kvCache"]["retained"], true);

    let status = runtime.runtime_status(InstanceTag::Chat).unwrap();
    assert!(status.model_loaded && status.context_loaded);
    assert_eq!(status.loaded_model_id.as_deref(), Some("smol"));
    assert_eq!(status.context_size, 2048);
    assert!(status.model_size_bytes > 0);
    assert!(status.sequence_position > 0);
    assert!(status.memory["devices"].is_array());
    assert!(status.metadata["offload"]["selectedGpuLayers"].is_number());
    eprintln!("offload: {}", status.metadata["offload"]);

    let kv = runtime
        .kv_cache_control(KvCacheOperation::Inspect { seq_id: 0 }, InstanceTag::Chat)
        .unwrap();
    assert!(kv.position_max >= 0);
    assert_eq!(kv.sequence_position, status.sequence_position);
    let cleared = runtime
        .kv_cache_control(KvCacheOperation::Clear, InstanceTag::Chat)
        .unwrap();
    assert!(cleared.mutated);
    assert_eq!(cleared.sequence_position, 0);
    assert_eq!(cleared.position_max, -1);
}

#[tokio::test]
async fn grammar_constrains_output_and_cancellation_aborts() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let Some(mut settings) = chat_settings() else {
        return;
    };
    settings.kv_cache.policy = Some("turn".into());
    let runtime = LlamaRuntime::new(settings);
    let options = GenerateOptions {
        request_metadata: json!({"grammar": "root ::= \"yes\" | \"no\""}),
        ..Default::default()
    };
    let message = qt_llama::stream_with_options(
        &runtime,
        &user("Is the sky blue?"),
        &options,
        &CancellationToken::new(),
        &mut |_| {},
    )
    .await;
    assert!(
        matches!(message.text().as_str(), "yes" | "no"),
        "{message:?}"
    );
    assert_eq!(message.stop_reason, StopReason::Stop);

    let bad = GenerateOptions {
        request_metadata: json!({"grammar": "root ::= ("}),
        ..Default::default()
    };
    let failed = qt_llama::stream_with_options(
        &runtime,
        &user("x"),
        &bad,
        &CancellationToken::new(),
        &mut |_| {},
    )
    .await;
    assert_eq!(failed.stop_reason, StopReason::Error);
    assert_eq!(
        failed.error_message.as_deref(),
        Some("failed to create llama sampler: grammar")
    );

    let cancel = CancellationToken::new();
    let trigger = cancel.clone();
    let long = GenerateOptions {
        max_output_tokens: Some(200),
        temperature: Some(0.8),
        ..Default::default()
    };
    let aborted = qt_llama::stream_with_options(
        &runtime,
        &user("Write a long story."),
        &long,
        &cancel,
        &mut |event| {
            if matches!(event, qt_core::provider::StreamEvent::TextDelta { .. }) {
                trigger.cancel();
            }
        },
    )
    .await;
    assert_eq!(aborted.stop_reason, StopReason::Aborted);
    assert!(!aborted.text().is_empty());
}

#[test]
fn embeddings_are_normalised_and_semantic() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let Some(mut model) = model("bge-small-en-v1.5-q8_0.gguf", "bge") else {
        return;
    };
    model.context_window = Some(512);
    let mut settings = LlamaSettings {
        models: vec![model],
        models_configured: true,
        ..Default::default()
    };
    settings.embeddings.pooling = Some("cls".into());
    let runtime = LlamaRuntime::new(settings);
    let embed = |text: &str| {
        let mut request = EmbeddingRequest::new("bge", text);
        request.queue_timeout = std::time::Duration::from_secs(60);
        runtime.embed(request, InstanceTag::Embed).unwrap()
    };
    let cat = embed("A cat sleeps on the sofa.");
    let kitten = embed("A kitten naps on the couch.");
    let market = embed("Quarterly bond yields rose sharply.");
    assert_eq!(cat.embedding.len(), 384);
    assert_eq!(cat.metadata["pooling"], "cls");
    assert_eq!(cat.metadata["normalized"], true);
    assert_eq!(cat.metadata["tag"], "embed");
    let dot = |a: &[f32], b: &[f32]| a.iter().zip(b).map(|(x, y)| x * y).sum::<f32>();
    assert!((dot(&cat.embedding, &cat.embedding) - 1.0).abs() < 1e-3);
    assert!(dot(&cat.embedding, &kitten.embedding) > dot(&cat.embedding, &market.embedding));
    let status = runtime.runtime_status(InstanceTag::Embed).unwrap();
    assert!(status.embeddings_enabled);
}

#[test]
fn discovery_reads_real_gguf_headers() {
    let dir = models_dir();
    let bonsai = dir.join("Ternary-Bonsai-27B-Q2_0.gguf");
    if !bonsai.is_file() {
        eprintln!("skipping: {} not found", bonsai.display());
        return;
    }
    let model = inspect_model_file(&bonsai);
    assert!(model.warnings.is_empty(), "{:?}", model.warnings);
    assert_eq!(model.gguf_file_type, Some(41));
    assert_eq!(model.quantization, "Q2_0");
    assert_eq!(model.capability, ModelCapability::Chat);
    assert!(model.block_count.is_some());

    let result = discover_models(&DiscoveryOptions {
        roots: vec![dir],
        recursive: false,
    });
    let bge = result
        .models
        .iter()
        .find(|m| m.path.ends_with("bge-small-en-v1.5-q8_0.gguf"));
    if let Some(bge) = bge {
        assert_eq!(bge.capability, ModelCapability::Embedding);
        assert_eq!(bge.quantization, "Q8_0");
        assert_eq!(bge.quantization_preference, "recommended_default");
        assert_eq!(bge.context_length, Some(512));
    }
}
