#!/usr/bin/env python3
"""Build and run the persisted cross-store link storage probe."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
pioenv = os.environ.get("PIOENV", "native")
sources = [
    ROOT / "scripts/retrieval_links_storage_probe.cpp",
    ROOT / "src/memory/storage/sqlite_memory_database.cpp",
    ROOT / "src/memory/storage/sqlite_retrieval_links.cpp",
    ROOT / "src/memory/models.cpp", ROOT / "src/memory/review.cpp",
    ROOT / "src/memory/review_json.cpp", ROOT / "src/memory/scope.cpp",
    ROOT / "src/memory/memory_service.cpp", ROOT / "src/foundation/error.cpp",
    ROOT / "src/foundation/filesystem.cpp", ROOT / "src/foundation/git_url.cpp",
    ROOT / "src/foundation/hash.cpp",
]
with tempfile.TemporaryDirectory(prefix="qt-link-probe-") as build:
    binary = Path(build) / "probe"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-pthread", "-Iinclude",
                    f"-I.pio/libdeps/{pioenv}/json/include", *map(str, sources),
                    "-lsqlite3", "-o", str(binary)], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix="qt-link-db-") as database:
        subprocess.run([str(binary), database], cwd=ROOT, check=True)
