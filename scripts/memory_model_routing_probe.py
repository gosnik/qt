#!/usr/bin/env python3
"""Compile and run deterministic built-in memory-model routing checks."""

import os
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / ".pio" / "test" / "memory-model-routing-probe"
BINARY = BUILD / "memory_model_routing_probe"


def main() -> int:
    pioenv = os.environ.get("PIOENV", "native")
    cxx = os.environ.get("CXX", "g++")
    BUILD.mkdir(parents=True, exist_ok=True)
    runtime = BUILD / "runtime"
    shutil.rmtree(runtime, ignore_errors=True)
    runtime.mkdir()
    command = [
        cxx,
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-ffunction-sections",
        "-fdata-sections",
        "-Iinclude",
        f"-I.pio/libdeps/{pioenv}/json/include",
        str(ROOT / "scripts" / "memory_model_routing_probe.cpp"),
        str(ROOT / "src" / "ai" / "llama_model_discovery.cpp"),
        "-Wl,--gc-sections",
        "-o",
        str(BINARY),
    ]
    subprocess.run(command, cwd=ROOT, check=True)
    return subprocess.run([str(BINARY), str(runtime)], cwd=ROOT, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
