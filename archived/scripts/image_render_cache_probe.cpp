#include "qt/tui/terminal/image.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using qt::tui::CellDimensions;
using qt::tui::Image;
using qt::tui::ImageDimensions;
using qt::tui::ImageProtocol;
using qt::tui::RenderBuffer;
using qt::tui::RenderConstraints;
using qt::tui::TerminalCapabilities;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

const RenderBuffer& rendered(Image& image, const RenderConstraints& constraints,
                             RenderBuffer& storage) {
    auto result = image.render(constraints);
    require(static_cast<bool>(result), "image render failed");
    storage = std::move(result.value());
    return storage;
}

void verify_cache_and_invalidation() {
    Image image("AA==", "image/png", {}, ImageDimensions{180U, 45U});
    const RenderConstraints wide{42U, 20U};
    RenderBuffer first;
    RenderBuffer second;
    RenderBuffer resized_cells;
    RenderBuffer iterm;

    qt::tui::set_terminal_capabilities(
        TerminalCapabilities{ImageProtocol::kitty, true, true});
    qt::tui::set_terminal_cell_dimensions(CellDimensions{9U, 18U});
    const auto& first_render = rendered(image, wide, first);
    const auto kitty_id = image.image_id();
    require(kitty_id.has_value(), "Kitty render did not allocate an image id");
    require(first_render.lines.back().find("\x1b_G") != std::string::npos,
            "Kitty render did not produce a Kitty sequence");

    const auto& cached_render = rendered(image, wide, second);
    require(cached_render.lines == first_render.lines,
            "same constraints and terminal state did not reuse the cached buffer");
    require(image.image_id() == kitty_id, "cached Kitty render changed its image id");

    qt::tui::set_terminal_cell_dimensions(CellDimensions{9U, 9U});
    const auto& cell_render = rendered(image, wide, resized_cells);
    require(cell_render.lines != first_render.lines,
            "cell-size change reused a stale image sequence");
    require(image.image_id() == kitty_id, "cell-size refresh did not reuse the Kitty id");

    qt::tui::set_terminal_capabilities(
        TerminalCapabilities{ImageProtocol::iterm2, true, true});
    const auto& protocol_render = rendered(image, wide, iterm);
    require(protocol_render.lines.back().find("\x1b]1337;File=") != std::string::npos,
            "protocol change reused a stale Kitty sequence");
}

}  // namespace

int main() {
    try {
        verify_cache_and_invalidation();
        std::cout << "image render cache probe: ok\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "image render cache probe: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
