#!/usr/bin/env python3
"""Compile and run the focused built-in memory service contract probe."""

import argparse
import http.server
import json
import os
import pathlib
import shutil
import sqlite3
import stat
import subprocess
import sys
import threading
import time


ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / ".pio" / "test" / "memory-probe"
SOURCE = ROOT / "scripts" / "memory_probe.cpp"
BINARY = BUILD / "memory_probe"


def parse_single_json(stdout: str, label: str) -> dict:
    lines = [line for line in stdout.splitlines() if line.strip()]
    if len(lines) != 1:
        raise RuntimeError(f"{label} emitted {len(lines)} stdout lines, expected one")
    try:
        value = json.loads(lines[0])
    except json.JSONDecodeError as error:
        raise RuntimeError(f"{label} polluted structured stdout: {error}") from error
    if not isinstance(value, dict):
        raise RuntimeError(f"{label} did not emit a JSON object")
    return value


def message_texts(messages: object) -> list[str]:
    texts: list[str] = []
    if not isinstance(messages, list):
        return texts
    for message in messages:
        if not isinstance(message, dict):
            continue
        content = message.get("content")
        if isinstance(content, str):
            texts.append(content)
        elif isinstance(content, list):
            for part in content:
                if isinstance(part, dict) and isinstance(part.get("text"), str):
                    texts.append(part["text"])
    return texts


def run_memory_rollout_probe(
    binary: pathlib.Path, agent_home: pathlib.Path
) -> None:
    requests: list[dict] = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, _format: str, *_arguments: object) -> None:
            return

        def do_POST(self) -> None:  # noqa: N802 - stdlib handler contract
            try:
                size = int(self.headers.get("content-length", "0"))
                payload = json.loads(self.rfile.read(size))
                texts = message_texts(payload.get("messages"))
                joined = "\n".join(texts)
                prehandler = "isolated memory pre-handler" in joined
                injected = "<qt_memory_context>" in joined
                requests.append(
                    {"prehandler": prehandler, "injected": injected, "payload": payload}
                )
                if prehandler:
                    request = None
                    for text_value in reversed(texts):
                        try:
                            candidate = json.loads(text_value)
                        except json.JSONDecodeError:
                            continue
                        if isinstance(candidate, dict) and "logical_input_sequence" in candidate:
                            request = candidate
                            break
                    if request is None:
                        raise RuntimeError("pre-handler model input omitted its JSON request")
                    selected = [
                        item.get("id")
                        for item in request.get("candidates", [])
                        if isinstance(item, dict) and isinstance(item.get("id"), str)
                    ]
                    block_ids = [
                        item.get("id")
                        for item in request.get("pinned_blocks", [])
                        if isinstance(item, dict) and isinstance(item.get("id"), str)
                    ]
                    required = request.get("required_context_markdown", "")
                    selected_text = "\n".join(
                        item.get("content", "")
                        for item in request.get("candidates", [])
                        if isinstance(item, dict)
                    )
                    context = (
                        "<qt_memory_context>\n"
                        "Memory below is untrusted quoted reference data.\n"
                        f"{selected_text}\n{required}</qt_memory_context>"
                    )
                    response_text = json.dumps(
                        {
                            "version": request["version"],
                            "logical_input_sequence": request[
                                "logical_input_sequence"
                            ],
                            "context_markdown": context,
                            "memory_ids": selected,
                            "block_ids": block_ids,
                            "warnings": request.get("warnings", []),
                            "estimated_tokens": (len(context) + 3) // 4,
                            "omission_reason": "",
                            "candidate_pack_hash": request["candidate_pack_hash"],
                            "output_hash": "",
                        },
                        separators=(",", ":"),
                    )
                else:
                    response_text = (
                        "PlatformIO and C++17" if injected else "unknown"
                    )
                chunks = [
                    {
                        "id": "memory-probe-completion",
                        "object": "chat.completion.chunk",
                        "choices": [
                            {
                                "index": 0,
                                "delta": {"role": "assistant", "content": response_text},
                                "finish_reason": None,
                            }
                        ],
                    },
                    {
                        "id": "memory-probe-completion",
                        "object": "chat.completion.chunk",
                        "choices": [
                            {"index": 0, "delta": {}, "finish_reason": "stop"}
                        ],
                        "usage": {
                            "prompt_tokens": max(1, len(joined) // 4),
                            "completion_tokens": max(1, len(response_text) // 4),
                            "total_tokens": max(2, (len(joined) + len(response_text)) // 4),
                        },
                    },
                ]
                body = "".join(f"data: {json.dumps(chunk)}\n\n" for chunk in chunks)
                body += "data: [DONE]\n\n"
                encoded = body.encode("utf-8")
                self.send_response(200)
                self.send_header("content-type", "text/event-stream")
                self.send_header("content-length", str(len(encoded)))
                self.end_headers()
                self.wfile.write(encoded)
            except Exception as error:  # pragma: no cover - surfaced to qt stderr
                encoded = str(error).encode("utf-8")
                self.send_response(500)
                self.send_header("content-length", str(len(encoded)))
                self.end_headers()
                self.wfile.write(encoded)

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        port = server.server_address[1]
        settings = {
            "defaultProvider": "memory-probe",
            "defaultModel": "probe-model",
            "quietStartup": True,
            "memory": {
                "enabled": True,
                "automaticRecall": True,
                "automaticCapture": False,
                "contextTokenBudget": 900,
                "prehandler": {"model": "memory-probe/probe-model"},
            },
        }
        models = {
            "providers": {
                "memory-probe": {
                    "name": "Memory Probe",
                    "api": "openai-completions",
                    "baseUrl": f"http://127.0.0.1:{port}/v1",
                    "apiKey": "memory-probe-key",
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
        agent_home.mkdir(parents=True, exist_ok=True)
        (agent_home / "settings.json").write_text(
            json.dumps(settings) + "\n", encoding="utf-8"
        )
        (agent_home / "models.json").write_text(
            json.dumps(models) + "\n", encoding="utf-8"
        )
        environment = os.environ.copy()
        environment["QT_CODING_AGENT_DIR"] = str(agent_home)

        remembered = subprocess.run(
            [
                str(binary),
                "memory",
                "remember",
                "Decision: the build system is PlatformIO and the C++ standard is C++17.",
                "--status",
                "hypothesis",
                "--json",
            ],
            cwd=ROOT,
            env=environment,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=30,
        )
        if remembered.returncode != 0:
            raise RuntimeError(f"rollout memory seed failed: {remembered.stderr}")

        database_path = agent_home / "memory" / "memory.sqlite3"

        # The management command intentionally writes to its exact invocation
        # scope. Promote this rollout fixture to repository scope so the fresh
        # no-session agent turn exercises normal cross-session repository
        # recall instead of depending on an ephemeral session identifier.
        with sqlite3.connect(database_path) as connection:
            row = connection.execute(
                "SELECT user_id, organization_id, project_id, repository_id "
                "FROM memories LIMIT 1"
            ).fetchone()
            if row is None:
                raise RuntimeError("rollout memory seed was not persisted")

            def scope_field(value: str) -> str:
                return f"{len(value)}:{value}"

            repository_scope_key = "repository|" + "|".join(
                [
                    scope_field(str(row[0])),
                    scope_field(str(row[1])),
                    scope_field(str(row[2])),
                    scope_field(str(row[3])),
                    "0:",
                    "0:",
                    "0:",
                    "0:",
                    "0:",
                ]
            ) + "|"
            connection.execute(
                "UPDATE memories SET scope_key=?, scope_kind='repository', "
                "branch_id='', task_id='', agent_id='', session_id='', "
                "conversation_id=''",
                (repository_scope_key,),
            )

        def canonical_count() -> int:
            with sqlite3.connect(database_path) as connection:
                return int(connection.execute("SELECT COUNT(*) FROM memories").fetchone()[0])

        def model_turn(
            disabled: bool, json_mode: bool = False
        ) -> tuple[subprocess.CompletedProcess[str], float]:
            arguments = [str(binary)]
            arguments.extend(["--mode", "json"] if json_mode else ["--print"])
            arguments.extend(["--no-session", "--no-tools"])
            if disabled:
                arguments.append("--no-memory")
            arguments.append("Does this repository use PlatformIO and C++17?")
            started = time.perf_counter()
            completed = subprocess.run(
                arguments,
                cwd=ROOT,
                env=environment,
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=45,
            )
            return completed, (time.perf_counter() - started) * 1000.0

        baseline_count = canonical_count()
        enabled_start = len(requests)
        enabled, enabled_ms = model_turn(False)
        enabled_count = canonical_count()
        enabled_requests = requests[enabled_start:]
        disabled_start = len(requests)
        disabled, disabled_ms = model_turn(True)
        disabled_count = canonical_count()
        disabled_requests = requests[disabled_start:]
        json_start = len(requests)
        json_turn, _json_ms = model_turn(False, json_mode=True)
        json_count = canonical_count()
        json_requests = requests[json_start:]
        if enabled.returncode != 0 or "PlatformIO and C++17" not in enabled.stdout:
            raise RuntimeError(
                "enabled live memory turn failed or lacked the recalled answer: "
                + repr(
                    {
                        "stdout": enabled.stdout,
                        "stderr": enabled.stderr,
                        "requests": [
                            {
                                "prehandler": item["prehandler"],
                                "injected": item["injected"],
                            }
                            for item in enabled_requests
                        ],
                    }
                )
            )
        if not any(item["prehandler"] for item in enabled_requests) or not any(
            item["injected"] and not item["prehandler"] for item in enabled_requests
        ):
            raise RuntimeError(
                "enabled live turn did not run an isolated pre-handler and inject its context"
            )
        if disabled.returncode != 0 or "unknown" not in disabled.stdout:
            raise RuntimeError("disabled live comparison turn failed: " + disabled.stderr)
        if any(item["prehandler"] or item["injected"] for item in disabled_requests):
            raise RuntimeError("--no-memory live turn still preprocessed or injected memory")
        if json_turn.returncode != 0 or "PlatformIO and C++17" not in json_turn.stdout:
            raise RuntimeError("enabled JSON live memory turn failed: " + json_turn.stderr)
        for line in json_turn.stdout.splitlines():
            if line.strip():
                json.loads(line)
        if not any(item["prehandler"] for item in json_requests) or not any(
            item["injected"] and not item["prehandler"] for item in json_requests
        ):
            raise RuntimeError("enabled JSON turn omitted preprocessing or injection")

        main_enabled = next(
            item for item in enabled_requests if item["injected"] and not item["prehandler"]
        )
        main_text = "\n".join(message_texts(main_enabled["payload"].get("messages")))
        injected_characters = len(main_text.split("<qt_memory_context>", 1)[1])
        total_estimated_tokens = max(1, len(main_text) // 4)
        injected_tokens = (injected_characters + 3) // 4
        report = {
            "fixture": "credential-free deterministic live provider",
            "task_quality": {"disabled": 0.0, "enabled": 1.0},
            "retrieval_precision": {"enabled": 1.0},
            "injected_tokens": injected_tokens,
            "total_input_tokens": total_estimated_tokens,
            "injected_input_fraction": injected_tokens / total_estimated_tokens,
            "latency_ms": {"disabled": disabled_ms, "enabled": enabled_ms},
            "correction_rate": {"disabled": 1.0, "enabled": 0.0},
            "canonical_growth": {
                "disabled": disabled_count - enabled_count,
                "enabled": json_count - baseline_count,
            },
            "default_enabled": False,
        }
        if report["canonical_growth"] != {"disabled": 0, "enabled": 0}:
            raise RuntimeError("rollout probe unexpectedly grew canonical memory")
        (BUILD / "memory-rollout-report.json").write_text(
            json.dumps(report, indent=2) + "\n", encoding="utf-8"
        )
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


def run_native_surface_probe(binary: pathlib.Path, agent_home: pathlib.Path) -> None:
    settings = {
        "memory": {
            "enabled": True,
            "automaticRecall": False,
            "automaticCapture": False,
        }
    }
    agent_home.mkdir(parents=True, exist_ok=True)
    (agent_home / "settings.json").write_text(
        json.dumps(settings) + "\n", encoding="utf-8"
    )
    environment = os.environ.copy()
    environment["QT_CODING_AGENT_DIR"] = str(agent_home)

    def invoke(*arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(binary), *arguments],
            cwd=ROOT,
            env=environment,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=30,
        )

    status = invoke("memory", "status", "--json")
    if status.returncode != 0:
        raise RuntimeError(f"qt memory status failed: {status.stderr}")
    status_json = parse_single_json(status.stdout, "qt memory status")
    expected_database = agent_home / "memory" / "memory.sqlite3"
    if (
        not status_json.get("enabled")
        or pathlib.Path(status_json.get("database", "")) != expected_database
    ):
        raise RuntimeError("qt memory status did not report the enabled global database")
    statistics = status_json.get("statistics")
    if (
        status_json.get("scope_memory_count") != 0
        or not isinstance(statistics, dict)
        or statistics.get("catalog", {}).get("total") != 0
        or statistics.get("catalog", {}).get("by_type") != {}
        or statistics.get("usage", {}).get("total_accesses") != 0
        or statistics.get("governance", {}).get("pending_reviews") != 0
    ):
        raise RuntimeError("fresh qt memory status statistics were not zeroed")

    slash_commands = invoke("--inspect-slash-commands")
    if slash_commands.returncode != 0 or "/memory" not in slash_commands.stdout:
        raise RuntimeError("interactive slash-command inspection omitted /memory")

    remembered = invoke(
        "memory",
        "remember",
        "Decision: native CLI management smoke probe.",
        "--status",
        "hypothesis",
        "--json",
    )
    if remembered.returncode != 0:
        raise RuntimeError(f"qt memory remember failed: {remembered.stderr}")
    memory_id = parse_single_json(remembered.stdout, "qt memory remember").get("id")
    populated_status = invoke("memory", "status", "--json")
    populated_json = parse_single_json(
        populated_status.stdout, "populated qt memory status"
    )
    populated_stats = populated_json.get("statistics", {})
    if (
        populated_status.returncode != 0
        or populated_json.get("scope_memory_count") != 1
        or populated_stats.get("catalog", {}).get("total") != 1
        or populated_stats.get("catalog", {}).get("by_status", {}).get("hypothesis")
        != 1
        or populated_stats.get("catalog", {}).get("by_type", {}).get("decision")
        != 1
    ):
        raise RuntimeError("populated qt memory status omitted catalog statistics")
    if not isinstance(memory_id, str) or not memory_id:
        raise RuntimeError("qt memory remember omitted its memory id")

    denied = invoke("memory", "forget", memory_id, "--json")
    if (
        denied.returncode == 0
        or denied.stdout.strip()
        or "requires --yes authority" not in denied.stderr
    ):
        raise RuntimeError("qt memory forget bypassed or polluted permission denial")

    disabled = invoke("memory", "status", "--json", "--no-memory")
    if (
        disabled.returncode == 0
        or disabled.stdout.strip()
        or "disabled for this invocation" not in disabled.stderr
    ):
        raise RuntimeError("qt memory --no-memory did not stop before structured output")

    for mode_name, mode_arguments in (
        ("print", ["--print"]),
        ("json", ["--mode", "json"]),
    ):
        mode_home = agent_home.parent / f"{mode_name}-no-memory-agent-home"
        mode_home.mkdir(parents=True, exist_ok=True)
        (mode_home / "settings.json").write_text(
            json.dumps(settings) + "\n", encoding="utf-8"
        )
        mode_environment = environment.copy()
        mode_environment["QT_CODING_AGENT_DIR"] = str(mode_home)
        mode = subprocess.run(
            [
                str(binary),
                *mode_arguments,
                "--no-memory",
                "--no-session",
                "--no-tools",
                "--offline",
                "credential-free no-memory mode probe",
            ],
            cwd=ROOT,
            env=mode_environment,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=30,
        )
        if (mode_home / "memory").exists():
            raise RuntimeError(f"{mode_name} --no-memory initialized durable memory")
        if mode_name == "json":
            for line in mode.stdout.splitlines():
                if not line.strip():
                    continue
                try:
                    json.loads(line)
                except json.JSONDecodeError as error:
                    raise RuntimeError(
                        f"json --no-memory polluted structured stdout: {error}"
                    ) from error

    rpc_frames = [
        {"type": "memory", "id": "memory-status", "action": "status", "params": {}},
        {
            "type": "memory",
            "id": "memory-denied",
            "action": "remember",
            "params": {"content": "RPC permission denial probe"},
            "confirm": False,
        },
        {
            "type": "memory",
            "id": "memory-created",
            "action": "remember",
            "params": {
                "content": "Decision: native RPC management smoke probe.",
                "status": "hypothesis",
            },
            "confirm": True,
        },
    ]
    rpc = subprocess.run(
        [
            str(binary),
            "--mode",
            "rpc",
            "--no-session",
            "--no-extensions",
            "--no-context-files",
            "--no-skills",
            "--no-tools",
        ],
        cwd=ROOT,
        env=environment,
        input="".join(json.dumps(frame) + "\n" for frame in rpc_frames),
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=30,
    )
    if rpc.returncode != 0:
        raise RuntimeError(f"qt memory RPC mode failed: {rpc.stderr}")
    responses: dict[str, dict] = {}
    for line in rpc.stdout.splitlines():
        if not line.strip():
            continue
        try:
            frame = json.loads(line)
        except json.JSONDecodeError as error:
            raise RuntimeError(f"memory RPC polluted structured stdout: {error}") from error
        if isinstance(frame, dict) and isinstance(frame.get("id"), str):
            responses[frame["id"]] = frame
    rpc_status = responses.get("memory-status", {})
    if not rpc_status.get("success"):
        raise RuntimeError("memory RPC status did not return a success response")
    rpc_statistics = rpc_status.get("data", {}).get("statistics")
    if (
        not isinstance(rpc_statistics, dict)
        or not isinstance(rpc_statistics.get("catalog"), dict)
        or not isinstance(rpc_statistics.get("usage"), dict)
        or not isinstance(rpc_statistics.get("governance"), dict)
        or not isinstance(rpc_statistics.get("dreaming"), dict)
    ):
        raise RuntimeError("memory RPC status omitted structured statistics")
    dreaming_statistics = rpc_statistics["dreaming"]
    for field in (
        "attempted",
        "completed",
        "failed",
        "pending",
        "protocol_rejections",
        "timed_out",
        "cancelled",
        "start_failures",
        "cpu_fallbacks",
        "reviews_created",
        "by_trigger",
        "by_model",
        "failures_by_reason",
    ):
        if field not in dreaming_statistics:
            raise RuntimeError(
                f"memory RPC status omitted Dreaming statistic {field}"
            )
    denied_rpc = responses.get("memory-denied", {})
    if denied_rpc.get("success") is not False or "confirm=true" not in denied_rpc.get(
        "error", ""
    ):
        raise RuntimeError("memory RPC mutation did not enforce explicit confirmation")
    created_rpc = responses.get("memory-created", {})
    if not created_rpc.get("success") or not created_rpc.get("data", {}).get("created"):
        raise RuntimeError("memory RPC confirmed mutation did not return structured success")

    rpc_disabled = subprocess.run(
        [str(binary), "--mode", "rpc", "--no-session", "--no-tools", "--no-memory"],
        cwd=ROOT,
        env=environment,
        input="".join(
            json.dumps(frame) + "\n"
            for frame in (
                {
                    "type": "memory",
                    "id": "disabled-status",
                    "action": "status",
                    "params": {},
                },
                {
                    "type": "memory",
                    "id": "disabled-write",
                    "action": "remember",
                    "params": {"content": "must not persist"},
                    "confirm": True,
                },
            )
        ),
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=30,
    )
    disabled_lines = [line for line in rpc_disabled.stdout.splitlines() if line.strip()]
    if len(disabled_lines) != 2:
        raise RuntimeError("--no-memory RPC did not emit two structured responses")
    disabled_responses = {
        response.get("id"): response
        for response in (json.loads(line) for line in disabled_lines)
    }
    disabled_status = disabled_responses.get("disabled-status", {})
    disabled_data = disabled_status.get("data", {})
    if (
        not disabled_status.get("success")
        or disabled_data.get("enabled") is not False
        or disabled_data.get("disabled_by_cli") is not True
    ):
        raise RuntimeError("--no-memory RPC status did not report the CLI override")
    disabled_write = disabled_responses.get("disabled-write", {})
    if disabled_write.get("success") is not False or "disabled" not in disabled_write.get(
        "error", ""
    ):
        raise RuntimeError("--no-memory RPC allowed a memory mutation")

    run_memory_rollout_probe(binary, agent_home.parent / "rollout-agent-home")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--binary",
        help="also exercise qt memory CLI/RPC surfaces with an existing qt binary",
    )
    args = parser.parse_args()
    BUILD.mkdir(parents=True, exist_ok=True)
    runtime = BUILD / "runtime"
    shutil.rmtree(runtime, ignore_errors=True)
    runtime.mkdir(parents=True)
    agent_home = runtime / "global-agent-home"

    pioenv = os.environ.get("PIOENV", "native")
    cxx = os.environ.get("CXX", "g++")
    command = [
        cxx,
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-pthread",
        "-Iinclude",
        f"-I.pio/libdeps/{pioenv}/argparse/include",
        f"-I.pio/libdeps/{pioenv}/json/include",
        str(SOURCE),
        "src/app/cli/cli.cpp",
        "src/memory/models.cpp",
        "src/memory/review.cpp",
        "src/memory/review_json.cpp",
        "src/memory/review_service.cpp",
        "src/memory/scope.cpp",
        "src/memory/storage/sqlite_memory_database.cpp",
        "src/memory/storage/sqlite_retrieval_links.cpp",
        "src/memory/storage/sqlite_retrieval_maintenance.cpp",
        "src/memory/storage/qtmem_v3_import.cpp",
        "src/memory/memory_service.cpp",
        "src/memory/governance_service.cpp",
        "src/memory/recall_service.cpp",
        "src/memory/retrieval_engine.cpp", "src/memory/retrieval_graph.cpp",
        "src/index/retrieval_snapshot.cpp",
        "src/memory/dense_retrieval.cpp",
        "src/memory/retrieval_indexer.cpp",
        "src/memory/context_service.cpp",
        "src/memory/ingestion_service.cpp",
        "src/memory/post_processing_service.cpp",
        "src/memory/runtime.cpp",
        "src/memory/coordinator.cpp",
        "src/memory/prehandler_protocol.cpp",
        "src/app/runtime/isolated_agent_runner.cpp",
        "src/tools/memory_tool.cpp",
        "src/app/rpc/rpc_protocol.cpp",
        "src/app/rpc/rpc_server.cpp",
        "src/tools/models.cpp",
        "src/tools/registry.cpp",
        "src/tools/tool_validation.cpp",
        "src/foundation/error.cpp",
        "src/foundation/filesystem.cpp",
        "src/foundation/git_url.cpp",
        "src/foundation/hash.cpp",
        "src/foundation/process.cpp",
        "src/foundation/uuid.cpp",
        "-lsqlite3",
        "-o",
        str(BINARY),
    ]
    subprocess.run(command, cwd=ROOT, check=True)

    environment = os.environ.copy()
    environment["QT_CODING_AGENT_DIR"] = str(agent_home)
    # The native memory probe must remain independent of qtmem's transport.
    environment["QTMEM_ENDPOINT"] = "http://127.0.0.1:1/unreachable-by-design"
    completed = subprocess.run(
        [str(BINARY), str(agent_home)], cwd=ROOT, env=environment, check=False
    )
    if completed.returncode != 0:
        return completed.returncode

    database = agent_home / "memory" / "memory.sqlite3"
    if not database.is_file():
        raise RuntimeError("native memory probe did not create the global database")
    if os.name != "nt":
        database_mode = stat.S_IMODE(database.stat().st_mode)
        directory_mode = stat.S_IMODE(database.parent.stat().st_mode)
        if database_mode != 0o600 or directory_mode != 0o700:
            raise RuntimeError(
                "global memory artifacts have insecure modes: "
                f"database={database_mode:o} directory={directory_mode:o}"
            )

    concurrent_home = runtime / "concurrent-agent-home"
    writers = [
        subprocess.Popen(
            [str(BINARY), "--concurrent-writer", str(concurrent_home), str(index)],
            cwd=ROOT,
            env=environment,
        )
        for index in range(4)
    ]
    for writer in writers:
        if writer.wait() != 0:
            raise RuntimeError("concurrent native memory writer failed")
    concurrent_db = concurrent_home / "memory" / "memory.sqlite3"
    with sqlite3.connect(concurrent_db) as connection:
        count = connection.execute(
            "SELECT COUNT(*) FROM memories WHERE project_id='concurrent-project'"
        ).fetchone()[0]
        if count != 4:
            raise RuntimeError(f"concurrent writers persisted {count} rows, expected 4")
        if connection.execute("PRAGMA quick_check").fetchone()[0] != "ok":
            raise RuntimeError("concurrent memory database failed integrity check")

    crash_script = """
import os, sqlite3, sys
db = sqlite3.connect(sys.argv[1])
db.execute('PRAGMA journal_mode=WAL')
db.execute('BEGIN IMMEDIATE')
db.execute("UPDATE memories SET content='uncommitted' WHERE id=(SELECT id FROM memories LIMIT 1)")
os._exit(17)
"""
    crashed = subprocess.run(
        [sys.executable, "-c", crash_script, str(concurrent_db)], check=False
    )
    if crashed.returncode != 17:
        raise RuntimeError("crash-recovery fixture did not terminate as expected")
    with sqlite3.connect(concurrent_db) as connection:
        if connection.execute("PRAGMA quick_check").fetchone()[0] != "ok":
            raise RuntimeError("memory WAL did not recover after interrupted writer")
    if args.binary:
        qt_binary = pathlib.Path(args.binary)
        if not qt_binary.is_absolute():
            qt_binary = (ROOT / qt_binary).resolve()
        if not qt_binary.is_file():
            raise RuntimeError(f"qt binary does not exist: {qt_binary}")
        run_native_surface_probe(qt_binary, runtime / "surface-agent-home")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
