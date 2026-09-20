#!/usr/bin/env python3
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sources = [
    ROOT / "scripts/mcp_v1_contract_probe.cpp",
    ROOT / "src/index/mcp_http_code_index.cpp", ROOT / "src/index/service.cpp",
    ROOT / "src/index/code_index.cpp", ROOT / "src/index/retrieval_snapshot.cpp",
    ROOT / "src/index/sqlite_store.cpp",
]
with tempfile.TemporaryDirectory(prefix="qt-mcp-v1-contract-") as directory:
    binary = Path(directory) / "probe"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-ffunction-sections",
                    "-fdata-sections", "-Wl,--gc-sections", "-Iinclude",
                    f"-I.pio/libdeps/{os.environ.get('PIOENV', 'native')}/json/include",
                    *map(str, sources), "-lsqlite3", "-pthread", "-o", str(binary)],
                   cwd=ROOT, check=True)
    subprocess.run([str(binary)], cwd=ROOT, check=True)
