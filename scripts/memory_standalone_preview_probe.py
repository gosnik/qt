#!/usr/bin/env python3
"""Exercise the production standalone `qt memory explain` preview path."""

import argparse
import hashlib
import sqlite3
import json
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def run_checked(command, **kwargs):
    result = subprocess.run(command, check=True, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            **kwargs)
    return result.stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default=str(ROOT / ".pio/build/native/qt"))
    parser.add_argument("--model", default="models/nomic-embed-text-v1.5.Q8_0.gguf")
    args = parser.parse_args()
    binary = Path(args.binary).resolve()
    model = Path(args.model).resolve()
    if not binary.exists():
        raise RuntimeError("native qt binary is missing: " + str(binary))
    if not model.exists():
        raise RuntimeError("Nomic Q8_0 model is missing: " + str(model))

    with tempfile.TemporaryDirectory(prefix="qt-standalone-preview-") as temp:
        root = Path(temp)
        agent_home = root / "agent-home"
        workspace = root / "workspace"
        agent_home.mkdir()
        workspace.mkdir()
        fixture_source = ROOT / "scripts/memory_standalone_preview_probe.cpp"
        native = ROOT / ".pio/build/native"
        objects = [p for p in sorted(native.glob("src/**/*.o"))
                   if p.name != "main.o"]
        objects += sorted(native.glob("lib*/*.a"))
        objects += sorted(native.glob("llama.cpp/**/*.a"))
        response = root / "objects.rsp"
        response.write_text("\n".join(str(path) for path in objects) + "\n")
        probe = root / "fixture"
        includes = ["-Iinclude", "-Isrc/index/tree_sitter_runtime",
                    "-I.pio/build/native/generated/sysroot/usr/include",
                    "-Ilib/boost_1_86_0", "-I.pio/libdeps/native/json/include",
                    "-Ilib/llama.cpp/include", "-Ilib/llama.cpp/ggml/include",
                    "-Ilib/llama.cpp/ggml/src", "-Ilib/llama.cpp/src"]
        link = ["g++", "-std=c++17", *includes, "-pthread", str(fixture_source),
                "-Wl,--start-group", "@" + str(response), "-Wl,--end-group",
                "-L.pio/build/native/generated/sysroot/usr/lib/x86_64-linux-gnu",
                "-lunistring", "-lsqlite3", "-lvulkan", "-lssl", "-lcrypto",
                "-lz", "-lzstd", "-ltinfo", "-ldl", "-lm", "-lrt", "-o", str(probe)]
        subprocess.run(link, cwd=ROOT, check=True)
        fixture = json.loads(run_checked([str(probe), str(agent_home),
                                          str(workspace), str(model)], cwd=ROOT))

        settings = {
            "memory": {
                "enabled": True,
                "automaticRecall": False,
                "automaticCapture": False,
                "recallPolicy": "balanced",
                "retrieval": {
                    "graphEnabled": False,
                    "denseEnabled": True,
                    "codeEnabled": True,
                    "embeddingModel": "nomic-embed-text-v1.5",
                    "queryPrefix": "search_query: ",
                    "documentPrefix": "search_document: ",
                },
            },
            "llama": {
                "models": [{"id": "nomic-embed-text-v1.5",
                             "name": "nomic-embed-text-v1.5",
                             "path": str(model),
                             "supportsEmbeddings": True}],
                "embeddings": {"enabled": True, "normalize": True, "pooling": "mean"},
                "backend": {"gpuLayers": 0},
                "context": {"contextSize": 2048},
            },
        }
        (agent_home / "settings.json").write_text(json.dumps(settings) + "\n")
        env = os.environ.copy()
        env["QT_CODING_AGENT_DIR"] = str(agent_home)
        query = "What is the safe way to renew a login secret?"
        command = [str(binary), "memory", "explain", query, "--json"]
        def persistent_state():
            with sqlite3.connect(f"file:{agent_home / 'memory' / 'memory.sqlite3'}?mode=ro", uri=True) as db:
                memories = db.execute("SELECT id,access_count,status,content FROM memories ORDER BY id").fetchall()
                jobs = db.execute("SELECT * FROM retrieval_jobs ORDER BY id").fetchall()
            spools = {p.name: p.read_bytes() for p in (agent_home / "memory" / "capture_pending").iterdir() if p.is_file()}
            return memories, jobs, spools
        baseline = persistent_state()
        assert baseline[2] and fixture["pending_jobs"] > 0
        index_before = hashlib.sha256(Path(fixture["index_database"]).read_bytes()).hexdigest()
        before = json.loads(run_checked(command, cwd=workspace, env=env))
        if not before.get("memories"):
            raise RuntimeError("dense preview returned no memories: " + json.dumps(before))
        if not any(item.get("id") == fixture["memory_id"] for item in before["memories"]):
            raise RuntimeError("nonlexical dense memory was not recalled")
        if not before.get("code_candidates"):
            raise RuntimeError("code lexical preview returned no candidates")
        if fixture["memory_id"] not in before.get("context", {}).get("selected", {}).get("memory_ids", []):
            raise RuntimeError("dense memory was not selected into context")
        if before.get("requested_modes", {}).get("dense") is not True:
            raise RuntimeError("standalone preview did not report dense mode")
        if "dense" not in before.get("observed_retrievers", []):
            raise RuntimeError("standalone preview did not observe dense retrieval")
        target = next(item for item in before["memories"] if item["id"] == fixture["memory_id"])
        assert target["score_breakdown"]["lexical_rank"] == 0
        assert target["score_breakdown"]["dense_rank"] > 0
        evidence = target["dense_match"]
        assert evidence["rank"] == target["score_breakdown"]["dense_rank"]
        assert -1.0 <= evidence["similarity"] <= 1.0
        assert evidence["similarity"] == target["score_breakdown"]["dense_similarity"]
        assert evidence["index_generation"] and evidence["revision_is_current"] is True
        assert evidence["chunk"]["owner_key"] == "memory:" + target["id"]
        assert target["revision"]["basis"] == "dense_capture"
        assert target["revision"]["number"] == evidence["chunk"]["owner_revision"]
        with sqlite3.connect(f"file:{agent_home / 'memory' / 'memory.sqlite3'}?mode=ro", uri=True) as db:
            stored = db.execute(
                "SELECT owner_revision,content,content_hash,source_uri,source_span,render_version "
                "FROM retrieval_chunks WHERE id=?", (evidence["chunk"]["id"],)).fetchone()
        assert stored == tuple(evidence["chunk"][key] for key in (
            "owner_revision", "text", "content_hash", "source_uri", "source_span", "render_version"))
        assert any(rank["retriever"] == "dense" for candidate in before["code_candidates"] for rank in candidate["ranks"])
        assert before["context"]["injected"] is False
        exact = json.loads(run_checked([str(binary), "memory", "explain", "preview_target", "--json"],
                                       cwd=workspace, env=env))
        assert "code_bm25" in exact["observed_retrievers"]
        assert exact["context"]["selected"]["code_ids"]
        assert any("preview_target" in chunk["text"] for candidate in exact["code_candidates"] for chunk in candidate["chunks"])
        assert persistent_state() == baseline, "preview changed access, durable jobs or pending captures"
        assert hashlib.sha256(Path(fixture["index_database"]).read_bytes()).hexdigest() == index_before

        (workspace / "main.cpp").write_text("int changed_target() { return 8; }\n")
        stale = json.loads(run_checked(command, cwd=workspace, env=env))
        if stale.get("code_candidates"):
            raise RuntimeError("stale code candidate survived source mutation")

        settings["memory"]["retrieval"]["embeddingModel"] = "missing-model"
        (agent_home / "settings.json").write_text(json.dumps(settings) + "\n")
        fallback = json.loads(run_checked(
            [str(binary), "memory", "explain", "Rotate credentials", "--json"],
            cwd=workspace, env=env))
        if not fallback.get("retrieval_warnings"):
            raise RuntimeError("missing dense model did not report a fallback warning")
        if not fallback.get("memories"):
            raise RuntimeError("missing dense model disabled lexical/graph fallback")

        assert persistent_state() == baseline, "fallback preview changed persistent work"
        assert hashlib.sha256(Path(fixture["index_database"]).read_bytes()).hexdigest() == index_before
        print(json.dumps({"standalone_preview": True,
                          "fixture": fixture,
                          "dense_memory": fixture["memory_id"],
                          "stale_code_excluded": True,
                          "missing_model_fallback": True}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
