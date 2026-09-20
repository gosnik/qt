#include "qt/tui/terminal/notcurses_backend.hpp"

#include "qt/foundation/utf8.hpp"
#include "qt/tui/input/keys.hpp"

#if __has_include(<notcurses/notcurses.h>) && __has_include(<version.h>)
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wc++20-extensions"
#endif
#include <notcurses/notcurses.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include <notcurses/nckeys.h>
#include <version.h>
#define QT_TUI_NOTCURSES_BACKEND_HAS_HEADERS 1
#else
#define QT_TUI_NOTCURSES_BACKEND_HAS_HEADERS 0
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qt::tui {
namespace {

using NotcursesStyle = NotcursesRenderOperation::Style;

std::optional<std::size_t> sgr_sequence_end(std::string_view text,
                                            std::size_t offset) noexcept {
    if (offset + 2 > text.size() || text[offset] != '\x1b' || text[offset + 1] != '[') {
        return std::nullopt;
    }
    for (std::size_t index = offset + 2; index < text.size(); ++index) {
        const auto ch = text[index];
        if (ch >= '@' && ch <= '~') {
            return index;
        }
    }
    return std::nullopt;
}

std::vector<int> sgr_parameters(std::string_view sequence) {
    std::vector<int> parameters;
    if (sequence.size() < 3 || sequence.back() != 'm') {
        return parameters;
    }

    const auto body = sequence.substr(2, sequence.size() - 3);
    if (body.empty()) {
        parameters.push_back(0);
        return parameters;
    }

    std::size_t start = 0;
    while (start <= body.size()) {
        const auto end = body.find(';', start);
        const auto token = body.substr(start, end == std::string_view::npos
                                                  ? std::string_view::npos
                                                  : end - start);
        if (token.empty()) {
            parameters.push_back(0);
        } else {
            const std::string token_text(token);
            char* parsed_end = nullptr;
            const auto value = std::strtol(token_text.c_str(), &parsed_end, 10);
            parameters.push_back(parsed_end != nullptr && *parsed_end == '\0'
                                     ? static_cast<int>(value)
                                     : 0);
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return parameters;
}

NotcursesStyle::Color palette_color(std::uint8_t index) noexcept {
    NotcursesStyle::Color color;
    color.kind = NotcursesStyle::Color::Kind::palette;
    color.palette_index = index;
    return color;
}

NotcursesStyle::Color rgb_color(int red, int green, int blue) noexcept {
    auto clamp = [](int value) {
        if (value < 0) {
            return 0;
        }
        if (value > 255) {
            return 255;
        }
        return value;
    };
    NotcursesStyle::Color color;
    color.kind = NotcursesStyle::Color::Kind::rgb;
    color.rgb = (static_cast<std::uint32_t>(clamp(red)) << 16U) |
                (static_cast<std::uint32_t>(clamp(green)) << 8U) |
                static_cast<std::uint32_t>(clamp(blue));
    return color;
}

void apply_basic_color(NotcursesStyle& style, int parameter) noexcept {
    if (parameter >= 30 && parameter <= 37) {
        style.foreground = palette_color(static_cast<std::uint8_t>(parameter - 30));
    } else if (parameter >= 90 && parameter <= 97) {
        style.foreground = palette_color(static_cast<std::uint8_t>(parameter - 90 + 8));
    } else if (parameter >= 40 && parameter <= 47) {
        style.background = palette_color(static_cast<std::uint8_t>(parameter - 40));
    } else if (parameter >= 100 && parameter <= 107) {
        style.background = palette_color(static_cast<std::uint8_t>(parameter - 100 + 8));
    }
}

void apply_sgr_sequence(NotcursesStyle& style, std::string_view sequence) {
    const auto parameters = sgr_parameters(sequence);
    if (parameters.empty()) {
        return;
    }

    for (std::size_t index = 0; index < parameters.size(); ++index) {
        const auto parameter = parameters[index];
        switch (parameter) {
            case 0:
                style = NotcursesStyle{};
                break;
            case 1:
                style.bold = true;
                break;
            case 3:
                style.italic = true;
                break;
            case 4:
                style.underline = true;
                break;
            case 9:
                style.struck = true;
                break;
            case 22:
                style.bold = false;
                break;
            case 23:
                style.italic = false;
                break;
            case 24:
                style.underline = false;
                break;
            case 29:
                style.struck = false;
                break;
            case 39:
                style.foreground = NotcursesStyle::Color{};
                break;
            case 49:
                style.background = NotcursesStyle::Color{};
                break;
            case 38:
            case 48: {
                const bool foreground = parameter == 38;
                if (index + 2 < parameters.size() && parameters[index + 1] == 5) {
                    const auto palette = parameters[index + 2] < 0
                                             ? 0
                                             : parameters[index + 2] > 255
                                                   ? 255
                                                   : parameters[index + 2];
                    auto color = palette_color(static_cast<std::uint8_t>(palette));
                    if (foreground) {
                        style.foreground = color;
                    } else {
                        style.background = color;
                    }
                    index += 2;
                } else if (index + 4 < parameters.size() && parameters[index + 1] == 2) {
                    auto color = rgb_color(parameters[index + 2], parameters[index + 3],
                                           parameters[index + 4]);
                    if (foreground) {
                        style.foreground = color;
                    } else {
                        style.background = color;
                    }
                    index += 4;
                }
                break;
            }
            default:
                apply_basic_color(style, parameter);
                break;
        }
    }
}

bool same_color(const NotcursesStyle::Color& left,
                const NotcursesStyle::Color& right) noexcept {
    return left.kind == right.kind && left.palette_index == right.palette_index &&
           left.rgb == right.rgb;
}

bool same_style(const NotcursesStyle& left, const NotcursesStyle& right) noexcept {
    return left.bold == right.bold && left.italic == right.italic &&
           left.underline == right.underline && left.struck == right.struck &&
           same_color(left.foreground, right.foreground) &&
           same_color(left.background, right.background);
}

std::size_t utf8_sequence_length(std::string_view text, std::size_t offset) noexcept {
    const auto lead = static_cast<unsigned char>(text[offset]);
    if ((lead & 0x80U) == 0U) {
        return 1;
    }
    if ((lead & 0xe0U) == 0xc0U && offset + 1 < text.size()) {
        return 2;
    }
    if ((lead & 0xf0U) == 0xe0U && offset + 2 < text.size()) {
        return 3;
    }
    if ((lead & 0xf8U) == 0xf0U && offset + 3 < text.size()) {
        return 4;
    }
    return 1;
}

std::size_t display_width_or_one(std::string_view text) {
    const auto width = foundation::utf8_display_width(text);
    if (!width) {
        return 1;
    }
    return width.value();
}

std::vector<NotcursesRenderOperation> styled_put_operations(std::size_t row,
                                                            std::string_view line) {
    std::vector<NotcursesRenderOperation> operations;
    NotcursesStyle current_style;
    std::string pending;
    std::size_t visible_column = 0;
    std::size_t pending_column = 0;
    NotcursesStyle pending_style;
    bool has_pending = false;

    auto flush = [&]() {
        if (pending.empty()) {
            return;
        }
        NotcursesRenderOperation operation;
        operation.kind = NotcursesRenderOperationKind::put_text;
        operation.row = row;
        operation.column = pending_column;
        operation.text = std::move(pending);
        operation.style = pending_style;
        operations.push_back(std::move(operation));
        pending.clear();
        has_pending = false;
    };

    for (std::size_t offset = 0; offset < line.size();) {
        if (auto end = sgr_sequence_end(line, offset)) {
            const auto sequence = line.substr(offset, *end - offset + 1);
            if (sequence.back() == 'm') {
                flush();
                apply_sgr_sequence(current_style, sequence);
                offset = *end + 1;
                continue;
            }
        }

        if (!has_pending || !same_style(pending_style, current_style)) {
            flush();
            pending_style = current_style;
            pending_column = visible_column;
            has_pending = true;
        }
        const auto sequence_length = utf8_sequence_length(line, offset);
        const auto cell = line.substr(offset, sequence_length);
        pending.append(cell.data(), cell.size());
        visible_column += display_width_or_one(cell);
        offset += sequence_length;
    }

    flush();
    return operations;
}

InputEvent key_event(std::string key, std::string bytes) {
    InputEvent event;
    event.kind = InputEventKind::key;
    event.bytes = std::move(bytes);
    event.key = std::move(key);
    return event;
}

std::string resize_report_bytes(const TerminalSize& size) {
    return "\x1b[8;" + std::to_string(size.rows) + ";" +
           std::to_string(size.columns) + "t";
}

std::string ansi_set_scrolling_region(std::size_t top_row,
                                      std::size_t bottom_row) {
    return "\x1b[" + std::to_string(top_row) + ";" +
           std::to_string(bottom_row) + "r";
}

std::string ansi_reset_scrolling_region() { return "\x1b[r"; }

std::string ansi_move_cursor_1_based(std::size_t row, std::size_t column) {
    return "\x1b[" + std::to_string(row) + ";" + std::to_string(column) + "H";
}

std::string ansi_clear_current_line() { return "\x1b[2K"; }

std::string ansi_set_auto_wrap(bool enabled) {
    return enabled ? "\x1b[?7h" : "\x1b[?7l";
}

std::string sgr_scroll_bytes(const NotcursesInputRecord& record) {
    const int button = record.id == NCKEY_SCROLL_UP ? 64 : 65;
    const int x = record.x >= 0 ? record.x : 0;
    const int y = record.y >= 0 ? record.y : 0;
    return "\x1b[<" + std::to_string(button) + ";" + std::to_string(x) + ";" +
           std::to_string(y) + "M";
}

std::string apply_notcurses_modifiers(std::string base, unsigned modifiers) {
    std::string key;
    if ((modifiers & NCKEY_MOD_SHIFT) != 0U &&
        base != "tab" && base != "enter" && base != "backspace" &&
        base != "space" &&
        !(base.size() == 1U &&
          std::isalpha(static_cast<unsigned char>(base.front())) != 0)) {
        key += "shift+";
    }
    if ((modifiers & NCKEY_MOD_CTRL) != 0U) {
        key += "ctrl+";
    }
    if ((modifiers & NCKEY_MOD_ALT) != 0U) {
        key += "alt+";
    }
    if ((modifiers & NCKEY_MOD_SUPER) != 0U) {
        key += "super+";
    }
    key += base;
    return key;
}

std::optional<std::string> notcurses_named_key(std::uint32_t id) {
    switch (id) {
        case NCKEY_UP:
            return "up";
        case NCKEY_DOWN:
            return "down";
        case NCKEY_LEFT:
            return "left";
        case NCKEY_RIGHT:
            return "right";
        case NCKEY_HOME:
            return "home";
        case NCKEY_END:
            return "end";
        case NCKEY_PGUP:
            return "pageUp";
        case NCKEY_PGDOWN:
            return "pageDown";
        case NCKEY_BACKSPACE:
            return "backspace";
        case NCKEY_DEL:
            return "delete";
        case NCKEY_ENTER:
            return "enter";
        case NCKEY_TAB:
            return "tab";
        case NCKEY_ESC:
            return "escape";
        default:
            return std::nullopt;
    }
}

#if QT_TUI_NOTCURSES_BACKEND_HAS_HEADERS
std::uint64_t option_flags(const NotcursesBackendOptions& options) noexcept {
    std::uint64_t flags = NCOPTION_NO_FONT_CHANGES;
    if (options.suppress_banners) {
        flags |= NCOPTION_SUPPRESS_BANNERS;
    }
    if (options.mode == NotcursesBackendMode::cli) {
        flags |= NCOPTION_CLI_MODE;
    } else if (!options.use_alternate_screen) {
        flags |= NCOPTION_NO_ALTERNATE_SCREEN | NCOPTION_NO_CLEAR_BITMAPS |
                 NCOPTION_PRESERVE_CURSOR | NCOPTION_SCROLLING;
    }
    if (!options.install_signal_handlers) {
        flags |= NCOPTION_NO_WINCH_SIGHANDLER | NCOPTION_NO_QUIT_SIGHANDLERS;
    }
    if (options.drain_input) {
        flags |= NCOPTION_DRAIN_INPUT;
    }
    return flags;
}

notcurses_options make_options(const NotcursesBackendOptions& options) noexcept {
    notcurses_options native{};
    native.termtype = options.termtype;
    native.flags = option_flags(options);
    return native;
}

struct notcurses* context_cast(void* context) noexcept {
    return static_cast<struct notcurses*>(context);
}

struct ncplane* plane_cast(void* plane) noexcept {
    return static_cast<struct ncplane*>(plane);
}

NotcursesInputRecord input_record_from_native(const ncinput& input) {
    NotcursesInputRecord record;
    record.id = input.id;
    record.y = input.y;
    record.x = input.x;
    record.modifiers = input.modifiers;
    record.release = input.evtype == NCTYPE_RELEASE;
    if (input.utf8[0] != '\0') {
        record.utf8 = input.utf8;
    }
    return record;
}

std::optional<TerminalSize> terminal_size_from_native(
    const struct notcurses* context) noexcept {
    if (context == nullptr) {
        return std::nullopt;
    }

    unsigned rows = 0;
    unsigned columns = 0;
    notcurses_term_dim_yx(context, &rows, &columns);
    if (rows == 0U || columns == 0U) {
        return std::nullopt;
    }
    return TerminalSize{columns, rows};
}

unsigned native_style_bits(const NotcursesStyle& style) noexcept {
    unsigned bits = NCSTYLE_NONE;
    if (style.bold) {
        bits |= NCSTYLE_BOLD;
    }
    if (style.italic) {
        bits |= NCSTYLE_ITALIC;
    }
    if (style.underline) {
        bits |= NCSTYLE_UNDERLINE;
    }
    if (style.struck) {
        bits |= NCSTYLE_STRUCK;
    }
    return bits;
}

int apply_native_color(struct ncplane* plane, const NotcursesStyle::Color& color,
                       bool foreground) noexcept {
    if (color.kind == NotcursesStyle::Color::Kind::default_color) {
        if (foreground) {
            ncplane_set_fg_default(plane);
        } else {
            ncplane_set_bg_default(plane);
        }
        return 0;
    }
    if (color.kind == NotcursesStyle::Color::Kind::palette) {
        return foreground ? ncplane_set_fg_palindex(plane, color.palette_index)
                          : ncplane_set_bg_palindex(plane, color.palette_index);
    }
    return foreground ? ncplane_set_fg_rgb(plane, color.rgb)
                      : ncplane_set_bg_rgb(plane, color.rgb);
}

int apply_native_style(struct ncplane* plane, const NotcursesStyle& style) noexcept {
    ncplane_set_styles(plane, native_style_bits(style));
    if (apply_native_color(plane, style.foreground, true) < 0) {
        return -1;
    }
    if (apply_native_color(plane, style.background, false) < 0) {
        return -1;
    }
    return 0;
}
#endif

}  // namespace

NotcursesDiagnostics notcurses_diagnostics() noexcept {
    NotcursesDiagnostics diagnostics;
    diagnostics.headers_available = QT_TUI_NOTCURSES_BACKEND_HAS_HEADERS != 0;
    diagnostics.linked = false;
    diagnostics.core_init_available = QT_TUI_NOTCURSES_BACKEND_HAS_HEADERS != 0;
    diagnostics.plane_render_available = QT_TUI_NOTCURSES_BACKEND_HAS_HEADERS != 0;
    diagnostics.input_available = QT_TUI_NOTCURSES_BACKEND_HAS_HEADERS != 0;

#if QT_TUI_NOTCURSES_BACKEND_HAS_HEADERS
    diagnostics.linked = notcurses_version() != nullptr;
    diagnostics.version = NotcursesVersion{
        std::string(NOTCURSES_VERSION_MAJOR) + "." + NOTCURSES_VERSION_MINOR + "." +
            NOTCURSES_VERSION_PATCH + "." + NOTCURSES_VERSION_TWEAK,
        NOTCURSES_VERNUM_MAJOR,
        NOTCURSES_VERNUM_MINOR,
        NOTCURSES_VERNUM_PATCH,
        NOTCURSES_VERNUM_TWEAK,
    };
#endif

    return diagnostics;
}

NotcursesCapabilityNegotiation negotiate_notcurses_capabilities(
    const NotcursesDiagnostics& diagnostics, NotcursesCapabilityRequest request) {
    NotcursesCapabilityNegotiation negotiation;
    negotiation.requested = request;

    if (!diagnostics.headers_available) {
        negotiation.fallback_reason = "notcurses headers are unavailable";
        return negotiation;
    }
    if (!diagnostics.linked) {
        negotiation.fallback_reason = "notcurses runtime is not linked";
        return negotiation;
    }

    negotiation.render_available =
        diagnostics.core_init_available && diagnostics.plane_render_available;
    negotiation.input_available = diagnostics.input_available;

    if (request.render && !negotiation.render_available) {
        negotiation.fallback_reason = "notcurses render capabilities are unavailable";
        return negotiation;
    }
    if (request.input && !negotiation.input_available) {
        negotiation.fallback_reason = "notcurses input capabilities are unavailable";
        return negotiation;
    }

    negotiation.usable = true;
    return negotiation;
}

std::uint32_t notcurses_resize_key_id() noexcept {
#if QT_TUI_NOTCURSES_BACKEND_HAS_HEADERS
    return NCKEY_RESIZE;
#else
    return 0U;
#endif
}

NotcursesOptionsPlan notcurses_options_plan(const NotcursesBackendOptions& options) noexcept {
    NotcursesOptionsPlan plan;
    plan.suppress_banners = options.suppress_banners;
    plan.mode = options.mode;
    plan.use_alternate_screen = options.use_alternate_screen;
    plan.install_signal_handlers = options.install_signal_handlers;
    plan.drain_input = options.drain_input;
    if (options.termtype != nullptr) {
        plan.termtype = options.termtype;
    }
#if QT_TUI_NOTCURSES_BACKEND_HAS_HEADERS
    plan.flags = option_flags(options);
#else
    plan.flags = 0;
#endif
    return plan;
}

NotcursesRenderPlan notcurses_render_plan(const RenderBuffer& buffer) {
    NotcursesRenderPlan plan;
    plan.width = buffer.width;
    plan.height = buffer.height();
    NotcursesRenderOperation erase;
    erase.kind = NotcursesRenderOperationKind::erase_plane;
    plan.operations.push_back(std::move(erase));

    for (std::size_t row = 0; row < buffer.lines.size(); ++row) {
        auto row_operations = styled_put_operations(row, buffer.lines[row]);
        plan.operations.insert(plan.operations.end(),
                               std::make_move_iterator(row_operations.begin()),
                               std::make_move_iterator(row_operations.end()));
    }

    NotcursesRenderOperation render;
    render.kind = NotcursesRenderOperationKind::render;
    plan.operations.push_back(std::move(render));
    return plan;
}

NotcursesRenderPlan notcurses_render_plan(const FrameDiff& frame_diff) {
    NotcursesRenderPlan plan;
    plan.width = frame_diff.width;
    plan.height = frame_diff.height;

    if (frame_diff.full_redraw) {
        NotcursesRenderOperation erase;
        erase.kind = NotcursesRenderOperationKind::erase_plane;
        plan.operations.push_back(std::move(erase));
    }

    for (const auto& update : frame_diff.updates) {
        auto row_operations = styled_put_operations(update.cursor.row, update.text);
        for (auto& operation : row_operations) {
            operation.column += update.cursor.column;
        }
        plan.operations.insert(plan.operations.end(),
                               std::make_move_iterator(row_operations.begin()),
                               std::make_move_iterator(row_operations.end()));
    }

    NotcursesRenderOperation render;
    render.kind = NotcursesRenderOperationKind::render;
    plan.operations.push_back(std::move(render));
    return plan;
}

std::optional<InputEvent> decode_notcurses_input(
    const NotcursesInputRecord& record, std::optional<TerminalSize> resize_size) {
    if (record.release) {
        return std::nullopt;
    }

    const auto lock_only_modifiers = [](unsigned modifiers) {
        return (modifiers & ~(64U | 128U)) == 0U;
    };
#if QT_TUI_NOTCURSES_BACKEND_HAS_HEADERS
    if (record.id == NCKEY_RESIZE) {
        if (!resize_size) {
            return std::nullopt;
        }
        InputEvent event;
        event.kind = InputEventKind::resize;
        event.bytes = resize_report_bytes(*resize_size);
        event.resize = *resize_size;
        return event;
    }
    if (record.id == NCKEY_SCROLL_UP || record.id == NCKEY_SCROLL_DOWN) {
        InputEvent event;
        event.kind = InputEventKind::scroll;
        event.bytes = sgr_scroll_bytes(record);
        event.scroll = ScrollEvent{
            record.id == NCKEY_SCROLL_UP ? 3 : -3,
            record.x >= 0 ? static_cast<std::size_t>(record.x) : 0U,
            record.y >= 0 ? static_cast<std::size_t>(record.y) : 0U,
        };
        return event;
    }

    if (const auto base = notcurses_named_key(record.id)) {
        const auto key = apply_notcurses_modifiers(*base, record.modifiers);
        if (const auto bytes = input_bytes_for_key_id(key)) {
            return key_event(key, *bytes);
        }
    }
#else
    (void)resize_size;
#endif

    if (!record.utf8.empty() && lock_only_modifiers(record.modifiers)) {
        InputEvent event;
        event.kind = InputEventKind::insert;
        event.bytes = record.utf8;
        event.text = record.utf8;
        return event;
    }

    if (const auto key = key_id_from_codepoint(record.id, record.modifiers)) {
        if (const auto bytes = input_bytes_for_key_id(*key)) {
            return key_event(*key, *bytes);
        }
    }

    if (!record.utf8.empty()) {
        InputEvent event;
        event.kind = InputEventKind::insert;
        event.bytes = record.utf8;
        event.text = record.utf8;
        return event;
    }

    return std::nullopt;
}

std::vector<InputEvent> decode_notcurses_inputs(
    const std::vector<NotcursesInputRecord>& records,
    std::optional<TerminalSize> resize_size) {
    std::vector<InputEvent> events;
    events.reserve(records.size());
    for (const auto& record : records) {
        auto event = decode_notcurses_input(record, resize_size);
        if (event) {
            events.push_back(std::move(*event));
        }
    }
    return events;
}

NotcursesBackend::~NotcursesBackend() { static_cast<void>(stop()); }

foundation::Status NotcursesBackend::start(NotcursesBackendOptions options) {
    if (active_) {
        return foundation::Status::success();
    }

    dry_run_ = true;
    dry_run_input_.clear();
    if (!options.enabled) {
        active_ = false;
        context_ = nullptr;
        return foundation::Status::success();
    }
    if (options.dry_run) {
        active_ = true;
        context_ = nullptr;
        return foundation::Status::success();
    }

#if QT_TUI_NOTCURSES_BACKEND_HAS_HEADERS
    if (options.output == nullptr) {
        return foundation::Status::failure(
            foundation::ErrorCode::invalid_argument,
            "notcurses real-context probe requires an explicit output FILE");
    }

    auto native_options = make_options(options);
    auto* context = notcurses_core_init(&native_options, options.output);
    if (context == nullptr) {
        return foundation::Status::failure(foundation::ErrorCode::io_error,
                                           "notcurses core context initialization failed");
    }

    context_ = context;
    dry_run_ = false;
    active_ = true;
    return foundation::Status::success();
#else
    return foundation::Status::failure(
        foundation::ErrorCode::invalid_argument,
        "notcurses backend real-context probe is unavailable without headers");
#endif
}

foundation::Status NotcursesBackend::stop() {
    if (!active_ && context_ == nullptr) {
        return foundation::Status::success();
    }

#if QT_TUI_NOTCURSES_BACKEND_HAS_HEADERS
    if (live_plane_ != nullptr) {
        ncplane_destroy(plane_cast(live_plane_));
        live_plane_ = nullptr;
    }
    if (context_ != nullptr && notcurses_stop(context_cast(context_)) != 0) {
        context_ = nullptr;
        active_ = false;
        dry_run_ = true;
        return foundation::Status::failure(foundation::ErrorCode::io_error,
                                           "notcurses context shutdown failed");
    }
#endif

    context_ = nullptr;
    live_plane_ = nullptr;
    active_ = false;
    dry_run_ = true;
    dry_run_input_.clear();
    return foundation::Status::success();
}

foundation::Status NotcursesBackend::present(const RenderBuffer& buffer) {
    auto plan = notcurses_render_plan(buffer);
    if (!active_ || dry_run_) {
        last_plan_ = std::move(plan);
        return foundation::Status::success();
    }
    last_plan_.reset();

#if QT_TUI_NOTCURSES_BACKEND_HAS_HEADERS
    auto* context = context_cast(context_);
    if (context == nullptr) {
        return foundation::Status::failure(foundation::ErrorCode::internal_error,
                                           "notcurses backend is active without a context");
    }

    auto* plane = notcurses_stdplane(context);
    for (const auto& operation : plan.operations) {
        if (operation.kind == NotcursesRenderOperationKind::erase_plane) {
            ncplane_erase(plane);
            continue;
        }
        if (operation.kind == NotcursesRenderOperationKind::put_text) {
            if (apply_native_style(plane, operation.style) < 0 ||
                ncplane_putstr_yx(plane, static_cast<int>(operation.row),
                                  static_cast<int>(operation.column),
                                  operation.text.c_str()) < 0) {
                return foundation::Status::failure(foundation::ErrorCode::io_error,
                                                   "notcurses plane text write failed");
            }
        }
    }
    if (notcurses_render(context) != 0) {
        return foundation::Status::failure(foundation::ErrorCode::io_error,
                                           "notcurses render failed");
    }
#endif

    return foundation::Status::success();
}

foundation::Status NotcursesBackend::present(const FrameDiff& frame_diff) {
    auto plan = notcurses_render_plan(frame_diff);
    if (!active_ || dry_run_) {
        last_plan_ = std::move(plan);
        return foundation::Status::success();
    }
    last_plan_.reset();

#if QT_TUI_NOTCURSES_BACKEND_HAS_HEADERS
    auto* context = context_cast(context_);
    if (context == nullptr) {
        return foundation::Status::failure(foundation::ErrorCode::internal_error,
                                           "notcurses backend is active without a context");
    }

    auto* plane = notcurses_stdplane(context);
    for (const auto& operation : plan.operations) {
        if (operation.kind == NotcursesRenderOperationKind::erase_plane) {
            ncplane_erase(plane);
            continue;
        }
        if (operation.kind == NotcursesRenderOperationKind::put_text) {
            if (apply_native_style(plane, operation.style) < 0 ||
                ncplane_putstr_yx(plane, static_cast<int>(operation.row),
                                  static_cast<int>(operation.column),
                                  operation.text.c_str()) < 0) {
                return foundation::Status::failure(foundation::ErrorCode::io_error,
                                                   "notcurses plane text write failed");
            }
        }
    }
    if (notcurses_render(context) != 0) {
        return foundation::Status::failure(foundation::ErrorCode::io_error,
                                           "notcurses render failed");
    }
#endif

    return foundation::Status::success();
}

foundation::Status NotcursesBackend::present_at(const RenderBuffer& buffer,
                                                std::size_t row) {
    auto plan = notcurses_render_plan(buffer);
    if (!active_ || dry_run_) {
        last_plan_ = std::move(plan);
        return foundation::Status::success();
    }
    last_plan_.reset();

#if QT_TUI_NOTCURSES_BACKEND_HAS_HEADERS
    auto* context = context_cast(context_);
    if (context == nullptr) {
        return foundation::Status::failure(foundation::ErrorCode::internal_error,
                                           "notcurses backend is active without a context");
    }

    auto* stdplane = notcurses_stdplane(context);
    unsigned rows = 0;
    unsigned columns = 0;
    notcurses_term_dim_yx(context, &rows, &columns);
    const std::size_t height = std::max<std::size_t>(1U, buffer.lines.size());
    const std::size_t width = std::max<std::size_t>(
        1U, buffer.width == 0U ? static_cast<std::size_t>(columns) : buffer.width);
    const std::size_t max_row =
        rows > height ? static_cast<std::size_t>(rows) - height : 0U;
    const int plane_y = static_cast<int>(std::min(row, max_row));
    auto* plane = live_plane_ == nullptr ? nullptr : plane_cast(live_plane_);
    if (plane != nullptr) {
        unsigned current_height = 0;
        unsigned current_width = 0;
        ncplane_dim_yx(plane, &current_height, &current_width);
        if (current_height != static_cast<unsigned>(height) ||
            current_width != static_cast<unsigned>(width)) {
            if (ncplane_resize_simple(plane, static_cast<unsigned>(height),
                                      static_cast<unsigned>(width)) < 0) {
                ncplane_destroy(plane);
                live_plane_ = nullptr;
                plane = nullptr;
            }
        }
        if (plane != nullptr && ncplane_move_yx(plane, plane_y, 0) < 0) {
            return foundation::Status::failure(
                foundation::ErrorCode::io_error,
                "notcurses live region plane move failed");
        }
    }
    if (plane == nullptr) {
        ncplane_options plane_options{};
        plane_options.y = plane_y;
        plane_options.x = 0;
        plane_options.rows = static_cast<unsigned>(height);
        plane_options.cols = static_cast<unsigned>(width);
        plane_options.name = "qt-live-region";
        plane_options.flags = NCPLANE_OPTION_FIXED;
        plane = ncplane_create(stdplane, &plane_options);
        if (plane == nullptr) {
            return foundation::Status::failure(
                foundation::ErrorCode::io_error,
                "notcurses live region plane creation failed");
        }
        live_plane_ = plane;
    }
    ncplane_set_base(plane, " ", 0, 0);
    ncplane_erase(plane);

    for (const auto& operation : plan.operations) {
        if (operation.kind == NotcursesRenderOperationKind::erase_plane) {
            ncplane_erase(plane);
            continue;
        }
        if (operation.kind == NotcursesRenderOperationKind::put_text) {
            if (apply_native_style(plane, operation.style) < 0 ||
                ncplane_putstr_yx(plane, static_cast<int>(operation.row),
                                  static_cast<int>(operation.column),
                                  operation.text.c_str()) < 0) {
                return foundation::Status::failure(
                    foundation::ErrorCode::io_error,
                    "notcurses live region text write failed");
            }
        }
    }
    if (notcurses_render(context) != 0) {
        return foundation::Status::failure(foundation::ErrorCode::io_error,
                                           "notcurses live region render failed");
    }
    return foundation::Status::success();
#else
    return foundation::Status::failure(
        foundation::ErrorCode::invalid_argument,
        "notcurses live region rendering is unavailable without headers");
#endif
}

foundation::Status NotcursesBackend::clear_live_region() {
    if (!active_ || dry_run_) {
        return foundation::Status::success();
    }

#if QT_TUI_NOTCURSES_BACKEND_HAS_HEADERS
    if (live_plane_ != nullptr) {
        ncplane_destroy(plane_cast(live_plane_));
        live_plane_ = nullptr;
        auto* context = context_cast(context_);
        if (context != nullptr && notcurses_render(context) != 0) {
            return foundation::Status::failure(foundation::ErrorCode::io_error,
                                               "notcurses live region clear failed");
        }
    }
    return foundation::Status::success();
#else
    return foundation::Status::success();
#endif
}

foundation::Result<std::vector<InputEvent>> NotcursesBackend::poll_input(
    std::optional<TerminalSize> resize_size, bool blocking) {
    if (!active_) {
        return std::vector<InputEvent>{};
    }

    if (dry_run_) {
        std::vector<NotcursesInputRecord> records;
        records.reserve(dry_run_input_.size());
        while (!dry_run_input_.empty()) {
            records.push_back(std::move(dry_run_input_.front()));
            dry_run_input_.pop_front();
        }
        return decode_notcurses_inputs(records, resize_size);
    }

#if QT_TUI_NOTCURSES_BACKEND_HAS_HEADERS
    auto* context = context_cast(context_);
    if (context == nullptr) {
        return foundation::Result<std::vector<InputEvent>>::failure(
            foundation::ErrorCode::internal_error,
            "notcurses backend is active without a context");
    }

    std::vector<NotcursesInputRecord> records;
    bool saw_resize = false;
    const auto decode_records = [&]() {
        auto effective_resize_size = resize_size;
        if (saw_resize) {
            effective_resize_size =
                current_size().value_or(effective_resize_size.value_or(TerminalSize{}));
            if (effective_resize_size->columns == 0U ||
                effective_resize_size->rows == 0U) {
                effective_resize_size.reset();
            }
        }
        return decode_notcurses_inputs(records, effective_resize_size);
    };
    const auto push_record = [&](std::uint32_t id, const ncinput& input) {
        auto record = input_record_from_native(input);
        if (record.id == NCKEY_RESIZE) {
            saw_resize = true;
        }
        records.push_back(std::move(record));
        return id;
    };

    if (blocking) {
        ncinput input{};
        const auto id = notcurses_get_blocking(context, &input);
        if (id == static_cast<std::uint32_t>(-1)) {
            return foundation::Result<std::vector<InputEvent>>::failure(
                foundation::ErrorCode::io_error, "notcurses input polling failed");
        }
        if (id != 0U) {
            push_record(id, input);
        }
    }

    for (;;) {
        ncinput input{};
        const auto id = notcurses_get_nblock(context, &input);
        if (id == 0U) {
            break;
        }
        if (id == static_cast<std::uint32_t>(-1)) {
            if (!records.empty()) {
                return decode_records();
            }
            return foundation::Result<std::vector<InputEvent>>::failure(
                foundation::ErrorCode::io_error, "notcurses input polling failed");
        }
        push_record(id, input);
    }
    return decode_records();
#else
    return foundation::Result<std::vector<InputEvent>>::failure(
        foundation::ErrorCode::invalid_argument,
        "notcurses input polling is unavailable without headers");
#endif
}

void NotcursesBackend::push_dry_run_input(NotcursesInputRecord record) {
    dry_run_input_.push_back(std::move(record));
}

std::optional<TerminalSize> NotcursesBackend::current_size() const noexcept {
#if QT_TUI_NOTCURSES_BACKEND_HAS_HEADERS
    if (!active_ || dry_run_) {
        return std::nullopt;
    }
    return terminal_size_from_native(context_cast(context_));
#else
    return std::nullopt;
#endif
}

bool NotcursesBackend::active() const noexcept { return active_; }

bool NotcursesBackend::dry_run() const noexcept { return dry_run_; }

const std::optional<NotcursesRenderPlan>& NotcursesBackend::last_plan() const noexcept {
    return last_plan_;
}

NotcursesSessionAdapter::NotcursesSessionAdapter(Terminal& terminal)
    : terminal_(terminal) {}

NotcursesSessionAdapter::~NotcursesSessionAdapter() { static_cast<void>(stop()); }

foundation::Status NotcursesSessionAdapter::start(NotcursesSessionOptions options) {
    if (active_) {
        return foundation::Status::success();
    }

    diagnostics_ = NotcursesSessionDiagnostics{};
    diagnostics_.requested_notcurses = options.prefer_notcurses;
    diagnostics_.capabilities = notcurses_diagnostics();
    diagnostics_.negotiation = negotiate_notcurses_capabilities(
        diagnostics_.capabilities, options.required_capabilities);
    diagnostics_.options = notcurses_options_plan(options.backend);
    diagnostics_.selected_renderer = NotcursesSessionRenderer::ansi;
    diagnostics_.fallback_reason =
        options.prefer_notcurses ? std::string{} : "notcurses was not requested";

    if (options.prefer_notcurses) {
        if (!diagnostics_.negotiation.usable) {
            diagnostics_.fallback_reason = diagnostics_.negotiation.fallback_reason;
        } else {
            options.backend.enabled = true;
            const auto start = backend_.start(options.backend);
            if (start) {
                diagnostics_.selected_renderer = NotcursesSessionRenderer::notcurses;
                diagnostics_.fallback_reason.clear();
                if (const auto size = backend_.current_size()) {
                    terminal_.set_size(*size);
                }
            } else {
                diagnostics_.fallback_reason =
                    "notcurses start failed: " + start.error().message;
                static_cast<void>(backend_.stop());
            }
        }
    }

    active_ = true;
    return foundation::Status::success();
}

foundation::Status NotcursesSessionAdapter::stop() {
    foundation::Status status = foundation::Status::success();
    if (using_notcurses()) {
        status = backend_.stop();
    } else {
        static_cast<void>(backend_.stop());
    }
    active_ = false;
    diagnostics_.selected_renderer = NotcursesSessionRenderer::ansi;
    return status;
}

foundation::Status NotcursesSessionAdapter::present(const RenderBuffer& buffer) {
    if (!active_) {
        auto start_status = start();
        if (!start_status) {
            return start_status;
        }
    }

    if (!using_notcurses()) {
        return present_ansi(buffer);
    }

    if (uses_terminal_scrollback()) {
        return present_ansi(buffer);
    }

    const auto status = backend_.present(buffer);
    if (status) {
        return status;
    }

    diagnostics_.fallback_reason = "notcurses present failed: " + status.error().message;
    diagnostics_.selected_renderer = NotcursesSessionRenderer::ansi;
    static_cast<void>(backend_.stop());
    return present_ansi(buffer);
}

foundation::Status NotcursesSessionAdapter::present(const RenderBuffer& buffer,
                                                    const FrameDiff& frame_diff) {
    if (!active_) {
        auto start_status = start();
        if (!start_status) {
            return start_status;
        }
    }

    if (!using_notcurses() || uses_terminal_scrollback()) {
        return present(buffer);
    }

    const auto status = backend_.present(frame_diff);
    if (status) {
        return status;
    }

    diagnostics_.fallback_reason = "notcurses present failed: " + status.error().message;
    diagnostics_.selected_renderer = NotcursesSessionRenderer::ansi;
    static_cast<void>(backend_.stop());
    return present_ansi(buffer);
}

foundation::Status NotcursesSessionAdapter::present_at(const RenderBuffer& buffer,
                                                       std::size_t row) {
    if (!active_) {
        auto start_status = start();
        if (!start_status) {
            return start_status;
        }
    }

    if (!using_notcurses()) {
        return present_ansi_at(buffer, row);
    }

    if (uses_terminal_scrollback()) {
        const auto reset = terminal_.write(ansi_reset_scrolling_region());
        if (!reset) {
            return reset;
        }
    }
    const auto status = backend_.present_at(buffer, row);
    if (status) {
        return status;
    }

    diagnostics_.fallback_reason =
        "notcurses live region present failed: " + status.error().message;
    diagnostics_.selected_renderer = NotcursesSessionRenderer::ansi;
    static_cast<void>(backend_.stop());
    return present_ansi_at(buffer, row);
}

foundation::Status NotcursesSessionAdapter::append_scrollback_lines(
    const std::vector<std::string>& lines,
    std::size_t fixed_bottom_lines) {
    if (lines.empty()) {
        return foundation::Status::success();
    }
    if (!active_) {
        auto start_status = start();
        if (!start_status) {
            return start_status;
        }
    }

    if (!uses_terminal_scrollback()) {
        for (const auto& line : lines) {
            const auto status = terminal_.write(line + "\r\n");
            if (!status) {
                return status;
            }
        }
        return foundation::Status::success();
    }

    const std::size_t rows = terminal_.size().rows;
    if (rows == 0U || fixed_bottom_lines >= rows) {
        for (const auto& line : lines) {
            const auto status = terminal_.write(line + "\r\n");
            if (!status) {
                return status;
            }
        }
        return foundation::Status::success();
    }

    const std::size_t scroll_top = 1U;
    const std::size_t scroll_bottom = rows - fixed_bottom_lines;
    std::string output;
    output += "\x1b[?25l";
    output += ansi_set_auto_wrap(false);
    output += ansi_set_scrolling_region(scroll_top, scroll_bottom);

    std::size_t next_line = 0U;
    if (!scrollback_region_initialized_) {
        const std::size_t seeded_lines = std::min(lines.size(), scroll_bottom);
        const std::size_t seed_top =
            lines.size() < scroll_bottom ? scroll_bottom - seeded_lines : scroll_top;
        for (; next_line < seeded_lines; ++next_line) {
            output += ansi_move_cursor_1_based(seed_top + next_line, 1U);
            output += ansi_clear_current_line();
            output += lines[next_line];
        }
        scrollback_region_initialized_ = true;
    }

    for (; next_line < lines.size(); ++next_line) {
        output += ansi_move_cursor_1_based(scroll_bottom, 1U);
        output += ansi_clear_current_line();
        output += lines[next_line];
        output += "\n";
    }
    output += ansi_set_auto_wrap(true);
    return terminal_.write(output);
}

foundation::Result<std::vector<InputEvent>> NotcursesSessionAdapter::poll_input(bool blocking) {
    if (!active_) {
        auto start_status = start();
        if (!start_status) {
            return foundation::Result<std::vector<InputEvent>>::failure(
                start_status.error().code, start_status.error().message,
                start_status.error().system_error);
        }
    }

    if (!using_notcurses()) {
        return std::vector<InputEvent>{};
    }

    const auto result = backend_.poll_input(terminal_.size(), blocking);
    if (result) {
        for (const auto& event : result.value()) {
            if (event.kind == InputEventKind::resize && event.resize.has_value()) {
                terminal_.set_size(*event.resize);
            }
        }
        return result;
    }

    diagnostics_.fallback_reason =
        "notcurses input polling failed: " + result.error().message;
    diagnostics_.selected_renderer = NotcursesSessionRenderer::ansi;
    static_cast<void>(backend_.stop());
    return std::vector<InputEvent>{};
}

void NotcursesSessionAdapter::reset() noexcept {
    ansi_renderer_.reset();
    scrollback_region_initialized_ = false;
}

bool NotcursesSessionAdapter::active() const noexcept { return active_; }

bool NotcursesSessionAdapter::using_notcurses() const noexcept {
    return diagnostics_.selected_renderer == NotcursesSessionRenderer::notcurses;
}

bool NotcursesSessionAdapter::uses_terminal_scrollback() const noexcept {
    return (diagnostics_.options.mode == NotcursesBackendMode::cli ||
            diagnostics_.options.mode == NotcursesBackendMode::tui) &&
           !diagnostics_.options.use_alternate_screen;
}

const NotcursesSessionDiagnostics& NotcursesSessionAdapter::diagnostics() const noexcept {
    return diagnostics_;
}

NotcursesBackend& NotcursesSessionAdapter::backend() noexcept { return backend_; }

const NotcursesBackend& NotcursesSessionAdapter::backend() const noexcept { return backend_; }

foundation::Status NotcursesSessionAdapter::present_ansi(const RenderBuffer& buffer) {
    auto diff = ansi_renderer_.present(buffer);
    return terminal_.write_synchronized(synchronized_output_for_diff(diff));
}

foundation::Status NotcursesSessionAdapter::present_ansi_at(
    const RenderBuffer& buffer,
    std::size_t row) {
    auto lines = buffer.lines;
    const auto cursor = extract_cursor_marker(lines, lines.size());
    std::string output;
    output += ansi_reset_scrolling_region();
    for (std::size_t index = 0; index < lines.size(); ++index) {
        output += ansi_move_cursor_1_based(row + index + 1U, 1U);
        output += ansi_clear_current_line();
        output += lines[index];
    }
    if (cursor) {
        output += ansi_move_cursor_1_based(row + cursor->row + 1U, cursor->column + 1U);
        output += "\x1b[?25h";
    } else {
        output += "\x1b[?25l";
    }
    return terminal_.write(output);
}

}  // namespace qt::tui
