#!/usr/bin/env python3
"""Compile and run the focused OpenAI model discovery probe."""

from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    output = ROOT / ".pio/test/openai-discovery-probe"
    output.mkdir(parents=True, exist_ok=True)
    binary = output / "openai_discovery_probe"
    subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Wpedantic",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-Iinclude", "-I.pio/libdeps/native/json/include",
        "scripts/openai_discovery_probe.cpp", "src/ai/openai_model_discovery.cpp",
        "src/ai/model_catalog.cpp", "src/ai/models.cpp", "src/foundation/error.cpp",
        "-pthread", "-o", str(binary),
    ], cwd=ROOT, check=True)
    subprocess.run([str(binary)], cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
