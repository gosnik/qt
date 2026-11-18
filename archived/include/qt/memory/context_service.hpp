#pragma once

#include "qt/memory/recall_service.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace qt::memory {

struct ContextOptions {
  std::size_t token_budget = 900;
  std::size_t bytes_per_token = 4;
  bool include_pinned_blocks = true;
  bool include_procedures = true;
  bool include_recent_episodes = true;
  bool include_evidence_links = true;
  bool include_warnings = true;
  bool include_provenance = true;
  std::size_t recent_episode_limit = 3U;
  std::size_t max_recent_episode_characters = 1200U;
  RecallPolicy policy = RecallPolicy::Balanced;
  bool include_global_user = false;
};

class ContextService {
 public:
  using CodeValidator = std::function<bool(const MemoryScope&, const RetrievalReference&)>;
  using MemoryValidator = std::function<bool(const MemoryScope&, const std::string&,
                                             std::int64_t, RecallPolicy, bool)>;
  using RelationValidator = std::function<bool(const MemoryScope&, const RetrievalPathEdge&,
                                               RecallPolicy, bool)>;
  explicit ContextService(BlockRepository& blocks,
                          SessionEventRepository* session_events = nullptr,
                          MemoryRepository* memories = nullptr,
                          CodeValidator code_validator = {},
                          StructuredMemoryRepository* structured = nullptr,
                          MemoryValidator memory_validator = {},
                          RelationValidator relation_validator = {})
      : blocks_(blocks), session_events_(session_events), memories_(memories),
        code_validator_(std::move(code_validator)), structured_(structured),
        memory_validator_(std::move(memory_validator)),
        relation_validator_(std::move(relation_validator)) {}

  ContextBlock get_block(const MemoryScope& scope, const std::string& id) const;
  ContextBlock update_block(const MemoryScope& scope, const std::string& id,
                            std::string content);
  std::vector<ContextBlock> blocks(const MemoryScope& scope) const;
  ContextPack build(const MemoryScope& scope,
                    const std::vector<RecalledMemory>& recalled,
                    const ContextOptions& options = {},
                    const std::vector<RetrievalCandidate>& code = {}) const;

 private:
  BlockRepository& blocks_;
  SessionEventRepository* session_events_ = nullptr;
  MemoryRepository* memories_ = nullptr;
  CodeValidator code_validator_;
  StructuredMemoryRepository* structured_ = nullptr;
  MemoryValidator memory_validator_;
  RelationValidator relation_validator_;
};

}  // namespace qt::memory
