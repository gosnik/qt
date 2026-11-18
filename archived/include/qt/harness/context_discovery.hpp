#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace qt::harness {

class RepositoryView;

enum class ContextFileKind {
  kAgents,
  kClaude,
};

enum class ContextFileScope {
  kGlobal,
  kAncestor,
  kCurrentDirectory,
};

struct ContextFileRecord {
  std::string path;
  std::string directory;
  ContextFileKind kind = ContextFileKind::kAgents;
  ContextFileScope scope = ContextFileScope::kCurrentDirectory;
};

struct RepoDiscoveryRequest {
  std::string cwd;
  std::optional<std::string> agent_home;
  std::optional<std::string> repository_root_hint;
  bool include_context_files = true;
  bool include_skill_roots = true;
  bool include_prompt_roots = true;
};

struct RepoContextDiscovery {
  std::string cwd;
  std::optional<std::string> repository_root;
  std::vector<ContextFileRecord> context_files;
  std::vector<std::string> skill_roots;
  std::vector<std::string> prompt_roots;
  std::vector<std::string> diagnostics;

  bool inside_repository() const noexcept {
    return repository_root.has_value() && !repository_root->empty();
  }

  bool has_context_files() const noexcept { return !context_files.empty(); }

  bool has_skill_roots() const noexcept { return !skill_roots.empty(); }

  bool has_prompt_roots() const noexcept { return !prompt_roots.empty(); }

  void add_context_file(ContextFileRecord record) {
    const auto duplicate = std::find_if(
        context_files.begin(), context_files.end(),
        [&record](const ContextFileRecord& existing) {
          return existing.path == record.path;
        });
    if (duplicate == context_files.end()) {
      context_files.push_back(std::move(record));
    }
  }

  void add_skill_root(std::string path) {
    if (!contains_path(skill_roots, path)) {
      skill_roots.push_back(std::move(path));
    }
  }

  void add_prompt_root(std::string path) {
    if (!contains_path(prompt_roots, path)) {
      prompt_roots.push_back(std::move(path));
    }
  }

  void add_diagnostic(std::string message) {
    if (!message.empty()) {
      diagnostics.push_back(std::move(message));
    }
  }

 private:
  static bool contains_path(const std::vector<std::string>& paths,
                            const std::string& candidate) {
    return std::find(paths.begin(), paths.end(), candidate) != paths.end();
  }
};

inline RepoContextDiscovery make_repo_context_discovery(
    const RepoDiscoveryRequest& request) {
  RepoContextDiscovery discovery;
  discovery.cwd = request.cwd;
  discovery.repository_root = request.repository_root_hint;
  return discovery;
}

RepoContextDiscovery discover_repo_context(const RepositoryView& repository,
                                           const RepoDiscoveryRequest& request);

}  // namespace qt::harness
