#include "qt/memory/recall_service.hpp"

#include "qt/memory/review.hpp"
#include "qt/memory/scope.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <sstream>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace qt::memory {
namespace {

std::vector<std::string> tokens(const std::string& text) {
  std::vector<std::string> result;
  std::string token;
  for (unsigned char ch : text) {
    if (std::isalnum(ch) != 0 || ch == '_' || ch == '-') {
      token.push_back(static_cast<char>(std::tolower(ch)));
    } else if (token.size() > 1) {
      result.push_back(std::move(token));
      token.clear();
    } else {
      token.clear();
    }
  }
  if (token.size() > 1) result.push_back(std::move(token));
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::uint64_t stable_hash(const std::string& value) {
  std::uint64_t result = 1469598103934665603ULL;
  for (unsigned char ch : value) {
    result ^= ch;
    result *= 1099511628211ULL;
  }
  return result;
}

double freshness(const TimePoint& updated) {
  if (updated == TimePoint{}) return 0.25;
  const auto age = std::chrono::duration_cast<std::chrono::hours>(
      std::chrono::system_clock::now() - updated).count() / 24.0;
  if (age <= 0.0) return 1.0;
  return std::exp(-age / 180.0);
}

std::string normalized_key(const std::string& text) {
  const auto words = tokens(text);
  std::ostringstream out;
  for (const auto& word : words) out << word << ' ';
  return out.str();
}

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

bool known_stale_status(const std::string& value) {
  const auto status = lowercase(value);
  return status == "changed" || status == "deleted" ||
         status == "drifted" || status == "invalid" ||
         status == "mismatch" || status == "missing" ||
         status == "outdated" || status == "stale";
}

}  // namespace

double RecallService::hashed_similarity(const std::string& lhs,
                                        const std::string& rhs) {
  constexpr std::size_t kDimensions = 256;
  std::array<double, kDimensions> left{};
  std::array<double, kDimensions> right{};
  for (const auto& token : tokens(lhs)) {
    const auto hash = stable_hash(token);
    left[hash % kDimensions] += ((hash >> 8U) & 1U) == 0 ? 1.0 : -1.0;
  }
  for (const auto& token : tokens(rhs)) {
    const auto hash = stable_hash(token);
    right[hash % kDimensions] += ((hash >> 8U) & 1U) == 0 ? 1.0 : -1.0;
  }
  double dot = 0.0;
  double left_norm = 0.0;
  double right_norm = 0.0;
  for (std::size_t i = 0; i < kDimensions; ++i) {
    dot += left[i] * right[i];
    left_norm += left[i] * left[i];
    right_norm += right[i] * right[i];
  }
  if (left_norm == 0.0 || right_norm == 0.0) return 0.0;
  return std::max(0.0, dot / std::sqrt(left_norm * right_norm));
}

std::vector<std::string> RecallService::warnings_for(
    const MemoryScope& scope, const MemoryEntry& memory) const {
  std::vector<std::string> warnings;
  if (memory.status == MemoryStatus::Candidate)
    warnings.push_back("candidate memory; not validated");
  if (memory.status == MemoryStatus::Hypothesis)
    warnings.push_back("hypothesis; verify before relying on it");
  if (memory.confidence < 0.5) warnings.push_back("low confidence");
  if (!memory.replacement_id.empty())
    warnings.push_back("replacement relationship: " + memory.replacement_id);
  if (memory.provenance.source_uri.empty() && memory.provenance.source_hash.empty())
    warnings.push_back("no durable source locator");
  if (memory.updated_at != TimePoint{} &&
      std::chrono::system_clock::now() - memory.updated_at > std::chrono::hours(24 * 365))
    warnings.push_back("possibly stale; last update is over one year old");
  if (graph_ != nullptr) {
    GraphQuery query;
    query.memory_id = memory.id;
    query.limit = 50;
    for (const auto& edge : graph_->query_edges(scope, query)) {
      if (edge.type == "conflicts_with" || edge.type == "contradicts") {
        const auto peer = edge.from_id == memory.id ? edge.to_id : edge.from_id;
        warnings.push_back("conflict relationship with memory " + peer);
      }
    }
  }
  return warnings;
}

std::optional<std::string> RecallService::source_drift_warning(
    const MemoryEntry& memory) const {
  if (sources_ == nullptr) return std::nullopt;
  const auto registered = sources_->sources(memory.scope);
  const auto drift = [&](const std::string& id, const std::string& uri,
                         const std::string& hash) -> std::optional<std::string> {
    for (const auto& source : registered) {
      const bool matches = !id.empty() ? source.id == id : !uri.empty() && source.uri == uri;
      if (!matches) continue;
      if (known_stale_status(source.hash_status))
        return "known stale source: hash status is " + source.hash_status;
      if (!hash.empty() && !source.hash.empty() && hash != source.hash)
        return "known stale source: recorded source hash has changed";
    }
    return std::nullopt;
  };
  if (const auto warning = drift(memory.provenance.source_id, memory.provenance.source_uri,
                                  memory.provenance.source_hash)) return warning;
  if (structured_) {
    if (const auto assertion = structured_->assertion(memory.scope, memory.id)) {
      for (const auto& evidence : assertion->evidence)
        if (const auto warning = drift(evidence.source_id, evidence.source_uri,
                                        evidence.source_hash)) return warning;
    }
  }
  return std::nullopt;
}

bool RecallService::eligible_for_context(const MemoryScope& scope,
                                         const std::string& memory_id,
                                         RecallPolicy policy,
                                         bool include_global_user) const {
  std::optional<MemoryEntry> current;
  try {
    current = memories_.find_for_recall(scope, memory_id, policy, include_global_user);
  } catch (...) {
    return false;
  }
  if (!current || !GovernanceService::trusted_for_recall(current->status))
    return false;
  try {
    if (source_drift_warning(*current).has_value()) return false;
  } catch (...) {
    return false;
  }
  if (reviews_ == nullptr) return true;
  try {
    auto blocked = reviews_->critical_conflict_memory_ids(scope);
    if (scope_key(current->scope) != scope_key(scope)) {
      const auto canonical = reviews_->critical_conflict_memory_ids(current->scope);
      blocked.insert(blocked.end(), canonical.begin(), canonical.end());
    }
    return std::find(blocked.begin(), blocked.end(), memory_id) == blocked.end();
  } catch (...) {
    return false;
  }
}

std::vector<RecalledMemory> RecallService::recall(
    const MemoryScope& scope, const RecallRequest& input) const {
  auto request = input;
  request.candidate_limit = std::min<std::size_t>(request.candidate_limit, 1000);
  request.result_limit = std::min<std::size_t>(request.result_limit, 1000);
  if (request.candidate_limit == 0 || request.result_limit == 0) return {};
  request.graph.max_seeds = std::min<std::size_t>(request.graph.max_seeds, 128);
  request.graph.max_depth = std::min<std::size_t>(request.graph.max_depth, 8);
  request.graph.max_fanout = std::min<std::size_t>(request.graph.max_fanout, 256);
  request.graph.max_edges = std::min<std::size_t>(request.graph.max_edges, 8192);
  request.graph.max_results = std::min<std::size_t>(request.graph.max_results, 1000);
  request.structured.limit = std::min<std::size_t>(request.structured.limit, 128);
  // An as-of structured query is a historical answer.  Do not mix it with
  // current lexical, graph, or dense candidates that would be presented as
  // evidence from the requested point in time.
  if (request.structured.as_of) {
    request.graph.enabled = false;
    request.dense_enabled = false;
  }
  MemoryQuery query;
  query.text = request.query;
  query.tags = request.tags;
  query.policy = request.policy;
  query.include_global_user = request.include_global_user;
  query.limit = request.candidate_limit;
  query.statuses = {MemoryStatus::Active, MemoryStatus::Validated};
  if (request.include_candidates) {
    query.statuses.push_back(MemoryStatus::Candidate);
    query.statuses.push_back(MemoryStatus::Hypothesis);
  }
  auto candidates = memories_.query(scope, query);
  std::vector<std::string> structured_seed_ids;
  std::map<std::string, MemoryAssertion> historical_assertions;
  if (request.structured.enabled && structured_ != nullptr &&
      request.structured.limit != 0U) {
    const auto resolve_entities = [&](const std::string& value) {
      if (value.empty()) return std::vector<MemoryEntity>{};
      return structured_->find_entities(scope, value, request.policy,
                                        request.include_global_user,
                                        request.structured.limit);
    };
    const auto subjects = resolve_entities(request.structured.subject);
    const auto objects = resolve_entities(request.structured.object);
    std::unordered_set<std::string> seen_assertions;
    std::size_t structured_queries = 0;
    const auto query_assertions = [&](const std::string& subject_id,
                                      const std::string& object_id) {
      if (structured_seed_ids.size() >= request.structured.limit ||
          structured_queries++ >= request.structured.limit) return;
      for (const auto& assertion : structured_->query_assertions(
               scope, subject_id, request.structured.predicate, object_id,
               request.structured.as_of, request.policy,
               request.include_global_user,
               request.structured.limit - structured_seed_ids.size())) {
        if (seen_assertions.insert(assertion.memory_id).second) {
          structured_seed_ids.push_back(assertion.memory_id);
          if (request.structured.as_of)
            historical_assertions.emplace(assertion.memory_id, assertion);
        }
      }
    };
    const bool unresolved = (!request.structured.subject.empty() && subjects.empty()) ||
                            (!request.structured.object.empty() && objects.empty());
    if (!unresolved && !subjects.empty()) {
      for (const auto& subject : subjects) {
        if (!objects.empty()) {
          for (const auto& object : objects) query_assertions(subject.id, object.id);
        } else {
          query_assertions(subject.id, {});
        }
      }
    } else if (!unresolved && !objects.empty()) {
      for (const auto& object : objects) query_assertions({}, object.id);
    } else if (!unresolved && (!request.structured.predicate.empty() || request.structured.as_of)) {
      query_assertions({}, {});
    }
  }
  if (request.structured.as_of) {
    candidates.clear();
    for (const auto& [id, assertion] : historical_assertions) {
      const auto current = memories_.find_for_recall(
          scope, id, request.policy, request.include_global_user);
      if (!current || !GovernanceService::trusted_for_recall(current->status))
        continue;
      auto historical = *current;
      historical.content = assertion.historical_content;
      historical.source = assertion.historical_source;
      historical.type = assertion.historical_type;
      historical.status = assertion.historical_status;
      historical.provenance = assertion.historical_provenance;
      historical.created_at = assertion.historical_created_at;
      historical.updated_at = assertion.historical_updated_at;
      candidates.push_back(std::move(historical));
    }
  }
  // Cache complete authority snapshots by scope for this recall only. Final
  // context assembly rechecks them so a newly filed review still blocks use.
  std::unordered_map<std::string, std::unordered_set<std::string>> conflict_scopes;
  const auto blocked_in_scope = [&](const MemoryScope& review_scope,
                                    const std::string& id) {
    const auto key = scope_key(review_scope);
    auto found = conflict_scopes.find(key);
    if (found == conflict_scopes.end()) {
      const auto ids = reviews_->critical_conflict_memory_ids(review_scope);
      found = conflict_scopes.emplace(key,
          std::unordered_set<std::string>(ids.begin(), ids.end())).first;
    }
    return found->second.count(id) != 0U;
  };
  const auto has_critical_conflict = [&](const MemoryEntry& memory) {
    if (reviews_ == nullptr) return false;
    try {
      return blocked_in_scope(scope, memory.id) ||
          (scope_key(memory.scope) != scope_key(scope) &&
           blocked_in_scope(memory.scope, memory.id));
    } catch (...) {
      // An unavailable authority lookup must not authorize a graph bridge.
      return true;
    }
  };
  // Candidate fusion can encounter the same owner repeatedly. Reuse its source
  // assessment only within this recall; final context authority is checked fresh.
  std::unordered_map<std::string, std::optional<std::string>> source_assessments;
  const auto source_assessment = [&](const MemoryEntry& memory) -> std::optional<std::string> {
    const auto found = source_assessments.find(memory.id);
    if (found != source_assessments.end()) return found->second;
    return source_assessments.emplace(memory.id, source_drift_warning(memory)).first->second;
  };
  const auto eligible_for_graph = [&](const MemoryEntry& memory) {
    if (!scope_can_access(scope, memory.scope, request.policy,
                          request.include_global_user)) return false;
    if (!GovernanceService::trusted_for_recall(memory.status) &&
        memory.status != MemoryStatus::Candidate &&
        memory.status != MemoryStatus::Hypothesis) return false;
    if (!request.include_candidates &&
        (memory.status == MemoryStatus::Candidate ||
         memory.status == MemoryStatus::Hypothesis)) return false;
    for (const auto& required_tag : request.tags) {
      if (std::find(memory.tags.begin(), memory.tags.end(), required_tag) ==
          memory.tags.end()) return false;
    }
    if (has_critical_conflict(memory)) return false;
    return !source_assessment(memory).has_value();
  };
  const auto score_memory = [&](const MemoryEntry& memory) {
    RecalledMemory recalled;
    recalled.score.hashed_similarity = hashed_similarity(request.query, memory.content);
    recalled.score.lexical = request.query.empty() ? 0.5 : recalled.score.hashed_similarity;
    recalled.score.confidence = memory.confidence;
    recalled.score.quality = memory.quality_score;
    recalled.score.freshness = freshness(memory.updated_at);
    recalled.score.scope = scope_score(scope, memory.scope);
    recalled.score.total =
        0.28 * recalled.score.lexical +
        0.22 * recalled.score.hashed_similarity +
        0.18 * recalled.score.confidence +
        0.10 * recalled.score.quality +
        0.08 * recalled.score.freshness +
        0.14 * recalled.score.scope;
    if (memory.status == MemoryStatus::Validated) recalled.score.total += 0.08;
    if (memory.status == MemoryStatus::Candidate) recalled.score.total *= 0.68;
    if (memory.status == MemoryStatus::Hypothesis) recalled.score.total *= 0.78;
    recalled.score.total = std::min(1.0, recalled.score.total);
    recalled.warnings = warnings_for(scope, memory);
    if (has_critical_conflict(memory)) {
      recalled.warnings.push_back(
          "critical unresolved conflict; excluded from guidance context");
      recalled.eligible_for_context = false;
    }
    if (const auto drift = source_assessment(memory)) {
      recalled.warnings.push_back(*drift);
      recalled.eligible_for_context = false;
    }
    recalled.memory = std::move(memory);
    return recalled;
  };
  std::vector<RecalledMemory> result;
  result.reserve(candidates.size());
  std::unordered_map<std::string, std::size_t> result_by_id;
  std::unordered_set<std::string> duplicates;
  for (std::size_t rank = 0; rank < candidates.size(); ++rank) {
    auto& memory = candidates[rank];
    if (!eligible_for_graph(memory)) {
      // Preserve the existing result behavior for non-graph recall, where
      // source/conflict exclusions are reported as ineligible warnings.
      if (request.graph.enabled) continue;
    }
    const auto key = request.graph.enabled || request.dense_enabled || request.structured.enabled
        ? memory.id : normalized_key(memory.content);
    if (!duplicates.insert(key).second) continue;
    auto recalled = score_memory(std::move(memory));
    if (const auto historical = historical_assertions.find(recalled.memory.id);
        historical != historical_assertions.end()) {
      recalled.historical = true;
      recalled.historical_revision = historical->second.revision;
      recalled.historical_recorded_at = historical->second.recorded_at;
      recalled.historical_as_of = request.structured.as_of;
      recalled.warnings.push_back("historical assertion snapshot; current guidance requires revalidation");
    }
    recalled.score.lexical_rank = rank + 1U;
    result_by_id[recalled.memory.id] = result.size();
    result.push_back(std::move(recalled));
  }
  if (request.dense_enabled && dense_) {
    if (!(request.cancelled && request.cancelled())) {
      try {
        const auto dense_candidates = dense_(scope, request);
        const std::size_t limit = std::min<std::size_t>(1000U, dense_candidates.size());
        std::unordered_set<std::string> dense_owners;
        for (std::size_t rank = 0; rank < limit && dense_owners.size() < request.candidate_limit; ++rank) {
          if (request.cancelled && request.cancelled()) break;
          const auto& candidate = dense_candidates[rank];
          const auto& captured = candidate.memory;
          const auto& match = candidate.hit;
          if (dense_owners.count(captured.id) != 0U) continue;
          std::optional<MemoryEntry> current;
          std::int64_t current_revision = 0;
          try {
            current = memories_.find_for_recall(
                scope, captured.id, request.policy, request.include_global_user);
            if (current) current_revision = memories_.retrieval_revision(current->scope, current->id);
          } catch (...) {
            continue;
          }
          // A provider can complete after a lifecycle or source update. Its
          // snapshot is ranking evidence, never current authority. Changed
          // content must be retrieved again using its own representation.
          if (!current || !eligible_for_graph(*current) ||
              current->content != captured.content ||
              current->updated_at != captured.updated_at ||
              scope_key(current->scope) != scope_key(captured.scope) ||
              match.chunk.owner_key != "memory:" + captured.id || match.chunk.id.empty() ||
              match.generation.empty() || match.chunk.owner_revision <= 0 ||
              match.chunk.owner_revision != current_revision ||
              !std::isfinite(match.similarity) || match.similarity < -1.0 || match.similarity > 1.0)
            continue;
          const auto& memory = *current;
          dense_owners.insert(memory.id);
          const auto owner_rank = dense_owners.size();
          const auto existing = result_by_id.find(memory.id);
          if (existing != result_by_id.end()) {
            result[existing->second].score.dense_rank = owner_rank;
            result[existing->second].dense_match = match;
            continue;
          }
          if (!duplicates.insert(memory.id).second) continue;
          auto recalled = score_memory(memory);
          recalled.score.dense_rank = owner_rank;
          recalled.dense_match = match;
          result_by_id[memory.id] = result.size();
          result.push_back(std::move(recalled));
        }
      } catch (...) {
        // Dense retrieval is optional; lexical and graph recall remain usable.
      }
    }
  }
  const bool graph_requested =
      !request.structured.as_of &&
      (request.graph.enabled || !structured_seed_ids.empty());
  if (graph_requested && request.graph.max_results != 0U) {
    std::size_t graph_edges_examined = 0;
    std::size_t graph_seeds_added = 0;
    std::size_t graph_results_added = 0;
    // Entity relations are grounded in governed assertion owners. Keep each
    // owner in the path so context can revalidate and budget all evidence.
    if (structured_ && request.graph.enabled) {
      struct EntityFrontier {
        std::string id;
        std::vector<RecalledAssertionStep> path;
      };
      std::vector<EntityFrontier> entity_frontier;
      std::unordered_set<std::string> entity_seen;
      const auto visible_entity = [&](const std::string& id) {
        const auto found = structured_->find_entities(scope, id, request.policy,
                                                       request.include_global_user, 2);
        return found.size() == 1 && found.front().id == id;
      };
      const auto entity_seed = [&](const std::string& id) {
        if (id.empty() || graph_seeds_added >= request.graph.max_seeds ||
            !visible_entity(id) || !entity_seen.insert(id).second) return;
        ++graph_seeds_added;
        entity_frontier.push_back({id, {}});
      };
      // Explicit entity queries work even when no lexical memory was found.
      for (const auto& value : {request.structured.subject, request.structured.object}) {
        if (value.empty()) continue;
        const auto matches = structured_->find_entities(scope, value, request.policy,
                                                         request.include_global_user, 2);
        if (matches.size() == 1) entity_seed(matches.front().id);
      }
      for (const auto& item : result) {
        if (!item.eligible_for_context || !eligible_for_graph(item.memory)) continue;
        const auto assertion = structured_->assertion(item.memory.scope, item.memory.id);
        if (!assertion) continue;
        entity_seed(assertion->subject_id);
      }
      const std::unordered_set<std::string> allowed_assertions =
          request.graph.allowed_relation_types.empty()
              ? std::unordered_set<std::string>{"supported_by", "derived_from", "discusses",
                                                "applies_to", "depends_on", "uses"}
              : std::unordered_set<std::string>(request.graph.allowed_relation_types.begin(),
                                                request.graph.allowed_relation_types.end());
      for (std::size_t front = 0; front < entity_frontier.size() &&
           graph_edges_examined < request.graph.max_edges &&
           graph_results_added < request.graph.max_results; ++front) {
        if (request.cancelled && request.cancelled()) break;
        const auto current = entity_frontier[front];
        if (current.path.size() >= request.graph.max_depth) continue;
        std::vector<MemoryAssertion> adjacent;
        const auto remaining = std::min(request.graph.max_fanout,
                                         request.graph.max_edges - graph_edges_examined);
        if (remaining == 0) break;
        if (request.graph.direction != GraphDirection::Incoming)
          adjacent = structured_->query_assertions(scope, current.id, {}, {}, {},
              request.policy, request.include_global_user, remaining);
        if (request.graph.direction != GraphDirection::Outgoing && adjacent.size() < remaining) {
          const auto incoming = structured_->query_assertions(scope, {}, {}, current.id, {},
              request.policy, request.include_global_user, remaining - adjacent.size());
          adjacent.insert(adjacent.end(), incoming.begin(), incoming.end());
        }
        for (const auto& assertion : adjacent) {
          if (graph_edges_examined++ >= request.graph.max_edges ||
              graph_results_added >= request.graph.max_results) break;
          if (!allowed_assertions.count(assertion.predicate) || assertion.evidence.empty()) continue;
          const auto owner = memories_.find_for_recall(scope, assertion.memory_id,
              request.policy, request.include_global_user);
          if (!owner || !eligible_for_graph(*owner)) continue;
          const bool reverse = assertion.subject_id != current.id;
          const auto target = reverse ? assertion.subject_id : assertion.object_id;
          if (!target.empty() && !visible_entity(target)) continue;
          auto path = current.path;
          if (std::any_of(path.begin(), path.end(), [&](const auto& step) {
                return step.memory_id == assertion.memory_id;
              })) continue;
          path.push_back({assertion.memory_id, assertion.revision, current.id, target, reverse});
          auto existing = result_by_id.find(assertion.memory_id);
          if (existing == result_by_id.end()) {
            auto recalled = score_memory(*owner);
            recalled.assertion_path = path;
            recalled.connecting_memory = true;
            recalled.score.graph_rank = ++graph_results_added;
            result_by_id[assertion.memory_id] = result.size();
            result.push_back(std::move(recalled));
          } else {
            auto& recalled = result[existing->second];
            if (recalled.assertion_path.empty()) recalled.assertion_path = path;
            if (recalled.score.graph_rank == 0) recalled.score.graph_rank = ++graph_results_added;
          }
          if (assertion.positive && !target.empty() && entity_seen.insert(target).second)
            entity_frontier.push_back({target, std::move(path)});
        }
      }
    }
    struct FrontierItem {
      std::string id;
      std::size_t depth = 0;
      std::vector<std::string> ids;
      std::vector<std::string> relations;
      std::vector<bool> reverse;
    };
    std::vector<FrontierItem> frontier;
    std::unordered_set<std::string> visited;
    auto& seeds_added = graph_seeds_added;
    const auto load_memory = [&](const std::string& id) {
      return memories_.find_for_recall(scope, id, request.policy,
                                      request.include_global_user);
    };
    const auto seed = [&](const std::string& id) {
      if (id.empty() || seeds_added >= request.graph.max_seeds ||
          !visited.insert(id).second) return;
      const auto memory = load_memory(id);
      if (!memory || !eligible_for_graph(*memory)) return;
      ++seeds_added;
      frontier.push_back({id, 0U, {id}, {}, {}});
      auto found = result_by_id.find(id);
      if (found == result_by_id.end()) {
        auto recalled = score_memory(*memory);
        recalled.connecting_memory = true;
        recalled.score.graph_rank = 1U;
        result_by_id[id] = result.size();
        result.push_back(std::move(recalled));
      } else if (result[found->second].score.graph_rank == 0U) {
        result[found->second].score.graph_rank = 1U;
      }
    };
    for (const auto& id : request.graph.seed_memory_ids) seed(id);
    for (const auto& id : structured_seed_ids) seed(id);
    std::vector<std::string> lexical_seed_ids;
    lexical_seed_ids.reserve(result.size());
    for (const auto& candidate : result) lexical_seed_ids.push_back(candidate.memory.id);
    for (const auto& id : lexical_seed_ids) seed(id);
    std::unordered_set<std::string> allowed(request.graph.allowed_relation_types.begin(),
                                             request.graph.allowed_relation_types.end());
    auto& examined_edges = graph_edges_examined;
    std::size_t graph_rank = graph_results_added + 1;
    while (graph_ != nullptr && !frontier.empty() && examined_edges < request.graph.max_edges &&
           graph_rank < request.graph.max_results) {
      auto current = std::move(frontier.front());
      frontier.erase(frontier.begin());
      if (current.depth >= request.graph.max_depth) continue;
      GraphQuery graph_query;
      graph_query.memory_id = current.id;
      graph_query.limit = request.graph.max_fanout;
      if (graph_query.limit == 0U) continue;
      for (const auto& edge : graph_->query_edges(scope, graph_query)) {
        if (examined_edges++ >= request.graph.max_edges) break;
        if (!allowed.empty() && allowed.count(edge.type) == 0U) continue;
        std::string neighbor;
        if (request.graph.direction == GraphDirection::Outgoing) {
          if (edge.from_id != current.id) continue;
          neighbor = edge.to_id;
        } else if (request.graph.direction == GraphDirection::Incoming) {
          if (edge.to_id != current.id) continue;
          neighbor = edge.from_id;
        } else {
          neighbor = edge.from_id == current.id ? edge.to_id : edge.from_id;
        }
        if (neighbor.empty() || !visited.insert(neighbor).second) continue;
        const auto memory = load_memory(neighbor);
        if (!memory || !eligible_for_graph(*memory)) continue;
        ++graph_rank;
        auto path_ids = current.ids;
        path_ids.push_back(neighbor);
        auto path_relations = current.relations;
        path_relations.push_back(edge.type);
        auto path_reverse = current.reverse;
        path_reverse.push_back(edge.to_id == current.id);
        auto found = result_by_id.find(neighbor);
        if (found == result_by_id.end()) {
          auto recalled = score_memory(*memory);
          recalled.connecting_memory = request.graph.include_connecting_memories;
          recalled.graph_path_ids = path_ids;
          recalled.graph_path_relations = path_relations;
          recalled.graph_path_reverse = path_reverse;
          recalled.score.graph_rank = graph_rank;
          result_by_id[neighbor] = result.size();
          result.push_back(std::move(recalled));
        } else {
          auto& recalled = result[found->second];
          if (recalled.graph_path_ids.empty()) {
            recalled.graph_path_ids = path_ids;
            recalled.graph_path_relations = path_relations;
            recalled.graph_path_reverse = path_reverse;
          }
          if (recalled.score.graph_rank == 0U) recalled.score.graph_rank = graph_rank;
        }
        if (current.depth + 1U < request.graph.max_depth)
          frontier.push_back({neighbor, current.depth + 1U, std::move(path_ids),
                              std::move(path_relations), std::move(path_reverse)});
        if (graph_rank >= request.graph.max_results) break;
      }
    }
  }
  for (auto& recalled : result) {
    const double lexical_rrf = recalled.score.lexical_rank == 0U
                                   ? 0.0
                                   : 1.0 / (60.0 + recalled.score.lexical_rank);
    const double graph_rrf = recalled.score.graph_rank == 0U
                                 ? 0.0
                                 : 1.0 / (60.0 + recalled.score.graph_rank);
    const double dense_rrf = recalled.score.dense_rank == 0U
                                 ? 0.0
                                 : 1.0 / (60.0 + recalled.score.dense_rank);
    if (graph_requested || request.dense_enabled) {
      recalled.score.reciprocal_rank_fusion = lexical_rrf + graph_rrf + dense_rrf;
      recalled.score.total = recalled.score.reciprocal_rank_fusion;
    }
  }
  std::stable_sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
    if (lhs.score.reciprocal_rank_fusion != rhs.score.reciprocal_rank_fusion)
      return lhs.score.reciprocal_rank_fusion > rhs.score.reciprocal_rank_fusion;
    if (lhs.score.scope != rhs.score.scope)
      return lhs.score.scope > rhs.score.scope;
    if (lhs.score.total != rhs.score.total) return lhs.score.total > rhs.score.total;
    return lhs.memory.id < rhs.memory.id;
  });
  if (result.size() > request.result_limit) result.resize(request.result_limit);
  return result;
}

void RecallService::record_access(const MemoryScope& scope,
                                  const std::vector<RecalledMemory>& memories) {
  std::vector<std::string> ids;
  ids.reserve(memories.size());
  for (const auto& recalled : memories) {
    if (recalled.eligible_for_context) ids.push_back(recalled.memory.id);
  }
  memories_.record_access(scope, ids);
}

}  // namespace qt::memory
