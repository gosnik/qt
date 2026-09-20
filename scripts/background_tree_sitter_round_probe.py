#!/usr/bin/env python3
"""Compile and run background Tree-sitter round and status-routing regression checks."""

import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    native = ROOT / ".pio" / "build" / "native"
    objects = [path for path in sorted(native.glob("src/**/*.o"))
               if path.name != "main.o"]
    objects += sorted(native.glob("lib*/*.a"))
    objects += sorted(native.glob("llama.cpp/**/*.a"))
    if not objects:
        raise RuntimeError("native objects are missing; run pio run -e native")

    with tempfile.TemporaryDirectory(prefix="qt-background-tree-sitter-") as temp:
        directory = Path(temp)
        response = directory / "objects.rsp"
        response.write_text("\n".join(map(str, objects)) + "\n")
        binary = directory / "probe"
        includes = [
            "-Iinclude", "-Isrc/index/tree_sitter_runtime",
            "-I.pio/build/native/generated/sysroot/usr/include",
            "-Ilib/boost_1_86_0", "-I.pio/libdeps/native/json/include",
            "-Ilib/llama.cpp/include", "-Ilib/llama.cpp/ggml/include",
            "-Ilib/llama.cpp/ggml/src", "-Ilib/llama.cpp/src",
        ]
        command = [
            os.environ.get("CXX", "g++"), "-std=c++17", *includes, "-pthread",
            str(ROOT / "scripts" / "background_tree_sitter_round_probe.cpp"),
            "-Wl,--start-group", "@" + str(response), "-Wl,--end-group",
            "-L.pio/build/native/generated/sysroot/usr/lib/x86_64-linux-gnu",
            "-lunistring", "-lsqlite3", "-lvulkan", "-lssl", "-lcrypto",
            "-lz", "-lzstd", "-ltinfo", "-ldl", "-lm", "-lrt",
            "-o", str(binary),
        ]
        subprocess.run(command, cwd=ROOT, check=True, timeout=120)
        return subprocess.run([str(binary), str(directory / "fixture")], cwd=ROOT,
                              check=False, timeout=60).returncode


if __name__ == "__main__":
    raise SystemExit(main())
