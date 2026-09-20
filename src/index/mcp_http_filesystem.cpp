#include "index/mcp_http_filesystem.hpp"

#include "index/path_util.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace qt::index {
namespace {

using json = nlohmann::json;

std::size_t query_size(const HttpRequest& request, const std::string& key, std::size_t fallback) {
    const auto found = request.query.find(key);
    if (found == request.query.end() || found->second.empty()) {
        return fallback;
    }
    return static_cast<std::size_t>(std::strtoull(found->second.c_str(), nullptr, 10));
}

bool query_bool(const HttpRequest& request, const std::string& key, bool fallback) {
    const auto found = request.query.find(key);
    if (found == request.query.end() || found->second.empty()) {
        return fallback;
    }
    return found->second != "0" && found->second != "false";
}

std::filesystem::path relative_project_path(const std::string& value) {
    const auto relative = std::filesystem::path{value}.lexically_normal();
    if (relative.empty() || relative.is_absolute()) {
        throw std::invalid_argument("file must be a relative path inside the project");
    }
    for (const auto& part : relative) {
        if (part == "..") {
            throw std::invalid_argument("file must not contain '..'");
        }
        if (part == ".git") {
            throw std::invalid_argument("path must not reference .git");
        }
    }
    return relative;
}

std::filesystem::path requested_relative_file(const HttpRequest& request) {
    const auto file_it = request.query.find("file");
    if (file_it == request.query.end() || file_it->second.empty()) {
        throw std::invalid_argument("file query parameter is required");
    }
    return relative_project_path(file_it->second);
}

std::filesystem::path requested_relative_path(const HttpRequest& request,
                                              const std::string& key,
                                              const std::string& fallback) {
    const auto found = request.query.find(key);
    return relative_project_path(found == request.query.end() || found->second.empty() ? fallback
                                                                                       : found->second);
}

std::filesystem::path checked_project_path(const BackgroundService& service,
                                           const std::filesystem::path& relative) {
    const auto root = std::filesystem::weakly_canonical(service.project_root());
    const auto full = std::filesystem::weakly_canonical((root / relative).lexically_normal());
    const auto root_text = root.generic_string();
    const auto full_text = full.generic_string();
    if (full_text != root_text && full_text.rfind(root_text + "/", 0) != 0) {
        throw std::invalid_argument("path escapes project root");
    }
    return full;
}

json filesystem_entry_payload(const BackgroundService& service,
                              const std::filesystem::path& relative) {
    const auto full = checked_project_path(service, relative);
    std::error_code ec;
    const bool is_directory = std::filesystem::is_directory(full, ec);
    const bool is_regular = std::filesystem::is_regular_file(full, ec);
    json entry{{"path", relative.generic_string()},
               {"name", relative.filename().string()},
               {"exists", std::filesystem::exists(full, ec)},
               {"type", is_directory ? "directory" : (is_regular ? "file" : "other")}};
    if (is_regular) {
        const auto size = std::filesystem::file_size(full, ec);
        if (!ec) {
            entry["size"] = size;
        }
    }
    return entry;
}

}  // namespace

json filesystem_stat_payload(const BackgroundService& service, const HttpRequest& request) {
    const auto relative = requested_relative_path(request, "path", ".");
    auto entry = filesystem_entry_payload(service, relative);
    entry["query"] = request.query;
    return entry;
}

json filesystem_list_payload(const BackgroundService& service, const HttpRequest& request) {
    const auto relative = requested_relative_path(request, "path", ".");
    const auto full = checked_project_path(service, relative);
    if (!std::filesystem::is_directory(full)) {
        throw std::invalid_argument("path must be a directory");
    }
    const bool recursive = query_bool(request, "recursive", false);
    const auto max_depth = query_size(request, "depth", recursive ? 4 : 1);
    const auto limit = query_size(request, "limit", 200);
    json entries = json::array();
    std::error_code ec;
    if (recursive) {
        for (std::filesystem::recursive_directory_iterator it(full, std::filesystem::directory_options::skip_permission_denied, ec), end;
             !ec && it != end;
             it.increment(ec)) {
            if (it->is_directory(ec) && should_skip_search_directory(it->path())) {
                it.disable_recursion_pending();
                continue;
            }
            if (it.depth() + 1 > static_cast<int>(max_depth)) {
                it.disable_recursion_pending();
                continue;
            }
            const auto child = std::filesystem::relative(it->path(), service.project_root(), ec);
            if (!ec) {
                entries.push_back(filesystem_entry_payload(service, child));
            }
            if (limit != 0 && entries.size() >= limit) {
                break;
            }
        }
    } else {
        for (std::filesystem::directory_iterator it(full, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_directory(ec) && should_skip_search_directory(it->path())) {
                continue;
            }
            const auto child = std::filesystem::relative(it->path(), service.project_root(), ec);
            if (!ec) {
                entries.push_back(filesystem_entry_payload(service, child));
            }
            if (limit != 0 && entries.size() >= limit) {
                break;
            }
        }
    }
    return {{"path", relative.generic_string()},
            {"recursive", recursive},
            {"depth", max_depth},
            {"entries", entries},
            {"query", request.query}};
}

json filesystem_tree_payload(const BackgroundService& service, const HttpRequest& request) {
    if (request.query.empty()) {
        throw std::invalid_argument("filesystem tree requires at least one query parameter");
    }
    HttpRequest tree_request = request;
    tree_request.query["recursive"] = "1";
    if (tree_request.query.find("depth") == tree_request.query.end()) {
        tree_request.query["depth"] = "3";
    }
    return filesystem_list_payload(service, tree_request);
}

json filesystem_read_payload(const BackgroundService& service, const HttpRequest& request) {
    const auto relative = requested_relative_file(request);
    const auto full = checked_project_path(service, relative);
    if (!std::filesystem::is_regular_file(full)) {
        throw std::invalid_argument("file must be a regular file");
    }
    const auto start_line = query_size(request, "start_line", 1);
    const auto end_line = query_size(request, "end_line", 0);
    const auto max_lines = query_size(request, "max_lines", 500);
    if (start_line == 0) {
        throw std::invalid_argument("start_line must be one-based");
    }
    std::ifstream input(full);
    if (!input) {
        throw std::runtime_error("could not open file: " + relative.generic_string());
    }
    json lines = json::array();
    std::string text;
    std::size_t line_number = 0;
    bool truncated = false;
    while (std::getline(input, text)) {
        ++line_number;
        if (line_number < start_line) {
            continue;
        }
        if (end_line != 0 && line_number > end_line) {
            break;
        }
        if (max_lines != 0 && lines.size() >= max_lines) {
            truncated = true;
            break;
        }
        lines.push_back({{"line", line_number}, {"text", text}});
    }
    return {{"file", relative.generic_string()},
            {"start_line", start_line},
            {"end_line", end_line == 0 ? line_number : end_line},
            {"lines", lines},
            {"truncated", truncated},
            {"query", request.query}};
}

json filesystem_search_payload(const BackgroundService& service, const HttpRequest& request) {
    const auto pattern_it = request.query.find("pattern");
    if (pattern_it == request.query.end() || pattern_it->second.empty()) {
        throw std::invalid_argument("filesystem search requires pattern query parameter");
    }
    const auto base = requested_relative_path(request, "path", ".");
    const auto base_full = checked_project_path(service, base);
    const auto limit = query_size(request, "limit", 100);
    json matches = json::array();
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(base_full, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end;
         it.increment(ec)) {
        if (it->is_directory(ec) && should_skip_search_directory(it->path())) {
            it.disable_recursion_pending();
            continue;
        }
        const auto relative = std::filesystem::relative(it->path(), service.project_root(), ec);
        if (ec) {
            continue;
        }
        const auto path_text = relative.generic_string();
        if (path_text.find(pattern_it->second) == std::string::npos) {
            continue;
        }
        matches.push_back(filesystem_entry_payload(service, relative));
        if (limit != 0 && matches.size() >= limit) {
            break;
        }
    }
    return {{"path", base.generic_string()}, {"matches", matches}, {"query", request.query}};
}

}  // namespace qt::index
