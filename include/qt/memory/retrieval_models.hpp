#pragma once

#include "qt/memory/models.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace qt::memory {

// Identifiers from different stores never share an implicit namespace.
enum class RetrievalOwnerKind { Memory, Code, Entity };

struct RetrievalReference {
  RetrievalOwnerKind kind = RetrievalOwnerKind::Memory;
  std::string id;
  std::int64_t revision = 0;
  std::string repository;
  std::string worktree;
  std::string path;
  std::string source_fingerprint;
  std::uint64_t snapshot_generation = 0;
};

struct MemoryEntity {
  std::string id;
  std::string kind;
  std::string name;
  std::vector<std::string> aliases;
  MemoryScope scope;
};

struct AssertionEvidence {
  std::string source_id;
  std::string source_revision;
  std::string source_uri;
  std::string source_hash;
  std::string source_span;
  std::string created_by;
};

// Assertions attach to an immutable revision of an existing governed memory.
// Lifecycle and authority remain owned by that memory, never by an index.
struct MemoryAssertion {
  std::string memory_id;
  std::int64_t revision = 0;
  std::string subject_id;
  std::string predicate;
  std::string object_id;
  std::string literal_json = "null";
  std::string qualifiers_json = "{}";
  bool positive = true;
  std::optional<TimePoint> valid_from;
  std::optional<TimePoint> valid_to;
  std::vector<AssertionEvidence> evidence;
  // The assertion's knowledge time and immutable memory snapshot.  Queries at
  // an earlier cutoff must use these captured values rather than the current
  // governed memory row.
  TimePoint recorded_at{};
  std::string historical_content;
  std::string historical_source;
  MemoryType historical_type = MemoryType::Observation;
  MemoryStatus historical_status = MemoryStatus::Hypothesis;
  Provenance historical_provenance;
  TimePoint historical_created_at{};
  TimePoint historical_updated_at{};
};

struct RetrievalIndexJob {
  std::int64_t id = 0;
  std::string owner_key;
  std::int64_t owner_revision = 0;
  std::string representation;
  std::string generation;
  std::string lease_token;
  std::size_t attempts = 0;
  std::string content;
  std::string source_uri;
};

struct RetrievalIndexStatus {
  std::size_t pending = 0;
  std::size_t leased = 0;
  std::size_t failed = 0;
  std::size_t complete = 0;
  std::size_t chunks = 0;
};

struct DenseIndexConfig {
  std::string generation;
  std::string model_id;
  std::string model_fingerprint;
  std::string tokenizer_fingerprint;
  std::string query_prefix;
  std::string document_prefix;
  std::string pooling;
  std::size_t dimensions = 0;
  bool normalize = true;
};

struct DenseChunkVector {
  std::string chunk_id;
  std::vector<float> values;
};

struct DenseMemoryCandidate {
  MemoryEntry memory;
  DenseSearchHit hit;
};

struct RetrievalPathEdge {
  RetrievalReference from;
  RetrievalReference to;
  std::string relation;
  std::string evidence_id;
  std::string resolution;
  std::optional<MemoryAssertion> assertion;
  bool heuristic = false;
};

inline const char* retrieval_relation_provenance(const RetrievalPathEdge& edge) {
  if (edge.assertion) return "stored assertion";
  if (edge.from.kind == RetrievalOwnerKind::Code && edge.to.kind == RetrievalOwnerKind::Code &&
      edge.evidence_id.rfind("code-snapshot:", 0) == 0)
    return edge.heuristic ? "heuristic code edge" : "code index relation";
  return "stored relation";
}

struct RetrievalRank {
  std::string retriever;
  std::size_t rank = 0;
  double score = 0.0;
};

struct RetrievalCandidate {
  RetrievalReference owner;
  // Canonical rendered representation used for context injection.
  std::vector<RetrievalChunk> chunks;
  // Transient supporting evidence: the exact representation chunks that
  // matched this owner (for example dense code hits), retained with their
  // similarity and index generation. These are never persisted as canonical
  // memory and never replace the rendered chunk above.
  std::vector<DenseSearchHit> matched_chunks;
  std::vector<RetrievalRank> ranks;
  std::vector<RetrievalPathEdge> path;
  std::vector<std::string> evidence_ids;
  bool eligible = false;
  std::vector<std::string> exclusion_reasons;
  double fused_score = 0.0;
};

}  // namespace qt::memory
