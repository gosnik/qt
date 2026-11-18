#pragma once

#include "qt/foundation/error.hpp"
#include "qt/tui/core/component.hpp"
#include "qt/tui/terminal/diff_renderer.hpp"
#include "qt/tui/input/input_decoder.hpp"
#include "qt/tui/terminal/terminal.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace qt::tui {

struct NotcursesVersion {
    std::string text;
    int major = 0;
    int minor = 0;
    int patch = 0;
    int tweak = 0;
};

struct NotcursesDiagnostics {
    bool headers_available = false;
    bool linked = false;
    bool core_init_available = false;
    bool plane_render_available = false;
    bool input_available = false;
    std::optional<NotcursesVersion> version;
};

struct NotcursesCapabilityRequest {
    bool render = true;
    bool input = true;
};

struct NotcursesCapabilityNegotiation {
    NotcursesCapabilityRequest requested;
    bool render_available = false;
    bool input_available = false;
    bool usable = false;
    std::string fallback_reason;
};

enum class NotcursesBackendMode {
    cli,
    tui,
};

struct NotcursesBackendOptions {
    bool enabled = false;
    bool dry_run = true;
    bool suppress_banners = true;
    NotcursesBackendMode mode = NotcursesBackendMode::cli;
    bool use_alternate_screen = false;
    bool install_signal_handlers = false;
    bool drain_input = true;
    const char* termtype = nullptr;
    std::FILE* output = nullptr;
};

struct NotcursesOptionsPlan {
    bool suppress_banners = true;
    NotcursesBackendMode mode = NotcursesBackendMode::cli;
    bool use_alternate_screen = false;
    bool install_signal_handlers = false;
    bool drain_input = true;
    std::string termtype;
    std::uint64_t flags = 0;
};

enum class NotcursesRenderOperationKind {
    erase_plane,
    put_text,
    render,
};

struct NotcursesRenderOperation {
    NotcursesRenderOperationKind kind = NotcursesRenderOperationKind::render;
    std::size_t row = 0;
    std::size_t column = 0;
    std::string text;
    struct Style {
        bool bold = false;
        bool italic = false;
        bool underline = false;
        bool struck = false;

        struct Color {
            enum class Kind {
                default_color,
                palette,
                rgb,
            };

            Kind kind = Kind::default_color;
            std::uint8_t palette_index = 0;
            std::uint32_t rgb = 0;
        };

        Color foreground;
        Color background;
    };
    Style style;
};

struct NotcursesRenderPlan {
    std::size_t width = 0;
    std::size_t height = 0;
    std::vector<NotcursesRenderOperation> operations;
};

struct NotcursesInputRecord {
    std::uint32_t id = 0;
    std::string utf8;
    int y = -1;
    int x = -1;
    unsigned modifiers = 0;
    bool release = false;
};

enum class NotcursesSessionRenderer {
    ansi,
    notcurses,
};

struct NotcursesSessionOptions {
    bool prefer_notcurses = false;
    NotcursesCapabilityRequest required_capabilities;
    NotcursesBackendOptions backend;
};

struct NotcursesSessionDiagnostics {
    bool requested_notcurses = false;
    NotcursesSessionRenderer selected_renderer = NotcursesSessionRenderer::ansi;
    std::string fallback_reason;
    NotcursesDiagnostics capabilities;
    NotcursesCapabilityNegotiation negotiation;
    NotcursesOptionsPlan options;
};

[[nodiscard]] NotcursesDiagnostics notcurses_diagnostics() noexcept;
[[nodiscard]] NotcursesCapabilityNegotiation negotiate_notcurses_capabilities(
    const NotcursesDiagnostics& diagnostics,
    NotcursesCapabilityRequest request = {});
[[nodiscard]] std::uint32_t notcurses_resize_key_id() noexcept;
[[nodiscard]] NotcursesOptionsPlan notcurses_options_plan(
    const NotcursesBackendOptions& options) noexcept;
[[nodiscard]] NotcursesRenderPlan notcurses_render_plan(const RenderBuffer& buffer);
[[nodiscard]] NotcursesRenderPlan notcurses_render_plan(const FrameDiff& frame_diff);
[[nodiscard]] std::optional<InputEvent> decode_notcurses_input(
    const NotcursesInputRecord& record, std::optional<TerminalSize> resize_size = {});
[[nodiscard]] std::vector<InputEvent> decode_notcurses_inputs(
    const std::vector<NotcursesInputRecord>& records,
    std::optional<TerminalSize> resize_size = {});

class NotcursesBackend {
  public:
    NotcursesBackend() = default;
    NotcursesBackend(const NotcursesBackend&) = delete;
    NotcursesBackend& operator=(const NotcursesBackend&) = delete;
    ~NotcursesBackend();

    [[nodiscard]] foundation::Status start(NotcursesBackendOptions options = {});
    [[nodiscard]] foundation::Status stop();
    [[nodiscard]] foundation::Status present(const RenderBuffer& buffer);
    [[nodiscard]] foundation::Status present(const FrameDiff& frame_diff);
    [[nodiscard]] foundation::Status present_at(const RenderBuffer& buffer,
                                                std::size_t row);
    [[nodiscard]] foundation::Status clear_live_region();
    [[nodiscard]] foundation::Result<std::vector<InputEvent>> poll_input(
        std::optional<TerminalSize> resize_size = {}, bool blocking = false);
    [[nodiscard]] std::optional<TerminalSize> current_size() const noexcept;
    void push_dry_run_input(NotcursesInputRecord record);

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool dry_run() const noexcept;
    [[nodiscard]] const std::optional<NotcursesRenderPlan>& last_plan() const noexcept;

  private:
    void* context_ = nullptr;
    void* live_plane_ = nullptr;
    bool active_ = false;
    bool dry_run_ = true;
    std::optional<NotcursesRenderPlan> last_plan_;
    std::deque<NotcursesInputRecord> dry_run_input_;
};

class NotcursesSessionAdapter {
  public:
    explicit NotcursesSessionAdapter(Terminal& terminal);
    NotcursesSessionAdapter(const NotcursesSessionAdapter&) = delete;
    NotcursesSessionAdapter& operator=(const NotcursesSessionAdapter&) = delete;
    ~NotcursesSessionAdapter();

    [[nodiscard]] foundation::Status start(NotcursesSessionOptions options = {});
    [[nodiscard]] foundation::Status stop();
    [[nodiscard]] foundation::Status present(const RenderBuffer& buffer);
    [[nodiscard]] foundation::Status present(const RenderBuffer& buffer,
                                             const FrameDiff& frame_diff);
    [[nodiscard]] foundation::Status present_at(const RenderBuffer& buffer,
                                                std::size_t row);
    [[nodiscard]] foundation::Status append_scrollback_lines(
        const std::vector<std::string>& lines,
        std::size_t fixed_bottom_lines = 0);
    [[nodiscard]] foundation::Result<std::vector<InputEvent>> poll_input(
        bool blocking = false);
    void reset() noexcept;
    void reset_scrollback_region() noexcept;

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool using_notcurses() const noexcept;
    [[nodiscard]] bool uses_terminal_scrollback() const noexcept;
    [[nodiscard]] const NotcursesSessionDiagnostics& diagnostics() const noexcept;
    [[nodiscard]] NotcursesBackend& backend() noexcept;
    [[nodiscard]] const NotcursesBackend& backend() const noexcept;

  private:
    [[nodiscard]] foundation::Status present_ansi(const RenderBuffer& buffer);
    [[nodiscard]] foundation::Status present_ansi_at(const RenderBuffer& buffer,
                                                     std::size_t row);

    Terminal& terminal_;
    NotcursesBackend backend_;
    DiffRenderer ansi_renderer_;
    NotcursesSessionDiagnostics diagnostics_;
    bool active_ = false;
    bool scrollback_region_initialized_ = false;
};

}  // namespace qt::tui
