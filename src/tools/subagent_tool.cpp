#include "qt/tools/subagent_tool.hpp"

#include "qt/foundation/frontmatter.hpp"
#include "qt/tools/tool_validation.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <unistd.h>

namespace qt::tools {
namespace {

constexpr std::size_t kMaximumConcurrency = 32U;

std::string model_tool_name(const std::string& name) {
    if (name == "read_text") return "read";
    if (name == "write_text") return "write";
    if (name == "search_text") return "grep";
    if (name == "list_directory") return "ls";
    return name;
}

std::string trim_copy(std::string_view text) {
    std::size_t begin = 0U;
    while (begin < text.size() &&
           std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1U])) != 0) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

std::string lowercase_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool parse_boolean(std::string value, bool fallback) {
    value = lowercase_copy(trim_copy(value));
    if (value == "true" || value == "yes" || value == "1") {
        return true;
    }
    if (value == "false" || value == "no" || value == "0") {
        return false;
    }
    return fallback;
}

std::vector<std::string> split_list(std::string_view value) {
    std::vector<std::string> result;
    std::string item;
    const auto flush = [&]() {
        const std::string trimmed = trim_copy(item);
        if (!trimmed.empty() &&
            std::find(result.begin(), result.end(), trimmed) == result.end()) {
            result.push_back(trimmed);
        }
        item.clear();
    };
    for (const char ch : value) {
        if (ch == ',' || std::isspace(static_cast<unsigned char>(ch)) != 0) {
            flush();
        } else {
            item.push_back(ch);
        }
    }
    flush();
    return result;
}

std::string document_body(std::string content) {
    for (std::size_t index = 0U; index < content.size(); ++index) {
        if (content[index] == '\r') {
            if (index + 1U < content.size() && content[index + 1U] == '\n') {
                content.erase(index, 1U);
            } else {
                content[index] = '\n';
            }
        }
    }
    if (content.rfind("---\n", 0U) != 0U) {
        return trim_copy(content);
    }
    const std::size_t closing = content.find("\n---", 4U);
    if (closing == std::string::npos) {
        return trim_copy(content);
    }
    std::size_t body = closing + 4U;
    if (body < content.size() && content[body] == '\n') {
        ++body;
    }
    return trim_copy(std::string_view(content).substr(body));
}

std::optional<std::size_t> parse_nonnegative_size(const std::string& value) {
    try {
        std::size_t consumed = 0U;
        const unsigned long long parsed = std::stoull(trim_copy(value), &consumed);
        if (consumed != trim_copy(value).size()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

void load_agent_directory(const qt::foundation::Path& directory,
                          SubagentSource source,
                          std::map<std::string, SubagentDefinition>& agents,
                          std::vector<std::string>& diagnostics) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error) {
        return;
    }

    std::filesystem::recursive_directory_iterator iterator(
        directory, std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        const auto entry = *iterator;
        iterator.increment(error);
        if (error) {
            diagnostics.push_back("unable to inspect subagent directory '" +
                                  directory.generic_string() + "': " +
                                  error.message());
            break;
        }
        std::error_code type_error;
        if (!entry.is_regular_file(type_error) || type_error ||
            entry.path().extension() != ".md" ||
            entry.path().filename().string().find(".chain.md") !=
                std::string::npos) {
            continue;
        }

        std::ifstream input(entry.path());
        if (!input) {
            diagnostics.push_back("unable to read subagent definition '" +
                                  entry.path().generic_string() + "'");
            continue;
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        const std::string content = buffer.str();
        const auto frontmatter = qt::foundation::parse_frontmatter(content);
        if (!frontmatter.had_frontmatter || !frontmatter.complete) {
            diagnostics.push_back("ignored invalid subagent definition '" +
                                  entry.path().generic_string() + "'");
            continue;
        }
        const auto name = frontmatter.fields.find("name");
        const auto description = frontmatter.fields.find("description");
        if (name == frontmatter.fields.end() || trim_copy(name->second).empty() ||
            description == frontmatter.fields.end() ||
            trim_copy(description->second).empty()) {
            diagnostics.push_back(
                "ignored subagent definition without name and description '" +
                entry.path().generic_string() + "'");
            continue;
        }

        SubagentDefinition agent;
        agent.name = trim_copy(name->second);
        if (const auto package = frontmatter.fields.find("package");
            package != frontmatter.fields.end() &&
            !trim_copy(package->second).empty()) {
            agent.name = trim_copy(package->second) + "." + agent.name;
        }
        agent.description = trim_copy(description->second);
        agent.system_prompt = document_body(content);
        agent.source = source;
        agent.file_path = entry.path();
        if (const auto field = frontmatter.fields.find("tools");
            field != frontmatter.fields.end()) {
            agent.tools = split_list(field->second);
        }
        if (const auto field = frontmatter.fields.find("model");
            field != frontmatter.fields.end() && !trim_copy(field->second).empty()) {
            agent.model = trim_copy(field->second);
        }
        if (const auto field = frontmatter.fields.find("thinking");
            field != frontmatter.fields.end() && !trim_copy(field->second).empty()) {
            agent.thinking = trim_copy(field->second);
        }
        if (const auto field = frontmatter.fields.find("systemPromptMode");
            field != frontmatter.fields.end()) {
            const std::string mode = lowercase_copy(trim_copy(field->second));
            if (mode == "append" || mode == "replace") {
                agent.system_prompt_mode = mode;
            }
        } else if (agent.name == "delegate") {
            agent.system_prompt_mode = "append";
        }
        if (const auto field = frontmatter.fields.find("inheritProjectContext");
            field != frontmatter.fields.end()) {
            agent.inherit_project_context = parse_boolean(field->second, false);
        } else if (agent.name == "delegate") {
            agent.inherit_project_context = true;
        }
        if (const auto field = frontmatter.fields.find("inheritSkills");
            field != frontmatter.fields.end()) {
            agent.inherit_skills = parse_boolean(field->second, false);
        }
        for (const char* key : {"maxSubagentDepth", "maxDepth"}) {
            if (const auto field = frontmatter.fields.find(key);
                field != frontmatter.fields.end()) {
                agent.max_subagent_depth = parse_nonnegative_size(field->second);
                break;
            }
        }
        if (const auto field = frontmatter.fields.find("disabled");
            field != frontmatter.fields.end()) {
            agent.disabled = parse_boolean(field->second, false);
        }
        agents[agent.name] = std::move(agent);
    }
}

struct ProjectAgentDirectories {
    std::optional<qt::foundation::Path> display_directory;
    std::vector<qt::foundation::Path> load_order;
};

std::vector<SubagentDefinition> builtin_subagents() {
    const auto make_leaf = [](std::string name, std::string description,
                              std::vector<std::string> tools,
                              std::string prompt) {
        SubagentDefinition agent;
        agent.name = std::move(name);
        agent.description = std::move(description);
        agent.tools = std::move(tools);
        agent.system_prompt = std::move(prompt);
        agent.max_subagent_depth = 0U;
        agent.source = SubagentSource::builtin;
        return agent;
    };
    return {
        make_leaf(
            "scout", "Map code and return concise evidence for a handoff",
            {"read", "symbol", "analyze_file", "references", "build_context",
             "search_index", "index", "grep", "find", "ls", "bash"},
            "You are a read-only scouting agent. Locate the relevant code with "
            "the symbol index for C/C++ identifiers and targeted text searches "
            "for literal content, follow callers and tests, verify every material "
            "claim, and return concise findings with exact file paths. Do not "
            "modify the repository."),
        make_leaf(
            "planner", "Design an implementation plan without changing files",
            {"read", "symbol", "analyze_file", "references", "build_context",
             "search_index", "index", "grep", "find", "ls"},
            "You are a planning agent. Inspect the actual repository, identify "
            "owners and constraints, compare viable approaches, and return an "
            "ordered implementation and verification plan. Do not modify files."),
        make_leaf(
            "reviewer", "Review correctness, safety, and verification gaps",
            {"read", "symbol", "analyze_file", "references", "build_context",
             "search_index", "index", "grep", "find", "ls", "bash"},
            "You are a review-only agent. Inspect the requested code and current "
            "diff, run focused read-only checks when useful, and report actionable "
            "findings ordered by severity with exact file paths. Do not edit files."),
        make_leaf(
            "worker", "Implement one bounded task and verify the result",
            {"read", "symbol", "analyze_file", "references", "build_context",
             "search_index", "index", "grep", "find", "ls", "bash", "edit",
             "write", "apply_patch"},
            "You are the single implementation worker for the delegated task. "
            "Inspect before editing, make a narrow production-ready change, follow "
            "repository instructions, run proportionate verification, and report "
            "the completed result and any remaining blocker."),
    };
}

ProjectAgentDirectories find_project_agent_directories(
    qt::foundation::Path cwd) {
    std::error_code error;
    cwd = std::filesystem::absolute(std::move(cwd), error);
    if (error) {
        return {};
    }
    while (!cwd.empty()) {
        const auto canonical = cwd / ".pi" / "agents";
        const auto legacy = cwd / ".agents";
        const bool has_canonical = std::filesystem::is_directory(canonical, error);
        error.clear();
        const bool has_legacy = std::filesystem::is_directory(legacy, error);
        error.clear();
        if (has_canonical || has_legacy) {
            ProjectAgentDirectories directories;
            directories.display_directory = has_canonical ? canonical : legacy;
            if (has_legacy) {
                directories.load_order.push_back(legacy);
            }
            if (has_canonical) {
                directories.load_order.push_back(canonical);
            }
            return directories;
        }
        const auto parent = cwd.parent_path();
        if (parent == cwd) {
            break;
        }
        cwd = parent;
    }
    return {};
}

std::size_t environment_depth(const char* name, std::size_t fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    const auto parsed = parse_nonnegative_size(value);
    return parsed.value_or(fallback);
}

std::string make_run_id() {
    static std::atomic<std::uint64_t> sequence{1U};
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    return "subagent-" + std::to_string(::getpid()) + "-" +
           std::to_string(millis) + "-" +
           std::to_string(sequence.fetch_add(1U));
}

std::string replace_all(std::string text, std::string_view needle,
                        std::string_view replacement) {
    if (needle.empty()) {
        return text;
    }
    std::size_t offset = 0U;
    while ((offset = text.find(needle, offset)) != std::string::npos) {
        text.replace(offset, needle.size(), replacement);
        offset += replacement.size();
    }
    return text;
}

struct RequestedTask {
    std::string agent;
    std::string task;
    std::optional<std::string> cwd;
    std::optional<std::string> model;
};

struct ChildResult {
    Json json = Json::object();
    bool success = false;
    std::string output;
};

}  // namespace

const char* to_string(SubagentSource source) noexcept {
    switch (source) {
        case SubagentSource::builtin:
            return "builtin";
        case SubagentSource::user:
            return "user";
        case SubagentSource::project:
            return "project";
    }
    return "user";
}

SubagentDiscoveryResult discover_subagents(
    const SubagentDiscoveryOptions& options) {
    SubagentDiscoveryResult result;
    std::map<std::string, SubagentDefinition> agents;
    const std::string scope = lowercase_copy(trim_copy(options.scope));
    if (scope != "user" && scope != "project" && scope != "both") {
        result.diagnostics.push_back("invalid subagent scope '" + options.scope +
                                     "'");
        return result;
    }
    for (auto& agent : builtin_subagents()) {
        agents[agent.name] = std::move(agent);
    }
    if (scope != "project") {
        load_agent_directory(options.agent_home / "agents", SubagentSource::user,
                             agents, result.diagnostics);
    }
    const auto project = find_project_agent_directories(options.cwd);
    result.project_agents_directory = project.display_directory;
    if (scope != "user") {
        for (const auto& directory : project.load_order) {
            load_agent_directory(directory, SubagentSource::project, agents,
                                 result.diagnostics);
        }
    }
    result.agents.reserve(agents.size());
    for (auto& [name, agent] : agents) {
        (void)name;
        if (!agent.disabled) {
            result.agents.push_back(std::move(agent));
        }
    }
    return result;
}

SubagentTool::SubagentTool(SubagentToolOptions options)
    : options_(std::move(options)),
      runner_(qt::app::runtime::IsolatedAgentRunnerOptions{
          options_.executable.empty() ? std::string("qt")
                                      : options_.executable,
          options_.executable_arguments}) {
    options_.default_concurrency =
        std::max<std::size_t>(1U, std::min(options_.default_concurrency,
                                           kMaximumConcurrency));
    options_.max_parallel_tasks =
        std::max<std::size_t>(1U, options_.max_parallel_tasks);
    descriptor_.name = "subagent";
    descriptor_.description =
        "Delegate work to isolated named child agents. Use exactly one execution "
        "mode: single {agent, task}, ordered parallel {tasks}, or sequential "
        "{chain}; action=list discovers configured agents. User agents are loaded "
        "from the global agents directory. Project agents are repo-controlled and "
        "require explicit scope and trust confirmation bypass.";
    descriptor_.deterministic = false;
    // A child can invoke mutating tools. Treat the delegation as one mutation
    // boundary so it is serialized and cannot bypass the parent's approval mode.
    descriptor_.destructive = true;
    descriptor_.local_only = true;
    const Json task_schema = {
        {"type", "object"},
        {"properties",
         {{"agent",
           {{"type", "string"},
            {"description", "Configured agent runtime name."}}},
          {"task",
           {{"type", "string"},
            {"description", "Task delegated to this child."}}},
          {"cwd",
           {{"type", "string"},
            {"description", "Child cwd inside the parent workspace."}}},
          {"model",
           {{"type", "string"},
            {"description", "Optional model override for this child."}}}}},
        {"required", Json::array({"agent", "task"})},
        {"additionalProperties", false},
    };
    descriptor_.input_schema = Json{
        {"type", "object"},
        {"properties",
         {{"action",
           {{"type", "string"},
            {"enum", options_.coordinator
                         ? Json::array({"list", "spawn", "send", "follow_up",
                                        "wait", "interrupt", "list_workers"})
                         : Json::array({"list"})},
            {"description", "Use list to inspect executable agent definitions."}}},
          {"agent",
           {{"type", "string"},
            {"description", "Agent runtime name for single mode."}}},
          {"task",
           {{"type", "string"},
            {"description",
             "Single-mode task, or original {task} value for a chain."}}},
          {"tasks",
           {{"type", "array"},
            {"items", task_schema},
            {"minItems", 1},
            {"maxItems", options_.max_parallel_tasks},
            {"description", "Parallel tasks; results retain this source order."}}},
          {"chain",
           {{"type", "array"},
            {"items", task_schema},
            {"minItems", 1},
            {"description",
             "Sequential tasks. Templates may use {previous} and {task}."}}},
          {"concurrency",
           {{"type", "integer"},
            {"minimum", 1},
            {"description", "Parallel worker limit; default 4."}}},
          {"agentScope",
           {{"type", "string"},
            {"enum", Json::array({"user", "project", "both"})},
            {"default", "user"},
            {"description", "Discovery scope; project roles are repo-controlled."}}},
          {"confirmProjectAgents",
           {{"type", "boolean"},
            {"default", true},
            {"description",
             "Keep true until project role files have been inspected and trusted."}}},
          {"cwd",
           {{"type", "string"},
            {"description", "Default child cwd inside the parent workspace."}}},
          {"model",
           {{"type", "string"},
            {"description", "Default child model override."}}}}},
        {"additionalProperties", false},
    };
    if (options_.coordinator) {
        auto& properties = descriptor_.input_schema["properties"];
        properties["name"] = Json{{"type", "string"}};
        properties["fork"] = Json{
            {"oneOf", Json::array({Json{{"type", "string"}, {"enum", Json::array({"none", "all"})}},
                                    Json{{"type", "integer"}, {"minimum", 1}}})},
            {"description", "none, all, or a positive user-turn count."}};
        properties["target"] = Json{{"type", "string"}};
        properties["message"] = Json{{"type", "string"}};
        properties["timeout_ms"] = Json{{"type", "integer"}, {"minimum", 0}, {"maximum", 60000}};
        properties["wake"] = Json{{"type", "boolean"}};
    }
}

const ToolDescriptor& SubagentTool::descriptor() const { return descriptor_; }

ToolResult SubagentTool::invoke(const ToolRequest& request,
                                const ToolContext& context) const {
    if (!is_valid(request) || request.tool_name != descriptor_.name) {
        return make_invalid_request("invalid subagent tool request", request);
    }
    if (!has_only_arguments(request,
                            {"action", "agent", "task", "tasks", "chain",
                             "concurrency", "agentScope", "confirmProjectAgents",
                             "cwd", "model", "name", "fork", "target", "message",
                             "timeout_ms", "wake"})) {
        return make_invalid_request("subagent received unsupported arguments",
                                    request);
    }

    if (request.arguments.contains("agentScope") &&
        !request.arguments.at("agentScope").is_string()) {
        return make_invalid_request("subagent agentScope must be a string",
                                    request);
    }
    if (request.arguments.contains("confirmProjectAgents") &&
        !request.arguments.at("confirmProjectAgents").is_boolean()) {
        return make_invalid_request(
            "subagent confirmProjectAgents must be a boolean", request);
    }
    if (request.arguments.contains("task") &&
        !request.arguments.at("task").is_string()) {
        return make_invalid_request("subagent task must be a string", request);
    }
    for (const char* key : {"name", "fork", "target", "message"}) {
        if (request.arguments.contains(key) &&
            !request.arguments.at(key).is_string() &&
            !(std::string(key) == "fork" && request.arguments.at(key).is_number_integer())) {
            return make_invalid_request(std::string("subagent ") + key + " must be a string", request);
        }
    }
    if (request.arguments.contains("model") &&
        !request.arguments.at("model").is_string()) {
        return make_invalid_request("subagent model must be a string", request);
    }
    if (request.arguments.contains("wake") &&
        !request.arguments.at("wake").is_boolean()) {
        return make_invalid_request("subagent wake must be a boolean", request);
    }
    if (request.arguments.contains("timeout_ms") &&
        !request.arguments.at("timeout_ms").is_number_integer()) {
        return make_invalid_request("subagent timeout_ms must be an integer", request);
    }
    const std::string scope = request.arguments.contains("agentScope")
                                  ? request.arguments.at("agentScope").get<std::string>()
                                  : std::string("user");
    if (scope != "user" && scope != "project" && scope != "both") {
        return make_invalid_request(
            "agentScope must be user, project, or both", request);
    }
    const auto discovery = discover_subagents(
        SubagentDiscoveryOptions{options_.agent_home, options_.cwd, scope});
    const std::size_t depth = environment_depth("QT_SUBAGENT_DEPTH", 0U);
    const std::size_t configured_max_depth = environment_depth(
        "QT_SUBAGENT_MAX_DEPTH", options_.max_subagent_depth);
    std::map<std::string, const SubagentDefinition*> agents;
    for (const auto& agent : discovery.agents) agents[agent.name] = &agent;

    if (request.arguments.contains("action")) {
        if (!request.arguments.at("action").is_string()) {
            return make_invalid_request("subagent action must be a string", request);
        }
        const auto action = request.arguments.at("action").get<std::string>();
        if (action != "list" && action != "spawn" && action != "send" &&
            action != "follow_up" && action != "wait" && action != "interrupt" &&
            action != "list_workers") {
            return make_invalid_request("unsupported subagent action", request);
        }
        if ((action == "spawn" || action == "send" || action == "follow_up" ||
             action == "wait" || action == "interrupt" || action == "list_workers") &&
            !options_.coordinator) {
            return make_execution_error(ToolResultStatus::execution_error,
                                        "persistent subagent coordinator is unavailable",
                                        request);
        }
        if (request.arguments.size() != 1U &&
            !(request.arguments.size() == 2U &&
              request.arguments.contains("agentScope"))) {
            if (action == "list") {
                return make_invalid_request(
                    "subagent action=list accepts only agentScope", request);
            }
        }
        if (action == "list") {
            Json agents = Json::array();
            for (const auto& agent : discovery.agents) {
                agents.push_back(Json{{"name", agent.name},
                                      {"description", agent.description},
                                      {"source", to_string(agent.source)},
                                      {"model", agent.model.value_or("")},
                                      {"tools", agent.tools},
                                      {"file", agent.file_path.generic_string()}});
            }
            ToolResult result;
            result.call_id = request.call_id;
            result.output = Json{{"mode", "management"},
                                 {"action", "list"},
                                 {"agent_scope", scope},
                                 {"agents", std::move(agents)},
                                 {"diagnostics", discovery.diagnostics}};
            result.output["output"] = result.output.at("agents").empty()
                                          ? "No subagents configured."
                                          : "Discovered " +
                                                std::to_string(
                                                    result.output.at("agents").size()) +
                                                " subagents.";
            return result;
        }

        const auto persistent_error = [&](std::string message,
                                          ToolResultStatus status = ToolResultStatus::invalid_request) {
            return make_execution_error(status, std::move(message), request);
        };
        const auto require_string = [&](const char* key) -> std::optional<std::string> {
            if (!request.arguments.contains(key) ||
                !request.arguments.at(key).is_string() ||
                trim_copy(request.arguments.at(key).get<std::string>()).empty()) {
                return std::nullopt;
            }
            return request.arguments.at(key).get<std::string>();
        };
        const auto reject_unexpected = [&](std::initializer_list<const char*> keys) -> std::optional<ToolResult> {
            if (!has_only_arguments(request, keys)) {
                return persistent_error("unsupported arguments for persistent subagent action");
            }
            return std::nullopt;
        };
        if (action == "list_workers") {
            if (auto error = reject_unexpected({"action"})) return *error;
            auto result = options_.coordinator->list();
            if (!result) return persistent_error(result.error().summary(), ToolResultStatus::execution_error);
            ToolResult output;
            output.call_id = request.call_id;
            output.output = result.value();
            return output;
        } else if (action == "send" || action == "follow_up") {
            if (auto error = reject_unexpected({"action", "target", "message", "task", "wake"})) return *error;
            const auto target = require_string("target");
            const char* payload_key = action == "send" ? "message" : "task";
            const auto payload = require_string(payload_key);
            if (!target || !payload || (action == "send" && request.arguments.contains("task")) ||
                (action == "follow_up" && request.arguments.contains("message"))) {
                return persistent_error(std::string("subagent ") + action +
                                        " requires target and " + payload_key);
            }
            if (action == "send" || action == "follow_up") {
                if (auto queued = require_mutation_approval(
                        ToolMutationKind::command, "message persistent subagent '" + *target + "'",
                        request, context)) return *queued;
            }
            const auto workers = options_.coordinator->list();
            if (!workers) return persistent_error(workers.error().summary(), ToolResultStatus::execution_error);
            const auto current_workspace = context.workspace_root
                                                ? qt::foundation::canonicalize_path(*context.workspace_root)
                                                : qt::foundation::Result<qt::foundation::Path>::success(
                                                      std::filesystem::current_path());
            if (!current_workspace) return persistent_error(current_workspace.error().summary(), ToolResultStatus::execution_error);
            bool found_worker = false;
            for (const auto& worker : workers.value().value("children", Json::array())) {
                if (!worker.is_object() || (worker.value("id", "") != *target && worker.value("name", "") != *target)) continue;
                found_worker = true;
                const auto permissions = worker.value("permissions", Json::object());
                if (permissions.value("workspace", "") != current_workspace.value().generic_string() ||
                    (permissions.value("allow_external", false) && !context.allow_external_filesystem_access)) {
                    return persistent_error("persistent worker permissions exceed current tool context",
                                            ToolResultStatus::permission_denied);
                }
                const char* active_key = context.metadata.contains("active_tools")
                                             ? "active_tools" : "active_tool_names";
                if (context.metadata.contains(active_key) &&
                    context.metadata.at(active_key).is_array() &&
                    permissions.contains("toolnames") && permissions.at("toolnames").is_array()) {
                    for (const auto& tool : permissions.at("toolnames")) {
                        if (!std::any_of(context.metadata.at(active_key).begin(),
                                         context.metadata.at(active_key).end(),
                                         [&](const Json& current) { return current.is_string() && tool.is_string() &&
                                             model_tool_name(current.get<std::string>()) == model_tool_name(tool.get<std::string>()); })) {
                            return persistent_error("persistent worker tool permissions exceed current tool context",
                                                    ToolResultStatus::permission_denied);
                        }
                    }
                }
                break;
            }
            if (!found_worker) return persistent_error("unknown child: " + *target, ToolResultStatus::not_found);
            auto result = options_.coordinator->send(
                *target, *payload,
                action == "follow_up" || request.arguments.value("wake", false));
            if (!result) return persistent_error(result.error().summary(), ToolResultStatus::execution_error);
            ToolResult output;
            output.call_id = request.call_id;
            output.output = result.value();
            return output;
        } else if (action == "wait" || action == "interrupt") {
            if (auto error = reject_unexpected({"action", "target", "timeout_ms"})) return *error;
            const auto target = require_string("target");
            if (!target) return persistent_error(std::string("subagent ") + action + " requires target");
            if (action == "interrupt") {
                if (request.arguments.contains("timeout_ms")) return persistent_error("interrupt does not accept timeout_ms");
                auto result = options_.coordinator->interrupt(*target);
                if (!result) return persistent_error(result.error().summary(), ToolResultStatus::execution_error);
                ToolResult output; output.call_id = request.call_id; output.output = result.value(); return output;
            }
            long long timeout = 0;
            if (request.arguments.contains("timeout_ms")) {
                if (!request.arguments.at("timeout_ms").is_number_integer()) return persistent_error("timeout_ms must be an integer");
                timeout = request.arguments.at("timeout_ms").get<long long>();
                if (timeout < 0 || timeout > 60000) return persistent_error("timeout_ms must be between 0 and 60000");
            }
            auto result = options_.coordinator->wait(*target, std::chrono::milliseconds(timeout));
            if (!result) return persistent_error(result.error().summary(), ToolResultStatus::execution_error);
            ToolResult output; output.call_id = request.call_id; output.output = result.value(); return output;
        } else if (action == "spawn") {
            if (auto error = reject_unexpected({"action", "agent", "task", "name", "fork", "agentScope",
                                                 "confirmProjectAgents", "cwd", "model"})) return *error;
            const auto agent_name = require_string("agent");
            const auto task_text = require_string("task");
            if (!agent_name || !task_text) return persistent_error("spawn requires non-empty agent and task");
            const auto found = agents.find(*agent_name);
            if (found == agents.end()) return persistent_error("unknown subagent '" + *agent_name + "'");
            std::vector<std::string> allowed_tools = found->second->tools;
            for (auto& tool : allowed_tools) tool = model_tool_name(tool);
            const Json* active_tools = nullptr;
            if (context.metadata.contains("active_tools") &&
                context.metadata.at("active_tools").is_array()) {
                active_tools = &context.metadata.at("active_tools");
                allowed_tools.erase(
                    std::remove_if(allowed_tools.begin(), allowed_tools.end(),
                                   [&](const std::string& tool) {
                                       return std::none_of(
                                           active_tools->begin(), active_tools->end(),
                                           [&](const Json& current) {
                                               return current.is_string() &&
                                                      model_tool_name(current.get<std::string>()) == tool;
                                           });
                                   }),
                    allowed_tools.end());
            }
            if (found->second->source == SubagentSource::project &&
                request.arguments.value("confirmProjectAgents", true)) {
                return persistent_error("project subagent '" + *agent_name + "' requires trust confirmation",
                                        ToolResultStatus::permission_denied);
            }
            const std::size_t child_max_depth = found->second->max_subagent_depth
                                                    ? std::min(configured_max_depth, *found->second->max_subagent_depth)
                                                    : configured_max_depth;
            if (depth >= configured_max_depth) {
                return persistent_error(
                    "subagent depth limit reached at depth " + std::to_string(depth) +
                        " (max " + std::to_string(configured_max_depth) + ")",
                    ToolResultStatus::permission_denied);
            }
            if (auto queued = require_mutation_approval(
                    ToolMutationKind::command, "spawn persistent subagent '" + *agent_name + "'",
                    request, context)) return *queued;
            qt::foundation::Path workspace = context.workspace_root.value_or(
                options_.cwd.empty() ? std::filesystem::current_path() : options_.cwd);
            const auto canonical_workspace = qt::foundation::canonicalize_path(workspace);
            if (!canonical_workspace) return persistent_error(canonical_workspace.error().summary(), ToolResultStatus::execution_error);
            qt::foundation::Path child_cwd = context.current_directory.value_or(canonical_workspace.value());
            if (request.arguments.contains("cwd")) {
                if (!request.arguments.at("cwd").is_string()) return persistent_error("subagent cwd must be a string");
                child_cwd = request.arguments.at("cwd").get<std::string>();
                if (child_cwd.is_relative()) child_cwd = canonical_workspace.value() / child_cwd;
            }
            const auto canonical_cwd = qt::foundation::canonicalize_path(child_cwd);
            if (!canonical_cwd || !is_workspace_path_allowed(canonical_cwd.value(), canonical_workspace.value(), context))
                return persistent_error("subagent cwd escapes workspace root", ToolResultStatus::permission_denied);
            qt::app::runtime::IsolatedAgentRequest process;
            process.arguments = {"--mode", "json", "--no-extensions"};
            if (!found->second->inherit_project_context) process.arguments.push_back("--no-context-files");
            if (!found->second->inherit_skills) process.arguments.push_back("--no-skills");
            if (allowed_tools.empty()) process.arguments.push_back("--no-tools");
            else {
                std::string tools;
                for (const auto& item : allowed_tools) { if (!tools.empty()) tools += ','; tools += item; }
                process.arguments.insert(process.arguments.end(), {"--tools", tools});
            }
            std::optional<std::string> model = request.arguments.contains("model")
                                                   ? std::optional<std::string>(request.arguments.at("model").get<std::string>())
                                                   : found->second->model;
            if (!model && context.metadata.contains("model")) {
                const auto& parent_model = context.metadata.at("model");
                if (parent_model.is_string() && !parent_model.get<std::string>().empty()) {
                    model = parent_model.get<std::string>();
                } else if (parent_model.is_object()) {
                    const auto inherited = parent_model.value(
                        "id", parent_model.value("name", std::string{}));
                    if (!inherited.empty()) {
                        model = inherited;
                        const auto provider = parent_model.value("provider", std::string{});
                        if (!provider.empty() && provider != "native")
                            process.arguments.insert(process.arguments.end(), {"--provider", provider});
                    }
                }
            }
            if (model && found->second->thinking && model->find(':') == std::string::npos &&
                *found->second->thinking != "off") {
                *model += ":" + *found->second->thinking;
            }
            if (model && !model->empty()) process.arguments.insert(process.arguments.end(), {"--model", *model});
            if (!found->second->system_prompt.empty()) process.arguments.insert(process.arguments.end(),
                {found->second->system_prompt_mode == "append" ? "--append-system-prompt" : "--system-prompt", found->second->system_prompt});
            process.cwd = canonical_cwd.value();
            process.environment = {{"QT_SUBAGENT_CHILD", "1"}, {"QT_SUBAGENT_DEPTH", std::to_string(depth + 1U)},
                                   {"QT_SUBAGENT_MAX_DEPTH", std::to_string(child_max_depth)},
                                   {"QT_SUBAGENT_ALLOW_EXTERNAL", context.allow_external_filesystem_access ? "1" : "0"},
                                   {"QT_CODING_AGENT_DIR", options_.agent_home.generic_string()}};
            qt::app::runtime::PersistentSubagentRequest persistent;
            persistent.name = request.arguments.value("name", *agent_name);
            persistent.agent = *agent_name;
            persistent.task = *task_text;
            persistent.process = std::move(process);
            persistent.permissions = Json{{"workspace", canonical_workspace.value().generic_string()},
                                          {"allow_external", context.allow_external_filesystem_access},
                                          {"toolnames", allowed_tools}, {"depth", depth + 1U},
                                          {"trust", !request.arguments.value("confirmProjectAgents", true)},
                                          {"model", model.value_or("")}};
            if (request.arguments.contains("fork")) {
                if (request.arguments.at("fork").is_number_integer()) {
                    if (request.arguments.at("fork").get<long long>() <= 0)
                        return persistent_error("fork must be none, all, or a positive user-turn count");
                    persistent.fork = request.arguments.at("fork").get<long long>();
                } else {
                    const auto fork = request.arguments.at("fork").get<std::string>();
                    if (fork == "none" || fork == "all") persistent.fork = fork;
                    else if (const auto parsed = parse_nonnegative_size(fork); parsed && *parsed > 0U)
                        persistent.fork = static_cast<unsigned long long>(*parsed);
                    else return persistent_error("fork must be none, all, or a positive user-turn count");
                }
            }
            auto result = options_.coordinator->spawn(std::move(persistent));
            if (!result) return persistent_error(result.error().summary(), ToolResultStatus::execution_error);
            ToolResult output; output.call_id = request.call_id; output.output = result.value(); return output;
        }
    }

    if (depth >= configured_max_depth) {
        return make_execution_error(
            ToolResultStatus::permission_denied,
            "subagent depth limit reached at depth " + std::to_string(depth) +
                " (max " + std::to_string(configured_max_depth) + ")", request);
    }

    const bool has_single = request.arguments.contains("agent") &&
                            request.arguments.contains("task");
    const bool has_parallel = request.arguments.contains("tasks") &&
                              request.arguments.at("tasks").is_array() &&
                              !request.arguments.at("tasks").empty();
    const bool has_chain = request.arguments.contains("chain") &&
                           request.arguments.at("chain").is_array() &&
                           !request.arguments.at("chain").empty();
    if (static_cast<int>(has_single) + static_cast<int>(has_parallel) +
            static_cast<int>(has_chain) !=
        1) {
        return make_invalid_request(
            "provide exactly one subagent mode: agent+task, tasks, or chain",
            request);
    }
    if ((has_single && (request.arguments.contains("tasks") ||
                        request.arguments.contains("chain") ||
                        request.arguments.contains("concurrency"))) ||
        (has_parallel && (request.arguments.contains("agent") ||
                          request.arguments.contains("task") ||
                          request.arguments.contains("chain"))) ||
        (has_chain && (request.arguments.contains("agent") ||
                       request.arguments.contains("tasks") ||
                       request.arguments.contains("concurrency")))) {
        return make_invalid_request(
            "subagent arguments from different execution modes cannot be combined",
            request);
    }

    std::size_t parallel_concurrency = options_.default_concurrency;
    if (request.arguments.contains("concurrency")) {
        if (!request.arguments.at("concurrency").is_number_integer()) {
            return make_invalid_request(
                "subagent concurrency must be an integer", request);
        }
        const auto requested =
            request.arguments.at("concurrency").get<long long>();
        if (requested < 1) {
            return make_invalid_request(
                "subagent concurrency must be at least 1", request);
        }
        parallel_concurrency = static_cast<std::size_t>(requested);
    }

    std::vector<RequestedTask> tasks;
    const auto parse_task = [&](const Json& value,
                                std::optional<std::string> default_cwd,
                                std::optional<std::string> default_model)
        -> std::optional<RequestedTask> {
        if (!value.is_object() || !value.contains("agent") ||
            !value.at("agent").is_string() || !value.contains("task") ||
            !value.at("task").is_string()) {
            return std::nullopt;
        }
        for (const auto& item : value.items()) {
            if (item.key() != "agent" && item.key() != "task" &&
                item.key() != "cwd" && item.key() != "model") {
                return std::nullopt;
            }
        }
        RequestedTask task{value.at("agent").get<std::string>(),
                           value.at("task").get<std::string>(),
                           std::move(default_cwd), std::move(default_model)};
        if (value.contains("cwd")) {
            if (!value.at("cwd").is_string()) {
                return std::nullopt;
            }
            task.cwd = value.at("cwd").get<std::string>();
        }
        if (value.contains("model")) {
            if (!value.at("model").is_string()) {
                return std::nullopt;
            }
            task.model = value.at("model").get<std::string>();
        }
        if (trim_copy(task.agent).empty() || trim_copy(task.task).empty()) {
            return std::nullopt;
        }
        return task;
    };

    std::optional<std::string> default_cwd;
    std::optional<std::string> default_model;
    if (request.arguments.contains("cwd")) {
        if (!request.arguments.at("cwd").is_string()) {
            return make_invalid_request("subagent cwd must be a string", request);
        }
        default_cwd = request.arguments.at("cwd").get<std::string>();
    }
    if (request.arguments.contains("model")) {
        if (!request.arguments.at("model").is_string()) {
            return make_invalid_request("subagent model must be a string", request);
        }
        default_model = request.arguments.at("model").get<std::string>();
    }
    if (has_single) {
        Json value{{"agent", request.arguments.at("agent")},
                   {"task", request.arguments.at("task")}};
        const auto task = parse_task(value, default_cwd, default_model);
        if (!task) {
            return make_invalid_request(
                "single subagent mode requires non-empty string agent and task",
                request);
        }
        tasks.push_back(*task);
    } else {
        const Json& values = request.arguments.at(has_parallel ? "tasks" : "chain");
        for (const auto& value : values) {
            const auto task = parse_task(value, default_cwd, default_model);
            if (!task) {
                return make_invalid_request(
                    "each subagent task requires agent and task strings and optional cwd/model",
                    request);
            }
            tasks.push_back(*task);
        }
    }
    if (has_parallel && tasks.size() > options_.max_parallel_tasks) {
        return make_invalid_request(
            "too many parallel subagent tasks (max " +
                std::to_string(options_.max_parallel_tasks) + ")",
            request);
    }

    for (const auto& task : tasks) {
        const auto found = agents.find(task.agent);
        if (found == agents.end()) {
            std::string available;
            for (const auto& [name, unused] : agents) {
                (void)unused;
                if (!available.empty()) {
                    available += ", ";
                }
                available += name;
            }
            return make_invalid_request(
                "unknown subagent '" + task.agent + "'. Available: " +
                    (available.empty() ? std::string("none") : available),
                request);
        }
        if (found->second->source == SubagentSource::project &&
            request.arguments.value("confirmProjectAgents", true)) {
            return make_execution_error(
                ToolResultStatus::permission_denied,
                "project subagent '" + task.agent +
                    "' requires trust confirmation. Inspect " +
                    found->second->file_path.generic_string() +
                    " and retry with confirmProjectAgents=false only for a trusted repository.",
                request);
        }
    }

    if (auto queued = require_mutation_approval(
            ToolMutationKind::command,
            "delegate " + std::to_string(tasks.size()) +
                (tasks.size() == 1U ? " task" : " tasks") +
                " to isolated subagents",
            request, context)) {
        return *queued;
    }

    const std::string run_id = make_run_id();
    const std::string mode = has_parallel ? "parallel" : has_chain ? "chain" : "single";
    std::vector<Json> states(tasks.size());
    for (std::size_t index = 0U; index < tasks.size(); ++index) {
        states[index] = Json{{"agent", tasks[index].agent},
                             {"task", tasks[index].task},
                             {"status", "pending"},
                             {"exit_code", -1}};
    }
    std::mutex state_mutex;
    std::mutex callback_mutex;
    const auto emit_update = [&]() {
        if (!context.update_callback) {
            return;
        }
        Json snapshot;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            snapshot = states;
        }
        ToolResult update;
        update.call_id = request.call_id;
        update.output = Json{{"mode", mode},
                             {"run_id", run_id},
                             {"running", true},
                             {"results", std::move(snapshot)}};
        std::lock_guard<std::mutex> lock(callback_mutex);
        context.update_callback(update);
    };

    const auto run_child = [&](const RequestedTask& task,
                               std::size_t index) -> ChildResult {
        const SubagentDefinition& agent = *agents.at(task.agent);
        qt::foundation::Path workspace = context.workspace_root.value_or(
            options_.cwd.empty() ? std::filesystem::current_path() : options_.cwd);
        const auto canonical_workspace = qt::foundation::canonicalize_path(workspace);
        if (!canonical_workspace) {
            ChildResult failed;
            failed.json = Json{{"agent", task.agent},
                               {"task", task.task},
                               {"status", "failed"},
                               {"exit_code", 1},
                               {"error", canonical_workspace.error().summary()}};
            return failed;
        }
        qt::foundation::Path cwd = context.current_directory.value_or(
            options_.cwd.empty() ? canonical_workspace.value() : options_.cwd);
        if (task.cwd && !task.cwd->empty()) {
            cwd = *task.cwd;
            if (cwd.is_relative()) {
                cwd = canonical_workspace.value() / cwd;
            }
        }
        const auto canonical_cwd = qt::foundation::canonicalize_path(cwd);
        if (!canonical_cwd ||
            !is_workspace_path_allowed(canonical_cwd.value(),
                                       canonical_workspace.value(), context)) {
            ChildResult failed;
            failed.json = Json{{"agent", task.agent},
                               {"task", task.task},
                               {"status", "failed"},
                               {"exit_code", 1},
                               {"error", "subagent cwd escapes workspace root"}};
            return failed;
        }

        qt::app::runtime::IsolatedAgentRequest process_request;
        process_request.arguments.insert(process_request.arguments.end(),
                                         {"--mode", "json", "--no-session",
                                          "--no-extensions"});
        if (!agent.inherit_project_context) {
            process_request.arguments.push_back("--no-context-files");
        }
        if (!agent.inherit_skills) {
            process_request.arguments.push_back("--no-skills");
        }
        if (agent.tools.empty()) {
            process_request.arguments.push_back("--no-tools");
        } else {
            std::string tools;
            for (const auto& tool : agent.tools) {
                if (!tools.empty()) {
                    tools.push_back(',');
                }
                tools += tool;
            }
            process_request.arguments.insert(process_request.arguments.end(),
                                             {"--tools", std::move(tools)});
        }
        std::optional<std::string> model = task.model ? task.model : agent.model;
        if (model && agent.thinking &&
            model->find(':') == std::string::npos && *agent.thinking != "off") {
            *model += ":" + *agent.thinking;
        }
        if (model && !model->empty()) {
            process_request.arguments.insert(process_request.arguments.end(),
                                             {"--model", *model});
        }
        if (!agent.system_prompt.empty()) {
            process_request.arguments.push_back(
                agent.system_prompt_mode == "append" ? "--append-system-prompt"
                                                     : "--system-prompt");
            process_request.arguments.push_back(agent.system_prompt);
        }
        process_request.arguments.push_back("Task: " + task.task);
        process_request.cwd = canonical_cwd.value();
        process_request.cancellation_token = context.cancellation_token;
        const std::size_t child_max_depth = agent.max_subagent_depth
                                                ? std::min(configured_max_depth,
                                                           *agent.max_subagent_depth)
                                                : configured_max_depth;
        process_request.environment = {
            {"QT_SUBAGENT_CHILD", std::string("1")},
            {"QT_SUBAGENT_DEPTH", std::to_string(depth + 1U)},
            {"QT_SUBAGENT_MAX_DEPTH", std::to_string(child_max_depth)},
            {"QT_SUBAGENT_RUN_ID", run_id},
            {"QT_SUBAGENT_NAME", agent.name},
            {"QT_CODING_AGENT_DIR", options_.agent_home.generic_string()},
        };
        process_request.output_callback = [&](const auto& update) {
            const std::string& streaming = update.final_output;
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                states[index]["status"] = "running";
                if (!streaming.empty()) {
                    states[index]["streaming_output"] = streaming;
                }
            }
            emit_update();
        };
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            states[index]["status"] = "running";
        }
        emit_update();
        const auto process = runner_.run(process_request);
        ChildResult child;
        if (!process) {
            child.json = Json{{"agent", task.agent},
                              {"agent_source", to_string(agent.source)},
                              {"task", task.task},
                              {"status", "failed"},
                              {"exit_code", 1},
                              {"error", process.error().summary()},
                              {"cwd", canonical_cwd.value().generic_string()}};
            return child;
        }
        child.output = process.value().final_output;
        const Json& messages = process.value().final_messages;
        child.success = process.value().exit_code == 0 &&
                        !process.value().cancelled &&
                        !process.value().timed_out && !child.output.empty();
        child.json = Json{{"agent", task.agent},
                          {"agent_source", to_string(agent.source)},
                          {"task", task.task},
                          {"status", child.success ? "completed" : "failed"},
                          {"exit_code", process.value().exit_code},
                          {"cancelled", process.value().cancelled},
                          {"timed_out", process.value().timed_out},
                          {"messages", messages},
                          {"output", child.output},
                          {"stderr", process.value().stderr_text},
                          {"cwd", canonical_cwd.value().generic_string()},
                          {"model", model.value_or("")}};
        if (!child.success && child.output.empty()) {
            child.json["error"] = process.value().stderr_text.empty()
                                      ? "child exited without a final assistant message"
                                      : process.value().stderr_text;
        }
        return child;
    };

    std::vector<ChildResult> results(tasks.size());
    if (has_chain) {
        std::string previous;
        const std::string original_task =
            request.arguments.value("task", std::string{});
        for (std::size_t index = 0U; index < tasks.size(); ++index) {
            tasks[index].task = replace_all(tasks[index].task, "{previous}", previous);
            tasks[index].task = replace_all(tasks[index].task, "{task}", original_task);
            results[index] = run_child(tasks[index], index);
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                states[index] = results[index].json;
            }
            emit_update();
            if (!results[index].success) {
                break;
            }
            previous = results[index].output;
        }
    } else if (has_parallel) {
        const std::size_t concurrency = std::min(
            {parallel_concurrency, tasks.size(), kMaximumConcurrency});
        std::atomic<std::size_t> next{0U};
        std::vector<std::thread> workers;
        workers.reserve(concurrency);
        for (std::size_t worker = 0U; worker < concurrency; ++worker) {
            workers.emplace_back([&]() {
                while (true) {
                    const std::size_t index = next.fetch_add(1U);
                    if (index >= tasks.size()) {
                        return;
                    }
                    results[index] = run_child(tasks[index], index);
                    {
                        std::lock_guard<std::mutex> lock(state_mutex);
                        states[index] = results[index].json;
                    }
                    emit_update();
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
    } else {
        results[0] = run_child(tasks[0], 0U);
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            states[0] = results[0].json;
        }
        emit_update();
    }

    std::size_t completed = 0U;
    Json result_items = Json::array();
    std::ostringstream text;
    for (std::size_t index = 0U; index < results.size(); ++index) {
        if (results[index].json.empty()) {
            break;
        }
        result_items.push_back(results[index].json);
        if (results[index].success) {
            ++completed;
        }
        if (has_parallel) {
            text << "=== Task " << (index + 1U) << ": " << tasks[index].agent
                 << " ===\n";
            text << (results[index].output.empty()
                         ? results[index].json.value("error", std::string("(no output)"))
                         : results[index].output)
                 << "\n\n";
        }
    }
    if (has_parallel) {
        const std::string task_sections = text.str();
        text.str({});
        text.clear();
        text << completed << '/' << results.size()
             << " subagents completed\n\n" << task_sections;
    } else if (!result_items.empty()) {
        const ChildResult* last = nullptr;
        for (const auto& child : results) {
            if (!child.json.empty()) {
                last = &child;
            }
        }
        if (last != nullptr) {
            text << last->output;
            if (last->output.empty()) {
                text << last->json.value("error", std::string("(no output)"));
            }
        }
    }

    ToolResult result;
    result.call_id = request.call_id;
    result.status = completed == results.size()
                        ? ToolResultStatus::success
                        : has_parallel ? ToolResultStatus::success
                                       : ToolResultStatus::execution_error;
    result.output = Json{{"mode", mode},
                         {"run_id", run_id},
                         {"agent_scope", scope},
                         {"completed", completed},
                         {"total", results.size()},
                         {"results", std::move(result_items)},
                         {"diagnostics", discovery.diagnostics},
                         {"output", trim_copy(text.str())}};
    if (result.status != ToolResultStatus::success) {
        result.error_message = result.output.at("output").get<std::string>();
    }
    apply_output_guards(result, output_guard_options_from_context(context));
    return result;
}

}  // namespace qt::tools
