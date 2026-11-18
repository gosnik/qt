#include "qt/tools/subagent_tool.hpp"
#include "qt/ai/images.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

using qt::tools::Json;
using qt::tools::SubagentDiscoveryOptions;
using qt::tools::SubagentSource;
using qt::tools::SubagentTool;
using qt::tools::SubagentToolOptions;
using qt::tools::ToolContext;
using qt::tools::ToolRequest;
using qt::tools::ToolResult;
using qt::tools::ToolResultStatus;

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void write_file(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  require(static_cast<bool>(output), "unable to create " + path.string());
  output << text;
}

void append_trace(const std::string& line) {
  const char* path = std::getenv("QT_SUBAGENT_PROBE_TRACE");
  if (path == nullptr || *path == '\0') {
    return;
  }
  const int descriptor = ::open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (descriptor < 0) {
    return;
  }
  const std::string record = line + "\n";
  (void)::write(descriptor, record.data(), record.size());
  (void)::close(descriptor);
}

int fake_child(int argc, char** argv) {
  std::string task;
  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument.rfind("Task: ", 0U) == 0U) {
      task = argument.substr(6U);
    }
  }
  append_trace("start " + task);
  const std::string marker = "sleep=";
  if (const auto offset = task.find(marker); offset != std::string::npos) {
    const auto end = task.find_first_not_of("0123456789", offset + marker.size());
    const auto value = task.substr(offset + marker.size(),
                                   end - (offset + marker.size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(std::stoll(value)));
  }
  if (task.find("fail") != std::string::npos) {
    std::cerr << "intentional child failure\n";
    append_trace("end " + task);
    return 7;
  }
  const Json message = {
      {"role", "assistant"},
      {"content", Json::array({Json{{"type", "text"},
                                     {"text", "reply(" + task + ")"}}})},
  };
  std::cout << Json{{"type", "agent_end"},
                    {"messages", Json::array({message})}}
                       .dump()
            << '\n';
  append_trace("end " + task);
  return 0;
}

ToolRequest request(Json arguments) {
  ToolRequest value;
  value.tool_name = "subagent";
  value.call_id = "probe-call";
  value.arguments = std::move(arguments);
  return value;
}

const Json& result_at(const ToolResult& result, std::size_t index) {
  require(result.output.contains("results") &&
              result.output.at("results").is_array() &&
              result.output.at("results").size() > index,
          "subagent result is missing child details");
  return result.output.at("results").at(index);
}

std::size_t maximum_trace_concurrency(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::string line;
  std::size_t active = 0U;
  std::size_t maximum = 0U;
  while (std::getline(input, line)) {
    if (line.rfind("start ", 0U) == 0U) {
      ++active;
      maximum = std::max(maximum, active);
    } else if (line.rfind("end ", 0U) == 0U) {
      require(active > 0U, "trace recorded an end without a start");
      --active;
    }
  }
  return maximum;
}

void create_agents(const std::filesystem::path& agent_home,
                   const std::filesystem::path& workspace) {
  write_file(agent_home / "agents" / "scout.md",
             "---\nname: scout\ndescription: User scout\n"
             "tools: read, grep\nmodel: probe/model\n"
             "inheritProjectContext: false\ninheritSkills: false\n"
             "maxSubagentDepth: 1\n---\nUser scout prompt.\n");
  write_file(agent_home / "agents" / "nested" / "helper.md",
             "---\nname: helper\ndescription: Nested helper\n"
             "systemPromptMode: append\n---\nNested prompt.\n");
  write_file(agent_home / "agents" / "disabled.md",
             "---\nname: disabled\ndescription: Disabled\n"
             "disabled: true\n---\nDisabled.\n");
  write_file(workspace / ".agents" / "legacy.md",
             "---\nname: legacy\ndescription: Legacy project agent\n"
             "---\nLegacy prompt.\n");
  write_file(workspace / ".pi" / "agents" / "scout.md",
             "---\nname: scout\ndescription: Project scout\n"
             "tools: ls bash\n---\nProject prompt.\n");
}

void test_discovery(const std::filesystem::path& agent_home,
                    const std::filesystem::path& workspace) {
  const auto user = qt::tools::discover_subagents(
      SubagentDiscoveryOptions{agent_home, workspace / "nested", "user"});
  require(user.agents.size() == 5U,
          "user discovery must include builtins, recurse, and skip disabled agents");
  const auto user_scout = std::find_if(
      user.agents.begin(), user.agents.end(),
      [](const auto& agent) { return agent.name == "scout"; });
  require(user_scout != user.agents.end() &&
              user_scout->source == SubagentSource::user,
          "user definitions must override builtins by runtime name");
  require(user_scout->tools == std::vector<std::string>({"read", "grep"}),
          "comma-separated tools were not parsed");
  require(user_scout->max_subagent_depth == 1U,
          "subagent depth frontmatter was not parsed");

  const auto both = qt::tools::discover_subagents(
      SubagentDiscoveryOptions{agent_home, workspace / "nested", "both"});
  const auto scout = std::find_if(
      both.agents.begin(), both.agents.end(),
      [](const auto& agent) { return agent.name == "scout"; });
  require(scout != both.agents.end() && scout->source == SubagentSource::project &&
              scout->description == "Project scout",
          "canonical project agent must override user and legacy definitions");
  require(both.project_agents_directory == workspace / ".pi" / "agents",
          "nearest canonical project agent directory was not reported");
}

void test_tool(const std::filesystem::path& executable,
               const std::filesystem::path& agent_home,
               const std::filesystem::path& workspace,
               const std::filesystem::path& trace) {
  SubagentToolOptions options;
  options.agent_home = agent_home;
  options.cwd = workspace;
  options.executable = executable.string();
  options.executable_arguments = {"--fake-child"};
  options.default_concurrency = 2U;
  options.max_parallel_tasks = 4U;
  options.max_subagent_depth = 2U;
  SubagentTool tool(std::move(options));

  ToolContext context;
  context.workspace_root = workspace;
  context.current_directory = workspace;
  std::size_t updates = 0U;
  context.update_callback = [&](const ToolResult& update) {
    require(update.output.value("running", false),
            "subagent update must identify running state");
    ++updates;
  };

  const auto listed = tool.invoke(request(Json{{"action", "list"}}), context);
  require(listed.status == ToolResultStatus::success &&
              listed.output.at("agents").size() == 5U,
          "action=list must return builtin and user agents without executing children");

  const auto invalid = tool.invoke(
      request(Json{{"agent", "scout"},
                   {"task", "one"},
                   {"tasks", Json::array({Json{{"agent", "scout"},
                                                {"task", "two"}}})}}),
      context);
  require(invalid.status == ToolResultStatus::invalid_request,
          "mixed execution modes must be rejected");
  const auto stray_mode_field = tool.invoke(
      request(Json{{"agent", "scout"},
                   {"tasks", Json::array({Json{{"agent", "helper"},
                                                {"task", "two"}}})}}),
      context);
  require(stray_mode_field.status == ToolResultStatus::invalid_request,
          "stray fields from another mode must be rejected");
  const auto invalid_concurrency = tool.invoke(
      request(Json{{"tasks", Json::array({Json{{"agent", "helper"},
                                                 {"task", "two"}}})},
                   {"concurrency", "many"}}),
      context);
  require(invalid_concurrency.status == ToolResultStatus::invalid_request,
          "invalid concurrency must fail before execution or approval");
  const auto invalid_scope = tool.invoke(
      request(Json{{"action", "list"}, {"agentScope", 3}}), context);
  require(invalid_scope.status == ToolResultStatus::invalid_request,
          "non-string agent scope must return a structured error");

  context.mutation_policy = qt::tools::ToolMutationPolicy::queue;
  context.mutation_queue = std::make_shared<qt::tools::ToolMutationQueue>();
  auto approval_request =
      request(Json{{"agent", "helper"}, {"task", "approved"}});
  const auto pending = tool.invoke(approval_request, context);
  require(pending.status == ToolResultStatus::pending_approval &&
              pending.output.value("queued", false),
          "subagent delegation must honor the parent mutation policy");
  approval_request.approved_mutation_id =
      pending.output.at("mutation_id").get<std::string>();
  require(context.mutation_queue->approve(*approval_request.approved_mutation_id),
          "unable to approve subagent mutation fixture");
  const auto approved = tool.invoke(approval_request, context);
  require(approved.status == ToolResultStatus::success &&
              result_at(approved, 0).at("output") == "reply(approved)",
          "approved subagent delegation did not execute");
  context.mutation_policy = qt::tools::ToolMutationPolicy::execute;
  context.mutation_queue.reset();

  const auto single = tool.invoke(
      request(Json{{"agent", "scout"}, {"task", "single"}}), context);
  require(single.status == ToolResultStatus::success &&
              single.output.at("mode") == "single" &&
              result_at(single, 0).at("output") == "reply(single)" &&
              result_at(single, 0).at("messages").size() == 1U &&
              !single.output.at("run_id").get<std::string>().empty(),
          "single child execution did not preserve output and identity");

  std::filesystem::remove(trace);
  const Json parallel_tasks = Json::array(
      {Json{{"agent", "helper"}, {"task", "sleep=120 first"}},
       Json{{"agent", "helper"}, {"task", "sleep=120 second"}},
       Json{{"agent", "helper"}, {"task", "sleep=120 third"}}});
  const auto parallel = tool.invoke(
      request(Json{{"tasks", parallel_tasks}, {"concurrency", 2}}), context);
  require(parallel.status == ToolResultStatus::success &&
              parallel.output.at("completed") == 3U &&
              result_at(parallel, 0).at("task") == "sleep=120 first" &&
              result_at(parallel, 1).at("task") == "sleep=120 second" &&
              result_at(parallel, 2).at("task") == "sleep=120 third",
          "parallel results must retain source order");
  require(maximum_trace_concurrency(trace) == 2U,
          "parallel worker pool did not enforce requested concurrency");

  const auto chain = tool.invoke(
      request(Json{{"chain",
                    Json::array({Json{{"agent", "helper"},
                                           {"task", "first"}},
                                 Json{{"agent", "helper"},
                                      {"task", "got {previous}"}}})}}),
      context);
  require(chain.status == ToolResultStatus::success &&
              result_at(chain, 1).at("task") == "got reply(first)" &&
              result_at(chain, 1).at("output") ==
                  "reply(got reply(first))",
          "chain did not substitute the previous child output");

  const auto partial = tool.invoke(
      request(Json{{"tasks",
                    Json::array({Json{{"agent", "helper"}, {"task", "ok"}},
                                 Json{{"agent", "helper"}, {"task", "fail"}}})}}),
      context);
  require(partial.status == ToolResultStatus::success &&
              partial.output.at("completed") == 1U &&
              result_at(partial, 1).at("exit_code") == 7,
          "parallel child failure must remain an individual ordered result");

  const auto gated = tool.invoke(
      request(Json{{"agent", "scout"},
                   {"task", "project"},
                   {"agentScope", "both"}}),
      context);
  require(gated.status == ToolResultStatus::permission_denied,
          "project agents must require an explicit trust bypass");
  const auto trusted = tool.invoke(
      request(Json{{"agent", "scout"},
                   {"task", "project"},
                   {"agentScope", "both"},
                   {"confirmProjectAgents", false}}),
      context);
  require(trusted.status == ToolResultStatus::success &&
              result_at(trusted, 0).at("agent_source") == "project",
          "trusted project agent did not execute");

  const auto escaped = tool.invoke(
      request(Json{{"agent", "helper"},
                   {"task", "escape"},
                   {"cwd", workspace.parent_path().string()}}),
      context);
  require(escaped.status == ToolResultStatus::execution_error &&
              result_at(escaped, 0).at("error") ==
                  "subagent cwd escapes workspace root",
          "subagent cwd must remain inside the workspace");
  context.allow_external_filesystem_access = true;
  const auto external_allowed = tool.invoke(
      request(Json{{"agent", "helper"},
                   {"task", "external allowed"},
                   {"cwd", workspace.parent_path().string()}}),
      context);
  require(external_allowed.status == ToolResultStatus::success &&
              result_at(external_allowed, 0).at("output") ==
                  "reply(external allowed)",
          "external filesystem access must allow an outside subagent cwd");
  context.allow_external_filesystem_access = false;

  require(::setenv("QT_SUBAGENT_DEPTH", "2", 1) == 0,
          "unable to set recursion fixture");
  const auto depth_blocked = tool.invoke(
      request(Json{{"agent", "helper"}, {"task", "too deep"}}), context);
  require(::unsetenv("QT_SUBAGENT_DEPTH") == 0,
          "unable to clear recursion fixture");
  require(depth_blocked.status == ToolResultStatus::permission_denied,
          "subagent recursion depth was not enforced");

  auto token = std::make_shared<qt::foundation::ProcessCancellationToken>();
  context.cancellation_token = token;
  std::thread cancel([token]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    token->cancel();
  });
  const auto cancelled = tool.invoke(
      request(Json{{"agent", "helper"},
                   {"task", "sleep=3000 cancel"}}),
      context);
  cancel.join();
  require(cancelled.status == ToolResultStatus::execution_error &&
              result_at(cancelled, 0).at("cancelled").get<bool>(),
          "parent cancellation did not terminate the child process tree");
  require(updates > 0U, "subagent execution did not emit progress updates");
}

}  // namespace

namespace qt::ai {
std::optional<ImageDataUrl> parse_image_data_url(const std::string&) {
  return std::nullopt;
}
}  // namespace qt::ai

int main(int argc, char** argv) {
  for (int index = 1; index < argc; ++index) {
    if (std::string(argv[index]) == "--fake-child") {
      return fake_child(argc, argv);
    }
  }

  try {
    require(argc == 2, "usage: subagent_probe <temporary-directory>");
    const std::filesystem::path root = std::filesystem::absolute(argv[1]);
    std::filesystem::remove_all(root);
    const auto agent_home = root / "agent-home";
    const auto workspace = root / "workspace";
    const auto trace = root / "trace.log";
    std::filesystem::create_directories(workspace / "nested");
    create_agents(agent_home, workspace);
    require(::setenv("QT_SUBAGENT_PROBE_TRACE", trace.c_str(), 1) == 0,
            "unable to configure trace fixture");

    test_discovery(agent_home, workspace);
    test_tool(std::filesystem::absolute(argv[0]), agent_home, workspace, trace);
    std::cout << "subagent probe passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "subagent probe failed: " << error.what() << '\n';
    return 1;
  }
}
