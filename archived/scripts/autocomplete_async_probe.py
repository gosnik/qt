#!/usr/bin/env python3
"""Compile and run the latest-only asynchronous attachment-completion probe."""

import pathlib
import subprocess


OBJECTS = (
    "src/tui/widgets/autocomplete.o",
    "src/tui/utils/fuzzy.o",
    "src/foundation/error.o",
    "src/foundation/process.o",
    "src/foundation/filesystem.o",
    "src/foundation/utf8.o",
)


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[1]
    native = root / ".pio" / "build" / "native"
    missing = [item for item in OBJECTS if not (native / item).is_file()]
    if missing:
        raise RuntimeError("native objects are missing; run `pio run -e native` first: "
                           + ", ".join(missing))
    output = root / ".pio" / "test" / "autocomplete-async"
    output.mkdir(parents=True, exist_ok=True)
    object_file = output / "probe.o"
    binary = output / "probe"
    subprocess.run(["g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Wpedantic",
                    "-pthread", "-Iinclude", "-I.pio/libdeps/native/uni-algo/include",
                    "-c", "scripts/autocomplete_async_probe.cpp", "-o", str(object_file)],
                   cwd=root, check=True)
    subprocess.run(["g++", "-pthread", str(object_file),
                    *[str(native / item) for item in OBJECTS],
                    str(native / "libf7a" / "libuni-algo.a"),
                    "-ldl", "-lrt", "-lz", "-o", str(binary)], cwd=root, check=True)
    subprocess.run([str(binary)], cwd=root, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
