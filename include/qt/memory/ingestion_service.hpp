#pragma once

#include "qt/memory/memory_service.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace qt::memory {

class MemoryIngestionService {
 public:
  explicit MemoryIngestionService(MemoryService& memories)
      : memories_(memories) {}

  static std::vector<std::string> chunk_transcript(
      const std::string& content, std::size_t max_characters = 6000U);
  std::vector<IngestionCandidate> extract(const IngestionRequest& request) const;
  IngestionResult ingest(const MemoryScope& scope,
                         const IngestionRequest& request,
                         bool persist_candidates);

 private:
  MemoryService& memories_;
};

}  // namespace qt::memory
