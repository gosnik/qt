#include "index/project_index.hpp"

#include "index/string_util.hpp"

namespace qt::index {

ProjectIndex::ProjectIndex(const ProjectScanResult& scan) : root_(scan.root) {
    for (const auto& file : scan.files) {
        files_by_kind_[file.kind].push_back(&file);
        if (file.kind != ProjectFileKind::BuildFile) {
            continue;
        }
        const auto absolute = (root_ / file.path).lexically_normal();
        build_files_by_name_[file.path.filename().string()].push_back(absolute);
        build_files_by_extension_[lowercase_ascii(file.path.extension().string())].push_back(absolute);
    }
}

const std::vector<const ProjectFile*>& ProjectIndex::files(ProjectFileKind kind) const {
    const auto found = files_by_kind_.find(kind);
    return found == files_by_kind_.end() ? empty_ : found->second;
}

std::vector<std::filesystem::path> ProjectIndex::build_files_named(
    const std::string& filename) const {
    const auto found = build_files_by_name_.find(filename);
    return found == build_files_by_name_.end() ? std::vector<std::filesystem::path>{}
                                               : found->second;
}

std::vector<std::filesystem::path> ProjectIndex::build_files_with_extension(
    const std::string& extension) const {
    const auto found = build_files_by_extension_.find(lowercase_ascii(extension));
    return found == build_files_by_extension_.end() ? std::vector<std::filesystem::path>{}
                                                    : found->second;
}

}  // namespace qt::index
