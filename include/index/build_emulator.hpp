#pragma once

#include "index/build_metadata.hpp"
#include "index/project_scan.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace qt::index {

struct BuildEmulationResult {
    std::map<std::string, ObservedCompileCommand> compile_commands;
    std::vector<std::filesystem::path> include_paths;
    std::vector<std::string> defines;
    std::map<std::string, std::size_t> edges_by_backend;
};

BuildEmulationResult emulate_build_systems(const ProjectScanResult& scan);
BuildEmulationResult emulate_build_systems(const ProjectScanResult& scan, bool low_priority);
std::string build_emulation_summary(const BuildEmulationResult& emulation);

}  // namespace qt::index
