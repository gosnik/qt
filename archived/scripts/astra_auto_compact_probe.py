#!/usr/bin/env python3
"""Exercise the real Astra extension in a PTY against a localhost-only provider.

No provider credentials or network model are needed. Each case uses an empty
workspace and isolated settings/session directories. Native auto-compaction and
memory are disabled; the startup index can only scan the empty workspace.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from dataclasses import dataclass, field
import http.server
import json
import os
from pathlib import Path
import shutil
import tempfile
import threading
from typing import Any, Callable, Iterator

from tui_pty_probe import TerminalProbe, strip_ansi


SUMMARY = "ASTRA_PROBE_COMPACTION_SUMMARY"
WARNING = "Astra auto-compaction failed; continuing with the existing context."
IDLE = b"\x1b]9;4;0;\x07"


@dataclass(frozen=True)
class Case:
    name: str
    model_id: str
    model_name: str
    totals: tuple[int, ...]
    compact_before: int | None = None  # Zero-based ordinary turn index.
    fail_compaction: bool = False


CASES = (
    Case("id-boundaries", "vendor/pReAsTrA-probe", "Probe Model",
         (199999, 200000, 200001, 1024, 1024), 3),
    Case("name-match", "probe-model", "Vendor preASTRApost Model",
         (200001, 1024, 1024), 1),
    Case("non-astra", "probe-model", "Other Model", (200001, 200001, 1024)),
    Case("failure", "vendor/ASTRA-probe", "Probe Model",
         (200001, 1024, 1024), 1, True),
)


def content_text(content: Any) -> str:
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        return "".join(part.get("text", "") for part in content
                       if isinstance(part, dict) and part.get("type") == "text")
    return ""


def prompt_for(case: Case, index: int) -> str:
    return f"ASTRA_PROMPT_{case.name}_{index}: Preserve My Text, punctuation & 42!"


def reply_for(case: Case, index: int) -> str:
    return f"ASTRA_REPLY_{case.name}_{index}"


def session_entries(directory: Path) -> tuple[Path, list[dict[str, Any]]]:
    files = list(directory.rglob("*.jsonl"))
    if len(files) != 1:
        raise RuntimeError(f"expected one session JSONL, found {files!r}")
    entries = [json.loads(line) for line in files[0].read_text(encoding="utf-8").splitlines()
               if line.strip()]
    return files[0], entries


@dataclass
class ProviderState:
    case: Case
    session_dir: Path
    requests: list[tuple[str, dict[str, Any]]] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)
    turns: int = 0
    compactions: int = 0


def handler_for(state: ProviderState) -> type[http.server.BaseHTTPRequestHandler]:
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args: Any) -> None:
            pass

        def respond(self, status: int, body: bytes, content_type: str) -> None:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            self.wfile.flush()

        def do_POST(self) -> None:  # noqa: N802
            try:
                self.handle_completion()
            except Exception as error:
                state.errors.append(str(error))
                self.respond(400, json.dumps({"error": {"message": str(error)}}).encode(),
                             "application/json")

        def handle_completion(self) -> None:
            if self.path != "/v1/chat/completions":
                raise RuntimeError(f"unexpected provider endpoint: {self.path}")
            payload = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            messages = payload.get("messages", [])
            compact = any(
                message.get("role") == "system" and content_text(message.get("content")).startswith(
                    "You are a context summarization assistant."
                ) for message in messages
            )
            state.requests.append(("compact" if compact else "turn", payload))
            if payload.get("model") != state.case.model_id:
                raise RuntimeError(f"unexpected requested model: {payload.get('model')!r}")
            if compact:
                state.compactions += 1
                if state.case.fail_compaction:
                    # Non-retryable failure: exercise the plugin's catch/notify path.
                    self.respond(400, json.dumps({"error": {
                        "message": "ASTRA_PROBE_FORCED_COMPACTION_FAILURE",
                        "type": "invalid_request_error",
                    }}).encode(), "application/json")
                    return
                text, total = SUMMARY, 32
            else:
                index = state.turns
                state.turns += 1
                text = reply_for(state.case, index)
                total = state.case.totals[index] if index < len(state.case.totals) else 1024
                if state.compactions and not state.case.fail_compaction:
                    # Must be durable BEFORE the continuing turn starts, not just
                    # flushed incidentally by /quit or by a later session save.
                    _, entries = session_entries(state.session_dir)
                    saved = [entry for entry in entries if entry.get("type") == "compaction"]
                    if len(saved) != 1 or saved[0].get("summary") != SUMMARY:
                        raise RuntimeError("input-hook compaction was not persisted before provider turn")

            usage = {"prompt_tokens": total - 8, "completion_tokens": 8, "total_tokens": total}
            if payload.get("stream"):
                chunks = [
                    {"choices": [{"delta": {"role": "assistant", "content": text},
                                  "finish_reason": None}]},
                    {"choices": [{"delta": {}, "finish_reason": "stop"}], "usage": usage},
                ]
                body = ("".join("data: " + json.dumps(chunk) + "\n\n" for chunk in chunks)
                        + "data: [DONE]\n\n").encode()
                self.respond(200, body, "text/event-stream")
            else:
                body = json.dumps({
                    "id": "astra-probe", "model": state.case.model_id,
                    "choices": [{"message": {"role": "assistant", "content": text},
                                 "finish_reason": "stop"}], "usage": usage,
                }).encode()
                self.respond(200, body, "application/json")

    return Handler


@contextmanager
def isolated_environment(workspace: Path, agent_home: Path) -> Iterator[None]:
    values = {
        "QT_CODING_AGENT_DIR": str(agent_home),
        "PI_CODING_AGENT_DIR": str(agent_home),
        "QT_EXPERIMENTAL_NOTCURSES": "0",
        "TERM": "xterm-256color",
        "NO_PROXY": "127.0.0.1,localhost",
        "no_proxy": "127.0.0.1,localhost",
    }
    previous = {key: os.environ.get(key) for key in values}
    cwd = Path.cwd()
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


def require(term: TerminalProbe, predicate: Callable[[bytes, str], bool],
            message: str, offset: int = 0) -> None:
    if not term.read_until(predicate, timeout=15.0, offset=offset):
        raise RuntimeError(message + "\n" + strip_ansi(bytes(term.output[-16000:])))


def quit_terminal(term: TerminalProbe) -> None:
    term.write(b"/quit\r")
    require(term, lambda _raw, _text: term.proc is not None and term.proc.poll() is not None,
            "interactive probe did not exit after /quit")
    if term.proc is None or term.proc.returncode != 0:
        raise RuntimeError("interactive probe exited unsuccessfully")


def submit(term: TerminalProbe, state: ProviderState, index: int) -> None:
    offset = len(term.output)
    term.write((prompt_for(state.case, index) + "\r").encode())
    # The reply alone may be streamed while the harness is still busy. OSC
    # progress-clear is emitted after the turn pump has returned to idle.
    require(term, lambda raw, text: reply_for(state.case, index) in text and IDLE in raw,
            f"{state.case.name}: turn {index} did not finish at idle", offset)
    if state.errors:
        raise RuntimeError(f"mock provider errors: {state.errors!r}")
    expected = ["turn"] * (index + 1)
    boundary = state.case.compact_before
    if boundary is not None and index >= boundary:
        expected.insert(boundary, "compact")
    observed = [kind for kind, _payload in state.requests]
    if observed != expected:
        raise RuntimeError(f"{state.case.name} turn {index}: expected {expected}, got {observed}")
    payload = state.requests[-1][1]
    users = [content_text(message.get("content")) for message in payload["messages"]
             if message.get("role") == "user"]
    prompt = prompt_for(state.case, index)
    if not users or users[-1] != prompt or users.count(prompt) != 1:
        raise RuntimeError(f"{state.case.name}: submitted prompt changed, disappeared, or duplicated")
    has_summary = SUMMARY in json.dumps(payload["messages"])
    expected_summary = boundary is not None and index >= boundary and not state.case.fail_compaction
    if has_summary != expected_summary:
        raise RuntimeError(f"{state.case.name} turn {index}: summary in provider context "
                           f"was {has_summary}, expected {expected_summary}")
    if boundary is not None and index == boundary:
        compaction_payload = state.requests[-2][1]
        if prompt in json.dumps(compaction_payload):
            raise RuntimeError("pending input was included in the context being compacted")
        if state.case.fail_compaction:
            require(term, lambda _raw, text: "warning: " + WARNING in text,
                    "compaction failure did not notify the user with a warning", offset)
            if prompt_for(state.case, 0) not in users:
                raise RuntimeError("failed compaction discarded the existing context")


def verify_session(state: ProviderState, turns: int) -> Path:
    path, entries = session_entries(state.session_dir)
    compactions = [entry for entry in entries if entry.get("type") == "compaction"]
    expected_count = int(state.case.compact_before is not None and not state.case.fail_compaction)
    if len(compactions) != expected_count:
        raise RuntimeError(f"{state.case.name}: expected {expected_count} durable compaction entries, "
                           f"got {len(compactions)}")
    ids = [entry["id"] for entry in entries if "id" in entry]
    if len(ids) != len(set(ids)):
        raise RuntimeError("duplicate durable session entry IDs")
    if compactions:
        entry = compactions[0]
        # Native message rows may omit top-level IDs; the loader generates them.
        # The fresh-process check below verifies the retained-entry reference.
        if (entry.get("summary") != SUMMARY or not entry.get("id")
                or not entry.get("firstKeptEntryId")):
            raise RuntimeError("persisted compaction summary or retained-entry reference was invalid")
    users = [content_text(entry["message"].get("content")) for entry in entries
             if entry.get("type") == "message" and entry.get("message", {}).get("role") == "user"]
    if users != [prompt_for(state.case, index) for index in range(turns)]:
        raise RuntimeError(f"{state.case.name}: durable user prompts were lost, changed, or duplicated")
    totals = [entry["message"].get("usage", {}).get("total_tokens") for entry in entries
              if entry.get("type") == "message"
              and entry.get("message", {}).get("role") == "assistant"]
    expected_totals = list(state.case.totals) + [1024] * (turns - len(state.case.totals))
    if totals != expected_totals:
        raise RuntimeError(f"{state.case.name}: saved usage totals {totals} != {expected_totals}")
    return path


def run_case(binary: Path, extension: Path, runtime: Path, case: Case) -> None:
    agent_home, workspace, sessions = (runtime / name for name in ("agent-home", "workspace", "sessions"))
    for directory in (agent_home, workspace, sessions):
        directory.mkdir(parents=True)
    state = ProviderState(case, sessions)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler_for(state))
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        (agent_home / "settings.json").write_text(json.dumps({
            "quietStartup": True,
            "compaction": {"enabled": False},
            "memory": {"enabled": False},
            "retry": {"enabled": False},
            "terminal": {"showTerminalProgress": True},
        }) + "\n", encoding="utf-8")
        (agent_home / "models.json").write_text(json.dumps({"providers": {"probe": {
            "api": "openai-completions",
            "baseUrl": f"http://127.0.0.1:{server.server_address[1]}/v1",
            "apiKey": "local-probe-key",
            # Keep the native context-window trigger far above our controlled
            # totals as well; only the plugin's absolute 200000 boundary matters.
            "models": [{"id": case.model_id, "name": case.model_name,
                        "contextWindow": 1000000, "maxTokens": 512}],
        }}}) + "\n", encoding="utf-8")
        argv = [str(binary), "--provider", "probe", "--model", case.model_id,
                "--no-memory", "--no-tools", "--no-skills", "--no-prompt-templates",
                "--no-extensions", "--extension", str(extension), "--session-dir", str(sessions)]
        with isolated_environment(workspace, agent_home):
            with TerminalProbe(argv, 32, 160, 15.0) as term:
                try:
                    require(term, lambda _raw, text: case.model_id in text,
                            f"{case.name}: interactive prompt did not become ready")
                    for index in range(len(case.totals)):
                        submit(term, state, index)
                    quit_terminal(term)
                finally:
                    (runtime / "terminal.raw").write_bytes(bytes(term.output))
            path = verify_session(state, len(case.totals))
            if case.compact_before is not None and not case.fail_compaction:
                # Reload the on-disk session in a fresh process, not just the
                # in-memory context, and ensure no duplicate compaction is saved.
                with TerminalProbe(argv + ["--session", str(path)], 32, 160, 15.0) as term:
                    try:
                        require(term, lambda _raw, text: case.model_id in text,
                                f"{case.name}: resumed prompt did not become ready")
                        submit(term, state, len(case.totals))
                        quit_terminal(term)
                    finally:
                        (runtime / "resumed-terminal.raw").write_bytes(bytes(term.output))
                verify_session(state, len(case.totals) + 1)
        print(f"PASS {case.name}: totals={case.totals}; turns={state.turns}; "
              f"compaction requests={state.compactions}; "
              + ("summary persisted once and reloaded" if case.compact_before is not None
                 and not case.fail_compaction else "no durable compaction"), flush=True)
    except Exception as error:
        raise RuntimeError(f"{case.name}: {error}\nprovider sequence: "
                           f"{[kind for kind, _ in state.requests]!r}\n"
                           f"provider errors: {state.errors!r}") from error
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5.0)
        (runtime / "requests.json").write_text(
            json.dumps({"requests": state.requests, "errors": state.errors}, indent=2) + "\n",
            encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default=".pio/build/native/qt")
    parser.add_argument("--keep-runtime", action="store_true", help="retain local probe artifacts")
    parser.add_argument("--case", action="append", choices=[case.name for case in CASES],
                        help="run only this case (repeatable; default: all)")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    binary = Path(args.binary)
    binary = (root / binary).resolve() if not binary.is_absolute() else binary.resolve()
    extension = root / "examples/extensions/astra-auto-compact.as"
    for path in (binary, extension):
        if not path.is_file():
            parser.error(f"required file is missing: {path}")
    # Outside the checkout so parent project settings/instructions and the
    # repository index cannot leak into the otherwise empty test workspace.
    runtime = Path(tempfile.mkdtemp(prefix="qt-astra-compact-probe-"))
    failures = []
    try:
        for case in CASES:
            if args.case and case.name not in args.case:
                continue
            try:
                run_case(binary, extension, runtime / case.name, case)
            except RuntimeError as error:
                failures.append(str(error))
                print(f"FAIL {error}", flush=True)
    finally:
        if args.keep_runtime:
            print(f"Probe artifacts: {runtime}", flush=True)
        else:
            shutil.rmtree(runtime, ignore_errors=True)
    if failures:
        print(f"Astra auto-compaction PTY probe failed: {len(failures)} case(s)")
        return 1
    print("Astra auto-compaction PTY probe passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
