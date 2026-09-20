#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace qt::index {

enum class FileSearchBackend {
    PortableFilesystem,
    FfsIndex,
    ReclsCompatible,
};

struct FileSearchOptions {
    std::filesystem::path root = ".";
    std::string name_fragment;
    FileSearchBackend backend = FileSearchBackend::FfsIndex;
};

FileSearchBackend parse_file_search_backend(const std::string& name);
const char* file_search_backend_name(FileSearchBackend backend);
std::vector<std::filesystem::path> search_files(const FileSearchOptions& options);
std::string file_search_integration_summary();

}  // namespace qt::index
