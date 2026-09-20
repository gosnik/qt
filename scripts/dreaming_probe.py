#!/usr/bin/env python3
"""Compile and run the focused built-in Dreaming lifecycle probe."""

from pathlib import Path
import os
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / ".pio" / "test" / "dreaming-probe"
BINARY = BUILD / "dreaming_probe"


def main() -> int:
    pioenv = os.environ.get("PIOENV", "native")
    cxx = os.environ.get("CXX", "g++")
    shutil.rmtree(BUILD, ignore_errors=True)
    BUILD.mkdir(parents=True)
    command = [
        cxx,
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-pthread",
        "-Iinclude",
        f"-I.pio/libdeps/{pioenv}/json/include",
        "scripts/dreaming_probe.cpp",
        "src/ai/models.cpp",
        "src/memory/models.cpp",
        "src/memory/review.cpp",
        "src/memory/review_json.cpp",
        "src/memory/scope.cpp",
        "src/memory/storage/sqlite_memory_database.cpp",
        "src/memory/storage/sqlite_retrieval_links.cpp",
        "src/memory/storage/sqlite_retrieval_maintenance.cpp",
        "src/memory/memory_service.cpp",
        "src/memory/governance_service.cpp",
        "src/memory/recall_service.cpp",
        "src/memory/context_service.cpp",
        "src/memory/ingestion_service.cpp",
        "src/memory/post_processing_service.cpp",
        "src/memory/review_service.cpp",
        "src/memory/runtime.cpp",
        "src/memory/coordinator.cpp",
        "src/memory/prehandler_protocol.cpp",
        "src/memory/dense_retrieval.cpp",
        "src/memory/retrieval_engine.cpp",
        "src/memory/retrieval_indexer.cpp",
        "src/memory/retrieval_graph.cpp",
        "src/index/retrieval_snapshot.cpp",
        "src/app/runtime/isolated_agent_runner.cpp",
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
    runtime = BUILD / "runtime"
    runtime.mkdir()
    completed = subprocess.run([str(BINARY), str(runtime)], cwd=ROOT, check=False)
    if completed.returncode != 0:
        return completed.returncode
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        print(f"dreaming probe failed: {error}", file=sys.stderr)
        raise SystemExit(1)
