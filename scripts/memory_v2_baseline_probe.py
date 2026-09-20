#!/usr/bin/env python3
"""Compile and run the V2 labelled corpus against production RecallService."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / ".pio" / "test" / "memory-v2-baseline-probe"
BINARY = BUILD / "memory_v2_baseline_probe"
FIXTURE = ROOT / "scripts" / "fixtures" / "memory_v2_retrieval.json"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path, default=FIXTURE)
    parser.add_argument("--report", type=Path, default=None,
                        help="optional JSON output path for a machine-readable run report")
    args = parser.parse_args()
    env = os.environ.get("PIOENV", "native")
    cxx = os.environ.get("CXX", "g++")
    shutil.rmtree(BUILD, ignore_errors=True)
    BUILD.mkdir(parents=True)
    command = [
        cxx, "-std=c++17", "-Wall", "-Wextra", "-Wpedantic", "-pthread",
        "-Iinclude", f"-I.pio/libdeps/{env}/json/include",
        "-include", "include/qt/memory/scope.hpp",
        "scripts/memory_v2_baseline_probe.cpp",
        "src/memory/models.cpp", "src/memory/scope.cpp",
        "src/memory/storage/sqlite_memory_database.cpp",
        "src/memory/storage/sqlite_retrieval_links.cpp",
        "src/memory/recall_service.cpp", "src/memory/memory_service.cpp",
        "src/memory/review.cpp", "src/memory/review_json.cpp",
        "src/memory/governance_service.cpp",
        "src/foundation/error.cpp", "src/foundation/filesystem.cpp",
        "src/foundation/git_url.cpp", "src/foundation/hash.cpp",
        "-lsqlite3", "-o", str(BINARY),
    ]
    subprocess.run(command, cwd=ROOT, check=True)
    runtime = BUILD / "runtime"
    completed = subprocess.run(
        [str(BINARY), str(args.fixture), str(runtime)],
        cwd=ROOT, check=True, text=True, capture_output=True,
    )
    report = json.loads(completed.stdout)
    if args.report is not None:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"report": str(args.report) if args.report else None,
                      "metrics": report["metrics"]}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
