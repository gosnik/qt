#!/usr/bin/env python3

"""Exercise the ordinary keyboard-driven Memory Review Inbox workflow."""

import argparse
import fcntl
import json
import os
import pathlib
import shutil
import signal
import struct
import sqlite3
import sys
import tempfile
import termios
import time


def wait_for_text(term, offset: int, *needles: str, timeout: float = 8.0) -> None:
    if term.read_until(
        lambda raw, text: all(needle in text for needle in needles),
        timeout=timeout,
        offset=offset,
    ):
        return
    rendered = bytes(term.output[offset:]).decode("utf-8", errors="replace")
    raise RuntimeError(
        "review UI did not render " + ", ".join(needles) +
        "\nrecent terminal output:\n" + rendered[-4000:]
    )


def seed_review(agent_home: pathlib.Path) -> None:
    outside_evidence = agent_home / "outside-workspace-evidence.txt"
    outside_evidence.write_text("external review evidence\n", encoding="utf-8")
    database = agent_home / "memory" / "memory.sqlite3"
    with sqlite3.connect(database) as connection:
        subject = connection.execute(
            "SELECT id,user_id,organization_id,project_id,repository_id,branch_id "
            "FROM memories ORDER BY created_at DESC LIMIT 1"
        ).fetchone()
        if subject is None:
            raise RuntimeError("memory remember did not create a seed memory")
        scope_kind = "branch" if subject[5] else "repository"
        scope_values = [subject[1], subject[2], subject[3], subject[4],
                        subject[5], "", "", "", ""]
        scope_key = scope_kind + "|" + "".join(
            f"{len(value)}:{value}|" for value in scope_values
        )
        connection.execute(
            "UPDATE memories SET scope_key=?,scope_kind=?,task_id='',"
            "agent_id='',session_id='',conversation_id='' WHERE id=?",
            (scope_key, scope_kind, subject[0]),
        )
        now = int(time.time() * 1000)
        connection.execute(
            """
            INSERT INTO review_items(
              id,scope_key,kind,status,priority,reason,subject_id,source_id,
              source_type,source_uri,source_hash,source_span,created_by,
              evidence_strength,candidate_json,decision,decided_by,
              decision_note,tags_json,created_at,updated_at,decided_at,
              candidate_version,revision,deferred_until,application_status,
              application_error,action_key)
            VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
            """,
            (
                "review_ui_probe", scope_key, "candidate_memory", "pending",
                "high", "Verify the keyboard-driven Review Inbox", subject[0],
                "", "manual", str(outside_evidence), "probe-hash", "", "probe",
                0.8, json.dumps({"content": "Review inbox probe candidate",
                                 "target_status": "active",
                                 "tags": ["review:ui-probe"]}),
                "", "", "", json.dumps(["review:ui-probe"]), now, now, 0,
                1, 0, 0, "not_started", "", "",
            ),
        )
        connection.commit()


def run_probe(root: pathlib.Path, binary: pathlib.Path,
              runtime_dir: pathlib.Path, input_backend: str) -> None:
    sys.path.insert(0, str(root / "scripts"))
    from tui_pty_probe import TerminalProbe

    agent_home = runtime_dir / "agent-home"
    workspace = runtime_dir / "workspace"
    shutil.rmtree(runtime_dir, ignore_errors=True)
    agent_home.mkdir(parents=True)
    workspace.mkdir(parents=True)
    (agent_home / "settings.json").write_text(
        json.dumps({
            "memory": {
                "enabled": True,
                "automaticRecall": False,
                "automaticCapture": False,
                "dreaming": {"enabled": False},
            }
        }) + "\n",
        encoding="utf-8",
    )

    previous_home = os.environ.get("QT_CODING_AGENT_DIR")
    previous_notcurses = os.environ.get("QT_EXPERIMENTAL_NOTCURSES")
    previous_cwd = pathlib.Path.cwd()
    os.environ["QT_CODING_AGENT_DIR"] = str(agent_home)
    os.environ["QT_EXPERIMENTAL_NOTCURSES"] = (
        "1" if input_backend == "notcurses" else "0"
    )
    os.chdir(workspace)
    try:
        with TerminalProbe([str(binary)], 20, 100, 10.0) as term:
            term.read_until(lambda raw, text: False, timeout=2.0)
            if term.proc is not None and term.proc.poll() is not None:
                raise RuntimeError("qt exited before accepting review input")

            offset = len(term.output)
            term.write(b"/memory remember Review inbox probe candidate\r")
            wait_for_text(term, offset, "memory remember", "review")

            term.write(b"\x15/quit\r")
            if not term.read_until(
                lambda raw, text: term.proc is not None
                and term.proc.poll() is not None,
                timeout=5.0,
            ):
                raise RuntimeError("qt did not exit after creating seed memory")

        seed_review(agent_home)

        # A Review Inbox screen transition must replace the current review
        # overlay. If it pushes another overlay instead, returning from a queue
        # and then exiting the landing screen reveals the stale queue and traps
        # keyboard focus in the review UI.
        with TerminalProbe([str(binary)], 20, 100, 10.0) as escape_term:
            escape_term.read_until(lambda raw, text: False, timeout=2.0)
            if escape_term.proc is not None and escape_term.proc.poll() is not None:
                raise RuntimeError("qt exited before the review Escape probe")
            offset = len(escape_term.output)
            escape_term.write(b"/memory review\r")
            wait_for_text(escape_term, offset, "Memory Review Inbox", "Review next")
            offset = len(escape_term.output)
            escape_term.write(b"\r")
            wait_for_text(escape_term, offset, "Review next", "A approve")
            offset = len(escape_term.output)
            escape_term.write(b"\x1b")
            wait_for_text(escape_term, offset, "Memory Review Inbox", "Browse pending")
            offset = len(escape_term.output)
            escape_term.write(b"\x1b")
            escape_term.read_until(lambda raw, text: False, timeout=0.5)
            escape_term.write(b"review-escape-restored")
            wait_for_text(escape_term, offset, "review-escape-restored")

        # The focused Escape path enters a queue and therefore persists resume
        # state. Keep it independent from the menu-index checks below.
        with sqlite3.connect(agent_home / "memory" / "memory.sqlite3") as connection:
            connection.execute("DELETE FROM review_view_state")

        with TerminalProbe([str(binary)], 20, 100, 10.0) as term:
            term.read_until(lambda raw, text: False, timeout=2.0)
            if term.proc is not None and term.proc.poll() is not None:
                raise RuntimeError("qt exited before accepting review input")

            offset = len(term.output)
            term.write(b"/memory review\r")
            wait_for_text(term, offset, "Memory Review Inbox", "Review next",
                          "Batch pending reviews", "Latest Dreaming:",
                          "History: approved=")

            offset = len(term.output)
            term.write(b"\x1b[B\x1b[B\r")
            wait_for_text(term, offset, "Batch pending reviews", "Selected: 0",
                          "Kinds: none", "Scopes: none", "Risk: none",
                          "Preview selected batch")
            offset = len(term.output)
            term.write(b"\x1b[B\x1b[B\x1b[B\r")
            wait_for_text(term, offset, "Batch pending reviews", "Selected: 1",
                          "Kinds: candidate_memory=1", "Risk:")
            offset = len(term.output)
            term.write(b"\r")
            wait_for_text(term, offset, "Batch action", "Approve selected",
                          "Reject selected", "homogeneous, low-risk")
            offset = len(term.output)
            term.write(b"\x1b[B\r")
            wait_for_text(term, offset, "Confirm batch reject", "Count: 1",
                          "Scope filter:", "Aggregate effect:",
                          "Change: review review_ui_probe compare_and_set",
                          "Confirm batch")
            offset = len(term.output)
            term.write(b"\r")
            wait_for_text(term, offset, "Batch pending reviews", "Selected: 1")
            offset = len(term.output)
            term.write(b"\x1b")
            wait_for_text(term, offset, "Memory Review Inbox", "Review next")

            offset = len(term.output)
            term.write(b"\r")
            wait_for_text(term, offset, "Review next", "A approve",
                          "E edit", "R reject", "D defer", "S next")

            offset = len(term.output)
            term.write(b"\r")
            wait_for_text(term, offset, "Review Detail", "Proposal:",
                          "Scope (exact):",
                          "Approve as active", "Edit & approve",
                          "Defer / Snooze")

            offset = len(term.output)
            for _ in range(6):
                term.write(b"\x1b[6~")
                term.read_until(lambda raw, text: False, timeout=0.12)
            wait_for_text(term, offset, "PgUp/PgDn scroll", "Will not:")
            for _ in range(6):
                term.write(b"\x1b[5~")
                term.read_until(lambda raw, text: False, timeout=0.12)

            offset = len(term.output)
            term.write(b"e")
            wait_for_text(term, offset, "Edit review proposal", "Content:")
            offset = len(term.output)
            term.write(b"\x1b")
            wait_for_text(term, offset, "Review Detail", "Approve as active")
            offset = len(term.output)
            term.write(b"e")
            wait_for_text(term, offset, "Edit review proposal", "Content:")
            term.write(b"\r")
            wait_for_text(term, offset, "Edit supported fields", "Type",
                          "Target trust state", "Tags", "Confidence",
                          "Replacement link", "Scope narrowing")
            offset = len(term.output)
            term.write(b"\x1b")
            wait_for_text(term, offset, "Review Detail", "Approve as active")

            offset = len(term.output)
            term.write(b"\x1b[B" * 5 + b"\r")
            wait_for_text(term, offset, "Evidence and Context")
            offset = len(term.output)
            term.write(b"\x1b[6~")
            wait_for_text(
                term, offset,
                "BLOCKED: source evidence is not a permitted local workspace path")
            term.write(b"\x1b")
            wait_for_text(term, offset, "Review Detail", "Approve as active")

            # The action list is intentionally scrollable at a 20-row terminal.
            # Exercise that narrow-terminal path before using the direct key.
            term.write(b"\x1b[B" * 6)
            wait_for_text(term, offset, "Next [S]")

            offset = len(term.output)
            term.write(b"s")
            wait_for_text(term, offset, "Review Detail",
                          "Review inbox probe candidate")

            offset = len(term.output)
            term.write(b"d")
            wait_for_text(term, offset, "Defer review", "Tomorrow",
                          "In one week")
            offset = len(term.output)
            term.write(b"\x1b")
            wait_for_text(term, offset, "Review Detail", "Approve")

            offset = len(term.output)
            term.write(b"r")
            wait_for_text(term, offset, "Review note", "Optional audit note")
            offset = len(term.output)
            term.write(b"\r")
            wait_for_text(term, offset, "Confirm reject", "Scope (exact):",
                          "Will:", "Will not:")
            offset = len(term.output)
            term.write(b"\x1b[6~")
            wait_for_text(term, offset,
                          "Change: review review_ui_probe compare_and_set",
                          "Before:", "After:")
            offset = len(term.output)
            term.write(b"\r")
            wait_for_text(term, offset, "Review Detail", "Approve")

            offset = len(term.output)
            term.write(b"\x1b")
            wait_for_text(term, offset, "Review next", "A approve")
            offset = len(term.output)
            term.write(b"\x1b")
            wait_for_text(term, offset, "Memory Review Inbox", "Browse pending")

            offset = len(term.output)
            # A saved review position adds Resume previous review at the top.
            term.write(b"\x1b[B\x1b[B\x1b[B\x1b[B\r")
            wait_for_text(term, offset, "Review filters", "Priority", "Status",
                          "Kind / category", "Tags", "Toggle oldest/newest")
            offset = len(term.output)
            term.write(b"\x1b[B\r")
            wait_for_text(term, offset, "Filter by priority", "Urgent", "High",
                          "Normal", "Low")
            offset = len(term.output)
            term.write(b"\x1b")
            wait_for_text(term, offset, "Review filters", "Apply filters")
            offset = len(term.output)
            term.write(b"\x1b")
            wait_for_text(term, offset, "Memory Review Inbox", "Review next")
            offset = len(term.output)
            fcntl.ioctl(term.master, termios.TIOCSWINSZ,
                        struct.pack("HHHH", 12, 52, 0, 0))
            if term.proc is not None:
                term.proc.send_signal(signal.SIGWINCH)
            wait_for_text(term, offset, "Memory Review Inbox", "Review next")

            # TerminalProbe owns cleanup; the workflow assertions above are
            # complete without coupling this focused probe to /quit handling.

        with TerminalProbe([str(binary)], 20, 100, 10.0) as focus_term:
            focus_term.read_until(lambda raw, text: False, timeout=2.0)
            if focus_term.proc is not None and focus_term.proc.poll() is not None:
                raise RuntimeError("qt exited before the focus restoration probe")
            offset = len(focus_term.output)
            focus_term.write(b"/memory review\r")
            wait_for_text(focus_term, offset, "Memory Review Inbox", "Review next")
            offset = len(focus_term.output)
            focus_term.write(b"\x1b")
            focus_term.read_until(lambda raw, text: False, timeout=1.0)
            focus_term.write(b"focus-restored")
            wait_for_text(focus_term, offset, "focus-restored")

    finally:
        os.chdir(previous_cwd)
        if previous_home is None:
            os.environ.pop("QT_CODING_AGENT_DIR", None)
        else:
            os.environ["QT_CODING_AGENT_DIR"] = previous_home
        if previous_notcurses is None:
            os.environ.pop("QT_EXPERIMENTAL_NOTCURSES", None)
        else:
            os.environ["QT_EXPERIMENTAL_NOTCURSES"] = previous_notcurses


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify the interactive Memory Review Inbox")
    parser.add_argument("--binary", default=".pio/build/native/qt")
    parser.add_argument(
        "--input-backend", choices=("fallback", "notcurses"),
        default="fallback",
        help="select the native terminal input path exercised by the probe",
    )
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    binary = pathlib.Path(args.binary)
    if not binary.is_absolute():
        binary = (root / binary).resolve()
    if not binary.is_file():
        raise RuntimeError("native qt binary is missing; run `pio run -e native`")
    with tempfile.TemporaryDirectory(prefix="qt-memory-review-ui-probe-") as temporary:
        run_probe(root, binary, pathlib.Path(temporary), args.input_backend)
    print("memory review UI probe passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
