#pragma once

#include "index/project_scan.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace qt::index {

class ProjectIndex {
public:
    explicit ProjectIndex(const ProjectScanResult& scan);

    const std::vector<const ProjectFile*>& files(ProjectFileKind kind) const;
    std::vector<std::filesystem::path> build_files_named(const std::string& filename) const;
    std::vector<std::filesystem::path> build_files_with_extension(const std::string& extension) const;

private:
    std::filesystem::path root_;
    std::map<ProjectFileKind, std::vector<const ProjectFile*>> files_by_kind_;
    std::map<std::string, std::vector<std::filesystem::path>> build_files_by_name_;
    std::map<std::string, std::vector<std::filesystem::path>> build_files_by_extension_;
    std::vector<const ProjectFile*> empty_;
};

}  // namespace qt::index
