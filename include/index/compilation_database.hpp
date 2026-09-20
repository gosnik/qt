#pragma once

#include "index/project_scan.hpp"

#include <filesystem>
#include <iosfwd>

namespace qt::index {

struct BuildMetadata;

std::size_t write_compilation_database(const ProjectScanResult& scan,
                                       const BuildMetadata& metadata,
                                       const std::filesystem::path& output_path);
std::size_t write_compilation_database(const ProjectScanResult& scan,
                                       const BuildMetadata& metadata,
                                       const std::filesystem::path& output_path,
                                       bool low_priority);
void write_compilation_database(const ProjectScanResult& scan,
                                const BuildMetadata& metadata,
                                std::ostream& out);
void write_compilation_database(const ProjectScanResult& scan,
                                const BuildMetadata& metadata,
                                std::ostream& out,
                                bool low_priority);

}  // namespace qt::index
