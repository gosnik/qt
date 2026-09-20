#include "qt/memory/retrieval_engine.hpp"

#include "qt/memory/scope.hpp"
#include "qt/foundation/hash.hpp"
#include "qt/memory/retrieval_graph.hpp"
#include "qt/memory/recall_service.hpp"

#include <chrono>
#include <set>
#include <stdexcept>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace qt::memory {

RetrievalEngine::RetrievalEngine(SQLiteMemoryDatabase& database, MemoryScope scope,
                               RetrievalEngineOptions options)
    : database_(database), options_(std::move(options)),
      indexer_(database, options_.embedder),
      dense_(database, options_.embedder, options_.cache_entries), scope_(std::move(scope)) {
  validate_scope(scope_);
  status_.enabled = options_.enabled;
  if (options_.enabled && options_.background_indexing)
    worker_ = std::thread([this] { run(); });
}

RetrievalEngine::~RetrievalEngine() { stop(); }

void RetrievalEngine::stop() {
  stopping_.store(true);
  condition_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void RetrievalEngine::set_scope(MemoryScope scope) {
  validate_scope(scope);
  { std::lock_guard<std::mutex> lock(mutex_); scope_ = std::move(scope); }
  condition_.notify_all();
}

void RetrievalEngine::set_foreground_active(bool active) {
  { std::lock_guard<std::mutex> lock(mutex_); foreground_active_ = active; }
  condition_.notify_all();
}

void RetrievalEngine::prepare_embedding(const RetrievalCancellation& cancellation) {
  if (!options_.background_indexing) {
    if (!options_.configure) throw std::runtime_error("dense model is not configured");
    const auto config = options_.configure(cancellation);
    std::lock_guard<std::mutex> lock(mutex_);
    status_.generation = config.generation;
  }
  if (options_.validate_embedding) options_.validate_embedding();
}

std::vector<MemoryEntry> RetrievalEngine::search(const MemoryScope& scope,
    const std::string& query, RecallPolicy policy, bool include_global_user,
    std::size_t limit, const RetrievalCancellation& cancellation) {
  auto candidates = search_candidates(scope, query, policy, include_global_user, limit, cancellation);
  std::vector<MemoryEntry> result;
  result.reserve(candidates.size());
  for (auto& candidate : candidates) result.push_back(std::move(candidate.memory));
  return result;
}

std::vector<DenseMemoryCandidate> RetrievalEngine::search_candidates(const MemoryScope& scope,
    const std::string& query, RecallPolicy policy, bool include_global_user,
    std::size_t limit, const RetrievalCancellation& cancellation) {
  if (!options_.enabled || !options_.dense_enabled || stopping_.load()) return {};
  const auto cancelled = [&] {
    return stopping_.load() || (cancellation && cancellation());
  };
  if (cancelled()) return {};
  try {
    prepare_embedding(cancelled);
  } catch (const std::exception& error) {
    std::lock_guard<std::mutex> lock(mutex_); status_.last_error = error.what();
    return {};
  }
  const auto hits = dense_.search(scope, query, policy, include_global_user, limit, cancelled);
  if (cancelled()) return {};
  return database_.resolve_dense_memory_candidates(scope, hits, policy, include_global_user);
}

RetrievalEngineStatus RetrievalEngine::status() const {
  RetrievalEngineStatus result;
  MemoryScope scope;
  { std::lock_guard<std::mutex> lock(mutex_); result = status_; scope = scope_; }
  for (const auto& visible : recall_scopes(scope, options_.policy, options_.include_global_user)) {
    const auto jobs = database_.retrieval_index_status(visible);
    result.jobs.pending += jobs.pending; result.jobs.leased += jobs.leased;
    result.jobs.failed += jobs.failed; result.jobs.complete += jobs.complete;
    result.jobs.chunks += jobs.chunks;
  }
  result.queries = dense_.stats();
  result.code.enabled = options_.code_enabled && static_cast<bool>(options_.code_snapshot);
  if (result.code.enabled) {
    const auto snapshot = options_.code_snapshot();
    if (snapshot) {
      result.code.available = true;
      result.code.stale = snapshot->stale();
      result.code.generation = snapshot->generation();
      if (!snapshot->nodes().empty()) {
        result.code.repository = snapshot->nodes().front().repository;
        result.code.worktree = snapshot->nodes().front().worktree;
      }
      const auto jobs = database_.retrieval_index_status(code_scope(scope, *snapshot));
      result.jobs.pending += jobs.pending; result.jobs.leased += jobs.leased;
      result.jobs.failed += jobs.failed; result.jobs.complete += jobs.complete;
      result.jobs.chunks += jobs.chunks;
    }
  }
  return result;
}

MemoryScope RetrievalEngine::code_scope(const MemoryScope& caller,
                                        const index::RetrievalSnapshot& snapshot) const {
  auto scope = caller;
  scope.kind = ScopeKind::Exact;
  scope.task.clear(); scope.session.clear(); scope.conversation.clear();
  scope.agent = "qt-code-index:" + foundation::stable_hash_hex(snapshot.root().generic_string());
  return scope;
}

bool RetrievalEngine::validate_code(const MemoryScope& caller,
                                     const RetrievalReference& ref) const {
  if (!options_.code_enabled || !options_.code_snapshot || ref.kind != RetrievalOwnerKind::Code)
    return false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (caller.user != scope_.user || caller.organization != scope_.organization ||
        caller.project != scope_.project || caller.repository != scope_.repository ||
        caller.branch != scope_.branch) return false;
  }
  const auto snapshot = options_.code_snapshot();
  if (!snapshot || snapshot->stale() || ref.snapshot_generation != snapshot->generation()) return false;
  for (const auto& node : snapshot->nodes()) {
    if (node.id != ref.id) continue;
    if (ref.repository != node.repository || ref.worktree != node.worktree ||
        ref.path != node.path.generic_string() || ref.source_fingerprint != node.source_fingerprint)
      return false;
    return !node.path.empty() && index::retrieval_source_fingerprint(snapshot->root() / node.path) == node.source_fingerprint;
  }
  return false;
}

bool RetrievalEngine::validate_relation(const MemoryScope& scope,
    const RetrievalPathEdge& edge, RecallPolicy policy, bool include_global_user) const {
  if (edge.resolution != "resolved" || edge.evidence_id.empty()) return false;
  if (edge.heuristic && (edge.assertion || edge.from.kind != RetrievalOwnerKind::Code ||
                         edge.to.kind != RetrievalOwnerKind::Code)) return false;
  try {
    for (const auto& endpoint : {edge.from, edge.to}) {
      if (endpoint.kind != RetrievalOwnerKind::Entity) continue;
      const auto entities = database_.find_entities(scope, endpoint.id, policy, include_global_user, 2);
      if (entities.size() != 1 || entities.front().id != endpoint.id) return false;
    }
    if (edge.assertion) {
      const auto& captured = *edge.assertion;
      const auto owner = database_.find_for_recall(scope, captured.memory_id, policy, include_global_user);
      if (!owner || captured.revision < 1 ||
          database_.retrieval_revision(owner->scope, captured.memory_id) != captured.revision) return false;
      RecallService authority(database_, database_, database_, database_, database_);
      if (!authority.eligible_for_context(scope, captured.memory_id, policy, include_global_user)) return false;
      const auto actual = database_.assertion(owner->scope, captured.memory_id);
      if (!actual || actual->revision != captured.revision || actual->evidence.empty() ||
          actual->subject_id != captured.subject_id || actual->object_id != captured.object_id ||
          actual->predicate != captured.predicate || actual->positive != captured.positive ||
          actual->literal_json != captured.literal_json || actual->qualifiers_json != captured.qualifiers_json ||
          actual->valid_from != captured.valid_from || actual->valid_to != captured.valid_to ||
          actual->evidence.size() != captured.evidence.size()) return false;
      for (std::size_t i = 0; i < actual->evidence.size(); ++i) {
        const auto& a = actual->evidence[i]; const auto& b = captured.evidence[i];
        if (a.source_id != b.source_id || a.source_revision != b.source_revision ||
            a.source_uri != b.source_uri || a.source_hash != b.source_hash ||
            a.source_span != b.source_span || a.created_by != b.created_by) return false;
      }
      const auto now = std::chrono::system_clock::now();
      if ((actual->valid_from && now < *actual->valid_from) ||
          (actual->valid_to && now >= *actual->valid_to) ||
          edge.evidence_id != "assertion:" + captured.memory_id + ":" + std::to_string(captured.revision))
        return false;
      if (edge.from.kind == RetrievalOwnerKind::Memory && edge.to.kind == RetrievalOwnerKind::Entity)
        return edge.from.id == captured.memory_id && edge.from.revision == captured.revision &&
            ((edge.relation == "assertion_subject" && edge.to.id == captured.subject_id) ||
             (edge.relation == "assertion_object" && captured.positive && edge.to.id == captured.object_id));
      if (edge.from.kind == RetrievalOwnerKind::Entity && edge.to.kind == RetrievalOwnerKind::Memory)
        return captured.object_id.empty() && edge.from.id == captured.subject_id &&
            edge.to.id == captured.memory_id && edge.to.revision == captured.revision &&
            edge.relation == captured.predicate;
      return edge.from.kind == RetrievalOwnerKind::Entity && edge.to.kind == RetrievalOwnerKind::Entity &&
          edge.from.id == captured.subject_id && edge.to.id == captured.object_id &&
          edge.relation == captured.predicate;
    }
    const auto same = [](const RetrievalReference& a, const RetrievalReference& b) {
      return a.kind == b.kind && a.id == b.id && a.revision == b.revision &&
          a.repository == b.repository && a.worktree == b.worktree && a.path == b.path &&
          a.source_fingerprint == b.source_fingerprint && a.snapshot_generation == b.snapshot_generation;
    };
    for (const auto& link : database_.links(scope, edge.from, policy, include_global_user, 256)) {
      if (!edge.heuristic && link.relation == edge.relation && link.evidence_id == edge.evidence_id &&
          same(link.from, edge.from) && same(link.to, edge.to)) return true;
    }
    if (edge.from.kind == RetrievalOwnerKind::Memory && edge.to.kind == RetrievalOwnerKind::Memory) {
      GraphQuery query; query.memory_id = edge.from.id; query.type = edge.relation; query.limit = 256;
      for (const auto& stored : database_.query_edges(scope, query)) {
        const auto evidence = stored.id.empty() ? stored.source : "graph_edge:" + stored.id;
        if (stored.from_id == edge.from.id && stored.to_id == edge.to.id &&
            stored.type == edge.relation && evidence == edge.evidence_id) return true;
      }
    }
    if (edge.from.kind == RetrievalOwnerKind::Code && edge.to.kind == RetrievalOwnerKind::Code &&
        options_.code_snapshot && validate_code(scope, edge.from) && validate_code(scope, edge.to)) {
      const auto snapshot = options_.code_snapshot();
      if (!snapshot || snapshot->stale()) return false;
      for (const auto& stored : snapshot->edges()) {
        const auto evidence = "code-snapshot:" + std::to_string(snapshot->generation()) +
                              ":" + stored.caller_id + ":" + stored.relation;
        if (stored.caller_id == edge.from.id && stored.relation == edge.relation &&
            !stored.ambiguous && stored.heuristic == edge.heuristic && evidence == edge.evidence_id &&
            std::find(stored.callee_ids.begin(), stored.callee_ids.end(), edge.to.id) != stored.callee_ids.end())
          return true;
      }
    }
  } catch (...) {
    return false;
  }
  return false;
}

std::vector<RetrievalCandidate> RetrievalEngine::search_code(const MemoryScope& scope,
    const std::string& query, std::size_t limit, const RetrievalCancellation& cancellation,
    const std::vector<RetrievalReference>& memory_seeds,
    const std::vector<RetrievalCandidate>& contextual_seeds,
    std::optional<std::pair<RecallPolicy, bool>> access) {
  const auto policy = access ? access->first : options_.policy;
  const bool include_global_user = access ? access->second : options_.include_global_user;
  std::vector<RetrievalCandidate> result;
  limit = std::min<std::size_t>(limit, 1000);
  if (!options_.code_enabled || !options_.code_snapshot || limit == 0 || query.empty()) return result;
  const auto cancelled = [&] { return stopping_.load() || (cancellation && cancellation()); };
  if (cancelled()) return result;
  const auto snapshot = options_.code_snapshot();
  if (!snapshot || snapshot->stale()) return result;
  std::unordered_map<std::string, std::size_t> owners;
  const auto merge_ranks = [](RetrievalCandidate& target, const RetrievalCandidate& source) {
    for (const auto& rank : source.ranks) {
      const auto prior = std::find_if(target.ranks.begin(), target.ranks.end(),
          [&](const auto& value) { return value.retriever == rank.retriever; });
      if (prior == target.ranks.end()) target.ranks.push_back(rank);
      else if (rank.rank < prior->rank) *prior = rank;
    }
  };
  const auto merge_matched = [](RetrievalCandidate& target, const RetrievalCandidate& source) {
    for (const auto& hit : source.matched_chunks) {
      const auto duplicate = std::any_of(target.matched_chunks.begin(), target.matched_chunks.end(),
          [&](const auto& prior) {
            return prior.chunk.id == hit.chunk.id && prior.generation == hit.generation &&
                   prior.chunk.owner_revision == hit.chunk.owner_revision;
          });
      if (!duplicate) target.matched_chunks.push_back(hit);
    }
  };
  const auto add = [&](RetrievalCandidate candidate) {
    if (!validate_code(scope, candidate.owner)) return false;
    const auto owner_key = std::to_string(static_cast<int>(candidate.owner.kind)) + ":" + candidate.owner.id;
    const auto existing = owners.find(owner_key);
    if (existing != owners.end()) {
      auto& prior = result[existing->second];
      merge_ranks(prior, candidate);
      merge_matched(prior, candidate);
      if (prior.path.empty()) {
        prior.path = std::move(candidate.path);
        prior.evidence_ids = std::move(candidate.evidence_ids);
      }
      return true;
    }
    const auto snippet = snapshot->read_snippet(candidate.owner.id);
    if (!snippet || !snippet->current) return false;
    RetrievalChunk chunk;
    chunk.id = snippet->chunk.id; chunk.owner_key = candidate.owner.id;
    chunk.owner_revision = candidate.owner.revision;
    chunk.text = snippet->chunk.text; chunk.content_hash = foundation::stable_hash_hex(chunk.text);
    chunk.source_uri = candidate.owner.path;
    chunk.source_span = std::to_string(snippet->chunk.first_line) + ":" + std::to_string(snippet->chunk.last_line);
    chunk.render_version = "code-window-v1";
    candidate.chunks = {std::move(chunk)}; candidate.eligible = true;
    owners[owner_key] = result.size(); result.push_back(std::move(candidate));
    return true;
  };
  const auto add_graph_memory = [&](RetrievalCandidate candidate) {
    if (candidate.owner.kind == RetrievalOwnerKind::Memory) {
      const auto memory = database_.find_for_recall(scope, candidate.owner.id,
                                                  policy, include_global_user);
      if (!memory || (memory->status != MemoryStatus::Active &&
                    memory->status != MemoryStatus::Validated)) return;
    } else if (candidate.owner.kind == RetrievalOwnerKind::Entity) {
      const auto entities = database_.find_entities(scope, candidate.owner.id, policy,
                                                     include_global_user, 2);
      if (entities.size() != 1 || entities.front().id != candidate.owner.id) return;
    } else return;
    const auto owner_key = std::to_string(static_cast<int>(candidate.owner.kind)) + ":" + candidate.owner.id;
    const auto existing = owners.find(owner_key);
    if (existing != owners.end()) {
      auto& prior = result[existing->second];
      merge_ranks(prior, candidate);
      if (prior.path.empty() || candidate.path.size() < prior.path.size()) {
        prior.path = std::move(candidate.path);
        prior.evidence_ids = std::move(candidate.evidence_ids);
      }
      return;
    }
    owners[owner_key] = result.size(); result.push_back(std::move(candidate));
  };
  try {
    if (options_.code_lexical) {
      // Providers may return several matching chunks for one owner. Bound the
      // raw examination separately from the requested distinct-owner budget.
      const auto matches = options_.code_lexical(query, 1000);
      std::unordered_set<std::string> lexical_owners;
      for (std::size_t i = 0; i < std::min<std::size_t>(1000U, matches.size()) &&
                              lexical_owners.size() < limit; ++i) {
        if (cancelled()) return {};
        if (lexical_owners.count(matches[i].id) != 0U)
          continue;
        RetrievalCandidate candidate; candidate.owner = matches[i];
        candidate.ranks.push_back({"code_bm25", lexical_owners.size() + 1U, 0});
        if (add(std::move(candidate))) lexical_owners.insert(matches[i].id);
      }
    }
  } catch (const std::exception& error) {
    std::lock_guard<std::mutex> lock(mutex_); status_.last_error = error.what();
  }
  try {
    if (options_.dense_enabled) {
      prepare_embedding(cancelled);
      const auto indexed_scope = code_scope(scope, *snapshot);
      const auto hits = dense_.search(indexed_scope, query, RecallPolicy::Strict, false, limit, cancelled);
      for (auto& candidate : database_.resolve_code_hits(indexed_scope, hits)) add(std::move(candidate));
    }
  } catch (const std::exception& error) {
    std::lock_guard<std::mutex> lock(mutex_); status_.last_error = error.what();
  }
  if (options_.code_graph_enabled) try {
    RetrievalGraphIdentity identity;
    const auto identity_source = std::find_if(result.begin(), result.end(),
                                              [](const auto& item) {
      return item.owner.kind == RetrievalOwnerKind::Code;
    });
    if (identity_source != result.end()) {
      identity.repository = identity_source->owner.repository;
      identity.worktree = identity_source->owner.worktree;
    }
    if (identity.repository.empty() && !snapshot->nodes().empty()) {
      identity.repository = snapshot->nodes().front().repository;
      identity.worktree = snapshot->nodes().front().worktree;
    }
    RecallService authority(database_, database_, database_, database_, database_);
    RetrievalGraphAdapter graph(database_, database_, [snapshot] { return snapshot; },
        std::move(identity), &database_, [&](const MemoryScope& visible, const RetrievalReference& ref) {
          if (ref.kind != RetrievalOwnerKind::Memory) return true;
          const auto memory = database_.find_for_recall(visible, ref.id, policy,
                                                        include_global_user);
          return memory && (ref.revision == 0 ||
              database_.retrieval_revision(memory->scope, ref.id) == ref.revision) &&
              authority.eligible_for_context(visible, ref.id, policy, include_global_user);
        }, &database_);
    RetrievalGraphQuery graph_query;
    graph_query.policy = policy;
    graph_query.include_global_user = include_global_user;
    graph_query.max_depth = std::min<std::size_t>(options_.graph_max_depth, 8);
    graph_query.max_fanout = std::min<std::size_t>(options_.graph_max_fanout, 256);
    graph_query.max_edges = std::min<std::size_t>(options_.graph_max_edges, 8192);
    graph_query.max_results = std::min<std::size_t>({limit + 16, options_.graph_max_results, 1000});
    graph_query.allowed_relations = {"related", "related_to", "documents", "discusses",
        "applies_to", "supported_by", "supports", "derived_from", "depends_on",
        "contains_file", "defines", "calls", "uses", "assertion_subject", "assertion_object"};
    const auto seed_limit = std::min<std::size_t>(options_.graph_max_seeds, 128);
    const auto key = [](const RetrievalReference& ref) {
      return std::to_string(static_cast<int>(ref.kind)) + ":" + ref.id;
    };
    std::unordered_map<std::string, std::vector<RetrievalPathEdge>> paths;
    // Interleave independent code and memory seeds so neither modality exhausts
    // the shared seed budget before the other can contribute.
    for (std::size_t i = 0; graph_query.seeds.size() < seed_limit &&
         (i < result.size() || i < memory_seeds.size() || i < contextual_seeds.size()); ++i) {
      if (i < result.size() && result[i].owner.kind == RetrievalOwnerKind::Code)
        graph_query.seeds.push_back({result[i].owner});
      if (graph_query.seeds.size() < seed_limit && i < memory_seeds.size() &&
          (memory_seeds[i].kind == RetrievalOwnerKind::Memory ||
           memory_seeds[i].kind == RetrievalOwnerKind::Entity))
        graph_query.seeds.push_back({memory_seeds[i]});
      if (graph_query.seeds.size() < seed_limit && i < contextual_seeds.size()) {
        if (cancelled()) return {};
        const auto& seed = contextual_seeds[i];
        if (!seed.eligible || seed.path.empty() || seed.path.size() >= graph_query.max_depth ||
            seed.path.size() > graph_query.max_edges ||
            (seed.owner.kind != RetrievalOwnerKind::Memory && seed.owner.kind != RetrievalOwnerKind::Entity))
          continue;
        bool valid = false;
        std::unordered_set<std::string> prefix_nodes;
        for (const auto& edge : seed.path) {
          if (!validate_relation(scope, edge, policy, include_global_user)) {
            valid = false; break;
          }
          valid = edge.assertion &&
              ((seed.owner.kind == RetrievalOwnerKind::Memory && edge.assertion->memory_id == seed.owner.id) ||
               (seed.owner.kind == RetrievalOwnerKind::Entity &&
                (edge.from.id == seed.owner.id || edge.to.id == seed.owner.id)));
          if (edge.assertion && !edge.assertion->positive) { valid = false; break; }
          prefix_nodes.insert(key(edge.from)); prefix_nodes.insert(key(edge.to));
          if (edge.assertion) prefix_nodes.insert(std::to_string(static_cast<int>(RetrievalOwnerKind::Memory)) +
                                                   ":" + edge.assertion->memory_id);
        }
        if (!valid) continue;
        prefix_nodes.erase(key(seed.owner));
        if (prefix_nodes.size() >= graph_query.max_results) continue;
        graph_query.max_results -= prefix_nodes.size();
        graph_query.max_edges -= seed.path.size();
        graph_query.seeds.push_back({seed.owner, seed.path.size()});
        paths.emplace(key(seed.owner), seed.path);
      }
    }
    if (cancelled()) return {};
    const auto graph_result = graph.traverse(scope, graph_query);
    const auto pin = [&](RetrievalReference ref) {
      if (ref.kind == RetrievalOwnerKind::Memory) {
        const auto memory = database_.find_for_recall(scope, ref.id, policy,
                                                       include_global_user);
        if (memory) ref.revision = database_.retrieval_revision(memory->scope, ref.id);
      }
      return ref;
    };
    for (const auto& seed : graph_query.seeds) paths.emplace(key(seed.reference), std::vector<RetrievalPathEdge>{});
    std::vector<RetrievalCandidate> graph_candidates;
    for (const auto& edge : graph_result.edges) {
      if (cancelled()) return {};
      const auto parent = paths.find(key(edge.from));
      if (parent == paths.end() || edge.resolution != "resolved") continue;
      const auto parent_path = parent->second;
      for (const auto& target : edge.candidates) {
        if (std::any_of(parent_path.begin(), parent_path.end(), [&](const auto& prior) {
              return key(prior.from) == key(target) || key(prior.to) == key(target) ||
                  (prior.assertion && target.kind == RetrievalOwnerKind::Memory &&
                   prior.assertion->memory_id == target.id);
            })) continue;
        const auto existing_path = paths.find(key(target));
        if (existing_path != paths.end() &&
            (!existing_path->second.empty() || !parent_path.empty() ||
             target.kind == edge.from.kind)) continue;
        auto path = parent_path;
        path.push_back({pin(edge.incoming ? target : edge.from),
                        pin(edge.incoming ? edge.from : target), edge.relation,
                        edge.evidence_id, edge.resolution, edge.assertion, edge.heuristic});
        if (edge.assertion_positive) paths.emplace(key(target), path);
        bool has_code = false, has_memory = false, has_entity = false;
        for (const auto& hop : path) {
          has_code = has_code || hop.from.kind == RetrievalOwnerKind::Code || hop.to.kind == RetrievalOwnerKind::Code;
          has_memory = has_memory || hop.from.kind == RetrievalOwnerKind::Memory || hop.to.kind == RetrievalOwnerKind::Memory || hop.assertion.has_value();
          has_entity = has_entity || hop.from.kind == RetrievalOwnerKind::Entity || hop.to.kind == RetrievalOwnerKind::Entity;
        }
        RetrievalCandidate linked;
        linked.owner = pin(target); linked.eligible = true;
        linked.ranks.push_back({"code_graph", graph_candidates.size() + 1, 0.0});
        if (has_code && (has_memory || has_entity)) linked.path = std::move(path);
        for (const auto& hop : linked.path)
          if (!hop.evidence_id.empty()) linked.evidence_ids.push_back(hop.evidence_id);
        // Intermediate code nodes supply the source windows required to render
        // a longer path; pure-memory paths remain owned by RecallService.
        if (linked.owner.kind == RetrievalOwnerKind::Code || !linked.path.empty())
          graph_candidates.push_back(std::move(linked));
      }
    }
    for (auto& candidate : graph_candidates) {
      if (candidate.owner.kind == RetrievalOwnerKind::Code) add(std::move(candidate));
      else add_graph_memory(std::move(candidate));
    }
  } catch (const std::exception& error) {
    std::lock_guard<std::mutex> lock(mutex_); status_.last_error = error.what();
  }
  if (cancelled() || options_.code_snapshot() != snapshot) return {};
  for (auto& candidate : result)
    for (const auto& rank : candidate.ranks) candidate.fused_score += 1.0 / (60.0 + rank.rank);
  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
    return a.fused_score != b.fused_score ? a.fused_score > b.fused_score : a.owner.id < b.owner.id;
  });
  if (result.size() > limit) result.resize(limit);
  return result;
}

void RetrievalEngine::run() {
  const auto cancelled = [this] { return stopping_.load(); };
  const auto wait_until_ready = [this](std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout, [this] {
      return stopping_.load() || !foreground_active_;
    });
  };
  std::optional<DenseIndexConfig> config;
  std::set<std::string> initialized;
  std::shared_ptr<const index::RetrievalSnapshot> published_code;
  std::string published_code_scope;
  auto configure_after = std::chrono::steady_clock::time_point::min();
  while (!stopping_.load()) {
    MemoryScope scope;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      condition_.wait(lock, [&] { return stopping_.load() || !foreground_active_; });
      if (stopping_.load()) break;
      scope = scope_; status_.running = true;
    }
    bool worked = false;
    try {
      if (config && options_.validate_embedding) options_.validate_embedding();
      if (options_.dense_enabled && !config &&
          std::chrono::steady_clock::now() >= configure_after) {
        configure_after = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        if (!options_.configure) throw std::runtime_error("dense model is not configured");
        config = options_.configure(cancelled);
        std::lock_guard<std::mutex> lock(mutex_);
        status_.generation = config->generation; status_.last_error.clear();
      }
      auto scopes = recall_scopes(scope, options_.policy, options_.include_global_user);
      if (options_.code_enabled && options_.code_snapshot) {
        const auto snapshot = options_.code_snapshot();
        if (snapshot && !snapshot->stale()) {
          const auto indexed_scope = code_scope(scope, *snapshot);
          const auto key = scope_key(indexed_scope);
          if (snapshot != published_code || key != published_code_scope) {
            database_.sync_code_snapshot(indexed_scope, *snapshot, cancelled);
            published_code = snapshot; published_code_scope = key;
          }
          scopes.push_back(indexed_scope);
        }
      }
      for (const auto& visible : scopes) {
        if (stopping_.load()) break;
        { std::lock_guard<std::mutex> lock(mutex_); if (foreground_active_) break; }
        const auto key = scope_key(visible);
        if (config && initialized.count(key) == 0) {
          database_.begin_dense_index(visible, *config);
          initialized.insert(key);
        }
        const auto step = indexer_.step(visible, cancelled,
            config ? config->generation : std::string{}, wait_until_ready);
        worked = worked || step.worked;
        if (config) {
          const auto active = database_.dense_index(visible);
          if (!active || active->generation != config->generation)
            (void)database_.activate_dense_index(visible, config->generation);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (step.completed) ++status_.completed_jobs;
        if (!step.error.empty()) status_.last_error = step.error;
      }
    } catch (const std::exception& error) {
      std::lock_guard<std::mutex> lock(mutex_); status_.last_error = error.what();
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_); status_.last_error = "retrieval worker failed";
    }
    {
      std::unique_lock<std::mutex> lock(mutex_);
      status_.running = false;
      if (!worked) condition_.wait_for(lock, std::chrono::milliseconds(500));
    }
  }
  std::lock_guard<std::mutex> lock(mutex_); status_.running = false;
}

}  // namespace qt::memory
