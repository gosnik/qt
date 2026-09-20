#include "qt/tools/complete_path_tool.hpp"

#include "qt/foundation/filesystem.hpp"
#include "qt/tools/tool_validation.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <vector>

namespace qt::tools {

namespace {

struct CompletionRecord {
    std::string name;
    qt::foundation::Path path;
    std::string kind;
    std::string completion;
};

bool ends_with_separator(const std::string& text) {
    if (text.empty()) {
        return true;
    }
    const char tail = text.back();
    return tail == '/' || tail == '\\';
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

std::string entry_kind(const std::filesystem::directory_entry& entry) {
    std::error_code error;
    if (entry.is_directory(error)) {
        return "directory";
    }
    error.clear();
    if (entry.is_regular_file(error)) {
        return "file";
    }
    return "other";
}

qt::foundation::Path contextualize_path(const qt::foundation::Path& path,
                                        const ToolContext& context) {
    if (!path.is_relative()) {
        return path;
    }
    if (context.current_directory) {
        return *context.current_directory / path;
    }
    if (context.workspace_root) {
        return *context.workspace_root / path;
    }
    return path;
}

std::string completion_text(const qt::foundation::Path& parent,
                            const std::string& name,
                            const std::string& kind) {
    auto completion = (parent / name).generic_string();
    if (completion == ".") {
        completion.clear();
    }
    if (kind == "directory") {
        completion += "/";
    }
    return completion;
}

}  // namespace

CompletePathTool::CompletePathTool() {
    descriptor_.name = "complete_path";
    descriptor_.description =
        "Complete a path prefix permitted by the runtime filesystem policy.";
    descriptor_.input_schema = Json{
        {"type", "object"},
        {"properties",
         {
             {"path",
              {
                  {"type", "string"},
                  {"description",
                   "Relative or absolute filesystem path prefix to complete."},
              }},
         }},
        {"required", Json::array({"path"})},
        {"additionalProperties", false},
    };
}

const ToolDescriptor& CompletePathTool::descriptor() const { return descriptor_; }

ToolResult CompletePathTool::invoke(const ToolRequest& request,
                                    const ToolContext& context) const {
    if (!is_valid(request)) {
        return make_invalid_request("invalid tool request", request);
    }
    if (request.tool_name != descriptor_.name) {
        return make_invalid_request("tool request name does not match complete_path",
                                    request);
    }
    if (!has_only_arguments(request, {"path"})) {
        return make_invalid_request("complete_path accepts only the path argument",
                                    request);
    }

    const auto path_text =
        require_string_argument(request, "path", descriptor_.name);
    if (!path_text) {
        return make_invalid_request(path_text.error().summary(), request);
    }

    const std::string expanded_path_text =
        expand_path_text(path_text.value());
    const qt::foundation::Path raw_path = expanded_path_text;
    const bool prefix_is_absolute = raw_path.is_absolute();
    const bool treat_as_directory_prefix = ends_with_separator(expanded_path_text);
    const auto contextual_prefix = contextualize_path(raw_path, context);
    const auto resolved_prefix = qt::foundation::canonicalize_path(contextual_prefix);
    if (!resolved_prefix) {
        return make_invalid_request(resolved_prefix.error().summary(), request);
    }

    std::optional<qt::foundation::Path> workspace_root;
    if (context.workspace_root) {
        const auto root = qt::foundation::canonicalize_path(*context.workspace_root);
        if (!root) {
            return make_execution_error(ToolResultStatus::execution_error,
                                        root.error().summary(), request);
        }
        if (!is_workspace_path_allowed(resolved_prefix.value(), root.value(),
                                       context)) {
            return make_execution_error(ToolResultStatus::permission_denied,
                                        "path escapes workspace_root", request);
        }
        workspace_root = root.value();
    }

    qt::foundation::Path relative_parent;
    std::string filename_prefix;
    qt::foundation::Path search_parent_input;
    if (treat_as_directory_prefix) {
        relative_parent = raw_path;
        search_parent_input = contextual_prefix;
    } else {
        relative_parent = raw_path.parent_path();
        filename_prefix = raw_path.filename().generic_string();
        search_parent_input = contextual_prefix.parent_path();
    }

    if (search_parent_input.empty()) {
        if (context.current_directory) {
            search_parent_input = *context.current_directory;
        } else if (context.workspace_root) {
            search_parent_input = *context.workspace_root;
        } else {
            search_parent_input = std::filesystem::current_path();
        }
    }

    const auto resolved_parent =
        qt::foundation::canonicalize_path(search_parent_input);
    if (!resolved_parent) {
        return make_invalid_request(resolved_parent.error().summary(), request);
    }
    if (workspace_root &&
        !is_workspace_path_allowed(resolved_parent.value(), *workspace_root,
                                   context)) {
        return make_execution_error(ToolResultStatus::permission_denied,
                                    "path escapes workspace_root", request);
    }

    std::error_code error;
    const auto parent_status = std::filesystem::status(resolved_parent.value(), error);
    if (error) {
        if (error == std::errc::no_such_file_or_directory) {
            ToolResult result;
            result.status = ToolResultStatus::success;
            result.call_id = request.call_id;
            result.output = Json{
                {"prefix", path_text.value()},
                {"resolved_prefix",
                 qt::foundation::path_for_display(resolved_prefix.value())},
                {"parent", qt::foundation::path_for_display(resolved_parent.value())},
                {"count", 0},
                {"matches", Json::array()},
            };
            return result;
        }
        return make_execution_error(ToolResultStatus::execution_error,
                                    qt::foundation::path_for_display(resolved_parent.value()) +
                                        ": " + error.message(),
                                    request);
    }
    if (!std::filesystem::exists(parent_status)) {
        ToolResult result;
        result.status = ToolResultStatus::success;
        result.call_id = request.call_id;
        result.output = Json{
            {"prefix", path_text.value()},
            {"resolved_prefix", qt::foundation::path_for_display(resolved_prefix.value())},
            {"parent", qt::foundation::path_for_display(resolved_parent.value())},
            {"count", 0},
            {"matches", Json::array()},
        };
        return result;
    }
    if (!std::filesystem::is_directory(parent_status)) {
        return make_execution_error(ToolResultStatus::invalid_request,
                                    qt::foundation::path_for_display(resolved_parent.value()) +
                                        " is not a directory",
                                    request);
    }

    std::vector<CompletionRecord> matches;
    for (std::filesystem::directory_iterator iterator(resolved_parent.value(), error);
         !error && iterator != std::filesystem::directory_iterator(); ++iterator) {
        const auto& entry = *iterator;
        const auto name = entry.path().filename().generic_string();
        if (name.rfind(filename_prefix, 0) != 0) {
            continue;
        }

        const auto kind = entry_kind(entry);
        qt::foundation::Path completion_path =
            prefix_is_absolute ? resolved_parent.value() : relative_parent;
        matches.push_back(CompletionRecord{
            name,
            entry.path(),
            kind,
            completion_text(completion_path, name, kind),
        });
    }
    if (error) {
        return make_execution_error(ToolResultStatus::execution_error,
                                    qt::foundation::path_for_display(resolved_parent.value()) +
                                        ": " + error.message(),
                                    request);
    }

    std::sort(matches.begin(), matches.end(),
              [](const CompletionRecord& lhs, const CompletionRecord& rhs) {
                  return lhs.completion < rhs.completion;
              });

    Json payload_matches = Json::array();
    for (const auto& match : matches) {
        payload_matches.push_back(Json{
            {"name", match.name},
            {"path", qt::foundation::path_for_display(match.path)},
            {"kind", match.kind},
            {"completion", match.completion},
        });
    }

    ToolResult result;
    result.status = ToolResultStatus::success;
    result.call_id = request.call_id;
    result.output = Json{
        {"prefix", path_text.value()},
        {"resolved_prefix", qt::foundation::path_for_display(resolved_prefix.value())},
        {"parent", qt::foundation::path_for_display(resolved_parent.value())},
        {"count", matches.size()},
        {"matches", std::move(payload_matches)},
    };
    return result;
}

}  // namespace qt::tools
