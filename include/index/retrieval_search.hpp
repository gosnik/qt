#pragma once

#include "index/retrieval_snapshot.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace qt::index {

struct RetrievalSearchHit {
    std::string node_id;
    std::string path;
    RetrievalCodeNodeKind kind = RetrievalCodeNodeKind::File;
    double rank = 0.0;
    std::size_t generation = 0;
    std::string source_fingerprint;
};

struct RetrievalSearchResult {
    std::string query;
    std::size_t generation = 0;
    std::vector<RetrievalSearchHit> hits;
    std::string diagnostic;
    bool stale = false;
};

}  // namespace qt::index
