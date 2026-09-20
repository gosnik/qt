#!/usr/bin/env python3
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sources = [
    ROOT / "scripts/retrieval_search_probe.cpp",
    ROOT / "src/index/service.cpp", ROOT / "src/index/sqlite_store.cpp",
    ROOT / "src/index/retrieval_snapshot.cpp",
]
with tempfile.TemporaryDirectory(prefix="qt-retrieval-search-") as directory:
    binary = Path(directory) / "probe"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-ffunction-sections", "-fdata-sections",
                    "-Wl,--gc-sections", "-I", str(ROOT / "include"),
                    "-I", str(ROOT / ".pio" / "libdeps" / os.environ.get("PIOENV", "native") / "json" / "include"),
                    *map(str, sources), "-lsqlite3", "-pthread", "-o", str(binary)],
                   cwd=ROOT, check=True)
    subprocess.run([str(binary)], cwd=ROOT, check=True)
