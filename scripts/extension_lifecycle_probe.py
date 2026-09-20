#!/usr/bin/env python3
"""Exercise AngelScript lifecycle hooks around a real local model turn."""

from __future__ import annotations

import argparse
import http.server
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading


def _script_source(marker_path: str) -> str:
    return f'''void activate() {{
  qt_on("session_start", "on_session_start");
  qt_on("session_shutdown", "on_session_shutdown");
  qt_on("before_agent_start", "on_before_agent_start");
  qt_on("tool_call", "on_tool_call");
  qt_on("tool_result", "on_tool_result");
}}

void on_session_start(const string &in event_json) {{
  qt_host_call("pi.appendEntry", "{{\\"customType\\":\\"lifecycle_start\\",\\"data\\":{{\\"marker\\":\\"LIFECYCLE_START\\"}}}}");
}}

void on_session_shutdown(const string &in event_json) {{
  qt_host_call("pi.appendEntry", "{{\\"customType\\":\\"lifecycle_shutdown\\",\\"data\\":{{\\"marker\\":\\"LIFECYCLE_SHUTDOWN\\"}}}}");
}}

string on_before_agent_start(const string &in event_json) {{
  return "{{\\"systemPrompt\\":\\"HOOK_SYSTEM_MARKER\\",\\"message\\":{{\\"customType\\":\\"hook_message\\",\\"content\\":\\"HOOK_MESSAGE_MARKER\\"}}}}";
}}

string on_tool_call(const string &in event_json) {{
  string tool = qt_json_string(qt_json_get(event_json, "toolName"));
  if (tool == "write") return "{{\\"block\\":true,\\"reason\\":\\"WRITE_BLOCK_MARKER\\"}}";
  return "null";
}}

string on_tool_result(const string &in event_json) {{
  return "{{\\"content\\":\\"TOOL_RESULT_REPLACED\\"}}";
}}
'''


class _Handler(http.server.BaseHTTPRequestHandler):
    state: dict[str, object] = {}

    def log_message(self, *_args: object) -> None:
        return

    def do_POST(self) -> None:  # noqa: N802
        size = int(self.headers.get("content-length", "0"))
        payload = json.loads(self.rfile.read(size))
        requests = self.state.setdefault("requests", [])
        assert isinstance(requests, list)
        requests.append(payload)
        if len(requests) == 1:
            delta = {
                "role": "assistant",
                "tool_calls": [{
                    "index": 0,
                    "id": "lifecycle-call-1",
                    "type": "function",
                    "function": {
                        "name": "write",
                        "arguments": json.dumps({
                            "path": self.state["marker_path"],
                            "content": "MUST_NOT_BE_WRITTEN",
                        }),
                    },
                }],
            }
            chunks = [
                {"choices": [{"delta": delta, "finish_reason": None}]},
                {"choices": [{"delta": {}, "finish_reason": "tool_calls"}]},
            ]
        else:
            chunks = [
                {"choices": [{"delta": {"role": "assistant", "content": "LIFECYCLE_DONE"}, "finish_reason": None}]},
                {"choices": [{"delta": {}, "finish_reason": "stop"}]},
            ]
        body = ("".join("data: " + json.dumps(chunk) + "\n\n" for chunk in chunks)
                + "data: [DONE]\n\n").encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()


def run_probe(binary: Path, runtime: Path) -> None:
    shutil.rmtree(runtime, ignore_errors=True)
    agent_home = runtime / "agent-home"
    workspace = runtime / "workspace"
    session_dir = runtime / "sessions"
    agent_home.mkdir(parents=True)
    workspace.mkdir(parents=True)
    session_dir.mkdir(parents=True)
    marker = workspace / "must-not-exist.txt"
    extension = workspace / "lifecycle-probe.as"
    extension.write_text(_script_source(str(marker)), encoding="utf-8")

    state: dict[str, object] = {"requests": [], "marker_path": str(marker)}
    _Handler.state = state
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    port = server.server_address[1]
    (agent_home / "models.json").write_text(
        json.dumps({"providers": {"probe": {
            "api": "openai-completions",
            "baseUrl": f"http://127.0.0.1:{port}/v1",
            "apiKey": "probe-key",
            "models": [{"id": "probe-model"}],
        }}}) + "\n",
        encoding="utf-8",
    )

    environment = dict(os.environ)
    environment["QT_CODING_AGENT_DIR"] = str(agent_home)
    environment.pop("OPENAI_API_KEY", None)
    command = [
        str(binary), "--provider", "probe", "--model", "probe-model",
        "--print", "--no-extensions", "--extension", str(extension),
        "--session-dir", str(session_dir), "use write_text now",
    ]
    try:
        result = subprocess.run(
            command, cwd=workspace, env=environment, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=40,
            check=False,
        )
        if result.returncode != 0:
            raise RuntimeError(
                f"lifecycle turn failed with {result.returncode}\n"
                + result.stdout[-8000:] + "\n" + result.stderr[-8000:]
            )
        requests = state["requests"]
        assert isinstance(requests, list)
        if len(requests) != 2:
            raise RuntimeError(f"expected two provider requests, got {len(requests)}")
        first_text = json.dumps(requests[0])
        second_text = json.dumps(requests[1])
        if "HOOK_SYSTEM_MARKER" not in first_text or "HOOK_MESSAGE_MARKER" not in first_text:
            raise RuntimeError("before_agent_start changes were absent from provider request")
        if "TOOL_RESULT_REPLACED" not in second_text:
            raise RuntimeError(
                "tool_result replacement was absent from next provider request\n"
                + second_text[-12000:]
            )
        if marker.exists():
            raise RuntimeError("blocked write_text created its marker file")
        if "LIFECYCLE_DONE" not in result.stdout:
            raise RuntimeError("final model response was not printed")

        files = list(session_dir.rglob("*.jsonl"))
        if len(files) != 1:
            raise RuntimeError(f"expected one session JSONL, found {files!r}")
        text = files[0].read_text(encoding="utf-8")
        for custom_type in ("lifecycle_start", "lifecycle_shutdown"):
            if text.count(f'"customType":"{custom_type}"') != 1:
                raise RuntimeError(f"{custom_type} entry was not persisted exactly once")
    finally:
        server.shutdown()
        server.server_close()


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
    runtime = Path(tempfile.mkdtemp(prefix="extension-lifecycle-probe-", dir=test_root))
    try:
        run_probe(binary, runtime)
    finally:
        shutil.rmtree(runtime, ignore_errors=True)
    print("AngelScript lifecycle probe passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
