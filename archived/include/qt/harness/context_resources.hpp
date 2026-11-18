#pragma once

#include <optional>
#include <string>
#include <vector>

#include <qt/harness/context_discovery.hpp>

namespace qt::harness {

class RepositoryView;

struct LoadedContextRecord {
  std::string path;
  std::string directory;
  ContextFileKind kind = ContextFileKind::kAgents;
  ContextFileScope scope = ContextFileScope::kCurrentDirectory;
  std::string content;
};

struct InstructionLoadEvent {
  std::string path;
  std::string directory;
  ContextFileKind kind = ContextFileKind::kAgents;
  ContextFileScope scope = ContextFileScope::kCurrentDirectory;
  std::size_t content_bytes = 0;
  std::string reason = "startup";
};

struct LoadedPromptContributionRecord {
  std::string path;
  std::string content;
};

struct RepoContextResources {
  RepoContextDiscovery discovery;
  std::vector<LoadedContextRecord> loaded_context_files;
  std::vector<InstructionLoadEvent> instruction_load_events;
  std::vector<std::string> diagnostics;

  bool has_loaded_context_files() const noexcept {
    return !loaded_context_files.empty();
  }

  void add_diagnostic(std::string message) {
    if (!message.empty()) {
      diagnostics.push_back(std::move(message));
    }
  }
};

struct RepoPromptContributionResources {
  std::optional<LoadedPromptContributionRecord> system_prompt;
  std::optional<LoadedPromptContributionRecord> append_system_prompt;
  std::vector<std::string> diagnostics;

  void add_diagnostic(std::string message) {
    if (!message.empty()) {
      diagnostics.push_back(std::move(message));
    }
  }
};

RepoContextResources load_repo_context_resources(
    const RepositoryView& repository, const RepoContextDiscovery& discovery);

RepoContextResources discover_and_load_repo_context(
    const RepositoryView& repository, const RepoDiscoveryRequest& request);

RepoPromptContributionResources load_repo_prompt_contributions(
    const RepositoryView& repository, const std::string& cwd,
    const std::optional<std::string>& agent_home);

RepoPromptContributionResources discover_and_load_repo_prompt_contributions(
    const RepositoryView& repository, const RepoDiscoveryRequest& request);

}  // namespace qt::harness
