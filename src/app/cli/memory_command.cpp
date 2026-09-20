#include "qt/memory/retrieval_explanation.hpp"
#include "qt/app/cli/memory_command.hpp"
#include "qt/app/runtime/memory_preview.hpp"

#include "qt/app/config/settings.hpp"
#include "qt/harness/repository.hpp"
#include "qt/memory/database.hpp"
#include "qt/memory/governance_service.hpp"
#include "qt/memory/memory_service.hpp"
#include "qt/memory/offline_import.hpp"
#include "qt/memory/recall_service.hpp"
#include "qt/memory/review_json.hpp"
#include "qt/memory/review_service.hpp"
#include "qt/memory/runtime.hpp"
#include "qt/memory/scope.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <unordered_set>
#include <vector>

namespace qt::app {
namespace {

using Json = nlohmann::json;

std::string env_or_empty(const char* name) {
  if (const char* value = std::getenv(name)) return value;
  return {};
}

std::string default_agent_home() {
  std::string configured = env_or_empty("QT_CODING_AGENT_DIR");
  const std::string home = env_or_empty("HOME");
  if (configured.empty()) return home.empty() ? ".qt" : home + "/.qt";
  if (configured == "~") return home.empty() ? configured : home;
  if (configured.rfind("~/", 0) == 0 && !home.empty()) {
    return home + configured.substr(1);
  }
  return configured;
}

struct ParsedMemoryCommand {
  std::string operation = "status";
  std::vector<std::string> positional;
  std::vector<std::string> tags;
  std::string type = "observation";
  std::string status = "hypothesis";
  std::string source;
  std::string source_uri;
  std::string note;
  std::string task_id;
  std::string evidence_uri;
  std::string until;
  std::string target_id;
  std::string edit_content;
  std::vector<std::string> review_kinds;
  std::vector<std::string> priorities;
  std::vector<std::string> review_source_types;
  std::vector<std::string> dream_cycles;
  std::string review_scope = "all";
  std::string created_after;
  std::string created_before;
  std::string decided_after;
  std::string subject;
  std::string predicate;
  std::string object;
  std::optional<qt::memory::TimePoint> as_of;
  std::optional<std::size_t> token_budget;
  std::size_t limit = 20U;
  std::size_t offset = 0U;
  std::size_t older_than_days = 0U;
  std::size_t keep_latest = 0U;
  std::size_t batch_limit = 1000U;
  bool emit_json = false;
  bool global_user = false;
  bool yes = false;
  bool replace = false;
  bool dry_run = false;
  bool no_memory = false;
  bool include_memories = false;
  bool include_audit = false;
  bool include_blocks = false;
  bool status_set = false;
  bool oldest_first = false;
  std::optional<bool> stale_filter;
  std::optional<bool> conflict_filter;
  bool help = false;
};

std::size_t parse_size(const std::string& value, const char* option) {
  std::size_t consumed = 0U;
  const auto parsed = std::stoull(value, &consumed);
  if (consumed != value.size() || parsed == 0U || parsed > 10000U) {
    throw std::invalid_argument(std::string(option) +
                                " requires a number from 1 to 10000");
  }
  return static_cast<std::size_t>(parsed);
}

ParsedMemoryCommand parse_command(const std::vector<std::string>& args) {
  ParsedMemoryCommand parsed;
  std::size_t index = 1U;
  if (index < args.size() && !args[index].empty() && args[index][0] != '-') {
    parsed.operation = args[index++];
  }
  const auto take_value = [&](const char* option, std::size_t& position) {
    if (++position >= args.size()) {
      throw std::invalid_argument(std::string(option) + " requires a value");
    }
    return args[position];
  };
  for (; index < args.size(); ++index) {
    const auto& arg = args[index];
    if (arg == "--json") parsed.emit_json = true;
    else if (arg == "--global") parsed.global_user = true;
    else if (arg == "--yes" || arg == "-y") parsed.yes = true;
    else if (arg == "--replace") parsed.replace = true;
    else if (arg == "--dry-run") parsed.dry_run = true;
    else if (arg == "--include-memories") parsed.include_memories = true;
    else if (arg == "--include-audit") parsed.include_audit = true;
    else if (arg == "--include-blocks") parsed.include_blocks = true;
    else if (arg == "--no-memory") parsed.no_memory = true;
    else if (arg == "--no-tools") {
      // Management commands do not expose model tools; accept the global flag.
    } else if (arg == "--help" || arg == "-h") parsed.help = true;
    else if (arg == "--limit") parsed.limit = parse_size(take_value("--limit", index), "--limit");
    else if (arg == "--token-budget") parsed.token_budget = parse_size(take_value("--token-budget", index), "--token-budget");
    else if (arg == "--subject") parsed.subject = take_value("--subject", index);
    else if (arg == "--predicate") parsed.predicate = take_value("--predicate", index);
    else if (arg == "--object") parsed.object = take_value("--object", index);
    else if (arg == "--as-of") {
      const auto value = take_value("--as-of", index);
      std::size_t consumed = 0;
      const auto seconds = std::stoll(value, &consumed);
      const auto maximum = std::chrono::duration_cast<std::chrono::seconds>(qt::memory::TimePoint::duration::max()).count();
      const auto minimum = std::chrono::duration_cast<std::chrono::seconds>(qt::memory::TimePoint::duration::min()).count();
      if (consumed != value.size() || seconds < minimum || seconds > maximum)
        throw std::invalid_argument("--as-of requires supported epoch seconds");
      parsed.as_of = qt::memory::TimePoint{std::chrono::seconds(seconds)};
    }
    else if (arg == "--older-than-days") parsed.older_than_days = parse_size(take_value("--older-than-days", index), "--older-than-days");
    else if (arg == "--keep-latest") parsed.keep_latest = parse_size(take_value("--keep-latest", index), "--keep-latest");
    else if (arg == "--batch-limit") parsed.batch_limit = parse_size(take_value("--batch-limit", index), "--batch-limit");
    else if (arg == "--tag") parsed.tags.push_back(take_value("--tag", index));
    else if (arg == "--type") parsed.type = take_value("--type", index);
    else if (arg == "--status") {
      parsed.status = take_value("--status", index);
      parsed.status_set = true;
    }
    else if (arg == "--kind") parsed.review_kinds.push_back(take_value("--kind", index));
    else if (arg == "--priority") parsed.priorities.push_back(take_value("--priority", index));
    else if (arg == "--source-type") parsed.review_source_types.push_back(take_value("--source-type", index));
    else if (arg == "--dream-cycle") parsed.dream_cycles.push_back(take_value("--dream-cycle", index));
    else if (arg == "--scope") parsed.review_scope = take_value("--scope", index);
    else if (arg == "--created-after") parsed.created_after = take_value("--created-after", index);
    else if (arg == "--created-before") parsed.created_before = take_value("--created-before", index);
    else if (arg == "--decided-after") parsed.decided_after = take_value("--decided-after", index);
    else if (arg == "--stale") parsed.stale_filter = true;
    else if (arg == "--not-stale") parsed.stale_filter = false;
    else if (arg == "--conflict") parsed.conflict_filter = true;
    else if (arg == "--not-conflict") parsed.conflict_filter = false;
    else if (arg == "--offset") {
      const auto value = take_value("--offset", index);
      std::size_t consumed = 0U;
      parsed.offset = static_cast<std::size_t>(std::stoull(value, &consumed));
      if (consumed != value.size() || parsed.offset > 100000U) {
        throw std::invalid_argument("--offset requires a number from 0 to 100000");
      }
    }
    else if (arg == "--oldest-first") parsed.oldest_first = true;
    else if (arg == "--source") parsed.source = take_value("--source", index);
    else if (arg == "--source-uri") parsed.source_uri = take_value("--source-uri", index);
    else if (arg == "--note") parsed.note = take_value("--note", index);
    else if (arg == "--task-id") parsed.task_id = take_value("--task-id", index);
    else if (arg == "--evidence-uri") parsed.evidence_uri = take_value("--evidence-uri", index);
    else if (arg == "--until") parsed.until = take_value("--until", index);
    else if (arg == "--target-id") parsed.target_id = take_value("--target-id", index);
    else if (arg == "--content") parsed.edit_content = take_value("--content", index);
    else if (!arg.empty() && arg[0] == '-') {
      throw std::invalid_argument("unknown memory option: " + arg);
    } else {
      parsed.positional.push_back(arg);
    }
  }
  return parsed;
}

void print_help() {
  std::cout
      << "Usage: qt memory <command> [arguments] [options]\n\n"
      << "Commands:\n"
      << "  status                         Show configuration, scope, pre-handler, and statistics\n"
      << "  recall|search <query>          Recall governed memories\n"
      << "  explain <query>                Preview memory selection without recording usage\n"
      << "  remember <content>             Store a candidate/hypothesis (never auto-validates)\n"
      << "  review list [filters]          List prioritized review items\n"
      << "  review show <id>               Show hydrated proposal, sources, and risk\n"
      << "  review next [filters]          Show the next prioritized review\n"
      << "  review preview <id> <action>   Preview exact governed effects\n"
      << "  review decide <id> <action>    Apply a previewed action (--yes required)\n"
      << "  review defer <id> --until TIME Defer without changing memory (--yes required)\n"
      << "  review reopen <id> --note TEXT Reopen a terminal review (--yes required)\n"
      << "  review history [id]            Show durable review action history\n"
      << "  conflicts                      List explicit contradiction graph edges\n"
      << "  stale                          List expired, deprecated, and superseded memories\n"
      << "  health                         Check schema, database path, and accessibility\n"
      << "  feedback <id> <outcome>        Record procedure outcome\n"
      << "  forget <id> --yes              Permanently delete one in-scope memory\n"
      << "  export <path|->                Export the current scope as JSON\n"
      << "  import <path> [--dry-run] [--replace --yes]\n"
      << "                                 Import native JSON or standalone qtmem v3 SQLite\n"
      << "  prune --older-than-days N [--keep-latest N] [--dry-run|--yes]\n"
      << "                                 Prune raw evidence; canonical, audit, and blocks require\n"
      << "                                 --include-memories, --include-audit, or --include-blocks\n"
      << "  scrub [--batch-limit N] [--dry-run|--yes]\n"
      << "                                 Redact sensitive data in historical in-scope rows\n\n"
      << "Review filters: --status STATUS --kind KIND --priority PRIORITY --tag TAG\n"
      << "  --scope all|exact|durable --source-type TYPE --dream-cycle ID\n"
      << "  --stale|--not-stale --conflict|--not-conflict\n"
      << "  --created-after AGE --created-before AGE --decided-after AGE\n"
      << "Retrieval: --subject ENTITY --predicate RELATION --object ENTITY --as-of EPOCH_SECONDS\n"
      << "  explain also accepts --token-budget N (1..10000)\n"
      << "Options: --json --global --limit N --offset N --oldest-first --note TEXT\n";
}

qt::memory::TimePoint parse_review_time(const std::string& value) {
  if (value.empty()) throw std::invalid_argument("--until requires a value");
  const auto now = std::chrono::system_clock::now();
  const char suffix = value.back();
  if (suffix == 'm' || suffix == 'h' || suffix == 'd' || suffix == 'w') {
    std::size_t consumed = 0U;
    const auto amount = std::stoll(value.substr(0, value.size() - 1U), &consumed);
    if (consumed != value.size() - 1U || amount <= 0) {
      throw std::invalid_argument("--until duration must be positive, for example 2h or 3d");
    }
    const auto seconds = suffix == 'm' ? amount * 60
                         : suffix == 'h' ? amount * 3600
                         : suffix == 'd' ? amount * 86400
                                         : amount * 604800;
    return now + std::chrono::seconds(seconds);
  }
  std::size_t consumed = 0U;
  const auto milliseconds = std::stoll(value, &consumed);
  if (consumed != value.size()) {
    throw std::invalid_argument(
        "--until must be a Unix millisecond timestamp or duration such as 2h");
  }
  return qt::memory::TimePoint{std::chrono::milliseconds(milliseconds)};
}

qt::memory::TimePoint parse_review_filter_time(const std::string& value) {
  if (value.empty()) throw std::invalid_argument("review time filter requires a value");
  const char suffix = value.back();
  if (suffix == 'm' || suffix == 'h' || suffix == 'd' || suffix == 'w') {
    std::size_t consumed = 0U;
    const auto amount = std::stoll(value.substr(0, value.size() - 1U), &consumed);
    if (consumed != value.size() - 1U || amount < 0) {
      throw std::invalid_argument("review age must be positive, for example 2h or 7d");
    }
    const auto seconds = suffix == 'm' ? amount * 60
                         : suffix == 'h' ? amount * 3600
                         : suffix == 'd' ? amount * 86400
                                         : amount * 604800;
    return std::chrono::system_clock::now() - std::chrono::seconds(seconds);
  }
  std::size_t consumed = 0U;
  const auto milliseconds = std::stoll(value, &consumed);
  if (consumed != value.size()) {
    throw std::invalid_argument(
        "review time must be a Unix millisecond timestamp or age such as 7d");
  }
  return qt::memory::TimePoint{std::chrono::milliseconds(milliseconds)};
}

Json normalize_import_state(Json state) {
  if (!state.is_object()) {
    throw std::invalid_argument("memory import root must be a JSON object");
  }
  if (state.contains("entries") && !state.contains("memories")) {
    state["memories"] = state.at("entries");
  }
  if (state.contains("edges") && !state.contains("graph_edges")) {
    state["graph_edges"] = state.at("edges");
  }
  if (!state.contains("memories") || !state.at("memories").is_array()) {
    throw std::invalid_argument(
        "memory import requires a memories or qtmem entries array");
  }
  for (const char* collection : {
           "graph_edges", "procedure_runs", "memory_revisions", "sources",
           "source_events", "review_items", "audit_events", "session_events",
           "blocks"}) {
    if (!state.contains(collection)) state[collection] = Json::array();
    if (!state.at(collection).is_array()) {
      throw std::invalid_argument(std::string("memory import collection must be an array: ") +
                                  collection);
    }
  }
  for (auto& memory : state["memories"]) {
    if (!memory.is_object()) continue;
    if (!memory.contains("type") && memory.contains("memory_type"))
      memory["type"] = memory.at("memory_type");
    if (!memory.contains("agent") && memory.contains("agent_id"))
      memory["agent"] = memory.at("agent_id");
    if (!memory.contains("conversation") && memory.contains("conversation_id"))
      memory["conversation"] = memory.at("conversation_id");
  }
  for (auto& revision : state["memory_revisions"]) {
    if (!revision.is_object()) continue;
    if (!revision.contains("kind") && revision.contains("revision_kind"))
      revision["kind"] = revision.at("revision_kind");
    if (!revision.contains("entry_json") && revision.contains("entry"))
      revision["entry_json"] = revision.at("entry").dump();
  }
  for (auto& source : state["sources"]) {
    if (!source.is_object()) continue;
    if (!source.contains("type") && source.contains("source_type"))
      source["type"] = source.at("source_type");
    if (!source.contains("uri") && source.contains("source_uri"))
      source["uri"] = source.at("source_uri");
    if (!source.contains("hash") && source.contains("source_hash"))
      source["hash"] = source.at("source_hash");
  }
  return state;
}

void write_secure_json(const std::filesystem::path& path, const Json& value) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output || !(output << value.dump(2) << '\n')) {
    throw std::runtime_error("unable to write memory JSON: " +
                             path.generic_string());
  }
  output.close();
  std::error_code mode_error;
  std::filesystem::permissions(
      path, std::filesystem::perms::owner_read |
                std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace, mode_error);
  if (mode_error) {
    throw std::runtime_error("unable to secure memory JSON: " +
                             mode_error.message());
  }
}

Json scope_json(const qt::memory::MemoryScope& scope) {
  return Json{{"kind", qt::memory::to_string(scope.kind)},
              {"user", scope.user},
              {"project", scope.project},
              {"repository", scope.repository},
              {"repository_display", scope.repository_display},
              {"branch", scope.branch},
              {"session", scope.session}};
}

Json statistics_json(const qt::memory::MemoryStatistics& stats) {
  return Json{
      {"catalog",
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
        {"pending_reviews", stats.pending_reviews}}},
      {"dreaming",
       {{"attempted", stats.dreaming.attempted},
        {"completed", stats.dreaming.completed},
        {"failed", stats.dreaming.failed},
        {"pending", stats.dreaming.pending},
        {"protocol_rejections", stats.dreaming.protocol_rejections},
        {"timed_out", stats.dreaming.timed_out},
        {"cancelled", stats.dreaming.cancelled},
        {"start_failures", stats.dreaming.start_failures},
        {"cpu_fallbacks", stats.dreaming.cpu_fallbacks},
        {"reviews_created", stats.dreaming.reviews_created},
        {"by_trigger", stats.dreaming.by_trigger},
        {"by_model", stats.dreaming.by_model},
        {"failures_by_reason", stats.dreaming.failures_by_reason}}}};
}

Json memory_json(const qt::memory::MemoryEntry& memory) {
  return Json{{"id", memory.id},
              {"type", qt::memory::to_string(memory.type)},
              {"status", qt::memory::to_string(memory.status)},
              {"content", memory.content},
              {"source", memory.source},
              {"tags", memory.tags},
              {"confidence", memory.confidence},
              {"quality_score", memory.quality_score},
              {"access_count", memory.access_count},
              {"source_uri", memory.provenance.source_uri},
              {"created_by", memory.provenance.created_by},
              {"success_count", memory.success_count},
              {"failure_count", memory.failure_count},
              {"last_outcome", qt::memory::to_string(memory.last_outcome)}};
}

void emit_review_cards(const std::vector<qt::memory::ReviewItem>& reviews,
                       const bool json) {
  Json items = Json::array();
  for (const auto& review : reviews) {
    items.push_back(qt::memory::review_item_json(review));
  }
  if (json) {
    std::cout << Json{{"items", std::move(items)}, {"count", reviews.size()}}.dump()
              << '\n';
    return;
  }
  std::cout << "Memory Review Inbox (" << reviews.size() << "):\n";
  if (reviews.empty()) {
    std::cout << "  No reviews match the active filters.\n";
    return;
  }
  for (std::size_t index = 0U; index < reviews.size(); ++index) {
    const auto& review = reviews[index];
    std::cout << "  " << (index + 1U) << ". [" << review.priority << "] "
              << review.kind << " — " << review.reason << '\n'
              << "     " << review.id << "  subject: "
              << (review.subject_id.empty() ? "none" : review.subject_id)
              << "  status: " << qt::memory::to_string(review.status) << '\n';
  }
}

void emit(const Json& value, bool json, std::string_view heading = {}) {
  if (json) {
    std::cout << value.dump() << '\n';
    return;
  }
  if (!heading.empty()) std::cout << heading << ":\n";
  if (value.is_array()) {
    if (value.empty()) std::cout << "  (none)\n";
    for (const auto& item : value) {
      if (item.contains("id")) std::cout << "  " << item.at("id").get<std::string>();
      if (item.contains("status")) std::cout << " [" << item.at("status").get<std::string>() << ']';
      if (item.contains("type")) std::cout << " (" << item.at("type").get<std::string>() << ')';
      if (item.contains("content")) std::cout << " " << item.at("content").get<std::string>();
      if (item.contains("from_id")) {
        std::cout << " " << item.at("from_id").get<std::string>() << " -> "
                  << item.at("to_id").get<std::string>();
      }
      std::cout << '\n';
    }
    return;
  }
  for (auto it = value.begin(); it != value.end(); ++it) {
    std::cout << "  " << it.key() << ": ";
    if (it->is_string()) std::cout << it->get<std::string>();
    else std::cout << it->dump();
    std::cout << '\n';
  }
}

std::string joined(const std::vector<std::string>& values) {
  std::string result;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0U) result.push_back(' ');
    result += values[index];
  }
  return result;
}

qt::memory::MemoryScope command_scope(const std::string& cwd,
                                      bool global_user) {
  auto scope = qt::memory::derive_runtime_memory_scope(cwd);
  if (!global_user) return scope;
  qt::memory::MemoryScope global;
  global.kind = qt::memory::ScopeKind::GlobalUser;
  global.user = scope.user;
  return global;
}

std::vector<qt::memory::MemoryStatus> all_statuses() {
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

}  // namespace

std::optional<int> run_memory_command(
    const std::vector<std::string>& raw_args) {
  if (raw_args.empty() || raw_args.front() != "memory") return std::nullopt;
  try {
    const auto command = parse_command(raw_args);
    if (command.help) {
      print_help();
      return EXIT_SUCCESS;
    }
    if (command.no_memory) {
      std::cerr << "qt memory: disabled for this invocation by --no-memory\n";
      return EXIT_FAILURE;
    }

    const std::string cwd = std::filesystem::current_path().generic_string();
    qt::harness::FilesystemRepository repository(cwd);
    LoadSettingsRequest request;
    request.cwd = cwd;
    request.agent_home = default_agent_home();
    const auto loaded = load_app_settings(repository, request);
    if (!loaded) {
      throw std::runtime_error("unable to load qt settings: " +
                               loaded.error().message);
    }
    const auto& settings = loaded.value();
    qt::memory::SQLiteMemoryDatabase database(settings.agent_home);
    qt::memory::MemoryService memories(database);
    qt::memory::RecallService recall(database, database, database, database, database);
    qt::memory::GovernanceService governance(memories, database, database);
    qt::memory::ReviewInspectionService review_inspection(
        memories, database, database, database,
        qt::memory::ReviewGovernancePolicy{
            settings.memory.dreaming.promotion_policy != "disabled"});
    qt::memory::ReviewApplicationService review_application(review_inspection,
                                                            database);
    (void)governance;
    const auto scope = command_scope(cwd, command.global_user);

    if (command.operation == "status" || command.operation == "health") {
      std::error_code error;
      const auto bytes = std::filesystem::file_size(database.path(), error);
      const auto statistics = database.statistics(
          scope, command.global_user ? qt::memory::RecallPolicy::Strict
                                     : qt::memory::RecallPolicy::Balanced,
          command.global_user);
      Json result{{"enabled", settings.memory.enabled},
                  {"automatic_recall", settings.memory.automatic_recall},
                  {"automatic_capture", settings.memory.automatic_capture},
                  {"prehandler_enabled", settings.memory.enabled &&
                                             settings.memory.automatic_recall},
                  {"dreaming",
                   Json{{"enabled", settings.memory.enabled &&
                                       settings.memory.dreaming.enabled},
                        {"configured", settings.memory.dreaming.enabled},
                        {"trigger", settings.memory.dreaming.trigger},
                        {"interval_seconds",
                         settings.memory.dreaming.interval_seconds},
                        {"idle_seconds",
                         settings.memory.dreaming.idle_seconds},
                        {"candidate_threshold",
                         settings.memory.dreaming.candidate_threshold},
                        {"model", settings.memory.dreaming.model
                                      ? Json(*settings.memory.dreaming.model)
                                      : Json(nullptr)},
                        {"verifier_model", settings.memory.dreaming.verifier_model
                                               ? Json(*settings.memory.dreaming.verifier_model)
                                               : Json(nullptr)},
                        {"timeout_ms", settings.memory.dreaming.timeout_ms},
                        {"max_model_input_tokens",
                         settings.memory.dreaming.max_model_input_tokens},
                        {"max_candidates",
                         settings.memory.dreaming.max_candidates},
                        {"research_policy",
                         settings.memory.dreaming.research_policy},
                        {"promotion_policy",
                         settings.memory.dreaming.promotion_policy}}},
                  {"database", database.path().generic_string()},
                  {"memory_root", database.root().generic_string()},
                  {"schema_version", database.schema_version()},
                  {"scope", scope_json(scope)},
                  {"scope_memory_count", statistics.memory_entries},
                  {"statistics", statistics_json(statistics)},
                  {"review_statistics", qt::memory::review_statistics_json(
                                            review_inspection
                                                .statistics_including_durable(
                                                    scope))}};
      if (command.operation == "health") {
        const std::string integrity = database.integrity_check();
        result["healthy"] = database.schema_version() ==
                                qt::memory::SQLiteMemoryDatabase::kSchemaVersion &&
                            integrity == "ok";
        result["integrity"] = integrity;
        result["database_bytes"] = error ? 0U : bytes;
        result["database_readable"] = !error;
      }
      emit(result, command.emit_json,
           command.operation == "health" ? "Memory health" : "Memory status");
      return EXIT_SUCCESS;
    }

    if (command.operation == "recall" || command.operation == "search" ||
        command.operation == "explain") {
      if (command.positional.empty()) {
        throw std::invalid_argument(command.operation + " requires a query");
      }
      qt::memory::RecallRequest recall_request;
      recall_request.query = joined(command.positional);
      recall_request.structured.subject = command.subject;
      recall_request.structured.predicate = command.predicate;
      recall_request.structured.object = command.object;
      recall_request.structured.as_of = command.as_of;
      recall_request.structured.enabled = command.as_of.has_value() || !command.subject.empty() ||
          !command.predicate.empty() || !command.object.empty();
      recall_request.tags = command.tags;
      recall_request.include_global_user = command.global_user;
      recall_request.candidate_limit = std::max<std::size_t>(command.limit, 50U);
      recall_request.result_limit = command.limit;
      const auto& retrieval = settings.memory.retrieval;
      recall_request.graph.enabled = retrieval.graph_enabled;
      recall_request.graph.max_depth = retrieval.max_depth;
      recall_request.graph.max_seeds = retrieval.max_seeds;
      recall_request.graph.max_edges = retrieval.max_edges;
      recall_request.graph.max_fanout = retrieval.max_fanout;
      recall_request.graph.max_results = retrieval.max_graph_results;
      if (settings.memory.recall_policy == "strict") recall_request.policy = qt::memory::RecallPolicy::Strict;
      else if (settings.memory.recall_policy == "exploratory" || settings.memory.recall_policy == "broad")
        recall_request.policy = qt::memory::RecallPolicy::Broad;
      if (command.operation == "explain") {
        auto services = runtime::make_memory_preview_services(settings, scope);
        const auto token_budget = command.token_budget.value_or(settings.memory.context_token_budget);
        const auto preview = services.runtime->preview_retrieval(recall_request, token_budget);
        Json values = Json::array();
        Json code = Json::array();
        std::vector<std::string> observed;
        const auto note = [&](const std::string& name) {
          if (std::find(observed.begin(), observed.end(), name) == observed.end()) observed.push_back(name);
        };
        for (const auto& item : preview.memories) {
          values.push_back(qt::memory::recalled_memory_json(item,
              services.runtime->current_retrieval_revision(item.memory.id,
                  recall_request.policy, recall_request.include_global_user), &preview.context));
          if (item.score.lexical_rank) note("lexical");
          if (item.score.graph_rank) note("graph");
          if (item.score.dense_rank) note("dense");
        }
        for (const auto& item : preview.code) {
          code.push_back(qt::memory::retrieval_candidate_json(item, &preview.context));
          for (const auto& rank : item.ranks) note(rank.retriever);
        }
        const auto status = services.runtime->retrieval_status();
        for (const auto& error : {status.last_error, status.queries.last_error})
          if (!error.empty()) services.warnings.push_back(error);
        emit(Json{{"memories", std::move(values)}, {"code_candidates", std::move(code)},
                  {"context", qt::memory::context_preview_json(preview.context, token_budget)},
                  {"requested_modes", {{"lexical", true}, {"graph", retrieval.graph_enabled && !command.as_of},
                                       {"dense", retrieval.dense_enabled && !command.as_of}, {"code", retrieval.code_enabled && !command.as_of}}},
                  {"observed_retrievers", observed}, {"dense_generation", status.generation},
                  {"retrieval_warnings", services.warnings}},
             command.emit_json, "Memory retrieval preview");
        return EXIT_SUCCESS;
      }
      const auto recalled = recall.recall(scope, recall_request);
      recall.record_access(scope, recalled);
      Json result = Json::array();
      for (const auto& item : recalled) {
        auto value = memory_json(item.memory);
        value["score"] = item.score.total;
        value["warnings"] = item.warnings;
        value["explanation"] = qt::memory::recalled_memory_json(item,
            database.retrieval_revision(item.memory.scope, item.memory.id));
        result.push_back(std::move(value));
      }
      emit(result, command.emit_json, "Recalled memories");
      return EXIT_SUCCESS;
    }

    if (command.operation == "remember") {
      if (command.positional.empty()) {
        throw std::invalid_argument("remember requires content");
      }
      if (command.status != "candidate" && command.status != "hypothesis") {
        throw std::invalid_argument(
            "remember status must be candidate or hypothesis");
      }
      qt::memory::MemoryWrite write;
      write.content = joined(command.positional);
      write.type = qt::memory::memory_type_from_string(command.type);
      write.status = qt::memory::memory_status_from_string(command.status);
      write.source = command.source.empty() ? "user:cli" : command.source;
      write.tags = command.tags;
      write.provenance.source_type = qt::memory::SourceType::Manual;
      write.provenance.source_uri = command.source_uri;
      write.provenance.created_by = "user";
      const auto id = memories.remember(scope, std::move(write));
      emit(Json{{"id", id}, {"status", command.status}, {"created", true}},
           command.emit_json, "Memory created");
      return EXIT_SUCCESS;
    }

    if (command.operation == "review") {
      const std::string operation = command.positional.empty()
                                        ? "list"
                                        : command.positional.front();
      qt::memory::ReviewQuery query;
      query.limit = command.limit;
      query.offset = command.offset;
      query.kinds = command.review_kinds;
      query.priorities = command.priorities;
      for (const auto& source_type : command.review_source_types) {
        const auto parsed = qt::memory::source_type_from_string(source_type);
        if (parsed == qt::memory::SourceType::Unknown &&
            source_type != "unknown") {
          throw std::invalid_argument("invalid review source type: " +
                                      source_type);
        }
        query.source_types.push_back(parsed);
      }
      query.tags = command.tags;
      query.dream_cycles = command.dream_cycles;
      query.stale = command.stale_filter;
      query.conflict = command.conflict_filter;
      query.oldest_first = command.oldest_first;
      if (!command.created_after.empty()) {
        query.created_after = parse_review_filter_time(command.created_after);
      }
      if (!command.created_before.empty()) {
        query.created_before = parse_review_filter_time(command.created_before);
      }
      if (!command.decided_after.empty()) {
        query.decided_after = parse_review_filter_time(command.decided_after);
      }
      if (command.review_scope == "all") {
        query.scope = qt::memory::ReviewQuery::ScopeSelector::All;
      } else if (command.review_scope == "exact") {
        query.scope = qt::memory::ReviewQuery::ScopeSelector::Exact;
      } else if (command.review_scope == "durable") {
        query.scope = qt::memory::ReviewQuery::ScopeSelector::Durable;
      } else {
        throw std::invalid_argument(
            "--scope must be all, exact, or durable");
      }
      if (command.status_set) {
        query.statuses = {qt::memory::review_status_from_string(command.status)};
      }
      if (operation == "list" || operation == "next") {
        if (operation == "next") query.limit = 1U;
        auto expanded = query;
        expanded.offset = 0U;
        expanded.limit = std::min<std::size_t>(
            1000U, query.offset + query.limit);
        std::vector<qt::memory::ReviewItem> items;
        if (query.scope != qt::memory::ReviewQuery::ScopeSelector::Durable) {
          items = review_inspection.query(scope, expanded);
        }
        auto durable_scope = scope;
        durable_scope.kind = durable_scope.branch.empty()
                                 ? qt::memory::ScopeKind::Repository
                                 : qt::memory::ScopeKind::Branch;
        durable_scope.task.clear();
        durable_scope.agent.clear();
        durable_scope.session.clear();
        durable_scope.conversation.clear();
        if (query.scope != qt::memory::ReviewQuery::ScopeSelector::Exact &&
            qt::memory::scope_key(durable_scope) !=
                qt::memory::scope_key(scope)) {
          auto durable = review_inspection.query(durable_scope, expanded);
          items.insert(items.end(), std::make_move_iterator(durable.begin()),
                       std::make_move_iterator(durable.end()));
        }
        const auto now = std::chrono::system_clock::now();
        std::stable_sort(items.begin(), items.end(), [&](const auto& left,
                                                         const auto& right) {
          const auto left_score = qt::memory::review_queue_score(left, now);
          const auto right_score = qt::memory::review_queue_score(right, now);
          if (left_score != right_score) return left_score > right_score;
          if (left.created_at != right.created_at) {
            return query.oldest_first ? left.created_at < right.created_at
                                      : left.created_at > right.created_at;
          }
          return left.id < right.id;
        });
        items.erase(std::unique(items.begin(), items.end(),
                                [](const auto& left, const auto& right) {
                                  return left.id == right.id;
                                }),
                    items.end());
        if (query.offset >= items.size()) {
          items.clear();
        } else {
          items.erase(items.begin(),
                      items.begin() + static_cast<std::ptrdiff_t>(query.offset));
          if (items.size() > query.limit) items.resize(query.limit);
        }
        emit_review_cards(items, command.emit_json);
        return EXIT_SUCCESS;
      }
      if (operation == "show") {
        if (command.positional.size() != 2U) {
          throw std::invalid_argument("review show requires a review id");
        }
        emit(qt::memory::hydrated_review_json(
                 review_inspection.hydrate(scope, command.positional[1])),
             command.emit_json, "Memory review detail");
        return EXIT_SUCCESS;
      }
      if (operation == "history") {
        const std::optional<std::string> id =
            command.positional.size() > 1U
                ? std::optional<std::string>{command.positional[1]}
                : std::nullopt;
        Json items = Json::array();
        for (const auto& record : database.review_history(scope, id, command.limit)) {
          items.push_back(qt::memory::review_action_record_json(record));
        }
        emit(Json{{"items", std::move(items)}}, command.emit_json,
             "Memory review history");
        return EXIT_SUCCESS;
      }
      if (operation == "preview" || operation == "decide" ||
          operation == "defer" || operation == "reopen") {
        const bool defer = operation == "defer";
        const bool reopen = operation == "reopen";
        const std::size_t required = (defer || reopen) ? 2U : 3U;
        if (command.positional.size() != required) {
          throw std::invalid_argument(
              "review action requires <id> and an action (defer/reopen infer the action)");
        }
        const std::string id = command.positional[1];
        Json action_input{{"action", defer ? "defer"
                                      : reopen ? "reopen"
                                               : command.positional[2]},
                          {"note", command.note},
                          {"target_id", command.target_id},
                          {"evidence_uri", command.evidence_uri}};
        if (!command.edit_content.empty()) {
          action_input["edit"] = Json{{"content", command.edit_content}};
        }
        if (defer) action_input["deferred_until"] =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                parse_review_time(command.until).time_since_epoch()).count();
        auto action = qt::memory::review_action_from_json(
            action_input, "user:cli", operation != "preview");
        if (operation == "preview") {
          emit(qt::memory::review_preview_json(
                   review_inspection.preview(scope, id, std::move(action))),
               command.emit_json, "Memory review preview");
          return EXIT_SUCCESS;
        }
        if (!command.yes) {
          throw std::invalid_argument("review mutation requires --yes authority");
        }
        if (reopen && command.note.empty()) {
          throw std::invalid_argument("review reopen requires --note with a reason");
        }
        emit(qt::memory::review_application_json(
                 review_application.apply(scope, id, std::move(action))),
             command.emit_json, "Review applied");
        return EXIT_SUCCESS;
      }
      throw std::invalid_argument(
          "review expects list, show, next, preview, decide, defer, reopen, or history");
      return EXIT_SUCCESS;
    }

    if (command.operation == "conflicts") {
      qt::memory::GraphQuery query;
      query.type = "contradicts";
      query.limit = command.limit;
      const auto conflicts = database.query_edges(scope, query);
      Json result = Json::array();
      for (const auto& edge : conflicts) {
        result.push_back(Json{{"id", edge.id},
                              {"from_id", edge.from_id},
                              {"to_id", edge.to_id},
                              {"type", edge.type},
                              {"confidence", edge.confidence},
                              {"source", edge.source}});
      }
      emit(result, command.emit_json, "Memory conflicts");
      return EXIT_SUCCESS;
    }

    if (command.operation == "stale") {
      qt::memory::MemoryQuery query;
      query.statuses = {qt::memory::MemoryStatus::Deprecated,
                        qt::memory::MemoryStatus::Expired,
                        qt::memory::MemoryStatus::Superseded};
      query.policy = qt::memory::RecallPolicy::Strict;
      query.limit = command.limit;
      const auto stale = memories.query(scope, query);
      Json result = Json::array();
      for (const auto& memory : stale) result.push_back(memory_json(memory));
      emit(result, command.emit_json, "Stale memories");
      return EXIT_SUCCESS;
    }

    if (command.operation == "feedback") {
      if (command.positional.size() != 2U) {
        throw std::invalid_argument("feedback requires <id> <outcome>");
      }
      qt::memory::ProcedureFeedback feedback;
      feedback.procedure_id = command.positional[0];
      feedback.outcome =
          qt::memory::procedure_outcome_from_string(command.positional[1]);
      if (feedback.outcome == qt::memory::ProcedureOutcome::Unknown) {
        throw std::invalid_argument(
            "feedback outcome must be success, failure, partial, or unused");
      }
      feedback.task_id = command.task_id;
      feedback.notes = command.note;
      feedback.evidence_uri = command.evidence_uri;
      const auto recorded = database.add_feedback(scope, std::move(feedback));
      emit(Json{{"run_id", recorded.first.id},
                {"memory", memory_json(recorded.second)}},
           command.emit_json, "Feedback recorded");
      return EXIT_SUCCESS;
    }

    if (command.operation == "forget") {
      if (command.positional.size() != 1U) {
        throw std::invalid_argument("forget requires exactly one memory id");
      }
      if (!command.yes) {
        throw std::invalid_argument("forget requires --yes authority");
      }
      const bool removed = memories.forget(scope, command.positional.front());
      emit(Json{{"id", command.positional.front()}, {"forgotten", removed}},
           command.emit_json, "Memory forget");
      return removed ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (command.operation == "export") {
      if (command.positional.size() != 1U) {
        throw std::invalid_argument("export requires exactly one path or '-'");
      }
      const Json state = database.export_state(scope);
      if (command.positional.front() == "-") {
        std::cout << state.dump(2) << '\n';
      } else {
        const std::filesystem::path path = command.positional.front();
        write_secure_json(path, state);
        emit(Json{{"path", path.generic_string()}, {"exported", true}},
             command.emit_json, "Memory exported");
      }
      return EXIT_SUCCESS;
    }

    if (command.operation == "import") {
      if (command.positional.size() != 1U) {
        throw std::invalid_argument("import requires exactly one JSON or SQLite path");
      }
      if (command.replace && !command.yes && !command.dry_run) {
        throw std::invalid_argument("import --replace requires --yes authority");
      }
      const std::filesystem::path import_path = command.positional.front();
      Json state;
      std::string import_format;
      std::vector<std::string> source_scopes;
      std::vector<std::string> import_warnings;
      if (qt::memory::QtmemV3ImportAdapter::is_sqlite_database(import_path)) {
        auto package = qt::memory::QtmemV3ImportAdapter::read(import_path);
        import_format = std::move(package.format);
        source_scopes = std::move(package.source_scopes);
        import_warnings = std::move(package.warnings);
        state = normalize_import_state(std::move(package.state));
      } else {
        std::ifstream input(import_path, std::ios::binary);
        if (!input) {
          throw std::runtime_error("unable to read memory import: " +
                                   import_path.generic_string());
        }
        state = Json::parse(input);
        import_format = state.contains("entries") ? "qtmem-json-v3"
                                                   : "native-json-v3";
        state = normalize_import_state(std::move(state));
      }
      const std::size_t incoming = state.at("memories").size();
      qt::memory::MemoryQuery existing_query;
      existing_query.limit = 0U;
      existing_query.include_superseded = true;
      existing_query.statuses = all_statuses();
      const auto existing_memories = memories.query(scope, existing_query);
      const std::size_t existing = existing_memories.size();
      std::size_t duplicates = 0U;
      Json duplicate_report = Json::array();
      if (!command.replace) {
        std::unordered_set<std::string> existing_content;
        for (const auto& memory : existing_memories) {
          existing_content.insert(
              qt::memory::MemoryService::normalize_content(memory.content));
        }
        Json filtered = Json::array();
        for (const auto& memory : state.at("memories")) {
          const auto content = qt::memory::MemoryService::normalize_content(
              memory.value("content", std::string{}));
          if (!content.empty() && existing_content.count(content) != 0U) {
            ++duplicates;
            if (duplicate_report.size() < 100U) {
              duplicate_report.push_back(
                  Json{{"source_id", memory.value("id", std::string{})},
                       {"type", memory.value("type", std::string("observation"))},
                       {"status", memory.value("status", std::string("hypothesis"))}});
            }
            continue;
          }
          existing_content.insert(content);
          filtered.push_back(memory);
        }
        state["memories"] = std::move(filtered);
      }
      if (command.dry_run) {
        emit(Json{{"path", command.positional.front()},
                  {"dry_run", true},
                  {"format", import_format},
                  {"incoming_memories", incoming},
                  {"duplicate_memories", duplicates},
                  {"duplicates", duplicate_report},
                  {"would_import_memories", state.at("memories").size()},
                  {"existing_scope_memories", existing},
                  {"replace", command.replace},
                  {"source_scopes", source_scopes},
                  {"target_scope", scope_json(scope)},
                  {"warnings", import_warnings}},
             command.emit_json, "Memory import dry run");
        return EXIT_SUCCESS;
      }
      std::optional<std::filesystem::path> backup_path;
      qt::memory::ExportLimits backup_limits;
      backup_limits.memories = 100000U;
      backup_limits.graph_edges = 100000U;
      backup_limits.procedure_runs = 100000U;
      backup_limits.revisions = 100000U;
      backup_limits.sources = 100000U;
      backup_limits.source_events = 100000U;
      backup_limits.reviews = 100000U;
      backup_limits.audit_events = 100000U;
      backup_limits.session_events = 100000U;
      backup_limits.blocks = 100000U;
      const auto backup_state = database.export_state(scope, backup_limits);
      bool has_existing_state = false;
      for (const char* collection : {
               "memories", "graph_edges", "procedure_runs", "memory_revisions",
               "sources", "source_events", "review_items", "audit_events",
               "session_events", "blocks"}) {
        has_existing_state = has_existing_state ||
                             !backup_state.at(collection).empty();
      }
      if (has_existing_state) {
        const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        backup_path = database.root() / "exports" /
                      ("pre-import-" + std::to_string(stamp) + ".json");
        std::size_t collision = 0U;
        while (std::filesystem::exists(*backup_path)) {
          ++collision;
          backup_path = database.root() / "exports" /
                        ("pre-import-" + std::to_string(stamp) + "-" +
                         std::to_string(collision) + ".json");
        }
        for (auto item = backup_state.at("truncated").begin();
             item != backup_state.at("truncated").end(); ++item) {
          if (item->get<bool>()) {
            throw std::runtime_error(
                "scope is too large for a complete reversible import backup: " +
                item.key());
          }
        }
        write_secure_json(*backup_path, backup_state);
      }
      database.import_state(scope, state, command.replace);
      emit(Json{{"path", command.positional.front()},
                {"imported", true},
                {"format", import_format},
                {"incoming_memories", incoming},
                {"duplicate_memories", duplicates},
                {"duplicates", duplicate_report},
                {"imported_memories", state.at("memories").size()},
                {"replaced", command.replace},
                {"source_scopes", source_scopes},
                {"target_scope", scope_json(scope)},
                {"warnings", import_warnings},
                {"backup", backup_path ? backup_path->generic_string()
                                        : std::string{}},
                {"rollback", backup_path
                     ? std::string("qt memory import ") +
                           backup_path->generic_string() + " --replace --yes"
                     : std::string("transactional; no pre-existing scope data")}},
           command.emit_json, "Memory imported");
      return EXIT_SUCCESS;
    }

    if (command.operation == "prune") {
      if (!command.positional.empty()) {
        throw std::invalid_argument("prune does not accept positional arguments");
      }
      if (!command.dry_run && !command.yes) {
        throw std::invalid_argument("prune requires --dry-run or --yes authority");
      }
      if (command.older_than_days == 0U && command.keep_latest == 0U) {
        throw std::invalid_argument(
            "prune requires --older-than-days or --keep-latest");
      }
      qt::memory::RetentionPolicy policy;
      if (command.older_than_days != 0U) {
        policy.older_than = std::chrono::system_clock::now() -
            std::chrono::hours(24U * command.older_than_days);
      }
      policy.keep_latest = command.keep_latest;
      policy.batch_limit = command.batch_limit;
      policy.raw_evidence = true;
      policy.canonical_memories = command.include_memories;
      policy.audit_events = command.include_audit;
      policy.context_blocks = command.include_blocks;
      policy.dry_run = command.dry_run;
      const auto report = database.enforce_retention(scope, policy);
      emit(Json{{"dry_run", report.dry_run},
                {"session_events", report.session_events},
                {"source_events", report.source_events},
                {"orphan_sources", report.orphan_sources},
                {"canonical_memories", report.canonical_memories},
                {"audit_events", report.audit_events},
                {"context_blocks", report.context_blocks},
                {"total", report.total()},
                {"target_scope", scope_json(scope)}},
           command.emit_json, "Memory retention");
      return EXIT_SUCCESS;
    }

    if (command.operation == "scrub") {
      if (!command.positional.empty()) {
        throw std::invalid_argument("scrub does not accept positional arguments");
      }
      if (!command.dry_run && !command.yes) {
        throw std::invalid_argument("scrub requires --dry-run or --yes authority");
      }
      qt::memory::RedactionScrubPolicy policy;
      policy.batch_limit = command.batch_limit;
      policy.dry_run = command.dry_run;
      const auto report = database.scrub_sensitive_data(scope, policy);
      Json collections = Json::array();
      for (const auto& collection : report.collections) {
        collections.push_back(Json{{"name", collection.name},
                                   {"examined", collection.examined},
                                   {"changed", collection.changed}});
      }
      emit(Json{{"dry_run", report.dry_run},
                {"truncated", report.truncated},
                {"examined", report.examined()},
                {"changed", report.changed()},
                {"collections", std::move(collections)},
                {"target_scope", scope_json(scope)}},
           command.emit_json, "Memory redaction scrub");
      return EXIT_SUCCESS;
    }

    throw std::invalid_argument("unknown memory command: " + command.operation);
  } catch (const std::exception& error) {
    std::cerr << "qt memory: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}

}  // namespace qt::app
