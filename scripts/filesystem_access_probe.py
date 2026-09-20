#!/usr/bin/env python3

import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys


def inspect(binary: pathlib.Path, root: pathlib.Path,
            environment: dict[str, str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(binary), "--inspect-settings", "--json"],
        cwd=root,
        env=environment,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def inspected_value(result: subprocess.CompletedProcess[str]) -> bool:
    if result.returncode != 0:
        raise RuntimeError("settings inspection failed: " + result.stderr)
    return json.loads(result.stdout)["settings"][
        "allow_external_filesystem_access"
    ]


def run_probe(root: pathlib.Path, binary: pathlib.Path,
              runtime_dir: pathlib.Path) -> None:
    sys.path.insert(0, str(root / "scripts"))
    from tui_pty_probe import TerminalProbe

    shutil.rmtree(runtime_dir, ignore_errors=True)
    build_dir = runtime_dir / "build"
    agent_home = runtime_dir / "agent-home"
    workspace = runtime_dir / "workspace"
    project_dir = workspace / ".pi"
    agent_home.mkdir(parents=True)
    project_dir.mkdir(parents=True)
    build_dir.mkdir(parents=True)

    pioenv = os.environ.get("PIOENV", "native")
    tool_probe = build_dir / "filesystem_access_probe"
    subprocess.run(
        [
            os.environ.get("CXX", "g++"),
            "-std=c++17", "-Wall", "-Wextra", "-Wpedantic", "-pthread",
            "-Iinclude", f"-I.pio/libdeps/{pioenv}/json/include",
            "scripts/filesystem_access_probe.cpp",
            "src/tools/write_text_tool.cpp", "src/tools/models.cpp",
            "src/tools/tool_validation.cpp", "src/foundation/error.cpp",
            "src/foundation/filesystem.cpp", "-o", str(tool_probe),
        ],
        cwd=root,
        check=True,
    )
    subprocess.run(
        [str(tool_probe), str(runtime_dir / "tool-runtime")],
        cwd=root,
        check=True,
    )
    settings_path = agent_home / "settings.json"
    project_settings_path = project_dir / "settings.json"
    mcp_settings_path = workspace / ".mcp.json"

    environment = os.environ.copy()
    environment["QT_CODING_AGENT_DIR"] = str(agent_home)

    if inspected_value(inspect(binary, workspace, environment)):
        raise RuntimeError("external filesystem access must default to false")

    settings_path.write_text(
        json.dumps({"allowExternalFilesystemAccess": True,
                    "probeSentinel": {"keep": 9}}) + "\n",
        encoding="utf-8",
    )
    if not inspected_value(inspect(binary, workspace, environment)):
        raise RuntimeError("global external filesystem access was not enabled")

    project_settings_path.write_text(
        json.dumps({"allowExternalFilesystemAccess": False}) + "\n",
        encoding="utf-8",
    )
    if inspected_value(inspect(binary, workspace, environment)):
        raise RuntimeError("project settings did not narrow global file access")

    settings_path.write_text(
        json.dumps({"allowExternalFilesystemAccess": False}) + "\n",
        encoding="utf-8",
    )
    project_settings_path.write_text(
        json.dumps({"allowExternalFilesystemAccess": True}) + "\n",
        encoding="utf-8",
    )
    broadened = inspect(binary, workspace, environment)
    if broadened.returncode == 0 or "may not broaden global policy" not in broadened.stdout:
        raise RuntimeError("project settings were allowed to broaden file access")

    project_settings_path.unlink()
    mcp_settings_path.write_text(
        json.dumps({"allowExternalFilesystemAccess": True}) + "\n",
        encoding="utf-8",
    )
    if inspected_value(inspect(binary, workspace, environment)):
        raise RuntimeError("project MCP settings broadened external file access")
    mcp_settings_path.unlink()

    settings_path.write_text(
        json.dumps({"allowExternalFilesystemAccess": False,
                    "probeSentinel": {"keep": 9}}) + "\n",
        encoding="utf-8",
    )
    prior_home = os.environ.get("QT_CODING_AGENT_DIR")
    prior_cwd = pathlib.Path.cwd()
    os.environ["QT_CODING_AGENT_DIR"] = str(agent_home)
    os.chdir(workspace)
    try:
        with TerminalProbe([str(binary)], 20, 110, 10.0) as term:
            term.read_until(lambda raw, text: False, timeout=2.0)
            if term.proc is not None and term.proc.poll() is not None:
                raise RuntimeError("qt exited before settings input")
            offset = len(term.output)
            term.write(b"/settings\r")
            if not term.read_until(
                lambda raw, text: "External file access" in text,
                timeout=6.0,
                offset=offset,
            ):
                raise RuntimeError("settings menu omitted external file access")
            term.write(b"\x1b[B\x1b[B\x1b[B\x1b[B\r")
            term.read_until(lambda raw, text: False, timeout=0.5)
            term.write(b"\x1b")
            term.read_until(lambda raw, text: False, timeout=0.25)
            term.write(b"/quit\r")
            if not term.read_until(
                lambda raw, text: term.proc is not None
                and term.proc.poll() is not None,
                timeout=4.0,
            ):
                raise RuntimeError("qt did not exit after settings update")
    finally:
        os.chdir(prior_cwd)
        if prior_home is None:
            os.environ.pop("QT_CODING_AGENT_DIR", None)
        else:
            os.environ["QT_CODING_AGENT_DIR"] = prior_home

    persisted = json.loads(settings_path.read_text(encoding="utf-8"))
    if persisted.get("allowExternalFilesystemAccess") is not True:
        raise RuntimeError("settings menu did not persist external file access")
    if persisted.get("probeSentinel") != {"keep": 9}:
        raise RuntimeError("settings update discarded an unrelated key")
    if not inspected_value(inspect(binary, workspace, environment)):
        raise RuntimeError("persisted external file access did not reload")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify external filesystem access settings and persistence"
    )
    parser.add_argument("--binary", default=".pio/build/native/qt")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    binary = pathlib.Path(args.binary)
    if not binary.is_absolute():
        binary = (root / binary).resolve()
    if not binary.is_file():
        raise RuntimeError("native qt binary is missing; run `pio run -e native`")
    run_probe(root, binary,
              root / ".pio" / "test" / "filesystem-access-probe")
    print("filesystem access settings probe passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
