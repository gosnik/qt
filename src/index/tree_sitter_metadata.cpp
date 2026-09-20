#include "index/tree_sitter_metadata.hpp"

#include "index/cancellation.hpp"

#include <nlohmann/json.hpp>
#include <tree_sitter/api.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

extern "C" const TSLanguage* tree_sitter_cpp(void);

namespace qt::index {
namespace {

using json = nlohmann::json;

bool is_cpp_parse_candidate(ProjectFileKind kind) {
    return kind == ProjectFileKind::CSource || kind == ProjectFileKind::CxxSource ||
           kind == ProjectFileKind::Header;
}

std::string lowercase_ascii(std::string value) {
    for (auto& ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

std::optional<std::string> dependency_or_generated_reason(const std::filesystem::path& relative_path) {
    std::vector<std::string> parts;
    for (const auto& part : relative_path) {
        const auto value = part.generic_string();
        if (!value.empty() && value != ".") {
            parts.push_back(value);
        }
    }
    if (parts.empty()) {
        return std::nullopt;
    }

    const auto first = lowercase_ascii(parts[0]);
    if (first == ".pio" || first == ".index" || first == "node_modules" ||
        first == "build" || first == "cmake-build-debug" || first == "cmake-build-release") {
        return "build, cache, or dependency directory";
    }
    if (first == "vendor" || first == "third_party" || first == "external") {
        return "vendored dependency directory";
    }
    if (first == "lib" && parts.size() > 1) {
        const auto second = lowercase_ascii(parts[1]);
        if (second.rfind("boost_", 0) == 0 || second == "tree-sitter" ||
            second == "tree-sitter-cpp") {
            return "vendored dependency directory";
        }
    }

    const auto lower_filename = lowercase_ascii(relative_path.filename().generic_string());
    if (lower_filename.rfind("moc_", 0) == 0 || lower_filename.rfind("qrc_", 0) == 0 ||
        lower_filename.rfind("ui_", 0) == 0 ||
        (lower_filename.size() > 6 &&
         lower_filename.substr(lower_filename.size() - 6) == ".pb.cc")) {
        return "generated source file";
    }
    return std::nullopt;
}

std::filesystem::path tree_output_path(const std::filesystem::path& output_directory,
                                       const std::filesystem::path& relative_path) {
    std::filesystem::path relative = relative_path;
    relative += ".tree";
    return output_directory / "trees" / relative;
}

std::int64_t file_time_ticks(const std::filesystem::path& path) {
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return 0;
    }
    return time.time_since_epoch().count();
}

std::uint64_t file_size_or_zero(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : size;
}

std::string checksum_text(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto ch : text) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

bool read_source_text(const std::filesystem::path& source, std::string& source_text) {
    std::ifstream input(source, std::ios::binary);
    if (!input) {
        return false;
    }
    source_text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return true;
}

struct ParseResult {
    bool parsed = false;
    std::string checksum;
};

ParseResult run_tree_sitter_parse(TSParser* parser,
                                  const std::string& source_text,
                                  const std::filesystem::path& output) {
    ParseResult result;
    result.checksum = checksum_text(source_text);
    if (source_text.size() > static_cast<std::size_t>(UINT32_MAX)) {
        return result;
    }

    TSTree* tree = ts_parser_parse_string(parser,
                                          nullptr,
                                          source_text.data(),
                                          static_cast<uint32_t>(source_text.size()));
    if (tree == nullptr) {
        return result;
    }

    const TSNode root = ts_tree_root_node(tree);
    char* tree_text = ts_node_string(root);
    result.parsed = tree_text != nullptr && !ts_node_has_error(root);
    if (tree_text != nullptr) {
        std::filesystem::create_directories(output.parent_path());
        std::ofstream out(output);
        if (out) {
            out << tree_text << '\n';
        }
        std::free(tree_text);
    }
    ts_tree_delete(tree);
    return result;
}

struct ManifestEntry {
    std::string source;
    std::string tree;
    bool parsed = false;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    std::string checksum;
};

std::map<std::string, ManifestEntry> read_manifest_entries(const std::filesystem::path& manifest_path) {
    std::map<std::string, ManifestEntry> entries;
    std::ifstream input(manifest_path);
    if (!input) {
        return entries;
    }

    json manifest = json::parse(input, nullptr, false);
    if (!manifest.is_object() || !manifest.value("files", json::array()).is_array()) {
        return entries;
    }

    for (const auto& item : manifest["files"]) {
        if (!item.is_object()) {
            continue;
        }
        ManifestEntry entry;
        entry.source = item.value("source", "");
        if (entry.source.empty()) {
            continue;
        }
        entry.tree = item.value("tree", "");
        entry.parsed = item.value("parsed", false);
        entry.size = item.value("size", static_cast<std::uint64_t>(0));
        entry.mtime = item.value("mtime", static_cast<std::int64_t>(0));
        entry.checksum = item.value("checksum", "");
        entries[entry.source] = std::move(entry);
    }
    return entries;
}

bool tree_artifact_exists(const std::filesystem::path& output_directory, const ManifestEntry& entry) {
    if (entry.tree.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(output_directory / entry.tree, ec);
}

void low_priority_sleep(bool low_priority, std::chrono::milliseconds duration) {
    scan_cancellation_checkpoint();
    if (!low_priority) {
        return;
    }
    std::this_thread::yield();
    std::this_thread::sleep_for(duration);
}

void low_priority_metadata_checkpoint(bool low_priority, std::size_t files_considered) {
    scan_cancellation_checkpoint();
    if (!low_priority) {
        return;
    }
    std::this_thread::yield();
    if (files_considered % 32U == 0U) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void low_priority_parse_checkpoint(bool low_priority) {
    low_priority_sleep(low_priority, std::chrono::milliseconds(25));
}

}  // namespace

TreeSitterResult generate_tree_sitter_metadata(const ProjectScanResult& scan,
                                               const TreeSitterOptions& options) {
    return generate_tree_sitter_metadata(scan, options, false);
}

TreeSitterResult generate_tree_sitter_metadata(const ProjectScanResult& scan,
                                               const TreeSitterOptions& options,
                                               bool low_priority) {
    TreeSitterResult result;
    result.attempted = options.enabled;
    result.output_directory = options.output_directory.empty()
                                  ? scan.root / ".index" / "tree-sitter"
                                  : options.output_directory;
    result.manifest_path = result.output_directory / "metadata.json";
    result.mode = low_priority ? "internal-low-priority" : "internal";

    if (!options.enabled) {
        result.message = "tree-sitter disabled";
        return result;
    }

    TSParser* parser = ts_parser_new();
    if (parser == nullptr) {
        result.message = "internal tree-sitter parser allocation failed";
        return result;
    }

    const TSLanguage* language = tree_sitter_cpp();
    if (language == nullptr || !ts_parser_set_language(parser, language)) {
        const auto abi = language == nullptr ? 0 : ts_language_abi_version(language);
        ts_parser_delete(parser);
        result.message =
            "internal tree-sitter-cpp grammar ABI is incompatible: " + std::to_string(abi);
        return result;
    }

    result.available = true;
    std::error_code ec;
    std::filesystem::create_directories(result.output_directory / "trees", ec);
    if (ec) {
        result.message = "could not create tree-sitter output directory: " + ec.message();
        return result;
    }

    json manifest{{"root", scan.root.string()},
                  {"mode", result.mode},
                  {"tree_sitter_language", "cpp"},
                  {"tree_sitter_abi", ts_language_abi_version(language)},
                  {"files", json::array()}};
    const auto existing_entries = read_manifest_entries(result.manifest_path);
    std::map<std::string, bool> current_sources;

    for (const auto& file : scan.files) {
        if (!is_cpp_parse_candidate(file.kind)) {
            continue;
        }
        ++result.files_considered;
        const auto relative_source = file.path.generic_string();
        current_sources[relative_source] = true;
        const auto source = (scan.root / file.path).lexically_normal();
        const auto output = tree_output_path(result.output_directory, file.path);
        const auto relative_tree =
            std::filesystem::relative(output, result.output_directory, ec).generic_string();
        const auto size = file_size_or_zero(source);
        const auto mtime = file_time_ticks(source);
        if (const auto reason = dependency_or_generated_reason(file.path)) {
            ++result.files_skipped;
            result.skipped_files.push_back(relative_source);
            std::error_code remove_ec;
            std::filesystem::remove(output, remove_ec);
            manifest["files"].push_back({{"source", relative_source},
                                         {"tree", relative_tree},
                                         {"parsed", false},
                                         {"size", size},
                                         {"mtime", mtime},
                                         {"checksum", ""},
                                         {"skipped", true},
                                         {"reason", *reason}});
            low_priority_metadata_checkpoint(low_priority, result.files_considered);
            continue;
        }
        const auto previous = existing_entries.find(relative_source);
        bool parsed = false;
        std::string checksum;
        bool reused = false;

        if (previous != existing_entries.end() && previous->second.size == size &&
            previous->second.mtime == mtime && tree_artifact_exists(result.output_directory, previous->second)) {
            parsed = previous->second.parsed;
            checksum = previous->second.checksum;
            reused = true;
        } else {
            std::string source_text;
            if (read_source_text(source, source_text)) {
                checksum = checksum_text(source_text);
                if (previous != existing_entries.end() && !previous->second.checksum.empty() &&
                    previous->second.checksum == checksum &&
                    tree_artifact_exists(result.output_directory, previous->second)) {
                    parsed = previous->second.parsed;
                    reused = true;
                } else {
                    low_priority_parse_checkpoint(low_priority);
                    const auto parse = run_tree_sitter_parse(parser, source_text, output);
                    low_priority_parse_checkpoint(low_priority);
                    parsed = parse.parsed;
                    checksum = parse.checksum;
                    ++result.files_updated;
                    if (previous == existing_entries.end()) {
                        ++result.files_added;
                    } else {
                        ++result.files_changed;
                    }
                }
            } else {
                ++result.files_updated;
                if (previous == existing_entries.end()) {
                    ++result.files_added;
                } else {
                    ++result.files_changed;
                }
            }
        }

        if (parsed) {
            ++result.files_parsed;
        } else {
            ++result.files_failed;
            result.failed_files.push_back(file.path.generic_string());
        }
        if (reused) {
            ++result.files_unchanged;
        }
        manifest["files"].push_back({{"source", relative_source},
                                     {"tree", relative_tree},
                                     {"parsed", parsed},
                                     {"size", size},
                                     {"mtime", mtime},
                                     {"checksum", checksum}});
        low_priority_metadata_checkpoint(low_priority, result.files_considered);
    }

    for (const auto& [source, entry] : existing_entries) {
        if (current_sources.find(source) != current_sources.end()) {
            continue;
        }
        ++result.files_deleted;
        if (!entry.tree.empty()) {
            std::error_code remove_ec;
            std::filesystem::remove(result.output_directory / entry.tree, remove_ec);
        }
        low_priority_metadata_checkpoint(low_priority, result.files_deleted);
    }
    ts_parser_delete(parser);

    std::ofstream out(result.manifest_path);
    if (!out) {
        result.message = "could not write tree-sitter metadata manifest";
        return result;
    }
    out << manifest.dump(2) << '\n';

    result.generated = result.files_parsed > 0;
    result.message = "tree-sitter metadata generated";
    return result;
}

}  // namespace qt::index
