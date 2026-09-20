#include "index/retrieval_snapshot.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <cstdint>
#include <cctype>

namespace qt::index {
namespace {

std::string kind_name(RetrievalCodeNodeKind kind) {
    switch (kind) {
        case RetrievalCodeNodeKind::Project: return "project";
        case RetrievalCodeNodeKind::File: return "file";
        case RetrievalCodeNodeKind::Class: return "class";
        case RetrievalCodeNodeKind::Method: return "method";
        case RetrievalCodeNodeKind::Function: return "function";
    }
    return "unknown";
}

std::string fingerprint_text(const std::string& text) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << "fnv1a64:" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

constexpr std::uintmax_t kMaxChunkSourceBytes = 1024U * 1024U;

std::string read_file(const std::filesystem::path& path, std::uintmax_t max_bytes = 0) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream out;
    if (max_bytes == 0) {
        out << input.rdbuf();
    } else {
        std::string buffer(static_cast<std::size_t>(std::min<std::uintmax_t>(max_bytes, 64U * 1024U)), '\0');
        std::uintmax_t remaining = max_bytes;
        while (remaining != 0 && input) {
            const auto amount = static_cast<std::streamsize>(std::min<std::uintmax_t>(remaining, buffer.size()));
            input.read(buffer.data(), amount);
            const auto got = input.gcount();
            if (got > 0) out.write(buffer.data(), got);
            if (got <= 0) break;
            remaining -= static_cast<std::uintmax_t>(got);
        }
    }
    return out.str();
}

std::string line_window(const std::string& source, std::size_t first, std::size_t last) {
    if (first == 0) first = 1;
    std::istringstream input(source);
    std::ostringstream out;
    std::string line;
    std::size_t number = 0;
    while (std::getline(input, line)) {
        ++number;
        if (number >= first && number <= last) {
            const auto used = static_cast<std::size_t>(out.tellp());
            if (used + line.size() + 1 > 32768) break;
            out << line << '\n';
        }
        if (number > last) break;
    }
    return out.str();
}

}  // namespace

std::string retrieval_source_fingerprint(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) return "missing";
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > kMaxChunkSourceBytes) return "unindexed";
    std::ifstream input(path, std::ios::binary);
    if (!input) return "unreadable";
    std::string contents(static_cast<std::size_t>(size), '\0');
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (static_cast<std::size_t>(input.gcount()) != contents.size() || input.peek() != EOF)
        return "changed";
    return fingerprint_text(contents);
}

std::string retrieval_node_id(const std::filesystem::path& worktree,
                              const std::filesystem::path& path,
                              RetrievalCodeNodeKind kind,
                              const std::string& qualified_name,
                              const std::string& signature_discriminator) {
    const auto key = worktree.generic_string() + "\n" + path.generic_string() + "\n" +
                     kind_name(kind) + "\n" + qualified_name + "\n" + signature_discriminator;
    return "code:" + fingerprint_text(key).substr(8);
}

std::shared_ptr<const RetrievalSnapshot> RetrievalSnapshot::build(
    const ProjectScanResult& scan, const CodeIndex& index, std::size_t generation, bool stale) {
    return build(scan, index, generation, std::unordered_map<std::string, std::string>{}, stale);
}

std::shared_ptr<const RetrievalSnapshot> RetrievalSnapshot::build(
    const ProjectScanResult& scan, const CodeIndex& index, std::size_t generation,
    const std::unordered_map<std::string, std::string>& scan_fingerprints, bool stale) {
    auto result = std::shared_ptr<RetrievalSnapshot>(new RetrievalSnapshot());
    result->root_ = scan.root;
    result->generation_ = generation;
    result->stale_ = stale;
    const auto worktree = scan.root.generic_string();
    RetrievalCodeNode project;
    project.id = retrieval_node_id(scan.root, {}, RetrievalCodeNodeKind::Project, worktree, "project");
    project.repository = worktree; project.worktree = worktree;
    project.kind = RetrievalCodeNodeKind::Project; project.name = scan.root.filename().string();
    project.qualified_name = worktree; project.generation = generation;
    result->nodes_.push_back(project);
    std::unordered_map<std::string, std::string> source;
    std::unordered_map<std::string, std::string> file_ids;
    for (const auto& file : scan.files) {
        const auto key = file.path.generic_string();
        const auto full_path = scan.root / file.path;
        const bool source_kind = file.kind != ProjectFileKind::Other ||
                                 file.path.extension() == ".md" || file.path.extension() == ".txt";
        std::error_code size_error;
        const auto size = std::filesystem::file_size(full_path, size_error);
        const bool bounded_source = source_kind && !size_error && size <= kMaxChunkSourceBytes;
        if (bounded_source) source[key] = read_file(full_path, kMaxChunkSourceBytes);
        const auto recorded = scan_fingerprints.find(key);
        const auto fp = recorded == scan_fingerprints.end() ? fingerprint_text(source[key]) : recorded->second;
        RetrievalCodeNode node;
        node.repository = worktree;
        node.worktree = worktree;
        node.path = file.path;
        node.kind = RetrievalCodeNodeKind::File;
        node.name = file.path.filename().string();
        node.qualified_name = key;
        node.signature_discriminator = "file";
        node.source_fingerprint = fp;
        node.generation = generation;
        node.id = retrieval_node_id(scan.root, file.path, node.kind, key, node.signature_discriminator);
        file_ids[key] = node.id;
        result->nodes_.push_back(node);
        RetrievalCodeEdge contains;
        contains.id = "contains:" + project.id + ":" + node.id;
        contains.caller_id = project.id; contains.callee_ids = {node.id};
        contains.path = file.path; contains.relation = "contains_file"; contains.heuristic = false;
        result->edges_.push_back(std::move(contains));
        const bool source_unchanged = recorded == scan_fingerprints.end() || fp == fingerprint_text(source[key]);
        if (bounded_source && !source[key].empty() && source_unchanged) {
            RetrievalCodeChunk chunk;
            chunk.owner_id = node.id;
            chunk.id = node.id + ":chunk:0";
            chunk.path = file.path;
            chunk.first_line = 1;
            chunk.last_line = 48;
            chunk.text = line_window(source[key], 1, 48);
            chunk.source_fingerprint = fp;
            chunk.generation = generation;
            result->chunks_.push_back(std::move(chunk));
        }
    }
    const auto add_symbol = [&](const std::filesystem::path& path, RetrievalCodeNodeKind kind,
                                const std::string& name, const std::string& qualified,
                                std::size_t line) {
        const auto key = path.generic_string();
        const auto recorded = scan_fingerprints.find(key);
        const auto fp = recorded == scan_fingerprints.end() ? fingerprint_text(source[key]) : recorded->second;
        const auto current_fp = fingerprint_text(source[key]);
        if (recorded != scan_fingerprints.end() && fp != current_fp) return std::string{};
        if (source[key].empty()) return std::string{};
        std::string signature;
        std::istringstream lines(source[key]);
        std::string source_line;
        for (std::size_t n = 0; n < line && std::getline(lines, source_line); ++n) {}
        for (std::size_t n = 0; n < 16 && !source_line.empty(); ++n) {
            const auto end = source_line.find_first_of("{;");
            signature += source_line.substr(0, end) + ' ';
            if (end != std::string::npos || signature.size() > 4096 || !std::getline(lines, source_line)) break;
        }
        if (signature.empty()) return std::string{};
        RetrievalCodeNode node;
        node.repository = worktree;
        node.worktree = worktree;
        node.path = path; node.kind = kind; node.name = name; node.qualified_name = qualified;
        node.signature_discriminator = signature; node.source_fingerprint = fp;
        node.generation = generation;
        node.id = retrieval_node_id(scan.root, path, kind, qualified, signature);
        const auto duplicates = std::count_if(result->nodes_.begin(), result->nodes_.end(), [&](const auto& other) {
            return other.path == path && other.kind == kind && other.qualified_name == qualified &&
                   other.signature_discriminator == signature;
        });
        if (duplicates != 0) node.id += ":ambiguous:" + std::to_string(duplicates);
        result->nodes_.push_back(node);
        RetrievalCodeChunk chunk;
        chunk.owner_id = node.id; chunk.id = node.id + ":chunk:0"; chunk.path = path;
        chunk.first_line = line > 16 ? line - 16 : 1; chunk.last_line = line + 32;
        chunk.text = line_window(source[key], chunk.first_line, chunk.last_line);
        chunk.source_fingerprint = fp; chunk.generation = generation;
        result->chunks_.push_back(std::move(chunk));
        return node.id;
    };
    std::unordered_map<std::string, std::vector<std::string>> symbol_ids;
    for (const auto& klass : index.classes) {
        const auto id = add_symbol(klass.file, RetrievalCodeNodeKind::Class,
                                   klass.name, klass.qualified_name, klass.line);
        if (!id.empty()) symbol_ids[klass.qualified_name].push_back(id);
    }
    for (const auto& function : index.functions) {
        const auto kind = function.kind == "method" ? RetrievalCodeNodeKind::Method
                                                      : RetrievalCodeNodeKind::Function;
        const auto id = add_symbol(function.file, kind, function.name,
                                   function.qualified_name, function.line);
        if (!id.empty()) symbol_ids[function.qualified_name].push_back(id);
    }
    for (std::size_t i = 0; i < index.call_graph.size(); ++i) {
        const auto& edge = index.call_graph[i];
        RetrievalCodeEdge projected;
        projected.id = "edge:" + std::to_string(i) + ":" + edge.file.generic_string() + ":" +
                       std::to_string(edge.line);
        projected.path = edge.file; projected.line = edge.line; projected.unresolved_name = edge.callee;
        projected.relation = "calls";
        const auto callers = symbol_ids[edge.caller];
        if (callers.size() == 1) projected.caller_id = callers.front();
        const auto callees = symbol_ids[edge.callee];
        projected.callee_ids = callees;
        projected.ambiguous = callees.size() > 1 || callers.size() != 1;
        result->edges_.push_back(std::move(projected));
    }
    for (const auto& node : result->nodes_) {
        if (node.kind == RetrievalCodeNodeKind::File || node.kind == RetrievalCodeNodeKind::Project) continue;
        const auto file = file_ids.find(node.path.generic_string());
        if (file == file_ids.end()) continue;
        RetrievalCodeEdge edge;
        edge.id = "defines:" + file->second + ":" + node.id;
        edge.caller_id = file->second;
        edge.callee_ids = {node.id};
        edge.path = node.path;
        edge.relation = "defines";
        result->edges_.push_back(std::move(edge));
    }
    return std::shared_ptr<const RetrievalSnapshot>(std::move(result));
}

std::vector<RetrievalCodeNode> RetrievalSnapshot::find_symbols(
    const std::string& query) const {
    return find_symbols(query, {});
}

std::vector<RetrievalCodeNode> RetrievalSnapshot::find_symbols(
    const std::string& query, const std::filesystem::path& path) const {
    std::vector<RetrievalCodeNode> result;
    for (const auto& node : nodes_) {
        if (!path.empty() && node.path != path) continue;
        if (node.name == query || node.qualified_name == query) result.push_back(node);
    }
    return result;
}

std::shared_ptr<const RetrievalCodeSnippet> RetrievalSnapshot::read_snippet(
    const std::string& node_id) const {
    if (stale_) return {};
    const auto node = std::find_if(nodes_.begin(), nodes_.end(), [&](const auto& value) {
        return value.id == node_id;
    });
    if (node == nodes_.end() || !std::filesystem::is_regular_file(root_ / node->path) ||
        retrieval_source_fingerprint(root_ / node->path) != node->source_fingerprint) {
        return {};
    }
    const auto chunk = std::find_if(chunks_.begin(), chunks_.end(), [&](const auto& value) {
        return value.owner_id == node_id;
    });
    if (chunk == chunks_.end()) return {};
    auto result = std::make_shared<RetrievalCodeSnippet>();
    result->chunk = *chunk;
    result->current = true;
    return result;
}

}  // namespace qt::index
