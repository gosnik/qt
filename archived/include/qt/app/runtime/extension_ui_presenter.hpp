#pragma once

#include <qt/app/interactive/interactive_terminal_session.hpp>
#include <qt/extensions/angelscript_host.hpp>
#include <qt/foundation/error.hpp>

#include <functional>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qt::app {

struct InteractiveExtensionUiState {
  std::map<std::string, std::string> statuses;
  std::optional<std::string> working_message;
  std::optional<std::vector<std::string>> working_frames;
  std::optional<std::string> hidden_thinking_label;
  bool working_visible = true;
  bool tools_expanded = false;
  std::map<std::string, std::vector<std::string>> widgets;
  std::optional<std::vector<std::string>> footer_lines;
  std::optional<std::vector<std::string>> header_lines;
  std::function<void(std::optional<std::string>)> apply_working_message;
  std::function<void(bool)> apply_working_visible;
  std::function<void(std::optional<std::string>)> apply_hidden_thinking_label;
  std::function<void(bool)> apply_tools_expanded;
  std::function<bool()> read_tools_expanded;
  std::function<void(std::optional<std::vector<std::string>>, std::size_t)>
      apply_working_indicator;
  std::function<void(const std::map<std::string, std::vector<std::string>>&)>
      apply_widgets;
  std::function<void(std::optional<std::vector<std::string>>)> apply_footer;
  std::function<void(std::optional<std::vector<std::string>>)> apply_header;
  std::function<qt::foundation::Result<qt::extensions::Json>(
      std::string_view, const qt::extensions::Json&)> theme_call;
  std::function<qt::foundation::Result<qt::extensions::Json>(
      std::string_view, const qt::extensions::Json&)> script_call;
  std::shared_ptr<qt::extensions::ExtensionHostCall> invocation_slot;
  std::optional<std::string> editor_handler;
};

using ExtensionUiPump = std::function<qt::foundation::Status()>;

qt::extensions::ExtensionHostCall make_interactive_extension_ui_host_call(
    InteractiveTerminalSession& session, InteractiveExtensionUiState& state,
    ExtensionUiPump pump,
    std::function<qt::foundation::Status(std::string)> set_title = {},
    std::function<void()> invalidate = {});

}  // namespace qt::app
