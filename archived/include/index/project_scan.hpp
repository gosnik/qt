#pragma once

#include <filesystem>
#include <iosfwd>
#include <map>
#include <string>
#include <vector>

namespace qt::index {

enum class ProjectFileKind {
    CSource,
    CxxSource,
    RustSource,
    TypeScriptSource,
    VueSource,
    PythonSource,
    Header,
    BuildFile,
    Config,
    Other,
};

struct ProjectFile {
    std::filesystem::path path;
    ProjectFileKind kind = ProjectFileKind::Other;
};

struct ProjectScanResult {
    std::filesystem::path root;
    std::vector<ProjectFile> files;
    std::vector<std::filesystem::path> existing_compilation_databases;
    std::map<std::string, std::size_t> build_files_by_type;
    std::size_t skipped_directories = 0;
};

ProjectScanResult scan_project_tree(const std::filesystem::path& root);
ProjectScanResult scan_project_tree(const std::filesystem::path& root, bool low_priority);
void write_scan_summary(const ProjectScanResult& scan, std::ostream& out);

}  // namespace qt::index
