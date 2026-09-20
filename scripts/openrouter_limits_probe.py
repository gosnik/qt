#!/usr/bin/env python3
"""Verify OpenRouter token limits, request fields, metadata, and cache safety."""

from __future__ import annotations

import argparse
import http.server
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading


ROOT = Path(__file__).resolve().parents[1]
NATIVE_BUILD = ROOT / ".pio" / "build" / "native"
OBJECTS = (
    "src/ai/provider_transport.o",
    "src/ai/openrouter_model_discovery.o",
    "src/ai/model_catalog.o",
    "src/ai/auth_storage.o",
    "src/ai/models.o",
    "src/ai/images.o",
    "src/ai/stream_utils.o",
    "src/ai/provider.o",
    "src/foundation/error.o",
    "src/foundation/filesystem.o",
    "src/foundation/process.o",
    "src/foundation/pi_user_agent.o",
)


def compile_cpp_probe(output: Path) -> Path:
    missing = [item for item in OBJECTS if not (NATIVE_BUILD / item).is_file()]
    if missing:
        raise RuntimeError(
            "native objects are missing; run `pio run -e native` first: "
            + ", ".join(missing)
        )

    probe_object = output / "openrouter_limits_probe.o"
    binary = output / "openrouter_limits_probe"
    subprocess.run(
        [
            "g++",
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Wpedantic",
            "-pthread",
            "-Iinclude",
            "-I.pio/libdeps/native/json/include",
            "-c",
            "scripts/openrouter_limits_probe.cpp",
            "-o",
            str(probe_object),
        ],
        cwd=ROOT,
        check=True,
    )
    subprocess.run(
        [
            "g++",
            "-pthread",
            str(probe_object),
            *(str(NATIVE_BUILD / item) for item in OBJECTS),
            "-lssl",
            "-lcrypto",
            "-lz",
            "-ldl",
            "-lrt",
            "-o",
            str(binary),
        ],
        cwd=ROOT,
        check=True,
    )
    return binary


def response_chunks() -> bytes:
    chunks = [
        {
            "id": "openrouter-limits-probe",
            "object": "chat.completion.chunk",
            "choices": [
                {
                    "index": 0,
                    "delta": {"role": "assistant", "content": "ok"},
                    "finish_reason": None,
                }
            ],
        },
        {
            "id": "openrouter-limits-probe",
            "object": "chat.completion.chunk",
            "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
            "usage": {
                "prompt_tokens": 8,
                "completion_tokens": 1,
                "total_tokens": 9,
            },
        },
    ]
    body = "".join(f"data: {json.dumps(chunk)}\n\n" for chunk in chunks)
    return (body + "data: [DONE]\n\n").encode("utf-8")


def exercise_runtime(binary: Path) -> None:
    requests: list[dict[str, object]] = []
    failures: list[str] = []
    transcripts: list[str] = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, _format: str, *_arguments: object) -> None:
            return

        def do_POST(self) -> None:  # noqa: N802 - stdlib handler contract
            try:
                size = int(self.headers.get("content-length", "0"))
                payload = json.loads(self.rfile.read(size))
                requests.append(payload)
                body = response_chunks()
                self.send_response(200)
                self.send_header("content-type", "text/event-stream")
                self.send_header("content-length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            except Exception as error:  # pragma: no cover - surfaced below
                failures.append(str(error))
                body = str(error).encode("utf-8")
                self.send_response(500)
                self.send_header("content-length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="qt-openrouter-runtime-") as temporary:
            temporary_root = Path(temporary)
            workspace = temporary_root / "workspace"
            agent_home = temporary_root / "agent-home"
            workspace.mkdir()
            agent_home.mkdir()
            port = server.server_address[1]
            settings = {
                "quietStartup": True,
                "memory": {"enabled": False},
            }
            models = {
                "providers": {
                    "openrouter": {
                        "baseUrl": f"http://127.0.0.1:{port}/v1",
                        "apiKey": "probe-key",
                        "authHeader": False,
                        "models": [
                            {
                                "id": "qwen/qwen3.5-9b",
                                "name": "Qwen: Qwen3.5-9B",
                                "api": "openai-completions",
                                "contextWindow": 262144,
                                "maxTokens": 81920,
                            },
                            {
                                "id": "qwen/qwen3.5-9b-low",
                                "name": "Qwen: Qwen3.5-9B Low",
                                "api": "openai-completions",
                                "contextWindow": 262144,
                                "maxTokens": 4096,
                            },
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
            environment["HOME"] = str(temporary_root)
            environment["QT_CODING_AGENT_DIR"] = str(agent_home)
            environment.pop("PI_OFFLINE", None)

            def run_model(model: str) -> None:
                completed = subprocess.run(
                    [
                        str(binary),
                        "--mode",
                        "json",
                        "--no-session",
                        "--no-memory",
                        "--no-skills",
                        "--no-extensions",
                        "--no-context-files",
                        "--no-tools",
                        "--provider",
                        "openrouter",
                        "--model",
                        model,
                        "Reply with ok.",
                    ],
                    cwd=workspace,
                    env=environment,
                    check=False,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    timeout=30,
                )
                if completed.returncode != 0:
                    raise RuntimeError(
                        f"qt OpenRouter run failed for {model}: "
                        + completed.stderr[-3000:]
                        + completed.stdout[-1000:]
                    )
                transcripts.append(
                    f"{model}: stdout={completed.stdout[-1000:]!r} "
                    f"stderr={completed.stderr[-2000:]!r}"
                )

            run_model("qwen/qwen3.5-9b")
            run_model("qwen/qwen3.5-9b-low")

            if failures:
                raise RuntimeError("; ".join(failures))
            if len(requests) != 2:
                raise RuntimeError(
                    f"expected two OpenRouter requests, received {len(requests)}; "
                    + "; ".join(transcripts)
                )
            expected = (
                ("qwen/qwen3.5-9b", 32000),
                ("qwen/qwen3.5-9b-low", 4096),
            )
            for payload, (model, limit) in zip(requests, expected):
                if payload.get("model") != model:
                    raise RuntimeError(
                        f"OpenRouter model id changed: {payload.get('model')!r}"
                    )
                if payload.get("max_completion_tokens") != limit:
                    raise RuntimeError(
                        f"{model} requested {payload.get('max_completion_tokens')!r} "
                        f"tokens instead of {limit}"
                    )
                if "max_tokens" in payload:
                    raise RuntimeError(f"{model} emitted both max-token fields")
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--binary",
        type=Path,
        default=NATIVE_BUILD / "qt",
        help="qt binary to exercise (default: native PlatformIO qt)",
    )
    arguments = parser.parse_args()
    binary = arguments.binary.resolve()
    if not binary.is_file():
        raise RuntimeError(f"qt binary is missing: {binary}")

    with tempfile.TemporaryDirectory(prefix="qt-openrouter-limits-") as temporary:
        output = Path(temporary)
        cpp_probe = compile_cpp_probe(output)
        isolated_home = output / "home"
        isolated_home.mkdir()
        subprocess.run([str(cpp_probe), str(isolated_home)], cwd=ROOT, check=True)
    exercise_runtime(binary)
    print("openrouter limits probe passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"openrouter limits probe failed: {error}", file=os.sys.stderr)
        raise SystemExit(1)
