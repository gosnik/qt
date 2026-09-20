#!/usr/bin/env python3
"""Verify that interactive /context exposes complete available context."""

from __future__ import annotations

import argparse
import http.server
import json
import os
import pathlib
import shutil
import sys
import threading
import time


def run_probe(binary: pathlib.Path, runtime: pathlib.Path) -> None:
    root = pathlib.Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(root / "scripts"))
    from tui_pty_probe import TerminalProbe, extract_scroll_margin_lines

    shutil.rmtree(runtime, ignore_errors=True)
    agent_home = runtime / "agent-home"
    workspace = runtime / "workspace"
    agent_home.mkdir(parents=True)
    workspace.mkdir()
    source_marker = "CONTEXT_SOURCE_BEGIN_" + "X" * 12000 + "_CONTEXT_SOURCE_END"
    (workspace / "AGENTS.md").write_text(
        "Context source marker: " + source_marker + "\n", encoding="utf-8")

    server_state = {"requests": 0}

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args: object) -> None:
            return

        def do_POST(self) -> None:  # noqa: N802
            self.rfile.read(int(self.headers.get("content-length", "0")))
            server_state["requests"] += 1
            time.sleep(1.0)
            chunks = [
                {"choices": [{"delta": {"role": "assistant",
                                          "content": "CONTEXT_ASSISTANT_MARKER"},
                              "finish_reason": None}]},
                {"choices": [{"delta": {}, "finish_reason": "stop"}]},
            ]
            body = ("".join("data: " + json.dumps(chunk) + "\n\n"
                            for chunk in chunks) + "data: [DONE]\n\n").encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    port = server.server_address[1]
    (agent_home / "models.json").write_text(json.dumps({"providers": {"probe": {
        "api": "openai-completions", "baseUrl": f"http://127.0.0.1:{port}/v1",
        "apiKey": "probe-key", "models": [{"id": "probe-model", "contextWindow": 8192}],
    }}}) + "\n", encoding="utf-8")
    (agent_home / "settings.json").write_text(
        json.dumps({"defaultProvider": "probe", "defaultModel": "probe-model"}) + "\n",
        encoding="utf-8")

    previous_home, previous_cwd = os.environ.get("QT_CODING_AGENT_DIR"), pathlib.Path.cwd()
    os.environ["QT_CODING_AGENT_DIR"] = str(agent_home)
    os.chdir(workspace)
    try:
        with TerminalProbe([str(binary)], 50, 160, 15.0) as term:
            if not term.read_until(lambda _raw, text: "probe-model" in text, timeout=10.0):
                raise RuntimeError("interactive prompt did not become ready")
            prompt_offset = len(term.output)
            term.write(b"/context prompt\r")
            if not term.read_until(
                    lambda raw, text: "assembled_system_prompt" in text and
                    "CONTEXT_SOURCE_BEGIN_" in text and
                    b"_CONTEXT_SOURCE_END" in raw and
                    "resolved_session_messages" not in text,
                    timeout=15.0, offset=prompt_offset):
                raise RuntimeError("/context prompt omitted complete context-file content")
            if server_state["requests"]:
                raise RuntimeError("/context prompt unexpectedly invoked the provider")
            term.write(b"context probe user message\r")
            active_offset = len(term.output)
            term.write(b"/context messages\r")
            if not term.read_until(
                    lambda _raw, text: "slash commands are unavailable while a turn is running" in text,
                    timeout=5.0, offset=active_offset):
                raise RuntimeError("/context was not rejected while its live state could change")
            if not term.read_until(lambda _raw, text: "CONTEXT_ASSISTANT_MARKER" in text, timeout=12.0):
                raise RuntimeError("probe provider response was not rendered")
            if not term.read_until(
                    lambda raw, _text: "CONTEXT_ASSISTANT_MARKER" in "\n".join(
                        extract_scroll_margin_lines(raw, 50)), timeout=5.0):
                raise RuntimeError("probe provider response was not committed to scrollback")
            messages_offset = len(term.output)
            term.write(b"/context messages\r")
            if not term.read_until(
                    lambda _raw, text: "resolved_session_messages" in text and
                    "context probe user message" in text and
                    "CONTEXT_ASSISTANT_MARKER" in text and
                    "last_provider_payload" in text,
                    timeout=15.0, offset=messages_offset):
                raise RuntimeError("/context messages omitted complete message contents")
            resources_offset = len(term.output)
            term.write(b"/context resources\r")
            if not term.read_until(
                    lambda _raw, text: "resources: skills=" in text and
                    "input_schema" in text and "tool " in text,
                    timeout=15.0, offset=resources_offset):
                raise RuntimeError("/context resources omitted complete tool schemas")
            default_offset = len(term.output)
            term.write(b"/context\r")
            if not term.read_until(
                    lambda raw, text: b"CONTEXT_SOURCE_BEGIN_" in raw and
                    b"_CONTEXT_SOURCE_END" in raw and
                    "resolved_session_messages" in text and
                    "resources: skills=" in text and "model_usage:" in text,
                    timeout=20.0, offset=default_offset):
                raise RuntimeError("bare /context omitted default full context")
            all_offset = len(term.output)
            term.write(b"/context all\r")
            if not term.read_until(
                    lambda raw, text: b"CONTEXT_SOURCE_BEGIN_" in raw and
                    b"_CONTEXT_SOURCE_END" in raw and
                    "last_provider_payload" in text,
                    timeout=20.0, offset=all_offset):
                raise RuntimeError("/context all omitted prompt source or provider payload")
            if server_state["requests"] != 1:
                raise RuntimeError("/context invoked the provider")
            term.write(b"/quit\r")
            term.read_until(lambda _raw, _text: term.proc is not None and term.proc.poll() is not None, timeout=5.0)
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
    run_probe(binary, root / ".pio" / "test" / "interactive-context-probe")
    print("interactive context probe passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
