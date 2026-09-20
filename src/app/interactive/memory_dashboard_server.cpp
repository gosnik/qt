#include "qt/app/interactive/memory_dashboard_server.hpp"

#include "generated/memory_dashboard_embedded_assets.hpp"

#include "qt/foundation/process.hpp"
#include "qt/foundation/uuid.hpp"
#include "qt/memory/review_json.hpp"
#include "qt/memory/retrieval_explanation.hpp"
#include "qt/memory/runtime.hpp"

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace qt::app {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaxRequestBytes = 64U * 1024U;

std::string utf8_prefix(const std::string &value,
                        const std::size_t maximum_bytes) {
  if (value.size() <= maximum_bytes)
    return value;
  std::size_t end = maximum_bytes;
  while (end > 0U &&
         (static_cast<unsigned char>(value[end]) & 0xc0U) == 0x80U) {
    --end;
  }
  return value.substr(0U, end);
}

std::string safe_json_dump(const Json &value) noexcept {
  try {
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
  } catch (...) {
    return R"({"error":"JSON response serialization failed"})";
  }
}

std::string lowercase(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

std::string trim(std::string value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

std::string query_value(std::string_view target, std::string_view name) {
  const auto question = target.find('?');
  if (question == std::string_view::npos)
    return {};
  std::string_view query = target.substr(question + 1U);
  while (!query.empty()) {
    const auto ampersand = query.find('&');
    const auto field = query.substr(0U, ampersand);
    const auto equals = field.find('=');
    if (equals != std::string_view::npos && field.substr(0U, equals) == name) {
      return std::string(field.substr(equals + 1U));
    }
    if (ampersand == std::string_view::npos)
      break;
    query.remove_prefix(ampersand + 1U);
  }
  return {};
}

std::string target_path(std::string_view target) {
  const auto question = target.find('?');
  return std::string(target.substr(0U, question));
}

std::string header_value(const std::string &request, const std::string &name) {
  std::istringstream lines(request);
  std::string line;
  const std::string wanted = lowercase(name);
  std::getline(lines, line);
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty())
      break;
    const auto colon = line.find(':');
    if (colon != std::string::npos &&
        lowercase(trim(line.substr(0U, colon))) == wanted) {
      return trim(line.substr(colon + 1U));
    }
  }
  return {};
}

void send_all(int socket, const std::string &value) {
  std::string_view remaining(value);
  while (!remaining.empty()) {
#if defined(MSG_NOSIGNAL)
    const ssize_t written =
        ::send(socket, remaining.data(), remaining.size(), MSG_NOSIGNAL);
#else
    const ssize_t written =
        ::send(socket, remaining.data(), remaining.size(), 0);
#endif
    if (written > 0) {
      remaining.remove_prefix(static_cast<std::size_t>(written));
      continue;
    }
    if (written < 0 && errno == EINTR)
      continue;
    break;
  }
}

void send_response(int socket, int status, std::string_view reason,
                   std::string_view content_type, const std::string &body,
                   bool gzip_encoded = false) {
  std::ostringstream response;
  response
      << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
      << "Content-Type: " << content_type << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Cache-Control: no-store\r\n"
      << "Content-Security-Policy: default-src 'self'; style-src "
         "'unsafe-inline'; script-src 'self' 'unsafe-inline' 'unsafe-eval' "
         "'wasm-unsafe-eval'; connect-src 'self'; img-src 'self' data:\r\n"
      << "Referrer-Policy: no-referrer\r\n"
      << "X-Content-Type-Options: nosniff\r\n"
      << "X-Frame-Options: DENY\r\n"
      << (gzip_encoded ? "Content-Encoding: gzip\r\n" : "")
      << "Connection: close\r\n\r\n";
  response.write(body.data(), static_cast<std::streamsize>(body.size()));
  send_all(socket, response.str());
}

Json statistics_json(const qt::memory::MemoryStatistics &stats) {
  return Json{{"catalog",
               {{"total", stats.memory_entries},
                {"by_type", stats.memory_counts},
                {"by_status", stats.trust_states},
                {"hypotheses", stats.hypotheses},
                {"low_confidence", stats.low_confidence},
                {"unprovenanced", stats.unprovenanced},
                {"deprecated", stats.deprecated},
                {"failed_procedures", stats.failed_procedures},
                {"high_use_hypotheses", stats.high_use_hypotheses}}},
              {"usage",
               {{"accessed_memories", stats.accessed_memories},
                {"total_accesses", stats.total_accesses},
                {"successful_outcomes", stats.successful_outcomes},
                {"failed_outcomes", stats.failed_outcomes}}},
              {"governance",
               {{"conflicts", stats.conflict_count},
                {"unresolved_conflicts", stats.unresolved_conflicts},
                {"pending_reviews", stats.pending_reviews}}}};
}

Json snapshot_json(const std::shared_ptr<qt::memory::MemoryRuntime> &runtime) {
  if (!runtime) {
    return Json{
        {"available", false},
        {"error", "Built-in memory is inactive. Enable it and restart qt."}};
  }

  qt::memory::MemoryQuery query;
  query.statuses = {
      qt::memory::MemoryStatus::Candidate,
      qt::memory::MemoryStatus::Hypothesis,
      qt::memory::MemoryStatus::Active,
      qt::memory::MemoryStatus::Validated,
      qt::memory::MemoryStatus::Deprecated,
      qt::memory::MemoryStatus::Superseded,
      qt::memory::MemoryStatus::Rejected,
      qt::memory::MemoryStatus::Expired,
      qt::memory::MemoryStatus::DreamArtifact,
  };
  query.policy = qt::memory::RecallPolicy::Broad;
  query.limit = 500U;

  Json memories = Json::array();
  for (const auto &memory :
       runtime->memories().query(runtime->scope(), query)) {
    memories.push_back(qt::memory::review_memory_json(memory));
  }

  qt::memory::ReviewQuery review_query;
  review_query.statuses = {qt::memory::ReviewStatus::Pending,
                           qt::memory::ReviewStatus::Deferred};
  review_query.include_due_deferred = true;
  review_query.limit = 200U;
  Json reviews = Json::array();
  for (const auto &review : runtime->review_items(review_query)) {
    reviews.push_back(qt::memory::review_item_json(review));
  }

  const auto &scope = runtime->scope();
  const auto dreaming = runtime->dreaming_status();
  const auto retrieval = runtime->retrieval_status();
  return Json{{"available", true},
              {"schema_version", runtime->schema_version()},
              {"scope",
               {{"kind", qt::memory::to_string(scope.kind)},
                {"project", scope.project},
                {"repository", scope.repository_display},
                {"branch", scope.branch},
                {"session", scope.session}}},
              {"statistics", statistics_json(runtime->statistics())},
              {"review_statistics", qt::memory::review_statistics_json(
                                        runtime->review_statistics())},
              {"dreaming",
               {{"enabled", dreaming.enabled},
                {"trigger", dreaming.trigger},
                {"running", dreaming.running},
                {"pending", dreaming.pending_runs},
                {"completed", dreaming.completed_runs},
                {"failed", dreaming.failed_runs},
                {"last_error", dreaming.last_error}}},
              {"retrieval",
               {{"enabled", retrieval.enabled},
                {"running", retrieval.running},
                {"generation", retrieval.generation},
                {"last_error", retrieval.last_error},
                {"completed_jobs", retrieval.completed_jobs},
                {"jobs", {{"pending", retrieval.jobs.pending},
                           {"leased", retrieval.jobs.leased},
                           {"failed", retrieval.jobs.failed},
                           {"complete", retrieval.jobs.complete},
                           {"chunks", retrieval.jobs.chunks}}},
                {"queries", {{"cache_hits", retrieval.queries.cache_hits},
                              {"embedding_failures", retrieval.queries.embedding_failures},
                              {"cache_entries", retrieval.queries.cache_entries},
                              {"cache_bytes", retrieval.queries.cache_bytes},
                              {"last_error", retrieval.queries.last_error}}},
                {"code", {{"enabled", retrieval.code.enabled},
                           {"available", retrieval.code.available},
                           {"stale", retrieval.code.stale},
                           {"generation", retrieval.code.generation},
                           {"repository", retrieval.code.repository},
                           {"worktree", retrieval.code.worktree}}}}},
              {"memories", std::move(memories)},
              {"reviews", std::move(reviews)}};
}

std::int64_t timestamp(qt::memory::TimePoint value) {
  if (value == qt::memory::TimePoint{})
    return 0;
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             value.time_since_epoch())
      .count();
}

Json compat_memory_json(const qt::memory::MemoryEntry &memory) {
  const bool requires_dream_review =
      memory.status == qt::memory::MemoryStatus::DreamArtifact ||
      memory.type == qt::memory::MemoryType::Dream ||
      memory.type == qt::memory::MemoryType::Research;
  return Json{
      {"id", memory.id},
      {"project", memory.scope.project},
      {"repository", memory.scope.repository_display},
      {"branch", memory.scope.branch},
      {"session", memory.scope.session},
      {"agent_id", memory.scope.agent},
      {"memory_type", qt::memory::to_string(memory.type)},
      {"status", qt::memory::to_string(memory.status)},
      {"requires_dream_review", requires_dream_review},
      {"content", memory.content},
      {"token_cost",
       {{"content_bytes", memory.content.size()},
        {"estimated_input_tokens", (memory.content.size() + 3U) / 4U},
        {"exact", false},
        {"method", "utf8_bytes_div_4"}}},
      {"source", memory.source},
      {"source_type", qt::memory::to_string(memory.provenance.source_type)},
      {"source_uri", memory.provenance.source_uri},
      {"source_hash", memory.provenance.source_hash},
      {"source_span", memory.provenance.source_span},
      {"created_by", memory.provenance.created_by},
      {"tags", memory.tags},
      {"confidence", memory.confidence},
      {"quality_score", memory.quality_score},
      {"access_count", memory.access_count},
      {"success_count", memory.success_count},
      {"failure_count", memory.failure_count},
      {"replacement_id", memory.replacement_id},
      {"superseded_by", memory.superseded_by},
      {"created_at", timestamp(memory.created_at)},
      {"updated_at", timestamp(memory.updated_at)},
      {"last_accessed_at", timestamp(memory.last_accessed_at)}};
}

bool is_dream_review(const qt::memory::ReviewItem &review) {
  return review.kind == "dream_artifact" ||
         review.kind == "dream_research_successor";
}

qt::memory::ReviewItem pending_dream_review_for_subject(
    const std::shared_ptr<qt::memory::MemoryRuntime> &runtime,
    const std::string &subject_id) {
  if (subject_id.empty()) {
    throw std::invalid_argument("dashboard review preview requires string id");
  }
  qt::memory::ReviewQuery query;
  query.statuses = {qt::memory::ReviewStatus::Pending};
  query.subject_id = subject_id;
  query.kinds = {"dream_artifact", "dream_research_successor"};
  query.scope = qt::memory::ReviewQuery::ScopeSelector::All;
  query.limit = 2U;
  std::optional<qt::memory::ReviewItem> match;
  for (const auto &review : runtime->review_items(query)) {
    if (review.subject_id != subject_id || !is_dream_review(review))
      continue;
    if (match) {
      throw std::invalid_argument(
          "multiple pending governed Dream reviews match this memory");
    }
    match = review;
  }
  if (!match) {
    throw std::invalid_argument(
        "no pending governed Dream review exists for this memory in the current runtime scope");
  }
  return *match;
}

qt::memory::ReviewItem dream_review_by_id(
    const std::shared_ptr<qt::memory::MemoryRuntime> &runtime,
    const std::string &review_id) {
  if (review_id.empty()) {
    throw std::invalid_argument(
        "dashboard review apply requires string review_id");
  }
  const auto detail = runtime->review_detail(review_id);
  if (!is_dream_review(detail.item)) {
    throw std::invalid_argument("review_id is not a governed Dream review");
  }
  return detail.item;
}

qt::memory::ReviewAction dashboard_dream_review_action(
    const Json &input, const std::string &actor, const bool human_authority) {
  if (!input.contains("action") || !input.at("action").is_string()) {
    throw std::invalid_argument("dashboard review requires action approve or reject");
  }
  const auto action_name = input.at("action").get<std::string>();
  if (action_name != "approve" && action_name != "reject") {
    throw std::invalid_argument("dashboard Dream review action must be approve or reject");
  }
  Json sanitized{{"action", action_name}};
  if (human_authority) {
    if (!input.at("expected_revision").is_number_unsigned() ||
        !input.at("idempotency_key").is_string() ||
        input.at("idempotency_key").get<std::string>().empty()) {
      throw std::invalid_argument(
          "dashboard review apply requires an unsigned preview revision and non-empty idempotency key");
    }
    sanitized["expected_revision"] = input.at("expected_revision");
    sanitized["idempotency_key"] = input.at("idempotency_key");
  }
  return qt::memory::review_action_from_json(sanitized, actor, human_authority);
}

std::vector<qt::memory::MemoryStatus> all_memory_statuses() {
  return {qt::memory::MemoryStatus::Candidate,
          qt::memory::MemoryStatus::Hypothesis,
          qt::memory::MemoryStatus::Active,
          qt::memory::MemoryStatus::Validated,
          qt::memory::MemoryStatus::Deprecated,
          qt::memory::MemoryStatus::Superseded,
          qt::memory::MemoryStatus::Rejected,
          qt::memory::MemoryStatus::Expired,
          qt::memory::MemoryStatus::DreamArtifact};
}

std::vector<qt::memory::MemoryEntry>
dashboard_memories(const std::shared_ptr<qt::memory::MemoryRuntime> &runtime,
                   const Json &input = Json::object()) {
  qt::memory::MemoryQuery query;
  query.text = input.value("query", std::string{});
  query.policy = qt::memory::RecallPolicy::Broad;
  query.limit = std::clamp<std::size_t>(input.value("limit", 200U), 1U, 500U);
  query.include_superseded = input.value("include_superseded", true);
  const std::string status = input.value("status", std::string{});
  query.statuses = status.empty()
                       ? all_memory_statuses()
                       : std::vector<qt::memory::MemoryStatus>{
                             qt::memory::memory_status_from_string(status)};
  const std::string type = input.value(
      "memory_type", input.value("type", std::string{}));
  if (!type.empty())
    query.type = qt::memory::memory_type_from_string(type);
  return runtime->memories().query(runtime->scope(), query);
}

Json dashboard_summary_json(
    const std::shared_ptr<qt::memory::MemoryRuntime> &runtime) {
  const auto stats = runtime->statistics();
  const int penalty = static_cast<int>(stats.hypotheses) * 2 +
                      static_cast<int>(stats.low_confidence) * 3 +
                      static_cast<int>(stats.unprovenanced) * 2 +
                      static_cast<int>(stats.deprecated) * 2 +
                      static_cast<int>(stats.unresolved_conflicts) * 10;
  Json urgent = Json::array();
  const auto add_urgent = [&urgent](const char *type, const char *title,
                                    std::size_t count) {
    if (count != 0U) {
      urgent.push_back(
          Json{{"type", type},
               {"title", title},
               {"priority", "high"},
               {"count", count},
               {"message", std::to_string(count) + " item(s) need attention"}});
    }
  };
  add_urgent("conflict", "Conflicting memories need resolution",
             stats.unresolved_conflicts);
  add_urgent("review", "Governed reviews are pending", stats.pending_reviews);
  add_urgent("provenance", "Memories are missing provenance",
             stats.unprovenanced);
  const auto &scope = runtime->scope();
  const auto retrieval = runtime->retrieval_status();
  const int health = std::max(0, 100 - penalty);
  const Json retrieval_json = Json{
      {"enabled", retrieval.enabled}, {"running", retrieval.running},
      {"generation", retrieval.generation}, {"last_error", retrieval.last_error},
      {"completed_jobs", retrieval.completed_jobs},
      {"jobs", Json{{"pending", retrieval.jobs.pending}, {"leased", retrieval.jobs.leased},
                     {"failed", retrieval.jobs.failed}, {"complete", retrieval.jobs.complete},
                     {"chunks", retrieval.jobs.chunks}}},
      {"queries", Json{{"cache_hits", retrieval.queries.cache_hits},
                        {"embedding_failures", retrieval.queries.embedding_failures},
                        {"cache_entries", retrieval.queries.cache_entries},
                        {"cache_bytes", retrieval.queries.cache_bytes},
                        {"last_error", retrieval.queries.last_error}}},
      {"code", Json{{"enabled", retrieval.code.enabled},
                     {"available", retrieval.code.available},
                     {"stale", retrieval.code.stale},
                     {"generation", retrieval.code.generation},
                     {"repository", retrieval.code.repository},
                     {"worktree", retrieval.code.worktree}}}};
  return Json{{"dashboard_summary",
               {{"project", scope.project},
                {"scope", "current_runtime"},
                {"projects", Json::array({scope.project})},
                {"health",
                 {{"project", scope.project},
                  {"score", health},
                  {"memory_entries", stats.memory_entries}}},
                {"health_score", health},
                {"memory_counts", stats.memory_counts},
                {"trust_states", stats.trust_states},
                {"urgent_reviews", std::move(urgent)},
                {"recent_activity", Json::array()},
                {"memory_entries", stats.memory_entries},
                {"conflict_count", stats.conflict_count},
                {"ranking_policy", "see retrieval query score breakdowns"},
                {"retrieval", retrieval_json}}}};
}

Json compatibility_response(
    const std::shared_ptr<qt::memory::MemoryRuntime> &runtime,
    const std::string &method, const std::string &path, const Json &input) {
  if (!runtime) {
    throw std::runtime_error("built-in memory is inactive");
  }
  if (method == "GET" && path == "/mcp/status") {
    return Json{{"service", "qt-memory-dashboard"},
                {"version", "native"},
                {"state_schema_version", runtime->schema_version()},
                {"bind_address", "127.0.0.1"},
                {"auth_required", true},
                {"memory_entries", runtime->memory_count()}};
  }
  if (method == "GET" && path == "/mcp/metrics") {
    const auto stats = runtime->statistics();
    return Json{{"memory_entries", stats.memory_entries},
                {"pending_reviews", stats.pending_reviews},
                {"total_accesses", stats.total_accesses}};
  }
  if (method == "GET" && path == "/mcp/logs") {
    return Json{{"entries", Json::array()}, {"count", 0}};
  }
  if (method == "GET" && path == "/mcp/ai/config")
    return Json::object();
  if (method == "GET" && path == "/mcp/ai/metadata") {
    return Json{{"services", Json::array()},
                {"reasoning_efforts", Json::array()},
                {"reasoning_summaries", Json::array()}};
  }
  if (method == "POST" && path == "/dashboard/summary") {
    return dashboard_summary_json(runtime);
  }
  if (method == "POST" && path == "/dashboard/retrieval/rebuild") {
    if (!input.is_object() || !input.value("confirm", false))
      throw std::invalid_argument("retrieval rebuild requires confirm=true");
    std::optional<std::string> owner_key;
    if (input.contains("owner_key")) {
      if (!input.at("owner_key").is_string())
        throw std::invalid_argument("retrieval rebuild owner_key must be a string");
      owner_key = input.at("owner_key").get<std::string>();
    }
    const auto report = runtime->rebuild_retrieval(std::move(owner_key));
    return Json{{"retrieval_rebuild", true},
                {"owners", report.owners},
                {"chunks_invalidated", report.chunks_invalidated},
                {"vectors_invalidated", report.vectors_invalidated},
                {"jobs_superseded", report.jobs_superseded},
                {"jobs_enqueued", report.jobs_enqueued}};
  }
  if (method == "POST" && path == "/dashboard/review/preview") {
    if (!input.is_object() || !input.contains("id") ||
        !input.at("id").is_string()) {
      throw std::invalid_argument(
          "dashboard review preview requires string id and action");
    }
    const auto review = pending_dream_review_for_subject(
        runtime, input.at("id").get<std::string>());
    auto action = dashboard_dream_review_action(
        input, "dashboard:user", false);
    return qt::memory::review_preview_json(
        runtime->preview_review(review.id, std::move(action)));
  }
  if (method == "POST" && path == "/dashboard/review/apply") {
    if (!input.is_object() || !input.value("confirm", false) ||
        !input.contains("review_id") || !input.at("review_id").is_string() ||
        !input.contains("expected_revision") ||
        !input.contains("idempotency_key")) {
      throw std::invalid_argument(
          "dashboard review apply requires review_id, preview revision, "
          "idempotency key, action, and confirm=true");
    }
    const auto review = dream_review_by_id(
        runtime, input.at("review_id").get<std::string>());
    auto action = dashboard_dream_review_action(input, "dashboard:user", true);
    return qt::memory::review_application_json(
        runtime->apply_review_action(review.id, std::move(action)));
  }
  if (method == "POST" && path == "/mcp/memory/query") {
    Json entries = Json::array();
    for (const auto &memory : dashboard_memories(runtime, input)) {
      Json entry = compat_memory_json(memory);
      const auto revision = runtime->current_retrieval_revision(
          memory.id, qt::memory::RecallPolicy::Broad, false);
      entry["revision"] = Json{{"historical", false},
                                {"number", revision ? Json(*revision)
                                                       : Json(nullptr)}};
      entries.push_back(std::move(entry));
    }
    return Json{{"entries", std::move(entries)}};
  }
  if (method == "POST" && path == "/mcp/memory/write") {
    qt::memory::MemoryWrite write;
    write.content = input.value("content", std::string{});
    write.type = qt::memory::memory_type_from_string(
        input.value("type", std::string("observation")));
    const std::string requested_status =
        input.value("status", std::string("hypothesis"));
    write.status = requested_status == "candidate"
                       ? qt::memory::MemoryStatus::Candidate
                       : qt::memory::MemoryStatus::Hypothesis;
    write.source = input.value("source", std::string("dashboard"));
    write.confidence = input.value("confidence", 0.5);
    write.provenance.source_type = qt::memory::source_type_from_string(
        input.value("source_type", std::string("manual")));
    write.provenance.source_uri = input.value("source_uri", std::string{});
    write.provenance.created_by = "user";
    if (input.contains("tags") && input.at("tags").is_array()) {
      write.tags = input.at("tags").get<std::vector<std::string>>();
    }
    const std::string id =
        runtime->memories().remember(runtime->scope(), std::move(write));
    return Json{{"id", id},
                {"entry", compat_memory_json(
                              runtime->memories().get(runtime->scope(), id))}};
  }
  if (method == "POST" &&
      (path == "/mcp/memory/promote" || path == "/mcp/memory/deprecate" ||
       path == "/mcp/memory/reject")) {
    qt::memory::LifecycleRequest request;
    request.id = input.value("id", std::string{});
    request.status =
        path == "/mcp/memory/promote"     ? qt::memory::MemoryStatus::Active
        : path == "/mcp/memory/deprecate" ? qt::memory::MemoryStatus::Deprecated
                                          : qt::memory::MemoryStatus::Rejected;
    request.actor = "dashboard:user";
    request.reason = "explicit memory dashboard action";
    try {
      (void)pending_dream_review_for_subject(runtime, request.id);
      throw std::invalid_argument(
          "Dream-derived memories require governed dashboard review preview "
          "and apply with explicit confirmation");
    } catch (const std::invalid_argument &error) {
      if (std::string_view(error.what()) !=
          "no pending governed Dream review exists for this memory in the current runtime scope") {
        throw;
      }
    }
    const auto subject = runtime->memories().get(runtime->scope(), request.id);
    if (subject.status == qt::memory::MemoryStatus::DreamArtifact ||
        subject.type == qt::memory::MemoryType::Dream ||
        subject.type == qt::memory::MemoryType::Research) {
      throw std::invalid_argument(
          "Dream-derived memories require governed dashboard review preview "
          "and apply with explicit confirmation");
    }
    const auto memory =
        runtime->governance().transition(runtime->scope(), request);
    return Json{{"entry", compat_memory_json(memory)}, {"updated", true}};
  }
  if (method == "POST" && path == "/mcp/memory/validate") {
    throw std::invalid_argument(
        "validation requires evidence and a governed /memory review action");
  }
  if (method == "POST" && path == "/mcp/memory/forget") {
    throw std::invalid_argument(
        "deletion requires `/memory forget <id> confirm`");
  }
  if (method == "POST" && path == "/mcp/memory/process") {
    qt::memory::IngestionRequest request;
    request.content = input.value("content", std::string{});
    request.source = input.value("source", std::string("dashboard:text"));
    request.provenance.source_type = qt::memory::source_type_from_string(
        input.value("source_type", std::string("processed_text")));
    request.provenance.source_uri = input.value("source_uri", std::string{});
    request.provenance.created_by = "user";
    request.minimum_confidence = input.value("confidence", 0.6);
    if (input.contains("tags") && input.at("tags").is_array()) {
      request.tags = input.at("tags").get<std::vector<std::string>>();
    }
    const bool persist = !input.value("dry_run", true);
    const auto ingested =
        runtime->ingestion().ingest(runtime->scope(), request, persist);
    Json candidates = Json::array();
    for (const auto &candidate : ingested.candidates) {
      candidates.push_back(
          Json{{"content", candidate.content},
               {"memory_type", qt::memory::to_string(candidate.type)},
               {"tags", candidate.tags},
               {"confidence", candidate.confidence}});
    }
    return Json{{"count", ingested.candidates.size()},
                {"candidates", std::move(candidates)},
                {"stored_ids", ingested.stored_ids},
                {"dry_run", !persist}};
  }
  if (method == "POST" && path == "/dashboard/memory") {
    const std::string id = input.value("id", std::string{});
    const auto memory = runtime->memories().get(runtime->scope(), id);
    const auto revisions = runtime->memories().revisions(runtime->scope(), id);
    return Json{
        {"memory",
         {{"entry", compat_memory_json(memory)},
          {"supersession_chain", Json::array({compat_memory_json(memory)})},
          {"revision_count", revisions.size()}}}};
  }
  if (method == "POST" && path == "/dashboard/memory/history") {
    const std::string id = input.value("id", std::string{});
    Json revisions = Json::array();
    for (const auto &revision :
         runtime->memories().revisions(runtime->scope(), id)) {
      revisions.push_back(
          Json{{"id", revision.id},
               {"memory_id", revision.memory_id},
               {"kind", revision.kind},
               {"entry", Json::parse(revision.entry_json, nullptr, false)},
               {"created_at", timestamp(revision.created_at)}});
    }
    const auto count = revisions.size();
    return Json{{"memory_id", id},
                {"revisions", std::move(revisions)},
                {"count", count}};
  }
  if (method == "POST" && path == "/dashboard/graph") {
    const auto memories = dashboard_memories(runtime, input);
    Json nodes = Json::array();
    for (const auto &memory : memories) {
      nodes.push_back(Json{{"id", memory.id},
                           {"label", utf8_prefix(memory.content, 56U)},
                           {"summary", utf8_prefix(memory.content, 180U)},
                           {"content", memory.content},
                           {"memory_type", qt::memory::to_string(memory.type)},
                           {"status", qt::memory::to_string(memory.status)},
                           {"search_text", memory.content}});
    }
    qt::memory::GraphQuery query;
    query.limit = std::clamp<std::size_t>(input.value("limit", 250U), 1U, 500U);
    query.type = input.value("edge_type", std::string{});
    Json edges = Json::array();
    for (const auto &edge : runtime->dashboard_graph_edges(query)) {
      edges.push_back(Json{{"id", edge.id},
                           {"from_id", edge.from_id},
                           {"to_id", edge.to_id},
                           {"type", edge.type},
                           {"confidence", edge.confidence}});
    }
    const auto edge_count = edges.size();
    return Json{
        {"graph",
          {{"nodes", std::move(nodes)},
          {"edges", std::move(edges)},
          {"groups", Json::array()},
          {"stats", {{"nodes", memories.size()}, {"edges", edge_count}}}}}};
  }
  if (method == "POST" && path == "/dashboard/sources") {
    Json sources = Json::array();
    for (const auto &source : runtime->dashboard_sources()) {
      sources.push_back(
          Json{{"source_id", source.id},
               {"source_type", qt::memory::to_string(source.type)},
               {"source_uri", source.uri},
               {"display_name", source.display_name},
               {"trust_level", source.trust_level},
               {"hash_status", source.hash_status},
               {"ingest_count", source.ingest_count},
               {"last_checked_at", timestamp(source.last_checked_at)}});
    }
    return Json{{"sources", std::move(sources)}};
  }
  if (method == "POST" && path == "/dashboard/source") {
    const std::string id = input.value("source_id", std::string{});
    auto sources = runtime->dashboard_sources();
    const auto found =
        std::find_if(sources.begin(), sources.end(),
                     [&id](const auto &source) { return source.id == id; });
    if (found == sources.end())
      throw std::invalid_argument("source not found");
    Json history = Json::array();
    for (const auto &event : runtime->dashboard_source_events(id)) {
      history.push_back(Json{{"id", event.id},
                             {"action", event.action},
                             {"status", event.status},
                             {"note", event.note},
                             {"created_at", timestamp(event.created_at)}});
    }
    return Json{{"source",
                 {{"source_id", found->id},
                  {"source_type", qt::memory::to_string(found->type)},
                  {"source_uri", found->uri},
                  {"display_name", found->display_name},
                  {"trust_level", found->trust_level},
                  {"hash_status", found->hash_status},
                  {"history", std::move(history)}}}};
  }
  if (method == "POST" &&
      (path == "/dashboard/agents" || path == "/dashboard/sessions")) {
    const bool agents = path == "/dashboard/agents";
    std::map<std::string, std::pair<std::size_t, std::int64_t>> grouped;
    for (const auto &memory : dashboard_memories(runtime)) {
      std::string key = agents ? memory.scope.agent : memory.scope.session;
      if (key.empty())
        key = "unknown";
      auto &value = grouped[key];
      ++value.first;
      value.second = std::max(value.second, timestamp(memory.updated_at));
    }
    Json values = Json::array();
    for (const auto &[id, value] : grouped) {
      values.push_back(agents ? Json{{"agent_id", id},
                                     {"memories_written", value.first},
                                     {"session_count", 0},
                                     {"memory_recall_events", 0},
                                     {"context_builds", 0},
                                     {"last_active", value.second}}
                              : Json{{"session", id},
                                     {"memories_written", value.first},
                                     {"agent_count", 0},
                                     {"memory_recall_events", 0},
                                     {"context_builds", 0},
                                     {"last_active", value.second}});
    }
    return agents
               ? Json{{"agents", std::move(values)}, {"count", grouped.size()}}
               : Json{{"sessions", std::move(values)},
                      {"count", grouped.size()}};
  }
  if (method == "POST" &&
      (path == "/dashboard/agent" || path == "/dashboard/session")) {
    const bool agent = path == "/dashboard/agent";
    const std::string id =
        input.value(agent ? "agent_id" : "session", std::string{});
    Json memories = Json::array();
    for (const auto &memory : dashboard_memories(runtime)) {
      const std::string &candidate =
          agent ? memory.scope.agent : memory.scope.session;
      if ((candidate.empty() ? std::string("unknown") : candidate) == id) {
        memories.push_back(compat_memory_json(memory));
      }
    }
    if (memories.empty())
      throw std::invalid_argument("dashboard group not found");
    Json summary =
        agent ? Json{{"agent_id", id}, {"memories_written", memories.size()}}
              : Json{{"session", id}, {"memories_written", memories.size()}};
    return agent ? Json{{"agent", std::move(summary)},
                        {"memories", std::move(memories)},
                        {"activity", Json::array()}}
                 : Json{{"session", std::move(summary)},
                        {"memories", std::move(memories)},
                        {"activity", Json::array()}};
  }
  if (method == "POST" && path == "/mcp/postprocess/scheduler/status") {
    const auto dreaming = runtime->dreaming_status();
    return Json{{"running", dreaming.running},
                {"enabled", dreaming.enabled},
                {"completed_runs", dreaming.completed_runs},
                {"failed_runs", dreaming.failed_runs}};
  }
  if (method == "POST" && path == "/context/build") {
    const auto parse_string = [&input](const char *key) {
      if (!input.contains(key)) return std::string{};
      if (!input.at(key).is_string())
        throw std::invalid_argument(std::string("context build ") + key +
                                    " must be a string");
      return input.at(key).get<std::string>();
    };
    qt::memory::RecallRequest request;
    request.query = parse_string("query");
    if (request.query.empty()) request.query = parse_string("task");
    if (request.query.empty())
      throw std::invalid_argument("context build requires a query");
    const std::string policy = parse_string("recall_policy");
    request.policy = runtime->coordinator().settings().recall_policy;
    if (policy == "strict") request.policy = qt::memory::RecallPolicy::Strict;
    else if (policy == "balanced") request.policy = qt::memory::RecallPolicy::Balanced;
    else if (policy == "broad") request.policy = qt::memory::RecallPolicy::Broad;
    else if (!policy.empty()) throw std::invalid_argument("context build recall_policy must be strict, balanced, or broad");
    if (input.contains("include_global_user") &&
        !input.at("include_global_user").is_boolean())
      throw std::invalid_argument("context build include_global_user must be boolean");
    request.include_global_user = input.value("include_global_user", false);
    for (const auto* key : {"candidate_limit", "result_limit", "budget_tokens"})
      if (input.contains(key) && !input.at(key).is_number_integer())
        throw std::invalid_argument(std::string("context build ") + key + " must be an integer");
    request.candidate_limit = input.value("candidate_limit", std::size_t{50});
    request.result_limit = input.value("result_limit", std::size_t{20});
    const auto structured_string = [&parse_string](const char *key) {
      return parse_string(key);
    };
    request.structured.subject = structured_string("subject");
    request.structured.predicate = structured_string("predicate");
    request.structured.object = structured_string("object");
    if (!request.structured.subject.empty() || !request.structured.predicate.empty() ||
        !request.structured.object.empty()) request.structured.enabled = true;
    if (input.contains("as_of")) {
      if (!input.at("as_of").is_number_integer())
        throw std::invalid_argument("context build as_of must be epoch seconds");
      if (input.at("as_of").is_number_unsigned() &&
              input.at("as_of").get<std::uint64_t>() > static_cast<std::uint64_t>(
                  std::chrono::duration_cast<std::chrono::seconds>(qt::memory::TimePoint::duration::max()).count()))
            throw std::invalid_argument("as_of is outside supported time range");
          const auto seconds = input.at("as_of").get<std::int64_t>();
      const auto max_seconds = std::chrono::duration_cast<std::chrono::seconds>(
          qt::memory::TimePoint::duration::max()).count();
      const auto min_seconds = std::chrono::duration_cast<std::chrono::seconds>(
          qt::memory::TimePoint::duration::min()).count();
      if (seconds < min_seconds || seconds > max_seconds)
        throw std::invalid_argument("context build as_of is outside supported time range");
      request.structured.as_of = qt::memory::TimePoint{std::chrono::seconds(seconds)};
      request.structured.enabled = true;
    }
    const auto token_budget = input.value("budget_tokens", std::size_t{900});
    if (token_budget == 0U || token_budget > 16000U)
      throw std::invalid_argument("context build budget_tokens must be from 1 to 16000");
    const auto preview = runtime->preview_retrieval(request, token_budget);
    const auto &pack = preview.context;
    Json warnings = Json::array();
    for (const auto &warning : pack.warnings) {
      warnings.push_back(Json{{"code", "memory_warning"},
                              {"severity", "warning"},
                              {"message", warning}});
    }
    return Json{{"context_pack",
                 {{"context_markdown", pack.markdown},
                  {"memory_ids", pack.memory_ids},
                  {"block_ids", pack.block_ids},
                  {"memory_count", pack.memory_ids.size()},
                  {"warning_count", pack.warnings.size()},
                  {"warnings", std::move(warnings)},
                  {"budget_tokens", token_budget},
                  {"used_tokens", pack.estimated_tokens},
                  {"omitted", pack.omitted},
                  {"code_ids", pack.code_ids},
                  {"explanation", qt::memory::context_preview_json(pack, token_budget)},
                  {"memories", [&] {
                     Json values = Json::array();
                     for (const auto &item : preview.memories) {
                       values.push_back(qt::memory::recalled_memory_json(
                           item, runtime->current_retrieval_revision(
                                     item.memory.id, request.policy,
                                     request.include_global_user), &pack));
                     }
                     return values;
                   }()},
                  {"code_candidates", [&] {
                     Json values = Json::array();
                     for (const auto &candidate : preview.code)
                       values.push_back(qt::memory::retrieval_candidate_json(candidate, &pack));
                     return values;
                   }()},
                  {"included_counts",
                   {{"memories", pack.memory_ids.size()},
                    {"blocks", pack.block_ids.size()},
                    {"code", pack.code_ids.size()}}}}}};
  }
  throw std::invalid_argument("dashboard operation is not available in qt");
}

[[maybe_unused]] std::string dashboard_html(const std::string &token) {
  return std::string(R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>qt Memory Dashboard</title><style>
:root{color-scheme:dark;--bg:#0b0e12;--panel:#131820;--panel2:#191f29;--line:#293241;--text:#e7edf5;--muted:#91a0b3;--cyan:#69d6e8;--green:#7bd88f;--amber:#f4bf75;--red:#ef6b73;--purple:#b89cff}*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 80% -10%,#18313a 0,transparent 35%),var(--bg);color:var(--text);font:14px/1.45 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}header{position:sticky;top:0;z-index:3;display:flex;align-items:center;gap:18px;padding:18px 24px;background:#0b0e12e8;border-bottom:1px solid var(--line);backdrop-filter:blur(10px)}h1{font-size:18px;margin:0;letter-spacing:.03em}.mark{color:var(--cyan)}#scope{color:var(--muted);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.spacer{flex:1}button,input,select{font:inherit;color:var(--text);background:var(--panel2);border:1px solid var(--line);border-radius:6px;padding:8px 10px}button{cursor:pointer}button:hover,.tab.active{border-color:var(--cyan);color:var(--cyan)}main{max-width:1500px;margin:auto;padding:22px}.tabs{display:flex;gap:8px;margin-bottom:18px}.view{display:none}.view.active{display:block}.grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:12px}.card{background:linear-gradient(145deg,var(--panel),#10151c);border:1px solid var(--line);border-radius:9px;padding:16px;min-width:0}.metric{font-size:28px;font-weight:700;margin-top:6px}.label,.muted{color:var(--muted)}.good{color:var(--green)}.warn{color:var(--amber)}.bad{color:var(--red)}.split{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-top:12px}.bar-row{display:grid;grid-template-columns:140px 1fr 45px;gap:10px;align-items:center;margin:8px 0}.bar{height:8px;background:#242c38;border-radius:10px;overflow:hidden}.bar i{display:block;height:100%;background:var(--cyan)}.toolbar{display:flex;gap:8px;margin-bottom:12px;flex-wrap:wrap}.toolbar input{flex:1;min-width:220px}table{width:100%;border-collapse:collapse;background:var(--panel);border:1px solid var(--line)}th,td{text-align:left;padding:10px 12px;border-bottom:1px solid var(--line);vertical-align:top}th{color:var(--muted);font-weight:500;position:sticky;top:65px;background:var(--panel2)}td.content{max-width:680px;white-space:pre-wrap;word-break:break-word}.pill{display:inline-block;padding:2px 7px;border:1px solid var(--line);border-radius:99px;font-size:12px}.empty{padding:42px;text-align:center;color:var(--muted)}#error{display:none;margin-bottom:14px;border-color:#72343b;color:#ff9ba2}@media(max-width:900px){.grid{grid-template-columns:1fr 1fr}.split{grid-template-columns:1fr}header{padding:14px}main{padding:14px}}@media(max-width:560px){.grid{grid-template-columns:1fr}.tabs{overflow:auto}}
</style></head><body><header><h1><span class="mark">qt</span> Memory Dashboard</h1><span id="scope">loading…</span><span class="spacer"></span><button id="refresh">Refresh</button></header><main><div id="error" class="card"></div><nav class="tabs"><button class="tab active" data-view="overview">Overview</button><button class="tab" data-view="explorer">Memory Explorer</button><button class="tab" data-view="reviews">Review Queue</button></nav><section id="overview" class="view active"><div id="metrics" class="grid"></div><div class="card" style="margin-top:12px"><h2>Retrieval</h2><div id="retrieval-status" class="muted">Loading…</div><button id="rebuild-retrieval" style="margin-top:10px">Rebuild retrieval</button></div><div class="split"><div class="card"><h2>Trust states</h2><div id="states"></div></div><div class="card"><h2>Memory types</h2><div id="types"></div></div></div></section><section id="explorer" class="view"><div class="toolbar"><input id="search" placeholder="Search content, tags, source, or ID"><select id="status"><option value="">All states</option></select><select id="type"><option value="">All types</option></select></div><div id="memories"></div></section><section id="reviews" class="view"><div class="card muted" style="margin-bottom:12px">Review decisions remain in the native <b>/memory review</b> Inbox, where every mutation receives a typed preview and explicit confirmation.</div><div id="review-list"></div></section></main><script>
const TOKEN=")HTML") +
         token +
         R"HTML(";history.replaceState(null,"","/dashboard");let state=null;
function initContextPreview(){const host=document.getElementById("retrieval-status").parentElement,bar=document.createElement("div");bar.className="toolbar";bar.style.marginTop="14px";const query=document.createElement("input");query.id="context-query";query.setAttribute("aria-label","Retrieval query");query.placeholder="Explain context for a query";const budget=document.createElement("input");budget.id="context-budget";budget.setAttribute("aria-label","Context token budget");budget.type="number";budget.min="1";budget.max="16000";budget.value="900";budget.style.maxWidth="130px";const button=document.createElement("button");button.textContent="Preview context";const output=document.createElement("div");output.id="context-preview";output.className="muted";bar.append(query,budget,button);host.append(bar,output);button.onclick=async()=>{output.textContent="Loading context preview…";try{const response=await fetch("/context/build",{method:"POST",headers:{Authorization:`Bearer ${TOKEN}`,"Content-Type":"application/json"},body:JSON.stringify({query:query.value,budget_tokens:Number(budget.value)})});const result=await response.json();if(!response.ok||result.error)throw new Error(result.error||`Preview failed (${response.status})`);const pack=result.context_pack||{},explanation=pack.explanation||{};output.textContent=`${pack.memory_count||0} memories · ${pack.code_ids?.length||0} code candidates · ${explanation.estimated_full_context?.tokens||pack.used_tokens||0} estimated tokens · ${pack.omitted||0} omitted`;const details=document.createElement("pre");details.textContent=JSON.stringify({preview:true,injected:explanation.injected,used:explanation.used,omission_reasons:explanation.omission_reasons||[],items:explanation.items||[],selected:explanation.selected||{},memories:(pack.memories||[]).map(x=>({id:x.id,score:x.score,selection:x.selection,ranking:x.score_breakdown})),code_candidates:(pack.code_candidates||[]).map(x=>({owner:x.owner,score:x.fused_score,selection:x.selection,ranks:x.ranks}))},null,2);output.append(details)}catch(error){output.textContent=error.message}}}
const esc=v=>String(v??"").replace(/[&<>"']/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]));
const count=(obj,key)=>Number(obj?.[key]||0);function metric(label,value,tone=""){return `<div class="card"><div class="label">${esc(label)}</div><div class="metric ${tone}">${esc(value)}</div></div>`}
function bars(id,data){const root=document.getElementById(id),values=Object.entries(data||{}).sort((a,b)=>b[1]-a[1]),max=Math.max(1,...values.map(x=>x[1]));root.innerHTML=values.length?values.map(([k,v])=>`<div class="bar-row"><span>${esc(k)}</span><div class="bar"><i style="width:${100*v/max}%"></i></div><b>${v}</b></div>`).join(""):'<div class="empty">No data</div>'}
function render(){if(!state?.available){throw new Error(state?.error||"Memory data unavailable")}const s=state.statistics,c=s.catalog,g=s.governance,u=s.usage;document.getElementById("scope").textContent=[state.scope.repository,state.scope.branch].filter(Boolean).join(" @ ")||state.scope.project||"current scope";const risk=count(g,"pending_reviews")+count(g,"unresolved_conflicts")+count(c,"low_confidence");document.getElementById("metrics").innerHTML=metric("Memories",c.total,"good")+metric("Pending reviews",g.pending_reviews,g.pending_reviews?"warn":"good")+metric("Unresolved conflicts",g.unresolved_conflicts,g.unresolved_conflicts?"bad":"good")+metric("Total recalls",u.total_accesses)+metric("Low confidence",c.low_confidence,risk?"warn":"good")+metric("Unprovenanced",c.unprovenanced,c.unprovenanced?"warn":"good")+metric("Dreaming",state.dreaming.running?"running":state.dreaming.enabled?"idle":"off",state.dreaming.running?"warn":"")+metric("Schema",state.schema_version);const r=state.retrieval||{};document.getElementById("retrieval-status").textContent=`${r.running?"running":r.enabled?"enabled":"disabled"} · generation ${r.generation||"none"} · pending ${count(r.jobs,"pending")} · failed ${count(r.jobs,"failed")} · cache hits ${count(r.queries,"cache_hits")}`;bars("states",c.by_status);bars("types",c.by_type);const statuses=[...new Set(state.memories.map(m=>m.status))].sort(),types=[...new Set(state.memories.map(m=>m.type))].sort();document.getElementById("status").innerHTML='<option value="">All states</option>'+statuses.map(x=>`<option>${esc(x)}</option>`).join("");document.getElementById("type").innerHTML='<option value="">All types</option>'+types.map(x=>`<option>${esc(x)}</option>`).join("");renderMemories();renderReviews()}
function renderMemories(){const q=document.getElementById("search").value.toLowerCase(),st=document.getElementById("status").value,ty=document.getElementById("type").value;const rows=(state?.memories||[]).filter(m=>(!st||m.status===st)&&(!ty||m.type===ty)&&(!q||JSON.stringify(m).toLowerCase().includes(q)));document.getElementById("memories").innerHTML=rows.length?`<table><thead><tr><th>Type / state</th><th>Memory</th><th>Confidence</th><th>Use</th></tr></thead><tbody>${rows.map(m=>`<tr><td><span class="pill">${esc(m.type)}</span><br><span class="muted">${esc(m.status)}</span></td><td class="content">${esc(m.content)}<br><small class="muted">${esc(m.id)} · ${esc(m.provenance?.source_uri||m.source||"")}</small></td><td>${Math.round(Number(m.confidence||0)*100)}%</td><td>${Number(m.access_count||0)}</td></tr>`).join("")}</tbody></table>`:'<div class="card empty">No memories match these filters.</div>'}
function renderReviews(){const rows=state?.reviews||[];document.getElementById("review-list").innerHTML=rows.length?`<table><thead><tr><th>Priority</th><th>Kind</th><th>Reason</th><th>Status</th></tr></thead><tbody>${rows.map(r=>`<tr><td>${esc(r.priority)}</td><td><span class="pill">${esc(r.kind)}</span></td><td class="content">${esc(r.reason)}<br><small class="muted">${esc(r.id)}</small></td><td>${esc(r.status)}</td></tr>`).join("")}</tbody></table>`:'<div class="card empty">The current scope has no pending reviews.</div>'}
async function load(){const error=document.getElementById("error");error.style.display="none";try{const response=await fetch("/api/snapshot",{headers:{Authorization:`Bearer ${TOKEN}`}});if(!response.ok)throw new Error(`Dashboard request failed (${response.status})`);state=await response.json();render()}catch(e){error.textContent=e.message;error.style.display="block"}}
async function rebuildRetrieval(){const button=document.getElementById("rebuild-retrieval"),error=document.getElementById("error");if(!confirm("Rebuild retrieval indexes for this runtime scope?"))return;button.disabled=true;button.textContent="Rebuilding…";try{const response=await fetch("/dashboard/retrieval/rebuild",{method:"POST",headers:{Authorization:`Bearer ${TOKEN}`,"Content-Type":"application/json"},body:JSON.stringify({confirm:true})});const result=await response.json();if(!response.ok||result.error)throw new Error(result.error||`Rebuild failed (${response.status})`);button.textContent=`Queued ${result.jobs_enqueued||0} jobs`;await load()}catch(e){error.textContent=e.message;error.style.display="block";button.textContent="Rebuild retrieval"}finally{button.disabled=false}}
document.querySelectorAll(".tab").forEach(b=>b.onclick=()=>{document.querySelectorAll(".tab,.view").forEach(x=>x.classList.remove("active"));b.classList.add("active");document.getElementById(b.dataset.view).classList.add("active")});["search","status","type"].forEach(id=>document.getElementById(id).addEventListener("input",renderMemories));document.getElementById("refresh").onclick=load;document.getElementById("rebuild-retrieval").onclick=rebuildRetrieval;initContextPreview();load();
</script></body></html>)HTML";
}

std::string wasm_dashboard_html(const std::string &token) {
  return std::string(R"HTML(<!doctype html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>qt Memory Dashboard</title><style>
:root{color-scheme:dark;--bg:#070c18;--text:#eef6ff;--muted:#a6b6c9;--accent:#5ce1e6;--status:3.25rem}*{box-sizing:border-box}html,body{margin:0;min-height:100%;background:linear-gradient(180deg,#0b1325,#070c18);color:var(--text);font-family:Inter,ui-sans-serif,system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;overflow:hidden}.viewport{position:fixed;inset:0 0 var(--status) 0}.viewport canvas{width:100%;height:100%;display:block;outline:none}.statusbar{position:fixed;left:0;right:0;bottom:0;min-height:var(--status);display:grid;grid-template-columns:minmax(0,max-content) minmax(0,1fr) auto;gap:.75rem;align-items:center;padding:.45rem max(.8rem,env(safe-area-inset-right)) calc(.45rem + env(safe-area-inset-bottom)) max(.8rem,env(safe-area-inset-left));background:#09101eed;border-top:1px solid #8badd024;font-size:.875rem}.statusbar b{min-width:0;font-size:.82rem;letter-spacing:.04em;text-transform:uppercase;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.statusbar span{min-width:0;color:var(--accent);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.runtime{color:var(--muted);font-size:.78rem;white-space:nowrap}@media(max-width:34rem){:root{--status:4.75rem}.statusbar{grid-template-columns:minmax(0,1fr);gap:.2rem}.runtime{justify-self:end}}
</style></head><body><main class="viewport"><canvas id="canvas" tabindex="0" role="application" aria-label="qt Memory Dashboard interactive canvas" oncontextmenu="event.preventDefault()">qt Memory Dashboard requires a browser with canvas support.</canvas></main><footer class="statusbar"><b>qt Memory Dashboard</b><span id="status-display" role="status" aria-live="polite">Booting WASM runtime…</span><small class="runtime">ImGui + WebAssembly</small></footer><script>
const dashboardToken=")HTML") +
         token +
         R"HTML(";localStorage.setItem("qtmem.dashboard.auth_token",dashboardToken);history.replaceState(null,"","/dashboard");window.qtmemDashboardBootstrap={initialProject:"",authRequired:true,assetsAvailable:true};var Module={preRun:[],postRun:[],print:t=>t&&console.log(t),printErr:t=>t&&console.error(t),setStatus:t=>{const e=document.getElementById("status-display");if(e)e.textContent=t||"Ready"},locateFile:p=>"/dashboard/"+p,canvas:document.getElementById("canvas")};const script=document.createElement("script");script.src="/dashboard/index.js";document.body.appendChild(script);
</script></body></html>)HTML";
}

std::pair<bool, std::string> open_browser(const std::string &url) {
  if (const char *disabled = std::getenv("QT_MEMORY_DASHBOARD_NO_BROWSER");
      disabled != nullptr && *disabled != '\0') {
    return {false, {}};
  }

  std::vector<std::string> command;
  if (const char *browser = std::getenv("BROWSER");
      browser != nullptr && *browser != '\0' &&
      std::string_view(browser).find_first_of(" \t\r\n") ==
          std::string_view::npos) {
    command = {browser, url};
#if defined(__APPLE__)
  } else {
    command = {"open", url};
#else
  } else {
    command = {"xdg-open", url};
#endif
  }
  const auto launched =
      qt::foundation::run_argv_process(std::move(command), std::nullopt, 5000);
  if (!launched)
    return {false, launched.error().message};
  if (launched.value().timed_out)
    return {false, "browser launcher timed out"};
  if (launched.value().exit_code != 0) {
    const std::string detail = trim(launched.value().stderr_text);
    return {false, detail.empty()
                       ? "browser launcher exited with status " +
                             std::to_string(launched.value().exit_code)
                       : detail};
  }
  return {true, {}};
}

} // namespace

MemoryDashboardServer::MemoryDashboardServer(RuntimeProvider runtime_provider)
    : runtime_provider_(std::move(runtime_provider)) {}

MemoryDashboardServer::~MemoryDashboardServer() { stop(); }

qt::foundation::Result<MemoryDashboardLaunch>
MemoryDashboardServer::start(bool launch_browser) {
  MemoryDashboardLaunch result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (listener_ < 0) {
      const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
      if (listener < 0) {
        return qt::foundation::Result<MemoryDashboardLaunch>::failure(
            qt::foundation::ErrorCode::io_error,
            "unable to create memory dashboard socket", errno);
      }
      const int enabled = 1;
      (void)::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled,
                         sizeof(enabled));
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      address.sin_port = 0;
      if (::bind(listener, reinterpret_cast<sockaddr *>(&address),
                 sizeof(address)) != 0 ||
          ::listen(listener, 16) != 0) {
        const int error = errno;
        ::close(listener);
        return qt::foundation::Result<MemoryDashboardLaunch>::failure(
            qt::foundation::ErrorCode::io_error,
            "unable to bind memory dashboard to loopback", error);
      }
      socklen_t length = sizeof(address);
      if (::getsockname(listener, reinterpret_cast<sockaddr *>(&address),
                        &length) != 0) {
        const int error = errno;
        ::close(listener);
        return qt::foundation::Result<MemoryDashboardLaunch>::failure(
            qt::foundation::ErrorCode::io_error,
            "unable to resolve memory dashboard address", error);
      }
      listener_ = listener;
      port_ = ntohs(address.sin_port);
      token_ = qt::foundation::generate_uuid_v4_string();
      result.started = true;
      thread_ = std::thread([this]() { serve(); });
    }
    result.url = "http://127.0.0.1:" + std::to_string(port_) +
                 "/dashboard?token=" + token_;
  }
  if (launch_browser) {
    auto browser = open_browser(result.url);
    result.browser_opened = browser.first;
    result.browser_error = std::move(browser.second);
  }
  return result;
}

void MemoryDashboardServer::stop() noexcept {
  int listener = -1;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    listener = std::exchange(listener_, -1);
    port_ = 0;
    token_.clear();
  }
  if (listener >= 0) {
    (void)::shutdown(listener, SHUT_RDWR);
    (void)::close(listener);
  }
  if (thread_.joinable())
    thread_.join();
}

bool MemoryDashboardServer::running() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return listener_ >= 0;
}

std::string MemoryDashboardServer::url() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (listener_ < 0)
    return {};
  return "http://127.0.0.1:" + std::to_string(port_) +
         "/dashboard?token=" + token_;
}

void MemoryDashboardServer::serve() noexcept {
  while (true) {
    int listener = -1;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      listener = listener_;
    }
    if (listener < 0)
      return;
    pollfd descriptor{listener, POLLIN, 0};
    const int ready = ::poll(&descriptor, 1, 250);
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready <= 0 || !(descriptor.revents & POLLIN))
      continue;
    sockaddr_in peer{};
    socklen_t peer_length = sizeof(peer);
    const int client =
        ::accept(listener, reinterpret_cast<sockaddr *>(&peer), &peer_length);
    if (client < 0) {
      if (errno == EINTR)
        continue;
      std::lock_guard<std::mutex> lock(mutex_);
      if (listener_ < 0)
        return;
      continue;
    }
    handle_client(client);
    (void)::shutdown(client, SHUT_RDWR);
    (void)::close(client);
  }
}

void MemoryDashboardServer::handle_client(int client) noexcept {
  try {
    std::string request;
    std::array<char, 4096> buffer{};
    while (request.find("\r\n\r\n") == std::string::npos &&
           request.size() < kMaxRequestBytes) {
      const ssize_t received = ::recv(client, buffer.data(), buffer.size(), 0);
      if (received > 0) {
        request.append(buffer.data(), static_cast<std::size_t>(received));
        continue;
      }
      if (received < 0 && errno == EINTR)
        continue;
      break;
    }
    const auto header_end = request.find("\r\n\r\n");
    if (header_end != std::string::npos) {
      std::size_t content_length = 0U;
      const std::string encoded_length =
          header_value(request, "content-length");
      if (!encoded_length.empty())
        content_length = std::stoull(encoded_length);
      if (content_length > kMaxRequestBytes ||
          header_end + 4U + content_length > kMaxRequestBytes) {
        send_response(client, 413, "Payload Too Large", "application/json",
                      R"({"error":"request is too large"})");
        return;
      }
      while (request.size() < header_end + 4U + content_length) {
        const ssize_t received =
            ::recv(client, buffer.data(), buffer.size(), 0);
        if (received > 0) {
          request.append(buffer.data(), static_cast<std::size_t>(received));
          continue;
        }
        if (received < 0 && errno == EINTR)
          continue;
        break;
      }
    }
    const auto line_end = request.find("\r\n");
    if (line_end == std::string::npos) {
      send_response(client, 400, "Bad Request", "application/json",
                    R"({"error":"malformed request"})");
      return;
    }
    std::istringstream first_line(request.substr(0U, line_end));
    std::string method;
    std::string target;
    std::string version;
    first_line >> method >> target >> version;
    if ((method != "GET" && method != "POST") || target.empty()) {
      send_response(client, 405, "Method Not Allowed", "application/json",
                    R"({"error":"only GET and POST are supported"})");
      return;
    }
    const std::string path = target_path(target);
    std::string token;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      token = token_;
    }
    if (method == "GET" && (path == "/dashboard" || path == "/dashboard/")) {
      if (query_value(target, "token") != token) {
        send_response(client, 401, "Unauthorized", "text/plain; charset=utf-8",
                      "Memory dashboard authorization failed.\n");
        return;
      }
      send_response(client, 200, "OK", "text/html; charset=utf-8",
                    wasm_dashboard_html(token));
      return;
    }
    if (method == "GET" && path.rfind("/dashboard/", 0U) == 0U) {
      const std::string asset_path =
          path.substr(std::string("/dashboard").size());
      if (const auto *asset =
              qt::app::generated::find_dashboard_asset(asset_path.c_str())) {
        send_response(client, 200, "OK", asset->content_type,
                      std::string(reinterpret_cast<const char *>(asset->data),
                                  asset->size),
                      asset->gzip_encoded);
      } else {
        send_response(client, 404, "Not Found", "application/json",
                      R"({"error":"dashboard asset not found"})");
      }
      return;
    }
    if (header_value(request, "authorization") != "Bearer " + token) {
      send_response(client, 401, "Unauthorized", "application/json",
                    R"({"error":"unauthorized"})");
      return;
    }
    if (method == "GET" && path == "/api/snapshot") {
      try {
        send_response(client, 200, "OK", "application/json; charset=utf-8",
                      safe_json_dump(snapshot_json(runtime_provider_())));
      } catch (const std::exception &error) {
        send_response(client, 500, "Internal Server Error", "application/json",
                      safe_json_dump(Json{{"error", error.what()}}));
      }
      return;
    }
    try {
      Json input = Json::object();
      if (method == "POST" && header_end != std::string::npos) {
        const std::string body = request.substr(header_end + 4U);
        if (!body.empty()) {
          input = Json::parse(body, nullptr, false);
          if (input.is_discarded() || !input.is_object()) {
            throw std::invalid_argument(
                "dashboard request body must be a valid JSON object");
          }
        }
      }
      send_response(
          client, 200, "OK", "application/json; charset=utf-8",
          safe_json_dump(
              compatibility_response(runtime_provider_(), method, path, input)));
    } catch (const std::invalid_argument &error) {
      send_response(client, 400, "Bad Request", "application/json",
                    safe_json_dump(Json{{"error", error.what()}}));
    } catch (const nlohmann::json::exception &error) {
      send_response(client, 400, "Bad Request", "application/json",
                    safe_json_dump(Json{{"error", "invalid dashboard JSON"},
                                        {"detail", error.what()}}));
    } catch (const std::exception &error) {
      send_response(client, 500, "Internal Server Error", "application/json",
                    safe_json_dump(Json{{"error", error.what()}}));
    }
    return;
  } catch (...) {
    send_response(client, 500, "Internal Server Error", "application/json",
                  R"({"error":"dashboard request failed"})");
  }
}

} // namespace qt::app
