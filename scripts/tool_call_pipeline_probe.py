#!/usr/bin/env python3
"""Exercise schema recovery and indexed symbol lookup through the real qt app."""

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


def text_dump(value: object) -> str:
    return json.dumps(value, separators=(",", ":"), sort_keys=True)


def tool_chunk(call_id: str, name: str, arguments: dict[str, object]) -> list[dict]:
    return [
        {
            "id": "tool-pipeline-probe",
            "object": "chat.completion.chunk",
            "choices": [
                {
                    "index": 0,
                    "delta": {
                        "role": "assistant",
                        "tool_calls": [
                            {
                                "index": 0,
                                "id": call_id,
                                "type": "function",
                                "function": {
                                    "name": name,
                                    "arguments": json.dumps(arguments),
                                },
                            }
                        ],
                    },
                    "finish_reason": None,
                }
            ],
        },
        {
            "id": "tool-pipeline-probe",
            "object": "chat.completion.chunk",
            "choices": [
                {"index": 0, "delta": {}, "finish_reason": "tool_calls"}
            ],
            "usage": {
                "prompt_tokens": 32,
                "completion_tokens": 8,
                "total_tokens": 40,
            },
        },
    ]


def text_chunks(text: str) -> list[dict]:
    return [
        {
            "id": "tool-pipeline-probe",
            "object": "chat.completion.chunk",
            "choices": [
                {
                    "index": 0,
                    "delta": {"role": "assistant", "content": text},
                    "finish_reason": None,
                }
            ],
        },
        {
            "id": "tool-pipeline-probe",
            "object": "chat.completion.chunk",
            "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
            "usage": {
                "prompt_tokens": 32,
                "completion_tokens": 8,
                "total_tokens": 40,
            },
        },
    ]


def run_probe(binary: pathlib.Path) -> None:
    requests: list[dict] = []
    failures: list[str] = []
    symbol_attempt = 0

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, _format: str, *_arguments: object) -> None:
            return

        def do_POST(self) -> None:  # noqa: N802 - stdlib handler contract
            nonlocal symbol_attempt
            try:
                size = int(self.headers.get("content-length", "0"))
                payload = json.loads(self.rfile.read(size))
                requests.append(payload)
                serialized = text_dump(payload.get("messages", []))
                messages = payload.get("messages", [])
                latest = text_dump(messages[-1]) if messages else ""

                if len(requests) == 1:
                    tools = payload.get("tools", [])
                    by_name = {
                        item.get("function", {}).get("name"): item.get(
                            "function", {}
                        )
                        for item in tools
                        if isinstance(item, dict)
                    }
                    symbol = by_name.get("symbol")
                    read = by_name.get("read")
                    if not isinstance(symbol, dict):
                        failures.append("symbol tool was not advertised")
                    else:
                        parameters = symbol.get("parameters", {})
                        if parameters.get("required") != ["name"]:
                            failures.append("symbol schema did not require only name")
                    if not isinstance(read, dict):
                        failures.append("read tool was not advertised")
                    else:
                        required = read.get("parameters", {}).get("required")
                        if required != ["path"]:
                            failures.append("read schema did not require path")
                    expected_index_tools = {
                        "analyze_file": ["file"],
                        "references": ["symbol"],
                        "build_context": ["file"],
                        "search_index": ["query"],
                    }
                    for name, required in expected_index_tools.items():
                        tool = by_name.get(name)
                        if not isinstance(tool, dict):
                            failures.append(f"{name} tool was not advertised")
                            continue
                        parameters = tool.get("parameters", {})
                        if parameters.get("required") != required:
                            failures.append(
                                f"{name} schema required fields were incorrect"
                            )
                        if parameters.get("additionalProperties") is not False:
                            failures.append(f"{name} schema was not strict")
                    if "call symbol first" not in serialized:
                        failures.append("system prompt omitted symbol routing guidance")
                    if "Use analyze_file" not in serialized:
                        failures.append("system prompt omitted direct index guidance")
                    chunks = tool_chunk("invalid-read", "read", {})
                elif len(requests) == 2 and "invalid_arguments" in latest:
                    if "required property is missing" not in serialized:
                        failures.append("validation error omitted the missing field")
                    chunks = tool_chunk(
                        f"symbol-{symbol_attempt}",
                        "symbol",
                        {"name": "run", "context": True, "limit": "5"},
                    )
                    symbol_attempt += 1
                elif "index_not_ready" in latest and symbol_attempt < 12:
                    chunks = tool_chunk(
                        f"symbol-{symbol_attempt}",
                        "symbol",
                        {"name": "run", "context": True, "limit": 5},
                    )
                    symbol_attempt += 1
                elif (
                    "symbols" in latest
                    and "Example::run" in latest
                    and "symbol-" in latest
                ):
                    chunks = tool_chunk(
                        "misnested-index",
                        "index",
                        {
                            "action": "call",
                            "arguments": {
                                "category": "code",
                                "operation": "symbol_lookup",
                                "pattern": "run",
                                "context": True,
                            },
                        },
                    )
                elif (
                    "symbols" in latest
                    and "Example::run" in latest
                    and "misnested-index" in latest
                ):
                    chunks = tool_chunk(
                        "invalid-analyze",
                        "analyze_file",
                        {"path": "example.cpp"},
                    )
                elif (
                    "invalid-analyze" in latest
                    and "invalid_arguments" in latest
                    and "expected_schema" in latest
                ):
                    chunks = tool_chunk(
                        "analyze-example",
                        "analyze_file",
                        {"file": "example.cpp", "limit": "20"},
                    )
                elif (
                    "analyze-example" in latest
                    and "helper" in latest
                    and "outgoing_calls" in latest
                ):
                    chunks = tool_chunk(
                        "references-helper",
                        "references",
                        {
                            "symbol": "helper",
                            "direction": "inbound",
                            "depth": "2",
                            "limit": 20,
                        },
                    )
                elif (
                    "references-helper" in latest
                    and "paths" in latest
                    and "approximate" in latest
                ):
                    chunks = tool_chunk(
                        "build-example",
                        "build_context",
                        {"file": "example.cpp", "limit": 20},
                    )
                elif (
                    "build-example" in latest
                    and "compile_flags" in latest
                    and "-std=c++17" in latest
                    and "include_graph" in latest
                ):
                    chunks = tool_chunk(
                        "search-example",
                        "search_index",
                        {"query": "Example", "limit": "10"},
                    )
                elif (
                    "search-example" in latest
                    and "matches" in latest
                    and "example.cpp" in latest
                ):
                    chunks = text_chunks("direct index tools complete")
                else:
                    failures.append(
                        "model transcript did not contain the expected validation "
                        "or symbol result"
                    )
                    chunks = text_chunks("probe failed")

                body = "".join(
                    f"data: {json.dumps(chunk)}\n\n" for chunk in chunks
                )
                body += "data: [DONE]\n\n"
                encoded = body.encode("utf-8")
                self.send_response(200)
                self.send_header("content-type", "text/event-stream")
                self.send_header("content-length", str(len(encoded)))
                self.end_headers()
                self.wfile.write(encoded)
            except Exception as error:  # pragma: no cover - surfaced to caller
                failures.append(str(error))
                encoded = str(error).encode("utf-8")
                self.send_response(500)
                self.send_header("content-length", str(len(encoded)))
                self.end_headers()
                self.wfile.write(encoded)

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="qt-tool-pipeline-") as temporary:
            root = pathlib.Path(temporary)
            workspace = root / "workspace"
            agent_home = root / "agent-home"
            workspace.mkdir()
            agent_home.mkdir()
            (workspace / "example.hpp").write_text(
                "#pragma once\n"
                "class Example {\n"
                " public:\n"
                "  int run(int value) const;\n"
                "};\n",
                encoding="utf-8",
            )
            (workspace / "example.cpp").write_text(
                '#include "example.hpp"\n'
                "int helper(int value) { return value + 1; }\n"
                "int Example::run(int value) const { return helper(value); }\n",
                encoding="utf-8",
            )
            (workspace / "compile_commands.json").write_text(
                json.dumps(
                    [
                        {
                            "directory": str(workspace),
                            "command": (
                                "c++ -I. -DPROBE_BUILD=1 -std=c++17 "
                                "-c example.cpp -o example.o"
                            ),
                            "file": str(workspace / "example.cpp"),
                        }
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            port = server.server_address[1]
            settings = {
                "defaultProvider": "tool-pipeline-probe",
                "defaultModel": "probe-model",
                "quietStartup": True,
                "memory": {"enabled": False},
            }
            models = {
                "providers": {
                    "tool-pipeline-probe": {
                        "name": "Tool Pipeline Probe",
                        "api": "openai-completions",
                        "baseUrl": f"http://127.0.0.1:{port}/v1",
                        "apiKey": "probe-key",
                        "authHeader": False,
                        "models": [
                            {
                                "id": "probe-model",
                                "name": "Probe Model",
                                "api": "openai-completions",
                                "input": ["text"],
                                "contextWindow": 8192,
                                "maxTokens": 256,
                            }
                        ],
                    }
                }
            }
            (agent_home / "settings.json").write_text(
                json.dumps(settings) + "\n", encoding="utf-8"
            )
            (agent_home / "models.json").write_text(
                json.dumps(models) + "\n", encoding="utf-8"
            )
            environment = os.environ.copy()
            environment["QT_CODING_AGENT_DIR"] = str(agent_home)
            command = [
                str(binary),
                "--mode",
                "json",
                "--no-session",
                "--no-memory",
                "--no-skills",
                "--no-extensions",
                "--no-context-files",
                "--tools",
                "read,symbol,analyze_file,references,build_context,"
                "search_index,index,grep",
                "Find the C++ definition of Example::run.",
            ]
            try:
                completed = subprocess.run(
                    command,
                    cwd=workspace,
                    env=environment,
                    check=False,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    timeout=45,
                )
            except subprocess.TimeoutExpired as error:
                last_messages = (
                    text_dump(requests[-1].get("messages", []))[-3000:]
                    if requests
                    else "(no provider request)"
                )
                raise RuntimeError(
                    f"qt tool pipeline timed out after {len(requests)} model "
                    f"turns; failures={failures}; last_messages={last_messages}"
                ) from error
            if completed.returncode != 0:
                raise RuntimeError(
                    "qt tool pipeline run failed: "
                    + completed.stderr[-4000:]
                    + completed.stdout[-2000:]
                )
            if "direct index tools complete" not in completed.stdout:
                raise RuntimeError(
                    "qt did not complete the direct index tool sequence: "
                    + completed.stdout[-4000:]
                )
            if failures:
                raise RuntimeError("; ".join(failures))
            if len(requests) < 4:
                raise RuntimeError("tool pipeline did not execute multiple model turns")
            print(
                "tool call pipeline probe passed "
                f"({len(requests)} model turns, {symbol_attempt} symbol attempts, "
                "4 direct index tools)"
            )
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True)
    arguments = parser.parse_args()
    run_probe(pathlib.Path(arguments.binary).resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
