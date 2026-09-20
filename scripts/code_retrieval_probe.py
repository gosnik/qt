#!/usr/bin/env python3
"""Compile and run the focused native V2-02 retrieval probe."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="qt-retrieval-probe-") as directory:
    binary = Path(directory) / "probe"
    subprocess.run([
        "g++", "-std=c++17", "-I", str(ROOT / "include"),
        str(ROOT / "scripts/code_retrieval_probe.cpp"),
        str(ROOT / "src/index/retrieval_snapshot.cpp"),
        "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
