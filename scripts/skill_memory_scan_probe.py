#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="qt-skill-memory-probe-") as tmp:
        temporary = Path(tmp)
        binary = temporary / "skill_memory_scan_probe"
        command = [
            "g++", "-std=c++17", "-Wall", "-Wextra", "-Wpedantic",
            f"-I{root / 'include'}",
            f"-I{root / '.pio/libdeps/native/json/include'}",
            str(root / "scripts/skill_memory_scan_probe.cpp"),
            str(root / "src/memory/skill_import.cpp"),
            str(root / "src/memory/memory_service.cpp"),
            str(root / "src/memory/governance_service.cpp"),
            str(root / "src/memory/models.cpp"),
            str(root / "src/memory/scope.cpp"),
            str(root / "src/memory/storage/sqlite_memory_database.cpp"),
            str(root / "src/memory/storage/sqlite_retrieval_links.cpp"),
            str(root / "src/foundation/error.cpp"),
            str(root / "src/foundation/filesystem.cpp"),
            str(root / "src/foundation/hash.cpp"),
            str(root / "src/foundation/uuid.cpp"),
            "-lsqlite3", "-pthread", "-o", str(binary),
        ]
        subprocess.run(command, check=True, cwd=root)
        subprocess.run([str(binary), str(temporary / "run")], check=True, cwd=root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
