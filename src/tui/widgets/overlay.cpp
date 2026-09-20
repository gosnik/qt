#include "qt/tui/widgets/overlay.hpp"

#include "qt/foundation/utf8.hpp"
#include "qt/tui/input/keys.hpp"
#include "qt/tui/terminal/terminal.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace qt::tui {

namespace {

Focusable* as_focusable(const ComponentPtr& component) {
    return component ? dynamic_cast<Focusable*>(component.get()) : nullptr;
}

InputHandler* as_input_handler(const ComponentPtr& component) {
    return component ? dynamic_cast<InputHandler*>(component.get()) : nullptr;
}

std::size_t non_negative(int value) {
    return value < 0 ? 0U : static_cast<std::size_t>(value);
}

std::size_t resolve_size(const OverlaySize& size, std::size_t reference) {
    if (size.kind == OverlaySize::Kind::percent) {
        return reference * size.value / 100U;
    }
    return size.value;
}

std::size_t clamp_to_range(std::size_t value, std::size_t low, std::size_t high) {
    return std::max(low, std::min(value, high));
}

std::optional<std::size_t> ansi_sequence_length(std::string_view text, std::size_t offset) {
    if (offset >= text.size() || text[offset] != '\x1b' || offset + 1 >= text.size()) {
        return std::nullopt;
    }

    const char introducer = text[offset + 1];
    if (introducer == '[') {
        std::size_t index = offset + 2;
        while (index < text.size()) {
            const unsigned char ch = static_cast<unsigned char>(text[index]);
            if (ch >= 0x40U && ch <= 0x7EU) {
                return index - offset + 1;
            }
            ++index;
        }
        return std::nullopt;
    }

    if (introducer == ']' || introducer == '_' || introducer == 'G') {
        std::size_t index = offset + 2;
        while (index < text.size()) {
            if (text[index] == '\x07') {
                return index - offset + 1;
            }
            if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == '\\') {
                return index - offset + 2;
            }
            ++index;
        }
    }

    return std::nullopt;
}

std::size_t utf8_codepoint_length(unsigned char lead) {
    if (lead < 0x80U) {
        return 1;
    }
    if ((lead & 0xE0U) == 0xC0U) {
        return 2;
    }
    if ((lead & 0xF0U) == 0xE0U) {
        return 3;
    }
    if ((lead & 0xF8U) == 0xF0U) {
        return 4;
    }
    return 1;
}

std::string pad_to_width(std::string text, std::size_t width) {
    auto measured = measure_line_width(text);
    if (!measured) {
        return text;
    }
    while (measured.value() < width) {
        text.push_back(' ');
        ++measured.value();
    }
    return text;
}

std::string take_columns(std::string_view text, std::size_t width) {
    std::string result;
    std::size_t columns = 0;
    std::size_t offset = 0;
    while (offset < text.size()) {
        if (auto sequence_length = ansi_sequence_length(text, offset)) {
            result.append(text.substr(offset, *sequence_length));
            offset += *sequence_length;
            continue;
        }

        const auto length =
            std::min(utf8_codepoint_length(static_cast<unsigned char>(text[offset])),
                     text.size() - offset);
        const auto glyph = text.substr(offset, length);
        const auto glyph_width = foundation::utf8_display_width(glyph);
        if (!glyph_width || columns + glyph_width.value() > width) {
            break;
        }

        result.append(glyph);
        columns += glyph_width.value();
        offset += length;
    }

    return result;
}

std::string drop_columns(std::string_view text, std::size_t columns_to_drop) {
    std::size_t columns = 0;
    std::size_t offset = 0;
    while (offset < text.size()) {
        if (columns >= columns_to_drop) {
            break;
        }
        if (auto sequence_length = ansi_sequence_length(text, offset)) {
            offset += *sequence_length;
            continue;
        }

        const auto length =
            std::min(utf8_codepoint_length(static_cast<unsigned char>(text[offset])),
                     text.size() - offset);
        const auto glyph = text.substr(offset, length);
        const auto glyph_width = foundation::utf8_display_width(glyph);
        if (!glyph_width) {
            offset += length;
            continue;
        }

        columns += glyph_width.value();
        offset += length;
    }

    return std::string(text.substr(offset));
}

bool contains_escape(std::string_view text) {
    return text.find('\x1b') != std::string_view::npos;
}

void normalize_surface_lines(RenderBuffer& buffer, std::size_t width, bool pad_to_height,
                             std::size_t height) {
    if (pad_to_height) {
        while (buffer.lines.size() < height) {
            buffer.lines.push_back(std::string(width, ' '));
        }
    }

    for (auto& line : buffer.lines) {
        if (contains_inline_image_sequence(line)) {
            continue;
        }
        line = pad_to_width(take_columns(line, width), width);
    }
}

void append_line_reset(std::string& line) {
    line.append(terminal_line_reset_sequence.data(), terminal_line_reset_sequence.size());
}

void terminate_styled_lines(RenderBuffer& buffer, std::size_t width) {
    for (auto& line : buffer.lines) {
        if (contains_inline_image_sequence(line)) {
            continue;
        }
        if (!contains_escape(line)) {
            line = pad_to_width(take_columns(line, width), width);
        }
        append_line_reset(line);
    }
}

std::string frame_row(std::string_view left, std::string_view middle,
                      std::string_view right, std::size_t width) {
    if (width == 0) {
        return {};
    }
    if (width == 1) {
        return std::string(left);
    }
    std::string line;
    line.append(left);
    for (std::size_t index = 0; index < width - 2U; ++index) {
        line.append(middle);
    }
    line.append(right);
    return line;
}

std::vector<std::string> framed_overlay_lines(std::vector<std::string> lines,
                                              std::size_t width) {
    if (width < 3U) {
        return lines;
    }

    const std::size_t inner_width = width - 2U;
    std::vector<std::string> framed;
    framed.reserve(lines.size() + 2U);
    framed.push_back(frame_row("┌", "─", "┐", width));
    for (auto& line : lines) {
        auto inner = pad_to_width(take_columns(line, inner_width), inner_width);
        std::string framed_line;
        framed_line.append("│");
        framed_line += std::move(inner);
        framed_line.append("│");
        framed.push_back(std::move(framed_line));
    }
    framed.push_back(frame_row("└", "─", "┘", width));
    return framed;
}

}  // namespace

OverlaySize OverlaySize::columns(std::size_t value) {
    return OverlaySize{Kind::columns, value};
}

OverlaySize OverlaySize::percent(std::size_t value) {
    return OverlaySize{Kind::percent, value};
}

OverlayMargin OverlayMargin::all(int value) {
    return OverlayMargin{value, value, value, value};
}

OverlayHandle::OverlayHandle(OverlaySurface* owner, std::size_t id) : owner_(owner), id_(id) {}

void OverlayHandle::hide() const {
    if (owner_ != nullptr) {
        if (auto* entry = owner_->find_entry(id_); entry != nullptr) {
            const bool had_focus = owner_->focused_component() == entry->component;
            const auto snapshot = *entry;
            owner_->overlays_.erase(std::remove_if(owner_->overlays_.begin(), owner_->overlays_.end(),
                                                   [this](const auto& candidate) {
                                                       return candidate.id == id_;
                                                   }),
                                    owner_->overlays_.end());
            if (had_focus) {
                owner_->restore_focus_after(snapshot);
            }
            owner_->invalidate();
        }
    }
}

void OverlayHandle::set_hidden(bool hidden) const {
    if (owner_ == nullptr) {
        return;
    }
    auto* entry = owner_->find_entry(id_);
    if (entry == nullptr || entry->hidden == hidden) {
        return;
    }
    entry->hidden = hidden;
    if (hidden) {
        if (owner_->focused_component() == entry->component) {
            owner_->restore_focus_after(*entry);
        }
        owner_->invalidate();
        return;
    }
    if (!entry->options.non_capturing && owner_->is_visible_now(*entry)) {
        owner_->bump_focus_order(*entry);
        owner_->set_focus(entry->component);
    }
    owner_->invalidate();
}

bool OverlayHandle::is_hidden() const {
    if (owner_ == nullptr) {
        return true;
    }
    const auto* entry = owner_->find_entry(id_);
    return entry == nullptr || entry->hidden;
}

void OverlayHandle::focus() const {
    if (owner_ == nullptr) {
        return;
    }
    auto* entry = owner_->find_entry(id_);
    if (entry == nullptr || !owner_->is_visible_now(*entry)) {
        return;
    }
    owner_->bump_focus_order(*entry);
    owner_->set_focus(entry->component);
}

void OverlayHandle::unfocus() const {
    if (owner_ == nullptr) {
        return;
    }
    auto* entry = owner_->find_entry(id_);
    if (entry == nullptr || owner_->focused_component() != entry->component) {
        return;
    }
    owner_->restore_focus_after(*entry);
}

bool OverlayHandle::is_focused() const {
    if (owner_ == nullptr) {
        return false;
    }
    const auto* entry = owner_->find_entry(id_);
    return entry != nullptr && owner_->focused_component() == entry->component;
}

OverlaySurface::OverlaySurface(ComponentPtr base) : base_(std::move(base)) {}

void OverlaySurface::set_base(ComponentPtr base) {
    base_ = std::move(base);
    invalidate();
}

void OverlaySurface::set_focus(ComponentPtr component) {
    if (focused_ == component) {
        return;
    }
    if (auto* focusable = as_focusable(focused_)) {
        focusable->set_focused(false);
    }
    focused_ = std::move(component);
    if (auto* focusable = as_focusable(focused_)) {
        focusable->set_focused(true);
    }
    invalidate();
}

ComponentPtr OverlaySurface::focused_component() const {
    return focused_;
}

void OverlaySurface::set_invalidation_callback(InvalidationCallback callback) {
    invalidation_callback_ = std::move(callback);
}

OverlayHandle OverlaySurface::show_overlay(ComponentPtr component, OverlayOptions options) {
    Entry entry;
    entry.id = next_id_++;
    entry.component = std::move(component);
    entry.options = std::move(options);
    entry.pre_focus = focused_;
    entry.focus_order = next_focus_order_++;

    overlays_.push_back(entry);
    auto& stored = overlays_.back();
    if (!stored.options.non_capturing && is_visible_now(stored)) {
        set_focus(stored.component);
    }
    invalidate();
    return OverlayHandle(this, stored.id);
}

void OverlaySurface::hide_overlay() {
    if (overlays_.empty()) {
        return;
    }
    const Entry removed = overlays_.back();
    const bool had_focus = focused_ == removed.component;
    overlays_.pop_back();
    if (had_focus) {
        restore_focus_after(removed);
    }
    invalidate();
}

void OverlaySurface::clear_overlays() {
    if (overlays_.empty()) {
        return;
    }
    overlays_.clear();
    set_focus(base_);
    invalidate();
}

bool OverlaySurface::has_overlay() const {
    return std::any_of(overlays_.begin(), overlays_.end(), [this](const Entry& entry) {
        return is_visible_now(entry);
    });
}

void OverlaySurface::handle_input(std::string_view bytes) {
    std::string current(bytes);
    for (const auto& [_, listener] : input_listeners_) {
        if (!listener) {
            continue;
        }
        auto result = listener(current);
        if (result.consume) {
            return;
        }
        if (result.data) {
            current = std::move(*result.data);
        }
        if (current.empty()) {
            return;
        }
    }
    bytes = current;

    const auto focused_overlay = std::find_if(overlays_.begin(), overlays_.end(), [this](const auto& entry) {
        return entry.component == focused_;
    });
    if (focused_overlay != overlays_.end() && !is_visible_now(*focused_overlay)) {
        restore_focus_after(*focused_overlay);
    }
    auto focused = focused_;
    if (auto* handler = as_input_handler(focused)) {
        if (is_key_release(bytes) && !handler->wants_key_release()) {
            return;
        }
        handler->handle_input(bytes);
    }
}

std::size_t OverlaySurface::add_input_listener(InputListener listener) {
    const std::size_t id = next_listener_id_++;
    input_listeners_.push_back({id, std::move(listener)});
    return id;
}

void OverlaySurface::remove_input_listener(std::size_t listener_id) {
    input_listeners_.erase(
        std::remove_if(input_listeners_.begin(), input_listeners_.end(),
                       [listener_id](const auto& entry) { return entry.first == listener_id; }),
        input_listeners_.end());
}

foundation::Result<RenderBuffer> OverlaySurface::render(const RenderConstraints& constraints) const {
    last_width_ = constraints.max_width;
    last_height_ = constraints.max_height;

    RenderBuffer result;
    if (base_) {
        auto base = base_->render(constraints);
        if (!base) {
            return base.error();
        }
        result = std::move(base.value());
    } else {
        auto empty = make_buffer({}, constraints);
        if (!empty) {
            return empty.error();
        }
        result = std::move(empty.value());
    }

    const std::size_t term_width = constraints.max_width;
    const std::size_t term_height = constraints.max_height;
    if (term_width == 0 || term_height == 0) {
        return result;
    }

    if (overlays_.empty()) {
        for (auto& line : result.lines) {
            if (contains_inline_image_sequence(line)) {
                continue;
            }
            append_line_reset(line);
        }
        return result;
    }

    normalize_surface_lines(result, term_width, true, term_height);

    std::vector<const Entry*> visible;
    for (const auto& entry : overlays_) {
        if (is_visible(entry, term_width, term_height)) {
            visible.push_back(&entry);
        }
    }
    std::stable_sort(visible.begin(), visible.end(), [](const Entry* lhs, const Entry* rhs) {
        return lhs->focus_order < rhs->focus_order;
    });

    for (const auto* entry : visible) {
        const auto initial = resolve_layout(entry->options, 0, term_width, term_height);
        const std::size_t render_height = initial.max_height.value_or(term_height);
        const bool frame_overlay =
            entry->options.frame && initial.width >= 3U && render_height >= 3U;
        const std::size_t content_width =
            frame_overlay ? initial.width - 2U : initial.width;
        const std::size_t content_height =
            frame_overlay ? render_height - 2U : render_height;
        auto rendered =
            entry->component->render(RenderConstraints{content_width, content_height});
        if (!rendered) {
            return rendered.error();
        }
        auto overlay_lines = std::move(rendered.value().lines);
        if (frame_overlay) {
            overlay_lines = framed_overlay_lines(std::move(overlay_lines), initial.width);
        }
        if (initial.max_height && overlay_lines.size() > *initial.max_height) {
            overlay_lines.resize(*initial.max_height);
        }
        const auto layout = resolve_layout(entry->options, overlay_lines.size(), term_width, term_height);
        for (std::size_t row = 0; row < overlay_lines.size(); ++row) {
            const std::size_t target = layout.row + row;
            if (target >= result.lines.size()) {
                continue;
            }
            result.lines[target] = composite_line(result.lines[target], overlay_lines[row], layout.col,
                                                  layout.width, term_width);
        }
    }

    terminate_styled_lines(result, term_width);
    result.width = term_width;
    if (result.lines.size() > term_height) {
        result.lines.resize(term_height);
    }
    return result;
}

OverlaySurface::Entry* OverlaySurface::find_entry(std::size_t id) {
    auto iter = std::find_if(overlays_.begin(), overlays_.end(),
                             [id](const Entry& entry) { return entry.id == id; });
    return iter == overlays_.end() ? nullptr : &*iter;
}

const OverlaySurface::Entry* OverlaySurface::find_entry(std::size_t id) const {
    auto iter = std::find_if(overlays_.begin(), overlays_.end(),
                             [id](const Entry& entry) { return entry.id == id; });
    return iter == overlays_.end() ? nullptr : &*iter;
}

const OverlaySurface::Entry* OverlaySurface::topmost_visible_capturing() const {
    for (auto iter = overlays_.rbegin(); iter != overlays_.rend(); ++iter) {
        if (!iter->options.non_capturing && is_visible_now(*iter)) {
            return &*iter;
        }
    }
    return nullptr;
}

bool OverlaySurface::is_visible(const Entry& entry, std::size_t width, std::size_t height) const {
    if (entry.hidden || !entry.component) {
        return false;
    }
    if (entry.options.visible) {
        return entry.options.visible(width, height);
    }
    return true;
}

bool OverlaySurface::is_visible_now(const Entry& entry) const {
    return is_visible(entry, last_width_, last_height_);
}

void OverlaySurface::restore_focus_after(const Entry& entry) {
    if (const auto* top = topmost_visible_capturing(); top != nullptr && top->id != entry.id) {
        set_focus(top->component);
    } else {
        set_focus(entry.pre_focus);
    }
}

void OverlaySurface::bump_focus_order(Entry& entry) {
    entry.focus_order = next_focus_order_++;
}

void OverlaySurface::invalidate() const {
    if (invalidation_callback_) {
        invalidation_callback_();
    }
}

OverlaySurface::Layout OverlaySurface::resolve_layout(const OverlayOptions& options,
                                                      std::size_t overlay_height,
                                                      std::size_t term_width,
                                                      std::size_t term_height) const {
    const std::size_t margin_top = non_negative(options.margin.top);
    const std::size_t margin_right = non_negative(options.margin.right);
    const std::size_t margin_bottom = non_negative(options.margin.bottom);
    const std::size_t margin_left = non_negative(options.margin.left);

    const std::size_t avail_width =
        std::max<std::size_t>(1, term_width > margin_left + margin_right
                                     ? term_width - margin_left - margin_right
                                     : 1);
    const std::size_t avail_height =
        std::max<std::size_t>(1, term_height > margin_top + margin_bottom
                                     ? term_height - margin_top - margin_bottom
                                     : 1);

    std::size_t width =
        options.width ? resolve_size(*options.width, term_width) : std::min<std::size_t>(80, avail_width);
    if (options.min_width) {
        width = std::max(width, *options.min_width);
    }
    width = clamp_to_range(width, 1, avail_width);

    std::optional<std::size_t> max_height;
    if (options.max_height) {
        max_height = clamp_to_range(resolve_size(*options.max_height, term_height), 1, avail_height);
    }
    const std::size_t effective_height =
        max_height ? std::min(overlay_height, *max_height) : std::min(overlay_height, avail_height);

    auto anchor_row = [&]() -> std::size_t {
        switch (options.anchor) {
            case OverlayAnchor::top_left:
            case OverlayAnchor::top_center:
            case OverlayAnchor::top_right:
                return margin_top;
            case OverlayAnchor::bottom_left:
            case OverlayAnchor::bottom_center:
            case OverlayAnchor::bottom_right:
                return margin_top + avail_height - effective_height;
            case OverlayAnchor::left_center:
            case OverlayAnchor::center:
            case OverlayAnchor::right_center:
                return margin_top + (avail_height - effective_height) / 2U;
        }
        return margin_top;
    };

    auto anchor_col = [&]() -> std::size_t {
        switch (options.anchor) {
            case OverlayAnchor::top_left:
            case OverlayAnchor::left_center:
            case OverlayAnchor::bottom_left:
                return margin_left;
            case OverlayAnchor::top_right:
            case OverlayAnchor::right_center:
            case OverlayAnchor::bottom_right:
                return margin_left + avail_width - width;
            case OverlayAnchor::top_center:
            case OverlayAnchor::center:
            case OverlayAnchor::bottom_center:
                return margin_left + (avail_width - width) / 2U;
        }
        return margin_left;
    };

    std::size_t row = anchor_row();
    if (options.row) {
        if (options.row->kind == OverlaySize::Kind::percent) {
            const std::size_t max_row = avail_height > effective_height ? avail_height - effective_height : 0;
            row = margin_top + max_row * options.row->value / 100U;
        } else {
            row = options.row->value;
        }
    }

    std::size_t col = anchor_col();
    if (options.col) {
        if (options.col->kind == OverlaySize::Kind::percent) {
            const std::size_t max_col = avail_width > width ? avail_width - width : 0;
            col = margin_left + max_col * options.col->value / 100U;
        } else {
            col = options.col->value;
        }
    }

    const int shifted_row = static_cast<int>(row) + options.offset_y;
    const int shifted_col = static_cast<int>(col) + options.offset_x;
    row = non_negative(shifted_row);
    col = non_negative(shifted_col);

    const std::size_t max_row =
        term_height > margin_bottom + effective_height ? term_height - margin_bottom - effective_height : margin_top;
    const std::size_t max_col =
        term_width > margin_right + width ? term_width - margin_right - width : margin_left;
    row = clamp_to_range(row, margin_top, max_row);
    col = clamp_to_range(col, margin_left, max_col);

    return Layout{width, row, col, max_height};
}

std::string OverlaySurface::composite_line(std::string_view base, std::string_view overlay,
                                           std::size_t start_col, std::size_t overlay_width,
                                           std::size_t total_width) const {
    if (contains_inline_image_sequence(base)) {
        return std::string(base);
    }

    const auto before = take_columns(base, start_col);
    const auto after_start = std::min(total_width, start_col + overlay_width);
    const auto after = drop_columns(base, after_start);
    auto overlay_text = take_columns(overlay, overlay_width);
    overlay_text = pad_to_width(std::move(overlay_text), overlay_width);

    std::string line = before;
    append_line_reset(line);
    line += overlay_text;
    append_line_reset(line);
    line += after;
    if (contains_escape(line)) {
        return line;
    }
    return pad_to_width(take_columns(line, total_width), total_width);
}

}  // namespace qt::tui
