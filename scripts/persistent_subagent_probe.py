#!/usr/bin/env python3
"""Build and run the persistent coordinator probe.

The C++ probe owns the assertions; this wrapper supplies a deterministic child
and keeps all generated files below .pio/test.
"""
from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys


FAKE_CHILD = r'''#!/usr/bin/env python3
import json, os, subprocess, sys, time

def arg(name, default=""):
    try: return sys.argv[sys.argv.index(name) + 1]
    except (ValueError, IndexError): return default

trace = arg("--trace", os.environ.get("QT_PROBE_TRACE", ""))
task = arg("--task", "")
stdin_task = sys.stdin.read().strip()
if stdin_task:
    task = stdin_task
def log(value):
    if trace:
        with open(trace, "a", encoding="utf-8") as out:
            out.write(value + "\n")
            out.flush()

log("start " + task)
if "descendant" in task:
    child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
    if trace: log("descendant " + str(child.pid))
if "sleep=" in task:
    value = task.split("sleep=", 1)[1].split()[0]
    time.sleep(float(value) / 1000.0)
if "malformed" in task:
    print("this is deliberately not json", flush=True)
else:
    print(json.dumps({"type":"agent_end", "messages":[
        {"role":"assistant", "content":[{"type":"text", "text":"reply(" + task + ")"},
          {"type":"toolCall", "id":"tc-1", "name":"probe", "arguments":{"task":task}}]}
    ]}), flush=True)
log("end " + task)
'''


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    out = root / ".pio" / "test" / "persistent-subagent-probe"
    out.mkdir(parents=True, exist_ok=True)
    child = out / "fake_child.py"
    child.write_text(FAKE_CHILD, encoding="utf-8")
    child.chmod(0o700)
    binary = out / "persistent_subagent_probe"
    pioenv = __import__("os").environ.get("PIOENV", "native")
    command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Wpedantic",
               "-pthread", "-Iinclude", f"-I.pio/libdeps/{pioenv}/json/include",
               "scripts/persistent_subagent_probe.cpp",
               "src/app/runtime/subagent_coordinator.cpp",
               "src/app/runtime/isolated_agent_runner.cpp",
               "src/ai/models.cpp", "src/foundation/uuid.cpp",
               "src/foundation/error.cpp", "src/foundation/filesystem.cpp",
               "src/foundation/process.cpp", "-o", str(binary)]
    subprocess.run(command, cwd=root, check=True)
    subprocess.run([str(binary), str(child), str(out / "runtime")], cwd=root, check=True)
    print("persistent subagent probe passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
