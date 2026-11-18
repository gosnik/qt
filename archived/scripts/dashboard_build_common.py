"""Shared helpers for qtmem dashboard WASM build integration."""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path


def tree_latest_mtime(path: Path) -> float:
    if not path.exists():
        return 0.0
    if path.is_file():
        return path.stat().st_mtime
    latest = path.stat().st_mtime
    for child in path.rglob("*"):
        if child.is_file():
            latest = max(latest, child.stat().st_mtime)
    return latest


def dashboard_build_needed(project_dir: Path, dist_dir: Path) -> bool:
    required_outputs = ("index.js", "index.wasm")
    if any(not (dist_dir / name).exists() for name in required_outputs):
        return True

    dist_latest = max((dist_dir / name).stat().st_mtime for name in required_outputs)
    source_roots = (
        project_dir / "dashboard_wasm" / "CMakeLists.txt",
        project_dir / "dashboard_wasm" / "src",
        project_dir / "dashboard_wasm" / "scripts",
        project_dir / "dashboard_wasm" / "web",
        project_dir / "dashboard_wasm" / "third_party",
    )
    source_latest = max(tree_latest_mtime(root) for root in source_roots)
    return source_latest > dist_latest


def build_dashboard_dist(project_dir: Path, build_type: str) -> None:
    emsdk_dir = os.environ.get("EMSDK_DIR", "").strip()
    emsdk_environment = Path(emsdk_dir) / "emsdk_env.sh" if emsdk_dir else None
    if shutil.which("emcmake") is None and not (
        emsdk_environment is not None and emsdk_environment.is_file()
    ):
        raise RuntimeError(
            "emcmake is required to rebuild the memory dashboard WASM bundle. "
            "Install Emscripten, set EMSDK_DIR, or leave "
            "custom_memory_dashboard_build disabled."
        )

    script_path = project_dir / "dashboard_wasm" / "scripts" / "build.sh"
    if not script_path.exists():
        raise RuntimeError(f"dashboard build script not found: {script_path}")

    env = os.environ.copy()
    env.setdefault("QT_MEMORY_DASHBOARD_BUILD_TYPE", build_type)
    subprocess.run(
        ["bash", str(script_path), f"-DCMAKE_BUILD_TYPE={build_type}"],
        check=True,
        cwd=str(project_dir),
        env=env,
    )
