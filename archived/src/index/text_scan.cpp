#include "index/text_scan.hpp"

#include <scn/scan.h>

#include <cctype>
#include <iterator>

namespace qt::index {
namespace {

template <typename Range>
bool has_only_trailing_space(const Range& text) {
    for (auto it = std::begin(text); it != std::end(text); ++it) {
        const auto ch = *it;
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::optional<int> scan_positive_int(std::string_view text) {
    const auto result = scn::scan<int>(text, "{}");
    if (!result) {
        return std::nullopt;
    }

    const int value = result->value();
    if (value < 1 || !has_only_trailing_space(result->range())) {
        return std::nullopt;
    }

    return value;
}

}  // namespace qt::index
