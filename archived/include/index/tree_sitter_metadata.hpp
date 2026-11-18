#pragma once

#include "index/project_scan.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace qt::index {

struct TreeSitterOptions {
    bool enabled = true;
    std::filesystem::path tree_sitter_executable;
    std::filesystem::path cpp_grammar_directory;
    std::filesystem::path output_directory;
};

struct TreeSitterResult {
    bool attempted = false;
    bool available = false;
    bool generated = false;
    std::size_t files_considered = 0;
    std::size_t files_parsed = 0;
    std::size_t files_updated = 0;
    std::size_t files_unchanged = 0;
    std::size_t files_added = 0;
    std::size_t files_changed = 0;
    std::size_t files_deleted = 0;
    std::size_t files_failed = 0;
    std::size_t files_skipped = 0;
    std::filesystem::path output_directory;
    std::filesystem::path manifest_path;
    std::vector<std::string> failed_files;
    std::vector<std::string> skipped_files;
    std::string mode;
    std::string message;
};

TreeSitterResult generate_tree_sitter_metadata(const ProjectScanResult& scan,
                                               const TreeSitterOptions& options);
TreeSitterResult generate_tree_sitter_metadata(const ProjectScanResult& scan,
                                               const TreeSitterOptions& options,
                                               bool low_priority);

}  // namespace qt::index
