#include "qt/memory/post_processing_service.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace qt::memory {
namespace {

std::unordered_set<std::string> words(const std::string& content) {
  std::unordered_set<std::string> result;
  std::string word;
  for (unsigned char ch : content) {
    if (std::isalnum(ch) != 0) {
      word.push_back(static_cast<char>(std::tolower(ch)));
    } else {
      if (word.size() > 2) result.insert(std::move(word));
      word.clear();
    }
  }
  if (word.size() > 2) result.insert(std::move(word));
  return result;
}

double jaccard(const std::string& lhs, const std::string& rhs) {
  const auto left = words(lhs);
  const auto right = words(rhs);
  if (left.empty() || right.empty()) return 0.0;
  std::size_t intersection = 0;
  for (const auto& word : left) if (right.count(word) != 0) ++intersection;
  return static_cast<double>(intersection) /
         static_cast<double>(left.size() + right.size() - intersection);
}

std::string tag_component(std::string value) {
  for (char& ch : value) {
    const auto byte = static_cast<unsigned char>(ch);
    ch = std::isalnum(byte) != 0
             ? static_cast<char>(std::tolower(byte))
             : '-';
  }
  value.erase(std::unique(value.begin(), value.end(),
                          [](const char lhs, const char rhs) {
                            return lhs == '-' && rhs == '-';
                          }),
              value.end());
  while (!value.empty() && value.front() == '-') value.erase(value.begin());
  while (!value.empty() && value.back() == '-') value.pop_back();
  return value.empty() ? "hypothesis" : value;
}

}  // namespace

PostProcessingReport PostProcessingService::process(const MemoryScope& scope,
                                                    std::size_t limit,
                                                    std::size_t maximum_pairs) {
  MemoryQuery query;
  query.limit = limit;
  query.policy = RecallPolicy::Strict;
  query.include_superseded = false;
  query.statuses = {MemoryStatus::Candidate, MemoryStatus::Hypothesis,
                    MemoryStatus::Active, MemoryStatus::Validated};
  const auto memories = memories_.query(scope, query);
  PostProcessingReport report;
  std::size_t examined = 0;
  for (std::size_t i = 0; i < memories.size(); ++i) {
    for (std::size_t j = i + 1; j < memories.size() && examined < maximum_pairs; ++j, ++examined) {
      const double similarity = jaccard(memories[i].content, memories[j].content);
      if (similarity < 0.34) continue;
      AssociationCandidate association;
      association.from_id = memories[i].id;
      association.to_id = memories[j].id;
      association.similarity = similarity;
      association.relation = similarity >= 0.72 ? "duplicates" : "related";
      report.associations.push_back(association);
      GraphEdge edge;
      edge.from_id = association.from_id;
      edge.to_id = association.to_id;
      edge.type = association.relation;
      edge.confidence = similarity;
      edge.source = "postprocess:deterministic";
      graph_.add_edge(scope, std::move(edge));
      if (similarity >= 0.72) {
        CompressionCandidate compression;
        compression.source_ids = {memories[i].id, memories[j].id};
        compression.content = memories[i].content.size() <= memories[j].content.size()
                                  ? memories[i].content : memories[j].content;
        compression.confidence = similarity;
        report.compressions.push_back(compression);
        ReviewItemWrite review;
        review.kind = "compression_candidate";
        review.reason = "deterministic duplicate similarity exceeded threshold";
        review.subject_id = memories[i].id;
        review.candidate_json = nlohmann::json{
            {"source_ids", compression.source_ids},
            {"content", compression.content},
            {"confidence", compression.confidence}}.dump();
        review.tags = {"postprocess:compression", "review:required"};
        report.review_ids.push_back(reviews_.add_review(scope, std::move(review)));
      }
    }
    if (examined >= maximum_pairs) break;
  }
  return report;
}

std::string PostProcessingService::remember_dream_artifact(
    const MemoryScope& scope, std::string content,
    std::vector<std::string> source_ids, double confidence) {
  DreamArtifactRequest request;
  request.content = std::move(content);
  request.source_memory_ids = std::move(source_ids);
  request.confidence = confidence;
  return remember_dream_artifact(scope, std::move(request)).id;
}

DreamArtifactResult PostProcessingService::remember_dream_artifact(
    const MemoryScope& scope, DreamArtifactRequest request) {
  if (!std::isfinite(request.confidence)) {
    throw std::invalid_argument("dream artifact confidence must be finite");
  }
  if (request.source_memory_ids.size() < 2U) {
    throw std::invalid_argument(
        "dream artifact requires at least two source memories");
  }
  std::sort(request.source_memory_ids.begin(),
            request.source_memory_ids.end());
  request.source_memory_ids.erase(
      std::unique(request.source_memory_ids.begin(),
                  request.source_memory_ids.end()),
      request.source_memory_ids.end());
  if (request.source_memory_ids.size() < 2U) {
    throw std::invalid_argument(
        "dream artifact requires two distinct source memories");
  }
  for (const auto& source_id : request.source_memory_ids) {
    (void)memories_.get(scope, source_id);
  }
  MemoryWrite write;
  write.type = MemoryType::Dream;
  write.status = MemoryStatus::DreamArtifact;
  write.source = "postprocess:dream-cycle";
  write.provenance.source_type = SourceType::Dreaming;
  write.provenance.source_uri = "memory://dreaming/" + request.cycle_id;
  write.provenance.source_hash = request.source_pack_hash;
  write.provenance.created_by = "memory_dreaming_model";
  write.provenance.evidence_strength = 0.2;
  write.provenance.source_span =
      nlohmann::json(request.source_memory_ids).dump();
  write.content = std::move(request.content);
  write.confidence = std::max(0.0, std::min(0.49, request.confidence));
  write.tags = {"dream:artifact", "review:dream", "review:required",
                "trusted:false",
                "dream:kind:" + tag_component(request.artifact_kind)};
  if (!request.cycle_id.empty()) {
    write.tags.push_back("dream:cycle:" + request.cycle_id);
  }
  for (const auto& tag : request.tags) {
    if (!tag.empty()) write.tags.push_back(tag);
  }
  const auto id = memories_.remember(scope, std::move(write));
  ReviewItemWrite review;
  review.kind = "dream_artifact";
  review.reason = "dream output is isolated from trusted recall";
  review.subject_id = id;
  review.provenance.source_type = SourceType::Dreaming;
  review.provenance.source_uri = "memory://dreaming/" + request.cycle_id;
  review.provenance.source_hash = request.source_pack_hash;
  review.provenance.created_by = "memory_dreaming_parent";
  review.provenance.evidence_strength = 0.2;
  review.candidate_json =
      nlohmann::json{{"memory_id", id},
                     {"source_ids", request.source_memory_ids},
                     {"artifact_kind", request.artifact_kind},
                     {"model", request.model},
                     {"cycle_id", request.cycle_id},
                     {"source_pack_hash", request.source_pack_hash}}
          .dump();
  review.tags = {"review:dream", "review:required", "model:untrusted"};
  const auto review_id = reviews_.add_review(scope, std::move(review));
  for (const auto& source_id : request.source_memory_ids) {
    GraphEdge edge;
    edge.from_id = id;
    edge.to_id = source_id;
    edge.type = "derived_from";
    edge.confidence = 0.55;
    edge.source = "postprocess:dream-cycle";
    graph_.add_edge(scope, std::move(edge));
  }
  return DreamArtifactResult{id, review_id, std::nullopt, std::nullopt};
}

DreamArtifactResult PostProcessingService::remember_researched_successor(
    const MemoryScope& scope, const std::string& dream_id,
    DreamResearchEvidence evidence) {
  const auto dream = memories_.get(scope, dream_id);
  if (dream.type != MemoryType::Dream ||
      dream.status != MemoryStatus::DreamArtifact) {
    throw std::invalid_argument(
        "researched successor requires a pending dream artifact");
  }
  if (evidence.summary.empty()) {
    throw std::invalid_argument("researched successor requires a summary");
  }
  if (!std::isfinite(evidence.confidence) ||
      !std::isfinite(evidence.provenance.evidence_strength)) {
    throw std::invalid_argument(
        "researched successor confidence values must be finite");
  }
  if (evidence.source_memory_ids.empty() &&
      evidence.provenance.source_uri.empty() &&
      evidence.provenance.source_hash.empty()) {
    throw std::invalid_argument(
        "researched successor requires supplied evidence provenance");
  }
  std::sort(evidence.source_memory_ids.begin(),
            evidence.source_memory_ids.end());
  evidence.source_memory_ids.erase(
      std::unique(evidence.source_memory_ids.begin(),
                  evidence.source_memory_ids.end()),
      evidence.source_memory_ids.end());
  for (const auto& source_id : evidence.source_memory_ids) {
    if (source_id == dream_id) {
      throw std::invalid_argument("dream cannot be its own research source");
    }
    (void)memories_.get(scope, source_id);
  }

  MemoryWrite write;
  write.type = MemoryType::Research;
  write.status = MemoryStatus::Candidate;
  write.source = "postprocess:dream-research";
  write.provenance = std::move(evidence.provenance);
  if (write.provenance.source_type == SourceType::Unknown ||
      write.provenance.source_type == SourceType::Dreaming) {
    write.provenance.source_type = SourceType::ProcessedText;
  }
  if (write.provenance.source_uri.empty()) {
    write.provenance.source_uri = "memory://dream-research/" + dream_id;
  }
  if (write.provenance.created_by.empty() ||
      write.provenance.created_by == "agent") {
    write.provenance.created_by = "supplied_research_evidence";
  }
  write.provenance.evidence_strength = std::max(
      0.5, std::min(1.0, write.provenance.evidence_strength));
  nlohmann::json source_span{
      {"source_memory_ids", evidence.source_memory_ids}};
  if (!write.provenance.source_span.empty()) {
    source_span["supplied_span"] = write.provenance.source_span;
  }
  write.provenance.source_span = source_span.dump();
  write.content = std::move(evidence.summary);
  write.confidence = std::max(0.0, std::min(0.9, evidence.confidence));
  write.tags = {"research", "research:supplied",
                "research:pending-validation", "dream:researched",
                "dream-source:" + dream_id, "review:required"};
  for (const auto& source_id : evidence.source_memory_ids) {
    write.tags.push_back("research-source:" + source_id);
  }
  for (const auto& tag : evidence.tags) {
    if (!tag.empty()) write.tags.push_back(tag);
  }
  const auto research_provenance = write.provenance;
  const auto research_id = memories_.remember(scope, std::move(write));

  ReviewItemWrite review;
  review.kind = "dream_research_successor";
  review.reason =
      "supplied research evidence requires review before lifecycle promotion";
  review.subject_id = research_id;
  review.provenance = research_provenance;
  review.provenance.created_by = "memory_dreaming_parent";
  review.candidate_json =
      nlohmann::json{{"memory_id", research_id},
                     {"dream_id", dream_id},
                     {"source_ids", evidence.source_memory_ids}}
          .dump();
  review.tags = {"review:dream-research", "review:required"};
  const auto research_review_id =
      reviews_.add_review(scope, std::move(review));

  for (const auto& relation : {"supports", "follows"}) {
    GraphEdge edge;
    edge.from_id = research_id;
    edge.to_id = dream_id;
    edge.type = relation;
    edge.confidence = 0.7;
    edge.source = "postprocess:dream-research";
    graph_.add_edge(scope, std::move(edge));
  }
  for (const auto& source_id : evidence.source_memory_ids) {
    GraphEdge edge;
    edge.from_id = research_id;
    edge.to_id = source_id;
    edge.type = "derived_from";
    edge.confidence = 0.8;
    edge.source = "postprocess:dream-research";
    graph_.add_edge(scope, std::move(edge));
  }
  DreamArtifactResult result;
  result.id = dream_id;
  result.researched_memory_id = research_id;
  result.research_review_id = research_review_id;
  return result;
}

}  // namespace qt::memory
