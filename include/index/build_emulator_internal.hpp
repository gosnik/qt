#pragma once

#include "index/build_emulator.hpp"
#include "index/project_index.hpp"

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace qt::index::build_emulator_internal {

bool is_source_path(const std::filesystem::path& path);
bool is_native_source_kind(ProjectFileKind kind);
bool is_c_source_path(const std::filesystem::path& path);

std::optional<std::string> parse_quoted_string_at(const std::string& text, std::size_t& pos);
std::optional<std::pair<std::string, std::string>> parse_assignment(std::string line);
std::string strip_ini_comment(std::string line);

void add_define(std::set<std::string>& defines, std::string value);
void add_include(std::set<std::string>& includes,
                 const std::filesystem::path& base,
                 std::string value);
void scan_tokens_for_metadata(const std::vector<std::string>& tokens,
                              const std::filesystem::path& base,
                              std::set<std::string>& includes,
                              std::set<std::string>& defines);
void append_compile_tokens(std::vector<std::string>& arguments,
                           const std::vector<std::string>& tokens,
                           const std::filesystem::path& base);
void add_existing_include(std::vector<std::filesystem::path>& includes,
                          std::set<std::string>& seen,
                          const std::filesystem::path& path);

std::vector<std::filesystem::path> project_build_files(const ProjectIndex& index,
                                                       const std::string& filename);
std::vector<std::filesystem::path> qmake_project_files(const ProjectIndex& index);
std::vector<std::filesystem::path> build_roots(const ProjectScanResult& scan);

void record_build_system_command(BuildEmulationResult& result,
                                 const std::filesystem::path& source,
                                 const std::filesystem::path& directory,
                                 std::vector<std::string> arguments,
                                 const std::string& backend);

void emulate_ninja(BuildEmulationResult& result,
                   std::set<std::string>& includes,
                   std::set<std::string>& defines,
                   const std::filesystem::path& build_ninja);
void emulate_cmake_make(BuildEmulationResult& result,
                        std::set<std::string>& includes,
                        std::set<std::string>& defines,
                        const std::filesystem::path& build_root);
void emulate_make_projects(BuildEmulationResult& result,
                           std::set<std::string>& includes,
                           std::set<std::string>& defines,
                           const ProjectScanResult& scan);
void emulate_qmake_projects(BuildEmulationResult& result,
                            std::set<std::string>& includes,
                            std::set<std::string>& defines,
                            const ProjectIndex& index);
void emulate_cargo_projects(BuildEmulationResult& result,
                            const ProjectScanResult& scan,
                            const ProjectIndex& index);
void emulate_npm_projects(BuildEmulationResult& result,
                          const ProjectScanResult& scan,
                          const ProjectIndex& index);
void emulate_platformio_projects(BuildEmulationResult& result,
                                 std::set<std::string>& includes,
                                 std::set<std::string>& defines,
                                 const ProjectScanResult& scan,
                                 const ProjectIndex& index);
void emulate_python_projects(BuildEmulationResult& result,
                             const ProjectScanResult& scan,
                             const ProjectIndex& index);
void emulate_waf_projects(BuildEmulationResult& result,
                          std::set<std::string>& includes,
                          const ProjectScanResult& scan);

}  // namespace qt::index::build_emulator_internal
