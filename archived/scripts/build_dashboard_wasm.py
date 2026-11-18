"""Optionally rebuild the qt memory dashboard WASM bundle."""

from __future__ import annotations

import os
import sys
from pathlib import Path

Import("env")  # type: ignore[name-defined]

SCRIPT_DIR = Path(env.subst("$PROJECT_DIR")).resolve() / "scripts"  # type: ignore[name-defined]
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from dashboard_build_common import build_dashboard_dist, dashboard_build_needed


def _flag_enabled(value: str) -> bool:
    return value.strip().lower() not in {"0", "false", "no", "off", ""}


project_dir = Path(env.subst("$PROJECT_DIR")).resolve()  # type: ignore[name-defined]
dist_option = os.environ.get("QT_MEMORY_DASHBOARD_DIST") or env.GetProjectOption("custom_memory_dashboard_dist", "dashboard_wasm/web/dist")  # type: ignore[name-defined]
dist_dir = (project_dir / dist_option).resolve()
build_enabled_raw = os.environ.get("QT_MEMORY_DASHBOARD_BUILD") or env.GetProjectOption("custom_memory_dashboard_build", "false")  # type: ignore[name-defined]
build_enabled = _flag_enabled(build_enabled_raw)
build_type = os.environ.get("QT_MEMORY_DASHBOARD_BUILD_TYPE") or env.GetProjectOption("custom_memory_dashboard_build_type", "Release")  # type: ignore[name-defined]

if build_enabled and dashboard_build_needed(project_dir, dist_dir):
    print(f"Building dashboard WASM bundle ({build_type})")
    build_dashboard_dist(project_dir, build_type)
