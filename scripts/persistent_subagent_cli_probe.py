#!/usr/bin/env python3
"""CLI integration probe for JSON-mode persistent delegation.

Uses an OpenAI-compatible local mock and the real native binary. The first
model turn asks the subagent tool to spawn a slow child; subsequent turns
return deterministic text, allowing the probe to verify the coordinator event
is fed back into the parent JSON stream and persisted session.
"""
from __future__ import annotations

import argparse
import http.server
import json
import os
import pathlib
import subprocess
import tempfile
import threading

ROOT = pathlib.Path(__file__).resolve().parents[1]

def sse(chunks: list[dict]) -> bytes:
    return ("".join("data: " + json.dumps(chunk) + "\n\n" for chunk in chunks) +
            "data: [DONE]\n\n").encode()


def text_chunk(text: str) -> list[dict]:
    return [{"id": "persistent-probe", "object": "chat.completion.chunk",
             "choices": [{"index": 0, "delta": {"role": "assistant", "content": text},
                           "finish_reason": None}]},
            {"id": "persistent-probe", "object": "chat.completion.chunk",
             "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]}]


def tool_chunk() -> list[dict]:
    return [{"id": "persistent-probe", "object": "chat.completion.chunk",
             "choices": [{"index": 0, "delta": {"role": "assistant", "tool_calls": [{
                 "index": 0, "id": "spawn-1", "type": "function",
                 "function": {"name": "subagent", "arguments": json.dumps({
                     "action": "spawn", "agent": "probe", "name": "cli-child",
                     "task": "slow child", "fork": "all"})}}]},
                           "finish_reason": None}]},
            {"id": "persistent-probe", "object": "chat.completion.chunk",
             "choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]}]


def run(binary: pathlib.Path) -> None:
    requests: list[dict] = []
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args: object) -> None: pass
        def do_POST(self) -> None:  # noqa: N802
            size = int(self.headers.get("content-length", "0"))
            payload = json.loads(self.rfile.read(size)); requests.append(payload)
            messages = payload.get("messages", [])
            has_tool_result = any(m.get("role") == "tool" for m in messages if isinstance(m, dict))
            serialized = json.dumps(messages)
            is_child = "slow child" in serialized and '"role": "user"' in serialized
            if not has_tool_result and not is_child and len(requests) == 1:
                chunks = tool_chunk()
            elif is_child:
                chunks = text_chunk("child complete")
            else:
                chunks = text_chunk("parent received child")
            body = sse(chunks)
            self.send_response(200); self.send_header("content-type", "text/event-stream")
            self.send_header("content-length", str(len(body))); self.end_headers(); self.wfile.write(body)

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix="qt-persistent-cli-",
                                          dir=ROOT / ".pio" / "test") as temp:
            root = pathlib.Path(temp); workspace = root / "workspace"; home = root / "agent-home"
            workspace.mkdir(); (home / "agents").mkdir(parents=True)
            (home / "agents" / "probe.md").write_text("---\nname: probe\ndescription: CLI probe\n---\nProbe child.\n", encoding="utf-8")
            models = {"providers": {"probe": {"name": "Probe", "api": "openai-completions",
                "baseUrl": f"http://127.0.0.1:{server.server_address[1]}/v1", "apiKey": "probe",
                "authHeader": False, "models": [{"id": "probe-model", "name": "Probe",
                "api": "openai-completions", "input": ["text"], "contextWindow": 8192, "maxTokens": 128}]}}}
            (home / "models.json").write_text(json.dumps(models), encoding="utf-8")
            env = os.environ.copy(); env["QT_CODING_AGENT_DIR"] = str(home)
            cmd = [str(binary), "--mode", "json", "--provider", "probe", "--model", "probe-model",
                   "--session-dir", str(root / "sessions"), "--no-extensions", "--no-skills",
                   "--no-context-files", "--no-memory", "Begin delegation."]
            result = subprocess.run(cmd, cwd=workspace, env=env, text=True, capture_output=True, timeout=60)
            if result.returncode != 0: raise RuntimeError(result.stderr[-2000:])
            lines = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
            events = [item.get("subagent", {}) for item in lines
                      if item.get("type") == "subagent_event"]
            completions = [event for event in events
                           if event.get("type") == "subagent_complete" and
                           event.get("status") == "completed"]
            if len(completions) != 1 or completions[0].get("generation") != 1:
                raise RuntimeError("JSON stream omitted subagent_event")
            if completions[0].get("output") != "child complete":
                raise RuntimeError("child output was not returned in completion event: " + json.dumps(completions[0]))
            child_requests = [json.dumps(item.get("messages", [])) for item in requests
                              if "slow child" in json.dumps(item.get("messages", []))]
            if not child_requests or not any("Begin delegation" in item and
                                             ("toolCall" in item or "tool_call" in item or
                                              "tool_calls" in item)
                                             for item in child_requests):
                raise RuntimeError("child provider request lost inherited parent context/tool call: " +
                                   json.dumps(child_requests))
            if not any("subagent_complete" in json.dumps(item.get("messages", []))
                       for item in requests):
                raise RuntimeError("parent provider did not receive completion custom message")
            sessions = list((home / "subagents").rglob("*.jsonl"))
            if not sessions or not any("subagent" in p.read_text(encoding="utf-8") for p in sessions):
                raise RuntimeError("delegation was not persisted in session JSONL")
    finally:
        server.shutdown()


def main() -> int:
    parser = argparse.ArgumentParser(); parser.add_argument("--binary", required=True)
    args = parser.parse_args(); run(pathlib.Path(args.binary).resolve()); print("persistent CLI probe passed"); return 0


if __name__ == "__main__": raise SystemExit(main())
