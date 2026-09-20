#!/usr/bin/env python3
"""Build and run the synthetic dense retrieval adapter probe."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
pioenv = os.environ.get("PIOENV", "native")
sources = [
    ROOT / "scripts/dense_retrieval_probe.cpp",
    ROOT / "src/memory/dense_retrieval.cpp",
    ROOT / "src/memory/retrieval_indexer.cpp",
    ROOT / "src/memory/storage/sqlite_memory_database.cpp",
    ROOT / "src/memory/storage/sqlite_retrieval_links.cpp",
    ROOT / "src/memory/models.cpp", ROOT / "src/memory/review.cpp",
    ROOT / "src/memory/review_json.cpp", ROOT / "src/memory/scope.cpp",
    ROOT / "src/memory/memory_service.cpp", ROOT / "src/foundation/error.cpp",
    ROOT / "src/foundation/filesystem.cpp", ROOT / "src/foundation/git_url.cpp",
    ROOT / "src/foundation/hash.cpp",
]
with tempfile.TemporaryDirectory(prefix="qt-dense-retrieval-") as directory:
    binary = Path(directory) / "probe"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-pthread", "-Iinclude",
                    f"-I.pio/libdeps/{pioenv}/json/include", *map(str, sources),
                    "-lsqlite3", "-o", str(binary)], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix="qt-dense-db-") as db_dir:
        subprocess.run([str(binary), db_dir], cwd=ROOT, check=True)
