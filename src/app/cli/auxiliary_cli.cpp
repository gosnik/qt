#include "qt/app/cli/auxiliary_cli.hpp"

#include <argparse/argparse.hpp>

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace qt::app {
namespace {

std::string env_or_empty(const char* name) {
  if (const char* value = std::getenv(name)) {
    return std::string(value);
  }
  return {};
}

std::string default_cwd() {
  return std::filesystem::current_path().generic_string();
}

std::string expand_home_path(const std::string& path) {
  if (path == "~") {
    const auto home = env_or_empty("HOME");
    return home.empty() ? path : home;
  }
  if (path.rfind("~/", 0) == 0) {
    const auto home = env_or_empty("HOME");
    return home.empty() ? path : home + path.substr(1);
  }
  return path;
}

std::string default_agent_home() {
  const auto env = env_or_empty("QT_CODING_AGENT_DIR");
  if (!env.empty()) {
    return expand_home_path(env);
  }
  const auto home = env_or_empty("HOME");
  return home.empty() ? ".qt" : home + "/.qt";
}

std::string default_demo_text() {
  return "demo output from qt\n";
}

void configure_auxiliary_cli_program(
    argparse::ArgumentParser& program) {
  program.add_description(
      "Minimal native harness shell for the in-progress qt port.");
  program.add_argument("--inspect-context")
      .help("inspect repository context discovery")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--inspect-context-resources")
      .help("inspect repository context discovery plus loaded file contents")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--inspect-prompt-assembly")
      .help("inspect loaded context resources plus assembled prompt sections")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--inspect-skills")
      .help("inspect discovered skill roots plus loaded SKILL.md entries")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--inspect-prompt-templates")
      .help("inspect discovered prompt template roots plus loaded prompt templates")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--inspect-slash-commands")
      .help("inspect built-in and discovered slash command surfaces")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--inspect-settings")
      .help("inspect merged app settings resolved from defaults plus settings files")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--inspect-app-resources")
      .help(
          "inspect deterministic app resource loading for prompts, skills, and themes")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--inspect-system-prompt")
      .help("inspect the assembled system prompt from loaded repository context")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--inspect-extensions")
      .help("inspect discovered extension manifests from extension roots")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--inspect-session")
      .help("inspect a JSONL session file")
      .default_value(std::string{})
      .metavar("PATH");
  program.add_argument("--resume-session")
      .help("resume a JSONL session file into metadata, entries, and branch snapshot")
      .default_value(std::string{})
      .metavar("PATH");
  program.add_argument("--inspect-session-catalog")
      .help("inspect a sessions directory and derived session graph")
      .default_value(std::string{})
      .metavar("PATH");
  program.add_argument("--create-session")
      .help("create a new persistent JSONL session repository")
      .default_value(std::string{})
      .metavar("PATH");
  program.add_argument("--fork-session")
      .help("fork a child session from an existing parent session JSONL file")
      .default_value(std::string{})
      .metavar("PATH");
  program.add_argument("--clone-session")
      .help("clone an existing session repository into a new repository path")
      .default_value(std::string{})
      .metavar("PATH");
  program.add_argument("--compact-session")
      .help("compact a session repository into retained metadata/label/summary plus a synthetic entry")
      .default_value(std::string{})
      .metavar("PATH");
  program.add_argument("--apply-session-compaction")
      .help("persist --compact-session output back to the session repository")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--inspect-extension-runners")
      .help("inspect extension runner launch plans and lifecycle scaffolds")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--child-session-path")
      .help("child session repository path used by --fork-session")
      .default_value(std::string{})
      .metavar("PATH");
  program.add_argument("--session-id")
      .help("session id used by --create-session and --fork-session")
      .default_value(std::string{})
      .metavar("ID");
  program.add_argument("--session-name")
      .help("optional display name used by session lifecycle modes")
      .default_value(std::string{})
      .metavar("TEXT");
  program.add_argument("--created-at")
      .help("optional ISO timestamp used by session lifecycle modes")
      .default_value(std::string{})
      .metavar("TEXT");
  program.add_argument("--child-working-directory")
      .help("optional working directory override used by --fork-session")
      .default_value(std::string{})
      .metavar("PATH");
  program.add_argument("--rpc-envelope")
      .help("load and normalize a JSON RPC request envelope")
      .default_value(std::string{})
      .metavar("PATH");
  program.add_argument("--rpc-frame")
      .help("load and decode a typed RPC command/response/event JSONL frame")
      .default_value(std::string{})
      .metavar("PATH");
  program.add_argument("--read-text")
      .help("execute the built-in read_text tool")
      .default_value(std::string{})
      .metavar("PATH");
  program.add_argument("--list-directory")
      .help("execute the built-in list_directory tool")
      .default_value(std::string{})
      .metavar("PATH");
  program.add_argument("--complete-path")
      .help("execute the built-in complete_path tool")
      .default_value(std::string{})
      .metavar("PATH");
  program.add_argument("--search-text")
      .help("execute the built-in search_text tool")
      .default_value(std::string{})
      .metavar("PATH");
  program.add_argument("--write-text")
      .help("execute the built-in write_text tool")
      .default_value(std::string{})
      .metavar("PATH");
  program.add_argument("--append-text")
      .help("execute the built-in append_text tool")
      .default_value(std::string{})
      .metavar("PATH");
  program.add_argument("--edit")
      .help("execute the built-in edit tool")
      .default_value(std::string{})
      .metavar("PATH");
  program.add_argument("--old-text")
      .help("old text payload used by --edit")
      .default_value(std::string{})
      .metavar("TEXT");
  program.add_argument("--new-text")
      .help("new text payload used by --edit")
      .default_value(std::string{})
      .metavar("TEXT");
  program.add_argument("--bash")
      .help("execute the built-in bash tool")
      .default_value(std::string{})
      .metavar("COMMAND");
  program.add_argument("--text")
      .help("text payload used by --write-text and --append-text")
      .default_value(std::string{})
      .metavar("TEXT");
  program.add_argument("--query")
      .help("query payload used by --search-text")
      .default_value(std::string{})
      .metavar("TEXT");
  program.add_argument("--custom-guidance")
      .help("optional extra guidance appended by --inspect-system-prompt")
      .default_value(std::string{})
      .metavar("TEXT");
  program.add_argument("--demo-agent-write")
      .help("run a fake single-turn agent flow that invokes write_text")
      .default_value(std::string{})
      .metavar("PATH");
  program.add_argument("--demo-text")
      .help("text payload used by --demo-agent-write")
      .default_value(default_demo_text())
      .metavar("TEXT");
  program.add_argument("--cwd")
      .help("working directory used by context discovery and tool execution")
      .default_value(default_cwd())
      .metavar("PATH");
  program.add_argument("--agent-home")
      .help("global agent home used for AGENTS/skills/prompts discovery")
      .default_value(default_agent_home())
      .metavar("PATH");
  program.add_argument("--repo-root-hint")
      .help("optional repository root hint for context discovery")
      .default_value(std::string{})
      .metavar("PATH");
  program.add_argument("--workspace-root")
      .help("workspace root enforced for tool execution")
      .default_value(std::string{})
      .metavar("PATH");
  program.add_argument("--extension")
      .help("explicit extension file or directory to load")
      .append()
      .default_value(std::vector<std::string>{})
      .metavar("PATH");
  program.add_argument("--no-extensions")
      .help("disable discovered and package-backed extension loading")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--log-file")
      .help("write structured runtime logs to a file")
      .default_value(std::vector<std::string>{})
      .append();
  program.add_argument("--log-stderr")
      .help("also write runtime logs to stderr")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--log-level")
      .help("log level: trace, debug, info, warn, error, critical, off")
      .default_value(std::string{});
  program.add_argument("--json").default_value(false).implicit_value(true);
}

std::vector<std::string> normalize_argv_like(
    const std::vector<std::string>& argv_like) {
  if (argv_like.empty()) {
    return {"qt"};
  }

  const std::string& first = argv_like.front();
  if (!first.empty() && first.front() != '-') {
    return argv_like;
  }

  std::vector<std::string> argv;
  argv.reserve(argv_like.size() + 1U);
  argv.push_back("qt");
  argv.insert(argv.end(), argv_like.begin(), argv_like.end());
  return argv;
}

ReferenceAuxiliaryModeArgs extract_auxiliary_mode_args(
    argparse::ArgumentParser& program) {
  ReferenceAuxiliaryModeArgs args;
  args.inspect_context = program.get<bool>("--inspect-context");
  args.inspect_context_resources =
      program.get<bool>("--inspect-context-resources");
  args.inspect_prompt_assembly =
      program.get<bool>("--inspect-prompt-assembly");
  args.inspect_skills = program.get<bool>("--inspect-skills");
  args.inspect_prompt_templates =
      program.get<bool>("--inspect-prompt-templates");
  args.inspect_slash_commands =
      program.get<bool>("--inspect-slash-commands");
  args.inspect_settings = program.get<bool>("--inspect-settings");
  args.inspect_app_resources = program.get<bool>("--inspect-app-resources");
  args.inspect_system_prompt = program.get<bool>("--inspect-system-prompt");
  args.inspect_extensions = program.get<bool>("--inspect-extensions");
  args.inspect_session_path = program.get<std::string>("--inspect-session");
  args.resume_session_path = program.get<std::string>("--resume-session");
  args.inspect_session_catalog_path =
      program.get<std::string>("--inspect-session-catalog");
  args.create_session_path = program.get<std::string>("--create-session");
  args.fork_session_path = program.get<std::string>("--fork-session");
  args.clone_session_path = program.get<std::string>("--clone-session");
  args.compact_session_path = program.get<std::string>("--compact-session");
  args.apply_session_compaction =
      program.get<bool>("--apply-session-compaction");
  args.inspect_extension_runners =
      program.get<bool>("--inspect-extension-runners");
  args.child_session_path = program.get<std::string>("--child-session-path");
  args.session_id = program.get<std::string>("--session-id");
  args.session_name = program.get<std::string>("--session-name");
  args.created_at = program.get<std::string>("--created-at");
  args.child_working_directory =
      program.get<std::string>("--child-working-directory");
  args.rpc_envelope_path = program.get<std::string>("--rpc-envelope");
  args.rpc_frame_path = program.get<std::string>("--rpc-frame");
  args.read_text_path = program.get<std::string>("--read-text");
  args.list_directory_path = program.get<std::string>("--list-directory");
  args.complete_path_prefix = program.get<std::string>("--complete-path");
  args.search_text_path = program.get<std::string>("--search-text");
  args.write_text_path = program.get<std::string>("--write-text");
  args.append_text_path = program.get<std::string>("--append-text");
  args.edit_path = program.get<std::string>("--edit");
  args.old_text = program.get<std::string>("--old-text");
  args.new_text = program.get<std::string>("--new-text");
  args.bash_command = program.get<std::string>("--bash");
  args.text = program.get<std::string>("--text");
  args.query = program.get<std::string>("--query");
  args.custom_guidance = program.get<std::string>("--custom-guidance");
  args.demo_agent_write_path =
      program.get<std::string>("--demo-agent-write");
  args.demo_text = program.get<std::string>("--demo-text");
  args.cwd = program.get<std::string>("--cwd");
  args.agent_home = program.get<std::string>("--agent-home");
  args.repository_root_hint = program.get<std::string>("--repo-root-hint");
  args.workspace_root = program.get<std::string>("--workspace-root");
  args.extension_args = program.get<std::vector<std::string>>("--extension");
  args.no_extensions = program.get<bool>("--no-extensions");
  args.emit_json = program.get<bool>("--json");
  return args;
}

std::string render_usage(argparse::ArgumentParser& program) {
  std::ostringstream usage_text;
  usage_text << program;
  return usage_text.str();
}

}  // namespace

ReferenceAuxiliaryCliParseResult parse_auxiliary_cli_args(
    const std::vector<std::string>& argv_like) {
  const std::vector<std::string> normalized = normalize_argv_like(argv_like);

  std::vector<const char*> argv;
  argv.reserve(normalized.size());
  for (const std::string& arg : normalized) {
    argv.push_back(arg.c_str());
  }

  return parse_auxiliary_cli_args(
      static_cast<int>(argv.size()), argv.data());
}

ReferenceAuxiliaryCliParseResult parse_auxiliary_cli_args(
    int argc, const char* const argv[]) {
  argparse::ArgumentParser program("qt");
  configure_auxiliary_cli_program(program);
  ReferenceAuxiliaryCliParseResult result;

  try {
    program.parse_args(argc, argv);
    result.ok = true;
    result.args = extract_auxiliary_mode_args(program);
  } catch (const std::exception& error) {
    result.error_text = std::string(error.what()) + '\n';
  }

  result.args.usage_text = render_usage(program);
  if (!result.ok) {
    result.error_text += result.args.usage_text;
  }

  return result;
}

}  // namespace qt::app
