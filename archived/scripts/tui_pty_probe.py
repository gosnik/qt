#!/usr/bin/env python3
import argparse
import fcntl
import os
import pty
import re
import select
import signal
import shlex
import struct
import subprocess
import sys
import termios
import time
import uuid


ESC = b"\x1b"
ST = ESC + b"\\"



class LibvtermUnavailable(RuntimeError):
    pass


class LibvtermReplay:
    helper_source = '\n#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <vterm.h>\n\nstruct ReplayState {\n  char* scrollback;\n  size_t scrollback_len;\n  size_t scrollback_cap;\n  int scrollback_lines;\n};\n\nstatic void append_bytes(struct ReplayState* state, const char* bytes, size_t len) {\n  if (len == 0) return;\n  if (state->scrollback_len + len + 1 > state->scrollback_cap) {\n    size_t next = state->scrollback_cap == 0 ? 4096 : state->scrollback_cap;\n    while (state->scrollback_len + len + 1 > next) next *= 2;\n    char* grown = (char*)realloc(state->scrollback, next);\n    if (grown == NULL) return;\n    state->scrollback = grown;\n    state->scrollback_cap = next;\n  }\n  memcpy(state->scrollback + state->scrollback_len, bytes, len);\n  state->scrollback_len += len;\n  state->scrollback[state->scrollback_len] = \'\\0\';\n}\n\nstatic void append_codepoint(struct ReplayState* state, uint32_t cp) {\n  char out[4];\n  size_t len = 0;\n  if (cp == 0) return;\n  if (cp < 0x80) {\n    out[len++] = (char)cp;\n  } else if (cp < 0x800) {\n    out[len++] = (char)(0xc0 | (cp >> 6));\n    out[len++] = (char)(0x80 | (cp & 0x3f));\n  } else if (cp < 0x10000) {\n    out[len++] = (char)(0xe0 | (cp >> 12));\n    out[len++] = (char)(0x80 | ((cp >> 6) & 0x3f));\n    out[len++] = (char)(0x80 | (cp & 0x3f));\n  } else {\n    out[len++] = (char)(0xf0 | (cp >> 18));\n    out[len++] = (char)(0x80 | ((cp >> 12) & 0x3f));\n    out[len++] = (char)(0x80 | ((cp >> 6) & 0x3f));\n    out[len++] = (char)(0x80 | (cp & 0x3f));\n  }\n  append_bytes(state, out, len);\n}\n\nstatic int on_damage(VTermRect rect, void* user) { (void)rect; (void)user; return 1; }\nstatic int on_moverect(VTermRect dest, VTermRect src, void* user) { (void)dest; (void)src; (void)user; return 1; }\nstatic int on_movecursor(VTermPos pos, VTermPos oldpos, int visible, void* user) { (void)pos; (void)oldpos; (void)visible; (void)user; return 1; }\nstatic int on_settermprop(VTermProp prop, VTermValue* val, void* user) { (void)prop; (void)val; (void)user; return 1; }\nstatic int on_bell(void* user) { (void)user; return 1; }\nstatic int on_resize(int rows, int cols, void* user) { (void)rows; (void)cols; (void)user; return 1; }\n\nstatic int on_sb_pushline(int cols, const VTermScreenCell* cells, void* user) {\n  struct ReplayState* state = (struct ReplayState*)user;\n  state->scrollback_lines += 1;\n  int last = cols - 1;\n  while (last >= 0 && cells[last].chars[0] == 0) last -= 1;\n  for (int col = 0; col <= last; ++col) {\n    if (cells[col].chars[0] == 0) {\n      append_bytes(state, " ", 1);\n      continue;\n    }\n    for (int idx = 0; idx < VTERM_MAX_CHARS_PER_CELL && cells[col].chars[idx] != 0; ++idx) {\n      append_codepoint(state, cells[col].chars[idx]);\n    }\n  }\n  append_bytes(state, "\\n", 1);\n  return 1;\n}\n\nstatic int on_sb_popline(int cols, VTermScreenCell* cells, void* user) {\n  (void)cols;\n  (void)cells;\n  (void)user;\n  return 0;\n}\n\nstatic void print_screen(VTermScreen* screen, int rows, int cols) {\n  for (int row = 0; row < rows; ++row) {\n    int last = cols - 1;\n    VTermScreenCell cell;\n    while (last >= 0) {\n      VTermPos pos = { row, last };\n      if (!vterm_screen_get_cell(screen, pos, &cell) || cell.chars[0] == 0) {\n        last -= 1;\n        continue;\n      }\n      break;\n    }\n    for (int col = 0; col <= last; ++col) {\n      VTermPos pos = { row, col };\n      memset(&cell, 0, sizeof(cell));\n      if (!vterm_screen_get_cell(screen, pos, &cell) || cell.chars[0] == 0) {\n        putchar(\' \');\n        continue;\n      }\n      for (int idx = 0; idx < VTERM_MAX_CHARS_PER_CELL && cell.chars[idx] != 0; ++idx) {\n        uint32_t cp = cell.chars[idx];\n        if (cp < 0x80) {\n          putchar((char)cp);\n        } else {\n          putchar(\'?\');\n        }\n      }\n    }\n    putchar(\'\\n\');\n  }\n}\n\nint main(int argc, char** argv) {\n  if (argc != 4) return 2;\n  int rows = atoi(argv[1]);\n  int cols = atoi(argv[2]);\n  const char* path = argv[3];\n  FILE* fp = fopen(path, "rb");\n  if (fp == NULL) return 3;\n\n  VTerm* vt = vterm_new(rows, cols);\n  if (vt == NULL) return 4;\n  VTermScreen* screen = vterm_obtain_screen(vt);\n  struct ReplayState state = {0};\n  VTermScreenCallbacks callbacks = {0};\n  callbacks.damage = on_damage;\n  callbacks.moverect = on_moverect;\n  callbacks.movecursor = on_movecursor;\n  callbacks.settermprop = on_settermprop;\n  callbacks.bell = on_bell;\n  callbacks.resize = on_resize;\n  callbacks.sb_pushline = on_sb_pushline;\n  callbacks.sb_popline = on_sb_popline;\n  vterm_screen_set_callbacks(screen, &callbacks, &state);\n  vterm_screen_reset(screen, 1);\n\n  char buffer[4096];\n  size_t nread = 0;\n  while ((nread = fread(buffer, 1, sizeof(buffer), fp)) > 0) {\n    vterm_input_write(vt, buffer, nread);\n    vterm_screen_flush_damage(screen);\n  }\n  fclose(fp);\n\n  printf("SCROLLBACK_LINES %d\\n", state.scrollback_lines);\n  printf("VISIBLE_BEGIN\\n");\n  print_screen(screen, rows, cols);\n  printf("VISIBLE_END\\n");\n  printf("SCROLLBACK_BEGIN\\n");\n  if (state.scrollback != NULL) fputs(state.scrollback, stdout);\n  printf("SCROLLBACK_END\\n");\n\n  free(state.scrollback);\n  vterm_free(vt);\n  return 0;\n}\n'

    def __init__(self, rows: int, cols: int, scrollback_limit: int):
        self.rows = rows
        self.cols = cols
        self.scrollback_limit = scrollback_limit
        self.scrollback: list[str] = []
        self.visible = ""

    def feed(self, data: bytes):
        import tempfile
        helper = self._helper_path()
        with tempfile.NamedTemporaryFile(delete=False) as raw_file:
            raw_file.write(data)
            raw_path = raw_file.name
        try:
            result = subprocess.run(
                [str(helper), str(self.rows), str(self.cols), raw_path],
                check=True,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
        finally:
            try:
                os.unlink(raw_path)
            except OSError:
                pass
        self._parse_output(result.stdout)
        if len(self.scrollback) > self.scrollback_limit:
            self.scrollback = self.scrollback[-self.scrollback_limit:]

    def visible_text(self) -> str:
        return self.visible

    def scrollback_text(self) -> str:
        return "\n".join(self.scrollback)

    def _parse_output(self, output: str):
        visible_match = re.search(r"VISIBLE_BEGIN\n(.*?)VISIBLE_END\n", output, re.DOTALL)
        scroll_match = re.search(r"SCROLLBACK_BEGIN\n(.*?)SCROLLBACK_END\n", output, re.DOTALL)
        self.visible = visible_match.group(1).rstrip("\n") if visible_match else ""
        scrollback = scroll_match.group(1) if scroll_match else ""
        self.scrollback = [line for line in scrollback.splitlines() if line.strip()]

    @classmethod
    def _helper_path(cls) -> str:
        from pathlib import Path
        build_dir = Path(".pio/build/tui-pty-probe")
        build_dir.mkdir(parents=True, exist_ok=True)
        source = build_dir / "libvterm_replay.c"
        binary = build_dir / "libvterm_replay"
        if not source.exists() or source.read_text() != cls.helper_source:
            source.write_text(cls.helper_source)
        if binary.exists() and binary.stat().st_mtime >= source.stat().st_mtime:
            return str(binary)

        pkg = cls._pkg_config_name()
        if pkg is None:
            raise LibvtermUnavailable(
                "libvterm development files are required: pkg-config could not find `vterm` or `libvterm`"
            )
        cflags = subprocess.check_output(["pkg-config", "--cflags", pkg], text=True).split()
        libs = subprocess.check_output(["pkg-config", "--libs", pkg], text=True).split()
        cmd = [os.environ.get("CC", "cc"), "-std=c99", "-Wall", "-Wextra", *cflags,
               str(source), "-o", str(binary), *libs]
        try:
            subprocess.run(cmd, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        except subprocess.CalledProcessError as error:
            raise LibvtermUnavailable(error.stderr.strip() or "failed to compile libvterm replay helper") from error
        return str(binary)

    @staticmethod
    def _pkg_config_name() -> str | None:
        for name in ("vterm", "libvterm"):
            if subprocess.run(["pkg-config", "--exists", name]).returncode == 0:
                return name
        return None

def strip_ansi(data: bytes) -> str:
    text = data.decode("utf-8", errors="ignore")
    text = re.sub(r"\x1b\][^\x1b]*(?:\x1b\\|\x07)", "", text)
    text = re.sub(r"\x1bP.*?\x1b\\", "", text, flags=re.DOTALL)
    text = re.sub(r"\x1b_G.*?\x1b\\", "", text, flags=re.DOTALL)
    text = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", text)
    text = re.sub(r"\x1b[()*+./][0-~]", "", text)
    text = re.sub(r"\x1b[()#=><78DM]", "", text)
    return text


class TerminalProbe:
    def __init__(self, argv: list[str], rows: int, cols: int, timeout: float):
        self.argv = argv
        self.rows = rows
        self.cols = cols
        self.timeout = timeout
        self.master = -1
        self.proc: subprocess.Popen[bytes] | None = None
        self.output = bytearray()
        self.responded: set[bytes] = set()

    def __enter__(self):
        master, slave = pty.openpty()
        self.master = master
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", self.rows, self.cols, 0, 0))
        def make_controlling_tty():
            os.setsid()
            if hasattr(termios, "TIOCSCTTY"):
                fcntl.ioctl(slave, termios.TIOCSCTTY, 0)

        env = os.environ.copy()
        env.setdefault("TERM", "xterm-256color")
        env.setdefault("QT_EXPERIMENTAL_NOTCURSES", "0")
        self.proc = subprocess.Popen(
            self.argv,
            stdin=slave,
            stdout=slave,
            stderr=slave,
            env=env,
            close_fds=True,
            preexec_fn=make_controlling_tty,
        )
        os.close(slave)
        return self

    def __exit__(self, exc_type, exc, tb):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=2)
        if self.master >= 0:
            os.close(self.master)

    def write(self, data: bytes):
        os.write(self.master, data)

    def read_until(self, predicate, timeout: float | None = None, offset: int = 0) -> bool:
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        while time.monotonic() < deadline:
            self._read_available(0.05)
            self._answer_terminal_queries()
            raw = bytes(self.output)[offset:]
            if predicate(raw, strip_ansi(raw)):
                return True
            if self.proc is not None and self.proc.poll() is not None:
                self._read_available(0)
                raw = bytes(self.output)[offset:]
                return predicate(raw, strip_ansi(raw))
        return False

    def _read_available(self, timeout: float):
        ready, _, _ = select.select([self.master], [], [], timeout)
        if not ready:
            return
        try:
            chunk = os.read(self.master, 8192)
        except OSError:
            return
        self.output.extend(chunk)

    def _respond_once(self, key: bytes, response: bytes):
        if key in self.output and key not in self.responded:
            self.responded.add(key)
            self.write(response)

    def _answer_terminal_queries(self):
        data = bytes(self.output)
        self._respond_once(ESC + b"[6n", ESC + b"[1;1R")
        self._respond_once(ESC + b"[c", ESC + b"[?1;2c")
        self._respond_once(ESC + b"[=c", ESC + b"[?1;2c")
        self._respond_once(ESC + b"[>c", ESC + b"[>0;115;0c")
        self._respond_once(ESC + b"[18t", ESC + b"[8;%d;%dt" % (self.rows, self.cols))
        self._respond_once(ESC + b"[14t", ESC + b"[4;%d;%dt" % (self.rows * 20, self.cols * 8))
        self._respond_once(ESC + b"[?u", ESC + b"[?0u")
        self._respond_once(ESC + b"_Gi=1,a=q;" + ST, ESC + b"_Gi=1;OK" + ST)

        for match in re.finditer(rb"\x1b\]4;([0-9]+);\?\x1b\\", data):
            key = match.group(0)
            if key not in self.responded:
                self.responded.add(key)
                self.write(ESC + b"]4;" + match.group(1) + b";rgb:0000/0000/0000" + ST)

        for osc in (b"10", b"11"):
            key = ESC + b"]" + osc + b";?" + ST
            if key in data and key not in self.responded:
                self.responded.add(key)
                self.write(ESC + b"]" + osc + b";rgb:ffff/ffff/ffff" + ST)

        for match in re.finditer(rb"\x1b\[\?([0-9]+)\$p", data):
            key = match.group(0)
            if key not in self.responded:
                self.responded.add(key)
                self.write(ESC + b"[?" + match.group(1) + b";0$y")

        for match in re.finditer(rb"\x1bP\+q([0-9A-Fa-f;]+)\x1b\\", data):
            key = match.group(0)
            if key not in self.responded:
                self.responded.add(key)
                self.write(ESC + b"P0+r" + ST)


def contains_any(text: str, needles: list[str]) -> bool:
    lowered = text.lower()
    return any(needle.lower() in lowered for needle in needles)


def virtual_scrollback_has_transcript(raw: bytes, rows: int, cols: int,
                                      scrollback_lines: int) -> bool:
    virtual = LibvtermReplay(rows, cols, scrollback_lines)
    virtual.feed(raw)
    scrollback_text = "\n".join(extract_scroll_margin_lines(raw, scrollback_lines))
    return contains_any(
        virtual.visible_text() + "\n" + scrollback_text,
        ["qt", "type a prompt", "commands", "help", "update"],
    ) and len(scrollback_text.strip()) > 0 and libvterm_screen_has_fixed_bottom(
        virtual.visible_text()
    )


def extract_scroll_margin_lines(raw: bytes, limit: int) -> list[str]:
    lines: list[str] = []
    current_region: tuple[int, int] | None = None
    cursor_row = 1
    cursor_col = 1
    capture = bytearray()
    index = 0
    while index < len(raw):
        if raw[index:index + 2] == b"\x1b[":
            end = index + 2
            while end < len(raw) and not (0x40 <= raw[end] <= 0x7E):
                end += 1
            if end >= len(raw):
                break
            body = raw[index + 2:end].decode("ascii", errors="ignore")
            final = chr(raw[end])
            if final == "r":
                parts = [part for part in body.split(";") if part]
                if len(parts) >= 2 and parts[0].isdigit() and parts[1].isdigit():
                    current_region = (int(parts[0]), int(parts[1]))
                else:
                    current_region = None
            elif final in ("H", "f"):
                parts = [part for part in body.split(";") if part]
                if len(parts) >= 2 and parts[0].isdigit() and parts[1].isdigit():
                    cursor_row = int(parts[0])
                    cursor_col = int(parts[1])
                    if current_region and cursor_row == current_region[1] and cursor_col == 1:
                        capture.clear()
            index = end + 1
            continue
        byte = raw[index]
        if byte == 0x0A:
            if current_region and cursor_row == current_region[1] and capture:
                lines.append(strip_ansi(bytes(capture)).strip())
                lines = [line for line in lines if line]
                if len(lines) > limit:
                    lines = lines[-limit:]
                capture.clear()
            index += 1
            continue
        if current_region and cursor_row == current_region[1]:
            capture.append(byte)
        index += 1
    return lines


def libvterm_screen_has_fixed_bottom(visible_text: str) -> bool:
    return contains_any(
        visible_text, ["qt>", "~/", "(openrouter)", "no-model", "openrouter"]
    )


def visible_screen_text(raw: bytes, rows: int, cols: int, scrollback_lines: int) -> str:
    virtual = LibvtermReplay(rows, cols, scrollback_lines)
    virtual.feed(raw)
    return virtual.visible_text()


def slash_overlay_visible(raw: bytes, rows: int, cols: int,
                          scrollback_lines: int) -> bool:
    visible = visible_screen_text(raw, rows, cols, scrollback_lines)
    text = visible + "\n" + strip_ansi(raw)
    return "/" in text and "Completions" in text and contains_any(
        text, ["model", "help", "quit", "llama"]
    )


def index_tree_output_visible(raw: bytes, rows: int, cols: int,
                              scrollback_lines: int) -> bool:
    visible = visible_screen_text(raw, rows, cols, scrollback_lines)
    text = visible + "\n" + strip_ansi(raw)
    if "unknown slash command" in text:
        return False
    return "index tree" in text and "categories" in text


def index_status_running_visible(raw: bytes, rows: int, cols: int,
                                 scrollback_lines: int) -> bool:
    visible = visible_screen_text(raw, rows, cols, scrollback_lines)
    text = visible + "\n" + strip_ansi(raw)
    if "unknown slash command" in text:
        return False
    return (
        "index status" in text and
        '"running": true' in text and
        '"scan_progress_percent"' in text and
        '"scan_phase"' in text
    )


def tmux_available() -> bool:
    return subprocess.run(
        ["sh", "-c", "command -v tmux >/dev/null 2>&1"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    ).returncode == 0


def run_tmux(args: argparse.Namespace, *tmux_args: str,
             check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["tmux", "-L", args.tmux_socket, *tmux_args],
        check=check,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def capture_tmux_pane(args: argparse.Namespace, target: str) -> str:
    result = run_tmux(args, "capture-pane", "-S", "-100", "-p", "-t", target)
    return result.stdout


def split_tmux_history_visible(capture: str, rows: int) -> tuple[str, str]:
    lines = capture.splitlines()
    if len(lines) <= rows:
        return "", capture
    return "\n".join(lines[:-rows]), "\n".join(lines[-rows:])


def tmux_capture_has_clean_scrollback(capture: str, rows: int) -> bool:
    history, visible = split_tmux_history_visible(capture, rows)
    transcript_ok = contains_any(
        history, ["/help -", "/model -", "/tools -", "/quit -", "Slash commands:"]
    ) and contains_any(history, ["qt", "Type a prompt"])
    fixed_visible_ok = contains_any(visible, ["~/", "openrouter", "no-model", "qt>"])
    leaked_overlay = contains_any(history, ["Completions"])
    leaked_footer = contains_any(history, ["~/", "(openrouter)", "(no-model)"])
    return transcript_ok and fixed_visible_ok and not leaked_overlay and not leaked_footer


def tmux_startup_visible_is_clean(capture: str, rows: int) -> bool:
    _, visible = split_tmux_history_visible(capture, rows)
    return (
        visible.count("~/") <= 1
        and contains_any(visible, ["qt"])
        and contains_any(visible, ["Type a prompt"])
        and contains_any(visible, ["openrouter", "no-model", "qt>"])
    )


def tmux_visible_has_frame(capture: str, rows: int) -> bool:
    _, visible = split_tmux_history_visible(capture, rows)
    return all(part in visible for part in ("┌", "┐", "└", "┘"))


def tmux_settings_overlay_visible(capture: str, rows: int) -> bool:
    _, visible = split_tmux_history_visible(capture, rows)
    return tmux_visible_has_frame(capture, rows) and contains_any(
        visible,
        ["Quiet startup", "Changelog", "Thinking", "Skill commands", "Terminal progress"],
    )


def tmux_model_overlay_visible_or_unavailable(capture: str, rows: int) -> bool:
    _, visible = split_tmux_history_visible(capture, rows)
    text = capture + "\n" + visible
    if contains_any(text, ["no models available", "no available models matched"]):
        return True
    return tmux_visible_has_frame(capture, rows) and contains_any(
        visible,
        ["openrouter", "llama", "model", "input", "output", "$"],
    )


def tmux_llama_overlay_visible(capture: str, rows: int) -> bool:
    _, visible = split_tmux_history_visible(capture, rows)
    return tmux_visible_has_frame(capture, rows) and contains_any(
        visible,
        ["status", "models", "select", "run", "prompt", "KV cache", "local llama"],
    ) and "llama provider:" not in visible


def tmux_slash_overlay_repaints_cleanly(capture: str, rows: int) -> bool:
    _, visible = split_tmux_history_visible(capture, rows)
    return (
        "Completions" in visible
        and visible.count("Completions") == 1
        and visible.count("┌") <= 1
        and visible.count("└") <= 1
    )


def wait_for_tmux_capture(args: argparse.Namespace, target: str, predicate) -> bool:
    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline:
        capture = capture_tmux_pane(args, target)
        if predicate(capture):
            return True
        time.sleep(0.1)
    return False


def tmux_scrollback_probe(args: argparse.Namespace) -> tuple[bool, str, str]:
    if not tmux_available():
        return False, "", "tmux is not installed or not on PATH"

    args.tmux_socket = f"qt-tui-probe-{os.getpid()}-{uuid.uuid4().hex[:8]}"
    binary = os.path.abspath(args.binary)
    command = "env QT_EXPERIMENTAL_NOTCURSES=0 " + shlex.quote(binary)
    sessions: list[str] = []

    def start_app() -> tuple[str, str, str]:
        session = f"qt-tui-probe-{uuid.uuid4().hex[:12]}"
        target = f"{session}:0.0"
        sessions.append(session)
        run_tmux(
            args,
            "new-session",
            "-d",
            "-x",
            str(args.cols),
            "-y",
            str(args.rows),
            "-s",
            session,
            command,
        )
        ready = wait_for_tmux_capture(
            args,
            target,
            lambda text: tmux_startup_visible_is_clean(text, args.rows),
        )
        if not ready:
            return target, capture_tmux_pane(args, target), (
                "tmux app pane did not reach a clean startup layout"
            )
        return target, "", ""

    try:
        target, capture, error = start_app()
        if error:
            return False, capture, error
        run_tmux(args, "send-keys", "-t", target, "/")
        overlay_ready = wait_for_tmux_capture(
            args,
            target,
            lambda text: contains_any(text, ["Completions"])
            and contains_any(text, ["help", "model", "quit", "llama"]),
        )
        if not overlay_ready:
            return False, capture_tmux_pane(args, target), "tmux slash overlay did not appear"
        run_tmux(args, "send-keys", "-t", target, "h", "e")
        typed_overlay_ready = wait_for_tmux_capture(
            args,
            target,
            lambda text: tmux_slash_overlay_repaints_cleanly(text, args.rows),
        )
        if not typed_overlay_ready:
            return False, capture_tmux_pane(args, target), (
                "tmux slash overlay left stale rows while typing"
            )
        run_tmux(args, "kill-session", "-t", target.split(":", 1)[0], check=False)

        target, capture, error = start_app()
        if error:
            return False, capture, error
        run_tmux(args, "send-keys", "-t", target, "/help", "Enter")
        ok = wait_for_tmux_capture(
            args,
            target,
            lambda text: tmux_capture_has_clean_scrollback(text, args.rows),
        )
        if not ok:
            capture = capture_tmux_pane(args, target)
            return False, capture, (
                "tmux capture-pane did not show clean native scrollback "
                "with overlay/footer restricted to the visible fixed area"
            )
        run_tmux(args, "kill-session", "-t", target.split(":", 1)[0], check=False)

        target, capture, error = start_app()
        if error:
            return False, capture, error
        run_tmux(args, "send-keys", "-t", target, "/settings", "Enter")
        settings_ok = wait_for_tmux_capture(
            args,
            target,
            lambda text: tmux_settings_overlay_visible(text, args.rows),
        )
        if not settings_ok:
            return False, capture_tmux_pane(args, target), (
                "tmux settings overlay was clipped or did not show a full frame"
            )
        run_tmux(args, "kill-session", "-t", target.split(":", 1)[0], check=False)

        target, capture, error = start_app()
        if error:
            return False, capture, error
        run_tmux(args, "send-keys", "-t", target, "/model", "Enter")
        model_ok = wait_for_tmux_capture(
            args,
            target,
            lambda text: tmux_model_overlay_visible_or_unavailable(text, args.rows),
        )
        capture = capture_tmux_pane(args, target)
        if not model_ok:
            return False, capture, (
                "tmux model overlay was clipped or did not show a full frame"
            )
        run_tmux(args, "kill-session", "-t", target.split(":", 1)[0], check=False)

        target, capture, error = start_app()
        if error:
            return False, capture, error
        run_tmux(args, "send-keys", "-t", target, "/llama", "Enter")
        llama_ok = wait_for_tmux_capture(
            args,
            target,
            lambda text: tmux_llama_overlay_visible(text, args.rows),
        )
        capture = capture_tmux_pane(args, target)
        if not llama_ok:
            return False, capture, (
                "tmux llama submenu was clipped or showed status instead of the menu"
            )
        return True, capture, ""
    finally:
        for session in sessions:
            run_tmux(args, "kill-session", "-t", session, check=False)


def main() -> int:
    parser = argparse.ArgumentParser(description="Drive the interactive TUI in a PTY.")
    parser.add_argument("--binary", default=".pio/build/native/qt")
    parser.add_argument("--timeout", type=float, default=8.0)
    parser.add_argument("--rows", type=int, default=12)
    parser.add_argument("--cols", type=int, default=100)
    parser.add_argument("--scrollback-lines", type=int, default=200)
    parser.add_argument("--dump", default="")
    parser.add_argument("--skip-tmux", action="store_true",
                        help="skip the tmux capture-pane scrollback assertion")
    args = parser.parse_args()
    args.tmux_socket = ""

    try:
        LibvtermReplay._helper_path()
    except LibvtermUnavailable as error:
        sys.stderr.write("tui pty probe requires libvterm:\n")
        sys.stderr.write(f"- {error}\n")
        sys.stderr.write("- install the libvterm development package that provides vterm.h and pkg-config metadata\n")
        return 2

    with TerminalProbe([args.binary], args.rows, args.cols, args.timeout) as slash_term:
        slash_term.read_until(
            lambda raw, text: libvterm_screen_has_fixed_bottom(
                visible_screen_text(
                    bytes(slash_term.output), args.rows, args.cols,
                    args.scrollback_lines,
                )
            ),
            timeout=args.timeout,
        )
        slash_offset = len(slash_term.output)
        slash_term.write(b"/")
        slash_ok = slash_term.read_until(
            lambda raw, text: slash_overlay_visible(
                bytes(slash_term.output), args.rows, args.cols,
                args.scrollback_lines,
            ),
            timeout=args.timeout,
            offset=slash_offset,
        )
        slash_raw = bytes(slash_term.output)

    with TerminalProbe([args.binary], args.rows, args.cols, args.timeout) as index_term:
        index_term.read_until(lambda raw, text: b"\x1b[?1h" in raw or "qt>" in text,
                              timeout=3.0)
        index_offset = len(index_term.output)
        index_term.write(b"/index tree\r")
        index_ok = index_term.read_until(
            lambda raw, text: index_tree_output_visible(
                bytes(index_term.output), args.rows, args.cols,
                args.scrollback_lines
            ),
            timeout=args.timeout,
            offset=index_offset,
        )
        index_raw = bytes(index_term.output)

    with TerminalProbe([args.binary], args.rows, args.cols, args.timeout) as index_status_term:
        index_status_term.read_until(lambda raw, text: b"\x1b[?1h" in raw or "qt>" in text,
                                     timeout=3.0)
        index_status_offset = len(index_status_term.output)
        index_status_term.write(b"/index status\r")
        index_status_ok = index_status_term.read_until(
            lambda raw, text: index_status_running_visible(
                bytes(index_status_term.output), args.rows, args.cols,
                args.scrollback_lines
            ),
            timeout=args.timeout,
            offset=index_status_offset,
        )
        index_status_raw = bytes(index_status_term.output)

    with TerminalProbe([args.binary], args.rows, args.cols, args.timeout) as scroll_term:
        scroll_term.read_until(lambda raw, text: b"\x1b[?1h" in raw or "qt>" in text,
                               timeout=3.0)
        help_offset = len(scroll_term.output)
        scroll_term.write(b"/help\r")
        scroll_ok = scroll_term.read_until(
            lambda raw, text: virtual_scrollback_has_transcript(
                bytes(scroll_term.output), args.rows, args.cols, args.scrollback_lines
            ),
            timeout=args.timeout,
            offset=help_offset,
        )
        scroll_raw = bytes(scroll_term.output)

    raw = (slash_raw + b"\n--- index session ---\n" + index_raw +
           b"\n--- index status session ---\n" + index_status_raw +
           b"\n--- scrollback session ---\n" + scroll_raw)
    text = strip_ansi(raw)

    virtual = LibvtermReplay(args.rows, args.cols, args.scrollback_lines)
    virtual.feed(scroll_raw)
    visible_text = virtual.visible_text()
    scrollback_lines = extract_scroll_margin_lines(scroll_raw, args.scrollback_lines)
    scrollback_text = "\n".join(scrollback_lines)

    if args.dump:
        with open(args.dump, "wb") as handle:
            handle.write(raw)

    failures = []
    if b"notcurses input polling failed" in raw:
        failures.append("notcurses input polling failed")
    if b"Segmentation fault" in raw:
        failures.append("segmentation fault")
    if b"\x1b_pi:c\x07" in raw:
        failures.append("cursor marker leaked into terminal output")
    if not slash_ok:
        failures.append("slash overlay did not render after typing `/`")
    if b"unknown slash command: /index" in index_raw:
        failures.append("interactive shell rejected /index as an unknown slash command")
    if not index_ok:
        failures.append("/index tree did not render grouped index output")
    if b"unknown slash command: /index" in index_status_raw:
        failures.append("interactive shell rejected /index status as an unknown slash command")
    if not index_status_ok:
        failures.append("/index status did not report running startup index progress")
    if b"\x1b[1;" not in scroll_raw:
        failures.append("no DECSTBM scroll-region write was observed")
    if len(scrollback_lines) == 0:
        failures.append("scroll-margin model did not move any lines into native scrollback")
    if not libvterm_screen_has_fixed_bottom(visible_text):
        failures.append("fixed bottom screen area was not preserved in the visible screen")
    if not contains_any(scrollback_text, ["qt", "type a prompt", "commands", "help"]):
        failures.append("native scrollback model does not contain transcript content")
    tmux_capture = ""
    if not args.skip_tmux:
        tmux_ok, tmux_capture, tmux_error = tmux_scrollback_probe(args)
        if not tmux_ok:
            failures.append(tmux_error)

    if failures:
        sys.stderr.write("tui pty probe failed:\n")
        for failure in failures:
            sys.stderr.write(f"- {failure}\n")
        sys.stderr.write("\nlast decoded output:\n")
        sys.stderr.write(text[-2000:])
        sys.stderr.write("\n\nvirtual visible screen:\n")
        sys.stderr.write(visible_text)
        sys.stderr.write("\n\nvirtual scrollback:\n")
        sys.stderr.write(scrollback_text[-2000:])
        if tmux_capture:
            sys.stderr.write("\n\ntmux capture-pane -S -100 -p:\n")
            sys.stderr.write(tmux_capture[-2000:])
            history, visible = split_tmux_history_visible(tmux_capture, args.rows)
            sys.stderr.write("\n\ntmux history portion:\n")
            sys.stderr.write(history[-2000:])
            sys.stderr.write("\n\ntmux visible portion:\n")
            sys.stderr.write(visible[-2000:])
        sys.stderr.write("\n")
        return 1

    print(
        "ok: slash overlay rendered; DECSTBM observed; "
        f"native scrollback lines={len(scrollback_lines)}"
        + ("; tmux capture-pane verified" if not args.skip_tmux else "")
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
