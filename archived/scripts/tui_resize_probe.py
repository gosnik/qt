#!/usr/bin/env python3
"""Drive the interactive TUI in a PTY and verify SIGWINCH resize handling.

The probe starts the app at 24x100, waits for the first render, resizes the
PTY to 20x60 (which makes the kernel deliver SIGWINCH to the foreground
process group), and asserts that the app repaints its live region at the new
width without exiting. It runs once with the ANSI renderer forced and once
with notcurses requested; the assertion is renderer-agnostic so a regression
in either path (including a notcurses-to-ANSI fallback) is caught.
"""
import argparse
import fcntl
import os
import struct
import sys
import termios

from tui_pty_probe import TerminalProbe, strip_ansi


def last_rule_run(data: bytes) -> int:
    """Last completed rule; queued output can precede the resized frame."""
    text = data.decode("utf-8", errors="ignore")
    best = 0
    current = 0
    for ch in text:
        if ch == "\u2500":
            current += 1
        else:
            if current:
                best = current
            current = 0
    return best


def run_case(binary: str, notcurses: bool, rows: int, cols: int,
             new_rows: int, new_cols: int, timeout: float) -> list[str]:
    failures: list[str] = []
    label = "notcurses-requested" if notcurses else "ansi"
    env = os.environ.copy()
    env["QT_EXPERIMENTAL_NOTCURSES"] = "1" if notcurses else "0"
    os.environ["QT_EXPERIMENTAL_NOTCURSES"] = env["QT_EXPERIMENTAL_NOTCURSES"]
    try:
        with TerminalProbe([binary], rows, cols, timeout) as term:
            if not term.read_until(lambda raw, _text: b"\x1b[?25l" in raw,
                                   timeout=timeout):
                failures.append(f"{label}: initial render did not appear")
                return failures
            term.read_until(lambda _raw, _text: False, timeout=0.5)
            before = len(term.output)
            fcntl.ioctl(term.master, termios.TIOCSWINSZ,
                        struct.pack("HHHH", new_rows, new_cols, 0, 0))
            got = term.read_until(lambda raw, _text: last_rule_run(raw) == new_cols,
                                  timeout=timeout, offset=before)
            after = bytes(term.output)[before:]
            if not got or not after:
                failures.append(f"{label}: no output after resize")
                return failures
            if term.proc is not None and term.proc.poll() is not None:
                failures.append(f"{label}: process exited after resize")
                return failures
            rule = last_rule_run(after)
            if rule != new_cols:
                failures.append(
                    f"{label}: live region repainted at width {rule}, "
                    f"expected {new_cols}")
            if b"Segmentation fault" in after:
                failures.append(f"{label}: segmentation fault after resize")
            if not failures:
                print(f"ok: {label} repainted at {new_cols} columns "
                      f"({len(after)} bytes)")
    finally:
        os.environ.pop("QT_EXPERIMENTAL_NOTCURSES", None)
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default=".pio/build/native/qt")
    parser.add_argument("--rows", type=int, default=24)
    parser.add_argument("--cols", type=int, default=100)
    parser.add_argument("--new-rows", type=int, default=20)
    parser.add_argument("--new-cols", type=int, default=60)
    parser.add_argument("--timeout", type=float, default=8.0)
    args = parser.parse_args()

    failures: list[str] = []
    for notcurses in (False, True):
        failures.extend(run_case(args.binary, notcurses, args.rows, args.cols,
                                 args.new_rows, args.new_cols, args.timeout))
    if failures:
        for failure in failures:
            sys.stderr.write(f"FAIL: {failure}\n")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
