#include "qt/memory/recall_service.hpp"
#include "qt/memory/scope.hpp"
#include "qt/memory/context_service.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <unordered_map>

using namespace qt::memory;

class Memories final : public MemoryRepository {
 public:
  std::string add(const MemoryScope&, MemoryWrite) override { return {}; }
  std::vector<MemoryEntry> query(const MemoryScope& scope, const MemoryQuery& q) const override {
    std::vector<MemoryEntry> out;
    for (const auto& memory : values) {
      if (!scope_can_access(scope, memory.scope, q.policy, q.include_global_user)) continue;
      if (!q.statuses.empty() && std::find(q.statuses.begin(), q.statuses.end(), memory.status) == q.statuses.end()) continue;
      if (!q.text.empty() && memory.content.find(q.text) == std::string::npos) continue;
      out.push_back(memory);
      if (q.limit != 0 && out.size() >= q.limit) break;
    }
    return out;
  }
  MemoryEntry get(const MemoryScope&, const std::string& id) const override {
    for (const auto& memory : values) if (memory.id == id) return memory;
    throw std::invalid_argument("missing memory");
  }
  MemoryEntry update(const MemoryScope&, const std::string&, const MemoryPatch&) override { throw std::logic_error("unused"); }
  bool forget(const MemoryScope&, const std::string&) override { return false; }
  std::vector<MemoryRevision> revisions(const MemoryScope&, const std::string&) const override { return {}; }
  void record_access(const MemoryScope&, const std::vector<std::string>&) override {}
  std::size_t size(const MemoryScope&) const override { return values.size(); }
  std::vector<MemoryEntry> values;
};

class Graph final : public GraphRepository {
 public:
  std::string add_edge(const MemoryScope&, GraphEdge) override { return {}; }
  std::vector<GraphEdge> query_edges(const MemoryScope&, const GraphQuery& q) const override {
    std::vector<GraphEdge> out;
    for (const auto& edge : edges) {
      if (!q.memory_id.empty() && edge.from_id != q.memory_id && edge.to_id != q.memory_id) continue;
      out.push_back(edge);
      if (q.limit != 0 && out.size() >= q.limit) break;
    }
    return out;
  }
  std::vector<GraphEdge> edges;
};

class Sources final : public SourceRepository {
 public:
  void record_event(const MemoryScope&, SourceRecord, SourceEvent) override {}
  std::vector<SourceRecord> sources(const MemoryScope&) const override { return values; }
  std::vector<SourceEvent> source_events(const MemoryScope&, const std::string&) const override { return {}; }
  std::vector<SourceRecord> values;
};

class Reviews final : public ReviewRepository {
 public:
  std::string add_review(const MemoryScope&, ReviewItemWrite) override { return {}; }
  std::vector<ReviewItem> reviews(const MemoryScope&, ReviewStatus,
                                  std::size_t) const override { return {}; }
  ReviewItem review(const MemoryScope&, const std::string&) const override {
    throw std::logic_error("unused");
  }
  std::vector<ReviewItem> query_reviews(const MemoryScope&, const ReviewQuery&) const override {
    return {};
  }
  ReviewItem decide_review(const MemoryScope&, const std::string&,
                           ReviewDecision) override { throw std::logic_error("unused"); }
  ReviewApplicationResult apply_review_mutation(const MemoryScope&, const std::string&,
                                                ReviewDecision,
                                                ReviewAtomicMutation) override {
    throw std::logic_error("unused");
  }
  std::vector<ReviewActionRecord> review_history(const MemoryScope&,
      const std::optional<std::string>&, std::size_t) const override { return {}; }
  std::size_t resurface_due_reviews(const MemoryScope&, TimePoint) override { return 0; }
  std::optional<ReviewViewState> load_review_view_state(const MemoryScope&) const override {
    return std::nullopt;
  }
  void save_review_view_state(const MemoryScope&, ReviewViewState) override {}
  void clear_review_view_state(const MemoryScope&) override {}
  ReviewStatistics review_statistics(const MemoryScope&) const override { return {}; }
};

class Blocks final : public BlockRepository {
 public:
  ContextBlock get_block(const MemoryScope&, const std::string& id) const override {
    return {id, {}, {}};
  }
  ContextBlock update_block(const MemoryScope&, const std::string& id,
                            std::string content) override {
    return {id, std::move(content), {}};
  }
  std::vector<ContextBlock> blocks(const MemoryScope&) const override { return {}; }
};

class Structured final : public StructuredMemoryRepository {
 public:
  std::vector<MemoryEntity> find_entities(const MemoryScope& scope,
                                           const std::string& name,
                                           RecallPolicy policy,
                                           bool include_global,
                                           std::size_t limit) const override {
    std::vector<MemoryEntity> out;
    for (const auto& entity : entities) {
      if (!scope_can_access(scope, entity.scope, policy, include_global)) continue;
      if (entity.id != name && entity.name != name &&
          std::find(entity.aliases.begin(), entity.aliases.end(), name) == entity.aliases.end()) continue;
      out.push_back(entity);
      if (out.size() >= limit) break;
    }
    return out;
  }
  std::vector<MemoryAssertion> query_assertions(
      const MemoryScope& scope, const std::string& subject_id,
      const std::string& predicate, const std::string& object_id,
      std::optional<TimePoint> as_of, RecallPolicy policy,
      bool include_global, std::size_t limit) const override {
    std::vector<MemoryAssertion> out;
    const auto when = as_of.value_or(std::chrono::system_clock::now());
    for (const auto& assertion : assertions) {
      const auto memory_it = std::find_if(memories->values.begin(), memories->values.end(),
                                          [&](const auto& memory) { return memory.id == assertion.memory_id; });
      if (memory_it == memories->values.end() ||
          !scope_can_access(scope, memory_it->scope, policy, include_global) ||
          memory_it->status != MemoryStatus::Active && memory_it->status != MemoryStatus::Validated) continue;
      if (!subject_id.empty() && assertion.subject_id != subject_id) continue;
      if (!predicate.empty() && assertion.predicate != predicate) continue;
      if (!object_id.empty() && assertion.object_id != object_id) continue;
      if (assertion.valid_from && when < *assertion.valid_from) continue;
      if (assertion.valid_to && when >= *assertion.valid_to) continue;
      out.push_back(assertion);
      if (out.size() >= limit) break;
    }
    return out;
  }
  const Memories* memories = nullptr;
  std::vector<MemoryEntity> entities;
  std::vector<MemoryAssertion> assertions;
};

static MemoryEntry memory(const std::string& id, const MemoryScope& scope, const std::string& text) {
  MemoryEntry value;
  value.id = id; value.scope = scope; value.status = MemoryStatus::Validated; value.content = text;
  return value;
}

int main() {
  MemoryScope scope;
  scope.user = "probe"; scope.project = "project"; scope.repository = "repo";
  Memories memories;
  memories.values = {memory("a", scope, "root seed"), memory("b", scope, "middle"),
                     memory("c", scope, "terminal"), memory("cycle", scope, "cycle"),
                     memory("forbidden", scope, "forbidden")};
  memories.values.back().status = MemoryStatus::Rejected;
  Graph graph;
  graph.edges = {{"1", "a", "b", "supports"}, {"2", "b", "c", "supports"},
                 {"3", "c", "a", "supports"}, {"4", "b", "forbidden", "supports"}};
  Sources sources;
  RecallService service(memories, graph, sources);
  RecallRequest request;
  request.query = "no lexical match";
  request.candidate_limit = 1;
  request.result_limit = 10;
  request.graph.enabled = true;
  request.graph.seed_memory_ids = {"a"};
  request.graph.max_depth = 3;
  request.graph.max_fanout = 4;
  request.graph.max_edges = 20;
  request.graph.max_results = 10;
  const auto result = service.recall(scope, request);
  assert(result.size() == 3);
  assert(result[0].memory.id == "a" || result[0].memory.id == "b");
  assert(std::any_of(result.begin(), result.end(), [](const auto& item) {
    return item.memory.id == "c" && item.graph_path_ids.size() >= 2;
  }));
  assert(std::none_of(result.begin(), result.end(), [](const auto& item) {
    return item.memory.id == "forbidden";
  }));
  for (const auto& item : result) assert(item.graph_path_ids.size() <= 4);
  memories.values[0].provenance.source_id = "source-a";
  memories.values[0].provenance.source_hash = "old-hash";
  SourceRecord drifted;
  drifted.id = "source-a";
  drifted.hash = "new-hash";
  drifted.hash_status = "changed";
  sources.values.push_back(drifted);
  assert(!service.eligible_for_context(scope, "a", RecallPolicy::Strict, false));
  sources.values.clear();
  assert(service.eligible_for_context(scope, "a", RecallPolicy::Strict, false));

  Structured structured;
  Reviews reviews;
  structured.memories = &memories;
  structured.entities = {
      {"subject-1", "service", "Service", {"shared-alias"}, scope},
      {"subject-2", "service", "ServiceTwo", {"shared-alias"}, scope},
      {"object-1", "component", "Component", {"component-alias"}, scope},
      {"outside", "service", "Service", {"shared-alias"},
       MemoryScope{ScopeKind::Exact, "other", {}, "project", "repo"}}};
  structured.assertions = {
      {"b", 1, "subject-1", "depends_on", "object-1", "null", "{}", true, {}, {}, {}},
      {"c", 1, "subject-2", "depends_on", "object-1", "null", "{}", true,
       std::chrono::system_clock::now() - std::chrono::hours(48),
       std::chrono::system_clock::now() - std::chrono::hours(24), {}}};
  RecallService structured_service(memories, graph, sources, reviews, structured);
  RecallRequest structured_request;
  structured_request.query = "no lexical match";
  structured_request.graph.enabled = true;
  structured_request.graph.max_depth = 0;
  structured_request.graph.max_results = 10;
  structured_request.structured.enabled = true;
  structured_request.structured.subject = "shared-alias";
  structured_request.structured.predicate = "depends_on";
  structured_request.structured.object = "component-alias";
  structured_request.structured.as_of = std::chrono::system_clock::now();
  const auto current_structured = structured_service.recall(scope, structured_request);
  assert(std::any_of(current_structured.begin(), current_structured.end(), [](const auto& item) {
    return item.memory.id == "b";
  }));
  assert(std::none_of(current_structured.begin(), current_structured.end(), [](const auto& item) {
    return item.memory.id == "c";
  }));
  auto unresolved_structured = structured_request;
  unresolved_structured.structured.object = "missing-object";
  assert(structured_service.recall(scope, unresolved_structured).empty());
  unresolved_structured = structured_request;
  unresolved_structured.structured.subject = "missing-subject";
  assert(structured_service.recall(scope, unresolved_structured).empty());
  structured_request.structured.as_of =
      std::chrono::system_clock::now() - std::chrono::hours(36);
  const auto historical_structured = structured_service.recall(scope, structured_request);
  assert(std::any_of(historical_structured.begin(), historical_structured.end(), [](const auto& item) {
    return item.memory.id == "c";
  }));

  Blocks blocks;
  ContextService context(blocks);
  ContextOptions ample;
  ample.token_budget = 10000;
  ample.bytes_per_token = 1;
  ample.include_pinned_blocks = false;
  ample.include_recent_episodes = false;
  const auto packed = context.build(scope, result, ample);
  assert(packed.markdown.find("Graph relation") != std::string::npos);
  assert(packed.markdown.find("[Memory a") != std::string::npos);
  assert(packed.markdown.find("[Memory b") != std::string::npos);
  assert(packed.markdown.find("[Memory c") != std::string::npos);
  assert(std::find(packed.memory_ids.begin(), packed.memory_ids.end(), "a") != packed.memory_ids.end());
  assert(std::find(packed.memory_ids.begin(), packed.memory_ids.end(), "b") != packed.memory_ids.end());
  assert(std::find(packed.memory_ids.begin(), packed.memory_ids.end(), "c") != packed.memory_ids.end());
  RecalledMemory oversized = result.front();
  oversized.memory.id = "oversized";
  oversized.memory.content = std::string(700, 'x');
  oversized.graph_path_ids = {"a", "oversized"};
  oversized.graph_path_relations = {"supports"};
  RecalledMemory fitting = result.back();
  fitting.memory.id = "fitting";
  fitting.memory.content = "small connected result";
  fitting.graph_path_ids = {"a", "fitting"};
  fitting.graph_path_relations = {"supports"};
  const auto bridge = *std::find_if(result.begin(), result.end(), [](const auto& item) {
    return item.memory.id == "a";
  });
  const auto overlap = context.build(scope, {oversized, fitting, bridge}, ample);
  assert(overlap.markdown.find("Graph relation") != std::string::npos);
  assert(std::find(overlap.memory_ids.begin(), overlap.memory_ids.end(), "a") != overlap.memory_ids.end());
  assert(std::find(overlap.memory_ids.begin(), overlap.memory_ids.end(), "oversized") != overlap.memory_ids.end());
  assert(std::find(overlap.memory_ids.begin(), overlap.memory_ids.end(), "fitting") != overlap.memory_ids.end());
  ContextOptions selective = ample;
  selective.token_budget = 500;
  const auto selective_pack = context.build(scope, {oversized, fitting, bridge}, selective);
  assert(std::find(selective_pack.memory_ids.begin(), selective_pack.memory_ids.end(), "oversized") ==
         selective_pack.memory_ids.end());
  assert(std::find(selective_pack.memory_ids.begin(), selective_pack.memory_ids.end(), "fitting") !=
         selective_pack.memory_ids.end());
  assert(std::find(selective_pack.memory_ids.begin(), selective_pack.memory_ids.end(), "a") !=
         selective_pack.memory_ids.end());
  assert(std::find(selective_pack.memory_ids.begin(), selective_pack.memory_ids.end(), "fitting") !=
         selective_pack.memory_ids.end());
  assert(std::find(selective_pack.memory_ids.begin(), selective_pack.memory_ids.end(), "oversized") ==
         selective_pack.memory_ids.end());
  ContextOptions tiny = ample;
  tiny.token_budget = 200;
  const auto omitted = context.build(scope, result, tiny);
  assert(std::any_of(omitted.warnings.begin(), omitted.warnings.end(), [](const auto& warning) {
    return warning.find("graph bundle omitted by context budget") != std::string::npos;
  }));

  request.graph.allowed_relation_types = {"missing"};
  assert(service.recall(scope, request).size() == 1);
  request.graph.allowed_relation_types.clear();
  request.graph.max_depth = 1;
  assert(service.recall(scope, request).size() == 3);
  std::cout << "memory graph recall probe passed\n";
}
