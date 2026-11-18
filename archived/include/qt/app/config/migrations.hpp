#pragma once

#include "qt/foundation/filesystem.hpp"

#include <string>
#include <vector>

namespace qt::app {

struct StartupMigrationResult {
  std::vector<std::string> migrated_auth_providers;
  bool moved_agent_root_sessions = false;
  bool moved_managed_binaries = false;
  bool migrated_global_commands_dir = false;
  bool migrated_project_commands_dir = false;
};

std::vector<std::string> migrate_auth_to_auth_json(
    const qt::foundation::Path& agent_home);

bool migrate_sessions_from_agent_root(const qt::foundation::Path& agent_home);

bool migrate_tools_to_bin(const qt::foundation::Path& agent_home);

bool migrate_commands_to_prompts(const qt::foundation::Path& base_dir);

StartupMigrationResult run_startup_migrations(
    const qt::foundation::Path& cwd, const qt::foundation::Path& agent_home);

}  // namespace qt::app
