#pragma once

#include "qt/memory/models.hpp"
#include "qt/memory/retrieval_models.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

#include <nlohmann/json_fwd.hpp>

namespace qt::memory {

// Serializes one governed recall result.  current_revision is supplied by the
// caller after its visibility check; historical results retain their captured
// revision and never substitute this value.
nlohmann::json recalled_memory_json(
    const RecalledMemory&, std::optional<std::int64_t> current_revision = std::nullopt,
    const ContextPack* preview = nullptr);

// Serializes a typed memory/code/entity retrieval candidate, including chunks,
// ranks, graph direction, assertion metadata and exclusion reasons.
nlohmann::json retrieval_candidate_json(const RetrievalCandidate&,
                                        const ContextPack* preview = nullptr);

// Describes a context pack without claiming that a preview was injected or
// consumed.  Token and byte values are estimates of the complete rendered pack.
nlohmann::json context_preview_json(const ContextPack&, std::size_t token_budget,
                                   std::size_t bytes_per_token = 4U);

}  // namespace qt::memory
