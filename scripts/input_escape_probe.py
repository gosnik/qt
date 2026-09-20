#!/usr/bin/env python3

import pathlib
import subprocess


OBJECTS = (
    "src/tui/input/input_decoder.o",
    "src/tui/input/stdin_buffer.o",
    "src/tui/input/keybindings.o",
    "src/tui/input/keys.o",
    "src/tui/input/kill_ring.o",
    "src/tui/core/component.o",
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

    build_dir = root / ".pio" / "test" / "input-escape"
    build_dir.mkdir(parents=True, exist_ok=True)
    probe_object = build_dir / "input_escape_probe.o"
    input_object = build_dir / "input.o"
    probe_binary = build_dir / "input_escape_probe"
    compile_prefix = [
            "g++",
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Wpedantic",
            "-Iinclude",
            "-I.pio/libdeps/native/uni-algo/include",
    ]
    subprocess.run(
        compile_prefix
        + [
            "-c",
            "scripts/input_escape_probe.cpp",
            "-o",
            str(probe_object),
        ],
        cwd=root,
        check=True,
    )
    subprocess.run(
        compile_prefix
        + ["-c", "src/tui/input/input.cpp", "-o", str(input_object)],
        cwd=root,
        check=True,
    )

    link_command = ["g++", "-pthread", str(probe_object), str(input_object)]
    link_command.extend(str(native / path) for path in OBJECTS)
    link_command.extend(
        [str(native / "libf7a" / "libuni-algo.a"), "-o", str(probe_binary)]
    )
    subprocess.run(link_command, cwd=root, check=True)
    subprocess.run([str(probe_binary)], cwd=root, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
