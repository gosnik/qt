import json
import os
import subprocess
from collections.abc import Iterable

Import("env")


def _as_list(value):
    if value is None:
        return []
    if isinstance(value, (str, bytes)):
        return [value]
    if isinstance(value, Iterable):
        return list(value)
    return [value]


def _define_to_flag(define):
    if isinstance(define, (list, tuple)):
        if len(define) == 1:
            return "-D" + str(define[0])
        if len(define) >= 2:
            return "-D" + str(define[0]) + "=" + str(define[1])
    return "-D" + str(define)


def _clang_tidy_action(source, target, env):
    del source, target
    build_dir = env.subst("$BUILD_DIR")
    os.makedirs(build_dir, exist_ok=True)

    flags = []
    seen = set()
    def add_flag(flag):
        if flag not in seen:
            seen.add(flag)
            flags.append(flag)

    for flag in _as_list(env.get("CXXFLAGS")):
        text = env.subst(str(flag))
        if text.startswith(("-std=", "-D", "-I")) or text == "-pthread":
            add_flag(text)

    for include_path in _as_list(env.get("CPPPATH")):
        add_flag("-I" + env.subst(str(include_path)))

    for define in _as_list(env.get("CPPDEFINES")):
        add_flag(_define_to_flag(define))

    if not any(flag.startswith("-std=") for flag in flags):
        flags.insert(0, "-std=c++17")

    flags_path = os.path.join(build_dir, "clang-tidy-flags.json")
    with open(flags_path, "w", encoding="utf-8") as handle:
        json.dump(flags, handle, indent=2)

    command = [
        env.subst("$PYTHONEXE"),
        "scripts/run_clang_tidy.py",
        "--env-name",
        env.subst("$PIOENV"),
        "--flags-json",
        flags_path,
    ]
    return subprocess.call(command)


env.AddCustomTarget(
    "clang-tidy",
    None,
    _clang_tidy_action,
    title="clang-tidy",
    description="Run clang-tidy over owned C++ sources and write a findings report",
)
