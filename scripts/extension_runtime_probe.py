#!/usr/bin/env python3
"""Build and run the engine-backed persistent AngelScript runtime probe."""

from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    output = ROOT / ".pio/test/extension-runtime-probe"
    output.mkdir(parents=True, exist_ok=True)
    binary = output / "extension_runtime_probe"
    angelscript_sources = sorted((ROOT / "lib/angelscript/src").glob("*.cpp"))
    command = [
        "g++", "-std=c++17", "-O0", "-Wall", "-Wextra", "-Wpedantic",
        "-Iinclude", "-Ilib/angelscript/include",
        "scripts/extension_runtime_probe.cpp", "src/extensions/angelscript_host.cpp",
        "src/extensions/api_compatibility.cpp", "src/extensions/manifest.cpp",
        "src/foundation/error.cpp", *map(str, angelscript_sources),
        "-pthread", "-o", str(binary),
    ]
    subprocess.run(command, cwd=ROOT, check=True)
    subprocess.run([str(binary)], cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
