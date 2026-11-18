#include "qt/memory/runtime.hpp"
#include "qt/ai/images.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace qt::ai {

std::optional<ImageDataUrl> parse_image_data_url(const std::string&) {
  return std::nullopt;
}

}  // namespace qt::ai

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace qt::memory;

constexpr const char* kDreamModel = "llama/ternary-bonsai-27b-q2-0";

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

bool contains(const std::vector<std::string>& values,
              const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

MemoryScope canonical_scope(MemoryScope scope) {
  scope.kind = scope.branch.empty() ? ScopeKind::Repository : ScopeKind::Branch;
  scope.task.clear();
  scope.agent.clear();
  scope.session.clear();
  scope.conversation.clear();
  return scope;
}

MemoryWrite source_memory(const std::size_t index) {
  MemoryWrite write;
  write.type = MemoryType::Observation;
  write.status = MemoryStatus::Hypothesis;
  write.source = "dreaming-probe";
  write.provenance.source_type = SourceType::Manual;
  write.provenance.source_uri =
      "test://dreaming/source/" + std::to_string(index);
  write.provenance.created_by = "user";
  write.provenance.evidence_strength = 0.8;
  write.content = "Dream source " + std::to_string(index) +
                  ": repeated native build failures suggest a bounded "
                  "artifact-cache invalidation pattern.";
  write.confidence = 0.7;
  write.quality_score = 1.0 - static_cast<double>(index) / 100.0;
  write.tags = {"dreaming:source", "native-build"};
  return write;
}

void write_child(const fs::path& path, const bool invalid_output,
                 const bool expect_web_tools = false,
                 const std::string& verifier_mode = "",
                 const bool research_missing_content = false) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  require(static_cast<bool>(output), "unable to create dreaming child fixture");
  output << R"PY(#!/usr/bin/env python3
import json
import os
import pathlib
import sqlite3
import sys

request = json.loads(sys.stdin.read())
arguments = sys.argv[1:]
research_phase = os.environ.get("QT_MEMORY_DREAM_RESEARCH") == "1"
required_flags = [
    "--no-session", "--no-extensions", "--no-context-files",
    "--no-skills", "--no-memory"
]
if any(flag not in arguments for flag in required_flags):
    raise SystemExit(41)
permit_research_phase = )PY";
  output << (expect_web_tools ? "True" : "False");
  output << R"PY(
verifier_mode = )PY";
  output << Json(verifier_mode).dump();
  output << R"PY(
research_missing_content = )PY";
  output << (research_missing_content ? "True" : "False");
  output << R"PY(
enabled_tools = []
if "--tools" in arguments:
    tools_index = arguments.index("--tools")
    if tools_index + 1 >= len(arguments):
        raise SystemExit(46)
    enabled_tools = [tool for tool in arguments[tools_index + 1].split(",") if tool]
verification_phase = os.environ.get("QT_MEMORY_DREAM_VERIFY") == "1"
if verification_phase:
    if (os.environ.get("PI_OFFLINE") != "1" or "--no-tools" not in arguments or
            enabled_tools or "--provider" not in arguments or
            arguments[arguments.index("--provider") + 1] != "llama" or
            "--model" not in arguments or
            arguments[arguments.index("--model") + 1] != "verifier"):
        raise SystemExit(51)
    documents = request.get("documents")
    if not isinstance(documents, list) or not documents or len(documents) > 4:
        raise SystemExit(52)
    with (pathlib.Path(os.environ["QT_CODING_AGENT_DIR"]) / "dream-child-invocations.log").open("a", encoding="utf-8") as log:
        log.write(json.dumps({"phase": "verify", "document_count": len(documents),
            "request_keys": sorted(request.keys()), "enabled_tools": enabled_tools}, separators=(",", ":")) + "\n")
    citations = []
    for document in documents:
        content = document.get("content", "")
        if not isinstance(content, str) or len(content) < 20:
            raise SystemExit(53)
        citations.append({"url": document.get("url"), "quote": content[:min(len(content), 80)]})
    if verifier_mode == "tamper_quote":
        citations[0]["quote"] = "this quote is not present in the fetched document"
    if verifier_mode == "tool_use":
        print(json.dumps({"type": "tool_execution_start", "toolCallId": "forbidden",
            "toolName": "web_fetch", "args": {}}, separators=(",", ":")))
    if verifier_mode in ("stale_subject", "stale_review"):
        database = pathlib.Path(os.environ["QT_CODING_AGENT_DIR"]) / "memory" / "memory.sqlite3"
        connection = sqlite3.connect(database)
        if verifier_mode == "stale_subject":
            connection.execute("UPDATE memories SET content=content || ' stale-race' "
                               "WHERE memory_type='research' AND status='candidate'")
        else:
            connection.execute("UPDATE review_items SET revision=revision+1 "
                               "WHERE kind='dream_research_successor' AND status='pending'")
        connection.commit()
        connection.close()
    if verifier_mode == "malformed":
        print(json.dumps({"type": "agent_end", "messages": [{"content": [
            {"type": "text", "text": "not valid verifier json"}]}]},
            separators=(",", ":")))
        raise SystemExit(0)
    verdict = ("unsupported" if verifier_mode == "unsupported" else
               "inconclusive" if verifier_mode == "inconclusive" else "supported")
    result = {"version": 1, "verdict": verdict,
        "summary_hash": request.get("summary_hash"),
        "evidence_hash": request.get("evidence_hash"),
        "rationale": "Every supplied fetched document was checked by the local verifier.",
        "citations": citations}
    print(json.dumps({"type": "agent_end", "messages": [{"content": [{"type": "text", "text": json.dumps(result, separators=(",", ":"))}]}]}, separators=(",", ":")))
    raise SystemExit(0)
if research_phase:
    allowed_web_tools = {"web_fetch", "web_search", "web_run"}
    if (not permit_research_phase or "--no-tools" in arguments or
            enabled_tools != ["web_run", "web_search", "web_fetch"]):
        raise SystemExit(47)
    if any(tool not in allowed_web_tools for tool in enabled_tools):
        raise SystemExit(48)
else:
    if "--no-tools" not in arguments or enabled_tools:
        raise SystemExit(49)
if research_phase and os.environ.get("QT_MEMORY_DREAMING") == "1":
    raise SystemExit(50)
if not research_phase and os.environ.get("QT_MEMORY_DREAMING") != "1":
    raise SystemExit(42)
if "--model" not in arguments:
    raise SystemExit(43)
model_index = arguments.index("--model")
if arguments[model_index + 1] != "llama/ternary-bonsai-27b-q2-0":
    raise SystemExit(44)

def governed_records(value):
    for key in ("candidates", "source_memories", "memories", "records"):
        records = value.get(key)
        if isinstance(records, list):
            return records
    return []

records = governed_records(request)
ids = [item.get("id") for item in records if isinstance(item, dict) and isinstance(item.get("id"), str)]
if not research_phase and (not ids or len(ids) > 8):
    raise SystemExit(45)

home = pathlib.Path(os.environ["QT_CODING_AGENT_DIR"])
with (home / "dream-child-invocations.log").open("a", encoding="utf-8") as log:
    log.write(json.dumps({
        "phase": "research" if research_phase else "synthesis",
        "candidate_count": len(ids),
        "source_ids": ids,
        "request_keys": sorted(request.keys()),
        "enabled_tools": enabled_tools,
    }, separators=(",", ":")) + "\n")

if research_phase:
    search_start = {
        "type": "tool_execution_start",
        "toolCallId": "research-search-1",
        "toolName": "web_search",
        "args": {"query": "artifact cache invalidation native builds"},
    }
    search_end = {
        "type": "tool_execution_end",
        "toolCallId": "research-search-1",
        "toolName": "web_search",
        "result": {
            "content": [{"type": "text", "text": "Search result discovered."}],
            "details": {
                "results": [{"title": "Source", "url": "https://example.test/source"}],
                "count": 1,
            },
        },
        "isError": False,
    }
    fetch_start = {
        "type": "tool_execution_start",
        "toolCallId": "research-fetch-1",
        "toolName": "web_fetch",
        "args": {"url": "https://example.test/source"},
    }
    fetch_end = {
        "type": "tool_execution_end",
        "toolCallId": "research-fetch-1",
        "toolName": "web_fetch",
        "result": {
            "content": [{"type": "text", "text": "Independent source supports bounded artifact-cache invalidation."}],
            "details": {
                "url": "https://example.test/source",
                "content": "Independent source supports bounded artifact-cache invalidation.",
                "status": 200,
                "public_only": True,
            },
        },
        "isError": False,
    }
    run_start = {
        "type": "tool_execution_start",
        "toolCallId": "research-run-1",
        "toolName": "web_run",
        "args": {"open": [{"ref_id": "turn0search0"}]},
    }
    run_end = {
        "type": "tool_execution_end",
        "toolCallId": "research-run-1",
        "toolName": "web_run",
        "result": {
            "content": [{"type": "text", "text": "A second source was opened."}],
            "details": {
                "references": [{"ref_id": "turn0search0", "title": "Second source", "url": "https://example.test/second",
                                "content": "Second independent source confirms generated files must be invalidated after incompatible artifact changes."}],
                "external_context": True,
            },
        },
        "isError": False,
    }
    if research_missing_content:
        run_end["result"]["details"]["references"][0].pop("content")
    research = {
        "version": 1,
        "summary": "Independent documentation supports the bounded artifact-cache invalidation hypothesis.",
        "source_urls": ["https://example.test/source", "https://example.test/second"],
        "confidence": 0.75,
    }
    for event in (search_start, search_end, fetch_start, fetch_end, run_start, run_end):
        print(json.dumps(event, separators=(",", ":")))
    print(json.dumps({
        "type": "agent_end",
        "messages": [{"content": [{"type": "text", "text": json.dumps(research, separators=(",", ":"))}]}],
    }, separators=(",", ":")))
    raise SystemExit(0)

selected = ["memory_not_governed"] if )PY";
  output << (invalid_output ? "True" : "False");
  output << R"PY( else ids[:2]
result = {
    "version": 1,
    "source_memory_ids": selected,
    "artifact_kind": "pattern_discovery",
    "content": "Dream hypothesis: repeated native build failures may share an artifact-cache invalidation cause.",
    "confidence": 0.45,
    "research_required": True,
}
event = {
    "type": "agent_end",
    "messages": [{"content": [{"type": "text", "text": json.dumps(result, separators=(",", ":"))}]}],
}
print(json.dumps(event, separators=(",", ":")))
)PY";
  require(static_cast<bool>(output), "unable to write dreaming child fixture");
#if !defined(_WIN32)
  require(::chmod(path.c_str(), S_IRUSR | S_IWUSR | S_IXUSR) == 0,
          "unable to make dreaming child executable");
#endif
}

std::vector<Json> json_lines(const fs::path& path) {
  std::ifstream input(path);
  std::vector<Json> result;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty()) {
      result.push_back(Json::parse(line));
    }
  }
  return result;
}

std::size_t line_count(const fs::path& path) {
  std::ifstream input(path);
  std::size_t result = 0U;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty()) {
      ++result;
    }
  }
  return result;
}

MemoryRuntimeOptions runtime_options(const fs::path& home,
                                     const fs::path& child) {
  MemoryRuntimeOptions options;
  options.agent_home = home.generic_string();
  options.cwd = fs::current_path().generic_string();
  options.session = "dreaming-probe-session";
  options.agent = "dreaming-probe-agent";
  options.automatic_capture = false;
  options.capture_post_process = "manual";
  options.isolated_agent.executable = child.generic_string();
  options.dreaming.enabled = true;
  options.dreaming.trigger = "manual";
  options.dreaming.idle_seconds = 1;
  options.dreaming.candidate_threshold = 2U;
  options.dreaming.model = std::string(kDreamModel);
  options.dreaming.timeout_ms = 2000;
  options.dreaming.max_model_input_tokens = 6000U;
  options.dreaming.max_candidates = 8U;
  options.dreaming.research_policy = "disabled";
  options.dreaming.promotion_policy = "review_only";
  return options;
}

std::vector<std::string> seed_sources(MemoryRuntime& runtime,
                                      const MemoryScope& scope,
                                      const std::size_t count) {
  std::vector<std::string> result;
  result.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    result.push_back(runtime.memories().remember(scope, source_memory(index)));
  }
  return result;
}

std::vector<SessionEvent> events(SQLiteMemoryDatabase& database,
                                 const MemoryScope& scope,
                                 const std::string& kind) {
  return database.session_events(scope, kind, 20U, 0U);
}

const ReviewItem& find_review(const std::vector<ReviewItem>& reviews,
                              const std::string& id) {
  const auto found = std::find_if(reviews.begin(), reviews.end(),
                                  [&](const ReviewItem& item) {
                                    return item.id == id;
                                  });
  require(found != reviews.end(), "dreaming review was not persisted");
  return *found;
}

void verify_successful_cycle(const fs::path& home) {
  fs::create_directories(home);
  const fs::path child = home / "dream-child.py";
  write_child(child, false);

  MemoryScope scope;
  std::string foreign_source_id;
  std::vector<std::string> source_ids;
  {
    MemoryRuntime runtime(runtime_options(home, child));
    scope = canonical_scope(runtime.scope());
    source_ids = seed_sources(runtime, scope, 12U);
    auto foreign_scope = scope;
    foreign_scope.repository += "_foreign";
    foreign_source_id =
        runtime.memories().remember(foreign_scope, source_memory(99U));
    require(!runtime.enqueue_dream_cycle("not-a-trigger"),
            "dream queue accepted an unknown trigger");
    require(runtime.enqueue_dream_cycle("manual"),
            "eligible dream cycle was not enqueued");
    require(runtime.enqueue_dream_cycle("candidate_threshold") &&
                runtime.enqueue_dream_cycle("interval"),
            "a configured Dreaming trigger was rejected by the bounded queue");
    const fs::path invocation_log = home / "dream-child-invocations.log";
    for (std::size_t attempt = 0U;
         attempt < 60U && line_count(invocation_log) < 3U; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    require(line_count(invocation_log) == 3U,
            "immediate Dreaming trigger queue did not drain");
    require(runtime.enqueue_dream_cycle("manual"),
            "repeated manual Dreaming cycle was not enqueued");
    for (std::size_t attempt = 0U;
         attempt < 60U && line_count(invocation_log) < 4U; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    require(line_count(invocation_log) == 4U,
            "repeated manual Dreaming cycle did not invoke the worker");
  }

  const auto selection_invocations =
      json_lines(home / "dream-child-invocations.log");
  std::set<std::string> candidate_packs;
  std::set<std::string> candidate_coverage;
  std::size_t synthesis_invocations = 0U;
  for (const auto& invocation : selection_invocations) {
    if (invocation.value("phase", std::string{}) != "synthesis") continue;
    ++synthesis_invocations;
    const auto& ids = invocation.at("source_ids");
    require(ids.is_array() && ids.size() == 8U,
            "Dreaming selection did not fill its bounded candidate pack");
    std::set<std::string> unique_ids;
    for (const auto& id : ids) {
      require(id.is_string(), "Dreaming candidate id was not a string");
      unique_ids.insert(id.get<std::string>());
      candidate_coverage.insert(id.get<std::string>());
    }
    require(unique_ids.size() == ids.size(),
            "Dreaming candidate pack contained duplicate memories");
    candidate_packs.insert(ids.dump());
  }
  require(synthesis_invocations == 4U && candidate_packs.size() > 1U &&
              candidate_coverage.size() > 8U,
          "repeated Dreaming cycles did not explore different source memories");

  SQLiteMemoryDatabase database(home);
  const auto claims = events(database, scope, "memory_dream_claim");
  const auto receipts = events(database, scope, "memory_dream_receipt");
  require(claims.size() == 4U && receipts.size() == 4U,
          "Dreaming triggers did not persist one claim and receipt each");
  Json receipt;
  for (const auto& event : receipts) {
    const auto candidate = Json::parse(event.payload_json);
    if (candidate.value("trigger", std::string{}) == "manual") {
      receipt = candidate;
      break;
    }
  }
  require(receipt.is_object(), "manual Dreaming receipt was not persisted");
  require(receipt.value("trigger", std::string{}) == "manual" &&
              receipt.value("model", std::string{}) == kDreamModel &&
              receipt.value("isolated_agent", false) &&
              receipt.value("review_created", false) &&
              receipt.value("error", std::string{}).empty() &&
              receipt.value("backend_fallback", std::string{}) == "none",
          "successful dream receipt lost isolation or routing evidence");
  require(receipt.contains("claim_key") &&
              !receipt.at("claim_key").get<std::string>().empty() &&
              receipt.contains("cycle_id") &&
              !receipt.at("cycle_id").get<std::string>().empty() &&
              receipt.value("protocol_version", std::string{}) ==
                  "dreaming-v6" &&
              receipt.value("selection_algorithm", std::string{}) ==
                  "least-recent-cycle-hash-v1" &&
              receipt.value("selection_seed", std::string{}) ==
                  receipt.value("cycle_id", std::string{}) &&
              receipt.value("candidate_pool_count", 0U) == 12U &&
              !receipt.value("candidate_pool_hash", std::string{}).empty() &&
              receipt.at("input_candidate_ids").is_array() &&
              receipt.at("input_candidate_ids").size() == 8U &&
              receipt.contains("source_pack_hash") &&
              !receipt.at("source_pack_hash").get<std::string>().empty() &&
              receipt.at("bounded_source_ids").is_array() &&
              receipt.at("bounded_source_ids").size() <= 8U,
          "dream receipt omitted bounded/idempotent source evidence");
  const auto successful_stats =
      database.statistics(scope, RecallPolicy::Strict, false).dreaming;
  require(successful_stats.attempted == 4U &&
              successful_stats.completed == 4U &&
              successful_stats.failed == 0U &&
              successful_stats.pending == 0U &&
              successful_stats.reviews_created == 4U &&
              successful_stats.by_model.at(kDreamModel) == 4U,
          "successful Dreaming run statistics were not persisted");
  require(std::none_of(receipt.at("bounded_source_ids").begin(),
                       receipt.at("bounded_source_ids").end(),
                       [&](const Json& id) {
                         return id.is_string() &&
                                id.get<std::string>() == foreign_source_id;
                       }),
          "dream source selection leaked across repository scope");

  const std::string dream_id = receipt.at("dream_id").get<std::string>();
  const std::string review_id = receipt.at("review_id").get<std::string>();
  MemoryService memories(database);
  const auto dream = memories.get(scope, dream_id);
  require(dream.type == MemoryType::Dream &&
              dream.status == MemoryStatus::DreamArtifact &&
              dream.provenance.source_type == SourceType::Dreaming &&
              dream.provenance.evidence_strength == 0.2 &&
              contains(dream.tags, "dream:artifact") &&
              contains(dream.tags, "review:dream") &&
              contains(dream.tags, "dream:kind:pattern-discovery") &&
              contains(dream.tags, "research:pending"),
          "dream artifact lost its isolated lifecycle or provenance");
  const auto pending = database.reviews(scope, ReviewStatus::Pending, 20U);
  const auto& review = find_review(pending, review_id);
  require(review.kind == "dream_artifact" && review.subject_id == dream_id,
          "dream artifact did not enter the governed review queue");
  const Json stored_candidate = Json::parse(review.candidate_json);
  const Json& candidate =
      stored_candidate.contains("payload") &&
              stored_candidate.at("payload").is_object()
          ? stored_candidate.at("payload")
          : stored_candidate;
  const Json& candidate_source_ids =
      candidate.contains("source_memory_ids")
          ? candidate.at("source_memory_ids")
          : candidate.at("source_ids");
  require(candidate_source_ids.is_array() &&
              !candidate_source_ids.empty() &&
              candidate_source_ids.size() <= 8U,
          "dream review escaped the governed source bound");
  for (const auto& id : candidate_source_ids) {
    require(id.is_string() && contains(source_ids, id.get<std::string>()),
            "dream review persisted an ungoverned source id");
  }

  GraphQuery graph_query;
  graph_query.from_id = dream_id;
  graph_query.type = "derived_from";
  graph_query.limit = 20U;
  const auto edges = database.query_edges(scope, graph_query);
  require(!edges.empty() && edges.size() <= 8U,
          "dream source graph was missing or unbounded");

  RecallService recall(database, database, database);
  RecallRequest recall_request;
  recall_request.query = "artifact cache invalidation cause";
  recall_request.include_candidates = true;
  recall_request.candidate_limit = 100U;
  const auto recalled = recall.recall(scope, recall_request);
  require(std::none_of(recalled.begin(), recalled.end(),
                       [&](const RecalledMemory& item) {
                         return item.memory.id == dream_id;
                       }),
          "unreviewed dream artifact leaked into ordinary recall");

  const fs::path invocation_log = home / "dream-child-invocations.log";
  require(line_count(invocation_log) == 4U,
          "Dreaming trigger queue did not run exactly four isolated workers");
  {
    MemoryRuntime replay(runtime_options(home, child));
    require(canonical_scope(replay.scope()).repository == scope.repository &&
                canonical_scope(replay.scope()).branch == scope.branch,
            "dream restart changed canonical scope");
    require(replay.enqueue_dream_cycle("manual"),
            "manual Dreaming cycle after restart was not enqueued");
  }
  SQLiteMemoryDatabase restarted(home);
  require(events(restarted, scope, "memory_dream_claim").size() == 5U &&
              events(restarted, scope, "memory_dream_receipt").size() == 5U &&
              line_count(invocation_log) == 5U,
          "manual Dreaming cycle after restart was silently deduplicated");

  DreamResearchEvidence research;
  research.summary =
      "Validated build documentation confirms stale artifact caches can reuse "
      "incompatible generated files.";
  research.source_memory_ids = {source_ids.front()};
  research.tags = {"native-build", "research:probe"};
  research.provenance.source_type = SourceType::Document;
  research.provenance.source_uri =
      "https://example.test/native-build-cache";
  research.provenance.source_hash = "sha256:dreaming-probe";
  research.provenance.source_span = "artifact cache section";
  research.provenance.created_by = "user:dreaming-probe";
  research.provenance.evidence_strength = 0.75;
  research.confidence = 0.8;

  std::string researched_id;
  std::string research_review_id;
  {
    auto options = runtime_options(home, child);
    // The default review_only policy must permit review-driven promotion.
    options.dreaming.promotion_policy = "review_only";
    options.dreaming.research_policy = "manual";
    MemoryRuntime runtime(std::move(options));
    runtime.set_session_context("dream-session", "dream-agent");
    const auto visible_reviews = runtime.pending_reviews(20U);
    require(find_review(visible_reviews, review_id).kind == "dream_artifact",
            "canonical dream review was hidden by exact session scope");
    const auto result = runtime.add_dream_research(dream_id, research);
    require(result.id == dream_id && result.researched_memory_id.has_value() &&
                result.research_review_id.has_value(),
            "validated research did not create a separate reviewed successor");
    researched_id = *result.researched_memory_id;
    research_review_id = *result.research_review_id;

    ReviewAction action;
    action.kind = ReviewActionKind::Approve;
    action.actor = "user:dreaming-probe";
    action.note = "Accept only as an unvalidated hypothesis.";
    action.human_authority = true;
    auto preview = runtime.preview_review(review_id, action);
    require(preview.applicable,
            "accepted dream review did not produce an applicable preview");
    action = std::move(preview.action);
    action.expected_revision = preview.review_revision;
    action.idempotency_key = preview.idempotency_token;
    action.human_authority = true;
    const auto application =
        runtime.apply_review_action(review_id, std::move(action));
    require(application.status == ReviewApplicationStatus::Applied &&
                application.decision_persisted &&
                application.domain_mutations_applied &&
                runtime.memories().get(scope, dream_id).status ==
                    MemoryStatus::Hypothesis,
            "accepted dream review did not promote only to hypothesis");
  }

  SQLiteMemoryDatabase final_database(home);
  MemoryService final_memories(final_database);
  require(final_memories.get(scope, dream_id).status ==
              MemoryStatus::Hypothesis,
          "accepted dream outcome did not survive restart");
  const auto researched = final_memories.get(scope, researched_id);
  require(researched.type == MemoryType::Research &&
              researched.status != MemoryStatus::Active &&
              researched.status != MemoryStatus::Validated &&
              contains(researched.tags, "research:supplied") &&
              contains(researched.tags, "research:pending-validation") &&
              contains(researched.tags, "dream:researched") &&
              contains(researched.tags, "dream-source:" + dream_id),
          "research successor bypassed review or lost dream provenance");
  const auto pending_reviews =
      final_database.reviews(scope, ReviewStatus::Pending, 20U);
  const auto& research_review = find_review(pending_reviews, research_review_id);
  require(research_review.kind == "dream_research_successor" &&
              research_review.subject_id == researched_id,
          "research successor did not remain review-gated");
}

void verify_rejected_output_receipt(const fs::path& home) {
  fs::create_directories(home);
  const fs::path child = home / "dream-invalid-child.py";
  write_child(child, true);
  MemoryScope scope;
  {
    MemoryRuntime runtime(runtime_options(home, child));
    scope = canonical_scope(runtime.scope());
    (void)seed_sources(runtime, scope, 4U);
    require(runtime.enqueue_dream_cycle("manual"),
            "invalid-output dream cycle was not enqueued");
  }

  SQLiteMemoryDatabase database(home);
  const auto receipts = events(database, scope, "memory_dream_receipt");
  require(receipts.size() == 1U,
          "rejected dream output did not persist exactly one receipt");
  const Json receipt = Json::parse(receipts.front().payload_json);
  require(!receipt.value("error", std::string{}).empty() &&
              receipt.value("failure_reason", std::string{}) ==
                  "protocol_rejection" &&
              receipt.value("protocol_error", std::string{}) ==
                  "unknown_source_id" &&
              !receipt.value("model_output_hash", std::string{}).empty() &&
              receipt.value("model_output_chars", 0U) > 0U &&
              !receipt.value("review_created", true) &&
              receipt.value("dream_id", std::string{}).empty() &&
              receipt.value("review_id", std::string{}).empty(),
          "rejected ungoverned dream output mutated canonical state");
  MemoryQuery dreams;
  dreams.limit = 0U;
  dreams.statuses = {MemoryStatus::DreamArtifact};
  require(database.query(scope, dreams).empty() &&
              database.reviews(scope, ReviewStatus::Pending, 20U).empty(),
          "invalid dream output created durable memory or review state");
  const auto failed_stats =
      database.statistics(scope, RecallPolicy::Strict, false).dreaming;
  require(failed_stats.attempted == 1U && failed_stats.completed == 0U &&
              failed_stats.failed == 1U && failed_stats.pending == 0U &&
              failed_stats.protocol_rejections == 1U &&
              failed_stats.failures_by_reason.at("protocol_rejection") == 1U,
          "rejected Dreaming run statistics were not persisted");

  const fs::path invocation_log = home / "dream-child-invocations.log";
  require(line_count(invocation_log) == 1U,
          "invalid dream worker did not run exactly once");
  {
    MemoryRuntime replay(runtime_options(home, child));
    require(replay.enqueue_dream_cycle("manual"),
            "failed Dreaming cycle after restart was not enqueued");
  }
  SQLiteMemoryDatabase restarted(home);
  require(events(restarted, scope, "memory_dream_claim").size() == 2U &&
              events(restarted, scope, "memory_dream_receipt").size() == 2U &&
              line_count(invocation_log) == 2U,
          "failed Dreaming cycle after restart was silently deduplicated");
}

void verify_research_web_tool_isolation(const fs::path& home) {
  fs::create_directories(home);
  const fs::path child = home / "dream-research-child.py";
  write_child(child, false, true);
  const fs::path invocation_log = home / "dream-child-invocations.log";
  MemoryScope scope;
  {
    auto options = runtime_options(home, child);
    options.dreaming.research_policy = "required";
    MemoryRuntime runtime(std::move(options));
    scope = canonical_scope(runtime.scope());
    (void)seed_sources(runtime, scope, 4U);
    require(runtime.enqueue_dream_cycle("manual"),
            "research-enabled Dreaming cycle was not enqueued");
    for (std::size_t attempt = 0U;
         attempt < 60U && line_count(invocation_log) < 2U; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    require(line_count(invocation_log) == 2U,
            "research-enabled Dreaming did not run synthesis and research exactly once");
  }

  const auto invocations = json_lines(invocation_log);
  const auto synthesis = std::find_if(
      invocations.begin(), invocations.end(), [](const Json& invocation) {
        return invocation.value("phase", std::string{}) == "synthesis";
      });
  const auto research = std::find_if(
      invocations.begin(), invocations.end(), [](const Json& invocation) {
        return invocation.value("phase", std::string{}) == "research";
      });
  require(synthesis != invocations.end() &&
              synthesis->at("enabled_tools").empty(),
          "research policy leaked tools into Dreaming synthesis");
  require(research != invocations.end() &&
              research->at("enabled_tools").is_array() &&
              research->at("enabled_tools") ==
                  Json::array({"web_run", "web_search", "web_fetch"}),
          "Dreaming research did not receive all three web tools");
  for (const auto& tool : research->at("enabled_tools")) {
    require(tool.is_string() &&
                (tool == "web_run" || tool == "web_search" ||
                 tool == "web_fetch"),
            "Dreaming research received a non-web tool");
  }

  SQLiteMemoryDatabase database(home);
  const auto receipts = events(database, scope, "memory_dream_receipt");
  require(receipts.size() == 1U,
          "research-enabled Dreaming did not persist exactly one receipt");
  const Json receipt = Json::parse(receipts.front().payload_json);
  require(receipt.value("isolated_agent", false) &&
              receipt.value("review_created", false) &&
              receipt.value("research_attempted", false) &&
              receipt.value("research_completed", false) &&
              receipt.at("research_tools") ==
                  Json::array({"web_run", "web_search", "web_fetch"}) &&
              !receipt.value("research_id", std::string{}).empty() &&
              !receipt.value("research_review_id", std::string{}).empty() &&
              receipt.value("research_failure_reason", std::string{}).empty() &&
              receipt.value("error", std::string{}).empty(),
          "web-enabled Dreaming research escaped the isolated review path");
  require(!receipt.contains("auto_validation"),
          "review-only Dreaming unexpectedly invoked automatic verification");

  MemoryService memories(database);
  const auto research_memory =
      memories.get(scope, receipt.at("research_id").get<std::string>());
  require(research_memory.type == MemoryType::Research &&
              research_memory.status != MemoryStatus::Active &&
              research_memory.status != MemoryStatus::Validated &&
              contains(research_memory.tags, "research:web") &&
              contains(research_memory.tags, "research:isolated-agent") &&
              research_memory.provenance.source_type == SourceType::ToolResult &&
              research_memory.provenance.source_uri ==
                  "https://example.test/source",
          "web research bypassed review or lost verified tool provenance");
}

void verify_auto_validation_case(const fs::path& home,
                                 const std::string& verifier_mode,
                                 const std::string& verifier_model,
                                 const bool expect_validated,
                                 const bool missing_source_content = false) {
  fs::create_directories(home);
  const fs::path child = home / "dream-auto-validate-child.py";
  write_child(child, false, true, verifier_mode, missing_source_content);
  const fs::path invocation_log = home / "dream-child-invocations.log";
  MemoryScope scope;
  {
    auto options = runtime_options(home, child);
    options.dreaming.research_policy = "required";
    options.dreaming.promotion_policy = "auto_validate";
    options.dreaming.verifier_model = verifier_model;
    MemoryRuntime runtime(std::move(options));
    scope = canonical_scope(runtime.scope());
    (void)seed_sources(runtime, scope, 4U);
    require(runtime.enqueue_dream_cycle("manual"),
            "auto-validation Dreaming cycle was not enqueued");
    const std::size_t expected_invocations =
        (verifier_model == "llama/verifier" && !missing_source_content) ? 3U : 2U;
    for (std::size_t attempt = 0U;
         attempt < 60U && line_count(invocation_log) < expected_invocations;
         ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    require(line_count(invocation_log) == expected_invocations,
            "Dream verifier invocation count did not fail closed");
    for (std::size_t attempt = 0U;
         attempt < 60U && runtime.dreaming_status().running; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const auto status = runtime.dreaming_status();
    require(!status.running && status.completed_runs == 1U,
            "Dream verifier did not finish before runtime shutdown");
  }

  const auto invocations = json_lines(invocation_log);
  const auto verification_count = static_cast<std::size_t>(std::count_if(
      invocations.begin(), invocations.end(), [](const Json& invocation) {
        return invocation.value("phase", std::string{}) == "verify";
      }));
  require(verification_count ==
              ((verifier_model == "llama/verifier" && !missing_source_content)
                   ? 1U
                   : 0U),
          "remote or evidence-incomplete verifier unexpectedly spawned");

  SQLiteMemoryDatabase database(home);
  const auto receipts = events(database, scope, "memory_dream_receipt");
  require(receipts.size() == 1U,
          "auto-validation cycle did not persist exactly one receipt");
  const Json receipt = Json::parse(receipts.front().payload_json);
  require(receipt.contains("auto_validation") &&
              receipt.at("auto_validation").is_object(),
          "auto-validation receipt was not recorded");
  const Json& auto_validation = receipt.at("auto_validation");
  require(auto_validation.value("validated", false) == expect_validated,
          "auto-validation receipt had an unexpected validation outcome");
  require(auto_validation.value("attempted", false) ==
              (verifier_model == "llama/verifier" && !missing_source_content),
          "auto-validation receipt had an unexpected verifier attempt state");
  if (expect_validated) {
    require(auto_validation.value("reason", std::string{}) == "supported" &&
                auto_validation.contains("verification") &&
                auto_validation.at("verification").value("verdict", std::string{}) ==
                    "supported",
            "supported verifier outcome lost its durable receipt evidence");
  }

  MemoryService memories(database);
  const auto dream = memories.get(scope, receipt.at("dream_id").get<std::string>());
  require(dream.status == MemoryStatus::DreamArtifact,
          "automatic validation changed the original Dream artifact");
  const auto researched =
      memories.get(scope, receipt.at("research_id").get<std::string>());
  if (expect_validated) {
    require(researched.status == MemoryStatus::Validated &&
                researched.provenance.created_by == "memory_dream_verifier" &&
                contains(researched.tags, "validation:local-model"),
            "supported verifier outcome did not atomically validate successor");
    const auto accepted = database.reviews(scope, ReviewStatus::Accepted, 20U);
    const auto review = find_review(
        accepted, receipt.at("research_review_id").get<std::string>());
    require(review.revision == 1U && review.decided_by == "memory_dream_verifier",
            "automatic validation did not atomically accept revision-zero review");
  } else {
    require(researched.status == MemoryStatus::Candidate,
            "failed verification changed the research successor lifecycle");
    const auto pending = database.reviews(scope, ReviewStatus::Pending, 20U);
    (void)find_review(pending, receipt.at("research_review_id").get<std::string>());
  }
}

void verify_interval_idle_gate(const fs::path& home) {
  fs::create_directories(home);
  const fs::path child = home / "dream-idle-child.py";
  write_child(child, false);
  const fs::path invocation_log = home / "dream-child-invocations.log";
  {
    auto options = runtime_options(home, child);
    options.dreaming.trigger = "interval";
    options.dreaming.interval_seconds = 1;
    options.dreaming.idle_seconds = 2;
    MemoryRuntime runtime(std::move(options));
    const auto scope = canonical_scope(runtime.scope());
    (void)seed_sources(runtime, scope, 4U);

    runtime.set_foreground_active(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    require(line_count(invocation_log) == 0U,
            "interval Dreaming ran while foreground work was active");
    const auto active_status = runtime.dreaming_status();
    require(active_status.waiting_for_idle &&
                active_status.idle_for_seconds == 0,
            "interval Dreaming status did not expose its idle gate");

    runtime.set_foreground_active(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    require(line_count(invocation_log) == 0U,
            "interval Dreaming ignored the configured idle duration");
    for (std::size_t attempt = 0U;
         attempt < 60U && line_count(invocation_log) == 0U; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    require(line_count(invocation_log) == 1U,
            "interval Dreaming did not run after both timing gates elapsed");
    for (std::size_t attempt = 0U;
         attempt < 60U && line_count(invocation_log) < 2U; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    require(line_count(invocation_log) >= 2U,
            "interval Dreaming did not run again for an unchanged source pack");
  }
}

void verify_post_turn_idle_gate(const fs::path& home) {
  fs::create_directories(home);
  const fs::path child = home / "dream-post-turn-child.py";
  write_child(child, false);
  const fs::path invocation_log = home / "dream-child-invocations.log";
  {
    auto options = runtime_options(home, child);
    options.automatic_capture = true;
    options.dreaming.trigger = "post_turn";
    options.dreaming.idle_seconds = 2;
    MemoryRuntime runtime(std::move(options));
    const auto episode_scope = runtime.scope();
    const auto scope = canonical_scope(runtime.scope());
    (void)seed_sources(runtime, scope, 4U);

    runtime.set_foreground_active(true);
    qt::ai::Message message;
    message.role = qt::ai::MessageRole::user;
    message.content = {qt::ai::TextContent{
        "Decision: retain native Dreaming receipts for audit review.",
        std::nullopt}};
    CompletedEpisodeMetadata metadata;
    metadata.ok = true;
    metadata.turn_index = 17U;
    runtime.enqueue_completed_episode(
        {message}, "qt-session://dreaming/post-turn-idle/17", metadata);

    bool episode_persisted = false;
    for (std::size_t attempt = 0U; attempt < 60U && !episode_persisted;
         ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      SQLiteMemoryDatabase database(home);
      episode_persisted =
          !events(database, episode_scope, "completed_episode").empty();
    }
    require(episode_persisted,
            "post-turn idle probe did not persist its completed episode");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    require(line_count(invocation_log) == 0U,
            "post-turn Dreaming ran while foreground work was active");

    runtime.set_foreground_active(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    require(line_count(invocation_log) == 0U,
            "post-turn Dreaming ignored the configured idle duration");
    for (std::size_t attempt = 0U;
         attempt < 60U && line_count(invocation_log) == 0U; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    require(line_count(invocation_log) == 1U,
            "post-turn Dreaming did not run after the idle gate elapsed");
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 2, "usage: dreaming_probe <temporary-root>");
    const fs::path root(argv[1]);
    require(!root.empty(), "temporary root is required");
    fs::create_directories(root);
    verify_successful_cycle(root / "success-agent-home");
    verify_rejected_output_receipt(root / "failure-agent-home");
    verify_research_web_tool_isolation(root / "research-agent-home");
    verify_auto_validation_case(root / "auto-validated-agent-home", "supported",
                                "llama/verifier", true);
    verify_auto_validation_case(root / "auto-unsupported-agent-home", "unsupported",
                                "llama/verifier", false);
    verify_auto_validation_case(root / "auto-inconclusive-agent-home", "inconclusive",
                                "llama/verifier", false);
    verify_auto_validation_case(root / "auto-malformed-agent-home", "malformed",
                                "llama/verifier", false);
    verify_auto_validation_case(root / "auto-tool-use-agent-home", "tool_use",
                                "llama/verifier", false);
    verify_auto_validation_case(root / "auto-stale-subject-agent-home", "stale_subject",
                                "llama/verifier", false);
    verify_auto_validation_case(root / "auto-stale-review-agent-home", "stale_review",
                                "llama/verifier", false);
    verify_auto_validation_case(root / "auto-tampered-agent-home", "tamper_quote",
                                "llama/verifier", false);
    verify_auto_validation_case(root / "auto-remote-agent-home", "supported",
                                "openai/remote-verifier", false);
    verify_auto_validation_case(root / "auto-missing-source-agent-home", "supported",
                                "llama/verifier", false, true);
    verify_interval_idle_gate(root / "idle-agent-home");
    verify_post_turn_idle_gate(root / "post-turn-idle-agent-home");
    std::cout << "dreaming probe passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "dreaming probe failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
