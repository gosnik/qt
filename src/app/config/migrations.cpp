#include "qt/app/config/migrations.hpp"

#include "qt/foundation/filesystem.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>

namespace qt::app {
namespace {

using Json = nlohmann::json;

std::optional<Json> read_json_file(const qt::foundation::Path& path) {
  const auto content = qt::foundation::read_text_file(path);
  if (!content) {
    return std::nullopt;
  }
  try {
    return Json::parse(content.value());
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

bool write_json_file(const qt::foundation::Path& path, const Json& json) {
  const auto status =
      qt::foundation::write_text_file(path, json.dump(2) + "\n");
  return status.ok();
}

std::optional<std::string> first_non_empty_line(
    const qt::foundation::Path& path) {
  std::ifstream input(path);
  if (!input) {
    return std::nullopt;
  }

  std::string line;
  while (std::getline(input, line)) {
    if (line.find_first_not_of(" \t\r\n") != std::string::npos) {
      return line;
    }
  }
  return std::nullopt;
}

std::string encode_session_cwd(std::string cwd) {
  while (!cwd.empty() && (cwd.front() == '/' || cwd.front() == '\\')) {
    cwd.erase(cwd.begin());
  }
  std::replace(cwd.begin(), cwd.end(), '/', '-');
  std::replace(cwd.begin(), cwd.end(), '\\', '-');
  std::replace(cwd.begin(), cwd.end(), ':', '-');
  return "--" + cwd + "--";
}

bool move_file_if_missing_target(const qt::foundation::Path& source,
                                 const qt::foundation::Path& target) {
  std::error_code exists_error;
  if (std::filesystem::exists(target, exists_error) || exists_error) {
    return false;
  }

  std::error_code create_error;
  std::filesystem::create_directories(target.parent_path(), create_error);
  if (create_error) {
    return false;
  }

  std::error_code rename_error;
  std::filesystem::rename(source, target, rename_error);
  return !rename_error;
}

}  // namespace

std::vector<std::string> migrate_auth_to_auth_json(
    const qt::foundation::Path& agent_home) {
  std::vector<std::string> providers;
  const auto auth_path = agent_home / "auth.json";
  const auto oauth_path = agent_home / "oauth.json";
  const auto settings_path = agent_home / "settings.json";

  std::error_code exists_error;
  if (std::filesystem::exists(auth_path, exists_error) || exists_error) {
    return providers;
  }

  Json migrated = Json::object();

  if (const auto oauth = read_json_file(oauth_path);
      oauth && oauth->is_object()) {
    for (auto it = oauth->begin(); it != oauth->end(); ++it) {
      if (!it.value().is_object()) {
        continue;
      }
      Json record = it.value();
      record["type"] = "oauth";
      migrated[it.key()] = std::move(record);
      providers.push_back(it.key());
    }

    std::error_code rename_error;
    std::filesystem::rename(oauth_path, oauth_path.string() + ".migrated",
                            rename_error);
  }

  if (const auto settings = read_json_file(settings_path);
      settings && settings->is_object()) {
    Json updated = *settings;
    const auto api_keys = updated.find("apiKeys");
    if (api_keys != updated.end() && api_keys->is_object()) {
      for (auto it = api_keys->begin(); it != api_keys->end(); ++it) {
        if (migrated.contains(it.key()) || !it.value().is_string()) {
          continue;
        }
        migrated[it.key()] = Json{{"type", "api_key"},
                                  {"key", it.value().get<std::string>()}};
        providers.push_back(it.key());
      }
      updated.erase("apiKeys");
      write_json_file(settings_path, updated);
    }
  }

  if (!migrated.empty()) {
    std::error_code create_error;
    std::filesystem::create_directories(auth_path.parent_path(), create_error);
    if (!create_error) {
      write_json_file(auth_path, migrated);
    }
  }

  return providers;
}

bool migrate_sessions_from_agent_root(const qt::foundation::Path& agent_home) {
  std::error_code read_error;
  if (!std::filesystem::exists(agent_home, read_error) || read_error) {
    return false;
  }

  bool moved_any = false;
  for (const auto& entry : std::filesystem::directory_iterator(agent_home)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".jsonl") {
      continue;
    }

    const auto first_line = first_non_empty_line(entry.path());
    if (!first_line) {
      continue;
    }

    Json header;
    try {
      header = Json::parse(*first_line);
    } catch (const std::exception&) {
      continue;
    }

    if (!header.is_object() || header.value("type", std::string{}) != "session" ||
        !header.contains("cwd") || !header.at("cwd").is_string()) {
      continue;
    }

    const auto target_dir =
        agent_home / "sessions" /
        encode_session_cwd(header.at("cwd").get<std::string>());
    const auto target = target_dir / entry.path().filename();
    moved_any = move_file_if_missing_target(entry.path(), target) || moved_any;
  }

  return moved_any;
}

bool migrate_tools_to_bin(const qt::foundation::Path& agent_home) {
  const auto tools_dir = agent_home / "tools";
  const auto bin_dir = agent_home / "bin";
  bool moved_any = false;

  for (const char* name : {"fd", "rg", "fd.exe", "rg.exe"}) {
    const auto source = tools_dir / name;
    std::error_code exists_error;
    if (!std::filesystem::exists(source, exists_error) || exists_error) {
      continue;
    }

    const auto target = bin_dir / name;
    std::error_code create_error;
    std::filesystem::create_directories(bin_dir, create_error);
    if (create_error) {
      continue;
    }

    std::error_code target_exists_error;
    if (std::filesystem::exists(target, target_exists_error) &&
        !target_exists_error) {
      std::error_code remove_error;
      std::filesystem::remove(source, remove_error);
      continue;
    }

    std::error_code rename_error;
    std::filesystem::rename(source, target, rename_error);
    if (!rename_error) {
      moved_any = true;
    }
  }

  return moved_any;
}

bool migrate_commands_to_prompts(const qt::foundation::Path& base_dir) {
  const auto commands_dir = base_dir / "commands";
  const auto prompts_dir = base_dir / "prompts";

  std::error_code commands_error;
  const bool has_commands =
      std::filesystem::exists(commands_dir, commands_error) && !commands_error;
  if (!has_commands) {
    return false;
  }

  std::error_code prompts_error;
  const bool has_prompts =
      std::filesystem::exists(prompts_dir, prompts_error) && !prompts_error;
  if (has_prompts || prompts_error) {
    return false;
  }

  std::error_code rename_error;
  std::filesystem::rename(commands_dir, prompts_dir, rename_error);
  return !rename_error;
}

StartupMigrationResult run_startup_migrations(
    const qt::foundation::Path& cwd, const qt::foundation::Path& agent_home) {
  StartupMigrationResult result;
  result.migrated_auth_providers = migrate_auth_to_auth_json(agent_home);
  result.moved_agent_root_sessions = migrate_sessions_from_agent_root(agent_home);
  result.moved_managed_binaries = migrate_tools_to_bin(agent_home);
  result.migrated_global_commands_dir = migrate_commands_to_prompts(agent_home);
  result.migrated_project_commands_dir =
      migrate_commands_to_prompts(cwd / ".pi");
  return result;
}

}  // namespace qt::app
