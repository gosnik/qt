#!/usr/bin/env python3
import argparse
import json
import os
import pathlib
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed


DEFAULT_CHECKS = None


def split_file_list(value):
    if not value:
        return []
    return [item for item in re.split(r"[\s,]+", value) if item]


def discover_files(root, explicit_files):
    if explicit_files:
        return [root / item for item in explicit_files]
    return sorted((root / "src").rglob("*.cpp"))


def build_command(clang_tidy, path, flags, checks):
    command = [clang_tidy, "--quiet"]
    if checks:
        command.append("-checks=" + checks)
    command.append(str(path))
    command.append("--")
    if path.suffix in {".h", ".hh", ".hpp", ".hxx"}:
        command.extend(["-x", "c++"])
    command.extend(flags)
    return command


def run_one(command):
    completed = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    return completed.returncode, completed.stdout, completed.stderr


def interesting_lines(output):
    lines = []
    for line in output.splitlines():
        if ": warning:" in line or ": error:" in line:
            lines.append(line)
    return lines


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--env-name", default="native")
    parser.add_argument("--flags-json", required=True)
    args = parser.parse_args()

    root = pathlib.Path.cwd()
    clang_tidy = os.environ.get("CLANG_TIDY", "clang-tidy")
    checks = os.environ.get("CLANG_TIDY_CHECKS", DEFAULT_CHECKS)
    explicit_files = split_file_list(os.environ.get("CLANG_TIDY_FILES", ""))
    jobs = int(os.environ.get("CLANG_TIDY_JOBS", str(os.cpu_count() or 1)))
    limit = int(os.environ.get("CLANG_TIDY_LIMIT", "0"))
    report_path = pathlib.Path(
        os.environ.get(
            "CLANG_TIDY_REPORT",
            str(root / ".pio" / "build" / args.env_name / "clang-tidy-report.txt"),
        )
    )

    with open(args.flags_json, "r", encoding="utf-8") as handle:
        flags = json.load(handle)

    files = discover_files(root, explicit_files)
    if limit > 0:
        files = files[:limit]
    if not files:
        print("clang-tidy: no files selected", file=sys.stderr)
        return 1

    report_path.parent.mkdir(parents=True, exist_ok=True)
    commands = [build_command(clang_tidy, path, flags, checks) for path in files]

    findings = []
    failures = 0
    with ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        futures = {pool.submit(run_one, command): command for command in commands}
        for future in as_completed(futures):
            command = futures[future]
            code, stdout, stderr = future.result()
            output = stdout + stderr
            lines = interesting_lines(output)
            if code != 0 and not lines:
                failures += 1
                lines = ["clang-tidy failed without diagnostics: " + " ".join(command)]
            if lines:
                findings.append((command[-(len(flags) + 2)], lines))

    findings.sort(key=lambda item: item[0])
    with open(report_path, "w", encoding="utf-8") as handle:
        handle.write("clang-tidy report\n")
        handle.write("=================\n")
        handle.write(f"environment: {args.env_name}\n")
        handle.write(f"files checked: {len(files)}\n")
        handle.write(f"files with findings: {len(findings)}\n")
        handle.write(f"failed invocations: {failures}\n\n")
        for file_path, lines in findings:
            handle.write(str(file_path) + "\n")
            for line in lines:
                handle.write("  " + line + "\n")
            handle.write("\n")

    print(f"clang-tidy checked {len(files)} files")
    print(f"clang-tidy findings: {len(findings)} files")
    print(f"clang-tidy report: {report_path}")
    return 2 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
