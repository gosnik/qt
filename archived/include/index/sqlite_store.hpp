#pragma once

#include "index/code_index.hpp"
#include "index/project_scan.hpp"
#include "index/tree_sitter_metadata.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

#include <nlohmann/json_fwd.hpp>

namespace qt::index {

struct SqliteIndexSnapshot {
    ProjectScanResult scan;
    CodeIndex code_index;
    std::size_t generation = 0;
    std::size_t compile_commands = 0;
    std::string build_metadata_summary;
    std::chrono::system_clock::time_point last_scan_finished;
    TreeSitterResult tree_sitter;
    bool fts_available = false;
    std::unordered_map<std::string, std::string> scan_fingerprints;
    bool scan_fingerprints_complete = false;
};

std::filesystem::path sqlite_index_database_path(const std::filesystem::path& state_directory);

std::optional<SqliteIndexSnapshot> load_sqlite_index_snapshot(
    const std::filesystem::path& database_path,
    const std::filesystem::path& project_root,
    std::string* error);

void save_sqlite_index_snapshot(const std::filesystem::path& database_path,
                                const std::filesystem::path& project_root,
                                const ProjectScanResult& scan,
                                const CodeIndex& code_index,
                                std::size_t generation,
                                std::size_t compile_commands,
                                const std::string& build_metadata_summary,
                                const TreeSitterResult& tree_sitter,
                                const std::unordered_map<std::string, std::string>&
                                    scan_fingerprints = {});

nlohmann::json sqlite_fts_search_payload(const std::filesystem::path& database_path,
                                          const std::filesystem::path& project_root,
                                          const std::string& query,
                                          std::size_t limit);

nlohmann::json sqlite_fts_search_payload_pinned(const std::filesystem::path& database_path,
                                                 const std::filesystem::path& project_root,
                                                 const std::string& query,
                                                 std::size_t limit,
                                                 std::size_t generation);

}  // namespace qt::index
