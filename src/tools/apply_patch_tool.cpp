#include "qt/tools/apply_patch_tool.hpp"

#include "qt/foundation/filesystem.hpp"
#include "qt/tools/tool_validation.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <sstream>
#include <system_error>
#include <vector>

namespace qt::tools {
namespace {

enum class PatchOperationKind { add, update, remove };

struct PatchOperation {
    PatchOperationKind kind = PatchOperationKind::update;
    qt::foundation::Path path;
    std::string old_text;
    std::string new_text;
};

std::vector<std::string> split_patch_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t offset = 0U;
    while (offset <= text.size()) {
        const auto newline = text.find('\n', offset);
        if (newline == std::string_view::npos) {
            lines.emplace_back(text.substr(offset));
            break;
        }
        auto line = std::string(text.substr(offset, newline - offset));
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        offset = newline + 1U;
    }
    return lines;
}

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.substr(0U, prefix.size()) == prefix;
}

qt::foundation::Path base_path(const ToolContext& context) {
    if (context.current_directory) {
        return *context.current_directory;
    }
    if (context.workspace_root) {
        return *context.workspace_root;
    }
    return std::filesystem::current_path();
}

qt::foundation::Result<qt::foundation::Path> resolve_patch_path(
    std::string_view raw_path, const ToolContext& context) {
    if (raw_path.empty()) {
        return qt::foundation::Error{qt::foundation::ErrorCode::invalid_argument,
                                     "apply_patch requires non-empty file paths"};
    }
    qt::foundation::Path path{std::string(raw_path)};
    if (path.is_relative()) {
        path = base_path(context) / path;
    }
    return qt::foundation::canonicalize_path(path);
}

qt::foundation::Result<std::vector<PatchOperation>> parse_patch(
    const std::string& patch, const ToolContext& context) {
    auto lines = split_patch_lines(patch);
    while (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    if (lines.size() < 2U || lines.front() != "*** Begin Patch" ||
        lines.back() != "*** End Patch") {
        return qt::foundation::Error{
            qt::foundation::ErrorCode::invalid_argument,
            "apply_patch input must start with *** Begin Patch and end with *** End Patch",
        };
    }

    std::vector<PatchOperation> operations;
    std::size_t index = 1U;
    while (index + 1U < lines.size()) {
        const auto& line = lines[index++];
        PatchOperation operation;
        if (starts_with(line, "*** Add File: ")) {
            operation.kind = PatchOperationKind::add;
            auto resolved = resolve_patch_path(line.substr(14U), context);
            if (!resolved) {
                return resolved.error();
            }
            operation.path = resolved.value();
            while (index + 1U < lines.size() && !starts_with(lines[index], "*** ")) {
                if (!starts_with(lines[index], "+")) {
                    return qt::foundation::Error{
                        qt::foundation::ErrorCode::invalid_argument,
                        "apply_patch add hunks require + lines",
                    };
                }
                operation.new_text += lines[index].substr(1U);
                operation.new_text += '\n';
                ++index;
            }
        } else if (starts_with(line, "*** Delete File: ")) {
            operation.kind = PatchOperationKind::remove;
            auto resolved = resolve_patch_path(line.substr(17U), context);
            if (!resolved) {
                return resolved.error();
            }
            operation.path = resolved.value();
        } else if (starts_with(line, "*** Update File: ")) {
            operation.kind = PatchOperationKind::update;
            auto resolved = resolve_patch_path(line.substr(17U), context);
            if (!resolved) {
                return resolved.error();
            }
            operation.path = resolved.value();
            while (index + 1U < lines.size() && !starts_with(lines[index], "*** ")) {
                if (starts_with(lines[index], "@@")) {
                    ++index;
                    continue;
                }
                if (lines[index].empty()) {
                    return qt::foundation::Error{
                        qt::foundation::ErrorCode::invalid_argument,
                        "apply_patch update hunks require prefixed lines",
                    };
                }
                const char marker = lines[index][0];
                const std::string body = lines[index].substr(1U);
                if (marker == ' ' || marker == '-') {
                    operation.old_text += body;
                    operation.old_text += '\n';
                }
                if (marker == ' ' || marker == '+') {
                    operation.new_text += body;
                    operation.new_text += '\n';
                }
                if (marker != ' ' && marker != '-' && marker != '+') {
                    return qt::foundation::Error{
                        qt::foundation::ErrorCode::invalid_argument,
                        "apply_patch update hunks require space, -, or + prefixes",
                    };
                }
                ++index;
            }
            if (operation.old_text.empty()) {
                return qt::foundation::Error{
                    qt::foundation::ErrorCode::invalid_argument,
                    "apply_patch update hunks require context or removed lines",
                };
            }
        } else {
            return qt::foundation::Error{
                qt::foundation::ErrorCode::invalid_argument,
                "apply_patch expected Add File, Delete File, or Update File hunk",
            };
        }
        operations.push_back(std::move(operation));
    }
    if (operations.empty()) {
        return qt::foundation::Error{qt::foundation::ErrorCode::invalid_argument,
                                     "apply_patch requires at least one hunk"};
    }
    return operations;
}

std::size_t count_occurrences(std::string_view text, std::string_view needle) {
    std::size_t count = 0U;
    std::size_t offset = 0U;
    while (offset <= text.size()) {
        const auto found = text.find(needle, offset);
        if (found == std::string_view::npos) {
            break;
        }
        ++count;
        offset = found + needle.size();
    }
    return count;
}

}  // namespace

ApplyPatchTool::ApplyPatchTool() {
    descriptor_.name = "apply_patch";
    descriptor_.description =
        "Apply a validated multi-file patch using the Codex-style patch envelope.";
    descriptor_.destructive = true;
    descriptor_.input_schema = Json{
        {"type", "object"},
        {"properties",
         {
             {"patch",
              {
                  {"type", "string"},
                  {"description", "Patch text beginning with *** Begin Patch."},
              }},
             {"patchText",
              {
                  {"type", "string"},
                  {"description", "opencode-compatible alias for patch."},
              }},
         }},
        {"required", Json::array({"patch"})},
        {"additionalProperties", false},
    };
}

const ToolDescriptor& ApplyPatchTool::descriptor() const { return descriptor_; }

ToolResult ApplyPatchTool::invoke(const ToolRequest& request,
                                  const ToolContext& context) const {
    if (!is_valid(request)) {
        return make_invalid_request("invalid tool request", request);
    }
    if (request.tool_name != descriptor_.name) {
        return make_invalid_request("tool request name does not match apply_patch",
                                    request);
    }
    if (!has_only_arguments(request, {"patch", "patchText"})) {
        return make_invalid_request("apply_patch accepts only the patch and patchText arguments",
                                    request);
    }
    const bool has_patch = request.arguments.contains("patch");
    const bool has_patch_text = request.arguments.contains("patchText");
    if (has_patch == has_patch_text) {
        return make_invalid_request(
            has_patch ? "apply_patch accepts patch or patchText, not both"
                      : "apply_patch requires a patch or patchText argument",
            request);
    }
    const auto patch = require_string_argument(
        request, has_patch ? "patch" : "patchText", descriptor_.name);
    if (!patch) {
        return make_invalid_request(patch.error().summary(), request);
    }
    const auto operations = parse_patch(patch.value(), context);
    if (!operations) {
        return make_invalid_request(operations.error().summary(), request);
    }

    std::optional<qt::foundation::Path> root;
    if (context.workspace_root) {
        const auto canonical_root = qt::foundation::canonicalize_path(*context.workspace_root);
        if (!canonical_root) {
            return make_execution_error(ToolResultStatus::execution_error,
                                        canonical_root.error().summary(), request);
        }
        root = canonical_root.value();
    }
    for (const auto& operation : operations.value()) {
        if (root &&
            !is_workspace_path_allowed(operation.path, *root, context)) {
            return make_execution_error(ToolResultStatus::permission_denied,
                                        "apply_patch path escapes workspace_root",
                                        request);
        }
    }
    if (auto queued = require_mutation_approval(
            ToolMutationKind::file_edit,
            "apply patch touching " + std::to_string(operations.value().size()) +
                " files",
            request, context)) {
        return *queued;
    }

    std::map<qt::foundation::Path, std::optional<std::string>> planned_contents;
    for (const auto& operation : operations.value()) {
        if (operation.kind == PatchOperationKind::add) {
            std::error_code error;
            if (std::filesystem::exists(operation.path, error)) {
                return make_execution_error(
                    ToolResultStatus::invalid_request,
                    "apply_patch add file already exists: " +
                        qt::foundation::path_for_display(operation.path),
                    request);
            }
            planned_contents[operation.path] = operation.new_text;
            continue;
        }
        const auto current_iterator = planned_contents.find(operation.path);
        std::optional<std::string> current;
        if (current_iterator != planned_contents.end()) {
            current = current_iterator->second;
        } else {
            const auto read = qt::foundation::read_text_file(operation.path);
            if (!read) {
                return make_execution_error(
                    read.error().code == qt::foundation::ErrorCode::not_found
                        ? ToolResultStatus::not_found
                        : ToolResultStatus::execution_error,
                    read.error().summary(), request);
            }
            current = read.value();
        }
        if (operation.kind == PatchOperationKind::remove) {
            planned_contents[operation.path] = std::nullopt;
            continue;
        }
        const std::size_t matches =
            count_occurrences(*current, operation.old_text);
        if (matches != 1U) {
            return make_execution_error(
                matches == 0U ? ToolResultStatus::not_found
                              : ToolResultStatus::invalid_request,
                "apply_patch update hunk matched " + std::to_string(matches) +
                    " locations in " +
                    qt::foundation::path_for_display(operation.path),
                request);
        }
        const auto position = current->find(operation.old_text);
        current->replace(position, operation.old_text.size(), operation.new_text);
        planned_contents[operation.path] = std::move(current);
    }

    Json files = Json::array();
    for (const auto& entry : planned_contents) {
        if (entry.second) {
            const auto status = qt::foundation::write_text_file(entry.first, *entry.second);
            if (!status) {
                return make_execution_error(ToolResultStatus::execution_error,
                                            status.error().summary(), request);
            }
            files.push_back(Json{{"path", qt::foundation::path_for_display(entry.first)},
                                 {"operation", "write"}});
        } else {
            std::error_code error;
            std::filesystem::remove(entry.first, error);
            if (error) {
                return make_execution_error(
                    ToolResultStatus::execution_error,
                    qt::foundation::path_for_display(entry.first) + ": " +
                        error.message(),
                    request);
            }
            files.push_back(Json{{"path", qt::foundation::path_for_display(entry.first)},
                                 {"operation", "delete"}});
        }
    }

    ToolResult result;
    result.status = ToolResultStatus::success;
    result.call_id = request.call_id;
    result.output = Json{{"applied", true},
                         {"file_count", files.size()},
                         {"files", std::move(files)},
                         {"text", "Patch applied"}};
    return result;
}

}  // namespace qt::tools
