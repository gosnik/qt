#include "qt/tools/search_text_tool.hpp"

#include "qt/foundation/filesystem.hpp"
#include "qt/foundation/process.hpp"
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

enum class SearchScope {
    single_file,
    recursive_directory,
};

enum class SearchTextOutputMode {
    content,
    files_with_matches,
    count,
};

std::size_t count_matches(const std::string& text, const std::string& query) {
    if (query.empty()) {
        return 0;
    }

    std::size_t count = 0;
    std::size_t offset = 0;
    while (true) {
        const auto position = text.find(query, offset);
        if (position == std::string::npos) {
            return count;
        }
        ++count;
        offset = position + query.size();
    }
}

std::size_t count_regex_matches(const std::string& text, const std::regex& pattern) {
    return static_cast<std::size_t>(
        std::distance(std::sregex_iterator(text.begin(), text.end(), pattern),
                      std::sregex_iterator()));
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t offset = 0U;
    while (offset <= text.size()) {
        const auto newline = text.find('\n', offset);
        if (newline == std::string_view::npos) {
            lines.emplace_back(text.substr(offset));
            break;
        }
        lines.emplace_back(text.substr(offset, newline - offset));
        offset = newline + 1U;
    }
    return lines;
}

std::string display_path_for_match(const qt::foundation::Path& file,
                                   const qt::foundation::Path& search_root,
                                   SearchScope scope) {
    if (scope == SearchScope::single_file) {
        return file.filename().generic_string();
    }
    std::error_code error;
    auto relative = std::filesystem::relative(file, search_root, error);
    if (error || relative.empty()) {
        return file.filename().generic_string();
    }
    return relative.generic_string();
}

bool glob_matches_display_path(const std::string& display_path,
                               const std::string& glob,
                               const std::regex& matcher) {
    if (std::regex_match(display_path, matcher)) {
        return true;
    }
    // Ripgrep treats a glob without a slash as a basename filter, so '*.ts'
    // also matches nested files. Preserve that behavior when the native
    // fallback or the post-ripgrep compatibility filter checks the path.
    if (glob.find('/') == std::string::npos) {
        const auto slash = display_path.find_last_of('/');
        const std::string basename =
            slash == std::string::npos ? display_path : display_path.substr(slash + 1U);
        return std::regex_match(basename, matcher);
    }
    return false;
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
        if ((index == 0U || glob[index - 1U] == '/') &&
            glob.compare(index, 3U, "**/") == 0) {
            // A **/ path component also matches zero directories (for
            // example, both **/*.ts and src/**/*.ts match files directly
            // under the corresponding search root), matching
            // ripgrep/minimatch semantics.
            pattern += "(?:.*/)?";
            index += 2U;
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
    if (type == "rust" || type == "rs") {
        return {"rs"};
    }
    if (type == "markdown" || type == "md") {
        return {"md", "markdown"};
    }
    if (type == "json") {
        return {"json", "jsonl"};
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

std::optional<std::vector<qt::foundation::Path>> ripgrep_file_list(
    const qt::foundation::Path& root,
    const std::optional<std::string>& glob,
    bool include_hidden,
    bool no_ignore) {
    qt::foundation::ProcessRequest request;
    request.mode = qt::foundation::ProcessLaunchMode::argv;
    request.cwd = root;
    request.timeout_ms = 10000;
    request.argv = {"rg", "--files"};
    if (include_hidden) {
        request.argv.push_back("--hidden");
    }
    if (no_ignore) {
        request.argv.push_back("--no-ignore");
    }
    if (glob) {
        request.argv.push_back("--glob");
        request.argv.push_back(*glob);
    }
    request.argv.push_back(".");

    const auto result = qt::foundation::run_process(request);
    if (!result || result.value().timed_out || result.value().exit_code != 0) {
        return std::nullopt;
    }

    std::vector<qt::foundation::Path> files;
    std::istringstream stream(result.value().stdout_text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (line.rfind("./", 0U) == 0U) {
            line.erase(0U, 2U);
        }
        const auto candidate = qt::foundation::canonicalize_path(root / line);
        if (candidate) {
            files.push_back(candidate.value());
        }
    }
    return files;
}

}  // namespace

SearchTextTool::SearchTextTool() {
    descriptor_.name = "search_text";
    descriptor_.description =
        "Search for an exact substring in a file or recursively across files in a directory.";
    descriptor_.input_schema = Json{
        {"type", "object"},
        {"properties",
         {
             {"path",
              {
                  {"type", "string"},
                  {"description", "Relative or absolute file or directory path to search."},
              }},
             {"query",
              {
                  {"type", "string"},
                  {"description", "Legacy exact substring to search for."},
              }},
             {"pattern",
              {
                  {"type", "string"},
                  {"description", "Regex pattern to search for."},
              }},
             {"limit",
              {
                  {"type", "number"},
                  {"description", "Maximum number of matches to return."},
              }},
             {"context",
              {
                  {"type", "number"},
                  {"description", "Number of context lines before and after each match."},
              }},
             {"ignoreCase",
              {
                  {"type", "boolean"},
                  {"description", "Case-insensitive search."},
              }},
             {"literal",
              {
                  {"type", "boolean"},
                  {"description", "Treat pattern as a literal string."},
              }},
             {"literal_text",
              {
                  {"type", "boolean"},
                  {"description", "Crush-compatible alias for literal."},
              }},
             {"glob",
              {
                  {"type", "string"},
                  {"description", "Filter searched files by glob pattern."},
              }},
             {"file_glob",
              {
                  {"type", "string"},
                  {"description", "Hermes-compatible alias for glob."},
              }},
             {"include",
              {
                  {"type", "string"},
                  {"description", "Glob include filter alias used by several references."},
              }},
             {"type",
              {
                  {"type", "string"},
                  {"description", "Restrict searched files by a common language/file type."},
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
             {"output_mode",
              {
                  {"type", "string"},
                  {"enum", Json::array({"content", "files_with_matches", "files_only", "count"})},
                  {"description",
                   "Output content matches, only the file paths with matches, or per-file match counts."},
              }},
             {"head_limit",
              {
                  {"type", "number"},
                  {"description", "Limit output entries; 0 means unlimited."},
              }},
             {"offset",
              {
                  {"type", "number"},
                  {"description", "Skip the first N output entries."},
              }},
             {"-A",
              {
                  {"type", "number"},
                  {"description", "Number of context lines after each match."},
              }},
             {"-B",
              {
                  {"type", "number"},
                  {"description", "Number of context lines before each match."},
              }},
             {"-C",
              {
                  {"type", "number"},
                  {"description", "Alias for context."},
              }},
             {"-n",
              {
                  {"type", "boolean"},
                  {"description", "Show line numbers in content output."},
              }},
             {"-i",
              {
                  {"type", "boolean"},
                  {"description", "Alias for ignoreCase."},
              }},
             {"multiline",
              {
                  {"type", "boolean"},
                  {"description", "Accept multiline regex mode requests."},
              }},
         }},
        {"required", Json::array({"path"})},
        {"additionalProperties", false},
    };
}

const ToolDescriptor& SearchTextTool::descriptor() const { return descriptor_; }

ToolResult SearchTextTool::invoke(const ToolRequest& request,
                                  const ToolContext& context) const {
    if (!is_valid(request)) {
        return make_invalid_request("invalid tool request", request);
    }
    if (request.tool_name != descriptor_.name) {
        return make_invalid_request("tool request name does not match search_text",
                                    request);
    }
    if (!has_only_arguments(request, {"path", "query", "pattern", "limit", "context",
                                      "ignoreCase", "literal", "literal_text", "glob", "file_glob",
                                      "include", "type", "hidden", "no_ignore", "output_mode",
                                      "head_limit", "offset", "-A", "-B", "-C", "-n",
                                      "-i", "multiline"})) {
        return make_invalid_request(
            "search_text accepts only the path, query, pattern, limit, context, ignoreCase, literal, literal_text, glob, file_glob, include, type, hidden, no_ignore, output_mode, head_limit, offset, -A, -B, -C, -n, -i, and multiline arguments",
            request);
    }

    ToolRequest path_request = request;
    if (!path_request.arguments.contains("path")) {
        path_request.arguments["path"] = ".";
    }
    const auto resolved_path =
        resolve_workspace_path(path_request, context, descriptor_.name);
    if (!resolved_path) {
        return make_invalid_request(resolved_path.error().summary(), request);
    }

    std::optional<qt::foundation::Path> workspace_root;
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
        workspace_root = root.value();
    }

    const auto non_empty_string_argument =
        [&](const char* name) -> qt::foundation::Result<bool> {
        if (!request.arguments.contains(name)) {
            return qt::foundation::Result<bool>::success(false);
        }
        if (!request.arguments.at(name).is_string()) {
            return qt::foundation::Result<bool>::failure(
                qt::foundation::ErrorCode::invalid_argument,
                "search_text requires string " + std::string(name));
        }
        return qt::foundation::Result<bool>::success(
            !request.arguments.at(name).get<std::string>().empty());
    };
    const auto query_present = non_empty_string_argument("query");
    if (!query_present) {
        return make_invalid_request(query_present.error().summary(), request);
    }
    const auto pattern_present = non_empty_string_argument("pattern");
    if (!pattern_present) {
        return make_invalid_request(pattern_present.error().summary(), request);
    }
    const bool has_query = query_present.value();
    const bool has_pattern = pattern_present.value();
    if (has_query == has_pattern) {
        return make_invalid_request(
            has_query ? "search_text requires exactly one of query or pattern"
                      : "search_text requires a non-empty search pattern",
            request);
    }
    const auto raw_pattern = require_string_argument(
        request, has_query ? "query" : "pattern", descriptor_.name);
    if (!raw_pattern) {
        return make_invalid_request(raw_pattern.error().summary(), request);
    }
    if (raw_pattern.value().empty()) {
        return make_invalid_request("search_text requires a non-empty search pattern",
                                    request);
    }
    bool ignore_case = false;
    if (request.arguments.contains("ignoreCase")) {
        if (!request.arguments.at("ignoreCase").is_boolean()) {
            return make_invalid_request(
                "search_text requires boolean ignoreCase", request);
        }
        ignore_case = request.arguments.at("ignoreCase").get<bool>();
    }
    if (request.arguments.contains("-i")) {
        if (!request.arguments.at("-i").is_boolean()) {
            return make_invalid_request("search_text requires boolean -i", request);
        }
        ignore_case = request.arguments.at("-i").get<bool>();
    }
    if (request.arguments.contains("-n") &&
        !request.arguments.at("-n").is_boolean()) {
        return make_invalid_request("search_text requires boolean -n", request);
    }
    if (request.arguments.contains("multiline") &&
        !request.arguments.at("multiline").is_boolean()) {
        return make_invalid_request("search_text requires boolean multiline", request);
    }
    bool literal = has_query;
    if (request.arguments.contains("literal")) {
        if (!request.arguments.at("literal").is_boolean()) {
            return make_invalid_request("search_text requires boolean literal", request);
        }
        literal = request.arguments.at("literal").get<bool>();
    }
    if (request.arguments.contains("literal_text")) {
        if (!request.arguments.at("literal_text").is_boolean()) {
            return make_invalid_request("search_text requires boolean literal_text", request);
        }
        literal = request.arguments.at("literal_text").get<bool>();
    }
    const std::string match_query =
        ignore_case && literal ? lowercase_ascii(raw_pattern.value())
                               : raw_pattern.value();
    std::optional<std::regex> regex_pattern;
    if (!literal) {
        try {
            regex_pattern = std::regex(
                raw_pattern.value(),
                std::regex::ECMAScript |
                    (ignore_case ? std::regex::icase : std::regex::flag_type{}));
        } catch (const std::regex_error& error) {
            return make_invalid_request(
                std::string("search_text invalid pattern: ") + error.what(), request);
        }
    }
    std::optional<std::regex> glob_filter;
    std::optional<std::string> glob_value;
    for (const char* key : {"glob", "file_glob", "include"}) {
        if (!request.arguments.contains(key)) {
            continue;
        }
        if (!request.arguments.at(key).is_string()) {
            return make_invalid_request(std::string("search_text requires string ") + key, request);
        }
        const auto glob = request.arguments.at(key).get<std::string>();
        if (glob.empty()) {
            return make_invalid_request(std::string("search_text requires non-empty ") + key, request);
        }
        if (glob_value && *glob_value != glob) {
            return make_invalid_request(
                "search_text accepts only one distinct glob/file_glob/include filter",
                request);
        }
        glob_value = glob;
    }
    if (glob_value) {
        try {
            glob_filter = glob_to_regex(*glob_value);
        } catch (const std::regex_error& error) {
            return make_invalid_request(
                std::string("search_text invalid glob: ") + error.what(), request);
        }
    }
    std::optional<std::vector<std::string>> type_extensions;
    if (request.arguments.contains("type")) {
        if (!request.arguments.at("type").is_string()) {
            return make_invalid_request("search_text requires string type", request);
        }
        const auto type = request.arguments.at("type").get<std::string>();
        if (type.empty()) {
            return make_invalid_request("search_text requires non-empty type", request);
        }
        type_extensions = extensions_for_type(type);
    }
    bool include_hidden = false;
    if (request.arguments.contains("hidden")) {
        if (!request.arguments.at("hidden").is_boolean()) {
            return make_invalid_request("search_text requires boolean hidden", request);
        }
        include_hidden = request.arguments.at("hidden").get<bool>();
    }
    bool no_ignore = false;
    if (request.arguments.contains("no_ignore")) {
        if (!request.arguments.at("no_ignore").is_boolean()) {
            return make_invalid_request("search_text requires boolean no_ignore", request);
        }
        no_ignore = request.arguments.at("no_ignore").get<bool>();
    }
    SearchTextOutputMode output_mode = SearchTextOutputMode::content;
    if (request.arguments.contains("output_mode")) {
        if (!request.arguments.at("output_mode").is_string()) {
            return make_invalid_request("search_text requires string output_mode", request);
        }
        const auto value = request.arguments.at("output_mode").get<std::string>();
        if (value == "content") {
            output_mode = SearchTextOutputMode::content;
        } else if (value == "files_with_matches") {
            output_mode = SearchTextOutputMode::files_with_matches;
        } else if (value == "files_only") {
            output_mode = SearchTextOutputMode::files_with_matches;
        } else if (value == "count") {
            output_mode = SearchTextOutputMode::count;
        } else {
            return make_invalid_request(
                "search_text output_mode must be content, files_with_matches, files_only, or count",
                request);
        }
    }
    std::size_t limit = 100U;
    std::size_t offset = 0U;
    std::size_t before_context_lines = 0U;
    std::size_t after_context_lines = 0U;
    const auto read_size_argument =
        [&](const char* key, bool allow_zero) -> qt::foundation::Result<std::size_t> {
        if (!request.arguments.at(key).is_number_unsigned() &&
            !request.arguments.at(key).is_number_integer()) {
            return qt::foundation::Result<std::size_t>::failure(
                qt::foundation::ErrorCode::invalid_argument,
                std::string("search_text requires numeric ") + key);
        }
        const auto value = request.arguments.at(key).get<long long>();
        if (value < 0 || (!allow_zero && value == 0)) {
            return qt::foundation::Result<std::size_t>::failure(
                qt::foundation::ErrorCode::invalid_argument,
                std::string("search_text requires ") +
                    (allow_zero ? "non-negative " : "positive ") + key);
        }
        return qt::foundation::Result<std::size_t>::success(
            static_cast<std::size_t>(value));
    };
    if (request.arguments.contains("limit")) {
        const auto parsed = read_size_argument("limit", false);
        if (!parsed) {
            return make_invalid_request(parsed.error().summary(), request);
        }
        limit = parsed.value();
    }
    if (request.arguments.contains("head_limit")) {
        const auto parsed = read_size_argument("head_limit", true);
        if (!parsed) {
            return make_invalid_request(parsed.error().summary(), request);
        }
        limit = parsed.value();
    }
    if (request.arguments.contains("offset")) {
        const auto parsed = read_size_argument("offset", true);
        if (!parsed) {
            return make_invalid_request(parsed.error().summary(), request);
        }
        offset = parsed.value();
    }
    const auto set_symmetric_context = [&](const char* key) -> bool {
        if (!request.arguments.contains(key)) {
            return true;
        }
        const auto parsed = read_size_argument(key, true);
        if (!parsed) {
            return false;
        }
        before_context_lines = parsed.value();
        after_context_lines = parsed.value();
        return true;
    };
    if (!set_symmetric_context("context")) {
        return make_invalid_request("search_text requires non-negative context", request);
    }
    if (!set_symmetric_context("-C")) {
        return make_invalid_request("search_text requires non-negative -C", request);
    }
    if (request.arguments.contains("-B")) {
        const auto parsed = read_size_argument("-B", true);
        if (!parsed) {
            return make_invalid_request(parsed.error().summary(), request);
        }
        before_context_lines = parsed.value();
    }
    if (request.arguments.contains("-A")) {
        const auto parsed = read_size_argument("-A", true);
        if (!parsed) {
            return make_invalid_request(parsed.error().summary(), request);
        }
        after_context_lines = parsed.value();
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

    std::vector<qt::foundation::Path> files;
    SearchScope search_scope = SearchScope::single_file;

    if (std::filesystem::is_regular_file(status)) {
        files.push_back(resolved_path.value());
    } else if (std::filesystem::is_directory(status)) {
        search_scope = SearchScope::recursive_directory;
        if (const auto rg_files =
                ripgrep_file_list(resolved_path.value(), glob_value, include_hidden,
                                  no_ignore)) {
            for (const auto& file : *rg_files) {
                if (workspace_root &&
                    !is_workspace_path_allowed(file, *workspace_root,
                                               context)) {
                    return make_execution_error(
                        ToolResultStatus::permission_denied,
                        "path escapes workspace_root", request);
                }
                files.push_back(file);
            }
        } else {
            const auto ignore_patterns =
                no_ignore ? std::vector<std::string>{}
                          : load_root_ignore_patterns(resolved_path.value());
            for (std::filesystem::recursive_directory_iterator iterator(
                     resolved_path.value(),
                     std::filesystem::directory_options::skip_permission_denied,
                     error);
                 !error && iterator != std::filesystem::recursive_directory_iterator();
                 ++iterator) {
                const auto& entry = *iterator;
                std::error_code entry_error;
                const auto display_path =
                    display_path_for_match(entry.path(), resolved_path.value(),
                                           SearchScope::recursive_directory);
                if (!include_hidden && has_hidden_component(display_path)) {
                    if (entry.is_directory(entry_error)) {
                        iterator.disable_recursion_pending();
                    }
                    continue;
                }
                if (entry.is_directory(entry_error) &&
                    entry.path().filename().generic_string() == ".git") {
                    iterator.disable_recursion_pending();
                    continue;
                }
                if (entry.is_directory(entry_error) &&
                    ignored_by_patterns(display_path, ignore_patterns)) {
                    iterator.disable_recursion_pending();
                    continue;
                }
                if (!entry.is_regular_file(entry_error)) {
                    if (entry_error) {
                        return make_execution_error(
                            ToolResultStatus::execution_error,
                            qt::foundation::path_for_display(entry.path()) + ": " +
                                entry_error.message(),
                            request);
                    }
                    continue;
                }
                if (ignored_by_patterns(display_path, ignore_patterns)) {
                    continue;
                }

                const auto candidate = qt::foundation::canonicalize_path(entry.path());
                if (!candidate) {
                    return make_execution_error(ToolResultStatus::execution_error,
                                                candidate.error().summary(), request);
                }
                if (workspace_root &&
                    !is_workspace_path_allowed(candidate.value(),
                                               *workspace_root, context)) {
                    return make_execution_error(ToolResultStatus::permission_denied,
                                                "path escapes workspace_root", request);
                }
                files.push_back(candidate.value());
            }
            if (error) {
                return make_execution_error(
                    ToolResultStatus::execution_error,
                    qt::foundation::path_for_display(resolved_path.value()) +
                        ": " + error.message(),
                    request);
            }
        }
    } else {
        return make_execution_error(ToolResultStatus::invalid_request,
                                    qt::foundation::path_for_display(resolved_path.value()) +
                                        " is not a file or directory",
                                    request);
    }

    std::sort(files.begin(), files.end(),
              [](const qt::foundation::Path& lhs, const qt::foundation::Path& rhs) {
                  return qt::foundation::path_for_display(lhs) <
                         qt::foundation::path_for_display(rhs);
              });

    Json matches = Json::array();
    std::vector<std::string> output_lines;
    std::size_t total_match_count = 0U;
    std::size_t seen_entry_count = 0U;
    std::size_t emitted_entry_count = 0U;
    bool match_limit_reached = false;
    for (const auto& file : files) {
        const std::string display_path =
            display_path_for_match(file, resolved_path.value(), search_scope);
        if (glob_filter &&
            !glob_matches_display_path(display_path, *glob_value, *glob_filter)) {
            continue;
        }
        if (type_extensions && !path_matches_type(display_path, *type_extensions)) {
            continue;
        }
        const auto text = qt::foundation::read_text_file(file);
        if (!text) {
            ToolResultStatus result_status = ToolResultStatus::execution_error;
            if (text.error().code == qt::foundation::ErrorCode::not_found) {
                result_status = ToolResultStatus::not_found;
            }
            return make_execution_error(result_status, text.error().summary(), request);
        }

        const std::string match_text =
            ignore_case ? lowercase_ascii(text.value()) : text.value();
        const auto match_count = regex_pattern
                                     ? count_regex_matches(text.value(), *regex_pattern)
                                     : count_matches(match_text, match_query);
        if (match_count == 0U) {
            continue;
        }
        total_match_count += match_count;

        if (output_mode == SearchTextOutputMode::files_with_matches ||
            output_mode == SearchTextOutputMode::count) {
            if (seen_entry_count++ < offset) {
                continue;
            }
            if (limit != 0U && emitted_entry_count >= limit) {
                match_limit_reached = true;
                break;
            }
            matches.push_back(Json{
                {"path", display_path},
                {"match_count", match_count},
            });
            if (output_mode == SearchTextOutputMode::files_with_matches) {
                output_lines.push_back(display_path);
            } else {
                output_lines.push_back(display_path + ":" +
                                       std::to_string(match_count));
            }
            ++emitted_entry_count;
        } else {
            matches.push_back(Json{
                {"path", display_path},
                {"match_count", match_count},
            });
            const auto lines = split_lines(text.value());
            const auto searchable_lines = split_lines(match_text);
            for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
                const bool line_matches =
                    regex_pattern
                        ? std::regex_search(lines[line_index], *regex_pattern)
                        : line_index < searchable_lines.size() &&
                              searchable_lines[line_index].find(match_query) !=
                                  std::string::npos;
                if (!line_matches) {
                    continue;
                }
                if (seen_entry_count++ < offset) {
                    continue;
                }
                if (limit != 0U && emitted_entry_count >= limit) {
                    match_limit_reached = true;
                    break;
                }
                const std::size_t begin =
                    line_index > before_context_lines
                        ? line_index - before_context_lines
                        : 0U;
                const std::size_t end =
                    std::min(lines.size() - 1U,
                             line_index + after_context_lines);
                for (std::size_t current = begin; current <= end; ++current) {
                    std::ostringstream row;
                    if (current == line_index) {
                        row << display_path << ":" << (current + 1U) << ": "
                            << lines[current];
                    } else {
                        row << display_path << "-" << (current + 1U) << "- "
                            << lines[current];
                    }
                    output_lines.push_back(row.str());
                }
                ++emitted_entry_count;
            }
        }
        if (match_limit_reached) {
            break;
        }
    }

    std::string output_text;
    if (output_lines.empty()) {
        output_text = "No matches found";
    } else {
        for (std::size_t index = 0; index < output_lines.size(); ++index) {
            if (index > 0U) {
                output_text += '\n';
            }
            output_text += output_lines[index];
        }
        if (match_limit_reached) {
            output_text += "\n\n[" + std::to_string(limit) +
                           " matches limit reached. Use limit=" +
                           std::to_string(limit * 2U) +
                           " for more, or refine pattern]";
        }
    }

    ToolResult result;
    result.status = ToolResultStatus::success;
    result.call_id = request.call_id;
    result.output = Json{
        {"path", qt::foundation::path_for_display(resolved_path.value())},
        {"query", raw_pattern.value()},
        {"pattern", raw_pattern.value()},
        {"literal", literal},
        {"ignore_case", ignore_case},
        {"glob", glob_value ? Json(*glob_value) : Json(nullptr)},
        {"type", request.arguments.contains("type") ? request.arguments.at("type") : Json(nullptr)},
        {"hidden", include_hidden},
        {"no_ignore", no_ignore},
        {"output_mode", output_mode == SearchTextOutputMode::content
                            ? Json("content")
                            : output_mode == SearchTextOutputMode::files_with_matches
                                  ? Json("files_with_matches")
                                  : Json("count")},
        {"offset", offset},
        {"recursive", search_scope == SearchScope::recursive_directory},
        {"files_searched", files.size()},
        {"count", matches.size()},
        {"match_count", total_match_count},
        {"text", output_text},
        {"matches", std::move(matches)},
    };
    if (match_limit_reached) {
        result.output["details"] = Json{{"matchLimitReached", limit}};
    }
    apply_output_guards(result, output_guard_options_from_context(context));
    return result;
}

}  // namespace qt::tools
