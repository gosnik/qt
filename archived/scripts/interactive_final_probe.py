#!/usr/bin/env python3
"""Exercise streamed assistant output and check transcript scrollback copies."""

from __future__ import annotations

import argparse
import fcntl
import http.server
import json
import os
import pathlib
import shutil
import struct
import termios
import threading
import signal
import time


def run_probe(binary: pathlib.Path, runtime: pathlib.Path) -> None:
    from tui_pty_probe import TerminalProbe, extract_scroll_margin_lines

    agent_home = runtime / "agent-home"
    workspace = runtime / "workspace"
    shutil.rmtree(runtime, ignore_errors=True)
    agent_home.mkdir(parents=True)
    workspace.mkdir(parents=True)

    server_state = {"requests": 0, "paste": None}
    delayed_started = threading.Event()
    delayed_release = threading.Event()
    (workspace / "tool-loop-fixture.txt").write_text("TOOL_PAYLOAD\n")

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args: object) -> None:
            return

        def do_POST(self) -> None:  # noqa: N802
            size = int(self.headers.get("content-length", "0"))
            request = json.loads(self.rfile.read(size))
            messages = request.get("messages", [])
            user = next((m.get("content", "") for m in reversed(messages)
                         if m.get("role") == "user"), "")
            if isinstance(user, list):
                user = "".join(item.get("text", "") for item in user)
            if user == "delayed shutdown probe":
                delayed_started.set()
                delayed_release.wait(10)
            server_state["requests"] += 1
            marker = ("FINAL_PROBE_ONE" if server_state["requests"] == 1
                      else "FINAL_PROBE_TWO")
            chunks = [
                {"choices": [{"delta": {"role": "assistant", "content": marker},
                              "finish_reason": None}]},
                {"choices": [{"delta": {}, "finish_reason": "stop"}]},
            ]
            if user == "tool loop probe":
                if any(m.get("role") == "tool" for m in messages):
                    chunks[0]["choices"][0]["delta"]["content"] = "AFTER_TOOL"
                else:
                    chunks[0]["choices"][0]["delta"]["content"] = "BEFORE_TOOL"
                    chunks.insert(1, {"choices": [{"index": 0, "delta": {
                        "tool_calls": [{"index": 0, "id": "tui-read", "type": "function",
                        "function": {"name": "read", "arguments": json.dumps({
                            "path": "tool-loop-fixture.txt"})}}]}, "finish_reason": None}]})
                    chunks[-1]["choices"][0]["finish_reason"] = "tool_calls"
            if user.startswith("CRLF_INPUT_PROBE_A"):
                server_state["paste"] = user
                chunks[0]["choices"][0]["delta"]["content"] = "PASTE_DONE"
            payload = "".join("data: " + json.dumps(chunk) + "\n\n"
                              for chunk in chunks) + "data: [DONE]\n\n"
            body = payload.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            try:
                self.wfile.write(body)
                self.wfile.flush()
            except OSError:
                pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    port = server.server_address[1]
    (agent_home / "models.json").write_text(
        json.dumps({"providers": {"probe": {
            "api": "openai-completions",
            "baseUrl": f"http://127.0.0.1:{port}/v1",
            "apiKey": "probe-key",
            "models": [{"id": "probe-model"}],
        }}}) + "\n", encoding="utf-8")
    (agent_home / "settings.json").write_text(
        json.dumps({"defaultProvider": "probe", "defaultModel": "probe-model"}) + "\n",
        encoding="utf-8")

    previous_home = os.environ.get("QT_CODING_AGENT_DIR")
    previous_cwd = pathlib.Path.cwd()
    os.environ["QT_CODING_AGENT_DIR"] = str(agent_home)
    os.chdir(workspace)
    try:
        with TerminalProbe([str(binary)], 20, 120, 12.0) as term:
            if not term.read_until(lambda _raw, text: "probe-model" in text,
                                   timeout=8.0):
                raise RuntimeError("interactive prompt did not become ready\n" +
                                   bytes(term.output[-5000:]).decode("utf-8", errors="replace"))
            term.read_until(lambda _raw, _text: False, timeout=0.75)
            term.write(b"first probe\r")
            if not term.read_until(lambda _raw, text: "FINAL_PROBE_ONE" in text,
                                   timeout=8.0):
                raise RuntimeError("first streamed marker was not rendered\n" +
                                   bytes(term.output[-5000:]).decode("utf-8", errors="replace"))
            if not term.read_until(
                    lambda raw, _text: "FINAL_PROBE_ONE" in "\n".join(
                        extract_scroll_margin_lines(raw, 20)),
                    timeout=5.0):
                raise RuntimeError("first marker was not committed to scrollback")
            fcntl.ioctl(term.master, termios.TIOCSWINSZ,
                        struct.pack("HHHH", 18, 100, 0, 0))
            term.read_until(lambda _raw, _text: False, timeout=0.5)
            term.write(b"second probe\r")
            if not term.read_until(lambda _raw, text: "FINAL_PROBE_TWO" in text,
                                   timeout=8.0):
                raise RuntimeError("second streamed marker was not rendered")
            if not term.read_until(
                    lambda raw, _text: "FINAL_PROBE_TWO" in "\n".join(
                        extract_scroll_margin_lines(raw, 20)),
                    timeout=5.0):
                raise RuntimeError("second marker was not committed to scrollback")
            term.write(b"tool loop probe\r")
            if not term.read_until(lambda raw, _text: "AFTER_TOOL" in "\n".join(
                    extract_scroll_margin_lines(raw, 20)), timeout=8.0):
                raise RuntimeError("tool-loop output was not committed")
            scrollback = "\n".join(extract_scroll_margin_lines(bytes(term.output), 20))
            positions = [scrollback.find(marker) for marker in
                         ("BEFORE_TOOL", "TOOL_PAYLOAD", "AFTER_TOOL")]
            if min(positions) < 0 or positions != sorted(positions):
                raise RuntimeError("tool-loop transcript lost or reordered output: " + scrollback)
            term.write(b"\x1b[200~CRLF_INPUT_PROBE_A\r\nCRLF_INPUT_PROBE_B\x1b[201~\r")
            if not term.read_until(lambda _raw, text: "PASTE_DONE" in text, timeout=8):
                raise RuntimeError("CRLF paste did not complete")
            if server_state["paste"] != "CRLF_INPUT_PROBE_A\nCRLF_INPUT_PROBE_B":
                raise RuntimeError("CRLF paste gained a blank line: " + repr(server_state["paste"]))
            term.write(b"/quit\r")
            term.read_until(lambda _raw, _text: term.proc is not None and
                            term.proc.poll() is not None, timeout=5.0)
            scrollback = "\n".join(
                extract_scroll_margin_lines(bytes(term.output), 20))
            for marker in ("FINAL_PROBE_ONE", "FINAL_PROBE_TWO"):
                count = scrollback.count(marker)
                if count != 1:
                    raise RuntimeError(
                        f"{marker} appeared {count} times in logical scrollback")
        for exit_kind in ("ctrl-d", "sigterm", "recoverable-input-error"):
            delayed_started.clear()
            delayed_release.clear()
            with TerminalProbe([str(binary)], 20, 120, 12.0) as term:
                if not term.read_until(lambda _raw, text: "probe-model" in text, timeout=8):
                    raise RuntimeError("shutdown prompt did not become ready")
                term.write(b"delayed shutdown probe\r")
                if not term.read_until(lambda _raw, _text: delayed_started.is_set(), timeout=5):
                    raise RuntimeError("delayed request did not start")
                if exit_kind == "recoverable-input-error":
                    term.write(b"\xff")
                    if not term.read_until(lambda _raw, text: "invalid utf-8 lead byte" in text,
                                           timeout=2):
                        raise RuntimeError("invalid input did not exercise recoverable rendering")
                    if term.proc.poll() is not None:
                        raise RuntimeError("recoverable editor error terminated the app")
                started = time.monotonic()
                if exit_kind in ("sigterm", "recoverable-input-error"):
                    term.proc.send_signal(signal.SIGTERM)
                else:
                    term.write(b"\x04")
                if not term.read_until(lambda _raw, _text: term.proc.poll() is not None, timeout=3):
                    raise RuntimeError("shutdown waited for the delayed provider (" + exit_kind + ")\n" + bytes(term.output[-2500:]).decode("utf-8", errors="replace"))
                flags = termios.tcgetattr(term.master)[3]
                if flags & (termios.ICANON | termios.ECHO) != (termios.ICANON | termios.ECHO):
                    raise RuntimeError("shutdown did not restore terminal input state")
                if time.monotonic() - started > 3:
                    raise RuntimeError("shutdown exceeded cancellation deadline")
                delayed_release.set()
        with TerminalProbe([str(binary)], 20, 120, 12.0) as term:
            if not term.read_until(lambda _raw, text: "probe-model" in text, timeout=8):
                raise RuntimeError("bash shutdown prompt did not become ready")
            pid_path = workspace / "bash-exit.pid"
            term.write(b"!echo $$ > bash-exit.pid; sleep 30\r")
            if not term.read_until(lambda _raw, _text: pid_path.is_file() and
                                   bool(pid_path.read_text().strip()), timeout=5):
                raise RuntimeError("asynchronous bash command did not start")
            bash_pid = int(pid_path.read_text().strip())
            term.proc.send_signal(signal.SIGTERM)
            if not term.read_until(lambda _raw, _text: term.proc.poll() is not None, timeout=3):
                raise RuntimeError("shutdown waited for asynchronous bash")
            flags = termios.tcgetattr(term.master)[3]
            if flags & (termios.ICANON | termios.ECHO) != (termios.ICANON | termios.ECHO):
                raise RuntimeError("bash shutdown did not restore terminal input state")
            try:
                os.kill(bash_pid, 0)
            except ProcessLookupError:
                pass
            else:
                raise RuntimeError("shutdown left the bash process alive")
    finally:
        delayed_release.set()
        os.chdir(previous_cwd)
        if previous_home is None:
            os.environ.pop("QT_CODING_AGENT_DIR", None)
        else:
            os.environ["QT_CODING_AGENT_DIR"] = previous_home
        server.shutdown()
        server.server_close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default=".pio/build/native/qt")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    binary = pathlib.Path(args.binary)
    if not binary.is_absolute():
        binary = (root / binary).resolve()
    if not binary.is_file():
        raise RuntimeError("native qt binary is missing; run pio run -e native")
    runtime = root / ".pio" / "test" / "interactive-final-probe"
    run_probe(binary, runtime)
    print("interactive final-output probe passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
