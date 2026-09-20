#!/usr/bin/env python3
"""Compile and run the focused OpenRouter catalog/cache normalization probe."""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / ".pio" / "test" / "openrouter-catalog-probe"


def main() -> int:
    BUILD.mkdir(parents=True, exist_ok=True)
    binary = BUILD / "openrouter_catalog_probe"
    command = [
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Wpedantic",
        "-Iinclude", "-I.pio/libdeps/native/json/include",
        "scripts/openrouter_catalog_probe.cpp",
        "src/ai/openrouter_model_discovery.cpp",
        "src/ai/model_catalog.cpp", "src/foundation/error.cpp",
        "-pthread", "-o", str(binary),
    ]
    subprocess.run(command, cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix="qt-openrouter-catalog-probe-") as tmp:
        subprocess.run([str(binary), tmp], cwd=ROOT, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
