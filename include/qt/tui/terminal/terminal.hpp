#pragma once

#include "qt/foundation/error.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <termios.h>
#endif

namespace qt::tui {

struct TerminalSize {
    std::size_t columns = 0;
    std::size_t rows = 0;
};

struct TerminalState {
    TerminalSize size;
    bool cursor_visible = true;
    std::string title;
    bool bracketed_paste = false;
    bool kitty_keyboard = false;
    bool modify_other_keys = false;
    bool progress_active = false;
};

enum class ImageProtocol {
    none,
    kitty,
    iterm2,
};

struct TerminalCapabilities {
    ImageProtocol images = ImageProtocol::none;
    bool true_color = false;
    bool hyperlinks = false;
};

inline constexpr std::string_view terminal_line_reset_sequence = "\x1b[0m\x1b]8;;\x07";

struct NativeTuiBackends {
    bool uni_algo = false;
    bool md4c = false;
    bool notcurses_headers = false;
    bool notcurses_linked = false;
};

struct CellDimensions {
    std::size_t width_px = 9;
    std::size_t height_px = 18;
};

struct ImageDimensions {
    std::size_t width_px = 0;
    std::size_t height_px = 0;
};

struct InlineImageOptions {
    std::optional<std::size_t> columns;
    std::optional<std::size_t> rows;
    std::optional<std::uint32_t> image_id;
    bool move_cursor = true;
    bool preserve_aspect_ratio = true;
    bool inline_image = true;
};

struct TerminalImageRenderOptions {
    std::size_t max_width_cells = 80;
    std::optional<std::uint32_t> image_id;
    bool move_cursor = true;
    bool preserve_aspect_ratio = true;
};

struct RenderedTerminalImage {
    std::string sequence;
    std::size_t rows = 0;
    std::optional<std::uint32_t> image_id;
};

struct TerminalLifecycleDiagnostics {
    bool terminal_lifecycle_available = false;
    bool stdin_tty = false;
    bool stdout_tty = false;
    std::optional<TerminalSize> size;
    std::vector<std::string> cleanup_diagnostics;
};

class TerminalLifecycleAdapter {
  public:
    virtual ~TerminalLifecycleAdapter() = default;

    [[nodiscard]] virtual bool stdin_is_tty() const noexcept = 0;
    [[nodiscard]] virtual bool stdout_is_tty() const noexcept = 0;
    [[nodiscard]] virtual foundation::Result<TerminalSize> query_size() = 0;
    [[nodiscard]] virtual foundation::Status enter_raw_mode() = 0;
    [[nodiscard]] virtual foundation::Status restore_cooked_mode() noexcept = 0;
    [[nodiscard]] virtual TerminalLifecycleDiagnostics diagnostics() const = 0;
};

class PosixTerminalLifecycleAdapter final : public TerminalLifecycleAdapter {
  public:
    ~PosixTerminalLifecycleAdapter() override;

    [[nodiscard]] bool stdin_is_tty() const noexcept override;
    [[nodiscard]] bool stdout_is_tty() const noexcept override;
    [[nodiscard]] foundation::Result<TerminalSize> query_size() override;
    [[nodiscard]] foundation::Status enter_raw_mode() override;
    [[nodiscard]] foundation::Status restore_cooked_mode() noexcept override;
    [[nodiscard]] TerminalLifecycleDiagnostics diagnostics() const override;

  private:
    bool active_ = false;
    mutable bool last_stdin_tty_ = false;
    mutable bool last_stdout_tty_ = false;
    std::optional<TerminalSize> last_size_;
    std::vector<std::string> cleanup_diagnostics_;
#if !defined(_WIN32)
    termios previous_termios_{};
    bool has_previous_termios_ = false;
#endif
};

class TerminalSink {
  public:
    virtual ~TerminalSink() = default;

    [[nodiscard]] virtual foundation::Status write(std::string_view bytes) = 0;
};

class BufferTerminalSink final : public TerminalSink {
  public:
    [[nodiscard]] foundation::Status write(std::string_view bytes) override;

    [[nodiscard]] const std::vector<std::string>& writes() const noexcept { return writes_; }
    [[nodiscard]] std::string joined_output() const;
    void clear();

  private:
    std::vector<std::string> writes_;
};

class OstreamTerminalSink final : public TerminalSink {
  public:
    explicit OstreamTerminalSink(std::ostream& output);

    [[nodiscard]] foundation::Status write(std::string_view bytes) override;

  private:
    std::ostream* output_ = nullptr;
};

[[nodiscard]] std::string ansi_clear_screen();
[[nodiscard]] std::string ansi_set_cursor_visible(bool visible);
[[nodiscard]] std::string ansi_begin_synchronized_output();
[[nodiscard]] std::string ansi_end_synchronized_output();
[[nodiscard]] std::string ansi_set_bracketed_paste(bool enabled);
[[nodiscard]] std::string ansi_query_kitty_keyboard_protocol();
[[nodiscard]] std::string ansi_set_kitty_keyboard_protocol(bool enabled);
[[nodiscard]] std::string ansi_set_modify_other_keys(bool enabled);
[[nodiscard]] std::string ansi_query_cell_size();
[[nodiscard]] std::string ansi_set_terminal_progress(bool active);
foundation::Result<std::string> ansi_set_title(std::string_view title);
[[nodiscard]] std::string ansi_kitty_inline_image(std::string_view base64_data,
                                                  const InlineImageOptions& options = {});
[[nodiscard]] std::string ansi_kitty_delete_image(std::uint32_t image_id);
[[nodiscard]] std::string ansi_kitty_delete_all_images();
[[nodiscard]] std::string ansi_iterm2_inline_image(std::string_view base64_data,
                                                   const InlineImageOptions& options = {});
[[nodiscard]] bool contains_inline_image_sequence(std::string_view line) noexcept;
[[nodiscard]] TerminalCapabilities detect_terminal_capabilities();
[[nodiscard]] bool detect_notcurses_auto_start_terminal() noexcept;
[[nodiscard]] TerminalCapabilities terminal_capabilities();
void set_terminal_capabilities(TerminalCapabilities capabilities);
void reset_terminal_capabilities_cache();
[[nodiscard]] NativeTuiBackends native_tui_backends() noexcept;
[[nodiscard]] CellDimensions terminal_cell_dimensions() noexcept;
void set_terminal_cell_dimensions(CellDimensions dimensions) noexcept;
[[nodiscard]] std::size_t calculate_image_rows(ImageDimensions image_dimensions,
                                                std::size_t target_width_cells,
                                                CellDimensions cell_dimensions = {});
[[nodiscard]] std::optional<ImageDimensions> parse_image_dimensions(
    std::string_view base64_data, std::string_view mime_type);
[[nodiscard]] std::optional<RenderedTerminalImage> render_terminal_image(
    std::string_view base64_data, ImageDimensions image_dimensions,
    const TerminalImageRenderOptions& options = {});
[[nodiscard]] std::string terminal_hyperlink(std::string_view text, std::string_view url);
[[nodiscard]] std::string terminal_image_fallback(std::string_view mime_type,
                                                  std::optional<ImageDimensions> dimensions = {},
                                                  std::string_view filename = {});

class SynchronizedOutput {
  public:
    SynchronizedOutput() = default;
    explicit SynchronizedOutput(std::string_view bytes);

    void write(std::string_view bytes);
    void clear();

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] const std::string& bytes() const noexcept;

  private:
    std::string bytes_;
};

class Terminal {
  public:
    explicit Terminal(TerminalSink& sink, TerminalState state = {});

    [[nodiscard]] const TerminalState& state() const noexcept;
    [[nodiscard]] TerminalSize size() const noexcept;

    void set_size(TerminalSize size) noexcept;

    [[nodiscard]] foundation::Status write(std::string_view text);
    [[nodiscard]] foundation::Status write_synchronized(std::string_view text);
    [[nodiscard]] foundation::Status write_synchronized(const SynchronizedOutput& output);
    [[nodiscard]] foundation::Status clear_screen();
    [[nodiscard]] foundation::Status set_cursor_visible(bool visible);
    [[nodiscard]] foundation::Status set_bracketed_paste(bool enabled);
    [[nodiscard]] foundation::Status query_kitty_keyboard_protocol();
    [[nodiscard]] foundation::Status set_kitty_keyboard_protocol(bool enabled);
    [[nodiscard]] foundation::Status set_modify_other_keys(bool enabled);
    [[nodiscard]] foundation::Status query_cell_size();
    [[nodiscard]] foundation::Status set_progress(bool active);
    [[nodiscard]] foundation::Status set_title(std::string title);

  private:
    TerminalSink& sink_;
    TerminalState state_;
};

struct TerminalSessionOptions {
    bool bracketed_paste = true;
    bool hide_cursor = true;
    bool query_kitty_keyboard = true;
    bool enable_modify_other_keys = true;
    bool query_cell_size_for_images = true;
};

class TerminalSessionLifecycle {
  public:
    explicit TerminalSessionLifecycle(Terminal& terminal,
                                      TerminalSessionOptions options = {});
    TerminalSessionLifecycle(Terminal& terminal, TerminalLifecycleAdapter& adapter,
                             TerminalSessionOptions options = {});
    TerminalSessionLifecycle(const TerminalSessionLifecycle&) = delete;
    TerminalSessionLifecycle& operator=(const TerminalSessionLifecycle&) = delete;

    [[nodiscard]] foundation::Status start();
    [[nodiscard]] foundation::Status poll_keyboard_protocol();
    [[nodiscard]] foundation::Status stop();
    [[nodiscard]] bool active() const noexcept;

  private:
    Terminal& terminal_;
    TerminalLifecycleAdapter* adapter_ = nullptr;
    TerminalSessionOptions options_;
    TerminalState previous_state_;
    std::optional<std::chrono::steady_clock::time_point>
        modify_other_keys_fallback_deadline_;
    bool active_ = false;
    bool adapter_active_ = false;
};

}  // namespace qt::tui
