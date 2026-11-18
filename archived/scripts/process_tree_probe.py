#!/usr/bin/env python3
"""Build and run the process-group/descendant cancellation probe."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main() -> int:
    with tempfile.TemporaryDirectory(prefix="qt-process-tree-") as directory:
        out = Path(directory)
        binary = out / "process_tree_probe"
        marker = out / "descendant.pid"
        subprocess.run([
            "g++", "-std=c++17", "-pthread", "-I", str(ROOT / "include"),
            str(ROOT / "scripts/process_tree_probe.cpp"),
            str(ROOT / "src/foundation/process.cpp"),
            str(ROOT / "src/foundation/filesystem.cpp"),
            str(ROOT / "src/foundation/error.cpp"),
            "-o", str(binary),
        ], check=True, cwd=ROOT)
        subprocess.run([str(binary), str(marker)], check=True, cwd=ROOT)
    print("process tree probe passed")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
