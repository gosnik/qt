#!/usr/bin/env python3
"""Build and run the focused InteractiveTerminalSession input rewrite probe."""

import pathlib
import subprocess


OBJECTS = (
    "src/app/interactive/interactive_shell.o",
    "src/app/interactive/interactive_terminal_session.o",
    "src/foundation/error.o",
    "src/foundation/utf8.o",
    "src/foundation/process.o",
    "src/foundation/filesystem.o",
    "src/app/slash/slash_commands.o",
    "src/app/slash/llama_slash_commands.o",
    "src/harness/prompt_templates.o",
    "src/tui/core/component.o",
    "src/tui/layout/box.o",
    "src/tui/layout/spacer.o",
    "src/tui/layout/dynamic_border.o",
    "src/tui/input/editor.o",
    "src/tui/input/input.o",
    "src/tui/input/input_decoder.o",
    "src/tui/input/keybindings.o",
    "src/tui/input/keybinding_hints.o",
    "src/tui/input/keys.o",
    "src/tui/input/kill_ring.o",
    "src/tui/input/stdin_buffer.o",
    "src/tui/terminal/ansi.o",
    "src/tui/terminal/diff_renderer.o",
    "src/tui/terminal/notcurses_backend.o",
    "src/tui/terminal/terminal.o",
    "src/tui/widgets/autocomplete.o",
    "src/tui/utils/fuzzy.o",
    "src/tui/render/text.o",
    "src/tui/widgets/extension/extension_editor.o",
    "src/tui/widgets/extension/extension_input.o",
    "src/tui/widgets/extension/extension_selector.o",
    "src/tui/widgets/messages/tool_execution.o",
    "src/tui/widgets/loader.o",
    "src/tui/widgets/countdown_timer.o",
    "src/tui/widgets/overlay.o",
    "src/tui/widgets/selectors/select.o",
)


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[1]
    native = root / ".pio" / "build" / "native"
    missing = [path for path in OBJECTS if not (native / path).is_file()]
    if missing:
        raise RuntimeError("native objects are missing; run `pio run -e native` first: "
                           + ", ".join(missing))

    build_dir = root / ".pio" / "test" / "interactive-terminal-input"
    build_dir.mkdir(parents=True, exist_ok=True)
    probe_object = build_dir / "interactive_terminal_input_probe.o"
    probe_binary = build_dir / "interactive_terminal_input_probe"
    compile_command = [
        "g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Wpedantic",
        "-Iinclude", "-I.pio/libdeps/native/uni-algo/include", "-c",
        "scripts/interactive_terminal_input_probe.cpp", "-o", str(probe_object),
    ]
    subprocess.run(compile_command, cwd=root, check=True)
    link_command = ["g++", "-pthread", str(probe_object)]
    link_command.extend(str(native / path) for path in OBJECTS)
    link_command.extend([
        str(native / "libf7a" / "libuni-algo.a"),
        str(native / "lib55d" / "libnotcurses.a"),
        str(native / "generated/sysroot/usr/lib/x86_64-linux-gnu/libunistring.a"),
        "-ltinfo", "-ldl", "-lrt", "-lz", "-o", str(probe_binary),
    ])
    subprocess.run(link_command, cwd=root, check=True)
    subprocess.run([str(probe_binary)], cwd=root, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
