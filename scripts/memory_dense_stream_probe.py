#!/usr/bin/env python3
"""Compile and run the production bounded dense stream publication probe."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PIOENV = os.environ.get("PIOENV", "native")
SOURCES = [
    "scripts/memory_dense_stream_probe.cpp",
    "src/memory/retrieval_indexer.cpp",
    "src/memory/storage/sqlite_memory_database.cpp",
    "src/memory/storage/sqlite_retrieval_links.cpp",
    "src/memory/models.cpp", "src/memory/review.cpp", "src/memory/review_json.cpp",
    "src/memory/scope.cpp", "src/memory/memory_service.cpp",
    "src/foundation/error.cpp", "src/foundation/filesystem.cpp",
    "src/foundation/git_url.cpp", "src/foundation/hash.cpp",
]

with tempfile.TemporaryDirectory(prefix="qt-memory-dense-stream-build-") as build_dir:
    binary = Path(build_dir) / "probe"
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra",
        "-Wpedantic", "-pthread", "-Iinclude",
        f"-I.pio/libdeps/{PIOENV}/json/include", *[str(ROOT / source) for source in SOURCES],
        "-lsqlite3", "-o", str(binary),
    ], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix="qt-memory-dense-stream-db-") as db_dir:
        subprocess.run([str(binary), db_dir], cwd=ROOT, check=True)
