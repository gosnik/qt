#include "qt/memory/retrieval_graph.hpp"

#include "qt/memory/scope.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace qt::memory {
namespace {

std::string key(const RetrievalReference& ref) {
  return std::to_string(static_cast<int>(ref.kind)) + ":" + ref.id;
}

bool allowed(const RetrievalGraphQuery& query, const std::string& relation) {
  return query.allowed_relations.empty() ||
         std::find(query.allowed_relations.begin(), query.allowed_relations.end(), relation) !=
             query.allowed_relations.end();
}

bool direction_ok(RetrievalGraphDirection direction, bool outgoing) {
  return direction == RetrievalGraphDirection::Both ||
         (outgoing && direction == RetrievalGraphDirection::Outgoing) ||
         (!outgoing && direction == RetrievalGraphDirection::Incoming);
}

RetrievalReference code_reference(const index::RetrievalCodeNode& node) {
  return {RetrievalOwnerKind::Code, node.id, 0, node.repository, node.worktree,
          node.path.generic_string(), node.source_fingerprint, node.generation};
}

}  // namespace

RetrievalGraphResult RetrievalGraphAdapter::traverse(
    const MemoryScope& scope, const RetrievalGraphQuery& query) const {
  RetrievalGraphResult result;
  const auto max_depth = std::min<std::size_t>(query.max_depth, 8U);
  const auto max_fanout = std::min<std::size_t>(query.max_fanout, 64U);
  const auto max_edges = std::min<std::size_t>(query.max_edges, 256U);
  const auto max_results = std::min<std::size_t>(query.max_results, 64U);
  if (max_results == 0U || max_edges == 0U || max_fanout == 0U) return result;
  const auto snapshot = snapshot_ ? snapshot_() : nullptr;
  if (snapshot && snapshot->stale()) result.omissions.push_back("code snapshot is stale");
  std::unordered_map<std::string, const index::RetrievalCodeNode*> code_by_id;
  std::unordered_map<std::string, std::vector<const index::RetrievalCodeEdge*>> code_out;
  std::unordered_map<std::string, std::vector<const index::RetrievalCodeEdge*>> code_in;
  if (snapshot) {
    for (const auto& node : snapshot->nodes()) {
      if (node.repository == identity_.repository && node.worktree == identity_.worktree)
        code_by_id.emplace(node.id, &node);
    }
    for (const auto& edge : snapshot->edges()) {
      code_out[edge.caller_id].push_back(&edge);
      for (const auto& callee : edge.callee_ids) code_in[callee].push_back(&edge);
    }
  }
  const auto code_node = [&](const RetrievalReference& ref)
      -> const index::RetrievalCodeNode* {
    if (!snapshot || snapshot->stale() || ref.kind != RetrievalOwnerKind::Code) return nullptr;
    const auto found = code_by_id.find(ref.id);
    if (found == code_by_id.end()) return nullptr;
    const auto& node = *found->second;
    if (!node.path.empty() &&
        index::retrieval_source_fingerprint(snapshot->root() / node.path) != node.source_fingerprint)
      return nullptr;
    const bool pinned = !ref.repository.empty() || !ref.worktree.empty() || !ref.path.empty() ||
                        !ref.source_fingerprint.empty() || ref.snapshot_generation != 0;
    if (pinned && (ref.repository.empty() || ref.worktree.empty() ||
                   ref.snapshot_generation == 0 ||
                   (node.kind != index::RetrievalCodeNodeKind::Project &&
                    (ref.path.empty() || ref.source_fingerprint.empty())))) return nullptr;
    if ((!ref.repository.empty() && ref.repository != node.repository) ||
        (!ref.worktree.empty() && ref.worktree != node.worktree) ||
        (!ref.path.empty() && ref.path != node.path.generic_string()) ||
        (!ref.source_fingerprint.empty() && ref.source_fingerprint != node.source_fingerprint) ||
        (ref.snapshot_generation != 0 && ref.snapshot_generation != node.generation)) return nullptr;
    return &node;
  };
  const auto normalize = [&](const RetrievalReference& ref) -> std::optional<RetrievalReference> {
    if (ref.kind != RetrievalOwnerKind::Code) return ref;
    const auto* node = code_node(ref);
    if (node == nullptr) return std::nullopt;
    return code_reference(*node);
  };
  const auto normalize_entity = [&](const RetrievalReference& ref)
      -> std::optional<RetrievalReference> {
    if (ref.kind != RetrievalOwnerKind::Entity || structured_ == nullptr || ref.id.empty())
      return ref.kind == RetrievalOwnerKind::Entity ? std::nullopt : std::optional<RetrievalReference>(ref);
    const auto matches = structured_->find_entities(scope, ref.id, query.policy,
                                                    query.include_global_user, 2);
    if (matches.size() != 1U) return std::nullopt;
    auto normalized = ref; normalized.id = matches.front().id;
    return normalized;
  };
  const auto visible = [&](const RetrievalReference& ref) {
    if (ref.kind == RetrievalOwnerKind::Code)
      return code_node(ref) != nullptr && (!authorizer_ || authorizer_(scope, ref));
    if (ref.kind == RetrievalOwnerKind::Entity) {
      if (structured_ == nullptr) return false;
      return structured_->find_entities(scope, ref.id, query.policy,
                                        query.include_global_user, 2).size() == 1U &&
             (!authorizer_ || authorizer_(scope, ref));
    }
    if (ref.kind != RetrievalOwnerKind::Memory) return false;
    const auto memory = memories_.find_for_recall(scope, ref.id, query.policy,
                                                  query.include_global_user);
    if (!memory || !scope_can_access(scope, memory->scope, query.policy,
                                     query.include_global_user) ||
        (memory->status != MemoryStatus::Active && memory->status != MemoryStatus::Validated))
      return false;
    return !authorizer_ || authorizer_(scope, ref);
  };
  struct Frontier { RetrievalReference ref; std::size_t depth; };
  std::vector<Frontier> frontier;
  std::unordered_set<std::string> visited;
  std::size_t examined_seeds = 0;
  for (const auto& seed : query.seeds) {
    if (result.nodes.size() >= max_results || examined_seeds++ >= 128) {
      result.omissions.push_back("seed budget exhausted");
      break;
    }
    auto normalized = normalize(seed.reference);
    if (normalized) normalized = normalize_entity(*normalized);
    if (!normalized || !visible(*normalized)) {
      result.omissions.push_back("seed endpoint omitted");
      continue;
    }
    if (seed.depth > max_depth) {
      result.omissions.push_back("seed depth exceeds traversal budget");
      continue;
    }
    if (visited.insert(key(*normalized)).second) {
      frontier.push_back({*normalized, seed.depth});
      result.nodes.push_back(*normalized);
    }
  }
  std::size_t examined = 0;
  while (!frontier.empty() && examined < max_edges && result.nodes.size() < max_results) {
    const auto current = frontier.front();
    frontier.erase(frontier.begin());
    if (current.depth >= max_depth) continue;
    std::vector<RetrievalGraphEdge> outgoing;
    if (current.ref.kind == RetrievalOwnerKind::Memory) {
      GraphQuery graph_query;
      graph_query.memory_id = current.ref.id;
      graph_query.limit = std::min(max_fanout, max_edges - examined);
      for (const auto& edge : graph_.query_edges(scope, graph_query)) {
        if (outgoing.size() >= max_fanout || examined >= max_edges) break;
        ++examined;
        const bool is_outgoing = edge.from_id == current.ref.id;
        if (!direction_ok(query.direction, is_outgoing) || !allowed(query, edge.type)) continue;
        const auto id = is_outgoing ? edge.to_id : edge.from_id;
        if (id.empty()) continue;
        RetrievalReference target;
        target.kind = RetrievalOwnerKind::Memory;
        target.id = id;
        outgoing.push_back({current.ref, {target}, edge.type,
                            edge.id.empty() ? edge.source : "graph_edge:" + edge.id,
                            "resolved", !is_outgoing});
      }
      if (structured_ != nullptr && examined < max_edges && outgoing.size() < max_fanout &&
          query.direction != RetrievalGraphDirection::Incoming) {
        const auto owner = memories_.find_for_recall(scope, current.ref.id,
            query.policy, query.include_global_user);
        const auto assertion = structured_->assertion(owner ? owner->scope : scope,
                                                       current.ref.id, query.as_of);
        const auto at = query.as_of.value_or(std::chrono::system_clock::now());
        const bool valid = owner && assertion && !assertion->evidence.empty() &&
            (current.ref.revision == 0 || current.ref.revision == assertion->revision) &&
            (!assertion->valid_from || at >= *assertion->valid_from) &&
            (!assertion->valid_to || at < *assertion->valid_to) &&
            (!authorizer_ || authorizer_(scope, {RetrievalOwnerKind::Memory,
                                                  assertion->memory_id, assertion->revision}));
        if (valid) {
          const auto evidence = "assertion:" + assertion->memory_id + ":" +
                                std::to_string(assertion->revision);
          const auto add_membership = [&](const std::string& relation,
                                          const std::string& entity_id) {
            if (entity_id.empty() || examined >= max_edges || outgoing.size() >= max_fanout ||
                !allowed(query, relation)) return;
            RetrievalGraphEdge projected;
            projected.from = current.ref;
            projected.candidates.push_back({RetrievalOwnerKind::Entity, entity_id});
            projected.relation = relation; projected.evidence_id = evidence;
            projected.resolution = "resolved"; projected.assertion_positive = assertion->positive;
            projected.valid_from = assertion->valid_from; projected.valid_to = assertion->valid_to;
            projected.assertion = *assertion;
            for (const auto& item : assertion->evidence) {
              const auto id = !item.source_id.empty() ? item.source_id : item.source_uri;
              if (!id.empty()) projected.assertion_evidence.push_back(id);
            }
            outgoing.push_back(std::move(projected)); ++examined;
          };
          add_membership("assertion_subject", assertion->subject_id);
          if (assertion->positive)
            add_membership("assertion_object", assertion->object_id);
        }
      }
    } else if (current.ref.kind == RetrievalOwnerKind::Entity && structured_ != nullptr) {
      if (query.allowed_relations.empty()) continue;
      std::vector<MemoryAssertion> assertions;
      if (query.direction != RetrievalGraphDirection::Incoming) {
        assertions = structured_->query_assertions(scope, current.ref.id, {}, {}, query.as_of,
                                                   query.policy, query.include_global_user, max_fanout);
      }
      if (query.direction != RetrievalGraphDirection::Outgoing) {
        const auto incoming = structured_->query_assertions(scope, {}, {}, current.ref.id,
                                                            query.as_of, query.policy,
                                                            query.include_global_user, max_fanout);
        assertions.insert(assertions.end(), incoming.begin(), incoming.end());
      }
      for (const auto& assertion : assertions) {
        if (outgoing.size() >= max_fanout || examined >= max_edges) break;
        const auto owner = memories_.find_for_recall(scope, assertion.memory_id,
                                                     query.policy, query.include_global_user);
        if (!owner || (owner->status != MemoryStatus::Active &&
                       owner->status != MemoryStatus::Validated)) continue;
        const auto at = query.as_of.value_or(std::chrono::system_clock::now());
        if (assertion.evidence.empty() ||
            (assertion.valid_from && at < *assertion.valid_from) ||
            (assertion.valid_to && at >= *assertion.valid_to)) continue;
        RetrievalReference assertion_owner;
        assertion_owner.kind = RetrievalOwnerKind::Memory;
        assertion_owner.id = assertion.memory_id;
        assertion_owner.revision = assertion.revision;
        if (authorizer_ && !authorizer_(scope, assertion_owner)) continue;
        const bool outgoing_assertion = assertion.subject_id == current.ref.id;
        if (!allowed(query, assertion.predicate)) continue;
        RetrievalGraphEdge projected;
        projected.from = current.ref; projected.relation = assertion.predicate;
        projected.incoming = !outgoing_assertion;
        projected.resolution = "resolved";
        projected.evidence_id = "assertion:" + assertion.memory_id + ":" +
                                std::to_string(assertion.revision);
        projected.assertion_positive = assertion.positive;
        projected.valid_from = assertion.valid_from; projected.valid_to = assertion.valid_to;
        projected.assertion = assertion;
        for (const auto& evidence : assertion.evidence) {
          const auto id = !evidence.source_id.empty() ? evidence.source_id : evidence.source_uri;
          if (!id.empty()) projected.assertion_evidence.push_back(id);
        }
        const auto target_id = outgoing_assertion ? assertion.object_id : assertion.subject_id;
        if (target_id.empty()) {
          // A literal is evidence on its governed owner, not an invented entity.
          if (!outgoing_assertion) continue;
          projected.candidates.push_back(assertion_owner);
        } else {
          projected.candidates.push_back({RetrievalOwnerKind::Entity, target_id});
        }
        outgoing.push_back(std::move(projected)); ++examined;
      }
    } else if (code_node(current.ref) != nullptr) {
      std::vector<const index::RetrievalCodeEdge*> code_edges;
      const auto append_edges = [&](const auto& edges) {
        for (const auto* edge : edges) {
          if (code_edges.size() >= std::min(max_fanout, max_edges - examined)) break;
          code_edges.push_back(edge);
        }
      };
      if (query.direction != RetrievalGraphDirection::Incoming) append_edges(code_out[current.ref.id]);
      if (query.direction != RetrievalGraphDirection::Outgoing) {
        const auto& incoming = code_in[current.ref.id];
        append_edges(incoming);
      }
      for (const auto* edge_ptr : code_edges) {
        if (outgoing.size() >= max_fanout || examined >= max_edges) break;
        ++examined;
        const auto& edge = *edge_ptr;
        const bool outgoing_edge = edge.caller_id == current.ref.id;
        const bool incoming_edge = !outgoing_edge && std::find(edge.callee_ids.begin(), edge.callee_ids.end(), current.ref.id) != edge.callee_ids.end();
        if ((!outgoing_edge && !incoming_edge) ||
            (!direction_ok(query.direction, outgoing_edge) &&
             !(incoming_edge && direction_ok(query.direction, false))) ||
            !allowed(query, edge.relation)) continue;
        RetrievalGraphEdge projected;
        projected.from = current.ref;
        projected.incoming = incoming_edge;
        projected.relation = edge.relation;
        projected.heuristic = edge.heuristic;
        projected.evidence_id = "code-snapshot:" + std::to_string(snapshot->generation()) +
                                ":" + edge.caller_id + ":" + edge.relation;
        projected.resolution = edge.ambiguous ? "ambiguous" :
                               (edge.callee_ids.empty() ? "unresolved" : "resolved");
        if (outgoing_edge) {
          for (const auto& id : edge.callee_ids) {
            if (projected.candidates.size() >= max_fanout) break;
            const auto found = code_by_id.find(id);
            if (found != code_by_id.end()) projected.candidates.push_back(code_reference(*found->second));
          }
        } else {
          const auto found = code_by_id.find(edge.caller_id);
          if (found != code_by_id.end()) projected.candidates.push_back(code_reference(*found->second));
        }
        if (projected.candidates.empty() && !edge.unresolved_name.empty())
          projected.resolution = "unresolved";
        outgoing.push_back(std::move(projected));
      }
    }
    if (links_ != nullptr && examined < max_edges && outgoing.size() < max_fanout) {
      for (const auto& link : links_->links(scope, current.ref, query.policy,
              query.include_global_user, std::min(max_fanout - outgoing.size(), max_edges - examined))) {
        if (examined >= max_edges) break;
        ++examined;
        if (!visible(link.from) || !visible(link.to)) {
          result.omissions.push_back("cross-store link endpoint omitted by visibility policy");
          continue;
        }
        if (!allowed(query, link.relation)) continue;
        const bool outgoing_link = key(link.from) == key(current.ref);
        const bool incoming_link = key(link.to) == key(current.ref);
        if ((!outgoing_link && !incoming_link) ||
            (!direction_ok(query.direction, outgoing_link) &&
             !(incoming_link && direction_ok(query.direction, false)))) continue;
        const auto target = outgoing_link ? link.to : link.from;
        outgoing.push_back({current.ref, {target}, link.relation, link.evidence_id, "resolved", incoming_link});
      }
    }
    std::size_t fanout = 0;
    for (const auto& edge : outgoing) {
      if (fanout++ >= max_fanout) break;
      RetrievalGraphEdge visible_edge = edge;
      visible_edge.candidates.clear();
      std::size_t remaining = max_results - result.nodes.size();
      for (const auto& candidate : edge.candidates) {
        if (!visible(candidate)) {
          result.omissions.push_back("edge endpoint omitted by visibility policy");
          continue;
        }
        if (visited.count(key(candidate)) == 0) {
          if (remaining == 0) continue;
          --remaining;
        }
        visible_edge.candidates.push_back(candidate);
      }
      if (visible_edge.candidates.empty() && !edge.candidates.empty()) continue;
      result.edges.push_back(visible_edge);
      for (const auto& candidate : visible_edge.candidates) {
        if (result.nodes.size() >= max_results) break;
        if (!visited.insert(key(candidate)).second) continue;
        result.nodes.push_back(candidate);
        if (result.nodes.size() < max_results && edge.assertion_positive)
          frontier.push_back({candidate, current.depth + 1U});
      }
      if (result.nodes.size() >= max_results) break;
    }
  }
  if (examined >= max_edges) result.omissions.push_back("edge budget exhausted");
  return result;
}

}  // namespace qt::memory
