#!/usr/bin/env python3
"""Compile and run prompt argument expansion boundary checks."""

from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    output = ROOT / ".pio/test/prompt-template-probe"
    output.mkdir(parents=True, exist_ok=True)
    binary = output / "prompt_template_probe"
    subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Wpedantic",
        "-Iinclude", "-I.pio/libdeps/native/json/include",
        "scripts/prompt_template_probe.cpp", "src/harness/prompt_templates.cpp",
        "src/foundation/error.cpp", "-o", str(binary),
    ], cwd=ROOT, check=True)
    subprocess.run([str(binary)], cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
