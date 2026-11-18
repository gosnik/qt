#!/usr/bin/env python3
"""Compile and run the focused governed memory-review contract probe."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / ".pio" / "test" / "memory-review-probe"
BINARY = BUILD / "memory_review_probe"
CONTRACTS = ROOT / "scripts" / "fixtures" / "memory_review_contracts.json"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--require-target-contract",
        action="store_true",
        help="fail until the typed backend supplies every Phase 1 lifecycle invariant",
    )
    return parser.parse_args()


def compile_probe() -> None:
    pioenv = os.environ.get("PIOENV", "native")
    cxx = os.environ.get("CXX", "g++")
    shutil.rmtree(BUILD, ignore_errors=True)
    BUILD.mkdir(parents=True)
    command = [
        cxx,
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-pthread",
        "-Iinclude",
        f"-I.pio/libdeps/{pioenv}/json/include",
        "scripts/memory_review_probe.cpp",
        "src/memory/models.cpp",
        "src/memory/review.cpp",
        "src/memory/review_json.cpp",
        "src/memory/review_service.cpp",
        "src/memory/scope.cpp",
        "src/memory/storage/sqlite_memory_database.cpp",
        "src/memory/storage/sqlite_retrieval_links.cpp",
        "src/memory/memory_service.cpp",
        "src/memory/governance_service.cpp",
        "src/memory/recall_service.cpp",
        "src/foundation/error.cpp",
        "src/foundation/filesystem.cpp",
        "src/foundation/git_url.cpp",
        "src/foundation/hash.cpp",
        "-lsqlite3",
        "-o",
        str(BINARY),
    ]
    subprocess.run(command, cwd=ROOT, check=True)


def validate_report(report: dict[str, object], require_target: bool) -> None:
    expected_kinds = {
        "candidate_memory",
        "dream_artifact",
        "dream_research_successor",
        "semantic_consolidation_candidate",
        "compression_candidate",
        "conflict",
        "procedure_revision",
        "failed_procedure",
        "staleness",
        "provenance_repair",
    }
    actual_kinds = set(report.get("fixture_kinds", []))
    if actual_kinds != expected_kinds:
        raise RuntimeError(f"review kind fixtures differ: {actual_kinds!r}")
    for key in (
        "scope_isolation",
        "malformed_legacy_payload_readable",
        "missing_subject_readable",
        "target_contract_examples_valid",
    ):
        if report.get(key) is not True:
            raise RuntimeError(f"review characterization invariant failed: {key}")
    if not require_target:
        return
    gaps: list[str] = []
    if report.get("terminal_overwrite_allowed") is not False:
        gaps.append("pending-only terminal compare-and-set")
    for key, label in (
        ("concurrent_single_winner", "single-winner concurrent decisions"),
        ("scheduled_deferral_available", "scheduled deferral/resurface"),
        ("reopen_available", "audited reopen"),
        ("typed_history_available", "typed review history"),
        ("preview_idempotency_available", "preview/idempotency tokens"),
        ("atomic_idempotent_replay", "idempotent atomic domain mutations"),
        ("atomic_rollback", "atomic domain rollback"),
        ("audit_completeness",
         "complete governed review audit identity and before/after effects"),
        ("queue_filters_complete",
         "source, Dreaming cycle, stale, and conflict queue filters"),
        ("empty_single_mixed_pagination_complete",
         "empty, single, mixed, and paginated queues"),
        ("stable_score_ordering_complete",
         "pure stable scoring and ID tie-breaking"),
        ("all_candidate_kinds_trust_scopes_complete",
         "candidate kinds, trust states, and scope levels"),
        ("critical_conflict_recall_gated",
         "critical-conflict recall guidance gating"),
        ("durable_ranking_signals_complete",
         "durable criticality, trust, usage, context, and procedure signals"),
        ("review_statistics_age_risk_duration_complete",
         "age, risk, scope, and duration statistics"),
        ("review_statistics_outcomes_complete", "typed outcome statistics"),
        ("review_statistics_batches_complete", "batch outcome statistics"),
        ("export_import_redaction_integrity_restart_complete",
         "export/import, redaction, integrity, and restart durability"),
        ("retention_preserves_governance_complete",
         "retention-safe review and action history"),
        ("secret_surface_redaction_complete",
         "secret-safe previews, receipts, history, audit metadata, and errors"),
    ):
        if report.get(key) is not True:
            gaps.append(label)
    if gaps:
        raise RuntimeError("target review backend gaps: " + ", ".join(gaps))


def validate_surface_parity() -> None:
    contracts = {
        "CLI": (
            ROOT / "src/app/cli/memory_command.cpp",
            ("review_inspection.preview", "review_application.apply",
             "review_preview_json", "review_application_json"),
        ),
        "RPC": (
            ROOT / "src/app/rpc/rpc_mode.cpp",
            ("memory->preview_review", "memory->apply_review_action",
             "review_preview_json", "review_application_json"),
        ),
        "tools": (
            ROOT / "src/tools/memory_tool.cpp",
            ("runtime_->preview_review", "runtime_->apply_review_action",
             "review_preview_json", "review_application_json"),
        ),
        "TUI": (
            ROOT / "src/app/interactive/interactive_mode.cpp",
            ("runtime_->preview_review", "runtime_->apply_review_action"),
        ),
    }
    for surface, (path, required) in contracts.items():
        source = path.read_text(encoding="utf-8")
        missing = [token for token in required if token not in source]
        if missing:
            raise RuntimeError(
                f"{surface} bypasses shared review semantics: {missing!r}"
            )


def main() -> int:
    args = parse_args()
    compile_probe()
    runtime = BUILD / "runtime"
    shutil.rmtree(runtime, ignore_errors=True)
    runtime.mkdir()
    completed = subprocess.run(
        [str(BINARY), str(runtime), str(CONTRACTS)],
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        sys.stderr.write(completed.stderr)
        return completed.returncode
    report = json.loads(completed.stdout)
    validate_report(report, args.require_target_contract)
    validate_surface_parity()
    report["surface_parity_shared_contract"] = True
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (subprocess.CalledProcessError, RuntimeError, json.JSONDecodeError) as error:
        print(f"memory review probe failed: {error}", file=sys.stderr)
        raise SystemExit(1)
