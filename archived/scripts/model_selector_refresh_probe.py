#!/usr/bin/env python3
"""Verify that refreshing an open model picker preserves its interaction state."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
OBJECTS = (
    "tui/core/component", "tui/input/input",
    "tui/input/keybindings", "tui/input/keybinding_hints", "tui/input/keys",
    "tui/input/kill_ring", "tui/input/input_decoder",
    "tui/input/stdin_buffer", "tui/terminal/terminal", "tui/layout/box", "tui/layout/dynamic_border",
    "tui/layout/spacer", "tui/render/text", "tui/widgets/selectors/select",
    "foundation/utf8", "foundation/error", "tui/terminal/ansi",
)


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="qt-model-selector-refresh-") as temp:
        binary = Path(temp) / "probe"
        subprocess.run([
            "g++", "-std=c++17", "-Wall", "-Wextra", "-Wpedantic",
            "-Iinclude", "-I.pio/libdeps/native/json/include",
            "scripts/model_selector_refresh_probe.cpp",
            "src/tui/widgets/selectors/model_selector.cpp",
            *(str(ROOT / ".pio/build/native/src" / (name + ".o"))
              for name in OBJECTS),
            *(str(path) for path in (ROOT / ".pio/build/native").glob("lib*/libuni-algo.a")),
            *(str(path) for path in (ROOT / ".pio/build/native").glob("lib*/libnotcurses.a")),
            *(str(path) for path in (ROOT / ".pio/build/native/generated/sysroot/usr/lib").glob("*/libunistring.a")),
            "-ltinfo", "-lz", "-pthread", "-o", str(binary),
        ], cwd=ROOT, check=True)
        subprocess.run([str(binary)], cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
