#include "qt/memory/retrieval_explanation.hpp"
#include <limits>
#include "qt/memory/database.hpp"
#include "qt/memory/context_service.hpp"
#include "qt/memory/recall_service.hpp"
#include "qt/memory/retrieval_engine.hpp"
#include "qt/memory/scope.hpp"
#include "index/retrieval_snapshot.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

using namespace qt::memory;
using namespace qt::index;

static MemoryScope make_scope() {
  MemoryScope scope; scope.kind = ScopeKind::Repository; scope.user = "entity-code";
  scope.project = "probe"; scope.repository = "repo"; return scope;
}

static RetrievalReference code_ref(const RetrievalCodeNode& node) {
  return {RetrievalOwnerKind::Code, node.id, 0, node.repository, node.worktree,
          node.path.generic_string(), node.source_fingerprint, node.generation};
}

int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::invalid_argument("requires agent home");
    const auto root = std::filesystem::path(argv[1]) / "repo";
    std::filesystem::remove_all(root); std::filesystem::create_directories(root);
    std::ofstream(root / "code.cpp") << "int target_symbol() { return 42; }\n";
    ProjectScanResult scan; scan.root = root; scan.files.push_back({"code.cpp", ProjectFileKind::CxxSource});
    CodeIndex index; index.functions.push_back({"target_symbol", "target_symbol", "code.cpp", 1, "function"});
    const auto snapshot = RetrievalSnapshot::build(scan, index, 4);
    const auto code_node = std::find_if(snapshot->nodes().begin(), snapshot->nodes().end(),
        [](const auto& node) { return node.kind == RetrievalCodeNodeKind::Function; });
    assert(code_node != snapshot->nodes().end());
    const auto code = code_ref(*code_node);
    const auto scope = make_scope();
    SQLiteMemoryDatabase database(argv[1]);
    MemoryEntity alpha; alpha.kind = "concept"; alpha.name = "alpha";
    alpha = database.add_entity(scope, std::move(alpha));
    MemoryEntity beta; beta.kind = "concept"; beta.name = "beta";
    beta = database.add_entity(scope, std::move(beta));
    MemoryEntity gamma; gamma.kind = "concept"; gamma.name = "gamma";
    gamma = database.add_entity(scope, std::move(gamma));
    MemoryEntity delta; delta.kind = "concept"; delta.name = "delta";
    delta = database.add_entity(scope, std::move(delta));
    MemoryEntity epsilon; epsilon.kind = "concept"; epsilon.name = "epsilon";
    epsilon = database.add_entity(scope, std::move(epsilon));

    MemoryWrite positive_write; positive_write.status = MemoryStatus::Validated;
    positive_write.content = "The implementation uses alpha for target_symbol.";
    const auto positive_id = database.add(scope, positive_write);
    MemoryAssertion positive; positive.memory_id = positive_id; positive.revision = database.retrieval_revision(scope, positive_id);
    positive.subject_id = alpha.id; positive.object_id = beta.id; positive.predicate = "applies_to";
    positive.literal_json = "null"; positive.qualifiers_json = "{\"confidence\":\"high\"}";
    positive.evidence.push_back({"design-doc", "r1", "uri:design", "hash", "1", "probe"});
    database.add_assertion(scope, positive);

    MemoryWrite negative_write; negative_write.status = MemoryStatus::Validated;
    negative_write.content = "The implementation does not use gamma for target_symbol.";
    const auto negative_id = database.add(scope, negative_write);
    MemoryAssertion negative; negative.memory_id = negative_id; negative.revision = database.retrieval_revision(scope, negative_id);
    negative.subject_id = alpha.id; negative.object_id = gamma.id; negative.predicate = "applies_to";
    negative.positive = false; negative.literal_json = "null";
    negative.qualifiers_json = "{\"confidence\":\"high\"}";
    negative.evidence.push_back({"design-doc", "r2", "uri:design-negative", "hash", "2", "probe"});
    database.add_assertion(scope, negative);
    MemoryWrite gamma_write; gamma_write.status = MemoryStatus::Validated;
    gamma_write.content = "Gamma has a downstream implementation.";
    const auto gamma_owner_id = database.add(scope, gamma_write);
    MemoryAssertion gamma_assertion; gamma_assertion.memory_id = gamma_owner_id;
    gamma_assertion.revision = database.retrieval_revision(scope, gamma_owner_id);
    gamma_assertion.subject_id = gamma.id; gamma_assertion.object_id = delta.id;
    gamma_assertion.predicate = "applies_to"; gamma_assertion.literal_json = "null";
    gamma_assertion.qualifiers_json = "{\"confidence\":\"high\"}";
    gamma_assertion.evidence.push_back({"design-doc", "r3", "uri:design-gamma", "hash", "3", "probe"});
    database.add_assertion(scope, gamma_assertion);
    MemoryWrite beta_write; beta_write.status = MemoryStatus::Validated;
    beta_write.content = "Beta has a downstream implementation.";
    const auto beta_owner_id = database.add(scope, beta_write);
    MemoryAssertion beta_assertion; beta_assertion.memory_id = beta_owner_id;
    beta_assertion.revision = database.retrieval_revision(scope, beta_owner_id);
    beta_assertion.subject_id = beta.id; beta_assertion.object_id = epsilon.id;
    beta_assertion.predicate = "applies_to"; beta_assertion.literal_json = "null";
    beta_assertion.qualifiers_json = "{\"confidence\":\"high\"}";
    beta_assertion.evidence.push_back({"design-doc", "r4", "uri:design-beta", "hash", "4", "probe"});
    database.add_assertion(scope, beta_assertion);
    MemoryWrite literal_write; literal_write.status = MemoryStatus::Validated;
    literal_write.content = "The target symbol supports literal 42.";
    const auto literal_id = database.add(scope, literal_write);
    MemoryAssertion literal_assertion; literal_assertion.memory_id = literal_id;
    literal_assertion.revision = database.retrieval_revision(scope, literal_id);
    literal_assertion.subject_id = alpha.id; literal_assertion.predicate = "supports";
    literal_assertion.literal_json = "42"; literal_assertion.qualifiers_json = "{\"confidence\":\"high\"}";
    literal_assertion.evidence.push_back({"design-doc", "r5", "uri:design-literal", "hash", "5", "probe"});
    database.add_assertion(scope, literal_assertion);

    CrossStoreLink link; link.from = {RetrievalOwnerKind::Entity, alpha.id}; link.to = code;
    link.relation = "applies_to"; link.evidence_id = "uri:entity-code";
    database.add_cross_store_link(scope, link);
    CrossStoreLink terminal_link; terminal_link.from = {RetrievalOwnerKind::Entity, epsilon.id};
    terminal_link.to = code; terminal_link.relation = "applies_to";
    terminal_link.evidence_id = "uri:delta-code";
    database.add_cross_store_link(scope, terminal_link);

    RetrievalEngineOptions options; options.code_enabled = true; options.code_graph_enabled = true;
    options.graph_max_depth = 6; options.graph_max_edges = 64; options.graph_max_results = 64;
    options.code_snapshot = [snapshot] { return snapshot; };
    options.code_lexical = [](const std::string&, std::size_t) { return std::vector<RetrievalReference>{}; };
    RetrievalEngine engine(database, scope, options);
    const auto revision = database.retrieval_revision(scope, positive_id);
    const auto direct = engine.search_code(scope, "target", 20, {},
        {{RetrievalOwnerKind::Entity, alpha.id}}, {});
    assert(std::any_of(direct.begin(), direct.end(), [&](const auto& item) {
      return item.owner.kind == RetrievalOwnerKind::Code && !item.path.empty();
    }));
    auto exact_request_scope = scope;
    exact_request_scope.kind = ScopeKind::Exact;
    exact_request_scope.session = "preview-session";
    const std::vector<RetrievalReference> scoped_seeds{{RetrievalOwnerKind::Entity, alpha.id}};
    assert(!engine.search_code(exact_request_scope, "target", 20, {}, scoped_seeds, {},
                              std::make_pair(RecallPolicy::Balanced, false)).empty());
    assert(engine.search_code(exact_request_scope, "target", 20, {}, scoped_seeds, {},
                             std::make_pair(RecallPolicy::Strict, false)).empty());
    const auto direct_code = std::find_if(direct.begin(), direct.end(), [](const auto& item) {
      return item.owner.kind == RetrievalOwnerKind::Code && !item.path.empty();
    });
    assert(direct_code != direct.end() && std::any_of(direct_code->path.begin(), direct_code->path.end(),
        [](const auto& edge) { return edge.from.kind == RetrievalOwnerKind::Entity; }));

    const auto memory_path = engine.search_code(scope, "target", 20, {},
        {{RetrievalOwnerKind::Memory, positive_id, revision}}, {});
    const auto memory_code = std::find_if(memory_path.begin(), memory_path.end(), [](const auto& item) {
      return item.owner.kind == RetrievalOwnerKind::Code && !item.path.empty();
    });
    assert(memory_code != memory_path.end());
    assert(std::any_of(memory_code->path.begin(), memory_code->path.end(), [&](const auto& edge) {
      return edge.assertion && edge.assertion->memory_id == positive_id &&
             edge.assertion->positive && edge.assertion->qualifiers_json.find("high") != std::string::npos &&
             edge.assertion->evidence.size() == 1;
    }));
    RetrievalCandidate contextual_prefix;
    contextual_prefix.owner = {RetrievalOwnerKind::Entity, epsilon.id};
    contextual_prefix.eligible = true;
    const auto positive_assertion = database.assertion(scope, positive_id);
    const auto beta_assertion_snapshot = database.assertion(scope, beta_owner_id);
    assert(positive_assertion && beta_assertion_snapshot);
    contextual_prefix.path = {
      {{RetrievalOwnerKind::Entity, alpha.id}, {RetrievalOwnerKind::Entity, beta.id},
       "applies_to", "assertion:" + positive_id + ":" + std::to_string(revision),
       "resolved", positive_assertion},
      {{RetrievalOwnerKind::Entity, beta.id}, {RetrievalOwnerKind::Entity, epsilon.id},
       "applies_to", "assertion:" + beta_owner_id + ":" + std::to_string(beta_assertion_snapshot->revision),
       "resolved", beta_assertion_snapshot}};
    const auto contextual = engine.search_code(scope, "target", 20, {}, {}, {contextual_prefix});
    assert(std::any_of(contextual.begin(), contextual.end(), [](const auto& item) {
      return item.owner.kind == RetrievalOwnerKind::Code && item.path.size() >= 3;
    }));
    auto code_options = options;
    code_options.code_lexical = [code](const std::string&, std::size_t) { return std::vector<RetrievalReference>{code}; };
    RetrievalEngine code_engine(database, scope, code_options);
    const auto negative_path = code_engine.search_code(scope, "target", 20, {}, {}, {});
    assert(std::any_of(negative_path.begin(), negative_path.end(), [](const auto& item) {
      return item.owner.kind == RetrievalOwnerKind::Entity && item.path.size() >= 2 &&
             std::any_of(item.path.begin(), item.path.end(), [](const auto& edge) {
               return edge.assertion && !edge.assertion->positive;
             });
    }));
    assert(std::any_of(negative_path.begin(), negative_path.end(), [&](const auto& item) {
      return item.owner.kind == RetrievalOwnerKind::Entity && item.owner.id == gamma.id &&
             item.path.size() == 2;
    }));
    assert(std::none_of(negative_path.begin(), negative_path.end(), [&](const auto& item) {
      return item.owner.kind == RetrievalOwnerKind::Entity && item.owner.id == delta.id;
    }));
    const auto literal_candidate = std::find_if(negative_path.begin(), negative_path.end(), [&](const auto& item) {
      return item.owner.kind == RetrievalOwnerKind::Memory && item.owner.id == literal_id;
    });
    assert(literal_candidate != negative_path.end() && std::any_of(literal_candidate->path.begin(),
        literal_candidate->path.end(), [](const auto& edge) {
          return edge.assertion && edge.assertion->literal_json == "42";
        }));

    RecallService authority(database, database, database, database, database);
    ContextService context(database, nullptr, &database,
        [&](const MemoryScope& s, const RetrievalReference& ref) { return engine.validate_code(s, ref); },
        &database,
        [&](const MemoryScope& s, const std::string& id, std::int64_t rev, RecallPolicy policy, bool global) {
          const auto memory = database.find_for_recall(s, id, policy, global);
          return memory && (rev == 0 || database.retrieval_revision(memory->scope, id) == rev) &&
                 authority.eligible_for_context(s, id, policy, global);
        },
        [&](const MemoryScope& s, const RetrievalPathEdge& edge, RecallPolicy policy, bool global) {
          return engine.validate_relation(s, edge, policy, global);
        });
    ContextOptions context_options; context_options.token_budget = 1200;
    const auto direct_pack = context.build(scope, {}, context_options, direct);
    assert(direct_pack.memory_ids.empty() && !direct_pack.code_ids.empty() &&
           direct_pack.markdown.find("Entity metadata") != std::string::npos);
    auto claim_options = context_options; claim_options.token_budget = 10000;
    const auto claims_pack = context.build(scope, {}, claim_options, negative_path);
    assert(claims_pack.markdown.find("\"polarity\":\"negative\"") != std::string::npos &&
           claims_pack.markdown.find("\"literal\":42") != std::string::npos);
    const auto packed = context.build(scope, {}, context_options, contextual);
    assert(packed.markdown.find("Structured claim") != std::string::npos &&
           packed.markdown.find("high") != std::string::npos &&
           packed.markdown.find("target_symbol") != std::string::npos &&
           packed.markdown.find(positive_id) != std::string::npos &&
           packed.markdown.find(beta_owner_id) != std::string::npos &&
           packed.markdown.find("uri:design-beta") != std::string::npos);
    assert(std::find(packed.memory_ids.begin(), packed.memory_ids.end(), positive_id) != packed.memory_ids.end() &&
           std::find(packed.memory_ids.begin(), packed.memory_ids.end(), beta_owner_id) != packed.memory_ids.end() &&
           !packed.code_ids.empty());
    // Multiple complete paths may fit the larger pack. Measure one complete
    // three-edge bundle so the boundary cannot accidentally admit another one.
    const auto boundary_candidate = std::find_if(contextual.begin(), contextual.end(), [](const auto& item) {
      return item.owner.kind == RetrievalOwnerKind::Code && item.path.size() == 3;
    });
    assert(boundary_candidate != contextual.end());
    const std::vector<RetrievalCandidate> boundary_candidates{*boundary_candidate};
    const auto boundary_pack = context.build(scope, {}, context_options, boundary_candidates);
    assert(boundary_pack.memory_ids.size() == 2 && boundary_pack.code_ids.size() == 1);
    auto large_divisor = context_options;
    large_divisor.token_budget = 1;
    large_divisor.bytes_per_token = std::numeric_limits<std::size_t>::max();
    const auto large_divisor_pack = context.build(scope, {}, large_divisor, boundary_candidates);
    assert(large_divisor_pack.estimated_tokens == 1);
    assert(context_preview_json(large_divisor_pack, 1, large_divisor.bytes_per_token)
               ["estimated_full_context"]["tokens"] == 1);
    const auto explained_code = retrieval_candidate_json(*boundary_candidate, &boundary_pack);
    assert(explained_code["selection"]["selected"] == true &&
           explained_code["selection"]["injected"] == false && explained_code["selection"]["used"].is_null());
    assert(explained_code["path"].size() == 3 &&
           explained_code["path"][0]["assertion"]["memory_id"] == positive_id &&
           explained_code["path"][1]["assertion"]["memory_id"] == beta_owner_id &&
           explained_code["path"][0]["assertion"]["evidence"][0]["source_revision"] == "r1");
    const auto explained_context = context_preview_json(boundary_pack, context_options.token_budget);
    assert(explained_context["estimated_full_context"]["bytes"] == boundary_pack.markdown.size() &&
           explained_context["token_budget"] == context_options.token_budget);
    ContextOptions tiny = context_options; tiny.token_budget = boundary_pack.estimated_tokens - 1;
    const auto tiny_pack = context.build(scope, {}, tiny, boundary_candidates);
    assert(tiny_pack.memory_ids.empty() && tiny_pack.markdown.find("target_symbol") == std::string::npos);
    const auto budget_explanation = context_preview_json(tiny_pack, tiny.token_budget);
    assert(std::any_of(tiny_pack.item_explanations.begin(), tiny_pack.item_explanations.end(),
        [&](const auto& item) {
          return item.graph_bundle && !item.selected && item.eligible && item.rendered_bytes &&
              item.reason == "context_budget_exceeded" && item.memory_ids.size() == 2 && item.code_ids.size() == 1;
        }));
    assert(budget_explanation["selected_item_bytes"] == 0);
    const auto omitted_candidate = retrieval_candidate_json(*boundary_candidate, &tiny_pack);
    assert(omitted_candidate["selection"]["selected"] == false &&
           !omitted_candidate["selection"]["item_indices"].empty() &&
           omitted_candidate["selection"]["item_omission_reasons"][0] == "context_budget_exceeded");
    tiny.token_budget = boundary_pack.estimated_tokens;
    const auto exact_pack = context.build(scope, {}, tiny, boundary_candidates);
    assert(exact_pack.memory_ids == boundary_pack.memory_ids && exact_pack.code_ids == boundary_pack.code_ids);
    const auto exact_explanation = context_preview_json(exact_pack, tiny.token_budget);
    assert(exact_explanation["selected_item_bytes"].get<std::size_t>() +
           exact_explanation["shared_overhead_bytes"].get<std::size_t>() == exact_pack.markdown.size());

    auto forged = contextual;
    for (auto& item : forged) for (auto& edge : item.path) if (edge.assertion) {
      edge.assertion->revision += 1; edge.evidence_id += ":forged";
    }
    const auto forged_pack = context.build(scope, {}, context_options, forged);
    assert(forged_pack.memory_ids.empty() && forged_pack.code_ids.empty());
    assert(std::any_of(forged_pack.item_explanations.begin(), forged_pack.item_explanations.end(),
        [](const auto& item) {
          return !item.eligible && !item.selected && !item.rendered_bytes &&
              item.reason == "evidence_validation_failed" && !item.code_ids.empty();
        }));
    auto recovered_candidates = forged;
    recovered_candidates.insert(recovered_candidates.end(), contextual.begin(), contextual.end());
    const auto recovered_pack = context.build(scope, {}, context_options, recovered_candidates);
    assert(recovered_pack.memory_ids == packed.memory_ids && recovered_pack.code_ids == packed.code_ids);
    SourceRecord source; source.id = "design-doc"; source.hash = "changed"; source.hash_status = "changed";
    SourceEvent event; event.action = "recheck";
    database.record_event(scope, source, event);
    const auto stale_pack = context.build(scope, {}, context_options, contextual);
    assert(stale_pack.memory_ids.empty() && stale_pack.code_ids.empty());
    assert(engine.search_code(scope, "target", 20, {},
        {{RetrievalOwnerKind::Memory, positive_id, revision}}, {}).empty());
    source.hash = "hash"; source.hash_status = "current";
    database.record_event(scope, source, event);
    MemoryPatch changed; changed.content = "The original bridge has changed.";
    database.update(scope, positive_id, changed);
    assert(context.build(scope, {}, context_options, contextual).memory_ids.empty());

    auto foreign_scope = scope; foreign_scope.repository = "foreign";
    MemoryEntity foreign_entity; foreign_entity.kind = "concept"; foreign_entity.name = "foreign";
    foreign_entity = database.add_entity(foreign_scope, std::move(foreign_entity));
    assert(engine.search_code(scope, "target", 20, {},
        {{RetrievalOwnerKind::Entity, foreign_entity.id}}, {}).empty());

    std::ofstream(root / "code.cpp", std::ios::app) << "// drift\n";
    assert(engine.search_code(scope, "target", 20, {}, {{RetrievalOwnerKind::Entity, alpha.id}}, {}).empty());
    std::filesystem::remove_all(root);
    std::cout << "memory entity-code probe passed\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
