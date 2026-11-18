#include "qt/app/runtime/extensions.hpp"

#include "qt/foundation/filesystem.hpp"
#include "qt/tools/package_manager_tool.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace qt::app {
namespace {

std::string extension_source_identity(
    const qt::harness::RepositoryView& repository, const std::string& path) {
  if (const auto canonical = repository.canonicalize_path(path)) {
    return *canonical;
  }
  return std::filesystem::path(path).lexically_normal().generic_string();
}

void merge_extension_load_result(
    const qt::harness::RepositoryView& repository,
    qt::extensions::ExtensionLoadResult& target,
    qt::extensions::ExtensionLoadResult loaded) {
  std::vector<std::string> root_identities;
  root_identities.reserve(target.roots.size());
  for (const auto& root : target.roots) {
    root_identities.push_back(extension_source_identity(repository, root));
  }

  for (auto& root : loaded.roots) {
    const std::string identity = extension_source_identity(repository, root);
    if (std::find(root_identities.begin(), root_identities.end(), identity) ==
        root_identities.end()) {
      target.roots.push_back(std::move(root));
      root_identities.push_back(identity);
    }
  }

  std::vector<std::string> extension_identities;
  extension_identities.reserve(target.loaded_extensions.size());
  for (const auto& extension : target.loaded_extensions) {
    extension_identities.push_back(
        extension_source_identity(repository, extension.path));
  }

  for (auto& extension : loaded.loaded_extensions) {
    const std::string identity =
        extension_source_identity(repository, extension.path);
    if (std::find(extension_identities.begin(), extension_identities.end(),
                  identity) != extension_identities.end()) {
      continue;
    }
    target.loaded_extensions.push_back(std::move(extension));
    extension_identities.push_back(identity);
  }

  for (auto& diagnostic : loaded.diagnostics) {
    target.diagnostics.push_back(std::move(diagnostic));
  }
}

}  // namespace

qt::extensions::ExtensionLoadResult load_extensions(
    const qt::harness::RepositoryView& repository, const std::string& cwd,
    const std::string& agent_home, const std::string& workspace_root,
    const std::vector<std::string>& cli_extensions,
    LoadReferenceExtensionOptions options) {
  qt::extensions::ExtensionLoadResult aggregate;

  if (!cli_extensions.empty()) {
    merge_extension_load_result(
        repository, aggregate,
        qt::extensions::load_extension_manifests(repository, cli_extensions));
  }

  if (!options.include_discovered_extensions) {
    return aggregate;
  }

  std::vector<std::string> discovered_roots;
  if (!workspace_root.empty()) {
    discovered_roots.push_back(
        (std::filesystem::path(workspace_root) / ".pi" / "extensions")
            .generic_string());
  }
  if (!agent_home.empty()) {
    discovered_roots.push_back(
        (std::filesystem::path(agent_home) / "extensions").generic_string());
  }

  if (!discovered_roots.empty()) {
    merge_extension_load_result(
        repository, aggregate,
        qt::extensions::load_extension_manifests(repository, discovered_roots));
  }

  if (agent_home.empty()) {
    return aggregate;
  }

  const auto package_settings = qt::tools::load_package_command_settings(
      qt::foundation::Path(workspace_root.empty() ? cwd : workspace_root),
      qt::foundation::Path(agent_home));
  if (!package_settings) {
    aggregate.add_diagnostic("failed to load package settings: " +
                             package_settings.error().summary());
    return aggregate;
  }

  const auto load_packages =
      [&](const std::vector<qt::tools::PackageConfiguredSource>& packages) {
        for (const auto& package : packages) {
          if (!package.installed_path) {
            continue;
          }
          if (package.filtered && package.extensions.empty()) {
            continue;
          }
          qt::extensions::ExtensionLoadOptions options;
          options.filter_patterns = package.extensions;
          merge_extension_load_result(
              repository, aggregate,
              qt::extensions::load_extension_manifests(
                  repository, {*package.installed_path}, options));
        }
      };

  load_packages(package_settings.value().project_packages);
  load_packages(package_settings.value().user_packages);
  return aggregate;
}

}  // namespace qt::app
