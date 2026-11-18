#include "index/build_emulator_internal.hpp"

#include "index/path_util.hpp"
#include "index/string_util.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

namespace qt::index::build_emulator_internal {

std::string ninja_unescape(std::string value) {
    std::string output;
    output.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '$' && index + 1 < value.size()) {
            const char next = value[index + 1];
            if (next == ' ' || next == ':' || next == '$') {
                output.push_back(next);
                ++index;
                continue;
            }
        }
        output.push_back(value[index]);
    }
    return output;
}

std::optional<std::string> compiler_from_rule_command(const std::string& command) {
    for (auto token : split_shell_like(command)) {
        const auto lower = lowercase_ascii(token);
        if (lower.find("ccache") != std::string::npos) {
            continue;
        }
        const auto variable_end = token.rfind('}');
        if (variable_end != std::string::npos && variable_end + 1 < token.size()) {
            token = token.substr(variable_end + 1);
        }
        const auto name = lowercase_ascii(std::filesystem::path(token).filename().string());
        if (name.find("gcc") != std::string::npos || name.find("g++") != std::string::npos ||
            name.find("clang") != std::string::npos || name.find("clang++") != std::string::npos ||
            name == "cc" || name == "c++") {
            return token;
        }
    }
    return std::nullopt;
}

std::unordered_map<std::string, std::string> scan_ninja_rule_compilers(
    const std::filesystem::path& build_dir) {
    std::unordered_map<std::string, std::string> compilers;
    for (const auto& path : {build_dir / "CMakeFiles" / "rules.ninja",
                             build_dir / "rules.ninja",
                             build_dir / "build.ninja"}) {
        std::ifstream input(path);
        if (!input) {
            continue;
        }

        std::string current_rule;
        std::string line;
        while (std::getline(input, line)) {
            if (line.rfind("rule ", 0) == 0) {
                current_rule = trim_ascii(line.substr(5));
                continue;
            }
            if (current_rule.empty() || line.empty() ||
                !std::isspace(static_cast<unsigned char>(line[0]))) {
                if (!line.empty() && !std::isspace(static_cast<unsigned char>(line[0]))) {
                    current_rule.clear();
                }
                continue;
            }
            const auto assignment = parse_assignment(line);
            if (!assignment || assignment->first != "command") {
                continue;
            }
            const auto compiler = compiler_from_rule_command(assignment->second);
            if (compiler) {
                compilers.emplace(current_rule, *compiler);
            }
        }
    }
    return compilers;
}

struct NinjaBuildEdge {
    std::filesystem::path output;
    std::filesystem::path input;
    std::string rule;
    std::map<std::string, std::string> variables;
};

std::optional<NinjaBuildEdge> parse_ninja_build_line(const std::filesystem::path& build_dir,
                                                     const std::string& line) {
    if (line.rfind("build ", 0) != 0) {
        return std::nullopt;
    }
    const auto colon = line.find(':', 6);
    if (colon == std::string::npos) {
        return std::nullopt;
    }

    const auto outputs = split_shell_like(ninja_unescape(line.substr(6, colon - 6)));
    auto rhs = line.substr(colon + 1);
    const auto order_only = rhs.find(" || ");
    if (order_only != std::string::npos) {
        rhs.resize(order_only);
    }
    const auto implicit = rhs.find(" | ");
    if (implicit != std::string::npos) {
        rhs.resize(implicit);
    }

    const auto rhs_tokens = split_shell_like(ninja_unescape(rhs));
    if (outputs.empty() || rhs_tokens.size() < 2) {
        return std::nullopt;
    }

    NinjaBuildEdge edge;
    edge.output = normalize_path(build_dir, outputs.front());
    edge.rule = rhs_tokens.front();
    for (std::size_t index = 1; index < rhs_tokens.size(); ++index) {
        const auto input = normalize_path(build_dir, rhs_tokens[index]);
        if (is_source_path(input)) {
            edge.input = input;
            break;
        }
    }
    if (edge.input.empty()) {
        return std::nullopt;
    }
    return edge;
}

void record_ninja_edge(BuildEmulationResult& result,
                       std::set<std::string>& includes,
                       std::set<std::string>& defines,
                       const std::filesystem::path& build_dir,
                       const NinjaBuildEdge& edge,
                       const std::unordered_map<std::string, std::string>& compilers) {
    const auto compiler = compilers.find(edge.rule);
    std::vector<std::string> arguments;
    arguments.emplace_back(compiler == compilers.end() ? (is_c_source_path(edge.input) ? "cc" : "c++")
                                                       : compiler->second);

    for (const std::string key : {"DEFINES", "INCLUDES", "FLAGS"}) {
        const auto value = edge.variables.find(key);
        if (value == edge.variables.end()) {
            continue;
        }
        auto tokens = split_shell_like(value->second);
        scan_tokens_for_metadata(tokens, build_dir, includes, defines);
        append_compile_tokens(arguments, tokens, build_dir);
    }
    arguments.emplace_back("-c");
    arguments.emplace_back(edge.input.string());
    arguments.emplace_back("-o");
    arguments.emplace_back(edge.output.string());

    auto& observed = result.compile_commands[edge.input.string()];
    if (observed.arguments.empty()) {
        observed.directory = build_dir;
        observed.arguments = std::move(arguments);
        observed.backend = "ninja";
        observed.origin = "emulated";
        observed.confidence = 90;
        ++result.edges_by_backend["ninja"];
    }
}

void emulate_ninja(BuildEmulationResult& result,
                   std::set<std::string>& includes,
                   std::set<std::string>& defines,
                   const std::filesystem::path& build_ninja) {
    std::ifstream input(build_ninja);
    if (!input) {
        return;
    }

    const auto build_dir = build_ninja.parent_path();
    const auto compilers = scan_ninja_rule_compilers(build_dir);
    std::optional<NinjaBuildEdge> current_edge;
    std::string line;
    while (std::getline(input, line)) {
        if (line.rfind("build ", 0) == 0) {
            if (current_edge) {
                record_ninja_edge(result, includes, defines, build_dir, *current_edge, compilers);
            }
            current_edge = parse_ninja_build_line(build_dir, line);
            continue;
        }
        if (!current_edge) {
            continue;
        }
        if (line.empty() || !std::isspace(static_cast<unsigned char>(line[0]))) {
            record_ninja_edge(result, includes, defines, build_dir, *current_edge, compilers);
            current_edge.reset();
            continue;
        }
        const auto assignment = parse_assignment(line);
        if (assignment) {
            current_edge->variables[assignment->first] = assignment->second;
        }
    }
    if (current_edge) {
        record_ninja_edge(result, includes, defines, build_dir, *current_edge, compilers);
    }
}


}  // namespace qt::index::build_emulator_internal
