#pragma once

#include "qt/memory/database.hpp"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace qt::memory {

using RetrievalCancellation = std::function<bool()>;
// Wait at most the supplied duration for background work to become runnable.
// False means still paused. Borrowed only for the synchronous step call.
using RetrievalIndexWait = std::function<bool(std::chrono::milliseconds)>;
// Synchronous: implementations must finish all access to the borrowed
// cancellation callback before returning or throwing.
using RetrievalEmbedder = std::function<std::vector<float>(
    const DenseIndexConfig&, const std::string&, const RetrievalCancellation&)>;

struct RetrievalIndexStep {
  bool worked = false;
  bool completed = false;
  std::string owner_key;
  std::string error;
};

// Processes one durable lease at a time. Runtime owns scheduling and lifetime;
// cancellation does not detach a worker or leave a publication running behind it.
class RetrievalIndexer {
 public:
  RetrievalIndexer(SQLiteMemoryDatabase& database, RetrievalEmbedder embedder)
      : database_(database), embedder_(std::move(embedder)) {}
  RetrievalIndexStep step(const MemoryScope&, const RetrievalCancellation& = {},
                         std::optional<std::string> dense_generation = std::nullopt,
                         const RetrievalIndexWait& = {});
  static std::vector<RetrievalChunk> render(const std::string&);

 private:
  SQLiteMemoryDatabase& database_;
  RetrievalEmbedder embedder_;
};

}  // namespace qt::memory
