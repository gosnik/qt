#!/usr/bin/env python3
"""Check real storage writes, including buffered disk-full failures on Linux."""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    output = ROOT / ".pio/test/storage-write-probe"
    output.mkdir(parents=True, exist_ok=True)
    binary = output / "storage_write_probe"
    subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Wpedantic",
        "-Iinclude", "-I.pio/libdeps/native/json/include",
        "scripts/storage_write_probe.cpp", "src/harness/session_repository.cpp",
        "src/harness/session.cpp", "src/foundation/filesystem.cpp",
        "src/foundation/error.cpp", "-o", str(binary),
    ], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix="qt-storage-probe-") as temporary:
        subprocess.run([str(binary), temporary], cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
