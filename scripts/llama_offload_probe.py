#!/usr/bin/env python3
"""Compile and run deterministic llama GPU offload planning checks."""

import os
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / ".pio" / "test" / "llama-offload-probe"
BINARY = BUILD / "llama_offload_probe"


def main() -> int:
    cxx = os.environ.get("CXX", "g++")
    BUILD.mkdir(parents=True, exist_ok=True)
    command = [
        cxx,
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Iinclude",
        str(ROOT / "scripts" / "llama_offload_probe.cpp"),
        str(ROOT / "src" / "ai" / "llama_offload.cpp"),
        "-o",
        str(BINARY),
    ]
    subprocess.run(command, cwd=ROOT, check=True)
    return subprocess.run([str(BINARY)], cwd=ROOT, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
