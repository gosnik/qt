#!/usr/bin/env python3
"""Build and run the provider queue cancellation/timeout probe."""

import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    cxx = os.environ.get("CXX", "g++")
    with tempfile.TemporaryDirectory(prefix="qt-llama-queue-") as temp:
        temp_path = Path(temp)
        binary = temp_path / "probe"
        objects_file = temp_path / "objects.rsp"
        native = ROOT / ".pio" / "build" / "native"
        objects = sorted(native.glob("src/**/*.o"))
        objects = [item for item in objects if item.name != "main.o"]
        objects += sorted(native.glob("lib*/*.a"))
        objects += sorted(native.glob("llama.cpp/**/*.a"))
        objects_file.write_text("\n".join(map(str, objects)) + "\n")
        includes = [
            "-Iinclude", "-Isrc/index/tree_sitter_runtime",
            "-I.pio/build/native/generated/sysroot/usr/include",
            "-Ilib/boost_1_86_0", "-I.pio/libdeps/native/json/include",
            "-Ilib/llama.cpp/include", "-Ilib/llama.cpp/ggml/include",
            "-Ilib/llama.cpp/ggml/src", "-Ilib/llama.cpp/src",
        ]
        command = [cxx, "-std=c++17", *includes, "-pthread",
                   str(ROOT / "scripts" / "llama_embedding_queue_probe.cpp"),
                   "-Wl,--start-group", "@" + str(objects_file),
                   "-Wl,--end-group",
                   "-L.pio/build/native/generated/sysroot/usr/lib/x86_64-linux-gnu",
                   "-lunistring", "-lsqlite3", "-lvulkan", "-lssl", "-lcrypto",
                   "-lz", "-lzstd", "-ltinfo", "-ldl", "-lm", "-lrt",
                   "-o", str(binary)]
        subprocess.run(command, cwd=ROOT, check=True, timeout=120)
        return subprocess.run([str(binary)], cwd=ROOT, check=False,
                               timeout=30).returncode


if __name__ == "__main__":
    raise SystemExit(main())
