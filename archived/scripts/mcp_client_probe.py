#!/usr/bin/env python3
import os
import pathlib
import subprocess
import sys


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[1]
    build_dir = root / ".pio" / "test" / "mcp-client-probe"
    build_dir.mkdir(parents=True, exist_ok=True)
    binary = build_dir / "mcp_client_probe"
    env = os.environ.copy()
    pioenv = env.get("PIOENV", "native")
    cxx = env.get("CXX", "g++")

    command = [
        cxx,
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Iinclude",
        f"-I.pio/libdeps/{pioenv}/json/include",
        "-Ilib/boost_1_86_0",
        "scripts/mcp_client_probe.cpp",
        "src/tools/mcp_client.cpp",
        "src/tools/tool_validation.cpp",
        "src/tools/models.cpp",
        "src/tools/registry.cpp",
        "src/foundation/error.cpp",
        "src/foundation/filesystem.cpp",
        "src/foundation/process.cpp",
        "-lssl",
        "-lcrypto",
        "-pthread",
        "-o",
        str(binary),
    ]
    subprocess.run(command, cwd=root, check=True)
    subprocess.run(
        [str(binary), sys.executable, str(build_dir / "runtime")],
        cwd=root,
        check=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
