#pragma once

#include "qt/app/interactive/interactive_shell.hpp"
#include "qt/foundation/error.hpp"
#include "qt/tui/widgets/autocomplete.hpp"
#include "qt/tui/core/component.hpp"
#include "qt/tui/terminal/diff_renderer.hpp"
#include "qt/tui/input/editor.hpp"
#include "qt/tui/input/input_decoder.hpp"
#include "qt/tui/widgets/overlay.hpp"
#include "qt/tui/widgets/selectors/select.hpp"
#include "qt/tui/terminal/terminal.hpp"
#include "qt/tui/terminal/render_metrics.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace qt::tui {
class NotcursesSessionAdapter;
}

namespace qt::app {

class InteractiveTerminalSession;

struct InteractiveSlashCommandResponse {
  bool handled = false;
  bool append_default_output = false;
  std::vector<std::string> transcript_lines;
};

enum class InteractivePendingMessageKind {
  kSteering,
  kFollowUp,
};

struct InteractivePendingMessage {
  InteractivePendingMessageKind kind = InteractivePendingMessageKind::kSteering;
  std::string text;
};

struct InteractiveTerminalSessionOptions {
  using FooterProvider = std::function<std::vector<std::string>()>;
  using HelpProvider = std::function<std::vector<std::string>()>;
  using PendingMessagesProvider =
      std::function<std::vector<InteractivePendingMessage>()>;
  using PromptHistoryChangedHandler =
      std::function<foundation::Status(const std::vector<std::string>&)>;
  using PendingMessagesDequeueHandler =
      std::function<bool(InteractiveTerminalSession&)>;
  using AppActionHandler = std::function<bool(InteractiveTerminalSession&)>;
  using ExtensionTextHandler = std::function<void(std::string)>;
  using ExtensionBoolHandler = std::function<void(bool)>;
  using ExtensionTerminalInputHandler =
      std::function<foundation::Result<std::optional<std::string>>(
          std::string_view)>;
  using ExtensionEditorInputHandler =
      std::function<foundation::Result<bool>(std::string_view)>;
  using ExtensionAutocompleteProvider =
      std::function<std::optional<qt::tui::AutocompleteSuggestions>(
          const std::vector<std::string>&, std::size_t, std::size_t, bool)>;
  using PasteImagePathProvider = std::function<std::optional<std::string>()>;
  using SlashCommandExecutor = std::function<foundation::Result<InteractiveSlashCommandResponse>(
      InteractiveTerminalSession&, const InteractiveShellEvent&)>;

  qt::tui::RenderConstraints constraints{80, 24};
  std::string input_prompt = "qt> ";
  bool clear_on_start = true;
  bool hide_cursor = true;
  FooterProvider footer_provider;
  HelpProvider help_provider;
  PendingMessagesProvider pending_messages_provider;
  PromptHistoryChangedHandler prompt_history_changed_handler;
  PendingMessagesDequeueHandler pending_messages_dequeue_handler;
  AppActionHandler interrupt_handler;
  AppActionHandler clear_handler;
  AppActionHandler exit_handler;
  PasteImagePathProvider paste_image_path_provider;
  std::shared_ptr<qt::tui::CombinedAutocompleteProvider> autocomplete_provider;
  SlashCommandExecutor slash_command_executor;
  qt::tui::NotcursesSessionAdapter* session_adapter = nullptr;
  AppActionHandler suspend_handler;
  AppActionHandler external_editor_handler;
};

struct InteractiveTerminalStep {
  std::vector<InteractiveShellEvent> events;
  qt::tui::FrameDiff frame_diff;
};

class InteractiveTerminalSession {
 public:
  InteractiveTerminalSession(InteractiveShellRuntime& shell, qt::tui::Terminal& terminal,
                             InteractiveTerminalSessionOptions options = {});

  [[nodiscard]] const qt::tui::Editor& editor() const noexcept;
  [[nodiscard]] const std::vector<std::string>& transcript_lines() const noexcept;
  [[nodiscard]] qt::tui::Editor& mutable_editor() noexcept;
  [[nodiscard]] const std::optional<std::string>& status_message() const noexcept;

  void set_autocomplete_provider(
      std::shared_ptr<qt::tui::CombinedAutocompleteProvider> provider);
  void set_extension_terminal_input_handler(
      InteractiveTerminalSessionOptions::ExtensionTerminalInputHandler handler);
  void set_extension_editor_component(qt::tui::ComponentPtr component);
  void set_extension_editor_input_handler(
      InteractiveTerminalSessionOptions::ExtensionEditorInputHandler handler);
  void set_extension_autocomplete_provider(
      InteractiveTerminalSessionOptions::ExtensionAutocompleteProvider provider);
  [[nodiscard]] const std::optional<foundation::Error>&
  extension_editor_input_error() const noexcept;
  void set_status_message(std::optional<std::string> message);
  void clear_status_message();
  void append_transcript_line(std::string line);
  void append_transcript_lines(std::vector<std::string> lines);
  void append_transcript_literal_lines(std::vector<std::string> lines);
  void append_transcript_component(qt::tui::ComponentPtr component);
  void finalize_transcript_components();
  void set_tools_expanded(bool expanded);
  [[nodiscard]] bool tools_expanded() const noexcept { return tools_expanded_; }
  void add_extension_autocomplete_provider(
      InteractiveTerminalSessionOptions::ExtensionAutocompleteProvider provider);
  void append_transcript_tool_call(std::string tool_name, std::string call_id,
                                   std::string arguments);
  void append_transcript_tool_result(std::string tool_name, std::string call_id,
                                     std::string status, std::string output,
                                     bool is_error = false);
  void append_transcript_tool_result_component(std::string tool_name,
                                               std::string call_id,
                                               std::string arguments,
                                               qt::tui::ComponentPtr component);
  void set_tool_entry_compaction_enabled_tools(
      std::vector<std::string> tool_names);
  [[nodiscard]] std::size_t compact_transcript_tool_entries(
      std::size_t keep_recent_entries = 48,
      std::size_t max_payload_chars = 240);
  void set_transcript_lines(std::vector<std::string> lines);
  void clear_transcript_lines();
  qt::tui::OverlayHandle show_overlay(
      qt::tui::ComponentPtr component,
      qt::tui::OverlayOptions options = qt::tui::OverlayOptions{});
  void hide_overlay();
  void clear_overlays();
  [[nodiscard]] bool has_overlay() const;
  qt::tui::OverlayHandle show_extension_select(
      std::string title, std::vector<std::string> options,
      InteractiveTerminalSessionOptions::ExtensionTextHandler on_select,
      InteractiveTerminalSessionOptions::AppActionHandler on_cancel = {});
  qt::tui::OverlayHandle show_extension_confirm(
      std::string title, std::string message,
      InteractiveTerminalSessionOptions::ExtensionBoolHandler on_confirm,
      InteractiveTerminalSessionOptions::AppActionHandler on_cancel = {});
  qt::tui::OverlayHandle show_extension_input(
      std::string title, std::string placeholder,
      InteractiveTerminalSessionOptions::ExtensionTextHandler on_submit,
      InteractiveTerminalSessionOptions::AppActionHandler on_cancel = {});
  qt::tui::OverlayHandle show_extension_editor(
      std::string title, std::string prefill,
      InteractiveTerminalSessionOptions::ExtensionTextHandler on_submit,
      InteractiveTerminalSessionOptions::AppActionHandler on_cancel = {});
  qt::tui::OverlayHandle show_extension_custom(
      qt::tui::ComponentPtr component,
      qt::tui::OverlayOptions options = qt::tui::OverlayOptions{});
  void show_extension_notify(std::string message, std::string_view type = {});
  void set_extension_widgets(
      std::map<std::string, std::vector<std::string>> widgets);
  void set_extension_header(std::optional<std::vector<std::string>> lines);
  void set_theme_name(std::string name);
  void set_theme_accent(std::string ansi_prefix);
  [[nodiscard]] const std::string& theme_name() const noexcept;

  [[nodiscard]] foundation::Status start();
  [[nodiscard]] foundation::Result<std::vector<InteractiveTerminalStep>> handle_input(
      std::string_view bytes);
  [[nodiscard]] foundation::Status apply_terminal_resize(
      qt::tui::TerminalSize size);
  [[nodiscard]] bool has_pending_input() const noexcept;
  [[nodiscard]] foundation::Result<std::vector<InteractiveTerminalStep>> run_script(
      const std::vector<std::string>& lines);
  [[nodiscard]] foundation::Result<InteractiveTerminalStep> submit_line(
      std::string_view line);
  [[nodiscard]] foundation::Result<InteractiveTerminalStep> render();
  [[nodiscard]] foundation::Result<qt::tui::RenderBuffer> base_render_buffer(
      const qt::tui::RenderConstraints& constraints) const;
  [[nodiscard]] qt::tui::RenderMetricsSnapshot render_metrics() const noexcept;
  void reset_render_metrics() noexcept;

 private:
  struct TranscriptEntry {
    enum class Kind {
      kLine,
      kLiteralLine,
      kComponent,
      kToolCall,
      kToolResult,
    };

    Kind kind = Kind::kLine;
    std::string line;
    std::string tool_name;
    std::string tool_call_id;
    std::string tool_payload;
    std::string tool_status;
    bool tool_error = false;
    bool compacted = false;
    bool live_component = false;
    qt::tui::ComponentPtr component;
  };

  struct BottomRenderCache {
    qt::tui::RenderConstraints constraints;
    std::vector<std::string> lines;
  };

  struct TranscriptRenderCacheEntry {
    bool valid = false;
    std::size_t width = 0;
    TranscriptEntry::Kind kind = TranscriptEntry::Kind::kLine;
    std::string line;
    std::vector<std::string> rendered_lines;
  };

  void invalidate_render_caches() const;
  void clear_transcript_render_cache() const;
  [[nodiscard]] static TranscriptEntry line_transcript_entry(std::string line);
  [[nodiscard]] static std::string tool_call_display_line(
      const TranscriptEntry& entry);
  [[nodiscard]] static std::string tool_result_display_line(
      const TranscriptEntry& entry);
  [[nodiscard]] static std::string compacted_tool_display_line(
      const TranscriptEntry& entry);
  [[nodiscard]] std::vector<std::string> footer_lines() const;
  [[nodiscard]] std::vector<std::string> transcript_render_lines(
      const qt::tui::RenderConstraints& constraints) const;
  [[nodiscard]] std::vector<std::string> transcript_render_lines(
      std::size_t begin, std::size_t end,
      const qt::tui::RenderConstraints& constraints) const;
  [[nodiscard]] std::vector<std::string> transcript_entry_render_lines(
      const TranscriptEntry& entry,
      const qt::tui::RenderConstraints& constraints) const;
  [[nodiscard]] const std::vector<std::string>& cached_transcript_entry_render_lines(
      std::size_t index, const qt::tui::RenderConstraints& constraints) const;
  [[nodiscard]] std::size_t transcript_rendered_line_count(
      const qt::tui::RenderConstraints& constraints) const;
  [[nodiscard]] std::vector<std::string> transcript_viewport_lines(
      const qt::tui::RenderConstraints& constraints,
      std::size_t viewport_height) const;
  [[nodiscard]] std::size_t first_trailing_live_component_index() const noexcept;
  [[nodiscard]] std::size_t trailing_live_component_line_count(
      const qt::tui::RenderConstraints& constraints) const;
  [[nodiscard]] std::vector<std::string> pending_message_lines() const;
  [[nodiscard]] std::vector<std::string> editor_render_lines(
      const qt::tui::RenderConstraints& constraints) const;
  [[nodiscard]] const std::vector<std::string>& bottom_render_lines_ref(
      const qt::tui::RenderConstraints& constraints) const;
  [[nodiscard]] std::vector<std::string> bottom_render_lines(
      const qt::tui::RenderConstraints& constraints) const;
  [[nodiscard]] std::size_t bottom_reserved_line_count(
      const qt::tui::RenderConstraints& constraints) const;
  [[nodiscard]] std::size_t scrollback_live_region_height(
      const qt::tui::RenderConstraints& constraints) const;
  void reserve_overlay_bottom_margin(qt::tui::OverlayOptions& options) const;
  [[nodiscard]] std::vector<std::string> layout_lines(
      std::vector<std::string> transcript, std::vector<std::string> bottom,
      const qt::tui::RenderConstraints& constraints) const;
  [[nodiscard]] std::optional<qt::tui::AutocompleteSuggestions>
  autocomplete_suggestions(bool force) const;
  [[nodiscard]] bool autocomplete_debounce_due() const;
  [[nodiscard]] bool autocomplete_attachment_context() const;
  void mark_autocomplete_edit();
  void refresh_debounced_autocomplete();
  void refresh_completed_recursive_autocomplete();
  void refresh_autocomplete(bool force = false);
  void show_autocomplete_overlay();
  void hide_autocomplete();
  bool handle_autocomplete_event(const qt::tui::InputEvent& event);
  void move_autocomplete_selection(int delta);
  bool apply_selected_autocomplete();
  [[nodiscard]] bool should_submit_selected_autocomplete() const;
  [[nodiscard]] bool autocomplete_active() const noexcept;
  bool scroll_transcript(int delta_lines);
  void append_events(const std::vector<InteractiveShellEvent>& events);
  void apply_resize(qt::tui::TerminalSize size);
  [[nodiscard]] foundation::Status flush_transcript_scrollback();
  [[nodiscard]] foundation::Status present(const qt::tui::RenderBuffer& buffer,
                                           const qt::tui::FrameDiff& frame_diff);

  InteractiveShellRuntime& shell_;
  qt::tui::Terminal& terminal_;
  InteractiveTerminalSessionOptions options_;
  std::shared_ptr<qt::tui::Editor> editor_;
  qt::tui::ComponentPtr extension_editor_component_;
  InteractiveTerminalSessionOptions::ExtensionTerminalInputHandler
      extension_terminal_input_handler_;
  InteractiveTerminalSessionOptions::ExtensionEditorInputHandler
      extension_editor_input_handler_;
  InteractiveTerminalSessionOptions::ExtensionAutocompleteProvider
      extension_autocomplete_provider_;
  std::optional<foundation::Error> extension_editor_input_error_;
  qt::tui::InputDecoder input_decoder_;
  qt::tui::DiffRenderer renderer_;
  qt::tui::ComponentPtr surface_component_;
  qt::tui::OverlaySurface overlay_surface_;
  qt::tui::AutocompleteState autocomplete_state_;
  std::shared_ptr<qt::tui::Select> autocomplete_select_;
  std::optional<qt::tui::OverlayHandle> autocomplete_overlay_;
  std::chrono::steady_clock::time_point last_autocomplete_edit_{};
  bool debounced_autocomplete_pending_ = false;
  std::uint64_t autocomplete_generation_ = 0;
  std::size_t autocomplete_request_row_ = 0;
  std::size_t autocomplete_request_column_ = 0;
  bool recursive_autocomplete_pending_ = false;
  bool clear_before_next_present_ = false;
  bool overlay_invalidation_rendering_ = false;
  std::optional<std::string> status_message_;
  std::vector<std::string> pending_submissions_;
  std::vector<std::string> transcript_lines_;
  std::map<std::string, std::vector<std::string>> extension_widgets_;
  std::optional<std::vector<std::string>> extension_header_;
  std::string theme_name_ = "dark";
  std::string theme_accent_ = "\x1b[36m";
  std::vector<TranscriptEntry> transcript_entries_;
  bool tools_expanded_ = false;
  std::unordered_set<std::string> tool_entry_compaction_enabled_tools_;
  bool tool_entry_compaction_tools_configured_ = false;
  std::size_t transcript_scroll_offset_ = 0;
  std::size_t transcript_scrollback_offset_ = 0;
  std::size_t finalized_transcript_component_frontier_ = 0;
  std::size_t tool_entry_compaction_frontier_ = 0;
  std::size_t presented_scrollback_live_region_height_ = 0;
  std::string pending_prompt_;
  mutable qt::tui::RenderMetricsRecorder render_metrics_;
  mutable std::optional<BottomRenderCache> bottom_render_cache_;
  mutable std::vector<TranscriptRenderCacheEntry> transcript_render_cache_;
};

}  // namespace qt::app
