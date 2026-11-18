#pragma once

#include <qt/extensions/loader.hpp>
#include <qt/harness/repository.hpp>

#include <string>
#include <vector>

namespace qt::app {

struct LoadReferenceExtensionOptions {
    bool include_discovered_extensions = true;
};

qt::extensions::ExtensionLoadResult load_extensions(
    const qt::harness::RepositoryView& repository, const std::string& cwd,
    const std::string& agent_home, const std::string& workspace_root,
    const std::vector<std::string>& cli_extensions,
    LoadReferenceExtensionOptions options = {});

}  // namespace qt::app
