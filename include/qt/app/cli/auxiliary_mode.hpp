#pragma once

#include <string>
#include <vector>

namespace qt::app {

struct ReferenceAuxiliaryModeArgs {
  bool inspect_context = false;
  bool inspect_context_resources = false;
  bool inspect_prompt_assembly = false;
  bool inspect_skills = false;
  bool inspect_prompt_templates = false;
  bool inspect_slash_commands = false;
  bool inspect_settings = false;
  bool inspect_app_resources = false;
  bool inspect_system_prompt = false;
  bool inspect_extensions = false;
  bool inspect_extension_runners = false;
  std::string inspect_session_path;
  std::string resume_session_path;
  std::string inspect_session_catalog_path;
  std::string create_session_path;
  std::string fork_session_path;
  std::string clone_session_path;
  std::string compact_session_path;
  bool apply_session_compaction = false;
  std::string child_session_path;
  std::string session_id;
  std::string session_name;
  std::string created_at;
  std::string child_working_directory;
  std::string rpc_envelope_path;
  std::string rpc_frame_path;
  std::string read_text_path;
  std::string list_directory_path;
  std::string complete_path_prefix;
  std::string search_text_path;
  std::string write_text_path;
  std::string append_text_path;
  std::string edit_path;
  std::string old_text;
  std::string new_text;
  std::string bash_command;
  std::string text;
  std::string query;
  std::string custom_guidance;
  std::string demo_agent_write_path;
  std::string demo_text;
  std::string cwd;
  std::string agent_home;
  std::string repository_root_hint;
  std::string workspace_root;
  std::vector<std::string> extension_args;
  bool no_extensions = false;
  bool emit_json = false;
  std::string usage_text;
};

int run_auxiliary_mode(const ReferenceAuxiliaryModeArgs& args);

}  // namespace qt::app
