#include "index/build_emulator_internal.hpp"

#include "index/path_util.hpp"
#include "index/string_util.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <unordered_map>

namespace qt::index::build_emulator_internal {

bool is_make_file_name(const std::filesystem::path& path) {
    const auto name = path.filename().string();
    const auto lower = lowercase_ascii(name);
    return name == "Makefile" || lower == "makefile" || name == "GNUmakefile" ||
           name == "Make.defs" || name == "Kbuild" || path.extension() == ".mk";
}

std::vector<std::filesystem::path> make_roots(const ProjectScanResult& scan) {
    std::set<std::string> roots;
    for (const auto& file : scan.files) {
        if (file.kind == ProjectFileKind::BuildFile && is_make_file_name(file.path)) {
            roots.insert((scan.root / file.path).parent_path().lexically_normal().string());
        }
    }
    std::vector<std::filesystem::path> result;
    result.reserve(roots.size());
    for (const auto& root : roots) {
        result.emplace_back(root);
    }
    return result;
}

std::set<std::string> make_root_lookup(const std::vector<std::filesystem::path>& roots) {
    std::set<std::string> lookup;
    for (const auto& root : roots) {
        lookup.insert(root.string());
    }
    return lookup;
}

std::optional<std::filesystem::path> owning_make_root(const std::set<std::string>& roots,
                                                      const std::filesystem::path& source,
                                                      const std::filesystem::path& project_root) {
    auto directory = source.parent_path();
    while (!directory.empty() && path_has_prefix(directory, project_root)) {
        if (roots.find(directory.string()) != roots.end()) {
            return directory;
        }
        if (directory == project_root) {
            break;
        }
        directory = directory.parent_path();
    }
    return std::nullopt;
}

std::string expand_make_variables(std::string value,
                                  const std::map<std::string, std::string>& variables) {
    for (std::size_t pos = 0; pos < value.size();) {
        auto start = value.find("$(", pos);
        const auto brace_start = value.find("${", pos);
        char close = ')';
        if (brace_start != std::string::npos && (start == std::string::npos || brace_start < start)) {
            start = brace_start;
            close = '}';
        }
        if (start == std::string::npos) {
            break;
        }
        const auto end = value.find(close, start + 2);
        if (end == std::string::npos) {
            break;
        }
        const auto key = value.substr(start + 2, end - start - 2);
        const auto found = variables.find(key);
        if (found == variables.end()) {
            pos = end + 1;
            continue;
        }
        value.replace(start, end - start + 1, found->second);
        pos = start + found->second.size();
    }
    return value;
}

void scan_make_file_variables(const std::filesystem::path& path,
                              std::map<std::string, std::string>& variables) {
    std::ifstream input(path);
    if (!input) {
        return;
    }

    std::string logical;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\\') {
            line.pop_back();
            logical += line;
            logical += ' ';
            continue;
        }
        logical += line;
        auto assignment = parse_assignment(logical);
        logical.clear();
        if (!assignment || assignment->first.empty()) {
            continue;
        }
        variables[assignment->first] =
            expand_make_variables(assignment->second, variables);
    }
}

void scan_kconfig_defines(const std::filesystem::path& path, std::set<std::string>& defines) {
    std::ifstream input(path);
    if (!input) {
        return;
    }

    std::string line;
    while (std::getline(input, line)) {
        line = trim_ascii(std::move(line));
        if (line.rfind("CONFIG_", 0) != 0) {
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        auto key = line.substr(0, equals);
        auto value = line.substr(equals + 1);
        if (value == "y") {
            add_define(defines, key + "=1");
        } else if (value == "n") {
            continue;
        } else {
            add_define(defines, key + "=" + value);
        }
    }
}

std::map<std::string, std::string> collect_make_variables(const ProjectScanResult& scan) {
    std::map<std::string, std::string> variables;
    variables["TOPDIR"] = scan.root.string();
    variables["CURDIR"] = scan.root.string();

    for (const auto& file : scan.files) {
        if (file.kind == ProjectFileKind::BuildFile && is_make_file_name(file.path)) {
            scan_make_file_variables(scan.root / file.path, variables);
        }
    }
    return variables;
}

std::vector<std::filesystem::path> make_include_roots(
    const std::filesystem::path& project_root,
    const std::filesystem::path& owner,
    const std::filesystem::path& source,
    const std::vector<std::filesystem::path>& variable_includes) {
    std::vector<std::filesystem::path> include_paths;
    std::set<std::string> seen;
    add_existing_include(include_paths, seen, project_root);
    add_existing_include(include_paths, seen, owner);
    add_existing_include(include_paths, seen, source.parent_path());

    for (const auto& conventional : {"include", "inc", "src"}) {
        add_existing_include(include_paths, seen, project_root / conventional);
        add_existing_include(include_paths, seen, owner / conventional);
    }

    for (const auto& include : variable_includes) {
        add_existing_include(include_paths, seen, include);
    }

    return include_paths;
}

bool is_safe_make_path_token(const std::string& token) {
    if (token.empty() || token == "/" || token == "." || token == "/dev/null") {
        return false;
    }
    if (token.find('$') != std::string::npos || token.find('(') != std::string::npos ||
        token.find(')') != std::string::npos || token.find('`') != std::string::npos ||
        token.find('<') != std::string::npos || token.find('>') != std::string::npos ||
        token.find('|') != std::string::npos || token.find('&') != std::string::npos ||
        token.find(';') != std::string::npos) {
        return false;
    }
    return true;
}

std::vector<std::filesystem::path> make_variable_include_roots(
    const std::filesystem::path& owner,
    const std::map<std::string, std::string>& variables) {
    std::vector<std::filesystem::path> include_paths;
    std::set<std::string> seen;
    for (const auto& [key, value] : variables) {
        const auto lower = lowercase_ascii(key);
        if (lower.find("include") == std::string::npos &&
            lower.find("incdir") == std::string::npos &&
            lower.find("cflags") == std::string::npos &&
            lower.find("cppflags") == std::string::npos &&
            lower.find("cxxflags") == std::string::npos) {
            continue;
        }
        auto tokens = split_shell_like(expand_make_variables(value, variables));
        for (std::size_t index = 0; index < tokens.size(); ++index) {
            const auto& token = tokens[index];
            if ((token == "-I" || token == "-isystem") && index + 1 < tokens.size()) {
                const auto include_token = tokens[++index];
                if (is_safe_make_path_token(include_token)) {
                    add_existing_include(include_paths, seen, normalize_path(owner, include_token));
                }
            } else if (token.rfind("-I", 0) == 0 && token.size() > 2) {
                const auto include_token = token.substr(2);
                if (is_safe_make_path_token(include_token)) {
                    add_existing_include(include_paths, seen, normalize_path(owner, include_token));
                }
            } else if ((token.find('/') != std::string::npos || token == "include") &&
                       is_safe_make_path_token(token)) {
                add_existing_include(include_paths, seen, normalize_path(owner, token));
            }
        }
    }

    return include_paths;
}

bool make_flag_takes_value(std::string_view token) {
    return token == "-I" || token == "-isystem" || token == "-include" || token == "-imacros" ||
           token == "-idirafter" || token == "-iquote" || token == "-D" || token == "-U" ||
           token == "/I" || token == "/D";
}

bool is_safe_make_flag_token(const std::string& token) {
    if (token.empty()) {
        return false;
    }
    if (token == "&&" || token == "||" || token == "|" || token == ";" ||
        token == "<" || token == ">" || token == "2>" || token == "1>" ||
        token == "/dev/null") {
        return false;
    }
    if (token.find('$') != std::string::npos || token.find('(') != std::string::npos ||
        token.find(')') != std::string::npos || token.find('`') != std::string::npos ||
        token.find('<') != std::string::npos || token.find('>') != std::string::npos ||
        token.find('|') != std::string::npos || token.find('&') != std::string::npos ||
        token.find(';') != std::string::npos) {
        return false;
    }
    return token.front() == '-' || token.rfind("/I", 0) == 0 || token.rfind("/D", 0) == 0;
}

std::vector<std::string> make_flag_tokens(const std::map<std::string, std::string>& variables,
                                          ProjectFileKind kind) {
    std::vector<std::string> tokens;
    const std::vector<std::string> keys = kind == ProjectFileKind::CSource
                                             ? std::vector<std::string>{"CPPFLAGS", "CFLAGS"}
                                             : std::vector<std::string>{"CPPFLAGS", "CXXFLAGS"};
    for (const auto& key : keys) {
        const auto found = variables.find(key);
        if (found == variables.end()) {
            continue;
        }
        auto split = split_shell_like(expand_make_variables(found->second, variables));
        for (std::size_t index = 0; index < split.size(); ++index) {
            auto token = std::move(split[index]);
            if (!is_safe_make_flag_token(token)) {
                continue;
            }
            const bool takes_value = make_flag_takes_value(token);
            tokens.push_back(std::move(token));
            if (takes_value && index + 1 < split.size()) {
                auto value = std::move(split[++index]);
                if (value.find('$') == std::string::npos && value.find('(') == std::string::npos &&
                    value.find(')') == std::string::npos && value.find('`') == std::string::npos &&
                    value.find('<') == std::string::npos && value.find('>') == std::string::npos &&
                    value.find('|') == std::string::npos && value.find('&') == std::string::npos &&
                    value.find(';') == std::string::npos) {
                    tokens.push_back(std::move(value));
                }
            }
        }
    }
    return tokens;
}

void emulate_make_projects(BuildEmulationResult& result,
                           std::set<std::string>& includes,
                           std::set<std::string>& defines,
                           const ProjectScanResult& scan) {
    const auto roots = make_roots(scan);
    if (roots.empty()) {
        return;
    }
    const auto root_lookup = make_root_lookup(roots);

    auto variables = collect_make_variables(scan);
    scan_kconfig_defines(scan.root / ".config", defines);
    std::map<std::string, std::vector<std::filesystem::path>> include_roots_by_owner;
    std::set<std::string> owners_with_cached_includes;

    for (const auto& file : scan.files) {
        if (!is_native_source_kind(file.kind) || is_ignored_dependency_path(file.path)) {
            continue;
        }
        const auto source = (scan.root / file.path).lexically_normal();
        const auto owner = owning_make_root(root_lookup, source, scan.root);
        if (!owner) {
            continue;
        }

        std::vector<std::string> arguments;
        arguments.emplace_back(file.kind == ProjectFileKind::CSource ? "cc" : "c++");
        arguments.emplace_back(file.kind == ProjectFileKind::CSource ? "-std=gnu11" : "-std=gnu++17");

        const auto flag_tokens = make_flag_tokens(variables, file.kind);
        scan_tokens_for_metadata(flag_tokens, *owner, includes, defines);
        append_compile_tokens(arguments, flag_tokens, *owner);

        const auto owner_key = owner->string();
        auto& owner_includes = include_roots_by_owner[owner_key];
        if (owners_with_cached_includes.insert(owner_key).second) {
            owner_includes = make_variable_include_roots(*owner, variables);
        }

        for (const auto& include : make_include_roots(scan.root, *owner, source, owner_includes)) {
            includes.insert(include.string());
            arguments.emplace_back("-I" + include.string());
        }
        for (const auto& define : defines) {
            if (define.rfind("CONFIG_", 0) == 0) {
                arguments.emplace_back("-D" + define);
            }
        }

        arguments.emplace_back("-c");
        arguments.emplace_back(source.string());
        arguments.emplace_back("-o");
        const auto relative_source = std::filesystem::relative(source, scan.root);
        arguments.emplace_back((scan.root / "build/index-make" / relative_source).string() + ".o");

        record_build_system_command(result, source, *owner, std::move(arguments), "make");
    }
}


}  // namespace qt::index::build_emulator_internal
