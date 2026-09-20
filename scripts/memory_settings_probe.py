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
        raise RuntimeError("qt exited before accepting memory settings input")


def inspect_settings(binary: pathlib.Path, root: pathlib.Path,
                     environment: dict[str, str]) -> dict:
    inspected = subprocess.run(
        [str(binary), "--inspect-settings", "--json"],
        cwd=root,
        env=environment,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    )
    return json.loads(inspected.stdout)["settings"]["memory"]


def inspect_result(binary: pathlib.Path, root: pathlib.Path,
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


def wait_for_text(term, offset: int, *needles: str) -> None:
    if not term.read_until(
        lambda raw, text: all(needle in text for needle in needles),
        timeout=6.0,
        offset=offset,
    ):
        raise RuntimeError("terminal did not render: " + ", ".join(needles))


def run_probe(root: pathlib.Path, binary: pathlib.Path,
              runtime_dir: pathlib.Path) -> None:
    sys.path.insert(0, str(root / "scripts"))
    from tui_pty_probe import TerminalProbe

    agent_home = runtime_dir / "agent-home"
    workspace = runtime_dir / "workspace"
    shutil.rmtree(runtime_dir, ignore_errors=True)
    agent_home.mkdir(parents=True)
    workspace.mkdir(parents=True)
    seeded_memory = {
        "enabled": False,
        "automaticRecall": True,
        "automaticCapture": True,
        "retrieval": {
            "graphEnabled": True,
            "denseEnabled": True,
            "codeEnabled": True,
            "embeddingModel": "probe-embed",
            "queryPrefix": "query: ",
            "documentPrefix": "document: ",
            "maxDepth": 3,
            "maxSeeds": 24,
            "maxEdges": 300,
            "maxGraphResults": 70,
            "maxFanout": 40,
            "queryCacheEntries": 80,
        },
        "recallPolicy": "architecture",
        "contextTokenBudget": 777,
        "candidateLimit": 17,
        "prehandler": {
            "model": "probe-model",
            "thinking": "minimal",
            "timeoutMs": 4321,
            "failOpen": False,
            "maxModelInputTokens": 6789,
            "futurePrehandlerKey": "preserve-me",
        },
        "capture": {
            "postProcess": "session_end",
            "defaultStatus": "candidate",
            "maxEvidenceChars": 23456,
            "futureCaptureKey": "preserve-me",
        },
        "dreaming": {
            "enabled": True,
            "trigger": "candidate_threshold",
            "intervalSeconds": 7200,
            "idleSeconds": 900,
            "candidateThreshold": 4,
            "model": "dream-probe-model",
            "verifierModel": "llama/verifier-probe-model",
            "timeoutMs": 120000,
            "maxModelInputTokens": 4096,
            "maxCandidates": 6,
            "researchPolicy": "manual",
            "promotionPolicy": "manual",
            "futureDreamingKey": "preserve-me",
        },
        "futureMemoryKey": {"preserve": True},
    }
    settings_path = agent_home / "settings.json"
    settings_path.write_text(
        json.dumps({
            "memory": seeded_memory,
            "llama": {"models": [
                {"id": "pty-a", "name": "PTY model A",
                 "path": str(agent_home / "pty-a-missing.gguf")},
                {"id": "pty-b", "name": "PTY model B",
                 "path": str(agent_home / "pty-b-missing.gguf")},
            ]},
            "probeSentinel": {"keep": 7},
        })
        + "\n",
        encoding="utf-8",
    )

    environment = os.environ.copy()
    environment["QT_CODING_AGENT_DIR"] = str(agent_home)
    defaults_home = runtime_dir / "defaults-home"
    defaults_home.mkdir()
    defaults_environment = environment.copy()
    defaults_environment["QT_CODING_AGENT_DIR"] = str(defaults_home)
    defaults = inspect_settings(binary, workspace, defaults_environment)
    if (defaults["dreaming"]["promotion_policy"] != "review_only" or
            defaults["dreaming"]["verifier_model"] is not None):
        raise RuntimeError("Dream verifier defaults were not review-only and unset")
    prior_home = os.environ.get("QT_CODING_AGENT_DIR")
    prior_cwd = pathlib.Path.cwd()
    os.environ["QT_CODING_AGENT_DIR"] = str(agent_home)
    os.chdir(workspace)
    try:
        with TerminalProbe([str(binary)], 18, 110, 10.0) as term:
            wait_for_terminal(term)
            offset = len(term.output)
            term.write(b"/memory\r")
            wait_for_text(term, offset, "Configure built-in memory", "status")

            offset = len(term.output)
            term.write(b"\x1b[B\r")
            wait_for_text(term, offset, "Enabled", "Automatic recall",
                          "Automatic capture", "Recall policy")

            term.write(b"\r\x1b[B\r\x1b[B\r")
            term.read_until(lambda raw, text: False, timeout=0.75)
            term.write(b"\x1b[B\x1b[B\r")
            term.read_until(lambda raw, text: False, timeout=0.25)
            term.write(b"\x15" + b"888\r")
            term.read_until(lambda raw, text: False, timeout=0.25)
            term.write(b"\x1b")
            term.read_until(lambda raw, text: False, timeout=0.25)
            term.write(b"\x1b")
            term.read_until(lambda raw, text: False, timeout=0.5)
            term.write(b"/quit\r")
            if not term.read_until(
                lambda raw, text: term.proc is not None
                and term.proc.poll() is not None,
                timeout=4.0,
            ):
                raise RuntimeError("qt did not exit after memory settings edit")

        persisted = json.loads(settings_path.read_text(encoding="utf-8"))
        memory = persisted["memory"]
        if memory["enabled"] is not True:
            raise RuntimeError("memory Enabled toggle was not persisted")
        if memory["automaticRecall"] is not False:
            raise RuntimeError("Automatic recall toggle was not persisted")
        if memory["automaticCapture"] is not False:
            raise RuntimeError("Automatic capture toggle was not persisted")
        if memory.get("retrieval") != seeded_memory["retrieval"]:
            raise RuntimeError("memory edit changed retrieval settings")
        if memory["contextTokenBudget"] != 888:
            raise RuntimeError("editable context token budget was not persisted")
        if memory["prehandler"]["model"] != "probe-model":
            raise RuntimeError("unmodified pre-handler model was unexpectedly changed")
        if persisted.get("probeSentinel") != {"keep": 7}:
            raise RuntimeError("memory edit overwrote unrelated global settings")
        if memory.get("futureMemoryKey") != {"preserve": True}:
            raise RuntimeError("memory edit discarded an unknown memory key")
        if memory["prehandler"].get("futurePrehandlerKey") != "preserve-me":
            raise RuntimeError("memory edit discarded an unknown pre-handler key")
        if memory["capture"].get("futureCaptureKey") != "preserve-me":
            raise RuntimeError("memory edit discarded an unknown capture key")
        if memory["dreaming"].get("futureDreamingKey") != "preserve-me":
            raise RuntimeError("memory edit discarded an unknown dreaming key")
        expected_dreaming = seeded_memory["dreaming"]
        for key, expected in expected_dreaming.items():
            if key == "futureDreamingKey":
                continue
            if memory["dreaming"].get(key) != expected:
                raise RuntimeError(f"memory edit changed dreaming setting {key}")

        # Reopen the list so row positions are deterministic. Model selection
        # uses typed provider/id filtering; Escape must discard a pending choice.
        with TerminalProbe([str(binary)], 18, 110, 10.0) as editors:
            wait_for_terminal(editors)
            offset = len(editors.output)
            editors.write(b"/memory settings\r")
            wait_for_text(editors, offset, "Pre-handler model")

            editors.write(b"\x1b[B" * 6 + b"\r")
            editors.write(b"\x15pty-a\r")
            editors.read_until(lambda raw, text: False, timeout=0.25)
            if json.loads(settings_path.read_text(encoding="utf-8"))["memory"]["prehandler"]["model"] != "llama/pty-a":
                raise RuntimeError("Pre-handler model list selection was not persisted")

            editors.write(b"\x1b[B" * 13 + b"\r")
            editors.write(b"\x15pty-b\r")
            editors.read_until(lambda raw, text: False, timeout=0.25)
            if json.loads(settings_path.read_text(encoding="utf-8"))["memory"]["dreaming"]["model"] != "llama/pty-b":
                raise RuntimeError("Dreaming model list selection was not persisted")

            editors.write(b"\x1b[B\r")
            editors.write(b"\x15pty-a\r")
            editors.read_until(lambda raw, text: False, timeout=0.25)
            if json.loads(settings_path.read_text(encoding="utf-8"))["memory"]["dreaming"]["verifierModel"] != "llama/pty-a":
                raise RuntimeError("Dreaming verifier model list selection was not persisted")

            # Reopen the verifier selector, filter a different local model,
            # then cancel rather than committing it.
            editors.write(b"\r\x15pty-b\x1b")
            editors.read_until(lambda raw, text: False, timeout=0.25)
            if json.loads(settings_path.read_text(encoding="utf-8"))["memory"]["dreaming"]["verifierModel"] != "llama/pty-a":
                raise RuntimeError("Dreaming verifier Escape changed the selected model")

            # A remote-only filter must not create a selectable verifier value.
            editors.write(b"\r\x15openai/remote\r\x1b")
            editors.read_until(lambda raw, text: False, timeout=0.25)
            if json.loads(settings_path.read_text(encoding="utf-8"))["memory"]["dreaming"]["verifierModel"] != "llama/pty-a":
                raise RuntimeError("Dreaming verifier accepted a remote model filter")

            # Each picker exposes its explicit reset choice.
            editors.write(b"\x1b[A\r\x15auto\r")
            editors.read_until(lambda raw, text: False, timeout=0.25)
            editors.write(b"\x1b[A" * 13 + b"\r\x15default\r")
            editors.read_until(lambda raw, text: False, timeout=0.25)
            editors.write(b"\x1b[B" * 14 + b"\r\x15unset\r")
            editors.read_until(lambda raw, text: False, timeout=0.25)
            editors.write(b"\x1b")
            editors.read_until(lambda raw, text: False, timeout=0.25)
            editors.write(b"/quit\r")
            if not editors.read_until(
                lambda raw, text: editors.proc is not None
                and editors.proc.poll() is not None,
                timeout=4.0,
            ):
                raise RuntimeError("qt did not exit after model-editor probe")

        persisted = json.loads(settings_path.read_text(encoding="utf-8"))
        memory = persisted["memory"]
        if memory["prehandler"]["model"] is not None:
            raise RuntimeError("Pre-handler model default reset was not persisted")
        if memory["dreaming"]["model"] is not None:
            raise RuntimeError("Dreaming model automatic reset was not persisted")
        if memory["dreaming"]["verifierModel"] is not None:
            raise RuntimeError("Dreaming verifier unset reset was not persisted")

        inspected = inspect_settings(binary, root, environment)
        if not inspected["enabled"]:
            raise RuntimeError("fresh settings inspection did not enable memory")
        if inspected["automatic_recall"] or inspected["automatic_capture"]:
            raise RuntimeError("fresh settings inspection lost automatic toggles")
        if inspected["recall_policy"] != "architecture":
            raise RuntimeError("advanced recall policy was not preserved")
        if inspected["context_token_budget"] != 888:
            raise RuntimeError("context token budget was not preserved")
        if inspected["prehandler"]["model"] is not None:
            raise RuntimeError("pre-handler model was not preserved")
        if inspected["capture"]["max_evidence_chars"] != 23456:
            raise RuntimeError("capture evidence limit was not preserved")
        expected_inspected_dreaming = {
            "enabled": True,
            "trigger": "candidate_threshold",
            "interval_seconds": 7200,
            "idle_seconds": 900,
            "candidate_threshold": 4,
            "model": None,
            "verifier_model": None,
            "timeout_ms": 120000,
            "max_model_input_tokens": 4096,
            "max_candidates": 6,
            "research_policy": "manual",
            "promotion_policy": "manual",
        }
        if inspected["dreaming"] != expected_inspected_dreaming:
            raise RuntimeError("fresh settings inspection lost dreaming settings")

        with TerminalProbe([str(binary)], 18, 110, 10.0) as restarted:
            wait_for_terminal(restarted)
            offset = len(restarted.output)
            restarted.write(b"/memory status\r")
            wait_for_text(
                restarted,
                offset,
                "enabled: true",
                "configured: true",
                "statistics:",
                "stored memories:",
            )

            offset = len(restarted.output)
            restarted.write(b"/memory settings\r")
            wait_for_text(restarted, offset, "Enabled", "Automatic recall")
            restarted.write(b"\r\x1b")
            restarted.read_until(lambda raw, text: False, timeout=0.5)
            restarted.write(b"/quit\r")
            if not restarted.read_until(
                lambda raw, text: restarted.proc is not None
                and restarted.proc.poll() is not None,
                timeout=4.0,
            ):
                raise RuntimeError("qt did not exit after disabling memory")

        disabled = inspect_settings(binary, root, environment)
        if disabled["enabled"]:
            raise RuntimeError("memory disable toggle did not survive restart")

        project_settings_dir = workspace / ".pi"
        project_settings_dir.mkdir()
        project_settings = project_settings_dir / "settings.json"
        project_settings.write_text(json.dumps({"memory": {"dreaming": {
            "promotionPolicy": "auto_validate"}}}) + "\n", encoding="utf-8")
        broadened_policy = inspect_result(binary, workspace, environment)
        if (broadened_policy.returncode == 0 or
                "memory.dreaming.promotionPolicy" not in
                broadened_policy.stdout + broadened_policy.stderr):
            raise RuntimeError("project settings elevated Dream auto-validation policy")

        settings_path.write_text(json.dumps({"memory": {"dreaming": {
            "promotionPolicy": "auto_validate",
            "verifierModel": "llama/global-verifier"}}}) + "\n",
            encoding="utf-8")
        project_settings.write_text(json.dumps({"memory": {"dreaming": {
            "verifierModel": "llama/project-verifier"}}}) + "\n",
            encoding="utf-8")
        switched_verifier = inspect_result(binary, workspace, environment)
        if (switched_verifier.returncode == 0 or
                "memory.dreaming.verifierModel" not in
                switched_verifier.stdout + switched_verifier.stderr):
            raise RuntimeError("project settings switched the global Dream verifier")
    finally:
        os.chdir(prior_cwd)
        if prior_home is None:
            os.environ.pop("QT_CODING_AGENT_DIR", None)
        else:
            os.environ["QT_CODING_AGENT_DIR"] = prior_home


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify interactive memory settings persistence"
    )
    parser.add_argument("--binary", default=".pio/build/native/qt")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    binary = pathlib.Path(args.binary)
    if not binary.is_absolute():
        binary = (root / binary).resolve()
    if not binary.is_file():
        raise RuntimeError("native qt binary is missing; run `pio run -e native`")
    run_probe(root, binary, root / ".pio" / "test" / "memory-settings-probe")
    print("memory settings probe passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
