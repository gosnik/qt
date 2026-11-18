#!/usr/bin/env python3

import argparse
import pathlib
import subprocess


OBJECTS = (
    "src/app/runtime/runtime_host.o",
    "src/harness/agent_harness.o",
    "src/harness/session_manager.o",
    "src/harness/session.o",
    "src/harness/session_graph.o",
    "src/harness/session_resume.o",
    "src/harness/session_repository.o",
    "src/harness/session_migration.o",
    "src/harness/session_lifecycle.o",
    "src/harness/compaction.o",
    "src/agent/agent.o",
    "src/agent/models.o",
    "src/ai/models.o",
    "src/ai/provider.o",
    "src/ai/provider_transport.o",
    "src/ai/images.o",
    "src/ai/stream_utils.o",
    "src/ai/auth_storage.o",
    "src/ai/model_catalog.o",
    "src/tools/models.o",
    "src/tools/tool_validation.o",
    "src/tools/registry.o",
    "src/foundation/error.o",
    "src/foundation/process.o",
    "src/foundation/filesystem.o",
    "src/foundation/pi_user_agent.o",
    "src/foundation/uuid.o",
    "src/foundation/clock.o",
)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify and benchmark AgentHarness configuration caching"
    )
    parser.add_argument("--turns", type=int, default=250)
    parser.add_argument(
        "--mode", choices=("cached", "rebuild", "both"), default="both"
    )
    args = parser.parse_args()
    if args.turns <= 0:
        parser.error("--turns must be positive")

    root = pathlib.Path(__file__).resolve().parents[1]
    pio_build = root / ".pio" / "build" / "native"
    missing = [path for path in OBJECTS if not (pio_build / path).is_file()]
    if missing:
        raise RuntimeError(
            "native objects are missing; run `pio run -e native` first: "
            + ", ".join(missing)
        )

    build_dir = root / ".pio" / "test" / "agent-harness-cache"
    build_dir.mkdir(parents=True, exist_ok=True)
    probe_object = build_dir / "agent_harness_cache_probe.o"
    probe_binary = build_dir / "agent_harness_cache_probe"
    compile_command = [
        "g++",
        "-std=c++17",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-pthread",
        "-Iinclude",
        "-I.pio/libdeps/native/json/include",
        "-c",
        "scripts/agent_harness_cache_probe.cpp",
        "-o",
        str(probe_object),
    ]
    subprocess.run(compile_command, cwd=root, check=True)

    link_command = ["g++", "-pthread", str(probe_object)]
    link_command.extend(str(pio_build / path) for path in OBJECTS)
    link_command.extend(
        ["-lssl", "-lcrypto", "-lz", "-ldl", "-lrt", "-o", str(probe_binary)]
    )
    subprocess.run(link_command, cwd=root, check=True)

    modes = ("cached", "rebuild") if args.mode == "both" else (args.mode,)
    for mode in modes:
        subprocess.run(
            [str(probe_binary), str(args.turns), mode], cwd=root, check=True
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
