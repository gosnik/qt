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


def run_probe(binary: pathlib.Path, runtime: pathlib.Path) -> None:
    from tui_pty_probe import TerminalProbe, extract_scroll_margin_lines

    agent_home = runtime / "agent-home"
    workspace = runtime / "workspace"
    shutil.rmtree(runtime, ignore_errors=True)
    agent_home.mkdir(parents=True)
    workspace.mkdir(parents=True)

    server_state = {"requests": 0}

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args: object) -> None:
            return

        def do_POST(self) -> None:  # noqa: N802
            size = int(self.headers.get("content-length", "0"))
            self.rfile.read(size)
            server_state["requests"] += 1
            marker = ("FINAL_PROBE_ONE" if server_state["requests"] == 1
                      else "FINAL_PROBE_TWO")
            chunks = [
                {"choices": [{"delta": {"role": "assistant", "content": marker},
                              "finish_reason": None}]},
                {"choices": [{"delta": {}, "finish_reason": "stop"}]},
            ]
            payload = "".join("data: " + json.dumps(chunk) + "\n\n"
                              for chunk in chunks) + "data: [DONE]\n\n"
            body = payload.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            self.wfile.flush()

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
    finally:
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
