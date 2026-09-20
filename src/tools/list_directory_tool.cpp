#include "qt/tools/list_directory_tool.hpp"

#include "qt/foundation/filesystem.hpp"
#include "qt/tools/tool_validation.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <regex>
#include <sstream>
#include <system_error>
#include <vector>

namespace qt::tools {

namespace {

struct DirectoryEntryRecord {
    std::string name;
    qt::foundation::Path path;
    std::string kind;
};

constexpr std::size_t kDefaultListLimit = 500U;

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
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

std::string display_entry_name(const DirectoryEntryRecord& record) {
    return record.kind == "directory" ? record.name + "/" : record.name;
}

std::string regex_escape(char ch) {
    static constexpr std::string_view kSpecial = R"(\.^$|()[]{}+)";
    if (kSpecial.find(ch) != std::string_view::npos) {
        return std::string("\\") + ch;
    }
    return std::string(1U, ch);
}

std::regex glob_to_regex(const std::string& glob) {
    std::string pattern = "^";
    for (std::size_t index = 0; index < glob.size(); ++index) {
        const char ch = glob[index];
        if (ch == '*') {
            if (index + 1U < glob.size() && glob[index + 1U] == '*') {
                pattern += ".*";
                ++index;
            } else {
                pattern += "[^/]*";
            }
        } else if (ch == '?') {
            pattern += "[^/]";
        } else {
            pattern += regex_escape(ch);
        }
    }
    pattern += "$";
    return std::regex(pattern);
}

std::string relative_display_path(const qt::foundation::Path& path,
                                  const qt::foundation::Path& root) {
    std::error_code error;
    const auto relative = std::filesystem::relative(path, root, error);
    if (error || relative.empty()) {
        return path.filename().generic_string();
    }
    return relative.generic_string();
}

bool ignored_by_patterns(const std::string& display_path,
                         const std::string& name,
                         const std::vector<std::regex>& patterns) {
    return std::any_of(patterns.begin(), patterns.end(), [&](const std::regex& pattern) {
        return std::regex_match(display_path, pattern) ||
               std::regex_match(name, pattern);
    });
}

std::string join_display_entries(const std::vector<DirectoryEntryRecord>& records) {
    std::ostringstream output;
    for (std::size_t index = 0; index < records.size(); ++index) {
        if (index > 0U) {
            output << '\n';
        }
        output << display_entry_name(records[index]);
    }
    return output.str();
}

}  // namespace

ListDirectoryTool::ListDirectoryTool() {
    descriptor_.name = "list_directory";
    descriptor_.description =
        "List entries in a directory permitted by the runtime filesystem policy.";
    descriptor_.input_schema = Json{
        {"type", "object"},
        {"properties",
         {
             {"path",
              {
                  {"type", "string"},
                  {"description",
                   "Relative or absolute path to the directory to inspect."},
              }},
             {"limit",
              {
                  {"type", "number"},
                  {"description", "Maximum number of entries to return."},
              }},
             {"recursive",
              {
                  {"type", "boolean"},
                  {"description", "Return a recursive tree instead of only direct children."},
              }},
             {"depth",
              {
                  {"type", "number"},
                  {"description", "Maximum recursive depth when recursive is true."},
              }},
             {"ignore",
              {
                  {"type", "array"},
                  {"description", "Glob patterns to exclude from the listing."},
                  {"items", {{"type", "string"}}},
              }},
         }},
        {"required", Json::array()},
        {"additionalProperties", false},
    };
}

const ToolDescriptor& ListDirectoryTool::descriptor() const { return descriptor_; }

ToolResult ListDirectoryTool::invoke(const ToolRequest& request,
                                     const ToolContext& context) const {
    if (!is_valid(request)) {
        return make_invalid_request("invalid tool request", request);
    }
    if (request.tool_name != descriptor_.name) {
        return make_invalid_request("tool request name does not match list_directory",
                                    request);
    }
    if (!has_only_arguments(request, {"path", "limit", "recursive", "depth", "ignore"})) {
        return make_invalid_request(
            "list_directory accepts only the path, limit, recursive, depth, and ignore arguments", request);
    }

    std::size_t limit = kDefaultListLimit;
    if (request.arguments.contains("limit")) {
        if (!request.arguments.at("limit").is_number_unsigned() &&
            !request.arguments.at("limit").is_number_integer()) {
            return make_invalid_request("list_directory requires numeric limit",
                                        request);
        }
        const auto parsed_limit = request.arguments.at("limit").get<long long>();
        if (parsed_limit <= 0) {
            return make_invalid_request("list_directory requires positive limit",
                                        request);
        }
        limit = static_cast<std::size_t>(parsed_limit);
    }
    bool recursive = false;
    if (request.arguments.contains("recursive")) {
        if (!request.arguments.at("recursive").is_boolean()) {
            return make_invalid_request("list_directory requires boolean recursive",
                                        request);
        }
        recursive = request.arguments.at("recursive").get<bool>();
    }
    std::optional<std::size_t> depth;
    if (request.arguments.contains("depth")) {
        if (!request.arguments.at("depth").is_number_unsigned() &&
            !request.arguments.at("depth").is_number_integer()) {
            return make_invalid_request("list_directory requires numeric depth",
                                        request);
        }
        const auto parsed_depth = request.arguments.at("depth").get<long long>();
        if (parsed_depth < 0) {
            return make_invalid_request("list_directory requires non-negative depth",
                                        request);
        }
        depth = static_cast<std::size_t>(parsed_depth);
        recursive = true;
    }
    std::vector<std::regex> ignore_patterns;
    if (request.arguments.contains("ignore")) {
        if (!request.arguments.at("ignore").is_array()) {
            return make_invalid_request("list_directory requires array ignore",
                                        request);
        }
        for (const auto& pattern : request.arguments.at("ignore")) {
            if (!pattern.is_string()) {
                return make_invalid_request(
                    "list_directory ignore entries must be strings", request);
            }
            try {
                ignore_patterns.push_back(glob_to_regex(pattern.get<std::string>()));
            } catch (const std::regex_error& error) {
                return make_invalid_request(
                    std::string("list_directory invalid ignore glob: ") +
                        error.what(),
                    request);
            }
        }
    }

    ToolRequest path_request = request;
    if (!path_request.arguments.contains("path")) {
        path_request.arguments["path"] = ".";
    }
    const auto resolved_path = resolve_workspace_path(path_request, context, descriptor_.name);
    if (!resolved_path) {
        return make_invalid_request(resolved_path.error().summary(), request);
    }

    if (context.workspace_root) {
        const auto root = qt::foundation::canonicalize_path(*context.workspace_root);
        if (!root) {
            return make_execution_error(ToolResultStatus::execution_error,
                                        root.error().summary(), request);
        }
        if (!is_workspace_path_allowed(resolved_path.value(), root.value(),
                                       context)) {
            return make_execution_error(ToolResultStatus::permission_denied,
                                        "path escapes workspace_root", request);
        }
    }

    std::error_code error;
    const auto status = std::filesystem::status(resolved_path.value(), error);
    if (error) {
        if (error == std::errc::no_such_file_or_directory) {
            return make_execution_error(
                ToolResultStatus::not_found,
                qt::foundation::path_for_display(resolved_path.value()), request);
        }
        return make_execution_error(ToolResultStatus::execution_error,
                                    qt::foundation::path_for_display(resolved_path.value()) +
                                        ": " + error.message(),
                                    request);
    }
    if (!std::filesystem::exists(status)) {
        return make_execution_error(ToolResultStatus::not_found,
                                    qt::foundation::path_for_display(resolved_path.value()),
                                    request);
    }
    if (!std::filesystem::is_directory(status)) {
        return make_execution_error(ToolResultStatus::invalid_request,
                                    qt::foundation::path_for_display(resolved_path.value()) +
                                        " is not a directory",
                                    request);
    }

    std::vector<DirectoryEntryRecord> records;
    if (recursive) {
        for (std::filesystem::recursive_directory_iterator iterator(
                 resolved_path.value(),
                 std::filesystem::directory_options::skip_permission_denied, error);
             !error && iterator != std::filesystem::recursive_directory_iterator();
             ++iterator) {
            const auto& entry = *iterator;
            const auto display =
                relative_display_path(entry.path(), resolved_path.value());
            const auto name = entry.path().filename().generic_string();
            if (ignored_by_patterns(display, name, ignore_patterns)) {
                if (entry.is_directory(error)) {
                    iterator.disable_recursion_pending();
                }
                continue;
            }
            if (depth && static_cast<std::size_t>(iterator.depth()) >= *depth &&
                entry.is_directory(error)) {
                iterator.disable_recursion_pending();
            }
            records.push_back(DirectoryEntryRecord{
                display,
                entry.path(),
                entry_kind(entry),
            });
        }
    } else {
        for (std::filesystem::directory_iterator iterator(resolved_path.value(), error);
             !error && iterator != std::filesystem::directory_iterator(); ++iterator) {
            const auto& entry = *iterator;
            const auto name = entry.path().filename().generic_string();
            if (ignored_by_patterns(name, name, ignore_patterns)) {
                continue;
            }
            records.push_back(DirectoryEntryRecord{
                name,
                entry.path(),
                entry_kind(entry),
            });
        }
    }
    if (error) {
        return make_execution_error(ToolResultStatus::execution_error,
                                    qt::foundation::path_for_display(resolved_path.value()) +
                                        ": " + error.message(),
                                    request);
    }

    std::sort(records.begin(), records.end(),
              [](const DirectoryEntryRecord& lhs, const DirectoryEntryRecord& rhs) {
                  const auto lhs_key = lowercase_ascii(lhs.name);
                  const auto rhs_key = lowercase_ascii(rhs.name);
                  return lhs_key == rhs_key ? lhs.name < rhs.name : lhs_key < rhs_key;
              });

    const bool entry_limit_reached = records.size() > limit;
    if (entry_limit_reached) {
        records.resize(limit);
    }

    Json entries = Json::array();
    for (const auto& record : records) {
        entries.push_back(Json{
            {"name", record.name},
            {"display", display_entry_name(record)},
            {"path", qt::foundation::path_for_display(record.path)},
            {"kind", record.kind},
        });
    }

    ToolResult result;
    result.status = ToolResultStatus::success;
    result.call_id = request.call_id;
    result.output = Json{
        {"path", qt::foundation::path_for_display(resolved_path.value())},
        {"text", records.empty() ? "(empty directory)" : join_display_entries(records)},
        {"entries", std::move(entries)},
        {"count", records.size()},
        {"recursive", recursive},
    };
    if (depth) {
        result.output["depth"] = *depth;
    }
    if (entry_limit_reached) {
        result.output["details"] = Json{{"entryLimitReached", limit}};
    }
    apply_output_guards(result, output_guard_options_from_context(context));
    return result;
}

}  // namespace qt::tools
