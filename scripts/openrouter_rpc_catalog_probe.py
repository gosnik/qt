#!/usr/bin/env python3
"""Verify RPC model commands consume the last-known OpenRouter catalog."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import select
import time
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="qt-openrouter-rpc-") as temporary:
        agent_home = Path(temporary)
        # A stale cache remains useful offline and must not disappear from RPC.
        cache = {"timestamp": 1, "models": [{
            "id": "probe/rpc-only", "display_name": "RPC discovery fixture",
            "reasoning": True, "vision": True,
            "metadata": {"contextWindow": 65536, "maxTokens": 4096},
        }]}
        (agent_home / "openrouter_models.json").write_text(json.dumps(cache))
        environment = dict(os.environ, QT_CODING_AGENT_DIR=temporary,
                           QT_OFFLINE="1")
        commands = [
            {"type": "get_available_models", "id": "list"},
            {"type": "set_model", "id": "select", "provider": "openrouter",
             "modelId": "probe/rpc-only"},
            {"type": "cycle_thinking_level", "id": "thinking"},
            {"type": "cycle_model", "id": "cycle"},
        ]
        with tempfile.TemporaryFile() as errors:
            process = subprocess.Popen(
                [str(args.binary.resolve()), "--mode", "rpc", "--offline",
                 "--no-session", "--no-memory", "--no-extensions",
                 "--no-context-files", "--no-skills", "--no-tools"],
                cwd=temporary, env=environment, stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=errors)
            pending = bytearray()

            def request(command):
                process.stdin.write((json.dumps(command) + "\n").encode())
                process.stdin.flush()
                deadline = time.monotonic() + 30
                while True:
                    while b"\n" in pending:
                        line, _, remaining = pending.partition(b"\n")
                        pending[:] = remaining
                        frame = json.loads(line)
                        if frame.get("id") == command["id"]:
                            assert frame.get("success"), frame
                            return frame
                    remaining_time = deadline - time.monotonic()
                    if remaining_time <= 0 or not select.select(
                            [process.stdout], [], [], remaining_time)[0]:
                        raise TimeoutError(f"RPC response timed out: {command}")
                    chunk = os.read(process.stdout.fileno(), 65536)
                    if not chunk:
                        errors.seek(0)
                        raise RuntimeError(errors.read().decode())
                    pending.extend(chunk)

            try:
                responses = {command["id"]: request(command)
                             for command in commands}
                cache["models"][0]["id"] = "probe/rpc-refreshed"
                cache["models"][0]["display_name"] = "Updated RPC fixture"
                (agent_home / "openrouter_models.json").write_text(
                    json.dumps(cache))
                refreshed = request({"type": "get_available_models",
                                     "id": "refreshed-list"})["data"]["models"]
                discovered = {model["id"]: model for model in refreshed
                              if model["provider"] == "openrouter"}
                assert "probe/rpc-only" not in discovered, discovered
                assert discovered["probe/rpc-refreshed"]["displayName"] == (
                    "Updated RPC fixture")
                selected = request({
                    "type": "set_model", "id": "refreshed-select",
                    "provider": "openrouter", "modelId": "probe/rpc-refreshed"})
                assert selected["data"] == {
                    "provider": "openrouter", "id": "probe/rpc-refreshed"}
                process.stdin.close()
                assert process.wait(timeout=10) == 0
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=10)
                process.stdout.close()
                if not process.stdin.closed:
                    process.stdin.close()
        models = responses["list"]["data"]["models"]
        index = next(i for i, model in enumerate(models)
                     if model["provider"] == "openrouter"
                     and model["id"] == "probe/rpc-only")
        assert models[index]["vision"] and models[index]["reasoning"]
        assert models[index]["displayName"] == "RPC discovery fixture"
        assert responses["select"]["data"] == {
            "provider": "openrouter", "id": "probe/rpc-only"}
        assert responses["thinking"]["data"] is not None
        expected = models[(index + 1) % len(models)]
        assert responses["cycle"]["data"]["model"] == {
            "provider": expected["provider"], "id": expected["id"]}
    print("OpenRouter RPC catalog probe passed")


if __name__ == "__main__":
    main()
