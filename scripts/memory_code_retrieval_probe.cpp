#include "qt/memory/database.hpp"
#include "qt/memory/retrieval_engine.hpp"
#include "qt/memory/context_service.hpp"
#include "qt/memory/coordinator.hpp"
#include "qt/memory/retrieval_explanation.hpp"

#include "qt/foundation/hash.hpp"

#include <cassert>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <thread>
#include <nlohmann/json.hpp>

using namespace qt::memory;
using namespace qt::index;

static MemoryScope caller_scope() {
  MemoryScope scope; scope.kind = ScopeKind::Repository; scope.user = "code-probe";
  scope.project = "project"; scope.repository = "repo"; return scope;
}

static std::shared_ptr<const RetrievalSnapshot> make_snapshot(
    const std::filesystem::path& root, std::size_t generation, const std::string& text) {
  std::ofstream(root / "code.cpp") << text;
  ProjectScanResult scan; scan.root = root; scan.files.push_back({"code.cpp", ProjectFileKind::CxxSource});
  CodeIndex index; index.functions.push_back({"unique_code_symbol", "unique_code_symbol", "code.cpp", 1, "function"});
  std::unordered_map<std::string, std::string> fingerprints;
  fingerprints.emplace("code.cpp", retrieval_source_fingerprint(root / "code.cpp"));
  return RetrievalSnapshot::build(scan, index, generation, fingerprints, false);
}

static MemoryScope indexed_scope(const MemoryScope& caller, const RetrievalSnapshot& snapshot) {
  auto scope = caller; scope.kind = ScopeKind::Exact; scope.task.clear(); scope.session.clear(); scope.conversation.clear();
  scope.agent = "qt-code-index:" + qt::foundation::stable_hash_hex(snapshot.root().generic_string());
  return scope;
}

static void probe_dense_memory_evidence(SQLiteMemoryDatabase& database, MemoryScope scope) {
  scope.repository = "dense-evidence-repository";
  MemoryWrite write;
  write.status = MemoryStatus::Validated;
  write.content = std::string(2500, 'x') + "\nSELECTED_CHUNK_EVIDENCE explains the durable choice.";
  write.provenance.source_uri = "test://memory/dense-evidence";
  const auto id = database.add(scope, write);
  DenseIndexConfig config;
  config.generation = "memory-evidence-generation";
  config.model_id = "evidence-model"; config.model_fingerprint = "evidence-model-v1";
  config.tokenizer_fingerprint = "evidence-tokenizer-v1";
  config.query_prefix = "query: "; config.document_prefix = "document: ";
  config.pooling = "mean"; config.dimensions = 2;
  auto embed = [](const DenseIndexConfig& model, const std::string& text,
                  const RetrievalCancellation&) {
    const bool selected = text.rfind(model.query_prefix, 0) == 0 ||
        text.find("SELECTED_CHUNK_EVIDENCE") != std::string::npos;
    return selected ? std::vector<float>{1.0F, 0.0F} : std::vector<float>{0.0F, 1.0F};
  };
  database.begin_dense_index(scope, config);
  RetrievalIndexer indexer(database, embed);
  while (indexer.step(scope, {}, config.generation).worked) {}
  RetrievalEngineOptions engine_options;
  engine_options.enabled = true; engine_options.dense_enabled = true;
  engine_options.background_indexing = false; engine_options.embedder = embed;
  engine_options.configure = [&](const RetrievalCancellation&) { return config; };
  RetrievalEngine engine(database, scope, engine_options);
  const auto captured = engine.search_candidates(scope, "nonlexical-query", RecallPolicy::Strict, false, 5);
  assert(captured.size() == 1 && captured.front().memory.id == id);
  const auto& match = captured.front().hit;
  assert(match.generation == config.generation && match.similarity == 1.0);
  assert(match.chunk.ordinal > 0 && match.chunk.owner_key == "memory:" + id);
  assert(match.chunk.text.find("SELECTED_CHUNK_EVIDENCE") != std::string::npos);
  assert(match.chunk.content_hash == qt::foundation::stable_hash_hex(match.chunk.text));
  assert(match.chunk.source_uri == write.provenance.source_uri);
  assert(match.chunk.source_span.rfind("bytes:", 0) == 0);
  assert(match.chunk.owner_revision == database.retrieval_revision(scope, id));
  auto forged = match;
  forged.chunk.text += " forged";
  assert(database.resolve_dense_memory_candidates(scope, {forged}, RecallPolicy::Strict, false).empty());
  forged = match; forged.chunk.source_span = "bytes:0:1";
  assert(database.resolve_dense_memory_candidates(scope, {forged}, RecallPolicy::Strict, false).empty());
  forged = match; forged.generation = "unknown-generation";
  assert(database.resolve_dense_memory_candidates(scope, {forged}, RecallPolicy::Strict, false).empty());

  auto provider_results = captured;
  RecallService recall(database, DenseCandidateProvider{
      [&](const MemoryScope&, const RecallRequest&) { return provider_results; }});
  RecallRequest request;
  request.query = "nonlexical-query"; request.policy = RecallPolicy::Strict;
  request.dense_enabled = true; request.include_candidates = false;
  const auto recalled = recall.recall(scope, request);
  assert(recalled.size() == 1 && recalled.front().dense_match);
  ContextService context(database, nullptr, &database);
  ContextOptions options; options.policy = RecallPolicy::Strict; options.token_budget = 8192;
  const auto pack = context.build(scope, recalled, options);
  assert(pack.memory_ids == std::vector<std::string>{id});
  const auto explanation = recalled_memory_json(recalled.front(), match.chunk.owner_revision, &pack);
  assert(explanation["dense_match"]["chunk"]["id"] == match.chunk.id);
  assert(explanation["dense_match"]["chunk"]["source_span"] == match.chunk.source_span);
  assert(explanation["dense_match"]["index_generation"] == config.generation);
  assert(explanation["dense_match"]["similarity"] == 1.0);
  assert(explanation["dense_match"]["rank"] == 1);
  assert(explanation["dense_match"]["revision_is_current"] == true);
  assert(explanation["revision"]["basis"] == "dense_capture");
  assert(explanation["selection"]["selected"] == true);

  // Hydrating identical restored prose must not erase the old vector revision.
  MemoryPatch edit; edit.content = "intermediate content";
  database.update(scope, id, edit);
  edit.content = write.content; database.update(scope, id, edit);
  provider_results.front().memory = database.get(scope, id);
  assert(provider_results.front().memory.content == captured.front().memory.content);
  assert(recall.recall(scope, request).empty());
  auto restored_snapshot = recalled;
  restored_snapshot.front().memory = provider_results.front().memory;
  const auto rejected = context.build(scope, restored_snapshot, options);
  assert(rejected.memory_ids.empty());
  const auto stale_explanation = recalled_memory_json(restored_snapshot.front(),
      database.retrieval_revision(scope, id), &rejected);
  assert(stale_explanation["revision"]["number"] == match.chunk.owner_revision);
  assert(stale_explanation["dense_match"]["revision_is_current"] == false);
  assert(stale_explanation["selection"]["selected"] == false);
  assert(database.resolve_dense_memory_candidates(scope, {match}, RecallPolicy::Strict, false).empty());
  ContextService unverified(database);
  assert(unverified.build(scope, recalled, options).memory_ids.empty());
}

int main() {
  const auto root = std::filesystem::temp_directory_path() / "qt-memory-code-probe";
  const auto other_root = std::filesystem::temp_directory_path() / "qt-memory-code-probe-other";
  std::filesystem::remove_all(root); std::filesystem::remove_all(other_root);
  std::filesystem::create_directories(root); std::filesystem::create_directories(other_root);
  const auto snapshot = make_snapshot(root, 1, "int unique_code_symbol() { return 42; }\n");
  const auto other_snapshot = make_snapshot(other_root, 1, "int unique_code_symbol() { return 9; }\n");
  const auto caller = caller_scope();
  SQLiteMemoryDatabase database(root / ".db");
  const auto indexed = indexed_scope(caller, *snapshot);
  database.sync_code_snapshot(indexed, *snapshot);
  auto embed = [](const DenseIndexConfig&, const std::string& text, const RetrievalCancellation&) {
    return std::vector<float>{text.find("unique_code_symbol") != std::string::npos ? 1.0F : 0.0F, 1.0F};
  };
  RetrievalIndexer indexer(database, embed);
  while (indexer.step(indexed).worked) {}
  DenseIndexConfig config; config.generation = "code-model-1"; config.model_id = "probe";
  config.model_fingerprint = "probe"; config.tokenizer_fingerprint = "probe";
  config.pooling = "mean"; config.dimensions = 2;
  database.begin_dense_index(indexed, config);
  while (indexer.step(indexed, {}, config.generation).worked) {}
  assert(database.dense_index(indexed, config.generation).has_value());
  const auto vectors_before = database.search_dense(indexed, config.generation, {1.0F, 1.0F},
                                                    RecallPolicy::Strict, false, 10);
  assert(!vectors_before.empty());
  auto stale_hit = vectors_before.front();
  ++stale_hit.chunk.owner_revision;
  const auto resolved_duplicate = database.resolve_code_hits(
      indexed, {stale_hit, vectors_before.front()});
  assert(resolved_duplicate.size() == 1);

  std::shared_ptr<const RetrievalSnapshot> current = snapshot;
  RetrievalEngineOptions options; options.code_enabled = true; options.code_graph_enabled = true;
  options.dense_enabled = true;
  options.code_snapshot = [&] { return current; }; options.embedder = embed;
  RetrievalEngine engine(database, caller, options);
  MemoryWrite linked_write; linked_write.status = MemoryStatus::Active;
  linked_write.content = "memory linked to the unique code symbol";
  linked_write.provenance.source_id = "mixed-source";
  linked_write.provenance.source_hash = "version-a";
  const auto linked_memory_id = database.add(caller, linked_write);
  const auto linked_revision = database.retrieval_revision(caller, linked_memory_id);
  MemoryWrite file_only_write; file_only_write.status = MemoryStatus::Active;
  file_only_write.content = "nonlexical provenance query for the implementation file";
  file_only_write.provenance.source_id = "file-only-source";
  const auto file_only_id = database.add(caller, file_only_write);
  const auto file_only_revision = database.retrieval_revision(caller, file_only_id);
  MemoryWrite decision_write; decision_write.status = MemoryStatus::Validated;
  decision_write.content = "Preserve the recovery decision behind this implementation.";
  const auto decision_id = database.add(caller, decision_write);
  GraphEdge decision_edge;
  decision_edge.from_id = linked_memory_id; decision_edge.to_id = decision_id;
  decision_edge.type = "derived_from"; decision_edge.source = "probe:evidence";
  database.add_edge(caller, decision_edge);
  bool linked = false;
  RetrievalReference file_ref;
  RetrievalReference symbol_ref;
  for (const auto& linked_node : snapshot->nodes()) {
    if (linked_node.path.empty()) continue;
    CrossStoreLink cross_link;
    cross_link.from = {RetrievalOwnerKind::Memory, linked_memory_id, linked_revision};
    cross_link.to = {RetrievalOwnerKind::Code, linked_node.id, 0, linked_node.repository,
                     linked_node.worktree, linked_node.path.generic_string(),
                     linked_node.source_fingerprint, linked_node.generation};
    cross_link.relation = "documents"; cross_link.evidence_id = "uri:file:///code-probe";
    database.add_cross_store_link(caller, cross_link); linked = true;
    if (linked_node.kind == RetrievalCodeNodeKind::File) file_ref = cross_link.to;
    if (linked_node.kind == RetrievalCodeNodeKind::Function) symbol_ref = cross_link.to;
  }
  assert(linked);
  RetrievalEngineOptions duplicate_options = options;
  duplicate_options.dense_enabled = false;
  duplicate_options.code_graph_enabled = false;
  duplicate_options.code_lexical = [&](const std::string&, std::size_t raw_limit) {
    std::vector<RetrievalReference> matches{file_ref, file_ref, file_ref, symbol_ref};
    matches.resize(std::min(raw_limit, matches.size()));
    return matches;
  };
  RetrievalEngine duplicate_engine(database, caller, duplicate_options);
  const auto duplicate_hits = duplicate_engine.search_code(caller, "duplicate query", 2);
  assert(duplicate_hits.size() == 2);
  for (std::size_t i = 0; i < duplicate_hits.size(); ++i) {
    const auto& candidate = duplicate_hits[i];
    assert(candidate.owner.id == (i == 0 ? file_ref.id : symbol_ref.id));
    assert(candidate.ranks.size() == 1);
    assert(candidate.ranks.front().retriever == "code_bm25");
    assert(candidate.ranks.front().rank == i + 1);
    assert(std::abs(candidate.fused_score - 1.0 / (61.0 + i)) < 1e-12);
  }
  database.add_cross_store_link(caller, CrossStoreLink{
      RetrievalReference{RetrievalOwnerKind::Memory, file_only_id, file_only_revision},
      file_ref, "documents", "uri:file-only-evidence"});
  const auto hits = engine.search_code(caller, "unique query", 10);
  assert(!hits.empty());
  assert(std::any_of(hits.begin(), hits.end(), [](const auto& hit) {
    return hit.owner.kind == RetrievalOwnerKind::Code;
  }));
  assert(std::any_of(hits.begin(), hits.end(), [&](const auto& hit) {
    return hit.owner.kind == RetrievalOwnerKind::Memory && hit.owner.id == linked_memory_id &&
           !hit.path.empty() && !hit.evidence_ids.empty();
  }));
  // Dense code candidates retain the matched representation chunk as transient
  // supporting evidence while still rendering the canonical source window.
  const auto dense_code = std::find_if(hits.begin(), hits.end(), [](const auto& candidate) {
    return candidate.owner.kind == RetrievalOwnerKind::Code && !candidate.matched_chunks.empty();
  });
  assert(dense_code != hits.end());
  assert(!dense_code->chunks.empty());
  assert(dense_code->chunks.front().render_version == "code-window-v1");
  for (const auto& hit : dense_code->matched_chunks) {
    assert(!hit.chunk.owner_key.empty());
    assert(hit.chunk.owner_key.size() > dense_code->owner.id.size() &&
           hit.chunk.owner_key.compare(hit.chunk.owner_key.size() - dense_code->owner.id.size(),
                                       dense_code->owner.id.size(), dense_code->owner.id) == 0);
    assert(hit.chunk.owner_revision == dense_code->owner.revision);
    assert(hit.generation == config.generation);
    assert(hit.similarity > 0.0);
    assert(!hit.chunk.content_hash.empty());
    assert(hit.chunk.id != dense_code->chunks.front().id);
  }
  const auto dense_explanation = retrieval_candidate_json(*dense_code);
  assert(dense_explanation["matched_chunks"].size() == dense_code->matched_chunks.size());
  assert(dense_explanation["matched_chunks"][0]["chunk"]["id"] ==
         dense_code->matched_chunks.front().chunk.id);
  assert(dense_explanation["matched_chunks"][0]["index_generation"] == config.generation);
  assert(dense_explanation["matched_chunks"][0]["similarity"].get<double>() > 0.0);
  for (const auto& candidate : hits) {
    std::set<std::string> matched_keys;
    for (const auto& hit : candidate.matched_chunks)
      assert(matched_keys.insert(hit.chunk.id + ":" + hit.generation + ":" +
                                 std::to_string(hit.chunk.owner_revision)).second);
  }
  // Retrieval status exposes effective code freshness for management APIs.
  const auto code_status = engine.status();
  assert(code_status.code.enabled && code_status.code.available && !code_status.code.stale);
  assert(code_status.code.generation == snapshot->generation());
  assert(!snapshot->nodes().empty());
  assert(code_status.code.repository == snapshot->nodes().front().repository);
  assert(code_status.code.worktree == snapshot->nodes().front().worktree);
  RetrievalEngineOptions no_code_options;
  RetrievalEngine no_code_engine(database, caller, no_code_options);
  assert(!no_code_engine.status().code.enabled && !no_code_engine.status().code.available);
  RetrievalEngineOptions provenance_options = options;
  provenance_options.dense_enabled = false;
  RetrievalEngine provenance_engine(database, caller, provenance_options);
  const auto provenance_hits = provenance_engine.search_code(caller, "zyxwvnohit", 20, {},
      {{RetrievalOwnerKind::Memory, file_only_id, file_only_revision}});
  const auto file_only_hit = std::find_if(provenance_hits.begin(), provenance_hits.end(),
      [&](const auto& candidate) {
        return candidate.owner.id == symbol_ref.id && candidate.path.size() == 2;
      });
  assert(file_only_hit != provenance_hits.end());
  const auto& provenance_candidate = *file_only_hit;
  assert(provenance_candidate.path.size() >= 2);
  assert(provenance_candidate.path.front().from.id == file_only_id &&
         provenance_candidate.path.front().to.id == file_ref.id);
  assert(provenance_candidate.path.back().from.id == file_ref.id &&
         provenance_candidate.path.back().to.id == symbol_ref.id &&
         provenance_candidate.path.back().heuristic);
  assert(retrieval_relation_provenance(provenance_candidate.path.back()) ==
         std::string("heuristic code edge"));
  auto copied_hop = provenance_candidate.path.back();
  copied_hop.heuristic = false;
  assert(!engine.validate_relation(caller, copied_hop, RecallPolicy::Balanced, false));
  nlohmann::json serialized = retrieval_candidate_json(provenance_candidate);
  assert(serialized["path"].back()["heuristic"] == true);
  assert(serialized["path"].back()["provenance"] == "heuristic code edge");
  assert(std::any_of(hits.begin(), hits.end(), [&](const auto& hit) {
    return hit.owner.id == decision_id && hit.path.size() == 2;
  }));
  RecallService authority(database, database, database, database);
  ContextService context(database, nullptr, &database,
                         [&](const MemoryScope& scope, const RetrievalReference& ref) {
                           return engine.validate_code(scope, ref);
                         }, &database,
                         [&](const MemoryScope& scope, const std::string& id, std::int64_t revision,
                             RecallPolicy policy, bool global) {
                           const auto current_memory = database.find_for_recall(scope, id, policy, global);
                           return current_memory && (revision == 0 ||
                               database.retrieval_revision(current_memory->scope, id) == revision) &&
                               authority.eligible_for_context(scope, id, policy, global);
                         },
                         [&](const MemoryScope& scope, const RetrievalPathEdge& edge,
                             RecallPolicy policy, bool global) {
                           return engine.validate_relation(scope, edge, policy, global);
                         });
  ContextOptions context_options; context_options.token_budget = 512;
  const auto packed = context.build(caller, {}, context_options, hits);
  assert(!packed.code_ids.empty() && !packed.memory_ids.empty() &&
         packed.markdown.find("unique_code_symbol") != std::string::npos &&
         packed.markdown.find("memory linked to the unique code symbol") != std::string::npos);
  ContextOptions provenance_context; provenance_context.token_budget = 4096;
  const auto provenance_pack = context.build(caller, {}, provenance_context, provenance_hits);
  assert(provenance_pack.markdown.find("heuristic code edge") != std::string::npos);
  assert(serialized["path"].front()["provenance"] == "stored relation");
  ContextOptions tiny; tiny.token_budget = 2;
  const auto bounded = context.build(caller, {}, tiny, hits);
  assert(bounded.estimated_tokens <= tiny.token_budget);
  assert(bounded.memory_ids.empty());
  ContextOptions partial = context_options; partial.token_budget = 100;
  const auto no_partial_bundle = context.build(caller, {}, partial, hits);
  assert(no_partial_bundle.memory_ids.empty());
  assert(packed.markdown.find("[Mixed graph evidence bundle]") != std::string::npos);
  auto missing_relations = hits;
  for (auto& candidate : missing_relations)
    for (auto& edge : candidate.path) edge.evidence_id += ":missing";
  assert(context.build(caller, {}, context_options, missing_relations).memory_ids.empty());
  assert(packed.markdown.find(linked_memory_id + " -documents-> ") != std::string::npos);
  assert(std::find(packed.memory_ids.begin(), packed.memory_ids.end(), decision_id) != packed.memory_ids.end());
  RetrievalEngineOptions reverse_options = options;
  reverse_options.dense_enabled = false;
  RetrievalEngine reverse_engine(database, caller, reverse_options);
  const auto reverse_hits = reverse_engine.search_code(caller, "no code lexical hits", 10, {},
      {{RetrievalOwnerKind::Memory, linked_memory_id, linked_revision}});
  assert(std::any_of(reverse_hits.begin(), reverse_hits.end(), [](const auto& hit) {
    return hit.owner.kind == RetrievalOwnerKind::Code && !hit.path.empty();
  }));
  const auto reverse_pack = context.build(caller, {}, context_options, reverse_hits);
  assert(!reverse_pack.code_ids.empty() && !reverse_pack.memory_ids.empty());
  MemoryCoordinatorSettings coordinator_settings; coordinator_settings.enabled = true;
  coordinator_settings.automatic_recall = true; coordinator_settings.context_token_budget = 512;
  qt::ai::CompletionRequest request;
  qt::ai::Message user; user.role = qt::ai::MessageRole::user;
  user.content.push_back(qt::ai::TextContent{"find unique code", std::nullopt});
  request.messages.push_back(std::move(user));
  RecallService recall(database);
  MemoryCoordinator coordinator(recall, context, caller, coordinator_settings, nullptr, nullptr,
                                    [&](const MemoryScope&, const RecallRequest&,
                                    const std::vector<RecalledMemory>&) { return hits; });
  assert(!coordinator.last_augmentation(1).has_value());
  const auto augmentation = coordinator.augment(1, 1, request);
  assert(augmentation && !augmentation.value().contribution.code_ids.empty());
  assert(std::find(augmentation.value().contribution.memory_ids.begin(),
                   augmentation.value().contribution.memory_ids.end(), linked_memory_id) !=
         augmentation.value().contribution.memory_ids.end());
  assert(!coordinator.last_augmentation(2).has_value());
  const auto cached = coordinator.last_augmentation(1);
  assert(cached && cached->contribution.code_ids == augmentation.value().contribution.code_ids);
  auto cached_copy = *cached;
  cached_copy.contribution.code_ids.clear();
  assert(coordinator.last_augmentation(1)->contribution.code_ids ==
         augmentation.value().contribution.code_ids);
  coordinator.reset();
  assert(!coordinator.last_augmentation(1).has_value());
  MemoryQuery no_durable_code; no_durable_code.limit = 0;
  const auto canonical = database.query(caller, no_durable_code);
  assert(canonical.size() == 3);
  MemoryPatch changed_decision; changed_decision.content = "Changed after retrieval";
  database.update(caller, decision_id, changed_decision);
  const auto revalidated_pack = context.build(caller, {}, context_options, hits);
  assert(std::find(revalidated_pack.memory_ids.begin(), revalidated_pack.memory_ids.end(), decision_id) ==
         revalidated_pack.memory_ids.end());
  SourceRecord drifted_source;
  drifted_source.id = "mixed-source"; drifted_source.hash = "version-b";
  drifted_source.hash_status = "changed";
  SourceEvent drift_event; drift_event.action = "recheck";
  database.record_event(caller, drifted_source, drift_event);
  const auto drifted_pack = context.build(caller, {}, context_options, hits);
  assert(std::find(drifted_pack.memory_ids.begin(), drifted_pack.memory_ids.end(), linked_memory_id) ==
         drifted_pack.memory_ids.end());
  assert(std::all_of(drifted_pack.memory_ids.begin(), drifted_pack.memory_ids.end(),
                     [&](const auto& id) { return id == file_only_id; }));
  const auto drifted_hits = engine.search_code(caller, "unique query", 10);
  assert(std::none_of(drifted_hits.begin(), drifted_hits.end(), [&](const auto& hit) {
    return hit.owner.kind == RetrievalOwnerKind::Memory && hit.owner.id != file_only_id;
  }));
  std::ofstream(root / "code.cpp", std::ios::app) << "// changed\n";
  assert(context.build(caller, {}, context_options, hits).memory_ids.empty());
  assert(engine.search_code(caller, "unique query", 10).empty());
  std::filesystem::remove(root / "code.cpp");
  assert(engine.search_code(caller, "unique query", 10).empty());

  // A new snapshot generation with unchanged text keeps the durable owner/chunk identity.
  std::filesystem::create_directories(root);
  current = make_snapshot(root, 2, "int unique_code_symbol() { return 42; }\n");
  const auto before = database.retrieval_index_status(indexed_scope(caller, *snapshot));
  database.sync_code_snapshot(indexed_scope(caller, *current), *current);
  const auto after = database.retrieval_index_status(indexed_scope(caller, *current));
  assert(after.chunks == before.chunks);
  const auto vectors_after = database.search_dense(indexed, config.generation, {1.0F, 1.0F},
                                                   RecallPolicy::Strict, false, 10);
  assert(vectors_after.size() == vectors_before.size());
  for (std::size_t i = 0; i < vectors_before.size(); ++i)
    assert(vectors_after[i].chunk.id == vectors_before[i].chunk.id);
  const auto refreshed = database.resolve_code_hits(indexed, vectors_after);
  assert(!refreshed.empty());
  for (const auto& candidate : refreshed) {
    assert(candidate.owner.snapshot_generation == current->generation());
    assert(engine.validate_code(caller, candidate.owner));
  }
  assert(!engine.validate_code(caller, file_ref));

  // An edit outside the rendered source windows changes the file fingerprint
  // while permitting content-addressed reuse of the identical representations.
  std::string long_source = "int unique_code_symbol() { return 42; }\n";
  for (int line = 0; line < 80; ++line) long_source += "// unchanged source window\n";
  current = make_snapshot(root, 3, long_source + "// old tail\n");
  database.sync_code_snapshot(indexed, *current);
  while (indexer.step(indexed, {}, config.generation).worked) {}
  const auto prior_tail_hits = database.search_dense(indexed, config.generation,
      {1.0F, 1.0F}, RecallPolicy::Strict, false, 10);
  const auto prior_tail_candidates = database.resolve_code_hits(indexed, prior_tail_hits);
  assert(!prior_tail_candidates.empty());
  current = make_snapshot(root, 4, long_source + "// new tail\n");
  database.sync_code_snapshot(indexed, *current);
  const auto refreshed_tail_hits = database.search_dense(indexed, config.generation,
      {1.0F, 1.0F}, RecallPolicy::Strict, false, 10);
  assert(refreshed_tail_hits.size() == prior_tail_hits.size());
  for (std::size_t i = 0; i < prior_tail_hits.size(); ++i) {
    assert(refreshed_tail_hits[i].chunk.id == prior_tail_hits[i].chunk.id);
    assert(refreshed_tail_hits[i].chunk.content_hash == prior_tail_hits[i].chunk.content_hash);
  }
  const auto refreshed_tail_candidates = database.resolve_code_hits(indexed, refreshed_tail_hits);
  assert(refreshed_tail_candidates.size() == prior_tail_candidates.size());
  for (std::size_t i = 0; i < refreshed_tail_candidates.size(); ++i) {
    const auto& before_ref = prior_tail_candidates[i].owner;
    const auto& after_ref = refreshed_tail_candidates[i].owner;
    assert(after_ref.id == before_ref.id && after_ref.revision == before_ref.revision);
    assert(after_ref.source_fingerprint != before_ref.source_fingerprint);
    assert(after_ref.snapshot_generation == 4 && engine.validate_code(caller, after_ref));
    assert(!engine.validate_code(caller, before_ref));
  }
  assert(!indexer.step(indexed, {}, config.generation).worked);

  // Worktree scopes remain independent, and cancellation leaves the transaction unchanged.
  const auto other_indexed = indexed_scope(caller, *other_snapshot);
  database.sync_code_snapshot(other_indexed, *other_snapshot);
  assert(scope_key(other_indexed) != scope_key(indexed_scope(caller, *current)));
  const auto cancelled_before = database.retrieval_index_status(other_indexed);
  bool cancelled = false;
  try { database.sync_code_snapshot(other_indexed, *other_snapshot, [] { return true; }); }
  catch (const std::exception&) { cancelled = true; }
  assert(cancelled && database.retrieval_index_status(other_indexed).chunks == cancelled_before.chunks);
  probe_dense_memory_evidence(database, caller);
  std::filesystem::remove_all(root); std::filesystem::remove_all(other_root);
  std::cout << "memory code retrieval probe passed\n";
}
