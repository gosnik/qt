#!/usr/bin/env python3
"""Compile and run the mixed memory/code retrieval graph probe."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="qt-retrieval-graph-") as directory:
    binary = Path(directory) / "probe"
    subprocess.run([
        "c++", "-std=c++17", "-Iinclude",
        str(ROOT / "scripts/retrieval_graph_probe.cpp"),
        str(ROOT / "src/memory/retrieval_graph.cpp"),
        str(ROOT / "src/index/retrieval_snapshot.cpp"),
        str(ROOT / "src/memory/models.cpp"), str(ROOT / "src/memory/scope.cpp"),
        "-o", str(binary),
    ], cwd=ROOT, check=True)
    subprocess.run([str(binary)], cwd=ROOT, check=True)
