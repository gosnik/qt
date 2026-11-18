#!/usr/bin/env python3
"""Exercise retrieval settings and recall explanation serialization through RPC."""
from __future__ import annotations
import argparse, json, os, pathlib, sqlite3, subprocess, tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]

def main() -> int:
    parser = argparse.ArgumentParser(); parser.add_argument("--binary", type=pathlib.Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="qt-retrieval-rpc-") as temporary:
        home = pathlib.Path(temporary)
        (home / "settings.json").write_text(json.dumps({"memory": {
            "enabled": True, "recallPolicy": "exploratory",
            "retrieval": {"graphEnabled": True, "denseEnabled": True,
                           "maxDepth": 3, "maxSeeds": 4, "maxEdges": 12,
                           "maxGraphResults": 8, "maxFanout": 5}}}), encoding="utf-8")
        frames = [
            {"type":"memory", "id":"status", "action":"status"},
            {"type":"memory", "id":"remember", "action":"remember", "confirm":True,
             "params":{"content":"probe retrieval explanation", "status":"hypothesis"}},
            {"type":"memory", "id":"recall", "action":"recall", "params":{"query":"probe", "limit":5}},
            {"type":"memory", "id":"explain", "action":"explain", "params":{"query":"probe", "limit":5, "token_budget":900}},
            {"type":"memory", "id":"tiny-explain", "action":"explain", "params":{"query":"probe", "limit":5, "token_budget":1}},
            {"type":"memory", "id":"bad-float-budget", "action":"explain", "params":{"query":"probe", "token_budget":1.5}},
            {"type":"memory", "id":"max-unsigned", "action":"explain", "params":{"query":"probe", "as_of":18446744073709551615}},
            {"type":"memory", "id":"bad-budget", "action":"explain", "params":{"query":"probe", "token_budget":0}},
            {"type":"memory", "id":"as-of", "action":"recall", "params":{"query":"probe", "as_of":0, "limit":5}},
            {"type":"memory", "id":"bad-global", "action":"recall", "params":{"query":"probe", "include_global_user":"yes"}},
            {"type":"memory", "id":"bad-time", "action":"recall", "params":{"query":"probe", "as_of":9223372036854775807}},
            {"type":"memory", "id":"bad-unsigned", "action":"recall", "params":{"query":"probe", "as_of":9223372036854775808}},
            {"type":"memory", "id":"rebuild-no-confirm", "action":"retrieval_rebuild", "params":{}},
            {"type":"memory", "id":"rebuild", "action":"retrieval_rebuild", "confirm":True, "params":{}},
        ]
        env = os.environ.copy(); env["QT_CODING_AGENT_DIR"] = str(home)
        completed = subprocess.run([str(args.binary.resolve()), "--mode", "rpc", "--no-session",
                                    "--no-extensions", "--no-context-files", "--no-skills", "--no-tools"],
                                   cwd=ROOT, env=env,
                                   input="".join(json.dumps(frame)+"\n" for frame in frames),
                                   text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   timeout=30, check=False)
        if completed.returncode != 0: raise RuntimeError(completed.stderr)
        responses = {item["id"]: item for line in completed.stdout.splitlines() if line.strip()
                     for item in [json.loads(line)] if isinstance(item, dict) and "id" in item}
        status = responses["status"]; assert status.get("success") is True
        status_data = status.get("data", status.get("result", {}))
        retrieval = status_data["retrieval"]
        configured = retrieval["configured"]
        assert configured["graph_enabled"] and configured["dense_enabled"]
        assert configured["max_depth"] == 3 and configured["max_seeds"] == 4
        assert isinstance(retrieval["queries"]["cache_entries"], int)
        assert isinstance(retrieval["queries"]["cache_bytes"], int)
        assert retrieval["queries"]["cache_bytes"] >= 0
        code_status = retrieval["code"]
        assert code_status["enabled"] is False and code_status["available"] is False
        assert code_status["stale"] is False and code_status["generation"] == 0
        assert code_status["repository"] == "" and code_status["worktree"] == ""
        recall = responses["recall"]; assert recall.get("success") is True
        recall_data = recall.get("data", recall.get("result", {}))
        assert isinstance(recall_data.get("memories"), list) and recall_data["memories"]
        for item in recall_data["memories"]:
            assert "score_breakdown" in item and "graph_path_ids" in item
            assert isinstance(item.get("assertion_path"), list)
            assert item["source"]["type"] and isinstance(item["revision"], dict)
            assert item["revision"]["historical"] is False
            assert isinstance(item["revision"]["number"], int) and item["revision"]["number"] > 0
            assert item["token_cost"]["exact"] is False
            assert item["token_cost"]["content_bytes"] == len(item["content"].encode("utf-8"))
            assert item["token_cost"]["estimated_input_tokens"] == (len(item["content"].encode("utf-8")) + 3) // 4
        for identifier, budget in (("explain", 900), ("tiny-explain", 1)):
            response = responses[identifier]
            assert response.get("success") is True, response
            data = response.get("data", response.get("result", {}))
            context = data["context"]
            assert data["requested_modes"]["dense"] is True
            assert isinstance(data["observed_retrievers"], list)
            assert isinstance(data["retrieval_warnings"], list)
            assert ("dense" in data["observed_retrievers"]) == any(
                item["score_breakdown"]["dense_rank"] > 0 for item in data["memories"])
            assert context["token_budget"] == budget and context["injected"] is False
            assert context["used"] is None and context["estimated_full_context"]["exact"] is False
            assert context["estimated_full_context"]["tokens"] <= budget
            assert isinstance(context["items"], list)
            assert context["selected_item_bytes"] + context["shared_overhead_bytes"] == context["estimated_full_context"]["bytes"]
            assert data["memories"] and isinstance(data["code_candidates"], list)
            for item in data["memories"]:
                assert item["selection"]["selected"] == (item["id"] in context["selected"]["memory_ids"])
                assert item["selection"]["injected"] is False and item["selection"]["used"] is None
                if item["dense_match"] is not None:
                    assert item["revision"]["basis"] == "dense_capture"
                    assert item["revision"]["number"] == item["dense_match"]["chunk"]["owner_revision"]
                    assert item["dense_match"]["revision_is_current"] is True
                else:
                    assert item["revision"]["basis"] == "current_canonical_at_explanation"
            if budget == 1:
                assert not context["selected"]["memory_ids"]
                assert context["items"] and all(not item["selected"] for item in context["items"])
                assert all(item["rendered_bytes"] is None for item in context["items"])
                assert all(item["reason"] in ("context_envelope_exceeds_budget", "memory_validation_failed") for item in context["items"])
        for identifier in ("bad-budget", "bad-float-budget", "max-unsigned"):
            assert responses[identifier].get("success") is False
        assert responses["remember"].get("success") is True
        assert responses["as-of"].get("success") is True
        for identifier in ("bad-global", "bad-time", "bad-unsigned"):
            assert responses[identifier].get("success") is False
            assert isinstance(responses[identifier].get("error"), str)
        assert responses["rebuild-no-confirm"].get("success") is False
        assert "confirm" in responses["rebuild-no-confirm"].get("error", "")
        rebuild = responses["rebuild"]
        assert rebuild.get("success") is True
        rebuild_data = rebuild.get("data", rebuild.get("result", {}))
        assert rebuild_data["owners"] >= 1 and rebuild_data["jobs_enqueued"] >= 1
        for key in ("owners", "chunks_invalidated", "vectors_invalidated",
                    "jobs_superseded", "jobs_enqueued"):
            assert isinstance(rebuild_data.get(key), int) and rebuild_data[key] >= 0
        with sqlite3.connect(home / "memory" / "memory.sqlite3") as db:
            before = db.execute("SELECT id,access_count FROM memories ORDER BY id").fetchall()
            assert sum(count for _, count in before) == len(recall_data["memories"])
        cli = subprocess.run([str(args.binary.resolve()), "memory", "explain", "probe", "--json"],
                             cwd=ROOT, env=env, text=True, capture_output=True, timeout=30, check=True)
        cli_data = json.loads(cli.stdout)
        assert cli_data["memories"] and cli_data["context"]["injected"] is False
        assert cli_data["requested_modes"]["graph"] is True
        for item in cli_data["memories"]:
            assert "score_breakdown" in item and "selection" in item
        for flags in (("--token-budget", "1"), ("--as-of", "0")):
            command = [str(args.binary.resolve()), "memory", "explain", "probe", "--json", *flags]
            extra = subprocess.run(command, cwd=ROOT, env=env, text=True, capture_output=True,
                                   timeout=30, check=True)
            extra_data = json.loads(extra.stdout)
            if flags[0] == "--token-budget":
                assert extra_data["context"]["token_budget"] == 1
                assert not extra_data["context"]["selected"]["memory_ids"]
            else:
                assert extra_data["requested_modes"] == {"lexical": True, "graph": False, "dense": False, "code": False}
                assert not extra_data["code_candidates"]
        invalid_cli = subprocess.run([str(args.binary.resolve()), "memory", "explain", "probe", "--as-of",
                                      "18446744073709551615", "--json"], cwd=ROOT, env=env,
                                     text=True, capture_output=True, timeout=30)
        assert invalid_cli.returncode != 0
        with sqlite3.connect(home / "memory" / "memory.sqlite3") as db:
            assert before == db.execute("SELECT id,access_count FROM memories ORDER BY id").fetchall()
        print(json.dumps({"retrieval_rpc": True, "configured": configured,
                          "rebuild": rebuild_data}))
    return 0

if __name__ == "__main__": raise SystemExit(main())
