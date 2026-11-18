//! `stream`: run a qt-core [`Request`] on the in-process llama runtime.
//!
//! Shaped like `qt_core::provider::stream`: text streams as
//! [`StreamEvent::TextDelta`]s, the call returns a complete
//! [`AssistantMessage`], failures become `stopReason: "error"` and
//! cancellation becomes `stopReason: "aborted"` with the text produced so
//! far. When tools are offered and the reply contains a tool call in the
//! archived JSON protocol, the final message holds only that tool call.

use qt_core::message::{AssistantMessage, ContentBlock, StopReason, Usage, now_millis};
use qt_core::provider::{Request, StreamEvent};
use serde_json::{Value, json};
use tokio_util::sync::CancellationToken;

use crate::LlamaError;
use crate::prompt::{parse_tool_call, serialize_prompt};
use crate::runtime::{
    CancelFlag, GenerateRequest, GenerateResult, GenerateStop, InstanceTag, LlamaRuntime,
};
pub use crate::sampling::GenerateOptions;
use crate::sampling::{
    SamplerStep, effective_generation, resolve_grammar, resolved_max_output_tokens, sampler_plan,
};
use crate::settings::{LlamaSettings, PROVIDER_API, PROVIDER_ID};

/// Build the backend request for `request` (prompt, sampler chain, limits).
pub fn make_generate_request(
    settings: &LlamaSettings,
    request: &Request,
    options: &GenerateOptions,
) -> Result<GenerateRequest, LlamaError> {
    let model = settings
        .model(&request.model)
        .ok_or_else(|| LlamaError::UnknownModel(request.model.clone()))?;
    let mut generation = effective_generation(settings, model, options);
    if generation.max_output_tokens.is_none() {
        generation.max_output_tokens = model.max_tokens;
    }
    let mut max_output_tokens = resolved_max_output_tokens(&generation);
    if options.max_output_tokens.is_none() && request.max_tokens > 0 {
        max_output_tokens = max_output_tokens.min(i64::from(request.max_tokens));
    }
    generation.max_output_tokens = Some(max_output_tokens);

    let mut plan = sampler_plan(&generation);
    let mut metadata = json!({
        "request": options.request_metadata,
        "options": options.options_metadata,
    });
    let grammar = resolve_grammar(settings, options)?;
    if grammar.enabled {
        metadata["grammar"] = json!({"root": grammar.root, "source": grammar.source});
        plan.insert(
            0,
            SamplerStep::Grammar {
                grammar: grammar.grammar,
                root: grammar.root,
                source: grammar.source,
            },
        );
    }
    Ok(GenerateRequest {
        model_id: model.id.clone(),
        prompt: serialize_prompt(request),
        generation,
        kv_cache: settings.kv_cache.clone(),
        sampler_plan: plan,
        max_output_tokens,
        metadata,
    })
}

/// Response metadata in the archived shape (`provider`, `api`, `model`,
/// `generation`, `kvCache`, `grammar`, `samplerPlan`, backend fields).
pub fn response_metadata(request: &GenerateRequest, result: &GenerateResult) -> Value {
    let mut metadata = if result.metadata.is_object() {
        result.metadata.clone()
    } else {
        json!({})
    };
    metadata["provider"] = json!(PROVIDER_ID);
    metadata["api"] = json!(PROVIDER_API);
    metadata["model"] = json!(request.model_id);
    metadata["generation"] = request.generation.to_json();
    if let Some(state) = metadata.get("kvCache").cloned() {
        metadata["kvCacheState"] = state;
    }
    metadata["kvCache"] = request.kv_cache.to_json();
    if let Some(grammar) = request.metadata.get("grammar") {
        metadata["grammar"] = grammar.clone();
    }
    metadata["samplerPlan"] = Value::Array(
        request
            .sampler_plan
            .iter()
            .map(SamplerStep::to_json)
            .collect(),
    );
    metadata
}

enum Event {
    Token(String),
    Done(Result<GenerateResult, LlamaError>),
}

/// Sets the flag when the stream future is dropped, so the worker stops.
struct CancelOnDrop(CancelFlag);

impl Drop for CancelOnDrop {
    fn drop(&mut self) {
        self.0.cancel();
    }
}

/// Stream one assistant response from the llama runtime.
pub async fn stream(
    runtime: &LlamaRuntime,
    request: &Request,
    cancel: &CancellationToken,
    on_event: &mut (dyn FnMut(StreamEvent) + Send),
) -> AssistantMessage {
    stream_with_options(
        runtime,
        request,
        &GenerateOptions::default(),
        cancel,
        on_event,
    )
    .await
}

/// [`stream`] with metadata overrides (generation settings, grammar) and
/// explicit temperature / output limit.
pub async fn stream_with_options(
    runtime: &LlamaRuntime,
    request: &Request,
    options: &GenerateOptions,
    cancel: &CancellationToken,
    on_event: &mut (dyn FnMut(StreamEvent) + Send),
) -> AssistantMessage {
    let mut message = AssistantMessage {
        api: PROVIDER_API.into(),
        provider: PROVIDER_ID.into(),
        model: request.model.clone(),
        ..Default::default()
    };
    let finish_error = |mut message: AssistantMessage, error: String| {
        message.stop_reason = StopReason::Error;
        message.error_message = Some(error);
        message.timestamp = Some(now_millis());
        message
    };
    if cancel.is_cancelled() {
        message.stop_reason = StopReason::Aborted;
        message.timestamp = Some(now_millis());
        return message;
    }
    let generate = match make_generate_request(runtime.settings(), request, options) {
        Ok(generate) => generate,
        Err(err) => return finish_error(message, err.to_string()),
    };

    let (tx, mut rx) = tokio::sync::mpsc::unbounded_channel::<Event>();
    let flag = CancelFlag::default();
    let _guard = CancelOnDrop(flag.clone());
    let job_flag = flag.clone();
    let job_request = generate.clone();
    let enqueued = runtime.enqueue(move |state| {
        let token_tx = tx.clone();
        let result = state.generate(
            InstanceTag::Chat,
            &job_request,
            &mut |piece: &str| {
                let _ = token_tx.send(Event::Token(piece.to_string()));
            },
            &|| job_flag.is_cancelled(),
        );
        let _ = tx.send(Event::Done(result));
    });
    if let Err(err) = enqueued {
        return finish_error(message, err.to_string());
    }

    let mut text_index: Option<usize> = None;
    let outcome = loop {
        tokio::select! {
            event = rx.recv() => match event {
                Some(Event::Token(piece)) => {
                    if cancel.is_cancelled() {
                        // Keep the text seen before the abort; drain the rest.
                        flag.cancel();
                        continue;
                    }
                    if piece.is_empty() {
                        continue;
                    }
                    let index = *text_index.get_or_insert_with(|| {
                        let index = message.content.len();
                        message.content.push(ContentBlock::text(""));
                        on_event(StreamEvent::BlockStart { index, block: ContentBlock::text("") });
                        index
                    });
                    if let Some(ContentBlock::Text { text }) = message.content.get_mut(index) {
                        text.push_str(&piece);
                    }
                    on_event(StreamEvent::TextDelta { index, delta: piece });
                }
                Some(Event::Done(result)) => break result,
                None => break Err(LlamaError::backend("llama worker job failed")),
            },
            _ = cancel.cancelled(), if !flag.is_cancelled() => flag.cancel(),
        }
    };

    message.timestamp = Some(now_millis());
    let result = match outcome {
        Ok(result) => result,
        Err(_) if cancel.is_cancelled() => {
            message.stop_reason = StopReason::Aborted;
            return message;
        }
        Err(err) => return finish_error(message, err.to_string()),
    };
    message.usage = Usage {
        input_tokens: result.input_tokens,
        output_tokens: result.output_tokens,
        total_tokens: result.input_tokens + result.output_tokens,
        ..Default::default()
    };
    if result.stop == GenerateStop::Aborted || cancel.is_cancelled() {
        message.stop_reason = StopReason::Aborted;
        return message;
    }
    message.stop_reason = match result.stop {
        GenerateStop::MaxTokens => StopReason::Length,
        _ => StopReason::Stop,
    };
    if !request.tools.is_empty()
        && let Some(call) = parse_tool_call(&result.text, &request.tools)
    {
        let block = ContentBlock::ToolCall {
            id: call.id,
            name: call.name,
            arguments: call.arguments,
        };
        let index = message.content.len();
        on_event(StreamEvent::BlockStart {
            index,
            block: block.clone(),
        });
        if let ContentBlock::ToolCall { arguments, .. } = &block {
            on_event(StreamEvent::ToolArgsDelta {
                index,
                delta: arguments.to_string(),
            });
        }
        message.content = vec![block];
        message.stop_reason = StopReason::ToolCall;
    }
    message
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::runtime::tests::{model_file, runtime_with};
    use qt_core::message::{Message, UserContent};
    use qt_core::provider::ToolSpec;
    use std::time::Duration;

    fn request(model: &str, text: &str) -> Request {
        Request {
            model: model.into(),
            system: "sys".into(),
            messages: vec![Message::User {
                content: UserContent::Text(text.into()),
                timestamp: None,
            }],
            max_tokens: 16_384,
            ..Default::default()
        }
    }

    fn settings(dir: &std::path::Path) -> LlamaSettings {
        LlamaSettings {
            models: vec![model_file(dir, "m", 16)],
            ..Default::default()
        }
    }

    #[test]
    fn generate_request_layers_limits_and_grammar() {
        let dir = tempfile::tempdir().unwrap();
        let mut s = settings(dir.path());
        s.models[0].max_tokens = Some(64);
        s.grammar.default_grammar = Some("root ::= \"a\"".into());
        let g =
            make_generate_request(&s, &request("m", "hi"), &GenerateOptions::default()).unwrap();
        assert_eq!(g.max_output_tokens, 64);
        assert_eq!(g.sampler_plan[0].name(), "grammar");
        assert_eq!(
            g.metadata["grammar"],
            json!({"root": "root", "source": "default"})
        );
        assert!(g.prompt.ends_with("assistant: "));

        let mut small = request("m", "hi");
        small.max_tokens = 10;
        let g = make_generate_request(&s, &small, &GenerateOptions::default()).unwrap();
        assert_eq!(g.max_output_tokens, 10);

        s.models[0].max_tokens = None;
        let g =
            make_generate_request(&s, &request("m", "hi"), &GenerateOptions::default()).unwrap();
        assert_eq!(g.max_output_tokens, 256);
        let explicit = GenerateOptions {
            max_output_tokens: Some(1000),
            ..Default::default()
        };
        let mut req = request("m", "hi");
        req.max_tokens = 10;
        assert_eq!(
            make_generate_request(&s, &req, &explicit)
                .unwrap()
                .max_output_tokens,
            1000
        );
        assert_eq!(
            make_generate_request(&s, &request("x", "hi"), &GenerateOptions::default())
                .unwrap_err(),
            LlamaError::UnknownModel("x".into())
        );
        let meta = response_metadata(&g, &GenerateResult::default());
        assert_eq!(meta["provider"], "llama");
        assert_eq!(meta["api"], "llama.cpp");
        assert_eq!(meta["samplerPlan"][0]["name"], "grammar");
    }

    #[tokio::test]
    async fn streams_text() {
        let dir = tempfile::tempdir().unwrap();
        let (runtime, log) = runtime_with(settings(dir.path()), |_| {});
        let mut events = Vec::new();
        let cancel = CancellationToken::new();
        let message = stream(&runtime, &request("m", "hello"), &cancel, &mut |e| {
            events.push(e)
        })
        .await;
        assert_eq!(message.stop_reason, StopReason::Stop);
        assert_eq!(message.text(), "Hello world");
        assert_eq!(message.provider, "llama");
        assert_eq!(message.api, "llama.cpp");
        assert_eq!(message.usage.output_tokens, 2);
        assert!(message.timestamp.is_some());
        assert_eq!(
            events,
            vec![
                StreamEvent::BlockStart {
                    index: 0,
                    block: ContentBlock::text("")
                },
                StreamEvent::TextDelta {
                    index: 0,
                    delta: "Hello".into()
                },
                StreamEvent::TextDelta {
                    index: 0,
                    delta: " world".into()
                },
            ]
        );
        assert!(log.prompts.lock().unwrap()[0].contains("\"content\": \"hello\""));
    }

    #[tokio::test]
    async fn parses_tool_calls_and_length_stops() {
        let dir = tempfile::tempdir().unwrap();
        let (runtime, _) = runtime_with(settings(dir.path()), |b| {
            b.reply = vec![
                "{\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\",".into(),
                "\"function\":{\"name\":\"read\",\"arguments\":\"{\\\"path\\\":\\\"a\\\"}\"}}]}"
                    .into(),
            ];
        });
        let mut req = request("m", "read a");
        req.tools = vec![ToolSpec {
            name: "read".into(),
            description: "Read".into(),
            parameters: json!({"type": "object"}),
        }];
        let mut events = Vec::new();
        let message = stream(&runtime, &req, &CancellationToken::new(), &mut |e| {
            events.push(e)
        })
        .await;
        assert_eq!(message.stop_reason, StopReason::ToolCall);
        let calls: Vec<_> = message.tool_calls().collect();
        assert_eq!(calls.len(), 1);
        assert_eq!(calls[0].0, "call_1");
        assert_eq!(calls[0].2, &json!({"path": "a"}));
        assert_eq!(message.content.len(), 1);
        assert!(matches!(
            events.last(),
            Some(StreamEvent::ToolArgsDelta { index: 1, .. })
        ));

        let mut limited = request("m", "hi");
        limited.max_tokens = 1;
        let message = stream(&runtime, &limited, &CancellationToken::new(), &mut |_| {}).await;
        assert_eq!(message.stop_reason, StopReason::Length);
    }

    #[tokio::test]
    async fn errors_and_cancellation() {
        let dir = tempfile::tempdir().unwrap();
        let (runtime, _) = runtime_with(settings(dir.path()), |b| {
            b.reply = (0..10_000).map(|i| format!("t{i} ")).collect();
        });
        let message = stream(
            &runtime,
            &request("missing", "x"),
            &CancellationToken::new(),
            &mut |_| {},
        )
        .await;
        assert_eq!(message.stop_reason, StopReason::Error);
        assert_eq!(
            message.error_message.as_deref(),
            Some("unknown llama model: missing")
        );

        let cancel = CancellationToken::new();
        cancel.cancel();
        let message = stream(&runtime, &request("m", "x"), &cancel, &mut |_| {}).await;
        assert_eq!(message.stop_reason, StopReason::Aborted);

        let cancel = CancellationToken::new();
        let trigger = cancel.clone();
        let mut seen = 0usize;
        let message = stream(&runtime, &request("m", "x"), &cancel, &mut |e| {
            if matches!(e, StreamEvent::TextDelta { .. }) {
                seen += 1;
                if seen == 3 {
                    trigger.cancel();
                }
            }
        })
        .await;
        assert_eq!(message.stop_reason, StopReason::Aborted);
        assert_eq!(message.text(), "t0 t1 t2 ");

        // The runtime keeps working after an abort.
        let ok = tokio::time::timeout(
            Duration::from_secs(10),
            stream(
                &runtime,
                &request("m", "again"),
                &CancellationToken::new(),
                &mut |_| {},
            ),
        )
        .await
        .unwrap();
        // 10 000 scripted tokens exceed the default 256-token limit.
        assert_eq!(ok.stop_reason, StopReason::Length);
        assert_eq!(ok.usage.output_tokens, 256);
    }
}
