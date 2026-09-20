#include "index/mcp_http_code_index.hpp"

#include "index/path_util.hpp"
#include "index/string_util.hpp"

#include <nlohmann/json.hpp>

#include <boost/algorithm/string.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <queue>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

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

std::size_t query_limit(const HttpRequest& request) {
    const auto found = request.query.find("limit");
    if (found == request.query.end() || found->second.empty()) {
        return 0;
    }
    return static_cast<std::size_t>(std::strtoull(found->second.c_str(), nullptr, 10));
}

json limited_array(json values, const HttpRequest& request) {
    const auto limit = query_limit(request);
    if (limit == 0 || values.size() <= limit) {
        return values;
    }
    json limited = json::array();
    for (std::size_t index = 0; index < limit; ++index) {
        limited.push_back(values[index]);
    }
    return limited;
}

bool contains_filter(const json& object, const std::string& key, const HttpRequest& request) {
    const auto wanted = request.query.find(key);
    if (wanted == request.query.end() || wanted->second.empty()) {
        return true;
    }
    const auto value = object.find(key);
    return value != object.end() && value->is_string() &&
           value->get<std::string>().find(wanted->second) != std::string::npos;
}

bool contains_text_filter(const std::string& value, const std::string& wanted) {
    return wanted.empty() || value.find(wanted) != std::string::npos;
}

bool path_prefix_matches(const std::string& value, const std::string& prefix) {
    if (prefix.empty() || prefix == ".") {
        return true;
    }
    std::filesystem::path normalized_path = std::filesystem::path{value}.lexically_normal();
    std::filesystem::path normalized_prefix = std::filesystem::path{prefix}.lexically_normal();
    const auto path_text = normalized_path.generic_string();
    auto prefix_text = normalized_prefix.generic_string();
    if (prefix_text == ".") {
        return true;
    }
    if (!prefix_text.empty() && prefix_text.back() == '/') {
        prefix_text.pop_back();
    }
    return path_text == prefix_text || path_text.rfind(prefix_text + "/", 0) == 0;
}

bool path_filter_matches(const std::string& value, const HttpRequest& request) {
    const auto prefix = request.query.find("path_prefix");
    if (prefix != request.query.end() && !prefix->second.empty()) {
        return path_prefix_matches(value, prefix->second);
    }
    const auto path = request.query.find("path");
    if (path != request.query.end() && !path->second.empty()) {
        return path_prefix_matches(value, path->second);
    }
    const auto contains = request.query.find("path_contains");
    return contains == request.query.end() || contains->second.empty() ||
           value.find(contains->second) != std::string::npos;
}

bool path_filter_matches(const json& object, const std::string& key, const HttpRequest& request) {
    const auto value = object.find(key);
    return value != object.end() && value->is_string() &&
           path_filter_matches(value->get<std::string>(), request);
}

std::string project_file_kind_name(ProjectFileKind kind) {
    switch (kind) {
        case ProjectFileKind::CSource:
            return "c_source";
        case ProjectFileKind::CxxSource:
            return "cxx_source";
        case ProjectFileKind::RustSource:
            return "rust_source";
        case ProjectFileKind::TypeScriptSource:
            return "typescript_source";
        case ProjectFileKind::VueSource:
            return "vue_source";
        case ProjectFileKind::PythonSource:
            return "python_source";
        case ProjectFileKind::Header:
            return "header";
        case ProjectFileKind::BuildFile:
            return "build_file";
        case ProjectFileKind::Config:
            return "config";
        case ProjectFileKind::Other:
            return "other";
    }
    return "other";
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

json filtered_classes(const BackgroundService& service, const HttpRequest& request) {
    const auto index = code_index_payload(service);
    json classes = json::array();
    for (const auto& klass : index["classes"]) {
        if (contains_filter(klass, "name", request) &&
            contains_filter(klass, "qualified_name", request) &&
            contains_filter(klass, "file", request) &&
            path_filter_matches(klass, "file", request)) {
            classes.push_back(klass);
        }
    }
    return limited_array(classes, request);
}

json filtered_functions(const BackgroundService& service, const HttpRequest& request) {
    const auto index = code_index_payload(service);
    json functions = json::array();
    for (const auto& function : index["functions"]) {
        if (contains_filter(function, "name", request) &&
            contains_filter(function, "qualified_name", request) &&
            contains_filter(function, "file", request) &&
            path_filter_matches(function, "file", request) &&
            contains_filter(function, "kind", request)) {
            functions.push_back(function);
        }
    }
    return limited_array(functions, request);
}

json filtered_call_graph(const BackgroundService& service, const HttpRequest& request) {
    const auto index = code_index_payload(service);
    json call_graph = json::array();
    for (const auto& edge : index["call_graph"]) {
        if (contains_filter(edge, "caller", request) &&
            contains_filter(edge, "callee", request) &&
            contains_filter(edge, "file", request) &&
            path_filter_matches(edge, "file", request)) {
            call_graph.push_back(edge);
        }
    }
    return limited_array(call_graph, request);
}

}  // namespace

json source_lines_payload(const std::filesystem::path& path,
                          const std::filesystem::path& relative,
                          std::size_t requested_line,
                          std::size_t before,
                          std::size_t after) {
    if (requested_line == 0) {
        throw std::invalid_argument("line must be one-based");
    }
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("could not open file: " + relative.generic_string());
    }

    const auto start_line = requested_line > before ? requested_line - before : 1;
    const auto end_line = requested_line + after;
    json lines = json::array();
    std::string text;
    std::size_t line_number = 0;
    while (std::getline(input, text)) {
        ++line_number;
        if (line_number < start_line) {
            continue;
        }
        if (line_number > end_line) {
            break;
        }
        lines.push_back({{"line", line_number}, {"text", text}, {"target", line_number == requested_line}});
    }

    return {{"file", relative.generic_string()},
            {"line", requested_line},
            {"before", before},
            {"after", after},
            {"start_line", start_line},
            {"end_line", line_number < end_line ? line_number : end_line},
            {"lines", lines}};
}

json read_context_payload(const BackgroundService& service, const HttpRequest& request) {
    const auto line_it = request.query.find("line");
    if (line_it == request.query.end() || line_it->second.empty()) {
        throw std::invalid_argument("read context requires file and line query parameters");
    }

    const auto relative = requested_relative_file(request);
    const auto requested_line = query_size(request, "line", 1);
    auto payload = source_lines_payload((service.project_root() / relative).lexically_normal(),
                                        relative,
                                        requested_line,
                                        query_size(request, "before", 5),
                                        query_size(request, "after", 5));
    payload["query"] = request.query;
    return payload;
}

json file_entry_payload(const BackgroundService& service, const ProjectFile& file) {
    const auto path = (service.project_root() / file.path).lexically_normal();
    std::error_code ec;
    json value{{"path", file.path.generic_string()},
               {"file", file.path.generic_string()},
               {"kind", project_file_kind_name(file.kind)},
               {"extension", file.path.extension().string()}};
    const auto size = std::filesystem::file_size(path, ec);
    if (!ec) {
        value["size"] = size;
    }
    return value;
}

json files_payload(const BackgroundService& service, const HttpRequest& request) {
    if (request.query.empty()) {
        throw std::invalid_argument("files requires at least one query parameter");
    }
    const auto scan = service.project_scan();
    json files = json::array();
    for (const auto& file : scan.files) {
        const auto entry = file_entry_payload(service, file);
        if (contains_filter(entry, "kind", request) &&
            path_filter_matches(entry, "path", request) &&
            contains_filter(entry, "extension", request)) {
            files.push_back(entry);
        }
    }
    return {{"root", scan.root.string()},
            {"files", limited_array(files, request)},
            {"total_files", scan.files.size()},
            {"query", request.query}};
}

json file_info_payload(const BackgroundService& service, const HttpRequest& request) {
    const auto relative = requested_relative_file(request);
    const auto full_path = (service.project_root() / relative).lexically_normal();
    std::error_code ec;
    json value{{"file", relative.generic_string()},
               {"exists", std::filesystem::is_regular_file(full_path, ec)}};
    const auto scan = service.project_scan();
    for (const auto& file : scan.files) {
        if (file.path == relative) {
            value.update(file_entry_payload(service, file));
            break;
        }
    }
    ec.clear();
    const auto size = std::filesystem::file_size(full_path, ec);
    if (!ec) {
        value["size"] = size;
    }
    return value;
}

bool searchable_file(ProjectFileKind kind) {
    return kind == ProjectFileKind::CSource || kind == ProjectFileKind::CxxSource ||
           kind == ProjectFileKind::Header || kind == ProjectFileKind::RustSource ||
           kind == ProjectFileKind::TypeScriptSource || kind == ProjectFileKind::VueSource ||
           kind == ProjectFileKind::PythonSource || kind == ProjectFileKind::BuildFile ||
           kind == ProjectFileKind::Config;
}

json search_payload(const BackgroundService& service, const HttpRequest& request) {
    const auto pattern_it = request.query.find("pattern");
    if (pattern_it == request.query.end() || pattern_it->second.empty()) {
        throw std::invalid_argument("search requires pattern query parameter");
    }
    const bool case_sensitive = request.query.find("case_sensitive") != request.query.end() &&
                                request.query.at("case_sensitive") != "0" &&
                                request.query.at("case_sensitive") != "false";
    const auto limit = query_size(request, "limit", 100);
    const auto before = query_size(request, "before", 0);
    const auto after = query_size(request, "after", 0);
    const auto scan = service.project_scan();
    json matches = json::array();
    for (const auto& file : scan.files) {
        if (!searchable_file(file.kind)) {
            continue;
        }
        const auto entry = file_entry_payload(service, file);
        if (!contains_filter(entry, "file", request) ||
            !path_filter_matches(entry, "file", request) ||
            !contains_filter(entry, "kind", request) ||
            !contains_filter(entry, "extension", request)) {
            continue;
        }
        std::ifstream input((service.project_root() / file.path).lexically_normal());
        if (!input) {
            continue;
        }
        std::string line;
        std::size_t line_number = 0;
        while (std::getline(input, line)) {
            ++line_number;
            const bool found = case_sensitive
                                   ? boost::algorithm::contains(line, pattern_it->second)
                                   : boost::algorithm::icontains(line, pattern_it->second);
            if (!found) {
                continue;
            }
            json match{{"file", file.path.generic_string()},
                       {"line", line_number},
                       {"text", line},
                       {"kind", project_file_kind_name(file.kind)}};
            if (before != 0 || after != 0) {
                match["context"] =
                    source_lines_payload((service.project_root() / file.path).lexically_normal(),
                                         file.path,
                                         line_number,
                                         before,
                                         after)["lines"];
            }
            matches.push_back(std::move(match));
            if (limit != 0 && matches.size() >= limit) {
                return {{"matches", matches}, {"query", request.query}, {"truncated", true}};
            }
        }
    }
    return {{"matches", matches}, {"query", request.query}, {"truncated", false}};
}

json ast_payload(const BackgroundService& service, const HttpRequest& request) {
    const auto relative = requested_relative_file(request);
    const auto status = service.status();
    std::filesystem::path tree_relative = relative;
    tree_relative += ".tree";
    const auto tree_path =
        (status.tree_sitter.output_directory / "trees" / tree_relative).lexically_normal();
    std::ifstream input(tree_path);
    if (!input) {
        throw std::runtime_error("tree-sitter AST not available for file: " + relative.generic_string());
    }

    const auto max_lines = query_size(request, "max_lines", 200);
    json lines = json::array();
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (max_lines != 0 && line_number > max_lines) {
            return {{"file", relative.generic_string()},
                    {"tree", tree_path.string()},
                    {"lines", lines},
                    {"truncated", true},
                    {"query", request.query}};
        }
        lines.push_back({{"line", line_number}, {"text", line}});
    }
    return {{"file", relative.generic_string()},
            {"tree", tree_path.string()},
            {"lines", lines},
            {"truncated", false},
            {"query", request.query}};
}

struct LineStats {
    std::size_t total = 0;
    std::size_t code = 0;
    std::size_t blank = 0;
    std::size_t comments = 0;
};

LineStats line_stats(const std::filesystem::path& path) {
    std::ifstream input(path);
    LineStats stats;
    bool block_comment = false;
    std::string line;
    while (std::getline(input, line)) {
        ++stats.total;
        auto trimmed = boost::algorithm::trim_copy(line);
        if (trimmed.empty()) {
            ++stats.blank;
            continue;
        }
        if (block_comment) {
            ++stats.comments;
            if (trimmed.find("*/") != std::string::npos) {
                block_comment = false;
            }
            continue;
        }
        if (boost::algorithm::starts_with(trimmed, "//")) {
            ++stats.comments;
            continue;
        }
        if (boost::algorithm::starts_with(trimmed, "/*")) {
            ++stats.comments;
            if (trimmed.find("*/") == std::string::npos) {
                block_comment = true;
            }
            continue;
        }
        ++stats.code;
    }
    return stats;
}

json line_stats_json(const LineStats& stats) {
    return {{"total", stats.total},
            {"code", stats.code},
            {"blank", stats.blank},
            {"comments", stats.comments}};
}

json project_summary_payload(const BackgroundService& service, const HttpRequest& request) {
    const auto scan = service.project_scan();
    const auto index = code_index_payload(service);
    std::map<std::string, std::size_t> kinds;
    for (const auto& file : scan.files) {
        ++kinds[project_file_kind_name(file.kind)];
    }

    std::map<std::string, std::size_t> callers_by_symbol;
    std::map<std::string, std::size_t> calls_by_file;
    for (const auto& edge : index["call_graph"]) {
        ++callers_by_symbol[edge.value("callee", std::string{})];
        ++calls_by_file[edge.value("file", std::string{})];
    }

    json architecture_nodes = json::array();
    for (const auto& klass : index["classes"]) {
        const auto name = klass.value("qualified_name", std::string{});
        architecture_nodes.push_back({{"name", name},
                                      {"kind", "class"},
                                      {"file", klass.value("file", std::string{})},
                                      {"line", klass.value("line", 0)},
                                      {"methods", klass["methods"].size()},
                                      {"incoming_calls", callers_by_symbol[name]}});
    }
    std::sort(architecture_nodes.begin(), architecture_nodes.end(), [](const json& lhs, const json& rhs) {
        if (lhs["incoming_calls"] != rhs["incoming_calls"]) {
            return lhs["incoming_calls"] > rhs["incoming_calls"];
        }
        return lhs["methods"] > rhs["methods"];
    });

    json top_files = json::array();
    for (const auto& file : scan.files) {
        const auto file_text = file.path.generic_string();
        std::size_t symbols = 0;
        for (const auto& klass : index["classes"]) {
            if (klass.value("file", std::string{}) == file_text) {
                ++symbols;
            }
        }
        for (const auto& function : index["functions"]) {
            if (function.value("file", std::string{}) == file_text) {
                ++symbols;
            }
        }
        if (symbols == 0 && calls_by_file[file_text] == 0) {
            continue;
        }
        top_files.push_back({{"file", file_text},
                             {"kind", project_file_kind_name(file.kind)},
                             {"symbols", symbols},
                             {"call_edges", calls_by_file[file_text]},
                             {"score", symbols * 10 + calls_by_file[file_text]}});
    }
    std::sort(top_files.begin(), top_files.end(), [](const json& lhs, const json& rhs) {
        return lhs["score"] > rhs["score"];
    });

    HttpRequest limit_request = request;
    if (limit_request.query.find("limit") == limit_request.query.end()) {
        limit_request.query["limit"] = "20";
    }

    return {{"root", scan.root.string()},
            {"files", scan.files.size()},
            {"file_kinds", kinds},
            {"symbols",
             {{"classes", index["classes"].size()},
              {"functions", index["functions"].size()},
              {"call_graph_edges", index["call_graph"].size()}}},
            {"build_files", scan.build_files_by_type},
            {"architecture_nodes", limited_array(architecture_nodes, limit_request)},
            {"top_files", limited_array(top_files, limit_request)},
            {"query", request.query}};
}

json file_analysis_payload(const BackgroundService& service, const HttpRequest& request) {
    const auto relative = requested_relative_file(request);
    const auto file_text = relative.generic_string();
    const auto index = code_index_payload(service);
    json classes = json::array();
    json functions = json::array();
    json outgoing = json::array();
    json incoming = json::array();

    for (const auto& klass : index["classes"]) {
        if (klass.value("file", std::string{}) == file_text) {
            classes.push_back(klass);
        }
    }
    for (const auto& function : index["functions"]) {
        if (function.value("file", std::string{}) == file_text) {
            functions.push_back(function);
        }
    }
    for (const auto& edge : index["call_graph"]) {
        if (edge.value("file", std::string{}) == file_text) {
            outgoing.push_back(edge);
        }
        for (const auto& function : functions) {
            if (edge.value("callee", std::string{}) == function.value("name", std::string{}) ||
                edge.value("callee", std::string{}) == function.value("qualified_name", std::string{})) {
                incoming.push_back(edge);
                break;
            }
        }
    }

    return {{"file", file_text},
            {"line_stats", line_stats_json(line_stats((service.project_root() / relative).lexically_normal()))},
            {"classes", classes},
            {"functions", functions},
            {"outgoing_calls", limited_array(outgoing, request)},
            {"incoming_calls", limited_array(incoming, request)},
            {"query", request.query}};
}

json symbol_payload(const BackgroundService& service, const HttpRequest& request) {
    const auto name_it = request.query.find("name");
    if (name_it == request.query.end() || name_it->second.empty()) {
        throw std::invalid_argument("symbol requires name query parameter");
    }
    const auto index = code_index_payload(service);
    json symbols = json::array();
    const auto add_symbol = [&](json symbol, const std::string& kind) {
        symbol["symbol_kind"] = kind;
        if (request.query.find("context") != request.query.end() &&
            request.query.at("context") != "0" && request.query.at("context") != "false") {
            HttpRequest context_request;
            context_request.query["file"] = symbol.value("file", std::string{});
            context_request.query["line"] = std::to_string(symbol.value("line", 1));
            context_request.query["before"] = std::to_string(query_size(request, "before", 3));
            context_request.query["after"] = std::to_string(query_size(request, "after", 8));
            symbol["context"] = read_context_payload(service, context_request)["lines"];
        }
        symbols.push_back(std::move(symbol));
    };

    for (const auto& klass : index["classes"]) {
        const auto name = klass.value("name", std::string{});
        const auto qualified_name = klass.value("qualified_name", std::string{});
        if ((contains_text_filter(name, name_it->second) ||
             contains_text_filter(qualified_name, name_it->second)) &&
            contains_filter(klass, "file", request) &&
            path_filter_matches(klass, "file", request)) {
            add_symbol(klass, "class");
        }
    }
    for (const auto& function : index["functions"]) {
        const auto name = function.value("name", std::string{});
        const auto qualified_name = function.value("qualified_name", std::string{});
        if ((contains_text_filter(name, name_it->second) ||
             contains_text_filter(qualified_name, name_it->second)) &&
            contains_filter(function, "file", request) &&
            path_filter_matches(function, "file", request) &&
            contains_filter(function, "kind", request)) {
            add_symbol(function, function.value("kind", "function"));
        }
    }
    return {{"symbols", limited_array(symbols, request)}, {"query", request.query}};
}

json memory_graph_payload(const BackgroundService& service) {
    const auto scan = service.project_scan();
    const auto index = code_index_payload(service);
    json nodes = json::array();
    json edges = json::array();
    const auto project_id = std::string{"project:"} + scan.root.string();
    nodes.push_back({{"id", project_id},
                     {"label", "Project"},
                     {"name", scan.root.filename().string()},
                     {"path", scan.root.string()}});

    std::map<std::string, std::string> function_ids_by_name;
    std::map<std::string, std::string> function_ids_by_qualified_name;
    for (const auto& file : scan.files) {
        const auto path = file.path.generic_string();
        const auto file_id = std::string{"file:"} + path;
        nodes.push_back({{"id", file_id},
                         {"label", "File"},
                         {"name", file.path.filename().string()},
                         {"path", path},
                         {"kind", project_file_kind_name(file.kind)}});
        edges.push_back({{"from", project_id}, {"to", file_id}, {"type", "CONTAINS_FILE"}});
    }
    for (const auto& klass : index["classes"]) {
        const auto qualified_name = klass.value("qualified_name", std::string{});
        const auto id = std::string{"class:"} + qualified_name;
        nodes.push_back({{"id", id},
                         {"label", "Class"},
                         {"name", klass.value("name", std::string{})},
                         {"qualified_name", qualified_name},
                         {"file", klass.value("file", std::string{})},
                         {"line", klass.value("line", 0)}});
        edges.push_back({{"from", std::string{"file:"} + klass.value("file", std::string{})},
                         {"to", id},
                         {"type", "DEFINES"}});
    }
    for (const auto& function : index["functions"]) {
        const auto qualified_name = function.value("qualified_name", std::string{});
        const auto label = function.value("kind", std::string{}) == "method" ? "Method" : "Function";
        const auto id = std::string{"function:"} + qualified_name;
        nodes.push_back({{"id", id},
                         {"label", label},
                         {"name", function.value("name", std::string{})},
                         {"qualified_name", qualified_name},
                         {"file", function.value("file", std::string{})},
                         {"line", function.value("line", 0)}});
        edges.push_back({{"from", std::string{"file:"} + function.value("file", std::string{})},
                         {"to", id},
                         {"type", "DEFINES"}});
        function_ids_by_name[function.value("name", std::string{})] = id;
        function_ids_by_qualified_name[qualified_name] = id;
    }
    for (const auto& edge : index["call_graph"]) {
        const auto caller = edge.value("caller", std::string{});
        const auto callee = edge.value("callee", std::string{});
        const auto caller_it = function_ids_by_qualified_name.find(caller);
        const auto callee_qualified_it = function_ids_by_qualified_name.find(callee);
        const auto callee_name_it = function_ids_by_name.find(callee);
        if (caller_it == function_ids_by_qualified_name.end()) {
            continue;
        }
        const auto to = callee_qualified_it != function_ids_by_qualified_name.end()
                            ? callee_qualified_it->second
                            : (callee_name_it != function_ids_by_name.end() ? callee_name_it->second
                                                                             : std::string{"external:"} + callee);
        edges.push_back({{"from", caller_it->second},
                         {"to", to},
                         {"type", "CALLS"},
                         {"callee", callee},
                         {"file", edge.value("file", std::string{})},
                         {"line", edge.value("line", 0)}});
    }

    return {{"format", "index-codebase-memory-v1"},
            {"root", scan.root.string()},
            {"nodes", nodes},
            {"edges", edges}};
}

json persist_memory_graph(const BackgroundService& service, json graph) {
    const auto directory = service.options().state_directory / "codebase-memory";
    const auto path = directory / "graph.json";
    std::filesystem::create_directories(directory);
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("could not write codebase memory graph: " + path.string());
    }
    output << graph.dump(2) << '\n';
    graph["artifact"] = path.string();
    return graph;
}

json graph_schema_payload(const BackgroundService& service) {
    const auto graph = memory_graph_payload(service);
    std::map<std::string, std::size_t> labels;
    std::map<std::string, std::size_t> edge_types;
    for (const auto& node : graph["nodes"]) {
        ++labels[node.value("label", std::string{})];
    }
    for (const auto& edge : graph["edges"]) {
        ++edge_types[edge.value("type", std::string{})];
    }
    return {{"node_labels", labels},
            {"edge_types", edge_types},
            {"nodes", graph["nodes"].size()},
            {"edges", graph["edges"].size()}};
}

json graph_schema_payload(const BackgroundService& service, const HttpRequest& request) {
    auto payload = graph_schema_payload(service);
    if (!query_bool(request, "include_counts", true)) {
        payload.erase("nodes");
        payload.erase("edges");
    }
    if (!request.query.empty()) {
        payload["query"] = request.query;
    }
    return payload;
}

json search_graph_payload(const BackgroundService& service, const HttpRequest& request) {
    const auto graph = memory_graph_payload(service);
    const auto label = request.query.count("label") ? request.query.at("label") : std::string{};
    const auto file = request.query.count("file") ? request.query.at("file") : std::string{};
    const auto pattern_text = request.query.count("name_pattern") ? request.query.at("name_pattern") : std::string{};
    std::regex pattern;
    const bool use_regex = !pattern_text.empty();
    if (use_regex) {
        pattern = std::regex(pattern_text);
    }

    json results = json::array();
    const auto offset = query_size(request, "offset", 0);
    const auto limit = query_size(request, "limit", 100);
    std::size_t skipped = 0;
    for (const auto& node : graph["nodes"]) {
        if (!label.empty() && node.value("label", std::string{}) != label) {
            continue;
        }
        if (!file.empty() && node.value("file", node.value("path", std::string{})).find(file) == std::string::npos) {
            continue;
        }
        const auto name = node.value("qualified_name", node.value("name", std::string{}));
        if (use_regex && !std::regex_search(name, pattern)) {
            continue;
        }
        if (skipped++ < offset) {
            continue;
        }
        results.push_back(node);
        if (limit != 0 && results.size() >= limit) {
            break;
        }
    }
    return {{"results", results}, {"query", request.query}};
}

json trace_call_path_payload(const BackgroundService& service, const HttpRequest& request) {
    const auto function_it = request.query.find("function_name");
    if (function_it == request.query.end() || function_it->second.empty()) {
        throw std::invalid_argument("trace call path requires function_name query parameter");
    }
    const auto direction = request.query.count("direction") ? request.query.at("direction") : std::string{"both"};
    const auto max_depth = std::min<std::size_t>(query_size(request, "depth", 3), 5);
    const auto index = code_index_payload(service);
    json paths = json::array();
    std::set<std::string> visited;
    std::queue<std::pair<std::string, std::size_t>> pending;
    pending.push({function_it->second, 0});
    visited.insert(function_it->second);

    while (!pending.empty()) {
        const auto [current, depth] = pending.front();
        pending.pop();
        if (depth >= max_depth) {
            continue;
        }
        for (const auto& edge : index["call_graph"]) {
            const auto caller = edge.value("caller", std::string{});
            const auto callee = edge.value("callee", std::string{});
            const auto matches_symbol = [&](const std::string& candidate) {
                if (candidate == current) {
                    return true;
                }
                return candidate.size() > current.size() + 2U &&
                       candidate.compare(candidate.size() - current.size(),
                                         current.size(), current) == 0 &&
                       candidate.compare(candidate.size() - current.size() - 2U,
                                         2U, "::") == 0;
            };
            const bool outbound =
                (direction == "outbound" || direction == "both") &&
                matches_symbol(caller);
            const bool inbound =
                (direction == "inbound" || direction == "both") &&
                matches_symbol(callee);
            if (!outbound && !inbound) {
                continue;
            }
            paths.push_back({{"depth", depth + 1},
                             {"direction", outbound ? "outbound" : "inbound"},
                             {"caller", caller},
                             {"callee", callee},
                             {"file", edge.value("file", std::string{})},
                             {"line", edge.value("line", 0)}});
            const auto next = outbound ? callee : caller;
            if (!next.empty() && visited.insert(next).second) {
                pending.push({next, depth + 1});
            }
        }
    }
    return {{"function_name", function_it->second},
            {"direction", direction},
            {"depth", max_depth},
            {"paths", limited_array(paths, request)},
            {"query", request.query}};
}

json dead_code_payload(const BackgroundService& service, const HttpRequest& request) {
    const auto index = code_index_payload(service);
    std::set<std::string> called;
    for (const auto& edge : index["call_graph"]) {
        called.insert(edge.value("callee", std::string{}));
    }
    json functions = json::array();
    for (const auto& function : index["functions"]) {
        const auto name = function.value("name", std::string{});
        const auto qualified_name = function.value("qualified_name", std::string{});
        if (name == "main" || called.count(name) != 0 || called.count(qualified_name) != 0) {
            continue;
        }
        functions.push_back(function);
    }
    return {{"functions", limited_array(functions, request)}, {"query", request.query}};
}

json code_snippet_payload(const BackgroundService& service, const HttpRequest& request) {
    const auto name_it = request.query.find("qualified_name");
    if (name_it == request.query.end() || name_it->second.empty()) {
        throw std::invalid_argument("code snippet requires qualified_name query parameter");
    }
    const auto index = code_index_payload(service);
    for (const auto& function : index["functions"]) {
        if (function.value("qualified_name", std::string{}).find(name_it->second) == std::string::npos) {
            continue;
        }
        HttpRequest context_request;
        context_request.query["file"] = function.value("file", std::string{});
        context_request.query["line"] = std::to_string(function.value("line", 1));
        context_request.query["before"] = std::to_string(query_size(request, "before", 2));
        context_request.query["after"] = std::to_string(query_size(request, "after", 20));
        auto payload = read_context_payload(service, context_request);
        payload["qualified_name"] = function.value("qualified_name", std::string{});
        return payload;
    }
    throw std::runtime_error("symbol not found: " + name_it->second);
}



json code_index_payload(const BackgroundService& service) {
    return json::parse(code_index_json(service.code_index()));
}

json classes_payload(const BackgroundService& service, const HttpRequest& request) {
    return {{"classes", filtered_classes(service, request)}, {"query", request.query}};
}

json functions_payload(const BackgroundService& service, const HttpRequest& request) {
    return {{"functions", filtered_functions(service, request)}, {"query", request.query}};
}

json callgraph_payload(const BackgroundService& service, const HttpRequest& request) {
    return {{"call_graph", filtered_call_graph(service, request)}, {"query", request.query}};
}

}  // namespace qt::index
