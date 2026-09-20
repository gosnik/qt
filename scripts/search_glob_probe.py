#!/usr/bin/env python3

import os
import pathlib
import shutil
import subprocess
import sys


def main() -> int:
    if shutil.which("rg") is None:
        raise RuntimeError("search glob probe requires ripgrep for backend comparison")
    root = pathlib.Path(__file__).resolve().parents[1]
    runtime = root / ".pio" / "test" / "search-glob-probe"
    shutil.rmtree(runtime, ignore_errors=True)
    runtime.mkdir(parents=True)
    pioenv = os.environ.get("PIOENV", "native")
    probe = runtime / "search_glob_probe"
    subprocess.run(
        [
            os.environ.get("CXX", "g++"),
            "-std=c++17", "-Wall", "-Wextra", "-Wpedantic", "-pthread",
            "-Iinclude", f"-I.pio/libdeps/{pioenv}/json/include",
            "scripts/search_glob_probe.cpp",
            "src/tools/search_text_tool.cpp", "src/tools/models.cpp",
            "src/tools/tool_validation.cpp", "src/foundation/error.cpp",
            "src/foundation/filesystem.cpp", "src/foundation/process.cpp",
            "-o", str(probe),
        ],
        cwd=root,
        check=True,
    )

    normal_env = os.environ.copy()
    subprocess.run([str(probe), str(runtime / "normal")],
                   cwd=root, env=normal_env, check=True)

    fallback_env = os.environ.copy()
    fallback_env["PATH"] = "/nonexistent"
    subprocess.run([str(probe), str(runtime / "fallback")],
                   cwd=root, env=fallback_env, check=True)
    print("search glob probe passed with rg and native fallback")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        print(f"search glob probe failed: {error}", file=sys.stderr)
        raise
