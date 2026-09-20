#!/usr/bin/env python3
"""Compile and run the bounded native memory graph recall probe."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sources = [
    ROOT / "scripts/memory_graph_recall_probe.cpp",
    ROOT / "src/memory/recall_service.cpp",
    ROOT / "src/memory/scope.cpp",
    ROOT / "src/memory/models.cpp",
    ROOT / "src/memory/governance_service.cpp",
    ROOT / "src/memory/memory_service.cpp",
    ROOT / "src/memory/context_service.cpp",
    ROOT / "src/memory/review.cpp",
]
with tempfile.TemporaryDirectory(prefix="qt-memory-graph-") as directory:
    binary = Path(directory) / "probe"
    subprocess.run(["g++", "-std=c++17", "-I", str(ROOT / "include"),
                    *map(str, sources), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
