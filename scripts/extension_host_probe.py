#!/usr/bin/env python3
"""Probe an AngelScript extension through normal interactive startup."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import tempfile


def _script_source() -> str:
    # The counter is deliberately global: a fresh AngelScript runtime for each
    # invocation would return 1 twice, while the production runtime must retain
    # the module state across command calls.
    return r'''int invocation_count = 0;

void activate() {
  qt_register_command("as_probe");
  qt_register_flag("probe_flag");
  qt_register_shortcut("ctrl+x", "shortcut_probe", "");
  qt_register_message_renderer("probe.note", "render_note");
  qt_on("session_start", "session_start_probe");
  qt_on("session_shutdown", "session_shutdown_probe");
}

void session_start_probe(const string &in event_json) {
  qt_host_call("pi.appendEntry", "{\"customType\":\"pty_session_start\",\"data\":{\"marker\":\"PTY_SESSION_START\"}}");
  string reason = qt_json_string(qt_json_get(event_json, "reason"));
  if (reason == "reload") {
    qt_host_call("ctx.ui.setWidget", "{\"key\":\"initial\",\"content\":[\"RELOAD_WIDGET_MARKER\"]}");
  } else {
    qt_host_call("ctx.ui.setWidget", "{\"key\":\"initial\",\"content\":[\"INITIAL_WIDGET_MARKER\"]}");
  }
}

void session_shutdown_probe(const string &in event_json) {
  qt_host_call("pi.appendEntry", "{\"customType\":\"pty_session_shutdown\",\"data\":{\"marker\":\"PTY_SESSION_SHUTDOWN\"}}");
}

string render_note(const string &in event_json) {
  return "{\"lines\":[\"CUSTOM_MESSAGE_MARKER\"]}";
}

string editor_ui(const string &in event_json) {
  return "{\"lines\":[\"EDITOR_RENDER_MARKER\"]}";
}

string shortcut_probe(const string &in event_json) {
  qt_host_call("pi.appendEntry", "{\"customType\":\"shortcut_probe\",\"data\":{\"marker\":\"SHORTCUT_ENTRY\"}}");
  qt_host_call("ctx.ui.notify", "{\"message\":\"SHORTCUT_NOTIFY_MARKER\"}");
  return "null";
}

string custom_ui(const string &in event_json) {
  string event_type = qt_json_string(qt_json_get(event_json, "type"));
  if (event_type == "render") {
    return "{\"lines\":[\"CUSTOM_RENDER_MARKER\"]}";
  }
  return "{\"handled\":true,\"done\":\"CUSTOM_DONE\"}";
}

string command_as_probe(const string &in arguments) {
  invocation_count += 1;
  qt_host_call("pi.setSessionName", "{\"name\":\"AS_SESSION\"}");
  qt_host_call("pi.appendEntry", "{\"customType\":\"as_probe\",\"data\":{\"marker\":\"AS_ENTRY\"}}");
  string flag = qt_host_call("pi.getFlag", "{\"name\":\"probe_flag\"}");
  string name = qt_host_call("pi.getSessionName", "{}");
  if (invocation_count == 1) {
    qt_host_call("ctx.ui.setWidget", "{\"key\":\"probe\",\"content\":[\"WIDGET_MARKER\"]}");
    qt_host_call("ctx.ui.setHeader", "{\"lines\":[\"HEADER_MARKER\"]}");
    qt_host_call("ctx.ui.setFooter", "{\"lines\":[\"FOOTER_MARKER\"]}");
    string input = qt_host_call("ctx.ui.input", "{\"title\":\"Probe input\"}");
    string custom = qt_host_call("ctx.ui.custom", "{\"handler\":\"custom_ui\"}");
    qt_host_call("pi.sendMessage", "{\"customType\":\"probe.note\",\"content\":\"NOTE_PAYLOAD\",\"display\":true,\"triggerTurn\":false}");
    qt_host_call("ctx.ui.setEditorComponent", "{\"handler\":\"editor_ui\"}");
    return "{\"count\":1,\"flag\":" + flag + ",\"name\":" + name +
           ",\"input\":" + input + ",\"custom\":" + custom + "}";
  }
  return "{\"count\":2,\"flag\":" + flag + ",\"name\":" + name + "}";
}
'''


def run_probe(binary: Path, runtime: Path) -> None:
    from tui_pty_probe import TerminalProbe, strip_ansi

    shutil.rmtree(runtime, ignore_errors=True)
    agent_home = runtime / "agent-home"
    workspace = runtime / "workspace"
    session_dir = runtime / "sessions"
    agent_home.mkdir(parents=True)
    workspace.mkdir(parents=True)
    session_dir.mkdir(parents=True)
    extension = workspace / "as-host-probe.as"
    extension.write_text(_script_source(), encoding="utf-8")

    # A configured local model lets startup resolve the interactive footer
    # without contacting a real provider. No turn is submitted by this probe.
    (agent_home / "models.json").write_text(
        json.dumps(
            {
                "providers": {
                    "probe": {
                        "api": "openai-completions",
                        "baseUrl": "http://127.0.0.1:1/v1",
                        "apiKey": "probe-key",
                        "models": [{"id": "probe-model"}],
                    }
                }
            }
        )
        + "\n",
        encoding="utf-8",
    )

    previous_home = os.environ.get("QT_CODING_AGENT_DIR")
    previous_cwd = Path.cwd()
    os.environ["QT_CODING_AGENT_DIR"] = str(agent_home)
    os.chdir(workspace)
    try:
        with TerminalProbe(
            [
                str(binary),
                "--provider",
                "probe",
                "--model",
                "probe-model",
                "--no-extensions",
                "--extension",
                str(extension),
                "--probe_flag",
                "--session-dir",
                str(session_dir),
            ],
            24,
            120,
            12.0,
        ) as term:
            if not term.read_until(
                lambda _raw, text: "probe-model" in text, timeout=8.0
            ):
                raise RuntimeError(
                    "interactive prompt did not become ready\n"
                    + strip_ansi(bytes(term.output[-8000:]))
                )
            term.read_until(lambda _raw, _text: False, timeout=1.0)

            term.write(b"/as_probe\r")
            if not term.read_until(
                lambda _raw, text: "Probe input" in text, timeout=8.0
            ):
                raise RuntimeError(
                    "ctx.ui.input dialog did not open\n"
                    + strip_ansi(bytes(term.output[-12000:]))
                )
            term.write(b"\x1b")
            if not term.read_until(
                lambda _raw, text: "CUSTOM_RENDER_MARKER" in text, timeout=8.0
            ):
                raise RuntimeError(
                    "ctx.ui.custom did not render its scripted component\n"
                    + strip_ansi(bytes(term.output[-12000:]))
                )
            term.write(b"x\r")
            if not term.read_until(
                lambda _raw, text: '"count":1' in text
                and '"custom":"CUSTOM_DONE"' in text,
                timeout=8.0,
            ):
                raise RuntimeError(
                    "ctx.ui.custom did not complete through input\n"
                    + strip_ansi(bytes(term.output[-12000:]))
                )
            output = strip_ansi(bytes(term.output))
            for marker in (
                "INITIAL_WIDGET_MARKER",
                "WIDGET_MARKER",
                "HEADER_MARKER",
                "FOOTER_MARKER",
                "EDITOR_RENDER_MARKER",
                "CUSTOM_MESSAGE_MARKER",
            ):
                if marker not in output:
                    raise RuntimeError(
                        f"{marker} was not rendered after its native UI callback\n"
                        + output[-12000:]
                    )

            term.write(b"\x18")
            if not term.read_until(
                lambda _raw, text: "SHORTCUT_NOTIFY_MARKER" in text, timeout=8.0
            ):
                raise RuntimeError(
                    "shortcut notify did not reach the interactive UI\n"
                    + strip_ansi(bytes(term.output[-12000:]))
                )
            # The AngelScript global is intentionally retained across command
            # invocations in one runtime.  Verify that before /reload, which
            # deliberately constructs a fresh runtime.
            term.write(b"/as_probe\r")
            if not term.read_until(
                lambda _raw, text: '"count":2' in text, timeout=8.0
            ):
                raise RuntimeError(
                    'AngelScript command did not return "count":2\n'
                    + strip_ansi(bytes(term.output[-12000:]))
                )
            output = strip_ansi(bytes(term.output))
            if '"flag":true' not in output:
                raise RuntimeError(
                    "extension flag was not visible through pi.getFlag\n"
                    + output[-12000:]
                )
            if '"name":"AS_SESSION"' not in output:
                raise RuntimeError(
                    "pi.setSessionName/pi.getSessionName did not round-trip\n"
                    + output[-12000:]
                )

            term.write(b"/reload\r")
            if not term.read_until(
                lambda _raw, text: "RELOAD_WIDGET_MARKER" in text, timeout=8.0
            ):
                raise RuntimeError(
                    "session reload did not rerun the session_start hook\n"
                    + strip_ansi(bytes(term.output[-12000:]))
                )

            term.write(b"/quit\r")
            if not term.read_until(
                lambda _raw, _text: term.proc is not None
                and term.proc.poll() is not None,
                timeout=5.0,
            ):
                raise RuntimeError("interactive probe did not exit after /quit")

        session_files = list(session_dir.rglob("*.jsonl"))
        if len(session_files) != 1:
            raise RuntimeError(
                "expected one persisted probe session, found "
                + repr([str(path) for path in session_files])
            )
        session_text = session_files[0].read_text(encoding="utf-8")
        if session_text.count('"customType":"as_probe"') != 2:
            raise RuntimeError(
                "pi.appendEntry did not persist twice in the session\n"
                + session_text[-12000:]
            )
        if session_text.count('"customType":"shortcut_probe"') != 1:
            raise RuntimeError(
                "shortcut handler did not persist exactly one entry\n"
                + session_text[-12000:]
            )
        for custom_type, expected_count in (
            ("pty_session_start", 2),
            ("pty_session_shutdown", 2),
        ):
            if session_text.count(f'"customType":"{custom_type}"') != expected_count:
                raise RuntimeError(
                    f"{custom_type} entry count was not {expected_count}\n"
                    + session_text[-12000:]
                )
    finally:
        os.chdir(previous_cwd)
        if previous_home is None:
            os.environ.pop("QT_CODING_AGENT_DIR", None)
        else:
            os.environ["QT_CODING_AGENT_DIR"] = previous_home


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default=".pio/build/native/qt")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    binary = Path(args.binary)
    if not binary.is_absolute():
        binary = (root / binary).resolve()
    if not binary.is_file():
        raise RuntimeError("native qt binary is missing; run pio run -e native")
    test_root = root / ".pio" / "test"
    test_root.mkdir(parents=True, exist_ok=True)
    runtime = Path(tempfile.mkdtemp(prefix="extension-host-probe-", dir=test_root))
    try:
        run_probe(binary, runtime)
    finally:
        shutil.rmtree(runtime, ignore_errors=True)
    print("AngelScript extension host probe passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
