#!/usr/bin/env python3
"""Run a successful persistent subagent lifecycle over the native RPC API."""
from __future__ import annotations

import argparse
import http.server
import json
import pathlib
import subprocess
import tempfile
import threading


class Provider(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_args: object) -> None:
        pass

    def do_POST(self) -> None:  # noqa: N802
        size = int(self.headers.get("content-length", "0"))
        _ = self.rfile.read(size)
        body = ("data: " + json.dumps({
            "id": "rpc-lifecycle", "object": "chat.completion.chunk",
            "choices": [{"index": 0, "delta": {"role": "assistant", "content": "done"},
                          "finish_reason": "stop"}],
        }) + "\n\ndata: [DONE]\n\n").encode()
        self.send_response(200)
        self.send_header("content-type", "text/event-stream")
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    args = parser.parse_args()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix="qt-rpc-lifecycle-") as temp:
            root = pathlib.Path(temp)
            home = root / "home"
            (home / "agents").mkdir(parents=True)
            (home / "agents" / "probe.md").write_text(
                "---\nname: probe\ndescription: lifecycle probe\ntools: read_text\n---\nAnswer briefly.\n",
                encoding="utf-8")
            (home / "models.json").write_text(json.dumps({"providers": {"probe": {
                "name": "Probe", "api": "openai-completions",
                "baseUrl": f"http://127.0.0.1:{server.server_address[1]}/v1",
                "apiKey": "probe", "authHeader": False,
                "models": [{"id": "probe-model", "name": "Probe", "api": "openai-completions",
                             "input": ["text"], "contextWindow": 8192, "maxTokens": 64}],
            }}}), encoding="utf-8")
            command = [str(pathlib.Path(args.binary).resolve()), "--mode", "rpc",
                       "--provider", "probe", "--model", "probe-model",
                       "--no-extensions", "--no-skills", "--no-context-files", "--no-memory",
                       "--session-dir", str(root / "sessions")]
            frames = [
                {"type": "subagent", "id": "spawn", "action": "spawn",
                 "arguments": {"agent": "probe", "name": "rpc-life", "task": "first"}},
                {"type": "subagent", "id": "wait1", "action": "wait",
                 "arguments": {"target": "rpc-life", "timeout_ms": 60000}},
                {"type": "subagent", "id": "send", "action": "send",
                 "arguments": {"target": "rpc-life", "message": "second", "wake": True}},
                {"type": "subagent", "id": "wait2", "action": "wait",
                 "arguments": {"target": "rpc-life", "timeout_ms": 60000}},
                {"type": "subagent", "id": "list", "action": "list_workers",
                 "arguments": {}},
            ]
            env = dict(__import__("os").environ)
            env["QT_CODING_AGENT_DIR"] = str(home)
            result = subprocess.run(command, input="".join(json.dumps(f) + "\n" for f in frames),
                                    text=True, capture_output=True, env=env, timeout=90)
            if result.returncode != 0:
                raise RuntimeError(result.stderr[-3000:])
            lines = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
            responses = {item.get("id"): item for item in lines if item.get("id")}
            if not responses.get("spawn", {}).get("success"):
                raise RuntimeError(f"spawn failed: {responses.get('spawn')}")
            if not responses.get("send", {}).get("success"):
                raise RuntimeError(f"send failed: {responses.get('send')}")
            if not responses.get("list", {}).get("success"):
                raise RuntimeError(f"list failed: {responses.get('list')}")
            for name in ("wait1", "wait2"):
                assert responses[name]["success"] and responses[name]["data"]["completed"], responses[name]
            events = [item["subagent"] for item in lines if item.get("type") == "subagent_event"
                      and item["subagent"].get("type") == "subagent_complete"]
            assert len(events) == 2 and {e["generation"] for e in events} == {1, 2}, events
            assert all(e["status"] == "completed" and e["output"] == "done" for e in events), events
            worker = responses["list"]["data"]["children"][0]
            assert worker["id"] == "/root/rpc-life" and worker["generation"] == 2, worker
            assert worker["permissions"]["toolnames"] == ["read"], worker
            child_text = pathlib.Path(worker["path"]).read_text()
            assert "first" in child_text and "second" in child_text and "done" in child_text
            index = pathlib.Path(worker["path"]).parent.parent / "children.json"
            assert not json.loads(index.read_text())["pending_events"], "RPC completion was not acknowledged"
            sessions = list((root / "sessions").rglob("*.jsonl"))
            assert len(sessions) == 1, sessions
            resume_frames = [
                {"type": "subagent", "id": "resume", "action": "follow_up",
                 "arguments": {"target": "/root/rpc-life", "task": "third"}},
                {"type": "subagent", "id": "wait3", "action": "wait",
                 "arguments": {"target": "/root/rpc-life", "timeout_ms": 10000}},
                {"type": "new_session", "id": "new"},
                {"type": "subagent", "id": "fresh", "action": "spawn",
                 "arguments": {"agent": "probe", "name": "rpc-life", "task": "fresh"}},
                {"type": "subagent", "id": "wait4", "action": "wait",
                 "arguments": {"target": "/root/rpc-life", "timeout_ms": 10000}},
            ]
            resumed = subprocess.run(command + ["--session", str(sessions[0])],
                input="".join(json.dumps(f) + "\n" for f in resume_frames), text=True,
                capture_output=True, env=env, timeout=40)
            assert resumed.returncode == 0, resumed.stderr
            resumed_lines = [json.loads(line) for line in resumed.stdout.splitlines() if line.strip()]
            resumed_responses = {item.get("id"): item for item in resumed_lines if item.get("id")}
            assert all(resumed_responses[f["id"]]["success"] for f in resume_frames), resumed_responses
            completed = [item["subagent"] for item in resumed_lines if item.get("type") == "subagent_event"
                         and item["subagent"].get("type") == "subagent_complete"]
            assert len(completed) == 2 and [e["generation"] for e in completed] == [3, 1], completed
            assert all(e["status"] == "completed" for e in completed), completed
            rejected = subprocess.run(command + ["--no-tools"], input=json.dumps(frames[0]) + "\n",
                text=True, capture_output=True, env=env, timeout=20)
            rejected_lines = [json.loads(line) for line in rejected.stdout.splitlines() if line.strip()]
            assert any(item.get("id") == "spawn" and not item.get("success", True)
                       and "disabled" in item.get("error", "") for item in rejected_lines), rejected_lines

    finally:
        server.shutdown()
    print("persistent subagent RPC lifecycle probe passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
