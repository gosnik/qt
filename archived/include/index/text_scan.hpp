#pragma once

#include <optional>
#include <string_view>

namespace qt::index {

std::optional<int> scan_positive_int(std::string_view text);

}  // namespace qt::index
