#!/usr/bin/env python3
"""Exercise AngelScript context, model-inspection, and compaction host calls."""

from __future__ import annotations

import argparse
import http.server
import json
import os
from pathlib import Path
import re
import shutil
import tempfile
import threading


def _script_source() -> str:
    return r'''void activate() {
  registerCommand("as_inspect", "Inspect extension context", "");
  registerCommand("as_trailing_usage", "Inspect trailing context usage", "");
  registerCommand("as_compact", "Compact extension context", "");
}

string command_as_inspect(const string &in arguments) {
  string context = qt_host_call("ctx.getContext", "{}");
  string model = qt_host_call("ctx.getModel", "{}");
  string queried = qt_host_call(
      "ctx.getModel", "{\"provider\":\"probe\",\"id\":\"probe-model\"}");
  string catalog = qt_host_call("ctx.getModelCatalog", "{}");
  string usage = qt_host_call("ctx.getContextUsage", "{}");
  return "{\"context\":" + context + ",\"model\":" + model +
         ",\"queried\":" + queried + ",\"catalog\":" + catalog +
         ",\"usage\":" + usage + "}";
}

string command_as_trailing_usage(const string &in arguments) {
  qt_host_call(
      "pi.sendMessage",
      "{\"customType\":\"probe\",\"content\":\"TRAILING_USAGE_MARKER "
      "abcdefghijklmnopqrstuvwxyz abcdefghijklmnopqrstuvwxyz "
      "abcdefghijklmnopqrstuvwxyz abcdefghijklmnopqrstuvwxyz\","
      "\"display\":false,\"triggerTurn\":false}");
  return "{\"trailingUsage\":" +
         qt_host_call("ctx.getContextUsage", "{}") + "}";
}

string command_as_compact(const string &in arguments) {
  return qt_host_call(
      "ctx.compact",
      "{\"customInstructions\":\"AS_COMPACT_INSTRUCTIONS\"}");
}
'''


class _Handler(http.server.BaseHTTPRequestHandler):
    requests: list[dict[str, object]] = []

    def log_message(self, *_args: object) -> None:
        return

    def do_POST(self) -> None:  # noqa: N802
        size = int(self.headers.get("content-length", "0"))
        payload = json.loads(self.rfile.read(size))
        self.requests.append(payload)
        encoded = json.dumps(payload)
        is_compaction = "AS_COMPACT_INSTRUCTIONS" in encoded
        text = "AS_COMPACT_SUMMARY" if is_compaction else "CONTEXT_READY"

        if payload.get("stream"):
            chunks = [
                {
                    "choices": [
                        {
                            "delta": {"role": "assistant", "content": text},
                            "finish_reason": None,
                        }
                    ]
                },
                {
                    "choices": [{"delta": {}, "finish_reason": "stop"}],
                    "usage": {
                        "prompt_tokens": 990,
                        "completion_tokens": 210,
                        "total_tokens": 1200,
                    },
                },
            ]
            body = (
                "".join("data: " + json.dumps(chunk) + "\n\n" for chunk in chunks)
                + "data: [DONE]\n\n"
            ).encode()
            content_type = "text/event-stream"
        else:
            body = json.dumps(
                {
                    "id": "as-context-probe",
                    "model": "probe-model",
                    "choices": [
                        {
                            "message": {"role": "assistant", "content": text},
                            "finish_reason": "stop",
                        }
                    ],
                    "usage": {
                        "prompt_tokens": 20,
                        "completion_tokens": 4,
                        "total_tokens": 24,
                    },
                }
            ).encode()
            content_type = "application/json"

        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()


def run_probe(binary: Path, runtime: Path) -> None:
    from tui_pty_probe import TerminalProbe, strip_ansi

    shutil.rmtree(runtime, ignore_errors=True)
    agent_home = runtime / "agent-home"
    workspace = runtime / "workspace"
    session_dir = runtime / "sessions"
    agent_home.mkdir(parents=True)
    workspace.mkdir(parents=True)
    session_dir.mkdir(parents=True)
    extension = workspace / "context-probe.as"
    extension.write_text(_script_source(), encoding="utf-8")

    _Handler.requests = []
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    port = server.server_address[1]
    (agent_home / "models.json").write_text(
        json.dumps(
            {
                "providers": {
                    "probe": {
                        "api": "openai-completions",
                        "baseUrl": f"http://127.0.0.1:{port}/v1",
                        "apiKey": "probe-key",
                        "headers": {
                            "Cookie": "probe-cookie-secret",
                            "X-Goog-Api-Key": "probe-google-secret",
                            "X-Custom-Token": "probe-custom-secret",
                        },
                        "models": [
                            {
                                "id": "probe-model",
                                "name": "Probe Model",
                                "contextWindow": 4096,
                                "maxTokens": 512,
                            }
                        ],
                    }
                }
            }
        )
        + "\n",
        encoding="utf-8",
    )

    previous_home = os.environ.get("QT_CODING_AGENT_DIR")
    previous_cwd = Path.cwd()
    os.environ["QT_CODING_AGENT_DIR"] = str(agent_home)
    os.chdir(workspace)
    try:
        with TerminalProbe(
            [
                str(binary),
                "--provider",
                "probe",
                "--model",
                "probe-model",
                "--no-extensions",
                "--extension",
                str(extension),
                "--session-dir",
                str(session_dir),
            ],
            28,
            140,
            15.0,
        ) as term:
            if not term.read_until(
                lambda _raw, text: "probe-model" in text, timeout=8.0
            ):
                raise RuntimeError("interactive prompt did not become ready")

            term.write(b"remember CONTEXT_CONTENT_MARKER\r")
            if not term.read_until(
                lambda _raw, text: "CONTEXT_READY" in text, timeout=12.0
            ):
                raise RuntimeError(
                    "model turn did not complete\n"
                    + strip_ansi(bytes(term.output[-12000:]))
                )

            term.write(b"/as_inspect\r")
            if not term.read_until(
                lambda _raw, text: "CONTEXT_CONTENT_MARKER" in text
                and '"contextWindow":4096' in text
                and '"provider":"probe"' in text
                and '"id":"probe-model"' in text,
                timeout=10.0,
            ):
                raise RuntimeError(
                    "AngelScript inspection APIs did not expose context/model metadata\n"
                    + strip_ansi(bytes(term.output[-20000:]))
                )
            inspected_output = strip_ansi(bytes(term.output))
            for secret in (
                "probe-key",
                "probe-cookie-secret",
                "probe-google-secret",
                "probe-custom-secret",
            ):
                if secret in inspected_output:
                    raise RuntimeError(
                        "model inspection exposed configured credentials"
                    )

            output_start = len(term.output)
            term.write(b"/as_trailing_usage\r")
            if not term.read_until(
                lambda _raw, text: '"trailingUsage":' in text,
                timeout=10.0,
            ):
                raise RuntimeError(
                    "trailing context usage was not reported\n"
                    + strip_ansi(bytes(term.output[-20000:]))
                )
            trailing_output = strip_ansi(bytes(term.output[output_start:]))
            usage_output = trailing_output.split('"trailingUsage":', 1)[1]
            token_match = re.search(r'"tokens":(\d+)', usage_output)
            if token_match is None or int(token_match.group(1)) <= 1200:
                raise RuntimeError(
                    "trailing message tokens were not added to provider usage"
                )

            term.write(b"/as_compact\r")
            if not term.read_until(
                lambda _raw, text: '"summary":"AS_COMPACT_SUMMARY"' in text,
                timeout=12.0,
            ):
                raise RuntimeError(
                    "AngelScript compaction did not complete\n"
                    + strip_ansi(bytes(term.output[-20000:]))
                )

            term.write(b"/as_inspect\r")
            if not term.read_until(
                lambda _raw, text: "AS_COMPACT_SUMMARY" in text
                and '"tokens":null' in text,
                timeout=10.0,
            ):
                raise RuntimeError(
                    "post-compaction context was not observable with unknown usage\n"
                    + strip_ansi(bytes(term.output[-20000:]))
                )

            term.write(b"/quit\r")
            if not term.read_until(
                lambda _raw, _text: term.proc is not None
                and term.proc.poll() is not None,
                timeout=5.0,
            ):
                raise RuntimeError("interactive probe did not exit after /quit")

        if len(_Handler.requests) != 2:
            raise RuntimeError(
                f"expected one turn and one compaction request, got {len(_Handler.requests)}"
            )
        if "AS_COMPACT_INSTRUCTIONS" not in json.dumps(_Handler.requests[1]):
            raise RuntimeError("custom compaction instructions were not sent to the model")

        session_files = list(session_dir.rglob("*.jsonl"))
        if len(session_files) != 1:
            raise RuntimeError(f"expected one session JSONL, found {session_files!r}")
        session_text = session_files[0].read_text(encoding="utf-8")
        if session_text.count('"type":"compaction"') != 1:
            raise RuntimeError("AngelScript compaction entry was not persisted exactly once")
        if "AS_COMPACT_SUMMARY" not in session_text:
            raise RuntimeError("persisted compaction summary was missing")
    finally:
        server.shutdown()
        server.server_close()
        os.chdir(previous_cwd)
        if previous_home is None:
            os.environ.pop("QT_CODING_AGENT_DIR", None)
        else:
            os.environ["QT_CODING_AGENT_DIR"] = previous_home


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default=".pio/build/native/qt")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    binary = Path(args.binary)
    if not binary.is_absolute():
        binary = (root / binary).resolve()
    if not binary.is_file():
        raise RuntimeError("native qt binary is missing; run pio run -e native")
    test_root = root / ".pio" / "test"
    test_root.mkdir(parents=True, exist_ok=True)
    runtime = Path(tempfile.mkdtemp(prefix="as-context-probe-", dir=test_root))
    try:
        run_probe(binary, runtime)
    finally:
        shutil.rmtree(runtime, ignore_errors=True)
    print("AngelScript context/model/compaction probe passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
