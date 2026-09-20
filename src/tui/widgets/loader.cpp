#include "qt/tui/widgets/loader.hpp"

#include "qt/tui/input/keybindings.hpp"

#include <utility>

namespace qt::tui {

namespace {

LoaderIndicator default_indicator() {
    return LoaderIndicator{{"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"}, 80};
}

}  // namespace

Loader::Loader() : indicator_(default_indicator()) {}

Loader::Loader(std::string message)
    : message_(std::move(message)), indicator_(default_indicator()) {}

Loader::Loader(std::string message, LoaderIndicator indicator)
    : message_(std::move(message)), indicator_(std::move(indicator)) {
    if (indicator_.interval_ms == 0) {
        indicator_.interval_ms = 80;
    }
}

foundation::Result<RenderBuffer> Loader::render(
    const RenderConstraints& constraints) const {
    std::string line;
    const auto frame = current_frame();
    if (!frame.empty()) {
        line = frame + " ";
    }
    line += message_;
    return make_buffer({"", std::move(line)}, constraints);
}

void Loader::set_message(std::string message) { message_ = std::move(message); }

void Loader::set_indicator(LoaderIndicator indicator) {
    indicator_ = std::move(indicator);
    if (indicator_.interval_ms == 0) {
        indicator_.interval_ms = 80;
    }
    current_frame_ = 0;
}

void Loader::advance_frame() noexcept {
    if (!indicator_.frames.empty()) {
        current_frame_ = (current_frame_ + 1) % indicator_.frames.size();
    }
}

const std::string& Loader::message() const noexcept { return message_; }

std::string Loader::current_frame() const {
    if (indicator_.frames.empty()) {
        return {};
    }
    return indicator_.frames[current_frame_ % indicator_.frames.size()];
}

std::size_t Loader::interval_ms() const noexcept { return indicator_.interval_ms; }

CancellableLoader::CancellableLoader() = default;

CancellableLoader::CancellableLoader(std::string message)
    : loader_(std::move(message)) {}

foundation::Result<RenderBuffer> CancellableLoader::render(
    const RenderConstraints& constraints) const {
    return loader_.render(constraints);
}

void CancellableLoader::handle_input(std::string_view bytes) {
    const auto& keybindings = get_keybindings_const();
    if (keybindings.matches(bytes, "tui.input.cancel") ||
        keybindings.matches(bytes, "tui.select.cancel")) {
        cancel();
    }
}

void CancellableLoader::cancel() noexcept {
    if (aborted_) {
        return;
    }
    aborted_ = true;
    if (on_abort_) {
        on_abort_();
    }
}

void CancellableLoader::set_abort_callback(AbortCallback callback) {
    on_abort_ = std::move(callback);
}

bool CancellableLoader::aborted() const noexcept { return aborted_; }

Loader& CancellableLoader::loader() noexcept { return loader_; }

const Loader& CancellableLoader::loader() const noexcept { return loader_; }

}  // namespace qt::tui
