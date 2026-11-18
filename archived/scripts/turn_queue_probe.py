#!/usr/bin/env python3
"""Verify bounded turn-operation admission and FIFO delivery."""
import pathlib
import subprocess

root = pathlib.Path(__file__).resolve().parents[1]
out = root / ".pio" / "test" / "turn-queue"
out.mkdir(parents=True, exist_ok=True)
binary = out / "probe"
subprocess.run(["g++", "-std=c++17", "-O2", "-pthread", "-Iinclude",
                "-I.pio/libdeps/native/json/include", "scripts/turn_queue_probe.cpp",
                "src/app/interactive/interactive_turn_pump.cpp", "src/foundation/error.cpp",
                "-o", str(binary)], cwd=root, check=True)
subprocess.run([str(binary)], cwd=root, check=True, timeout=10)
