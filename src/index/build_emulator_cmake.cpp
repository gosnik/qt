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

struct CMakeMakeFlags {
    std::string compiler;
    std::map<std::string, std::string> variables;
};

CMakeMakeFlags scan_flags_make(const std::filesystem::path& flags_make) {
    CMakeMakeFlags flags;
    std::ifstream input(flags_make);
    if (!input) {
        return flags;
    }

    std::string line;
    while (std::getline(input, line)) {
        if (line.rfind("# compile ", 0) == 0) {
            const auto with = line.find(" with ");
            if (with != std::string::npos) {
                flags.compiler = trim_ascii(line.substr(with + 6));
            }
            continue;
        }
        const auto assignment = parse_assignment(line);
        if (assignment) {
            flags.variables[assignment->first] = assignment->second;
        }
    }
    return flags;
}

std::filesystem::path cmake_make_build_root_for(const std::filesystem::path& flags_make) {
    auto dir = flags_make.parent_path();
    while (!dir.empty() && dir.filename() != "CMakeFiles") {
        dir = dir.parent_path();
    }
    return !dir.empty() && dir.filename() == "CMakeFiles" ? dir.parent_path()
                                                          : flags_make.parent_path();
}

std::vector<std::pair<std::filesystem::path, std::filesystem::path>> scan_depend_info(
    const std::filesystem::path& depend_info,
    const std::filesystem::path& build_root) {
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> sources;
    const auto text = read_text_file(depend_info);
    const auto block = text.find("CMAKE_DEPENDS_DEPENDENCY_FILES");
    if (block == std::string::npos) {
        return sources;
    }

    std::vector<std::string> values;
    for (std::size_t pos = block; pos < text.size();) {
        pos = text.find('"', pos);
        if (pos == std::string::npos) {
            break;
        }
        auto value = parse_quoted_string_at(text, pos);
        if (!value) {
            break;
        }
        values.push_back(std::move(*value));
    }

    for (std::size_t index = 0; index + 1 < values.size(); index += 4) {
        auto source = normalize_path(build_root, values[index]);
        if (is_source_path(source)) {
            sources.emplace_back(std::move(source), normalize_path(build_root, values[index + 1]));
        }
    }
    return sources;
}

void record_cmake_make_source(BuildEmulationResult& result,
                              std::set<std::string>& includes,
                              std::set<std::string>& defines,
                              const std::filesystem::path& build_root,
                              const CMakeMakeFlags& flags,
                              const std::filesystem::path& source,
                              const std::filesystem::path& output) {
    std::vector<std::string> arguments;
    arguments.emplace_back(flags.compiler.empty() ? (is_c_source_path(source) ? "cc" : "c++")
                                                  : flags.compiler);

    const std::vector<std::string> prefixes{is_c_source_path(source) ? "C" : "CXX", "C", "CXX"};
    std::set<std::string> added_keys;
    for (const auto& prefix : prefixes) {
        for (const auto& suffix : {"_DEFINES", "_INCLUDES", "_FLAGS"}) {
            const auto key = prefix + std::string(suffix);
            if (!added_keys.insert(key).second) {
                continue;
            }
            const auto value = flags.variables.find(key);
            if (value == flags.variables.end()) {
                continue;
            }
            auto tokens = split_shell_like(value->second);
            scan_tokens_for_metadata(tokens, build_root, includes, defines);
            append_compile_tokens(arguments, tokens, build_root);
        }
    }

    arguments.emplace_back("-c");
    arguments.emplace_back(source.string());
    arguments.emplace_back("-o");
    arguments.emplace_back(output.string());

    auto& observed = result.compile_commands[source.string()];
    if (observed.arguments.empty()) {
        observed.directory = build_root;
        observed.arguments = std::move(arguments);
        observed.backend = "cmake-make";
        observed.origin = "emulated";
        observed.confidence = 85;
        ++result.edges_by_backend["cmake-make"];
    }
}

void emulate_cmake_make(BuildEmulationResult& result,
                        std::set<std::string>& includes,
                        std::set<std::string>& defines,
                        const std::filesystem::path& build_root) {
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(
             build_root, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator{};
         it.increment(ec)) {
        if (it->is_directory(ec)) {
            const auto name = it->path().filename().string();
            if (name == "_deps" || name == "external" || name == "Testing") {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file(ec) || it->path().filename() != "flags.make") {
            continue;
        }

        const auto flags = scan_flags_make(it->path());
        const auto effective_build_root = cmake_make_build_root_for(it->path());
        const auto depend_info = it->path().parent_path() / "DependInfo.cmake";
        for (const auto& [source, output] : scan_depend_info(depend_info, effective_build_root)) {
            record_cmake_make_source(
                result, includes, defines, effective_build_root, flags, source, output);
        }
    }
}


}  // namespace qt::index::build_emulator_internal
