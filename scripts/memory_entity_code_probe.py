#!/usr/bin/env python3
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sources = [
    ROOT / "scripts/memory_entity_code_probe.cpp",
    ROOT / "src/memory/retrieval_engine.cpp", ROOT / "src/memory/dense_retrieval.cpp",
    ROOT / "src/memory/retrieval_indexer.cpp", ROOT / "src/memory/storage/sqlite_memory_database.cpp",
    ROOT / "src/memory/context_service.cpp", ROOT / "src/memory/recall_service.cpp",
    ROOT / "src/memory/retrieval_explanation.cpp",
    ROOT / "src/memory/governance_service.cpp", ROOT / "src/memory/storage/sqlite_retrieval_links.cpp",
    ROOT / "src/memory/retrieval_graph.cpp", ROOT / "src/memory/models.cpp",
    ROOT / "src/memory/review.cpp", ROOT / "src/memory/review_json.cpp", ROOT / "src/memory/scope.cpp",
    ROOT / "src/memory/memory_service.cpp", ROOT / "src/foundation/error.cpp",
    ROOT / "src/foundation/filesystem.cpp", ROOT / "src/foundation/git_url.cpp",
    ROOT / "src/foundation/hash.cpp", ROOT / "src/index/retrieval_snapshot.cpp",
]
with tempfile.TemporaryDirectory(prefix="qt-memory-entity-code-") as directory:
    binary = Path(directory) / "probe"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-pthread", "-ffunction-sections",
                    "-fdata-sections", "-Wl,--gc-sections", "-Iinclude",
                    "-I.pio/libdeps/native/json/include", *map(str, sources),
                    "-lsqlite3", "-o", str(binary)], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix="qt-memory-entity-code-home-") as home:
        subprocess.run([str(binary), home], cwd=ROOT, check=True)
