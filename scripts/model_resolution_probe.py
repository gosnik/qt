#!/usr/bin/env python3
"""Compile and run the focused provider-scoped model-resolution probe."""

from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / ".pio" / "test" / "model-resolution-probe"


def main() -> int:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    binary = OUTPUT / "model_resolution_probe"
    command = [
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Wpedantic",
        "-Iinclude", "-I.pio/libdeps/native/json/include",
        "scripts/model_resolution_probe.cpp",
        "src/app/runtime/model_resolution.cpp",
        "src/app/runtime/provider_identity.cpp",
        "src/ai/model_catalog.cpp", "src/ai/auth_storage.cpp",
        "src/ai/models.cpp", "src/foundation/error.cpp",
        "src/foundation/filesystem.cpp", "src/foundation/process.cpp",
        "-pthread", "-o", str(binary),
    ]
    subprocess.run(command, cwd=ROOT, check=True)
    subprocess.run([str(binary)], cwd=ROOT, check=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        print(f"model resolution probe failed: {error}", file=sys.stderr)
        raise SystemExit(1)
