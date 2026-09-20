#include <qt/harness/context_resources.hpp>

#include <qt/harness/repository.hpp>

namespace qt::harness {
namespace {

std::optional<LoadedPromptContributionRecord> load_optional_prompt_contribution(
    const RepositoryView& repository, const std::string& path,
    RepoPromptContributionResources& resources, const char* label) {
  if (!repository.file_exists(path)) {
    return std::nullopt;
  }

  const auto content = repository.read_text_file(path);
  if (!content) {
    resources.add_diagnostic(std::string("failed to read ") + label +
                             " file: " + path);
    return std::nullopt;
  }

  return LoadedPromptContributionRecord{path, *content};
}

}  // namespace

RepoContextResources load_repo_context_resources(
    const RepositoryView& repository, const RepoContextDiscovery& discovery) {
  RepoContextResources resources;
  resources.discovery = discovery;

  for (const auto& message : discovery.diagnostics) {
    resources.add_diagnostic(message);
  }

  for (const auto& record : discovery.context_files) {
    const auto content = repository.read_text_file(record.path);
    if (!content) {
      resources.add_diagnostic("failed to read context file: " + record.path);
      continue;
    }

    LoadedContextRecord loaded{
        record.path,
        record.directory,
        record.kind,
        record.scope,
        *content,
    };
    resources.instruction_load_events.push_back(InstructionLoadEvent{
        loaded.path,
        loaded.directory,
        loaded.kind,
        loaded.scope,
        loaded.content.size(),
        "startup",
    });
    resources.loaded_context_files.push_back(std::move(loaded));
  }

  return resources;
}

RepoContextResources discover_and_load_repo_context(
    const RepositoryView& repository, const RepoDiscoveryRequest& request) {
  return load_repo_context_resources(repository,
                                     discover_repo_context(repository, request));
}

RepoPromptContributionResources load_repo_prompt_contributions(
    const RepositoryView& repository, const std::string& cwd,
    const std::optional<std::string>& agent_home) {
  RepoPromptContributionResources resources;

  const std::string project_system_prompt = cwd + "/.pi/SYSTEM.md";
  const std::string global_system_prompt =
      agent_home && !agent_home->empty() ? *agent_home + "/SYSTEM.md" : "";
  const std::string project_append_prompt = cwd + "/.pi/APPEND_SYSTEM.md";
  const std::string global_append_prompt =
      agent_home && !agent_home->empty() ? *agent_home + "/APPEND_SYSTEM.md" : "";

  resources.system_prompt = load_optional_prompt_contribution(
      repository, project_system_prompt, resources, "project system prompt");
  if (!resources.system_prompt.has_value() && !global_system_prompt.empty()) {
    resources.system_prompt = load_optional_prompt_contribution(
        repository, global_system_prompt, resources, "global system prompt");
  }

  resources.append_system_prompt = load_optional_prompt_contribution(
      repository, project_append_prompt, resources,
      "project append system prompt");
  if (!resources.append_system_prompt.has_value() &&
      !global_append_prompt.empty()) {
    resources.append_system_prompt = load_optional_prompt_contribution(
        repository, global_append_prompt, resources,
        "global append system prompt");
  }

  return resources;
}

RepoPromptContributionResources discover_and_load_repo_prompt_contributions(
    const RepositoryView& repository, const RepoDiscoveryRequest& request) {
  const auto discovery = discover_repo_context(repository, request);
  return load_repo_prompt_contributions(repository, discovery.cwd,
                                        request.agent_home);
}

}  // namespace qt::harness
