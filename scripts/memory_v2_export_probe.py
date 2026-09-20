#!/usr/bin/env python3
"""Compile and run the canonical structured memory export/import probe."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
pioenv = os.environ.get("PIOENV", "native")
sources = [
    ROOT / "scripts/memory_v2_export_probe.cpp",
    ROOT / "src/memory/storage/sqlite_memory_database.cpp",
    ROOT / "src/memory/storage/sqlite_retrieval_links.cpp",
    ROOT / "src/memory/models.cpp", ROOT / "src/memory/review.cpp",
    ROOT / "src/memory/review_json.cpp", ROOT / "src/memory/scope.cpp",
    ROOT / "src/memory/memory_service.cpp", ROOT / "src/foundation/error.cpp",
    ROOT / "src/foundation/filesystem.cpp", ROOT / "src/foundation/git_url.cpp",
    ROOT / "src/foundation/hash.cpp",
]
with tempfile.TemporaryDirectory(prefix="qt-export-probe-") as build_dir:
    binary = Path(build_dir) / "probe"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-pthread", "-Iinclude",
                    f"-I.pio/libdeps/{pioenv}/json/include", *map(str, sources),
                    "-lsqlite3", "-o", str(binary)], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix="qt-export-source-") as source_dir, \
         tempfile.TemporaryDirectory(prefix="qt-export-target-") as target_dir:
        subprocess.run([str(binary), source_dir, target_dir], cwd=ROOT, check=True)
