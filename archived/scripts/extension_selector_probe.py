#!/usr/bin/env python3
"""Offline PTY regression for the native /extensions inventory."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
import shutil
import tempfile

from tui_pty_probe import TerminalProbe, strip_ansi


@contextmanager
def isolated_environment(workspace: Path, agent_home: Path):
    values = {
        "HOME": str(agent_home.parent / "home"),
        "QT_CODING_AGENT_DIR": str(agent_home),
        "PI_CODING_AGENT_DIR": str(agent_home),
        "QT_EXPERIMENTAL_NOTCURSES": "0",
        "TERM": "xterm-256color",
    }
    previous = {key: os.environ.get(key) for key in values}
    cwd = Path.cwd()
    Path(values["HOME"]).mkdir()
    try:
        os.environ.update(values)
        os.chdir(workspace)
        yield
    finally:
        os.chdir(cwd)
        for key, value in previous.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


def require(term: TerminalProbe, predicate, message: str, offset: int = 0):
    if not term.read_until(predicate, timeout=10.0, offset=offset):
        raise RuntimeError(message + "\n" + strip_ansi(bytes(term.output[-16000:])))


def drain(term: TerminalProbe):
    term.read_until(lambda _raw, _text: False, timeout=0.3)


def open_selector(term: TerminalProbe, argument: str, rows: dict[str, str]) -> str:
    drain(term)
    offset = len(term.output)
    term.write(("/extensions" + (" " + argument if argument else "") + "\r").encode())
    require(term, lambda _raw, text: "Esc to cancel" in text and all(
        re.search(re.escape(label) + r"\s+" + value + r"\b", text)
        for label, value in rows.items()), "extension selector rows missing", offset)
    drain(term)
    text = strip_ansi(bytes(term.output[offset:]))
    if "No package-managed resources matched." in text or "No extensions matched." in text:
        raise RuntimeError("extension selector unexpectedly empty\n" + text)
    return text


def close_selector(term: TerminalProbe):
    term.write(b"\x1b")
    drain(term)


def no_matches(term: TerminalProbe, argument: str):
    offset = len(term.output)
    term.write(("/extensions " + argument + "\r").encode())
    require(term, lambda _raw, text: "No extensions matched." in text,
            "missing no-match feedback", offset)
    drain(term)


def quit_terminal(term: TerminalProbe):
    term.write(b"/quit\r")
    require(term, lambda _raw, _text: term.proc is not None and term.proc.poll() is not None,
            "interactive process did not exit")
    if term.proc.returncode != 0:
        raise RuntimeError(f"interactive process exited with {term.proc.returncode}")


def fixture(root: Path, extension: Path):
    workspace = root / "ws"
    agent_home = root / "agent"
    workspace.mkdir(parents=True)
    agent_home.mkdir()
    # No model turn is sent. The footer only needs a resolvable local catalog entry.
    (agent_home / "models.json").write_text(json.dumps({"providers": {"probe": {
        "api": "openai-completions", "baseUrl": "http://127.0.0.1:1/v1",
        "apiKey": "probe-key", "models": [{"id": "selector-model"}]
    }}}), encoding="utf-8")
    target = workspace / "examples/extensions/astra-auto-compact.as"
    target.parent.mkdir(parents=True)
    shutil.copyfile(extension, target)
    for path in [workspace / ".pi/extensions/project-probe.as",
                 agent_home / "extensions/user-probe.as"]:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("void activate() {}\n", encoding="utf-8")
    return workspace, agent_home


def readonly(term: TerminalProbe, settings: Path):
    before = settings.read_bytes() if settings.exists() else None
    offset = len(term.output)
    for key in [b"\r", b" "]:
        term.write(key)
        drain(term)
    # Force a fresh frame: read-only confirmation must leave the overlay open.
    term.write(b"\x1b[B")
    require(term, lambda _raw, text: "Read-only" in text and "loaded" in text,
            "read-only confirmation closed or changed the selector", offset)
    text = strip_ansi(bytes(term.output[offset:]))
    if "Enabled extensions" in text or "Disabled extensions" in text or "error:" in text:
        raise RuntimeError("read-only row invoked package persistence\n" + text)
    after = settings.read_bytes() if settings.exists() else None
    if before != after:
        raise RuntimeError("read-only row changed package settings")


def run_case(binary: Path, extension: Path, root: Path, no_extensions: bool):
    workspace, agent_home = fixture(root, extension)
    settings = agent_home / "settings.json"
    argv = [str(binary), "--provider", "probe", "--model", "selector-model",
            "--no-session", "-e", "examples/extensions/astra-auto-compact.as"]
    if no_extensions:
        argv.append("--no-extensions")
    with isolated_environment(workspace, agent_home), TerminalProbe(argv, 32, 160, 10) as term:
        require(term, lambda _raw, text: "selector-model" in text, "startup failed")
        rows = {"astra-auto-compact": "loaded"}
        if not no_extensions:
            rows.update({"project-probe": "loaded", "user-probe": "loaded"})
        text = open_selector(term, "", rows)
        if no_extensions and re.search(r"(?:project|user)-probe\s+loaded", text):
            raise RuntimeError("--no-extensions did not suppress discovery")
        if "Read-only" not in text or "Enter/Space to change" in text:
            raise RuntimeError("read-only selector has an editable hint")
        readonly(term, settings)
        close_selector(term)
        for argument in ["ASTRA", "EXAMPLES/EXTENSIONS/ASTRA-AUTO-COMPACT.AS"]:
            text = open_selector(term, argument, {"astra-auto-compact": "loaded"})
            if "astra-auto-compact.as" not in text:
                raise RuntimeError("extension source path is missing\n" + text)
            close_selector(term)
        no_matches(term, "not-an-extension-xyz")
        if no_extensions:
            no_matches(term, "project-probe")
        quit_terminal(term)


def run_package_case(binary: Path, extension: Path, root: Path):
    workspace, agent_home = fixture(root, extension)
    package = workspace / "package"
    package.mkdir()
    (package / "available.as").write_text("void activate() {}\n")
    settings = agent_home / "settings.json"
    settings.write_text(json.dumps({"packages": [{
        "source": str(package), "extensions": ["-available.as"]}]}))
    alias = workspace / "alias.as"
    alias.symlink_to(workspace / "examples/extensions/astra-auto-compact.as")
    # A package referencing the same file as -e must not duplicate it or offer
    # a package toggle that cannot disable the explicit loading path.
    (package / "package.json").write_text(json.dumps({"pi": {
        "extensions": ["available.as", "../examples/extensions/astra-auto-compact.as"]}}))
    argv = [str(binary), "--provider", "probe", "--model", "selector-model",
            "--session-dir", str(root / "sessions"), "--no-extensions", "-e", str(alias)]
    with isolated_environment(workspace, agent_home), TerminalProbe(argv, 32, 160, 10) as term:
        require(term, lambda _raw, text: "selector-model" in text, "startup failed")
        text = open_selector(term, "", {"astra-auto-compact": "loaded", "available": "off"})
        if re.search(r"\balias\s+loaded\b", text):
            raise RuntimeError("CLI alias duplicates the package entry")
        close_selector(term)
        text = open_selector(term, "astra", {"astra-auto-compact": "loaded"})
        if "read-only" not in text:
            raise RuntimeError("CLI/package overlap lost its read-only indication")
        readonly(term, settings)
        close_selector(term)
        text = open_selector(term, "available", {"available": "off"})
        if "Enter/Space to change" not in text:
            raise RuntimeError("package toggle hint is missing")
        offset = len(term.output)
        term.write(b"\r")
        require(term, lambda _raw, text: "Enabled extensions available." in text,
                "package toggle did not complete", offset)
        entries = json.loads(settings.read_text())["packages"][0]["extensions"]
        if "+available.as" not in entries or "-available.as" in entries:
            raise RuntimeError("package toggle did not persist the selected resource: " + settings.read_text())
        drain(term)
        quit_terminal(term)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path(".pio/build/native/qt"))
    args = parser.parse_args()
    binary = args.binary.resolve()
    extension = Path(__file__).resolve().parents[1] / "examples/extensions/astra-auto-compact.as"
    if not binary.is_file() or not extension.is_file():
        parser.error("build the native binary and provide the Astra extension example first")
    with tempfile.TemporaryDirectory(prefix="qtes-") as directory:
        root = Path(directory)
        for no_extensions in [False, True]:
            run_case(binary, extension, root / str(no_extensions), no_extensions)
        run_package_case(binary, extension, root / "packages")
    print("Extension selector probe passed (CLI, discovery, filters, read-only, packages)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
