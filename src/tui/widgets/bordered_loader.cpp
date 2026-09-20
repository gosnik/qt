#include "qt/tui/widgets/bordered_loader.hpp"
#include "qt/tui/layout/box.hpp"
#include "qt/tui/layout/dynamic_border.hpp"
#include "qt/tui/layout/spacer.hpp"
#include "qt/tui/render/text.hpp"

#include <memory>
#include <string>
#include <utility>

namespace qt::tui {

BorderedLoader::BorderedLoader(std::string message, bool cancellable)
    : cancellable_(cancellable) {
    box_ = std::make_shared<Box>(1, 0);
    box_->add_child(std::make_shared<DynamicBorder>());

    if (cancellable_) {
        cancellable_loader_ = std::make_shared<CancellableLoader>(std::move(message));
        box_->add_child(cancellable_loader_);
        box_->add_child(std::make_shared<Spacer>(1));
        box_->add_child(std::make_shared<Text>("esc to cancel", 1, 0));
    } else {
        plain_loader_ = std::make_shared<Loader>(std::move(message));
        box_->add_child(plain_loader_);
    }

    box_->add_child(std::make_shared<Spacer>(1));
    box_->add_child(std::make_shared<DynamicBorder>());
}

foundation::Result<RenderBuffer> BorderedLoader::render(
    const RenderConstraints& constraints) const {
    return box_->render(constraints);
}

void BorderedLoader::handle_input(std::string_view bytes) {
    if (cancellable_ && cancellable_loader_) {
        cancellable_loader_->handle_input(bytes);
    }
}

bool BorderedLoader::aborted() const noexcept {
    if (cancellable_ && cancellable_loader_) {
        return cancellable_loader_->aborted();
    }
    return false;
}

void BorderedLoader::set_abort_callback(AbortCallback callback) {
    if (cancellable_ && cancellable_loader_) {
        cancellable_loader_->set_abort_callback(std::move(callback));
    }
}

void BorderedLoader::cancel() noexcept {
    if (cancellable_ && cancellable_loader_) {
        cancellable_loader_->cancel();
    }
}

Loader& BorderedLoader::loader() noexcept {
    if (cancellable_ && cancellable_loader_) {
        return cancellable_loader_->loader();
    }
    return *plain_loader_;
}

const Loader& BorderedLoader::loader() const noexcept {
    if (cancellable_ && cancellable_loader_) {
        return cancellable_loader_->loader();
    }
    return *plain_loader_;
}

}  // namespace qt::tui
