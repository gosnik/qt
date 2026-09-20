#pragma once

#include "qt/memory/governance_service.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace qt::memory {

struct SkillImportItem {
  std::filesystem::path skill_path;
  std::string memory_id;
  std::string action;
  std::string diagnostic;
  std::size_t related_files = 0U;
};

using SkillImportProgress = std::function<void(const SkillImportItem&)>;

struct SkillImportOptions {
  std::filesystem::path root;
  // When set, the canonical scan root must remain inside this directory. This
  // is rechecked by the worker immediately before traversal.
  std::filesystem::path containment_root;
  bool recursive = false;
  std::size_t maximum_skill_bytes = 96U * 1024U;
  std::size_t maximum_related_files = 32U;
  std::size_t maximum_package_files = 512U;
  std::size_t maximum_related_file_bytes = 12U * 1024U;
  std::size_t maximum_skills = 1024U;
  std::size_t maximum_walk_entries = 65536U;
  std::size_t maximum_depth = 64U;
  SkillImportProgress progress;
};

struct SkillImportReport {
  std::filesystem::path root;
  bool recursive = false;
  std::size_t discovered = 0U;
  std::size_t created = 0U;
  std::size_t updated = 0U;
  std::size_t unchanged = 0U;
  std::size_t failed = 0U;
  std::vector<SkillImportItem> items;
};

SkillImportReport import_skill_memories(MemoryService& memories,
                                        GovernanceService& governance,
                                        const MemoryScope& scope,
                                        const SkillImportOptions& options);

}  // namespace qt::memory
