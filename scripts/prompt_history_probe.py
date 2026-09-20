#!/usr/bin/env python3
import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys


def wait_for_terminal(term) -> None:
    term.read_until(lambda raw, text: False, timeout=2.0)
    if term.proc is not None and term.proc.poll() is not None:
        raise RuntimeError("qt exited before accepting prompt input")


def run_tui_probe(root: pathlib.Path, binary: pathlib.Path,
                  runtime_dir: pathlib.Path) -> None:
    sys.path.insert(0, str(root / "scripts"))
    from tui_pty_probe import TerminalProbe

    agent_home = runtime_dir / "agent-home"
    default_home = runtime_dir / "default-agent-home"
    workspace = runtime_dir / "other-workspace"
    shutil.rmtree(agent_home, ignore_errors=True)
    shutil.rmtree(default_home, ignore_errors=True)
    agent_home.mkdir(parents=True)
    default_home.mkdir(parents=True)
    workspace.mkdir(parents=True, exist_ok=True)
    (agent_home / "settings.json").write_text(
        json.dumps({"terminal": {"promptHistoryLimit": 2}}) + "\n",
        encoding="utf-8",
    )

    prior_home = os.environ.get("QT_CODING_AGENT_DIR")
    prior_cwd = pathlib.Path.cwd()
    os.environ["QT_CODING_AGENT_DIR"] = str(agent_home)
    prompts = [
        "/prompt-history-probe-one",
        "/prompt-history-probe-two",
        "/prompt-history-probe-three",
    ]
    try:
        default_env = os.environ.copy()
        default_env["QT_CODING_AGENT_DIR"] = str(default_home)
        default_inspected = subprocess.run(
            [str(binary), "--inspect-settings", "--json"],
            cwd=root,
            env=default_env,
            check=True,
            text=True,
            stdout=subprocess.PIPE,
        )
        default_settings = json.loads(default_inspected.stdout)["settings"]
        if default_settings["terminal"]["prompt_history_limit"] != 500:
            raise RuntimeError("default prompt-history limit is not 500")

        inspected = subprocess.run(
            [str(binary), "--inspect-settings", "--json"],
            cwd=root,
            check=True,
            text=True,
            stdout=subprocess.PIPE,
        )
        settings = json.loads(inspected.stdout)["settings"]
        if settings["terminal"]["prompt_history_limit"] != 2:
            raise RuntimeError("configured prompt-history limit was not inspected")

        ctrl_c = b"\x03"
        with TerminalProbe([str(binary)], 14, 100, 8.0) as clear_term:
            wait_for_terminal(clear_term)
            clear_term.write(
                b"discard-one" + ctrl_c + b"discard-two" + ctrl_c
            )
            clear_term.read_until(lambda raw, text: False, timeout=0.75)
            if clear_term.proc is None or clear_term.proc.poll() is not None:
                raise RuntimeError(
                    "Ctrl+C exited while non-empty prompt input was present"
                )

            offset = len(clear_term.output)
            clear_term.write(b"/help\r")
            if not clear_term.read_until(
                lambda raw, text: "Slash commands:" in text,
                timeout=5.0,
                offset=offset,
            ):
                raise RuntimeError("Ctrl+C did not clear non-empty prompt input")

            clear_term.write(b"/quit\r")
            if not clear_term.read_until(
                lambda raw, text: clear_term.proc is not None
                and clear_term.proc.poll() is not None,
                timeout=3.0,
            ):
                raise RuntimeError("qt did not exit after Ctrl+C prompt probe")
            if clear_term.proc is None or clear_term.proc.returncode != 0:
                raise RuntimeError("qt returned a failure after Ctrl+C prompt probe")

        with TerminalProbe([str(binary)], 14, 100, 8.0) as exit_term:
            wait_for_terminal(exit_term)
            exit_term.write(ctrl_c + ctrl_c)
            if not exit_term.read_until(
                lambda raw, text: exit_term.proc is not None
                and exit_term.proc.poll() is not None,
                timeout=3.0,
            ):
                raise RuntimeError("empty-editor double Ctrl+C did not exit")
            if exit_term.proc is None or exit_term.proc.returncode != 0:
                raise RuntimeError("double Ctrl+C returned a failure status")

        for invalid in (-1, "invalid"):
            (agent_home / "settings.json").write_text(
                json.dumps({"terminal": {"promptHistoryLimit": invalid}}) + "\n",
                encoding="utf-8",
            )
            rejected = subprocess.run(
                [str(binary), "--inspect-settings", "--json"],
                cwd=root,
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            if (
                rejected.returncode == 0
                or "terminal.promptHistoryLimit" not in rejected.stdout
            ):
                raise RuntimeError(f"invalid prompt-history limit was accepted: {invalid!r}")
        (agent_home / "settings.json").write_text(
            json.dumps({"terminal": {"promptHistoryLimit": 2}}) + "\n",
            encoding="utf-8",
        )

        with TerminalProbe([str(binary)], 14, 100, 8.0) as writer:
            wait_for_terminal(writer)
            for prompt in prompts:
                offset = len(writer.output)
                writer.write(prompt.encode() + b"\r")
                if not writer.read_until(
                    lambda raw, text: "unknown slash command" in text,
                    timeout=5.0,
                    offset=offset,
                ):
                    raise RuntimeError(f"qt did not accept {prompt}")

        os.chdir(workspace)
        with TerminalProbe([str(binary)], 14, 100, 8.0) as reader:
            wait_for_terminal(reader)
            offset = len(reader.output)
            reader.write(b"\x1b[A")
            if not reader.read_until(
                lambda raw, text: prompts[2] in text,
                timeout=5.0,
                offset=offset,
            ):
                raise RuntimeError("fresh process did not recall newest prompt")
            offset = len(reader.output)
            reader.write(b"\x1b[A")
            if not reader.read_until(
                lambda raw, text: prompts[1] in text,
                timeout=5.0,
                offset=offset,
            ):
                raise RuntimeError("fresh process did not recall second-newest prompt")
        os.chdir(prior_cwd)

        history_path = agent_home / "history.jsonl"
        records = [
            json.loads(line)
            for line in history_path.read_text(encoding="utf-8").splitlines()
        ]
        if [record["prompt"] for record in records] != prompts[1:]:
            raise RuntimeError("configured limit did not retain the newest two prompts")

        (agent_home / "settings.json").write_text(
            json.dumps({"terminal": {"promptHistoryLimit": 0}}) + "\n",
            encoding="utf-8",
        )
        with TerminalProbe([str(binary)], 14, 100, 8.0) as disabled:
            wait_for_terminal(disabled)
            disabled.write(b"/prompt-history-disabled\r")
            if not disabled.read_until(
                lambda raw, text: "unknown slash command" in text,
                timeout=5.0,
            ):
                raise RuntimeError("zero-limit qt process did not accept input")
        if history_path.exists():
            raise RuntimeError(
                "zero prompt-history limit did not remove the history file"
            )
    finally:
        os.chdir(prior_cwd)
        if prior_home is None:
            os.environ.pop("QT_CODING_AGENT_DIR", None)
        else:
            os.environ["QT_CODING_AGENT_DIR"] = prior_home


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify global prompt history")
    parser.add_argument("--binary", default="")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    build_dir = root / ".pio" / "test" / "prompt-history-probe"
    build_dir.mkdir(parents=True, exist_ok=True)
    binary = build_dir / "prompt_history_probe"
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
        "scripts/prompt_history_probe.cpp",
        "src/app/config/prompt_history.cpp",
        "src/foundation/error.cpp",
        "src/foundation/filesystem.cpp",
        "-o",
        str(binary),
    ]
    subprocess.run(command, cwd=root, check=True)
    subprocess.run([str(binary), str(build_dir / "runtime")], cwd=root, check=True)
    if args.binary:
        qt_binary = pathlib.Path(args.binary)
        if not qt_binary.is_absolute():
            qt_binary = (root / qt_binary).resolve()
        run_tui_probe(root, qt_binary, build_dir / "tui-runtime")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
