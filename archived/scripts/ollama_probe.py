#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="qt-ollama-probe-") as tmp:
        binary = Path(tmp) / "ollama_probe"
        command = [
            "g++",
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Wpedantic",
            f"-I{root / 'include'}",
            f"-I{root / '.pio/libdeps/native/json/include'}",
            str(root / "scripts/ollama_probe.cpp"),
            str(root / "src/ai/ollama.cpp"),
            str(root / "src/foundation/error.cpp"),
            "-o",
            str(binary),
        ]
        subprocess.run(command, check=True, cwd=root)
        subprocess.run([str(binary)], check=True, cwd=root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
