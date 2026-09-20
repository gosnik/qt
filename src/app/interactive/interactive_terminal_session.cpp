#include "qt/app/interactive/interactive_terminal_session.hpp"

#include "qt/foundation/error.hpp"
#include "qt/tui/core/component.hpp"
#include "qt/tui/widgets/extension/extension_editor.hpp"
#include "qt/tui/widgets/extension/extension_input.hpp"
#include "qt/tui/widgets/extension/extension_selector.hpp"
#include "qt/tui/widgets/messages/tool_execution.hpp"
#include "qt/tui/input/keybindings.hpp"
#include "qt/tui/terminal/notcurses_backend.hpp"
#include "qt/tui/core/style.hpp"
#include "qt/tui/utils/line_split.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qt::app {
namespace {

using LineStyler = std::string (*)(std::string_view);
constexpr auto kAutocompleteAttachmentDebounce =
    std::chrono::milliseconds(75);

qt::tui::OverlayOptions extension_dialog_overlay_options() {
  qt::tui::OverlayOptions options;
  options.anchor = qt::tui::OverlayAnchor::bottom_center;
  options.width = qt::tui::OverlaySize::percent(100);
  options.margin = qt::tui::OverlayMargin::all(1);
  return options;
}

void apply_menu_overlay_defaults(qt::tui::OverlayOptions& options) {
  if (options.non_capturing) {
    return;
  }
  if (!options.width && !options.max_height && !options.row && !options.col &&
      options.anchor == qt::tui::OverlayAnchor::center) {
    options.anchor = qt::tui::OverlayAnchor::bottom_center;
    options.max_height = qt::tui::OverlaySize::percent(70);
    options.margin.top = std::max(options.margin.top, 1);
    options.margin.right = std::max(options.margin.right, 1);
    options.margin.bottom = std::max(options.margin.bottom, 1);
    options.margin.left = std::max(options.margin.left, 1);
  }
}

std::vector<std::string> split_lines(std::string_view text) {
  return qt::tui::split_lines_copy(text);
}

std::string first_display_line(std::string_view text) {
  const auto lines = qt::tui::split_lines_view(text);
  return lines.empty() ? std::string{} : std::string(lines.front());
}

bool ansi_styled_line_is_blank(std::string_view text) {
  for (std::size_t offset = 0; offset < text.size();) {
    if (text[offset] == '\x1b' && offset + 1 < text.size()) {
      if (text[offset + 1] == '[') {
        offset += 2;
        while (offset < text.size()) {
          const char ch = text[offset++];
          if (ch >= '@' && ch <= '~') {
            break;
          }
        }
        continue;
      }
      if (text[offset + 1] == '_' || text[offset + 1] == ']') {
        offset += 2;
        while (offset < text.size()) {
          if (text[offset] == '\a') {
            ++offset;
            break;
          }
          if (text[offset] == '\x1b' && offset + 1 < text.size() &&
              text[offset + 1] == '\\') {
            offset += 2;
            break;
          }
          ++offset;
        }
        continue;
      }
    }
    if (!std::isspace(static_cast<unsigned char>(text[offset]))) {
      return false;
    }
    ++offset;
  }
  return true;
}

bool is_space_only(std::string_view text) {
  return std::all_of(text.begin(), text.end(),
                     [](unsigned char ch) { return std::isspace(ch) != 0; });
}

bool uses_notcurses_live_region(
    const qt::tui::NotcursesSessionAdapter* adapter) {
  return adapter != nullptr && adapter->uses_terminal_scrollback() &&
         adapter->using_notcurses();
}

void trim_trailing_spaces(std::string &text) {
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.pop_back();
  }
}

std::size_t display_width_or_size(std::string_view text) {
  const auto width = qt::tui::measure_line_width(text);
  return width ? width.value() : text.size();
}

std::vector<std::string_view> display_tokens(std::string_view text) {
  std::vector<std::string_view> tokens;
  std::size_t start = 0;
  while (start < text.size()) {
    const bool token_is_space =
        std::isspace(static_cast<unsigned char>(text[start])) != 0;
    std::size_t end = start + 1U;
    while (end < text.size() &&
           (std::isspace(static_cast<unsigned char>(text[end])) != 0) ==
               token_is_space) {
      ++end;
    }
    tokens.push_back(text.substr(start, end - start));
    start = end;
  }
  return tokens;
}

std::vector<std::string> split_display_token(std::string_view token,
                                             std::size_t width) {
  std::vector<std::string> chunks;
  std::string remaining(token);
  while (!remaining.empty()) {
    auto chunk = qt::tui::truncate_line(remaining, width);
    if (!chunk || chunk.value().empty()) {
      chunks.push_back(std::move(remaining));
      break;
    }
    remaining.erase(0, chunk.value().size());
    chunks.push_back(std::move(chunk.value()));
  }
  return chunks.empty() ? std::vector<std::string>{""} : chunks;
}

std::vector<std::string> wrap_display_line(std::string_view line,
                                           std::size_t width) {
  if (width == 0) {
    return {std::string(line)};
  }
  if (line.empty()) {
    return {""};
  }

  std::vector<std::string> wrapped;
  std::string current;
  std::size_t current_width = 0;

  for (const auto token : display_tokens(line)) {
    const bool token_is_space = is_space_only(token);
    const std::size_t token_width = display_width_or_size(token);

    if (token_width > width && !token_is_space) {
      if (!current.empty()) {
        trim_trailing_spaces(current);
        if (!current.empty()) {
          wrapped.push_back(std::move(current));
        }
        current.clear();
      }

      auto chunks = split_display_token(token, width);
      if (chunks.size() > 1U) {
        wrapped.insert(wrapped.end(), std::make_move_iterator(chunks.begin()),
                       std::make_move_iterator(chunks.end() - 1));
      }
      current = std::move(chunks.back());
      current_width = display_width_or_size(current);
      continue;
    }

    if (token_is_space) {
      if (current.empty()) {
        continue;
      }
      if (current_width + token_width > width) {
        trim_trailing_spaces(current);
        if (!current.empty()) {
          wrapped.push_back(std::move(current));
        }
        current.clear();
        current_width = 0;
      } else {
        current.append(token);
        current_width += token_width;
      }
      continue;
    }

    if (!current.empty() && current_width + token_width > width) {
      trim_trailing_spaces(current);
      if (!current.empty()) {
        wrapped.push_back(std::move(current));
      }
      current.clear();
      current_width = 0;
    }

    current.append(token);
    current_width += token_width;
  }

  trim_trailing_spaces(current);
  if (!current.empty()) {
    wrapped.push_back(std::move(current));
  }
  return wrapped.empty() ? std::vector<std::string>{""} : wrapped;
}

std::vector<std::string> wrap_literal_display_line(std::string_view line,
                                                   std::size_t width) {
  if (width == 0 || line.empty()) {
    return {std::string(line)};
  }

  std::vector<std::string> wrapped;
  std::string remaining(line);
  while (!remaining.empty()) {
    auto chunk = qt::tui::truncate_line(remaining, width);
    if (!chunk || chunk.value().empty()) {
      wrapped.push_back(std::move(remaining));
      break;
    }
    remaining.erase(0, chunk.value().size());
    wrapped.push_back(std::move(chunk.value()));
  }
  return wrapped.empty() ? std::vector<std::string>{""} : wrapped;
}

std::string plain_line(std::string_view text) { return std::string(text); }

bool starts_with(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

LineStyler pending_line_styler(std::string_view line) {
  if (line.empty()) {
    return plain_line;
  }
  if (starts_with(line, "-> ")) {
    return qt::tui::style::dim;
  }
  return qt::tui::style::cyan;
}

std::string llama_header_line(std::string_view line) {
  return qt::tui::style::bold(qt::tui::style::foreground_256(117, line));
}

std::string llama_detail_line(std::string_view line) {
  return qt::tui::style::foreground_256(250, line);
}

std::string llama_muted_line(std::string_view line) {
  return qt::tui::style::foreground_256(244, line);
}

std::string llama_tool_call_line(std::string_view line) {
  return qt::tui::style::background_256(
      236, qt::tui::style::foreground_256(250, line));
}

std::string llama_tool_result_line(std::string_view line) {
  return qt::tui::style::background_256(
      236, qt::tui::style::foreground_256(152, line));
}

LineStyler transcript_content_styler(std::string_view line) {
  if (starts_with(line, "error:") || starts_with(line, "Error:") ||
      starts_with(line, "warning:") || starts_with(line, "Warning:")) {
    return qt::tui::style::red;
  }
  if (starts_with(line, "Llama ") || starts_with(line, "llama provider:") ||
      starts_with(line, "instance:") || starts_with(line, "instances:")) {
    return llama_header_line;
  }
  if (starts_with(line, "tool_call:")) {
    return llama_tool_call_line;
  }
  if (starts_with(line, "tool_result:")) {
    return llama_tool_result_line;
  }
  if (starts_with(line, "tools_available:") ||
      starts_with(line, "input_tokens:") || starts_with(line, "dimensions:") ||
      starts_with(line, "preview:") || starts_with(line, "kv_state:") ||
      starts_with(line, "- embed:") || starts_with(line, "- rank:") ||
      starts_with(line, "- instruct:") || starts_with(line, "- chat:")) {
    return llama_muted_line;
  }
  if (starts_with(line, "selected:") || starts_with(line, "top_level_model:") ||
      starts_with(line, "loaded:") || starts_with(line, "context:") ||
      starts_with(line, "policy:") || starts_with(line, "kv_cache:") ||
      starts_with(line, "gpu:") || starts_with(line, "grammar:") ||
      starts_with(line, "embeddings:") || starts_with(line, "runtime:") ||
      starts_with(line, "memory:") || starts_with(line, "backend:") ||
      starts_with(line, "gpu_memory:") || starts_with(line, "device_memory:")) {
    return llama_detail_line;
  }
  if (starts_with(line, "[quit]")) {
    return qt::tui::style::dim;
  }
  return plain_line;
}

template <typename StyleForLine>
std::vector<std::string> wrap_styled_display_lines(
    const std::vector<std::string> &lines, std::size_t width,
    StyleForLine style_for_line) {
  std::vector<std::string> styled_lines;
  for (const auto &line : lines) {
    for (const auto split : qt::tui::split_lines_view(line)) {
      const auto styler = style_for_line(split);
      auto wrapped = wrap_display_line(split, width);
      for (auto &wrapped_line : wrapped) {
        if (!wrapped_line.empty()) {
          wrapped_line = styler(wrapped_line);
        }
        styled_lines.push_back(std::move(wrapped_line));
      }
    }
  }
  return styled_lines;
}

const char *pending_message_label(InteractivePendingMessageKind kind) noexcept {
  switch (kind) {
  case InteractivePendingMessageKind::kSteering:
    return "Steering";
  case InteractivePendingMessageKind::kFollowUp:
    return "Follow-up";
  }
  return "Queued";
}

class SessionSurfaceComponent final : public qt::tui::Component {
public:
  explicit SessionSurfaceComponent(const InteractiveTerminalSession &session)
      : session_(session) {}

  [[nodiscard]] foundation::Result<qt::tui::RenderBuffer>
  render(const qt::tui::RenderConstraints &constraints) const override {
    return session_.base_render_buffer(constraints);
  }

private:
  const InteractiveTerminalSession &session_;
};

} // namespace

InteractiveTerminalSession::InteractiveTerminalSession(
    InteractiveShellRuntime &shell, qt::tui::Terminal &terminal,
    InteractiveTerminalSessionOptions options)
    : shell_(shell), terminal_(terminal), options_(std::move(options)),
      editor_(std::make_shared<qt::tui::Editor>()),
      surface_component_(std::make_shared<SessionSurfaceComponent>(*this)),
      overlay_surface_(surface_component_),
      autocomplete_select_(std::make_shared<qt::tui::Select>(
          std::vector<qt::tui::SelectItem>{}, 8U)) {
  input_decoder_.set_kitty_keyboard_response_handler([this]() {
    const auto status = terminal_.set_kitty_keyboard_protocol(true);
    (void)status;
  });
  editor_->set_submit_callback([this](const std::string &text) {
    pending_submissions_.push_back(text);
    editor_->set_text("");
  });
  editor_->set_change_callback(
      [this](const std::string &) {
        invalidate_render_caches();
        mark_autocomplete_edit();
        refresh_autocomplete(false);
      });
  if (options_.paste_image_path_provider) {
    editor_->set_paste_image_callback([this]() -> std::optional<std::string> {
      return options_.paste_image_path_provider();
    });
  }
  overlay_surface_.set_focus(editor_);
  overlay_surface_.set_invalidation_callback([this]() {
    if (overlay_invalidation_rendering_) {
      return;
    }
    overlay_invalidation_rendering_ = true;
    const auto rendered = render();
    (void)rendered;
    overlay_invalidation_rendering_ = false;
  });
  (void)overlay_surface_.add_input_listener([this](std::string_view bytes) {
    qt::tui::OverlaySurface::InputListenerResult result;
    if (options_.autocomplete_provider == nullptr &&
        !extension_autocomplete_provider_) {
      return result;
    }

    if (autocomplete_active()) {
      if (qt::tui::matches_key(bytes, "up") ||
          qt::tui::matches_key(bytes, "ctrl+p")) {
        move_autocomplete_selection(-1);
        result.consume = true;
        return result;
      }
      if (qt::tui::matches_key(bytes, "down") ||
          qt::tui::matches_key(bytes, "ctrl+n")) {
        move_autocomplete_selection(1);
        result.consume = true;
        return result;
      }
      if (qt::tui::matches_key(bytes, "tab")) {
        (void)apply_selected_autocomplete();
        result.consume = true;
        return result;
      }
      if (qt::tui::matches_key(bytes, "enter")) {
        const bool submit_after_completion =
            should_submit_selected_autocomplete();
        const bool applied = apply_selected_autocomplete();
        result.consume = !(submit_after_completion && applied);
        return result;
      }
      if (qt::tui::matches_key(bytes, "escape") ||
          qt::tui::matches_key(bytes, "ctrl+c")) {
        hide_autocomplete();
        result.consume = true;
        return result;
      }
      return result;
    }

    if (qt::tui::matches_key(bytes, "tab")) {
      refresh_autocomplete(true);
      result.consume = true;
    }
    return result;
  });
}

const qt::tui::Editor &InteractiveTerminalSession::editor() const noexcept {
  return *editor_;
}

const std::vector<std::string> &
InteractiveTerminalSession::transcript_lines() const noexcept {
  return transcript_lines_;
}

qt::tui::Editor &InteractiveTerminalSession::mutable_editor() noexcept {
  return *editor_;
}

const std::optional<std::string> &
InteractiveTerminalSession::status_message() const noexcept {
  return status_message_;
}

void InteractiveTerminalSession::invalidate_render_caches() const {
  bottom_render_cache_.reset();
}

void InteractiveTerminalSession::clear_transcript_render_cache() const {
  transcript_render_cache_.clear();
}

void InteractiveTerminalSession::set_autocomplete_provider(
    std::shared_ptr<qt::tui::CombinedAutocompleteProvider> provider) {
  options_.autocomplete_provider = std::move(provider);
  refresh_autocomplete(false);
}

void InteractiveTerminalSession::set_extension_terminal_input_handler(
    InteractiveTerminalSessionOptions::ExtensionTerminalInputHandler handler) {
  extension_terminal_input_handler_ = std::move(handler);
}

void InteractiveTerminalSession::set_extension_editor_component(
    qt::tui::ComponentPtr component) {
  extension_editor_component_ = std::move(component);
  invalidate_render_caches();
  overlay_surface_.invalidate();
}

void InteractiveTerminalSession::set_extension_editor_input_handler(
    InteractiveTerminalSessionOptions::ExtensionEditorInputHandler handler) {
  extension_editor_input_handler_ = std::move(handler);
  extension_editor_input_error_.reset();
}

void InteractiveTerminalSession::set_extension_autocomplete_provider(
    InteractiveTerminalSessionOptions::ExtensionAutocompleteProvider provider) {
  extension_autocomplete_provider_ = std::move(provider);
  refresh_autocomplete(false);
}

void InteractiveTerminalSession::add_extension_autocomplete_provider(
    InteractiveTerminalSessionOptions::ExtensionAutocompleteProvider provider) {
  if (!provider) return;
  auto previous = std::move(extension_autocomplete_provider_);
  extension_autocomplete_provider_ =
      [previous = std::move(previous), provider = std::move(provider)](
          const std::vector<std::string>& lines, std::size_t row,
          std::size_t column, bool force)
          -> std::optional<qt::tui::AutocompleteSuggestions> {
    auto added = provider(lines, row, column, force);
    auto existing = previous ? previous(lines, row, column, force) : std::nullopt;
    if (!added) return existing;
    if (!existing || existing->prefix != added->prefix) return added;
    existing->items.insert(existing->items.end(), added->items.begin(), added->items.end());
    return existing;
  };
  refresh_autocomplete(false);
}

const std::optional<foundation::Error>&
InteractiveTerminalSession::extension_editor_input_error() const noexcept {
  return extension_editor_input_error_;
}

void InteractiveTerminalSession::set_status_message(
    std::optional<std::string> message) {
  status_message_ = std::move(message);
  invalidate_render_caches();
}

void InteractiveTerminalSession::clear_status_message() {
  status_message_.reset();
  invalidate_render_caches();
}

bool is_tool_result_status_token(std::string_view token) {
  return token == "success" || token == "pending_approval" ||
         token == "invalid_request" || token == "not_found" ||
         token == "permission_denied" || token == "blocked_by_extension" ||
         token == "execution_error" || token == "error";
}

InteractiveTerminalSession::TranscriptEntry
InteractiveTerminalSession::line_transcript_entry(std::string line) {
  TranscriptEntry entry;
  entry.line = std::move(line);
  if (starts_with(entry.line, "tool_call:")) {
    entry.kind = TranscriptEntry::Kind::kToolCall;
    std::string rest = entry.line.substr(std::string_view("tool_call:").size());
    while (!rest.empty() &&
           std::isspace(static_cast<unsigned char>(rest.front())) != 0) {
      rest.erase(rest.begin());
    }
    const auto split = rest.find_first_of(" \t");
    entry.tool_name = split == std::string::npos ? rest : rest.substr(0, split);
    entry.tool_payload =
        split == std::string::npos ? std::string{} : rest.substr(split + 1U);
    return entry;
  }
  if (starts_with(entry.line, "tool_result:")) {
    entry.kind = TranscriptEntry::Kind::kToolResult;
    std::string rest =
        entry.line.substr(std::string_view("tool_result:").size());
    while (!rest.empty() &&
           std::isspace(static_cast<unsigned char>(rest.front())) != 0) {
      rest.erase(rest.begin());
    }
    const auto split = rest.find_first_of(" \t");
    const std::string first_token =
        split == std::string::npos ? rest : rest.substr(0, split);
    if (is_tool_result_status_token(first_token)) {
      entry.tool_status = first_token;
      entry.tool_payload =
          split == std::string::npos ? std::string{} : rest.substr(split + 1U);
    } else {
      entry.tool_name = first_token;
      if (split != std::string::npos) {
        std::string remainder = rest.substr(split + 1U);
        while (!remainder.empty() &&
               std::isspace(static_cast<unsigned char>(remainder.front())) !=
                   0) {
          remainder.erase(remainder.begin());
        }
        const auto status_split = remainder.find_first_of(" \t");
        const std::string maybe_status =
            status_split == std::string::npos
                ? remainder
                : remainder.substr(0, status_split);
        if (is_tool_result_status_token(maybe_status)) {
          entry.tool_status = maybe_status;
          entry.tool_payload = status_split == std::string::npos
                                   ? std::string{}
                                   : remainder.substr(status_split + 1U);
        } else {
          entry.tool_payload = std::move(remainder);
        }
      }
    }
    entry.tool_error = entry.tool_status.find("error") != std::string::npos ||
                       entry.tool_payload.find("error") != std::string::npos;
    return entry;
  }
  entry.kind = TranscriptEntry::Kind::kLine;
  return entry;
}

std::string InteractiveTerminalSession::tool_call_display_line(
    const TranscriptEntry& entry) {
  if (!entry.line.empty()) {
    return entry.line;
  }
  std::string line = "tool_call:";
  if (!entry.tool_name.empty()) {
    line += " " + entry.tool_name;
  }
  if (!entry.tool_payload.empty()) {
    line += " " + entry.tool_payload;
  }
  return line;
}

std::string InteractiveTerminalSession::tool_result_display_line(
    const TranscriptEntry& entry) {
  if (!entry.line.empty()) {
    return entry.line;
  }
  std::string line = "tool_result:";
  if (!entry.tool_name.empty()) {
    line += " " + entry.tool_name;
  }
  if (!entry.tool_status.empty()) {
    line += " " + entry.tool_status;
  }
  if (!entry.tool_payload.empty()) {
    line += " " + entry.tool_payload;
  }
  return line;
}

std::string InteractiveTerminalSession::compacted_tool_display_line(
    const TranscriptEntry& entry) {
  const bool result = entry.kind == TranscriptEntry::Kind::kToolResult;
  std::string line = result ? "tool_result:" : "tool_call:";
  if (!entry.tool_name.empty()) {
    line += " " + entry.tool_name;
  }
  if (result && !entry.tool_status.empty()) {
    line += " " + entry.tool_status;
  }
  line += result ? " output compacted" : " arguments compacted";
  const std::size_t size = !entry.tool_payload.empty()
                               ? entry.tool_payload.size()
                               : entry.line.size();
  if (size > 0U) {
    line += " (" + std::to_string(size) + " chars)";
  }
  if (!entry.tool_call_id.empty()) {
    line += " [" + entry.tool_call_id + "]";
  }
  return line;
}

void InteractiveTerminalSession::append_transcript_line(std::string line) {
  invalidate_render_caches();
  auto entry = line_transcript_entry(line);
  transcript_entries_.push_back(std::move(entry));
  transcript_lines_.push_back(std::move(line));
}

void InteractiveTerminalSession::append_transcript_lines(
    std::vector<std::string> lines) {
  invalidate_render_caches();
  for (const auto& line : lines) {
    transcript_entries_.push_back(line_transcript_entry(line));
  }
  transcript_lines_.insert(transcript_lines_.end(),
                           std::make_move_iterator(lines.begin()),
                           std::make_move_iterator(lines.end()));
}

void InteractiveTerminalSession::append_transcript_literal_lines(
    std::vector<std::string> lines) {
  invalidate_render_caches();
  for (const auto& line : lines) {
    TranscriptEntry entry;
    entry.kind = TranscriptEntry::Kind::kLiteralLine;
    entry.line = line;
    transcript_entries_.push_back(std::move(entry));
  }
  transcript_lines_.insert(transcript_lines_.end(),
                           std::make_move_iterator(lines.begin()),
                           std::make_move_iterator(lines.end()));
}

void InteractiveTerminalSession::append_transcript_component(
    qt::tui::ComponentPtr component) {
  if (component == nullptr) {
    return;
  }
  if (auto* tool = dynamic_cast<qt::tui::ToolExecutionComponent*>(component.get())) {
    tool->set_expanded(tools_expanded_);
  }
  invalidate_render_caches();
  TranscriptEntry entry;
  entry.kind = TranscriptEntry::Kind::kComponent;
  entry.component = std::move(component);
  entry.live_component = true;
  transcript_entries_.push_back(std::move(entry));
}

void InteractiveTerminalSession::finalize_transcript_components() {
  for (auto& entry : transcript_entries_) {
    entry.live_component = false;
  }
  invalidate_render_caches();
}

void InteractiveTerminalSession::set_tools_expanded(bool expanded) {
  tools_expanded_ = expanded;
  for (auto& entry : transcript_entries_) {
    if (auto* tool = dynamic_cast<qt::tui::ToolExecutionComponent*>(entry.component.get())) {
      tool->set_expanded(expanded);
    }
  }
  invalidate_render_caches();
}

void InteractiveTerminalSession::append_transcript_tool_call(
    std::string tool_name, std::string call_id, std::string arguments) {
  invalidate_render_caches();
  TranscriptEntry entry;
  entry.kind = TranscriptEntry::Kind::kToolCall;
  entry.tool_name = std::move(tool_name);
  entry.tool_call_id = std::move(call_id);
  entry.tool_payload = std::move(arguments);
  entry.line = tool_call_display_line(entry);
  transcript_lines_.push_back(entry.line);
  transcript_entries_.push_back(std::move(entry));
}

void InteractiveTerminalSession::append_transcript_tool_result(
    std::string tool_name, std::string call_id, std::string status,
    std::string output, bool is_error) {
  invalidate_render_caches();
  TranscriptEntry entry;
  entry.kind = TranscriptEntry::Kind::kToolResult;
  entry.tool_name = std::move(tool_name);
  entry.tool_call_id = std::move(call_id);
  entry.tool_status = std::move(status);
  entry.tool_payload = std::move(output);
  entry.tool_error = is_error;
  entry.line = tool_result_display_line(entry);
  transcript_lines_.push_back(entry.line);
  transcript_entries_.push_back(std::move(entry));
}

void InteractiveTerminalSession::append_transcript_tool_result_component(
    std::string tool_name, std::string call_id, std::string arguments,
    qt::tui::ComponentPtr component) {
  if (component == nullptr) {
    append_transcript_tool_call(std::move(tool_name), std::move(call_id),
                                std::move(arguments));
    return;
  }
  if (auto* tool = dynamic_cast<qt::tui::ToolExecutionComponent*>(component.get())) {
    tool->set_expanded(tools_expanded_);
  }
  invalidate_render_caches();
  TranscriptEntry entry;
  entry.kind = TranscriptEntry::Kind::kToolResult;
  entry.tool_name = std::move(tool_name);
  entry.tool_call_id = std::move(call_id);
  entry.tool_payload = std::move(arguments);
  entry.component = std::move(component);
  entry.live_component = true;
  transcript_entries_.push_back(std::move(entry));
}

void InteractiveTerminalSession::set_tool_entry_compaction_enabled_tools(
    std::vector<std::string> tool_names) {
  tool_entry_compaction_enabled_tools_.clear();
  tool_entry_compaction_tools_configured_ = true;
  tool_entry_compaction_enabled_tools_.reserve(tool_names.size());
  for (auto& name : tool_names) {
    if (!name.empty()) {
      tool_entry_compaction_enabled_tools_.insert(std::move(name));
    }
  }
}

std::size_t InteractiveTerminalSession::compact_transcript_tool_entries(
    std::size_t keep_recent_entries, std::size_t max_payload_chars) {
  if (transcript_entries_.size() <= keep_recent_entries) {
    return 0U;
  }
  const std::size_t end = transcript_entries_.size() - keep_recent_entries;
  std::size_t compacted = 0U;
  for (std::size_t index = 0; index < end; ++index) {
    auto& entry = transcript_entries_[index];
    if (entry.compacted || (entry.kind != TranscriptEntry::Kind::kToolCall &&
                            entry.kind != TranscriptEntry::Kind::kToolResult)) {
      continue;
    }
    if (!entry.tool_name.empty() && tool_entry_compaction_tools_configured_ &&
        tool_entry_compaction_enabled_tools_.find(entry.tool_name) ==
            tool_entry_compaction_enabled_tools_.end()) {
      continue;
    }
    const bool oversized = entry.tool_payload.size() > max_payload_chars ||
                           entry.line.size() > max_payload_chars;
    if (!oversized && entry.component == nullptr) {
      continue;
    }
    entry.line = compacted_tool_display_line(entry);
    entry.tool_payload.clear();
    entry.component.reset();
    entry.compacted = true;
    ++compacted;
  }
  if (compacted == 0U) {
    return 0U;
  }

  transcript_lines_.clear();
  for (const auto& entry : transcript_entries_) {
    if (entry.kind == TranscriptEntry::Kind::kComponent ||
        entry.component != nullptr) {
      continue;
    }
    if (entry.kind == TranscriptEntry::Kind::kToolCall) {
      transcript_lines_.push_back(tool_call_display_line(entry));
    } else if (entry.kind == TranscriptEntry::Kind::kToolResult) {
      transcript_lines_.push_back(tool_result_display_line(entry));
    } else {
      transcript_lines_.push_back(entry.line);
    }
  }
  invalidate_render_caches();
  clear_transcript_render_cache();
  return compacted;
}

void InteractiveTerminalSession::set_transcript_lines(
    std::vector<std::string> lines) {
  invalidate_render_caches();
  clear_transcript_render_cache();
  transcript_entries_.clear();
  transcript_entries_.reserve(lines.size());
  for (const auto& line : lines) {
    transcript_entries_.push_back(line_transcript_entry(line));
  }
  transcript_lines_ = std::move(lines);
  transcript_scrollback_offset_ = 0;
}

void InteractiveTerminalSession::clear_transcript_lines() {
  invalidate_render_caches();
  clear_transcript_render_cache();
  transcript_lines_.clear();
  transcript_entries_.clear();
  transcript_scrollback_offset_ = 0;
}

qt::tui::OverlayHandle
InteractiveTerminalSession::show_overlay(qt::tui::ComponentPtr component,
                                         qt::tui::OverlayOptions options) {
  invalidate_render_caches();
  if (!options.non_capturing) {
    hide_autocomplete();
  }
  apply_menu_overlay_defaults(options);
  reserve_overlay_bottom_margin(options);
  return overlay_surface_.show_overlay(std::move(component),
                                       std::move(options));
}

void InteractiveTerminalSession::hide_overlay() {
  overlay_surface_.hide_overlay();
}

void InteractiveTerminalSession::clear_overlays() {
  hide_autocomplete();
  overlay_surface_.clear_overlays();
}

bool InteractiveTerminalSession::has_overlay() const {
  return overlay_surface_.has_overlay();
}

qt::tui::OverlayHandle InteractiveTerminalSession::show_extension_select(
    std::string title, std::vector<std::string> options,
    InteractiveTerminalSessionOptions::ExtensionTextHandler on_select,
    InteractiveTerminalSessionOptions::AppActionHandler on_cancel) {
  auto selector = std::make_shared<qt::tui::ExtensionSelector>(
      std::move(title), std::move(options),
      [this, callback = std::move(on_select)](std::string value) {
        auto callback_copy = callback;
        hide_overlay();
        if (callback_copy) {
          callback_copy(std::move(value));
        }
      },
      [this, callback = std::move(on_cancel)]() {
        auto callback_copy = callback;
        hide_overlay();
        if (callback_copy) {
          (void)callback_copy(*this);
        }
      });
  return show_overlay(selector, extension_dialog_overlay_options());
}

qt::tui::OverlayHandle InteractiveTerminalSession::show_extension_confirm(
    std::string title, std::string message,
    InteractiveTerminalSessionOptions::ExtensionBoolHandler on_confirm,
    InteractiveTerminalSessionOptions::AppActionHandler on_cancel) {
  return show_extension_select(
      std::move(title) + "\n" + std::move(message), {"Yes", "No"},
      [callback = std::move(on_confirm)](const std::string &value) {
        if (callback) {
          callback(value == "Yes");
        }
      },
      std::move(on_cancel));
}

qt::tui::OverlayHandle InteractiveTerminalSession::show_extension_input(
    std::string title, std::string placeholder,
    InteractiveTerminalSessionOptions::ExtensionTextHandler on_submit,
    InteractiveTerminalSessionOptions::AppActionHandler on_cancel) {
  auto input = std::make_shared<qt::tui::ExtensionInputComponent>(
      std::move(title),
      [this, callback = std::move(on_submit)](std::string value) {
        auto callback_copy = callback;
        hide_overlay();
        if (callback_copy) {
          callback_copy(std::move(value));
        }
      },
      [this, callback = std::move(on_cancel)]() {
        auto callback_copy = callback;
        hide_overlay();
        if (callback_copy) {
          (void)callback_copy(*this);
        }
      });
  if (!placeholder.empty()) {
    input->input().set_value(std::move(placeholder));
  }
  return show_overlay(input, extension_dialog_overlay_options());
}

qt::tui::OverlayHandle InteractiveTerminalSession::show_extension_editor(
    std::string title, std::string prefill,
    InteractiveTerminalSessionOptions::ExtensionTextHandler on_submit,
    InteractiveTerminalSessionOptions::AppActionHandler on_cancel) {
  auto editor = std::make_shared<qt::tui::ExtensionEditorComponent>(
      std::move(title), std::move(prefill),
      [this, callback = std::move(on_submit)](std::string value) {
        auto callback_copy = callback;
        hide_overlay();
        if (callback_copy) {
          callback_copy(std::move(value));
        }
      },
      [this, callback = std::move(on_cancel)]() {
        auto callback_copy = callback;
        hide_overlay();
        if (callback_copy) {
          (void)callback_copy(*this);
        }
      });
  return show_overlay(editor, extension_dialog_overlay_options());
}

qt::tui::OverlayHandle InteractiveTerminalSession::show_extension_custom(
    qt::tui::ComponentPtr component, qt::tui::OverlayOptions options) {
  return show_overlay(std::move(component), std::move(options));
}

void InteractiveTerminalSession::show_extension_notify(std::string message,
                                                       std::string_view type) {
  if (type == "warning") {
    append_transcript_line("warning: " + std::move(message));
  } else if (type == "error") {
    append_transcript_line("error: " + std::move(message));
  } else {
    set_status_message(std::move(message));
  }
}

void InteractiveTerminalSession::set_extension_widgets(
    std::map<std::string, std::vector<std::string>> widgets) {
  extension_widgets_ = std::move(widgets);
  bottom_render_cache_.reset();
}

void InteractiveTerminalSession::set_extension_header(
    std::optional<std::vector<std::string>> lines) {
  extension_header_ = std::move(lines);
  clear_transcript_render_cache();
}

void InteractiveTerminalSession::set_theme_name(std::string name) {
  theme_name_ = std::move(name);
  bottom_render_cache_.reset();
  clear_transcript_render_cache();
}

const std::string& InteractiveTerminalSession::theme_name() const noexcept {
  return theme_name_;
}

void InteractiveTerminalSession::set_theme_accent(std::string ansi_prefix) {
  theme_accent_ = std::move(ansi_prefix);
  bottom_render_cache_.reset();
}

foundation::Status InteractiveTerminalSession::start() {
  renderer_.reset();
  if (options_.session_adapter != nullptr) {
    options_.session_adapter->reset();
  }
  editor_->set_focused(true);

  const bool notcurses_rendering =
      options_.session_adapter != nullptr &&
      options_.session_adapter->using_notcurses();
  const bool terminal_scrollback_rendering =
      options_.session_adapter != nullptr &&
      options_.session_adapter->uses_terminal_scrollback();

  if (options_.clear_on_start &&
      (!notcurses_rendering || terminal_scrollback_rendering)) {
    const auto clear = terminal_.clear_screen();
    if (!clear) {
      return clear;
    }
  }

  if (options_.hide_cursor &&
      (!notcurses_rendering || terminal_scrollback_rendering)) {
    const auto cursor = terminal_.set_cursor_visible(false);
    if (!cursor) {
      return cursor;
    }
  }

  auto initial = render();
  if (!initial) {
    return foundation::Status(initial.error());
  }
  return foundation::Status::success();
}

foundation::Result<std::vector<InteractiveTerminalStep>>
InteractiveTerminalSession::handle_input(std::string_view bytes) {
  pending_submissions_.clear();

  if (!bytes.empty() && extension_terminal_input_handler_) {
    auto extension_input = extension_terminal_input_handler_(bytes);
    if (!extension_input) return extension_input.error();
    if (!extension_input.value().has_value()) {
      auto rendered = render();
      if (!rendered) return rendered.error();
      return std::vector<InteractiveTerminalStep>{std::move(rendered.value())};
    }
    bytes = *extension_input.value();
  }

  if (bytes.empty() && has_pending_input()) {
    (void)input_decoder_.flush_expired();
  } else {
    input_decoder_.push(bytes);
  }
  if (bytes.empty()) {
    refresh_debounced_autocomplete();
  }
  auto events = input_decoder_.drain();
  for (const auto &event : events) {
    if (event.kind == qt::tui::InputEventKind::resize && event.resize) {
      apply_resize(*event.resize);
      continue;
    }

    if (event.kind == qt::tui::InputEventKind::key &&
        qt::tui::matches_key(event.bytes, "ctrl+c") &&
        (autocomplete_active() || has_overlay())) {
      clear_overlays();
      continue;
    }

    if (handle_autocomplete_event(event)) {
      continue;
    }

    if (!autocomplete_active() && !has_overlay() &&
        event.kind == qt::tui::InputEventKind::key &&
        qt::tui::get_keybindings_const().matches(event.bytes,
                                                 "app.message.dequeue")) {
      if (options_.pending_messages_dequeue_handler &&
          options_.pending_messages_dequeue_handler(*this)) {
        continue;
      }
    }

    if (!autocomplete_active() && !has_overlay() &&
        event.kind == qt::tui::InputEventKind::key &&
        qt::tui::get_keybindings_const().matches(event.bytes,
                                                 "app.interrupt")) {
      if (options_.interrupt_handler && options_.interrupt_handler(*this)) {
        continue;
      }
    }

    if (!autocomplete_active() && !has_overlay() &&
        event.kind == qt::tui::InputEventKind::key &&
        qt::tui::get_keybindings_const().matches(event.bytes, "app.clear")) {
      if (options_.clear_handler && options_.clear_handler(*this)) {
        continue;
      }
      if (!editor_->text().empty()) {
        editor_->set_text("");
        continue;
      }
    }

    if (!autocomplete_active() && !has_overlay() &&
        event.kind == qt::tui::InputEventKind::key && editor_->text().empty() &&
        qt::tui::get_keybindings_const().matches(event.bytes, "app.exit")) {
      if (options_.exit_handler && options_.exit_handler(*this)) {
        continue;
      }
    }

    if (!autocomplete_active() && !has_overlay() &&
        event.kind == qt::tui::InputEventKind::key &&
        qt::tui::get_keybindings_const().matches(event.bytes,
                                                 "app.editor.external")) {
      if (options_.external_editor_handler &&
          options_.external_editor_handler(*this)) {
        continue;
      }
    }

    if (!autocomplete_active() && !has_overlay() &&
        event.kind == qt::tui::InputEventKind::key &&
        qt::tui::get_keybindings_const().matches(event.bytes, "app.suspend")) {
      if (options_.suspend_handler && options_.suspend_handler(*this)) {
        continue;
      }
    }

    if (!has_overlay() && event.kind == qt::tui::InputEventKind::scroll &&
        event.scroll && scroll_transcript(event.scroll->delta_lines)) {
      continue;
    }

    if (!autocomplete_active() && !has_overlay() &&
        event.kind == qt::tui::InputEventKind::key &&
        extension_editor_input_handler_) {
      auto consumed = extension_editor_input_handler_(event.bytes);
      if (!consumed) {
        extension_editor_input_error_ = consumed.error();
        return consumed.error();
      }
      if (consumed.value()) continue;
    }

    overlay_surface_.handle_input(event.bytes);
  }

  std::vector<InteractiveTerminalStep> steps;
  if (pending_submissions_.empty()) {
    auto rendered = render();
    if (!rendered) {
      return rendered.error();
    }
    steps.push_back(std::move(rendered.value()));
    return steps;
  }

  for (const auto &submission : pending_submissions_) {
    auto step = submit_line(submission);
    if (!step) {
      return step.error();
    }
    steps.push_back(std::move(step.value()));
    if (shell_.should_quit()) {
      break;
    }
  }
  pending_submissions_.clear();
  return steps;
}

bool InteractiveTerminalSession::has_pending_input() const noexcept {
  return !input_decoder_.pending_bytes().empty() || debounced_autocomplete_pending_;
}

foundation::Result<std::vector<InteractiveTerminalStep>>
InteractiveTerminalSession::run_script(const std::vector<std::string> &lines) {
  std::vector<InteractiveTerminalStep> steps;
  for (const auto &line : lines) {
    if (shell_.should_quit()) {
      break;
    }

    auto step = submit_line(line);
    if (!step) {
      return step.error();
    }
    steps.push_back(std::move(step.value()));
  }
  return steps;
}

foundation::Result<InteractiveTerminalStep>
InteractiveTerminalSession::submit_line(std::string_view line) {
  auto events = shell_.submit_line(line);
  if (!events) {
    return events.error();
  }

  append_events(events.value());
  editor_->add_to_history(std::string(line));
  if (options_.prompt_history_changed_handler) {
    const auto history_status =
        options_.prompt_history_changed_handler(editor_->history());
    if (!history_status) {
      append_transcript_line("error: " + history_status.error().message);
    }
  }
  editor_->set_text("");
  transcript_scroll_offset_ = 0;
  hide_autocomplete();
  (void)compact_transcript_tool_entries();

  auto rendered = render();
  if (!rendered) {
    return rendered.error();
  }

  rendered.value().events = std::move(events.value());
  return rendered.value();
}

foundation::Result<InteractiveTerminalStep>
InteractiveTerminalSession::render() {
  const auto total_start = std::chrono::steady_clock::now();
  invalidate_render_caches();
  auto buffer = overlay_surface_.render(options_.constraints);
  if (!buffer) {
    return buffer.error();
  }

  auto presented_buffer = std::move(buffer.value());
  const auto diff_start = std::chrono::steady_clock::now();
  auto frame_diff = renderer_.present(std::move(presented_buffer));
  render_metrics_.record_diff(std::chrono::steady_clock::now() - diff_start,
                              frame_diff.rows_scanned,
                              frame_diff.updates.size());
  const auto* presented_frame = renderer_.previous_frame();
  if (presented_frame == nullptr) {
    return foundation::Error{foundation::ErrorCode::internal_error,
                             "diff renderer did not retain the presented frame"};
  }
  const auto present_start = std::chrono::steady_clock::now();
  const auto status = present(*presented_frame, frame_diff);
  render_metrics_.record_present(std::chrono::steady_clock::now() - present_start);
  if (!status) {
    return status.error();
  }
  render_metrics_.record_total(std::chrono::steady_clock::now() - total_start);

  InteractiveTerminalStep step;
  step.frame_diff = std::move(frame_diff);
  return step;
}

foundation::Result<qt::tui::RenderBuffer>
InteractiveTerminalSession::base_render_buffer(
    const qt::tui::RenderConstraints &constraints) const {
  std::vector<std::string> transcript;
  const bool native_scrollback_live_region =
      options_.session_adapter != nullptr &&
      options_.session_adapter->uses_terminal_scrollback() &&
      transcript_scroll_offset_ == 0U;
  const auto bottom_start = std::chrono::steady_clock::now();
  auto bottom = bottom_render_lines_ref(constraints);
  render_metrics_.record_bottom(std::chrono::steady_clock::now() - bottom_start);
  const std::size_t bottom_reserved =
      constraints.max_height == 0
          ? bottom.size()
          : std::min(bottom.size(), constraints.max_height);
  const bool restoring_overlay_region =
      native_scrollback_live_region && !has_overlay() &&
      presented_scrollback_live_region_height_ > bottom_reserved &&
      !uses_notcurses_live_region(options_.session_adapter);
  const bool overlay_live_repaint =
      native_scrollback_live_region && has_overlay() &&
      !uses_notcurses_live_region(options_.session_adapter);
  const auto transcript_start = std::chrono::steady_clock::now();
  if (!native_scrollback_live_region || restoring_overlay_region ||
      overlay_live_repaint) {
    transcript = transcript_render_lines(constraints);
  } else {
    const auto live_component_start = first_trailing_live_component_index();
    if (live_component_start < transcript_entries_.size()) {
      transcript = transcript_render_lines(live_component_start,
                                           transcript_entries_.size(),
                                           constraints);
    }
  }
  if (extension_header_) {
    transcript.insert(transcript.begin(), extension_header_->begin(),
                      extension_header_->end());
  }
  render_metrics_.record_transcript(std::chrono::steady_clock::now() -
                                    transcript_start);

  return qt::tui::make_buffer(layout_lines(std::move(transcript),
                                           std::move(bottom), constraints),
                              constraints);
}

qt::tui::RenderMetricsSnapshot
InteractiveTerminalSession::render_metrics() const noexcept {
  return render_metrics_.snapshot();
}

void InteractiveTerminalSession::reset_render_metrics() noexcept {
  render_metrics_.reset();
}

std::vector<std::string> InteractiveTerminalSession::footer_lines() const {
  if (!options_.footer_provider) {
    return {};
  }
  return options_.footer_provider();
}

std::vector<std::string> InteractiveTerminalSession::transcript_render_lines(
    const qt::tui::RenderConstraints &constraints) const {
  return transcript_render_lines(0U, transcript_entries_.size(), constraints);
}

std::vector<std::string> InteractiveTerminalSession::transcript_render_lines(
    std::size_t begin, std::size_t end,
    const qt::tui::RenderConstraints &constraints) const {
  std::vector<std::string> rendered;
  transcript_render_cache_.resize(transcript_entries_.size());
  begin = std::min(begin, transcript_entries_.size());
  end = std::min(end, transcript_entries_.size());
  if (begin > end) {
    begin = end;
  }
  for (std::size_t index = begin; index < end; ++index) {
    const auto& entry = transcript_entries_[index];
    std::vector<std::string> wrapped;
    const bool cacheable = entry.component == nullptr &&
                           (entry.kind == TranscriptEntry::Kind::kLine ||
                            entry.kind == TranscriptEntry::Kind::kLiteralLine ||
                            entry.kind == TranscriptEntry::Kind::kToolCall ||
                            entry.kind == TranscriptEntry::Kind::kToolResult);
    if (cacheable) {
      auto& cached = transcript_render_cache_[index];
      if (!cached.valid || cached.width != constraints.max_width ||
          cached.kind != entry.kind || cached.line != entry.line) {
        cached.valid = true;
        cached.width = constraints.max_width;
        cached.kind = entry.kind;
        cached.line = entry.line;
        cached.rendered_lines =
            transcript_entry_render_lines(entry, constraints);
      }
      wrapped = cached.rendered_lines;
    } else {
      wrapped = transcript_entry_render_lines(entry, constraints);
    }
    rendered.insert(rendered.end(), std::make_move_iterator(wrapped.begin()),
                    std::make_move_iterator(wrapped.end()));
  }
  if (begin == 0U && end == transcript_entries_.size() &&
      rendered.empty() && !transcript_lines_.empty()) {
    return wrap_styled_display_lines(
        transcript_lines_, constraints.max_width,
        [this](std::string_view line) -> LineStyler {
          if (!options_.input_prompt.empty() &&
              starts_with(line, options_.input_prompt)) {
            return qt::tui::style::cyan;
          }
          return transcript_content_styler(line);
        });
  }
  return rendered;
}

std::size_t
InteractiveTerminalSession::first_trailing_live_component_index() const noexcept {
  std::size_t index = transcript_entries_.size();
  while (index > 0U && transcript_entries_[index - 1U].component != nullptr &&
         transcript_entries_[index - 1U].live_component) {
    --index;
  }
  return index;
}

std::size_t InteractiveTerminalSession::trailing_live_component_line_count(
    const qt::tui::RenderConstraints& constraints) const {
  const auto live_component_start = first_trailing_live_component_index();
  if (live_component_start >= transcript_entries_.size()) {
    return 0U;
  }
  return transcript_render_lines(live_component_start, transcript_entries_.size(),
                                 constraints)
      .size();
}

std::vector<std::string>
InteractiveTerminalSession::transcript_entry_render_lines(
    const TranscriptEntry &entry,
    const qt::tui::RenderConstraints &constraints) const {
  if (entry.kind == TranscriptEntry::Kind::kComponent ||
      entry.component != nullptr) {
    if (entry.component == nullptr) {
      return {};
    }
    const auto component_height = constraints.max_height == 0
                                      ? std::size_t{10000}
                                      : constraints.max_height;
    auto buffer = entry.component->render(
        qt::tui::RenderConstraints{constraints.max_width, component_height});
    if (!buffer) {
      return {"error: " + buffer.error().message};
    }
    return std::move(buffer.value().lines);
  }

  if (entry.kind == TranscriptEntry::Kind::kLiteralLine) {
    std::vector<std::string> rendered;
    for (const auto split : qt::tui::split_lines_view(entry.line)) {
      auto wrapped = wrap_literal_display_line(split, constraints.max_width);
      rendered.insert(rendered.end(), std::make_move_iterator(wrapped.begin()),
                      std::make_move_iterator(wrapped.end()));
    }
    return rendered;
  }

  if (entry.kind == TranscriptEntry::Kind::kToolCall) {
    return wrap_styled_display_lines(
        std::vector<std::string>{tool_call_display_line(entry)},
        constraints.max_width,
        [](std::string_view) -> LineStyler { return llama_tool_call_line; });
  }

  if (entry.kind == TranscriptEntry::Kind::kToolResult) {
    return wrap_styled_display_lines(
        std::vector<std::string>{tool_result_display_line(entry)},
        constraints.max_width,
        [](std::string_view) -> LineStyler { return llama_tool_result_line; });
  }

  return wrap_styled_display_lines(
      std::vector<std::string>{entry.line}, constraints.max_width,
      [this](std::string_view line) -> LineStyler {
        if (!options_.input_prompt.empty() &&
            starts_with(line, options_.input_prompt)) {
          return qt::tui::style::cyan;
        }
        return transcript_content_styler(line);
      });
}

std::vector<std::string>
InteractiveTerminalSession::pending_message_lines() const {
  if (!options_.pending_messages_provider) {
    return {};
  }

  const auto messages = options_.pending_messages_provider();
  if (messages.empty()) {
    return {};
  }

  std::vector<std::string> lines;
  lines.reserve(messages.size() + 2U);
  lines.emplace_back("");
  for (const auto &message : messages) {
    if (message.text.empty()) {
      continue;
    }
    std::string line = pending_message_label(message.kind);
    line += ": ";
    line += first_display_line(message.text);
    lines.push_back(std::move(line));
  }
  if (lines.size() == 1U) {
    return {};
  }
  lines.emplace_back("-> Alt+Up to edit all queued messages");
  return lines;
}

std::vector<std::string> InteractiveTerminalSession::editor_render_lines(
    const qt::tui::RenderConstraints &constraints) const {
  if (extension_editor_component_) {
    auto rendered = extension_editor_component_->render(constraints);
    if (!rendered) {
      return {"<editor render error: " + rendered.error().message + ">"};
    }
    return std::move(rendered.value().lines);
  }
  const auto height =
      constraints.max_height == 0 ? 24U : constraints.max_height;
  const auto desired_visible = std::max<std::size_t>(5U, height * 30U / 100U);
  const auto max_visible = std::max<std::size_t>(
      1U, std::min(desired_visible, height > 2U ? height - 2U : 1U));
  const auto previous_max_visible = editor_->max_visible_lines();
  editor_->set_max_visible_lines(max_visible);
  auto rendered = editor_->render(
      qt::tui::RenderConstraints{constraints.max_width, max_visible + 2U});
  editor_->set_max_visible_lines(previous_max_visible);
  if (!rendered) {
    return {"<editor render error: " + rendered.error().message + ">"};
  }
  return std::move(rendered.value().lines);
}

const std::vector<std::string>&
InteractiveTerminalSession::bottom_render_lines_ref(
    const qt::tui::RenderConstraints &constraints) const {
  if (bottom_render_cache_ &&
      bottom_render_cache_->constraints.max_width == constraints.max_width &&
      bottom_render_cache_->constraints.max_height == constraints.max_height) {
    return bottom_render_cache_->lines;
  }

  std::vector<std::string> bottom;

  for (const auto& [key, lines] : extension_widgets_) {
    (void)key;
    auto widget_lines = wrap_styled_display_lines(
        lines, constraints.max_width,
        [](std::string_view) { return plain_line; });
    bottom.insert(bottom.end(), widget_lines.begin(), widget_lines.end());
  }

  auto pending_lines = wrap_styled_display_lines(
      pending_message_lines(), constraints.max_width, pending_line_styler);
  bottom.insert(bottom.end(), pending_lines.begin(), pending_lines.end());

  if (status_message_ && !status_message_->empty()) {
    auto status_lines = wrap_styled_display_lines(
        std::vector<std::string>{*status_message_}, constraints.max_width,
        [this](std::string_view) {
          return [this](std::string_view text) {
            return theme_accent_ + std::string(text) +
                   std::string(qt::tui::style::reset);
          };
        });
    bottom.insert(bottom.end(), std::make_move_iterator(status_lines.begin()),
                  std::make_move_iterator(status_lines.end()));
  }

  auto editor_lines = editor_render_lines(constraints);
  bottom.insert(bottom.end(), editor_lines.begin(), editor_lines.end());

  auto footer = footer_lines();
  bottom.insert(bottom.end(), footer.begin(), footer.end());
  bottom_render_cache_ = BottomRenderCache{constraints, std::move(bottom)};
  return bottom_render_cache_->lines;
}

std::vector<std::string> InteractiveTerminalSession::bottom_render_lines(
    const qt::tui::RenderConstraints &constraints) const {
  return bottom_render_lines_ref(constraints);
}

std::size_t InteractiveTerminalSession::bottom_reserved_line_count(
    const qt::tui::RenderConstraints &constraints) const {
  return bottom_render_lines_ref(constraints).size();
}

std::size_t InteractiveTerminalSession::scrollback_live_region_height(
    const qt::tui::RenderConstraints &constraints) const {
  const std::size_t live_component_lines =
      trailing_live_component_line_count(constraints);
  const std::size_t reserved =
      bottom_reserved_line_count(constraints) + live_component_lines;
  if (constraints.max_height == 0) {
    return reserved;
  }
  return std::min(reserved, constraints.max_height);
}

void InteractiveTerminalSession::reserve_overlay_bottom_margin(
    qt::tui::OverlayOptions &options) const {
  if (options_.constraints.max_height == 0) {
    return;
  }
  const std::size_t reserved = bottom_reserved_line_count(options_.constraints);
  const std::size_t capped =
      std::min(reserved + 1U, options_.constraints.max_height - 1U);
  options.margin.bottom =
      std::max(options.margin.bottom, static_cast<int>(capped));
}

std::vector<std::string> InteractiveTerminalSession::layout_lines(
    std::vector<std::string> transcript, std::vector<std::string> bottom,
    const qt::tui::RenderConstraints &constraints) const {
  if (constraints.max_height == 0) {
    transcript.insert(transcript.end(), bottom.begin(), bottom.end());
    return transcript;
  }

  std::vector<std::string> lines;
  lines.reserve(constraints.max_height);

  std::size_t remaining = constraints.max_height;
  std::vector<std::string> bottom_visible;
  if (bottom.size() > remaining) {
    bottom_visible.insert(bottom_visible.end(),
                          bottom.end() - static_cast<std::ptrdiff_t>(remaining),
                          bottom.end());
    remaining = 0;
  } else {
    bottom_visible = std::move(bottom);
    remaining -= bottom_visible.size();
  }

  if (remaining > 0 && !transcript.empty()) {
    const std::size_t window = std::min(remaining, transcript.size());
    const std::size_t max_offset = transcript.size() - window;
    const std::size_t offset = std::min(transcript_scroll_offset_, max_offset);
    const std::size_t start = transcript.size() - window - offset;
    if (window < remaining) {
      const std::size_t padding = remaining - window;
      lines.insert(lines.end(), padding, std::string{});
      remaining -= padding;
    }
    lines.insert(
        lines.end(), transcript.begin() + static_cast<std::ptrdiff_t>(start),
        transcript.begin() + static_cast<std::ptrdiff_t>(start + window));
    remaining -= window;
  }

  while (remaining > 0) {
    lines.emplace_back("");
    --remaining;
  }

  lines.insert(lines.end(), bottom_visible.begin(), bottom_visible.end());
  return lines;
}

std::optional<qt::tui::AutocompleteSuggestions>
InteractiveTerminalSession::autocomplete_suggestions(bool force) const {
  if (overlay_surface_.focused_component() != editor_ ||
      (!force && editor_->browsing_history())) {
    return std::nullopt;
  }

  const auto lines = split_lines(editor_->text());
  std::optional<qt::tui::AutocompleteSuggestions> builtin;
  if (options_.autocomplete_provider != nullptr) {
    builtin = options_.autocomplete_provider->get_suggestions(
        lines, editor_->cursor_row(), editor_->cursor_column(), force);
  }
  std::optional<qt::tui::AutocompleteSuggestions> extension;
  if (extension_autocomplete_provider_) {
    extension = extension_autocomplete_provider_(
        lines, editor_->cursor_row(), editor_->cursor_column(), force);
  }
  if (!builtin) return extension;
  if (!extension) return builtin;
  qt::tui::AutocompleteSuggestions combined = std::move(*builtin);
  if (combined.prefix != extension->prefix) return extension;
  combined.items.insert(combined.items.end(), extension->items.begin(),
                        extension->items.end());
  return combined;
}

bool InteractiveTerminalSession::autocomplete_debounce_due() const {
  if (!debounced_autocomplete_pending_) {
    return false;
  }
  return std::chrono::steady_clock::now() - last_autocomplete_edit_ >=
         kAutocompleteAttachmentDebounce;
}

bool InteractiveTerminalSession::autocomplete_attachment_context() const {
  const auto lines = split_lines(editor_->text());
  const std::size_t row = editor_->cursor_row();
  if (row >= lines.size()) {
    return false;
  }
  const auto& line = lines[row];
  const std::size_t cursor = std::min(editor_->cursor_column(), line.size());
  const std::string_view before(line.data(), cursor);
  const auto delimiter = before.find_last_of(" \t\"'=");
  const std::size_t token_start =
      delimiter == std::string_view::npos ? 0U : delimiter + 1U;
  return token_start < before.size() && before[token_start] == '@';
}

void InteractiveTerminalSession::mark_autocomplete_edit() {
  last_autocomplete_edit_ = std::chrono::steady_clock::now();
}

void InteractiveTerminalSession::refresh_debounced_autocomplete() {
  if (!autocomplete_debounce_due()) {
    return;
  }
  debounced_autocomplete_pending_ = false;
  refresh_autocomplete(false);
}

void InteractiveTerminalSession::refresh_autocomplete(bool force) {
  if ((options_.autocomplete_provider == nullptr &&
       !extension_autocomplete_provider_) ||
      overlay_surface_.focused_component() != editor_) {
    hide_autocomplete();
    return;
  }
  if (!force && autocomplete_attachment_context() && !autocomplete_debounce_due()) {
    debounced_autocomplete_pending_ = true;
    hide_autocomplete();
    return;
  }
  debounced_autocomplete_pending_ = false;

  const auto suggestions = autocomplete_suggestions(force);
  if (!suggestions || suggestions->items.empty()) {
    hide_autocomplete();
    return;
  }

  autocomplete_state_.set_suggestions(*suggestions);
  std::vector<qt::tui::SelectItem> items;
  items.reserve(suggestions->items.size());
  for (const auto &item : suggestions->items) {
    items.push_back(qt::tui::SelectItem{
        item.value, item.label.empty() ? item.value : item.label,
        item.description.value_or(std::string{})});
  }
  autocomplete_select_->set_title(std::optional<std::string>{"Completions"});
  autocomplete_select_->set_items(std::move(items));
  autocomplete_select_->set_selected_index(
      autocomplete_state_.selected_index());

  if (!autocomplete_overlay_) {
    show_autocomplete_overlay();
    return;
  }
  overlay_surface_.set_focus(editor_);
}

void InteractiveTerminalSession::show_autocomplete_overlay() {
  qt::tui::OverlayOptions overlay_options;
  overlay_options.width = qt::tui::OverlaySize::percent(60U);
  overlay_options.min_width = 24U;
  overlay_options.max_height = qt::tui::OverlaySize::percent(35U);
  overlay_options.anchor = qt::tui::OverlayAnchor::bottom_left;
  overlay_options.offset_x = 0;
  overlay_options.offset_y = 0;
  overlay_options.margin = qt::tui::OverlayMargin::all(1);
  overlay_options.non_capturing = true;
  reserve_overlay_bottom_margin(overlay_options);
  autocomplete_overlay_ =
      overlay_surface_.show_overlay(autocomplete_select_, overlay_options);
  overlay_surface_.set_focus(editor_);
}

void InteractiveTerminalSession::hide_autocomplete() {
  autocomplete_state_.clear();
  if (autocomplete_overlay_) {
    autocomplete_overlay_->hide();
    autocomplete_overlay_.reset();
  }
}

bool InteractiveTerminalSession::handle_autocomplete_event(
    const qt::tui::InputEvent &event) {
  if ((options_.autocomplete_provider == nullptr &&
       !extension_autocomplete_provider_) ||
      event.kind != qt::tui::InputEventKind::key) {
    return false;
  }

  const auto is_key = [&](std::string_view key) {
    return event.key && *event.key == key;
  };
  const auto &keybindings = qt::tui::get_keybindings_const();

  if (autocomplete_active()) {
    if (is_key("up") || is_key("ctrl+p") ||
        keybindings.matches(event.bytes, "tui.select.up") ||
        keybindings.matches(event.bytes, "tui.cursor.up")) {
      move_autocomplete_selection(-1);
      return true;
    }
    if (is_key("down") || is_key("ctrl+n") ||
        keybindings.matches(event.bytes, "tui.select.down") ||
        keybindings.matches(event.bytes, "tui.cursor.down")) {
      move_autocomplete_selection(1);
      return true;
    }
    if (is_key("tab") || keybindings.matches(event.bytes, "tui.input.tab")) {
      (void)apply_selected_autocomplete();
      return true;
    }
    if (is_key("enter") ||
        keybindings.matches(event.bytes, "tui.select.confirm") ||
        keybindings.matches(event.bytes, "tui.input.submit")) {
      const bool submit_after_completion =
          should_submit_selected_autocomplete();
      const bool applied = apply_selected_autocomplete();
      return !(submit_after_completion && applied);
    }
    if (is_key("escape") || is_key("ctrl+c") ||
        keybindings.matches(event.bytes, "tui.select.cancel") ||
        keybindings.matches(event.bytes, "tui.input.cancel")) {
      hide_autocomplete();
      return true;
    }
    return false;
  }

  if (is_key("tab") || keybindings.matches(event.bytes, "tui.input.tab")) {
    refresh_autocomplete(true);
    return true;
  }

  return false;
}

void InteractiveTerminalSession::move_autocomplete_selection(int delta) {
  if (!autocomplete_active()) {
    return;
  }

  if (delta < 0) {
    autocomplete_state_.select_previous();
  } else if (delta > 0) {
    autocomplete_state_.select_next();
  }
  autocomplete_select_->set_selected_index(
      autocomplete_state_.selected_index());
  if (autocomplete_overlay_) {
    overlay_surface_.invalidate();
  }
}

bool InteractiveTerminalSession::apply_selected_autocomplete() {
  if (!autocomplete_active()) {
    return false;
  }

  const auto *item = autocomplete_state_.selected_item();
  const auto suggestions = autocomplete_state_.suggestions();
  if (item == nullptr || !suggestions ||
      options_.autocomplete_provider == nullptr) {
    return false;
  }

  const auto applied = options_.autocomplete_provider->apply_completion(
      split_lines(editor_->text()), editor_->cursor_row(),
      editor_->cursor_column(), *item, suggestions->prefix);
  editor_->apply_completion_contents(applied.lines, applied.cursor_line,
                                     applied.cursor_column);
  hide_autocomplete();
  return true;
}

bool InteractiveTerminalSession::should_submit_selected_autocomplete() const {
  if (!autocomplete_active()) {
    return false;
  }

  const auto suggestions = autocomplete_state_.suggestions();
  if (!suggestions || suggestions->prefix.empty() ||
      autocomplete_state_.selected_item() == nullptr) {
    return false;
  }
  const auto* item = autocomplete_state_.selected_item();

  const auto lines = split_lines(editor_->text());
  if (editor_->cursor_row() >= lines.size()) {
    return false;
  }

  const auto &current_line = lines[editor_->cursor_row()];
  const std::size_t cursor =
      std::min(editor_->cursor_column(), current_line.size());
  const std::string text_before_cursor = current_line.substr(0, cursor);
  const auto slash_offset = text_before_cursor.find_first_not_of(" \t");
  if (slash_offset == std::string::npos ||
      text_before_cursor[slash_offset] != '/') {
    return false;
  }
  const std::string slash_text = text_before_cursor.substr(slash_offset);
  if (suggestions->prefix.front() == '/') {
    return slash_text == suggestions->prefix &&
           slash_text.find_first_of(" \t") == std::string::npos;
  }

  if (slash_text.find_first_of(" \t") == std::string::npos) {
    return false;
  }
  return item->value == suggestions->prefix || item->label == suggestions->prefix;
}

bool InteractiveTerminalSession::autocomplete_active() const noexcept {
  return autocomplete_state_.active();
}

bool InteractiveTerminalSession::scroll_transcript(int delta_lines) {
  const auto transcript = transcript_render_lines(options_.constraints);
  if (transcript.empty()) {
    return false;
  }

  const auto bottom = bottom_render_lines(options_.constraints);
  const std::size_t fixed = std::min(options_.constraints.max_height,
                                     bottom.size());
  const std::size_t window = options_.constraints.max_height > fixed
                                 ? options_.constraints.max_height - fixed
                                 : 0U;
  if (window == 0 || transcript.size() <= window) {
    transcript_scroll_offset_ = 0;
    return false;
  }

  const std::size_t max_offset = transcript.size() - window;
  if (delta_lines > 0) {
    transcript_scroll_offset_ =
        std::min(max_offset, transcript_scroll_offset_ +
                                 static_cast<std::size_t>(delta_lines));
  } else if (delta_lines < 0) {
    const auto amount = static_cast<std::size_t>(-delta_lines);
    transcript_scroll_offset_ = amount >= transcript_scroll_offset_
                                    ? 0
                                    : transcript_scroll_offset_ - amount;
  } else {
    return false;
  }
  return true;
}

void InteractiveTerminalSession::append_events(
    const std::vector<InteractiveShellEvent> &events) {
  for (const auto &event : events) {
    switch (event.kind) {
    case InteractiveShellEventKind::kPromptReady:
      pending_prompt_ = event.text;
      break;
    case InteractiveShellEventKind::kInputSubmitted: {
      const std::string prompt =
          pending_prompt_.empty() ? options_.input_prompt : pending_prompt_;
      append_transcript_line(prompt + event.text);
      pending_prompt_.clear();
      break;
    }
    case InteractiveShellEventKind::kPromptDispatched:
    case InteractiveShellEventKind::kShellCommandDispatched:
      if (!event.text.empty()) {
        append_transcript_line(event.text);
      }
      break;
    case InteractiveShellEventKind::kSlashCommandDispatched: {
      bool append_default = !event.text.empty();
      if (options_.slash_command_executor) {
        const auto handled = options_.slash_command_executor(*this, event);
        if (!handled) {
          append_transcript_line("error: " + handled.error().message);
          break;
        }
        if (handled.value().handled) {
          append_default = handled.value().append_default_output;
          append_transcript_lines(
              std::vector<std::string>(handled.value().transcript_lines));
        }
      }
      if (append_default && !event.text.empty()) {
        append_transcript_line(event.text);
      }
      break;
    }
    case InteractiveShellEventKind::kHelpDisplayed:
      if (options_.help_provider) {
        append_transcript_lines(options_.help_provider());
      } else if (!event.text.empty()) {
        append_transcript_line(event.text);
      }
      break;
    case InteractiveShellEventKind::kQuitRequested:
      append_transcript_line("[quit]");
      break;
    case InteractiveShellEventKind::kError:
      append_transcript_line("error: " + event.text);
      break;
    }
  }
}

void InteractiveTerminalSession::apply_resize(qt::tui::TerminalSize size) {
  invalidate_render_caches();
  terminal_.set_size(size);
  if (size.columns > 0) {
    options_.constraints.max_width = size.columns;
  }
  if (size.rows > 0) {
    options_.constraints.max_height = size.rows;
  }
  renderer_.reset();
  clear_before_next_present_ = true;
  presented_scrollback_live_region_height_ = 0U;
  if (autocomplete_overlay_ && autocomplete_active()) {
    autocomplete_overlay_->hide();
    autocomplete_overlay_.reset();
    show_autocomplete_overlay();
  }
  if (options_.session_adapter != nullptr) {
    options_.session_adapter->reset();
  }
}

foundation::Status InteractiveTerminalSession::flush_transcript_scrollback() {
  if (options_.session_adapter == nullptr ||
      !options_.session_adapter->uses_terminal_scrollback()) {
    return foundation::Status::success();
  }
  if (transcript_scrollback_offset_ >= transcript_entries_.size()) {
    return foundation::Status::success();
  }
  const auto flush_end = first_trailing_live_component_index();
  if (transcript_scrollback_offset_ >= flush_end) {
    return foundation::Status::success();
  }

  const std::size_t width = options_.constraints.max_width == 0
                                ? std::size_t{80}
                                : options_.constraints.max_width;
  const std::size_t fixed_bottom_lines =
      std::max(scrollback_live_region_height(options_.constraints),
               presented_scrollback_live_region_height_);
  std::vector<std::string> lines;
  for (std::size_t index = transcript_scrollback_offset_;
       index < flush_end; ++index) {
    auto rendered_lines = transcript_entry_render_lines(
        transcript_entries_[index], qt::tui::RenderConstraints{width, 10000U});
    lines.insert(lines.end(), std::make_move_iterator(rendered_lines.begin()),
                 std::make_move_iterator(rendered_lines.end()));
  }
  if (!lines.empty()) {
    const auto status =
        options_.session_adapter->append_scrollback_lines(lines, fixed_bottom_lines);
    if (!status) {
      return status;
    }
  }
  transcript_scrollback_offset_ = flush_end;
  return foundation::Status::success();
}

foundation::Status
InteractiveTerminalSession::present(const qt::tui::RenderBuffer &buffer,
                                    const qt::tui::FrameDiff &frame_diff) {
  if (options_.session_adapter != nullptr) {
    const auto scrollback = flush_transcript_scrollback();
    if (!scrollback) {
      return scrollback;
    }
    clear_before_next_present_ = false;
    if (options_.session_adapter->uses_terminal_scrollback() &&
        transcript_scroll_offset_ == 0U) {
      const std::size_t current_height =
          scrollback_live_region_height(options_.constraints);
      std::size_t height =
          std::max(current_height, presented_scrollback_live_region_height_);
      std::vector<std::string> live_lines;
      std::size_t row = options_.constraints.max_height > height
                            ? options_.constraints.max_height - height
                            : 0U;
      const auto& source = buffer.lines;
      if (has_overlay()) {
        std::size_t first_content = source.size();
        for (std::size_t index = 0; index < source.size(); ++index) {
          if (!ansi_styled_line_is_blank(source[index])) {
            first_content = index;
            break;
          }
        }
        if (first_content < source.size()) {
          const std::size_t previous_top =
              options_.constraints.max_height >
                      presented_scrollback_live_region_height_
                  ? options_.constraints.max_height -
                        presented_scrollback_live_region_height_
                  : 0U;
          row = std::min(first_content, previous_top);
          live_lines.insert(
              live_lines.end(),
              source.begin() + static_cast<std::ptrdiff_t>(row),
              source.end());
          height = live_lines.size();
        }
      } else if (height > 0U) {
        const std::size_t count = std::min(height, source.size());
        row = source.size() >= count ? source.size() - count : 0U;
        live_lines.insert(
            live_lines.end(),
            source.end() - static_cast<std::ptrdiff_t>(count),
            source.end());
        if (height >= options_.constraints.max_height && count < height) {
          live_lines.insert(live_lines.begin(), height - count, std::string{});
          row = 0U;
        }
      }
      auto live_buffer = qt::tui::make_buffer(
          std::move(live_lines),
          qt::tui::RenderConstraints{options_.constraints.max_width,
                                     live_lines.size()});
      if (!live_buffer) {
        return foundation::Status(live_buffer.error());
      }
      const auto presented =
          options_.session_adapter->present_at(live_buffer.value(), row);
      if (presented) {
        presented_scrollback_live_region_height_ =
            has_overlay() ? height : current_height;
      }
      return presented;
    }
    return options_.session_adapter->present(buffer, frame_diff);
  }

  auto output = qt::tui::synchronized_output_for_diff(frame_diff);
  if (clear_before_next_present_) {
    qt::tui::SynchronizedOutput cleared;
    cleared.write("\x1b[2J\x1b[H\x1b[3J");
    cleared.write(output.bytes());
    output = std::move(cleared);
    clear_before_next_present_ = false;
  }
  if (options_.hide_cursor) {
    const auto hide_cursor = qt::tui::ansi_set_cursor_visible(false);
    const auto &bytes = output.bytes();
    if (bytes.size() >= hide_cursor.size() &&
        bytes.compare(bytes.size() - hide_cursor.size(), hide_cursor.size(),
                      hide_cursor) == 0) {
      qt::tui::SynchronizedOutput adjusted;
      adjusted.write(bytes.substr(0, bytes.size() - hide_cursor.size()));
      return terminal_.write_synchronized(adjusted);
    }
  }
  return terminal_.write_synchronized(output);
}

} // namespace qt::app
