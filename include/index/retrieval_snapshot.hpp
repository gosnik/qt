#pragma once

#include "index/code_index.hpp"
#include "index/project_scan.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace qt::index {

enum class RetrievalCodeNodeKind { Project, File, Class, Method, Function };

struct RetrievalCodeNode {
    std::string id;
    std::string repository;
    std::string worktree;
    std::filesystem::path path;
    RetrievalCodeNodeKind kind = RetrievalCodeNodeKind::File;
    std::string name;
    std::string qualified_name;
    std::string signature_discriminator;
    std::string source_fingerprint;
    std::size_t generation = 0;
    std::size_t line = 0;
};

struct RetrievalCodeChunk {
    std::string id;
    std::string owner_id;
    std::filesystem::path path;
    std::string text;
    std::size_t first_line = 0;
    std::size_t last_line = 0;
    std::string source_fingerprint;
    std::size_t generation = 0;
    bool bounded_source_window = true;
};

struct RetrievalCodeEdge {
    std::string id;
    std::string caller_id;
    std::vector<std::string> callee_ids;
    std::string unresolved_name;
    std::filesystem::path path;
    std::size_t line = 0;
    bool heuristic = true;
    bool ambiguous = false;
    std::string relation;
};

struct RetrievalCodeSnippet {
    RetrievalCodeChunk chunk;
    bool current = false;
};

// An immutable, read-only view of one native code-index generation.
class RetrievalSnapshot {
public:
    static std::shared_ptr<const RetrievalSnapshot> build(
        const ProjectScanResult& scan, const CodeIndex& index, std::size_t generation,
        bool stale = false);
    static std::shared_ptr<const RetrievalSnapshot> build(
        const ProjectScanResult& scan, const CodeIndex& index, std::size_t generation,
        const std::unordered_map<std::string, std::string>& scan_fingerprints,
        bool stale = false);

    std::size_t generation() const noexcept { return generation_; }
    bool stale() const noexcept { return stale_; }
    const std::filesystem::path& root() const noexcept { return root_; }
    const std::vector<RetrievalCodeNode>& nodes() const noexcept { return nodes_; }
    const std::vector<RetrievalCodeChunk>& chunks() const noexcept { return chunks_; }
    const std::vector<RetrievalCodeEdge>& edges() const noexcept { return edges_; }

    // Returns all candidates. Callers must handle zero and multiple matches.
    std::vector<RetrievalCodeNode> find_symbols(const std::string& query) const;
    std::vector<RetrievalCodeNode> find_symbols(const std::string& query,
                                                const std::filesystem::path& path) const;
    std::shared_ptr<const RetrievalCodeSnippet> read_snippet(const std::string& node_id) const;

private:
    std::filesystem::path root_;
    std::size_t generation_ = 0;
    bool stale_ = false;
    std::vector<RetrievalCodeNode> nodes_;
    std::vector<RetrievalCodeChunk> chunks_;
    std::vector<RetrievalCodeEdge> edges_;
};

std::string retrieval_source_fingerprint(const std::filesystem::path& path);
std::string retrieval_node_id(const std::filesystem::path& worktree,
                              const std::filesystem::path& path,
                              RetrievalCodeNodeKind kind,
                              const std::string& qualified_name,
                              const std::string& signature_discriminator);

}  // namespace qt::index
