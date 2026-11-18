#include "qt/tui/terminal/diff_renderer.hpp"

#include "qt/foundation/utf8.hpp"
#include "qt/tui/terminal/ansi.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qt::tui {

namespace {

std::optional<std::size_t> visible_width(std::string_view text) {
    std::size_t width = 0;
    std::size_t offset = 0;
    while (offset < text.size()) {
        if (auto sequence = extract_ansi_sequence(text, offset)) {
            offset += sequence->length;
            continue;
        }

        const auto next_escape = text.find('\x1b', offset);
        const auto end = next_escape == std::string_view::npos ? text.size()
                         : next_escape == offset ? offset + 1U : next_escape;
        auto span_width = foundation::utf8_display_width(text.substr(offset, end - offset));
        if (!span_width) {
            return std::nullopt;
        }
        width += span_width.value();
        offset = end;
    }
    return width;
}

bool contains_sgr(std::string_view text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        auto sequence = extract_ansi_sequence(text, offset);
        if (!sequence) {
            ++offset;
            continue;
        }
        if (is_sgr_sequence(text.substr(offset), *sequence)) {
            return true;
        }
        offset += sequence->length;
    }
    return false;
}

std::size_t utf8_boundary_at_or_before(std::string_view text, std::size_t offset) {
    offset = std::min(offset, text.size());
    if (offset == text.size()) {
        return offset;
    }
    while (offset > 0) {
        const unsigned char ch = static_cast<unsigned char>(text[offset]);
        if ((ch & 0xC0U) != 0x80U) {
            break;
        }
        --offset;
    }
    return offset;
}

struct CursorMarkerLocation {
    CursorPosition cursor;
    std::size_t byte_offset = 0;
};

std::optional<CursorMarkerLocation> find_cursor_marker(const RenderBuffer& buffer,
                                                       std::size_t viewport_height) {
    const std::size_t viewport_top =
        buffer.lines.size() > viewport_height ? buffer.lines.size() - viewport_height : 0U;
    for (std::size_t row = buffer.lines.size(); row > viewport_top; --row) {
        const auto& line = buffer.lines[row - 1U];
        const auto marker = line.find(cursor_marker);
        if (marker == std::string::npos) {
            continue;
        }

        const auto before_marker = std::string_view(line).substr(0, marker);
        const auto column = visible_width(before_marker).value_or(before_marker.size());
        return CursorMarkerLocation{CursorPosition{row - 1U, column}, marker};
    }
    return std::nullopt;
}

std::string normalize_row_text(std::string_view raw, std::size_t width) {
    std::string line(raw);
    if (contains_inline_image_sequence(line)) {
        return line;
    }

    auto measured = visible_width(line);
    if (!measured) {
        if (line.size() < width) {
            line.resize(width, ' ');
        } else if (line.size() > width) {
            line.resize(width);
        }
        line.append(terminal_line_reset_sequence.data(), terminal_line_reset_sequence.size());
        return line;
    }

    if (measured.value() < width) {
        line.append(width - measured.value(), ' ');
    }
    line.append(terminal_line_reset_sequence.data(), terminal_line_reset_sequence.size());
    return line;
}

const DiffRendererNormalizedRowCacheEntry& normalize_row_cached(
    std::vector<DiffRendererNormalizedRowCacheEntry>& cache, std::size_t row,
    std::string_view raw, std::size_t width) {
    if (cache.size() <= row) {
        cache.resize(row + 1U);
    }

    auto& entry = cache[row];
    if (!entry.valid || entry.width != width || entry.raw != raw) {
        entry.valid = true;
        entry.width = width;
        entry.raw.assign(raw);
        entry.normalized = normalize_row_text(entry.raw, width);
    }
    return entry;
}

std::string_view raw_row(const RenderBuffer& buffer, std::size_t row) noexcept {
    if (row >= buffer.lines.size()) {
        return {};
    }
    return buffer.lines[row];
}

std::optional<RowUpdate> diff_row(std::size_t row, const std::string& previous,
                                  const std::string& next) {
    if (previous == next) {
        return std::nullopt;
    }
    if (contains_sgr(previous) || contains_sgr(next)) {
        return RowUpdate{CursorPosition{row, 0}, next};
    }

    std::size_t prefix = 0;
    while (prefix < previous.size() && prefix < next.size() && previous[prefix] == next[prefix]) {
        ++prefix;
    }
    prefix = std::min(utf8_boundary_at_or_before(previous, prefix),
                      utf8_boundary_at_or_before(next, prefix));

    std::size_t previous_suffix = previous.size();
    std::size_t next_suffix = next.size();
    while (previous_suffix > prefix && next_suffix > prefix &&
           previous[previous_suffix - 1] == next[next_suffix - 1]) {
        --previous_suffix;
        --next_suffix;
    }

    const auto column = visible_width(next.substr(0, prefix)).value_or(prefix);
    return RowUpdate{CursorPosition{row, column}, next.substr(prefix, next_suffix - prefix)};
}

std::vector<std::uint32_t> extract_kitty_image_ids(std::string_view line) {
    constexpr std::string_view prefix = "\x1b_G";

    std::vector<std::uint32_t> ids;
    std::size_t sequence_start = 0;
    while ((sequence_start = line.find(prefix, sequence_start)) != std::string_view::npos) {
        const auto params_start = sequence_start + prefix.size();
        const auto params_end = line.find(';', params_start);
        if (params_end == std::string_view::npos) {
            break;
        }

        std::size_t param_start = params_start;
        while (param_start <= params_end) {
            const auto param_end = line.find(',', param_start);
            const auto clamped_end = std::min(param_end, params_end);
            const auto param = line.substr(param_start, clamped_end - param_start);
            if (param.rfind("i=", 0) == 0 && param.size() > 2) {
                std::uint64_t value = 0;
                bool valid = true;
                for (const char ch : param.substr(2)) {
                    if (ch < '0' || ch > '9') {
                        valid = false;
                        break;
                    }
                    value = (value * 10U) + static_cast<std::uint64_t>(ch - '0');
                    if (value > 0xffffffffULL) {
                        valid = false;
                        break;
                    }
                }
                if (valid && value > 0U) {
                    ids.push_back(static_cast<std::uint32_t>(value));
                }
            }

            if (param_end == std::string_view::npos || param_end >= params_end) {
                break;
            }
            param_start = param_end + 1;
        }

        sequence_start = params_end + 1;
    }

    return ids;
}

void append_unique(std::vector<std::uint32_t>& values, std::uint32_t value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

bool has_update_for_row(const std::vector<RowUpdate>& updates, std::size_t row) {
    return std::any_of(updates.begin(), updates.end(),
                       [row](const RowUpdate& update) { return update.cursor.row == row; });
}

}  // namespace

bool DiffRenderer::has_previous_frame() const noexcept { return previous_.has_value(); }

const RenderBuffer* DiffRenderer::previous_frame() const noexcept {
    return previous_ ? &previous_.value() : nullptr;
}

FrameDiff DiffRenderer::diff(const RenderBuffer& next) const {
    next_row_cache_.clear();
    const auto cursor_marker_location = find_cursor_marker(next, next.height());
    auto cursor_position = cursor_marker_location
                               ? std::optional<CursorPosition>(
                                     cursor_marker_location->cursor)
                               : std::nullopt;

    const bool width_changed =
        previous_ && previous_width_ != 0U && previous_width_ != next.width;
    const bool height_changed =
        previous_ && previous_height_ != 0U && previous_height_ != next.height();
    const std::size_t previous_buffer_length =
        previous_ ? std::max(max_lines_rendered_, previous_->height()) : next.height();
    const std::size_t viewport_height = next.height();
    const std::size_t previous_viewport_top =
        height_changed && viewport_height > 0U && previous_buffer_length > viewport_height
            ? previous_buffer_length - viewport_height
            : previous_viewport_top_;
    const std::size_t next_buffer_length =
        std::max(viewport_height, next.height());
    const std::size_t viewport_top =
        viewport_height > 0U && next_buffer_length > viewport_height
            ? next_buffer_length - viewport_height
            : 0U;

    const std::size_t width =
        std::max(previous_ ? previous_->width : 0U, next.width);
    const std::size_t height =
        std::max(previous_ ? previous_->height() : 0U, next.height());

    FrameDiff frame_diff;
    frame_diff.width = next.width;
    frame_diff.height = next.height();
    frame_diff.previous_viewport_top = previous_viewport_top;
    frame_diff.viewport_top = viewport_top;
    frame_diff.rows_scanned = height;
    frame_diff.cursor_position = cursor_position;

    RenderBuffer empty_previous;
    const RenderBuffer& previous = previous_ ? previous_.value() : empty_previous;

    std::optional<std::size_t> first_changed;
    std::optional<std::size_t> last_changed;

    for (std::size_t row = 0; row < height; ++row) {
        const auto& previous_row = normalize_row_cached(
            previous_row_cache_, row, raw_row(previous, row), width);
        std::string next_raw_storage;
        std::string_view next_raw = raw_row(next, row);
        if (cursor_marker_location && cursor_marker_location->cursor.row == row) {
            next_raw_storage.reserve(next_raw.size() - cursor_marker.size());
            next_raw_storage.append(next_raw.substr(0, cursor_marker_location->byte_offset));
            next_raw_storage.append(next_raw.substr(cursor_marker_location->byte_offset +
                                                   cursor_marker.size()));
            next_raw = next_raw_storage;
        }
        const auto& next_row = normalize_row_cached(next_row_cache_, row, next_raw, width);
        auto update = diff_row(row, previous_row.normalized, next_row.normalized);
        if (update) {
            if (!first_changed) {
                first_changed = row;
            }
            last_changed = row;
            frame_diff.updates.push_back(std::move(update.value()));
        }
    }

    const bool content_shrank =
        previous_ && next.height() < max_lines_rendered_;
    const bool changed_above_previous_viewport =
        first_changed && *first_changed < previous_viewport_top;
    if (width_changed || height_changed || content_shrank ||
        changed_above_previous_viewport) {
        frame_diff.full_redraw = true;
        frame_diff.clear_scrollback = true;
        frame_diff.updates.clear();
        const std::size_t redraw_height = next.height();
        frame_diff.updates.reserve(redraw_height);
        for (std::size_t row = 0; row < redraw_height; ++row) {
            std::string next_raw_storage;
            std::string_view next_raw = raw_row(next, row);
            if (cursor_marker_location && cursor_marker_location->cursor.row == row) {
                next_raw_storage.reserve(next_raw.size() - cursor_marker.size());
                next_raw_storage.append(next_raw.substr(0, cursor_marker_location->byte_offset));
                next_raw_storage.append(next_raw.substr(cursor_marker_location->byte_offset +
                                                       cursor_marker.size()));
                next_raw = next_raw_storage;
            }
            const auto& next_row =
                normalize_row_cached(next_row_cache_, row, next_raw, next.width);
            frame_diff.updates.push_back(
                RowUpdate{CursorPosition{row, 0U}, next_row.normalized});
        }
    }

    if (first_changed) {
        for (std::size_t row = *first_changed; row < height; ++row) {
            const auto image_ids = extract_kitty_image_ids(raw_row(previous, row));
            if (image_ids.empty()) {
                continue;
            }
            for (const auto image_id : image_ids) {
                append_unique(frame_diff.deleted_image_ids, image_id);
            }
            last_changed = std::max(last_changed.value_or(row), row);
        }
    }

    if (last_changed) {
        for (std::size_t row = 0; row <= *last_changed; ++row) {
            if (has_update_for_row(frame_diff.updates, row)) {
                continue;
            }
            if (extract_kitty_image_ids(raw_row(previous, row)).empty()) {
                continue;
            }
            std::string next_raw_storage;
            std::string_view next_raw = raw_row(next, row);
            if (cursor_marker_location && cursor_marker_location->cursor.row == row) {
                next_raw_storage.reserve(next_raw.size() - cursor_marker.size());
                next_raw_storage.append(next_raw.substr(0, cursor_marker_location->byte_offset));
                next_raw_storage.append(next_raw.substr(cursor_marker_location->byte_offset +
                                                       cursor_marker.size()));
                next_raw = next_raw_storage;
            }
            const auto& next_row = normalize_row_cached(next_row_cache_, row, next_raw, width);
            frame_diff.updates.push_back(
                RowUpdate{CursorPosition{row, 0U}, next_row.normalized});
        }
    }

    std::sort(frame_diff.updates.begin(), frame_diff.updates.end(),
              [](const RowUpdate& lhs, const RowUpdate& rhs) {
                  if (lhs.cursor.row != rhs.cursor.row) {
                      return lhs.cursor.row < rhs.cursor.row;
                  }
                  return lhs.cursor.column < rhs.cursor.column;
              });

    return frame_diff;
}

FrameDiff DiffRenderer::present(RenderBuffer next) {
    auto cursor_position = extract_cursor_marker(next.lines, next.height());
    auto frame_diff = diff(next);
    frame_diff.cursor_position = cursor_position;
    previous_width_ = next.width;
    previous_height_ = next.height();
    max_lines_rendered_ = frame_diff.clear_scrollback
                              ? next.height()
                              : std::max(max_lines_rendered_, next.height());
    previous_viewport_top_ =
        previous_height_ > 0U && max_lines_rendered_ > previous_height_
            ? max_lines_rendered_ - previous_height_
            : 0U;
    previous_ = std::move(next);
    previous_row_cache_ = std::move(next_row_cache_);
    next_row_cache_.clear();
    return frame_diff;
}

foundation::Result<FrameDiff> DiffRenderer::render(const Component& component,
                                                   const RenderConstraints& constraints) {
    auto next = component.render(constraints);
    if (!next) {
        return next.error();
    }

    return present(std::move(next.value()));
}

void DiffRenderer::reset() noexcept {
    previous_.reset();
    previous_width_ = 0;
    previous_height_ = 0;
    previous_viewport_top_ = 0;
    max_lines_rendered_ = 0;
    previous_row_cache_.clear();
    next_row_cache_.clear();
}

std::string ansi_move_cursor(CursorPosition cursor) {
    return "\x1b[" + std::to_string(cursor.row + 1) + ';' +
           std::to_string(cursor.column + 1) + 'H';
}

SynchronizedOutput synchronized_output_for_diff(const FrameDiff& frame_diff) {
    SynchronizedOutput output;
    if (frame_diff.full_redraw) {
        output.write("\x1b[2J\x1b[H");
        if (frame_diff.clear_scrollback) {
            output.write("\x1b[3J");
        }
    }
    for (const auto image_id : frame_diff.deleted_image_ids) {
        output.write(ansi_kitty_delete_image(image_id));
    }
    for (const auto& update : frame_diff.updates) {
        output.write(ansi_move_cursor(update.cursor));
        output.write(update.text);
    }
    if (frame_diff.cursor_position) {
        output.write(ansi_move_cursor(*frame_diff.cursor_position));
    } else {
        output.write(ansi_set_cursor_visible(false));
    }
    return output;
}

}  // namespace qt::tui
