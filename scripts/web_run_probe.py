#!/usr/bin/env python3
"""Compile and run the focused optimized web-access contract probe."""

import os
import pathlib
import subprocess


ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / ".pio" / "test" / "web-run-probe"
SOURCE = ROOT / "scripts" / "web_run_probe.cpp"
BINARY = BUILD / "web_run_probe"


def main() -> int:
    BUILD.mkdir(parents=True, exist_ok=True)
    pioenv = os.environ.get("PIOENV", "native")
    cxx = os.environ.get("CXX", "g++")
    command = [
        cxx,
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Iinclude",
        f"-I.pio/libdeps/{pioenv}/json/include",
        "-Ilib/boost_1_86_0",
        str(SOURCE),
        "src/tools/web_tool.cpp",
        "src/tools/models.cpp",
        "src/tools/tool_validation.cpp",
        "src/index/mcp_http_fetch.cpp",
        "src/index/string_util.cpp",
        "src/foundation/error.cpp",
        "src/foundation/filesystem.cpp",
        "-lssl",
        "-lcrypto",
        "-pthread",
        "-o",
        str(BINARY),
    ]
    subprocess.run(command, cwd=ROOT, check=True)
    return subprocess.run([str(BINARY)], cwd=ROOT, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
