#!/usr/bin/env python3
"""Verify stable review RPC errors and pollable batch job schema."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=pathlib.Path, required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="qt-review-rpc-") as temporary:
        agent_home = pathlib.Path(temporary)
        (agent_home / "settings.json").write_text(
            json.dumps({"memory": {"enabled": True}}), encoding="utf-8"
        )
        environment = os.environ.copy()
        environment["QT_CODING_AGENT_DIR"] = str(agent_home)
        frames = (
            {
                "type": "memory",
                "id": "bad-schema",
                "action": "review",
                "params": {"action": 7},
            },
            {
                "type": "memory",
                "id": "missing-job",
                "action": "review",
                "params": {"action": "batch_status", "job_id": "unknown"},
            },
            {
                "type": "memory",
                "id": "bad-batch",
                "action": "review",
                "params": {"action": "batch_start", "items": "not-an-array"},
                "confirm": True,
            },
        )
        completed = subprocess.run(
            [
                str(args.binary.resolve()),
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
            input="".join(json.dumps(frame) + "\n" for frame in frames),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=30,
            check=False,
        )
        if completed.returncode != 0:
            raise RuntimeError(completed.stderr)
        responses = {
            value["id"]: value
            for line in completed.stdout.splitlines()
            if line.strip()
            for value in (json.loads(line),)
            if isinstance(value, dict) and isinstance(value.get("id"), str)
        }
        expected = {
            "bad-schema": "invalid_review_schema",
            "missing-job": "review_not_found",
            "bad-batch": "invalid_review_schema",
        }
        for identifier, code in expected.items():
            response = responses.get(identifier, {})
            if response.get("success") is not False:
                raise RuntimeError(f"{identifier} unexpectedly succeeded: {response}")
            if response.get("error_code") != code:
                raise RuntimeError(
                    f"{identifier} returned {response.get('error_code')!r}, expected {code!r}"
                )
            if not isinstance(response.get("error"), str) or not response["error"]:
                raise RuntimeError(f"{identifier} omitted a human-readable error")
        print(json.dumps({"review_rpc_contract": True, "cases": expected}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
