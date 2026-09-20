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

void emulate_cargo_projects(BuildEmulationResult& result,
                            const ProjectScanResult& scan,
                            const ProjectIndex& index) {
    for (const auto& manifest : project_build_files(index, "Cargo.toml")) {
        const auto project_dir = manifest.parent_path();
        for (const auto* file : index.files(ProjectFileKind::RustSource)) {
            if (is_ignored_dependency_path(file->path)) {
                continue;
            }
            const auto source = (scan.root / file->path).lexically_normal();
            if (!path_has_prefix(source, project_dir)) {
                continue;
            }
            record_build_system_command(result,
                                        source,
                                        project_dir,
                                        {"cargo",
                                         "check",
                                         "--manifest-path",
                                         manifest.string(),
                                         "--message-format=json"},
                                        "cargo");
        }
    }
}

bool package_uses_vue_or_typescript(const std::string& package_text) {
    return package_text.find("vue-tsc") != std::string::npos ||
           package_text.find("\"vite\"") != std::string::npos ||
           package_text.find("\"typescript\"") != std::string::npos ||
           package_text.find("\"vue\"") != std::string::npos;
}

void emulate_npm_projects(BuildEmulationResult& result,
                          const ProjectScanResult& scan,
                          const ProjectIndex& index) {
    for (const auto& package_json : project_build_files(index, "package.json")) {
        const auto project_dir = package_json.parent_path();
        const auto package_text = read_text_file(package_json);
        if (!package_uses_vue_or_typescript(package_text)) {
            continue;
        }

        std::vector<std::string> arguments{"npm", "run", "build"};
        std::vector<const ProjectFile*> frontend_sources = index.files(ProjectFileKind::TypeScriptSource);
        const auto& vue_sources = index.files(ProjectFileKind::VueSource);
        frontend_sources.insert(frontend_sources.end(), vue_sources.begin(), vue_sources.end());
        for (const auto* file : frontend_sources) {
            if (is_ignored_dependency_path(file->path)) {
                continue;
            }
            const auto source = (scan.root / file->path).lexically_normal();
            if (!path_has_prefix(source, project_dir)) {
                continue;
            }
            record_build_system_command(result, source, project_dir, arguments, "npm");
        }
    }
}


std::vector<std::filesystem::path> python_roots(const ProjectScanResult& scan,
                                                const ProjectIndex& index) {
    std::set<std::string> roots;
    for (const auto* file : index.files(ProjectFileKind::BuildFile)) {
        const auto name = file->path.filename().string();
        if (name == "setup.py" || name == "pyproject.toml" || name == "setup.cfg") {
            roots.insert((scan.root / file->path).parent_path().lexically_normal().string());
        }
    }

    std::vector<std::filesystem::path> result;
    result.reserve(roots.size());
    for (const auto& root : roots) {
        result.emplace_back(root);
    }
    return result;
}

void emulate_python_projects(BuildEmulationResult& result,
                             const ProjectScanResult& scan,
                             const ProjectIndex& index) {
    for (const auto& root : python_roots(scan, index)) {
        std::vector<std::string> arguments;
        if (std::filesystem::is_regular_file(root / "pyproject.toml")) {
            arguments = {"python3", "-m", "build", root.string()};
        } else if (std::filesystem::is_regular_file(root / "setup.py")) {
            arguments = {"python3", (root / "setup.py").string(), "build"};
        } else {
            arguments = {"python3", "-m", "compileall", root.string()};
        }

        for (const auto* file : index.files(ProjectFileKind::PythonSource)) {
            if (is_ignored_dependency_path(file->path)) {
                continue;
            }
            const auto source = (scan.root / file->path).lexically_normal();
            if (!path_has_prefix(source, root)) {
                continue;
            }
            record_build_system_command(result, source, root, arguments, "python");
        }
    }
}

}  // namespace qt::index::build_emulator_internal
