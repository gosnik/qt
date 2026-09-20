#pragma once

#include "qt/foundation/error.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace qt::foundation {

Result<std::size_t> utf8_display_width(std::string_view text);
Result<std::string> utf8_truncate(std::string_view text, std::size_t max_columns,
                                  std::string_view ellipsis = "");

/// Returns true if the grapheme is a single regional indicator symbol.
/// Regional indicators appear in pairs to form flag emoji (e.g. "🇨🇳").
/// Each individual indicator is a separate grapheme from uni-algo, but
/// a pair should be treated as one atomic unit with width 2.
bool is_regional_indicator_grapheme(std::string_view grapheme);

}  // namespace qt::foundation
