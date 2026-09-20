#include "qt/tui/terminal/terminal.hpp"

#include "qt/tui/input/keys.hpp"

#include <md4c.h>
#include <uni_algo/version.h>

#if __has_include(<notcurses/notcurses.h>)
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wc++20-extensions"
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include <notcurses/notcurses.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#define QT_TUI_HAS_NOTCURSES_HEADERS 1
#else
#define QT_TUI_HAS_NOTCURSES_HEADERS 0
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <ostream>
#include <utility>

#if !defined(_WIN32)
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace qt::tui {

namespace {

std::optional<TerminalCapabilities> cached_capabilities;
CellDimensions cached_cell_dimensions;

bool title_has_reserved_control(std::string_view title) {
    for (const char ch : title) {
        if (ch == '\a' || ch == '\x1b') {
            return true;
        }
    }
    return false;
}

void append_optional_size_param(std::string& output, char name,
                                const std::optional<std::size_t>& value) {
    if (!value) {
        return;
    }

    output.push_back(',');
    output.push_back(name);
    output.push_back('=');
    output += std::to_string(*value);
}

void append_optional_image_id_param(std::string& output,
                                    const std::optional<std::uint32_t>& value) {
    if (!value) {
        return;
    }

    output += ",i=";
    output += std::to_string(*value);
}

std::string lower_env(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return {};
    }

    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lowered;
}

bool has_env(const char* name) { return std::getenv(name) != nullptr; }

std::optional<std::size_t> positive_env_size(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }

    char* end = nullptr;
    const auto parsed = std::strtoull(value, &end, 10);
    if (end == value || *end != '\0' || parsed == 0) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(parsed);
}

std::optional<TerminalSize> query_terminal_size_nonblocking() noexcept {
#if !defined(_WIN32) && defined(TIOCGWINSZ)
    winsize size{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 &&
        size.ws_col > 0 && size.ws_row > 0) {
        return TerminalSize{static_cast<std::size_t>(size.ws_col),
                            static_cast<std::size_t>(size.ws_row)};
    }
#endif

    const auto columns = positive_env_size("COLUMNS");
    const auto rows = positive_env_size("LINES");
    if (columns && rows) {
        return TerminalSize{*columns, *rows};
    }
    return std::nullopt;
}

bool starts_with(std::string_view value, std::string_view prefix) noexcept {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::optional<std::vector<std::uint8_t>> decode_base64(std::string_view input) {
    std::array<int, 256> table{};
    table.fill(-1);
    for (int i = 0; i < 26; ++i) {
        table[static_cast<std::size_t>('A' + i)] = i;
        table[static_cast<std::size_t>('a' + i)] = 26 + i;
    }
    for (int i = 0; i < 10; ++i) {
        table[static_cast<std::size_t>('0' + i)] = 52 + i;
    }
    table[static_cast<std::size_t>('+')] = 62;
    table[static_cast<std::size_t>('/')] = 63;

    std::vector<std::uint8_t> output;
    int value = 0;
    int bits = -8;
    bool saw_padding = false;
    for (const unsigned char ch : input) {
        if (std::isspace(ch)) {
            continue;
        }
        if (ch == '=') {
            saw_padding = true;
            continue;
        }
        if (saw_padding) {
            return std::nullopt;
        }
        const int decoded = table[ch];
        if (decoded < 0) {
            return std::nullopt;
        }
        value = (value << 6) + decoded;
        bits += 6;
        if (bits >= 0) {
            output.push_back(static_cast<std::uint8_t>((value >> bits) & 0xff));
            bits -= 8;
        }
    }
    return output;
}

std::uint16_t read_u16_be(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset]) << 8U) |
                                      bytes[offset + 1]);
}

std::uint16_t read_u16_le(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset] |
                                      (static_cast<std::uint16_t>(bytes[offset + 1]) << 8U));
}

std::uint32_t read_u24_le(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U);
}

std::uint32_t read_u32_be(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::uint32_t read_u32_le(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

bool bytes_equal(const std::vector<std::uint8_t>& bytes, std::size_t offset,
                 std::string_view expected) {
    if (offset + expected.size() > bytes.size()) {
        return false;
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (bytes[offset + i] != static_cast<std::uint8_t>(expected[i])) {
            return false;
        }
    }
    return true;
}

std::optional<ImageDimensions> parse_png_dimensions(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 24 || !bytes_equal(bytes, 0, "\x89PNG")) {
        return std::nullopt;
    }
    return ImageDimensions{read_u32_be(bytes, 16), read_u32_be(bytes, 20)};
}

std::optional<ImageDimensions> parse_gif_dimensions(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 10 ||
        (!bytes_equal(bytes, 0, "GIF87a") && !bytes_equal(bytes, 0, "GIF89a"))) {
        return std::nullopt;
    }
    return ImageDimensions{read_u16_le(bytes, 6), read_u16_le(bytes, 8)};
}

std::optional<ImageDimensions> parse_jpeg_dimensions(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 2 || bytes[0] != 0xff || bytes[1] != 0xd8) {
        return std::nullopt;
    }

    std::size_t offset = 2;
    while (offset < bytes.size() - 9) {
        if (bytes[offset] != 0xff) {
            ++offset;
            continue;
        }

        const std::uint8_t marker = bytes[offset + 1];
        if (marker >= 0xc0 && marker <= 0xc2) {
            return ImageDimensions{read_u16_be(bytes, offset + 7), read_u16_be(bytes, offset + 5)};
        }
        if (offset + 3 >= bytes.size()) {
            return std::nullopt;
        }
        const auto length = read_u16_be(bytes, offset + 2);
        if (length < 2) {
            return std::nullopt;
        }
        offset += 2U + length;
    }
    return std::nullopt;
}

std::optional<ImageDimensions> parse_webp_dimensions(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 30 || !bytes_equal(bytes, 0, "RIFF") || !bytes_equal(bytes, 8, "WEBP")) {
        return std::nullopt;
    }
    if (bytes_equal(bytes, 12, "VP8 ")) {
        return ImageDimensions{read_u16_le(bytes, 26) & 0x3fffU, read_u16_le(bytes, 28) & 0x3fffU};
    }
    if (bytes_equal(bytes, 12, "VP8L")) {
        if (bytes.size() < 25) {
            return std::nullopt;
        }
        const auto bits = read_u32_le(bytes, 21);
        return ImageDimensions{(bits & 0x3fffU) + 1U, ((bits >> 14U) & 0x3fffU) + 1U};
    }
    if (bytes_equal(bytes, 12, "VP8X")) {
        return ImageDimensions{read_u24_le(bytes, 24) + 1U, read_u24_le(bytes, 27) + 1U};
    }
    return std::nullopt;
}

}  // namespace

foundation::Status BufferTerminalSink::write(std::string_view bytes) {
    writes_.emplace_back(bytes);
    return foundation::Status::success();
}

std::string BufferTerminalSink::joined_output() const {
    std::string output;
    for (const auto& chunk : writes_) {
        output += chunk;
    }
    return output;
}

void BufferTerminalSink::clear() { writes_.clear(); }

PosixTerminalLifecycleAdapter::~PosixTerminalLifecycleAdapter() {
    const auto restore = restore_cooked_mode();
    (void)restore;
}

bool PosixTerminalLifecycleAdapter::stdin_is_tty() const noexcept {
#if !defined(_WIN32)
    last_stdin_tty_ = ::isatty(STDIN_FILENO) != 0;
#else
    last_stdin_tty_ = false;
#endif
    return last_stdin_tty_;
}

bool PosixTerminalLifecycleAdapter::stdout_is_tty() const noexcept {
#if !defined(_WIN32)
    last_stdout_tty_ = ::isatty(STDOUT_FILENO) != 0;
#else
    last_stdout_tty_ = false;
#endif
    return last_stdout_tty_;
}

foundation::Result<TerminalSize> PosixTerminalLifecycleAdapter::query_size() {
    if (!stdout_is_tty()) {
        last_size_.reset();
        return foundation::Result<TerminalSize>::failure(
            foundation::ErrorCode::io_error,
            "terminal size is unavailable because stdout is not a tty");
    }

    const auto size = query_terminal_size_nonblocking();
    if (size) {
        last_size_ = *size;
        return *size;
    }
    last_size_.reset();
    return foundation::Result<TerminalSize>::failure(
        foundation::ErrorCode::io_error,
        "terminal size is unavailable from ioctl or environment");
}

foundation::Status PosixTerminalLifecycleAdapter::enter_raw_mode() {
    if (!stdin_is_tty()) {
        return foundation::Status::success();
    }

#if !defined(_WIN32)
    if (!has_previous_termios_) {
        if (::tcgetattr(STDIN_FILENO, &previous_termios_) != 0) {
            return foundation::Status::failure(
                foundation::ErrorCode::io_error,
                "failed to read terminal mode before entering raw mode");
        }
    }

    termios raw = previous_termios_;
    raw.c_iflag &= static_cast<tcflag_t>(~(BRKINT | ICRNL | INPCK | ISTRIP | IXON));
    raw.c_oflag &= static_cast<tcflag_t>(~(OPOST));
    raw.c_cflag |= CS8;
    raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | IEXTEN | ISIG));
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        return foundation::Status::failure(foundation::ErrorCode::io_error,
                                           "failed to enter terminal raw mode");
    }
    has_previous_termios_ = true;
    active_ = true;
    return foundation::Status::success();
#else
    return foundation::Status::failure(foundation::ErrorCode::io_error,
                                       "terminal raw mode is not supported on this platform");
#endif
}

foundation::Status PosixTerminalLifecycleAdapter::restore_cooked_mode() noexcept {
    if (!active_) {
        return foundation::Status::success();
    }

#if !defined(_WIN32)
    if (has_previous_termios_ &&
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &previous_termios_) != 0) {
        active_ = false;
        cleanup_diagnostics_.push_back("failed to restore terminal cooked mode");
        return foundation::Status::failure(foundation::ErrorCode::io_error,
                                           cleanup_diagnostics_.back());
    }
    active_ = false;
    has_previous_termios_ = false;
    return foundation::Status::success();
#else
    return foundation::Status::success();
#endif
}

TerminalLifecycleDiagnostics PosixTerminalLifecycleAdapter::diagnostics() const {
    return TerminalLifecycleDiagnostics{true, last_stdin_tty_,
                                        last_stdout_tty_, last_size_, cleanup_diagnostics_};
}

OstreamTerminalSink::OstreamTerminalSink(std::ostream& output)
    : output_(&output) {}

foundation::Status OstreamTerminalSink::write(std::string_view bytes) {
    if (output_ == nullptr) {
        return foundation::Status::failure(foundation::ErrorCode::invalid_argument,
                                           "terminal output stream is not available");
    }

    output_->write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output_->flush();
    if (!*output_) {
        return foundation::Status::failure(foundation::ErrorCode::io_error,
                                           "failed to write terminal output");
    }
    return foundation::Status::success();
}

std::string ansi_clear_screen() {
    return "\x1b[2J\x1b[H";
}

std::string ansi_set_cursor_visible(bool visible) {
    return visible ? "\x1b[?25h" : "\x1b[?25l";
}

std::string ansi_begin_synchronized_output() {
    return "\x1b[?2026h";
}

std::string ansi_end_synchronized_output() {
    return "\x1b[?2026l";
}

std::string ansi_set_bracketed_paste(bool enabled) {
    return enabled ? "\x1b[?2004h" : "\x1b[?2004l";
}

std::string ansi_query_kitty_keyboard_protocol() {
    return "\x1b[?u";
}

std::string ansi_set_kitty_keyboard_protocol(bool enabled) {
    return enabled ? "\x1b[>7u" : "\x1b[<u";
}

std::string ansi_set_modify_other_keys(bool enabled) {
    return enabled ? "\x1b[>4;2m" : "\x1b[>4;0m";
}

std::string ansi_query_cell_size() {
    return "\x1b[16t";
}

std::string ansi_set_terminal_progress(bool active) {
    return active ? "\x1b]9;4;3\x07" : "\x1b]9;4;0;\x07";
}

foundation::Result<std::string> ansi_set_title(std::string_view title) {
    if (title_has_reserved_control(title)) {
        return foundation::Result<std::string>::failure(
            foundation::ErrorCode::invalid_argument,
            "terminal title must not contain escape or bell characters");
    }

    return std::string("\x1b]0;") + std::string(title) + '\a';
}

std::string ansi_kitty_inline_image(std::string_view base64_data,
                                    const InlineImageOptions& options) {
    constexpr std::size_t chunk_size = 4096;

    std::string params = "a=T,f=100,q=2";
    if (!options.move_cursor) {
        params += ",C=1";
    }
    append_optional_size_param(params, 'c', options.columns);
    append_optional_size_param(params, 'r', options.rows);
    append_optional_image_id_param(params, options.image_id);

    auto sequence_for_chunk = [](std::string_view chunk, std::string_view chunk_params) {
        std::string sequence = "\x1b_G";
        sequence += chunk_params;
        sequence.push_back(';');
        sequence.append(chunk.data(), chunk.size());
        sequence += "\x1b\\";
        return sequence;
    };

    if (base64_data.size() <= chunk_size) {
        return sequence_for_chunk(base64_data, params);
    }

    std::string output;
    std::size_t offset = 0;
    bool first = true;
    while (offset < base64_data.size()) {
        const std::size_t size = std::min(chunk_size, base64_data.size() - offset);
        const bool last = offset + size >= base64_data.size();
        std::string chunk_params;
        if (first) {
            chunk_params = params + ",m=1";
            first = false;
        } else if (last) {
            chunk_params = "m=0";
        } else {
            chunk_params = "m=1";
        }

        output += sequence_for_chunk(base64_data.substr(offset, size), chunk_params);
        offset += size;
    }

    return output;
}

std::string ansi_kitty_delete_image(std::uint32_t image_id) {
    return "\x1b_Ga=d,d=I,i=" + std::to_string(image_id) + ",q=2\x1b\\";
}

std::string ansi_kitty_delete_all_images() {
    return "\x1b_Ga=d,d=A,q=2\x1b\\";
}

std::string ansi_iterm2_inline_image(std::string_view base64_data,
                                     const InlineImageOptions& options) {
    std::string sequence = "\x1b]1337;File=inline=";
    sequence += options.inline_image ? '1' : '0';

    if (options.columns) {
        sequence += ";width=";
        sequence += std::to_string(*options.columns);
    }
    if (options.rows) {
        sequence += ";height=";
        sequence += std::to_string(*options.rows);
    } else {
        sequence += ";height=auto";
    }
    if (!options.preserve_aspect_ratio) {
        sequence += ";preserveAspectRatio=0";
    }

    sequence.push_back(':');
    sequence.append(base64_data.data(), base64_data.size());
    sequence.push_back('\a');
    return sequence;
}

bool contains_inline_image_sequence(std::string_view line) noexcept {
    return line.find("\x1b_G") != std::string_view::npos ||
           line.find("\x1b]1337;File=") != std::string_view::npos;
}

TerminalCapabilities detect_terminal_capabilities() {
    const auto term_program = lower_env("TERM_PROGRAM");
    const auto term = lower_env("TERM");
    const auto color_term = lower_env("COLORTERM");
    const bool true_color = color_term == "truecolor" || color_term == "24bit";

    if (has_env("TMUX") || starts_with(term, "tmux") || starts_with(term, "screen")) {
        return TerminalCapabilities{ImageProtocol::none, true_color, false};
    }
    if (has_env("KITTY_WINDOW_ID") || term_program == "kitty") {
        return TerminalCapabilities{ImageProtocol::kitty, true, true};
    }
    if (term_program == "ghostty" || term.find("ghostty") != std::string::npos ||
        has_env("GHOSTTY_RESOURCES_DIR")) {
        return TerminalCapabilities{ImageProtocol::kitty, true, true};
    }
    if (has_env("WEZTERM_PANE") || term_program == "wezterm") {
        return TerminalCapabilities{ImageProtocol::kitty, true, true};
    }
    if (has_env("ITERM_SESSION_ID") || term_program == "iterm.app") {
        return TerminalCapabilities{ImageProtocol::iterm2, true, true};
    }
    if (term_program == "vscode" || term_program == "alacritty") {
        return TerminalCapabilities{ImageProtocol::none, true, true};
    }
    return TerminalCapabilities{ImageProtocol::none, true_color, false};
}

bool detect_notcurses_auto_start_terminal() noexcept {
    const auto term_program = lower_env("TERM_PROGRAM");
    const auto term = lower_env("TERM");

    if (has_env("TMUX") || starts_with(term, "tmux") || starts_with(term, "screen")) {
        return false;
    }
    if (has_env("KITTY_WINDOW_ID") || term_program == "kitty" ||
        starts_with(term, "xterm-kitty")) {
        return true;
    }
    if (term_program == "ghostty" || term.find("ghostty") != std::string::npos ||
        has_env("GHOSTTY_RESOURCES_DIR")) {
        return true;
    }
    if (has_env("WEZTERM_PANE") || term_program == "wezterm" ||
        term.find("wezterm") != std::string::npos) {
        return true;
    }
    if (has_env("ITERM_SESSION_ID") || term_program == "iterm.app") {
        return true;
    }
    if (term_program == "vscode" || term_program == "alacritty") {
        return true;
    }
    if (has_env("WT_SESSION") || has_env("KONSOLE_VERSION") || has_env("VTE_VERSION")) {
        return true;
    }
    return false;
}

TerminalCapabilities terminal_capabilities() {
    if (!cached_capabilities) {
        cached_capabilities = detect_terminal_capabilities();
    }
    return *cached_capabilities;
}

void set_terminal_capabilities(TerminalCapabilities capabilities) {
    cached_capabilities = capabilities;
}

void reset_terminal_capabilities_cache() { cached_capabilities.reset(); }

NativeTuiBackends native_tui_backends() noexcept {
    NativeTuiBackends backends;
    backends.uni_algo = una::version::library.major() >= 0;
    backends.md4c = true;
    backends.notcurses_headers = QT_TUI_HAS_NOTCURSES_HEADERS != 0;
#if QT_TUI_HAS_NOTCURSES_HEADERS
    backends.notcurses_linked = notcurses_version() != nullptr;
#endif
    return backends;
}

CellDimensions terminal_cell_dimensions() noexcept { return cached_cell_dimensions; }

void set_terminal_cell_dimensions(CellDimensions dimensions) noexcept {
    cached_cell_dimensions = dimensions;
}

std::size_t calculate_image_rows(ImageDimensions image_dimensions, std::size_t target_width_cells,
                                 CellDimensions cell_dimensions) {
    if (image_dimensions.width_px == 0 || image_dimensions.height_px == 0 ||
        cell_dimensions.width_px == 0 || cell_dimensions.height_px == 0 ||
        target_width_cells == 0) {
        return 1;
    }

    const auto target_width_px = target_width_cells * cell_dimensions.width_px;
    const auto numerator = image_dimensions.height_px * target_width_px;
    const auto denominator = image_dimensions.width_px * cell_dimensions.height_px;
    return std::max<std::size_t>(1, (numerator + denominator - 1U) / denominator);
}

std::optional<ImageDimensions> parse_image_dimensions(std::string_view base64_data,
                                                      std::string_view mime_type) {
    const auto bytes = decode_base64(base64_data);
    if (!bytes) {
        return std::nullopt;
    }
    if (mime_type == "image/png") {
        return parse_png_dimensions(*bytes);
    }
    if (mime_type == "image/jpeg") {
        return parse_jpeg_dimensions(*bytes);
    }
    if (mime_type == "image/gif") {
        return parse_gif_dimensions(*bytes);
    }
    if (mime_type == "image/webp") {
        return parse_webp_dimensions(*bytes);
    }
    return std::nullopt;
}

std::optional<RenderedTerminalImage> render_terminal_image(
    std::string_view base64_data, ImageDimensions image_dimensions,
    const TerminalImageRenderOptions& options) {
    const auto capabilities = terminal_capabilities();
    if (capabilities.images == ImageProtocol::none) {
        return std::nullopt;
    }

    const auto rows =
        calculate_image_rows(image_dimensions, options.max_width_cells, terminal_cell_dimensions());
    InlineImageOptions inline_options;
    inline_options.columns = options.max_width_cells;
    inline_options.rows = rows;
    inline_options.image_id = options.image_id;
    inline_options.move_cursor = options.move_cursor;
    inline_options.preserve_aspect_ratio = options.preserve_aspect_ratio;

    if (capabilities.images == ImageProtocol::kitty) {
        return RenderedTerminalImage{ansi_kitty_inline_image(base64_data, inline_options), rows,
                                     options.image_id};
    }
    if (capabilities.images == ImageProtocol::iterm2) {
        inline_options.rows.reset();
        return RenderedTerminalImage{ansi_iterm2_inline_image(base64_data, inline_options), rows,
                                     std::nullopt};
    }
    return std::nullopt;
}

std::string terminal_hyperlink(std::string_view text, std::string_view url) {
    std::string output = "\x1b]8;;";
    output += url;
    output += "\x1b\\";
    output += text;
    output += "\x1b]8;;\x1b\\";
    return output;
}

std::string terminal_image_fallback(std::string_view mime_type,
                                    std::optional<ImageDimensions> dimensions,
                                    std::string_view filename) {
    std::string output = "[Image:";
    if (!filename.empty()) {
        output.push_back(' ');
        output += filename;
    }
    output += " [";
    output += mime_type;
    output.push_back(']');
    if (dimensions) {
        output.push_back(' ');
        output += std::to_string(dimensions->width_px);
        output.push_back('x');
        output += std::to_string(dimensions->height_px);
    }
    output.push_back(']');
    return output;
}

SynchronizedOutput::SynchronizedOutput(std::string_view bytes) { write(bytes); }

void SynchronizedOutput::write(std::string_view bytes) { bytes_ += bytes; }

void SynchronizedOutput::clear() { bytes_.clear(); }

bool SynchronizedOutput::empty() const noexcept { return bytes_.empty(); }

const std::string& SynchronizedOutput::bytes() const noexcept { return bytes_; }

Terminal::Terminal(TerminalSink& sink, TerminalState state)
    : sink_(sink), state_(std::move(state)) {}

const TerminalState& Terminal::state() const noexcept { return state_; }

TerminalSize Terminal::size() const noexcept { return state_.size; }

void Terminal::set_size(TerminalSize size) noexcept { state_.size = size; }

foundation::Status Terminal::write(std::string_view text) { return sink_.write(text); }

foundation::Status Terminal::write_synchronized(std::string_view text) {
    std::string output = ansi_begin_synchronized_output();
    output += text;
    output += ansi_end_synchronized_output();
    return sink_.write(output);
}

foundation::Status Terminal::write_synchronized(const SynchronizedOutput& output) {
    return write_synchronized(output.bytes());
}

foundation::Status Terminal::clear_screen() { return sink_.write(ansi_clear_screen()); }

foundation::Status Terminal::set_cursor_visible(bool visible) {
    const auto status = sink_.write(ansi_set_cursor_visible(visible));
    if (status) {
        state_.cursor_visible = visible;
    }
    return status;
}

foundation::Status Terminal::set_bracketed_paste(bool enabled) {
    const auto status = sink_.write(ansi_set_bracketed_paste(enabled));
    if (status) {
        state_.bracketed_paste = enabled;
    }
    return status;
}

foundation::Status Terminal::query_kitty_keyboard_protocol() {
    return sink_.write(ansi_query_kitty_keyboard_protocol());
}

foundation::Status Terminal::set_kitty_keyboard_protocol(bool enabled) {
    const auto status = sink_.write(ansi_set_kitty_keyboard_protocol(enabled));
    if (status) {
        state_.kitty_keyboard = enabled;
        set_kitty_protocol_active(enabled);
    }
    return status;
}

foundation::Status Terminal::set_modify_other_keys(bool enabled) {
    const auto status = sink_.write(ansi_set_modify_other_keys(enabled));
    if (status) {
        state_.modify_other_keys = enabled;
    }
    return status;
}

foundation::Status Terminal::query_cell_size() {
    return sink_.write(ansi_query_cell_size());
}

foundation::Status Terminal::set_progress(bool active) {
    const auto status = sink_.write(ansi_set_terminal_progress(active));
    if (status) {
        state_.progress_active = active;
    }
    return status;
}

foundation::Status Terminal::set_title(std::string title) {
    auto sequence = ansi_set_title(title);
    if (!sequence) {
        return foundation::Status(sequence.error());
    }

    const auto status = sink_.write(sequence.value());
    if (status) {
        state_.title = std::move(title);
    }
    return status;
}

TerminalSessionLifecycle::TerminalSessionLifecycle(Terminal& terminal,
                                                   TerminalSessionOptions options)
    : terminal_(terminal), options_(options) {}

TerminalSessionLifecycle::TerminalSessionLifecycle(Terminal& terminal,
                                                   TerminalLifecycleAdapter& adapter,
                                                   TerminalSessionOptions options)
    : terminal_(terminal), adapter_(&adapter), options_(options) {}

foundation::Status TerminalSessionLifecycle::start() {
    if (active_) {
        return foundation::Status::success();
    }

    previous_state_ = terminal_.state();
    if (adapter_ != nullptr) {
        const auto raw_mode = adapter_->enter_raw_mode();
        if (!raw_mode) {
            return raw_mode;
        }
        adapter_active_ = true;

        const auto size = adapter_->query_size();
        if (size) {
            terminal_.set_size(size.value());
        }
    }

    const auto restore_adapter_on_error = [this](const foundation::Status& status) {
        if (adapter_active_ && adapter_ != nullptr) {
            const auto restore = adapter_->restore_cooked_mode();
            (void)restore;
            adapter_active_ = false;
        }
        return status;
    };

    if (options_.bracketed_paste) {
        const auto status = terminal_.set_bracketed_paste(true);
        if (!status) {
            return restore_adapter_on_error(status);
        }
    }
    if (options_.hide_cursor) {
        const auto status = terminal_.set_cursor_visible(false);
        if (!status) {
            return restore_adapter_on_error(status);
        }
    }
    if (options_.query_kitty_keyboard) {
        const auto status = terminal_.query_kitty_keyboard_protocol();
        if (!status) {
            return restore_adapter_on_error(status);
        }
        if (options_.enable_modify_other_keys) {
            modify_other_keys_fallback_deadline_ =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
        }
    } else if (options_.enable_modify_other_keys) {
        const auto status = terminal_.set_modify_other_keys(true);
        if (!status) {
            return restore_adapter_on_error(status);
        }
    }
    if (options_.query_cell_size_for_images &&
        terminal_capabilities().images != ImageProtocol::none) {
        const auto status = terminal_.query_cell_size();
        if (!status) {
            return restore_adapter_on_error(status);
        }
    }

    active_ = true;
    return foundation::Status::success();
}

foundation::Status TerminalSessionLifecycle::poll_keyboard_protocol() {
    if (!active_ || !modify_other_keys_fallback_deadline_) {
        return foundation::Status::success();
    }
    if (terminal_.state().kitty_keyboard || terminal_.state().modify_other_keys) {
        modify_other_keys_fallback_deadline_.reset();
        return foundation::Status::success();
    }
    if (std::chrono::steady_clock::now() < *modify_other_keys_fallback_deadline_) {
        return foundation::Status::success();
    }

    modify_other_keys_fallback_deadline_.reset();
    return terminal_.set_modify_other_keys(true);
}

foundation::Status TerminalSessionLifecycle::stop() {
    if (!active_) {
        return foundation::Status::success();
    }

    foundation::Status first_error = foundation::Status::success();
    const auto capture_error = [&first_error](const foundation::Status& status) {
        if (!status && first_error) {
            first_error = status;
        }
    };

    if (terminal_.state().progress_active != previous_state_.progress_active) {
        capture_error(terminal_.set_progress(previous_state_.progress_active));
    }
    if (terminal_.state().kitty_keyboard != previous_state_.kitty_keyboard) {
        capture_error(terminal_.set_kitty_keyboard_protocol(previous_state_.kitty_keyboard));
    }
    if (terminal_.state().modify_other_keys != previous_state_.modify_other_keys) {
        capture_error(terminal_.set_modify_other_keys(previous_state_.modify_other_keys));
    }
    if (terminal_.state().bracketed_paste != previous_state_.bracketed_paste) {
        capture_error(terminal_.set_bracketed_paste(previous_state_.bracketed_paste));
    }
    if (terminal_.state().cursor_visible != previous_state_.cursor_visible) {
        capture_error(terminal_.set_cursor_visible(previous_state_.cursor_visible));
    }
    if (adapter_active_ && adapter_ != nullptr) {
        capture_error(adapter_->restore_cooked_mode());
        adapter_active_ = false;
    }

    active_ = false;
    modify_other_keys_fallback_deadline_.reset();
    return first_error;
}

bool TerminalSessionLifecycle::active() const noexcept { return active_; }

}  // namespace qt::tui
