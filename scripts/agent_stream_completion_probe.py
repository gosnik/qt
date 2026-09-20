#!/usr/bin/env python3
"""Build and run agent stream-completion regressions against native objects."""

from __future__ import annotations

import pathlib
import subprocess

from agent_harness_cache_probe import OBJECTS


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[1]
    native_build = root / ".pio" / "build" / "native"
    missing = [path for path in OBJECTS if not (native_build / path).is_file()]
    if missing:
        raise RuntimeError(
            "native objects are missing; run `pio run -e native` first: "
            + ", ".join(missing)
        )

    build_dir = root / ".pio" / "test" / "agent-stream-completion"
    build_dir.mkdir(parents=True, exist_ok=True)
    probe_object = build_dir / "agent_stream_completion_probe.o"
    probe_binary = build_dir / "agent_stream_completion_probe"
    subprocess.run(
        [
            "g++",
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Wpedantic",
            "-pthread",
            "-Iinclude",
            "-I.pio/libdeps/native/json/include",
            "-c",
            "scripts/agent_stream_completion_probe.cpp",
            "-o",
            str(probe_object),
        ],
        cwd=root,
        check=True,
    )
    subprocess.run(
        [
            "g++",
            "-pthread",
            str(probe_object),
            *(str(native_build / path) for path in OBJECTS),
            "-lssl",
            "-lcrypto",
            "-lz",
            "-ldl",
            "-lrt",
            "-o",
            str(probe_binary),
        ],
        cwd=root,
        check=True,
    )
    subprocess.run([str(probe_binary)], cwd=root, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
