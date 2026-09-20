#!/usr/bin/env python3

import argparse
import json
import os
import pathlib
import re
import shutil
import sqlite3
import sys
import urllib.error
import urllib.request


def post_json(origin: str, token: str, path: str, payload: dict) -> dict:
    request = urllib.request.Request(
        origin + path,
        data=json.dumps(payload).encode(),
        method="POST",
        headers={"Authorization": "Bearer " + token,
                 "Content-Type": "application/json"},
    )
    with urllib.request.urlopen(request, timeout=3.0) as response:
        return json.loads(response.read())


def expect_http_error(origin: str, token: str, path: str, payload: dict,
                      expected_text: str) -> None:
    try:
        post_json(origin, token, path, payload)
        raise RuntimeError(path + " unexpectedly succeeded")
    except urllib.error.HTTPError as error:
        body = json.loads(error.read())
        if error.code != 400 or expected_text not in body.get("error", ""):
            raise RuntimeError(path + " returned the wrong refusal: " + str(body))


def seed_dream_review(database_path: pathlib.Path, memory_id: str,
                      review_id: str, kind: str, memory_type: str,
                      status: str) -> str:
    candidate = ({"memory_id": memory_id, "source_ids": [],
                  "cycle_id": "dashboard-probe"}
                 if kind == "dream_artifact" else
                 {"memory_id": memory_id, "dream_id": "dream-parent",
                  "source_ids": []})
    with sqlite3.connect(database_path) as database:
        scope = database.execute(
            "SELECT user_id,organization_id,project_id,repository_id,branch_id "
            "FROM memories WHERE id=?", (memory_id,)
        ).fetchone()
        if scope is None:
            raise RuntimeError("Dream review fixture subject was not stored")
        kind_name = "branch" if scope[4] else "repository"
        durable_values = (*scope[:4], scope[4] if kind_name == "branch" else "",
                          "", "", "", "")
        scope_key = kind_name + "|" + "".join(
            str(len(value)) + ":" + value + "|" for value in durable_values
        )
        database.execute(
            "UPDATE memories SET scope_key=?, scope_kind=?, task_id='', agent_id='', "
            "session_id='', conversation_id='', memory_type=?, status=? WHERE id=?",
            (scope_key, kind_name, memory_type, status, memory_id),
        )
        database.execute(
            "INSERT INTO review_items("
            "id,scope_key,kind,status,priority,reason,subject_id,source_id,"
            "source_type,source_uri,source_hash,source_span,created_by,"
            "evidence_strength,candidate_json,decision,decided_by,decision_note,"
            "tags_json,created_at,updated_at,decided_at,candidate_version,revision,"
            "deferred_until,application_status,application_error,action_key) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            (review_id, scope_key, kind, "pending", "normal",
             "isolated dashboard Dream review fixture", memory_id, "",
             "dreaming", "memory://dreaming/dashboard-probe", "", "",
             "dashboard-probe", 0.2, json.dumps(candidate), "", "", "",
             json.dumps(["review:required"]), 1, 1, 0, 1, 0, 0,
             "not_started", "", ""),
        )
    return scope_key


def seed_unrelated_dream_reviews(database_path: pathlib.Path, scope_key: str) -> None:
    rows = []
    for index in range(1001):
        rows.append((
            "review-dashboard-noise-" + str(index), scope_key, "dream_artifact",
            "pending", "normal", "unrelated Dream review queue fixture",
            "memory-dashboard-noise-" + str(index), "", "dreaming",
            "memory://dreaming/dashboard-noise", "", "", "dashboard-probe", 0.2,
            json.dumps({"memory_id": "memory-dashboard-noise-" + str(index),
                        "source_ids": [], "cycle_id": "dashboard-noise"}),
            "", "", "", json.dumps(["review:required"]), 1, 1, 0, 1, 0, 0,
            "not_started", "", "",
        ))
    with sqlite3.connect(database_path) as database:
        database.executemany(
            "INSERT INTO review_items("
            "id,scope_key,kind,status,priority,reason,subject_id,source_id,"
            "source_type,source_uri,source_hash,source_span,created_by,"
            "evidence_strength,candidate_json,decision,decided_by,decision_note,"
            "tags_json,created_at,updated_at,decided_at,candidate_version,revision,"
            "deferred_until,application_status,application_error,action_key) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            rows,
        )


def run_probe(root: pathlib.Path, binary: pathlib.Path,
              runtime_dir: pathlib.Path) -> None:
    sys.path.insert(0, str(root / "scripts"))
    from tui_pty_probe import TerminalProbe, strip_ansi

    shutil.rmtree(runtime_dir, ignore_errors=True)
    agent_home = runtime_dir / "agent-home"
    workspace = runtime_dir / "workspace"
    agent_home.mkdir(parents=True)
    workspace.mkdir(parents=True)
    (agent_home / "settings.json").write_text(
        json.dumps({"memory": {"enabled": True,
                                "automaticRecall": False,
                                "automaticCapture": False}}) + "\n",
        encoding="utf-8",
    )

    previous_home = os.environ.get("QT_CODING_AGENT_DIR")
    previous_browser = os.environ.get("QT_MEMORY_DASHBOARD_NO_BROWSER")
    previous_cwd = pathlib.Path.cwd()
    os.environ["QT_CODING_AGENT_DIR"] = str(agent_home)
    os.environ["QT_MEMORY_DASHBOARD_NO_BROWSER"] = "1"
    os.chdir(workspace)
    dashboard_url = ""
    try:
        with TerminalProbe([str(binary)], 20, 120, 10.0) as term:
            term.read_until(lambda raw, text: False, timeout=2.0)
            if term.proc is None or term.proc.poll() is not None:
                raise RuntimeError("qt exited before dashboard input")

            offset = len(term.output)
            term.write(b"/memory dashboard\r")
            if not term.read_until(
                lambda raw, text: "Memory dashboard started:" in text,
                timeout=8.0,
                offset=offset,
            ):
                raise RuntimeError("/memory dashboard did not start the server")
            rendered = strip_ansi(bytes(term.output)[offset:])
            match = re.search(
                r"http://127\.0\.0\.1:\d+/dashboard\?token=[0-9a-f-]+",
                rendered,
            )
            if not match:
                raise RuntimeError("dashboard launch did not print an authenticated URL")
            dashboard_url = match.group(0)
            token = dashboard_url.rsplit("=", 1)[1]
            origin = dashboard_url.split("/dashboard", 1)[0]
            api_url = origin + "/api/snapshot"

            with urllib.request.urlopen(dashboard_url, timeout=3.0) as response:
                html = response.read().decode("utf-8")
                if response.status != 200 or "qt Memory Dashboard" not in html:
                    raise RuntimeError("dashboard shell was not served")
                if "ImGui + WebAssembly" not in html or "/dashboard/index.js" not in html:
                    raise RuntimeError("dashboard shell did not bootstrap the WASM client")

            for asset, content_type in (("index.js", "application/javascript"),
                                        ("index.wasm", "application/wasm")):
                with urllib.request.urlopen(
                    origin + "/dashboard/" + asset, timeout=3.0
                ) as response:
                    if response.status != 200 or content_type not in response.headers.get(
                        "Content-Type", ""
                    ):
                        raise RuntimeError("WASM dashboard asset has the wrong response")
                    if response.headers.get("Content-Encoding") != "gzip":
                        raise RuntimeError("WASM dashboard asset was not served compressed")

            try:
                urllib.request.urlopen(api_url, timeout=3.0)
                raise RuntimeError("dashboard API accepted an unauthenticated request")
            except urllib.error.HTTPError as error:
                if error.code != 401:
                    raise

            request = urllib.request.Request(
                api_url, headers={"Authorization": "Bearer " + token}
            )
            with urllib.request.urlopen(request, timeout=3.0) as response:
                snapshot = json.loads(response.read())
            if not snapshot.get("available"):
                raise RuntimeError("dashboard snapshot did not use the live memory runtime")
            if snapshot.get("scope", {}).get("repository") is None:
                raise RuntimeError("dashboard snapshot omitted scope metadata")
            if snapshot.get("statistics", {}).get("catalog", {}).get("total") != 0:
                raise RuntimeError("fresh dashboard reported unexpected memories")
            if not isinstance(snapshot.get("memories"), list) or not isinstance(
                snapshot.get("reviews"), list
            ):
                raise RuntimeError("dashboard snapshot omitted explorer collections")
            retrieval = snapshot.get("retrieval")
            if not isinstance(retrieval, dict) or not isinstance(
                retrieval.get("running"), bool
            ) or not isinstance(retrieval.get("jobs"), dict):
                raise RuntimeError("dashboard snapshot omitted live retrieval status")
            if not isinstance(retrieval.get("queries", {}).get("cache_entries"), int) or not isinstance(
                retrieval.get("queries", {}).get("cache_bytes"), int
            ):
                raise RuntimeError("dashboard snapshot omitted query-cache accounting")
            code_status = retrieval.get("code")
            if not isinstance(code_status, dict) or code_status.get("enabled") is not False or \
                    code_status.get("available") is not False or \
                    code_status.get("stale") is not False or \
                    code_status.get("generation") != 0 or \
                    code_status.get("repository") != "" or code_status.get("worktree") != "":
                raise RuntimeError("dashboard snapshot omitted effective code freshness")
            if snapshot["memories"]:
                token_cost = snapshot["memories"][0].get("token_cost", {})
                if token_cost.get("exact") is not False or "estimated_input_tokens" not in token_cost:
                    raise RuntimeError("dashboard memory omitted labelled token estimate")

            summary_request = urllib.request.Request(
                origin + "/dashboard/summary",
                data=b"{}",
                method="POST",
                headers={"Authorization": "Bearer " + token,
                         "Content-Type": "application/json"},
            )
            with urllib.request.urlopen(summary_request, timeout=3.0) as response:
                summary = json.loads(response.read())
            if "dashboard_summary" not in summary:
                raise RuntimeError("WASM compatibility adapter omitted summary data")
            if not isinstance(summary["dashboard_summary"].get("retrieval"), dict):
                raise RuntimeError("dashboard summary omitted retrieval status")
            summary_retrieval = summary["dashboard_summary"]["retrieval"]
            if not isinstance(summary_retrieval.get("code"), dict) or \
                    summary_retrieval["code"].get("enabled") is not False:
                raise RuntimeError("dashboard summary omitted effective code freshness")
            if not isinstance(summary_retrieval.get("queries", {}).get("cache_bytes"), int):
                raise RuntimeError("dashboard summary omitted query-cache accounting")

            context_request = urllib.request.Request(
                origin + "/context/build",
                data=json.dumps({"query": "graph boundary", "budget_tokens": 900}).encode(),
                method="POST",
                headers={"Authorization": "Bearer " + token,
                         "Content-Type": "application/json"},
            )
            with urllib.request.urlopen(context_request, timeout=8.0) as response:
                context_body = json.loads(response.read())
            context_pack = context_body.get("context_pack", {})
            if not isinstance(context_pack.get("explanation"), dict):
                raise RuntimeError("context preview omitted selection explanation")
            explanation = context_pack["explanation"]
            if explanation.get("injected") is not False:
                raise RuntimeError("context preview claimed injected context")
            if explanation.get("token_budget") != 900:
                raise RuntimeError("context preview omitted requested token budget")
            if not isinstance(context_pack.get("memories"), list) or not isinstance(
                context_pack.get("code_candidates"), list
            ):
                raise RuntimeError("context preview omitted candidate explanations")

            invalid_context = urllib.request.Request(
                origin + "/context/build",
                data=b'{"query":"graph boundary","recall_policy":"invalid"}',
                method="POST",
                headers={"Authorization": "Bearer " + token,
                         "Content-Type": "application/json"},
            )
            try:
                urllib.request.urlopen(invalid_context, timeout=3.0)
                raise RuntimeError("context preview accepted invalid recall policy")
            except urllib.error.HTTPError as error:
                if error.code != 400:
                    raise

            rebuild_request = urllib.request.Request(
                origin + "/dashboard/retrieval/rebuild",
                data=b'{"confirm":true}',
                method="POST",
                headers={"Authorization": "Bearer " + token,
                         "Content-Type": "application/json"},
            )
            with urllib.request.urlopen(rebuild_request, timeout=3.0) as response:
                rebuild = json.loads(response.read())
            if rebuild.get("retrieval_rebuild") is not True:
                raise RuntimeError("dashboard retrieval rebuild did not return confirmation")
            for key in ("owners", "chunks_invalidated", "vectors_invalidated",
                        "jobs_superseded", "jobs_enqueued"):
                if not isinstance(rebuild.get(key), int) or rebuild[key] < 0:
                    raise RuntimeError("dashboard retrieval rebuild omitted bounded counts")

            write_request = urllib.request.Request(
                origin + "/mcp/memory/write",
                data=json.dumps({"content": "x" * 55 + "—" + "y" * 121
                                            + "—graph boundary",
                                 "type": "observation",
                                 "status": "validated",
                                 "source_type": "manual"}).encode(),
                method="POST",
                headers={"Authorization": "Bearer " + token,
                         "Content-Type": "application/json"},
            )
            with urllib.request.urlopen(write_request, timeout=3.0) as response:
                written = json.loads(response.read())
            if not written.get("id") or written.get("entry", {}).get("status") != "hypothesis":
                raise RuntimeError("dashboard write bypassed conservative lifecycle policy")

            database_path = agent_home / "memory" / "memory.sqlite3"
            def write_fixture(content: str) -> str:
                fixture = post_json(origin, token, "/mcp/memory/write", {
                    "content": content, "type": "observation",
                    "source_type": "manual",
                })
                if not fixture.get("id"):
                    raise RuntimeError("could not create Dream review fixture memory")
                return fixture["id"]

            approve_subject = write_fixture("dashboard Dream approval fixture")
            reject_subject = write_fixture("dashboard Dream rejection fixture")
            stale_subject = write_fixture("dashboard Dream stale preview fixture")
            outside_subject = write_fixture("dashboard Dream out-of-scope fixture")
            durable_scope_key = seed_dream_review(
                database_path, approve_subject, "review-dashboard-approve",
                "dream_artifact", "dream", "dream_artifact")
            seed_dream_review(database_path, reject_subject, "review-dashboard-reject",
                              "dream_research_successor", "research", "candidate")
            seed_dream_review(database_path, stale_subject, "review-dashboard-stale",
                              "dream_artifact", "dream", "dream_artifact")
            seed_unrelated_dream_reviews(database_path, durable_scope_key)
            seed_dream_review(database_path, outside_subject, "review-dashboard-outside",
                              "dream_artifact", "dream", "dream_artifact",
                              )
            with sqlite3.connect(database_path) as database:
                exact_scope_key = database.execute(
                    "SELECT scope_key FROM memories WHERE id=?", (written["id"],)
                ).fetchone()[0]
                database.execute(
                    "UPDATE review_items SET scope_key=? WHERE id=?",
                    (durable_scope_key + ":outside", "review-dashboard-outside"),
                )
            if exact_scope_key == durable_scope_key:
                raise RuntimeError("Dream fixture did not use a distinct durable runtime scope")

            approve_preview = post_json(origin, token, "/dashboard/review/preview", {
                "id": approve_subject, "action": "approve", "note": "must be ignored",
            })
            if approve_preview.get("review_id") != "review-dashboard-approve" or \
                    approve_preview.get("action", {}).get("actor") != "dashboard:user" or \
                    not approve_preview.get("applicable") or \
                    not approve_preview.get("requires_human_authority") or \
                    not approve_preview.get("idempotency_token") or \
                    approve_preview.get("review_revision") != 0:
                raise RuntimeError("dashboard Dream approval preview omitted governed contract")
            approve_result = post_json(origin, token, "/dashboard/review/apply", {
                "review_id": approve_preview["review_id"], "action": "approve",
                "expected_revision": approve_preview["review_revision"],
                "idempotency_key": approve_preview["idempotency_token"], "confirm": True,
                "payload": {"edit": {"target_status": "active"}},
            })
            if approve_result.get("status") != "applied" or not approve_result.get("decision_persisted"):
                raise RuntimeError("dashboard Dream approval did not apply")
            approve_replay = post_json(origin, token, "/dashboard/review/apply", {
                "review_id": approve_preview["review_id"], "action": "approve",
                "expected_revision": approve_preview["review_revision"],
                "idempotency_key": approve_preview["idempotency_token"], "confirm": True,
            })
            if not approve_replay.get("idempotent_replay") or \
                    approve_replay.get("status") != "applied":
                raise RuntimeError("dashboard Dream approval did not replay idempotently")
            with sqlite3.connect(database_path) as database:
                approve_status = database.execute(
                    "SELECT status FROM memories WHERE id=?", (approve_subject,)
                ).fetchone()[0]
                approve_decisions = database.execute(
                    "SELECT COUNT(*) FROM review_actions WHERE review_id=?",
                    ("review-dashboard-approve",),
                ).fetchone()[0]
            if approve_status != "hypothesis" or approve_decisions != 1:
                raise RuntimeError("dashboard Dream approval did not stop at hypothesis")

            reject_preview = post_json(origin, token, "/dashboard/review/preview", {
                "id": reject_subject, "action": "reject",
            })
            reject_result = post_json(origin, token, "/dashboard/review/apply", {
                "review_id": reject_preview["review_id"], "action": "reject",
                "expected_revision": reject_preview["review_revision"],
                "idempotency_key": reject_preview["idempotency_token"], "confirm": True,
            })
            if reject_result.get("status") != "applied":
                raise RuntimeError("dashboard Dream rejection did not apply")
            with sqlite3.connect(database_path) as database:
                reject_status = database.execute(
                    "SELECT status FROM memories WHERE id=?", (reject_subject,)
                ).fetchone()[0]
                reject_decisions = database.execute(
                    "SELECT COUNT(*) FROM review_actions WHERE review_id=?",
                    ("review-dashboard-reject",),
                ).fetchone()[0]
            if reject_status != "rejected" or reject_decisions != 1:
                raise RuntimeError("dashboard Dream rejection did not persist exactly once")

            stale_preview = post_json(origin, token, "/dashboard/review/preview", {
                "id": stale_subject, "action": "approve",
            })
            expect_http_error(origin, token, "/dashboard/review/apply", {
                "review_id": stale_preview["review_id"], "action": "approve",
                "expected_revision": stale_preview["review_revision"],
                "idempotency_key": stale_preview["idempotency_token"],
            }, "confirm=true")
            expect_http_error(origin, token, "/dashboard/review/apply", {
                "review_id": stale_preview["review_id"], "action": "approve",
                "expected_revision": stale_preview["review_revision"],
                "idempotency_key": "", "confirm": True,
            }, "non-empty idempotency key")
            with sqlite3.connect(database_path) as database:
                database.execute(
                    "UPDATE review_items SET revision=revision+1 WHERE id=?",
                    ("review-dashboard-stale",),
                )
            expect_http_error(origin, token, "/dashboard/review/apply", {
                "review_id": stale_preview["review_id"], "action": "approve",
                "expected_revision": stale_preview["review_revision"],
                "idempotency_key": stale_preview["idempotency_token"], "confirm": True,
            }, "stale idempotency token")
            with sqlite3.connect(database_path) as database:
                stale_decisions = database.execute(
                    "SELECT COUNT(*) FROM review_actions WHERE review_id=?",
                    ("review-dashboard-stale",),
                ).fetchone()[0]
                stale_status = database.execute(
                    "SELECT status FROM memories WHERE id=?", (stale_subject,)
                ).fetchone()[0]
            if stale_decisions != 0 or stale_status != "dream_artifact":
                raise RuntimeError("stale dashboard preview mutated a pending Dream subject")
            expect_http_error(origin, token, "/dashboard/review/preview", {
                "id": outside_subject, "action": "approve",
            }, "no pending governed Dream review")
            expect_http_error(origin, token, "/mcp/memory/promote", {
                "id": approve_subject,
            }, "require governed dashboard review")
            expect_http_error(origin, token, "/mcp/memory/deprecate", {
                "id": reject_subject,
            }, "require governed dashboard review")
            expect_http_error(origin, token, "/mcp/memory/reject", {
                "id": outside_subject,
            }, "require governed dashboard review")
            with sqlite3.connect(database_path) as database:
                outside_decisions = database.execute(
                    "SELECT COUNT(*) FROM review_actions WHERE review_id=?",
                    ("review-dashboard-outside",),
                ).fetchone()[0]
                outside_status = database.execute(
                    "SELECT status FROM memories WHERE id=?", (outside_subject,)
                ).fetchone()[0]
            if outside_decisions != 0 or outside_status != "dream_artifact":
                raise RuntimeError("failed dashboard Dream action mutated an out-of-scope subject")

            query_request = urllib.request.Request(
                origin + "/mcp/memory/query",
                data=json.dumps({"query": "graph boundary"}).encode(),
                method="POST",
                headers={"Authorization": "Bearer " + token,
                         "Content-Type": "application/json"},
            )
            with urllib.request.urlopen(query_request, timeout=3.0) as response:
                query_body = json.loads(response.read())
            queried = query_body.get("entries", [])
            if not queried or not isinstance(queried[0].get("revision"), dict):
                raise RuntimeError("dashboard query omitted current retrieval revision")
            if queried[0]["revision"].get("number", 0) <= 0:
                raise RuntimeError("dashboard query returned no canonical retrieval revision")

            graph_request = urllib.request.Request(
                origin + "/dashboard/graph",
                data=b'{"memory_type":"observation"}',
                method="POST",
                headers={"Authorization": "Bearer " + token,
                         "Content-Type": "application/json"},
            )
            with urllib.request.urlopen(graph_request, timeout=3.0) as response:
                graph = json.loads(response.read())
            nodes = graph.get("graph", {}).get("nodes", [])
            if not nodes or nodes[0].get("label") != "x" * 55:
                raise RuntimeError("dashboard graph split a UTF-8 code point")
            expected_summary = "x" * 55 + "—" + "y" * 121
            if nodes[0].get("summary") != expected_summary:
                raise RuntimeError("dashboard graph summary split a UTF-8 code point")
            if not isinstance(graph.get("graph", {}).get("groups"), list):
                raise RuntimeError("dashboard graph omitted WASM group metadata")

            with sqlite3.connect(database_path) as database:
                database.execute(
                    "UPDATE memories SET content=CAST(x'6c656761637920ff' AS TEXT) "
                    "WHERE id=?",
                    (written["id"],),
                )
            with urllib.request.urlopen(graph_request, timeout=3.0) as response:
                repaired_graph_body = response.read().decode("utf-8")
                repaired_graph = json.loads(repaired_graph_body)
            repaired_nodes = repaired_graph.get("graph", {}).get("nodes", [])
            if not repaired_nodes or repaired_nodes[0].get("content") != "legacy �":
                raise RuntimeError("dashboard did not repair malformed legacy UTF-8")

            malformed_request = urllib.request.Request(
                origin + "/dashboard/graph",
                data=b'{"limit":',
                method="POST",
                headers={"Authorization": "Bearer " + token,
                         "Content-Type": "application/json"},
            )
            try:
                urllib.request.urlopen(malformed_request, timeout=3.0)
                raise RuntimeError("dashboard accepted malformed JSON")
            except urllib.error.HTTPError as error:
                if error.code != 400 or "error" not in json.loads(error.read()):
                    raise

            offset = len(term.output)
            term.write(b"/memory\r")
            if not term.read_until(
                lambda raw, text: "Open the scoped memory dashboard" in text,
                timeout=6.0,
                offset=offset,
            ):
                raise RuntimeError("/memory menu omitted the dashboard item")
            offset = len(term.output)
            term.write(b"\x1b[B\x1b[B\r")
            if not term.read_until(
                lambda raw, text: "Memory dashboard already running:" in text,
                timeout=6.0,
                offset=offset,
            ):
                raise RuntimeError("/memory menu did not launch the running dashboard")

            term.write(b"/quit\r")
            if not term.read_until(
                lambda raw, text: term.proc is not None
                and term.proc.poll() is not None,
                timeout=5.0,
            ):
                raise RuntimeError("qt did not stop after dashboard probe")

        try:
            urllib.request.urlopen(dashboard_url, timeout=0.5)
            raise RuntimeError("dashboard listener survived interactive shutdown")
        except (urllib.error.URLError, TimeoutError, ConnectionError):
            pass
    finally:
        os.chdir(previous_cwd)
        if previous_home is None:
            os.environ.pop("QT_CODING_AGENT_DIR", None)
        else:
            os.environ["QT_CODING_AGENT_DIR"] = previous_home
        if previous_browser is None:
            os.environ.pop("QT_MEMORY_DASHBOARD_NO_BROWSER", None)
        else:
            os.environ["QT_MEMORY_DASHBOARD_NO_BROWSER"] = previous_browser


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify the interactive loopback memory dashboard"
    )
    parser.add_argument("--binary", default=".pio/build/native/qt")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    binary = pathlib.Path(args.binary)
    if not binary.is_absolute():
        binary = (root / binary).resolve()
    if not binary.is_file():
        raise RuntimeError("native qt binary is missing; run `pio run -e native`")
    run_probe(root, binary, root / ".pio" / "test" / "memory-dashboard-probe")
    print("memory dashboard probe passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
