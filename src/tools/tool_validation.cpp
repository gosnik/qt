#include "qt/tools/tool_validation.hpp"

#include "qt/foundation/filesystem.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>

namespace qt::tools {

namespace {

ToolResult make_error(ToolResultStatus status, std::string message,
                      const ToolRequest& request) {
    ToolResult result;
    result.status = status;
    result.error_message = std::move(message);
    result.call_id = request.call_id;
    return result;
}

std::size_t number_from_guard_metadata(const Json& metadata, const char* name,
                                       std::size_t fallback) {
    if (!metadata.is_object() || !metadata.contains("output_guard") ||
        !metadata.at("output_guard").is_object()) {
        return fallback;
    }

    const Json& guard = metadata.at("output_guard");
    if (!guard.contains(name) ||
        !(guard.at(name).is_number_integer() || guard.at(name).is_number_unsigned())) {
        return fallback;
    }
    if (guard.at(name).is_number_unsigned()) {
        return guard.at(name).get<std::size_t>();
    }
    const auto value = guard.at(name).get<long long>();
    if (value < 0) {
        return fallback;
    }
    return static_cast<std::size_t>(value);
}

std::string json_path_child(const std::string& path, const std::string& key) {
    if (path == "/") {
        return "/" + key;
    }
    return path + "/" + key;
}

std::string expand_path_text(std::string path) {
    if (!path.empty() && path.front() == '@') {
        path.erase(path.begin());
    }

    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return path;
    }
    if (path == "~") {
        return std::string(home);
    }
    if (path.rfind("~/", 0U) == 0U) {
        return std::string(home) + path.substr(1U);
    }
    return path;
}

void apply_output_guards_to_value(Json& value, const std::string& path,
                                  OutputGuardOptions options, Json& fields) {
    if (value.is_string()) {
        const std::string text = value.get<std::string>();
        if (text.size() > options.max_string_bytes) {
            value = text.substr(0U, options.max_string_bytes);
            fields.push_back(Json{
                {"path", path},
                {"type", "string"},
                {"original_bytes", text.size()},
                {"preview_bytes", options.max_string_bytes},
            });
        }
        return;
    }

    if (value.is_array()) {
        const std::size_t original_size = value.size();
        const std::size_t preview_size =
            std::min(original_size, options.max_array_items);
        for (std::size_t index = 0U; index < preview_size; ++index) {
            apply_output_guards_to_value(value.at(index),
                                         json_path_child(path, std::to_string(index)),
                                         options, fields);
        }
        if (original_size > options.max_array_items) {
            value.erase(value.begin() + static_cast<Json::difference_type>(preview_size),
                        value.end());
            fields.push_back(Json{
                {"path", path},
                {"type", "array"},
                {"original_items", original_size},
                {"preview_items", preview_size},
            });
        }
        return;
    }

    if (value.is_object()) {
        for (auto& item : value.items()) {
            apply_output_guards_to_value(item.value(),
                                         json_path_child(path, item.key()),
                                         options, fields);
        }
    }
}

}  // namespace

ToolResult make_invalid_request(std::string message, const ToolRequest& request) {
    return make_error(ToolResultStatus::invalid_request, std::move(message), request);
}

ToolResult make_execution_error(ToolResultStatus status, std::string message,
                                const ToolRequest& request) {
    return make_error(status, std::move(message), request);
}

std::optional<ToolResult> require_mutation_approval(
    ToolMutationKind kind, std::string summary, const ToolRequest& request,
    const ToolContext& context) {
    if (context.mutation_policy == ToolMutationPolicy::execute) {
        return std::nullopt;
    }

    if (!context.mutation_queue) {
        return make_invalid_request("mutation queue is required by tool context", request);
    }

    if (request.approved_mutation_id) {
        if (context.mutation_queue->is_approved(*request.approved_mutation_id)) {
            return std::nullopt;
        }
        return make_execution_error(ToolResultStatus::permission_denied,
                                    "mutation approval was not granted", request);
    }

    ToolMutation mutation;
    mutation.kind = kind;
    mutation.tool_name = request.tool_name;
    mutation.call_id = request.call_id;
    mutation.summary = std::move(summary);
    mutation.arguments = request.arguments;
    const std::string id = context.mutation_queue->enqueue(std::move(mutation));

    ToolResult result;
    result.status = ToolResultStatus::pending_approval;
    result.call_id = request.call_id;
    result.output = Json{
        {"mutation_id", id},
        {"queued", true},
        {"kind", to_string(kind)},
        {"tool_name", request.tool_name},
    };
    return result;
}

OutputGuardOptions output_guard_options_from_context(const ToolContext& context) {
    OutputGuardOptions options;
    options.max_string_bytes = number_from_guard_metadata(
        context.metadata, "max_string_bytes", options.max_string_bytes);
    options.max_array_items = number_from_guard_metadata(
        context.metadata, "max_array_items", options.max_array_items);
    return options;
}

void apply_output_guards(ToolResult& result, OutputGuardOptions options) {
    Json fields = Json::array();
    apply_output_guards_to_value(result.output, "", options, fields);
    result.output["output_guard"] = Json{
        {"truncated", !fields.empty()},
        {"max_string_bytes", options.max_string_bytes},
        {"max_array_items", options.max_array_items},
        {"fields", std::move(fields)},
    };
}

qt::foundation::Result<std::string> require_string_argument(const ToolRequest& request,
                                                            std::string_view name,
                                                            std::string_view tool_name) {
    const std::string key{name};
    if (!request.arguments.contains(key) || !request.arguments.at(key).is_string()) {
        return qt::foundation::Error{
            qt::foundation::ErrorCode::invalid_argument,
            std::string(tool_name) + " requires a string " + key + " argument",
        };
    }
    return request.arguments.at(key).get<std::string>();
}

qt::foundation::Result<qt::foundation::Path> resolve_workspace_path(
    const ToolRequest& request, const ToolContext& context, std::string_view tool_name) {
    const auto path_text = require_string_argument(request, "path", tool_name);
    if (!path_text) {
        return path_text.error();
    }

    qt::foundation::Path path = expand_path_text(path_text.value());
    if (path.is_relative()) {
        if (context.current_directory) {
            path = *context.current_directory / path;
        } else if (context.workspace_root) {
            path = *context.workspace_root / path;
        }
    }

    return qt::foundation::canonicalize_path(path);
}

bool is_within_workspace_root(const qt::foundation::Path& path,
                              const qt::foundation::Path& root) {
    auto path_it = path.begin();
    auto root_it = root.begin();

    for (; root_it != root.end(); ++root_it, ++path_it) {
        if (path_it == path.end() || *path_it != *root_it) {
            return false;
        }
    }

    return true;
}

bool is_workspace_path_allowed(const qt::foundation::Path& path,
                               const qt::foundation::Path& root,
                               const ToolContext& context) {
    return context.allow_external_filesystem_access ||
           is_within_workspace_root(path, root);
}

}  // namespace qt::tools
