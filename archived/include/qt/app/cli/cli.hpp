#pragma once

#include <iosfwd>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qt::app {

enum class ReferenceCliDiagnosticSeverity {
  warning,
  error,
};

struct ReferenceCliDiagnostic {
  ReferenceCliDiagnosticSeverity severity = ReferenceCliDiagnosticSeverity::error;
  std::string message;
};

enum class ReferenceStartupMode {
  interactive,
  print,
  json,
  rpc,
};

struct ReferenceCliArgs {
  std::optional<std::string> provider;
  std::optional<std::string> model;
  std::optional<std::string> api_key;
  std::optional<std::string> system_prompt;
  std::vector<std::string> append_system_prompt;
  std::optional<std::string> thinking;
  bool continue_session = false;
  bool resume = false;
  bool help = false;
  bool version = false;
  std::optional<std::string> mode;
  bool no_session = false;
  std::optional<std::string> session;
  std::optional<std::string> fork;
  std::optional<std::string> session_dir;
  std::vector<std::string> models;
  std::vector<std::string> tools;
  bool no_tools = false;
  bool no_builtin_tools = false;
  bool no_memory = false;
  std::vector<std::string> extensions;
  bool no_extensions = false;
  bool print = false;
  std::optional<std::string> export_path;
  bool no_skills = false;
  std::vector<std::string> skills;
  std::vector<std::string> prompt_templates;
  bool no_prompt_templates = false;
  std::vector<std::string> themes;
  bool no_themes = false;
  bool no_context_files = false;
  std::vector<std::string> local_model_dirs;
  std::optional<std::string> list_models;
  bool list_models_all = false;
  bool offline = false;
  bool verbose = false;
  std::vector<std::string> log_files;
  bool log_stderr = false;
  std::optional<std::string> log_level;
  std::vector<std::string> messages;
  std::vector<std::string> file_args;
  nlohmann::json unknown_flags = nlohmann::json::object();
  nlohmann::json extension_flags = nlohmann::json::object();
  std::vector<ReferenceCliDiagnostic> diagnostics;
};

bool is_valid_thinking_level(std::string_view level);
ReferenceCliArgs parse_cli_args(const std::vector<std::string>& args);
bool has_native_mode(const std::vector<std::string>& args);
ReferenceStartupMode resolve_startup_mode(
    const ReferenceCliArgs& parsed, bool stdin_is_tty);
void print_help(std::ostream& out);

}  // namespace qt::app
