#pragma once

#include "qt/memory/retrieval_indexer.hpp"

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace qt::memory {

struct DenseRetrievalStats {
  std::size_t cache_hits = 0;
  std::size_t embedding_failures = 0;
  std::size_t cache_entries = 0;
  std::size_t cache_bytes = 0;
  std::string last_error;
};

class DenseRetrievalService {
 public:
  DenseRetrievalService(SQLiteMemoryDatabase& database,
                        RetrievalEmbedder embedder,
                        std::size_t cache_capacity = 64,
                        std::size_t cache_byte_capacity = 32U * 1024U * 1024U);

  std::vector<DenseSearchHit> search(const MemoryScope& scope,
                                     const std::string& query,
                                     RecallPolicy policy,
                                     bool include_global_user,
                                     std::size_t limit,
                                     const RetrievalCancellation& cancellation = {});
  DenseRetrievalStats stats() const;
  void clear_cache();

 private:
  struct CacheEntry {
    std::vector<float> embedding;
    std::size_t age = 0;
    std::size_t bytes = 0;
  };

  SQLiteMemoryDatabase& database_;
  RetrievalEmbedder embedder_;
  std::size_t cache_capacity_;
  std::size_t cache_byte_capacity_;
  std::size_t cache_bytes_ = 0;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, CacheEntry> cache_;
  std::size_t next_age_ = 0;
  DenseRetrievalStats stats_;
};

}  // namespace qt::memory
