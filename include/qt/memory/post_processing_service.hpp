#pragma once

#include "qt/memory/ingestion_service.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace qt::memory {

struct AssociationCandidate {
  std::string from_id;
  std::string to_id;
  std::string relation = "related";
  double similarity = 0.0;
};

struct CompressionCandidate {
  std::vector<std::string> source_ids;
  std::string content;
  double confidence = 0.0;
};

struct PostProcessingReport {
  std::vector<AssociationCandidate> associations;
  std::vector<CompressionCandidate> compressions;
  std::vector<std::string> review_ids;
};

struct DreamArtifactRequest {
  std::string cycle_id;
  std::string artifact_kind = "hypothesis";
  std::string content;
  std::vector<std::string> source_memory_ids;
  std::vector<std::string> tags;
  std::string model;
  std::string source_pack_hash;
  double confidence = 0.45;
};

// Research is evidence supplied by a trusted caller. Dreaming never performs
// network access or treats the model's prose as research evidence.
struct DreamResearchEvidence {
  std::string summary;
  std::vector<std::string> source_memory_ids;
  std::vector<std::string> tags;
  Provenance provenance;
  double confidence = 0.7;
};

struct DreamArtifactResult {
  std::string id;
  std::string review_id;
  std::optional<std::string> researched_memory_id;
  std::optional<std::string> research_review_id;
};

class PostProcessingService {
 public:
  PostProcessingService(MemoryService& memories, GraphRepository& graph,
                        ReviewRepository& reviews)
      : memories_(memories), graph_(graph), reviews_(reviews) {}

  PostProcessingReport process(const MemoryScope& scope,
                               std::size_t limit = 500,
                               std::size_t maximum_pairs = 100);
  std::string remember_dream_artifact(const MemoryScope& scope,
                                      std::string content,
                                      std::vector<std::string> source_ids,
                                      double confidence = 0.45);
  DreamArtifactResult remember_dream_artifact(
      const MemoryScope& scope, DreamArtifactRequest request);
  DreamArtifactResult remember_researched_successor(
      const MemoryScope& scope, const std::string& dream_id,
      DreamResearchEvidence evidence);

 private:
  MemoryService& memories_;
  GraphRepository& graph_;
  ReviewRepository& reviews_;
};

}  // namespace qt::memory
