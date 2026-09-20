#!/usr/bin/env python3
"""Probe OpenAI model discovery through the CLI and interactive model picker."""

from __future__ import annotations

import argparse
import http.server
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading


def _run_cli(binary: Path, agent_home: Path, *arguments: str) -> str:
    environment = dict(os.environ)
    environment["QT_CODING_AGENT_DIR"] = str(agent_home)
    # OpenRouter discovery is unrelated to this fixture.  Never let an inherited
    # credential make the probe contact a user's account.
    environment.pop("OPENROUTER_API_KEY", None)
    result = subprocess.run(
        [str(binary), *arguments],
        cwd=agent_home,
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=30,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"CLI {' '.join(arguments)} failed with {result.returncode}\n"
            f"stdout:\n{result.stdout[-5000:]}\nstderr:\n{result.stderr[-5000:]}"
        )
    return result.stdout + result.stderr


def run_probe(binary: Path, runtime: Path) -> None:
    from tui_pty_probe import TerminalProbe, strip_ansi

    shutil.rmtree(runtime, ignore_errors=True)
    agent_home = runtime / "agent-home"
    workspace = runtime / "workspace"
    agent_home.mkdir(parents=True)
    workspace.mkdir()

    state = {"requests": 0, "completed": 0, "authorization": []}

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args: object) -> None:
            return

        def do_GET(self) -> None:  # noqa: N802
            if self.path != "/v1/models":
                self.send_error(404)
                return
            state["requests"] += 1
            state["authorization"].append(self.headers.get("Authorization"))
            if state["requests"] == 1:
                # Keep the startup refresh in flight while the selector opens.
                threading.Event().wait(2.0)
            body = json.dumps(
                {
                    "object": "list",
                    "data": [
                        {"id": "gpt-6-astra", "object": "model"},
                        {"id": "gpt-6-catalog-probe", "object": "model"},
                        {"id": "text-embedding-3-small", "object": "model"},
                        {"id": "gpt-6-image", "object": "model"},
                    ],
                }
            ).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            state["completed"] += 1

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    port = server.server_address[1]
    (agent_home / "models.json").write_text(
        json.dumps(
            {
                "providers": {
                    "openai": {
                        "baseUrl": f"http://127.0.0.1:{port}/v1",
                        "apiKey": "probe-key",
                    }
                }
            }
        )
        + "\n",
        encoding="utf-8",
    )

    previous_home = os.environ.get("QT_CODING_AGENT_DIR")
    previous_cwd = Path.cwd()
    previous_openai_key = os.environ.get("OPENAI_API_KEY")
    previous_openrouter_key = os.environ.get("OPENROUTER_API_KEY")
    os.environ["QT_CODING_AGENT_DIR"] = str(agent_home)
    os.environ.pop("OPENAI_API_KEY", None)
    os.environ.pop("OPENROUTER_API_KEY", None)
    os.chdir(workspace)
    try:
        with TerminalProbe(
            [
                str(binary),
                "--provider",
                "openai",
                "--model",
                "gpt-6-astra",
                "--no-session",
            ],
            24,
            120,
            12.0,
        ) as term:
            if not term.read_until(
                lambda _raw, text: "gpt-6-astra" in text, timeout=10.0
            ):
                raise RuntimeError(
                    "interactive footer did not become ready\n"
                    + strip_ansi(bytes(term.output[-8000:]))
                )
            # Wait for the background refresh request to start, but keep its
            # delayed response in flight while opening the selector.
            if not term.read_until(
                lambda _raw, _text: state["requests"] >= 1, timeout=3.0
            ):
                raise RuntimeError("interactive startup did not request /v1/models")
            term.read_until(lambda _raw, _text: False, timeout=0.25)
            if state["completed"] != 0:
                raise RuntimeError("discovery response completed before picker opened")
            term.write(b"/model\r")
            term.read_until(lambda _raw, _text: False, timeout=0.5)
            term.write(b"catalog-probe")
            if not term.read_until(
                lambda _raw, text: "gpt-6-catalog-probe" in text, timeout=8.0
            ):
                raise RuntimeError(
                    "discovered model was not shown by the model picker\n"
                    + strip_ansi(bytes(term.output[-10000:]))
                )
            term.write(b"\x1b")
            term.read_until(lambda _raw, _text: False, timeout=0.25)
            term.write(b"/quit\r")
            term.read_until(
                lambda _raw, _text: term.proc is not None
                and term.proc.poll() is not None,
                timeout=5.0,
            )

        if not state["authorization"] or any(
            value != "Bearer probe-key" for value in state["authorization"]
        ):
            raise RuntimeError(
                "unexpected OpenAI discovery authorization headers: "
                + repr(state["authorization"])
            )

        online = _run_cli(
            binary,
            agent_home,
            "--provider",
            "openai",
            "--list-models",
            "catalog-probe",
            "--no-session",
        )
        if "gpt-6-catalog-probe" not in online:
            raise RuntimeError(
                "online --list-models omitted discovered catalog-probe\n"
                + online[-8000:]
                + f"\nserver state: {state!r}"
            )

        request_count = state["requests"]
        offline = _run_cli(
            binary,
            agent_home,
            "--provider",
            "openai",
            "--offline",
            "--list-models",
            "astra",
            "--no-session",
        )
        if "gpt-6-astra" not in offline:
            raise RuntimeError("offline --list-models omitted configured Astra")
        if state["requests"] != request_count:
            raise RuntimeError("offline --list-models unexpectedly requested /v1/models")
    finally:
        os.chdir(previous_cwd)
        if previous_home is None:
            os.environ.pop("QT_CODING_AGENT_DIR", None)
        else:
            os.environ["QT_CODING_AGENT_DIR"] = previous_home
        if previous_openai_key is None:
            os.environ.pop("OPENAI_API_KEY", None)
        else:
            os.environ["OPENAI_API_KEY"] = previous_openai_key
        if previous_openrouter_key is None:
            os.environ.pop("OPENROUTER_API_KEY", None)
        else:
            os.environ["OPENROUTER_API_KEY"] = previous_openrouter_key
        server.shutdown()
        server.server_close()


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
    runtime = Path(tempfile.mkdtemp(prefix="openai-models-probe-", dir=test_root))
    try:
        run_probe(binary, runtime)
    finally:
        shutil.rmtree(runtime, ignore_errors=True)
    print("OpenAI model discovery probe passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
