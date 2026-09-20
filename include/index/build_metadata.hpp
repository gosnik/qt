#pragma once

#include "index/project_scan.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace qt::index {

struct ObservedCompileCommand {
    std::filesystem::path directory;
    std::vector<std::string> arguments;
    std::string backend;
    std::string origin;
    unsigned confidence = 0;
};

struct BuildMetadata {
    std::vector<std::filesystem::path> include_paths;
    std::vector<std::string> defines;
    std::map<std::string, ObservedCompileCommand> observed_compile_commands;
    std::map<std::string, std::size_t> emulated_edges_by_backend;
    std::map<std::string, std::size_t> build_files_scanned_by_family;
};

BuildMetadata scan_build_metadata(const ProjectScanResult& scan);
BuildMetadata scan_build_metadata(const ProjectScanResult& scan, bool low_priority);
std::string build_metadata_summary(const BuildMetadata& metadata);

}  // namespace qt::index
