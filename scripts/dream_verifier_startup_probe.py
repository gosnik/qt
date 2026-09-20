#!/usr/bin/env python3
"""Exercise the real child startup guard without a network or model download."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=Path(".pio/build/native/qt"))
    binary = parser.parse_args().binary.resolve()
    with tempfile.TemporaryDirectory(prefix="qt-dream-verifier-startup-") as temporary:
        home = Path(temporary)
        (home / "settings.json").write_text(json.dumps({
            "memory": {"enabled": False},
            "llama": {"models": [{"id": "verifier", "name": "Verifier fixture",
                                   "path": str(home / "not-installed.gguf")}]},
        }), encoding="utf-8")
        environment = os.environ.copy()
        for key in ("QT_MEMORY_PREHANDLER", "QT_MEMORY_DREAMING", "QT_MEMORY_DREAM_RESEARCH"):
            environment.pop(key, None)
        environment.update(QT_CODING_AGENT_DIR=str(home), QT_MEMORY_DREAM_VERIFY="1",
                           PI_OFFLINE="1")
        base = [str(binary), "--mode", "json", "--no-session", "--no-extensions",
                "--no-context-files", "--no-skills", "--no-memory"]
        cases = [
            ("remote provider", ["--no-tools", "--provider", "openai", "--model", "gpt-4o"]),
            ("tools enabled", ["--provider", "llama", "--model", "verifier"]),
        ]
        for name, arguments in cases:
            result = subprocess.run(base + arguments, input="Verify the supplied evidence.",
                                    text=True, capture_output=True, cwd=home,
                                    env=environment, timeout=20)
            if result.returncode == 0 or "Dream verification requires" not in result.stdout + result.stderr:
                raise RuntimeError(name + " did not fail at the isolated verifier guard: " +
                                   (result.stdout + result.stderr)[-1500:])
        result = subprocess.run(base + ["--no-tools", "--provider", "llama", "--model", "verifier"],
                                input="Verify the supplied evidence.", text=True,
                                capture_output=True, cwd=home, env=environment, timeout=20)
        if result.returncode == 0 or "Dream verification requires" in result.stdout + result.stderr:
            raise RuntimeError("missing local weights did not fail after an accepted verifier startup")
    print("Dream verifier startup probe passed: remote/tools refused, missing weights fail closed")


if __name__ == "__main__":
    main()
