#include "index/build_emulator.hpp"

#include "index/build_emulator_internal.hpp"
#include "index/cancellation.hpp"
#include "index/path_util.hpp"
#include "index/project_index.hpp"
#include "index/string_util.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <set>
#include <thread>

namespace qt::index::build_emulator_internal {

bool is_source_path(const std::filesystem::path& path) {
    const auto ext = lowercase_ascii(path.extension().string());
    return ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".c++" ||
           ext == ".ino";
}

bool is_native_source_kind(ProjectFileKind kind) {
    return kind == ProjectFileKind::CSource || kind == ProjectFileKind::CxxSource;
}

bool is_c_source_path(const std::filesystem::path& path) {
    return lowercase_ascii(path.extension().string()) == ".c";
}

void add_existing_include(std::vector<std::filesystem::path>& includes,
                          std::set<std::string>& seen,
                          const std::filesystem::path& path);

void record_build_system_command(BuildEmulationResult& result,
                                 const std::filesystem::path& source,
                                 const std::filesystem::path& directory,
                                 std::vector<std::string> arguments,
                                 const std::string& backend);

std::optional<std::string> parse_quoted_string_at(const std::string& text, std::size_t& pos) {
    if (pos >= text.size() || text[pos] != '"') {
        return std::nullopt;
    }

    std::string value;
    bool escaped = false;
    for (++pos; pos < text.size(); ++pos) {
        const char ch = text[pos];
        if (escaped) {
            switch (ch) {
                case '"':
                case '\\':
                case '/':
                    value.push_back(ch);
                    break;
                case 'b':
                    value.push_back('\b');
                    break;
                case 'f':
                    value.push_back('\f');
                    break;
                case 'n':
                    value.push_back('\n');
                    break;
                case 'r':
                    value.push_back('\r');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                default:
                    value.push_back(ch);
                    break;
            }
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            ++pos;
            return value;
        }
        value.push_back(ch);
    }
    return std::nullopt;
}

std::optional<std::pair<std::string, std::string>> parse_assignment(std::string line) {
    line = trim_ascii(std::move(line));
    const auto comment = line.find('#');
    if (comment != std::string::npos) {
        line.resize(comment);
        line = trim_ascii(std::move(line));
    }
    const auto equals = line.find('=');
    if (equals == std::string::npos) {
        return std::nullopt;
    }
    auto key = trim_ascii(line.substr(0, equals));
    for (const std::string suffix : {"+", ":", "?", "!"}) {
        if (!key.empty() && key.back() == suffix[0]) {
            key.pop_back();
            key = trim_ascii(std::move(key));
            break;
        }
    }
    return std::make_pair(std::move(key), trim_ascii(line.substr(equals + 1)));
}

std::string strip_ini_comment(std::string line) {
    for (std::size_t index = 0; index < line.size(); ++index) {
        if (line[index] != ';' && line[index] != '#') {
            continue;
        }
        if (index == 0 || std::isspace(static_cast<unsigned char>(line[index - 1]))) {
            line.resize(index);
            break;
        }
    }
    return trim_ascii(std::move(line));
}

void add_define(std::set<std::string>& defines, std::string value) {
    if (value.rfind("-D", 0) == 0) {
        value = value.substr(2);
    }
    value = trim_ascii(std::move(value));
    if (value.empty() || value.find('$') != std::string::npos || value.find('/') != std::string::npos) {
        return;
    }
    const auto lead = static_cast<unsigned char>(value.front());
    if (!std::isalpha(lead) && value.front() != '_') {
        return;
    }
    defines.insert(value);
}

void add_include(std::set<std::string>& includes,
                 const std::filesystem::path& base,
                 std::string value) {
    if (value.rfind("-I", 0) == 0) {
        value = value.substr(2);
    }
    value = trim_ascii(std::move(value));
    if (value.empty() || value == "SYSTEM" || value.find('$') != std::string::npos) {
        return;
    }
    const auto path = normalize_path(base, value);
    if (std::filesystem::is_directory(path)) {
        includes.insert(path.string());
    }
}

void scan_tokens_for_metadata(const std::vector<std::string>& tokens,
                              const std::filesystem::path& base,
                              std::set<std::string>& includes,
                              std::set<std::string>& defines) {
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        const auto& token = tokens[index];
        if ((token == "-I" || token == "-isystem" || token == "/I") && index + 1 < tokens.size()) {
            add_include(includes, base, tokens[++index]);
        } else if (token.rfind("-I", 0) == 0) {
            add_include(includes, base, token.substr(2));
        } else if (token.rfind("/I", 0) == 0) {
            add_include(includes, base, token.substr(2));
        } else if ((token == "-D" || token == "/D") && index + 1 < tokens.size()) {
            add_define(defines, tokens[++index]);
        } else if (token.rfind("-D", 0) == 0) {
            add_define(defines, token.substr(2));
        } else if (token.rfind("/D", 0) == 0) {
            add_define(defines, token.substr(2));
        }
    }
}

void append_compile_tokens(std::vector<std::string>& arguments,
                           const std::vector<std::string>& tokens,
                           const std::filesystem::path& base) {
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        const auto& token = tokens[index];
        if ((token == "-I" || token == "-isystem" || token == "/I") && index + 1 < tokens.size()) {
            arguments.push_back(token);
            arguments.push_back(normalize_path(base, tokens[++index]).string());
        } else if (token.rfind("-I", 0) == 0 && token.size() > 2) {
            arguments.push_back("-I" + normalize_path(base, token.substr(2)).string());
        } else if (token.rfind("/I", 0) == 0 && token.size() > 2) {
            arguments.push_back("/I" + normalize_path(base, token.substr(2)).string());
        } else {
            arguments.push_back(token);
        }
    }
}


std::vector<std::filesystem::path> project_build_files(const ProjectIndex& index,
                                                       const std::string& filename) {
    std::vector<std::filesystem::path> result;
    for (const auto& path : index.build_files_named(filename)) {
        if (!is_ignored_dependency_path(path)) {
            result.push_back(path);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::filesystem::path> qmake_project_files(const ProjectIndex& index) {
    std::vector<std::filesystem::path> result;
    for (const auto& ext : {".pro", ".pri", ".prf"}) {
        for (const auto& path : index.build_files_with_extension(ext)) {
            if (!is_ignored_dependency_path(path)) {
                result.push_back(path);
            }
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

void record_build_system_command(BuildEmulationResult& result,
                                 const std::filesystem::path& source,
                                 const std::filesystem::path& directory,
                                 std::vector<std::string> arguments,
                                 const std::string& backend) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec)) {
        return;
    }
    auto& observed = result.compile_commands[source.lexically_normal().string()];
    if (observed.arguments.empty()) {
        observed.directory = directory;
        observed.arguments = std::move(arguments);
        observed.backend = backend;
        observed.origin = "emulated";
        observed.confidence = backend == "make" ? 55 : 75;
        ++result.edges_by_backend[backend];
    }
}

void add_existing_include(std::vector<std::filesystem::path>& includes,
                          std::set<std::string>& seen,
                          const std::filesystem::path& path) {
    std::error_code ec;
    const auto normalized = std::filesystem::weakly_canonical(path, ec);
    if (!ec && std::filesystem::is_directory(normalized, ec) &&
        seen.insert(normalized.string()).second) {
        includes.push_back(normalized);
    }
}

std::vector<std::filesystem::path> build_roots(const ProjectScanResult& scan) {
    std::set<std::string> roots;
    for (const auto& relative_database : scan.existing_compilation_databases) {
        if (relative_database == std::filesystem::path{"compile_commands.json"}) {
            continue;
        }
        roots.insert((scan.root / relative_database).parent_path().lexically_normal().string());
    }

    const auto build_dir = scan.root / "build";
    std::error_code ec;
    if (std::filesystem::is_directory(build_dir, ec)) {
        for (std::filesystem::directory_iterator it(build_dir, ec);
             !ec && it != std::filesystem::directory_iterator{};
             it.increment(ec)) {
            if (it->is_directory(ec)) {
                roots.insert(it->path().lexically_normal().string());
            }
        }
    }

    std::vector<std::filesystem::path> result;
    result.reserve(roots.size());
    for (const auto& root : roots) {
        result.emplace_back(root);
    }
    return result;
}

}  // namespace qt::index::build_emulator_internal

namespace qt::index {

namespace {

void low_priority_checkpoint(bool low_priority) {
    scan_cancellation_checkpoint();
    if (!low_priority) {
        return;
    }
    std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

}  // namespace

BuildEmulationResult emulate_build_systems(const ProjectScanResult& scan) {
    return emulate_build_systems(scan, false);
}

BuildEmulationResult emulate_build_systems(const ProjectScanResult& scan, bool low_priority) {
    using namespace build_emulator_internal;

    BuildEmulationResult result;
    const ProjectIndex index(scan);
    std::set<std::string> includes;
    std::set<std::string> defines;

    for (const auto& root : build_roots(scan)) {
        const auto build_ninja = root / "build.ninja";
        if (std::filesystem::is_regular_file(build_ninja)) {
            emulate_ninja(result, includes, defines, build_ninja);
            low_priority_checkpoint(low_priority);
        }
        emulate_cmake_make(result, includes, defines, root);
        low_priority_checkpoint(low_priority);
    }
    emulate_cargo_projects(result, scan, index);
    low_priority_checkpoint(low_priority);
    emulate_npm_projects(result, scan, index);
    low_priority_checkpoint(low_priority);
    emulate_platformio_projects(result, includes, defines, scan, index);
    low_priority_checkpoint(low_priority);
    emulate_qmake_projects(result, includes, defines, index);
    low_priority_checkpoint(low_priority);
    emulate_python_projects(result, scan, index);
    low_priority_checkpoint(low_priority);
    emulate_waf_projects(result, includes, scan);
    low_priority_checkpoint(low_priority);
    emulate_make_projects(result, includes, defines, scan);
    low_priority_checkpoint(low_priority);

    for (const auto& include : includes) {
        result.include_paths.emplace_back(include);
    }
    result.defines.assign(defines.begin(), defines.end());
    return result;
}

std::string build_emulation_summary(const BuildEmulationResult& emulation) {
    std::string summary = "commands=" + std::to_string(emulation.compile_commands.size()) +
                          " include_paths=" + std::to_string(emulation.include_paths.size()) +
                          " defines=" + std::to_string(emulation.defines.size()) +
                          " backends=";
    bool first = true;
    for (const auto& [backend, count] : emulation.edges_by_backend) {
        if (!first) {
            summary += ',';
        }
        first = false;
        summary += backend + ':' + std::to_string(count);
    }
    return summary;
}

}  // namespace qt::index
