#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qt::foundation {

struct FrontmatterParseResult {
    /// Key-value pairs from frontmatter
    std::map<std::string, std::string> fields;
    /// Diagnostic messages from parsing
    std::vector<std::string> diagnostics;
    /// Whether frontmatter opening delimiter was found
    bool had_frontmatter = false;
    /// Whether closing delimiter was found
    bool complete = false;
};

/// Parse YAML-like frontmatter delimited by --- lines.
/// Handles comments (lines starting with #), key: value pairs,
/// quoted values, and missing closing delimiter diagnostics.
[[nodiscard]] FrontmatterParseResult parse_frontmatter(std::string_view content);

/// Oldest extant format: code fence with optional language hint.
/// ```skill ... ``` or ```SKILL ... ``` blocks before the skill body.
[[nodiscard]] FrontmatterParseResult parse_code_fence_frontmatter(std::string_view content);

}  // namespace qt::foundation
