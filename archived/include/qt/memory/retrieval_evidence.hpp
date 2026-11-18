#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace qt::memory {

// Captured representation evidence is transient; it is not canonical memory.
struct RetrievalChunk {
  std::string id;
  std::string owner_key;
  std::int64_t owner_revision = 0;
  std::size_t ordinal = 0;
  std::string text;
  std::string content_hash;
  std::string source_uri;
  std::string source_span;
  std::string render_version = "text-v1";
};

struct DenseSearchHit {
  RetrievalChunk chunk;
  double similarity = 0.0;
  std::string generation;
};

}  // namespace qt::memory
