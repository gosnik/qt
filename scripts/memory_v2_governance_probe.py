#!/usr/bin/env python3
"""Compile and run the V2 governance atomicity probe."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / ".pio/test/memory-v2-governance"

def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    env = os.environ.get("PIOENV", "native")
    sources = [
        "scripts/memory_v2_governance_probe.cpp",
        "src/memory/models.cpp", "src/memory/review.cpp", "src/memory/review_json.cpp",
        "src/memory/scope.cpp", "src/memory/memory_service.cpp",
        "src/memory/governance_service.cpp", "src/memory/review_service.cpp",
        "src/memory/storage/sqlite_memory_database.cpp",
        "src/memory/storage/sqlite_retrieval_links.cpp",
        "src/foundation/error.cpp", "src/foundation/filesystem.cpp",
        "src/foundation/git_url.cpp", "src/foundation/hash.cpp",
    ]
    binary = BUILD / "probe"
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra",
        "-Wpedantic", "-pthread", "-Iinclude",
        f"-I.pio/libdeps/{env}/json/include", *sources, "-lsqlite3", "-o", str(binary)
    ], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix="memory-v2-governance-") as directory:
        subprocess.run([str(binary), directory], cwd=ROOT, check=True)

if __name__ == "__main__":
    main()
