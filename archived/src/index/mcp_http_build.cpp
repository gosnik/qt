#include "index/mcp_http_build.hpp"

#include "index/path_util.hpp"
#include "index/string_util.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_set>

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

std::string query_value(const HttpRequest& request, const std::string& key, const std::string& fallback = {}) {
    const auto found = request.query.find(key);
    return found == request.query.end() || found->second.empty() ? fallback : found->second;
}

std::string relative_or_original(const std::filesystem::path& root, const std::string& value) {
    std::error_code ec;
    const auto path = std::filesystem::path{value}.lexically_normal();
    if (path_has_prefix(path, root)) {
        const auto relative = std::filesystem::relative(path, root, ec);
        if (!ec && !relative.empty()) {
            return relative.generic_string();
        }
    }
    return path.generic_string();
}

bool entry_matches_file(const json& entry, const std::string& file_filter) {
    if (file_filter.empty()) {
        return true;
    }
    const auto file = entry.value("file", std::string{});
    return file.find(file_filter) != std::string::npos;
}


json read_compile_commands_file(const BackgroundService& service) {
    std::ifstream input(service.options().compile_commands_output);
    if (!input) {
        return json::array();
    }
    json parsed = json::parse(input, nullptr, false);
    return parsed.is_array() ? parsed : json::array();
}

std::vector<std::string> entry_arguments(const json& entry) {
    std::vector<std::string> args;
    if (entry.contains("arguments") && entry["arguments"].is_array()) {
        for (const auto& value : entry["arguments"]) {
            if (value.is_string()) {
                args.push_back(value.get<std::string>());
            }
        }
    }
    return args;
}

}  // namespace

json cmake_compile_commands_payload(const BackgroundService& service, const HttpRequest& request) {
    const auto commands = read_compile_commands_file(service);
    const auto limit = query_size(request, "limit", 200);
    const auto file_filter = query_value(request, "file");
    json entries = json::array();
    for (const auto& entry : commands) {
        if (!entry_matches_file(entry, file_filter)) {
            continue;
        }
        if (limit != 0 && entries.size() >= limit) {
            break;
        }
        entries.push_back(entry);
    }
    return {{"path", service.options().compile_commands_output.string()},
            {"count", commands.size()},
            {"entries", entries},
            {"query", request.query}};
}

json compile_flags_for_entry(const BackgroundService& service, const json& entry) {
    const auto root = service.project_root();
    const auto args = entry_arguments(entry);
    json includes = json::array();
    json defines = json::array();
    json standards = json::array();
    json flags = json::array();
    std::string compiler;
    std::string output = entry.value("output", std::string{});
    if (!args.empty()) {
        compiler = args.front();
    }
    for (std::size_t index = 1; index < args.size(); ++index) {
        const auto& arg = args[index];
        if ((arg == "-I" || arg == "-isystem" || arg == "/I") && index + 1 < args.size()) {
            includes.push_back(relative_or_original(root, args[++index]));
        } else if (arg.rfind("-I", 0) == 0 && arg.size() > 2) {
            includes.push_back(relative_or_original(root, arg.substr(2)));
        } else if (arg.rfind("/I", 0) == 0 && arg.size() > 2) {
            includes.push_back(relative_or_original(root, arg.substr(2)));
        } else if ((arg == "-D" || arg == "/D") && index + 1 < args.size()) {
            defines.push_back(args[++index]);
        } else if (arg.rfind("-D", 0) == 0 && arg.size() > 2) {
            defines.push_back(arg.substr(2));
        } else if (arg.rfind("/D", 0) == 0 && arg.size() > 2) {
            defines.push_back(arg.substr(2));
        } else if (arg.rfind("-std=", 0) == 0 || arg.rfind("/std:", 0) == 0) {
            standards.push_back(arg);
        } else if (arg == "-o" && index + 1 < args.size()) {
            output = args[++index];
        } else if (arg != "-c" && arg != entry.value("file", std::string{})) {
            flags.push_back(arg);
        }
    }
    return {{"file", relative_or_original(root, entry.value("file", std::string{}))},
            {"directory", entry.value("directory", std::string{})},
            {"compiler", compiler},
            {"standards", standards},
            {"defines", defines},
            {"includes", includes},
            {"flags", flags},
            {"output", output}};
}

json cmake_compile_flags_payload(const BackgroundService& service, const HttpRequest& request) {
    const auto commands = read_compile_commands_file(service);
    const auto limit = query_size(request, "limit", 100);
    const auto file_filter = query_value(request, "file");
    json entries = json::array();
    for (const auto& entry : commands) {
        if (!entry_matches_file(entry, file_filter)) {
            continue;
        }
        if (limit != 0 && entries.size() >= limit) {
            break;
        }
        entries.push_back(compile_flags_for_entry(service, entry));
    }
    return {{"entries", entries}, {"compile_commands", commands.size()}, {"query", request.query}};
}

std::vector<std::filesystem::path> include_search_paths_for_entry(const BackgroundService& service,
                                                                  const json& entry) {
    std::vector<std::filesystem::path> paths;
    std::unordered_set<std::string> seen;
    const auto add = [&](std::filesystem::path path) {
        path = path.lexically_normal();
        if (seen.insert(path.string()).second) {
            paths.push_back(path);
        }
    };
    const auto file = std::filesystem::path{entry.value("file", std::string{})};
    if (!file.empty()) {
        add(file.parent_path());
    }
    add(service.project_root());
    for (const auto& arg : entry_arguments(entry)) {
        if (arg.rfind("-I", 0) == 0 && arg.size() > 2) {
            add(std::filesystem::path{arg.substr(2)});
        }
    }
    return paths;
}

std::string resolve_include(const std::vector<std::filesystem::path>& search_paths,
                            const std::string& include_name) {
    for (const auto& base : search_paths) {
        const auto candidate = (base / include_name).lexically_normal();
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return candidate.generic_string();
        }
    }
    return {};
}

json cmake_include_graph_payload(const BackgroundService& service, const HttpRequest& request) {
    const auto commands = read_compile_commands_file(service);
    const auto limit = query_size(request, "limit", 300);
    const auto file_filter = query_value(request, "file");
    const std::regex include_regex(R"(^\s*#\s*include\s*([<"])([^>"]+)[>"])");
    json edges = json::array();
    std::unordered_map<std::string, json> command_by_file;
    for (const auto& command : commands) {
        command_by_file[std::filesystem::path{command.value("file", std::string{})}.lexically_normal().string()] = command;
    }
    for (const auto& file : service.project_scan().files) {
        if (file.kind != ProjectFileKind::CSource && file.kind != ProjectFileKind::CxxSource &&
            file.kind != ProjectFileKind::Header) {
            continue;
        }
        const auto source = (service.project_root() / file.path).lexically_normal();
        if (!file_filter.empty() && file.path.generic_string().find(file_filter) == std::string::npos) {
            continue;
        }
        std::ifstream input(source);
        if (!input) {
            continue;
        }
        auto command_it = command_by_file.find(source.string());
        const auto search_paths = command_it == command_by_file.end()
                                      ? std::vector<std::filesystem::path>{source.parent_path(), service.project_root()}
                                      : include_search_paths_for_entry(service, command_it->second);
        std::string line;
        std::size_t line_number = 0;
        while (std::getline(input, line)) {
            ++line_number;
            std::smatch match;
            if (!std::regex_search(line, match, include_regex)) {
                continue;
            }
            if (limit != 0 && edges.size() >= limit) {
                return {{"edges", edges}, {"truncated", true}, {"query", request.query}};
            }
            const auto include_name = match[2].str();
            const auto resolved = resolve_include(search_paths, include_name);
            edges.push_back({{"from", file.path.generic_string()},
                             {"line", line_number},
                             {"include", include_name},
                             {"system", match[1].str() == "<"},
                             {"resolved", resolved.empty() ? "" : relative_or_original(service.project_root(), resolved)}});
        }
    }
    return {{"edges", edges}, {"truncated", false}, {"query", request.query}};
}

void parse_cmake_targets(const BackgroundService& service, json& nodes, json& edges) {
    const auto scan = service.project_scan();
    const std::regex target_regex(R"(\badd_(executable|library)\s*\(\s*([A-Za-z0-9_.:+-]+))", std::regex::icase);
    const std::regex link_regex(R"(\btarget_link_libraries\s*\(\s*([A-Za-z0-9_.:+-]+)\s+([^)]*))", std::regex::icase);
    const std::regex sources_regex(R"(\btarget_sources\s*\(\s*([A-Za-z0-9_.:+-]+)\s+([^)]*))", std::regex::icase);
    std::unordered_set<std::string> seen_nodes;
    const auto add_node = [&](const std::string& name, const std::string& type, const std::filesystem::path& file) {
        const auto key = type + ":" + name;
        if (seen_nodes.insert(key).second) {
            nodes.push_back({{"name", name}, {"type", type}, {"source", file.generic_string()}, {"backend", "cmake"}});
        }
    };
    for (const auto& file : scan.files) {
        if (file.path.filename() != "CMakeLists.txt" && file.path.extension() != ".cmake") {
            continue;
        }
        std::ifstream input(service.project_root() / file.path);
        if (!input) {
            continue;
        }
        std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        for (std::sregex_iterator it(content.begin(), content.end(), target_regex), end; it != end; ++it) {
            add_node((*it)[2].str(), lowercase_ascii((*it)[1].str()), file.path);
        }
        for (std::sregex_iterator it(content.begin(), content.end(), link_regex), end; it != end; ++it) {
            const auto target = (*it)[1].str();
            add_node(target, "target", file.path);
            std::stringstream deps((*it)[2].str());
            std::string dep;
            while (deps >> dep) {
                if (dep == "PRIVATE" || dep == "PUBLIC" || dep == "INTERFACE") {
                    continue;
                }
                edges.push_back({{"from", target}, {"to", dep}, {"type", "links"}, {"backend", "cmake"}});
            }
        }
        for (std::sregex_iterator it(content.begin(), content.end(), sources_regex), end; it != end; ++it) {
            const auto target = (*it)[1].str();
            std::stringstream sources((*it)[2].str());
            std::string source;
            while (sources >> source) {
                if (source == "PRIVATE" || source == "PUBLIC" || source == "INTERFACE") {
                    continue;
                }
                edges.push_back({{"from", target}, {"to", source}, {"type", "uses_source"}, {"backend", "cmake"}});
            }
        }
    }
}

void parse_ninja_targets(const BackgroundService& service, json& nodes, json& edges) {
    const auto scan = service.project_scan();
    std::unordered_set<std::string> seen_nodes;
    for (const auto& file : scan.files) {
        if (file.path.filename() != "build.ninja" && file.path.extension() != ".ninja") {
            continue;
        }
        std::ifstream input(service.project_root() / file.path);
        if (!input) {
            continue;
        }
        std::string line;
        while (std::getline(input, line)) {
            line = trim_ascii(std::move(line));
            if (line.rfind("build ", 0) != 0) {
                continue;
            }
            const auto colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            const auto outputs = trim_ascii(line.substr(6, colon - 6));
            std::stringstream output_stream(outputs);
            std::string output;
            while (output_stream >> output) {
                if (seen_nodes.insert(output).second) {
                    nodes.push_back({{"name", output}, {"type", "output"}, {"source", file.path.generic_string()}, {"backend", "ninja"}});
                }
            }
            std::stringstream dep_stream(line.substr(colon + 1));
            std::string rule;
            dep_stream >> rule;
            std::string dep;
            while (dep_stream >> dep) {
                if (dep == "|" || dep == "||") {
                    continue;
                }
                std::stringstream outputs_again(outputs);
                while (outputs_again >> output) {
                    edges.push_back({{"from", output}, {"to", dep}, {"type", rule}, {"backend", "ninja"}});
                }
            }
        }
    }
}

json cmake_target_graph_payload(const BackgroundService& service, const HttpRequest& request) {
    json nodes = json::array();
    json edges = json::array();
    parse_cmake_targets(service, nodes, edges);
    parse_ninja_targets(service, nodes, edges);
    const auto limit = query_size(request, "limit", 500);
    if (limit != 0) {
        while (nodes.size() > limit) {
            nodes.erase(nodes.end() - 1);
        }
        while (edges.size() > limit) {
            edges.erase(edges.end() - 1);
        }
    }
    return {{"nodes", nodes}, {"edges", edges}, {"query", request.query}};
}


}  // namespace qt::index
