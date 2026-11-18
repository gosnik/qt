#include "qt/tui/layout/spacer.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace qt::tui {

Spacer::Spacer(std::size_t lines)
    : lines_(lines) {}

void Spacer::set_lines(std::size_t lines) noexcept { lines_ = lines; }

std::size_t Spacer::lines() const noexcept { return lines_; }

foundation::Result<RenderBuffer> Spacer::render(const RenderConstraints& constraints) const {
    const std::size_t count = std::min(lines_, constraints.max_height);
    return make_buffer(std::vector<std::string>(count), constraints);
}

}  // namespace qt::tui
