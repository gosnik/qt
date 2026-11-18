#pragma once

#include "qt/memory/repositories.hpp"

#include <string>
#include <vector>

namespace qt::memory {

class MemoryService {
 public:
  explicit MemoryService(MemoryRepository& memories) : memories_(memories) {}

  std::string remember(const MemoryScope& scope, MemoryWrite write);
  std::vector<MemoryEntry> query(const MemoryScope& scope,
                                 const MemoryQuery& query) const;
  MemoryEntry get(const MemoryScope& scope, const std::string& id) const;
  MemoryEntry update(const MemoryScope& scope, const std::string& id,
                     MemoryPatch patch);
  MemoryEntry apply_governed_update(const MemoryScope& scope,
                                    const std::string& id,
                                    MemoryStatus expected_status,
                                    MemoryPatch patch,
                                    std::optional<GraphEdge> edge,
                                    AuditEventWrite audit);
  bool forget(const MemoryScope& scope, const std::string& id);
  std::vector<MemoryRevision> revisions(const MemoryScope& scope,
                                        const std::string& id) const;

  static std::string normalize_content(std::string content);
  static bool redact_secrets(std::string& content);
  static MemoryType infer_type(const std::string& content,
                               MemoryType fallback = MemoryType::Observation);

 private:
  MemoryRepository& memories_;
};

}  // namespace qt::memory
