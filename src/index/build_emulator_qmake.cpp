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

struct QMakeProjectFacts {
    std::vector<std::filesystem::path> sources;
    std::vector<std::filesystem::path> includes;
    std::vector<std::string> defines;
};

QMakeProjectFacts parse_qmake_project_file(const std::filesystem::path& project_file) {
    QMakeProjectFacts facts;
    std::set<std::string> seen_sources;
    std::set<std::string> seen_includes;
    std::set<std::string> seen_defines;
    std::ifstream input(project_file);
    if (!input) {
        return facts;
    }

    const auto base = project_file.parent_path();
    std::string logical;
    std::string line;
    while (std::getline(input, line)) {
        const auto comment = line.find('#');
        if (comment != std::string::npos) {
            line.resize(comment);
        }
        line = trim_ascii(std::move(line));
        if (line.empty()) {
            continue;
        }
        if (!line.empty() && line.back() == '\\') {
            line.pop_back();
            logical += line;
            logical += ' ';
            continue;
        }
        logical += line;
        const auto assignment = parse_assignment(logical);
        logical.clear();
        if (!assignment) {
            continue;
        }

        const auto key = lowercase_ascii(assignment->first);
        for (auto token : split_shell_like(assignment->second)) {
            token.erase(std::remove(token.begin(), token.end(), '\\'), token.end());
            token = trim_ascii(std::move(token));
            if (token.empty() || token.find('$') != std::string::npos) {
                continue;
            }
            if (key == "sources") {
                const auto source = normalize_path(base, token);
                if (is_source_path(source) && std::filesystem::is_regular_file(source) &&
                    seen_sources.insert(source.string()).second) {
                    facts.sources.push_back(source);
                }
            } else if (key == "includepath") {
                const auto include = normalize_path(base, token);
                if (std::filesystem::is_directory(include) &&
                    seen_includes.insert(include.string()).second) {
                    facts.includes.push_back(include);
                }
            } else if (key == "defines") {
                if (token.rfind("-D", 0) == 0) {
                    token = token.substr(2);
                }
                if (!token.empty() && seen_defines.insert(token).second) {
                    facts.defines.push_back(token);
                }
            }
        }
    }
    return facts;
}

void emulate_qmake_projects(BuildEmulationResult& result,
                            std::set<std::string>& includes,
                            std::set<std::string>& defines,
                            const ProjectIndex& index) {
    for (const auto& project_file : qmake_project_files(index)) {
        const auto facts = parse_qmake_project_file(project_file);
        const auto project_dir = project_file.parent_path();
        for (const auto& source : facts.sources) {
            std::vector<std::string> arguments;
            arguments.emplace_back(is_c_source_path(source) ? "cc" : "c++");
            arguments.emplace_back(is_c_source_path(source) ? "-std=gnu11" : "-std=gnu++17");

            for (const auto& define : facts.defines) {
                defines.insert(define);
                arguments.emplace_back("-D" + define);
            }

            std::vector<std::filesystem::path> source_includes = facts.includes;
            std::set<std::string> seen_include;
            for (const auto& include : source_includes) {
                seen_include.insert(include.string());
            }
            add_existing_include(source_includes, seen_include, project_dir);
            add_existing_include(source_includes, seen_include, source.parent_path());
            for (const auto& include : source_includes) {
                includes.insert(include.string());
                arguments.emplace_back("-I" + include.string());
            }

            arguments.emplace_back("-c");
            arguments.emplace_back(source.string());
            arguments.emplace_back("-o");
            arguments.emplace_back((project_dir / "build/index-qmake" /
                                    (source.filename().string() + ".o"))
                                       .string());

            record_build_system_command(result, source, project_dir, std::move(arguments), "qmake");
        }
    }
}


}  // namespace qt::index::build_emulator_internal
