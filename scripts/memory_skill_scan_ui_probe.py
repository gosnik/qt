#!/usr/bin/env python3

import argparse
import json
import os
import pathlib
import shutil
import sys
import tempfile


def wait_for_terminal(term) -> None:
    term.read_until(lambda raw, text: False, timeout=2.0)
    if term.proc is not None and term.proc.poll() is not None:
        raise RuntimeError("qt exited before accepting skill scan input")


def wait_for_text(term, offset: int, *needles: str, timeout: float = 8.0) -> None:
    if not term.read_until(
        lambda raw, text: all(needle in text for needle in needles),
        timeout=timeout,
        offset=offset,
    ):
        rendered = bytes(term.output[offset:]).decode("utf-8", errors="replace")
        raise RuntimeError(
            "terminal did not render: " + ", ".join(needles) +
            "\nrecent terminal output:\n" + rendered[-4000:]
        )


def run_probe(root: pathlib.Path, binary: pathlib.Path,
              runtime_dir: pathlib.Path) -> None:
    sys.path.insert(0, str(root / "scripts"))
    from tui_pty_probe import TerminalProbe

    agent_home = runtime_dir / "agent-home"
    workspace = runtime_dir / "workspace"
    outside = runtime_dir / "outside"
    shutil.rmtree(runtime_dir, ignore_errors=True)
    (workspace / "alpha").mkdir(parents=True)
    outside.mkdir(parents=True)
    agent_home.mkdir(parents=True)
    (workspace / "alpha" / "SKILL.md").write_text(
        "---\nname: alpha\ndescription: UI probe\n---\n\n# Alpha\n",
        encoding="utf-8",
    )
    (agent_home / "settings.json").write_text(
        json.dumps({
            "memory": {
                "enabled": True,
                "automaticRecall": False,
                "automaticCapture": False,
            },
            "allowExternalFilesystemAccess": False,
        }) + "\n",
        encoding="utf-8",
    )

    previous_home = os.environ.get("QT_CODING_AGENT_DIR")
    previous_cwd = pathlib.Path.cwd()
    os.environ["QT_CODING_AGENT_DIR"] = str(agent_home)
    os.chdir(workspace)
    try:
        with TerminalProbe([str(binary)], 20, 120, 10.0) as term:
            wait_for_terminal(term)

            offset = len(term.output)
            term.write(b"/memory\r")
            wait_for_text(term, offset, "scan skills folder", "Import SKILL.md")

            offset = len(term.output)
            term.write(b"\x1b[B\x1b[B\r")
            wait_for_text(term, offset, "Scan this folder",
                          "Scan this folder + subfolders")

            offset = len(term.output)
            term.write(b"\x1b[B\r")
            wait_for_text(term, offset, "Memory skill scan started:",
                          "recursive: yes")
            wait_for_text(term, offset, "Memory skill scan: created alpha/SKILL.md",
                          "Memory skill scan finished: discovered=1",
                          timeout=12.0)

            offset = len(term.output)
            term.write(
                ("\x15/memory scan-skills " + str(outside) + "\r").encode("utf-8")
            )
            wait_for_text(term, offset, "outside the workspace")

            term.write(b"/quit\r")
            if not term.read_until(
                lambda raw, text: term.proc is not None
                and term.proc.poll() is not None,
                timeout=5.0,
            ):
                raise RuntimeError("qt did not exit after the skill scan UI probe")
    finally:
        os.chdir(previous_cwd)
        if previous_home is None:
            os.environ.pop("QT_CODING_AGENT_DIR", None)
        else:
            os.environ["QT_CODING_AGENT_DIR"] = previous_home


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify the interactive memory skill-folder scan workflow"
    )
    parser.add_argument("--binary", default=".pio/build/native/qt")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    binary = pathlib.Path(args.binary)
    if not binary.is_absolute():
        binary = (root / binary).resolve()
    if not binary.is_file():
        raise RuntimeError("native qt binary is missing; run `pio run -e native`")
    with tempfile.TemporaryDirectory(prefix="qt-memory-skill-ui-probe-") as temporary:
        run_probe(root, binary, pathlib.Path(temporary))
    print("memory skill scan UI probe passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
