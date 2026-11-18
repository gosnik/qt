//! The RPC output stream: one JSON object per LF-terminated line on stdout
//! (Pi `serializeJsonLine`), shared by responses, agent events and
//! extension UI requests, plus the run pump that turns each run's
//! [`AgentEvent`] stream into Pi event lines.

use std::io::Write;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use async_channel::{Receiver, Sender};
use serde_json::Value;

use qt_core::agent::AgentEvent;
use qt_core::events_json::PiEventSerializer;

/// Thread-safe JSONL writer.
#[derive(Clone)]
pub struct RpcOutput {
    writer: Arc<Mutex<Box<dyn Write + Send>>>,
    broken: Arc<AtomicBool>,
}

impl RpcOutput {
    pub fn new(writer: Box<dyn Write + Send>) -> Self {
        Self {
            writer: Arc::new(Mutex::new(writer)),
            broken: Arc::new(AtomicBool::new(false)),
        }
    }

    pub fn stdout() -> Self {
        Self::new(Box::new(std::io::stdout()))
    }

    /// Write `value` as one line and flush. A failed write (closed pipe)
    /// marks the stream broken; later writes are dropped.
    pub fn write(&self, value: &Value) {
        if self.broken.load(Ordering::SeqCst) {
            return;
        }
        let line = format!("{value}\n");
        let mut writer = self.writer.lock().expect("rpc output lock");
        if writer
            .write_all(line.as_bytes())
            .and_then(|()| writer.flush())
            .is_err()
        {
            self.broken.store(true, Ordering::SeqCst);
        }
    }

    pub fn is_broken(&self) -> bool {
        self.broken.load(Ordering::SeqCst)
    }
}

/// Agent events in Pi's JSON shape, with the run state RPC reports
/// (`isCompacting`, an auto-retry in progress).
pub struct EventStream {
    output: RpcOutput,
    serializer: Mutex<PiEventSerializer>,
    compacting: AtomicBool,
    retrying: AtomicBool,
}

impl EventStream {
    pub fn new(output: RpcOutput) -> Self {
        Self {
            output,
            serializer: Mutex::new(PiEventSerializer::new()),
            compacting: AtomicBool::new(false),
            retrying: AtomicBool::new(false),
        }
    }

    /// Provider, model and API recorded on streamed partial messages.
    pub fn set_model(&self, provider: &str, model: &str, api: &str) {
        self.serializer
            .lock()
            .expect("serializer lock")
            .set_model(provider, model, api);
    }

    /// Write the Pi event lines of `event`.
    pub fn emit(&self, event: &AgentEvent) {
        match event {
            AgentEvent::CompactionStart { .. } => self.compacting.store(true, Ordering::SeqCst),
            AgentEvent::CompactionEnd { .. } => self.compacting.store(false, Ordering::SeqCst),
            AgentEvent::RetryStart { .. } => self.retrying.store(true, Ordering::SeqCst),
            AgentEvent::RetryEnd { .. } | AgentEvent::AgentEnd { .. } => {
                self.retrying.store(false, Ordering::SeqCst)
            }
            _ => {}
        }
        let mut serializer = self.serializer.lock().expect("serializer lock");
        for value in serializer.serialize(event) {
            self.output.write(&value);
        }
    }

    /// Write a non-agent event (`queue_update` while idle,
    /// `extension_error`, `session_info_changed`, `subagent_event`).
    pub fn emit_value(&self, value: &Value) {
        let _serializer = self.serializer.lock().expect("serializer lock");
        self.output.write(value);
    }

    pub fn is_compacting(&self) -> bool {
        self.compacting.load(Ordering::SeqCst)
    }

    /// Whether an auto-retry wait (or the retried request) is in progress.
    pub fn is_retrying(&self) -> bool {
        self.retrying.load(Ordering::SeqCst)
    }
}

/// Supplies `(provider, model, api)` for the run about to be pumped.
pub type ModelSource = Arc<dyn Fn() -> Option<(String, String, String)> + Send + Sync>;

/// Drains run event streams one after another (runs never overlap) into
/// the [`EventStream`].
#[derive(Clone)]
pub struct RunPump {
    runs: Sender<Receiver<AgentEvent>>,
    inflight: Arc<AtomicUsize>,
}

impl RunPump {
    pub fn start(events: Arc<EventStream>, model: ModelSource) -> Self {
        let (runs, queue) = async_channel::unbounded::<Receiver<AgentEvent>>();
        let inflight = Arc::new(AtomicUsize::new(0));
        let pending = inflight.clone();
        std::thread::Builder::new()
            .name("rpc-events".into())
            .spawn(move || {
                while let Ok(run) = queue.recv_blocking() {
                    if let Some((provider, id, api)) = model() {
                        events.set_model(&provider, &id, &api);
                    }
                    while let Ok(event) = run.recv_blocking() {
                        events.emit(&event);
                    }
                    pending.fetch_sub(1, Ordering::SeqCst);
                }
            })
            .expect("spawn rpc event thread");
        Self { runs, inflight }
    }

    /// Queue a run's events.
    pub fn submit(&self, run: Receiver<AgentEvent>) {
        self.inflight.fetch_add(1, Ordering::SeqCst);
        if self.runs.try_send(run).is_err() {
            self.inflight.fetch_sub(1, Ordering::SeqCst);
        }
    }

    /// Runs whose events have not been written completely.
    pub fn pending(&self) -> usize {
        self.inflight.load(Ordering::SeqCst)
    }

    /// Wait until every submitted run was written, at most `timeout`.
    pub fn wait_idle(&self, timeout: Duration) -> bool {
        let deadline = Instant::now() + timeout;
        while self.pending() > 0 {
            if Instant::now() >= deadline {
                return false;
            }
            std::thread::sleep(Duration::from_millis(5));
        }
        true
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use qt_core::message::{AssistantMessage, ContentBlock, Message, StopReason, Usage};

    /// A writer that collects lines for assertions.
    #[derive(Clone, Default)]
    pub struct Captured(pub Arc<Mutex<Vec<u8>>>);

    impl Write for Captured {
        fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
            self.0.lock().unwrap().extend_from_slice(buf);
            Ok(buf.len())
        }
        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }

    impl Captured {
        pub fn lines(&self) -> Vec<Value> {
            String::from_utf8(self.0.lock().unwrap().clone())
                .unwrap()
                .lines()
                .map(|l| serde_json::from_str(l).unwrap())
                .collect()
        }
    }

    struct Broken;
    impl Write for Broken {
        fn write(&mut self, _: &[u8]) -> std::io::Result<usize> {
            Err(std::io::ErrorKind::BrokenPipe.into())
        }
        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }

    #[test]
    fn lines_are_lf_terminated_json() {
        let captured = Captured::default();
        let output = RpcOutput::new(Box::new(captured.clone()));
        output.write(&serde_json::json!({"a": "line\u{2028}sep"}));
        let text = String::from_utf8(captured.0.lock().unwrap().clone()).unwrap();
        assert!(text.ends_with('\n'));
        assert_eq!(text.matches('\n').count(), 1);
        assert_eq!(captured.lines()[0]["a"], "line\u{2028}sep");
        let broken = RpcOutput::new(Box::new(Broken));
        broken.write(&serde_json::json!({}));
        assert!(broken.is_broken());
    }

    #[test]
    fn pump_writes_runs_in_order_and_tracks_state() {
        let captured = Captured::default();
        let events = Arc::new(EventStream::new(RpcOutput::new(Box::new(captured.clone()))));
        let pump = RunPump::start(
            events.clone(),
            Arc::new(|| Some(("p".into(), "m".into(), "openai-completions".into()))),
        );
        let (tx, rx) = async_channel::unbounded();
        pump.submit(rx);
        tx.try_send(AgentEvent::AgentStart).unwrap();
        tx.try_send(AgentEvent::RetryStart {
            attempt: 1,
            max_attempts: 3,
            delay_ms: 10,
            error: "overloaded".into(),
        })
        .unwrap();
        // The retry flag follows the events as they are written.
        let deadline = Instant::now() + Duration::from_secs(5);
        while !events.is_retrying() && Instant::now() < deadline {
            std::thread::sleep(Duration::from_millis(2));
        }
        assert!(events.is_retrying());
        tx.try_send(AgentEvent::MessageEnd(Message::Assistant(
            AssistantMessage {
                content: vec![ContentBlock::text("done")],
                stop_reason: StopReason::Stop,
                ..Default::default()
            },
        )))
        .unwrap();
        tx.try_send(AgentEvent::AgentEnd {
            usage: Usage::default(),
            aborted: false,
        })
        .unwrap();
        drop(tx);
        assert!(pump.wait_idle(Duration::from_secs(5)));
        assert!(!events.is_retrying());
        let types: Vec<String> = captured
            .lines()
            .iter()
            .map(|l| l["type"].as_str().unwrap().to_string())
            .collect();
        assert_eq!(
            types,
            [
                "agent_start",
                "auto_retry_start",
                "message_start",
                "message_end",
                "agent_end"
            ]
        );
        let end = &captured.lines()[3];
        assert_eq!(end["message"]["content"][0]["text"], "done");
        assert_eq!(captured.lines()[2]["message"]["provider"], "p");
    }
}
