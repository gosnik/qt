#include <qt/harness/context_discovery.hpp>
#include <qt/harness/repository.hpp>

#include <filesystem>

namespace qt::harness {
namespace {

using Path = std::filesystem::path;

constexpr const char* kAgentsFilename = "AGENTS.md";
constexpr const char* kAgentsFilenameUpperExtension = "AGENTS.MD";
constexpr const char* kClaudeFilename = "CLAUDE.md";
constexpr const char* kClaudeFilenameUpperExtension = "CLAUDE.MD";
constexpr const char* kSkillsDirectory = "skills";
constexpr const char* kPromptsDirectory = "prompts";
constexpr const char* kProjectConfigDirectory = ".pi";
constexpr const char* kAgentSkillsDirectory = ".agents";
constexpr const char* kAgentHomeDirectory = "agent";

std::string path_to_string(const Path& path) { return path.generic_string(); }

std::optional<std::string> resolve_user_agents_skill_root(
    const std::string& agent_home) {
  const Path home_path(agent_home);
  const Path pi_directory = home_path.parent_path();
  if (home_path.filename() != kAgentHomeDirectory ||
      pi_directory.filename() != kProjectConfigDirectory) {
    return std::nullopt;
  }

  const Path user_home = pi_directory.parent_path();
  if (user_home.empty()) {
    return std::nullopt;
  }

  return path_to_string(Path(user_home) / kAgentSkillsDirectory /
                        kSkillsDirectory);
}

bool is_same_or_descendant(const std::string& path, const std::string& root) {
  const Path path_value(path);
  const Path root_value(root);

  auto path_it = path_value.begin();
  auto root_it = root_value.begin();

  for (; root_it != root_value.end(); ++root_it, ++path_it) {
    if (path_it == path_value.end() || *path_it != *root_it) {
      return false;
    }
  }

  return true;
}

std::vector<std::string> walk_directories(const RepositoryView& repository,
                                          const std::string& cwd,
                                          const std::optional<std::string>& stop_at) {
  std::vector<std::string> directories;
  auto current = repository.canonicalize_path(cwd);
  if (!current) {
    return directories;
  }

  while (current) {
    directories.push_back(*current);
    if (stop_at && *current == *stop_at) {
      break;
    }

    const auto parent = repository.parent_directory(*current);
    if (!parent || *parent == *current) {
      break;
    }
    current = parent;
  }

  std::reverse(directories.begin(), directories.end());
  return directories;
}

void add_context_candidates(RepoContextDiscovery& discovery,
                            const RepositoryView& repository,
                            const std::string& directory,
                            ContextFileScope scope) {
  const struct Candidate {
    const char* filenames[2];
    ContextFileKind kind;
  } candidates[] = {
      {{kAgentsFilename, kAgentsFilenameUpperExtension}, ContextFileKind::kAgents},
      {{kClaudeFilename, kClaudeFilenameUpperExtension}, ContextFileKind::kClaude},
  };

  for (const auto& candidate : candidates) {
    for (const char* filename : candidate.filenames) {
      const Path path = Path(directory) / filename;
      const std::string full_path = path_to_string(path);
      if (!repository.file_exists(full_path)) {
        continue;
      }

      discovery.add_context_file(ContextFileRecord{
          full_path,
          directory,
          candidate.kind,
          scope,
      });
      break;
    }
  }
}

enum class ContextRootKind { skills, prompts };

void add_root_if_directory(RepoContextDiscovery& discovery,
                           const RepositoryView& repository,
                           std::string path,
                           ContextRootKind kind) {
  if (!repository.directory_exists(path)) {
    return;
  }
  if (kind == ContextRootKind::skills) {
    discovery.add_skill_root(std::move(path));
  } else {
    discovery.add_prompt_root(std::move(path));
  }
}

std::optional<std::string> resolve_repository_root(
    RepoContextDiscovery& discovery,
    const RepositoryView& repository,
    const std::string& cwd,
    const std::optional<std::string>& root_hint) {
  if (root_hint && !root_hint->empty()) {
    const auto canonical_hint = repository.canonicalize_path(*root_hint);
    if (!canonical_hint) {
      discovery.add_diagnostic("repository_root_hint could not be canonicalized: " +
                               *root_hint);
      return repository.find_repository_root(cwd);
    }
    if (!is_same_or_descendant(cwd, *canonical_hint)) {
      discovery.add_diagnostic("repository_root_hint does not contain cwd: " +
                               *canonical_hint);
      return repository.find_repository_root(cwd);
    }
    return canonical_hint;
  }

  return repository.find_repository_root(cwd);
}

}  // namespace

RepoContextDiscovery discover_repo_context(const RepositoryView& repository,
                                           const RepoDiscoveryRequest& request) {
  RepoContextDiscovery discovery = make_repo_context_discovery(request);

  const auto canonical_cwd = repository.canonicalize_path(
      request.cwd.empty() ? repository.cwd() : request.cwd);
  if (!canonical_cwd) {
    discovery.add_diagnostic("cwd could not be canonicalized: " + request.cwd);
    return discovery;
  }
  discovery.cwd = *canonical_cwd;

  discovery.repository_root = resolve_repository_root(
      discovery, repository, discovery.cwd, request.repository_root_hint);

  if (request.include_context_files && request.agent_home &&
      !request.agent_home->empty()) {
    const auto global_root = repository.canonicalize_path(*request.agent_home);
    if (global_root) {
      add_context_candidates(discovery, repository, *global_root,
                             ContextFileScope::kGlobal);
    } else {
      discovery.add_diagnostic("agent_home could not be canonicalized: " +
                               *request.agent_home);
    }
  }

  const auto context_directories =
      walk_directories(repository, discovery.cwd, std::nullopt);
  const auto resource_directories =
      walk_directories(repository, discovery.cwd, discovery.repository_root);

  if (request.include_context_files) {
    for (const auto& directory : context_directories) {
      const auto scope = directory == discovery.cwd
                             ? ContextFileScope::kCurrentDirectory
                             : ContextFileScope::kAncestor;
      add_context_candidates(discovery, repository, directory, scope);
    }
  }

  if (request.include_skill_roots) {
    if (request.agent_home && !request.agent_home->empty()) {
      if (const auto global_root = repository.canonicalize_path(*request.agent_home)) {
        add_root_if_directory(
            discovery, repository,
            path_to_string(Path(*global_root) / kSkillsDirectory),
            ContextRootKind::skills);
        if (const auto user_agents_root =
                resolve_user_agents_skill_root(*global_root)) {
          add_root_if_directory(
              discovery, repository, *user_agents_root,
              ContextRootKind::skills);
        }
      }
    }

    if (discovery.repository_root) {
      add_root_if_directory(
          discovery, repository,
          path_to_string(Path(*discovery.repository_root) / kProjectConfigDirectory /
                         kSkillsDirectory),
          ContextRootKind::skills);
    }

    for (const auto& directory : resource_directories) {
      add_root_if_directory(
          discovery, repository,
          path_to_string(Path(directory) / kAgentSkillsDirectory / kSkillsDirectory),
          ContextRootKind::skills);
    }
  }

  if (request.include_prompt_roots) {
    if (request.agent_home && !request.agent_home->empty()) {
      if (const auto global_root = repository.canonicalize_path(*request.agent_home)) {
        add_root_if_directory(
            discovery, repository,
            path_to_string(Path(*global_root) / kPromptsDirectory),
            ContextRootKind::prompts);
      }
    }

    if (discovery.repository_root) {
      add_root_if_directory(
          discovery, repository,
          path_to_string(Path(*discovery.repository_root) / kProjectConfigDirectory /
                         kPromptsDirectory),
          ContextRootKind::prompts);
    }

    for (const auto& directory : resource_directories) {
      add_root_if_directory(
          discovery, repository,
          path_to_string(Path(directory) / kAgentSkillsDirectory / kPromptsDirectory),
          ContextRootKind::prompts);
    }
  }

  return discovery;
}

}  // namespace qt::harness
