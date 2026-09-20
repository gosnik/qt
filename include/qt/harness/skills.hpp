#pragma once

#include <string>
#include <vector>

#include <qt/harness/context_discovery.hpp>

namespace qt::harness {

class RepositoryView;

struct LoadedSkillRecord {
  std::string root;
  std::string directory;
  std::string path;
  std::string name;
  std::string description;
  std::string content;
  bool disable_model_invocation = false;
};

struct SkillResources {
  std::vector<std::string> roots;
  std::vector<LoadedSkillRecord> loaded_skills;
  std::vector<std::string> diagnostics;

  bool has_loaded_skills() const noexcept { return !loaded_skills.empty(); }

  void add_diagnostic(std::string message) {
    if (!message.empty()) {
      diagnostics.push_back(std::move(message));
    }
  }
};

SkillResources load_skill_resources(const RepositoryView& repository,
                                   const std::vector<std::string>& skill_roots);

SkillResources discover_and_load_skills(const RepositoryView& repository,
                                        const RepoContextDiscovery& discovery);

}  // namespace qt::harness
