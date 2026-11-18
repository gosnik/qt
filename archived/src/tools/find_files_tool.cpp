#include "qt/tools/find_files_tool.hpp"

#include "qt/foundation/filesystem.hpp"
#include "qt/tools/tool_validation.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>
#include <system_error>
#include <vector>

namespace qt::tools {

namespace {

constexpr std::size_t kDefaultLimit = 1000U;

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
        if (index == 0U && glob.rfind("**/", 0U) == 0U) {
            pattern += "(?:.*/)?";
            index = 2U;
            continue;
        }
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

qt::foundation::Result<qt::foundation::Path> resolve_search_path(
    const ToolRequest& request, const ToolContext& context) {
    qt::foundation::Path path = ".";
    if (request.arguments.contains("path")) {
        if (!request.arguments.at("path").is_string()) {
            return qt::foundation::Error{
                qt::foundation::ErrorCode::invalid_argument,
                "find requires a string path argument",
            };
        }
        path = request.arguments.at("path").get<std::string>();
    }

    if (path.is_relative()) {
        if (context.current_directory) {
            path = *context.current_directory / path;
        } else if (context.workspace_root) {
            path = *context.workspace_root / path;
        }
    }

    return qt::foundation::canonicalize_path(path);
}

qt::foundation::Result<std::size_t> positive_size_argument(
    const ToolRequest& request, std::string_view name, std::size_t fallback) {
    const std::string key{name};
    if (!request.arguments.contains(key)) {
        return fallback;
    }
    const Json& value = request.arguments.at(key);
    if (!(value.is_number_integer() || value.is_number_unsigned())) {
        return qt::foundation::Error{
            qt::foundation::ErrorCode::invalid_argument,
            "find requires numeric " + key,
        };
    }
    long long numeric = 0;
    if (value.is_number_unsigned()) {
        const auto unsigned_value = value.get<unsigned long long>();
        if (unsigned_value > static_cast<unsigned long long>(
                                 std::numeric_limits<long long>::max())) {
            return qt::foundation::Error{
                qt::foundation::ErrorCode::invalid_argument,
                "find requires positive " + key,
            };
        }
        numeric = static_cast<long long>(unsigned_value);
    } else {
        numeric = value.get<long long>();
    }
    if (numeric <= 0) {
        return qt::foundation::Error{
            qt::foundation::ErrorCode::invalid_argument,
            "find requires positive " + key,
        };
    }
    return static_cast<std::size_t>(numeric);
}

bool should_skip_directory(const qt::foundation::Path& path) {
    const auto name = path.filename().generic_string();
    return name == ".git" || name == "node_modules";
}

std::vector<std::string> load_root_ignore_patterns(const qt::foundation::Path& root) {
    std::vector<std::string> patterns;
    const auto content = qt::foundation::read_text_file(root / ".gitignore");
    if (!content) {
        return patterns;
    }
    std::size_t offset = 0U;
    while (offset <= content.value().size()) {
        const auto newline = content.value().find('\n', offset);
        std::string line = newline == std::string::npos
                               ? content.value().substr(offset)
                               : content.value().substr(offset, newline - offset);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.erase(line.begin());
        }
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        if (!line.empty() && line.front() != '#') {
            patterns.push_back(std::move(line));
        }
        if (newline == std::string::npos) {
            break;
        }
        offset = newline + 1U;
    }
    return patterns;
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

bool ignore_pattern_matches(const std::string& display_path,
                            const std::string& pattern) {
    std::string normalized = pattern;
    while (!normalized.empty() && normalized.front() == '/') {
        normalized.erase(normalized.begin());
    }
    if (normalized.empty()) {
        return false;
    }
    const bool directory_pattern = normalized.back() == '/';
    if (directory_pattern) {
        normalized.pop_back();
        return display_path == normalized ||
               display_path.rfind(normalized + "/", 0U) == 0U;
    }

    try {
        const auto matcher = glob_to_regex(normalized);
        if (std::regex_match(display_path, matcher)) {
            return true;
        }
        if (normalized.find('/') == std::string::npos) {
            const auto slash = display_path.find_last_of('/');
            const std::string basename =
                slash == std::string::npos ? display_path : display_path.substr(slash + 1U);
            return std::regex_match(basename, matcher);
        }
    } catch (const std::regex_error&) {
        return false;
    }
    return false;
}

bool ignored_by_patterns(const std::string& display_path,
                         const std::vector<std::string>& patterns) {
    bool ignored = false;
    for (const auto& pattern : patterns) {
        if (pattern.empty()) {
            continue;
        }
        const bool negated = pattern.front() == '!';
        const std::string effective_pattern =
            negated ? pattern.substr(1U) : pattern;
        if (effective_pattern.empty()) {
            continue;
        }
        if (ignore_pattern_matches(display_path, effective_pattern)) {
            ignored = !negated;
        }
    }
    return ignored;
}

bool has_hidden_component(const std::string& display_path) {
    std::size_t offset = 0U;
    while (offset < display_path.size()) {
        const auto slash = display_path.find('/', offset);
        const auto end = slash == std::string::npos ? display_path.size() : slash;
        if (end > offset && display_path[offset] == '.') {
            return true;
        }
        if (slash == std::string::npos) {
            break;
        }
        offset = slash + 1U;
    }
    return false;
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::vector<std::string> extensions_for_type(std::string type) {
    type = lowercase_ascii(std::move(type));
    if (type == "cpp" || type == "c++") {
        return {"cc", "cpp", "cxx", "c", "h", "hh", "hpp", "hxx"};
    }
    if (type == "typescript" || type == "ts") {
        return {"ts", "tsx", "mts", "cts"};
    }
    if (type == "javascript" || type == "js") {
        return {"js", "jsx", "mjs", "cjs"};
    }
    if (type == "python" || type == "py") {
        return {"py"};
    }
    if (type == "markdown" || type == "md") {
        return {"md", "markdown"};
    }
    return {std::move(type)};
}

bool path_matches_type(const std::string& display_path,
                       const std::vector<std::string>& extensions) {
    const auto slash = display_path.find_last_of('/');
    const auto basename =
        slash == std::string::npos ? display_path : display_path.substr(slash + 1U);
    const auto dot = basename.find_last_of('.');
    if (dot == std::string::npos || dot + 1U >= basename.size()) {
        return false;
    }
    const auto extension = lowercase_ascii(basename.substr(dot + 1U));
    return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

std::string join_lines(const std::vector<std::string>& lines) {
    std::ostringstream stream;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index > 0U) {
            stream << '\n';
        }
        stream << lines.at(index);
    }
    return stream.str();
}

}  // namespace

FindFilesTool::FindFilesTool() {
    descriptor_.name = "find";
    descriptor_.description =
        "Find files by glob pattern relative to the current workspace directory.";
    descriptor_.input_schema = Json{
        {"type", "object"},
        {"properties",
         {
             {"pattern",
              {
                  {"type", "string"},
                  {"description", "Glob pattern to match files."},
              }},
             {"glob",
              {
                  {"type", "string"},
                  {"description", "Claude/opencode/Crush-compatible alias for pattern."},
              }},
             {"include",
              {
                  {"type", "string"},
                  {"description", "Include glob alias for pattern."},
              }},
             {"type",
              {
                  {"type", "string"},
                  {"description", "Restrict file results by common file type."},
              }},
             {"directories",
              {
                  {"type", "boolean"},
                  {"description", "Match directories as well as files."},
              }},
             {"hidden",
              {
                  {"type", "boolean"},
                  {"description", "Include hidden files and directories."},
              }},
             {"no_ignore",
              {
                  {"type", "boolean"},
                  {"description", "Ignore .gitignore-style exclusion rules."},
              }},
             {"path",
              {
                  {"type", "string"},
                  {"description", "Directory to search. Defaults to current directory."},
              }},
             {"limit",
              {
                  {"type", "integer"},
                  {"description", "Maximum number of results. Defaults to 1000."},
              }},
         }},
        {"required", Json::array({"pattern"})},
        {"additionalProperties", false},
    };
}

const ToolDescriptor& FindFilesTool::descriptor() const { return descriptor_; }

ToolResult FindFilesTool::invoke(const ToolRequest& request,
                                 const ToolContext& context) const {
    if (!is_valid(request)) {
        return make_invalid_request("invalid tool request", request);
    }
    if (request.tool_name != descriptor_.name) {
        return make_invalid_request("tool request name does not match find", request);
    }
    if (!has_only_arguments(request, {"pattern", "glob", "include", "path", "limit",
                                      "type", "directories", "hidden", "no_ignore"})) {
        return make_invalid_request(
            "find accepts only the pattern, glob, include, path, limit, type, directories, hidden, and no_ignore arguments", request);
    }

    std::optional<std::string> pattern_value;
    for (const char* key : {"pattern", "glob", "include"}) {
        if (!request.arguments.contains(key)) {
            continue;
        }
        if (!request.arguments.at(key).is_string()) {
            return make_invalid_request(std::string("find requires a string ") + key + " argument", request);
        }
        const auto value = request.arguments.at(key).get<std::string>();
        if (value.empty()) {
            return make_invalid_request(std::string("find requires a non-empty ") + key + " argument",
                                        request);
        }
        if (pattern_value && *pattern_value != value) {
            return make_invalid_request(
                "find accepts only one distinct pattern/glob/include argument",
                request);
        }
        pattern_value = value;
    }
    if (!pattern_value) {
        return make_invalid_request("find requires a non-empty pattern argument",
                                    request);
    }
    std::optional<std::vector<std::string>> type_extensions;
    if (request.arguments.contains("type")) {
        if (!request.arguments.at("type").is_string()) {
            return make_invalid_request("find requires a string type argument", request);
        }
        type_extensions = extensions_for_type(request.arguments.at("type").get<std::string>());
    }
    bool match_directories = false;
    if (request.arguments.contains("directories")) {
        if (!request.arguments.at("directories").is_boolean()) {
            return make_invalid_request("find requires boolean directories", request);
        }
        match_directories = request.arguments.at("directories").get<bool>();
    }
    bool include_hidden = false;
    if (request.arguments.contains("hidden")) {
        if (!request.arguments.at("hidden").is_boolean()) {
            return make_invalid_request("find requires boolean hidden", request);
        }
        include_hidden = request.arguments.at("hidden").get<bool>();
    }
    bool no_ignore = false;
    if (request.arguments.contains("no_ignore")) {
        if (!request.arguments.at("no_ignore").is_boolean()) {
            return make_invalid_request("find requires boolean no_ignore", request);
        }
        no_ignore = request.arguments.at("no_ignore").get<bool>();
    }

    const auto limit = positive_size_argument(request, "limit", kDefaultLimit);
    if (!limit) {
        return make_invalid_request(limit.error().summary(), request);
    }

    const auto search_path = resolve_search_path(request, context);
    if (!search_path) {
        return make_invalid_request(search_path.error().summary(), request);
    }
    if (context.workspace_root) {
        const auto root = qt::foundation::canonicalize_path(*context.workspace_root);
        if (!root) {
            return make_execution_error(ToolResultStatus::execution_error,
                                        root.error().summary(), request);
        }
        if (!is_workspace_path_allowed(search_path.value(), root.value(),
                                       context)) {
            return make_execution_error(ToolResultStatus::permission_denied,
                                        "find path escapes workspace_root", request);
        }
    }

    std::error_code error;
    if (!std::filesystem::exists(search_path.value(), error)) {
        return make_execution_error(ToolResultStatus::execution_error,
                                    "Path not found: " +
                                        qt::foundation::path_for_display(
                                            search_path.value()),
                                    request);
    }
    if (!std::filesystem::is_directory(search_path.value(), error)) {
        return make_invalid_request("find path must be a directory", request);
    }

    std::regex matcher;
    try {
        matcher = glob_to_regex(*pattern_value);
    } catch (const std::regex_error& regex_error) {
        return make_invalid_request(
            std::string("find invalid glob: ") + regex_error.what(), request);
    }

    std::vector<std::string> matches;
    const auto ignore_patterns =
        no_ignore ? std::vector<std::string>{}
                  : load_root_ignore_patterns(search_path.value());
    bool result_limit_reached = false;
    std::filesystem::recursive_directory_iterator iterator(
        search_path.value(), std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        const auto entry_path = iterator->path();
        const auto display = relative_display_path(entry_path, search_path.value());
        if (!include_hidden && has_hidden_component(display)) {
            if (iterator->is_directory(error)) {
                iterator.disable_recursion_pending();
            }
            iterator.increment(error);
            continue;
        }
        if (iterator->is_directory(error)) {
            const bool ignored_directory =
                should_skip_directory(entry_path) ||
                ignored_by_patterns(display, ignore_patterns);
            if (!ignored_directory && match_directories &&
                (std::regex_match(display, matcher) ||
                 std::regex_match(entry_path.filename().generic_string(), matcher))) {
                matches.push_back(display);
                if (matches.size() >= limit.value()) {
                    result_limit_reached = true;
                    break;
                }
            }
            if (ignored_directory) {
                iterator.disable_recursion_pending();
            }
        } else if (iterator->is_regular_file(error)) {
            if (ignored_by_patterns(display, ignore_patterns)) {
                iterator.increment(error);
                continue;
            }
            if (type_extensions && !path_matches_type(display, *type_extensions)) {
                iterator.increment(error);
                continue;
            }
            if (std::regex_match(display, matcher) ||
                std::regex_match(entry_path.filename().generic_string(), matcher)) {
                matches.push_back(display);
                if (matches.size() >= limit.value()) {
                    result_limit_reached = true;
                    break;
                }
            }
        }
        iterator.increment(error);
    }
    if (error) {
        return make_execution_error(ToolResultStatus::execution_error,
                                    "find failed: " + error.message(), request);
    }

    std::sort(matches.begin(), matches.end());
    const std::string text =
        matches.empty() ? "No files found matching pattern" : join_lines(matches);

    ToolResult result;
    result.status = ToolResultStatus::success;
    result.call_id = request.call_id;
    result.output = Json{
        {"pattern", *pattern_value},
        {"path", qt::foundation::path_for_display(search_path.value())},
        {"count", matches.size()},
        {"matches", matches},
        {"text", text},
        {"directories", match_directories},
        {"hidden", include_hidden},
        {"no_ignore", no_ignore},
    };
    if (request.arguments.contains("type")) {
        result.output["type"] = request.arguments.at("type");
    }
    if (request.arguments.contains("limit")) {
        result.output["limit"] = request.arguments.at("limit");
    }
    if (result_limit_reached) {
        result.output["details"] = Json{{"resultLimitReached", limit.value()}};
        result.output["text"] =
            text + "\n\n[" + std::to_string(limit.value()) +
            " results limit reached]";
    }
    apply_output_guards(result, output_guard_options_from_context(context));
    return result;
}

}  // namespace qt::tools
