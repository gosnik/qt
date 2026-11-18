#!/usr/bin/env python3
"""Build and run focused terminal-render regressions after `pio run -e native`."""
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / ".pio" / "build" / "native"

def main() -> int:
    required = ["src/foundation/error.o", "src/foundation/utf8.o", "src/tui/core/component.o",
                "src/tui/layout/box.o", "src/tui/layout/spacer.o", "src/tui/render/text.o",
                "src/tui/render/markdown.o", "src/tui/terminal/ansi.o",
                "src/tui/terminal/terminal.o", "src/tui/terminal/diff_renderer.o",
                "src/tui/terminal/notcurses_backend.o", "src/tui/input/keys.o", "src/tui/widgets/overlay.o",
                "src/tui/widgets/messages/assistant_message.o"]
    if any(not (BUILD / item).is_file() for item in required):
        raise RuntimeError("native render objects are missing; run `pio run -e native` first")
    out = ROOT / ".pio" / "test" / "tui-render-regression"
    out.mkdir(parents=True, exist_ok=True)
    obj, binary = out / "probe.o", out / "probe"
    subprocess.run(["g++", "-std=c++17", "-Iinclude", "-I.pio/libdeps/native/json/include", "-c", "scripts/tui_render_regression_probe.cpp", "-o", str(obj)], cwd=ROOT, check=True)
    command = ["g++", str(obj), *[str(BUILD / item) for item in required],
               str(BUILD / "libf7a/libuni-algo.a"), str(BUILD / "lib9ac/libmd4c.a"),
               str(BUILD / "lib55d/libnotcurses.a"),
               str(BUILD / "generated/sysroot/usr/lib/x86_64-linux-gnu/libunistring.a"),
               "-pthread", "-ltinfo", "-ldl", "-lrt", "-lz", "-o", str(binary)]
    subprocess.run(command, cwd=ROOT, check=True)
    subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=5)
    return 0
if __name__ == "__main__": raise SystemExit(main())
