#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <qt/agent/agent.hpp>
#include <qt/app/runtime/noninteractive_mode.hpp>
#include <qt/app/cli/cli.hpp>
#include <qt/foundation/error.hpp>
#include <qt/harness/session.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace qt::app {

struct ReferenceStartupSession {
  std::string mode = "create";
  bool persistent = true;
  std::optional<std::string> repository_path;
  std::optional<std::string> parent_repository_path;
  std::optional<std::string> session_id;
  std::optional<std::string> parent_session_id;
  std::optional<std::string> working_directory;
  std::vector<std::string> prompt_history;
  std::size_t entry_count = 0;
};

struct ReferenceStartupSessionResolution {
  ReferenceStartupSession session;
  std::optional<std::string> error;
  std::optional<std::string> stdout_message;
  int exit_code = EXIT_SUCCESS;
};

bool session_arg_looks_like_path(std::string_view session_arg);
std::string generated_session_id();
std::string child_session_path(const std::string& session_dir,
                                         const std::string& source_path,
                                         const std::string& child_session_id);
foundation::Result<ReferenceStartupSession> create_startup_session(
    const std::string& session_dir, std::string mode);
foundation::Result<ReferenceStartupSession> fork_startup_session(
    const std::string& source_path, const std::string& session_dir,
    std::optional<std::string> fork_entry_id = std::nullopt);
foundation::Result<ReferenceStartupSession> open_startup_session(
    const std::string& repository_path, std::string mode);
foundation::Result<ReferenceStartupSession> clone_startup_session(
    const ReferenceStartupSession& startup_session,
    const std::string& session_dir);
bool session_file_has_header(const std::string& path);
ReferenceStartupSessionResolution resolve_startup_session(
    const ReferenceCliArgs& parsed, const std::string& session_dir,
    const std::string& cwd);
foundation::Result<std::string> resolve_startup_session_dir(
    const ReferenceCliArgs& parsed, const std::string& cwd,
    const std::string& agent_home);
std::optional<std::string> missing_session_lookup_arg(
    const ReferenceCliArgs& parsed, const std::string& session_dir);
harness::SessionEntry repository_ready_session_entry(
    const harness::SessionEntry& entry);
foundation::Status append_startup_turn(
    const std::string& repository_path, const ai::Message& user_message,
    const agent::TurnResult& turn);
foundation::Status append_startup_turn(
    const std::string& repository_path, const NoninteractiveRunResult& run);
foundation::Status append_startup_messages(
    const std::string& repository_path,
    const std::vector<ai::Message>& messages);
nlohmann::json session_header_json(
    const harness::SessionMetadata& metadata);

}  // namespace qt::app
