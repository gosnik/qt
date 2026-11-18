//! `--mode rpc` (archived `rpc_mode.cpp` / `rpc_server.cpp`, Pi
//! `rpc-mode.ts`): JSON commands arrive one per LF-terminated line on
//! stdin; responses, agent events and extension UI requests leave as JSON
//! lines on stdout.
//!
//! The input thread decodes every line. `extension_ui_response` frames are
//! routed to the waiting dialog at once, and `abort`, `abort_bash` and
//! `abort_retry` are answered without waiting; every other command runs in
//! order on a worker thread. A `prompt` is acknowledged as soon as its run
//! starts (or, for an extension command, when the command finishes) and the
//! run's events are streamed by the event thread. When stdin closes, open
//! dialogs resolve to their defaults, queued commands finish and the
//! process waits for the active run before shutting down.

pub mod models;
pub mod output;
pub mod protocol;
pub mod session;
pub mod ui;

use std::collections::HashSet;
use std::io::BufRead;
use std::sync::Arc;
use std::sync::mpsc::Sender;
use std::time::Duration;

use qt_runtime::{Runtime, RuntimeOptions};

use crate::args::CliArgs;
use crate::session::StartupStop;
use output::RpcOutput;
use protocol::{Frame, Request, RpcError};
use session::RpcSession;

/// How long shutdown waits for memory capture of the finished runs.
const CAPTURE_FLUSH: Duration = Duration::from_secs(30);

/// Read one LF-delimited record (without the LF); `None` at end of input.
/// Only `\n` separates records, so U+2028 / U+2029 inside JSON strings are
/// kept (Pi `attachJsonlLineReader`); a trailing `\r` is removed when the
/// frame is decoded.
pub fn read_frame(input: &mut impl BufRead) -> std::io::Result<Option<String>> {
    let mut bytes = Vec::new();
    if input.read_until(b'\n', &mut bytes)? == 0 {
        return Ok(None);
    }
    if bytes.last() == Some(&b'\n') {
        bytes.pop();
    }
    Ok(Some(String::from_utf8_lossy(&bytes).into_owned()))
}

/// Decode one line and route it.
fn accept(
    session: &Arc<RpcSession>,
    line: &str,
    seen: &mut HashSet<String>,
    worker: &Sender<Request>,
) {
    match protocol::decode_line(line) {
        Err(error) => {
            if let Some(id) = &error.id {
                seen.insert(id.clone());
            }
            session.respond(
                error.id.as_deref(),
                &error.command,
                Err(RpcError::internal(error.message)),
            );
        }
        Ok(Frame::UiResponse { id, payload }) => {
            if let Err(message) = session.ui().resolve(&id, payload) {
                session.respond(None, "invalid", Err(RpcError::internal(message)));
            }
        }
        Ok(Frame::Command(request)) => {
            if let Some(id) = &request.id
                && !seen.insert(id.clone())
            {
                session.respond(
                    None,
                    "invalid",
                    Err(RpcError::internal(format!(
                        "duplicate rpc request id: {id}"
                    ))),
                );
                return;
            }
            if let Some(request) = session.handle_interrupt(request) {
                let _ = worker.send(request);
            }
        }
    }
}

/// Serve `input` until it ends. Returns the exit code.
pub fn serve(session: Arc<RpcSession>, mut input: impl BufRead) -> i32 {
    let (tx, rx) = std::sync::mpsc::channel::<Request>();
    let worker = {
        let session = session.clone();
        std::thread::Builder::new()
            .name("rpc-commands".into())
            .spawn(move || {
                for request in rx {
                    session.handle(request);
                }
            })
            .expect("spawn rpc worker")
    };
    let mut seen = HashSet::new();
    let mut failed = false;
    loop {
        match read_frame(&mut input) {
            Ok(Some(line)) => accept(&session, &line, &mut seen, &tx),
            Ok(None) => break,
            Err(error) if error.kind() == std::io::ErrorKind::Interrupted => continue,
            Err(_) => {
                eprintln!("qt: failed to read rpc input");
                failed = true;
                break;
            }
        }
    }
    session.ui().close();
    drop(tx);
    let _ = worker.join();
    session.wait_quiescent();
    if failed { 1 } else { 0 }
}

/// `qt-cli --mode rpc`.
pub fn run(parsed: CliArgs, options: RuntimeOptions) -> i32 {
    let runtime = match Runtime::build(options) {
        Ok(r) => r,
        Err(e) => {
            eprintln!("Error: {e:#}");
            return 1;
        }
    };
    for d in runtime.diagnostics() {
        tracing::warn!(target: "qt_cli", "{d}");
    }
    let conversation =
        match crate::session::open(runtime.engine(), &parsed, runtime.startup_model(), false) {
            Ok(c) => c,
            Err(StartupStop::Message(text, code)) => {
                if !text.is_empty() {
                    eprintln!("{text}");
                }
                runtime.shutdown(CAPTURE_FLUSH);
                return code;
            }
            Err(StartupStop::Error(text)) => {
                eprintln!("{text}");
                runtime.shutdown(CAPTURE_FLUSH);
                return 1;
            }
        };
    runtime.apply_startup_selection(&conversation);
    let session = RpcSession::start(runtime.clone(), conversation, RpcOutput::stdout());
    runtime.connect_mcp();
    tracing::info!(target: "qt_cli", "rpc_ready");
    let code = serve(session, std::io::stdin().lock());
    runtime.shutdown(CAPTURE_FLUSH);
    code
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn frames_split_on_lf_only() {
        let data = "{\"a\":\"x\u{2028}y\"}\r\n\n{\"b\":1}".as_bytes();
        let mut reader = std::io::BufReader::new(data);
        assert_eq!(
            read_frame(&mut reader).unwrap().as_deref(),
            Some("{\"a\":\"x\u{2028}y\"}\r")
        );
        assert_eq!(read_frame(&mut reader).unwrap().as_deref(), Some(""));
        // A final record without LF is still read.
        assert_eq!(
            read_frame(&mut reader).unwrap().as_deref(),
            Some("{\"b\":1}")
        );
        assert_eq!(read_frame(&mut reader).unwrap(), None);
    }
}
