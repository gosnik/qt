#include "index/project_scan.hpp"

#include "index/cancellation.hpp"
#include "index/gitignore.hpp"
#include "index/path_util.hpp"
#include "index/string_util.hpp"

#include <algorithm>
#include <chrono>
#include <ostream>
#include <stdexcept>
#include <thread>
#include <unordered_set>

namespace qt::index {
namespace {

void low_priority_checkpoint(bool low_priority, std::size_t operations) {
    scan_cancellation_checkpoint();
    if (!low_priority) {
        return;
    }
    std::this_thread::yield();
    if (operations % 64U == 0U) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool has_extension(const std::filesystem::path& path,
                   const std::unordered_set<std::string>& extensions) {
    return extensions.find(lowercase_ascii(path.extension().string())) != extensions.end();
}

std::string build_file_type(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    const std::string lower_name = lowercase_ascii(name);
    const std::string ext = lowercase_ascii(path.extension().string());

    if (name == "CMakeLists.txt" || name == "CMakeLists.txt.in" || ext == ".cmake" ||
        name == "CMakePresets.json" || name == "CMakeUserPresets.json") {
        return "CMake";
    }
    if (name == "Makefile" || name == "makefile" || name == "GNUmakefile" || ext == ".mk") {
        return "Make";
    }
    if (lower_name == "build.ninja" || lower_name == "rules.ninja" || ext == ".ninja") {
        return "Ninja";
    }
    if (lower_name == "meson.build" || lower_name == "meson.options" ||
        lower_name == "meson_options.txt" || lower_name == "cross-file.txt" ||
        lower_name == "native-file.txt") {
        return "Meson";
    }
    if (lower_name == "configure.ac" || lower_name == "configure.in" ||
        lower_name == "makefile.am" || lower_name == "makefile.in" || lower_name == "aclocal.m4") {
        return "Autotools";
    }
    if (name == "WORKSPACE" || name == "WORKSPACE.bazel" || name == "MODULE.bazel" ||
        name == "BUILD" || name == "BUILD.bazel" || name == ".bazelrc") {
        return "Bazel";
    }
    if (name == "BUCK" || name == "BUCK.v2" || name == ".buckconfig") {
        return "Buck";
    }
    if (ext == ".pro" || ext == ".pri" || ext == ".prf") {
        return "QMake";
    }
    if (lower_name == "sconstruct" || lower_name == "sconscript") {
        return "SCons";
    }
    if (lower_name == "premake4.lua" || lower_name == "premake5.lua") {
        return "Premake";
    }
    if (lower_name == "xmake.lua") {
        return "Xmake";
    }
    if (lower_name == "wscript") {
        return "Waf";
    }
    if (name == "BUILD.gn" || ext == ".gni" || name == ".gn" || name == "args.gn") {
        return "GN";
    }
    if (ext == ".sln" || ext == ".vcxproj" || ext == ".filters" || ext == ".props" ||
        ext == ".targets") {
        return "MSBuild";
    }
    if (ext == ".xcodeproj" || ext == ".xcworkspace" || name == "project.pbxproj" ||
        name == "Package.swift") {
        return "Xcode";
    }
    if (lower_name == "platformio.ini" || lower_name == "library.json" ||
        lower_name == "module.json" || lower_name == "library.properties" ||
        lower_name == "sketch.yaml") {
        return "Embedded";
    }
    if (lower_name == "cargo.toml" || lower_name == "cargo.lock" || lower_name == "build.rs") {
        return "Cargo";
    }
    if (lower_name == "package.json" || lower_name == "package-lock.json" ||
        lower_name == "vite.config.ts" || lower_name == "vite.config.js" ||
        lower_name == "tsconfig.json") {
        return "Npm";
    }
    if (lower_name == "setup.py" || lower_name == "setup.cfg" ||
        lower_name == "pyproject.toml" || lower_name == "requirements.txt") {
        return "Python";
    }
    if (ext == ".px4board" || ext == ".board") {
        return "Embedded";
    }
    if (lower_name == "vcpkg.json" || lower_name == "conanfile.txt" ||
        lower_name == "conanfile.py" || lower_name == "conan.lock" ||
        lower_name == "manifest.json" || lower_name == "packages.config") {
        return "Package";
    }
    if (lower_name == "flake.nix" || lower_name == "flake.lock" ||
        lower_name == "shell.nix" || lower_name == "default.nix") {
        return "Nix";
    }
    if (name == "Dockerfile" || lower_name == "docker-compose.yml" ||
        lower_name == "docker-compose.yaml" || lower_name == "devcontainer.json") {
        return "Container";
    }

    return {};
}

ProjectFileKind classify_file(const std::filesystem::path& path) {
    static const std::unordered_set<std::string> c_sources{".c"};
    static const std::unordered_set<std::string> cxx_sources{".cc", ".cpp", ".cxx", ".c++", ".ino"};
    static const std::unordered_set<std::string> rust_sources{".rs"};
    static const std::unordered_set<std::string> ts_sources{".ts", ".tsx"};
    static const std::unordered_set<std::string> python_sources{".py"};
    static const std::unordered_set<std::string> headers{
        ".h", ".hh", ".hpp", ".hxx", ".ipp", ".inl"};
    static const std::unordered_set<std::string> configs{
        ".json", ".yaml", ".yml", ".toml", ".xml", ".ini", ".conf"};

    if (!build_file_type(path).empty()) {
        return ProjectFileKind::BuildFile;
    }
    if (has_extension(path, c_sources)) {
        return ProjectFileKind::CSource;
    }
    if (has_extension(path, cxx_sources)) {
        return ProjectFileKind::CxxSource;
    }
    if (has_extension(path, rust_sources)) {
        return ProjectFileKind::RustSource;
    }
    if (has_extension(path, ts_sources)) {
        return ProjectFileKind::TypeScriptSource;
    }
    if (lowercase_ascii(path.extension().string()) == ".vue") {
        return ProjectFileKind::VueSource;
    }
    if (has_extension(path, python_sources)) {
        return ProjectFileKind::PythonSource;
    }
    if (has_extension(path, headers)) {
        return ProjectFileKind::Header;
    }
    if (has_extension(path, configs)) {
        return ProjectFileKind::Config;
    }

    return ProjectFileKind::Other;
}

const char* kind_name(ProjectFileKind kind) {
    switch (kind) {
        case ProjectFileKind::CSource:
            return "c_sources";
        case ProjectFileKind::CxxSource:
            return "cxx_sources";
        case ProjectFileKind::RustSource:
            return "rust_sources";
        case ProjectFileKind::TypeScriptSource:
            return "typescript_sources";
        case ProjectFileKind::VueSource:
            return "vue_sources";
        case ProjectFileKind::PythonSource:
            return "python_sources";
        case ProjectFileKind::Header:
            return "headers";
        case ProjectFileKind::BuildFile:
            return "build_files";
        case ProjectFileKind::Config:
            return "configs";
        case ProjectFileKind::Other:
            return "other";
    }
    return "other";
}

}  // namespace

ProjectScanResult scan_project_tree(const std::filesystem::path& root) {
    return scan_project_tree(root, false);
}

ProjectScanResult scan_project_tree(const std::filesystem::path& root, bool low_priority) {
    if (!std::filesystem::exists(root)) {
        throw std::runtime_error("project root does not exist: " + root.string());
    }

    ProjectScanResult result;
    result.root = std::filesystem::weakly_canonical(root);
    const auto gitignore = GitIgnoreMatcher::load(result.root);

    const auto record_compile_database = [&](const std::filesystem::path& path) {
        std::error_code relative_ec;
        auto relative = std::filesystem::relative(path, result.root, relative_ec);
        result.existing_compilation_databases.push_back(relative_ec ? path : relative);
    };

    const auto root_compile_database = result.root / "compile_commands.json";
    if (std::filesystem::is_regular_file(root_compile_database)) {
        record_compile_database(root_compile_database);
    }

    const auto build_dir = result.root / "build";
    if (std::filesystem::is_directory(build_dir)) {
        std::error_code build_ec;
        for (std::filesystem::directory_iterator build_it(build_dir, build_ec);
             !build_ec && build_it != std::filesystem::directory_iterator{};
             build_it.increment(build_ec)) {
            if (!build_it->is_directory(build_ec)) {
                continue;
            }
            const auto candidate = build_it->path() / "compile_commands.json";
            if (std::filesystem::is_regular_file(candidate)) {
                record_compile_database(candidate);
            }
        }
    }

    std::error_code ec;
    const std::filesystem::recursive_directory_iterator end;
    std::size_t visited = 0;
    for (std::filesystem::recursive_directory_iterator it(
             result.root, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != end; it.increment(ec)) {
        low_priority_checkpoint(low_priority, ++visited);
        if (it->is_directory(ec)) {
            if (is_nix_store_path(it->path()) || should_skip_scan_directory(it->path()) ||
                gitignore.ignores(it->path(), true)) {
                ++result.skipped_directories;
                it.disable_recursion_pending();
            }
            continue;
        }

        if (!it->is_regular_file(ec) || is_nix_store_path(it->path()) ||
            gitignore.ignores(it->path(), false)) {
            continue;
        }

        ProjectFile file;
        file.path = std::filesystem::relative(it->path(), result.root, ec);
        if (ec) {
            ec.clear();
            file.path = it->path();
        }
        file.kind = classify_file(file.path);

        if (file.kind == ProjectFileKind::BuildFile) {
            ++result.build_files_by_type[build_file_type(file.path)];
        }

        result.files.push_back(std::move(file));
    }

    std::sort(result.files.begin(), result.files.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.path.generic_string() < rhs.path.generic_string();
    });
    std::sort(result.existing_compilation_databases.begin(),
              result.existing_compilation_databases.end(),
              [](const auto& lhs, const auto& rhs) {
                  return lhs.generic_string() < rhs.generic_string();
              });

    return result;
}

void write_scan_summary(const ProjectScanResult& scan, std::ostream& out) {
    std::map<ProjectFileKind, std::size_t> counts;
    for (const auto& file : scan.files) {
        ++counts[file.kind];
    }

    out << "project_root=" << scan.root.string() << '\n'
        << "total_files=" << scan.files.size() << '\n'
        << "skipped_directories=" << scan.skipped_directories << '\n';

    for (const auto kind : {ProjectFileKind::CSource,
                           ProjectFileKind::CxxSource,
                           ProjectFileKind::RustSource,
                           ProjectFileKind::TypeScriptSource,
                           ProjectFileKind::VueSource,
                           ProjectFileKind::PythonSource,
                           ProjectFileKind::Header,
                           ProjectFileKind::BuildFile,
                           ProjectFileKind::Config,
                           ProjectFileKind::Other}) {
        out << kind_name(kind) << '=' << counts[kind] << '\n';
    }

    out << "build_file_types=";
    bool first = true;
    for (const auto& [type, count] : scan.build_files_by_type) {
        if (!first) {
            out << ',';
        }
        first = false;
        out << type << ':' << count;
    }
    out << '\n';

    out << "existing_compilation_databases=" << scan.existing_compilation_databases.size() << '\n';
    for (const auto& path : scan.existing_compilation_databases) {
        out << "compile_database=" << path.generic_string() << '\n';
    }
}

}  // namespace qt::index
