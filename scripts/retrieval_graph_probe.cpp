#include "qt/memory/retrieval_graph.hpp"

#include <cassert>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace qt::memory;
using namespace qt::index;

class Memories final : public MemoryRepository {
 public:
  std::vector<MemoryEntry> query(const MemoryScope&, const MemoryQuery&) const override { return values; }
  MemoryEntry get(const MemoryScope&, const std::string& id) const override {
    for (const auto& value : values) if (value.id == id) return value;
    throw std::invalid_argument("missing memory");
  }
  std::string add(const MemoryScope&, MemoryWrite) override { return {}; }
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
  std::vector<GraphEdge> query_edges(const MemoryScope&, const GraphQuery& query) const override {
    std::vector<GraphEdge> result;
    for (const auto& edge : edges) {
      if (!query.memory_id.empty() && edge.from_id != query.memory_id && edge.to_id != query.memory_id) continue;
      result.push_back(edge);
    }
    return result;
  }
  std::vector<GraphEdge> edges;
};

class Links final : public CrossStoreLinkRepository {
 public:
  std::vector<CrossStoreLink> links(const MemoryScope&, const RetrievalReference& from,
                                    RecallPolicy, bool, std::size_t) const override {
    std::vector<CrossStoreLink> result;
    for (const auto& link : values) if (link.from.kind == from.kind && link.from.id == from.id) result.push_back(link);
    return result;
  }
  std::vector<CrossStoreLink> values;
};

class Structured final : public StructuredMemoryRepository {
 public:
  std::vector<MemoryEntity> entities;
  std::vector<MemoryAssertion> assertions;
  std::optional<MemoryAssertion> assertion(const MemoryScope& scope, const std::string& id,
      std::optional<TimePoint> = std::nullopt) const override {
    for (auto it = assertions.rbegin(); it != assertions.rend(); ++it)
      if (it->memory_id == id && scope_key(scope) == scope_key(entities.front().scope)) return *it;
    return std::nullopt;
  }
  std::vector<MemoryEntity> find_entities(const MemoryScope& scope, const std::string& name,
      RecallPolicy, bool, std::size_t limit) const override {
    std::vector<MemoryEntity> result;
    for (const auto& entity : entities) {
      if (scope_key(entity.scope) != scope_key(scope)) continue;
      if (entity.id == name || entity.name == name ||
          std::find(entity.aliases.begin(), entity.aliases.end(), name) != entity.aliases.end()) {
        result.push_back(entity); if (result.size() >= limit) break;
      }
    }
    return result;
  }
  std::vector<MemoryAssertion> query_assertions(const MemoryScope& scope,
      const std::string& subject, const std::string& predicate, const std::string& object,
      std::optional<TimePoint> as_of, RecallPolicy, bool, std::size_t limit) const override {
    const auto at = as_of.value_or(std::chrono::system_clock::now());
    std::vector<MemoryAssertion> result;
    for (const auto& assertion : assertions) {
      if (assertion.subject_id != subject && !subject.empty()) continue;
      if (assertion.predicate != predicate && !predicate.empty()) continue;
      if (assertion.object_id != object && !object.empty()) continue;
      if (assertion.valid_from && at < *assertion.valid_from) continue;
      if (assertion.valid_to && at >= *assertion.valid_to) continue;
      const auto owner = std::find_if(entities.begin(), entities.end(), [&](const auto& entity) {
        return entity.id == assertion.subject_id && scope_key(entity.scope) == scope_key(scope);
      });
      if (owner == entities.end()) continue;
      result.push_back(assertion); if (result.size() >= limit) break;
    }
    return result;
  }
};

static MemoryEntry memory(const std::string& id, const MemoryScope& scope) {
  MemoryEntry value; value.id = id; value.scope = scope; value.status = MemoryStatus::Validated;
  value.content = id; return value;
}

int main() {
  const auto root = std::filesystem::temp_directory_path() / "qt-retrieval-graph-probe";
  std::filesystem::remove_all(root); std::filesystem::create_directories(root);
  std::ofstream(root / "main.cpp") << "int f() { return 1; }\nint f() { return 2; }\nint caller() { return f(); }\n";
  ProjectScanResult scan; scan.root = root; scan.files.push_back({"main.cpp", ProjectFileKind::CxxSource});
  CodeIndex index;
  index.functions.push_back({"f", "f", "main.cpp", 1, "function"});
  index.functions.push_back({"f", "f", "main.cpp", 2, "function"});
  index.functions.push_back({"caller", "caller", "main.cpp", 3, "function"});
  index.call_graph.push_back({"caller", "f", "main.cpp", 3});
  const auto snapshot = RetrievalSnapshot::build(scan, index, 9);

  MemoryScope scope; scope.user = "probe"; scope.project = "project"; scope.repository = "repo";
  Memories memories; memories.values = {memory("m", scope), memory("positive-owner", scope)};
  Graph graph; graph.edges.push_back({"e", "m", "missing", "links", 1.0, ""});
  Links links;
  const auto file = std::find_if(snapshot->nodes().begin(), snapshot->nodes().end(), [](const auto& n) {
    return n.kind == RetrievalCodeNodeKind::File;
  });
  CrossStoreLink link;
  link.from.kind = RetrievalOwnerKind::Memory; link.from.id = "m";
  link.to.kind = RetrievalOwnerKind::Code; link.to.id = file->id;
  link.relation = "references"; link.evidence_id = "evidence-1";
  links.values.push_back(link);
  RetrievalGraphAdapter adapter(graph, memories, [snapshot] { return snapshot; },
                                 {root.generic_string(), root.generic_string()}, &links);
  RetrievalGraphQuery query; query.seeds.push_back({{RetrievalOwnerKind::Memory, "m"}});
  query.max_depth = 3; query.max_results = 20;
  const auto result = adapter.traverse(scope, query);
  assert(result.nodes.size() >= 3);
  assert(std::any_of(result.edges.begin(), result.edges.end(), [](const auto& edge) {
    return edge.relation == "references";
  }));

  Structured structured;
  MemoryEntity alpha{"entity-alpha", "person", "Alice", {"alias-a"}, scope};
  MemoryEntity beta{"entity-beta", "project", "Beta", {}, scope};
  MemoryEntity gamma{"entity-gamma", "project", "Gamma", {}, scope};
  structured.entities = {alpha, beta, gamma};
  MemoryAssertion assertion; assertion.memory_id = "m"; assertion.revision = 1; assertion.subject_id = alpha.id;
  assertion.object_id = beta.id; assertion.predicate = "owns"; assertion.positive = false;
  assertion.evidence.push_back({"source", "1", "uri:test", "hash", "1", "probe"});
  structured.assertions.push_back(assertion);
  auto hidden = assertion; hidden.memory_id = "hidden-owner"; structured.assertions.push_back(hidden);
  MemoryAssertion onward = assertion; onward.memory_id = "positive-owner"; onward.subject_id = beta.id;
  onward.object_id = gamma.id; onward.positive = true; structured.assertions.push_back(onward);
  RetrievalGraphAdapter entity_adapter(graph, memories, [snapshot] { return snapshot; },
                                        {root.generic_string(), root.generic_string()}, nullptr, {}, &structured);
  RetrievalGraphQuery entity_query; entity_query.max_depth = 2;
  entity_query.allowed_relations = {"owns"};
  entity_query.seeds.push_back({{RetrievalOwnerKind::Entity, "alias-a"}});
  const auto entity_result = entity_adapter.traverse(scope, entity_query);
  assert(entity_result.nodes.size() == 2 && entity_result.edges.size() == 1);
  assert(!entity_result.edges.front().assertion_positive &&
         entity_result.edges.front().assertion_evidence.size() == 1 &&
         entity_result.edges.front().evidence_id == "assertion:m:1" &&
         entity_result.edges.front().assertion.has_value() &&
         entity_result.edges.front().assertion->memory_id == "m");
  assert(std::none_of(entity_result.nodes.begin(), entity_result.nodes.end(),
                      [](const auto& node) { return node.id == "entity-gamma"; }));
  RetrievalGraphAdapter denied_assertion(graph, memories, [snapshot] { return snapshot; },
      {root.generic_string(), root.generic_string()}, nullptr,
      [](const MemoryScope&, const RetrievalReference& ref) {
        return ref.kind != RetrievalOwnerKind::Memory || ref.id != "m";
      }, &structured);
  assert(denied_assertion.traverse(scope, entity_query).edges.empty());
  structured.assertions.front().valid_from = std::chrono::system_clock::now() + std::chrono::hours(1);
  assert(entity_adapter.traverse(scope, entity_query).edges.empty());
  RetrievalGraphQuery membership; membership.max_depth = 1;
  membership.allowed_relations = {"assertion_subject", "assertion_object"};
  membership.seeds.push_back({{RetrievalOwnerKind::Memory, "m"}});
  const auto negative_membership = entity_adapter.traverse(scope, membership);
  assert(negative_membership.edges.empty());
  structured.assertions.front().valid_from.reset();
  const auto negative_membership_valid = entity_adapter.traverse(scope, membership);
  assert(std::any_of(negative_membership_valid.edges.begin(), negative_membership_valid.edges.end(),
      [](const auto& edge) { return edge.relation == "assertion_subject" &&
                                    !edge.assertion_positive && edge.candidates.size() == 1; }));
  assert(std::none_of(negative_membership_valid.edges.begin(), negative_membership_valid.edges.end(),
      [](const auto& edge) { return edge.relation == "assertion_object"; }));
  membership.direction = RetrievalGraphDirection::Incoming;
  assert(entity_adapter.traverse(scope, membership).edges.empty());
  membership.direction = RetrievalGraphDirection::Both;
  membership.seeds.clear(); membership.seeds.push_back({{RetrievalOwnerKind::Memory, "positive-owner"}});
  const auto positive_membership = entity_adapter.traverse(scope, membership);
  assert(std::any_of(positive_membership.edges.begin(), positive_membership.edges.end(),
      [](const auto& edge) { return edge.relation == "assertion_subject"; }));
  assert(std::any_of(positive_membership.edges.begin(), positive_membership.edges.end(),
      [](const auto& edge) { return edge.relation == "assertion_object"; }));
  assert(std::any_of(result.edges.begin(), result.edges.end(), [](const auto& edge) {
    return edge.relation == "calls" && edge.resolution == "ambiguous" && edge.candidates.size() == 2;
  }));
  assert(std::none_of(result.edges.begin(), result.edges.end(), [](const auto& edge) {
    return std::any_of(edge.candidates.begin(), edge.candidates.end(),
                       [](const auto& candidate) { return candidate.id == "missing"; });
  }));
  assert(std::none_of(result.omissions.begin(), result.omissions.end(), [](const auto& text) {
    return text.find("missing") != std::string::npos;
  }));

  const auto first_function = std::find_if(snapshot->nodes().begin(), snapshot->nodes().end(),
                                           [](const auto& node) {
    return node.kind == RetrievalCodeNodeKind::Function;
  });
  assert(first_function != snapshot->nodes().end());
  RetrievalGraphQuery incoming; incoming.direction = RetrievalGraphDirection::Incoming;
  incoming.max_depth = 1; incoming.seeds.push_back({{RetrievalOwnerKind::Code, first_function->id}});
  const auto incoming_result = adapter.traverse(scope, incoming);
  assert(std::any_of(incoming_result.edges.begin(), incoming_result.edges.end(), [](const auto& edge) {
    return edge.incoming && edge.resolution == "ambiguous";
  }));

  RetrievalGraphQuery capped; capped.max_depth = 0; capped.max_results = 3;
  for (int i = 0; i < 100; ++i) {
    memories.values.push_back(memory("seed-" + std::to_string(i), scope));
    capped.seeds.push_back({{RetrievalOwnerKind::Memory, "seed-" + std::to_string(i)}});
  }
  const auto capped_result = adapter.traverse(scope, capped);
  assert(capped_result.nodes.size() == 3);

  RetrievalGraphQuery stale_pin;
  stale_pin.max_depth = 0;
  stale_pin.seeds.push_back({{RetrievalOwnerKind::Code, first_function->id, 0,
                              first_function->repository, first_function->worktree,
                              first_function->path.generic_string(),
                              first_function->source_fingerprint,
                              first_function->generation + 1}});
  assert(adapter.traverse(scope, stale_pin).nodes.empty());

  std::ofstream(root / "main.cpp", std::ios::app) << "// changed\n";
  const auto changed = adapter.traverse(scope, query);
  assert(std::any_of(changed.omissions.begin(), changed.omissions.end(), [](const auto& omission) {
    return omission.find("endpoint") != std::string::npos || omission.find("stale") != std::string::npos;
  }));
  std::filesystem::remove_all(root);
  std::cout << "retrieval graph probe passed\n";
}
