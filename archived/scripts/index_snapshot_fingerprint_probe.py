#!/usr/bin/env python3
"""Compile and run persisted scan-fingerprint snapshot checks."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="qt-index-fingerprint-probe-") as directory:
    binary = Path(directory) / "probe"
    subprocess.run([
        "g++", "-std=c++17", "-I", str(ROOT / "include"),
        str(ROOT / "scripts/index_snapshot_fingerprint_probe.cpp"),
        str(ROOT / "src/index/sqlite_store.cpp"),
        str(ROOT / "src/index/retrieval_snapshot.cpp"), "-lsqlite3", "-o", str(binary),
    ], check=True, cwd=ROOT)
    subprocess.run([str(binary), str(Path(directory) / "fixture")], check=True, cwd=ROOT)
