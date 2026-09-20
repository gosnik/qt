#include <qt/app/runtime/extension_ui_presenter.hpp>
#include <qt/app/interactive/scripted_extension_component.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace qt::app {
namespace {

using qt::extensions::ExtensionHostCall;
using qt::extensions::Json;
using qt::foundation::ErrorCode;
using qt::foundation::Result;
using qt::foundation::Status;

Result<Json> unsupported(std::string method) {
  return Result<Json>::failure(
      ErrorCode::invalid_argument,
      "not_implemented: " + method +
          " requires a typed in-process UI component or presenter");
}

std::string required_string(const Json& arguments, const char* field) {
  if (!arguments.is_object() || !arguments.contains(field) ||
      !arguments.at(field).is_string()) {
    throw std::invalid_argument(std::string("UI method requires string ") + field);
  }
  return arguments.at(field).get<std::string>();
}

std::vector<std::string> required_strings(const Json& arguments,
                                          const char* field) {
  if (!arguments.is_object() || !arguments.contains(field) ||
      !arguments.at(field).is_array()) {
    throw std::invalid_argument(std::string("UI method requires array ") + field);
  }
  std::vector<std::string> result;
  for (const auto& item : arguments.at(field)) {
    if (!item.is_string()) {
      throw std::invalid_argument(std::string(field) + " must contain strings");
    }
    result.push_back(item.get<std::string>());
  }
  return result;
}

template <typename T, typename Show>
Result<Json> wait_for_dialog(InteractiveTerminalSession& session,
                             ExtensionUiPump& pump, const Json& arguments,
                             Show show, T cancelled_value) {
  if (!pump) {
    return Result<Json>::failure(
        ErrorCode::invalid_argument,
        "interactive extension UI requires a terminal input pump");
  }

  std::optional<std::chrono::steady_clock::time_point> deadline;
  if (arguments.is_object() && arguments.contains("timeout") &&
      arguments.at("timeout").is_number_integer()) {
    const auto timeout = arguments.at("timeout").get<std::int64_t>();
    if (timeout < 0) {
      return Result<Json>::failure(ErrorCode::invalid_argument,
                                   "UI timeout must not be negative");
    }
    deadline = std::chrono::steady_clock::now() +
               std::chrono::milliseconds(timeout);
  }

  struct Pending {
    bool done = false;
    bool cancelled = false;
    T value{};
  } pending;
  const auto cancel = [&pending, cancelled_value]() {
    pending.done = true;
    pending.cancelled = true;
    pending.value = cancelled_value;
  };
  show(pending, cancel);

  while (!pending.done) {
    if (deadline.has_value() && std::chrono::steady_clock::now() >= *deadline) {
      session.hide_overlay();
      pending.done = true;
      pending.cancelled = true;
      pending.value = cancelled_value;
      break;
    }
    const auto pumped = pump();
    if (!pumped) {
      session.hide_overlay();
      return Result<Json>::failure(pumped.error().code, pumped.error().message,
                                   pumped.error().system_error);
    }
    if (!pending.done && deadline.has_value()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  if (pending.cancelled && std::is_same_v<T, std::string>) {
    return Result<Json>::success(nullptr);
  }
  return Result<Json>::success(Json(pending.value));
}

template <typename T>
Result<Json> wait_for_dialog_without_timeout(
    InteractiveTerminalSession& session, ExtensionUiPump& pump, const Json& args,
    T cancelled_value,
    std::function<void(std::function<void(T)>, std::function<void()>)> show) {
  return wait_for_dialog<T>(
      session, pump, args,
      [&](auto& pending, const auto& cancel) {
        show([&pending](T value) {
          pending.done = true;
          pending.value = std::move(value);
        }, cancel);
      },
      std::move(cancelled_value));
}

}  // namespace

qt::extensions::ExtensionHostCall make_interactive_extension_ui_host_call(
    InteractiveTerminalSession& session, InteractiveExtensionUiState& state,
    ExtensionUiPump pump, std::function<Status(std::string)> set_title,
    std::function<void()> invalidate) {
  return [&session, &state, pump = std::move(pump),
          set_title = std::move(set_title), invalidate = std::move(invalidate)](
             std::string_view method, const Json& arguments) mutable -> Result<Json> {
    try {
      if (method == "ctx.ui.select") {
        const auto title = required_string(arguments, "title");
        const auto options = required_strings(arguments, "options");
        return wait_for_dialog_without_timeout<std::string>(
            session, pump, arguments, std::string{},
            [&](auto on_submit, auto on_cancel) {
              session.show_extension_select(title, options, std::move(on_submit),
                                             [on_cancel](InteractiveTerminalSession&) {
                                               on_cancel();
                                               return true;
                                             });
            });
      }
      if (method == "ctx.ui.confirm") {
        const auto title = required_string(arguments, "title");
        const auto message = required_string(arguments, "message");
        return wait_for_dialog_without_timeout<bool>(
            session, pump, arguments, false,
            [&](auto on_submit, auto on_cancel) {
              session.show_extension_confirm(title, message, std::move(on_submit),
                                              [on_cancel](InteractiveTerminalSession&) {
                                                on_cancel();
                                                return true;
                                              });
            });
      }
      if (method == "ctx.ui.input") {
        const auto title = required_string(arguments, "title");
        const auto placeholder = arguments.value("placeholder", std::string{});
        return wait_for_dialog_without_timeout<std::string>(
            session, pump, arguments, std::string{},
            [&](auto on_submit, auto on_cancel) {
              session.show_extension_input(title, placeholder, std::move(on_submit),
                                            [on_cancel](InteractiveTerminalSession&) {
                                              on_cancel();
                                              return true;
                                            });
            });
      }
      if (method == "ctx.ui.editor") {
        const auto title = required_string(arguments, "title");
        const auto prefill = arguments.value("prefill", std::string{});
        return wait_for_dialog_without_timeout<std::string>(
            session, pump, arguments, std::string{},
            [&](auto on_submit, auto on_cancel) {
              session.show_extension_editor(title, prefill, std::move(on_submit),
                                             [on_cancel](InteractiveTerminalSession&) {
                                               on_cancel();
                                               return true;
                                             });
            });
      }
      if (method == "ctx.ui.notify") {
        session.show_extension_notify(required_string(arguments, "message"),
                                      arguments.value("notifyType", "info"));
        if (invalidate) invalidate();
        return Result<Json>::success(nullptr);
      }
      if (method == "ctx.ui.setStatus") {
        const auto key = required_string(arguments, "statusKey");
        if (arguments.contains("statusText") && !arguments.at("statusText").is_null()) {
          state.statuses[key] = required_string(arguments, "statusText");
        } else {
          state.statuses.erase(key);
        }
        if (invalidate) invalidate();
        return Result<Json>::success(nullptr);
      }
      if (method == "ctx.ui.setWorkingMessage") {
        if (arguments.contains("message") && !arguments.at("message").is_null()) {
          state.working_message = required_string(arguments, "message");
        } else {
          state.working_message.reset();
        }
        if (state.apply_working_message) {
          state.apply_working_message(state.working_message);
        }
        if (invalidate) invalidate();
        return Result<Json>::success(nullptr);
      }
      if (method == "ctx.ui.setWorkingVisible") {
        state.working_visible = arguments.value("visible", true);
        if (state.apply_working_visible) state.apply_working_visible(state.working_visible);
        if (invalidate) invalidate();
        return Result<Json>::success(nullptr);
      }
      if (method == "ctx.ui.setWorkingIndicator") {
        if (!arguments.contains("frames") || arguments.at("frames").is_null()) {
          state.working_frames.reset();
        } else {
          state.working_frames = required_strings(arguments, "frames");
        }
        if (state.apply_working_indicator) {
          const auto interval = arguments.value("intervalMs", 80U);
          state.apply_working_indicator(state.working_frames, interval);
        }
        if (invalidate) invalidate();
        return Result<Json>::success(nullptr);
      }
      if (method == "ctx.ui.setHiddenThinkingLabel") {
        if (arguments.contains("label") && !arguments.at("label").is_null()) {
          state.hidden_thinking_label = required_string(arguments, "label");
        } else {
          state.hidden_thinking_label.reset();
        }
        if (state.apply_hidden_thinking_label) {
          state.apply_hidden_thinking_label(state.hidden_thinking_label);
        }
        if (invalidate) invalidate();
        return Result<Json>::success(nullptr);
      }
      if (method == "ctx.ui.getToolsExpanded") {
        return Result<Json>::success(state.read_tools_expanded
                                        ? state.read_tools_expanded()
                                        : state.tools_expanded);
      }
      if (method == "ctx.ui.setToolsExpanded") {
        state.tools_expanded = arguments.value("expanded", false);
        if (state.apply_tools_expanded) state.apply_tools_expanded(state.tools_expanded);
        if (invalidate) invalidate();
        return Result<Json>::success(nullptr);
      }
      if (method == "ctx.ui.setWidget") {
        if (!state.apply_widgets) return unsupported(std::string(method));
        const auto key = required_string(arguments, "key");
        if (!arguments.contains("content") || arguments.at("content").is_null()) {
          state.widgets.erase(key);
        } else {
          state.widgets[key] = required_strings(arguments, "content");
        }
        if (state.apply_widgets) state.apply_widgets(state.widgets);
        if (invalidate) invalidate();
        return Result<Json>::success(nullptr);
      }
      if (method == "ctx.ui.setFooter" || method == "ctx.ui.setHeader") {
        const bool footer = method == "ctx.ui.setFooter";
        std::optional<std::vector<std::string>> lines;
        if (arguments.contains("lines") && !arguments.at("lines").is_null()) {
          lines = required_strings(arguments, "lines");
        } else if (arguments.contains("content") &&
                   !arguments.at("content").is_null()) {
          lines = required_strings(arguments, "content");
        }
        if (footer) {
          state.footer_lines = lines;
          if (state.apply_footer) state.apply_footer(state.footer_lines);
        } else {
          if (!state.apply_header) return unsupported(std::string(method));
          state.header_lines = lines;
          state.apply_header(state.header_lines);
        }
        if (invalidate) invalidate();
        return Result<Json>::success(nullptr);
      }
      if (method == "ctx.ui.setTitle") {
        if (!set_title) return unsupported(std::string(method));
        const auto status = set_title(required_string(arguments, "title"));
        if (!status) {
          return Result<Json>::failure(status.error().code,
                                       status.error().message,
                                       status.error().system_error);
        }
        return Result<Json>::success(nullptr);
      }
      if (method == "ctx.ui.onTerminalInput") {
        if (!arguments.contains("handler") || arguments.at("handler").is_null()) {
          session.set_extension_terminal_input_handler({});
          return Result<Json>::success(nullptr);
        }
        if (!state.script_call) return unsupported(std::string(method));
        const auto handler = required_string(arguments, "handler");
        session.set_extension_terminal_input_handler(
            [call = state.script_call, handler](std::string_view bytes)
                -> qt::foundation::Result<std::optional<std::string>> {
              auto result = call(handler, Json{{"type", "input"},
                                                {"data", std::string(bytes)}});
              if (!result) return result.error();
              if (result.value().is_null()) return std::optional<std::string>{};
              if (!result.value().is_string()) {
                return qt::foundation::Result<std::optional<std::string>>::failure(
                    ErrorCode::invalid_argument,
                    "terminal input handler must return string or null");
              }
              return std::optional<std::string>(result.value().get<std::string>());
            });
        return Result<Json>::success(nullptr);
      }
      if (method == "ctx.ui.setEditorComponent") {
        if (!arguments.contains("handler") || arguments.at("handler").is_null()) {
          session.set_extension_editor_component(nullptr);
          session.set_extension_editor_input_handler({});
          state.editor_handler.reset();
          return Result<Json>::success(nullptr);
        }
        if (!state.script_call) return unsupported(std::string(method));
        const auto handler = required_string(arguments, "handler");
        auto component = std::make_shared<ScriptedExtensionComponent>(
            [call = state.script_call, handler](const Json& event) {
              return call(handler, event);
            });
        session.set_extension_editor_component(component);
        state.editor_handler = handler;
        return Result<Json>::success(nullptr);
      }
      if (method == "ctx.ui.getEditorComponent") {
        return Result<Json>::success(state.editor_handler
            ? Json{{"handler", *state.editor_handler}} : Json(nullptr));
      }
      if (method == "ctx.ui.addAutocompleteProvider") {
        if (!state.script_call) return unsupported(std::string(method));
        const auto handler = required_string(arguments, "handler");
        session.add_extension_autocomplete_provider(
            [call = state.script_call, handler](const std::vector<std::string>& lines,
                                                std::size_t row,
                                                std::size_t column, bool force)
                -> std::optional<qt::tui::AutocompleteSuggestions> {
              auto result = call(handler, Json{{"lines", lines}, {"row", row},
                                                {"column", column}, {"force", force}});
              if (!result || result.value().is_null()) return std::nullopt;
              if (!result.value().is_object() ||
                  !result.value().contains("items") ||
                  !result.value().at("items").is_array()) return std::nullopt;
              qt::tui::AutocompleteSuggestions suggestions;
              suggestions.prefix = result.value().value("prefix", "");
              for (const auto& item : result.value().at("items")) {
                if (!item.is_object() || !item.contains("value") ||
                    !item.at("value").is_string()) continue;
                qt::tui::AutocompleteItem parsed;
                parsed.value = item.at("value").get<std::string>();
                parsed.label = item.value("label", parsed.value);
                if (item.contains("description") &&
                    item.at("description").is_string()) {
                  parsed.description = item.at("description").get<std::string>();
                }
                suggestions.items.push_back(std::move(parsed));
              }
              return suggestions;
            });
        return Result<Json>::success(nullptr);
      }
      if (method == "ctx.ui.custom") {
        if (!pump) return unsupported(std::string(method));
        if (!state.script_call) return unsupported(std::string(method));
        const auto handler = required_string(arguments, "handler");
        struct Pending {
          bool done = false;
          Json value = nullptr;
          std::optional<qt::foundation::Error> error;
        } pending;
        auto component = std::make_shared<ScriptedExtensionComponent>(
            [call = state.script_call, handler](const Json& event) {
              return call(handler, event);
            },
            [&pending](Result<Json> result) {
              pending.done = true;
              if (result) {
                pending.value = result.value();
              } else {
                pending.error = result.error();
              }
            });
        try {
          session.show_overlay(component);
        } catch (...) {
          component->dispose();
          throw;
        }
        while (!pending.done) {
          const auto pumped = pump();
          if (!pumped) {
            session.hide_overlay();
            component->dispose();
            return Result<Json>::failure(pumped.error().code,
                                         pumped.error().message,
                                         pumped.error().system_error);
          }
        }
        session.hide_overlay();
        component->dispose();
        if (pending.error) {
          return Result<Json>::failure(pending.error->code,
                                       pending.error->message,
                                       pending.error->system_error);
        }
        return Result<Json>::success(std::move(pending.value));
      }
      if (method == "ctx.ui.setEditorText") {
        session.mutable_editor().set_text(required_string(arguments, "text"));
        if (invalidate) invalidate();
        return Result<Json>::success(nullptr);
      }
      if (method == "ctx.ui.getEditorText") {
        return Result<Json>::success(session.editor().text());
      }
      if (method == "ctx.ui.pasteToEditor") {
        const auto text = required_string(arguments, "text");
        session.mutable_editor().handle_input("\x1b[200~" + text +
                                              "\x1b[201~");
        if (invalidate) invalidate();
        return Result<Json>::success(nullptr);
      }
      if (method == "ctx.ui.getAllThemes" || method == "ctx.ui.getTheme" ||
          method == "ctx.ui.setTheme") {
        if (!state.theme_call) return unsupported(std::string(method));
        return state.theme_call(method, arguments);
      }
      return Result<Json>::failure(ErrorCode::invalid_argument,
                                   "unknown extension host call: " +
                                       std::string(method));
    } catch (const std::exception& error) {
      session.hide_overlay();
      return Result<Json>::failure(ErrorCode::invalid_argument, error.what());
    }
  };
}

}  // namespace qt::app
