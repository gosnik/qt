#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "qt/ai/models.hpp"

namespace qt::ai {

struct PartialJsonOptions {
  std::size_t max_bytes = 64 * 1024;
  std::size_t max_candidates = 256;
};

std::string sanitize_json_unicode_escapes(std::string_view text);
std::string repair_json_string_literals(std::string_view json);
std::optional<Json> parse_json_with_repair(std::string_view json);
std::string trim_incomplete_json_fragment(
    std::string_view json, const PartialJsonOptions& options = {});
Json parse_streaming_json(std::string_view json,
                          const PartialJsonOptions& options = {});

}  // namespace qt::ai
