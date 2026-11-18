#!/usr/bin/env python3
import argparse
import os
import pathlib
import shutil
import subprocess
import sys


def wait_for_terminal(term) -> None:
    term.read_until(lambda raw, text: False, timeout=2.0)
    if term.proc is not None and term.proc.poll() is not None:
        raise RuntimeError("qt exited before accepting subagent menu input")


def wait_for_text(term, offset: int, *needles: str) -> None:
    if not term.read_until(
        lambda raw, text: all(needle in text for needle in needles),
        timeout=8.0,
        offset=offset,
    ):
        rendered = bytes(term.output[offset:]).decode("utf-8", errors="replace")
        raise RuntimeError(
            "subagent menu did not render: " + ", ".join(needles) +
            "\nrecent terminal output:\n" + rendered[-4000:]
        )


def run_menu_probe(root: pathlib.Path, qt_binary: pathlib.Path,
                   runtime_dir: pathlib.Path) -> None:
    sys.path.insert(0, str(root / "scripts"))
    from tui_pty_probe import TerminalProbe

    shutil.rmtree(runtime_dir, ignore_errors=True)
    agent_home = runtime_dir / "agent-home"
    workspace = runtime_dir / "workspace"
    (agent_home / "agents").mkdir(parents=True)
    (workspace / ".agents").mkdir(parents=True)
    (agent_home / "agents" / "aaa-custom.md").write_text(
        "---\nname: aaa-custom\ndescription: Custom UI role\n"
        "model: custom-model\ntools: read_text\n---\n\nInspect carefully.\n",
        encoding="utf-8",
    )
    (workspace / ".agents" / "project-role.md").write_text(
        "---\nname: project-role\ndescription: Repository-controlled role\n"
        "tools: read_text\n---\n\nInspect repository conventions.\n",
        encoding="utf-8",
    )
    fake_child = runtime_dir / "fake-subagent.py"
    fake_trace = runtime_dir / "fake-subagent.trace"
    fake_child.write_text(
        "#!/usr/bin/env python3\n"
        "import json, os, sys\n"
        "task = next((arg[6:] for arg in sys.argv[1:] if arg.startswith('Task: ')), '')\n"
        "with open(os.environ['QT_SUBAGENT_MENU_TRACE'], 'a', encoding='utf-8') as out:\n"
        "    out.write(task + '\\n')\n"
        "message = {'role': 'assistant', 'content': "
        "[{'type': 'text', 'text': 'reply(' + task + ')'}]}\n"
        "print(json.dumps({'type': 'agent_end', 'messages': [message]}))\n",
        encoding="utf-8",
    )
    fake_child.chmod(0o700)

    previous_home = os.environ.get("QT_CODING_AGENT_DIR")
    previous_executable = os.environ.get("QT_SUBAGENT_EXECUTABLE")
    previous_trace = os.environ.get("QT_SUBAGENT_MENU_TRACE")
    previous_cwd = pathlib.Path.cwd()
    os.environ["QT_CODING_AGENT_DIR"] = str(agent_home)
    os.environ["QT_SUBAGENT_EXECUTABLE"] = str(fake_child)
    os.environ["QT_SUBAGENT_MENU_TRACE"] = str(fake_trace)
    os.chdir(workspace)
    try:
        with TerminalProbe([str(qt_binary)], 20, 120, 10.0) as term:
            wait_for_terminal(term)
            offset = len(term.output)
            term.write(b"/subagent\r")
            wait_for_text(term, offset, "Subagents", "aaa-custom",
                          "Parallel delegation", "Inspect project roles")

            offset = len(term.output)
            term.write(b"\r")
            wait_for_text(term, offset, "Subagent · aaa-custom",
                          "Use this agent", "View details")

            offset = len(term.output)
            term.write(b"\x1b[B\r")
            wait_for_text(term, offset, "Subagent: aaa-custom",
                          "source: user", "model: custom-model")

            offset = len(term.output)
            term.write(b"/subagent\r")
            wait_for_text(term, offset, "Subagents", "aaa-custom")
            offset = len(term.output)
            term.write(b"\r")
            wait_for_text(term, offset, "Subagent · aaa-custom",
                          "Use this agent")
            offset = len(term.output)
            term.write(b"\r")
            wait_for_text(term, offset, "Delegate · aaa-custom", "Task:",
                          "tracked tool call")
            offset = len(term.output)
            term.write(b"menu tracked\r")
            wait_for_text(term, offset, "reply(menu tracked)")
            if not fake_trace.is_file() or "menu tracked" not in fake_trace.read_text(
                    encoding="utf-8"):
                raise RuntimeError("subagent menu did not launch the selected child")

            offset = len(term.output)
            term.write(b"/subagent list project\r")
            wait_for_text(term, offset, "Subagents (scope: project)",
                          "project-role [project]",
                          "require explicit trust before execution")

            term.write(b"/quit\r")
            if not term.read_until(
                lambda raw, text: term.proc is not None
                and term.proc.poll() is not None,
                timeout=5.0,
            ):
                raise RuntimeError("qt did not exit after subagent menu probe")

        session_text = "\n".join(
            path.read_text(encoding="utf-8")
            for path in (agent_home / "sessions").glob("*.jsonl")
        )
        if ("user_initiated_tool_call" not in session_text or
                '"type":"tool_result"' not in session_text or
                "reply(menu tracked)" not in session_text):
            raise RuntimeError(
                "tracked subagent call/result was not persisted to the session")

        with TerminalProbe([str(qt_binary), "--no-tools"], 20, 120, 10.0) as disabled:
            wait_for_terminal(disabled)
            offset = len(disabled.output)
            disabled.write(b"/subagent\r")
            wait_for_text(disabled, offset,
                          "subagent tool is disabled for this runtime")
            disabled.write(b"/quit\r")
            if not disabled.read_until(
                lambda raw, text: disabled.proc is not None
                and disabled.proc.poll() is not None,
                timeout=5.0,
            ):
                raise RuntimeError("disabled-tool subagent probe did not exit")
    finally:
        os.chdir(previous_cwd)
        if previous_home is None:
            os.environ.pop("QT_CODING_AGENT_DIR", None)
        else:
            os.environ["QT_CODING_AGENT_DIR"] = previous_home
        if previous_executable is None:
            os.environ.pop("QT_SUBAGENT_EXECUTABLE", None)
        else:
            os.environ["QT_SUBAGENT_EXECUTABLE"] = previous_executable
        if previous_trace is None:
            os.environ.pop("QT_SUBAGENT_MENU_TRACE", None)
        else:
            os.environ["QT_SUBAGENT_MENU_TRACE"] = previous_trace


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify the isolated subagent core and optional /subagent menu"
    )
    parser.add_argument("--binary", help="Native qt binary for PTY menu coverage")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    build_dir = root / ".pio" / "test" / "subagent-probe"
    build_dir.mkdir(parents=True, exist_ok=True)
    binary = build_dir / "subagent_probe"
    pioenv = os.environ.get("PIOENV", "native")
    cxx = os.environ.get("CXX", "g++")
    command = [
        cxx,
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-pthread",
        "-Iinclude",
        f"-I.pio/libdeps/{pioenv}/json/include",
        "scripts/subagent_probe.cpp",
        "src/tools/subagent_tool.cpp",
        "src/app/runtime/subagent_coordinator.cpp",
        "src/app/runtime/isolated_agent_runner.cpp",
        "src/ai/models.cpp",
        "src/foundation/uuid.cpp",
        "src/tools/models.cpp",
        "src/tools/tool_validation.cpp",
        "src/foundation/error.cpp",
        "src/foundation/filesystem.cpp",
        "src/foundation/frontmatter.cpp",
        "src/foundation/process.cpp",
        "-o",
        str(binary),
    ]
    subprocess.run(command, cwd=root, check=True)
    subprocess.run([str(binary), str(build_dir / "runtime")], cwd=root, check=True)
    if args.binary:
        qt_binary = pathlib.Path(args.binary)
        if not qt_binary.is_absolute():
            qt_binary = (root / qt_binary).resolve()
        if not qt_binary.is_file():
            raise RuntimeError("native qt binary is missing; run `pio run -e native`")
        run_menu_probe(root, qt_binary, build_dir / "menu-runtime")
    print("subagent probe passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
