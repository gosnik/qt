#!/usr/bin/env python3
"""Compile and run the focused image render-cache regression probe."""

import pathlib
import subprocess


OBJECTS = (
    "src/tui/terminal/image.o",
    "src/tui/terminal/terminal.o",
    "src/tui/core/component.o",
    "src/tui/terminal/ansi.o",
    "src/tui/input/keys.o",
    "src/foundation/error.o",
    "src/foundation/utf8.o",
)


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[1]
    native = root / ".pio" / "build" / "native"
    missing = [path for path in OBJECTS if not (native / path).is_file()]
    if missing:
        raise RuntimeError(
            "native objects are missing; run `pio run -e native` first: "
            + ", ".join(missing)
        )

    build_dir = root / ".pio" / "test" / "image-render-cache"
    build_dir.mkdir(parents=True, exist_ok=True)
    probe_object = build_dir / "image_render_cache_probe.o"
    probe_binary = build_dir / "image_render_cache_probe"
    subprocess.run(
        [
            "g++",
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Wpedantic",
            "-Iinclude",
            "-I.pio/libdeps/native/uni-algo/include",
            "-c",
            "scripts/image_render_cache_probe.cpp",
            "-o",
            str(probe_object),
        ],
        cwd=root,
        check=True,
    )
    command = ["g++", "-pthread", str(probe_object)]
    command.extend(str(native / path) for path in OBJECTS)
    command.extend([
        str(native / "libf7a" / "libuni-algo.a"),
        str(native / "lib55d" / "libnotcurses.a"),
        str(native / "generated/sysroot/usr/lib/x86_64-linux-gnu/libunistring.a"),
        "-ltinfo", "-ldl", "-lrt", "-lz", "-o", str(probe_binary),
    ])
    subprocess.run(command, cwd=root, check=True)
    subprocess.run([str(probe_binary)], cwd=root, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
