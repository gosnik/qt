#include "qt/memory/retrieval_explanation.hpp"

#include <algorithm>
#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

namespace qt::memory {
namespace {

using Json = nlohmann::json;

std::int64_t timestamp(const TimePoint value) {
  if (value == TimePoint{}) return 0;
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             value.time_since_epoch())
      .count();
}

const char* owner_kind(const RetrievalOwnerKind kind) {
  switch (kind) {
    case RetrievalOwnerKind::Memory: return "memory";
    case RetrievalOwnerKind::Code: return "code";
    case RetrievalOwnerKind::Entity: return "entity";
  }
  return "unknown";
}

Json reference_json(const RetrievalReference& reference) {
  return Json{{"kind", owner_kind(reference.kind)},
              {"id", reference.id},
              {"revision", reference.revision},
              {"repository", reference.repository},
              {"worktree", reference.worktree},
              {"path", reference.path},
              {"source_fingerprint", reference.source_fingerprint},
              {"snapshot_generation", reference.snapshot_generation}};
}

Json chunk_json(const RetrievalChunk& chunk) {
  return Json{{"id", chunk.id}, {"owner_key", chunk.owner_key},
              {"owner_revision", chunk.owner_revision}, {"ordinal", chunk.ordinal},
              {"text", chunk.text}, {"content_hash", chunk.content_hash},
              {"source_uri", chunk.source_uri}, {"source_span", chunk.source_span},
              {"render_version", chunk.render_version}};
}

Json assertion_json(const MemoryAssertion& assertion) {
  Json evidence = Json::array();
  for (const auto& item : assertion.evidence) {
    evidence.push_back(Json{{"source_id", item.source_id},
                            {"source_revision", item.source_revision},
                            {"source_uri", item.source_uri},
                            {"source_hash", item.source_hash},
                            {"source_span", item.source_span},
                            {"created_by", item.created_by}});
  }
  return Json{{"memory_id", assertion.memory_id},
              {"revision", assertion.revision},
              {"subject_id", assertion.subject_id},
              {"predicate", assertion.predicate},
              {"object_id", assertion.object_id},
              {"literal_json", assertion.literal_json},
              {"qualifiers_json", assertion.qualifiers_json},
              {"polarity", assertion.positive ? "positive" : "negative"},
              {"positive", assertion.positive},
              {"valid_from", timestamp(assertion.valid_from.value_or(TimePoint{}))},
              {"valid_to", timestamp(assertion.valid_to.value_or(TimePoint{}))},
              {"recorded_at", timestamp(assertion.recorded_at)},
              {"evidence", std::move(evidence)}};
}

Json selection_json(const ContextPack& pack, const RetrievalReference& owner,
                    const bool eligible) {
  std::optional<bool> selected;
  if (owner.kind == RetrievalOwnerKind::Memory) {
    selected = std::find(pack.memory_ids.begin(), pack.memory_ids.end(), owner.id) !=
               pack.memory_ids.end();
  } else if (owner.kind == RetrievalOwnerKind::Code) {
    selected = std::find(pack.code_ids.begin(), pack.code_ids.end(), owner.id) !=
               pack.code_ids.end();
  }
  Json indices = Json::array();
  Json reasons = Json::array();
  bool has_validation = false;
  bool any_eligible = false;
  for (std::size_t index = 0; index < pack.item_explanations.size(); ++index) {
    const auto& item = pack.item_explanations[index];
    const auto& ids = owner.kind == RetrievalOwnerKind::Code ? item.code_ids : item.memory_ids;
    if (owner.kind == RetrievalOwnerKind::Entity ||
        std::find(ids.begin(), ids.end(), owner.id) == ids.end()) continue;
    indices.push_back(index);
    has_validation = true;
    any_eligible = any_eligible || item.eligible;
    if (!item.selected && std::find(reasons.begin(), reasons.end(), Json(item.reason)) == reasons.end())
      reasons.push_back(item.reason);
  }
  const bool final_eligible = eligible && (!has_validation || any_eligible);
  const auto reason = !final_eligible ? "ineligible" :
      (!selected ? "entity metadata selection is bundle-owned" :
       (*selected ? "selected" : "not selected; see item reasons"));
  return Json{{"preview", true},
              {"retrieved", true},
              {"eligible", final_eligible},
              {"selected", selected ? Json(*selected) : Json(nullptr)},
              {"injected", false},
              {"used", nullptr},
              {"reason", reason},
              {"item_indices", std::move(indices)},
              {"item_omission_reasons", std::move(reasons)}};
}

}  // namespace

nlohmann::json recalled_memory_json(const RecalledMemory& item,
                                    const std::optional<std::int64_t> current_revision,
                                    const ContextPack* preview) {
  const auto estimated_tokens = (item.memory.content.size() + 3U) / 4U;
  Json graph_path = Json::array();
  for (std::size_t index = 0; index + 1U < item.graph_path_ids.size(); ++index) {
    const bool reverse = index < item.graph_path_reverse.size() && item.graph_path_reverse[index];
    const auto& from = item.graph_path_ids[index + (reverse ? 1U : 0U)];
    const auto& to = item.graph_path_ids[index + (reverse ? 0U : 1U)];
    graph_path.push_back(Json{
        {"from", Json{{"kind", "memory"}, {"id", from}}},
        {"to", Json{{"kind", "memory"}, {"id", to}}},
        {"relation", index < item.graph_path_relations.size() ? item.graph_path_relations[index] : ""},
        {"reverse", reverse}});
    graph_path.back()["provenance"] = "stored relation";
  }
  Json assertion_path = Json::array();
  for (const auto& step : item.assertion_path) {
    assertion_path.push_back(Json{{"memory_id", step.memory_id},
                                  {"revision", step.revision},
                                  {"from_entity_id", step.from_entity_id},
                                  {"to_entity_id", step.to_entity_id},
                                  {"provenance", "stored assertion"},
                                  {"reverse", step.reverse}});
  }
  Json result{{"id", item.memory.id},
              {"type", to_string(item.memory.type)},
              {"status", to_string(item.memory.status)},
              {"content", item.memory.content},
              {"score", item.score.total},
              {"source", Json{{"label", item.memory.source},
                               {"type", to_string(item.memory.provenance.source_type)},
                               {"uri", item.memory.provenance.source_uri},
                               {"hash", item.memory.provenance.source_hash},
                               {"span", item.memory.provenance.source_span},
                               {"created_by", item.memory.provenance.created_by}}},
              {"revision", Json{{"historical", item.historical},
                                      {"basis", item.historical
                                          ? "historical_capture"
                                          : (item.dense_match ? "dense_capture"
                                                              : "current_canonical_at_explanation")},
                                      {"number", item.historical
                                                   ? Json(item.historical_revision)
                                                   : (item.dense_match ? Json(item.dense_match->chunk.owner_revision)
                                                      : (current_revision ? Json(*current_revision)
                                                                          : Json(nullptr)))},
                                  {"recorded_at", timestamp(item.historical_recorded_at.value_or(TimePoint{}))},
                                  {"as_of", timestamp(item.historical_as_of.value_or(TimePoint{}))}}},
              {"token_cost", Json{{"content_bytes", item.memory.content.size()},
                                   {"estimated_input_tokens", estimated_tokens},
                                   {"exact", false},
                                   {"method", "utf8_bytes_div_4"},
                                   {"scope", "memory_prose_only"}}},
              {"score_breakdown", Json{{"lexical", item.score.lexical},
                                        {"hashed_similarity", item.score.hashed_similarity},
                                        {"confidence", item.score.confidence},
                                        {"quality", item.score.quality},
                                        {"freshness", item.score.freshness},
                                        {"scope", item.score.scope},
                                        {"lexical_rank", item.score.lexical_rank},
                                        {"graph_rank", item.score.graph_rank},
                                        {"dense_rank", item.score.dense_rank},
                                        {"dense_similarity", item.dense_match
                                            ? Json(item.dense_match->similarity) : Json(nullptr)},
                                        {"reciprocal_rank_fusion", item.score.reciprocal_rank_fusion}}},
              {"graph_path_ids", item.graph_path_ids},
              {"graph_path_relations", item.graph_path_relations},
              {"graph_path_reverse", item.graph_path_reverse},
              {"graph_path", std::move(graph_path)},
              {"assertion_path", std::move(assertion_path)},
              {"connecting_memory", item.connecting_memory},
              {"eligible_for_context", item.eligible_for_context},
              {"historical", item.historical},
              {"historical_revision", item.historical_revision},
              {"warnings", item.warnings}};
  result["dense_match"] = nullptr;
  if (item.dense_match) {
    const auto& match = *item.dense_match;
    result["dense_match"] = Json{{"retriever", "dense"}, {"rank", item.score.dense_rank},
        {"similarity", match.similarity}, {"index_generation", match.generation},
        {"chunk", chunk_json(match.chunk)},
        {"revision_is_current", current_revision
            ? Json(*current_revision == match.chunk.owner_revision) : Json(nullptr)}};
  }
  if (preview != nullptr)
    result["selection"] = selection_json(*preview, RetrievalReference{
        RetrievalOwnerKind::Memory, item.memory.id}, item.eligible_for_context);
  return result;
}

nlohmann::json retrieval_candidate_json(const RetrievalCandidate& candidate,
                                        const ContextPack* preview) {
  Json chunks = Json::array();
  for (const auto& chunk : candidate.chunks) {
    chunks.push_back(chunk_json(chunk));
  }
  Json matched_chunks = Json::array();
  for (const auto& hit : candidate.matched_chunks) {
    matched_chunks.push_back(Json{{"retriever", "dense"},
                                  {"similarity", hit.similarity},
                                  {"index_generation", hit.generation},
                                  {"chunk", chunk_json(hit.chunk)}});
  }
  Json ranks = Json::array();
  for (const auto& rank : candidate.ranks)
    ranks.push_back(Json{{"retriever", rank.retriever}, {"rank", rank.rank}, {"score", rank.score}});
  Json path = Json::array();
  for (const auto& edge : candidate.path) {
    Json item{{"from", reference_json(edge.from)},
              {"to", reference_json(edge.to)},
              {"relation", edge.relation},
              {"evidence_id", edge.evidence_id},
              {"resolution", edge.resolution},
              {"provenance", retrieval_relation_provenance(edge)},
              {"heuristic", edge.heuristic}};
    if (edge.assertion) item["assertion"] = assertion_json(*edge.assertion);
    path.push_back(std::move(item));
  }
  Json result{{"owner", reference_json(candidate.owner)},
              {"chunks", std::move(chunks)},
              {"matched_chunks", std::move(matched_chunks)},
              {"ranks", std::move(ranks)},
              {"path", std::move(path)},
              {"evidence_ids", candidate.evidence_ids},
              {"eligible", candidate.eligible},
              {"exclusion_reasons", candidate.exclusion_reasons},
              {"fused_score", candidate.fused_score}};
  if (preview != nullptr)
    result["selection"] = selection_json(*preview, candidate.owner, candidate.eligible);
  return result;
}

nlohmann::json context_preview_json(const ContextPack& pack,
                                    const std::size_t token_budget,
                                    const std::size_t bytes_per_token) {
  const auto bounded_bytes_per_token = std::max<std::size_t>(1U, bytes_per_token);
  const auto bytes = pack.markdown.size();
  const auto tokens = bytes / bounded_bytes_per_token +
                      (bytes % bounded_bytes_per_token != 0U);
  Json items = Json::array();
  std::size_t selected_item_bytes = 0U;
  for (const auto& item : pack.item_explanations) {
    if (item.selected && item.rendered_bytes) selected_item_bytes += *item.rendered_bytes;
    items.push_back(Json{{"category", item.category},
                         {"memory_ids", item.memory_ids}, {"code_ids", item.code_ids},
                         {"block_ids", item.block_ids}, {"graph_bundle", item.graph_bundle},
                         {"selected", item.selected}, {"eligible", item.eligible},
                         {"reason", item.reason}, {"warnings", item.warnings},
                         {"rendered_bytes", item.rendered_bytes ? Json(*item.rendered_bytes) : Json(nullptr)},
                         {"estimated_tokens", item.rendered_bytes
                             ? Json(*item.rendered_bytes / bounded_bytes_per_token +
                                    (*item.rendered_bytes % bounded_bytes_per_token != 0U))
                             : Json(nullptr)},
                         {"cost_scope", "item_without_shared_envelope_or_heading"},
                         {"exact_tokens", false}});
  }
  return Json{{"items", std::move(items)},
              {"selected_item_bytes", selected_item_bytes},
              {"shared_overhead_bytes", bytes >= selected_item_bytes ? bytes - selected_item_bytes : 0U},
              {"selected", Json{{"memory_ids", pack.memory_ids},
                                 {"required_memory_ids", pack.required_memory_ids},
                                 {"code_ids", pack.code_ids},
                                 {"block_ids", pack.block_ids}}},
              {"injected", false},
              {"used", nullptr},
              {"omitted", pack.omitted},
              {"omission_reasons", pack.warnings},
              {"token_budget", token_budget},
              {"estimated_full_context", Json{{"bytes", bytes},
                                                {"tokens", tokens},
                                                {"exact", false},
                                                {"method", "rendered_utf8_bytes_div_bytes_per_token"}}}};
}

}  // namespace qt::memory
