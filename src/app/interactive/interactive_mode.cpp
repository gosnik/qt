#include "qt/app/interactive/interactive_mode.hpp"

#include "qt/ai/images.hpp"
#include "qt/ai/llama_provider.hpp"
#include "qt/ai/openrouter_model_discovery.hpp"
#include "qt/ai/openai_model_discovery.hpp"
#include "qt/ai/ollama.hpp"
#include "qt/app/auth/auth_flow.hpp"
#include "qt/app/config/prompt_history.hpp"
#include "qt/app/config/theme_watcher.hpp"
#include "qt/app/export/export_html.hpp"
#include "qt/app/runtime/extension_contribution_runner.hpp"
#include "qt/app/runtime/extension_runtime_binding.hpp"
#include "qt/app/runtime/subagent_runtime.hpp"
#include "qt/app/runtime/subagent_coordinator.hpp"
#include "qt/tui/input/keys.hpp"
#include "qt/app/runtime/extension_host_api.hpp"
#include "qt/app/runtime/extension_lifecycle_runner.hpp"
#include "qt/app/runtime/extension_ui_presenter.hpp"
#include "qt/app/runtime/initial_message.hpp"
#include "qt/app/auth/interactive_auth_commands.hpp"
#include "qt/app/interactive/interactive_footer.hpp"
#include "qt/app/interactive/memory_dashboard_server.hpp"
#include "qt/app/interactive/interactive_shell.hpp"
#include "qt/app/interactive/interactive_startup_diagnostics.hpp"
#include "qt/app/interactive/interactive_terminal_session.hpp"
#include "qt/app/interactive/interactive_turn_pump.hpp"
#include "qt/app/interactive/interactive_working_indicator.hpp"
#include "qt/app/slash/llama_slash_commands.hpp"
#include "qt/app/runtime/noninteractive_mode.hpp"
#include "qt/app/runtime/openai_model_refresh.hpp"
#include "qt/app/runtime/extensions.hpp"
#include "qt/app/runtime/runtime.hpp"
#include "qt/app/sessions/session_tree.hpp"
#include "qt/app/sessions/session_cwd.hpp"
#include "qt/app/sessions/session_shared.hpp"
#include "qt/app/export/resource_inspection.hpp"
#include "qt/app/slash/slash_commands.hpp"
#include "qt/app/export/stream_output.hpp"
#include "qt/app/interactive/terminal_suspend.hpp"
#include "qt/extensions/loader.hpp"
#include "qt/foundation/filesystem.hpp"
#include "qt/foundation/logging.hpp"
#include "qt/foundation/process.hpp"
#include "qt/foundation/uuid.hpp"
#include "qt/memory/skill_import.hpp"
#include "qt/harness/skills.hpp"
#include "qt/harness/prompt_templates.hpp"
#include "qt/harness/goal.hpp"
#include "qt/harness/goal_store.hpp"
#include "qt/harness/repository.hpp"
#include "qt/harness/session_catalog.hpp"
#include "qt/harness/session_repository.hpp"
#include "qt/memory/runtime.hpp"
#include "qt/tools/bash_tool.hpp"
#include "qt/tools/clipboard_tool.hpp"
#include "qt/tools/memory_tool.hpp"
#include "qt/tools/mcp_client.hpp"
#include "qt/tools/package_manager_tool.hpp"
#include "qt/tools/subagent_tool.hpp"
#include "qt/tools/tool_validation.hpp"
#include "qt/tools/tools_manager.hpp"
#include "qt/tui/input/input.hpp"
#include "qt/tui/core/style.hpp"
#include "qt/tui/widgets/messages/assistant_message.hpp"
#include "qt/tui/widgets/messages/custom_message.hpp"
#include "qt/tui/widgets/messages/tool_execution.hpp"
#include "qt/tui/render/text.hpp"
#include "qt/tui/utils/line_split.hpp"
#include "qt/tui/widgets/selectors/login_dialog.hpp"
#include "qt/tui/widgets/selectors/model_selector.hpp"
#include "qt/tui/terminal/notcurses_backend.hpp"
#include "qt/tui/widgets/settings_list.hpp"
#include "qt/tui/widgets/selectors/session_selector_search.hpp"
#include "qt/tui/terminal/terminal.hpp"
#include "qt/tui/widgets/selectors/thinking_selector.hpp"
#include "qt/tui/widgets/messages/user_message.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>
#include <csignal>
#endif

namespace qt::app {
namespace {

using Json = nlohmann::json;

qt::foundation::Status persist_native_extension_entries(
    qt::app::ReferenceStartupRuntime& runtime,
    qt::app::ReferenceStartupSession& session, std::size_t baseline) {
  if (!session.persistent || !session.repository_path) {
    return qt::foundation::Status::success();
  }
  runtime.host.sync_session_manager_from_harness();
  const auto& entries = runtime.host.session_manager().get_entries();
  if (baseline > entries.size()) return qt::foundation::Status::failure(
      qt::foundation::ErrorCode::invalid_argument,
      "extension session entry baseline exceeds current entries");
  qt::harness::SessionRepository repository(*session.repository_path);
  for (std::size_t index = baseline; index < entries.size(); ++index) {
    const auto status = repository.append(qt::app::repository_ready_session_entry(entries[index]));
    if (!status) return status;
    ++session.entry_count;
  }
  return qt::foundation::Status::success();
}

std::optional<std::vector<std::string>> render_extension_custom_message(
    const qt::app::ReferenceStartupRuntime& runtime, const Json& message,
    std::size_t width, bool expanded) {
  const auto type = message.value("customType", std::string{});
  for (const auto& extension :
       runtime.services.resource_bundle.extension_host.registry.loaded_extensions) {
    if (!extension.script_runtime || !extension.host_call) continue;
    for (const auto& renderer : extension.script_runtime->registration().message_renderers) {
      if (renderer.custom_type != type) continue;
      auto event = message;
      event["type"] = "render";
      event["width"] = width;
      event["expanded"] = expanded;
      const auto rendered = extension.script_runtime->invoke(
          renderer.handler, event, *extension.host_call);
      if (!rendered) {
        std::cerr << "qt: custom message renderer: " << rendered.error().message << '\n';
        return std::nullopt;
      }
      try {
        return rendered.value().at("lines").get<std::vector<std::string>>();
      } catch (const Json::exception&) {
        std::cerr << "qt: custom message renderer must return {lines: [...]}\n";
        return std::nullopt;
      }
    }
  }
  return std::nullopt;
}

constexpr std::string_view kReferenceCliVersion = "0.0.0";

qt::tui::SelectLayoutOptions make_interactive_select_layout() {
  qt::tui::SelectLayoutOptions layout;
  layout.min_primary_column_width = 12U;
  layout.max_primary_column_width = 32U;
  return layout;
}

const qt::tui::SelectLayoutOptions kReferenceInteractiveSelectLayout =
    make_interactive_select_layout();

std::string lowercase_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string memory_stat_counts(
    const std::map<std::string, std::size_t>& counts) {
  if (counts.empty()) return "(none)";
  std::string result;
  for (const auto& [name, count] : counts) {
    if (!result.empty()) result += ", ";
    result += name + "=" + std::to_string(count);
  }
  return result;
}

std::vector<std::string> memory_statistics_lines(
    const qt::memory::MemoryStatistics& stats) {
  return {
      "  statistics:",
      "    stored memories: " + std::to_string(stats.memory_entries),
      "    by status: " + memory_stat_counts(stats.trust_states),
      "    by type: " + memory_stat_counts(stats.memory_counts),
      "    hypotheses: " + std::to_string(stats.hypotheses),
      "    low confidence: " + std::to_string(stats.low_confidence),
      "    unprovenanced: " + std::to_string(stats.unprovenanced),
      "    deprecated: " + std::to_string(stats.deprecated),
      "    accessed memories: " + std::to_string(stats.accessed_memories),
      "    total accesses: " + std::to_string(stats.total_accesses),
      "    successful outcomes: " +
          std::to_string(stats.successful_outcomes),
      "    failed outcomes: " + std::to_string(stats.failed_outcomes),
      "    failed procedures: " + std::to_string(stats.failed_procedures),
      "    high-use hypotheses: " +
          std::to_string(stats.high_use_hypotheses),
      "    conflicts: " + std::to_string(stats.conflict_count),
      "    unresolved conflicts: " +
          std::to_string(stats.unresolved_conflicts),
      "    pending reviews: " + std::to_string(stats.pending_reviews),
  };
}

void append_extension_result_text(std::vector<std::string>& lines,
                                  const Json& value) {
  if (value.is_null()) {
    return;
  }
  if (value.is_string()) {
    lines.push_back(value.get<std::string>());
    return;
  }
  if (value.is_array()) {
    for (const auto& item : value) {
      append_extension_result_text(lines, item);
    }
    return;
  }
  if (!value.is_object()) {
    lines.push_back(value.dump());
    return;
  }

  for (const char* field : {"message", "text", "output"}) {
    const auto it = value.find(field);
    if (it != value.end() && it->is_string()) {
      lines.push_back(it->get<std::string>());
      return;
    }
  }

  const auto content = value.find("content");
  if (content != value.end() && content->is_array()) {
    for (const auto& item : *content) {
      if (item.is_string()) {
        lines.push_back(item.get<std::string>());
      } else if (item.is_object()) {
        const auto type = item.find("type");
        const auto text = item.find("text");
        if (text != item.end() && text->is_string() &&
            (type == item.end() || !type->is_string() ||
             type->get<std::string>() == "text")) {
          lines.push_back(text->get<std::string>());
        }
      }
    }
    if (!lines.empty()) {
      return;
    }
  }

  lines.push_back(value.dump());
}

Json image_contents_json(const std::vector<qt::ai::ImageContent>& images) {
  if (images.empty()) {
    return Json(nullptr);
  }

  Json output = Json::array();
  for (const auto& image : images) {
    output.push_back(Json(image));
  }
  return output;
}

qt::foundation::Result<std::vector<qt::ai::ImageContent>>
image_contents_from_json(const Json& images) {
  if (images.is_null()) {
    return std::vector<qt::ai::ImageContent>{};
  }
  if (!images.is_array()) {
    return qt::foundation::Result<std::vector<qt::ai::ImageContent>>::failure(
        qt::foundation::ErrorCode::invalid_argument,
        "extension input images must be null or an array");
  }

  std::vector<qt::ai::ImageContent> output;
  output.reserve(images.size());
  try {
    for (const auto& image : images) {
      output.push_back(image.get<qt::ai::ImageContent>());
    }
  } catch (const std::exception& error) {
    return qt::foundation::Result<std::vector<qt::ai::ImageContent>>::failure(
        qt::foundation::ErrorCode::invalid_argument,
        std::string("extension input image was invalid: ") + error.what());
  }
  return output;
}

std::vector<std::string> extension_command_result_lines(
    std::string_view invocation_name,
    const qt::extensions::ExtensionContributionResult& result) {
  std::vector<std::string> lines;
  if (!result.diagnostics.empty()) {
    lines.insert(lines.end(), result.diagnostics.begin(), result.diagnostics.end());
  }

  if (result.ok) {
    append_extension_result_text(lines, result.value);
    if (lines.empty()) {
      lines.push_back("Extension command /" + std::string(invocation_name) +
                      " completed.");
    }
    return lines;
  }

  std::string message = "extension command /";
  message += invocation_name;
  message += " failed";
  if (result.error.has_value() && !result.error->message.empty()) {
    message += ": ";
    message += result.error->message;
  }
  lines.push_back(std::move(message));
  return lines;
}

bool ends_with_case_insensitive(std::string_view text, std::string_view suffix) {
  if (suffix.size() > text.size()) {
    return false;
  }

  const std::size_t offset = text.size() - suffix.size();
  for (std::size_t index = 0; index < suffix.size(); ++index) {
    const char lhs = static_cast<char>(
        std::tolower(static_cast<unsigned char>(text[offset + index])));
    const char rhs = static_cast<char>(
        std::tolower(static_cast<unsigned char>(suffix[index])));
    if (lhs != rhs) {
      return false;
    }
  }
  return true;
}

bool env_flag_enabled(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return false;
  }

  std::string normalized = lowercase_ascii(value);
  return normalized == "1" || normalized == "true" || normalized == "yes" ||
         normalized == "on";
}

std::vector<std::string> split_display_lines(std::string_view text) {
  return qt::tui::split_lines_copy(text);
}

const std::vector<std::string>& tool_entry_compaction_builtin_tools() {
  static const std::vector<std::string> tools{
      "append_text",        "apply_patch",       "bash",
      "clipboard",          "complete_path",     "edit_text",
      "find",               "index",             "list_directory",
      "list_mcp_resources", "package_manager",   "read_text",
      "read_mcp_resource",  "search_text",       "web_fetch",
      "web_search",         "web_run",          "write_text",
  };
  return tools;
}

bool contains_string(const std::vector<std::string>& values,
                     const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

std::vector<std::string> normalize_tool_entry_compaction_tools(
    std::vector<std::string> tools) {
  std::vector<std::string> unique;
  unique.reserve(tools.size());
  for (auto& tool : tools) {
    if (!tool.empty() && !contains_string(unique, tool)) {
      unique.push_back(std::move(tool));
    }
  }

  std::vector<std::string> ordered;
  ordered.reserve(unique.size());
  for (const auto& builtin : tool_entry_compaction_builtin_tools()) {
    if (contains_string(unique, builtin)) {
      ordered.push_back(builtin);
    }
  }
  for (const auto& tool : unique) {
    if (!contains_string(tool_entry_compaction_builtin_tools(), tool)) {
      ordered.push_back(tool);
    }
  }
  return ordered;
}

std::string tool_entry_compaction_summary(
    const std::vector<std::string>& enabled_tools) {
  std::size_t enabled = 0U;
  for (const auto& tool : tool_entry_compaction_builtin_tools()) {
    if (contains_string(enabled_tools, tool)) {
      ++enabled;
    }
  }
  return std::to_string(enabled) + "/" +
         std::to_string(tool_entry_compaction_builtin_tools().size()) + " on";
}

std::string sanitize_terminal_title(std::string title) {
  for (char &ch : title) {
    const auto byte = static_cast<unsigned char>(ch);
    if (byte < 0x20U || byte == 0x7FU) {
      ch = ' ';
    }
  }
  return title;
}

std::string cwd_terminal_title_suffix() {
  std::error_code error;
  const auto cwd = std::filesystem::current_path(error);
  if (error || cwd.empty()) {
    return {};
  }
  auto name = cwd.filename().string();
  if (name.empty()) {
    name = cwd.string();
  }
  return sanitize_terminal_title(std::move(name));
}

std::vector<std::string> compaction_transcript_lines(std::string_view reason,
                                                     std::string_view summary) {
  std::vector<std::string> lines;
  lines.push_back(reason == "manual"
                      ? "Compacted the active session context."
                      : "Auto-compacted the active session context.");
  auto summary_lines = split_display_lines(summary);
  if (summary_lines.empty()) {
    lines.push_back("(empty compaction summary)");
  } else {
    lines.insert(lines.end(), std::make_move_iterator(summary_lines.begin()),
                 std::make_move_iterator(summary_lines.end()));
  }
  return lines;
}

std::vector<std::string> compaction_transcript_lines(
    std::string_view reason,
    const qt::harness::ModelBackedCompactionResult& result) {
  return compaction_transcript_lines(reason, result.summary);
}

std::string compact_transcript_json(const Json& value) {
  if (value.is_discarded() || value.is_null()) {
    return "";
  }
  return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::string pretty_transcript_json(const Json& value) {
  if (value.is_discarded() || value.is_null()) {
    return "";
  }
  return value.dump(2, ' ', false, Json::error_handler_t::replace);
}

std::string truncate_transcript_text(std::string text,
                                     std::size_t max_size = 4000U) {
  if (text.size() <= max_size) {
    return text;
  }
  text.resize(max_size);
  text += "\n[truncated]";
  return text;
}

void append_labeled_transcript_block(std::vector<std::string>& lines,
                                     std::string label,
                                     std::string text) {
  text = truncate_transcript_text(std::move(text));
  if (text.empty()) {
    lines.push_back(std::move(label));
    return;
  }

  const auto block_lines = split_display_lines(text);
  lines.push_back(std::move(label));
  for (const auto& line : block_lines) {
    lines.push_back("  " + line);
  }
}

std::string thinking_transcript_line(std::string_view line) {
  return qt::tui::style::sgr("3;38;5;244", line);
}

std::string tool_pending_background(std::string_view line) {
  return qt::tui::style::background_256(234, line);
}

std::string tool_success_background(std::string_view line) {
  return tool_pending_background(line);
}

std::string tool_error_background(std::string_view line) {
  return tool_pending_background(line);
}

std::string tool_title_color(std::string_view line) {
  return qt::tui::style::bold(qt::tui::style::foreground_256(117, line));
}

std::string tool_args_color(std::string_view line) {
  return qt::tui::style::foreground_256(250, line);
}

std::string tool_result_color(std::string_view line) {
  return qt::tui::style::foreground_256(250, line);
}

qt::tools::ToolRequest tool_request_from_call(
    const qt::ai::ToolCallContent& call) {
  qt::tools::ToolRequest request;
  request.tool_name = call.name;
  request.arguments = call.arguments;
  if (!call.id.empty()) {
    request.call_id = call.id;
  }
  return request;
}

qt::tools::ToolResult invalid_tool_result(
    const qt::tools::ToolRequest& request,
    qt::tools::ToolResultStatus status, std::string error_message) {
  qt::tools::ToolResult result;
  result.status = status;
  result.call_id = request.call_id;
  result.error_message = std::move(error_message);
  return result;
}

qt::tools::ToolResult invoke_interactive_tool_request(
    const qt::tools::ToolRequest& request,
    const std::shared_ptr<qt::tools::ToolRegistry>& registry,
    qt::tools::ToolContext context) {
  auto log = qt::foundation::logger("interactive", "tools");
  log.info("tool_invoke_begin",
           {qt::foundation::log_field("tool", request.tool_name),
            qt::foundation::log_field("call_id",
                                      request.call_id.value_or(""))});
  if (!qt::tools::is_valid(request)) {
    log.warn("tool_invoke_invalid",
             {qt::foundation::log_field("tool", request.tool_name)});
    return invalid_tool_result(
        request, qt::tools::ToolResultStatus::invalid_request,
        "provider returned an invalid tool call request");
  }
  if (!registry) {
    log.warn("tool_invoke_missing_registry",
             {qt::foundation::log_field("tool", request.tool_name)});
    return invalid_tool_result(request, qt::tools::ToolResultStatus::not_found,
                               "agent tool registry is not configured");
  }
  const auto* tool = registry->find(request.tool_name);
  if (tool == nullptr) {
    log.warn("tool_invoke_not_found",
             {qt::foundation::log_field("tool", request.tool_name)});
    return invalid_tool_result(request, qt::tools::ToolResultStatus::not_found,
                               "requested tool is not registered");
  }
  auto prepared = registry->prepare_request(request);
  const auto definitions = registry->descriptors();
  const auto definition = std::find_if(
      definitions.begin(), definitions.end(),
      [&](const qt::tools::ToolDescriptor& candidate) {
        return candidate.name == request.tool_name;
      });
  if (definition != definitions.end()) {
    qt::ai::ToolDefinition ai_definition{
        definition->name, definition->description, definition->input_schema};
    qt::ai::ToolCallContent call;
    call.id = request.call_id.value_or("");
    call.name = request.tool_name;
    call.arguments = prepared.arguments;
    const auto validated =
        qt::ai::validate_tool_arguments(ai_definition, call);
    if (!validated) {
      auto result = invalid_tool_result(
          request, qt::tools::ToolResultStatus::invalid_request,
          validated.error().message);
      result.output =
          qt::ai::Json{{"code", "invalid_arguments"},
                       {"tool", request.tool_name},
                       {"retryable", true},
                       {"received_arguments", prepared.arguments},
                       {"expected_schema", definition->input_schema}};
      return result;
    }
    prepared.arguments = validated.value();
  }
  qt::tools::ToolResult result = tool->invoke(prepared, context);
  result.call_id = request.call_id;
  log.info("tool_invoke_end",
           {qt::foundation::log_field("tool", request.tool_name),
            qt::foundation::log_field("call_id",
                                      request.call_id.value_or("")),
            qt::foundation::log_field("status",
                                      qt::tools::to_string(result.status))});
  return result;
}

qt::ai::Message tool_result_message(const qt::tools::ToolRequest& request,
                                    const qt::tools::ToolResult& result) {
  qt::ai::ToolResultContent content;
  content.tool_call_id = request.call_id.value_or("");
  content.name = request.tool_name;
  content.is_error = !qt::tools::is_success(result);
  content.result = qt::ai::Json{{"status", qt::tools::to_string(result.status)},
                                {"output", result.output}};
  if (result.error_message) {
    content.result["error"] = *result.error_message;
  }

  qt::ai::Message message;
  message.role = qt::ai::MessageRole::tool;
  message.name = request.tool_name;
  message.content.push_back(std::move(content));
  return message;
}

struct LiveTranscriptUpdate {
  struct Item {
    enum class Kind {
      kLine,
      kComponent,
    };

    Kind kind = Kind::kLine;
    std::string line;
    std::string tool_name;
    std::string tool_call_id;
    std::string tool_arguments;
    qt::tui::ComponentPtr component;
  };

  std::vector<Item> items;
  bool dirty = false;

  [[nodiscard]] bool empty() const noexcept { return items.empty() && !dirty; }

  void mark_dirty() noexcept { dirty = true; }

  void add_line(std::string line) {
    Item item;
    item.kind = Item::Kind::kLine;
    item.line = std::move(line);
    items.push_back(std::move(item));
  }

  void add_component(qt::tui::ComponentPtr component) {
    if (!component) {
      return;
    }
    Item item;
    item.kind = Item::Kind::kComponent;
    item.component = std::move(component);
    items.push_back(std::move(item));
  }

  void add_tool_component(std::string tool_name, std::string tool_call_id,
                          std::string tool_arguments,
                          qt::tui::ComponentPtr component) {
    if (!component) {
      return;
    }
    Item item;
    item.kind = Item::Kind::kComponent;
    item.tool_name = std::move(tool_name);
    item.tool_call_id = std::move(tool_call_id);
    item.tool_arguments = std::move(tool_arguments);
    item.component = std::move(component);
    items.push_back(std::move(item));
  }
};

bool provider_event_content_type_is(const qt::ai::StreamEvent& event,
                                    std::string_view expected) {
  if (!event.metadata.is_object() || !event.metadata.contains("content_type") ||
      !event.metadata.at("content_type").is_string()) {
    return false;
  }
  return event.metadata.at("content_type").get<std::string>() == expected;
}

std::string provider_event_content_type(const qt::ai::StreamEvent& event) {
  if (!event.metadata.is_object() || !event.metadata.contains("content_type") ||
      !event.metadata.at("content_type").is_string()) {
    return "";
  }
  return event.metadata.at("content_type").get<std::string>();
}

std::size_t provider_event_content_index(
    const qt::ai::StreamEvent& event,
    std::size_t fallback = 0U) {
  if (!event.metadata.is_object() || !event.metadata.contains("content_index")) {
    return fallback;
  }

  const auto& value = event.metadata.at("content_index");
  if (value.is_number_unsigned()) {
    return static_cast<std::size_t>(value.get<std::uint64_t>());
  }
  if (value.is_number_integer()) {
    const auto index = value.get<std::int64_t>();
    if (index >= 0) {
      return static_cast<std::size_t>(index);
    }
  }
  return fallback;
}

bool provider_event_is_text_delta(const qt::ai::StreamEvent& event) {
  const auto content_type = provider_event_content_type(event);
  if (content_type == "text") {
    return true;
  }
  if (!content_type.empty()) {
    return false;
  }
  return !event.content ||
         qt::ai::content_kind(*event.content) == qt::ai::ContentKind::text;
}

std::string tool_result_output_text(const qt::tools::ToolResult& result) {
  if (result.output.is_object()) {
    const auto output = result.output.find("output");
    if (output != result.output.end() && output->is_string()) {
      return output->get<std::string>();
    }
    const auto text = result.output.find("text");
    if (text != result.output.end() && text->is_string()) {
      return text->get<std::string>();
    }
  }
  if (result.error_message && !result.error_message->empty()) {
    return *result.error_message;
  }
  return compact_transcript_json(result.output);
}

void append_content_transcript_lines(std::vector<std::string>& lines,
                                     const qt::ai::Content& content) {
  switch (qt::ai::content_kind(content)) {
    case qt::ai::ContentKind::text: {
      const auto& text = std::get<qt::ai::TextContent>(content).text;
      if (!text.empty()) {
        auto text_lines = split_display_lines(text);
        lines.insert(lines.end(), std::make_move_iterator(text_lines.begin()),
                     std::make_move_iterator(text_lines.end()));
      }
      break;
    }
    case qt::ai::ContentKind::thinking: {
      const auto& thinking = std::get<qt::ai::ThinkingContent>(content);
      append_labeled_transcript_block(
          lines, "thinking:",
          thinking.redacted ? std::string("[redacted]") : thinking.text);
      break;
    }
    case qt::ai::ContentKind::image: {
      const auto& image = std::get<qt::ai::ImageContent>(content);
      lines.push_back("[image: " + image.media_type + "]");
      break;
    }
    case qt::ai::ContentKind::tool_call: {
      const auto& call = std::get<qt::ai::ToolCallContent>(content);
      append_labeled_transcript_block(
          lines, "tool call: " + call.name,
          compact_transcript_json(call.arguments));
      break;
    }
    case qt::ai::ContentKind::tool_result: {
      const auto& result = std::get<qt::ai::ToolResultContent>(content);
      std::string label = "tool result: " + result.name;
      label += result.is_error ? " error" : " success";
      append_labeled_transcript_block(lines, std::move(label),
                                      compact_transcript_json(result.result));
      break;
    }
  }
}

std::string content_delta_text(const qt::ai::StreamEvent& event) {
  if (event.delta && !event.delta->empty()) {
    return *event.delta;
  }
  if (!event.content) {
    return "";
  }
  switch (qt::ai::content_kind(*event.content)) {
    case qt::ai::ContentKind::text:
      return std::get<qt::ai::TextContent>(*event.content).text;
    case qt::ai::ContentKind::thinking:
      return std::get<qt::ai::ThinkingContent>(*event.content).text;
    case qt::ai::ContentKind::tool_call:
    case qt::ai::ContentKind::tool_result:
    case qt::ai::ContentKind::image:
      return "";
  }
  return "";
}

std::string thinking_delta_text(const qt::ai::StreamEvent& event) {
  if (event.content &&
      qt::ai::content_kind(*event.content) == qt::ai::ContentKind::thinking) {
    return content_delta_text(event);
  }
  if (provider_event_content_type_is(event, "thinking")) {
    return content_delta_text(event);
  }
  return "";
}

std::string tool_request_key(const qt::tools::ToolRequest& request) {
  if (request.call_id && !request.call_id->empty()) {
    return *request.call_id;
  }
  return request.tool_name + ":" + compact_transcript_json(request.arguments);
}

std::string tool_result_key(const qt::tools::ToolRequest* request,
                            const qt::tools::ToolResult& result) {
  if (result.call_id && !result.call_id->empty()) {
    return *result.call_id;
  }
  if (request != nullptr) {
    return tool_request_key(*request);
  }
  return compact_transcript_json(result.output);
}

class LiveTurnTranscript {
 public:
  void set_hidden_thinking_label(std::optional<std::string> label) {
    std::lock_guard<std::mutex> lock(mutex_);
    hidden_thinking_label_ = label.value_or(std::string("Thinking..."));
    if (thinking_component_ && hide_thinking_) {
      thinking_component_->set_text(thinking_transcript_line(hidden_thinking_label_));
    }
  }

  void set_hide_thinking(bool hide, std::string label) {
    std::lock_guard<std::mutex> lock(mutex_);
    hide_thinking_ = hide;
    hidden_thinking_label_ = std::move(label);
    if (thinking_component_) {
      if (hide_thinking_) {
        thinking_component_->set_text(thinking_transcript_line(hidden_thinking_label_));
      } else if (!thinking_text_.empty()) {
        thinking_component_->set_text(thinking_transcript_line(thinking_text_));
      }
    }
  }

  void start() {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = true;
    queued_events_.clear();
    saw_thinking_delta_ = false;
    rendered_assistant_output_ = false;
    text_blocks_.clear();
    assistant_message_ = qt::ai::Message{};
    assistant_message_.role = qt::ai::MessageRole::assistant;
    assistant_component_.reset();
    thinking_text_.clear();
    thinking_component_.reset();
    shown_tool_calls_.clear();
    shown_tool_results_.clear();
    tool_components_.clear();
  }

  void stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = false;
    queued_events_.clear();
    text_blocks_.clear();
    assistant_message_ = qt::ai::Message{};
    assistant_message_.role = qt::ai::MessageRole::assistant;
    assistant_component_.reset();
    thinking_text_.clear();
    thinking_component_.reset();
    tool_components_.clear();
  }

  void push(const qt::agent::AgentEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_) {
      queued_events_.push_back(event);
    }
  }

  [[nodiscard]] bool has_pending_update() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !queued_events_.empty();
  }

  [[nodiscard]] bool has_immediate_pending_update() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(queued_events_.begin(), queued_events_.end(),
                       [](const qt::agent::AgentEvent& event) {
                         return event_requires_immediate_render(event);
                       });
  }

  [[nodiscard]] bool rendered_assistant_output() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rendered_assistant_output_;
  }

  LiveTranscriptUpdate drain_update() {
    std::vector<qt::agent::AgentEvent> events;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      events.swap(queued_events_);
    }

    LiveTranscriptUpdate update;
    for (const auto& event : events) {
      append_event_update(update, event);
    }
    return update;
  }

 private:
  static bool event_requires_immediate_render(const qt::agent::AgentEvent& event) {
    switch (event.kind) {
      case qt::agent::AgentEventKind::provider_event:
        return event.provider_event &&
               event.provider_event->kind != qt::ai::EventKind::content_delta;
      case qt::agent::AgentEventKind::tool_execution_started:
      case qt::agent::AgentEventKind::tool_executed:
      case qt::agent::AgentEventKind::subagent_event:
      case qt::agent::AgentEventKind::auto_retry_started:
      case qt::agent::AgentEventKind::turn_failed:
        return true;
      case qt::agent::AgentEventKind::tool_execution_updated:
      case qt::agent::AgentEventKind::message_queued:
      case qt::agent::AgentEventKind::turn_started:
      case qt::agent::AgentEventKind::provider_request:
      case qt::agent::AgentEventKind::message_committed:
      case qt::agent::AgentEventKind::auto_retry_completed:
      case qt::agent::AgentEventKind::turn_completed:
        break;
    }
    return false;
  }

  void append_thinking_update(LiveTranscriptUpdate& update,
                             std::string text) {
    if (text.empty()) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    saw_thinking_delta_ = true;

    if (hide_thinking_) {
      if (!thinking_component_) {
        thinking_component_ = std::make_shared<qt::tui::Text>(
            thinking_transcript_line(hidden_thinking_label_), 1, 0);
        update.add_component(thinking_component_);
      }
      thinking_text_ += text;
      return;
    }

    if (!thinking_component_) {
      thinking_text_ = text;
      thinking_component_ = std::make_shared<qt::tui::Text>(
          thinking_transcript_line(thinking_text_), 1, 0);
      update.add_component(thinking_component_);
      return;
    }
    thinking_text_ += text;
    thinking_component_->set_text(thinking_transcript_line(thinking_text_));
    update.mark_dirty();
  }

  void rebuild_assistant_message_locked() {
    assistant_message_.content.clear();
    for (const auto& [index, text] : text_blocks_) {
      (void)index;
      if (!text.empty()) {
        assistant_message_.content.emplace_back(
            qt::ai::TextContent{text, std::nullopt});
      }
    }
  }

  void append_assistant_text_update(LiveTranscriptUpdate& update,
                                    const qt::ai::StreamEvent& event,
                                    std::string text,
                                    bool replace) {
    if (text.empty()) {
      return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (event.message_id && !event.message_id->empty()) {
      assistant_message_.id = event.message_id;
    }

    const auto index = provider_event_content_index(event);
    auto& block = text_blocks_[index];
    if (replace) {
      block = std::move(text);
    } else {
      block += text;
    }
    rebuild_assistant_message_locked();

    if (!assistant_component_) {
      assistant_component_ =
          std::make_shared<qt::tui::AssistantMessageComponent>(
              &assistant_message_, hide_thinking_, hidden_thinking_label_,
              thinking_transcript_line, qt::tui::style::red);
      update.add_component(assistant_component_);
    } else {
      assistant_component_->update_content(assistant_message_);
      update.mark_dirty();
    }
    rendered_assistant_output_ = true;
  }

  std::shared_ptr<qt::tui::ToolExecutionComponent>
  tool_component_for_request(LiveTranscriptUpdate& update,
                             const qt::tools::ToolRequest& request) {
    const std::string key = tool_request_key(request);
    const auto existing = tool_components_.find(key);
    if (existing != tool_components_.end()) {
      return existing->second;
    }

    auto component = std::make_shared<qt::tui::ToolExecutionComponent>(
        request.tool_name, request.call_id.value_or(key),
        pretty_transcript_json(request.arguments), tool_title_color,
        tool_args_color, tool_result_color, qt::tui::style::red,
        tool_pending_background);
    component->set_background_colors(tool_pending_background,
                                     tool_success_background,
                                     tool_error_background);
    component->set_running();
    tool_components_.emplace(key, component);
    update.add_tool_component(request.tool_name, request.call_id.value_or(key),
                              pretty_transcript_json(request.arguments),
                              component);
    return component;
  }

  void append_tool_call_update(LiveTranscriptUpdate& update,
                              const qt::tools::ToolRequest& request) {
    const std::string key = tool_request_key(request);
    if (!shown_tool_calls_.insert(key).second) {
      return;
    }
    (void)tool_component_for_request(update, request);
  }

  void append_tool_result_update(LiveTranscriptUpdate& update,
                                const qt::tools::ToolRequest* request,
                                const qt::tools::ToolResult& result,
                                bool completed) {
    const std::string key = tool_result_key(request, result);
    std::shared_ptr<qt::tui::ToolExecutionComponent> component;
    if (request != nullptr) {
      component = tool_component_for_request(update, *request);
    } else {
      const auto existing = tool_components_.find(key);
      if (existing != tool_components_.end()) {
        component = existing->second;
      }
    }
    if (!component) {
      qt::tools::ToolRequest synthetic_request;
      synthetic_request.tool_name = "tool result";
      synthetic_request.arguments = Json::object();
      synthetic_request.call_id = key;
      component = tool_component_for_request(update, synthetic_request);
    }

    const std::string output =
        truncate_transcript_text(tool_result_output_text(result), 600U);
    if (completed) {
      if (!shown_tool_results_.insert(key).second) {
        return;
      }
      component->set_result(
          output, result.status != qt::tools::ToolResultStatus::success);
      update.mark_dirty();
      return;
    }

    if (!output.empty()) {
      component->append_result(output);
      update.mark_dirty();
    }
  }

  void append_provider_event_update(LiveTranscriptUpdate& update,
                                   const qt::ai::StreamEvent& event) {
    if (event.kind == qt::ai::EventKind::content_delta) {
      if (provider_event_is_text_delta(event)) {
        append_assistant_text_update(update, event, content_delta_text(event),
                                     false);
      } else {
        const auto thinking_text = thinking_delta_text(event);
        if (!thinking_text.empty()) {
          append_thinking_update(update, thinking_text);
        }
      }
      return;
    }
    if (event.kind != qt::ai::EventKind::content_done) {
      return;
    }

    if (!event.content) {
      if (!saw_thinking_delta_ &&
          provider_event_content_type_is(event, "thinking")) {
        append_thinking_update(update, content_delta_text(event));
      }
      return;
    }

    switch (qt::ai::content_kind(*event.content)) {
      case qt::ai::ContentKind::text:
        append_assistant_text_update(update, event, content_delta_text(event),
                                     true);
        break;
      case qt::ai::ContentKind::thinking:
        if (!saw_thinking_delta_) {
          const auto& thinking =
              std::get<qt::ai::ThinkingContent>(*event.content);
          append_thinking_update(update,
                                 thinking.redacted ? std::string("[redacted]")
                                                  : thinking.text);
        }
        break;
      case qt::ai::ContentKind::tool_call: {
        const auto& call = std::get<qt::ai::ToolCallContent>(*event.content);
        qt::tools::ToolRequest request;
        request.tool_name = call.name;
        request.arguments = call.arguments;
        request.call_id = call.id;
        append_tool_call_update(update, request);
        break;
      }
      case qt::ai::ContentKind::image:
      case qt::ai::ContentKind::tool_result:
        break;
    }
  }

  void append_event_update(LiveTranscriptUpdate& update,
                          const qt::agent::AgentEvent& event) {
    switch (event.kind) {
      case qt::agent::AgentEventKind::subagent_event:
        if (event.subagent) {
          update.add_line("subagent: " + event.subagent->value("child_id", "") +
                          " " + event.subagent->value("status", "updated"));
        }
        break;
      case qt::agent::AgentEventKind::provider_event:
        if (event.provider_event) {
          append_provider_event_update(update, *event.provider_event);
        }
        break;
      case qt::agent::AgentEventKind::tool_execution_started:
        if (event.tool_request) {
          append_tool_call_update(update, *event.tool_request);
        }
        break;
      case qt::agent::AgentEventKind::tool_execution_updated:
        if (event.tool_result) {
          append_tool_result_update(
              update, event.tool_request ? &*event.tool_request : nullptr,
              *event.tool_result, false);
        }
        break;
      case qt::agent::AgentEventKind::tool_executed:
        if (event.tool_result) {
          append_tool_result_update(
              update, event.tool_request ? &*event.tool_request : nullptr,
              *event.tool_result, true);
        }
        break;
      case qt::agent::AgentEventKind::auto_retry_started:
        if (event.error) {
          update.add_line("retrying: " + *event.error);
        }
        break;
      case qt::agent::AgentEventKind::turn_failed:
        if (event.error) {
          update.add_line("error: " + *event.error);
        }
        break;
      case qt::agent::AgentEventKind::message_queued:
      case qt::agent::AgentEventKind::turn_started:
      case qt::agent::AgentEventKind::provider_request:
      case qt::agent::AgentEventKind::message_committed:
      case qt::agent::AgentEventKind::auto_retry_completed:
      case qt::agent::AgentEventKind::turn_completed:
        break;
    }
  }

  mutable std::mutex mutex_;
  bool active_ = false;
  bool hide_thinking_ = false;
  std::string hidden_thinking_label_ = "Thinking...";
  std::vector<qt::agent::AgentEvent> queued_events_;
  bool rendered_assistant_output_ = false;
  std::map<std::size_t, std::string> text_blocks_;
  qt::ai::Message assistant_message_;
  std::shared_ptr<qt::tui::AssistantMessageComponent> assistant_component_;
  bool saw_thinking_delta_ = false;
  std::string thinking_text_;
  std::shared_ptr<qt::tui::Text> thinking_component_;
  std::unordered_set<std::string> shown_tool_calls_;
  std::unordered_set<std::string> shown_tool_results_;
  std::unordered_map<std::string, std::shared_ptr<qt::tui::ToolExecutionComponent>>
      tool_components_;
};

std::optional<bool> env_flag_override(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return std::nullopt;
  }
  return env_flag_enabled(name);
}

bool fd_input_ready(int fd, long timeout_usec) {
#if !defined(_WIN32)
  if (fd < 0) {
    return false;
  }
  fd_set ready;
  FD_ZERO(&ready);
  FD_SET(fd, &ready);
  timeval timeout{};
  timeout.tv_sec = 0;
  timeout.tv_usec = timeout_usec;
  const int selected = ::select(fd + 1, &ready, nullptr, nullptr, &timeout);
  return selected > 0 && FD_ISSET(fd, &ready);
#else
  (void)fd;
  (void)timeout_usec;
  return true;
#endif
}

std::string read_fd_available_bytes(int fd) {
  std::string bytes;
#if !defined(_WIN32)
  constexpr std::size_t kReadChunkBytes = 4096U;
  constexpr std::size_t kReadBatchLimitBytes = 64U * 1024U;
  constexpr auto kMinimumInterReadInterval =
      std::chrono::microseconds(500);
  constexpr long kInterChunkCoalesceUsec = 1000L;

  bytes.reserve(kReadChunkBytes);
  char buffer[kReadChunkBytes];
  auto next_read_at = std::chrono::steady_clock::time_point{};
  while (bytes.size() < kReadBatchLimitBytes) {
    if (next_read_at != std::chrono::steady_clock::time_point{}) {
      std::this_thread::sleep_until(next_read_at);
      if (!fd_input_ready(fd, 0L)) {
        break;
      }
    }
    const auto remaining = kReadBatchLimitBytes - bytes.size();
    const auto read_size = std::min(remaining, kReadChunkBytes);
    const auto count = ::read(fd, buffer, read_size);
    if (count <= 0) {
      break;
    }
    bytes.append(buffer, static_cast<std::size_t>(count));
    next_read_at =
        std::chrono::steady_clock::now() + kMinimumInterReadInterval;
    if (!fd_input_ready(fd, kInterChunkCoalesceUsec)) {
      break;
    }
  }
#else
  (void)fd;
  char ch = '\0';
  if (std::cin.get(ch)) {
    bytes.push_back(ch);
  }
#endif

  return bytes;
}

class TerminalInputPump {
 public:
  TerminalInputPump() = default;
  TerminalInputPump(const TerminalInputPump&) = delete;
  TerminalInputPump& operator=(const TerminalInputPump&) = delete;
  ~TerminalInputPump() { stop(); }

  [[nodiscard]] bool start() {
    if (running_.exchange(true)) {
      return true;
    }
#if !defined(_WIN32)
    fd_ = ::open("/dev/tty", O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd_ < 0) {
      running_ = false;
      return false;
    }
#endif
    worker_ = std::thread([this]() { run(); });
    return true;
  }

  void stop() {
    if (!running_.exchange(false)) {
      return;
    }
    queue_ready_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
#if !defined(_WIN32)
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
#endif
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
  }

  [[nodiscard]] bool running() const noexcept { return running_.load(); }

  std::vector<std::string> wait_and_drain(
      std::chrono::milliseconds maximum_wait) {
    std::unique_lock<std::mutex> lock(mutex_);
    queue_ready_.wait_for(lock, maximum_wait, [this]() {
      return !queue_.empty() || !running_.load();
    });
    return drain_locked();
  }

 private:
  std::vector<std::string> drain_locked() {
    std::vector<std::string> batches;
    batches.reserve(queue_.size());
    while (!queue_.empty()) {
      batches.push_back(std::move(queue_.front()));
      queue_.pop_front();
    }
    return batches;
  }

  void run() {
    constexpr long kActivePollUsec = 2000L;
    constexpr long kMaximumIdlePollUsec = 25000L;
    constexpr auto kMinimumBatchInterval = std::chrono::milliseconds(1);
    constexpr std::size_t kQueuedBatchTargetBytes = 64U * 1024U;

    long poll_timeout_usec = kActivePollUsec;
    auto next_read_at = std::chrono::steady_clock::time_point{};
    while (running_.load()) {
      if (!fd_input_ready(fd_, poll_timeout_usec)) {
        poll_timeout_usec =
            std::min(poll_timeout_usec * 2L, kMaximumIdlePollUsec);
        continue;
      }
      const auto now = std::chrono::steady_clock::now();
      if (next_read_at != std::chrono::steady_clock::time_point{} &&
          now < next_read_at) {
        std::this_thread::sleep_until(next_read_at);
      }
      if (!running_.load()) {
        break;
      }
      auto bytes = read_fd_available_bytes(fd_);
      if (bytes.empty()) {
        poll_timeout_usec =
            std::min(poll_timeout_usec * 2L, kMaximumIdlePollUsec);
        std::this_thread::sleep_for(
            std::chrono::microseconds(poll_timeout_usec));
        continue;
      }
      next_read_at =
          std::chrono::steady_clock::now() + kMinimumBatchInterval;
      poll_timeout_usec = kActivePollUsec;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!queue_.empty() &&
            queue_.back().size() + bytes.size() <= kQueuedBatchTargetBytes) {
          queue_.back().append(bytes);
        } else {
          queue_.push_back(std::move(bytes));
        }
      }
      queue_ready_.notify_one();
    }
  }

  std::atomic_bool running_{false};
  std::thread worker_;
  std::mutex mutex_;
  std::condition_variable queue_ready_;
  std::deque<std::string> queue_;
#if !defined(_WIN32)
  int fd_ = -1;
#else
  int fd_ = 0;
#endif
};

std::string trim_ascii_copy(std::string value) {
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), [](char ch) {
                return std::isspace(static_cast<unsigned char>(ch)) == 0;
              }));
  value.erase(std::find_if(value.rbegin(), value.rend(), [](char ch) {
                return std::isspace(static_cast<unsigned char>(ch)) == 0;
              }).base(),
              value.end());
  return value;
}

std::optional<std::string> parse_optional_command_path(std::string_view text) {
  const std::string trimmed = trim_ascii_copy(std::string(text));
  if (trimmed.empty()) {
    return std::nullopt;
  }

  const char first = trimmed.front();
  if (first == '"' || first == '\'') {
    const auto end = trimmed.find(first, 1);
    if (end == std::string::npos) {
      return std::nullopt;
    }
    return trimmed.substr(1, end - 1);
  }

  const auto split = trimmed.find_first_of(" \t\r\n");
  if (split == std::string::npos) {
    return trimmed;
  }
  return trimmed.substr(0, split);
}

bool path_is_equal_or_under(const std::filesystem::path& path,
                            const std::filesystem::path& root) {
  const auto normalized_path = path.lexically_normal();
  const auto normalized_root = root.lexically_normal();
  auto path_it = normalized_path.begin();
  for (auto root_it = normalized_root.begin(); root_it != normalized_root.end();
       ++root_it, ++path_it) {
    if (path_it == normalized_path.end() || *path_it != *root_it) {
      return false;
    }
  }
  return true;
}

std::string expand_home_path(std::string path) {
  if (path == "~" || path.rfind("~/", 0) == 0) {
    if (const char* home = std::getenv("HOME")) {
      return std::string(home) + path.substr(1);
    }
  }
  return path;
}

qt::foundation::Result<std::string> resolve_safe_llama_grammar_path(
    const qt::app::AppSettings& settings, const std::string& raw_path) {
  const std::string expanded = expand_home_path(raw_path);
  std::filesystem::path candidate(expanded);
  if (candidate.is_relative()) {
    candidate = std::filesystem::path(settings.cwd) / candidate;
  }

  std::error_code error;
  const auto resolved = std::filesystem::weakly_canonical(candidate, error);
  if (error) {
    return qt::foundation::Result<std::string>::failure(
        qt::foundation::ErrorCode::invalid_argument,
        "failed to resolve llama grammar path: " + raw_path);
  }

  std::vector<std::filesystem::path> roots;
  roots.push_back(std::filesystem::path(settings.cwd));
  if (!settings.workspace_root.empty()) {
    roots.push_back(std::filesystem::path(settings.workspace_root));
  }
  if (!settings.agent_home.empty()) {
    roots.push_back(std::filesystem::path(settings.agent_home));
  }
  roots.push_back(std::filesystem::current_path() / "lib" / "llama.cpp" /
                  "grammars");

  for (const auto& root : roots) {
    const auto canonical_root = std::filesystem::weakly_canonical(root, error);
    if (error) {
      error.clear();
      continue;
    }
    if (path_is_equal_or_under(resolved, canonical_root)) {
      return resolved.generic_string();
    }
  }

  return qt::foundation::Result<std::string>::failure(
      qt::foundation::ErrorCode::invalid_argument,
      "llama grammar path must be under cwd, workspace root, agent home, or lib/llama.cpp/grammars");
}

std::pair<std::string, std::string> split_first_ascii_argument(
    std::string_view arguments) {
  std::string text = trim_ascii_copy(std::string(arguments));
  if (text.empty()) {
    return {"", ""};
  }
  const auto split = text.find_first_of(" \t\r\n");
  if (split == std::string::npos) {
    return {text, ""};
  }
  return {text.substr(0, split), trim_ascii_copy(text.substr(split + 1))};
}

bool is_index_category(std::string_view value) {
  static const std::set<std::string> categories = {
      "core",  "code",  "fs",    "git",  "build",
      "debug", "serial", "fetch", "rtos", "util",
  };
  return categories.find(std::string(value)) != categories.end();
}

std::optional<Json> parse_index_json_arguments(std::string_view raw) {
  const std::string text = trim_ascii_copy(std::string(raw));
  if (text.empty()) {
    return Json::object();
  }
  Json parsed = Json::parse(text, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return std::nullopt;
  }
  return parsed;
}

qt::tools::ToolRequest make_index_tree_request(std::optional<std::string> category,
                                               bool include_schemas) {
  qt::tools::ToolRequest request;
  request.tool_name = "index";
  request.arguments = Json{{"action", "tree"},
                           {"include_schemas", include_schemas}};
  if (category && !category->empty()) {
    request.arguments["category"] = *category;
  }
  return request;
}

qt::tools::ToolRequest make_index_call_request(std::string operation,
                                               Json arguments) {
  qt::tools::ToolRequest request;
  request.tool_name = "index";
  request.arguments = Json{{"action", "call"},
                           {"operation", std::move(operation)},
                           {"arguments", std::move(arguments)}};
  return request;
}

std::vector<std::string> index_tool_result_lines(
    std::string label, const qt::tools::ToolResult& result) {
  std::vector<std::string> lines;
  if (!qt::tools::is_success(result)) {
    lines.push_back(label + " failed: " +
                    qt::tools::to_string(result.status) +
                    (result.error_message ? " - " + *result.error_message : ""));
    return lines;
  }

  lines.push_back(std::move(label));
  const auto rendered = result.output.dump(2);
  auto output_lines = split_display_lines(rendered);
  lines.insert(lines.end(), output_lines.begin(), output_lines.end());
  return lines;
}

std::optional<qt::tools::Json> infer_search_text_arguments(
    std::string_view original_text) {
  const auto command = split_first_ascii_argument(original_text);
  const std::string verb = lowercase_ascii(command.first);
  if (verb != "search" && verb != "grep" && verb != "find") {
    return std::nullopt;
  }

  auto term_and_path = split_first_ascii_argument(command.second);
  if (lowercase_ascii(term_and_path.first) == "for") {
    term_and_path = split_first_ascii_argument(term_and_path.second);
  }
  if (term_and_path.first.empty()) {
    return std::nullopt;
  }

  std::string path = trim_ascii_copy(term_and_path.second);
  const auto maybe_in_path = split_first_ascii_argument(path);
  if (lowercase_ascii(maybe_in_path.first) == "in") {
    path = trim_ascii_copy(maybe_in_path.second);
  }
  if (path.empty()) {
    path = ".";
  }

  return qt::tools::Json{{"query", term_and_path.first},
                         {"path", path},
                         {"literal", true}};
}

class PromptOverlay final : public qt::tui::Component,
                            public qt::tui::InputHandler {
 public:
  using SubmitCallback = std::function<void(const std::string&)>;
  using CancelCallback = std::function<void()>;

  PromptOverlay(std::string title, std::string label, std::string hint = {})
      : title_(std::move(title)), label_(std::move(label)), hint_(std::move(hint)) {
    input_.set_focused(true);
    input_.set_submit_callback([this](const std::string& value) {
      if (on_submit_) {
        on_submit_(value);
      }
    });
    input_.set_escape_callback([this]() {
      if (on_cancel_) {
        on_cancel_();
      }
    });
  }

  void set_submit_callback(SubmitCallback callback) {
    on_submit_ = std::move(callback);
  }

  void set_cancel_callback(CancelCallback callback) {
    on_cancel_ = std::move(callback);
  }

  void set_value(std::string value) { input_.set_value(std::move(value)); }

  [[nodiscard]] qt::foundation::Result<qt::tui::RenderBuffer> render(
      const qt::tui::RenderConstraints& constraints) const override {
    const std::size_t width =
        constraints.max_width == 0 ? 60U : constraints.max_width;
    std::vector<std::string> lines;
    lines.push_back(title_);
    lines.push_back(label_ + input_.value() + std::string(qt::tui::cursor_marker));
    if (!hint_.empty()) {
      lines.push_back(hint_);
    }
    return qt::tui::make_buffer(
        std::move(lines),
        qt::tui::RenderConstraints{width, constraints.max_height});
  }

  void handle_input(std::string_view bytes) override { input_.handle_input(bytes); }

 private:
  std::string title_;
  std::string label_;
  std::string hint_;
  qt::tui::Input input_;
  SubmitCallback on_submit_;
  CancelCallback on_cancel_;
};

class ReviewMenuOverlay final : public qt::tui::Component,
                                public qt::tui::InputHandler {
 public:
  using ShortcutCallback =
      std::function<void(char, const qt::tui::SelectItem*)>;

  ReviewMenuOverlay(std::vector<std::string> detail_lines,
                    std::vector<qt::tui::SelectItem> actions,
                    std::size_t selected_index = 0U,
                    std::size_t max_visible = 12U)
      : detail_lines_(std::move(detail_lines)),
        actions_(std::nullopt, std::move(actions), selected_index, max_visible,
                 kReferenceInteractiveSelectLayout) {}

  void set_select_callback(qt::tui::Select::SelectCallback callback) {
    actions_.set_select_callback(std::move(callback));
  }

  void set_cancel_callback(qt::tui::Select::CancelCallback callback) {
    actions_.set_cancel_callback(std::move(callback));
  }

  void set_selection_change_callback(
      qt::tui::Select::SelectCallback callback) {
    actions_.set_selection_change_callback(std::move(callback));
  }

  void set_shortcut_callback(ShortcutCallback callback) {
    shortcut_callback_ = std::move(callback);
  }

  [[nodiscard]] qt::foundation::Result<qt::tui::RenderBuffer> render(
      const qt::tui::RenderConstraints& constraints) const override {
    const std::size_t width = constraints.max_width == 0U
                                  ? 80U
                                  : constraints.max_width;
    const std::size_t reserved_actions =
        constraints.max_height >= 8U ? 5U : 2U;
    const std::size_t detail_capacity =
        constraints.max_height > reserved_actions
            ? constraints.max_height - reserved_actions
            : constraints.max_height;
    const std::size_t maximum_offset =
        detail_lines_.size() > detail_capacity
            ? detail_lines_.size() - detail_capacity
            : 0U;
    const std::size_t offset = std::min(detail_offset_, maximum_offset);
    const std::size_t end =
        std::min(detail_lines_.size(), offset + detail_capacity);
    std::vector<std::string> lines(
        detail_lines_.begin() + static_cast<std::ptrdiff_t>(offset),
        detail_lines_.begin() + static_cast<std::ptrdiff_t>(end));
    if (maximum_offset != 0U && !lines.empty()) {
      lines.front() = "[detail " + std::to_string(offset + 1U) + "-" +
                      std::to_string(end) + "/" +
                      std::to_string(detail_lines_.size()) +
                      "; PgUp/PgDn scroll] " + lines.front();
    }
    if (!lines.empty()) {
      lines.push_back({});
    }
    const std::size_t remaining =
        constraints.max_height > lines.size()
            ? constraints.max_height - lines.size()
            : 0U;
    if (remaining != 0U) {
      auto rendered = actions_.render({width, remaining});
      if (!rendered) {
        return rendered.error();
      }
      lines.insert(lines.end(), rendered.value().lines.begin(),
                   rendered.value().lines.end());
    }
    return qt::tui::make_buffer(
        std::move(lines),
        qt::tui::RenderConstraints{width, constraints.max_height});
  }

  void handle_input(std::string_view bytes) override {
    if (bytes == "\x1b[5~") {
      detail_offset_ = detail_offset_ > 4U ? detail_offset_ - 4U : 0U;
      return;
    }
    if (bytes == "\x1b[6~") {
      if (!detail_lines_.empty())
        detail_offset_ = std::min(detail_lines_.size() - 1U,
                                  detail_offset_ + 4U);
      return;
    }
    if (bytes.size() == 1U && shortcut_callback_) {
      const unsigned char byte = static_cast<unsigned char>(bytes.front());
      const char key = static_cast<char>(std::tolower(byte));
      if (key == 'a' || key == 'e' || key == 'r' || key == 'd' ||
          key == 's') {
        shortcut_callback_(key, actions_.selected_item());
        return;
      }
    }
    actions_.handle_input(bytes);
  }

 private:
  std::vector<std::string> detail_lines_;
  qt::tui::Select actions_;
  ShortcutCallback shortcut_callback_;
  std::size_t detail_offset_ = 0U;
};

std::string review_text_excerpt(std::string text,
                                const std::size_t limit = 110U) {
  std::replace(text.begin(), text.end(), '\n', ' ');
  std::replace(text.begin(), text.end(), '\r', ' ');
  while (text.find("  ") != std::string::npos) {
    text.replace(text.find("  "), 2U, " ");
  }
  if (text.size() <= limit) return text;
  return text.substr(0U, limit > 3U ? limit - 3U : limit) + "...";
}

std::string review_age(qt::memory::TimePoint time) {
  if (time == qt::memory::TimePoint{}) return "unknown age";
  const auto seconds = std::max<std::int64_t>(
      0, std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now() - time)
             .count());
  if (seconds < 60) return std::to_string(seconds) + "s ago";
  if (seconds < 3600) return std::to_string(seconds / 60) + "m ago";
  if (seconds < 86400) return std::to_string(seconds / 3600) + "h ago";
  return std::to_string(seconds / 86400) + "d ago";
}

std::string review_duration(std::int64_t seconds) {
  seconds = std::max<std::int64_t>(0, seconds);
  if (seconds < 60) return std::to_string(seconds) + "s";
  if (seconds < 3600) return std::to_string(seconds / 60) + "m";
  if (seconds < 86400) return std::to_string(seconds / 3600) + "h";
  return std::to_string(seconds / 86400) + "d";
}

std::string review_candidate_content(
    const qt::memory::HydratedReview& review) {
  return std::visit(
      [](const auto& payload) -> std::string {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload,
                                     qt::memory::DreamArtifactCandidate>) {
          return payload.content;
        } else if constexpr (
            std::is_same_v<Payload, qt::memory::ConsolidationCandidate>) {
          return payload.canonical_content;
        } else if constexpr (
            std::is_same_v<Payload, qt::memory::ConflictCandidate>) {
          return payload.explanation;
        } else if constexpr (
            std::is_same_v<Payload,
                           qt::memory::ProcedureRevisionCandidate>) {
          return payload.proposed_content;
        } else if constexpr (
            std::is_same_v<Payload, qt::memory::GeneralReviewCandidate>) {
          return payload.content;
        } else {
          return {};
        }
      },
      review.candidate.payload);
}

std::string join_review_values(const std::vector<std::string>& values,
                               std::string_view separator = ", ") {
  std::string result;
  for (const auto& value : values) {
    if (!result.empty()) result += separator;
    result += value;
  }
  return result.empty() ? "none" : result;
}

std::string exact_review_scope(const qt::memory::MemoryScope& scope) {
  const auto shown = [](const std::string& value) -> const std::string& {
    static const std::string none = "<none>";
    return value.empty() ? none : value;
  };
  return "project=" + shown(scope.project) +
         " repository=" + shown(scope.repository) +
         " branch=" + shown(scope.branch) + " session=" + shown(scope.session) +
         " task=" + shown(scope.task);
}

class ReviewInboxController final
    : public std::enable_shared_from_this<ReviewInboxController> {
 public:
  ReviewInboxController(
      qt::app::InteractiveTerminalSession& ui,
      std::shared_ptr<qt::memory::MemoryRuntime> runtime,
      qt::tools::ToolContext evidence_tool_context = {})
      : ui_(ui), runtime_(std::move(runtime)),
        evidence_tool_context_(std::move(evidence_tool_context)),
        editor_text_on_open_(ui.mutable_editor().text()) {}

  void show_landing() {
    if (!keep_alive_) keep_alive_ = shared_from_this();
    try {
      if (!view_state_loaded_) {
        restored_view_state_ = runtime_->load_review_view_state();
        view_state_loaded_ = true;
      }
      const auto pending_categories = pending_kind_counts();
      const auto stats = runtime_->review_statistics();
      qt::memory::ReviewQuery dream_query;
      dream_query.kinds = {"dream_artifact", "dream_research_successor"};
      dream_query.statuses = {qt::memory::ReviewStatus::Pending,
                              qt::memory::ReviewStatus::Deferred,
                              qt::memory::ReviewStatus::Accepted,
                              qt::memory::ReviewStatus::Rejected};
      dream_query.include_due_deferred = false;
      dream_query.limit = 1U;
      const auto latest_dream = runtime_->review_items(dream_query);
      std::vector<std::string> lines = {
          "Memory Review Inbox",
          "Scope: " + scope_label(),
          "Pending " + std::to_string(stats.pending) + "  |  urgent/high " +
              std::to_string(stats.urgent) + "  |  due deferred " +
              std::to_string(stats.due_deferred),
          "Oldest pending: " +
              (stats.pending == 0U
                   ? std::string("none")
                   : review_duration(stats.oldest_pending_age_seconds)),
          "Latest Dreaming: " +
              (latest_dream.empty()
                   ? std::string("none")
                   : latest_dream.front().kind + " " + latest_dream.front().id +
                         " (" + review_age(latest_dream.front().created_at) + ")"),
          "History: approved=" + std::to_string(stats.approved) +
              " rejected=" + std::to_string(stats.rejected) +
              " deferred=" + std::to_string(stats.deferred) +
              " reopened=" + std::to_string(stats.reopened) +
              " failed=" + std::to_string(stats.failed) +
              " compensated=" + std::to_string(stats.compensated),
      };
      if (!pending_categories.empty()) {
        std::vector<std::string> counts;
        counts.reserve(pending_categories.size());
        for (const auto& [kind, count] : pending_categories) {
          counts.push_back(kind + "=" + std::to_string(count));
        }
        lines.push_back("Categories: " + join_review_values(counts));
      }
      std::vector<qt::tui::SelectItem> items = {
          {"next", "Review next", "Highest-priority pending item"},
          {"pending", "Browse pending", "All pending reviews in ranked order"},
          {"batch", "Batch pending reviews", "Select safe homogeneous work"},
          {"filters", "Filters", "Combine status, category, priority, tags, and ordering"},
          {"category", "Browse by category", "Filter pending reviews by kind"},
          {"dream", "Review latest Dream", "Newest pending Dreaming result"},
          {"deferred", "Deferred", "Deferred reviews and due work"},
          {"history", "History", "Accepted, rejected, and deferred decisions"},
          {"status", "Memory status", "Return to memory health and settings"},
      };
      if (restored_view_state_ &&
          !restored_view_state_->selected_review_id.empty()) {
        items.insert(items.begin(),
                     {"resume", "Resume previous review",
                      restored_view_state_->selected_review_id});
        if (has_active_filters(restored_view_state_->query)) {
          lines.push_back("Active saved filters: " +
                          query_filter_label(restored_view_state_->query));
          items.insert(items.begin() + 1,
                       {"clear_filters", "Clear saved filters",
                        "Return to the complete pending queue"});
        }
      }
      auto panel = std::make_shared<ReviewMenuOverlay>(
          std::move(lines), std::move(items), 0U, 10U);
      std::weak_ptr<ReviewInboxController> weak = shared_from_this();
      panel->set_select_callback([weak](const qt::tui::SelectItem& item) {
        if (const auto self = weak.lock()) self->select_landing(item.value);
      });
      panel->set_cancel_callback([weak]() {
        if (const auto self = weak.lock()) self->close();
      });
      show_review_overlay(panel);
    } catch (const std::exception& error) {
      fail("Unable to open the Memory Review Inbox", error);
    }
  }

 private:
  template <typename Component>
  void show_review_overlay(std::shared_ptr<Component> component) {
    // Review screens form a navigation state machine, not an overlay stack.
    // Keeping prior screens underneath makes the landing screen's Escape only
    // reveal an older review screen and leaves the user trapped in review.
    ui_.hide_overlay();
    ui_.show_overlay(std::move(component));
  }

  std::string scope_label() const {
    const auto& scope = runtime_->scope();
    std::string label = scope.repository_display.empty()
                            ? scope.repository
                            : scope.repository_display;
    if (label.empty()) label = scope.project.empty() ? "global" : scope.project;
    if (!scope.branch.empty()) label += " @ " + scope.branch;
    return label;
  }

  std::map<std::string, std::size_t> pending_kind_counts() const {
    qt::memory::ReviewQuery query;
    query.limit = 1000U;
    std::map<std::string, std::size_t> result;
    for (const auto& item : runtime_->review_items(query)) ++result[item.kind];
    return result;
  }

  static bool has_active_filters(const qt::memory::ReviewQuery& query) {
    return !query.kinds.empty() || !query.priorities.empty() ||
           !query.tags.empty() || query.created_after.has_value() ||
           query.created_before.has_value() || query.decided_after.has_value() ||
           query.deferred_due_before.has_value() ||
           query.statuses !=
               std::vector<qt::memory::ReviewStatus>{
                   qt::memory::ReviewStatus::Pending};
  }

  static std::string query_filter_label(const qt::memory::ReviewQuery& query) {
    std::vector<std::string> filters;
    if (!query.kinds.empty())
      filters.push_back("kind=" + join_review_values(query.kinds));
    if (!query.priorities.empty())
      filters.push_back("priority=" + join_review_values(query.priorities));
    if (!query.tags.empty())
      filters.push_back("tags=" + join_review_values(query.tags));
    if (query.statuses !=
        std::vector<qt::memory::ReviewStatus>{qt::memory::ReviewStatus::Pending}) {
      std::vector<std::string> statuses;
      for (const auto status : query.statuses)
        statuses.push_back(qt::memory::to_string(status));
      filters.push_back("status=" + join_review_values(statuses));
    }
    if (query.created_after || query.created_before)
      filters.push_back("created-age");
    if (query.decided_after) filters.push_back("decided-age");
    if (query.deferred_due_before) filters.push_back("deferred-due");
    return join_review_values(filters);
  }

  void persist_view(const std::string& selected_id) {
    qt::memory::ReviewViewState state;
    state.query = queue_query_;
    state.selected_review_id = selected_id;
    state.queue_offset = queue_position_;
    runtime_->save_review_view_state(state);
    restored_view_state_ = std::move(state);
    view_state_loaded_ = true;
  }

  void fail(std::string prefix, const std::exception& error) {
    ui_.hide_overlay();
    ui_.append_transcript_line(std::move(prefix) + ": " + error.what());
    keep_alive_.reset();
  }

  void close() {
    ui_.hide_overlay();
    ui_.mutable_editor().set_text(editor_text_on_open_);
    ui_.mutable_editor().set_focused(true);
    keep_alive_.reset();
  }

  void select_landing(const std::string& value) {
    if (value == "clear_filters") {
      runtime_->clear_review_view_state();
      restored_view_state_.reset();
      qt::memory::ReviewQuery query;
      show_queue("Pending reviews", std::move(query));
      return;
    }
    if (value == "resume" && restored_view_state_) {
      queue_title_ = "Resumed review queue";
      queue_query_ = restored_view_state_->query;
      queue_position_ = restored_view_state_->queue_offset;
      const auto items = runtime_->review_items(queue_query_);
      const auto selected = std::find_if(
          items.begin(), items.end(), [this](const qt::memory::ReviewItem& item) {
            return item.id == restored_view_state_->selected_review_id;
          });
      if (selected != items.end()) {
        queue_position_ = static_cast<std::size_t>(selected - items.begin());
      }
      show_queue(queue_title_, queue_query_, true);
      return;
    }
    if (value == "status") {
      ui_.hide_overlay();
      ui_.mutable_editor().set_text("/memory status");
      keep_alive_.reset();
      return;
    }
    if (value == "category") {
      show_categories();
      return;
    }
    if (value == "filters") {
      filter_query_ = restored_view_state_ ? restored_view_state_->query
                                           : qt::memory::ReviewQuery{};
      show_filters();
      return;
    }
    if (value == "batch") {
      batch_query_ = restored_view_state_ ? restored_view_state_->query
                                          : qt::memory::ReviewQuery{};
      batch_query_.statuses = {qt::memory::ReviewStatus::Pending};
      batch_selected_ids_.clear();
      show_batch_selection();
      return;
    }
    qt::memory::ReviewQuery query;
    if (value == "history") {
      history_filter_ = "all";
      show_history();
      return;
    }
    if (value == "deferred") {
      query.statuses = {qt::memory::ReviewStatus::Deferred};
      query.include_due_deferred = true;
      show_queue("Deferred reviews", std::move(query));
      return;
    }
    if (value == "dream") {
      show_latest_dream();
      return;
    }
    if (value == "next") query.limit = 1U;
    show_queue(value == "next" ? "Review next" : "Pending reviews",
               std::move(query));
  }

  void show_categories() {
    try {
      const auto categories = pending_kind_counts();
      std::vector<qt::tui::SelectItem> items;
      for (const auto& [kind, count] : categories) {
        items.push_back({kind, kind, std::to_string(count) + " pending"});
      }
      items.push_back({"__back__", "Back", "Review Inbox"});
      auto panel = std::make_shared<ReviewMenuOverlay>(
          std::vector<std::string>{"Review categories", "Scope: " + scope_label()},
          std::move(items));
      std::weak_ptr<ReviewInboxController> weak = shared_from_this();
      panel->set_select_callback([weak](const qt::tui::SelectItem& item) {
        const auto self = weak.lock();
        if (!self) return;
        if (item.value == "__back__") {
          self->show_landing();
          return;
        }
        qt::memory::ReviewQuery query;
        query.kinds = {item.value};
        self->show_queue("Pending: " + item.value, std::move(query));
      });
      panel->set_cancel_callback([weak]() {
        if (const auto self = weak.lock()) self->show_landing();
      });
      show_review_overlay(panel);
    } catch (const std::exception& error) {
      fail("Unable to load review categories", error);
    }
  }

  bool history_matches(const qt::memory::ReviewActionRecord& record) const {
    if (history_filter_ == "all") return true;
    if (history_filter_ == "failed")
      return record.application_status ==
             qt::memory::ReviewApplicationStatus::Failed;
    if (history_filter_ == "reopened") return record.action == "reopen";
    if (history_filter_ == "compensated") return record.action == "compensate";
    return qt::memory::to_string(record.after_status) == history_filter_;
  }

  void show_history() {
    try {
      const auto history = runtime_->review_history(std::nullopt, 200U);
      std::vector<qt::tui::SelectItem> items = {
          {"__filter__", "Filter history", history_filter_},
          {"__back__", "Back", "Review Inbox"},
      };
      for (const auto& record : history) {
        if (!history_matches(record)) continue;
        items.push_back(
            {record.id,
             record.action + "  " +
                 qt::memory::to_string(record.after_status) + "  " +
                 qt::memory::to_string(record.application_status),
             "review=" + record.review_id + " actor=" + record.actor +
                 " receipt=" + review_text_excerpt(record.result_json, 90U)});
      }
      auto panel = std::make_shared<ReviewMenuOverlay>(
          std::vector<std::string>{
              "Review history", "Filter: " + history_filter_,
              "Accepted, rejected, deferred, reopened, failed, and compensated actions with linked receipts."},
          std::move(items), 0U, 14U);
      std::weak_ptr<ReviewInboxController> weak = shared_from_this();
      panel->set_select_callback([weak, history](const qt::tui::SelectItem& item) {
        const auto self = weak.lock();
        if (!self) return;
        if (item.value == "__back__") self->show_landing();
        else if (item.value == "__filter__") self->show_history_filters();
        else {
          const auto found = std::find_if(
              history.begin(), history.end(), [&](const auto& record) {
                return record.id == item.value;
              });
          if (found != history.end()) self->show_history_receipt(*found);
        }
      });
      panel->set_cancel_callback([weak]() {
        if (const auto self = weak.lock()) self->show_landing();
      });
      show_review_overlay(panel);
    } catch (const std::exception& error) {
      fail("Unable to load review history", error);
    }
  }

  void show_history_filters() {
    auto panel = std::make_shared<ReviewMenuOverlay>(
        std::vector<std::string>{"Filter review history"},
        std::vector<qt::tui::SelectItem>{
            {"all", "All", "Every recorded action"},
            {"accepted", "Accepted", "Accepted outcomes"},
            {"rejected", "Rejected", "Rejected outcomes"},
            {"deferred", "Deferred", "Scheduled for later"},
            {"reopened", "Reopened", "Reopened terminal decisions"},
            {"failed", "Failed", "Application failures"},
            {"compensated", "Compensated", "Governed corrections"}});
    std::weak_ptr<ReviewInboxController> weak = shared_from_this();
    panel->set_select_callback([weak](const qt::tui::SelectItem& item) {
      if (const auto self = weak.lock()) {
        self->history_filter_ = item.value;
        self->show_history();
      }
    });
    panel->set_cancel_callback([weak]() {
      if (const auto self = weak.lock()) self->show_history();
    });
    show_review_overlay(panel);
  }

  void show_history_receipt(const qt::memory::ReviewActionRecord& record) {
    auto panel = std::make_shared<ReviewMenuOverlay>(
        std::vector<std::string>{
            "Review history receipt", "Review: " + record.review_id,
            "Action: " + record.action + " by " + record.actor,
            "Status: " + std::string(qt::memory::to_string(record.before_status)) +
                " -> " + qt::memory::to_string(record.after_status),
            "Application: " +
                std::string(qt::memory::to_string(record.application_status)),
            "Note: " + record.note, "Error: " + record.application_error,
            "Linked receipt: " + record.result_json},
        std::vector<qt::tui::SelectItem>{{"back", "Back to history", "Preserve filter"}});
    std::weak_ptr<ReviewInboxController> weak = shared_from_this();
    panel->set_select_callback([weak](const qt::tui::SelectItem&) {
      if (const auto self = weak.lock()) self->show_history();
    });
    panel->set_cancel_callback([weak]() {
      if (const auto self = weak.lock()) self->show_history();
    });
    show_review_overlay(panel);
  }

  void show_batch_selection() {
    try {
      const auto items = runtime_->review_items(batch_query_);
      std::map<std::string, std::size_t> kinds;
      std::map<std::string, std::size_t> risks;
      std::set<std::string> scopes;
      for (const auto& item : items) {
        if (batch_selected_ids_.count(item.id) == 0U) continue;
        ++kinds[item.kind];
        const auto detail = runtime_->review_detail(item.id);
        ++risks[detail.risk.risk_class];
        scopes.insert(exact_review_scope(detail.subject ? detail.subject->scope
                                                       : runtime_->scope()));
      }
      const auto counts = [](const auto& values) {
        std::vector<std::string> result;
        for (const auto& [name, count] : values)
          result.push_back(name + "=" + std::to_string(count));
        return join_review_values(result);
      };
      std::vector<std::string> scope_values(scopes.begin(), scopes.end());
      std::vector<std::string> lines = {
          "Batch pending reviews",
          "Selected: " + std::to_string(batch_selected_ids_.size()) +
              " / " + std::to_string(items.size()),
          "Kinds: " + counts(kinds), "Scopes: " + join_review_values(scope_values),
          "Risk: " + counts(risks),
          "Enter toggles selection; preview enforces safe homogeneous actions.",
      };
      std::vector<qt::tui::SelectItem> choices = {
          {"__preview__", "Preview selected batch",
           "Choose approve or reject and inspect aggregate effects"},
          {"__clear__", "Clear selection", "Keep the current filters"},
          {"__back__", "Back", "Return to Review Inbox"},
      };
      for (const auto& item : items) {
        choices.push_back(
            {item.id,
             std::string(batch_selected_ids_.count(item.id) != 0U ? "[x] "
                                                                  : "[ ] ") +
                 item.kind + "  " + item.priority,
             review_text_excerpt(item.reason, 90U)});
      }
      auto panel = std::make_shared<ReviewMenuOverlay>(
          std::move(lines), std::move(choices), 0U, 14U);
      std::weak_ptr<ReviewInboxController> weak = shared_from_this();
      panel->set_select_callback([weak](const qt::tui::SelectItem& item) {
        const auto self = weak.lock();
        if (!self) return;
        if (item.value == "__back__") self->show_landing();
        else if (item.value == "__clear__") {
          self->batch_selected_ids_.clear();
          self->show_batch_selection();
        } else if (item.value == "__preview__") {
          self->show_batch_action_choices();
        } else {
          if (!self->batch_selected_ids_.erase(item.value))
            self->batch_selected_ids_.insert(item.value);
          self->show_batch_selection();
        }
      });
      panel->set_cancel_callback([weak]() {
        if (const auto self = weak.lock()) self->show_landing();
      });
      show_review_overlay(panel);
    } catch (const std::exception& error) {
      fail("Unable to open batch review selection", error);
    }
  }

  void show_batch_action_choices() {
    if (batch_selected_ids_.empty()) {
      ui_.append_transcript_line("Batch review requires at least one selected item.");
      show_batch_selection();
      return;
    }
    auto panel = std::make_shared<ReviewMenuOverlay>(
        std::vector<std::string>{
            "Batch action", "Selected: " +
                                std::to_string(batch_selected_ids_.size()),
            "Approval is limited by runtime policy to homogeneous, low-risk, source-backed items."},
        std::vector<qt::tui::SelectItem>{
            {"approve", "Approve selected", "Runtime rejects unsafe approval batches"},
            {"reject", "Reject selected", "Preserve audit history without deletion"},
            {"back", "Back", "Keep selection and filters"}});
    std::weak_ptr<ReviewInboxController> weak = shared_from_this();
    panel->set_select_callback([weak](const qt::tui::SelectItem& item) {
      const auto self = weak.lock();
      if (!self) return;
      if (item.value == "back") self->show_batch_selection();
      else self->preview_batch(item.value == "approve"
                                   ? qt::memory::ReviewActionKind::Approve
                                   : qt::memory::ReviewActionKind::Reject);
    });
    panel->set_cancel_callback([weak]() {
      if (const auto self = weak.lock()) self->show_batch_selection();
    });
    show_review_overlay(panel);
  }

  void preview_batch(qt::memory::ReviewActionKind kind) {
    try {
      std::vector<qt::memory::ReviewBatchItem> items;
      for (const auto& id : batch_selected_ids_) {
        qt::memory::ReviewAction action;
        action.kind = kind;
        action.actor = "user:interactive";
        action.human_authority = true;
        items.push_back({id, std::move(action)});
      }
      auto preview = runtime_->preview_review_batch(items);
      if (!preview.applicable) {
        std::vector<std::string> lines = {
            "Batch cannot be applied", "Selection and filters were preserved."};
        for (const auto& warning : preview.warnings)
          lines.push_back("Warning: " + review_text_excerpt(warning, 180U));
        auto panel = std::make_shared<ReviewMenuOverlay>(
            std::move(lines),
            std::vector<qt::tui::SelectItem>{{"back", "Inspect selection", "Retry or review individually"}});
        std::weak_ptr<ReviewInboxController> weak = shared_from_this();
        panel->set_select_callback([weak](const qt::tui::SelectItem&) {
          if (const auto self = weak.lock()) self->show_batch_selection();
        });
        panel->set_cancel_callback([weak]() {
          if (const auto self = weak.lock()) self->show_batch_selection();
        });
        show_review_overlay(panel);
        return;
      }
      for (std::size_t index = 0U; index < items.size(); ++index) {
        items[index].action.expected_revision = preview.items[index].review_revision;
        items[index].action.idempotency_key = preview.items[index].idempotency_token;
      }
      std::map<std::string, std::size_t> effects;
      std::vector<std::string> lines = {
          "Confirm batch " + std::string(qt::memory::to_string(kind)),
          "Count: " + std::to_string(items.size()),
          "Scope filter: " + query_filter_label(batch_query_),
      };
      for (const auto& item : preview.items) {
        for (const auto& change : item.changes)
          ++effects[change.record_type + ":" + change.operation];
      }
      for (const auto& [effect, count] : effects)
        lines.push_back("Aggregate effect: " + effect + " x" +
                        std::to_string(count));
      for (const auto& item : preview.items) {
        for (const auto& change : item.changes)
          lines.push_back("Change: " + change.record_type + " " +
                          change.record_id + " " + change.operation);
      }
      auto panel = std::make_shared<ReviewMenuOverlay>(
          std::move(lines),
          std::vector<qt::tui::SelectItem>{
              {"confirm", "Confirm batch", "Apply the exact aggregate preview"},
              {"cancel", "Cancel", "Keep selection and filters"}},
          1U, 2U);
      std::weak_ptr<ReviewInboxController> weak = shared_from_this();
      panel->set_select_callback(
          [weak, items = std::move(items), token = preview.idempotency_token](
              const qt::tui::SelectItem& item) mutable {
            const auto self = weak.lock();
            if (!self) return;
            if (item.value == "confirm")
              self->apply_batch(std::move(items), std::move(token));
            else
              self->show_batch_selection();
          });
      panel->set_cancel_callback([weak]() {
        if (const auto self = weak.lock()) self->show_batch_selection();
      });
      show_review_overlay(panel);
    } catch (const std::exception& error) {
      ui_.append_transcript_line(std::string("Batch preview failed: ") + error.what());
      show_batch_selection();
    }
  }

  void apply_batch(std::vector<qt::memory::ReviewBatchItem> items,
                   std::string preview_token) {
    const auto total = items.size();
    ui_.append_transcript_line("Memory review batch progress: 0/" +
                               std::to_string(total));
    try {
      const auto result =
          runtime_->apply_review_batch(std::move(items), std::move(preview_token));
      batch_selected_ids_.clear();
      for (const auto& item : result.items)
        if (!item.error.empty() || !item.result ||
            !item.result->decision_persisted)
          batch_selected_ids_.insert(item.review_id);
      ui_.append_transcript_line(
          "Memory review batch result: processed=" + std::to_string(total) +
          "/" + std::to_string(total) +
          "; applied=" + std::to_string(result.applied_count) +
          "; failed=" + std::to_string(result.failed_count) +
          (result.partial_failure ? "; partial failure" : ""));
      if (!batch_selected_ids_.empty())
        ui_.append_transcript_line(
            "Failed batch selections were preserved for retry or inspection.");
      show_batch_selection();
    } catch (const std::exception& error) {
      ui_.append_transcript_line(std::string("Memory review batch failed: ") +
                                 error.what());
      show_batch_selection();
    }
  }

  void show_filters() {
    std::vector<std::string> lines = {
        "Review filters",
        "Active: " + (has_active_filters(filter_query_)
                           ? query_filter_label(filter_query_)
                           : std::string("none")),
        std::string("Ordering: ") +
            (filter_query_.oldest_first ? "oldest first" : "newest first"),
    };
    std::vector<qt::tui::SelectItem> items = {
        {"apply", "Apply filters", "Open the resulting review queue"},
        {"priority", "Priority", join_review_values(filter_query_.priorities)},
        {"status", "Status", filter_status_label()},
        {"kind", "Kind / category", join_review_values(filter_query_.kinds)},
        {"tags", "Tags", join_review_values(filter_query_.tags)},
        {"ordering", "Toggle oldest/newest",
         filter_query_.oldest_first ? "oldest first" : "newest first"},
        {"clear", "Clear all filters", "Restore the complete pending queue"},
        {"back", "Back", "Review Inbox"},
    };
    auto panel = std::make_shared<ReviewMenuOverlay>(
        std::move(lines), std::move(items), 0U, 10U);
    std::weak_ptr<ReviewInboxController> weak = shared_from_this();
    panel->set_select_callback([weak](const qt::tui::SelectItem& item) {
      const auto self = weak.lock();
      if (!self) return;
      if (item.value == "apply") {
        self->show_queue("Filtered reviews", self->filter_query_);
      } else if (item.value == "priority") {
        self->show_priority_filter();
      } else if (item.value == "status") {
        self->show_status_filter();
      } else if (item.value == "kind") {
        self->show_kind_filter();
      } else if (item.value == "tags") {
        self->show_tag_filter();
      } else if (item.value == "ordering") {
        self->filter_query_.oldest_first =
            !self->filter_query_.oldest_first;
        self->show_filters();
      } else if (item.value == "clear") {
        self->filter_query_ = qt::memory::ReviewQuery{};
        self->runtime_->clear_review_view_state();
        self->restored_view_state_.reset();
        self->show_filters();
      } else {
        self->show_landing();
      }
    });
    panel->set_cancel_callback([weak]() {
      if (const auto self = weak.lock()) self->show_landing();
    });
    show_review_overlay(panel);
  }

  std::string filter_status_label() const {
    std::vector<std::string> values;
    for (const auto status : filter_query_.statuses)
      values.push_back(qt::memory::to_string(status));
    return join_review_values(values);
  }

  void show_priority_filter() {
    std::vector<qt::tui::SelectItem> items = {
        {"", "Any priority", "Clear the priority filter"},
        {"urgent", "Urgent", "Only urgent work"},
        {"high", "High", "Only high-priority work"},
        {"normal", "Normal", "Only normal-priority work"},
        {"low", "Low", "Only low-priority work"},
    };
    show_single_filter("Filter by priority", std::move(items),
                       [this](const std::string& value) {
                         filter_query_.priorities =
                             value.empty() ? std::vector<std::string>{}
                                           : std::vector<std::string>{value};
                       });
  }

  void show_status_filter() {
    std::vector<qt::tui::SelectItem> items = {
        {"pending", "Pending", "Work awaiting a decision"},
        {"deferred", "Deferred", "Work scheduled for later"},
        {"terminal", "History", "Accepted and rejected decisions"},
        {"all", "All statuses", "Pending, deferred, accepted, and rejected"},
    };
    show_single_filter("Filter by status", std::move(items),
                       [this](const std::string& value) {
                         using Status = qt::memory::ReviewStatus;
                         if (value == "pending")
                           filter_query_.statuses = {Status::Pending};
                         else if (value == "deferred")
                           filter_query_.statuses = {Status::Deferred};
                         else if (value == "terminal")
                           filter_query_.statuses = {Status::Accepted,
                                                     Status::Rejected};
                         else
                           filter_query_.statuses = {Status::Pending,
                                                     Status::Deferred,
                                                     Status::Accepted,
                                                     Status::Rejected};
                       });
  }

  void show_kind_filter() {
    const auto categories = runtime_->review_statistics().by_kind;
    std::vector<qt::tui::SelectItem> items = {
        {"", "Any kind", "Clear the kind filter"}};
    for (const auto& [kind, count] : categories) {
      items.push_back({kind, kind, std::to_string(count) + " total"});
    }
    show_single_filter("Filter by kind / category", std::move(items),
                       [this](const std::string& value) {
                         filter_query_.kinds =
                             value.empty() ? std::vector<std::string>{}
                                           : std::vector<std::string>{value};
                       });
  }

  void show_tag_filter() {
    auto prompt = std::make_shared<PromptOverlay>(
        "Filter by tags", "Tags: ",
        "Comma-separated; blank clears the tag filter; Esc returns");
    if (!filter_query_.tags.empty())
      prompt->set_value(join_review_values(filter_query_.tags));
    std::weak_ptr<ReviewInboxController> weak = shared_from_this();
    prompt->set_submit_callback([weak](const std::string& value) {
      const auto self = weak.lock();
      if (!self) return;
      self->filter_query_.tags.clear();
      std::istringstream stream(value);
      std::string tag;
      while (std::getline(stream, tag, ',')) {
        tag = trim_ascii_copy(tag);
        if (!tag.empty()) self->filter_query_.tags.push_back(std::move(tag));
      }
      self->show_filters();
    });
    prompt->set_cancel_callback([weak]() {
      if (const auto self = weak.lock()) self->show_filters();
    });
    show_review_overlay(prompt);
  }

  void show_single_filter(
      std::string title, std::vector<qt::tui::SelectItem> items,
      std::function<void(const std::string&)> apply) {
    auto panel = std::make_shared<ReviewMenuOverlay>(
        std::vector<std::string>{std::move(title)}, std::move(items));
    std::weak_ptr<ReviewInboxController> weak = shared_from_this();
    panel->set_select_callback(
        [weak, apply = std::move(apply)](const qt::tui::SelectItem& item) {
          if (const auto self = weak.lock()) {
            apply(item.value);
            self->show_filters();
          }
        });
    panel->set_cancel_callback([weak]() {
      if (const auto self = weak.lock()) self->show_filters();
    });
    show_review_overlay(panel);
  }

  void show_latest_dream() {
    try {
      qt::memory::ReviewQuery query;
      query.kinds = {"dream_artifact", "dream_research_successor"};
      query.limit = 1000U;
      const auto reviews = runtime_->review_items(query);
      if (reviews.empty()) {
        show_empty("Latest Dream review");
        return;
      }
      const auto latest = std::max_element(
          reviews.begin(), reviews.end(),
          [](const qt::memory::ReviewItem& left,
             const qt::memory::ReviewItem& right) {
            if (left.created_at != right.created_at)
              return left.created_at < right.created_at;
            return left.id > right.id;
          });
      queue_title_ = "Pending Dream reviews";
      queue_query_ = query;
      queue_position_ = static_cast<std::size_t>(latest - reviews.begin());
      show_detail(latest->id);
    } catch (const std::exception& error) {
      fail("Unable to load the latest Dream review", error);
    }
  }

  void show_queue(std::string title, qt::memory::ReviewQuery query,
                  bool preserve_position = false) {
    try {
      auto items = runtime_->review_items(query);
      if (items.empty()) {
        show_empty(std::move(title));
        return;
      }
      if (!preserve_position || queue_title_ != title) queue_position_ = 0U;
      queue_title_ = std::move(title);
      queue_query_ = query;
      queue_position_ = std::min(queue_position_, items.size() - 1U);
      const auto selected_id = items[queue_position_].id;
      persist_view(selected_id);

      std::vector<qt::tui::SelectItem> cards;
      cards.reserve(items.size() + 1U);
      for (std::size_t index = 0U; index < items.size(); ++index) {
        const auto& item = items[index];
        std::string proposal = item.reason;
        std::string badges;
        try {
          const auto hydrated = runtime_->review_detail(item.id);
          const auto candidate = review_candidate_content(hydrated);
          if (!candidate.empty()) proposal = candidate;
          badges = "risk " + hydrated.risk.risk_class;
          if (!hydrated.blocking_diagnostics.empty()) badges += ", blocked";
          if (hydrated.subject) {
            badges += ", " + std::string(qt::memory::to_string(
                                   hydrated.subject->status));
            badges += ", confidence " +
                      format_decimal(hydrated.subject->confidence);
            badges += ", evidence " +
                      format_decimal(
                          hydrated.subject->provenance.evidence_strength);
            badges += ", scope " + exact_review_scope(hydrated.subject->scope);
          }
          if (!hydrated.recall.warnings.empty()) badges += ", recall warning";
        } catch (const std::exception&) {
          badges = "unavailable detail";
        }
        const std::string label =
            std::to_string(index + 1U) + "/" + std::to_string(items.size()) +
            "  " + item.priority + "  " + item.kind;
        cards.push_back({item.id, label,
                         review_text_excerpt(proposal, 62U) + " | reason " +
                             review_text_excerpt(item.reason, 52U) + " | " + badges +
                             " | " + review_age(item.created_at)});
      }
      cards.push_back({"__back__", "Back to Review Inbox", "Preserve queue position"});
      auto panel = std::make_shared<ReviewMenuOverlay>(
          std::vector<std::string>{queue_title_, "Scope: " + scope_label(),
                                   "↑/↓ move  Enter details  A approve  E edit  R reject  D defer  S next"},
          std::move(cards), queue_position_, 14U);
      std::weak_ptr<ReviewInboxController> weak = shared_from_this();
      panel->set_selection_change_callback(
          [weak](const qt::tui::SelectItem& item) {
            if (const auto self = weak.lock()) self->remember_position(item.value);
          });
      panel->set_select_callback([weak](const qt::tui::SelectItem& item) {
        const auto self = weak.lock();
        if (!self) return;
        if (item.value == "__back__") self->show_landing();
        else self->show_detail(item.value);
      });
      panel->set_shortcut_callback(
          [weak](char key, const qt::tui::SelectItem* item) {
            const auto self = weak.lock();
            if (!self || item == nullptr || item->value == "__back__") return;
            self->shortcut_action(item->value, key);
          });
      panel->set_cancel_callback([weak]() {
        if (const auto self = weak.lock()) self->show_landing();
      });
      show_review_overlay(panel);
    } catch (const std::exception& error) {
      fail("Unable to load memory reviews", error);
    }
  }

  static std::string format_decimal(double value) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2) << value;
    return stream.str();
  }

  void remember_position(const std::string& id) {
    try {
      const auto current = runtime_->review_items(queue_query_);
      const auto match = std::find_if(
          current.begin(), current.end(),
          [&id](const qt::memory::ReviewItem& item) { return item.id == id; });
      if (match != current.end()) {
        queue_position_ = static_cast<std::size_t>(match - current.begin());
        persist_view(id);
      }
    } catch (const std::exception&) {
    }
  }

  void show_empty(std::string title) {
    auto panel = std::make_shared<ReviewMenuOverlay>(
        std::vector<std::string>{std::move(title),
                                 "No review is required for this view.",
                                 "Memory status explains queue health and settings."},
        std::vector<qt::tui::SelectItem>{
            {"status", "Memory status", "Show memory health"},
            {"back", "Back", "Review Inbox"}});
    std::weak_ptr<ReviewInboxController> weak = shared_from_this();
    panel->set_select_callback([weak](const qt::tui::SelectItem& item) {
      const auto self = weak.lock();
      if (!self) return;
      if (item.value == "status") {
        self->ui_.hide_overlay();
        self->ui_.mutable_editor().set_text("/memory status");
        self->keep_alive_.reset();
      } else {
        self->show_landing();
      }
    });
    panel->set_cancel_callback([weak]() {
      if (const auto self = weak.lock()) self->show_landing();
    });
    show_review_overlay(panel);
  }

  void shortcut_action(const std::string& id, char key) {
    if (key == 's') {
      show_next(id);
      return;
    }
    const auto kind = key == 'a' ? qt::memory::ReviewActionKind::Approve
                      : key == 'e'
                          ? qt::memory::ReviewActionKind::EditAndApprove
                      : key == 'r' ? qt::memory::ReviewActionKind::Reject
                                   : qt::memory::ReviewActionKind::Defer;
    begin_action(id, kind);
  }

  void show_next(const std::string& id) {
    try {
      const auto items = runtime_->review_items(queue_query_);
      if (items.empty()) {
        show_empty(queue_title_);
        return;
      }
      const auto current = std::find_if(
          items.begin(), items.end(),
          [&id](const qt::memory::ReviewItem& item) { return item.id == id; });
      queue_position_ =
          current == items.end()
              ? std::min(queue_position_, items.size() - 1U)
              : (static_cast<std::size_t>(current - items.begin()) + 1U) %
                    items.size();
      persist_view(items[queue_position_].id);
      show_detail(items[queue_position_].id);
    } catch (const std::exception& error) {
      fail("Unable to advance the review queue", error);
    }
  }

  static bool actionable(const qt::memory::HydratedReview& review,
                         qt::memory::ReviewActionKind kind) {
    using Kind = qt::memory::ReviewCandidateKind;
    if (review.item.status != qt::memory::ReviewStatus::Pending) {
      return kind == qt::memory::ReviewActionKind::Reopen ||
             (kind == qt::memory::ReviewActionKind::Compensate &&
              review.subject.has_value());
    }
    if (kind == qt::memory::ReviewActionKind::Reject ||
        kind == qt::memory::ReviewActionKind::Defer) return true;
    if (kind == qt::memory::ReviewActionKind::Approve ||
        kind == qt::memory::ReviewActionKind::EditAndApprove) {
      return review.candidate.kind == Kind::DreamArtifact ||
             review.candidate.kind == Kind::DreamResearchSuccessor ||
             review.candidate.kind == Kind::General ||
             review.candidate.kind == Kind::ProcedureRevision;
    }
    if (kind == qt::memory::ReviewActionKind::Relate ||
        kind == qt::memory::ReviewActionKind::MarkContradiction ||
        kind == qt::memory::ReviewActionKind::KeepBoth ||
        kind == qt::memory::ReviewActionKind::Supersede) {
      return review.candidate.kind == Kind::Conflict ||
             review.candidate.kind == Kind::SemanticConsolidation ||
             review.candidate.kind == Kind::Compression ||
             (kind == qt::memory::ReviewActionKind::Supersede &&
              review.candidate.kind == Kind::Staleness);
    }
    if (kind == qt::memory::ReviewActionKind::Merge) {
      return review.candidate.kind == Kind::SemanticConsolidation ||
             review.candidate.kind == Kind::Compression ||
             review.candidate.kind == Kind::Conflict;
    }
    if (kind == qt::memory::ReviewActionKind::NarrowScope) {
      return review.subject.has_value();
    }
    if (kind == qt::memory::ReviewActionKind::AttachEvidence) {
      bool research_required = false;
      if (const auto* dream =
              std::get_if<qt::memory::DreamArtifactCandidate>(
                  &review.candidate.payload))
        research_required = dream->research_required;
      return review.subject.has_value() &&
             (review.candidate.kind == Kind::ProvenanceRepair ||
              review.item.provenance.source_uri.empty() || research_required);
    }
    if (kind == qt::memory::ReviewActionKind::Deprecate ||
        kind == qt::memory::ReviewActionKind::Revalidate ||
        kind == qt::memory::ReviewActionKind::Retain) {
      return review.candidate.kind == Kind::Staleness ||
             (kind == qt::memory::ReviewActionKind::Deprecate &&
              review.candidate.kind == Kind::ProvenanceRepair);
    }
    return false;
  }

  void append_acceptance_preview(
      std::vector<std::string>& lines,
      const qt::memory::HydratedReview& detail) const {
    using CandidateKind = qt::memory::ReviewCandidateKind;
    qt::memory::ReviewAction action;
    action.actor = "user:interactive-preview";
    action.human_authority = true;
    switch (detail.candidate.kind) {
      case CandidateKind::SemanticConsolidation:
      case CandidateKind::Compression:
        action.kind = qt::memory::ReviewActionKind::Merge;
        break;
      case CandidateKind::Conflict:
        action.kind = qt::memory::ReviewActionKind::KeepBoth;
        break;
      case CandidateKind::Staleness:
        action.kind = qt::memory::ReviewActionKind::Retain;
        action.note = "preview current-memory retention";
        break;
      case CandidateKind::ProvenanceRepair:
        action.kind = qt::memory::ReviewActionKind::Deprecate;
        break;
      default:
        action.kind = qt::memory::ReviewActionKind::Approve;
        break;
    }
    lines.push_back("What accepting will do (default typed outcome):");
    try {
      const auto preview = runtime_->preview_review(detail.item.id, action);
      for (const auto& value : preview.will_do)
        lines.push_back("  Will: " + review_text_excerpt(value, 140U));
      for (const auto& change : preview.changes)
        lines.push_back("  Exact: " + change.record_type + " " +
                        change.record_id + " " + change.operation +
                        " scope={" +
                        exact_review_scope(detail.subject ? detail.subject->scope
                                                          : runtime_->scope()) +
                        "} after=" +
                        review_text_excerpt(change.after_json, 100U));
      if (preview.will_do.empty() && preview.changes.empty())
        lines.push_back("  Will: require one of the typed outcomes below");
      lines.push_back("Will not:");
      if (preview.will_not_do.empty())
        lines.push_back(
            "  Will not: validate, promote, or widen recall beyond the exact preview");
      else
        for (const auto& value : preview.will_not_do)
          lines.push_back("  Will not: " + review_text_excerpt(value, 140U));
    } catch (const std::exception& error) {
      lines.push_back("  BLOCKED: " + review_text_excerpt(error.what(), 140U));
      lines.push_back("Will not:");
      lines.push_back("  Will not: mutate any memory until a valid typed outcome is previewed");
    }
  }

  void show_detail(const std::string& id) {
    try {
      const auto detail = runtime_->review_detail(id);
      persist_view(id);
      std::vector<std::string> lines = {
          "Review Detail  [" + detail.item.priority + "] " + detail.item.kind,
          "Queue: " + queue_title_ + "  |  " + review_age(detail.item.created_at),
          "Scope (exact): " +
              exact_review_scope(detail.subject ? detail.subject->scope
                                                : runtime_->scope()),
          "Proposal: " + review_text_excerpt(review_candidate_content(detail), 150U),
          "Reason: " + review_text_excerpt(detail.item.reason, 150U),
          "Risk: " + detail.risk.risk_class + " (" +
              format_decimal(detail.risk.score) + ")  |  evidence " +
              format_decimal(detail.item.provenance.evidence_strength),
          "Recall now: strict=" + yes_no(detail.recall.strict) +
              " balanced=" + yes_no(detail.recall.balanced) +
              " exploratory=" + yes_no(detail.recall.broad),
          "Keys: A approve  E edit  R reject  D defer  Next [S]  Esc back",
      };
      if (detail.subject) {
        lines.push_back("Subject: " + detail.subject->id + "  " +
                        qt::memory::to_string(detail.subject->type) + "/" +
                        qt::memory::to_string(detail.subject->status) +
                        "  confidence=" + format_decimal(detail.subject->confidence));
      } else {
        lines.push_back("Subject: unavailable");
      }
      using CandidateKind = qt::memory::ReviewCandidateKind;
      if (detail.candidate.kind == CandidateKind::DreamArtifact) {
        if (const auto* dream =
                std::get_if<qt::memory::DreamArtifactCandidate>(
                    &detail.candidate.payload);
            dream != nullptr && dream->research_required) {
          lines.push_back(
              "Research required: attach independently supplied evidence before stronger trust.");
          if (detail.related.empty())
            lines.push_back("Linked research successors: none");
          else
            for (const auto& related : detail.related)
              lines.push_back("Linked research successor: " + related.id + "  " +
                              review_text_excerpt(related.content, 120U));
        }
      } else if (detail.candidate.kind == CandidateKind::DreamResearchSuccessor) {
        if (const auto* research =
                std::get_if<qt::memory::DreamResearchCandidate>(
                    &detail.candidate.payload)) {
          const auto original = std::find_if(
              detail.sources.begin(), detail.sources.end(),
              [&](const qt::memory::MemoryEntry& source) {
                return source.id == research->dream_id;
              });
          lines.push_back(
              "Original dream: " + research->dream_id + "  " +
              (original == detail.sources.end()
                   ? std::string("unavailable")
                   : review_text_excerpt(original->content, 130U)));
          lines.push_back(
              "Supplied evidence: " +
              (research->evidence_uri.empty() ? std::string("unavailable")
                                              : research->evidence_uri));
          if (detail.subject) {
            lines.push_back("Research successor: " + detail.subject->id + "  " +
                            review_text_excerpt(detail.subject->content, 130U));
          }
        }
      } else if (detail.candidate.kind == CandidateKind::Staleness) {
        lines.push_back(
            "Staleness: needs review; this is not an assertion that the memory is false.");
      } else if (detail.candidate.kind == CandidateKind::ProvenanceRepair) {
        lines.push_back(
            "Provenance repair: attach evidence, narrow scope, or deprecate to downgrade trust.");
      }
      std::string visible_approval = "Approve";
      if (detail.candidate.kind == CandidateKind::DreamArtifact ||
          detail.candidate.kind == CandidateKind::DreamResearchSuccessor ||
          detail.candidate.kind == CandidateKind::ProcedureRevision)
        visible_approval = "Approve as hypothesis";
      else if (detail.candidate.kind == CandidateKind::General)
        visible_approval = "Approve as active";
      lines.push_back("Outcomes: " + visible_approval +
                      " | Edit & approve | Reject | Defer / Snooze | "
                      "Inspect sources | Next [S] | Back");
      if (detail.candidate.kind == CandidateKind::SemanticConsolidation) {
        for (const auto& source : detail.sources) {
          lines.push_back(
              "Selected memory " + source.id + ": " +
              qt::memory::to_string(source.status) + " confidence=" +
              format_decimal(source.confidence) + " evidence=" +
              format_decimal(source.provenance.evidence_strength) + " uses=" +
              std::to_string(source.access_count) + " scope={" +
              exact_review_scope(source.scope) + "} source=" +
              qt::memory::to_string(source.provenance.source_type) + " " +
              review_text_excerpt(source.content, 90U));
        }
        for (const auto& edge : detail.edges)
          lines.push_back("Relationship/conflict: " + edge.from_id + " --" +
                          edge.type + "--> " + edge.to_id);
      }
      append_acceptance_preview(lines, detail);
      for (const auto& diagnostic : detail.blocking_diagnostics) {
        lines.push_back("BLOCKED: " + review_text_excerpt(diagnostic, 150U));
      }
      if (!detail.procedure_runs.empty()) {
        const auto& run = detail.procedure_runs.front();
        lines.push_back(
            "Recent procedure outcome: " +
            std::string(qt::memory::to_string(run.outcome)) + "  " +
            review_text_excerpt(run.notes, 100U) +
            (run.evidence_uri.empty() ? std::string{}
                                      : "  evidence=" + run.evidence_uri));
      }

      std::vector<qt::tui::SelectItem> actions;
      add_action(actions, detail, qt::memory::ReviewActionKind::Approve,
                 visible_approval, "A", "Apply the typed proposal");
      add_action(actions, detail, qt::memory::ReviewActionKind::EditAndApprove,
                 "Edit & approve", "E", "Edit proposal text before applying");
      const bool relationship_review =
          detail.candidate.kind == CandidateKind::SemanticConsolidation ||
          detail.candidate.kind == CandidateKind::Compression;
      const bool conflict_review =
          detail.candidate.kind == CandidateKind::Conflict;
      add_action(actions, detail, qt::memory::ReviewActionKind::Reject,
                 relationship_review
                     ? "No relationship"
                     : conflict_review ? "Not a conflict" : "Reject",
                 "R", relationship_review
                          ? "Reject the proposed relationship without changing memories"
                          : conflict_review
                                ? "Dismiss this conflict without changing memories"
                                : "Reject without deleting audit history");
      add_action(actions, detail, qt::memory::ReviewActionKind::Defer,
                 detail.candidate.kind == CandidateKind::Staleness
                     ? "Retain / review later"
                     : "Defer / Snooze",
                 "D", "Resurface later without domain mutation");
      add_action(actions, detail, qt::memory::ReviewActionKind::Relate,
                 "Relate", {}, "Preserve memories and add related edges");
      add_action(actions, detail,
                 qt::memory::ReviewActionKind::MarkContradiction,
                 "Mark contradiction", {}, "Preserve both with contradiction edges");
      add_action(actions, detail, qt::memory::ReviewActionKind::KeepBoth,
                 "Keep both", {}, "Preserve both with a conflict warning");
      add_action(actions, detail, qt::memory::ReviewActionKind::Supersede,
                 detail.candidate.kind == CandidateKind::Staleness
                     ? "Supersede with current memory"
                     : "Supersede",
                 {}, "Choose the canonical surviving memory");
      add_action(actions, detail, qt::memory::ReviewActionKind::Merge,
                 detail.candidate.kind == CandidateKind::Conflict
                     ? "Merge conflicting memories"
                 : detail.candidate.kind == CandidateKind::Compression
                     ? "Merge compressed canonical memory"
                     : "Merge canonical memory",
                 {}, "Create a canonical hypothesis and preserve lineage");
      add_action(actions, detail, qt::memory::ReviewActionKind::NarrowScope,
                 "Narrow to current scope", {},
                 "Create a current-scope copy and supersede the broader memory");
      add_action(actions, detail, qt::memory::ReviewActionKind::Revalidate,
                 detail.candidate.kind == CandidateKind::Staleness
                     ? "Update / revalidate from evidence"
                     : "Revalidate",
                 {}, "Require current independent evidence");
      add_action(actions, detail, qt::memory::ReviewActionKind::Deprecate,
                 detail.candidate.kind == CandidateKind::ProvenanceRepair
                     ? "Deprecate / downgrade trust"
                     : "Deprecate stale memory",
                 {}, "Remove the memory from ordinary recall");
      add_action(actions, detail, qt::memory::ReviewActionKind::Retain,
                 "Retain current memory", {},
                 "Keep the memory with an explicit reviewed-current marker");
      add_action(actions, detail, qt::memory::ReviewActionKind::AttachEvidence,
                 detail.candidate.kind == CandidateKind::DreamArtifact
                     ? "Attach independent evidence"
                     : "Attach evidence",
                 {},
                 "Repair provenance with independently supplied evidence");
      add_action(actions, detail, qt::memory::ReviewActionKind::Reopen,
                 "Reopen", {}, "Return this decision to the pending queue");
      add_action(actions, detail, qt::memory::ReviewActionKind::Compensate,
                 "Correct with compensation", {},
                 "Append a governed correction without rewriting history");
      actions.push_back({"inspect", "Inspect sources",
                         std::to_string(detail.sources.size()) + " sources, " +
                             std::to_string(detail.edges.size()) + " graph edges"});
      actions.push_back({"next", "Next [S]", "Skip to the next item without mutation"});
      actions.push_back({"back", "Back to queue", "Position is preserved"});

      auto panel = std::make_shared<ReviewMenuOverlay>(
          std::move(lines), std::move(actions), 0U, 13U);
      std::weak_ptr<ReviewInboxController> weak = shared_from_this();
      panel->set_select_callback([weak, id](const qt::tui::SelectItem& item) {
        const auto self = weak.lock();
        if (!self) return;
        if (item.value == "back") self->return_to_queue();
        else if (item.value == "next") self->show_next(id);
        else if (item.value == "inspect") self->show_evidence(id);
        else self->begin_action(
            id, qt::memory::review_action_kind_from_string(item.value));
      });
      panel->set_shortcut_callback(
          [weak, id](char key, const qt::tui::SelectItem*) {
            if (const auto self = weak.lock()) self->shortcut_action(id, key);
          });
      panel->set_cancel_callback([weak]() {
        if (const auto self = weak.lock()) self->return_to_queue();
      });
      show_review_overlay(panel);
    } catch (const std::exception& error) {
      fail("Unable to open review detail", error);
    }
  }

  static std::string yes_no(bool value) { return value ? "yes" : "no"; }

  static void add_action(std::vector<qt::tui::SelectItem>& actions,
                         const qt::memory::HydratedReview& review,
                         qt::memory::ReviewActionKind kind,
                         std::string label, std::string shortcut,
                         std::string description) {
    if (!actionable(review, kind)) return;
    if (!shortcut.empty()) label += " [" + shortcut + "]";
    actions.push_back({qt::memory::to_string(kind), std::move(label),
                       std::move(description)});
  }

  void return_to_queue() {
    show_queue(queue_title_, queue_query_, true);
  }

  void show_evidence(const std::string& id) {
    try {
      const auto detail = runtime_->review_detail(id);
      std::vector<std::string> lines = {
          "Evidence and Context",
          "Review: " + detail.item.id,
          std::string("Provenance: ") +
              qt::memory::to_string(detail.item.provenance.source_type) +
              "  " + detail.item.provenance.source_uri,
          "Source hash: " +
              (detail.item.provenance.source_hash.empty()
                   ? std::string("none")
                   : detail.item.provenance.source_hash),
          "Source/model: " + detail.item.provenance.created_by,
          "Tags: " + join_review_values(detail.item.tags),
      };
      if (detail.subject) {
        lines.push_back("Subject content: " +
                        review_text_excerpt(detail.subject->content, 180U));
        lines.push_back("Usage/outcomes: accesses=" +
                        std::to_string(detail.subject->access_count) +
                        " success=" + std::to_string(detail.subject->success_count) +
                        " failure=" + std::to_string(detail.subject->failure_count));
      }
      for (const auto& source : detail.sources) {
        lines.push_back("Source " + source.id + " [" +
                        qt::memory::to_string(source.status) +
                        " confidence=" + format_decimal(source.confidence) +
                        " evidence=" +
                        format_decimal(source.provenance.evidence_strength) +
                        "] scope={" + exact_review_scope(source.scope) + "}: " +
                        review_text_excerpt(source.content, 130U));
        lines.push_back("  provenance=" +
                        std::string(qt::memory::to_string(
                            source.provenance.source_type)) +
                        " uri=" + source.provenance.source_uri +
                        " hash=" + source.provenance.source_hash);
      }
      for (const auto& related : detail.related)
        lines.push_back("Related " + related.id + ": " +
                        review_text_excerpt(related.content, 150U));
      for (const auto& edge : detail.edges) {
        lines.push_back("Edge: " + edge.from_id + " --" + edge.type + "--> " +
                        edge.to_id);
      }
      for (const auto& warning : detail.recall.warnings) {
        lines.push_back("Warning: " + warning);
      }
      const std::size_t procedure_limit =
          std::min<std::size_t>(5U, detail.procedure_runs.size());
      for (std::size_t index = 0U; index < procedure_limit; ++index) {
        const auto& run = detail.procedure_runs[index];
        lines.push_back(
            "Procedure run " + run.id + ": " +
            qt::memory::to_string(run.outcome) + "  " +
            review_text_excerpt(run.notes, 120U) +
            (run.evidence_uri.empty() ? std::string{}
                                      : "  evidence=" + run.evidence_uri));
      }
      std::vector<qt::tui::SelectItem> actions;
      const auto evidence = source_evidence_path(detail.item.provenance.source_uri);
      if (evidence)
        actions.push_back({"open", "Open source evidence", evidence->generic_string()});
      else if (!detail.item.provenance.source_uri.empty())
        lines.push_back(
            "BLOCKED: source evidence is not a permitted local workspace path");
      actions.push_back({"back", "Back to review", "No changes"});
      auto panel = std::make_shared<ReviewMenuOverlay>(
          std::move(lines), std::move(actions), 0U, 2U);
      std::weak_ptr<ReviewInboxController> weak = shared_from_this();
      panel->set_select_callback([weak, id, evidence](const qt::tui::SelectItem& item) {
        if (const auto self = weak.lock()) {
          if (item.value == "open" && evidence)
            self->open_source_evidence(id, *evidence);
          else
            self->show_detail(id);
        }
      });
      panel->set_cancel_callback([weak, id]() {
        if (const auto self = weak.lock()) self->show_detail(id);
      });
      show_review_overlay(panel);
    } catch (const std::exception& error) {
      fail("Unable to inspect review evidence", error);
    }
  }

  std::optional<std::filesystem::path> source_evidence_path(
      std::string uri) const {
    if (uri.rfind("file://", 0U) == 0U) uri.erase(0U, 7U);
    if (uri.empty() || uri.find("://") != std::string::npos) return std::nullopt;
    std::error_code error;
    std::filesystem::path requested(uri);
    if (requested.is_relative()) {
      if (evidence_tool_context_.current_directory)
        requested = *evidence_tool_context_.current_directory / requested;
      else if (evidence_tool_context_.workspace_root)
        requested = *evidence_tool_context_.workspace_root / requested;
    }
    auto path = std::filesystem::weakly_canonical(requested, error);
    if (error || !std::filesystem::is_regular_file(path, error))
      return std::nullopt;
    if (evidence_tool_context_.allow_external_filesystem_access) return path;
    if (!evidence_tool_context_.workspace_root) return std::nullopt;
    return qt::tools::is_workspace_path_allowed(
               path, *evidence_tool_context_.workspace_root,
               evidence_tool_context_)
               ? std::optional<std::filesystem::path>(path)
               : std::nullopt;
  }

  void open_source_evidence(const std::string& id,
                            const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::string content(8192U, '\0');
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    content.resize(static_cast<std::size_t>(input.gcount()));
    qt::memory::MemoryService::redact_secrets(content);
    auto panel = std::make_shared<ReviewMenuOverlay>(
        std::vector<std::string>{"Source evidence", path.generic_string(), content},
        std::vector<qt::tui::SelectItem>{{"back", "Back to evidence", "Read-only"}});
    std::weak_ptr<ReviewInboxController> weak = shared_from_this();
    panel->set_select_callback([weak, id](const qt::tui::SelectItem&) {
      if (const auto self = weak.lock()) self->show_evidence(id);
    });
    panel->set_cancel_callback([weak, id]() {
      if (const auto self = weak.lock()) self->show_evidence(id);
    });
    show_review_overlay(panel);
  }

  void begin_action(const std::string& id,
                    qt::memory::ReviewActionKind kind) {
    try {
      const auto detail = runtime_->review_detail(id);
      if (!actionable(detail, kind)) {
        ui_.append_transcript_line(
            "Review action is not available for this candidate kind.");
        show_detail(id);
        return;
      }
      qt::memory::ReviewAction action;
      action.kind = kind;
      action.actor = "user:interactive";
      action.human_authority = true;
      action.expected_revision = detail.item.revision;
      if (kind == qt::memory::ReviewActionKind::EditAndApprove ||
          kind == qt::memory::ReviewActionKind::Merge) {
        auto prompt = std::make_shared<PromptOverlay>(
            kind == qt::memory::ReviewActionKind::Merge
                ? "Edit canonical merge"
                : "Edit review proposal",
            "Content: ",
            "Enter applies this text to the preview; Esc returns unchanged");
        const auto current = review_candidate_content(detail);
        prompt->set_value(current.empty() && detail.subject
                              ? detail.subject->content
                              : current);
        std::weak_ptr<ReviewInboxController> weak = shared_from_this();
        prompt->set_submit_callback([weak, id, action](const std::string& content) mutable {
          const auto self = weak.lock();
          if (!self) return;
          qt::memory::ReviewEdit edit;
          edit.content = content;
          action.edit = std::move(edit);
          if (action.kind == qt::memory::ReviewActionKind::EditAndApprove)
            self->show_edit_fields(id, std::move(action));
          else
            self->prompt_note(id, std::move(action));
        });
        prompt->set_cancel_callback([weak, id]() {
          if (const auto self = weak.lock()) self->show_detail(id);
        });
        show_review_overlay(prompt);
        return;
      }
      if (kind == qt::memory::ReviewActionKind::Defer) {
        show_defer_choices(id, std::move(action));
        return;
      }
      if (kind == qt::memory::ReviewActionKind::Supersede) {
        show_target_choices(id, std::move(action), detail);
        return;
      }
      if (kind == qt::memory::ReviewActionKind::NarrowScope) {
        qt::memory::ReviewEdit edit;
        edit.scope = runtime_->scope();
        action.edit = std::move(edit);
        prompt_note(id, std::move(action));
        return;
      }
      if (kind == qt::memory::ReviewActionKind::Compensate) {
        if (!detail.subject) {
          throw std::invalid_argument(
              "compensation requires an available subject memory");
        }
        action.target_id = detail.subject->id;
        auto prompt = std::make_shared<PromptOverlay>(
            "Correct reviewed memory", "Replacement content: ",
            "A required audit reason is requested next; Esc returns");
        prompt->set_value(detail.subject->content);
        std::weak_ptr<ReviewInboxController> weak = shared_from_this();
        prompt->set_submit_callback(
            [weak, id, action](const std::string& content) mutable {
              const auto self = weak.lock();
              if (!self) return;
              qt::memory::ReviewEdit edit;
              edit.content = content;
              action.edit = std::move(edit);
              self->prompt_note(id, std::move(action));
            });
        prompt->set_cancel_callback([weak, id]() {
          if (const auto self = weak.lock()) self->show_detail(id);
        });
        show_review_overlay(prompt);
        return;
      }
      if (kind == qt::memory::ReviewActionKind::Revalidate ||
          kind == qt::memory::ReviewActionKind::AttachEvidence) {
        if (kind == qt::memory::ReviewActionKind::Revalidate &&
            detail.candidate.kind == qt::memory::ReviewCandidateKind::Staleness) {
          auto prompt = std::make_shared<PromptOverlay>(
              "Update stale memory", "Current content: ",
              "Edit the current value, then attach independent evidence");
          prompt->set_value(detail.subject ? detail.subject->content
                                           : std::string{});
          std::weak_ptr<ReviewInboxController> weak = shared_from_this();
          prompt->set_submit_callback(
              [weak, id, action](const std::string& content) mutable {
                const auto self = weak.lock();
                if (!self) return;
                qt::memory::ReviewEdit edit;
                edit.content = content;
                action.edit = std::move(edit);
                self->prompt_review_evidence(id, std::move(action));
              });
          prompt->set_cancel_callback([weak, id]() {
            if (const auto self = weak.lock()) self->show_detail(id);
          });
          show_review_overlay(prompt);
        } else {
          prompt_review_evidence(id, std::move(action));
        }
        return;
      }
      prompt_note(id, std::move(action));
    } catch (const std::exception& error) {
      fail("Unable to prepare review action", error);
    }
  }

  void show_edit_fields(const std::string& id,
                        qt::memory::ReviewAction action) {
    std::vector<qt::tui::SelectItem> fields = {
        {"finish", "Preview edited approval", "Review all exact stored changes"},
    };
    const auto detail = runtime_->review_detail(id);
    if (detail.candidate.kind == qt::memory::ReviewCandidateKind::General) {
      fields.push_back({"type", "Type", "Set the typed memory category"});
      fields.push_back(
          {"status", "Target trust state", "Candidate, hypothesis, or active"});
      fields.push_back({"tags", "Tags", "Replace the governed tag set"});
      fields.push_back(
          {"confidence", "Confidence", "Set a value from 0 to 1"});
      fields.push_back({"replacement", "Replacement link",
                        "Set an explicit replacement memory ID"});
    } else if (detail.candidate.kind ==
               qt::memory::ReviewCandidateKind::ProcedureRevision) {
      fields.push_back({"tags", "Tags", "Replace procedure tags"});
      fields.push_back(
          {"confidence", "Confidence", "Set a value from 0 to 1"});
      fields.push_back({"replacement", "Replacement link",
                        "Link the revised procedure"});
    }
    fields.push_back({"back", "Cancel edit", "Return without mutation"});
    auto panel = std::make_shared<ReviewMenuOverlay>(
        std::vector<std::string>{
            "Edit supported fields", "Content is staged.",
            "Scope narrowing remains a separate explicit governed action."},
        std::move(fields));
    std::weak_ptr<ReviewInboxController> weak = shared_from_this();
    panel->set_select_callback(
        [weak, id, action](const qt::tui::SelectItem& item) mutable {
          const auto self = weak.lock();
          if (!self) return;
          if (item.value == "finish") self->prompt_note(id, std::move(action));
          else if (item.value == "back") self->show_detail(id);
          else if (item.value == "type" || item.value == "status")
            self->show_edit_choice(id, std::move(action), item.value);
          else
            self->prompt_edit_value(id, std::move(action), item.value);
        });
    panel->set_cancel_callback([weak, id]() {
      if (const auto self = weak.lock()) self->show_detail(id);
    });
    show_review_overlay(panel);
  }

  void show_edit_choice(const std::string& id, qt::memory::ReviewAction action,
                        const std::string& field) {
    std::vector<qt::tui::SelectItem> choices;
    if (field == "type") {
      for (const char* value : {"observation", "fact", "decision", "constraint",
                                "preference", "procedural"})
        choices.push_back({value, value, "Typed memory category"});
    } else {
      for (const char* value : {"candidate", "hypothesis", "active"})
        choices.push_back({value, value, "Governed target trust state"});
    }
    auto panel = std::make_shared<ReviewMenuOverlay>(
        std::vector<std::string>{field == "type" ? "Choose memory type"
                                                 : "Choose target trust state"},
        std::move(choices));
    std::weak_ptr<ReviewInboxController> weak = shared_from_this();
    panel->set_select_callback(
        [weak, id, action, field](const qt::tui::SelectItem& item) mutable {
          const auto self = weak.lock();
          if (!self) return;
          if (!action.edit) action.edit = qt::memory::ReviewEdit{};
          if (field == "type")
            action.edit->type = qt::memory::memory_type_from_string(item.value);
          else
            action.edit->target_status =
                qt::memory::memory_status_from_string(item.value);
          self->show_edit_fields(id, std::move(action));
        });
    panel->set_cancel_callback([weak, id, action]() mutable {
      if (const auto self = weak.lock())
        self->show_edit_fields(id, std::move(action));
    });
    show_review_overlay(panel);
  }

  void prompt_edit_value(const std::string& id, qt::memory::ReviewAction action,
                         const std::string& field) {
    auto prompt = std::make_shared<PromptOverlay>(
        "Edit " + field, field + ": ",
        field == "tags" ? "Comma-separated tags"
        : field == "confidence" ? "Number from 0 to 1"
                                 : "Existing in-scope memory ID");
    std::weak_ptr<ReviewInboxController> weak = shared_from_this();
    prompt->set_submit_callback(
        [weak, id, action, field](const std::string& value) mutable {
          const auto self = weak.lock();
          if (!self) return;
          try {
            if (!action.edit) action.edit = qt::memory::ReviewEdit{};
            if (field == "tags") {
              std::vector<std::string> tags;
              std::istringstream stream(value);
              std::string tag;
              while (std::getline(stream, tag, ',')) {
                tag = trim_ascii_copy(std::move(tag));
                if (!tag.empty()) tags.push_back(std::move(tag));
              }
              action.edit->tags = std::move(tags);
            } else if (field == "confidence") {
              const double confidence = std::stod(value);
              if (confidence < 0.0 || confidence > 1.0)
                throw std::invalid_argument("confidence must be from 0 to 1");
              action.edit->confidence = confidence;
            } else {
              action.edit->replacement_id = trim_ascii_copy(value);
            }
            self->show_edit_fields(id, std::move(action));
          } catch (const std::exception& error) {
            self->ui_.append_transcript_line(std::string("Invalid review edit: ") +
                                             error.what());
            self->show_edit_fields(id, std::move(action));
          }
        });
    prompt->set_cancel_callback([weak, id, action]() mutable {
      if (const auto self = weak.lock())
        self->show_edit_fields(id, std::move(action));
    });
    show_review_overlay(prompt);
  }

  void prompt_review_evidence(const std::string& id,
                              qt::memory::ReviewAction action) {
    auto prompt = std::make_shared<PromptOverlay>(
        action.kind == qt::memory::ReviewActionKind::Revalidate
            ? "Revalidation evidence"
            : "Attach review evidence",
        "URI: ", "Independent evidence is required; Esc returns");
    std::weak_ptr<ReviewInboxController> weak = shared_from_this();
    prompt->set_submit_callback(
        [weak, id, action](const std::string& uri) mutable {
          const auto self = weak.lock();
          if (!self) return;
          action.evidence_uri = uri;
          self->prompt_note(id, std::move(action));
        });
    prompt->set_cancel_callback([weak, id]() {
      if (const auto self = weak.lock()) self->show_detail(id);
    });
    show_review_overlay(prompt);
  }

  void show_defer_choices(const std::string& id,
                          qt::memory::ReviewAction action) {
    auto panel = std::make_shared<ReviewMenuOverlay>(
        std::vector<std::string>{"Defer review", "Choose when it should resurface."},
        std::vector<qt::tui::SelectItem>{
            {"3600", "In one hour", "Short interruption"},
            {"86400", "Tomorrow", "Resurface in 24 hours"},
            {"604800", "In one week", "Resurface in 7 days"},
            {"back", "Back", "Do not defer"}});
    std::weak_ptr<ReviewInboxController> weak = shared_from_this();
    panel->set_select_callback([weak, id, action](const qt::tui::SelectItem& item) mutable {
      const auto self = weak.lock();
      if (!self) return;
      if (item.value == "back") {
        self->show_detail(id);
        return;
      }
      action.deferred_until = std::chrono::system_clock::now() +
                              std::chrono::seconds(std::stoll(item.value));
      self->prompt_note(id, std::move(action));
    });
    panel->set_cancel_callback([weak, id]() {
      if (const auto self = weak.lock()) self->show_detail(id);
    });
    show_review_overlay(panel);
  }

  void show_target_choices(const std::string& id,
                           qt::memory::ReviewAction action,
                           const qt::memory::HydratedReview& detail) {
    std::vector<qt::tui::SelectItem> targets;
    const auto* conflict =
        std::get_if<qt::memory::ConflictCandidate>(&detail.candidate.payload);
    for (const auto& source : detail.sources) {
      std::string label = source.id;
      if (conflict != nullptr && !conflict->memory_ids.empty())
        label = source.id == conflict->memory_ids.front()
                    ? "Old remains current"
                    : "New supersedes old";
      targets.push_back({source.id, std::move(label),
                         review_text_excerpt(source.content, 80U)});
    }
    if (detail.subject) {
      std::string label = detail.subject->id;
      if (conflict != nullptr && !conflict->memory_ids.empty())
        label = detail.subject->id == conflict->memory_ids.front()
                    ? "Old remains current"
                    : "New supersedes old";
      targets.push_back({detail.subject->id, std::move(label),
                         review_text_excerpt(detail.subject->content, 80U)});
    }
    targets.push_back({"__back__", "Back", "Do not supersede"});
    auto panel = std::make_shared<ReviewMenuOverlay>(
        std::vector<std::string>{"Choose the surviving memory",
                                 "Other reviewed memories will point to it."},
        std::move(targets));
    std::weak_ptr<ReviewInboxController> weak = shared_from_this();
    panel->set_select_callback([weak, id, action](const qt::tui::SelectItem& item) mutable {
      const auto self = weak.lock();
      if (!self) return;
      if (item.value == "__back__") {
        self->show_detail(id);
        return;
      }
      action.target_id = item.value;
      self->prompt_note(id, std::move(action));
    });
    panel->set_cancel_callback([weak, id]() {
      if (const auto self = weak.lock()) self->show_detail(id);
    });
    show_review_overlay(panel);
  }

  void prompt_note(const std::string& id, qt::memory::ReviewAction action) {
    auto prompt = std::make_shared<PromptOverlay>(
        "Review note", "Note: ",
        "Optional audit note; Enter previews, Esc cancels");
    std::weak_ptr<ReviewInboxController> weak = shared_from_this();
    prompt->set_submit_callback([weak, id, action](const std::string& note) mutable {
      if (const auto self = weak.lock()) {
        action.note = note;
        self->show_confirmation(id, std::move(action));
      }
    });
    prompt->set_cancel_callback([weak, id]() {
      if (const auto self = weak.lock()) self->show_detail(id);
    });
    show_review_overlay(prompt);
  }

  void show_confirmation(const std::string& id,
                         qt::memory::ReviewAction action) {
    try {
      auto preview = runtime_->preview_review(id, action);
      if (!preview.applicable) {
        ui_.append_transcript_line(
            "Review action cannot be applied: " +
            (preview.warnings.empty() ? std::string("not applicable")
                                      : preview.warnings.front()));
        show_detail(id);
        return;
      }
      action.expected_revision = preview.review_revision;
      action.idempotency_key = preview.idempotency_token;
      const auto detail = runtime_->review_detail(id);
      const auto& confirmation_scope =
          action.edit && action.edit->scope
              ? *action.edit->scope
              : detail.subject ? detail.subject->scope : runtime_->scope();
      std::vector<std::string> lines = {
          "Confirm " + std::string(qt::memory::to_string(action.kind)),
          "Review: " + id,
          "Scope (exact): " + exact_review_scope(confirmation_scope),
      };
      for (const auto& value : preview.will_do) {
        lines.push_back("Will: " + review_text_excerpt(value, 160U));
      }
      for (const auto& value : preview.will_not_do) {
        lines.push_back("Will not: " + review_text_excerpt(value, 160U));
      }
      for (const auto& warning : preview.warnings) {
        lines.push_back("Warning: " + review_text_excerpt(warning, 160U));
      }
      for (const auto& change : preview.changes) {
        lines.push_back("Change: " + change.record_type + " " +
                        change.record_id + " " + change.operation);
        lines.push_back("  Before: " +
                        review_text_excerpt(change.before_json, 140U));
        lines.push_back("  After: " +
                        review_text_excerpt(change.after_json, 140U));
      }
      if (!action.note.empty()) lines.push_back("Audit note: " + action.note);
      auto panel = std::make_shared<ReviewMenuOverlay>(
          std::move(lines),
          std::vector<qt::tui::SelectItem>{
              {"confirm", "Confirm", "Apply this exact preview atomically"},
              {"cancel", "Cancel", "Return without changes"}},
          1U, 2U);
      std::weak_ptr<ReviewInboxController> weak = shared_from_this();
      panel->set_select_callback([weak, id, action](const qt::tui::SelectItem& item) mutable {
        const auto self = weak.lock();
        if (!self) return;
        if (item.value == "confirm") self->apply(id, std::move(action));
        else self->show_detail(id);
      });
      panel->set_cancel_callback([weak, id]() {
        if (const auto self = weak.lock()) self->show_detail(id);
      });
      show_review_overlay(panel);
    } catch (const std::exception& error) {
      fail("Unable to preview review action", error);
    }
  }

  void apply(const std::string& id, qt::memory::ReviewAction action) {
    try {
      const auto result = runtime_->apply_review_action(id, std::move(action));
      ui_.hide_overlay();
      std::string receipt =
          "Memory review " + id + ": " + qt::memory::to_string(result.status) +
          "; decision=" + (result.decision_persisted ? "persisted" : "not persisted") +
          "; domain=" + (result.domain_mutations_applied ? "applied" : "unchanged") +
          "; revision=" + std::to_string(result.revision);
      if (!result.changed_ids.empty()) {
        receipt += "; changed=" + join_review_values(result.changed_ids);
      }
      if (result.idempotent_replay) receipt += "; idempotent replay";
      if (!result.error.empty()) receipt += "; error=" + result.error;
      ui_.append_transcript_line(std::move(receipt));
      if (!result.failed_ids.empty()) {
        ui_.append_transcript_line("Review failed records: " +
                                   join_review_values(result.failed_ids));
      }
      return_to_queue();
    } catch (const std::exception& error) {
      ui_.append_transcript_line(std::string("Memory review was not changed: ") +
                                 error.what());
      show_detail(id);
    }
  }

  qt::app::InteractiveTerminalSession& ui_;
  std::shared_ptr<qt::memory::MemoryRuntime> runtime_;
  qt::memory::ReviewQuery queue_query_;
  qt::memory::ReviewQuery filter_query_;
  qt::memory::ReviewQuery batch_query_;
  std::set<std::string> batch_selected_ids_;
  std::string history_filter_ = "all";
  qt::tools::ToolContext evidence_tool_context_;
  std::string queue_title_ = "Pending reviews";
  std::size_t queue_position_ = 0U;
  bool view_state_loaded_ = false;
  std::optional<qt::memory::ReviewViewState> restored_view_state_;
  std::string editor_text_on_open_;
  std::shared_ptr<ReviewInboxController> keep_alive_;
};

bool model_matches_search(const qt::ai::ModelCatalogModelRecord& model,
                          std::string_view search) {
  if (search.empty()) {
    return true;
  }
  const std::string needle = lowercase_ascii(std::string(search));
  const std::string provider = lowercase_ascii(model.ref.provider);
  const std::string name = lowercase_ascii(model.ref.name);
  const std::string display = lowercase_ascii(model.display_name);
  return provider.find(needle) != std::string::npos ||
         name.find(needle) != std::string::npos ||
         display.find(needle) != std::string::npos;
}

std::string trim_decimal_zeros(std::string value) {
  const auto dot = value.find('.');
  if (dot == std::string::npos) {
    return value;
  }
  while (!value.empty() && value.back() == '0') {
    value.pop_back();
  }
  if (!value.empty() && value.back() == '.') {
    value.pop_back();
  }
  return value;
}

std::string format_usd_per_million(double value) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(value < 1.0 ? 4 : 2) << value;
  return "$" + trim_decimal_zeros(stream.str()) + "/1M";
}

std::string model_pricing_summary(
    const qt::ai::ModelCatalogModelRecord& model) {
  const auto rates = qt::ai::token_cost_rates_from_model_metadata(model);
  if (!rates) {
    return "";
  }
  return "in " + format_usd_per_million(rates->input_usd_per_million) +
         " / out " + format_usd_per_million(rates->output_usd_per_million);
}

std::string model_display_with_pricing(
    const qt::ai::ModelCatalogModelRecord& model) {
  std::string description = model.display_name;
  const std::string pricing = model_pricing_summary(model);
  if (!pricing.empty()) {
    if (!description.empty()) {
      description += " ";
    }
    description += pricing;
  }
  return description;
}

std::string format_openrouter_credit_amount(double value) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(value < 10.0 ? 2 : 1) << value;
  return trim_decimal_zeros(stream.str());
}

bool is_openrouter_provider(std::string_view provider) {
  return lowercase_ascii(std::string(provider)) == "openrouter";
}

std::optional<std::string> provider_metadata_api_key(
    const qt::ai::ModelCatalogProviderRecord& provider) {
  if (!provider.metadata.is_object() || !provider.metadata.contains("apiKey") ||
      !provider.metadata.at("apiKey").is_string()) {
    return std::nullopt;
  }
  const std::string value = provider.metadata.at("apiKey").get<std::string>();
  return value.empty() ? std::nullopt : std::optional<std::string>{value};
}

bool provider_config_command_resolves(const std::string& configured) {
  static std::unordered_map<std::string, bool> cache;
  const auto cached = cache.find(configured);
  if (cached != cache.end()) {
    return cached->second;
  }

  bool resolved = false;
  const auto result = qt::foundation::run_shell_command(
      configured.substr(1U), std::nullopt, std::int64_t{10'000});
  if (result && !result.value().timed_out && result.value().exit_code == 0) {
    resolved = !trim_ascii_copy(result.value().stdout_text).empty();
  }
  cache.emplace(configured, resolved);
  return resolved;
}

bool provider_metadata_flag(const qt::ai::ModelCatalogProviderRecord& provider,
                            std::string_view key) {
  if (!provider.metadata.is_object() ||
      !provider.metadata.contains(std::string(key))) {
    return false;
  }
  const auto& value = provider.metadata.at(std::string(key));
  return value.is_boolean() && value.get<bool>();
}

bool provider_has_available_credentials(
    const qt::ai::AuthStorage& auth_storage,
    const qt::ai::ModelCatalogProviderRecord& provider) {
  if (provider_metadata_flag(provider, "local") ||
      provider_metadata_flag(provider, "direct") ||
      (provider.metadata.is_object() &&
       provider.metadata.contains("requiresAuth") &&
       provider.metadata.at("requiresAuth").is_boolean() &&
       !provider.metadata.at("requiresAuth").get<bool>())) {
    return true;
  }

  const auto configured_api_key = provider_metadata_api_key(provider);
  if (configured_api_key) {
    const std::string configured = trim_ascii_copy(*configured_api_key);
    if (!configured.empty() && configured.front() == '!' &&
        !provider_config_command_resolves(configured)) {
      return false;
    }
  }

  const auto status = qt::app::describe_auth_provider_status(
      auth_storage, provider.descriptor.id, configured_api_key);
  if (status.configured) {
    return true;
  }

  return qt::ai::get_env_api_key(provider.descriptor.id).has_value();
}

class MemoryModelPicker final : public qt::tui::Component,
                                public qt::tui::Focusable {
 public:
  MemoryModelPicker(std::string title, std::vector<qt::tui::SelectItem> choices,
                    std::size_t selected, qt::tui::SettingItem::SubmenuDone done)
      : title_(std::move(title)),
        choices_(std::move(choices)),
        list_(std::nullopt, choices_, selected, 8U) {
    qt::tui::SelectLayoutOptions layout;
    layout.min_primary_column_width = 24U;
    layout.max_primary_column_width = 64U;
    list_.set_layout(std::move(layout));
    search_.set_change_callback([this](const std::string& value) {
      const std::string query = lowercase_ascii(value);
      std::vector<qt::tui::SelectItem> matches;
      for (const auto& item : choices_) {
        if (lowercase_ascii(item.label + " " + item.description).find(query) !=
            std::string::npos) {
          matches.push_back(item);
        }
      }
      list_.set_items(std::move(matches));
    });
    list_.set_select_callback([done](const qt::tui::SelectItem& item) {
      done(item.value);
    });
    list_.set_cancel_callback([done]() { done(std::nullopt); });
  }

  qt::foundation::Result<qt::tui::RenderBuffer> render(
      const qt::tui::RenderConstraints& constraints) const override {
    std::vector<std::string> lines{title_};
    auto search = search_.render({constraints.max_width, 1U});
    if (!search) return search.error();
    lines.insert(lines.end(), search.value().lines.begin(), search.value().lines.end());
    const std::size_t available = constraints.max_height > 3U
                                      ? constraints.max_height - 3U : 1U;
    if (list_.selected_item() == nullptr) {
      lines.push_back("No matching models");
    } else {
      auto choices = list_.render({constraints.max_width, available});
      if (!choices) return choices.error();
      lines.insert(lines.end(), choices.value().lines.begin(), choices.value().lines.end());
    }
    lines.push_back("Type to filter · ↑/↓ navigate · Enter select · Esc cancel");
    return qt::tui::make_buffer(std::move(lines), constraints);
  }

  void handle_input(std::string_view bytes) override {
    const auto& keys = qt::tui::get_keybindings_const();
    if (keys.matches(bytes, "tui.select.up") ||
        keys.matches(bytes, "tui.cursor.up") ||
        keys.matches(bytes, "tui.select.down") ||
        keys.matches(bytes, "tui.cursor.down") ||
        keys.matches(bytes, "tui.select.confirm") ||
        keys.matches(bytes, "tui.input.submit") ||
        keys.matches(bytes, "tui.select.cancel") ||
        keys.matches(bytes, "tui.input.cancel")) {
      list_.handle_input(bytes);
    } else {
      search_.handle_input(bytes);
    }
  }

  void set_focused(bool focused) override { search_.set_focused(focused); }
  bool focused() const noexcept override { return search_.focused(); }

 private:
  std::string title_;
  std::vector<qt::tui::SelectItem> choices_;
  qt::tui::Input search_;
  qt::tui::Select list_;
};

bool model_is_available(const qt::ai::ModelCatalog& catalog,
                        const qt::ai::AuthStorage& auth_storage,
                        const qt::ai::ModelCatalogModelRecord& model) {
  const auto* provider =
      qt::ai::find_provider_record(catalog, model.ref.provider);
  return provider != nullptr &&
         provider_has_available_credentials(auth_storage, *provider);
}

std::string trim_trailing_line_endings(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.pop_back();
  }
  return text;
}

std::string strip_single_trailing_line_ending(std::string text) {
  if (!text.empty() && text.back() == '\n') {
    text.pop_back();
    if (!text.empty() && text.back() == '\r') {
      text.pop_back();
    }
  } else if (!text.empty() && text.back() == '\r') {
    text.pop_back();
  }
  return text;
}

std::optional<std::string> external_editor_command_from_env() {
  if (const char* visual = std::getenv("VISUAL")) {
    std::string command = trim_ascii_copy(visual);
    if (!command.empty()) {
      return command;
    }
  }
  if (const char* editor = std::getenv("EDITOR")) {
    std::string command = trim_ascii_copy(editor);
    if (!command.empty()) {
      return command;
    }
  }
  return std::nullopt;
}

std::string shell_quote_argument(std::string_view argument) {
#if defined(_WIN32)
  std::string quoted = "\"";
  for (const char ch : argument) {
    if (ch == '"' || ch == '\\') {
      quoted.push_back('\\');
    }
    quoted.push_back(ch);
  }
  quoted.push_back('"');
  return quoted;
#else
  std::string quoted = "'";
  for (const char ch : argument) {
    if (ch == '\'') {
      quoted += "'\\''";
    } else {
      quoted.push_back(ch);
    }
  }
  quoted.push_back('\'');
  return quoted;
#endif
}

qt::foundation::Result<qt::foundation::Path> make_external_editor_temp_path() {
  std::error_code error;
  const auto temp_dir = std::filesystem::temp_directory_path(error);
  if (error) {
    return qt::foundation::Result<qt::foundation::Path>::failure(
        qt::foundation::ErrorCode::io_error,
        "unable to locate temporary directory", error.value());
  }
  return temp_dir / ("qt-editor-" + qt::foundation::generate_uuid_v4_string() + ".md");
}

class ScopedPathRemoval {
 public:
  explicit ScopedPathRemoval(qt::foundation::Path path) : path_(std::move(path)) {}
  ~ScopedPathRemoval() {
    if (!path_.empty()) {
      std::error_code ignored;
      std::filesystem::remove(path_, ignored);
    }
  }

  ScopedPathRemoval(const ScopedPathRemoval&) = delete;
  ScopedPathRemoval& operator=(const ScopedPathRemoval&) = delete;

 private:
  qt::foundation::Path path_;
};

qt::foundation::Result<std::string> run_interactive_prompt(
    qt::harness::AgentHarness& harness,
    const ReferenceStartupSession& startup_session, std::string prompt,
    std::string message_id, std::vector<qt::ai::ImageContent> images = {},
    qt::ai::Json metadata = qt::ai::Json::object()) {
  const auto run = qt::app::run_noninteractive_prompt(
      harness, qt::app::NoninteractivePromptRequest{
                   std::move(prompt), std::move(message_id), std::move(images),
                   std::move(metadata)});
  if (!run) {
    return qt::foundation::Result<std::string>::failure(
        run.error().code, run.error().message, run.error().system_error);
  }

  if (startup_session.persistent && startup_session.repository_path) {
    const auto appended =
        append_startup_turn(*startup_session.repository_path, run.value());
    if (!appended.ok()) {
      return qt::foundation::Result<std::string>::failure(
          appended.error().code, appended.error().message,
          appended.error().system_error);
    }
  }

  return qt::foundation::Result<std::string>::success(
      trim_trailing_line_endings(run.value().print_output));
}

qt::foundation::Result<std::string> run_interactive_tool_call(
    qt::harness::AgentHarness& harness,
    const ReferenceStartupSession& startup_session, std::string user_text,
    qt::tools::ToolRequest request) {
  try {
    harness.clear_events();
    const auto transcript_start = harness.agent().state().transcript.size();
    const auto turn =
        harness.submit_tool_call(std::move(user_text), std::move(request));
    if (startup_session.persistent && startup_session.repository_path) {
      const auto& transcript = harness.agent().state().transcript;
      std::vector<qt::ai::Message> committed(
          transcript.begin() + static_cast<std::ptrdiff_t>(transcript_start),
          transcript.end());
      const auto appended = append_startup_messages(
          *startup_session.repository_path, committed);
      if (!appended) {
        return qt::foundation::Result<std::string>::failure(
            appended.error().code, appended.error().message,
            appended.error().system_error);
      }
    }
    if (!turn.ok) {
      return qt::foundation::Result<std::string>::failure(
          qt::foundation::ErrorCode::internal_error,
          turn.error.value_or("tool call failed"));
    }
    return qt::foundation::Result<std::string>::success({});
  } catch (const std::exception& error) {
    return qt::foundation::Result<std::string>::failure(
        qt::foundation::ErrorCode::internal_error, error.what());
  }
}

std::string interactive_bash_context_text(
    std::string_view command,
    std::string_view output,
    bool cancelled,
    bool truncated,
    const std::optional<std::string>& full_output_path,
    const std::optional<int>& exit_code) {
  std::string text = "Ran `" + std::string(command) + "`\n";
  if (!output.empty()) {
    text += "```\n";
    text += std::string(output);
    text += "\n```";
  } else {
    text += "(no output)";
  }
  if (cancelled) {
    text += "\n\n(command cancelled)";
  } else if (exit_code && *exit_code != 0) {
    text += "\n\nCommand exited with code " + std::to_string(*exit_code);
  }
  if (truncated && full_output_path && !full_output_path->empty()) {
    text += "\n\n[Output truncated. Full output: " + *full_output_path + "]";
  }
  return text;
}

struct ReferenceInteractiveBashExecution {
  std::string command;
  std::string output;
  std::optional<int> exit_code;
  bool cancelled = false;
  bool truncated = false;
  std::optional<std::string> full_output_path;
  bool exclude_from_context = false;
};

qt::foundation::Result<ReferenceInteractiveBashExecution>
execute_interactive_bash(
    std::string command,
    bool exclude_from_context,
    qt::tools::ToolContext tool_context,
    std::shared_ptr<qt::foundation::ProcessCancellationToken>
        cancellation_token = nullptr) {
  tool_context.cancellation_token = std::move(cancellation_token);
  qt::tools::ToolRequest request;
  request.tool_name = "bash";
  request.arguments = qt::tools::Json{{"command", command}};
  const qt::tools::BashTool bash_tool;
  const auto executed = bash_tool.invoke(request, tool_context);
  if (executed.status == qt::tools::ToolResultStatus::invalid_request ||
      !executed.output.is_object()) {
    return qt::foundation::Result<ReferenceInteractiveBashExecution>::failure(
        qt::foundation::ErrorCode::invalid_argument,
        executed.error_message.value_or("failed to execute bash command"));
  }

  ReferenceInteractiveBashExecution result;
  result.command = std::move(command);
  result.output = executed.output.value("stdout", std::string{}) +
                  executed.output.value("stderr", std::string{});
  result.cancelled = executed.output.value("cancelled", false);
  const auto details = executed.output.find("details");
  result.truncated =
      details != executed.output.end() && details->is_object() &&
      details->contains("truncation");
  if (details != executed.output.end() && details->is_object() &&
      details->contains("fullOutputPath") &&
      details->at("fullOutputPath").is_string()) {
    result.full_output_path = details->at("fullOutputPath").get<std::string>();
  }
  result.exclude_from_context = exclude_from_context;
  if (!result.cancelled && executed.output.contains("exit_code") &&
      executed.output.at("exit_code").is_number_integer()) {
    result.exit_code = executed.output.at("exit_code").get<int>();
  }
  return qt::foundation::Result<ReferenceInteractiveBashExecution>::success(
      std::move(result));
}

qt::foundation::Result<std::string> record_interactive_bash(
    ReferenceStartupRuntime& runtime,
    ReferenceStartupSession& startup_session,
    const ReferenceInteractiveBashExecution& execution) {
  qt::harness::SessionBashExecutionEntry bash_entry;
  bash_entry.command = execution.command;
  bash_entry.output = execution.output;
  bash_entry.exit_code = execution.exit_code;
  bash_entry.cancelled = execution.cancelled;
  bash_entry.truncated = execution.truncated;
  bash_entry.full_output_path = execution.full_output_path;
  bash_entry.exclude_from_context = execution.exclude_from_context;

  qt::harness::SessionEntry session_entry;
  session_entry.data = std::move(bash_entry);
  const std::string entry_id =
      runtime.host.session_manager().append_entry(std::move(session_entry));
  const auto* appended = runtime.host.session_manager().get_entry(entry_id);
  if (appended == nullptr) {
    return qt::foundation::Result<std::string>::failure(
        qt::foundation::ErrorCode::internal_error,
        "failed to append interactive bash session entry");
  }

  if (!execution.exclude_from_context) {
    qt::ai::Message message;
    message.id = entry_id;
    message.role = qt::ai::MessageRole::user;
    message.content.push_back(qt::ai::TextContent{
        interactive_bash_context_text(
            execution.command, execution.output, execution.cancelled,
            execution.truncated, execution.full_output_path, execution.exit_code),
        std::nullopt});
    runtime.host.harness().agent().state().transcript.push_back(std::move(message));
  }

  if (startup_session.persistent && startup_session.repository_path) {
    const auto appended_status =
        qt::harness::SessionRepository(*startup_session.repository_path)
            .append(repository_ready_session_entry(*appended));
    if (!appended_status.ok()) {
      return qt::foundation::Result<std::string>::failure(
          appended_status.error().code, appended_status.error().message,
          appended_status.error().system_error);
    }
    ++startup_session.entry_count;
  }

  if (!execution.output.empty()) {
    return qt::foundation::Result<std::string>::success(
        trim_trailing_line_endings(execution.output));
  }
  return qt::foundation::Result<std::string>::success("(no output)");
}

}  // namespace

int run_interactive_mode(
    const ReferenceInteractiveModeRequest& request) {
  auto log = qt::foundation::logger("app", "interactive_mode");
  const auto& parsed = request.parsed;
  const auto& stdin_content = request.stdin_content;
  const std::string& cwd = request.cwd;
  const std::string& session_dir = request.session_dir;
  log.info("mode_begin",
           {qt::foundation::log_field("cwd", cwd),
            qt::foundation::log_field("session_dir", session_dir),
            qt::foundation::log_field("messages",
                                      static_cast<std::uint64_t>(
                                          parsed.messages.size())),
            qt::foundation::log_field("stdin", stdin_content.has_value())});

  const auto startup_settings =
      load_startup_settings(request.startup_session);
  if (!startup_settings) {
    std::cerr << "qt: " << startup_settings.error().message << '\n';
    return EXIT_FAILURE;
  }

  qt::app::PrepareInitialMessageRequest initial_request;
  initial_request.messages = parsed.messages;
  initial_request.file_args = parsed.file_args;
  initial_request.stdin_content = stdin_content;
  initial_request.cwd = cwd;
  initial_request.auto_resize_images =
      startup_settings.value().image_auto_resize;
  const auto initial = qt::app::prepare_initial_message(initial_request);
  StreamOutputPlan diagnostics_output;
  const bool has_initial_errors =
      append_initial_message_diagnostics(diagnostics_output, initial.diagnostics);
  emit_stream_output(diagnostics_output, std::cout, std::cerr);
  if (has_initial_errors) {
    return EXIT_FAILURE;
  }

  auto runtime_result =
      make_startup_runtime(parsed, request.startup_session);
  if (!runtime_result) {
    std::cerr << "qt: " << runtime_result.error().message << '\n';
    return EXIT_FAILURE;
  }
  auto runtime = std::make_shared<qt::app::ReferenceStartupRuntime>(
      std::move(runtime_result.value()));
  qt::app::bind_extension_host_call(runtime, parsed);
  qt::app::bind_subagent_runtime(runtime);
  qt::app::MemoryDashboardServer memory_dashboard(
      [runtime]() { return runtime->services.memory_runtime; });

  // Shared goal store for tracking session goals across the interactive session
  auto goal_store = std::make_shared<qt::harness::GoalStore>();

  log.info("runtime_ready",
           {qt::foundation::log_field(
                "provider",
                runtime->services.model_selection.provider.descriptor.id),
            qt::foundation::log_field(
                "model", runtime->services.model_selection.model.ref.name),
            qt::foundation::log_field(
                "tools",
                static_cast<std::uint64_t>(
                    runtime->services.tool_definitions.size()))});
  auto current_session =
      std::make_shared<ReferenceStartupSession>(request.startup_session);
  bool session_shutdown_hooks_fired = false;
  std::vector<std::string> startup_banner_lines;
  std::vector<std::string> startup_lines;
  if (!runtime->services.settings.quiet_startup) {
    startup_banner_lines = qt::app::make_qt_startup_banner_lines();
    startup_lines.push_back(
        "Type a prompt, use /help for commands, /quit to exit.");
    const auto startup_diagnostics =
        qt::app::collect_interactive_startup_diagnostics(
            runtime->services.settings, runtime->services.diagnostics);
    for (const auto& diagnostic : startup_diagnostics) {
      startup_lines.push_back("Warning: " + diagnostic);
    }
    if (const auto auth_warning =
            qt::app::make_anthropic_subscription_auth_warning(
                runtime->services.auth_storage,
                runtime->host.harness().model())) {
      const std::string warning = "Warning: " + *auth_warning;
      if (std::find(startup_lines.begin(), startup_lines.end(), warning) ==
          startup_lines.end()) {
        startup_lines.push_back(warning);
      }
    }
  }

  qt::app::InteractiveShellRuntime shell("qt> ");
  std::size_t submitted_message_count = 0;
  std::vector<qt::ai::ImageContent> pending_prompt_images;
  struct PendingClipboardImage {
    std::string path;
    qt::ai::ImageContent content;
  };
  std::vector<PendingClipboardImage> pending_clipboard_images;
  std::vector<std::string> pending_steer_messages;
  std::vector<std::string> pending_follow_up_messages;
  bool interactive_exit_requested = false;
  auto last_clear_request = std::chrono::steady_clock::time_point{};
  qt::app::InteractiveWorkingIndicator working_indicator;
  working_indicator.set_default_message("Working... (esc to interrupt)");
  LiveTurnTranscript live_turn_transcript;
  std::mutex pending_compaction_transcript_mutex;
  std::optional<std::vector<std::string>> pending_compaction_transcript;
  const auto take_pending_compaction_transcript =
      [&]() -> std::optional<std::vector<std::string>> {
    std::lock_guard<std::mutex> lock(pending_compaction_transcript_mutex);
    if (!pending_compaction_transcript.has_value()) {
      return std::nullopt;
    }
    auto result = std::move(pending_compaction_transcript);
    pending_compaction_transcript.reset();
    return result;
  };
  const auto live_turn_subscription = runtime->host.harness().subscribe(
      [&live_turn_transcript, &pending_compaction_transcript_mutex,
       &pending_compaction_transcript](
          const qt::harness::AgentHarness::SubscribedEvent& event) {
        if (const auto* agent_event =
                std::get_if<qt::agent::AgentEvent>(&event)) {
          live_turn_transcript.push(*agent_event);
          return;
        }
        const auto* compaction_end =
            std::get_if<qt::harness::AgentHarness::CompactionEndEvent>(&event);
        if (compaction_end == nullptr || compaction_end->aborted ||
            !compaction_end->result.has_value() ||
            compaction_end->reason == "manual") {
          return;
        }
        std::lock_guard<std::mutex> lock(pending_compaction_transcript_mutex);
        pending_compaction_transcript = compaction_transcript_lines(
            compaction_end->reason, *compaction_end->result);
      });

  // Theme hot-reload watcher.
  qt::app::ThemeWatcher theme_watcher;

  qt::app::InteractiveTurnPump turn_pump(
      [runtime, current_session](qt::app::InteractiveTurnPumpRequest request) {
        if (request.tool_request) {
          return run_interactive_tool_call(
              runtime->host.harness(), *current_session,
              std::move(request.prompt),
              std::move(*request.tool_request));
        }
        return run_interactive_prompt(
            runtime->host.harness(), *current_session, std::move(request.prompt),
            std::move(request.message_id), std::move(request.images),
            std::move(request.metadata));
      });
  std::mutex bash_pump_mutex;
  std::optional<std::future<qt::foundation::Result<ReferenceInteractiveBashExecution>>>
      pending_bash_execution;
  std::shared_ptr<qt::foundation::ProcessCancellationToken> pending_bash_token;
  const auto bash_pump_active = [&bash_pump_mutex, &pending_bash_execution]() {
    std::lock_guard<std::mutex> lock(bash_pump_mutex);
    return pending_bash_execution.has_value();
  };
  const auto start_async_bash =
      [&bash_pump_mutex, &pending_bash_execution, &pending_bash_token,
       runtime](
          std::string command,
          bool exclude_from_context) -> qt::foundation::Result<bool> {
    std::lock_guard<std::mutex> lock(bash_pump_mutex);
    if (pending_bash_execution.has_value()) {
      return qt::foundation::Result<bool>::success(false);
    }

    auto token =
        std::make_shared<qt::foundation::ProcessCancellationToken>();
    pending_bash_token = token;
    auto tool_context = runtime->services.tool_context;
    pending_bash_execution.emplace(std::async(
        std::launch::async,
        [command = std::move(command), exclude_from_context, token,
         tool_context = std::move(tool_context)]() mutable {
          return execute_interactive_bash(
              std::move(command), exclude_from_context, std::move(tool_context),
              token);
        }));
    return qt::foundation::Result<bool>::success(true);
  };
  const auto abort_async_bash =
      [&bash_pump_mutex, &pending_bash_token]() -> bool {
    std::shared_ptr<qt::foundation::ProcessCancellationToken> token;
    {
      std::lock_guard<std::mutex> lock(bash_pump_mutex);
      token = pending_bash_token;
    }
    if (!token) {
      return false;
    }
    token->cancel();
    return true;
  };
  const auto start_async_prompt =
      [&submitted_message_count, &turn_pump, &working_indicator,
       &live_turn_transcript](
          std::string prompt,
          std::vector<qt::ai::ImageContent> images = {},
          std::optional<std::string> working_message = std::nullopt,
          qt::ai::Json metadata = qt::ai::Json::object())
      -> qt::foundation::Result<bool> {
    ++submitted_message_count;
    std::ostringstream message_id;
    message_id << "interactive-message-" << submitted_message_count;
    live_turn_transcript.start();
    qt::app::InteractiveTurnPumpRequest request;
    request.prompt = std::move(prompt);
    request.message_id = message_id.str();
    request.images = std::move(images);
    request.metadata = std::move(metadata);
    const auto submitted = turn_pump.submit(std::move(request));
    if (!submitted || !submitted.value()) {
      live_turn_transcript.stop();
      return submitted;
    }
    working_indicator.set_message(std::move(working_message));
    working_indicator.start();
    return submitted;
  };
  const auto start_async_tool_call =
      [&submitted_message_count, &turn_pump, &working_indicator,
       &live_turn_transcript](std::string user_text,
                              qt::tools::ToolRequest request,
                              std::string working_message)
      -> qt::foundation::Result<bool> {
    ++submitted_message_count;
    std::ostringstream message_id;
    message_id << "interactive-tool-" << submitted_message_count;
    live_turn_transcript.start();
    qt::app::InteractiveTurnPumpRequest pump_request;
    pump_request.prompt = std::move(user_text);
    pump_request.message_id = message_id.str();
    pump_request.tool_request = std::move(request);
    const auto submitted = turn_pump.submit(std::move(pump_request));
    if (!submitted || !submitted.value()) {
      live_turn_transcript.stop();
      return submitted;
    }
    working_indicator.set_message(std::move(working_message));
    working_indicator.start();
    return submitted;
  };
  const auto take_clipboard_images_for_text =
      [&pending_clipboard_images](std::string_view text) {
        std::vector<qt::ai::ImageContent> images;
        std::vector<PendingClipboardImage> retained;
        for (auto& image : pending_clipboard_images) {
          if (!image.path.empty() &&
              text.find(std::string_view(image.path)) != std::string_view::npos) {
            images.push_back(std::move(image.content));
          } else {
            retained.push_back(std::move(image));
          }
        }
        pending_clipboard_images = std::move(retained);
        return images;
      };

  const auto build_registry = [&]() {
    qt::app::SlashCommandRegistryOptions registry_options;
    registry_options.enable_skill_commands =
        runtime->services.settings.enable_skill_commands;
    return qt::app::build_slash_command_registry(
        runtime->services.resource_bundle.prompt_templates,
        runtime->services.resource_bundle.skills,
        runtime->services.resource_bundle.extensions, registry_options);
  };

  const auto run_input_hooks =
      [runtime, &current_session, &turn_pump](std::string text,
                const std::vector<qt::ai::ImageContent>& images)
      -> qt::foundation::Result<qt::extensions::ExtensionInputEventRoutingState> {
    // Active-turn hooks may queue mutations; the worker owns their persistence.
    // Idle hooks can compact synchronously, before the next turn's baseline.
    const bool persist_idle_changes = !turn_pump.active();
    const auto baseline = persist_idle_changes
        ? runtime->host.harness().session_manager().get_entries().size() : 0U;
    auto result = qt::app::run_extension_input_hooks_subprocess(
        runtime->services.resource_bundle.extension_host, std::move(text),
        image_contents_json(images),
        qt::extensions::ExtensionInputSource::kInteractive);
    if (persist_idle_changes) {
      const auto persisted =
          persist_native_extension_entries(*runtime, *current_session, baseline);
      if (!persisted) return persisted.error();
    }
    return result;
  };

  const auto expand_prompt_text_after_input = [&build_registry](std::string text) {
    if (!text.empty() && text.front() == '/') {
      const auto execution = qt::app::execute_slash_command(build_registry(), text);
      if (execution &&
          (execution.value().kind ==
               qt::app::SlashCommandExecutionKind::kPromptExpansion ||
           execution.value().kind ==
               qt::app::SlashCommandExecutionKind::kSkillExpansion)) {
        text = execution.value().expanded_prompt;
      }
    }
    return text;
  };

  const auto start_prompt_after_input =
      [&expand_prompt_text_after_input, &start_async_prompt](
          std::string text, std::vector<qt::ai::ImageContent> images,
          std::optional<std::string> working_message = std::nullopt)
      -> qt::foundation::Result<bool> {
    text = expand_prompt_text_after_input(std::move(text));
    return start_async_prompt(std::move(text), std::move(images),
                              std::move(working_message));
  };

  struct ExtensionPrompt {
    std::string text;
    std::vector<qt::ai::ImageContent> images;
    qt::ai::Json metadata = qt::ai::Json::object();
  };
  std::vector<ExtensionPrompt> extension_prompts;
  const auto queue_extension_prompt = [&](const Json& arguments)
      -> qt::foundation::Result<Json> {
    try {
      ExtensionPrompt prompt;
      const auto& content = arguments.at("content");
      if (content.is_string()) prompt.text = content.get<std::string>();
      else {
        Json images = Json::array();
        for (const auto& block : content) {
          if (block.at("type") == "text") prompt.text += block.at("text").get<std::string>();
          else if (block.at("type") == "image") images.push_back(block);
          else return qt::foundation::Result<Json>::failure(
              qt::foundation::ErrorCode::invalid_argument, "unsupported user message content");
        }
        auto decoded = image_contents_from_json(images);
        if (!decoded) return decoded.error();
        prompt.images = std::move(decoded.value());
      }
      if (arguments.contains("customType")) {
        const auto custom_type = arguments.at("customType").get<std::string>();
        if (custom_type.empty()) {
          return qt::foundation::Result<Json>::failure(
              qt::foundation::ErrorCode::invalid_argument,
              "customType must not be empty");
        }
        prompt.metadata = Json{{"role", "custom"},
                               {"customType", custom_type},
                               {"display", arguments.value("display", true)}};
        if (arguments.contains("details") && !arguments.at("details").is_null()) {
          prompt.metadata["details"] = arguments.at("details");
        }
      }
      const auto delivery = arguments.value("deliverAs", std::string("followUp"));
      if (delivery != "followUp" && delivery != "steer") {
        return qt::foundation::Result<Json>::failure(
            qt::foundation::ErrorCode::invalid_argument, "deliverAs must be steer or followUp");
      }
      if (turn_pump.active() && prompt.images.empty()) {
        const auto kind = delivery == "steer"
            ? qt::app::InteractiveTurnPumpOperationKind::kSteer
            : qt::app::InteractiveTurnPumpOperationKind::kFollowUp;
        if (!turn_pump.queue_operation({kind, prompt.text})) {
          extension_prompts.push_back(std::move(prompt));
        }
      } else {
        extension_prompts.push_back(std::move(prompt));
      }
      return Json(nullptr);
    } catch (const Json::exception&) {
      return qt::foundation::Result<Json>::failure(
          qt::foundation::ErrorCode::invalid_argument, "invalid sendUserMessage arguments");
    }
  };
  const auto queue_extension_custom_message =
      [&runtime, &parsed, &turn_pump, &queue_extension_prompt](
          const Json& arguments) -> qt::foundation::Result<Json> {
    if (turn_pump.active()) {
      return qt::app::dispatch_extension_host_call(
          *runtime, parsed, "pi.sendMessage", arguments);
    }
    if (arguments.value("triggerTurn", false)) {
      // The interactive turn pump creates and streams the user entry itself;
      // appending the custom entry first would produce a duplicate prompt.
      return queue_extension_prompt(arguments);
    }
    Json append_args = arguments;
    append_args["triggerTurn"] = false;
    const auto appended = qt::app::dispatch_extension_host_call(
        *runtime, parsed, "pi.sendMessage", append_args);
    if (!appended) return appended;
    return Json(nullptr);
  };

  shell.set_prompt_handler([&pending_prompt_images, &pending_steer_messages,
                            &turn_pump, &run_input_hooks,
                            &expand_prompt_text_after_input,
                            &start_prompt_after_input,
                            &take_clipboard_images_for_text](
                               std::string_view text) {
    if (turn_pump.active()) {
      auto input_result = run_input_hooks(std::string(text), {});
      if (!input_result) {
        return qt::app::InteractiveDispatchResult{false,
                                                  input_result.error().message};
      }
      if (input_result.value().handled) {
        return qt::app::InteractiveDispatchResult{true, ""};
      }
      const std::string steer_text = expand_prompt_text_after_input(
          input_result.value().current_event.text);
      pending_steer_messages.emplace_back(steer_text);
      (void)turn_pump.queue_operation(qt::app::InteractiveTurnPumpOperation{
          qt::app::InteractiveTurnPumpOperationKind::kSteer,
          steer_text});
      std::ostringstream output;
      output << "[steered #" << pending_steer_messages.size() << "] "
             << steer_text;
      return qt::app::InteractiveDispatchResult{true, output.str()};
    }

    auto images = std::move(pending_prompt_images);
    auto pasted_images = take_clipboard_images_for_text(text);
    images.insert(images.end(), std::make_move_iterator(pasted_images.begin()),
                  std::make_move_iterator(pasted_images.end()));
    auto input_result = run_input_hooks(std::string(text), images);
    if (!input_result) {
      pending_prompt_images.clear();
      return qt::app::InteractiveDispatchResult{false,
                                                input_result.error().message};
    }
    if (input_result.value().handled) {
      pending_prompt_images.clear();
      return qt::app::InteractiveDispatchResult{true, ""};
    }
    auto input_images =
        image_contents_from_json(input_result.value().current_event.images);
    if (!input_images) {
      pending_prompt_images.clear();
      return qt::app::InteractiveDispatchResult{false,
                                                input_images.error().message};
    }
    const auto submitted = start_prompt_after_input(
        input_result.value().current_event.text, std::move(input_images.value()));
    pending_prompt_images.clear();
    if (!submitted) {
      return qt::app::InteractiveDispatchResult{false, submitted.error().message};
    }
    if (!submitted.value()) {
      return qt::app::InteractiveDispatchResult{
          false, "interactive turn is already running"};
    }
    return qt::app::InteractiveDispatchResult{true, ""};
  });
  shell.set_shell_command_handler(
      [&start_async_bash, &bash_pump_active, &working_indicator](
          std::string_view command, bool exclude_from_context) {
        if (bash_pump_active()) {
          return qt::app::InteractiveDispatchResult{
              false, "A bash command is already running. Press Esc to cancel it first."};
        }
        const auto started =
            start_async_bash(std::string(command), exclude_from_context);
        if (!started) {
          return qt::app::InteractiveDispatchResult{false,
                                                    started.error().message};
        }
        if (!started.value()) {
          return qt::app::InteractiveDispatchResult{
              false, "A bash command is already running. Press Esc to cancel it first."};
        }
        working_indicator.set_message("Running bash... (esc to cancel)");
        working_indicator.start();
        return qt::app::InteractiveDispatchResult{true, ""};
      });

  shell.register_slash_command(
      "follow-up",
      [&turn_pump, &pending_follow_up_messages](std::string_view arguments) {
        std::string text(arguments);
        while (!text.empty() &&
               std::isspace(static_cast<unsigned char>(text.front())) != 0) {
          text.erase(text.begin());
        }
        while (!text.empty() &&
               std::isspace(static_cast<unsigned char>(text.back())) != 0) {
          text.pop_back();
        }
        if (text.empty()) {
          return qt::app::InteractiveDispatchResult{
              false, "follow-up requires message text"};
        }
        if (!turn_pump.active()) {
          return qt::app::InteractiveDispatchResult{
              false, "follow-up requires a running turn"};
        }
        pending_follow_up_messages.push_back(text);
        (void)turn_pump.queue_operation(qt::app::InteractiveTurnPumpOperation{
            qt::app::InteractiveTurnPumpOperationKind::kFollowUp,
            text});
        std::ostringstream output;
        output << "[follow-up #" << pending_follow_up_messages.size() << "] "
               << text;
        return qt::app::InteractiveDispatchResult{true, output.str()};
      });

  qt::tui::OstreamTerminalSink terminal_sink(std::cout);
  qt::tui::Terminal terminal(terminal_sink);
  qt::tui::PosixTerminalLifecycleAdapter terminal_adapter;
  qt::tui::TerminalSessionLifecycle terminal_lifecycle(terminal, terminal_adapter);
  qt::tui::NotcursesSessionAdapter notcurses_session(terminal);
  const auto restore_cursor = [&terminal]() {
    const auto status = terminal.set_cursor_visible(true);
    (void)status;
  };

  qt::tui::NotcursesSessionOptions notcurses_options;
  const auto notcurses_override = env_flag_override("QT_EXPERIMENTAL_NOTCURSES");
  const bool terminal_supports_direct_mode =
#if defined(_WIN32)
      false;
#else
      ::isatty(STDIN_FILENO) != 0 && ::isatty(STDOUT_FILENO) != 0;
#endif
  notcurses_options.prefer_notcurses =
      notcurses_override.value_or(terminal_supports_direct_mode);
  notcurses_options.backend.dry_run = false;
  notcurses_options.backend.mode = qt::tui::NotcursesBackendMode::cli;
  notcurses_options.backend.use_alternate_screen = false;
  notcurses_options.backend.output = stdout;
  const auto lifecycle_status = terminal_lifecycle.start();
  if (!lifecycle_status) {
    std::cerr << "qt: " << lifecycle_status.error().message << '\n';
    return EXIT_FAILURE;
  }
  const auto notcurses_status = notcurses_session.start(notcurses_options);
  if (!notcurses_status) {
    std::cerr << "qt: " << notcurses_status.error().message << '\n';
    const auto lifecycle_stop = terminal_lifecycle.stop();
    (void)lifecycle_stop;
    return EXIT_FAILURE;
  }
  qt::tui::RenderConstraints constraints{80, 24};
  if (terminal.size().columns > 0) {
    constraints.max_width = terminal.size().columns;
  }
  if (terminal.size().rows > 0) {
    constraints.max_height = terminal.size().rows;
  }
  auto last_terminal_progress_keepalive =
      std::chrono::steady_clock::time_point{};
  TerminalInputPump terminal_input_pump;
  qt::app::InteractiveExtensionUiState extension_ui_state;
  extension_ui_state.apply_working_message =
      [&working_indicator](std::optional<std::string> message) {
        working_indicator.set_message(std::move(message));
      };
  extension_ui_state.apply_working_visible =
      [&working_indicator](bool visible) { working_indicator.set_visible(visible); };
  extension_ui_state.apply_hidden_thinking_label =
      [&live_turn_transcript](std::optional<std::string> label) {
        live_turn_transcript.set_hidden_thinking_label(std::move(label));
      };
  extension_ui_state.apply_working_indicator =
      [&working_indicator](std::optional<std::vector<std::string>> frames,
                           std::size_t interval_ms) {
        qt::tui::LoaderIndicator indicator;
        if (frames.has_value()) {
          indicator.frames = std::move(*frames);
          indicator.interval_ms = interval_ms;
        }
        working_indicator.set_indicator(std::move(indicator));
      };
  bool model_terminal_title_active = false;
  const auto base_terminal_title = [&]() {
    std::string title = "qt";
    const auto session_name =
        runtime->host.session_manager().get_session_name();
    if (session_name.has_value() && !session_name->empty()) {
      title += " - " + *session_name;
    }
    const auto cwd = cwd_terminal_title_suffix();
    if (!cwd.empty()) {
      title += " - " + cwd;
    }
    return sanitize_terminal_title(std::move(title));
  };
  const auto set_terminal_title_if_needed =
      [&](std::string title) -> qt::foundation::Status {
    title = sanitize_terminal_title(std::move(title));
    if (terminal.state().title == title) {
      return qt::foundation::Status::success();
    }
    return terminal.set_title(std::move(title));
  };
  const auto restore_model_terminal_title = [&]() -> qt::foundation::Status {
    if (!model_terminal_title_active) {
      return qt::foundation::Status::success();
    }
    model_terminal_title_active = false;
    return set_terminal_title_if_needed(base_terminal_title());
  };
  const auto refresh_model_terminal_title = [&]() -> qt::foundation::Status {
    if (!turn_pump.active() || !working_indicator.active()) {
      return restore_model_terminal_title();
    }
    const auto status = working_indicator.status_line();
    if (!status.has_value()) {
      return restore_model_terminal_title();
    }
    model_terminal_title_active = true;
    return set_terminal_title_if_needed(*status + " - " +
                                        base_terminal_title());
  };
  const auto run_current_session_shutdown_hooks_once =
      [&runtime, &current_session, &session_shutdown_hooks_fired](
          qt::extensions::ExtensionSessionShutdownReason reason,
          std::optional<std::string> target_session_file =
              std::nullopt) -> qt::foundation::Status {
    if (session_shutdown_hooks_fired) {
      return qt::foundation::Status::success();
    }
    const auto baseline = runtime->host.harness().session_manager().get_entries().size();
    session_shutdown_hooks_fired = true;
    auto hooks = qt::app::run_extension_session_shutdown_hooks_subprocess(
        runtime->services.resource_bundle.extension_host, reason,
        std::move(target_session_file));
    const auto persisted = persist_native_extension_entries(*runtime, *current_session, baseline);
    if (!persisted) return persisted;
    if (!hooks) {
      return qt::foundation::Status(hooks.error());
    }
    return qt::foundation::Status::success();
  };
  const auto stop_terminal_session = [&]() {
    terminal_input_pump.stop();
    if (runtime->services.subagent_coordinator) runtime->services.subagent_coordinator->shutdown();
    qt::foundation::Status first_error =
        run_current_session_shutdown_hooks_once(
            qt::extensions::ExtensionSessionShutdownReason::kQuit);
    if (terminal.state().progress_active) {
      const auto progress = terminal.set_progress(false);
      if (first_error && !progress) {
        first_error = progress;
      }
    }
    const auto title = restore_model_terminal_title();
    if (first_error && !title) {
      first_error = title;
    }
    const auto notcurses_stop = notcurses_session.stop();
    if (first_error && !notcurses_stop) {
      first_error = notcurses_stop;
    }
    const auto lifecycle_stop = terminal_lifecycle.stop();
    if (!first_error) {
      return first_error;
    }
    return lifecycle_stop;
  };
  const auto start_terminal_session = [&]() -> qt::foundation::Status {
    auto lifecycle = terminal_lifecycle.start();
    if (!lifecycle) {
      return lifecycle;
    }
    auto notcurses = notcurses_session.start(notcurses_options);
    if (!notcurses) {
      const auto lifecycle_stop = terminal_lifecycle.stop();
      (void)lifecycle_stop;
      return notcurses;
    }
    return qt::foundation::Status::success();
  };
  const auto refresh_terminal_progress = [&]() -> qt::foundation::Status {
    const bool should_show =
        runtime->services.settings.terminal_show_progress && turn_pump.active();
    if (!should_show) {
      last_terminal_progress_keepalive =
          std::chrono::steady_clock::time_point{};
      if (terminal.state().progress_active) {
        return terminal.set_progress(false);
      }
      return qt::foundation::Status::success();
    }

    const auto now = std::chrono::steady_clock::now();
    if (!terminal.state().progress_active ||
        last_terminal_progress_keepalive ==
            std::chrono::steady_clock::time_point{} ||
        now - last_terminal_progress_keepalive >= std::chrono::seconds(1)) {
      const auto status = terminal.set_progress(true);
      if (!status) {
        return status;
      }
      last_terminal_progress_keepalive = now;
    }
    return qt::foundation::Status::success();
  };
  const auto refresh_keyboard_protocol = [&]() -> qt::foundation::Status {
    return terminal_lifecycle.poll_keyboard_protocol();
  };

  const auto render_message_text = [](const qt::ai::Message& message) {
    std::vector<std::string> lines;
    for (const auto& content : message.content) {
      append_content_transcript_lines(lines, content);
    }

    std::string text;
    for (const auto& line : lines) {
      if (!text.empty()) {
        text += '\n';
      }
      text += line;
    }
    return text;
  };

  struct InteractivePackageResourceItem {
    std::string id;
    std::string source;
    qt::tools::PackageSourceScope scope = qt::tools::PackageSourceScope::user;
    qt::tools::PackageResourceKind resource_kind =
        qt::tools::PackageResourceKind::extensions;
    std::string package_root;
    std::string resource_path;
    std::string label;
    std::string description;
    bool enabled = true;
    bool can_toggle = true;
  };

  const auto build_autocomplete_provider = [&]() {
    const auto registry = build_registry();
    std::vector<qt::tui::SlashCommandCompletion> commands;
    commands.reserve(registry.commands.size());
    for (const auto& command : registry.commands) {
      qt::tui::SlashCommandCompletion completion;
      completion.name = command.name;
      if (!command.description.empty()) {
        completion.description = command.description;
      }
      completion.argument_hint = command.argument_hint;
      if (command.name == "model") {
        completion.argument_completions = [runtime](const std::string& prefix) {
          std::vector<qt::tui::AutocompleteItem> items;
          for (const auto& model : runtime->services.model_catalog.models) {
            if (!model_is_available(runtime->services.model_catalog,
                                    runtime->services.auth_storage, model)) {
              continue;
            }
            if (!model_matches_search(model, prefix)) {
              continue;
            }
            items.push_back(qt::tui::AutocompleteItem{
                model.ref.provider + "/" + model.ref.name, model.ref.name,
                model_display_with_pricing(model)});
          }
          return items;
        };
      } else if (command.name == "thinking") {
        completion.argument_completions = [](const std::string& prefix) {
          static const std::vector<std::string> levels = {
              "off", "minimal", "low", "medium", "high", "xhigh"};
          std::vector<qt::tui::AutocompleteItem> items;
          for (const auto& level : levels) {
            if (!prefix.empty() && level.find(prefix) == std::string::npos) {
              continue;
            }
            items.push_back(
                qt::tui::AutocompleteItem{level, level, std::nullopt});
          }
          return items;
        };
      } else if (command.name == "llama") {
        completion.argument_completions = [](const std::string& prefix) {
          std::vector<qt::tui::AutocompleteItem> items;
          const auto first = split_first_ascii_argument(prefix);
          if (first.first == "run") {
            for (const char* mode : {"embed", "rank", "instruct", "chat"}) {
              const std::string value = std::string("run ") + mode;
              if (!first.second.empty() &&
                  std::string(mode).find(first.second) == std::string::npos) {
                continue;
              }
              items.push_back(qt::tui::AutocompleteItem{
                  value, value,
                  std::optional<std::string>{"llama run mode"}});
            }
            return items;
          }
          for (const auto& subcommand : qt::app::llama_slash_subcommand_infos()) {
            const std::string value = subcommand.name;
            if (!prefix.empty() && value.find(prefix) == std::string::npos) {
              continue;
            }
            items.push_back(qt::tui::AutocompleteItem{
                value,
                subcommand.argument_hint[0] == '\0'
                    ? value
                    : value + " " + subcommand.argument_hint,
                std::optional<std::string>{subcommand.description}});
          }
          return items;
        };
      } else if (command.name == "ollama") {
        completion.argument_completions = [runtime](const std::string& prefix) {
          std::vector<qt::tui::AutocompleteItem> items;
          const auto first = split_first_ascii_argument(prefix);
          static const std::vector<std::string> commands = {
              "instances", "add", "configure", "remove", "status", "models",
              "running", "info", "pull", "load", "unload", "delete"};
          if (first.second.empty()) {
            for (const auto& value : commands) {
              if (first.first.empty() || value.find(first.first) != std::string::npos) {
                items.push_back({value, value, std::optional<std::string>{"Ollama operation"}});
              }
            }
          } else {
            for (const auto& instance : runtime->services.settings.ollama_instances) {
              if (instance.id.find(first.second) != std::string::npos) {
                const std::string value = first.first + " " + instance.id;
                items.push_back({value, value, instance.base_url});
              }
            }
          }
          return items;
        };
      } else if (command.name == "memory") {
        completion.argument_completions = [](const std::string& prefix) {
          static const std::vector<std::string> commands = {
              "dashboard", "status", "settings", "scan-skills", "dream",
              "recall", "remember", "review", "feedback", "forget"};
          std::vector<qt::tui::AutocompleteItem> items;
          for (const auto& value : commands) {
            if (prefix.empty() || value.find(prefix) != std::string::npos) {
              items.push_back({value, value,
                               std::optional<std::string>{
                                   value == "scan-skills"
                                       ? "Scan a folder for skill memories"
                                       : "Memory operation"}});
            }
          }
          return items;
        };
      } else if (command.name == "subagent") {
        completion.argument_completions = [](const std::string& prefix) {
          struct SubagentCompletion {
            const char* value;
            const char* label;
            const char* description;
          };
          static constexpr SubagentCompletion completions[] = {
              {"list", "list [user|project|both]",
               "List discovered subagent definitions"},
              {"help", "help", "Show role locations and execution guidance"},
          };
          std::vector<qt::tui::AutocompleteItem> items;
          for (const auto& item : completions) {
            const std::string value = item.value;
            if (!prefix.empty() && value.find(prefix) == std::string::npos) {
              continue;
            }
            items.push_back(qt::tui::AutocompleteItem{
                value, item.label, std::optional<std::string>{item.description}});
          }
          return items;
        };
      } else if (command.name == "mcp") {
        completion.argument_completions = [](const std::string& prefix) {
          struct McpCompletion {
            const char* value;
            const char* label;
            const char* description;
          };
          static constexpr McpCompletion completions[] = {
              {"status", "status", "Show configured MCP server status"},
              {"tools", "tools", "List registered MCP tools"},
              {"resources", "resources", "List MCP resources"},
              {"reconnect", "reconnect [server]", "Reconnect all servers or one server"},
              {"setup", "setup", "Show MCP config paths and starter config"},
          };
          std::vector<qt::tui::AutocompleteItem> items;
          for (const auto& item : completions) {
            const std::string value = item.value;
            if (!prefix.empty() && value.find(prefix) == std::string::npos) {
              continue;
            }
            items.push_back(qt::tui::AutocompleteItem{
                value, item.label, std::optional<std::string>{item.description}});
          }
          return items;
        };
      }
      commands.push_back(std::move(completion));
    }

    const std::string tools_dir =
        runtime->services.settings.agent_home.empty()
            ? std::string{}
            : (std::filesystem::path(runtime->services.settings.agent_home) /
               "bin")
                  .string();
    qt::tools::ToolsManager tools_manager(tools_dir);
    return std::make_shared<qt::tui::CombinedAutocompleteProvider>(
        std::move(commands), runtime->services.settings.cwd, 50U,
        tools_manager.find_fd());
  };

  const auto help_provider = [&]() {
    const auto registry = build_registry();
    std::vector<std::string> lines;
    lines.push_back("Slash commands:");
    for (const auto& command : registry.commands) {
      std::string line = "/" + command.name;
      if (command.argument_hint && !command.argument_hint->empty()) {
        line += " " + *command.argument_hint;
      }
      if (!command.description.empty()) {
        line += " - " + command.description;
      }
      lines.push_back(std::move(line));
    }
    if (!registry.diagnostics.empty()) {
      lines.push_back("");
      lines.push_back("Diagnostics:");
      for (const auto& diagnostic : registry.diagnostics) {
        lines.push_back("- " + diagnostic);
      }
    }
    return lines;
  };

  const auto persist_user_settings =
      [&](const std::function<void(Json&)>& mutate) -> qt::foundation::Status {
    if (runtime->services.settings.agent_home.empty()) {
      return qt::foundation::Status::failure(
          qt::foundation::ErrorCode::invalid_argument,
          "agent_home is required to persist settings");
    }

    const auto path =
        std::filesystem::path(runtime->services.settings.agent_home) /
        "settings.json";
    Json document = Json::object();
    std::error_code exists_error;
    if (std::filesystem::exists(path, exists_error) && !exists_error) {
      const auto content = qt::foundation::read_text_file(path);
      if (!content) {
        return qt::foundation::Status(content.error());
      }
      try {
        document = Json::parse(content.value());
      } catch (const std::exception& error) {
        return qt::foundation::Status::failure(
            qt::foundation::ErrorCode::invalid_argument,
            std::string("invalid settings JSON: ") + error.what());
      }
      if (!document.is_object()) {
        document = Json::object();
      }
    }

    mutate(document);
    const auto ensured = qt::foundation::ensure_directory(path.parent_path());
    if (!ensured) {
      return ensured;
    }
    return qt::foundation::write_text_file(path,
                                           pretty_transcript_json(document) + "\n");
  };

  const auto persist_boolean_setting =
      [&](std::string_view id, bool enabled) -> qt::foundation::Status {
    return persist_user_settings([&](Json& document) {
      if (id == "quiet_startup") {
        document["quietStartup"] = enabled;
      } else if (id == "collapse_changelog") {
        document["collapseChangelog"] = enabled;
      } else if (id == "hide_thinking_block") {
        document["hideThinkingBlock"] = enabled;
      } else if (id == "enable_skill_commands") {
        document["enableSkillCommands"] = enabled;
      } else if (id == "allow_external_filesystem_access") {
        document["allowExternalFilesystemAccess"] = enabled;
      } else if (id == "image_auto_resize") {
        if (!document.contains("images") || !document.at("images").is_object()) {
          document["images"] = Json::object();
        }
        document["images"]["autoResize"] = enabled;
      } else if (id == "terminal_show_progress") {
        if (!document.contains("terminal") ||
            !document.at("terminal").is_object()) {
          document["terminal"] = Json::object();
        }
        document["terminal"]["showTerminalProgress"] = enabled;
      }
    });
  };

  const auto persist_enabled_models =
      [&](const std::vector<std::string>& enabled_models)
      -> qt::foundation::Status {
    return persist_user_settings([&](Json& document) {
      document["enabledModels"] = enabled_models;
    });
  };

  const auto persist_tool_entry_compaction_enabled_tools =
      [&](const std::vector<std::string>& enabled_tools)
      -> qt::foundation::Status {
    return persist_user_settings([&](Json& document) {
      if (!document.contains("terminal") ||
          !document.at("terminal").is_object()) {
        document["terminal"] = Json::object();
      }
      document["terminal"]["toolEntryCompactionEnabledTools"] = enabled_tools;
    });
  };

  const auto persist_prompt_history_limit =
      [&](std::size_t max_entries) -> qt::foundation::Status {
    return persist_user_settings([&](Json& document) {
      if (!document.contains("terminal") ||
          !document.at("terminal").is_object()) {
        document["terminal"] = Json::object();
      }
      document["terminal"]["promptHistoryLimit"] = max_entries;
    });
  };

  const auto persist_memory_settings =
      [&](const MemorySettings& settings) -> qt::foundation::Status {
    return persist_user_settings([&](Json& document) {
      if (!document.contains("memory") ||
          !document.at("memory").is_object()) {
        document["memory"] = Json::object();
      }
      auto& memory = document["memory"];
      memory["enabled"] = settings.enabled;
      memory["automaticRecall"] = settings.automatic_recall;
      memory["automaticCapture"] = settings.automatic_capture;
      memory["recallPolicy"] = settings.recall_policy;
      memory["contextTokenBudget"] = settings.context_token_budget;
      memory["candidateLimit"] = settings.candidate_limit;

      if (!memory.contains("prehandler") ||
          !memory.at("prehandler").is_object()) {
        memory["prehandler"] = Json::object();
      }
      auto& prehandler = memory["prehandler"];
      prehandler["model"] = settings.prehandler.model
                                  ? Json(*settings.prehandler.model)
                                  : Json(nullptr);
      prehandler["thinking"] = settings.prehandler.thinking;
      prehandler["timeoutMs"] = settings.prehandler.timeout_ms;
      prehandler["failOpen"] = settings.prehandler.fail_open;
      prehandler["maxModelInputTokens"] =
          settings.prehandler.max_model_input_tokens;

      if (!memory.contains("capture") ||
          !memory.at("capture").is_object()) {
        memory["capture"] = Json::object();
      }
      auto& capture = memory["capture"];
      capture["postProcess"] = settings.capture.post_process;
      capture["defaultStatus"] = settings.capture.default_status;
      capture["maxEvidenceChars"] = settings.capture.max_evidence_chars;

      if (!memory.contains("dreaming") ||
          !memory.at("dreaming").is_object()) {
        memory["dreaming"] = Json::object();
      }
      auto& dreaming = memory["dreaming"];
      dreaming["enabled"] = settings.dreaming.enabled;
      dreaming["trigger"] = settings.dreaming.trigger;
      dreaming["intervalSeconds"] = settings.dreaming.interval_seconds;
      dreaming["idleSeconds"] = settings.dreaming.idle_seconds;
      dreaming["candidateThreshold"] =
          settings.dreaming.candidate_threshold;
      dreaming["model"] = settings.dreaming.model
                                ? Json(*settings.dreaming.model)
                                : Json(nullptr);
      dreaming["verifierModel"] = settings.dreaming.verifier_model
                                        ? Json(*settings.dreaming.verifier_model)
                                        : Json(nullptr);
      dreaming["timeoutMs"] = settings.dreaming.timeout_ms;
      dreaming["maxModelInputTokens"] =
          settings.dreaming.max_model_input_tokens;
      dreaming["maxCandidates"] = settings.dreaming.max_candidates;
      dreaming["researchPolicy"] = settings.dreaming.research_policy;
      dreaming["promotionPolicy"] = settings.dreaming.promotion_policy;
    });
  };

  const auto persist_ollama_instances =
      [&](const std::vector<qt::ai::OllamaInstanceSettings>& instances)
      -> qt::foundation::Status {
    return persist_user_settings([&](Json& document) {
      Json values = Json::array();
      for (const auto& instance : instances) {
        Json value{{"id", instance.id},
                   {"name", instance.name},
                   {"baseUrl", instance.base_url},
                   {"enabled", instance.enabled},
                   {"timeoutMs", instance.timeout_ms}};
        if (instance.api_key) {
          value["apiKey"] = *instance.api_key;
        }
        value["models"] = Json::array();
        for (const auto& model : instance.models) {
          value["models"].push_back(
              Json{{"name", model.name},
                   {"modifiedAt", model.modified_at},
                   {"digest", model.digest},
                   {"size", model.size},
                   {"capabilities", model.capabilities},
                   {"contextWindow", model.context_window},
                   {"maxTokens", model.max_tokens},
                   {"details", model.details}});
        }
        values.push_back(std::move(value));
      }
      document["ollama"] = Json{{"instances", std::move(values)}};
    });
  };

  const auto persist_llama_model_selection =
      [&](const std::string& model_id) -> qt::foundation::Status {
    return persist_user_settings([&](Json& document) {
      if (!document.contains("llama") || !document.at("llama").is_object()) {
        document["llama"] = Json::object();
      }
      document["llama"]["selectedModel"] = model_id;
    });
  };

  const auto persist_model_selection =
      [&](const qt::ai::ModelRef& model) -> qt::foundation::Status {
    return persist_user_settings([&](Json& document) {
      document["defaultProvider"] = model.provider;
      document["defaultModel"] = model.name;
      if (model.provider == qt::ai::kLlamaProviderId) {
        if (!document.contains("llama") ||
            !document.at("llama").is_object()) {
          document["llama"] = Json::object();
        }
        document["llama"]["selectedModel"] = model.name;
      }
    });
  };

  const auto hotkey_lines = []() {
    return std::vector<std::string>{
        "Keyboard shortcuts:",
        "Enter - submit",
        "Shift+Enter - insert newline",
        "Tab - autocomplete",
        "Up/Down - history recall when the editor is idle",
        "Esc - close selectors and autocomplete",
        "Ctrl+C - interrupt or clear",
        "Ctrl+D - exit on empty input",
        "Use /help to view the full slash-command list.",
    };
  };

  const auto changelog_lines = []() {
    const auto path =
        std::filesystem::path("reference/pi/packages/coding-agent/CHANGELOG.md");
    const auto content = qt::foundation::read_text_file(path);
    if (!content) {
      return std::vector<std::string>{"No changelog entries are available."};
    }

    std::vector<std::string> lines;
    std::istringstream input(content.value());
    std::string line;
    bool in_latest_section = false;
    while (std::getline(input, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (line.rfind("## ", 0) == 0) {
        if (in_latest_section) {
          break;
        }
        in_latest_section = true;
      }
      if (in_latest_section) {
        lines.push_back(line);
      }
    }

    if (lines.empty()) {
      lines.push_back("No changelog entries are available.");
    }
    return lines;
  };

  const auto show_text_prompt =
      [&](qt::app::InteractiveTerminalSession& ui, std::string title,
          std::string label, std::string hint,
          const std::function<void(const std::string&)>& on_submit) {
        auto dialog = std::make_shared<qt::tui::LoginDialogComponent>(
            "api-key", label, [&ui](bool, std::string) { ui.hide_overlay(); },
            std::move(title));
        dialog->show_prompt(
            std::move(label), std::move(hint),
            [&ui, on_submit](const std::string& value) {
          ui.hide_overlay();
          on_submit(value);
        });
        ui.show_overlay(dialog);
      };

  const auto persist_leaf_if_needed =
      [&](const std::optional<std::string>& previous_leaf)
      -> qt::foundation::Status {
    if (!current_session->persistent || !current_session->repository_path) {
      return qt::foundation::Status::success();
    }

    const auto current_leaf = runtime->host.session_manager().get_leaf_id();
    if (!current_leaf || current_leaf == previous_leaf) {
      return qt::foundation::Status::success();
    }

    const auto* entry = runtime->host.session_manager().get_entry(*current_leaf);
    if (entry == nullptr) {
      return qt::foundation::Status::failure(
          qt::foundation::ErrorCode::not_found,
          "session manager leaf entry was not found");
    }

    return qt::harness::SessionRepository(*current_session->repository_path)
        .append(*entry);
  };
  const auto persist_extension_entries =
      [&](std::size_t previous_entry_count) -> qt::foundation::Status {
    return persist_native_extension_entries(*runtime, *current_session, previous_entry_count);
  };
  const auto persist_prompt_history =
      [&](const std::vector<std::string>& history) -> qt::foundation::Status {
    qt::app::PromptHistoryStore global_history(
        qt::foundation::Path(runtime->services.settings.agent_home),
        runtime->services.settings.terminal_prompt_history_limit);
    qt::foundation::Status global_status = qt::foundation::Status::success();
    if (runtime->services.settings.terminal_prompt_history_limit == 0U) {
      global_status = global_history.save({});
    } else if (!history.empty()) {
      global_status = global_history.append(history.front());
    }

    runtime->host.session_state().metadata().prompt_history = history;
    current_session->prompt_history = history;

    if (!current_session->persistent || !current_session->repository_path) {
      return global_status;
    }

    qt::harness::SessionEntry entry;
    entry.data = qt::harness::SessionMetadataEntry{
        runtime->host.session_state().metadata()};
    const auto status =
        qt::harness::SessionRepository(*current_session->repository_path)
            .append(entry);
    if (status) {
      ++current_session->entry_count;
    }
    if (!global_status) {
      return global_status;
    }
    return status;
  };

  const auto restore_prompt_history =
      [&](qt::app::InteractiveTerminalSession& ui,
          const std::vector<std::string>& legacy_session_history)
      -> std::optional<std::string> {
    const std::size_t max_entries =
        runtime->services.settings.terminal_prompt_history_limit;
    auto& editor = ui.mutable_editor();
    editor.set_history_limit(max_entries);

    qt::app::PromptHistoryStore store(
        qt::foundation::Path(runtime->services.settings.agent_home),
        max_entries);
    const auto loaded = store.load();
    if (loaded) {
      if (loaded.value().file_exists || max_entries == 0U) {
        editor.set_history(loaded.value().prompts);
        return std::nullopt;
      }

      editor.set_history(legacy_session_history);
      if (!editor.history().empty()) {
        const auto migrated = store.save(editor.history());
        if (!migrated) {
          return "unable to migrate legacy prompt history to " +
                 qt::foundation::path_for_display(store.path()) + ": " +
                 migrated.error().message;
        }
      }
      return std::nullopt;
    }

    editor.set_history(legacy_session_history);
    const auto recovered = store.save(editor.history());
    if (!recovered) {
      return "unable to load or reset global prompt history: " +
             loaded.error().message + "; " + recovered.error().message;
    }
    return "reset invalid global prompt history: " + loaded.error().message;
  };

  const auto runtime_repository = [&]() -> qt::harness::FilesystemRepository {
    return qt::harness::FilesystemRepository(runtime->services.settings.cwd);
  };

  const auto package_settings_cwd = [&]() {
    return qt::foundation::Path(
        runtime->services.settings.workspace_root.empty()
            ? runtime->services.settings.cwd
            : runtime->services.settings.workspace_root);
  };

  const auto load_runtime_package_settings =
      [&]() -> qt::foundation::Result<qt::tools::PackageManagerCommandSettings> {
    return qt::tools::load_package_command_settings(
        package_settings_cwd(),
        qt::foundation::Path(runtime->services.settings.agent_home));
  };

  const auto persist_active_tools_config =
      [&](const std::vector<std::string>& active_tool_names)
      -> qt::foundation::Status {
    runtime->host.harness().set_active_tool_names(active_tool_names);
    const auto before_leaf = runtime->host.session_manager().get_leaf_id();
    runtime->host.session_manager().append_custom_entry(
        std::string("tools-config"), Json{{"enabledTools", active_tool_names}});
    return persist_leaf_if_needed(before_leaf);
  };

  const auto load_package_resource_items =
      [&](qt::tools::PackageResourceKind resource_kind)
      -> qt::foundation::Result<std::vector<InteractivePackageResourceItem>> {
    const auto package_settings = load_runtime_package_settings();
    if (!package_settings) {
      return qt::foundation::Result<
          std::vector<InteractivePackageResourceItem>>::failure(
          package_settings.error().code, package_settings.error().message,
          package_settings.error().system_error);
    }

    const auto normalize_path = [](const std::string& path) {
      return std::filesystem::path(path).lexically_normal().generic_string();
    };
    const auto extension_identity = [&](const std::string& path) {
      const auto canonical = runtime_repository().canonicalize_path(path);
      return canonical ? *canonical : normalize_path(path);
    };

    std::unordered_set<std::string> enabled_paths;
    std::unordered_set<std::string> externally_loaded_paths;
    if (resource_kind == qt::tools::PackageResourceKind::extensions) {
      std::unordered_set<std::string> external_roots;
      for (const auto& path : parsed.extensions) {
        external_roots.insert(extension_identity(path));
      }
      external_roots.insert(extension_identity(
          (std::filesystem::path(runtime->services.settings.cwd) / ".pi" /
           "extensions").generic_string()));
      external_roots.insert(extension_identity(
          (std::filesystem::path(runtime->services.settings.agent_home) /
           "extensions").generic_string()));
      for (const auto& extension :
           runtime->services.resource_bundle.extensions.loaded_extensions) {
        const auto identity = extension_identity(extension.path);
        enabled_paths.insert(identity);
        if (external_roots.count(identity) != 0U ||
            external_roots.count(extension_identity(extension.root)) != 0U) {
          externally_loaded_paths.insert(identity);
        }
      }
    } else if (resource_kind == qt::tools::PackageResourceKind::skills) {
      for (const auto& skill :
           runtime->services.resource_bundle.skills.loaded_skills) {
        enabled_paths.insert(normalize_path(skill.path));
      }
    } else if (resource_kind == qt::tools::PackageResourceKind::prompts) {
      for (const auto& prompt :
           runtime->services.resource_bundle.prompt_templates
               .loaded_prompt_templates) {
        enabled_paths.insert(normalize_path(prompt.path));
      }
    } else {
      const auto inspection =
          qt::app::inspect_app_resources(runtime_repository(),
                                         runtime->services.settings);
      for (const auto& theme : inspection.themes.loaded_themes) {
        enabled_paths.insert(normalize_path(theme.path));
      }
    }

    std::vector<qt::tools::PackageConfiguredSource> configured_packages =
        package_settings.value().project_packages;
    configured_packages.insert(configured_packages.end(),
                               package_settings.value().user_packages.begin(),
                               package_settings.value().user_packages.end());

    std::vector<InteractivePackageResourceItem> items;
    std::unordered_set<std::string> seen_paths;
    for (const auto& package : configured_packages) {
      if (!package.installed_path || package.installed_path->empty()) {
        continue;
      }

      const std::string package_root = normalize_path(*package.installed_path);
      const auto scope_text = std::string(qt::tools::to_string(package.scope));

      if (resource_kind == qt::tools::PackageResourceKind::extensions) {
        const auto loaded =
            qt::extensions::load_extension_manifests(runtime_repository(),
                                                     {package_root});
        for (const auto& extension : loaded.loaded_extensions) {
          const std::string path = normalize_path(extension.path);
          const std::string identity = extension_identity(path);
          if (!seen_paths.insert(identity).second) {
            continue;
          }
          const std::string name = extension.manifest.name.empty()
                                       ? extension.directory
                                       : extension.manifest.name;
          items.push_back(InteractivePackageResourceItem{
              path,
              package.source,
              package.scope,
              resource_kind,
              package_root,
              path,
              name,
              scope_text + " package " + package.source + " - " + path +
                  (externally_loaded_paths.count(identity) != 0U
                       ? " (read-only; loaded outside package settings)"
                       : ""),
              enabled_paths.find(identity) != enabled_paths.end(),
              externally_loaded_paths.count(identity) == 0U,
          });
        }
        continue;
      }

      if (resource_kind == qt::tools::PackageResourceKind::skills) {
        const auto loaded =
            qt::harness::load_skill_resources(runtime_repository(), {package_root});
        for (const auto& skill : loaded.loaded_skills) {
          const std::string path = normalize_path(skill.path);
          if (!seen_paths.insert(path).second) {
            continue;
          }
          items.push_back(InteractivePackageResourceItem{
              path,
              package.source,
              package.scope,
              resource_kind,
              package_root,
              path,
              skill.name,
              scope_text + " package " + package.source + " - " + path,
              enabled_paths.find(path) != enabled_paths.end(),
          });
        }
        continue;
      }

      if (resource_kind == qt::tools::PackageResourceKind::prompts) {
        const auto loaded = qt::harness::load_prompt_template_resources(
            runtime_repository(), {package_root});
        for (const auto& prompt : loaded.loaded_prompt_templates) {
          const std::string path = normalize_path(prompt.path);
          if (!seen_paths.insert(path).second) {
            continue;
          }
          const std::string description =
              prompt.description.empty() ? path : prompt.description;
          items.push_back(InteractivePackageResourceItem{
              path,
              package.source,
              package.scope,
              resource_kind,
              package_root,
              path,
              prompt.name,
              scope_text + " package " + package.source + " - " + description,
              enabled_paths.find(path) != enabled_paths.end(),
          });
        }
        continue;
      }

      const auto loaded =
          qt::app::load_theme_resources(runtime_repository(), {package_root});
      for (const auto& theme : loaded.loaded_themes) {
        const std::string path = normalize_path(theme.path);
        if (!seen_paths.insert(path).second) {
          continue;
        }
        items.push_back(InteractivePackageResourceItem{
            path,
            package.source,
            package.scope,
            resource_kind,
            package_root,
            path,
            theme.name,
            scope_text + " package " + package.source + " - " + path,
            enabled_paths.find(path) != enabled_paths.end(),
        });
      }
    }

    // CLI and auto-discovered extensions are not necessarily package resources.
    // Reuse the live inventory instead of loading their scripts a second time.
    if (resource_kind == qt::tools::PackageResourceKind::extensions) {
      for (const auto& extension :
           runtime->services.resource_bundle.extensions.loaded_extensions) {
        if (!seen_paths.insert(extension_identity(extension.path)).second) {
          continue;
        }
        InteractivePackageResourceItem item;
        item.id = normalize_path(extension.path);
        item.resource_path = item.id;
        item.label = extension.manifest.name.empty() ? extension.directory
                                                    : extension.manifest.name;
        item.description = "Loaded extension - " + item.resource_path +
                           " (read-only; managed outside package settings)";
        item.can_toggle = false;
        items.push_back(std::move(item));
      }
    }

    std::sort(items.begin(), items.end(),
              [](const InteractivePackageResourceItem& lhs,
                 const InteractivePackageResourceItem& rhs) {
                if (lhs.label != rhs.label) {
                  return lhs.label < rhs.label;
                }
                if (lhs.source != rhs.source) {
                  return lhs.source < rhs.source;
                }
                return lhs.resource_path < rhs.resource_path;
              });
    return qt::foundation::Result<
        std::vector<InteractivePackageResourceItem>>::success(std::move(items));
  };

  qt::app::ReferenceFooterDataProvider footer_data_provider(
      runtime->services.settings.cwd);
  {
    std::set<std::string> providers;
    for (const auto& model : runtime->services.model_catalog.models) {
      if (!model_is_available(runtime->services.model_catalog,
                              runtime->services.auth_storage, model)) {
        continue;
      }
      if (!model.ref.provider.empty()) {
        providers.insert(model.ref.provider);
      }
    }
    footer_data_provider.set_available_provider_count(providers.size());
  }

  struct OpenRouterCreditRefresh {
    std::optional<std::future<qt::foundation::Result<qt::ai::OpenRouterCredits>>>
        pending;
    std::optional<std::string> status_text;
    std::chrono::steady_clock::time_point last_started{};
    bool last_model_was_openrouter = false;
  } openrouter_credits;

  struct RemoteModelRefresh {
    std::optional<std::future<qt::foundation::Result<qt::ai::ModelCatalog>>>
        pending;
    qt::ai::ModelCatalog previous_discovery;
    std::chrono::steady_clock::time_point last_started{};
    bool announce_completion = false;
  } openrouter_models;
  openrouter_models.previous_discovery = qt::ai::cached_openrouter_models();
  RemoteModelRefresh openai_models;

  std::weak_ptr<qt::tui::ModelSelectorComponent> active_model_selector;
  const auto build_model_selector_items = [&]() {
    std::vector<qt::tui::ModelSelectorItem> items;
    const auto& scoped_models = runtime->services.settings.enabled_models;
    for (const auto& model : runtime->services.model_catalog.models) {
      if (!model_is_available(runtime->services.model_catalog,
                              runtime->services.auth_storage, model)) {
        continue;
      }
      const std::string id = model.ref.provider + "/" + model.ref.name;
      const auto rates = qt::ai::token_cost_rates_from_model_metadata(model);
      items.push_back(qt::tui::ModelSelectorItem{
          model.ref.provider,
          model.ref.name,
          model.display_name,
          rates ? std::optional<double>(rates->input_usd_per_million)
                : std::nullopt,
          rates ? std::optional<double>(rates->output_usd_per_million)
                : std::nullopt,
          runtime->host.harness().model().provider == model.ref.provider &&
              runtime->host.harness().model().name == model.ref.name,
          std::find(scoped_models.begin(), scoped_models.end(), id) !=
              scoped_models.end()});
    }
    return items;
  };

  const auto start_openrouter_model_refresh = [&](bool force,
                                                  bool announce_completion) {
    if (parsed.offline || openrouter_models.pending.has_value()) {
      return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!force && openrouter_models.last_started !=
                      std::chrono::steady_clock::time_point{} &&
        now - openrouter_models.last_started < std::chrono::minutes(5)) {
      return false;
    }
    openrouter_models.last_started = now;
    openrouter_models.announce_completion =
        openrouter_models.announce_completion || announce_completion;
    const auto auth_storage = runtime->services.auth_storage;
    openrouter_models.pending.emplace(std::async(
        std::launch::async, [auth_storage]() {
          return qt::ai::discover_openrouter_models(auth_storage, false);
        }));
    return true;
  };

  const auto start_openai_model_refresh = [&](bool force,
                                              bool announce_completion) {
    if (parsed.offline || openai_models.pending.has_value()) {
      return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!force && openai_models.last_started !=
                      std::chrono::steady_clock::time_point{} &&
        now - openai_models.last_started < std::chrono::minutes(5)) {
      return false;
    }
    openai_models.last_started = now;
    openai_models.announce_completion = announce_completion;
    const auto catalog = runtime->services.model_catalog;
    const auto auth = runtime->services.auth_storage;
    const auto api_key = runtime->host.harness().model().provider == "openai"
                             ? parsed.api_key : std::nullopt;
    openai_models.pending.emplace(std::async(
        std::launch::async, [catalog, auth, api_key]() {
          return qt::app::refresh_openai_models(catalog, auth, api_key);
        }));
    return true;
  };

  const auto openrouter_credit_secret = [&]() -> std::optional<std::string> {
    qt::ai::ProviderCredentialResolutionRequest request;
    request.provider = "openrouter";
    request.auth_storage = runtime->services.auth_storage;
    request.auth_record = runtime->services.model_selection.auth;
    const auto credential = qt::ai::resolve_provider_credential(request);
    return credential.secret;
  };
  const auto start_openrouter_credit_refresh = [&](bool force) {
    const bool selected =
        is_openrouter_provider(runtime->host.harness().model().provider);
    if (!selected) {
      openrouter_credits.status_text.reset();
      openrouter_credits.last_model_was_openrouter = false;
      return false;
    }
    if (!openrouter_credits.last_model_was_openrouter) {
      force = true;
      openrouter_credits.last_model_was_openrouter = true;
    }
    if (openrouter_credits.pending.has_value()) {
      return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!force && openrouter_credits.last_started !=
                      std::chrono::steady_clock::time_point{} &&
        now - openrouter_credits.last_started < std::chrono::minutes(5)) {
      return false;
    }
    const auto secret = openrouter_credit_secret();
    if (!secret || secret->empty()) {
      openrouter_credits.status_text = "credits unavailable";
      return true;
    }
    openrouter_credits.last_started = now;
    openrouter_credits.pending.emplace(std::async(
        std::launch::async, [secret]() {
          return qt::ai::fetch_openrouter_credits(*secret);
        }));
    return false;
  };
  const auto poll_openrouter_credit_refresh = [&]() {
    if (!is_openrouter_provider(runtime->host.harness().model().provider)) {
      const bool changed = openrouter_credits.status_text.has_value() ||
                           openrouter_credits.last_model_was_openrouter;
      openrouter_credits.status_text.reset();
      openrouter_credits.last_model_was_openrouter = false;
      return changed;
    }
    (void)start_openrouter_credit_refresh(false);
    if (!openrouter_credits.pending.has_value()) {
      return false;
    }
    if (openrouter_credits.pending->wait_for(std::chrono::milliseconds(0)) !=
        std::future_status::ready) {
      return false;
    }

    const auto result = openrouter_credits.pending->get();
    openrouter_credits.pending.reset();
    std::optional<std::string> next_status;
    if (result) {
      const double remaining =
          result.value().total_credits - result.value().total_usage;
      next_status = "credits $" + format_openrouter_credit_amount(remaining);
    } else {
      next_status = "credits unavailable";
    }
    if (next_status == openrouter_credits.status_text) {
      return false;
    }
    openrouter_credits.status_text = std::move(next_status);
    return true;
  };
  (void)start_openrouter_credit_refresh(true);
  (void)start_openrouter_model_refresh(true, false);
  (void)start_openai_model_refresh(true, false);

  std::vector<std::string> cached_footer_lines;
  const auto compute_footer_lines = [&]() {
    if (extension_ui_state.footer_lines.has_value()) {
      return *extension_ui_state.footer_lines;
    }
    footer_data_provider.clear_extension_statuses();
    for (const auto& [key, text] : extension_ui_state.statuses) {
      footer_data_provider.set_extension_status(key, text);
    }
    const auto stats = qt::harness::compute_session_stats(
        runtime->host.session_manager().get_entries(),
        rpc_context_window(runtime->services.model_catalog,
                           runtime->host.harness().model()));
    qt::app::ReferenceFooterRenderState state;
    state.cwd = runtime->services.settings.cwd;
    state.session_name = runtime->host.session_manager().get_session_name();
    state.model_provider = runtime->host.harness().model().provider;
    state.model_name = runtime->host.harness().model().name;
    state.reasoning_supported =
        runtime->services.model_selection.model.capabilities.reasoning;
    state.thinking_level = runtime->host.harness().thinking_level();
    state.provider_status = openrouter_credits.status_text;
    state.session_stats = std::move(stats);
    return qt::app::build_interactive_footer_lines(
        state, footer_data_provider, constraints.max_width);
  };
  const auto footer_provider = [&]() {
    return cached_footer_lines;
  };

  // Build the interactive host once for every extension invocation.  Keeping
  // this factory shared by commands and keyboard shortcuts ensures both paths
  // receive the same UI callbacks and that the invocation slot remains alive
  // for the duration of script initiated UI calls.
  const auto make_interactive_extension_host =
      [&runtime, &runtime_repository, &parsed, &queue_extension_prompt,
       &queue_extension_custom_message,
       &terminal_input_pump, &notcurses_session, &extension_ui_state,
       &set_terminal_title_if_needed, &cached_footer_lines,
       &compute_footer_lines, &constraints](
          qt::app::InteractiveTerminalSession& ui,
          std::shared_ptr<qt::extensions::AngelScriptRuntime> script_runtime,
          std::shared_ptr<qt::extensions::ExtensionHostCall> invocation_slot)
      -> qt::extensions::ExtensionHostCall {
    auto pump_extension_ui = [&ui, &terminal_input_pump, &notcurses_session]()
        -> qt::foundation::Status {
      if (notcurses_session.using_notcurses()) {
        const auto events = notcurses_session.poll_input(false);
        if (!events) return qt::foundation::Status(events.error());
        for (const auto& input : events.value()) {
          if (input.bytes.empty()) continue;
          const auto steps = ui.handle_input(input.bytes);
          if (!steps) return qt::foundation::Status(steps.error());
        }
        return qt::foundation::Status::success();
      }
      if (!terminal_input_pump.running() && !terminal_input_pump.start()) {
        return qt::foundation::Status::failure(
            qt::foundation::ErrorCode::io_error,
            "failed to start terminal input pump for extension UI");
      }
      auto batches = terminal_input_pump.wait_and_drain(
          std::chrono::milliseconds(25));
      if (batches.empty()) {
        const auto steps = ui.handle_input("");
        if (!steps) return qt::foundation::Status(steps.error());
      }
      for (auto& bytes : batches) {
        std::replace(bytes.begin(), bytes.end(), '\n', '\r');
        const auto steps = ui.handle_input(bytes);
        if (!steps) return qt::foundation::Status(steps.error());
      }
      return qt::foundation::Status::success();
    };

    extension_ui_state.apply_widgets =
        [&ui](const auto& widgets) { ui.set_extension_widgets(widgets); };
    extension_ui_state.apply_header =
        [&ui](auto lines) { ui.set_extension_header(std::move(lines)); };
    extension_ui_state.theme_call =
        [runtime, &runtime_repository, &ui](
            std::string_view method, const qt::ai::Json& arguments)
        -> qt::foundation::Result<qt::ai::Json> {
      const auto inspection = qt::app::inspect_app_resources(
          runtime_repository(), runtime->services.settings);
      if (method == "ctx.ui.getAllThemes") {
        qt::ai::Json themes = qt::ai::Json::array();
        themes.push_back(qt::ai::Json{{"name", "dark"}, {"path", nullptr}});
        themes.push_back(qt::ai::Json{{"name", "light"}, {"path", nullptr}});
        for (const auto& theme : inspection.themes.loaded_themes) {
          themes.push_back(qt::ai::Json{{"name", theme.name}, {"path", theme.path}});
        }
        return qt::foundation::Result<qt::ai::Json>::success(std::move(themes));
      }
      if (method == "ctx.ui.getTheme") {
        if (!arguments.is_object() || !arguments.contains("name") ||
            !arguments.at("name").is_string()) {
          return qt::foundation::Result<qt::ai::Json>::failure(
              qt::foundation::ErrorCode::invalid_argument,
              "UI method requires string name");
        }
        const auto name = arguments.at("name").get<std::string>();
        for (const auto& theme : inspection.themes.loaded_themes) {
          if (theme.name == name || theme.canonical_name == name) {
            return qt::foundation::Result<qt::ai::Json>::success(
                qt::ai::Json{{"name", theme.name}, {"path", theme.path},
                             {"canonicalName", theme.canonical_name},
                             {"content", theme.content}});
          }
        }
        return qt::foundation::Result<qt::ai::Json>::success(nullptr);
      }
      if (method == "ctx.ui.setTheme") {
        if (!arguments.is_object() || !arguments.contains("name") ||
            !arguments.at("name").is_string()) {
          return qt::foundation::Result<qt::ai::Json>::success(
              qt::ai::Json{{"success", false}, {"error", "theme name is required"}});
        }
        const auto name = arguments.at("name").get<std::string>();
        bool valid = name == "dark" || name == "light";
        std::string accent = name == "light" ? "\x1b[32m" : "\x1b[36m";
        for (const auto& theme : inspection.themes.loaded_themes) {
          valid = valid || theme.name == name || theme.canonical_name == name;
          if (theme.name == name || theme.canonical_name == name) {
            const auto parsed_theme = qt::app::parse_theme_document(theme.name, theme.content);
            if (!parsed_theme) return qt::foundation::Result<qt::ai::Json>::success(
                qt::ai::Json{{"success", false}, {"error", parsed_theme.error().message}});
            const auto color = parsed_theme.value().colors.find("accent");
            if (color != parsed_theme.value().colors.end()) {
              const auto ansi = qt::app::theme_foreground_ansi(
                  color->second, qt::app::ThemeColorMode::kTrueColor);
              if (ansi) accent = ansi.value();
            }
          }
        }
        if (!valid) return qt::foundation::Result<qt::ai::Json>::success(
            qt::ai::Json{{"success", false}, {"error", "unknown theme: " + name}});
        runtime->services.settings.theme = name;
        ui.set_theme_accent(std::move(accent));
        return qt::foundation::Result<qt::ai::Json>::success(
            qt::ai::Json{{"success", true}});
      }
      return qt::foundation::Result<qt::ai::Json>::success(nullptr);
    };
    if (script_runtime) {
      extension_ui_state.script_call =
          [script_runtime, invocation_slot](std::string_view handler,
                                             const qt::ai::Json& event)
          -> qt::foundation::Result<qt::ai::Json> {
        if (!*invocation_slot) return qt::foundation::Result<qt::ai::Json>::failure(
            qt::foundation::ErrorCode::invalid_argument,
            "extension host call is not bound");
        return script_runtime->invoke(handler, event, *invocation_slot);
      };
    } else {
      extension_ui_state.script_call = {};
    }
    auto presenter_call = qt::app::make_interactive_extension_ui_host_call(
        ui, extension_ui_state, std::move(pump_extension_ui),
        set_terminal_title_if_needed, [&ui, &cached_footer_lines, &compute_footer_lines]() {
          cached_footer_lines = compute_footer_lines();
          (void)ui.render();
        });
    return [weak_runtime = std::weak_ptr<qt::app::ReferenceStartupRuntime>(runtime),
            owner_thread = std::this_thread::get_id(), &parsed,
            &queue_extension_prompt, &queue_extension_custom_message,
            script_runtime,
            weak_slot = std::weak_ptr<qt::extensions::ExtensionHostCall>(invocation_slot),
            &extension_ui_state, &ui, &constraints,
            presenter_call = std::move(presenter_call)](
               std::string_view method, const qt::ai::Json& arguments)
        -> qt::foundation::Result<qt::ai::Json> {
      if (std::this_thread::get_id() != owner_thread) {
        return qt::foundation::Result<qt::ai::Json>::failure(
            qt::foundation::ErrorCode::invalid_argument,
            "interactive extension host calls require the UI thread");
      }
      const auto active_runtime = weak_runtime.lock();
      if (!active_runtime) return qt::foundation::Result<qt::ai::Json>::failure(
          qt::foundation::ErrorCode::invalid_argument, "extension runtime has ended");
      if (method.rfind("ctx.ui.", 0) == 0) {
        if (script_runtime) {
          const auto invocation_slot = weak_slot.lock();
          if (!invocation_slot) return qt::foundation::Result<qt::ai::Json>::failure(
              qt::foundation::ErrorCode::invalid_argument,
              "extension invocation has ended");
          extension_ui_state.script_call =
              [script_runtime, invocation_slot](std::string_view handler,
                                                 const qt::ai::Json& event)
              -> qt::foundation::Result<qt::ai::Json> {
            if (!*invocation_slot) {
              return qt::foundation::Result<qt::ai::Json>::failure(
                  qt::foundation::ErrorCode::invalid_argument,
                  "extension host call is not bound");
            }
            return script_runtime->invoke(handler, event, *invocation_slot);
          };
        }
        return presenter_call(method, arguments);
      }
      if (method == "pi.sendUserMessage") return queue_extension_prompt(arguments);
      if (method == "pi.sendMessage") {
        const auto sent = queue_extension_custom_message(arguments);
        if (sent && !arguments.value("triggerTurn", false) &&
            arguments.value("display", true)) {
          if (auto lines = render_extension_custom_message(
                  *active_runtime, arguments, constraints.max_width,
                  ui.tools_expanded())) {
            ui.append_transcript_literal_lines(std::move(*lines));
          }
        }
        return sent;
      }
      return qt::app::dispatch_extension_host_call(*active_runtime, parsed, method,
                                                    arguments);
    };
  };

  const auto seed_transcript = [&](qt::app::InteractiveTerminalSession& ui) {
    std::vector<std::string> lines;
    const auto component_lines =
        [&](qt::tui::Component& component) -> std::optional<std::vector<std::string>> {
      auto rendered = component.render(
          qt::tui::RenderConstraints{constraints.max_width, 0});
      if (!rendered) {
        return std::nullopt;
      }
      return std::move(rendered.value().lines);
    };
    for (const auto& message :
         runtime->host.harness().agent().state().transcript) {
      const std::string text = render_message_text(message);
      if (text.empty()) {
        continue;
      }
      const auto message_lines = [&]() {
        std::vector<std::string> result;
        std::size_t start = 0;
        while (start <= text.size()) {
          const auto end = text.find('\n', start);
          if (end == std::string::npos) {
            result.push_back(text.substr(start));
            break;
          }
          result.push_back(text.substr(start, end - start));
          start = end + 1;
          if (start == text.size()) {
            result.push_back("");
            break;
          }
        }
        return result;
      }();

      if (message.role == qt::ai::MessageRole::user) {
        if (message.metadata.value("role", std::string{}) == "custom") {
          if (message.metadata.contains("display") &&
              message.metadata.at("display").is_boolean() &&
              !message.metadata.at("display").get<bool>()) {
            continue;
          }
          auto custom_message = message.metadata;
          custom_message["content"] = text;
          if (auto rendered = render_extension_custom_message(
                  *runtime, custom_message, constraints.max_width,
                  ui.tools_expanded())) {
            lines.insert(lines.end(), rendered->begin(), rendered->end());
            continue;
          }
          qt::tui::CustomMessageComponent custom(
              message.metadata.value("customType", std::string("custom")),
              text);
          custom.set_expanded(true);
          if (auto rendered = component_lines(custom)) {
            lines.insert(lines.end(), rendered->begin(), rendered->end());
            continue;
          }
        }
        qt::tui::UserMessageComponent user(text, {}, {}, "qt> ");
        if (auto rendered = component_lines(user)) {
          lines.insert(lines.end(), rendered->begin(), rendered->end());
          continue;
        }
        bool first = true;
        for (const auto& line : message_lines) {
          lines.push_back((first ? "qt> " : "    ") + line);
          first = false;
        }
      } else {
        lines.insert(lines.end(), message_lines.begin(), message_lines.end());
      }
    }
    ui.set_transcript_lines(std::move(lines));
    cached_footer_lines = compute_footer_lines();
  };
  const auto replace_transcript_with_compaction_summary =
      [&](qt::app::InteractiveTerminalSession& ui, std::string_view reason,
          const qt::harness::ModelBackedCompactionResult& result) {
        ui.set_transcript_lines(compaction_transcript_lines(reason, result));
        cached_footer_lines = compute_footer_lines();
      };

  const auto session_before_switch_reason =
      [](const ReferenceStartupSession& session) {
        if (session.mode == "resume" || session.mode == "import") {
          return qt::extensions::ExtensionSessionBeforeSwitchReason::kResume;
        }
        return qt::extensions::ExtensionSessionBeforeSwitchReason::kNew;
      };
  const auto session_start_reason_for =
      [](const ReferenceStartupSession& session) {
        if (session.mode == "resume" || session.mode == "import") {
          return qt::extensions::ExtensionSessionStartReason::kResume;
        }
        if (session.mode == "fork") {
          return qt::extensions::ExtensionSessionStartReason::kFork;
        }
        if (session.mode == "reload") {
          return qt::extensions::ExtensionSessionStartReason::kReload;
        }
        return qt::extensions::ExtensionSessionStartReason::kNew;
      };
  const auto session_shutdown_reason_for =
      [](const ReferenceStartupSession& session) {
        if (session.mode == "resume" || session.mode == "import") {
          return qt::extensions::ExtensionSessionShutdownReason::kResume;
        }
        if (session.mode == "fork") {
          return qt::extensions::ExtensionSessionShutdownReason::kFork;
        }
        if (session.mode == "reload") {
          return qt::extensions::ExtensionSessionShutdownReason::kReload;
        }
        return qt::extensions::ExtensionSessionShutdownReason::kNew;
      };

  const auto switch_to_session =
      [&](qt::app::InteractiveTerminalSession& ui,
          const ReferenceStartupSession& next_session,
          std::string note) -> qt::foundation::Status {
    auto before_switch =
        qt::app::run_extension_session_before_switch_hooks_subprocess(
            runtime->services.resource_bundle.extension_host,
            session_before_switch_reason(next_session),
            next_session.repository_path);
    if (!before_switch) {
      return qt::foundation::Status(before_switch.error());
    }
    if (before_switch.value().cancelled) {
      return qt::foundation::Status::failure(
          qt::foundation::ErrorCode::invalid_argument,
          "session switch cancelled by extension");
    }

    if (turn_pump.active()) {
      const auto completed = turn_pump.cancel_and_wait(
          [runtime]() { runtime->host.harness().request_abort(); });
      if (!completed) {
        return qt::foundation::Status(completed.error());
      }
      pending_steer_messages.clear();
      pending_follow_up_messages.clear();
      live_turn_transcript.stop();
      working_indicator.stop();
      working_indicator.set_message(std::nullopt);
    }

    if (runtime->services.subagent_coordinator) runtime->services.subagent_coordinator->shutdown();
    auto next_runtime = make_startup_runtime(parsed, next_session);
    if (!next_runtime) {
      return qt::foundation::Status(next_runtime.error());
    }

    const auto shutdown_status = run_current_session_shutdown_hooks_once(
        session_shutdown_reason_for(next_session),
        next_session.mode == "reload" ? std::nullopt : next_session.repository_path);
    if (!shutdown_status) {
      return shutdown_status;
    }

    const auto previous_session_file = current_session->repository_path;
    ui.set_extension_terminal_input_handler({});
    ui.set_extension_editor_input_handler({});
    ui.set_extension_editor_component(nullptr);
    ui.set_extension_autocomplete_provider({});
    ui.set_extension_widgets({});
    ui.set_extension_header(std::nullopt);
    extension_ui_state.statuses.clear();
    extension_ui_state.widgets.clear();
    extension_ui_state.working_message.reset();
    extension_ui_state.working_frames.reset();
    extension_ui_state.hidden_thinking_label.reset();
    extension_ui_state.working_visible = true;
    extension_ui_state.tools_expanded = false;
    working_indicator.set_visible(true);
    working_indicator.set_indicator(qt::tui::LoaderIndicator{});
    working_indicator.set_message(std::nullopt);
    live_turn_transcript.set_hidden_thinking_label(std::nullopt);
    ui.set_tools_expanded(false);
    extension_ui_state.footer_lines.reset();
    extension_ui_state.header_lines.reset();
    extension_ui_state.apply_widgets = {};
    extension_ui_state.apply_footer = {};
    extension_ui_state.apply_header = {};
    extension_ui_state.editor_handler = {};
    extension_ui_state.theme_call = {};
    extension_ui_state.script_call = {};
    extension_ui_state.invocation_slot.reset();
    extension_prompts.clear();
    *runtime = std::move(next_runtime.value());
    qt::app::bind_extension_host_call(runtime, parsed);
    qt::app::bind_subagent_runtime(runtime);
    const auto lifecycle_owner_thread = std::this_thread::get_id();
    for (auto& extension :
         runtime->services.resource_bundle.extension_host.registry.loaded_extensions) {
      if (!extension.script_runtime) continue;
      const auto fallback = *extension.host_call;
      const auto invocation_slot = extension.host_call;
      auto ui_host = make_interactive_extension_host(
          ui, extension.script_runtime, invocation_slot);
      *invocation_slot =
          [ui_host, fallback, lifecycle_owner_thread](
              std::string_view method, const qt::ai::Json& arguments)
          -> qt::foundation::Result<qt::ai::Json> {
        if (std::this_thread::get_id() != lifecycle_owner_thread && fallback) {
          return fallback(method, arguments);
        }
        return ui_host(method, arguments);
      };
      extension.host_call = invocation_slot;
    }
    session_shutdown_hooks_fired = false;
    *current_session = next_session;
    ui.set_tool_entry_compaction_enabled_tools(
        runtime->services.settings.terminal_tool_entry_compaction_enabled_tools);
    const auto start_baseline = runtime->host.harness().session_manager().get_entries().size();
    auto start_hooks = qt::app::run_extension_session_start_hooks_subprocess(
        runtime->services.resource_bundle.extension_host,
        session_start_reason_for(next_session),
        next_session.mode == "reload" ? std::nullopt : previous_session_file);
    const auto start_persisted = persist_extension_entries(start_baseline);
    if (!start_persisted) return start_persisted;
    if (!start_hooks) {
      return qt::foundation::Status(start_hooks.error());
    }
    footer_data_provider.set_cwd(runtime->services.settings.cwd);
    ui.set_autocomplete_provider(build_autocomplete_provider());
    const auto prompt_history_warning = restore_prompt_history(
        ui, runtime->host.session_state().metadata().prompt_history);
    seed_transcript(ui);
    if (prompt_history_warning) {
      ui.append_transcript_line("Warning: " + *prompt_history_warning);
    }
    if (!note.empty()) {
      ui.append_transcript_line(std::move(note));
    }
    return qt::foundation::Status::success();
  };

  struct UiPostQueue {
    std::atomic_bool alive{true};
    std::mutex mutex;
    std::vector<std::function<void(qt::app::InteractiveTerminalSession&)>>
        actions;
  };
  auto ui_post_queue = std::make_shared<UiPostQueue>();
  const auto post_to_ui =
      [ui_post_queue](
          std::function<void(qt::app::InteractiveTerminalSession&)> action) {
        if (!action || !ui_post_queue->alive.load()) {
          return;
        }
        std::lock_guard<std::mutex> lock(ui_post_queue->mutex);
        if (ui_post_queue->alive.load()) {
          ui_post_queue->actions.push_back(std::move(action));
        }
      };
  const auto flush_ui_post_queue =
      [&](qt::app::InteractiveTerminalSession& session) -> qt::foundation::Status {
    std::vector<std::function<void(qt::app::InteractiveTerminalSession&)>>
        actions;
    {
      std::lock_guard<std::mutex> lock(ui_post_queue->mutex);
      actions.swap(ui_post_queue->actions);
    }
    if (actions.empty()) {
      return qt::foundation::Status::success();
    }
    for (auto& action : actions) {
      action(session);
    }
    const auto rendered = session.render();
    if (!rendered) {
      return qt::foundation::Status(rendered.error());
    }
    return qt::foundation::Status::success();
  };

  qt::app::InteractiveAuthCommandContext interactive_auth_context{
      runtime->services,
      [&, current_session](qt::app::InteractiveTerminalSession& ui,
                           std::string note) {
        return switch_to_session(ui, *current_session, std::move(note));
      },
      show_text_prompt,
      post_to_ui,
  };

  const auto command_executor =
      [&](qt::app::InteractiveTerminalSession& ui,
          const qt::app::InteractiveShellEvent& event)
      -> qt::foundation::Result<qt::app::InteractiveSlashCommandResponse> {
    qt::app::InteractiveSlashCommandResponse response;
    const auto slash_invocation_text = [](const qt::app::InteractiveShellEvent& slash_event) {
      std::string text = "/";
      text += slash_event.command;
      if (!slash_event.arguments.empty()) {
        text += " ";
        text += slash_event.arguments;
      }
      return text;
    };
    const auto registry = build_registry();
    const auto registered_command =
        qt::app::find_slash_command(registry, event.command);
    const bool is_extension_command =
        registered_command &&
        registered_command->source == qt::app::SlashCommandSource::kExtension;

    // Most built-ins are independent of the in-flight provider request, and
    // session replacement commands cancel and join that request before
    // replacing the runtime. Keep only commands with a concrete live-state
    // conflict unavailable instead of rejecting every command not explicitly
    // allowlisted. Full context inspection and compaction access transcript
    // state owned by the turn worker, while reload must not replace resources
    // underneath it. Mutating llama operations similarly share provider state
    // with an active local-model request.
    static const std::set<std::string> kUnavailableDuringActiveTurn = {
        "compact",
        "context",
        "reload",
    };
    const bool unavailable_during_active_turn =
        kUnavailableDuringActiveTurn.find(event.command) !=
            kUnavailableDuringActiveTurn.end() ||
        (event.command == "llama" &&
         !qt::app::is_llama_slash_command_read_only(event.arguments));
    if (turn_pump.active() && unavailable_during_active_turn &&
        !is_extension_command) {
      return qt::foundation::Result<
          qt::app::InteractiveSlashCommandResponse>::failure(
          qt::foundation::ErrorCode::invalid_argument,
          "slash commands are unavailable while a turn is running");
    }

    const auto auth_command = qt::app::try_handle_interactive_auth_command(
        ui, event, interactive_auth_context);
    if (!auth_command) {
      return auth_command.error();
    }
    if (auth_command.value()) {
      return *auth_command.value();
    }

    {
      const auto execution =
          qt::app::execute_slash_command(registry, slash_invocation_text(event));
      if (!execution) {
        return qt::foundation::Result<
            qt::app::InteractiveSlashCommandResponse>::failure(
            execution.error().code, execution.error().message,
            execution.error().system_error);
      }
      if (execution.value().kind ==
              qt::app::SlashCommandExecutionKind::kPromptExpansion ||
          execution.value().kind ==
              qt::app::SlashCommandExecutionKind::kSkillExpansion) {
        auto input_result = run_input_hooks(slash_invocation_text(event), {});
        if (!input_result) {
          return qt::foundation::Result<
              qt::app::InteractiveSlashCommandResponse>::failure(
              input_result.error().code, input_result.error().message,
              input_result.error().system_error);
        }
        if (input_result.value().handled) {
          response.handled = true;
          response.append_default_output = false;
          return response;
        }
        auto input_images =
            image_contents_from_json(input_result.value().current_event.images);
        if (!input_images) {
          return qt::foundation::Result<
              qt::app::InteractiveSlashCommandResponse>::failure(
              input_images.error().code, input_images.error().message,
              input_images.error().system_error);
        }
        const auto submitted = start_prompt_after_input(
            input_result.value().current_event.text,
            std::move(input_images.value()), "Running prompt...");
        if (!submitted) {
          return qt::foundation::Result<
              qt::app::InteractiveSlashCommandResponse>::failure(
              submitted.error().code, submitted.error().message,
              submitted.error().system_error);
        }
        if (!submitted.value()) {
          return qt::foundation::Result<
              qt::app::InteractiveSlashCommandResponse>::failure(
              qt::foundation::ErrorCode::invalid_argument,
              "prompt submission is already running");
        }
        response.handled = true;
        response.append_default_output = false;
        return response;
      }
      if (execution.value().kind ==
          qt::app::SlashCommandExecutionKind::kExtension) {
        const auto descriptor =
            qt::extensions::find_extension_command_contribution(
                runtime->services.resource_bundle.extension_host.registry,
                execution.value().command.name);
        if (!descriptor) {
          return qt::foundation::Result<
              qt::app::InteractiveSlashCommandResponse>::failure(
              descriptor.error().code, descriptor.error().message,
              descriptor.error().system_error);
        }

        const auto extension_entry_baseline =
            runtime->host.session_manager().get_entries().size();
        const auto invocation_slot =
            std::make_shared<qt::extensions::ExtensionHostCall>();
        extension_ui_state.invocation_slot = invocation_slot;
        auto host_call = make_interactive_extension_host(
            ui, descriptor.value().extension.script_runtime, invocation_slot);
        *invocation_slot = host_call;
        auto command_result = qt::app::run_extension_contribution_subprocess(
            descriptor.value(), Json{{"args", event.arguments}},
            qt::app::ExtensionContributionRunOptions{
                30'000, nullptr, *invocation_slot});
        const auto persist_extension_state =
            persist_extension_entries(extension_entry_baseline);
        if (!persist_extension_state) {
          return qt::foundation::Result<
              qt::app::InteractiveSlashCommandResponse>::failure(
              persist_extension_state.error().code,
              persist_extension_state.error().message,
              persist_extension_state.error().system_error);
        }
        if (!command_result) {
          return qt::foundation::Result<
              qt::app::InteractiveSlashCommandResponse>::failure(
              command_result.error().code, command_result.error().message,
              command_result.error().system_error);
        }
        if (!command_result.value().ok) {
          std::string message = "extension command /";
          message += execution.value().command.name;
          message += " failed";
          if (command_result.value().error.has_value() &&
              !command_result.value().error->message.empty()) {
            message += ": ";
            message += command_result.value().error->message;
          }
          return qt::foundation::Result<
              qt::app::InteractiveSlashCommandResponse>::failure(
              qt::foundation::ErrorCode::invalid_argument, std::move(message));
        }

        response.handled = true;
        response.append_default_output = false;
        response.transcript_lines = extension_command_result_lines(
            execution.value().command.name, command_result.value());
        return response;
      }
    }

    const auto exact_model =
        [&](std::string_view value) -> const qt::ai::ModelCatalogModelRecord* {
      return qt::ai::match_model_record(runtime->services.model_catalog,
                                        std::string(value));
    };

    const auto session_summary_lines = [&]() {
      std::vector<std::string> lines;
      const auto entries =
          load_startup_session_entries(*current_session);
      const auto stats = qt::harness::compute_session_stats(
          entries, rpc_context_window(runtime->services.model_catalog,
                                      runtime->host.harness().model()));
      std::size_t bash_commands = 0;
      for (const auto& entry : entries) {
        switch (entry.kind()) {
          case qt::harness::SessionEntryKind::kBashExecution:
            ++bash_commands;
            break;
          default:
            break;
        }
      }

      if (const auto name = runtime->host.session_manager().get_session_name();
          name && !name->empty()) {
        lines.push_back("name: " + *name);
      }
      lines.push_back("file: " +
                      current_session->repository_path.value_or(
                          std::string("in-memory")));
      lines.push_back("id: " +
                      current_session->session_id.value_or(
                          std::string("in-memory")));
      lines.push_back("cwd: " + runtime->services.settings.cwd);
      lines.push_back("model: " + runtime->host.harness().model().provider +
                      "/" + runtime->host.harness().model().name);
      lines.push_back("thinking: " + runtime->host.harness().thinking_level());
      lines.push_back("messages: user=" + std::to_string(stats.user_messages) +
                      ", assistant=" + std::to_string(stats.assistant_messages) +
                      ", tool_calls=" + std::to_string(stats.tool_calls) +
                      ", tool_results=" + std::to_string(stats.tool_results) +
                      ", bash=" + std::to_string(bash_commands));
      lines.push_back("tokens: input=" + std::to_string(stats.tokens.input) +
                      ", output=" + std::to_string(stats.tokens.output) +
                      ", total=" + std::to_string(stats.tokens.total));
      if (stats.cost > 0.0) {
        std::ostringstream out;
        out << "cost: $" << std::fixed << std::setprecision(4) << stats.cost;
        lines.push_back(out.str());
      }
      if (stats.has_unknown_usage) {
        lines.push_back("usage: partial (some assistant usage is unknown)");
      }
      if (stats.context_usage && stats.context_usage->tokens) {
        std::ostringstream out;
        out << "context: " << *stats.context_usage->tokens << "/"
            << stats.context_usage->context_window;
        if (stats.context_usage->percent) {
          out << " (" << std::fixed << std::setprecision(1)
              << *stats.context_usage->percent << "%)";
        }
        lines.push_back(out.str());
      }
      return lines;
    };

    const auto context_kind_label =
        [](qt::harness::ContextFileKind kind) -> const char* {
      switch (kind) {
        case qt::harness::ContextFileKind::kAgents:
          return "AGENTS.md";
        case qt::harness::ContextFileKind::kClaude:
          return "CLAUDE.md";
      }
      return "context";
    };
    const auto context_scope_label =
        [](qt::harness::ContextFileScope scope) -> const char* {
      switch (scope) {
        case qt::harness::ContextFileScope::kGlobal:
          return "global";
        case qt::harness::ContextFileScope::kAncestor:
          return "ancestor";
        case qt::harness::ContextFileScope::kCurrentDirectory:
          return "current";
      }
      return "unknown";
    };
    const auto message_text_bytes = [](const qt::ai::Message& message) {
      std::size_t bytes = 0;
      for (const auto& content : message.content) {
        if (const auto* text = std::get_if<qt::ai::TextContent>(&content)) {
          bytes += text->text.size();
        } else if (const auto* tool_result =
                       std::get_if<qt::ai::ToolResultContent>(&content)) {
          bytes += tool_result->result.dump().size();
        } else if (const auto* tool_call =
                       std::get_if<qt::ai::ToolCallContent>(&content)) {
          bytes += tool_call->arguments.dump().size();
        }
      }
      return bytes;
    };
    const auto context_summary_lines = [&](std::string_view arguments) {
      std::vector<std::string> lines;
      const auto& bundle = runtime->services.resource_bundle;
      const auto session_context =
          runtime->host.session_manager().build_session_context();
      const auto session_stats = qt::harness::compute_session_stats(
          runtime->host.session_manager().entries(),
          rpc_context_window(runtime->services.model_catalog,
                             runtime->host.harness().model()));

      std::set<std::string> sections;
      std::string normalized_arguments = lowercase_ascii(std::string(arguments));
      std::replace(normalized_arguments.begin(), normalized_arguments.end(), ',',
                   ' ');
      std::istringstream argument_stream(normalized_arguments);
      for (std::string section; argument_stream >> section;) {
        if (section == "all") {
          sections = {"environment", "prompt", "messages", "resources",
                      "diagnostics", "memory", "usage"};
          break;
        }
        if (section == "context" || section == "system") {
          section = "prompt";
        } else if (section == "session" || section == "history") {
          section = "messages";
        } else if (section == "resource") {
          section = "resources";
        } else if (section == "diagnostic") {
          section = "diagnostics";
        }
        sections.insert(std::move(section));
      }
      if (sections.empty()) {
        sections = {"environment", "prompt", "messages", "resources",
                    "diagnostics", "memory", "usage"};
      }
      const std::set<std::string> supported_sections = {
          "environment", "prompt", "messages", "resources", "diagnostics",
          "memory", "usage"};
      for (const auto& section : sections) {
        if (supported_sections.find(section) == supported_sections.end()) {
          return std::vector<std::string>{
              "Unknown /context section: " + section,
              "Use /context [all|environment|prompt|messages|resources|diagnostics|memory|usage]."};
        }
      }

      const auto append_content = [&lines](std::string_view heading,
                                           std::string_view content) {
        lines.push_back(std::string(heading));
        if (content.empty()) {
          lines.push_back("(empty)");
          return;
        }
        std::size_t start = 0;
        while (start <= content.size()) {
          const auto end = content.find('\n', start);
          lines.emplace_back(content.substr(
              start, end == std::string_view::npos ? std::string_view::npos
                                                    : end - start));
          if (end == std::string_view::npos) {
            break;
          }
          start = end + 1;
        }
      };
      const auto append_diagnostics = [&lines](std::string_view heading,
                                               const std::vector<std::string>& values) {
        lines.push_back(std::string(heading));
        if (values.empty()) {
          lines.push_back("- none");
          return;
        }
        for (const auto& value : values) {
          lines.push_back("- " + value);
        }
      };
      std::size_t context_bytes = 0;
      for (const auto& section : bundle.prompt_assembly.sections) {
        context_bytes += section.content.size();
      }
      std::size_t transcript_bytes = 0;
      for (const auto& message : session_context.messages) {
        transcript_bytes += message_text_bytes(message);
      }

      lines.push_back("Context inspection (default is all available content).");
      lines.push_back("The next provider request is assembled at dispatch; this shows the current assembled system prompt and resolved session context, not unavailable future-turn state.");

      if (sections.count("environment")) {
        lines.push_back("environment:");
        lines.push_back("- cwd: " + runtime->services.settings.cwd);
        lines.push_back("- repository_root: " +
                        bundle.context_resources.discovery.repository_root
                            .value_or(std::string("(none)")));
        lines.push_back("- workspace_root: " +
                        bundle.system_prompt_input.workspace_root.value_or(
                            runtime->services.settings.workspace_root.empty()
                                ? runtime->services.settings.cwd
                                : runtime->services.settings.workspace_root));
        if (bundle.system_prompt_input.shell) lines.push_back("- shell: " + *bundle.system_prompt_input.shell);
        if (bundle.system_prompt_input.current_date) lines.push_back("- current_date: " + *bundle.system_prompt_input.current_date);
        if (bundle.system_prompt_input.timezone) lines.push_back("- timezone: " + *bundle.system_prompt_input.timezone);
        if (bundle.system_prompt_input.network_access) lines.push_back("- network: " + *bundle.system_prompt_input.network_access);
        if (bundle.system_prompt_input.tool_permissions) lines.push_back("- tool_permissions: " + *bundle.system_prompt_input.tool_permissions);
        if (bundle.system_prompt_input.active_tool_count) lines.push_back("- active_tools: " + std::to_string(*bundle.system_prompt_input.active_tool_count));
      }

      if (sections.count("prompt")) {
        append_content("assembled_system_prompt (" + std::to_string(bundle.system_prompt.prompt.size()) + " bytes):", bundle.system_prompt.prompt);
        lines.push_back("prompt_sections: " + std::to_string(bundle.prompt_assembly.sections.size()) + ", bytes=" + std::to_string(context_bytes));
        for (const auto& record : bundle.prompt_assembly.sections) {
          append_content("prompt_section [" + std::string(context_scope_label(record.scope)) + "] " + context_kind_label(record.kind) + " " + record.path + " (" + std::to_string(record.content.size()) + " bytes):", record.content);
        }
        if (bundle.prompt_assembly.sections.empty()) lines.push_back("- no project context sections loaded");
        if (bundle.prompt_contributions.system_prompt) append_content("system_prompt_override " + bundle.prompt_contributions.system_prompt->path + ":", bundle.prompt_contributions.system_prompt->content);
        if (bundle.prompt_contributions.append_system_prompt) append_content("append_system_prompt " + bundle.prompt_contributions.append_system_prompt->path + ":", bundle.prompt_contributions.append_system_prompt->content);
      }

      if (sections.count("messages")) {
        lines.push_back("resolved_session_messages: " + std::to_string(session_context.messages.size()) + ", text_bytes=" + std::to_string(transcript_bytes));
        if (session_context.model) lines.push_back("session_model: " + session_context.model->provider + "/" + session_context.model->model_id);
        lines.push_back("session_thinking: " + session_context.thinking_level);
        for (std::size_t index = 0; index < session_context.messages.size(); ++index) append_content("message[" + std::to_string(index) + "]:", qt::ai::Json(session_context.messages[index]).dump(2));
        const auto& active_transcript = runtime->host.harness().agent().state().transcript;
        lines.push_back("active_agent_transcript: " +
                        std::to_string(active_transcript.size()) +
                        " messages (before per-request context hooks)");
        for (std::size_t index = 0; index < active_transcript.size(); ++index) {
          append_content("active_message[" + std::to_string(index) + "]:",
                         qt::ai::Json(active_transcript[index]).dump(2));
        }
        const auto& pending = runtime->host.harness().agent().state().pending;
        lines.push_back("active_pending_messages: " +
                        std::to_string(pending.size()));
        for (const auto& message : pending) {
          append_content("pending_message sequence=" +
                             std::to_string(message.sequence) + ":",
                         qt::ai::Json(message.message).dump(2));
        }
        const auto& completed_requests =
            runtime->host.harness().after_provider_response_events();
        if (completed_requests.empty()) {
          lines.push_back("last_provider_payload: unavailable (no completed provider request in this runtime)");
        } else {
          append_content("last_provider_payload (captured completed request; not a prediction of the next request):",
                         qt::ai::Json(completed_requests.back().request).dump(2));
        }
      }

      if (sections.count("resources")) {
        lines.push_back("resources: skills=" + std::to_string(bundle.skills.loaded_skills.size()) + ", prompts=" + std::to_string(bundle.prompt_templates.loaded_prompt_templates.size()) + ", extensions=" + std::to_string(bundle.extensions.loaded_extensions.size()) + ", tools=" + std::to_string(runtime->services.tool_definitions.size()));
        for (const auto& skill : bundle.skills.loaded_skills) append_content("skill " + skill.name + " " + skill.path + (skill.disable_model_invocation ? " (hidden from model):" : ":"), skill.content);
        for (const auto& prompt : bundle.prompt_templates.loaded_prompt_templates) append_content("prompt /" + prompt.name + " " + prompt.path + ":", prompt.content);
        for (const auto& extension : bundle.extensions.loaded_extensions) append_content("extension manifest " + extension.manifest.name + " " + extension.path + ":", qt::extensions::Json(extension.manifest).dump(2));
        if (!bundle.extensions.loaded_extensions.empty()) lines.push_back("extension implementation source is not retained in the resource bundle; manifest paths above identify the loaded sources.");
        const auto& active_tool_names = runtime->host.harness().active_tool_names();
        for (const auto& tool : runtime->services.tool_definitions) {
          const bool active = std::find(active_tool_names.begin(), active_tool_names.end(), tool.name) != active_tool_names.end();
          append_content("tool " + tool.name + (active ? " (active):" : " (disabled):"), qt::ai::Json(tool).dump(2));
        }
      }

      if (sections.count("diagnostics")) {
        append_diagnostics("runtime_diagnostics:", runtime->services.diagnostics);
        append_diagnostics("resource_diagnostics:", bundle.diagnostics);
        append_diagnostics("context_diagnostics:", bundle.context_resources.diagnostics);
        append_diagnostics("prompt_assembly_diagnostics:", bundle.prompt_assembly.diagnostics);
        append_diagnostics("system_prompt_diagnostics:", bundle.system_prompt.diagnostics);
        lines.push_back("instruction_load_events: " + std::to_string(bundle.context_resources.instruction_load_events.size()));
        for (const auto& event : bundle.context_resources.instruction_load_events) lines.push_back("- [" + std::string(context_scope_label(event.scope)) + "] " + context_kind_label(event.kind) + " " + event.path + " reason=" + event.reason + " bytes=" + std::to_string(event.content_bytes));
        lines.push_back("instruction_load_hooks_planned: " + std::to_string(bundle.instruction_loaded_routing_plans.size()));
      }

      if (sections.count("memory")) {
        lines.push_back("memory: latest prepared transient augmentation only; it does not prove provider consumption and is not persisted session context.");
        if (!runtime->services.memory_runtime) {
          lines.push_back("- memory runtime is unavailable");
        } else if (const auto augmentation = runtime->services.memory_runtime
                       ->coordinator().last_augmentation(
                           runtime->host.harness()
                               .model_context_projection_epoch())) {
          const auto& contribution = augmentation->contribution;
          lines.push_back("- logical_input_sequence: " + std::to_string(contribution.logical_input_sequence));
          lines.push_back("- memory_ids: " + qt::ai::Json(contribution.memory_ids).dump());
          lines.push_back("- code_ids: " + qt::ai::Json(contribution.code_ids).dump());
          lines.push_back("- block_ids: " + qt::ai::Json(contribution.block_ids).dump());
          lines.push_back("- estimated_tokens: " + std::to_string(contribution.estimated_tokens));
          lines.push_back("- omission_reason: " + contribution.omission_reason);
          lines.push_back("- isolated_agent: " + std::string(augmentation->used_isolated_agent ? "true" : "false") + ", deterministic_fallback: " + (augmentation->used_deterministic_fallback ? "true" : "false"));
          for (const auto& warning : contribution.warnings) lines.push_back("- warning [" + warning.code + "] " + warning.message + " memory=" + warning.memory_id + " from=" + warning.from_id + " to=" + warning.to_id);
          append_diagnostics("memory_diagnostics:", augmentation->diagnostics);
          append_content("memory_context_markdown:", contribution.context_markdown);
        } else {
          lines.push_back("- no augmentation has been prepared in this projection");
        }
      }

      if (sections.count("usage")) {
        lines.push_back("model_usage: input=" + std::to_string(session_stats.tokens.input) + ", output=" + std::to_string(session_stats.tokens.output) + ", total=" + std::to_string(session_stats.tokens.total));
        if (session_stats.context_usage) {
          lines.push_back("model_context_usage: tokens=" +
                          (session_stats.context_usage->tokens
                               ? std::to_string(*session_stats.context_usage->tokens)
                               : std::string("unknown")) +
                          ", context_window=" +
                          std::to_string(
                              session_stats.context_usage->context_window) +
                          (session_stats.context_usage->percent
                               ? ", percent=" + std::to_string(
                                                    *session_stats.context_usage
                                                         ->percent)
                               : std::string(", percent=unknown")));
        } else {
          lines.push_back("model_context_usage: unavailable (no model context-window estimate)");
        }
      }
      return lines;
    };

    if (event.command == "status") {
      response.handled = true;
      response.append_default_output = false;
      const auto auth_status = qt::app::describe_auth_provider_status(
          runtime->services.auth_storage, runtime->host.harness().model().provider,
          runtime->services.model_selection.auth &&
                  runtime->services.model_selection.auth->api_key
              ? runtime->services.model_selection.auth->api_key
              : std::nullopt);
      response.transcript_lines = {
          "cwd: " + runtime->services.settings.cwd,
          "session: " +
              current_session->session_id.value_or(std::string("in-memory")),
          "model: " + runtime->host.harness().model().provider + "/" +
              runtime->host.harness().model().name,
          "thinking: " + runtime->host.harness().thinking_level(),
          "auth: " + auth_status.label,
          "tools: " + std::to_string(runtime->services.tool_definitions.size()) +
              ", prompts: " +
              std::to_string(runtime->services.resource_bundle.prompt_templates
                                 .loaded_prompt_templates.size()) +
              ", skills: " +
              std::to_string(
                  runtime->services.resource_bundle.skills.loaded_skills.size()) +
              ", extensions: " +
              std::to_string(runtime->services.resource_bundle.extensions
                                 .loaded_extensions.size()),
      };
      return response;
    }

    if (event.command == "context") {
      response.handled = true;
      response.append_default_output = false;
      response.transcript_lines = context_summary_lines(event.arguments);
      return response;
    }

    if (event.command == "session") {
      response.handled = true;
      response.append_default_output = false;
      response.transcript_lines = session_summary_lines();
      return response;
    }

    if (event.command == "ollama") {
      response.handled = true;
      response.append_default_output = false;

      auto& instances = runtime->services.settings.ollama_instances;
      const auto find_instance = [&](const std::string& id)
          -> qt::ai::OllamaInstanceSettings* {
        const auto found = std::find_if(
            instances.begin(), instances.end(),
            [&](const auto& instance) {
              return lowercase_ascii(instance.id) == lowercase_ascii(id);
            });
        return found == instances.end() ? nullptr : &*found;
      };
      const auto replace_instance_catalog = [&](const std::string& id) {
        const std::string provider_id = qt::ai::ollama_provider_id(id);
        auto& catalog = runtime->services.model_catalog;
        catalog.providers.erase(
            std::remove_if(catalog.providers.begin(), catalog.providers.end(),
                           [&](const auto& provider) {
                             return provider.descriptor.id == provider_id;
                           }),
            catalog.providers.end());
        catalog.models.erase(
            std::remove_if(catalog.models.begin(), catalog.models.end(),
                           [&](const auto& model) {
                             return model.ref.provider == provider_id;
                           }),
            catalog.models.end());
        catalog = qt::ai::merge_catalogs(
            catalog, qt::ai::model_catalog_from_ollama_settings(instances));
      };
      const auto refresh_instance = [&](qt::ai::OllamaInstanceSettings& instance)
          -> qt::foundation::Status {
        const auto discovered = qt::ai::discover_ollama_models(instance);
        if (!discovered) {
          return qt::foundation::Status(discovered.error());
        }
        instance.models = discovered.value();
        const auto persisted = persist_ollama_instances(instances);
        if (!persisted) {
          return persisted;
        }
        replace_instance_catalog(instance.id);
        ui.set_autocomplete_provider(build_autocomplete_provider());
        return qt::foundation::Status::success();
      };
      const auto instance_or_error = [&](const std::string& id)
          -> qt::ai::OllamaInstanceSettings* {
        auto* instance = find_instance(id);
        if (instance == nullptr) {
          response.transcript_lines = {
              "unknown Ollama instance: " + id,
              "use /ollama instances or /ollama add <id> <url> [name]"};
        }
        return instance;
      };

      const auto args = split_first_ascii_argument(event.arguments);
      const std::string subcommand = lowercase_ascii(args.first);
      if (args.first.empty()) {
        std::vector<qt::tui::SelectItem> instance_items;
        instance_items.push_back({"__add__", "Add instance", "Configure a new Ollama server"});
        for (const auto& instance : instances) {
          instance_items.push_back(
              {instance.id,
               (instance.name.empty() ? instance.id : instance.name) +
                   (instance.enabled ? "" : " (disabled)"),
               instance.base_url + " · " + std::to_string(instance.models.size()) +
                   " cached models"});
        }
        auto selector = std::make_shared<qt::tui::Select>(
            std::optional<std::string>{"Ollama instances"},
            std::move(instance_items), 0U, 12U, kReferenceInteractiveSelectLayout);
        selector->set_select_callback([&ui](const qt::tui::SelectItem& item) {
          ui.hide_overlay();
          if (item.value == "__add__") {
            ui.mutable_editor().set_text("/ollama add ");
            return;
          }
          std::vector<qt::tui::SelectItem> actions = {
              {"status", "Status", "Query version and connectivity"},
              {"models", "Models / refresh", "Refresh models and metadata"},
              {"select", "Select model", "Open /model for this instance"},
              {"running", "Running models", "Query loaded models and memory usage"},
              {"info", "Model info", "Query /api/show"},
              {"pull", "Pull model", "Download a model to this instance"},
              {"load", "Load model", "Load a model into memory"},
              {"unload", "Unload model", "Unload a model from memory"},
              {"configure", "Settings", "Configure this instance"},
              {"delete", "Delete model", "Delete a model from this instance"},
              {"remove", "Remove instance", "Remove this saved instance"},
          };
          auto action_selector = std::make_shared<qt::tui::Select>(
              std::optional<std::string>{"Ollama · " + item.value},
              std::move(actions), 0U, 12U, kReferenceInteractiveSelectLayout);
          action_selector->set_select_callback(
              [&ui, instance_id = item.value](const qt::tui::SelectItem& action) {
                ui.hide_overlay();
                if (action.value == "select") {
                  ui.mutable_editor().set_text(
                      "/model " + qt::ai::ollama_provider_id(instance_id) + "/");
                  return;
                }
                if (action.value != "configure") {
                  ui.mutable_editor().set_text(
                      "/ollama " + action.value + " " + instance_id + " ");
                  return;
                }
                std::vector<qt::tui::SelectItem> settings = {
                    {"url", "Base URL", "HTTP or HTTPS Ollama API root"},
                    {"name", "Display name", "Friendly instance label"},
                    {"enabled", "Enabled", "Enable or disable model registration"},
                    {"timeout", "Request timeout", "Timeout in milliseconds"},
                    {"key", "API key", "Optional bearer credential"},
                };
                auto settings_selector = std::make_shared<qt::tui::Select>(
                    std::optional<std::string>{"Ollama settings · " + instance_id},
                    std::move(settings), 0U, 8U, kReferenceInteractiveSelectLayout);
                settings_selector->set_select_callback(
                    [&ui, instance_id](const qt::tui::SelectItem& setting) {
                      ui.hide_overlay();
                      ui.mutable_editor().set_text("/ollama configure " + instance_id +
                                                   " " + setting.value + " ");
                    });
                settings_selector->set_cancel_callback([&ui]() { ui.hide_overlay(); });
                ui.show_overlay(settings_selector);
              });
          action_selector->set_cancel_callback([&ui]() { ui.hide_overlay(); });
          ui.show_overlay(action_selector);
        });
        selector->set_cancel_callback([&ui]() { ui.hide_overlay(); });
        ui.show_overlay(selector);
        return response;
      }

      if (subcommand == "instances" || subcommand == "list") {
        response.transcript_lines.push_back("Ollama instances:");
        if (instances.empty()) {
          response.transcript_lines.push_back("- none configured");
        }
        for (const auto& instance : instances) {
          response.transcript_lines.push_back(
              "- " + instance.id + " (" +
              (instance.name.empty() ? instance.id : instance.name) + "): " +
              instance.base_url + ", " + (instance.enabled ? "enabled" : "disabled") +
              ", " + std::to_string(instance.models.size()) + " cached models");
        }
        return response;
      }

      if (subcommand == "add") {
        const auto id_and_rest = split_first_ascii_argument(args.second);
        const auto url_and_name = split_first_ascii_argument(id_and_rest.second);
        if (id_and_rest.first.empty() || url_and_name.first.empty()) {
          response.transcript_lines = {"usage: /ollama add <id> <url> [name]"};
          return response;
        }
        if (!std::all_of(id_and_rest.first.begin(), id_and_rest.first.end(),
                         [](unsigned char ch) {
                           return std::isalnum(ch) != 0 || ch == '-' || ch == '_';
                         })) {
          response.transcript_lines = {
              "Ollama instance id must contain only letters, numbers, '-' or '_'"};
          return response;
        }
        if (find_instance(id_and_rest.first) != nullptr) {
          response.transcript_lines = {"Ollama instance already exists: " + id_and_rest.first};
          return response;
        }
        qt::ai::OllamaInstanceSettings instance;
        instance.id = id_and_rest.first;
        instance.name = url_and_name.second.empty() ? instance.id : url_and_name.second;
        instance.base_url = qt::ai::normalize_ollama_base_url(url_and_name.first);
        if (instance.base_url.rfind("http://", 0U) != 0U &&
            instance.base_url.rfind("https://", 0U) != 0U) {
          response.transcript_lines = {"Ollama URL must start with http:// or https://"};
          return response;
        }
        instances.push_back(std::move(instance));
        auto& added = instances.back();
        const auto refreshed = refresh_instance(added);
        if (!refreshed) {
          const auto persisted = persist_ollama_instances(instances);
          response.transcript_lines = {
              "Configured Ollama instance " + added.id + " at " + added.base_url,
              "Warning: initial model query failed: " + refreshed.error().message};
          if (!persisted) {
            response.transcript_lines.push_back("Warning: " + persisted.error().message);
          }
        } else {
          response.transcript_lines = {
              "Configured Ollama instance " + added.id + " at " + added.base_url,
              "Discovered " + std::to_string(added.models.size()) + " models"};
        }
        return response;
      }

      if (subcommand == "configure") {
        const auto id_and_rest = split_first_ascii_argument(args.second);
        const auto field_and_value = split_first_ascii_argument(id_and_rest.second);
        auto* instance = instance_or_error(id_and_rest.first);
        if (instance == nullptr) {
          return response;
        }
        if (field_and_value.first.empty() || field_and_value.second.empty()) {
          response.transcript_lines = {
              "usage: /ollama configure <id> <url|name|enabled|timeout|key> <value>"};
          return response;
        }
        const std::string field = lowercase_ascii(field_and_value.first);
        if (field == "url") {
          const auto url = qt::ai::normalize_ollama_base_url(field_and_value.second);
          if (url.rfind("http://", 0U) != 0U && url.rfind("https://", 0U) != 0U) {
            response.transcript_lines = {"Ollama URL must start with http:// or https://"};
            return response;
          }
          instance->base_url = url;
          instance->models.clear();
        } else if (field == "name") {
          instance->name = field_and_value.second;
        } else if (field == "enabled") {
          const std::string value = lowercase_ascii(field_and_value.second);
          if (value != "true" && value != "false" && value != "on" && value != "off") {
            response.transcript_lines = {"enabled value must be true/false or on/off"};
            return response;
          }
          instance->enabled = value == "true" || value == "on";
        } else if (field == "timeout") {
          try {
            instance->timeout_ms = std::max<std::int64_t>(1, std::stoll(field_and_value.second));
          } catch (const std::exception&) {
            response.transcript_lines = {"timeout must be an integer number of milliseconds"};
            return response;
          }
        } else if (field == "key") {
          instance->api_key = field_and_value.second == "-" ? std::nullopt
                                                             : std::optional<std::string>{field_and_value.second};
        } else {
          response.transcript_lines = {"unknown Ollama setting: " + field};
          return response;
        }
        const std::string changed_id = instance->id;
        const auto persisted = persist_ollama_instances(instances);
        if (!persisted) {
          response.transcript_lines = {"Failed to save Ollama settings: " + persisted.error().message};
          return response;
        }
        replace_instance_catalog(changed_id);
        response.transcript_lines = {"Updated Ollama instance " + changed_id + " setting " + field};
        return response;
      }

      if (subcommand == "remove") {
        const auto id_and_confirm = split_first_ascii_argument(args.second);
        if (id_and_confirm.second != "--yes") {
          response.transcript_lines = {
              "Removing an instance only removes qt configuration; server data is unchanged.",
              "confirm with: /ollama remove " + id_and_confirm.first + " --yes"};
          return response;
        }
        auto* configured = find_instance(id_and_confirm.first);
        if (configured == nullptr) {
          response.transcript_lines = {"unknown Ollama instance: " + id_and_confirm.first};
          return response;
        }
        const std::string canonical_id = configured->id;
        const auto before = instances.size();
        instances.erase(std::remove_if(instances.begin(), instances.end(),
                                       [&](const auto& value) {
                                         return value.id == canonical_id;
                                       }),
                        instances.end());
        if (instances.size() == before) {
          response.transcript_lines = {"unknown Ollama instance: " + id_and_confirm.first};
          return response;
        }
        const auto persisted = persist_ollama_instances(instances);
        replace_instance_catalog(canonical_id);
        response.transcript_lines = persisted
            ? std::vector<std::string>{"Removed Ollama instance " + canonical_id}
            : std::vector<std::string>{"Failed to save Ollama settings: " + persisted.error().message};
        return response;
      }

      auto id_and_rest = split_first_ascii_argument(args.second);
      if (id_and_rest.first.empty() && !instances.empty()) {
        id_and_rest.first = instances.front().id;
      } else if (subcommand == "info" && find_instance(id_and_rest.first) == nullptr &&
                 instances.size() == 1U) {
        id_and_rest.second = args.second;
        id_and_rest.first = instances.front().id;
      }
      if ((subcommand == "status" || subcommand == "models" ||
           subcommand == "query") && lowercase_ascii(id_and_rest.first) == "all") {
        for (auto& configured : instances) {
          if (subcommand == "status") {
            const auto version = qt::ai::ollama_request(
                configured, "GET", "/api/version");
            response.transcript_lines.push_back(
                configured.id + ": " +
                (version ? "online · " +
                               version.value().value("version", std::string("unknown"))
                         : "unavailable · " + version.error().message));
          } else {
            const auto refreshed = refresh_instance(configured);
            response.transcript_lines.push_back(
                configured.id + ": " +
                (refreshed ? std::to_string(configured.models.size()) + " models"
                           : "failed · " + refreshed.error().message));
          }
        }
        if (instances.empty()) {
          response.transcript_lines = {"no Ollama instances configured"};
        }
        return response;
      }
      auto* instance = instance_or_error(id_and_rest.first);
      if (instance == nullptr) {
        return response;
      }
      if (subcommand == "status") {
        const auto version = qt::ai::ollama_request(*instance, "GET", "/api/version");
        response.transcript_lines = version
            ? std::vector<std::string>{
                  "Ollama " + instance->id + ": online",
                  "url: " + instance->base_url,
                  "version: " + version.value().value("version", std::string("unknown")),
                  "cached models: " + std::to_string(instance->models.size())}
            : std::vector<std::string>{"Ollama " + instance->id + ": unavailable",
                                       version.error().message};
        return response;
      }
      if (subcommand == "models" || subcommand == "query") {
        const auto refreshed = refresh_instance(*instance);
        if (!refreshed) {
          response.transcript_lines = {refreshed.error().message};
          return response;
        }
        response.transcript_lines = {
            "Ollama " + instance->id + " models (select with /model " +
                qt::ai::ollama_provider_id(instance->id) + "/<model>):"};
        for (const auto& model : instance->models) {
          response.transcript_lines.push_back(
              "- " + model.name + " · context " +
              std::to_string(model.context_window) + " · max output " +
              std::to_string(model.max_tokens));
        }
        if (instance->models.empty()) {
          response.transcript_lines.push_back("- no models installed");
        }
        return response;
      }
      if (subcommand == "running") {
        const auto running = qt::ai::ollama_request(*instance, "GET", "/api/ps");
        response.transcript_lines = running
            ? std::vector<std::string>{"Ollama " + instance->id + " running models:",
                                       pretty_transcript_json(running.value())}
            : std::vector<std::string>{running.error().message};
        return response;
      }
      if (subcommand == "info") {
        if (id_and_rest.second.empty()) {
          response.transcript_lines = {"usage: /ollama info <instance> <model>"};
          return response;
        }
        const auto info = qt::ai::ollama_request(
            *instance, "POST", "/api/show",
            Json{{"model", id_and_rest.second}, {"verbose", false}});
        response.transcript_lines = info
            ? std::vector<std::string>{pretty_transcript_json(info.value())}
            : std::vector<std::string>{info.error().message};
        return response;
      }
      if (subcommand == "pull") {
        if (id_and_rest.second.empty()) {
          response.transcript_lines = {"usage: /ollama pull <instance> <model>"};
          return response;
        }
        const auto pulled = qt::ai::ollama_request(
            *instance, "POST", "/api/pull",
            Json{{"model", id_and_rest.second}, {"stream", false}});
        if (!pulled) {
          response.transcript_lines = {pulled.error().message};
          return response;
        }
        const auto refreshed = refresh_instance(*instance);
        response.transcript_lines = {
            "Pulled " + id_and_rest.second + " on " + instance->id,
            refreshed ? "Model catalog refreshed"
                      : "Warning: catalog refresh failed: " + refreshed.error().message};
        return response;
      }
      if (subcommand == "load" || subcommand == "unload") {
        if (id_and_rest.second.empty()) {
          response.transcript_lines = {"usage: /ollama " + subcommand +
                                       " <instance> <model>"};
          return response;
        }
        Json body{{"model", id_and_rest.second}, {"messages", Json::array()},
                  {"stream", false}};
        body["keep_alive"] = subcommand == "unload" ? Json(0) : Json("5m");
        const auto changed = qt::ai::ollama_request(*instance, "POST", "/api/chat", body);
        response.transcript_lines = changed
            ? std::vector<std::string>{
                  (subcommand == "load" ? "Loaded " : "Unloaded ") +
                  id_and_rest.second + " on " + instance->id}
            : std::vector<std::string>{changed.error().message};
        return response;
      }
      if (subcommand == "delete") {
        const auto model_and_confirm = split_first_ascii_argument(id_and_rest.second);
        if (model_and_confirm.first.empty() || model_and_confirm.second != "--yes") {
          response.transcript_lines = {
              "Deleting a model removes it from the Ollama server.",
              "confirm with: /ollama delete " + instance->id + " <model> --yes"};
          return response;
        }
        const auto removed = qt::ai::ollama_request(
            *instance, "DELETE", "/api/delete", Json{{"model", model_and_confirm.first}});
        if (!removed) {
          response.transcript_lines = {removed.error().message};
          return response;
        }
        const auto refreshed = refresh_instance(*instance);
        response.transcript_lines = {"Deleted " + model_and_confirm.first + " from " + instance->id};
        if (!refreshed) {
          response.transcript_lines.push_back("Warning: " + refreshed.error().message);
        }
        return response;
      }

      response.transcript_lines = {
          "unknown Ollama command: " + args.first,
          "usage: /ollama <instances|add|configure|remove|status|models|running|info|pull|load|unload|delete>"};
      return response;
    }

    if (event.command == "subagent") {
      response.handled = true;
      response.append_default_output = false;

      const bool tool_available = runtime->services.tool_registry &&
                                  runtime->services.tool_registry->find("subagent");
      if (!tool_available) {
        response.transcript_lines = {
            "The subagent tool is disabled for this runtime.",
            "Enable built-in tools or include subagent in the --tools allowlist."};
        return response;
      }

      const auto args = split_first_ascii_argument(event.arguments);
      const std::string subcommand = lowercase_ascii(args.first);
      if (subcommand == "spawn" || subcommand == "workers" ||
          subcommand == "send" || subcommand == "follow_up" ||
          subcommand == "interrupt" || subcommand == "wait" || subcommand == "call") {
        Json arguments;
        const auto target = split_first_ascii_argument(args.second);
        try {
          if (subcommand == "call") {
            arguments = Json::parse(args.second);
            if (!arguments.is_object()) throw std::invalid_argument("arguments must be an object");
          } else if (subcommand == "workers") {
            arguments = Json{{"action", "list_workers"}};
          } else if (subcommand == "spawn") {
            arguments = Json{{"action", "spawn"}, {"agent", target.first}, {"task", target.second}};
          } else {
            arguments = Json{{"action", subcommand}, {"target", target.first}};
            if (subcommand == "send") arguments["message"] = target.second;
            if (subcommand == "follow_up") arguments["task"] = target.second;
            if (subcommand == "wait") arguments["timeout_ms"] = 1000;
          }
        } catch (const std::exception& error) {
          response.transcript_lines = {std::string("invalid subagent request: ") + error.what()};
          return response;
        }
        qt::tools::ToolRequest tool;
        tool.tool_name = "subagent";
        tool.arguments = std::move(arguments);
        const auto started = start_async_tool_call(
            "/subagent " + event.arguments, std::move(tool), "Managing subagent");
        if (!started) return started.error();
        if (!started.value()) response.transcript_lines = {"Another turn is already running."};
        return response;
      }
      const auto discover = [&](const std::string& scope) {
        return qt::tools::discover_subagents(qt::tools::SubagentDiscoveryOptions{
            runtime->services.settings.agent_home,
            runtime->services.settings.cwd,
            scope});
      };
      const auto definition_lines = [](const qt::tools::SubagentDefinition& agent) {
        std::vector<std::string> lines = {
            "Subagent: " + agent.name,
            "  source: " + std::string(qt::tools::to_string(agent.source)),
            "  description: " + agent.description,
            "  model: " + agent.model.value_or("inherit parent selection"),
            "  thinking: " + agent.thinking.value_or("inherit parent selection"),
            "  project context: " +
                std::string(agent.inherit_project_context ? "inherit" : "isolated"),
            "  skills: " + std::string(agent.inherit_skills ? "inherit" : "isolated"),
            "  max child depth: " +
                (agent.max_subagent_depth
                     ? std::to_string(*agent.max_subagent_depth)
                     : std::string("runtime default")),
        };
        std::string tools = "  tools: ";
        if (agent.tools.empty()) {
          tools += "none";
        } else {
          for (std::size_t index = 0U; index < agent.tools.size(); ++index) {
            if (index != 0U) {
              tools += ", ";
            }
            tools += agent.tools[index];
          }
        }
        lines.push_back(std::move(tools));
        if (!agent.file_path.empty()) {
          lines.push_back("  definition: " + agent.file_path.generic_string());
        }
        return lines;
      };
      const auto discovery_lines = [&](const std::string& scope) {
        const auto discovered = discover(scope);
        std::vector<std::string> lines = {
            "Subagents (scope: " + scope + "):"};
        if (discovered.agents.empty()) {
          lines.push_back("  No executable subagents discovered.");
        }
        for (const auto& agent : discovered.agents) {
          lines.push_back("  " + agent.name + " [" +
                          qt::tools::to_string(agent.source) + "] - " +
                          agent.description);
        }
        if (discovered.project_agents_directory) {
          lines.push_back("  project definitions: " +
                          discovered.project_agents_directory->generic_string());
        }
        for (const auto& diagnostic : discovered.diagnostics) {
          lines.push_back("  warning: " + diagnostic);
        }
        if (scope == "project" || scope == "both") {
          lines.push_back(
              "Project roles are repository-controlled and require explicit trust before execution.");
        }
        return lines;
      };
      const auto help_lines = [&]() {
        return std::vector<std::string>{
            "Subagent menu:",
            "  /subagent                  Open the role and workflow menu",
            "  /subagent list [scope]     List user, project, or both scopes",
            "  /subagent help             Show this guidance",
            "",
            "User roles: " +
                (std::filesystem::path(runtime->services.settings.agent_home) /
                 "agents")
                    .generic_string(),
            "Project roles: nearest ancestor .agents/ and .pi/agents/ directories",
            "Selecting Use this agent asks for a task and launches one tracked subagent tool call without a provider request.",
            "  /subagent spawn <role> <task>       Start a persistent worker",
            "  /subagent workers                   List worker identities and state",
            "  /subagent send <path> <message>      Queue a message without waking an idle worker",
            "  /subagent follow_up <path> <task>    Reuse or wake a worker",
            "  /subagent interrupt <path>           Interrupt the current child turn",
            "  /subagent wait <path>                Wait up to one second",
            "  /subagent call <JSON>                Full tool arguments, including name and fork"};
      };

      if (subcommand == "list") {
        const std::string scope = args.second.empty()
                                      ? std::string("user")
                                      : lowercase_ascii(args.second);
        if (scope != "user" && scope != "project" && scope != "both") {
          response.transcript_lines = {
              "usage: /subagent list [user|project|both]"};
          return response;
        }
        response.transcript_lines = discovery_lines(scope);
        return response;
      }
      if (subcommand == "help") {
        response.transcript_lines = help_lines();
        return response;
      }
      if (!subcommand.empty()) {
        response.transcript_lines = {
            "unknown subagent command: " + args.first,
            "usage: /subagent [list [user|project|both]|help]"};
        return response;
      }

      const auto user_discovery = discover("user");
      const auto project_discovery = discover("project");
      const std::size_t project_role_count = std::count_if(
          project_discovery.agents.begin(), project_discovery.agents.end(),
          [](const auto& agent) {
            return agent.source == qt::tools::SubagentSource::project;
          });
      std::vector<qt::tui::SelectItem> items;
      items.reserve(user_discovery.agents.size() + 4U);
      for (const auto& agent : user_discovery.agents) {
        std::string description = "[";
        description += qt::tools::to_string(agent.source);
        description += "] " + agent.description;
        items.push_back({"agent:" + agent.name, agent.name,
                         std::move(description)});
      }
      items.push_back({"__parallel__", "Parallel delegation",
                       "Prepare an ordered, bounded parallel subagent request"});
      items.push_back({"__chain__", "Sequential chain",
                       "Prepare a chain whose steps can consume {previous}"});
      items.push_back({"__project__", "Inspect project roles",
                       std::to_string(project_role_count) +
                           " repository-controlled roles; execution requires trust"});
      items.push_back({"__help__", "Help and role locations",
                       "Show discovery paths and supported behavior"});

      auto select = std::make_shared<qt::tui::Select>(
          std::optional<std::string>{"Subagents · user + builtins"},
          std::move(items), 0U, 14U, kReferenceInteractiveSelectLayout);
      select->set_select_callback(
          [&ui, agents = user_discovery.agents,
           project_lines = discovery_lines("project"),
           guidance = help_lines(), definition_lines,
           &start_async_tool_call](
              const qt::tui::SelectItem& item) {
            if (item.value == "__parallel__") {
              ui.hide_overlay();
              ui.mutable_editor().set_text(
                  "Use the subagent tool in parallel mode for these tasks: ");
              return;
            }
            if (item.value == "__chain__") {
              ui.hide_overlay();
              ui.mutable_editor().set_text(
                  "Use the subagent tool as a sequential chain for this workflow: ");
              return;
            }
            if (item.value == "__project__" || item.value == "__help__") {
              ui.hide_overlay();
              const auto& lines =
                  item.value == "__project__" ? project_lines : guidance;
              for (const auto& line : lines) {
                ui.append_transcript_line(line);
              }
              return;
            }

            const std::string name = item.value.rfind("agent:", 0U) == 0U
                                         ? item.value.substr(6U)
                                         : std::string{};
            const auto found = std::find_if(
                agents.begin(), agents.end(), [&](const auto& agent) {
                  return agent.name == name;
                });
            if (found == agents.end()) {
              ui.hide_overlay();
              ui.append_transcript_line("Subagent is no longer available: " + name);
              return;
            }
            const auto agent = *found;
            ui.hide_overlay();
            std::vector<qt::tui::SelectItem> actions = {
                {"use", "Use this agent",
                 "Enter a task and launch a tracked subagent tool call"},
                {"details", "View details",
                 "Show source, model, tools, inheritance, and definition path"},
            };
            auto action_selector = std::make_shared<qt::tui::Select>(
                std::optional<std::string>{"Subagent · " + agent.name},
                std::move(actions), 0U, 8U, kReferenceInteractiveSelectLayout);
            action_selector->set_select_callback(
                [&ui, agent, definition_lines,
                 &start_async_tool_call](const qt::tui::SelectItem& action) {
                  ui.hide_overlay();
                  if (action.value == "use") {
                    auto task_prompt = std::make_shared<PromptOverlay>(
                        "Delegate · " + agent.name, "Task: ",
                        "Enter launches a tracked tool call · Esc cancels");
                    task_prompt->set_submit_callback(
                        [&ui, agent, &start_async_tool_call](
                            const std::string& value) {
                          const std::string task = trim_ascii_copy(value);
                          if (task.empty()) {
                            return;
                          }
                          ui.hide_overlay();
                          qt::tools::ToolRequest request;
                          request.tool_name = "subagent";
                          request.arguments = qt::tools::Json{
                              {"agent", agent.name}, {"task", task}};
                          const auto started = start_async_tool_call(
                              "Delegate to subagent \"" + agent.name +
                                  "\": " + task,
                              std::move(request),
                              "Running subagent " + agent.name);
                          if (!started) {
                            ui.append_transcript_line(
                                "error: " + started.error().message);
                          } else if (!started.value()) {
                            ui.append_transcript_line(
                                "error: another turn is already running");
                          }
                        });
                    task_prompt->set_cancel_callback(
                        [&ui]() { ui.hide_overlay(); });
                    ui.show_overlay(task_prompt);
                    return;
                  }
                  for (const auto& line : definition_lines(agent)) {
                    ui.append_transcript_line(line);
                  }
                });
            action_selector->set_cancel_callback([&ui]() { ui.hide_overlay(); });
            ui.show_overlay(action_selector);
          });
      select->set_cancel_callback([&ui]() { ui.hide_overlay(); });
      ui.show_overlay(select);
      return response;
    }

    if (event.command == "mcp") {
      response.handled = true;
      response.append_default_output = false;

      const auto mcp_args = split_first_ascii_argument(event.arguments);
      const std::string subcommand =
          mcp_args.first.empty() ? std::string{} : lowercase_ascii(mcp_args.first);
      const auto& servers = runtime->services.settings.mcp_servers;
      const auto server_tool_count = [&](const std::string& server_name) {
        std::size_t count = 0U;
        const std::string description_prefix = "MCP " + server_name + "/";
        for (const auto& tool : runtime->services.tool_definitions) {
          if (tool.name.rfind("mcp_", 0) == 0 &&
              tool.description.rfind(description_prefix, 0) == 0) {
            ++count;
          }
        }
        return count;
      };
      const auto matching_server = [&](const std::string& name)
          -> std::optional<qt::app::McpServerSettings> {
        for (const auto& server : servers) {
          if (server.name == name) {
            return server;
          }
        }
        return std::nullopt;
      };
      const auto config_path_lines = [&]() {
        std::vector<std::string> lines = {
            "MCP config paths:",
            "1. ~/.config/mcp/mcp.json",
            "2. <qt state dir>/mcp.json",
            "3. <workspace>/.mcp.json",
            "4. <workspace>/.pi/mcp.json",
            "",
            "Starter project .mcp.json:",
            "{",
            "  \"mcpServers\": {}",
            "}",
        };
        return lines;
      };
      const auto status_lines = [&]() {
        std::vector<std::string> lines = {"MCP Server Status:", ""};
        if (servers.empty()) {
          lines.push_back("No MCP servers configured");
          lines.push_back("Run /mcp setup to see config paths and a starter .mcp.json.");
          return lines;
        }
        for (const auto& server : servers) {
          std::string line = server.enabled ? "[on] " : "[off] ";
          line += server.name;
          line += ": ";
          line += server.enabled ? "configured" : "disabled";
          line += " (";
          line += server.transport;
          line += ", ";
          line += std::to_string(server_tool_count(server.name));
          line += " tools";
          line += ")";
          if (server.url) {
            line += " ";
            line += *server.url;
          } else if (server.command) {
            line += " ";
            line += *server.command;
          }
          lines.push_back(std::move(line));
        }
        return lines;
      };
      const auto tool_lines = [&]() {
        std::vector<std::string> lines = {"MCP Tools:", ""};
        for (const auto& tool : runtime->services.tool_definitions) {
          if (tool.name.rfind("mcp_", 0) != 0) {
            continue;
          }
          lines.push_back("  " + tool.name + " - " + tool.description);
        }
        if (lines.size() == 2U) {
          lines.push_back("No MCP tools are currently registered.");
        } else {
          lines.push_back("");
          lines.push_back("Total: " + std::to_string(lines.size() - 3U) +
                          " tools");
        }
        return lines;
      };
      const auto reconnect = [&](const std::optional<std::string>& target) {
        if (!runtime->services.mcp_client_manager) {
          response.transcript_lines = {
              "MCP is not initialized.",
              "Run /mcp setup to configure MCP servers.",
          };
          return;
        }
        if (target && !matching_server(*target)) {
          response.transcript_lines = {"MCP server not found: " + *target};
          return;
        }

        std::vector<std::string> diagnostics;
        qt::tools::register_mcp_tools(
            *runtime->services.tool_registry,
            runtime->services.mcp_client_manager, &diagnostics);
        runtime->services.tool_definitions =
            qt::app::build_tool_definitions(*runtime->services.tool_registry);
        const std::size_t mcp_tool_total =
            std::count_if(runtime->services.tool_definitions.begin(),
                          runtime->services.tool_definitions.end(),
                          [](const qt::ai::ToolDefinition& tool) {
                            return tool.name.rfind("mcp_", 0) == 0;
                          });

        response.transcript_lines = {
            target ? "MCP reconnect: " + *target : "MCP reconnect: all servers",
            "registered_mcp_tools: " + std::to_string(mcp_tool_total),
        };
        if (!diagnostics.empty()) {
          response.transcript_lines.push_back("diagnostics:");
          for (const auto& diagnostic : diagnostics) {
            response.transcript_lines.push_back("- " + diagnostic);
          }
        }
      };

      if (mcp_args.first.empty()) {
        std::vector<qt::tui::SelectItem> items = {
            {"status", "status", "Show configured MCP server status"},
            {"tools", "tools", "List registered MCP tools"},
            {"resources", "resources", "List MCP resources through the resource tool"},
            {"reconnect", "reconnect", "Reconnect all MCP servers and refresh registered tools"},
            {"setup", "setup", "Show config precedence and a starter .mcp.json"},
        };
        auto select = std::make_shared<qt::tui::Select>(
            std::optional<std::string>{"mcp"}, std::move(items), 0U, 10U,
            kReferenceInteractiveSelectLayout);
        select->set_select_callback([&ui](const qt::tui::SelectItem& item) {
          ui.hide_overlay();
          ui.mutable_editor().set_text("/mcp " + item.value + " ");
        });
        select->set_cancel_callback([&ui]() { ui.hide_overlay(); });
        ui.show_overlay(select);
        return response;
      }

      if (subcommand == "status") {
        response.transcript_lines = status_lines();
        return response;
      }

      if (subcommand == "tools") {
        response.transcript_lines = tool_lines();
        return response;
      }

      if (subcommand == "resources") {
        qt::tools::ToolRequest request;
        request.tool_name = "list_mcp_resources";
        request.arguments = Json::object();
        if (!mcp_args.second.empty()) {
          request.arguments["server"] = mcp_args.second;
        }
        auto tool_context = runtime->services.tool_context;
        const auto result = invoke_interactive_tool_request(
            request, runtime->services.tool_registry, tool_context);
        response.transcript_lines = index_tool_result_lines("MCP resources", result);
        return response;
      }

      if (subcommand == "reconnect") {
        reconnect(mcp_args.second.empty()
                      ? std::nullopt
                      : std::optional<std::string>{mcp_args.second});
        return response;
      }

      if (subcommand == "setup") {
        response.transcript_lines = config_path_lines();
        return response;
      }

      response.transcript_lines = {
          "unknown mcp command: " + mcp_args.first,
          "usage: /mcp",
          "usage: /mcp status",
          "usage: /mcp tools",
          "usage: /mcp resources [server]",
          "usage: /mcp reconnect [server]",
          "usage: /mcp setup",
      };
      return response;
    }

    if (event.command == "memory") {
      response.handled = true;
      response.append_default_output = false;

      const auto memory_args = split_first_ascii_argument(event.arguments);
      const std::string subcommand = memory_args.first.empty()
                                         ? std::string{}
                                         : lowercase_ascii(memory_args.first);
      const auto show_memory_settings =
          [runtime, &ui, &persist_memory_settings]() {
        const auto boolean_text = [](bool value) {
          return value ? std::string("on") : std::string("off");
        };
        const auto setting_value =
            [boolean_text](const MemorySettings& settings,
                           const std::string& id) -> std::string {
          if (id == "enabled") return boolean_text(settings.enabled);
          if (id == "automatic_recall") {
            return boolean_text(settings.automatic_recall);
          }
          if (id == "automatic_capture") {
            return boolean_text(settings.automatic_capture);
          }
          if (id == "recall_policy") return settings.recall_policy;
          if (id == "context_token_budget") {
            return std::to_string(settings.context_token_budget);
          }
          if (id == "candidate_limit") {
            return std::to_string(settings.candidate_limit);
          }
          if (id == "prehandler_model") {
            return settings.prehandler.model.value_or("default");
          }
          if (id == "prehandler_thinking") {
            return settings.prehandler.thinking;
          }
          if (id == "prehandler_timeout_ms") {
            return std::to_string(settings.prehandler.timeout_ms);
          }
          if (id == "prehandler_fail_open") {
            return boolean_text(settings.prehandler.fail_open);
          }
          if (id == "prehandler_max_model_input_tokens") {
            return std::to_string(
                settings.prehandler.max_model_input_tokens);
          }
          if (id == "capture_post_process") {
            return settings.capture.post_process;
          }
          if (id == "capture_default_status") {
            return settings.capture.default_status;
          }
          if (id == "capture_max_evidence_chars") {
            return std::to_string(settings.capture.max_evidence_chars);
          }
          if (id == "dreaming_enabled") {
            return boolean_text(settings.dreaming.enabled);
          }
          if (id == "dreaming_trigger") {
            return settings.dreaming.trigger;
          }
          if (id == "dreaming_interval_seconds") {
            return std::to_string(settings.dreaming.interval_seconds);
          }
          if (id == "dreaming_idle_seconds") {
            return std::to_string(settings.dreaming.idle_seconds);
          }
          if (id == "dreaming_candidate_threshold") {
            return std::to_string(settings.dreaming.candidate_threshold);
          }
          if (id == "dreaming_model") {
            return settings.dreaming.model.value_or("auto");
          }
          if (id == "dreaming_verifier_model") {
            return settings.dreaming.verifier_model.value_or("unset");
          }
          if (id == "dreaming_timeout_ms") {
            return std::to_string(settings.dreaming.timeout_ms);
          }
          if (id == "dreaming_max_model_input_tokens") {
            return std::to_string(
                settings.dreaming.max_model_input_tokens);
          }
          if (id == "dreaming_max_candidates") {
            return std::to_string(settings.dreaming.max_candidates);
          }
          if (id == "dreaming_research_policy") {
            return settings.dreaming.research_policy;
          }
          if (id == "dreaming_promotion_policy") {
            return settings.dreaming.promotion_policy;
          }
          return {};
        };

        const auto positive_integer_submenu = [&ui](std::uint64_t maximum) {
          return [&ui, maximum](
                     const std::string& current,
                     qt::tui::SettingItem::SubmenuDone done)
                     -> qt::tui::ComponentPtr {
            auto input = std::make_shared<qt::tui::Input>();
            input->set_value(current);
            input->handle_input("\x05");
            input->set_submit_callback(
                [&ui, done, maximum](const std::string& submitted) mutable {
                  const std::string value = trim_ascii_copy(submitted);
                  const bool decimal =
                      !value.empty() &&
                      std::all_of(value.begin(), value.end(), [](char ch) {
                        return ch >= '0' && ch <= '9';
                      });
                  try {
                    const auto parsed = decimal ? std::stoull(value) : 0U;
                    if (parsed == 0U || parsed > maximum) {
                      throw std::invalid_argument("not positive");
                    }
                  } catch (const std::exception&) {
                    ui.append_transcript_line(
                        "error: memory setting must be a whole number between "
                        "1 and " +
                        std::to_string(maximum));
                    return;
                  }
                  done(value);
                });
            input->set_escape_callback(
                [done]() mutable { done(std::nullopt); });
            return input;
          };
        };
        const auto numeric_submenu = positive_integer_submenu(
            std::numeric_limits<std::uint32_t>::max());
        const auto timeout_submenu = positive_integer_submenu(
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()));
        const auto model_submenu = [runtime](std::string title,
                                            std::string fallback,
                                            std::string fallback_description,
                                            bool local_only) {
          return [runtime, title = std::move(title), fallback = std::move(fallback),
                  fallback_description = std::move(fallback_description), local_only](
                     const std::string& current,
                     qt::tui::SettingItem::SubmenuDone done) -> qt::tui::ComponentPtr {
            std::vector<qt::tui::SelectItem> choices{
                {fallback, fallback, fallback_description}};
            std::size_t selected = 0;
            for (const auto& model : runtime->services.model_catalog.models) {
              if ((local_only && model.ref.provider != "llama") ||
                  !model_is_available(runtime->services.model_catalog,
                                      runtime->services.auth_storage, model)) {
                continue;
              }
              const std::string id = model.ref.provider + "/" + model.ref.name;
              if (std::any_of(choices.begin(), choices.end(),
                              [&id](const auto& item) { return item.value == id; })) {
                continue;
              }
              if (id == current) selected = choices.size();
              choices.push_back({id, id, model.display_name});
            }
            if (current != fallback && selected == 0 &&
                (!local_only || current.rfind("llama/", 0) == 0)) {
              selected = choices.size();
              choices.push_back({current, current, "Current setting (not available in catalog)"});
            }
            const std::string heading = title +
                (local_only && choices.size() == 1
                     ? " — no local models configured; add one with /llama"
                     : " — type to filter");
            return std::make_shared<MemoryModelPicker>(
                heading, std::move(choices), selected, std::move(done));
          };
        };

        const auto& memory = runtime->services.settings.memory;
        std::vector<qt::tui::SettingItem> items{
            {"enabled", "Enabled",
             "Enable built-in memory on the next qt start",
             boolean_text(memory.enabled), {"off", "on"}, {}},
            {"automatic_recall", "Automatic recall",
             "Recall memory before prompts on the next qt start",
             boolean_text(memory.automatic_recall), {"off", "on"}, {}},
            {"automatic_capture", "Automatic capture",
             "Capture completed episodes on the next qt start",
             boolean_text(memory.automatic_capture), {"off", "on"}, {}},
            {"recall_policy", "Recall policy",
             "Retrieval policy for the isolated pre-handler",
             memory.recall_policy,
             {"strict", "balanced", "exploratory", "debugging",
              "architecture"},
             {}},
            {"context_token_budget", "Context token budget",
             "Maximum recalled tokens injected into a prompt",
             std::to_string(memory.context_token_budget), {}, numeric_submenu},
            {"candidate_limit", "Candidate limit",
             "Maximum memories considered during recall",
             std::to_string(memory.candidate_limit), {}, numeric_submenu},
            {"prehandler_model", "Pre-handler model",
             "Select an available model or child default selection",
             memory.prehandler.model.value_or("default"), {},
             model_submenu("Pre-handler model", "default", "Child default selection", false)},
            {"prehandler_thinking", "Pre-handler thinking",
             "Thinking level for the isolated memory child",
             memory.prehandler.thinking,
             {"off", "minimal", "low", "medium", "high", "xhigh"}, {}},
            {"prehandler_timeout_ms", "Pre-handler timeout",
             "Positive timeout in milliseconds",
             std::to_string(memory.prehandler.timeout_ms), {}, timeout_submenu},
            {"prehandler_fail_open", "Pre-handler fail-open",
             "Continue without memory when recall preprocessing fails",
             boolean_text(memory.prehandler.fail_open), {"off", "on"}, {}},
            {"prehandler_max_model_input_tokens", "Pre-handler input limit",
             "Maximum tokens projected into the isolated child",
             std::to_string(memory.prehandler.max_model_input_tokens), {},
             numeric_submenu},
            {"capture_post_process", "Capture post-process",
             "When reviewable candidates are generated",
             memory.capture.post_process,
             {"turn_end", "session_end", "disabled"}, {}},
            {"capture_default_status", "Capture default status",
             "Governance status assigned to captured memories",
             memory.capture.default_status, {"candidate", "hypothesis"}, {}},
            {"capture_max_evidence_chars", "Capture evidence limit",
             "Maximum evidence characters retained per episode",
             std::to_string(memory.capture.max_evidence_chars), {},
             numeric_submenu},
            {"dreaming_enabled", "Dreaming enabled",
             "Opt in to isolated governed memory exploration on the next start",
             boolean_text(memory.dreaming.enabled), {"off", "on"}, {}},
            {"dreaming_trigger", "Dreaming trigger",
             "Schedule a governed Dreaming cycle after memory capture",
             memory.dreaming.trigger,
             {"post_turn", "interval", "candidate_threshold"}, {}},
            {"dreaming_interval_seconds", "Dreaming interval",
             "Minimum interval in seconds for interval scheduling",
             std::to_string(memory.dreaming.interval_seconds), {},
             positive_integer_submenu(31536000)},
            {"dreaming_idle_seconds", "Dreaming idle duration",
             "Required application idle time before background dreaming",
             std::to_string(memory.dreaming.idle_seconds), {},
             positive_integer_submenu(86400)},
            {"dreaming_candidate_threshold", "Dreaming threshold",
             "Durable candidate count required by threshold scheduling",
             std::to_string(memory.dreaming.candidate_threshold), {},
             positive_integer_submenu(1000)},
            {"dreaming_model", "Dreaming model",
             "Select an available model or automatic task selection",
             memory.dreaming.model.value_or("auto"), {},
             model_submenu("Dreaming model", "auto", "Automatic task selection", false)},
            {"dreaming_verifier_model", "Dreaming verifier model",
             "Select a local llama.cpp model; unset reuses Dreaming only when local",
             memory.dreaming.verifier_model.value_or("unset"), {},
             model_submenu("Dreaming verifier model", "unset",
                           "Reuse Dreaming model only when it is local", true)},
            {"dreaming_timeout_ms", "Dreaming timeout",
             "Isolated worker timeout in milliseconds (maximum 600000)",
             std::to_string(memory.dreaming.timeout_ms), {},
             positive_integer_submenu(600000)},
            {"dreaming_max_model_input_tokens", "Dreaming input limit",
             "Maximum isolated-worker input tokens (maximum 6000)",
             std::to_string(memory.dreaming.max_model_input_tokens), {},
             positive_integer_submenu(6000)},
            {"dreaming_max_candidates", "Dreaming candidate limit",
             "Governed records in one dream cycle (between 2 and 8)",
             std::to_string(memory.dreaming.max_candidates),
             {"2", "3", "4", "5", "6", "7", "8"}, {}},
            {"dreaming_research_policy", "Dreaming research policy",
             "disabled, manual evidence, or required isolated web research",
             memory.dreaming.research_policy,
             {"disabled", "manual", "required"}, {}},
            {"dreaming_promotion_policy", "Dreaming promotion policy",
             "review_only/manual: review approval may promote to Hypothesis; "
             "auto_validate: locally verify researched successors; disabled: no promotion",
             memory.dreaming.promotion_policy,
             {"review_only", "manual", "auto_validate", "disabled"}, {}},
        };

        auto settings = std::make_shared<qt::tui::SettingsList>(
            std::move(items), 12U, qt::tui::SettingsListTheme{},
            qt::tui::SettingsListOptions{true});
        std::weak_ptr<qt::tui::SettingsList> weak_settings = settings;
        auto changed = std::make_shared<bool>(false);
        settings->set_change_callback(
            [runtime, &ui, &persist_memory_settings, setting_value,
             weak_settings, changed](const std::string& id,
                                     const std::string& value) {
              const MemorySettings previous =
                  runtime->services.settings.memory;
              MemorySettings next = previous;
              const bool enabled = value == "on";
              try {
                if (id == "enabled") {
                  next.enabled = enabled;
                } else if (id == "automatic_recall") {
                  next.automatic_recall = enabled;
                } else if (id == "automatic_capture") {
                  next.automatic_capture = enabled;
                } else if (id == "recall_policy") {
                  next.recall_policy = value;
                } else if (id == "context_token_budget") {
                  next.context_token_budget =
                      static_cast<std::size_t>(std::stoull(value));
                } else if (id == "candidate_limit") {
                  next.candidate_limit =
                      static_cast<std::size_t>(std::stoull(value));
                } else if (id == "prehandler_model") {
                  next.prehandler.model =
                      value == "default"
                          ? std::optional<std::string>{}
                          : std::optional<std::string>{value};
                } else if (id == "prehandler_thinking") {
                  next.prehandler.thinking = value;
                } else if (id == "prehandler_timeout_ms") {
                  next.prehandler.timeout_ms = std::stoll(value);
                } else if (id == "prehandler_fail_open") {
                  next.prehandler.fail_open = enabled;
                } else if (id == "prehandler_max_model_input_tokens") {
                  next.prehandler.max_model_input_tokens =
                      static_cast<std::size_t>(std::stoull(value));
                } else if (id == "capture_post_process") {
                  next.capture.post_process = value;
                } else if (id == "capture_default_status") {
                  next.capture.default_status = value;
                } else if (id == "capture_max_evidence_chars") {
                  next.capture.max_evidence_chars =
                      static_cast<std::size_t>(std::stoull(value));
                } else if (id == "dreaming_enabled") {
                  next.dreaming.enabled = enabled;
                } else if (id == "dreaming_trigger") {
                  next.dreaming.trigger = value;
                } else if (id == "dreaming_interval_seconds") {
                  next.dreaming.interval_seconds = std::stoull(value);
                } else if (id == "dreaming_idle_seconds") {
                  next.dreaming.idle_seconds = std::stoull(value);
                } else if (id == "dreaming_candidate_threshold") {
                  next.dreaming.candidate_threshold =
                      static_cast<std::size_t>(std::stoull(value));
                } else if (id == "dreaming_model") {
                  next.dreaming.model =
                      value == "auto"
                          ? std::optional<std::string>{}
                          : std::optional<std::string>{value};
                } else if (id == "dreaming_verifier_model") {
                  if (lowercase_ascii(value) == "auto") {
                    throw std::invalid_argument(
                        "Dreaming verifier model cannot be auto");
                  }
                  next.dreaming.verifier_model =
                      value == "unset"
                          ? std::optional<std::string>{}
                          : std::optional<std::string>{value};
                } else if (id == "dreaming_timeout_ms") {
                  next.dreaming.timeout_ms = std::stoll(value);
                } else if (id == "dreaming_max_model_input_tokens") {
                  next.dreaming.max_model_input_tokens =
                      static_cast<std::size_t>(std::stoull(value));
                } else if (id == "dreaming_max_candidates") {
                  next.dreaming.max_candidates =
                      static_cast<std::size_t>(std::stoull(value));
                } else if (id == "dreaming_research_policy") {
                  next.dreaming.research_policy = value;
                } else if (id == "dreaming_promotion_policy") {
                  next.dreaming.promotion_policy = value;
                } else {
                  return;
                }
              } catch (const std::exception&) {
                if (const auto list = weak_settings.lock()) {
                  list->update_value(id, setting_value(previous, id));
                }
                ui.append_transcript_line(
                    "error: invalid memory setting value");
                return;
              }

              const auto persisted = persist_memory_settings(next);
              if (!persisted) {
                if (const auto list = weak_settings.lock()) {
                  list->update_value(id, setting_value(previous, id));
                }
                ui.append_transcript_line("error: " +
                                          persisted.error().message);
                return;
              }
              runtime->services.settings.memory = std::move(next);
              *changed = true;
            });
        settings->set_cancel_callback([&ui, changed]() {
          ui.hide_overlay();
          if (*changed) {
            ui.append_transcript_line(
                "Memory settings saved; changes apply to the next runtime.");
          }
        });
        ui.show_overlay(settings);
      };

      const auto launch_skill_scan =
          [memory_runtime = runtime->services.memory_runtime,
           containment_root =
               runtime->services.settings.allow_external_filesystem_access
                   ? std::filesystem::path{}
                   : std::filesystem::path(
                         runtime->services.settings.workspace_root),
           post_to_ui](std::filesystem::path folder, bool recursive) {
        if (!memory_runtime) {
          return;
        }
        auto scope = memory_runtime->scope();
        scope.kind = qt::memory::ScopeKind::Repository;
        scope.branch.clear();
        scope.task.clear();
        scope.agent.clear();
        scope.session.clear();
        scope.conversation.clear();
        std::thread(
            [memory_runtime, post_to_ui, folder = std::move(folder), recursive,
             containment_root = std::move(containment_root),
             scope = std::move(scope)]() mutable {
              try {
                qt::memory::SkillImportOptions options;
                options.root = folder;
                options.containment_root = containment_root;
                options.recursive = recursive;
                options.progress =
                    [post_to_ui, folder](const qt::memory::SkillImportItem& item) {
                      std::error_code relative_error;
                      const auto relative = std::filesystem::relative(
                          item.skill_path, folder, relative_error);
                      const std::string display =
                          relative_error ? item.skill_path.generic_string()
                                         : relative.generic_string();
                      const std::string action = item.action;
                      const std::string diagnostic = item.diagnostic;
                      const std::size_t related_files = item.related_files;
                      post_to_ui([action, display, diagnostic, related_files](
                                     qt::app::InteractiveTerminalSession& session) {
                        std::string line =
                            "Memory skill scan: " + action + " " + display;
                        if (related_files != 0U) {
                          line += " (" + std::to_string(related_files) +
                                  " related files)";
                        }
                        if (!diagnostic.empty()) {
                          line += " — " + diagnostic;
                        }
                        session.append_transcript_line(std::move(line));
                      });
                    };
                auto report = qt::memory::import_skill_memories(
                    memory_runtime->memories(), memory_runtime->governance(),
                    scope, options);
                post_to_ui([report = std::move(report)](
                               qt::app::InteractiveTerminalSession& session) {
                  session.append_transcript_line(
                      "Memory skill scan finished: discovered=" +
                      std::to_string(report.discovered) +
                      ", created=" + std::to_string(report.created) +
                      ", updated=" + std::to_string(report.updated) +
                      ", unchanged=" + std::to_string(report.unchanged) +
                      ", failed=" + std::to_string(report.failed));
                });
              } catch (const std::exception& error) {
                post_to_ui([message = std::string(error.what())](
                               qt::app::InteractiveTerminalSession& session) {
                  session.append_transcript_line(
                      "Memory skill scan failed: " + message);
                });
              }
            })
            .detach();
      };

      const auto show_skill_folder_picker =
          [runtime, &ui, launch_skill_scan]() {
        if (!runtime->services.memory_runtime) {
          ui.hide_overlay();
          ui.append_transcript_line(
              "Built-in memory is disabled. Enable it in /memory settings and "
              "start a new runtime before scanning skills.");
          return;
        }
        std::error_code error;
        auto start = std::filesystem::weakly_canonical(
            runtime->services.settings.cwd, error);
        if (error || start.empty()) {
          error.clear();
          start = std::filesystem::current_path(error);
        }
        auto workspace = std::filesystem::weakly_canonical(
            runtime->services.settings.workspace_root, error);
        if (error || workspace.empty()) {
          workspace = start;
        }
        const bool allow_external =
            runtime->services.settings.allow_external_filesystem_access;
        const auto allowed = [workspace, allow_external](
                                 const std::filesystem::path& path) {
          if (allow_external) {
            return true;
          }
          std::error_code relative_error;
          const auto relative =
              std::filesystem::relative(path, workspace, relative_error);
          return !relative_error && !relative.empty() &&
                 *relative.begin() != "..";
        };

        auto browse = std::make_shared<
            std::function<void(const std::filesystem::path&)>>();
        std::weak_ptr<std::function<void(const std::filesystem::path&)>> weak =
            browse;
        *browse = [weak, &ui, launch_skill_scan,
                   allowed](const std::filesystem::path& requested) {
          const auto owner = weak.lock();
          if (!owner) {
            return;
          }
          std::error_code canonical_error;
          const auto current =
              std::filesystem::weakly_canonical(requested, canonical_error);
          if (canonical_error ||
              !std::filesystem::is_directory(current, canonical_error) ||
              canonical_error || !allowed(current)) {
            ui.append_transcript_line(
                "Memory skill folder is unavailable or outside the allowed workspace.");
            return;
          }
          std::vector<qt::tui::SelectItem> items = {
              {"__scan__", "Scan this folder", "Only this folder's SKILL.md"},
              {"__recursive__", "Scan this folder + subfolders",
               "Discover nested skill packages recursively"},
          };
          const auto parent = current.parent_path();
          if (parent != current && allowed(parent)) {
            items.push_back({"__parent__", "..", parent.generic_string()});
          }
          std::vector<std::filesystem::path> directories;
          std::error_code iteration_error;
          std::filesystem::directory_iterator iterator(
              current,
              std::filesystem::directory_options::skip_permission_denied,
              iteration_error);
          const std::filesystem::directory_iterator end;
          while (!iteration_error && iterator != end) {
            std::error_code entry_error;
            if (iterator->is_directory(entry_error) && !entry_error &&
                !iterator->is_symlink(entry_error)) {
              const auto canonical = std::filesystem::weakly_canonical(
                  iterator->path(), entry_error);
              if (!entry_error && allowed(canonical)) {
                directories.push_back(canonical);
              }
            }
            iteration_error.clear();
            iterator.increment(iteration_error);
          }
          std::sort(directories.begin(), directories.end());
          for (const auto& directory : directories) {
            items.push_back({directory.generic_string(),
                             directory.filename().generic_string() + "/",
                             directory.generic_string()});
          }
          auto selector = std::make_shared<qt::tui::Select>(
              std::optional<std::string>{current.generic_string()},
              std::move(items), 0U, 14U, kReferenceInteractiveSelectLayout);
          selector->set_select_callback(
              [owner, weak, &ui, launch_skill_scan,
               current, parent](const qt::tui::SelectItem& item) {
                if (item.value == "__scan__" ||
                    item.value == "__recursive__") {
                  const bool recursive = item.value == "__recursive__";
                  ui.hide_overlay();
                  ui.append_transcript_line(
                      "Memory skill scan started: " + current.generic_string() +
                      " (recursive: " + (recursive ? "yes" : "no") + ")");
                  launch_skill_scan(current, recursive);
                  return;
                }
                if (const auto callback = weak.lock()) {
                  ui.hide_overlay();
                  (*callback)(item.value == "__parent__"
                                  ? parent
                                  : std::filesystem::path(item.value));
                }
              });
          selector->set_cancel_callback([&ui]() { ui.hide_overlay(); });
          ui.show_overlay(selector);
        };
        (*browse)(start);
      };

      const auto show_review_inbox = [runtime, &ui]() {
        if (!runtime->services.memory_runtime) {
          ui.hide_overlay();
          ui.append_transcript_line(
              "Built-in memory is disabled. Enable it in /memory settings and "
              "start a new runtime before reviewing memory.");
          return;
        }
        auto controller = std::make_shared<ReviewInboxController>(
            ui, runtime->services.memory_runtime, runtime->services.tool_context);
        controller->show_landing();
      };

      const auto start_memory_dashboard = [runtime, &ui, &memory_dashboard]() {
        ui.hide_overlay();
        if (!runtime->services.memory_runtime) {
          ui.append_transcript_line(
              "Built-in memory is disabled. Enable it in /memory settings and "
              "start a new runtime before opening the dashboard.");
          return;
        }
        const auto launched = memory_dashboard.start(true);
        if (!launched) {
          ui.append_transcript_line(
              "Memory dashboard failed to start: " + launched.error().message);
          return;
        }
        ui.append_transcript_line(
            std::string(launched.value().started
                            ? "Memory dashboard started: "
                            : "Memory dashboard already running: ") +
            launched.value().url);
        if (!launched.value().browser_opened &&
            !launched.value().browser_error.empty()) {
          ui.append_transcript_line(
              "Browser could not be opened automatically: " +
              launched.value().browser_error);
        }
      };

      if (subcommand.empty()) {
        std::vector<qt::tui::SelectItem> items = {
            {"status", "status",
             "Show configuration, scope, pre-handler state, and statistics"},
            {"settings", "settings", "Configure built-in memory for the next runtime"},
            {"dashboard", "dashboard",
             "Open the scoped memory dashboard in a local browser"},
            {"scan-skills", "scan skills folder",
             "Import SKILL.md packages as skill memories in the background"},
            {"dream", "dream", "Run or inspect governed native Dreaming"},
            {"recall", "recall", "Recall governed memory for a query"},
            {"remember", "remember", "Create a reviewable hypothesis"},
            {"review", "review", "Open the scope-aware Memory Review Inbox"},
            {"feedback", "feedback", "Record a procedural-memory outcome"},
            {"forget", "forget", "Request deletion of an in-scope memory"},
        };
        auto select = std::make_shared<qt::tui::Select>(
            std::optional<std::string>{"memory"}, std::move(items), 0U, 10U,
            kReferenceInteractiveSelectLayout);
        select->set_select_callback(
            [&ui, show_memory_settings, show_skill_folder_picker,
             show_review_inbox, start_memory_dashboard](
                const qt::tui::SelectItem& item) {
          if (item.value == "dashboard") {
            start_memory_dashboard();
            return;
          }
          if (item.value == "settings") {
            show_memory_settings();
            return;
          }
          if (item.value == "scan-skills") {
            ui.hide_overlay();
            show_skill_folder_picker();
            return;
          }
          if (item.value == "review") {
            ui.hide_overlay();
            show_review_inbox();
            return;
          }
          ui.hide_overlay();
          ui.mutable_editor().set_text("/memory " + item.value + " ");
        });
        select->set_cancel_callback([&ui]() { ui.hide_overlay(); });
        ui.show_overlay(select);
        return response;
      }

      if (subcommand == "settings") {
        show_memory_settings();
        return response;
      }

      if (subcommand == "dashboard") {
        start_memory_dashboard();
        return response;
      }

      if (subcommand == "scan-skills" && memory_args.second.empty()) {
        show_skill_folder_picker();
        return response;
      }

      if (subcommand == "status") {
        const auto& settings = runtime->services.settings;
        const auto& memory = runtime->services.memory_runtime;
        response.transcript_lines = {
            "Built-in memory:",
            "  enabled: " + std::string(memory ? "true" : "false"),
            "  configured: " +
                std::string(settings.memory.enabled ? "true" : "false"),
            "  automatic recall: " +
                std::string(settings.memory.automatic_recall ? "true" : "false"),
            "  automatic capture: " +
                std::string(settings.memory.automatic_capture ? "true" : "false"),
            "  dreaming configured: " +
                std::string(settings.memory.dreaming.enabled ? "true"
                                                             : "false"),
            "  dreaming trigger: " + settings.memory.dreaming.trigger,
            "  dreaming schedule: interval=" +
                std::to_string(settings.memory.dreaming.interval_seconds) +
                " s, idle=" +
                std::to_string(settings.memory.dreaming.idle_seconds) + " s",
            "  dreaming model: " +
                settings.memory.dreaming.model.value_or("auto"),
            "  dreaming verifier model: " +
                settings.memory.dreaming.verifier_model.value_or("unset"),
            "  dreaming bounds: " +
                std::to_string(settings.memory.dreaming.max_candidates) +
                " candidates / " +
                std::to_string(
                    settings.memory.dreaming.max_model_input_tokens) +
                " tokens / " +
                std::to_string(settings.memory.dreaming.timeout_ms) + " ms",
            "  dreaming policies: research=" +
                settings.memory.dreaming.research_policy +
                ", promotion=" +
                settings.memory.dreaming.promotion_policy,
            "  pre-handler fail-open: " +
                std::string(settings.memory.prehandler.fail_open ? "true"
                                                                 : "false"),
            "  database: " + settings.agent_home + "/memory/memory.sqlite3",
            "  pre-handler: " +
                std::string(memory && settings.memory.automatic_recall
                                ? "isolated child with deterministic fallback"
                                : "inactive"),
        };
        if (memory) {
          response.transcript_lines.push_back(
              "  schema: " + std::to_string(memory->schema_version()));
          response.transcript_lines.push_back(
              "  scope: " + memory->scope().repository_display +
              (memory->scope().branch.empty()
                   ? std::string{}
                   : " @ " + memory->scope().branch));
          const auto statistics = memory_statistics_lines(memory->statistics());
          response.transcript_lines.insert(response.transcript_lines.end(),
                                           statistics.begin(),
                                           statistics.end());
          const auto reviews = memory->review_statistics();
          response.transcript_lines.push_back(
              "  review queue: pending=" + std::to_string(reviews.pending) +
              ", urgent/high=" + std::to_string(reviews.urgent) +
              ", due deferred=" + std::to_string(reviews.due_deferred) +
              ", oldest=" +
              (reviews.pending == 0U
                   ? std::string("none")
                   : review_duration(reviews.oldest_pending_age_seconds)));
          if (reviews.pending != 0U || reviews.due_deferred != 0U) {
            response.transcript_lines.push_back(
                "  review attention: open /memory review to triage the "
                "scope-aware Inbox");
          }
          const auto dreaming = memory->dreaming_status();
          response.transcript_lines.push_back(
              "  dreaming runtime: " +
              std::string(dreaming.running ? "running" : "idle") +
              ", pending=" + std::to_string(dreaming.pending_runs) +
              ", completed=" + std::to_string(dreaming.completed_runs) +
              ", failed=" + std::to_string(dreaming.failed_runs));
          if (dreaming.trigger == "interval" ||
              dreaming.trigger == "post_turn") {
            response.transcript_lines.push_back(
                "  dreaming idle: " +
                std::to_string(dreaming.idle_for_seconds) + "/" +
                std::to_string(dreaming.idle_seconds) + "s" +
                (dreaming.waiting_for_idle ? " (waiting)" : ""));
          }
          if (!dreaming.last_dream_id.empty()) {
            response.transcript_lines.push_back(
                "  last dream: " + dreaming.last_dream_id +
                " (review " + dreaming.last_review_id + ")");
          }
          if (!dreaming.last_error.empty()) {
            response.transcript_lines.push_back(
                "  dreaming last error: " + dreaming.last_error);
          }
        } else {
          response.transcript_lines.push_back(
              "  statistics: unavailable (memory runtime inactive)");
        }
        bool reported_warning = false;
        for (const auto& diagnostic : runtime->services.diagnostics) {
          if (lowercase_ascii(diagnostic).find("memory") == std::string::npos) {
            continue;
          }
          if (!reported_warning) {
            response.transcript_lines.push_back("  warnings:");
            reported_warning = true;
          }
          response.transcript_lines.push_back("    - " + diagnostic);
        }
        if (!reported_warning) {
          response.transcript_lines.push_back("  warnings: none");
        }
        return response;
      }

      if (!runtime->services.memory_runtime) {
        response.transcript_lines = {
            "Built-in memory is disabled for this runtime.",
            "Enable memory.enabled in global settings and start a new runtime.",
        };
        return response;
      }

      if (subcommand == "scan-skills") {
        const auto mode_and_path = split_first_ascii_argument(memory_args.second);
        const bool recursive = lowercase_ascii(mode_and_path.first) == "recursive";
        const std::string path_text =
            recursive ? mode_and_path.second : memory_args.second;
        if (path_text.empty()) {
          response.transcript_lines = {
              "usage: /memory scan-skills [recursive] <folder>"};
          return response;
        }
        std::error_code path_error;
        std::filesystem::path requested(path_text);
        if (requested.is_relative()) {
          requested = std::filesystem::path(runtime->services.settings.cwd) /
                      requested;
        }
        auto folder = std::filesystem::weakly_canonical(requested, path_error);
        if (path_error || !std::filesystem::is_directory(folder, path_error) ||
            path_error) {
          response.transcript_lines = {"memory skill scan folder is not readable: " +
                                       path_text};
          return response;
        }
        if (!runtime->services.settings.allow_external_filesystem_access) {
          std::error_code workspace_error;
          const auto workspace = std::filesystem::weakly_canonical(
              runtime->services.settings.workspace_root, workspace_error);
          const auto relative = std::filesystem::relative(
              folder, workspace, workspace_error);
          if (workspace_error || relative.empty() || *relative.begin() == "..") {
            response.transcript_lines = {
                "memory skill scan folder is outside the workspace; enable "
                "external filesystem access in /settings to allow it"};
            return response;
          }
        }
        launch_skill_scan(folder, recursive);
        response.transcript_lines = {
            "Memory skill scan started: " + folder.generic_string() +
            " (recursive: " + (recursive ? "yes" : "no") + ")"};
        return response;
      }

      if (subcommand == "dream") {
        const auto operation = split_first_ascii_argument(memory_args.second);
        const std::string action = lowercase_ascii(operation.first);
        if (action.empty() || action == "status") {
          const auto status =
              runtime->services.memory_runtime->dreaming_status();
          response.transcript_lines = {
              "Memory Dreaming:",
              "  enabled: " + std::string(status.enabled ? "true" : "false"),
              "  trigger: " + status.trigger,
              "  idle duration: " + std::to_string(status.idle_seconds) + "s",
              "  idle for: " + std::to_string(status.idle_for_seconds) + "s",
              "  waiting for idle: " +
                  std::string(status.waiting_for_idle ? "true" : "false"),
              "  state: " + std::string(status.running ? "running" : "idle"),
              "  pending: " + std::to_string(status.pending_runs),
              "  completed: " + std::to_string(status.completed_runs),
              "  failed: " + std::to_string(status.failed_runs),
              "  last dream: " +
                  (status.last_dream_id.empty() ? std::string("none")
                                                : status.last_dream_id),
              "  last review: " +
                  (status.last_review_id.empty() ? std::string("none")
                                                 : status.last_review_id),
              "  last error: " +
                  (status.last_error.empty() ? std::string("none")
                                             : status.last_error),
          };
          return response;
        }
        if (action == "run") {
          const bool queued =
              runtime->services.memory_runtime->enqueue_dream_cycle("manual");
          response.transcript_lines = {
              queued ? "Memory Dreaming cycle queued in the background."
                     : "Memory Dreaming cycle was not queued; enable Dreaming "
                       "and verify its queue/model settings."};
          return response;
        }
        if (action == "research") {
          try {
            const Json input = Json::parse(operation.second);
            if (!input.is_object() ||
                !input.contains("dream_id") ||
                !input.at("dream_id").is_string() ||
                !input.contains("summary") ||
                !input.at("summary").is_string()) {
              throw std::invalid_argument(
                  "research JSON requires string dream_id and summary");
            }
            qt::memory::DreamResearchEvidence evidence;
            evidence.summary = input.at("summary").get<std::string>();
            evidence.source_memory_ids = input.value(
                "source_memory_ids", std::vector<std::string>{});
            evidence.tags = input.value("tags", std::vector<std::string>{});
            evidence.confidence = input.value("confidence", 0.7);
            evidence.provenance.source_type =
                qt::memory::source_type_from_string(
                    input.value("source_type", std::string("document")));
            evidence.provenance.source_uri =
                input.value("source_uri", std::string{});
            evidence.provenance.source_hash =
                input.value("source_hash", std::string{});
            evidence.provenance.source_span =
                input.value("source_span", std::string{});
            evidence.provenance.created_by = "user:interactive";
            evidence.provenance.evidence_strength =
                input.value("evidence_strength", 0.7);
            const auto stored =
                runtime->services.memory_runtime->add_dream_research(
                    input.at("dream_id").get<std::string>(),
                    std::move(evidence));
            response.transcript_lines = {
                "Dream research successor stored for review: " +
                    stored.researched_memory_id.value_or("none"),
                "Research review: " +
                    stored.research_review_id.value_or("none")};
          } catch (const std::exception& error) {
            response.transcript_lines = {
                std::string("Unable to store dream research: ") + error.what(),
                "usage: /memory dream research "
                "{\"dream_id\":\"...\",\"summary\":\"...\","
                "\"source_uri\":\"...\"}"};
          }
          return response;
        }
        response.transcript_lines = {
            "usage: /memory dream [status|run|research <json>]"};
        return response;
      }

      // Slash commands are an explicit user-facing management surface. Keep
      // them available when model-facing tools are filtered or disabled, while
      // reusing the exact same direct-service implementations and validation.
      auto memory_registry = std::make_shared<qt::tools::ToolRegistry>();
      qt::tools::register_memory_tools(
          *memory_registry, runtime->services.memory_runtime);
      const auto invoke_memory = [&](qt::tools::ToolRequest request,
                                     const std::string& label) {
        auto tool_context = runtime->services.tool_context;
        const auto result = invoke_interactive_tool_request(
            request, memory_registry, std::move(tool_context));
        response.transcript_lines = index_tool_result_lines(label, result);
      };

      if (subcommand == "recall") {
        if (memory_args.second.empty()) {
          response.transcript_lines = {"usage: /memory recall <query>"};
          return response;
        }
        qt::tools::ToolRequest request;
        request.tool_name = "memory_recall";
        request.arguments = Json{{"query", memory_args.second}};
        invoke_memory(std::move(request), "memory recall");
        return response;
      }

      if (subcommand == "remember") {
        if (memory_args.second.empty()) {
          response.transcript_lines = {"usage: /memory remember <content>"};
          return response;
        }
        qt::tools::ToolRequest request;
        request.tool_name = "memory_remember";
        request.arguments = Json{{"content", memory_args.second},
                                 {"status", "hypothesis"},
                                 {"source", "user:interactive"}};
        invoke_memory(std::move(request), "memory remember");
        return response;
      }

      if (subcommand == "review") {
        const auto review = split_first_ascii_argument(memory_args.second);
        if (review.first.empty() || lowercase_ascii(review.first) == "list") {
          show_review_inbox();
          return response;
        }
        if (lowercase_ascii(review.first) == "decide") {
          const auto id = split_first_ascii_argument(review.second);
          const auto decision = split_first_ascii_argument(id.second);
          if (id.first.empty() || decision.first.empty() ||
              lowercase_ascii(decision.second) != "confirm") {
            response.transcript_lines = {
                "usage: /memory review decide <review-id> "
                "<accepted|rejected|deferred> confirm"};
            return response;
          }
          qt::memory::ReviewAction action;
          const auto normalized = lowercase_ascii(decision.first);
          if (normalized == "accepted") {
            action.kind = qt::memory::ReviewActionKind::Approve;
          } else if (normalized == "rejected") {
            action.kind = qt::memory::ReviewActionKind::Reject;
          } else if (normalized == "deferred") {
            action.kind = qt::memory::ReviewActionKind::Defer;
            action.deferred_until = std::chrono::system_clock::now() +
                                    std::chrono::hours(24);
          } else {
            response.transcript_lines = {
                "decision must be accepted, rejected, or deferred"};
            return response;
          }
          action.actor = "user:interactive";
          action.human_authority = true;
          try {
            const auto preview =
                runtime->services.memory_runtime->preview_review(id.first, action);
            if (!preview.applicable) {
              response.transcript_lines = {
                  "Memory review action is not applicable: " +
                  (preview.warnings.empty() ? std::string("no safe mutation")
                                            : preview.warnings.front())};
              return response;
            }
            action.expected_revision = preview.review_revision;
            action.idempotency_key = preview.idempotency_token;
            const auto result = runtime->services.memory_runtime
                                    ->apply_review_action(id.first,
                                                         std::move(action));
            response.transcript_lines = {
                "Memory review " + id.first + ": " +
                    qt::memory::to_string(result.status) +
                    "; decision=" +
                    (result.decision_persisted ? "persisted" : "not persisted") +
                    "; domain=" +
                    (result.domain_mutations_applied ? "applied" : "unchanged") +
                    "; revision=" + std::to_string(result.revision)};
          } catch (const std::exception& error) {
            response.transcript_lines = {
                std::string("Memory review was not changed: ") + error.what()};
          }
          return response;
        } else {
          response.transcript_lines = {
              "usage: /memory review",
              "usage: /memory review decide <review-id> "
              "<accepted|rejected|deferred> confirm"};
          return response;
        }
      }

      if (subcommand == "feedback") {
        const auto feedback = split_first_ascii_argument(memory_args.second);
        const auto outcome = split_first_ascii_argument(feedback.second);
        if (feedback.first.empty() || outcome.first.empty() ||
            lowercase_ascii(outcome.second) != "confirm") {
          response.transcript_lines = {
              "usage: /memory feedback <memory-id> "
              "<success|failure|partial|unused> confirm"};
          return response;
        }
        qt::tools::ToolRequest request;
        request.tool_name = "memory_feedback";
        request.arguments = Json{{"id", feedback.first},
                                 {"outcome", lowercase_ascii(outcome.first)}};
        invoke_memory(std::move(request), "memory feedback");
        return response;
      }

      if (subcommand == "forget") {
        const auto forget = split_first_ascii_argument(memory_args.second);
        if (forget.first.empty() || lowercase_ascii(forget.second) != "confirm") {
          response.transcript_lines = {
              "usage: /memory forget <memory-id> confirm",
              "Deletion also requires the normal mutation approval policy."};
          return response;
        }
        qt::tools::ToolRequest request;
        request.tool_name = "memory_forget";
        request.arguments = Json{{"id", forget.first}, {"confirm", true}};
        invoke_memory(std::move(request), "memory forget");
        return response;
      }

      response.transcript_lines = {
          "unknown memory command: " + memory_args.first,
          "usage: /memory [dashboard|status|settings|scan-skills|dream|recall|remember|review|feedback|forget]",
      };
      return response;
    }

    if (event.command == "index") {
      response.handled = true;
      response.append_default_output = false;

      const auto index_args = split_first_ascii_argument(event.arguments);
      const std::string subcommand = lowercase_ascii(index_args.first);
      auto invoke_index = [&](const qt::tools::ToolRequest& request,
                              const std::string& label) {
        auto tool_context = runtime->services.tool_context;
        const auto result = invoke_interactive_tool_request(
            request, runtime->services.tool_registry, tool_context);
        response.transcript_lines = index_tool_result_lines(label, result);
      };
      const auto parse_or_fail = [&](std::string_view raw)
          -> std::optional<Json> {
        auto parsed = parse_index_json_arguments(raw);
        if (!parsed) {
          response.transcript_lines = {
              "index arguments must be a JSON object.",
              "usage: /index call <operation> {\"name\":\"value\"}",
              "usage: /index <category.operation> {\"name\":\"value\"}",
          };
        }
        return parsed;
      };

      if (index_args.first.empty()) {
        std::vector<qt::tui::SelectItem> items = {
            {"tree", "tree", "List grouped index operation categories"},
            {"schemas", "schemas", "List operation categories with JSON schemas"},
            {"status", "status", "Show index metadata and scan status"},
            {"summary", "summary", "Show project summary from the index"},
            {"rescan", "rescan", "Request an immediate background rescan"},
            {"core", "core", "Core index, graph, metadata, and scan operations"},
            {"code", "code", "Code search, symbols, files, AST, and call graph"},
            {"fs", "fs", "Filesystem stat, list, tree, read, and search"},
            {"git", "git", "Git status, log, diff, show, blame, and branches"},
            {"build", "build", "Build metadata, compile flags, and include graph"},
            {"debug", "debug", "GDB/OpenOCD debug helper operations"},
            {"serial", "serial", "Serial/UART list, open, read, write, and close"},
            {"rtos", "rtos", "RTOS task graph operations"},
            {"fetch", "fetch", "Fetch URL content through the index component"},
            {"util", "util", "JSON, text, URL, hash, CSV, and encoding utilities"},
            {"call", "call", "Run any operation: /index call fs.read {json}"},
        };
        auto select = std::make_shared<qt::tui::Select>(
            std::optional<std::string>{"index"}, std::move(items), 0U, 12U,
            kReferenceInteractiveSelectLayout);
        select->set_select_callback([&ui](const qt::tui::SelectItem& item) {
          ui.hide_overlay();
          ui.mutable_editor().set_text("/index " + item.value + " ");
        });
        select->set_cancel_callback([&ui]() { ui.hide_overlay(); });
        ui.show_overlay(select);
        return response;
      }

      if (subcommand == "tree") {
        const std::string category = lowercase_ascii(index_args.second);
        const std::optional<std::string> requested_category =
            category.empty() ? std::nullopt : std::optional<std::string>{category};
        invoke_index(make_index_tree_request(requested_category, false),
                     category.empty() ? "index tree" : "index tree: " + category);
        return response;
      }

      if (subcommand == "schemas") {
        const std::string category = lowercase_ascii(index_args.second);
        const std::optional<std::string> requested_category =
            category.empty() ? std::nullopt : std::optional<std::string>{category};
        invoke_index(make_index_tree_request(requested_category, true),
                     category.empty() ? "index schemas"
                                      : "index schemas: " + category);
        return response;
      }

      if (is_index_category(subcommand)) {
        invoke_index(make_index_tree_request(std::optional<std::string>{subcommand},
                                             false),
                     "index category: " + subcommand);
        return response;
      }

      if (subcommand == "status") {
        invoke_index(make_index_call_request(
                         "status", Json{{"include_build_metadata", true},
                                        {"include_tree_sitter", true}}),
                     "index status");
        return response;
      }

      if (subcommand == "metadata") {
        invoke_index(make_index_call_request(
                         "metadata", Json{{"include_compile_commands", true},
                                          {"include_tree_sitter", true}}),
                     "index metadata");
        return response;
      }

      if (subcommand == "summary") {
        Json arguments = Json::object();
        if (!index_args.second.empty()) {
          try {
            arguments["limit"] = std::stoi(index_args.second);
          } catch (const std::exception&) {
            response.transcript_lines = {"usage: /index summary [limit]"};
            return response;
          }
        }
        invoke_index(make_index_call_request("project_summary", std::move(arguments)),
                     "index summary");
        return response;
      }

      if (subcommand == "rescan") {
        Json arguments = Json::object();
        if (!index_args.second.empty()) {
          arguments["reason"] = index_args.second;
        }
        invoke_index(make_index_call_request("rescan", std::move(arguments)),
                     "index rescan");
        return response;
      }

      if (subcommand == "call") {
        const auto call_args = split_first_ascii_argument(index_args.second);
        if (call_args.first.empty()) {
          response.transcript_lines = {
              "usage: /index call <operation> [json-object]",
              "example: /index call fs.read {\"file\":\"PLAN.md\"}",
          };
          return response;
        }
        auto arguments = parse_or_fail(call_args.second);
        if (!arguments) {
          return response;
        }
        invoke_index(make_index_call_request(call_args.first, std::move(*arguments)),
                     "index call: " + call_args.first);
        return response;
      }

      if (subcommand.find('.') != std::string::npos) {
        auto arguments = parse_or_fail(index_args.second);
        if (!arguments) {
          return response;
        }
        invoke_index(make_index_call_request(subcommand, std::move(*arguments)),
                     "index call: " + subcommand);
        return response;
      }

      response.transcript_lines = {
          "unknown index command: " + index_args.first,
          "usage: /index",
          "usage: /index tree [category]",
          "usage: /index schemas [category]",
          "usage: /index call <operation> [json-object]",
      };
      return response;
    }

    if (event.command == "llama") {
      response.handled = true;
      response.append_default_output = false;

      const auto llama_args = split_first_ascii_argument(event.arguments);
      const std::string subcommand =
          llama_args.first.empty() ? std::string("status")
                                   : lowercase_ascii(llama_args.first);
      auto& llama = runtime->services.settings.llama;
      const auto current_model = runtime->host.harness().model();
      const bool top_level_llama =
          current_model.provider == qt::ai::kLlamaProviderId ||
          current_model.api == qt::ai::kLlamaProviderApi;
      auto selected_llama_model_id = runtime->services.selected_llama_model_id;
      if (!selected_llama_model_id && top_level_llama) {
        selected_llama_model_id = current_model.name;
      }
      const bool has_llama_selection =
          selected_llama_model_id && !selected_llama_model_id->empty();
      const auto llama_provider = runtime->services.llama_provider;
      auto active_generation = llama.generation;
      if (has_llama_selection) {
        for (const auto& model : llama.models) {
          if (model.id == *selected_llama_model_id) {
            active_generation = qt::ai::merge_llama_generation_settings(
                llama.generation, model.generation);
            break;
          }
        }
      }

      const auto optional_int = [](const std::optional<int>& value,
                                   const std::string& fallback) {
        return value ? std::to_string(*value) : fallback;
      };
      const auto optional_int64 = [](const std::optional<std::int64_t>& value,
                                     const std::string& fallback) {
        return value ? std::to_string(*value) : fallback;
      };
      const auto optional_bool = [](const std::optional<bool>& value,
                                    const std::string& fallback) {
        if (!value) {
          return fallback;
        }
        return *value ? std::string("true") : std::string("false");
      };
      const auto optional_double = [](const std::optional<double>& value,
                                      const std::string& fallback) {
        if (!value) {
          return fallback;
        }
        std::ostringstream out;
        out << *value;
        return out.str();
      };
      const auto format_bytes = [](std::uint64_t bytes) {
        const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
        double value = static_cast<double>(bytes);
        std::size_t unit_index = 0;
        while (value >= 1024.0 && unit_index + 1U < std::size(units)) {
          value /= 1024.0;
          ++unit_index;
        }
        std::ostringstream out;
        if (unit_index == 0U) {
          out << bytes << ' ' << units[unit_index];
        } else {
          out << std::fixed << std::setprecision(2) << value << ' '
              << units[unit_index];
        }
        return out.str();
      };
      const auto json_bytes = [](const Json& json, std::string_view key) {
        const auto found = json.find(std::string(key));
        if (found == json.end() || !found->is_number_unsigned()) {
          return std::uint64_t{0};
        }
        return found->get<std::uint64_t>();
      };
      const auto kv_state_line = [](const qt::ai::LlamaKvCacheState& state) {
        return "kv_state: seq=" + std::to_string(state.seq_id) +
               ", min=" + std::to_string(state.position_min) +
               ", max=" + std::to_string(state.position_max) +
               ", position=" + std::to_string(state.sequence_position) +
               ", canShift=" + std::string(state.can_shift ? "true" : "false");
      };
      const auto active_tool_definitions = [&]() {
        std::vector<qt::ai::ToolDefinition> tools;
        const auto& active_tool_names =
            runtime->host.harness().active_tool_names();
        for (const auto& name : active_tool_names) {
          const auto match = std::find_if(
              runtime->services.tool_definitions.begin(),
              runtime->services.tool_definitions.end(),
              [&](const qt::ai::ToolDefinition& tool) {
                return tool.name == name;
              });
          if (match != runtime->services.tool_definitions.end()) {
            tools.push_back(*match);
          }
        }
        return tools;
      };
      const auto completion_text =
          [](const qt::ai::CompletionResponse& completion) {
        std::string text;
        for (const auto& content : completion.message.content) {
          if (const auto* text_content =
                  std::get_if<qt::ai::TextContent>(&content)) {
            text += text_content->text;
          } else if (const auto* tool_call =
                         std::get_if<qt::ai::ToolCallContent>(&content)) {
            text += "tool_call: " + tool_call->name + " " +
                    tool_call->arguments.dump();
          }
        }
        return text;
      };
      const auto append_context_pressure_line =
          [](std::vector<std::string>& lines,
             const qt::ai::CompletionResponse& completion) {
        const auto kv_cache = completion.metadata.find("kvCache");
        if (kv_cache == completion.metadata.end() || !kv_cache->is_object()) {
          return;
        }
        const auto pressure = kv_cache->find("contextPressure");
        if (pressure == kv_cache->end() || !pressure->is_object()) {
          return;
        }
        const auto pruned = pressure->value("prunedTokens", 0);
        if (pruned <= 0) {
          return;
        }
        lines.push_back("kv_pruned: " + std::to_string(pruned) +
                        " tokens, position=" +
                        std::to_string(
                            pressure->value("afterSequencePosition", 0)));
      };

      if (llama_args.first.empty()) {
        std::vector<qt::tui::SelectItem> items;
        for (const auto& info : qt::app::llama_slash_subcommand_infos()) {
          std::string label = info.name;
          if (info.argument_hint[0] != '\0') {
            label += " ";
            label += info.argument_hint;
          }
          items.push_back(qt::tui::SelectItem{info.name, std::move(label),
                                              info.description});
        }
        auto select = std::make_shared<qt::tui::Select>(
            std::optional<std::string>{"llama"}, std::move(items), 0U, 10U,
            kReferenceInteractiveSelectLayout);
        select->set_select_callback([&ui](const qt::tui::SelectItem& item) {
          ui.hide_overlay();
          ui.mutable_editor().set_text("/llama " + item.value + " ");
        });
        select->set_cancel_callback([&ui]() { ui.hide_overlay(); });
        ui.show_overlay(select);
        return response;
      }

      if (subcommand == "status") {
        std::string loaded = "unavailable until llama provider runtime is initialized";
        std::optional<qt::ai::LlamaKvCacheState> kv_state;
        std::optional<qt::ai::LlamaRuntimeStatus> runtime_status;
        if (llama_provider) {
          const auto loaded_model = llama_provider->loaded_model_id();
          loaded = loaded_model ? *loaded_model : std::string("(not loaded)");
          kv_state = llama_provider->kv_cache_control({});
          runtime_status = llama_provider->runtime_status();
        }
        response.transcript_lines = {
            "llama provider: " +
                std::string(llama.models.empty() ? "no configured models"
                                                 : "configured"),
            "selected: " +
                std::string(has_llama_selection
                                ? std::string(qt::ai::kLlamaProviderId) + "/" +
                                      *selected_llama_model_id
                                : "(not selected)"),
            "top_level_model: " + current_model.provider + "/" +
                current_model.name,
            "loaded: " + loaded,
            "context: " +
                optional_int64(llama.context.context_size, "llama.cpp default"),
            "policy: temperature=" +
                optional_double(active_generation.temperature, "default") +
                ", topK=" + optional_int(active_generation.top_k, "default") +
                ", topP=" + optional_double(active_generation.top_p, "default") +
                ", minP=" + optional_double(active_generation.min_p, "default"),
            "kv_cache: " + llama.kv_cache.policy.value_or("session"),
            "gpu: gpuLayers=" +
                optional_int(llama.backend.gpu_layers, "default") +
                ", mainGpu=" + optional_int(llama.backend.main_gpu, "default") +
                ", splitMode=" + llama.backend.split_mode.value_or("default"),
            "grammar: root=" + llama.grammar.root.value_or("root") +
                ", default=" + llama.grammar.default_grammar.value_or("(none)"),
            "embeddings: enabled=" +
                optional_bool(llama.embeddings.enabled, "false") +
                ", pooling=" + llama.embeddings.pooling.value_or("default"),
        };
        if (kv_state) {
          response.transcript_lines.push_back(kv_state_line(*kv_state));
        }
        if (runtime_status) {
          response.transcript_lines.push_back(
              "runtime: modelLoaded=" +
              std::string(runtime_status->model_loaded ? "true" : "false") +
              ", contextLoaded=" +
              std::string(runtime_status->context_loaded ? "true" : "false") +
              ", contextSize=" + std::to_string(runtime_status->context_size) +
              ", sequencePosition=" +
              std::to_string(runtime_status->sequence_position) +
              ", embeddings=" +
              std::string(runtime_status->embeddings_enabled ? "true" : "false"));
          response.transcript_lines.push_back(
              "memory: model=" +
              format_bytes(runtime_status->model_size_bytes) +
              ", state=" + format_bytes(runtime_status->state_size_bytes) +
              ", host=" +
              format_bytes(json_bytes(runtime_status->memory.value("host", Json::object()),
                                      "allocatedBytes")));
          response.transcript_lines.push_back(
              "backend: gpuOffloadSupported=" +
              std::string(runtime_status->gpu_offload_supported ? "true" : "false") +
              ", maxDevices=" + std::to_string(runtime_status->max_devices));
          const Json devices =
              runtime_status->memory.value("devices", Json::array());
          if (devices.empty()) {
            response.transcript_lines.push_back(
                "gpu_memory: no accelerator device allocations reported");
          } else {
            for (const auto& device : devices) {
              const auto name = device.value("name", std::string("device"));
              const auto type = device.value("type", std::string("unknown"));
              const bool gpu_memory = device.value("gpuMemory", false);
              response.transcript_lines.push_back(
                  std::string(gpu_memory ? "gpu_memory: " : "device_memory: ") +
                  name + " type=" + type +
                  " total=" + format_bytes(json_bytes(device, "totalBytes")) +
                  ", free=" + format_bytes(json_bytes(device, "freeBytes")) +
                  ", used=" + format_bytes(json_bytes(device, "usedBytes")) +
                  ", llama=" +
                  format_bytes(json_bytes(device, "allocatedBytes")) +
                  " (model=" +
                  format_bytes(json_bytes(device, "modelBytes")) +
                  ", context=" +
                  format_bytes(json_bytes(device, "contextBytes")) +
                  ", compute=" +
                  format_bytes(json_bytes(device, "computeBytes")) + ")");
            }
          }
        }
        if (llama_provider) {
          response.transcript_lines.push_back("instances:");
          for (const auto tag : {qt::ai::LlamaInstanceTag::kEmbed,
                                 qt::ai::LlamaInstanceTag::kRank,
                                 qt::ai::LlamaInstanceTag::kInstruct,
                                 qt::ai::LlamaInstanceTag::kChat}) {
            const auto status = llama_provider->runtime_status(tag);
            response.transcript_lines.push_back(
                "- " + std::string(qt::ai::llama_instance_tag_name(tag)) +
                ": " +
                (status.loaded_model_id ? *status.loaded_model_id
                                        : std::string("(not loaded)")) +
                ", contextLoaded=" +
                std::string(status.context_loaded ? "true" : "false") +
                ", embeddings=" +
                std::string(status.embeddings_enabled ? "true" : "false") +
                ", sequencePosition=" +
                std::to_string(status.sequence_position));
          }
        }
        return response;
      }

      if (subcommand == "models") {
        if (llama.models.empty()) {
          response.transcript_lines = {"No llama models are configured."};
          return response;
        }
        for (const auto& model : llama.models) {
          std::string line = has_llama_selection &&
                                     *selected_llama_model_id == model.id
                                 ? "* "
                                 : "- ";
          line += model.id;
          line += " - ";
          line += model.name;
          line += " (";
          line += model.path;
          line += ")";
          response.transcript_lines.push_back(std::move(line));
        }
        return response;
      }

      if (subcommand == "select") {
        const std::string model_id = trim_ascii_copy(llama_args.second);
        if (model_id.empty()) {
          return qt::foundation::Result<
              qt::app::InteractiveSlashCommandResponse>::failure(
              qt::foundation::ErrorCode::invalid_argument,
              "Usage: /llama select <model-id>");
        }
        const auto* model =
            qt::ai::find_model_record(runtime->services.model_catalog, "llama",
                                      model_id);
        if (model == nullptr) {
          return qt::foundation::Result<
              qt::app::InteractiveSlashCommandResponse>::failure(
              qt::foundation::ErrorCode::not_found,
              "llama model not found: " + model_id);
        }

        runtime->services.selected_llama_model_id = model->ref.name;
        llama.selected_model_id = model->ref.name;
        const auto persisted = persist_llama_model_selection(model->ref.name);
        if (!persisted) {
          return persisted.error();
        }
        response.transcript_lines = {
            "Llama model set to llama/" + model->ref.name};
        return response;
      }

      if (subcommand == "grammar") {
        const auto grammar_action = split_first_ascii_argument(llama_args.second);
        const std::string verb =
            grammar_action.first.empty() ? std::string("status")
                                         : lowercase_ascii(grammar_action.first);
        auto options = runtime->host.harness().agent().provider_options();
        if (!options.metadata.is_object()) {
          options.metadata = qt::ai::Json::object();
        }
        if (!options.metadata.contains("llama") ||
            !options.metadata["llama"].is_object()) {
          options.metadata["llama"] = qt::ai::Json::object();
        }

        if (verb == "status") {
          response.transcript_lines = {
              "grammar: root=" + llama.grammar.root.value_or("root") +
                  ", default=" +
                  llama.grammar.default_grammar.value_or("(none)"),
              "runtime_override: " +
                  options.metadata["llama"].value("grammar",
                                                  qt::ai::Json(nullptr)).dump(),
          };
          return response;
        }
        if (verb == "off") {
          options.metadata["llama"]["grammar"] =
              qt::ai::Json{{"enabled", false}};
          runtime->host.harness().set_stream_options(std::move(options));
          response.transcript_lines = {
              "Llama grammar constraints disabled for subsequent turns."};
          return response;
        }
        if (verb == "inline") {
          const std::string grammar = trim_ascii_copy(grammar_action.second);
          if (grammar.empty()) {
            return qt::foundation::Result<
                qt::app::InteractiveSlashCommandResponse>::failure(
                qt::foundation::ErrorCode::invalid_argument,
                "Usage: /llama grammar inline <gbnf>");
          }
          options.metadata["llama"]["grammar"] =
              qt::ai::Json{{"inline", grammar},
                           {"root", llama.grammar.root.value_or("root")}};
          runtime->host.harness().set_stream_options(std::move(options));
          response.transcript_lines = {
              "Llama inline grammar configured for subsequent turns.",
              "root: " + llama.grammar.root.value_or("root")};
          return response;
        }
        if (verb == "file") {
          const auto file_args = split_first_ascii_argument(grammar_action.second);
          if (file_args.first.empty()) {
            return qt::foundation::Result<
                qt::app::InteractiveSlashCommandResponse>::failure(
                qt::foundation::ErrorCode::invalid_argument,
                "Usage: /llama grammar file <path> [root]");
          }
          const auto resolved_path = resolve_safe_llama_grammar_path(
              runtime->services.settings, file_args.first);
          if (!resolved_path) {
            return resolved_path.error();
          }
          options.metadata["llama"]["grammar"] =
              qt::ai::Json{{"file", resolved_path.value()},
                           {"root", file_args.second.empty()
                                        ? llama.grammar.root.value_or("root")
                                        : file_args.second}};
          runtime->host.harness().set_stream_options(std::move(options));
          response.transcript_lines = {
              "Llama file grammar configured for subsequent turns.",
              "path: " + resolved_path.value()};
          return response;
        }
      }

      if (subcommand == "kv") {
        if (!llama_provider) {
          response.transcript_lines = {
              "Llama KV cache is unavailable until the runtime starts with a llama model."};
          return response;
        }

        const auto action = split_first_ascii_argument(llama_args.second);
        const std::string verb =
            action.first.empty() ? std::string("status")
                                 : lowercase_ascii(action.first);
        const auto parse_i32 = [](const std::string& value)
            -> std::optional<std::int32_t> {
          try {
            std::size_t consumed = 0;
            const int parsed_value = std::stoi(value, &consumed, 10);
            if (consumed != value.size()) {
              return std::nullopt;
            }
            return static_cast<std::int32_t>(parsed_value);
          } catch (const std::exception&) {
            return std::nullopt;
          }
        };

        qt::ai::LlamaKvCacheOperation operation;
        if (verb == "status") {
          operation.kind = qt::ai::LlamaKvCacheOperationKind::kInspect;
        } else if (verb == "clear") {
          operation.kind = qt::ai::LlamaKvCacheOperationKind::kClear;
        } else if (verb == "keep") {
          operation.kind = qt::ai::LlamaKvCacheOperationKind::kKeep;
          const auto seq = parse_i32(trim_ascii_copy(action.second));
          if (!seq) {
            return qt::foundation::Result<
                qt::app::InteractiveSlashCommandResponse>::failure(
                qt::foundation::ErrorCode::invalid_argument,
                "Usage: /llama kv keep <seq-id>");
          }
          operation.seq_id = *seq;
        } else if (verb == "drop") {
          operation.kind = qt::ai::LlamaKvCacheOperationKind::kDrop;
          auto rest = split_first_ascii_argument(action.second);
          const auto seq = parse_i32(rest.first);
          if (!seq) {
            return qt::foundation::Result<
                qt::app::InteractiveSlashCommandResponse>::failure(
                qt::foundation::ErrorCode::invalid_argument,
                "Usage: /llama kv drop <seq-id> [from] [to]");
          }
          operation.seq_id = *seq;
          rest = split_first_ascii_argument(rest.second);
          if (!rest.first.empty()) {
            const auto from = parse_i32(rest.first);
            if (!from) {
              return qt::foundation::Result<
                  qt::app::InteractiveSlashCommandResponse>::failure(
                  qt::foundation::ErrorCode::invalid_argument,
                  "Usage: /llama kv drop <seq-id> [from] [to]");
            }
            operation.from = *from;
          }
          if (!rest.second.empty()) {
            const auto to_arg = split_first_ascii_argument(rest.second);
            const auto to = parse_i32(to_arg.first);
            if (!to) {
              return qt::foundation::Result<
                  qt::app::InteractiveSlashCommandResponse>::failure(
                  qt::foundation::ErrorCode::invalid_argument,
                  "Usage: /llama kv drop <seq-id> [from] [to]");
            }
            operation.to = *to;
          }
        } else if (verb == "shift") {
          operation.kind = qt::ai::LlamaKvCacheOperationKind::kShift;
          auto rest = split_first_ascii_argument(action.second);
          const auto seq = parse_i32(rest.first);
          rest = split_first_ascii_argument(rest.second);
          const auto from = parse_i32(rest.first);
          rest = split_first_ascii_argument(rest.second);
          const auto to = parse_i32(rest.first);
          const auto delta = parse_i32(rest.second);
          if (!seq || !from || !to || !delta) {
            return qt::foundation::Result<
                qt::app::InteractiveSlashCommandResponse>::failure(
                qt::foundation::ErrorCode::invalid_argument,
                "Usage: /llama kv shift <seq-id> <from> <to> <delta>");
          }
          operation.seq_id = *seq;
          operation.from = *from;
          operation.to = *to;
          operation.delta = *delta;
        } else {
          return qt::foundation::Result<
              qt::app::InteractiveSlashCommandResponse>::failure(
              qt::foundation::ErrorCode::invalid_argument,
              "Unsupported llama KV command: " + verb);
        }

        const auto state = llama_provider->kv_cache_control(operation);
        response.transcript_lines = {
            verb == "status" ? std::string("Llama KV cache status.")
                             : "Llama KV cache updated: " + verb,
            kv_state_line(state),
        };
        return response;
      }

      if (subcommand == "run") {
        if (!llama_provider || !has_llama_selection) {
          response.transcript_lines = {
              "Llama run is unavailable until a llama model is selected."};
          return response;
        }
        const auto run_args = split_first_ascii_argument(llama_args.second);
        const auto tag = qt::ai::parse_llama_instance_tag(
            lowercase_ascii(run_args.first));
        if (!tag) {
          return qt::foundation::Result<
              qt::app::InteractiveSlashCommandResponse>::failure(
              qt::foundation::ErrorCode::invalid_argument,
              "Usage: /llama run <embed|rank|instruct|chat> <text>");
        }
        const std::string text = trim_ascii_copy(run_args.second);
        if (text.empty()) {
          return qt::foundation::Result<
              qt::app::InteractiveSlashCommandResponse>::failure(
              qt::foundation::ErrorCode::invalid_argument,
              "Usage: /llama run <embed|rank|instruct|chat> <text>");
        }

        if (*tag == qt::ai::LlamaInstanceTag::kEmbed) {
          qt::ai::LlamaEmbeddingRequest embedding_request;
          embedding_request.model_id = *selected_llama_model_id;
          embedding_request.text = text;
          qt::ai::LlamaEmbeddingResult embedding;
          try {
            embedding = llama_provider->embed(embedding_request, *tag);
          } catch (const std::exception& error) {
            response.transcript_lines = {
                std::string("Llama embed run failed: ") + error.what()};
            return response;
          }
          std::ostringstream preview;
          const std::size_t preview_count =
              std::min<std::size_t>(embedding.embedding.size(), 3U);
          for (std::size_t index = 0; index < preview_count; ++index) {
            if (index > 0) {
              preview << ", ";
            }
            preview << embedding.embedding[index];
          }
          response.transcript_lines = {
              "Llama embed run complete.",
              "instance: embed",
              "dimensions: " + std::to_string(embedding.embedding.size()),
              "input_tokens: " + std::to_string(embedding.usage.input_tokens),
              "preview: [" + preview.str() +
                  (embedding.embedding.size() > preview_count ? ", ...]" : "]"),
          };
          return response;
        }

        qt::ai::CompletionRequest llama_request;
        llama_request.model = qt::ai::ModelRef{qt::ai::kLlamaProviderId,
                                               qt::ai::kLlamaProviderApi,
                                               *selected_llama_model_id};
        qt::ai::Message message;
        message.role = qt::ai::MessageRole::user;
        message.content = {qt::ai::TextContent{text, std::nullopt}};
        llama_request.messages = {std::move(message)};
        llama_request.metadata["llamaInstanceTag"] =
            qt::ai::llama_instance_tag_name(*tag);
        if (*tag == qt::ai::LlamaInstanceTag::kInstruct) {
          llama_request.tools = active_tool_definitions();
          llama_request.tool_choice.kind = qt::ai::ToolChoiceKind::auto_select;
          llama_request.max_output_tokens = 128;
        }

        auto options = runtime->host.harness().agent().provider_options();
        qt::ai::CompletionResponse completion;
        try {
          qt::ai::LlamaKvCacheOperation clear_cache;
          clear_cache.kind = qt::ai::LlamaKvCacheOperationKind::kClear;
          (void)llama_provider->kv_cache_control(clear_cache, *tag);
          completion = llama_provider->run_tagged(*tag, llama_request, options);
        } catch (const std::exception& error) {
          response.transcript_lines = {
              std::string("Llama ") +
              std::string(qt::ai::llama_instance_tag_name(*tag)) +
              " run failed: " + error.what()};
          return response;
        }
        response.transcript_lines = {
            "instance: " + std::string(qt::ai::llama_instance_tag_name(*tag))};
        append_context_pressure_line(response.transcript_lines, completion);
        bool executed_tool = false;
        bool stopped_tool_loop = false;
        Json executed_tool_results = Json::array();
        if (*tag == qt::ai::LlamaInstanceTag::kInstruct) {
          constexpr std::size_t kMaxLlamaInstructToolRounds = 8U;
          std::size_t tool_rounds = 0;
          std::unordered_set<std::string> successful_tool_call_keys;
          while (tool_rounds < kMaxLlamaInstructToolRounds) {
            std::vector<qt::ai::ToolCallContent> calls;
            for (const auto& content : completion.message.content) {
              if (const auto* call =
                      std::get_if<qt::ai::ToolCallContent>(&content)) {
                calls.push_back(*call);
              }
            }
            if (calls.empty()) {
              break;
            }

            executed_tool = true;
            ++tool_rounds;
            std::vector<qt::ai::Message> next_messages;
            Json round_tool_results = Json::array();
            bool suppressed_repeated_call = false;
            for (const auto& call : calls) {
              auto request = tool_request_from_call(call);
              if (request.tool_name == "search_text" &&
                  request.arguments.is_object() &&
                  !request.arguments.contains("query") &&
                  !request.arguments.contains("pattern")) {
                if (auto inferred = infer_search_text_arguments(text)) {
                  if (request.arguments.contains("path")) {
                    (*inferred)["path"] = request.arguments["path"];
                  }
                  request.arguments = std::move(*inferred);
                }
              }
              const std::string key = request.tool_name + ":" +
                                      compact_transcript_json(request.arguments);
              if (successful_tool_call_keys.count(key) != 0U) {
                response.transcript_lines.push_back(
                    "tool_loop: repeated tool call suppressed: " +
                    request.tool_name + " " +
                    compact_transcript_json(request.arguments));
                suppressed_repeated_call = true;
                continue;
              }
              response.transcript_lines.push_back(
                  "tool_call: " + call.name + " " + call.arguments.dump());
              auto tool_context = runtime->services.tool_context;
              const auto result = invoke_interactive_tool_request(
                  request, runtime->services.tool_registry, tool_context);
              if (qt::tools::is_success(result)) {
                successful_tool_call_keys.insert(key);
              }
              response.transcript_lines.push_back(
                  std::string("tool_result: ") +
                  qt::tools::to_string(result.status) +
                  (result.error_message ? " - " + *result.error_message : ""));
              Json result_record = Json{{"tool", request.tool_name},
                                        {"arguments", request.arguments},
                                        {"status", qt::tools::to_string(result.status)},
                                        {"output", result.output}};
              if (result.error_message) {
                result_record["error"] = *result.error_message;
              }
              round_tool_results.push_back(result_record);
              executed_tool_results.push_back(std::move(result_record));
              next_messages.push_back(tool_result_message(request, result));
            }
            if (suppressed_repeated_call && next_messages.empty()) {
              stopped_tool_loop = true;
              break;
            }

            qt::ai::Message follow_up_instruction;
            follow_up_instruction.role = qt::ai::MessageRole::user;
            std::string result_summary =
                truncate_transcript_text(compact_transcript_json(round_tool_results),
                                         6000U);
            follow_up_instruction.content = {qt::ai::TextContent{
                "Use the tool result above to continue the original request. "
                "If more information is needed, request another framework tool. "
                "Do not repeat an exact tool call that has already succeeded. "
                "If enough information is available, answer in normal text.\n"
                "Tool results for this step: " +
                    result_summary,
                std::nullopt}};
            next_messages.push_back(std::move(follow_up_instruction));
            llama_request.messages = std::move(next_messages);

            try {
              completion =
                  llama_provider->run_tagged(*tag, llama_request, options);
              append_context_pressure_line(response.transcript_lines,
                                           completion);
            } catch (const std::exception& error) {
              response.transcript_lines.push_back(
                  std::string("Llama instruct follow-up failed: ") +
                  error.what());
              break;
            }
          }
          if (tool_rounds == kMaxLlamaInstructToolRounds &&
              completion.stop_reason == qt::ai::StopReason::tool_call) {
            stopped_tool_loop = true;
            response.transcript_lines.push_back(
                "Llama instruct tool loop stopped after " +
                std::to_string(kMaxLlamaInstructToolRounds) + " rounds.");
          }
        }
        const std::string output = completion_text(completion);
        if (!stopped_tool_loop && (!executed_tool || !output.empty())) {
          response.transcript_lines.push_back(
              output.empty() ? std::string("(llama returned no text)") : output);
        }
        if (executed_tool && (output.empty() || stopped_tool_loop) &&
            !executed_tool_results.empty()) {
          response.transcript_lines.push_back(
              "tool_output: " +
              truncate_transcript_text(compact_transcript_json(executed_tool_results),
                                       4000U));
        }
        if (*tag == qt::ai::LlamaInstanceTag::kInstruct) {
          response.transcript_lines.push_back(
              "tools_available: " + std::to_string(llama_request.tools.size()));
        }
        return response;
      }

      if (subcommand == "embeddings") {
        if (!llama_provider || !has_llama_selection) {
          response.transcript_lines = {
              "Llama embeddings are unavailable until a llama model is selected and loaded."};
          return response;
        }
        const std::string text = trim_ascii_copy(llama_args.second);
        if (text.empty()) {
          return qt::foundation::Result<
              qt::app::InteractiveSlashCommandResponse>::failure(
              qt::foundation::ErrorCode::invalid_argument,
              "Usage: /llama embeddings <text>");
        }

        qt::ai::LlamaEmbeddingRequest embedding_request;
        embedding_request.model_id = *selected_llama_model_id;
        embedding_request.text = text;
        qt::ai::LlamaEmbeddingResult embedding;
        try {
          embedding = llama_provider->embed(embedding_request);
        } catch (const std::exception& error) {
          response.transcript_lines = {
              std::string("Llama embedding failed: ") + error.what()};
          return response;
        }
        std::ostringstream preview;
        const std::size_t preview_count =
            std::min<std::size_t>(embedding.embedding.size(), 3U);
        for (std::size_t index = 0; index < preview_count; ++index) {
          if (index > 0) {
            preview << ", ";
          }
          preview << embedding.embedding[index];
        }
        response.transcript_lines = {
            "Llama embedding generated.",
            "dimensions: " + std::to_string(embedding.embedding.size()),
            "input_tokens: " + std::to_string(embedding.usage.input_tokens),
            "preview: [" + preview.str() +
                (embedding.embedding.size() > preview_count ? ", ...]" : "]"),
        };
        return response;
      }

      if (subcommand == "prompt") {
        if (!llama_provider || !has_llama_selection) {
          response.transcript_lines = {
              "Llama prompt is unavailable until a llama model is selected."};
          return response;
        }
        const std::string prompt = trim_ascii_copy(llama_args.second);
        if (prompt.empty()) {
          return qt::foundation::Result<
              qt::app::InteractiveSlashCommandResponse>::failure(
              qt::foundation::ErrorCode::invalid_argument,
              "Usage: /llama prompt <text>");
        }

        qt::ai::CompletionRequest llama_request;
        llama_request.model = qt::ai::ModelRef{qt::ai::kLlamaProviderId,
                                               qt::ai::kLlamaProviderApi,
                                               *selected_llama_model_id};
        qt::ai::Message message;
        message.role = qt::ai::MessageRole::user;
        message.content = {qt::ai::TextContent{prompt, std::nullopt}};
        llama_request.messages = {std::move(message)};
        auto options = runtime->host.harness().agent().provider_options();
        try {
          qt::ai::LlamaKvCacheOperation clear_cache;
          clear_cache.kind = qt::ai::LlamaKvCacheOperationKind::kClear;
          (void)llama_provider->kv_cache_control(clear_cache);
        } catch (const std::exception&) {
        }

        qt::ai::CompletionResponse completion;
        try {
          completion = llama_provider->complete(llama_request, options);
        } catch (const std::exception& error) {
          response.transcript_lines = {
              std::string("Llama prompt failed: ") + error.what()};
          return response;
        }

        std::string text;
        for (const auto& content : completion.message.content) {
          if (const auto* text_content =
                  std::get_if<qt::ai::TextContent>(&content)) {
            text += text_content->text;
          }
        }
        response.transcript_lines = {
            text.empty() ? std::string("(llama returned no text)") : text};
        return response;
      }

      if (subcommand == "gpu") {
        auto options = runtime->host.harness().agent().provider_options();
        if (!options.metadata.is_object()) {
          options.metadata = qt::ai::Json::object();
        }
        if (!options.metadata.contains("llama") ||
            !options.metadata["llama"].is_object()) {
          options.metadata["llama"] = qt::ai::Json::object();
        }

        const std::string raw = trim_ascii_copy(llama_args.second);
        const auto pending_it = options.metadata["llama"].find("pendingBackend");
        const qt::ai::Json pending =
            pending_it == options.metadata["llama"].end()
                ? qt::ai::Json::object()
                : *pending_it;
        if (raw.empty() || lowercase_ascii(raw) == "status") {
          response.transcript_lines = {
              "gpu_applied: gpuLayers=" +
                  optional_int(llama.backend.gpu_layers, "default") +
                  ", mainGpu=" + optional_int(llama.backend.main_gpu, "default") +
                  ", splitMode=" + llama.backend.split_mode.value_or("default") +
                  ", offloadKqv=" +
                  optional_bool(llama.backend.offload_kqv, "default"),
              "gpu_pending: " + pending.dump(),
          };
          return response;
        }

        const auto parse_bool = [](const std::string& value)
            -> std::optional<bool> {
          const std::string normalized = lowercase_ascii(value);
          if (normalized == "1" || normalized == "true" ||
              normalized == "yes" || normalized == "on") {
            return true;
          }
          if (normalized == "0" || normalized == "false" ||
              normalized == "no" || normalized == "off") {
            return false;
          }
          return std::nullopt;
        };
        const auto parse_int = [](const std::string& value)
            -> std::optional<int> {
          try {
            std::size_t consumed = 0;
            const int parsed_value = std::stoi(value, &consumed, 10);
            if (consumed != value.size()) {
              return std::nullopt;
            }
            return parsed_value;
          } catch (const std::exception&) {
            return std::nullopt;
          }
        };

        qt::ai::Json next_pending = pending.is_object()
                                        ? pending
                                        : qt::ai::Json::object();
        std::string remaining = raw;
        while (!remaining.empty()) {
          const auto token = split_first_ascii_argument(remaining);
          remaining = token.second;
          const auto equals = token.first.find('=');
          if (equals == std::string::npos || equals == 0 ||
              equals + 1 >= token.first.size()) {
            return qt::foundation::Result<
                qt::app::InteractiveSlashCommandResponse>::failure(
                qt::foundation::ErrorCode::invalid_argument,
                "Usage: /llama gpu [gpuLayers=<n>] [mainGpu=<n>] [splitMode=<none|layer|row>] [offloadKqv=<bool>]");
          }
          const std::string key = token.first.substr(0, equals);
          const std::string value = token.first.substr(equals + 1);
          if (key == "gpuLayers" || key == "mainGpu") {
            const auto parsed_int = parse_int(value);
            if (!parsed_int || (key == "mainGpu" && *parsed_int < 0)) {
              return qt::foundation::Result<
                  qt::app::InteractiveSlashCommandResponse>::failure(
                  qt::foundation::ErrorCode::invalid_argument,
                  key + " must be an integer" +
                      (key == "mainGpu" ? std::string(" >= 0") : std::string{}));
            }
            next_pending[key] = *parsed_int;
          } else if (key == "splitMode") {
            const std::string normalized = lowercase_ascii(value);
            if (normalized != "none" && normalized != "layer" &&
                normalized != "row") {
              return qt::foundation::Result<
                  qt::app::InteractiveSlashCommandResponse>::failure(
                  qt::foundation::ErrorCode::invalid_argument,
                  "splitMode must be one of: none, layer, row");
            }
            next_pending[key] = normalized;
          } else if (key == "offloadKqv" || key == "useMmap" ||
                     key == "useMlock") {
            const auto parsed_bool = parse_bool(value);
            if (!parsed_bool) {
              return qt::foundation::Result<
                  qt::app::InteractiveSlashCommandResponse>::failure(
                  qt::foundation::ErrorCode::invalid_argument,
                  key + " must be a boolean");
            }
            next_pending[key] = *parsed_bool;
          } else {
            return qt::foundation::Result<
                qt::app::InteractiveSlashCommandResponse>::failure(
                qt::foundation::ErrorCode::invalid_argument,
                "Unsupported llama GPU setting: " + key);
          }
        }

        options.metadata["llama"]["pendingBackend"] = next_pending;
        runtime->host.harness().set_stream_options(std::move(options));
        response.transcript_lines = {
            "Llama GPU/backend settings updated for the next reload.",
            "pending: " + next_pending.dump(),
        };
        return response;
      }

      response.transcript_lines = {
          "Llama command planned: " + subcommand,
          llama_args.second.empty() ? std::string("arguments: (none)")
                                    : "arguments: " + llama_args.second,
          "Provider runtime support for this command is pending."};
      return response;
    }

    if (event.command == "copy") {
      response.handled = true;
      response.append_default_output = false;
      std::string last_text;
      const auto& transcript = runtime->host.harness().agent().state().transcript;
      for (auto it = transcript.rbegin(); it != transcript.rend(); ++it) {
        if (it->role == qt::ai::MessageRole::assistant) {
          last_text = render_message_text(*it);
          break;
        }
      }
      if (last_text.empty()) {
        response.transcript_lines = {"No assistant text is available yet."};
      } else {
        if (!qt::tools::is_valid_clipboard_text(last_text)) {
          response.transcript_lines = {"Last assistant text:", last_text};
          return response;
        }
        const auto available_commands =
            qt::tools::detect_available_clipboard_commands();
        const auto command =
            qt::tools::select_clipboard_copy_command(available_commands);
        if (!command) {
          response.transcript_lines = {"Last assistant text:", last_text};
          response.transcript_lines.push_back(
              "No supported clipboard command is available.");
          return response;
        }

        const auto temp_path =
            std::filesystem::temp_directory_path() / "qt-clipboard-copy.txt";
        const auto written = qt::foundation::write_text_file(temp_path, last_text);
        if (!written) {
          return written.error();
        }

        std::ostringstream shell_command;
        shell_command << "cat '" << temp_path.generic_string() << "' | "
                      << command->executable;
        for (const auto& argument : command->arguments) {
          shell_command << " '" << argument << "'";
        }
        const auto copied = qt::foundation::run_shell_command(shell_command.str());
        std::error_code remove_error;
        std::filesystem::remove(temp_path, remove_error);
        if (!copied) {
          return copied.error();
        }
        if (copied.value().exit_code != 0) {
          return qt::foundation::Result<
              qt::app::InteractiveSlashCommandResponse>::failure(
              qt::foundation::ErrorCode::io_error,
              copied.value().stderr_text.empty() ? "clipboard copy failed"
                                                 : copied.value().stderr_text);
        }
        response.transcript_lines = {
            "Copied the last assistant message to the clipboard."};
      }
      return response;
    }

    if (event.command == "thinking") {
      response.handled = true;
      response.append_default_output = false;
      const auto apply_level =
          [&](std::string level) -> qt::foundation::Status {
        if (!is_valid_thinking_level(level)) {
          return qt::foundation::Status::failure(
              qt::foundation::ErrorCode::invalid_argument,
              "unsupported thinking level: " + level);
        }
        if (const auto* record = qt::ai::find_model_record(
                runtime->services.model_catalog,
                runtime->host.harness().model().provider,
                runtime->host.harness().model().name);
            record != nullptr) {
          level = qt::ai::clamp_thinking_level(*record, level);
        }
        const auto before_leaf = runtime->host.session_manager().get_leaf_id();
        runtime->host.harness().set_thinking_level(std::move(level));
        runtime->host.sync_session_manager_from_harness();
        return persist_leaf_if_needed(before_leaf);
      };

      if (!event.arguments.empty()) {
        const auto status = apply_level(event.arguments);
        if (!status) {
          return status.error();
        }
        response.transcript_lines = {"Thinking level set to " +
                                     runtime->host.harness().thinking_level()};
        return response;
      }

      auto levels = qt::ai::supported_thinking_levels(
          runtime->services.model_selection.model);
      if (levels.empty()) {
        levels = {"off", "minimal", "low", "medium", "high", "xhigh"};
      }
      auto selector = std::make_shared<qt::tui::ThinkingSelectorComponent>(
          runtime->host.harness().thinking_level(), std::move(levels),
          [runtime, &persist_leaf_if_needed, &ui, &cached_footer_lines,
           &compute_footer_lines](std::string level) {
            qt::foundation::Status status = qt::foundation::Status::success();
            if (!is_valid_thinking_level(level)) {
              status = qt::foundation::Status::failure(
                  qt::foundation::ErrorCode::invalid_argument,
                  "unsupported thinking level: " + level);
            } else {
              const auto before_leaf =
                  runtime->host.session_manager().get_leaf_id();
              runtime->host.harness().set_thinking_level(std::move(level));
              runtime->host.sync_session_manager_from_harness();
              status = persist_leaf_if_needed(before_leaf);
            }
            ui.hide_overlay();
            if (!status) {
              ui.append_transcript_line("error: " + status.error().message);
              return;
            }
            cached_footer_lines = compute_footer_lines();
            ui.append_transcript_line("Thinking level set to " +
                                      runtime->host.harness().thinking_level());
          },
          [&ui]() { ui.hide_overlay(); });
      ui.show_overlay(selector);
      return response;
    }

    if (event.command == "plan") {
      response.handled = true;
      response.append_default_output = false;

      // Plan mode is tracked as a session-level toggle.
      // When active, the system prompt includes plan mode instructions
      // that restrict the agent to non-mutating operations.
      static bool plan_mode_active = false;

      if (!event.arguments.empty()) {
        const std::string arg = event.arguments;
        if (arg == "on" || arg == "enable" || arg == "true") {
          plan_mode_active = true;
          response.transcript_lines = {
              "Plan mode enabled.",
              "The agent will now work in planning mode: exploring, analyzing, "
              "and producing detailed plans without making changes.",
              "Use /plan off to return to normal mode."};
          return response;
        }
        if (arg == "off" || arg == "disable" || arg == "false") {
          plan_mode_active = false;
          response.transcript_lines = {
              "Plan mode disabled.",
              "The agent will now work in normal mode and can make changes."};
          return response;
        }
        response.transcript_lines = {
            "Usage: /plan [on|off]",
            "  /plan       - Toggle plan mode",
            "  /plan on    - Enable plan mode",
            "  /plan off   - Disable plan mode"};
        return response;
      }

      // Toggle
      plan_mode_active = !plan_mode_active;
      if (plan_mode_active) {
        response.transcript_lines = {
            "Plan mode enabled.",
            "The agent will now work in planning mode: exploring, analyzing, "
            "and producing detailed plans without making changes.",
            "Use /plan off to return to normal mode."};
      } else {
        response.transcript_lines = {
            "Plan mode disabled.",
            "The agent will now work in normal mode and can make changes."};
      }
      return response;
    }

    if (event.command == "model") {
      response.handled = true;
      response.append_default_output = false;

      if (lowercase_ascii(event.arguments) == "refresh") {
        if (parsed.offline) {
          response.transcript_lines = {
              "Model refresh is unavailable in offline mode"};
        } else {
          const bool started_openrouter = start_openrouter_model_refresh(true, true);
          const bool started_openai = start_openai_model_refresh(true, true);
          openrouter_models.announce_completion = true;
          openai_models.announce_completion = true;
          response.transcript_lines = {started_openrouter || started_openai
              ? "Refreshing OpenAI and OpenRouter model catalogs…"
              : "Model catalog refresh is already in progress"};
        }
        return response;
      }

      const auto apply_model =
          [&](const qt::ai::ModelCatalogModelRecord& model)
          -> qt::foundation::Status {
        auto provider_selection = qt::app::make_runtime_provider_selection(
            runtime->services, parsed, model.ref.provider, model.ref.name);
        if (!provider_selection) {
          return qt::foundation::Status(provider_selection.error());
        }

        const auto before_leaf = runtime->host.session_manager().get_leaf_id();
        runtime->services.model_selection =
            std::move(provider_selection.value().model_selection);
        runtime->using_echo_provider = provider_selection.value().using_echo_provider;
        runtime->host.harness().set_model(
            runtime->services.model_selection.model.ref,
            std::move(provider_selection.value().provider),
            std::move(provider_selection.value().options));
        runtime->services.settings.default_provider =
            runtime->services.model_selection.model.ref.provider;
        runtime->services.settings.default_model =
            runtime->services.model_selection.model.ref.name;
        if (runtime->services.model_selection.model.ref.provider ==
            qt::ai::kLlamaProviderId) {
          runtime->services.selected_llama_model_id =
              runtime->services.model_selection.model.ref.name;
          runtime->services.settings.llama.selected_model_id =
              runtime->services.model_selection.model.ref.name;
        }
        runtime->host.sync_session_manager_from_harness();
        const auto persisted = persist_leaf_if_needed(before_leaf);
        if (!persisted) {
          return persisted;
        }
        const auto settings_persisted =
            persist_model_selection(runtime->services.model_selection.model.ref);
        if (!settings_persisted) {
          return settings_persisted;
        }
        return qt::foundation::Status::success();
      };

      if (const auto* exact = exact_model(event.arguments); exact != nullptr) {
        if (!model_is_available(runtime->services.model_catalog,
                                runtime->services.auth_storage, *exact)) {
          return qt::foundation::Result<
              qt::app::InteractiveSlashCommandResponse>::failure(
              qt::foundation::ErrorCode::invalid_argument,
              "model is not available; run /login or configure credentials: " +
                  exact->ref.provider + "/" + exact->ref.name);
        }
        const auto status = apply_model(*exact);
        if (!status) {
          return status.error();
        }
        response.transcript_lines = {"Model set to " + exact->ref.provider + "/" +
                                     exact->ref.name};
        return response;
      }

      auto selector = std::make_shared<qt::tui::ModelSelectorComponent>(
          build_model_selector_items(),
          [runtime, &parsed, &persist_leaf_if_needed, &persist_model_selection,
           &ui, &cached_footer_lines, &compute_footer_lines](
              qt::tui::ModelSelectorItem item) {
        const auto* model =
            qt::ai::find_model_record(runtime->services.model_catalog,
                                      item.provider, item.id);
        if (model == nullptr) {
          ui.hide_overlay();
          ui.append_transcript_line("error: model not found: " +
                                    item.provider + "/" + item.id);
          return;
        }
        auto provider_selection = qt::app::make_runtime_provider_selection(
            runtime->services, parsed, model->ref.provider, model->ref.name);
        if (!provider_selection) {
          ui.hide_overlay();
          ui.append_transcript_line("error: " +
                                    provider_selection.error().message);
          return;
        }

        const auto before_leaf = runtime->host.session_manager().get_leaf_id();
        runtime->services.model_selection =
            std::move(provider_selection.value().model_selection);
        runtime->using_echo_provider = provider_selection.value().using_echo_provider;
        runtime->host.harness().set_model(
            runtime->services.model_selection.model.ref,
            std::move(provider_selection.value().provider),
            std::move(provider_selection.value().options));
        runtime->services.settings.default_provider =
            runtime->services.model_selection.model.ref.provider;
        runtime->services.settings.default_model =
            runtime->services.model_selection.model.ref.name;
        if (runtime->services.model_selection.model.ref.provider ==
            qt::ai::kLlamaProviderId) {
          runtime->services.selected_llama_model_id =
              runtime->services.model_selection.model.ref.name;
          runtime->services.settings.llama.selected_model_id =
              runtime->services.model_selection.model.ref.name;
        }
        runtime->host.sync_session_manager_from_harness();
        auto status = persist_leaf_if_needed(before_leaf);
        if (status) {
          status =
              persist_model_selection(runtime->services.model_selection.model.ref);
        }
        ui.hide_overlay();
        if (!status) {
          ui.append_transcript_line("error: " + status.error().message);
          return;
        }
        cached_footer_lines = compute_footer_lines();
        ui.append_transcript_line("Model set to " + item.provider + "/" +
                                  item.id);
          },
          [&ui]() { ui.hide_overlay(); }, event.arguments);
      if (selector->filtered_models().empty()) {
        return qt::foundation::Result<
            qt::app::InteractiveSlashCommandResponse>::failure(
            qt::foundation::ErrorCode::not_found,
            event.arguments.empty()
                ? "no models available; run /login or configure credentials"
                : "no available models matched: " + event.arguments);
      }

      active_model_selector = selector;
      ui.show_overlay(selector);
      return response;
    }

    if (event.command == "scoped-models") {
      response.handled = true;
      response.append_default_output = false;
      std::vector<std::string> enabled_models =
          runtime->services.settings.enabled_models;
      if (enabled_models.empty()) {
        for (const auto& model : runtime->services.model_catalog.models) {
          if (!model_is_available(runtime->services.model_catalog,
                                  runtime->services.auth_storage, model)) {
            continue;
          }
          enabled_models.push_back(model.ref.provider + "/" + model.ref.name);
        }
      }

      auto is_enabled = [&](const std::string& id) {
        return std::find(enabled_models.begin(), enabled_models.end(), id) !=
               enabled_models.end();
      };

      std::vector<qt::tui::SettingItem> items;
      for (const auto& model : runtime->services.model_catalog.models) {
        if (!model_is_available(runtime->services.model_catalog,
                                runtime->services.auth_storage, model)) {
          continue;
        }
        const std::string id = model.ref.provider + "/" + model.ref.name;
        items.push_back({id,
                         id,
                         model.display_name,
                         is_enabled(id) ? std::string("on") : std::string("off"),
                         {"off", "on"},
                         {}});
      }

      auto settings = std::make_shared<qt::tui::SettingsList>(
          std::move(items), 12U, qt::tui::SettingsListTheme{},
          qt::tui::SettingsListOptions{true});
      settings->set_change_callback([&, settings](const std::string& id,
                                                  const std::string& value) {
        auto& configured = runtime->services.settings.enabled_models;
        if (configured.empty()) {
          for (const auto& model : runtime->services.model_catalog.models) {
            if (!model_is_available(runtime->services.model_catalog,
                                    runtime->services.auth_storage, model)) {
              continue;
            }
            configured.push_back(model.ref.provider + "/" + model.ref.name);
          }
        }
        configured.erase(
            std::remove(configured.begin(), configured.end(), id),
            configured.end());
        if (value == "on") {
          configured.push_back(id);
          std::sort(configured.begin(), configured.end());
          configured.erase(std::unique(configured.begin(), configured.end()),
                           configured.end());
        }
        const auto persisted = persist_enabled_models(configured);
        if (!persisted) {
          ui.append_transcript_line("error: " + persisted.error().message);
        }
      });
      settings->set_cancel_callback([&ui]() { ui.hide_overlay(); });
      ui.show_overlay(settings);
      return response;
    }

    if (event.command == "name") {
      response.handled = true;
      response.append_default_output = false;
      if (event.arguments.empty()) {
        const auto name = runtime->host.session_manager().get_session_name();
        response.transcript_lines = {"Session name: " +
                                     name.value_or(std::string("(unnamed)"))};
        return response;
      }
      const auto entry_id = runtime->host.harness().set_session_name(event.arguments);
      runtime->host.sync_session_manager_from_harness();
      if (current_session->persistent && current_session->repository_path &&
          entry_id) {
        const auto* entry = runtime->host.session_manager().get_entry(*entry_id);
        if (entry != nullptr) {
          const auto persisted =
              qt::harness::SessionRepository(*current_session->repository_path)
                  .append(*entry);
          if (!persisted) {
            return persisted.error();
          }
        }
      }
      response.transcript_lines = {"Session name set to " + event.arguments};
      return response;
    }

    if (event.command == "import") {
      response.handled = true;
      response.append_default_output = false;
      const auto path = parse_optional_command_path(event.arguments);
      if (!path) {
        return qt::foundation::Result<
            qt::app::InteractiveSlashCommandResponse>::failure(
            qt::foundation::ErrorCode::invalid_argument,
            "Usage: /import <path.jsonl>");
      }

      const auto show_missing_cwd_prompt =
          [&, path](ReferenceStartupSession opened_session,
                    ReferenceMissingSessionCwdIssue issue) {
            std::vector<qt::tui::SelectItem> items{
                {"continue", "Continue in current cwd", issue.fallback_cwd},
                {"cancel", "Cancel import", "Keep the current session"}};
            auto select = std::make_shared<qt::tui::Select>(
                std::optional<std::string>{"Stored session cwd does not exist"},
                std::move(items), 0, 6U, kReferenceInteractiveSelectLayout);
            select->set_select_callback(
                [&, select, opened_session = std::move(opened_session), issue,
                 path](const qt::tui::SelectItem& item) mutable {
                  ui.hide_overlay();
                  if (item.value != "continue") {
                    ui.append_transcript_line("Import cancelled.");
                    return;
                  }

                  auto next_session = opened_session;
                  next_session.working_directory = issue.fallback_cwd;
                  std::string note = "Imported session from " + *path +
                                     ". Using current cwd: " +
                                     issue.fallback_cwd;
                  const auto status =
                      switch_to_session(ui, next_session, std::move(note));
                  if (!status) {
                    ui.append_transcript_line("error: " +
                                              status.error().message);
                  }
                });
            select->set_cancel_callback([&ui]() { ui.hide_overlay(); });
            ui.show_overlay(select);
          };

      auto confirm = std::make_shared<qt::tui::Select>(
          std::optional<std::string>{"Import session"},
          std::vector<qt::tui::SelectItem>{
              {"continue", "Replace current session", *path},
              {"cancel", "Cancel import", "Keep the current session"},
          },
          0, 6U, kReferenceInteractiveSelectLayout);
      confirm->set_select_callback([&, confirm, path](const qt::tui::SelectItem& item) {
        ui.hide_overlay();
        if (item.value != "continue") {
          ui.append_transcript_line("Import cancelled.");
          return;
        }

        auto opened = open_startup_session(*path, "import");
        if (!opened) {
          ui.append_transcript_line("error: " + opened.error().message);
          return;
        }

        if (const auto issue = missing_session_cwd_issue(
                opened.value(), cwd);
            issue) {
          show_missing_cwd_prompt(opened.value(), *issue);
          return;
        }

        const auto status = switch_to_session(
            ui, opened.value(), "Imported session from " + *path + ".");
        if (!status) {
          ui.append_transcript_line("error: " + status.error().message);
        }
      });
      confirm->set_cancel_callback([&ui]() { ui.hide_overlay(); });
      ui.show_overlay(confirm);
      return response;
    }

    if (event.command == "reload") {
      response.handled = true;
      response.append_default_output = false;
      auto reloaded_session = *current_session;
      reloaded_session.mode = "reload";
      const auto status =
          switch_to_session(ui, reloaded_session, "Runtime resources reloaded.");
      if (!status) {
        return status.error();
      }
      return response;
    }

    if (event.command == "new") {
      response.handled = true;
      response.append_default_output = false;
      const auto created = create_startup_session(session_dir, "create");
      if (!created) {
        return created.error();
      }
      const auto status =
          switch_to_session(ui, created.value(), "Started a new session.");
      if (!status) {
        return status.error();
      }
      return response;
    }

    if (event.command == "clone") {
      response.handled = true;
      response.append_default_output = false;
      const auto cloned =
          clone_startup_session(*current_session, session_dir);
      if (!cloned) {
        return cloned.error();
      }
      const auto status =
          switch_to_session(ui, cloned.value(), "Cloned to new session.");
      if (!status) {
        return status.error();
      }
      return response;
    }

    if (event.command == "fork") {
      response.handled = true;
      response.append_default_output = false;
      if (!current_session->repository_path) {
        return qt::foundation::Result<
            qt::app::InteractiveSlashCommandResponse>::failure(
            qt::foundation::ErrorCode::invalid_argument,
            "Cannot fork session: no active session file");
      }

      const auto messages =
          session_fork_messages(runtime->host.session_manager());
      if (messages.empty()) {
        response.transcript_lines = {"No messages to fork from."};
        return response;
      }

      std::vector<qt::tui::SelectItem> items;
      items.reserve(messages.size());
      std::size_t selected_index = messages.size() - 1U;
      for (const auto& message : messages) {
        std::string label = message.text;
        const auto line_break = label.find('\n');
        if (line_break != std::string::npos) {
          label = label.substr(0, line_break);
        }
        if (label.size() > 72U) {
          label = label.substr(0, 69U) + "...";
        }
        items.push_back(
            qt::tui::SelectItem{message.entry_id, label, message.text});
      }

      auto select = std::make_shared<qt::tui::Select>(
          std::optional<std::string>{"Fork from user message"}, std::move(items),
          selected_index, 12U, kReferenceInteractiveSelectLayout);
      select->set_select_callback(
          [&, select, messages](const qt::tui::SelectItem& item) {
            ui.hide_overlay();
            const auto it = std::find_if(
                messages.begin(), messages.end(),
                [&](const ReferenceRpcForkMessage& message) {
                  return message.entry_id == item.value;
                });
            if (it == messages.end()) {
              ui.append_transcript_line(
                  "error: selected fork message was not found");
              return;
            }
            const auto forked = fork_startup_session(
                *current_session->repository_path, session_dir, item.value);
            if (!forked) {
              ui.append_transcript_line("error: " + forked.error().message);
              return;
            }
            const auto status =
                switch_to_session(ui, forked.value(), "Forked to new session.");
            if (!status) {
              ui.append_transcript_line("error: " + status.error().message);
              return;
            }
            ui.mutable_editor().set_text(it->text);
          });
      select->set_cancel_callback([&ui]() { ui.hide_overlay(); });
      ui.show_overlay(select);
      return response;
    }

    if (event.command == "resume" || event.command == "tree") {
      response.handled = true;
      response.append_default_output = false;
      if (event.command == "tree") {
        std::function<void(std::optional<std::string>)> show_tree_selector;
        show_tree_selector = [&](std::optional<std::string> selection_hint) {
          const auto tree_items = build_session_tree_items(
              runtime->host.session_manager(), selection_hint);
          if (!tree_items) {
            ui.append_transcript_line("error: " + tree_items.error().message);
            return;
          }
          if (tree_items.value().empty()) {
            ui.append_transcript_line("No session tree entries available.");
            return;
          }

          std::vector<qt::tui::SelectItem> items;
          items.reserve(tree_items.value().size());
          std::size_t selected_index = 0U;
          for (std::size_t index = 0; index < tree_items.value().size();
               ++index) {
            const auto& item = tree_items.value()[index];
            items.push_back(
                qt::tui::SelectItem{item.entry_id, item.label, item.entry_id});
            if (item.selected) {
              selected_index = index;
            }
          }

          auto select = std::make_shared<qt::tui::Select>(
              std::optional<std::string>{"Session tree"}, std::move(items),
              selected_index, 12U, kReferenceInteractiveSelectLayout);
          select->set_select_callback(
              [&, select](const qt::tui::SelectItem& item) {
                const auto current_leaf =
                    runtime->host.session_manager().get_leaf_id();
                if (current_leaf ==
                    std::optional<std::string>(item.value)) {
                  ui.hide_overlay();
                  ui.append_transcript_line("Already at this point.");
                  return;
                }

                const auto selection = make_session_tree_selection(
                    runtime->host.session_manager(), item.value);
                if (!selection) {
                  ui.hide_overlay();
                  ui.append_transcript_line("error: " +
                                            selection.error().message);
                  return;
                }

                const auto navigate =
                    [&, selection_value = selection.value(),
                     entry_id = item.value](ReferenceSessionTreeSummaryOptions
                                                options) {
                      const auto applied =
                          apply_session_tree_selection(
                              runtime->host, selection_value,
                              std::move(options));
                      if (!applied) {
                        ui.append_transcript_line("error: " +
                                                  applied.error().message);
                        return;
                      }
                      seed_transcript(ui);
                      ui.mutable_editor().set_text(
                          applied.value().editor_text.value_or(std::string{}));
                      if (applied.value().summary_entry_id.has_value()) {
                        ui.append_transcript_line(
                            "Moved to session entry " + entry_id +
                            " with a branch summary.");
                      } else {
                        ui.append_transcript_line("Moved to session entry " +
                                                  entry_id + ".");
                      }
                    };

                const bool has_old_leaf = current_leaf.has_value();
                if (!has_old_leaf) {
                  ui.hide_overlay();
                  navigate({});
                  return;
                }

                auto summary_select = std::make_shared<qt::tui::Select>(
                    std::optional<std::string>{"Summarize branch?"},
                    std::vector<std::string>{"No summary",
                                             "Summarize",
                                             "Summarize with custom prompt"},
                    0U);
                summary_select->set_select_callback(
                    [&, summary_select, navigate,
                     entry_id = item.value](const qt::tui::SelectItem& choice) {
                      ui.hide_overlay();
                      if (choice.value == "No summary") {
                        navigate({});
                        return;
                      }
                      if (choice.value == "Summarize") {
                        ReferenceSessionTreeSummaryOptions options;
                        options.summarize = true;
                        navigate(std::move(options));
                        return;
                      }

                      auto prompt = std::make_shared<PromptOverlay>(
                          "Custom summarization instructions", "> ",
                          "Focus the branch summary before navigating.");
                      prompt->set_submit_callback(
                          [&, navigate](const std::string& value) {
                            ui.hide_overlay();
                            ReferenceSessionTreeSummaryOptions options;
                            options.summarize = true;
                            options.custom_instructions = value;
                            navigate(std::move(options));
                          });
                      prompt->set_cancel_callback([&ui]() {
                        ui.hide_overlay();
                      });
                      ui.show_overlay(prompt);
                    });
                summary_select->set_cancel_callback(
                    [&, entry_id = item.value]() {
                      ui.hide_overlay();
                      show_tree_selector(entry_id);
                    });
                ui.show_overlay(summary_select);
              });
          select->set_cancel_callback([&ui]() { ui.hide_overlay(); });
          ui.show_overlay(select);
        };
        show_tree_selector(event.arguments.empty()
                               ? std::nullopt
                               : std::optional<std::string>(event.arguments));
        return response;
      }

      const auto catalog = qt::harness::load_session_catalog(session_dir);
      if (!catalog) {
        return catalog.error();
      }
      std::vector<qt::tui::SessionSelectorSearchItem> items;
      for (const auto& record : catalog.value().session_records) {
        const auto& snapshot = record.snapshot;
        items.push_back(qt::tui::SessionSelectorSearchItem{
            snapshot.metadata.session_id,
            snapshot.metadata.display_name.value_or(
                snapshot.metadata.session_id),
            record.repository_path});
      }

      if (items.empty()) {
        response.transcript_lines = {"No sessions available."};
        return response;
      }

      auto selector = std::make_shared<qt::tui::SessionSelectorSearchComponent>(
          std::move(items),
          [&](qt::tui::SessionSelectorSearchItem item) {
        ui.hide_overlay();
        const auto opened = open_startup_session(item.path, "resume");
        if (!opened) {
          ui.append_transcript_line("error: " + opened.error().message);
          return;
        }
        const auto status =
            switch_to_session(ui, opened.value(),
                              "Loaded session " + item.session_id + ".");
        if (!status) {
          ui.append_transcript_line("error: " + status.error().message);
        }
          },
          [&ui]() { ui.hide_overlay(); });
      ui.show_overlay(selector);
      return response;
    }

    if (event.command == "tools") {
      response.handled = true;
      response.append_default_output = false;
      const auto active_tool_names = runtime->host.harness().active_tool_names();
      auto is_active = [&](const std::string& tool_name) {
        return std::find(active_tool_names.begin(), active_tool_names.end(),
                         tool_name) != active_tool_names.end();
      };

      std::vector<qt::tui::SettingItem> items;
      for (const auto& tool : runtime->services.tool_definitions) {
        if (!event.arguments.empty() &&
            tool.name.find(event.arguments) == std::string::npos) {
          continue;
        }
        items.push_back({tool.name,
                         tool.name,
                         tool.description,
                         is_active(tool.name) ? std::string("on")
                                              : std::string("off"),
                         {"off", "on"},
                         {}});
      }
      if (items.empty()) {
        response.transcript_lines = {"No tools matched."};
        return response;
      }

      auto settings = std::make_shared<qt::tui::SettingsList>(
          std::move(items), 12U, qt::tui::SettingsListTheme{},
          qt::tui::SettingsListOptions{true});
      settings->set_change_callback([&, settings](const std::string& id,
                                                  const std::string& value) {
        std::vector<std::string> next_active_tools =
            runtime->host.harness().active_tool_names();
        next_active_tools.erase(
            std::remove(next_active_tools.begin(), next_active_tools.end(), id),
            next_active_tools.end());
        if (value == "on") {
          next_active_tools.push_back(id);
        }

        const auto persisted = persist_active_tools_config(next_active_tools);
        if (!persisted) {
          ui.append_transcript_line("error: " + persisted.error().message);
        }
      });
      settings->set_cancel_callback([&ui]() { ui.hide_overlay(); });
      ui.show_overlay(settings);
      return response;
    }

    if (event.command == "extensions" || event.command == "skills" ||
        event.command == "prompts" || event.command == "themes") {
      response.handled = true;
      response.append_default_output = false;

      qt::tools::PackageResourceKind resource_kind =
          qt::tools::PackageResourceKind::extensions;
      if (event.command == "skills") {
        resource_kind = qt::tools::PackageResourceKind::skills;
      } else if (event.command == "prompts") {
        resource_kind = qt::tools::PackageResourceKind::prompts;
      } else if (event.command == "themes") {
        resource_kind = qt::tools::PackageResourceKind::themes;
      }

      const auto resources = load_package_resource_items(resource_kind);
      if (!resources) {
        return qt::foundation::Result<
            qt::app::InteractiveSlashCommandResponse>::failure(
            resources.error().code, resources.error().message,
            resources.error().system_error);
      }

      const std::string filter = lowercase_ascii(event.arguments);
      auto matches_filter = [&](const InteractivePackageResourceItem& item) {
        if (filter.empty()) {
          return true;
        }
        return lowercase_ascii(item.label).find(filter) != std::string::npos ||
               lowercase_ascii(item.description).find(filter) !=
                   std::string::npos ||
               lowercase_ascii(item.resource_path).find(filter) !=
                   std::string::npos;
      };

      std::vector<qt::tui::SettingItem> items;
      auto records =
          std::make_shared<std::vector<InteractivePackageResourceItem>>();
      for (const auto& resource : resources.value()) {
        if (!matches_filter(resource)) {
          continue;
        }
        records->push_back(resource);
        items.push_back({resource.id,
                         resource.label,
                         resource.description,
                         resource.can_toggle
                             ? (resource.enabled ? std::string("on")
                                                 : std::string("off"))
                             : std::string("loaded"),
                         resource.can_toggle
                             ? std::vector<std::string>{"off", "on"}
                             : std::vector<std::string>{},
                         {}});
      }
      if (items.empty()) {
        response.transcript_lines = {
            resource_kind == qt::tools::PackageResourceKind::extensions
                ? "No extensions matched."
                : "No package-managed resources matched."};
        return response;
      }

      auto settings = std::make_shared<qt::tui::SettingsList>(
          std::move(items), 12U, qt::tui::SettingsListTheme{},
          qt::tui::SettingsListOptions{true});
      settings->set_change_callback(
          [&, settings, records](const std::string& id, const std::string& value) {
            const auto it =
                std::find_if(records->begin(), records->end(),
                             [&](const InteractivePackageResourceItem& resource) {
                               return resource.id == id;
                             });
            if (it == records->end()) {
              ui.append_transcript_line("error: resource not found: " + id);
              return;
            }
            if (!it->can_toggle) {
              return;
            }

            const auto package_settings = load_runtime_package_settings();
            if (!package_settings) {
              ui.append_transcript_line("error: " +
                                        package_settings.error().message);
              return;
            }

            qt::tools::PackageResourceToggleRequest req;
            req.source = it->source;
            req.scope = it->scope;
            req.resource_kind = it->resource_kind;
            req.package_root = it->package_root;
            req.resource_path = it->resource_path;
            req.enabled = value == "on";

            const auto plan = qt::tools::make_package_resource_toggle_plan(
                req, package_settings.value());
            const auto applied = qt::tools::execute_package_settings_plan(
                plan, package_settings_cwd(),
                qt::foundation::Path(runtime->services.settings.agent_home));
            if (!applied) {
              ui.append_transcript_line("error: " + applied.error().message);
              return;
            }

            ui.hide_overlay();
            const auto status = switch_to_session(
                ui, *current_session,
                std::string(value == "on" ? "Enabled " : "Disabled ") +
                    qt::tools::to_string(it->resource_kind) + " " + it->label +
                    ".");
            if (!status) {
              ui.append_transcript_line("error: " + status.error().message);
            }
          });
      settings->set_cancel_callback([&ui]() { ui.hide_overlay(); });
      ui.show_overlay(settings);
      return response;
    }

    if (event.command == "settings") {
      response.handled = true;
      response.append_default_output = false;
      auto boolean_text = [](bool value) {
        return value ? std::string("on") : std::string("off");
      };
      auto tool_description = [&](const std::string& name) {
        for (const auto& tool : runtime->services.tool_definitions) {
          if (tool.name == name) {
            return tool.description;
          }
        }
        if (name == "index") {
          return std::string(
              "Grouped project index, code, filesystem, git, debug, build, "
              "serial, fetch, RTOS, and utility operations.");
        }
        return std::string("Built-in tool transcript entries.");
      };
      auto tool_compaction_submenu =
          [&, boolean_text, tool_description](
              const std::string&,
              qt::tui::SettingItem::SubmenuDone done) -> qt::tui::ComponentPtr {
        std::vector<qt::tui::SettingItem> tool_items;
        tool_items.reserve(tool_entry_compaction_builtin_tools().size());
        const auto& enabled_tools =
            runtime->services.settings
                .terminal_tool_entry_compaction_enabled_tools;
        for (const auto& tool : tool_entry_compaction_builtin_tools()) {
          tool_items.push_back(
              {tool,
               tool,
               tool_description(tool),
               boolean_text(contains_string(enabled_tools, tool)),
               {"off", "on"},
               {}});
        }

        auto submenu = std::make_shared<qt::tui::SettingsList>(
            std::move(tool_items), 12U, qt::tui::SettingsListTheme{},
            qt::tui::SettingsListOptions{true});
        submenu->set_change_callback(
            [&](const std::string& id, const std::string& value) {
              auto configured =
                  runtime->services.settings
                      .terminal_tool_entry_compaction_enabled_tools;
              configured.erase(
                  std::remove(configured.begin(), configured.end(), id),
                  configured.end());
              if (value == "on") {
                configured.push_back(id);
              }
              configured =
                  normalize_tool_entry_compaction_tools(std::move(configured));
              runtime->services.settings
                  .terminal_tool_entry_compaction_enabled_tools = configured;
              ui.set_tool_entry_compaction_enabled_tools(configured);

              const auto persisted =
                  persist_tool_entry_compaction_enabled_tools(configured);
              if (!persisted) {
                ui.append_transcript_line("error: " +
                                          persisted.error().message);
              }
            });
        submenu->set_cancel_callback(
            [&, done = std::move(done)]() mutable {
              done(tool_entry_compaction_summary(
                  runtime->services.settings
                      .terminal_tool_entry_compaction_enabled_tools));
            });
        return submenu;
      };

      std::vector<qt::tui::SettingItem> items{
          {"quiet_startup", "Quiet startup", "Hide startup hints",
           boolean_text(runtime->services.settings.quiet_startup), {"off", "on"},
           {}},
          {"collapse_changelog", "Collapse changelog",
           "Use compact changelog on startup",
           boolean_text(runtime->services.settings.collapse_changelog),
           {"off", "on"}, {}},
          {"hide_thinking_block", "Hide thinking",
           "Prefer compact thinking display",
           boolean_text(runtime->services.settings.hide_thinking_block),
           {"off", "on"}, {}},
          {"enable_skill_commands", "Skill commands",
           "Enable /skill:* slash commands",
           boolean_text(runtime->services.settings.enable_skill_commands),
           {"off", "on"}, {}},
          {"allow_external_filesystem_access", "External file access",
           "Allow model-facing native tools to use paths outside the workspace",
           boolean_text(
               runtime->services.settings.allow_external_filesystem_access),
           {"off", "on"}, {}},
          {"image_auto_resize", "Image auto resize",
           "Resize oversized startup images",
           boolean_text(runtime->services.settings.image_auto_resize),
           {"off", "on"}, {}},
          {"terminal_show_progress", "Terminal progress",
           "Show terminal task progress during active turns",
           boolean_text(runtime->services.settings.terminal_show_progress),
           {"off", "on"}, {}},
          {"prompt_history_limit", "Prompt history",
           "Maximum prompts retained in the global qt history",
           std::to_string(
               runtime->services.settings.terminal_prompt_history_limit),
           {"0", "50", "100", "250", "500", "1000", "5000"}, {}},
          {"tool_entry_compaction_tools", "Tool compaction",
           "Choose which built-in tool transcript entries can be compacted",
           tool_entry_compaction_summary(
               runtime->services.settings
                   .terminal_tool_entry_compaction_enabled_tools),
           {},
           tool_compaction_submenu},
      };

      auto settings = std::make_shared<qt::tui::SettingsList>(
          std::move(items), 10U, qt::tui::SettingsListTheme{},
          qt::tui::SettingsListOptions{true});
      settings->set_change_callback([&, settings](const std::string& id,
                                                  const std::string& value) {
        if (id == "prompt_history_limit") {
          const std::size_t max_entries =
              static_cast<std::size_t>(std::stoull(value));
          runtime->services.settings.terminal_prompt_history_limit =
              max_entries;
          ui.mutable_editor().set_history_limit(max_entries);

          const auto persisted_setting =
              persist_prompt_history_limit(max_entries);
          if (!persisted_setting) {
            ui.append_transcript_line("error: " +
                                      persisted_setting.error().message);
          }
          qt::app::PromptHistoryStore store(
              qt::foundation::Path(runtime->services.settings.agent_home),
              max_entries);
          const auto persisted_history = store.save(ui.editor().history());
          if (!persisted_history) {
            ui.append_transcript_line("error: " +
                                      persisted_history.error().message);
          }
          runtime->host.session_state().metadata().prompt_history =
              ui.editor().history();
          current_session->prompt_history = ui.editor().history();
          return;
        }
        if (id == "allow_external_filesystem_access") {
          const bool enabled = value == "on";
          const bool previous =
              runtime->services.settings.allow_external_filesystem_access;
          const auto persisted = persist_boolean_setting(id, enabled);
          if (!persisted) {
            settings->update_value(id, boolean_text(previous));
            ui.append_transcript_line("error: " + persisted.error().message);
            return;
          }
          runtime->services.settings.allow_external_filesystem_access = enabled;
          runtime->services.tool_context.allow_external_filesystem_access =
              enabled;
          runtime->host.harness().set_external_filesystem_access(enabled);
          ui.append_transcript_line(
              std::string("External file access ") +
              (enabled ? "enabled" : "disabled") + ".");
          return;
        }
        const bool enabled = value == "on";
        if (id == "quiet_startup") {
          runtime->services.settings.quiet_startup = enabled;
        } else if (id == "collapse_changelog") {
          runtime->services.settings.collapse_changelog = enabled;
        } else if (id == "hide_thinking_block") {
          runtime->services.settings.hide_thinking_block = enabled;
          live_turn_transcript.set_hide_thinking(enabled, "Thinking...");
        } else if (id == "enable_skill_commands") {
          runtime->services.settings.enable_skill_commands = enabled;
          ui.set_autocomplete_provider(build_autocomplete_provider());
        } else if (id == "image_auto_resize") {
          runtime->services.settings.image_auto_resize = enabled;
        } else if (id == "terminal_show_progress") {
          runtime->services.settings.terminal_show_progress = enabled;
        } else if (id == "tool_entry_compaction_tools") {
          return;
        }
        const auto persisted = persist_boolean_setting(id, enabled);
        if (!persisted) {
          ui.append_transcript_line("error: " + persisted.error().message);
        }
      });
      settings->set_cancel_callback([&ui]() { ui.hide_overlay(); });
      ui.show_overlay(settings);
      return response;
    }

    if (event.command == "share") {
      response.handled = true;
      response.append_default_output = false;
      if (!current_session->repository_path) {
        return qt::foundation::Result<
            qt::app::InteractiveSlashCommandResponse>::failure(
            qt::foundation::ErrorCode::invalid_argument,
            "Cannot share an in-memory session");
      }

      const auto share_plan = qt::app::plan_share_session_gist();
      const auto shared = qt::app::execute_share_session_gist(
          share_plan,
          [](const std::vector<std::string>& argv) {
            return qt::foundation::run_argv_process(argv);
          },
          [&](std::string_view output_path) {
            return qt::app::export_session_file_to_html(
                *current_session->repository_path,
                std::optional<std::string_view>(output_path));
          });
      if (!shared) {
        return qt::foundation::Result<
            qt::app::InteractiveSlashCommandResponse>::failure(
            shared.error().code, shared.error().message,
            shared.error().system_error);
      }
      response.transcript_lines = {shared.value().status_text};
      return response;
    }

    if (event.command == "compact") {
      response.handled = true;
      response.append_default_output = false;
      const auto before_leaf = runtime->host.session_manager().get_leaf_id();
      auto compacted = runtime->host.harness().compact_session(event.arguments);
      if (!compacted) {
        return compacted.error();
      }
      runtime->host.sync_session_manager_from_harness();
      const auto persisted = persist_leaf_if_needed(before_leaf);
      if (!persisted) {
        return persisted.error();
      }
      replace_transcript_with_compaction_summary(ui, "manual",
                                                 compacted.value());
      return response;
    }

    if (event.command == "goal") {
      response.handled = true;
      response.append_default_output = false;

      // Use the shared goal store for tracking session goals
      const std::string session_id =
          current_session->session_id.value_or(std::string("default"));
      const std::string subcommand = event.arguments;

      if (subcommand.empty()) {
        // Show current goal status or usage help
        const auto goal = goal_store->get_session_goal(session_id);
        if (!goal.has_value()) {
          response.transcript_lines = {
              "No active goal.",
              qt::harness::kGoalUsage};
        } else {
          response.transcript_lines = {
              "Goal",
              "Status: " + std::string(qt::harness::goal_status_label(goal->status)),
              "Objective: " + goal->objective,
              "Time used: " + qt::harness::format_goal_elapsed_seconds(goal->time_used_seconds),
          };
          if (goal->token_budget.has_value()) {
            response.transcript_lines.push_back(
                "Tokens used: " + std::to_string(goal->tokens_used) + "/" +
                std::to_string(*goal->token_budget));
          } else {
            response.transcript_lines.push_back(
                "Tokens used: " + std::to_string(goal->tokens_used));
          }
          // Add command hints based on status
          std::string hints;
          if (qt::harness::is_active(goal->status)) {
            hints = "Commands: /goal edit, /goal pause, /goal clear";
          } else if (goal->status == qt::harness::ThreadGoalStatus::kPaused ||
                     goal->status == qt::harness::ThreadGoalStatus::kBlocked ||
                     goal->status == qt::harness::ThreadGoalStatus::kUsageLimited) {
            hints = "Commands: /goal edit, /goal resume, /goal clear";
          } else {
            hints = "Commands: /goal edit, /goal clear";
          }
          response.transcript_lines.push_back("");
          response.transcript_lines.push_back(hints);
        }
        return response;
      }

      if (subcommand == "clear") {
        const auto deleted = goal_store->delete_session_goal(session_id);
        if (deleted.has_value()) {
          response.transcript_lines = {"Goal cleared."};
        } else {
          response.transcript_lines = {"No active goal to clear."};
        }
        return response;
      }

      if (subcommand == "pause") {
        const auto paused = goal_store->pause_active_session_goal(session_id);
        if (paused.has_value()) {
          response.transcript_lines = {"Goal paused."};
        } else {
          response.transcript_lines = {"No active goal to pause."};
        }
        return response;
      }

      if (subcommand == "resume") {
        qt::harness::GoalUpdate update;
        update.status = qt::harness::ThreadGoalStatus::kActive;
        const auto current = goal_store->get_session_goal(session_id);
        if (!current.has_value()) {
          response.transcript_lines = {"No goal to resume."};
          return response;
        }
        if (current->status != qt::harness::ThreadGoalStatus::kPaused) {
          response.transcript_lines = {"Goal is not paused."};
          return response;
        }
        const auto resumed = goal_store->update_session_goal(session_id, update);
        if (resumed.has_value()) {
          response.transcript_lines = {
              "Goal resumed.",
              "Continuing autonomous goal execution..."};

          // Trigger continuation prompt if no turn is active
          if (!turn_pump.active()) {
            const std::string continuation_prompt =
                qt::harness::goal_continuation_prompt(*resumed);
            pending_follow_up_messages.push_back(continuation_prompt);
          }
        } else {
          response.transcript_lines = {"Failed to resume goal."};
        }
        return response;
      }

      if (subcommand == "edit") {
        const auto current = goal_store->get_session_goal(session_id);
        if (!current.has_value()) {
          response.transcript_lines = {
              "No active goal to edit.",
              qt::harness::kGoalUsage};
          return response;
        }
        response.transcript_lines = {
            "Current objective: " + current->objective,
            "Type /goal <new objective> to update."};
        return response;
      }

      if (subcommand == "status") {
        const auto goal = goal_store->get_session_goal(session_id);
        if (!goal.has_value()) {
          response.transcript_lines = {"No active goal."};
        } else {
          response.transcript_lines = {qt::harness::goal_usage_summary(*goal)};
        }
        return response;
      }

      // Set a new goal with the provided objective
      const auto existing = goal_store->get_session_goal(session_id);
      if (existing.has_value() && qt::harness::is_active(existing->status)) {
        // Update existing active goal's objective
        qt::harness::GoalUpdate update;
        update.objective = subcommand;
        const auto updated = goal_store->update_session_goal(session_id, update);
        if (updated.has_value()) {
          response.transcript_lines = {
              "Goal updated.",
              "Objective: " + subcommand};
        } else {
          response.transcript_lines = {"Failed to update goal."};
        }
      } else {
        // Create new goal
        const auto created = goal_store->replace_session_goal(
            session_id, subcommand, qt::harness::ThreadGoalStatus::kActive);
        response.transcript_lines = {
            "Goal set.",
            "Objective: " + created.objective,
            "Starting autonomous goal execution..."};

        // Trigger the first continuation prompt if no turn is active
        if (!turn_pump.active()) {
          const std::string continuation_prompt =
              qt::harness::goal_continuation_prompt(created);
          // Queue the continuation as a follow-up message to be submitted
          // after this slash command response is displayed
          pending_follow_up_messages.push_back(continuation_prompt);
        }
      }
      return response;
    }

    if (event.command == "changelog") {
      response.handled = true;
      response.append_default_output = false;
      response.transcript_lines = changelog_lines();
      return response;
    }

    if (event.command == "hotkeys") {
      response.handled = true;
      response.append_default_output = false;
      response.transcript_lines = hotkey_lines();
      return response;
    }

    if (event.command == "export") {
      response.handled = true;
      response.append_default_output = false;
      if (!current_session->repository_path) {
        return qt::foundation::Result<
            qt::app::InteractiveSlashCommandResponse>::failure(
            qt::foundation::ErrorCode::invalid_argument,
            "Cannot export an in-memory session");
      }

      const std::string path = event.arguments;
      if (path.empty() || ends_with_case_insensitive(path, ".html")) {
        const auto exported = qt::app::export_session_file_to_html(
            *current_session->repository_path,
            path.empty() ? std::nullopt
                         : std::optional<std::string_view>(path));
        if (!exported) {
          return exported.error();
        }
        response.transcript_lines = {"Exported HTML to " + exported.value()};
        return response;
      }

      if (ends_with_case_insensitive(path, ".jsonl")) {
        std::error_code error;
        std::filesystem::copy_file(*current_session->repository_path, path,
                                   std::filesystem::copy_options::overwrite_existing,
                                   error);
        if (error) {
          return qt::foundation::Result<
              qt::app::InteractiveSlashCommandResponse>::failure(
              qt::foundation::ErrorCode::io_error,
              "failed to export session JSONL: " + error.message());
        }
        response.transcript_lines = {"Exported JSONL to " + path};
        return response;
      }

      return qt::foundation::Result<
          qt::app::InteractiveSlashCommandResponse>::failure(
          qt::foundation::ErrorCode::invalid_argument,
          "unsupported export path: " + path);
    }

    return response;
  };

  qt::app::InteractiveTerminalSession* active_session_ptr = nullptr;
  qt::app::InteractiveTerminalSessionOptions configured_options;
  configured_options.constraints = constraints;
  configured_options.input_prompt = "qt> ";
  configured_options.clear_on_start = true;
  configured_options.hide_cursor = false;
  configured_options.footer_provider = footer_provider;
  configured_options.help_provider = help_provider;
  configured_options.prompt_history_changed_handler = persist_prompt_history;
  configured_options.pending_messages_provider =
      [&pending_steer_messages, &pending_follow_up_messages, &turn_pump,
       runtime]() {
        std::vector<qt::app::InteractivePendingMessage> messages;
        // Start with the cached string queues (source of truth for display)
        messages.reserve(pending_steer_messages.size() +
                         pending_follow_up_messages.size());
        for (const auto& text : pending_steer_messages) {
          messages.push_back(qt::app::InteractivePendingMessage{
              qt::app::InteractivePendingMessageKind::kSteering, text});
        }
        for (const auto& text : pending_follow_up_messages) {
          messages.push_back(qt::app::InteractivePendingMessage{
              qt::app::InteractivePendingMessageKind::kFollowUp, text});
        }
        return messages;
      };
  configured_options.interrupt_handler =
      [runtime, &turn_pump, &working_indicator, &pending_steer_messages,
       &pending_follow_up_messages, &bash_pump_active, &abort_async_bash](
          qt::app::InteractiveTerminalSession& session) {
        if (!turn_pump.active() && bash_pump_active()) {
          if (abort_async_bash()) {
            working_indicator.set_message("Cancelling bash...");
            return true;
          }
          return false;
        }
        if (!turn_pump.active()) {
          return false;
        }

        std::string restored_text;
        const auto append_restored = [&restored_text](const std::string& text) {
          if (text.empty()) {
            return;
          }
          if (!restored_text.empty()) {
            restored_text += "\n\n";
          }
          restored_text += text;
        };
        for (const auto& text : pending_steer_messages) {
          append_restored(text);
        }
        for (const auto& text : pending_follow_up_messages) {
          append_restored(text);
        }

        runtime->host.harness().request_abort();
        pending_steer_messages.clear();
        pending_follow_up_messages.clear();
        if (!restored_text.empty()) {
          session.mutable_editor().set_text(std::move(restored_text));
        }
        working_indicator.set_message("Interrupting...");
        return true;
      };
  configured_options.clear_handler =
      [&interactive_exit_requested,
       &last_clear_request](qt::app::InteractiveTerminalSession& session) {
        if (!session.editor().text().empty()) {
          session.mutable_editor().set_text("");
          last_clear_request = std::chrono::steady_clock::time_point{};
          return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_clear_request < std::chrono::milliseconds(500)) {
          interactive_exit_requested = true;
          session.clear_overlays();
          session.append_transcript_line("[quit]");
          return true;
        }
        session.mutable_editor().set_text("");
        last_clear_request = now;
        return true;
      };
  configured_options.exit_handler =
      [&interactive_exit_requested](qt::app::InteractiveTerminalSession& session) {
        interactive_exit_requested = true;
        session.append_transcript_line("[quit]");
        return true;
      };
  configured_options.paste_image_path_provider =
      [&pending_clipboard_images]() -> std::optional<std::string> {
    const auto image = qt::tools::read_clipboard_image();
    if (!image.ok || image.bytes.empty()) {
      return std::nullopt;
    }

    std::error_code temp_error;
    const auto temp_dir = std::filesystem::temp_directory_path(temp_error);
    if (temp_error) {
      return std::nullopt;
    }

    const auto written = qt::tools::write_clipboard_image_to_temp_path(
        image.bytes, image.mime_type, temp_dir,
        qt::foundation::generate_uuid_v4_string());
    if (!written.ok) {
      return std::nullopt;
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(image.bytes.size());
    for (const unsigned char byte : image.bytes) {
      bytes.push_back(static_cast<std::uint8_t>(byte));
    }

    PendingClipboardImage pending;
    pending.path = written.path.generic_string();
    pending.content =
        qt::ai::ImageContent{written.mime_type, qt::ai::encode_image_base64(bytes)};
    pending_clipboard_images.push_back(std::move(pending));
    return pending_clipboard_images.back().path;
      };
  configured_options.external_editor_handler =
      [&, active_session_ptr](qt::app::InteractiveTerminalSession& session) mutable {
        if (active_session_ptr == nullptr) {
          active_session_ptr = &session;
        }

        const auto editor_command = external_editor_command_from_env();
        if (!editor_command) {
          session.set_status_message(
              "No editor configured. Set $VISUAL or $EDITOR.");
          return true;
        }

        auto temp_path = make_external_editor_temp_path();
        if (!temp_path) {
          session.append_transcript_line("error: " + temp_path.error().message);
          return true;
        }
        ScopedPathRemoval cleanup(temp_path.value());

        const auto write = qt::foundation::write_text_file(
            temp_path.value(), session.editor().expanded_text());
        if (!write) {
          session.append_transcript_line("error: " + write.error().message);
          return true;
        }

        const auto stopped = stop_terminal_session();
        if (!stopped) {
          session.append_transcript_line("error: " + stopped.error().message);
          const auto restarted = start_terminal_session();
          (void)restarted;
          return true;
        }

        const std::string command_line =
            *editor_command + " " + shell_quote_argument(temp_path.value().string());
        const int exit_code = std::system(command_line.c_str());
        if (exit_code == 0) {
          auto edited = qt::foundation::read_text_file(temp_path.value());
          if (edited) {
            session.mutable_editor().set_text(
                strip_single_trailing_line_ending(std::move(edited.value())));
            session.clear_status_message();
          } else {
            session.append_transcript_line("error: " + edited.error().message);
          }
        } else {
          session.set_status_message("External editor exited without changes.");
        }

        const auto restarted = start_terminal_session();
        if (!restarted) {
          session.append_transcript_line("error: " + restarted.error().message);
          return true;
        }
        if (active_session_ptr != nullptr) {
          const auto rendered = active_session_ptr->start();
          if (!rendered) {
            session.append_transcript_line("error: " + rendered.error().message);
          }
        }
        return true;
      };
  configured_options.suspend_handler =
      [&, active_session_ptr](qt::app::InteractiveTerminalSession& session) mutable {
        if (active_session_ptr == nullptr) {
          active_session_ptr = &session;
        }
        qt::app::TerminalSuspendController controller(
            qt::app::TerminalSuspendControllerOptions{
#if defined(_WIN32)
                false,
#else
                true,
#endif
                stop_terminal_session,
                {},
                start_terminal_session,
                [active_session_ptr]() -> qt::foundation::Status {
                  if (active_session_ptr == nullptr) {
                    return qt::foundation::Status::success();
                  }
                  return active_session_ptr->start();
                }});
        const auto suspended = controller.suspend_and_resume();
        if (!suspended) {
          session.append_transcript_line("error: " + suspended.error().message);
        }
        return true;
      };
  configured_options.autocomplete_provider = build_autocomplete_provider();
  configured_options.slash_command_executor =
      [&](qt::app::InteractiveTerminalSession& ui,
          const qt::app::InteractiveShellEvent& event) {
        auto result = command_executor(ui, event);
        if (result && result.value().handled) {
          cached_footer_lines = compute_footer_lines();
        }
        return result;
      };
  configured_options.session_adapter = &notcurses_session;
  qt::app::InteractiveTerminalSession configured_session(
      shell, terminal, std::move(configured_options));
  configured_session.set_tool_entry_compaction_enabled_tools(
      runtime->services.settings.terminal_tool_entry_compaction_enabled_tools);
  const auto prompt_history_warning = restore_prompt_history(
      configured_session,
      runtime->host.session_state().metadata().prompt_history);
  seed_transcript(configured_session);
  if (prompt_history_warning) {
    configured_session.append_transcript_line("Warning: " +
                                              *prompt_history_warning);
  }
  if (!startup_banner_lines.empty()) {
    configured_session.append_transcript_literal_lines(
        std::move(startup_banner_lines));
  }
  if (!startup_lines.empty()) {
    configured_session.append_transcript_lines(std::move(startup_lines));
  }
  auto& active_session = configured_session;
  active_session_ptr = &active_session;
  extension_ui_state.apply_tools_expanded =
      [&active_session](bool expanded) { active_session.set_tools_expanded(expanded); };
  extension_ui_state.read_tools_expanded =
      [&active_session]() { return active_session.tools_expanded(); };

  // Session start hooks run after the terminal session exists so ctx.ui calls
  // can reach the live UI. Preserve the original headless host for calls made
  // from an extension worker thread.
  const auto lifecycle_entry_baseline =
      runtime->host.session_manager().get_entries().size();
  const auto lifecycle_owner_thread = std::this_thread::get_id();
  for (auto& extension :
       runtime->services.resource_bundle.extension_host.registry.loaded_extensions) {
    if (!extension.script_runtime) continue;
    const auto fallback = *extension.host_call;
    const auto invocation_slot = extension.host_call;
    auto ui_host = make_interactive_extension_host(
        active_session, extension.script_runtime, invocation_slot);
    *invocation_slot =
        [ui_host, fallback, lifecycle_owner_thread](
            std::string_view method, const qt::ai::Json& arguments)
        -> qt::foundation::Result<qt::ai::Json> {
      if (std::this_thread::get_id() != lifecycle_owner_thread && fallback) {
        return fallback(method, arguments);
      }
      return ui_host(method, arguments);
    };
    extension.host_call = invocation_slot;
  }
  auto startup_lifecycle_hooks =
      qt::app::run_extension_session_start_hooks_subprocess(
          runtime->services.resource_bundle.extension_host,
          qt::extensions::ExtensionSessionStartReason::kStartup);
  if (!startup_lifecycle_hooks) {
    std::cerr << "qt: " << startup_lifecycle_hooks.error().message << '\n';
    return EXIT_FAILURE;
  }
  const auto persisted_lifecycle_entries =
      persist_extension_entries(lifecycle_entry_baseline);
  if (!persisted_lifecycle_entries) {
    std::cerr << "qt: " << persisted_lifecycle_entries.error().message << '\n';
    return EXIT_FAILURE;
  }

  // Start the theme file watcher after the session is fully initialized.
  const auto& current_theme_name = runtime->services.settings.theme;
  if (!current_theme_name.empty() && current_theme_name != "dark" &&
      current_theme_name != "light") {
    for (const auto& root : runtime->services.settings.theme_roots) {
      const std::string candidate = root + "/" + current_theme_name + ".json";
      if (theme_watcher.start_watching(
              current_theme_name, candidate, [&active_session]() {
                (void)active_session.render();
              })) {
        break;
      }
    }
  }

  bool footer_branch_changed = false;
  footer_data_provider.on_branch_change(
      [&footer_branch_changed]() { footer_branch_changed = true; });
  std::optional<
      std::future<std::optional<qt::app::ReferenceLatestCliRelease>>>
      pending_version_check;
  if (!runtime->services.settings.quiet_startup) {
    pending_version_check.emplace(std::async(
        std::launch::async, []() {
          return qt::app::check_for_new_cli_release(
              kReferenceCliVersion);
        }));
  }
  const auto start_status = active_session.start();
  if (!start_status) {
    std::cerr << "qt: " << start_status.error().message << '\n';
    const auto stop = stop_terminal_session();
    (void)stop;
    return EXIT_FAILURE;
  }
  const auto post_start_raw_mode_status = terminal_adapter.enter_raw_mode();
  if (!post_start_raw_mode_status) {
    std::cerr << "qt: " << post_start_raw_mode_status.error().message << '\n';
    const auto stop = stop_terminal_session();
    (void)stop;
    return EXIT_FAILURE;
  }
  if (!notcurses_session.using_notcurses()) {
    (void)terminal_input_pump.start();
  }

  const auto flush_pending_version_notification =
      [&](bool wait_for_completion) -> qt::foundation::Status {
    if (!pending_version_check) {
      return qt::foundation::Status::success();
    }
    if (!wait_for_completion &&
        pending_version_check->wait_for(std::chrono::milliseconds(0)) !=
            std::future_status::ready) {
      return qt::foundation::Status::success();
    }

    const auto release = pending_version_check->get();
    pending_version_check.reset();
    if (!release) {
      return qt::foundation::Status::success();
    }

    active_session.append_transcript_lines(
        qt::app::make_new_version_notification_lines(
            release->version));
    const auto rendered = active_session.render();
    if (!rendered) {
      return qt::foundation::Status(rendered.error());
    }
    return qt::foundation::Status::success();
  };
  constexpr auto streaming_frame_interval = std::chrono::milliseconds(33);
  auto last_live_turn_render = std::chrono::steady_clock::time_point{};
  const auto flush_live_turn_transcript =
      [&](bool force_render) -> qt::foundation::Status {
    if (!force_render && live_turn_transcript.has_pending_update() &&
        !live_turn_transcript.has_immediate_pending_update() &&
        last_live_turn_render != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() - last_live_turn_render <
            streaming_frame_interval) {
      return qt::foundation::Status::success();
    }

    auto update = live_turn_transcript.drain_update();
    if (update.empty()) {
      return qt::foundation::Status::success();
    }
    for (auto& item : update.items) {
      if (item.kind == LiveTranscriptUpdate::Item::Kind::kComponent) {
        if (!item.tool_name.empty() || !item.tool_call_id.empty()) {
          active_session.append_transcript_tool_result_component(
              std::move(item.tool_name), std::move(item.tool_call_id),
              std::move(item.tool_arguments), std::move(item.component));
        } else {
          active_session.append_transcript_component(std::move(item.component));
        }
      } else {
        active_session.append_transcript_line(std::move(item.line));
      }
    }
    const auto rendered = active_session.render();
    if (!rendered) {
      return qt::foundation::Status(rendered.error());
    }
    last_live_turn_render = std::chrono::steady_clock::now();
    return qt::foundation::Status::success();
  };
  const auto flush_completed_turn =
      [&](bool wait_for_completion) -> qt::foundation::Status {
    const auto live_status = flush_live_turn_transcript(wait_for_completion);
    if (!live_status) {
      return live_status;
    }
    const auto completed = turn_pump.poll(wait_for_completion);
    if (!completed) {
      return qt::foundation::Status(completed.error());
    }
    if (!completed.value().has_value()) {
      const auto progress_status = refresh_terminal_progress();
      if (!progress_status) {
        return progress_status;
      }
      return qt::foundation::Status::success();
    }

    const auto& turn_result = *completed.value();
    const auto final_live_status = flush_live_turn_transcript(true);
    if (!final_live_status) {
      return final_live_status;
    }
    const bool live_assistant_output_rendered =
        live_turn_transcript.rendered_assistant_output();
    active_session.finalize_transcript_components();
    live_turn_transcript.stop();
    working_indicator.stop();
    working_indicator.set_message(std::nullopt);
    const auto title_status = refresh_model_terminal_title();
    if (!title_status) {
      return title_status;
    }
    const auto progress_status = refresh_terminal_progress();
    if (!progress_status) {
      return progress_status;
    }
    auto compacted_transcript = take_pending_compaction_transcript();
    if (compacted_transcript.has_value()) {
      active_session.set_transcript_lines(std::move(*compacted_transcript));
      cached_footer_lines = compute_footer_lines();
      active_session.clear_status_message();
    } else if (!turn_result) {
      active_session.append_transcript_line("error: " +
                                            turn_result.error().message);
    } else if (!turn_result.value().empty() &&
               !live_assistant_output_rendered) {
      active_session.append_transcript_lines(
          split_display_lines(turn_result.value()));
    }
    (void)active_session.compact_transcript_tool_entries();

    // Drain queued steer/follow-up operations and execute them on the
    // UI thread via the harness while the phase is still idle.
    {
      auto queued_ops = turn_pump.drain_queued_operations();
      for (const auto &op : queued_ops) {
        try {
          if (op.kind == qt::app::InteractiveTurnPumpOperationKind::kSteer) {
            runtime->host.harness().steer(op.text);
          } else {
            runtime->host.harness().follow_up(op.text);
          }
        } catch (const std::exception &ex) {
          active_session.append_transcript_line(
              std::string("error: failed to queue message: ") + ex.what());
        }
      }
    }

    if (!shell.should_quit() && !interactive_exit_requested) {
      std::optional<std::string> next_prompt;
      if (!pending_steer_messages.empty()) {
        next_prompt = pending_steer_messages.front();
        pending_steer_messages.erase(pending_steer_messages.begin());
      } else if (!pending_follow_up_messages.empty()) {
        next_prompt = pending_follow_up_messages.front();
        pending_follow_up_messages.erase(pending_follow_up_messages.begin());
      }

      if (next_prompt) {
        const auto started = start_async_prompt(std::move(*next_prompt), {},
                                                "Running queued message");
        if (!started) {
          return qt::foundation::Status(started.error());
        }
        if (!started.value()) {
          return qt::foundation::Status::failure(
              qt::foundation::ErrorCode::internal_error,
              "interactive turn pump rejected queued prompt");
        }
        const auto line = working_indicator.status_line();
        if (line) {
          active_session.set_status_message(*line);
        }
        const auto title_status = refresh_model_terminal_title();
        if (!title_status) {
          return title_status;
        }
      } else {
        // No pending messages - check for active goal continuation
        const std::string session_id =
            current_session->session_id.value_or(std::string("default"));
        const auto active_goal = goal_store->get_session_goal(session_id);
        if (active_goal.has_value() &&
            qt::harness::is_active(active_goal->status)) {
          // Generate and submit continuation prompt for active goal
          const std::string continuation_prompt =
              qt::harness::goal_continuation_prompt(*active_goal);
          const auto started = start_async_prompt(
              continuation_prompt, {}, "Continuing goal: " + active_goal->objective);
          if (!started) {
            return qt::foundation::Status(started.error());
          }
          if (!started.value()) {
            return qt::foundation::Status::failure(
                qt::foundation::ErrorCode::internal_error,
                "interactive turn pump rejected goal continuation prompt");
          }
          const auto line = working_indicator.status_line();
          if (line) {
            active_session.set_status_message(*line);
          }
          const auto title_status = refresh_model_terminal_title();
          if (!title_status) {
            return title_status;
          }
        } else {
          active_session.clear_status_message();
        }
      }
    }

    (void)start_openrouter_credit_refresh(true);
    cached_footer_lines = compute_footer_lines();
    const auto rendered = active_session.render();
    if (!rendered) {
      return qt::foundation::Status(rendered.error());
    }
    return qt::foundation::Status::success();
  };
  const auto flush_completed_bash =
      [&](bool wait_for_completion) -> qt::foundation::Status {
    std::future<qt::foundation::Result<ReferenceInteractiveBashExecution>> ready;
    {
      std::lock_guard<std::mutex> lock(bash_pump_mutex);
      if (!pending_bash_execution.has_value()) {
        return qt::foundation::Status::success();
      }
      if (!wait_for_completion &&
          pending_bash_execution->wait_for(std::chrono::milliseconds(0)) !=
              std::future_status::ready) {
        const auto progress_status = refresh_terminal_progress();
        if (!progress_status) {
          return progress_status;
        }
        return qt::foundation::Status::success();
      }
      ready = std::move(*pending_bash_execution);
      pending_bash_execution.reset();
      pending_bash_token.reset();
    }

    working_indicator.stop();
    working_indicator.set_message(std::nullopt);
    const auto progress_status = refresh_terminal_progress();
    if (!progress_status) {
      return progress_status;
    }

    const auto executed = ready.get();
    if (!executed) {
      active_session.append_transcript_line("error: " + executed.error().message);
    } else {
      const auto recorded = record_interactive_bash(
          *runtime, *current_session, executed.value());
      if (!recorded) {
        return qt::foundation::Status(recorded.error());
      }
      if (!recorded.value().empty()) {
        active_session.append_transcript_line(recorded.value());
      }
    }

    cached_footer_lines = compute_footer_lines();
    const auto rendered = active_session.render();
    if (!rendered) {
      return qt::foundation::Status(rendered.error());
    }
    return qt::foundation::Status::success();
  };
  const auto refresh_provider_status_footer = [&]() -> qt::foundation::Status {
    if (!poll_openrouter_credit_refresh()) {
      return qt::foundation::Status::success();
    }
    cached_footer_lines = compute_footer_lines();
    const auto rendered = active_session.render();
    if (!rendered) {
      return qt::foundation::Status(rendered.error());
    }
    return qt::foundation::Status::success();
  };
  const auto refresh_remote_model_catalogs = [&]()
      -> qt::foundation::Status {
    if (!turn_pump.active() && runtime->services.subagent_coordinator) {
      for (const auto& event : runtime->services.subagent_coordinator->take_events()) {
        active_session.append_transcript_line(
            "subagent: " + event.value("child_id", "") + " " + event.value("status", "updated"));
        ExtensionPrompt prompt;
        prompt.text = event.dump();
        prompt.metadata = Json{{"role", "custom"}, {"customType", "subagent_event"},
                               {"display", true}, {"details", event}};
        extension_prompts.push_back(std::move(prompt));
      }
    }
    if (!turn_pump.active() && !extension_prompts.empty()) {
      auto prompt = std::move(extension_prompts.front());
      extension_prompts.erase(extension_prompts.begin());
      const auto started = start_async_prompt(std::move(prompt.text),
          std::move(prompt.images), "Running extension prompt...",
          std::move(prompt.metadata));
      if (!started) return qt::foundation::Status(started.error());
      if (!started.value()) return qt::foundation::Status::failure(
          qt::foundation::ErrorCode::internal_error, "extension prompt could not start");
    }
    (void)start_openai_model_refresh(false, false);
    if (openai_models.pending.has_value() &&
        openai_models.pending->wait_for(std::chrono::milliseconds(0)) ==
            std::future_status::ready) {
      const bool announce = openai_models.announce_completion;
      openai_models.announce_completion = false;
      auto refreshed = openai_models.pending->get();
      openai_models.pending.reset();
      if (refreshed) {
        runtime->services.model_catalog = qt::ai::replace_openai_models(
            runtime->services.model_catalog, openai_models.previous_discovery,
            refreshed.value());
        openai_models.previous_discovery = std::move(refreshed.value());
        if (auto selector = active_model_selector.lock()) {
          selector->set_models(build_model_selector_items());
        }
        active_session.set_autocomplete_provider(build_autocomplete_provider());
        if (announce) {
          active_session.append_transcript_line(
              "OpenAI model catalog refreshed (" +
              std::to_string(openai_models.previous_discovery.models.size()) +
              " chat models)");
        }
      } else if (announce) {
        active_session.append_transcript_line("error: " + refreshed.error().message);
      }
      const auto rendered = active_session.render();
      if (!rendered) {
        return qt::foundation::Status(rendered.error());
      }
    }
    (void)start_openrouter_model_refresh(false, false);
    if (!openrouter_models.pending.has_value() ||
        openrouter_models.pending->wait_for(std::chrono::milliseconds(0)) !=
            std::future_status::ready) {
      return qt::foundation::Status::success();
    }

    const bool announce = openrouter_models.announce_completion;
    openrouter_models.announce_completion = false;
    auto refreshed = openrouter_models.pending->get();
    openrouter_models.pending.reset();
    if (!refreshed) {
      if (announce) {
        active_session.append_transcript_line(
            "error: " + refreshed.error().message);
      }
      return qt::foundation::Status::success();
    }

    runtime->services.model_catalog = qt::ai::replace_openrouter_models(
        runtime->services.model_catalog, openrouter_models.previous_discovery,
        refreshed.value());
    openrouter_models.previous_discovery = std::move(refreshed.value());
    if (auto selector = active_model_selector.lock()) {
      selector->set_models(build_model_selector_items());
    }
    active_session.set_autocomplete_provider(build_autocomplete_provider());
    if (announce) {
      active_session.append_transcript_line(
          "OpenRouter model catalog refreshed (" +
          std::to_string(openrouter_models.previous_discovery.models.size()) +
          " models)");
    }
    const auto rendered = active_session.render();
    if (!rendered) {
      return qt::foundation::Status(rendered.error());
    }
    return qt::foundation::Status::success();
  };
  const auto refresh_working_indicator =
      [&](bool force_render) -> qt::foundation::Status {
    if ((!turn_pump.active() && !bash_pump_active()) ||
        !working_indicator.active()) {
      return refresh_model_terminal_title();
    }

    bool changed =
        working_indicator.advance_if_due(std::chrono::steady_clock::now());
    const auto title_status = refresh_model_terminal_title();
    if (!title_status) {
      return title_status;
    }
    const auto line = working_indicator.status_line();
    if (active_session.status_message() != line) {
      active_session.set_status_message(line);
      changed = true;
    }

    if (!changed && !force_render) {
      return qt::foundation::Status::success();
    }

    const auto rendered = active_session.render();
    if (!rendered) {
      return qt::foundation::Status(rendered.error());
    }
    return qt::foundation::Status::success();
  };

  const auto clear_exit_overlays = [&]() -> qt::foundation::Status {
    if (!active_session.has_overlay()) {
      return qt::foundation::Status::success();
    }
    active_session.clear_overlays();
    const auto rendered = active_session.render();
    if (!rendered) {
      return qt::foundation::Status(rendered.error());
    }
    return qt::foundation::Status::success();
  };

  std::size_t known_review_attention = 0U;
  if (runtime->services.memory_runtime) {
    try {
      const auto statistics =
          runtime->services.memory_runtime->review_statistics();
      known_review_attention = statistics.pending + statistics.due_deferred;
    } catch (const std::exception&) {
    }
  }
  auto next_review_notification_poll =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  const auto poll_review_notifications = [&]() -> qt::foundation::Status {
    const auto now = std::chrono::steady_clock::now();
    if (now < next_review_notification_poll) {
      return qt::foundation::Status::success();
    }
    next_review_notification_poll = now + std::chrono::seconds(5);
    if (!runtime->services.memory_runtime || turn_pump.active() ||
        bash_pump_active() || !active_session.editor().text().empty() ||
        active_session.has_overlay()) {
      return qt::foundation::Status::success();
    }
    try {
      const auto statistics =
          runtime->services.memory_runtime->review_statistics();
      const std::size_t attention =
          statistics.pending + statistics.due_deferred;
      if (attention > known_review_attention) {
        active_session.append_transcript_line(
            "Memory review: " +
            std::to_string(attention - known_review_attention) +
            " new/due item(s). Open /memory review.");
        const auto rendered = active_session.render();
        if (!rendered) return qt::foundation::Status(rendered.error());
      }
      known_review_attention = attention;
    } catch (const std::exception&) {
      // Review notifications are advisory and must never interrupt input.
    }
    return qt::foundation::Status::success();
  };

  if (!notcurses_session.using_notcurses()) {
    const auto version_status = flush_pending_version_notification(true);
    if (!version_status) {
      std::cerr << "qt: " << version_status.error().message << '\n';
      const auto stop = stop_terminal_session();
      (void)stop;
      return EXIT_FAILURE;
    }
  }

  if ((initial.initial_message.has_value() && !initial.initial_message->empty()) ||
      !initial.initial_images.empty()) {
    pending_prompt_images.reserve(initial.initial_images.size());
    for (const auto& image : initial.initial_images) {
      pending_prompt_images.push_back(image.content);
    }
    const auto initial_step =
        active_session.submit_line(initial.initial_message.value_or(std::string{}));
    if (!initial_step) {
      std::cerr << "qt: " << initial_step.error().message << '\n';
      const auto stop = stop_terminal_session();
      (void)stop;
      return EXIT_FAILURE;
    }
  }

  // Install signal handlers for graceful shutdown on SIGTERM/SIGHUP.
  // The handler sets a flag that is checked in the main loop.
  static volatile sig_atomic_t signal_shutdown_requested = 0;
  struct SignalHandlerInstaller {
    void (*previous_sigint)(int);
    void (*previous_sigterm)(int);
    void (*previous_sighup)(int);
    void (*previous_sigpipe)(int);
    SignalHandlerInstaller() {
      previous_sigint = std::signal(SIGINT, [](int) {
        signal_shutdown_requested = 1;
      });
      previous_sigterm = std::signal(SIGTERM, [](int) {
        signal_shutdown_requested = 1;
      });
      previous_sighup = std::signal(SIGHUP, [](int) {
        signal_shutdown_requested = 1;
      });
      previous_sigpipe = std::signal(SIGPIPE, SIG_IGN);
    }
    ~SignalHandlerInstaller() {
      std::signal(SIGINT, previous_sigint);
      std::signal(SIGTERM, previous_sigterm);
      std::signal(SIGHUP, previous_sighup);
      std::signal(SIGPIPE, previous_sigpipe);
    }
  };
  SignalHandlerInstaller signal_installer;

  const auto check_signal_shutdown =
      [&interactive_exit_requested]() -> bool {
    if (signal_shutdown_requested == 0) {
      return false;
    }
    interactive_exit_requested = true;
    return true;
  };

  const auto dispatch_extension_shortcut =
      [&active_session, runtime, &persist_extension_entries,
       &cached_footer_lines, &compute_footer_lines,
       &make_interactive_extension_host, &extension_ui_state](std::string_view bytes)
      -> qt::foundation::Result<bool> {
    if (active_session.has_overlay()) return qt::foundation::Result<bool>::success(false);
    const auto matches = [bytes](std::string_view key) {
      return key == bytes || qt::tui::matches_key(bytes, key);
    };
    for (const auto& extension :
         runtime->services.resource_bundle.extensions.loaded_extensions) {
      if (!extension.script_runtime || !extension.host_call ||
          !*extension.host_call || !extension.manifest.metadata.is_object() ||
          !extension.manifest.metadata.contains("shortcuts")) {
        continue;
      }
      for (const auto& shortcut : extension.manifest.metadata.at("shortcuts")) {
        if (!shortcut.is_object() || matches(shortcut.value("key", ""))) {
          if (!shortcut.is_object()) continue;
          const auto handler = shortcut.value("handler", "");
          if (handler.empty()) continue;
          const auto baseline = runtime->host.session_manager().get_entries().size();
          const auto invocation_slot =
              std::make_shared<qt::extensions::ExtensionHostCall>();
          extension_ui_state.invocation_slot = invocation_slot;
          auto host_call = make_interactive_extension_host(
              active_session, extension.script_runtime, invocation_slot);
          *invocation_slot = host_call;
          auto result = extension.script_runtime->invoke(
              handler, qt::ai::Json{{"key", std::string(bytes)}},
              *invocation_slot);
          const auto persisted = persist_extension_entries(baseline);
          if (!persisted) return persisted.error();
          if (!result) {
            active_session.append_transcript_line("error: " + result.error().message);
          }
          cached_footer_lines = compute_footer_lines();
          const auto rendered = active_session.render();
          if (!rendered) return rendered.error();
          return qt::foundation::Result<bool>::success(true);
        }
      }
    }
    return qt::foundation::Result<bool>::success(false);
  };

  if (notcurses_session.using_notcurses()) {
    while (!shell.should_quit() && !interactive_exit_requested) {
      if (check_signal_shutdown()) {
        active_session.append_transcript_line("[signal: shutting down]");
        break;
      }
      const auto ui_post_status = flush_ui_post_queue(active_session);
      if (!ui_post_status) {
        std::cerr << "qt: " << ui_post_status.error().message << '\n';
        const auto stop = stop_terminal_session();
        (void)stop;
        return EXIT_FAILURE;
      }
      const auto raw_mode_status = terminal_adapter.enter_raw_mode();
      if (!raw_mode_status) {
        std::cerr << "qt: " << raw_mode_status.error().message << '\n';
        const auto stop = stop_terminal_session();
        (void)stop;
        return EXIT_FAILURE;
      }
      if (footer_data_provider.refresh()) {
        footer_branch_changed = true;
      }
      if (footer_branch_changed) {
        footer_branch_changed = false;
        cached_footer_lines = compute_footer_lines();
        const auto rendered = active_session.render();
        if (!rendered) {
          std::cerr << "qt: " << rendered.error().message << '\n';
          const auto stop = stop_terminal_session();
          (void)stop;
          return EXIT_FAILURE;
        }
      }
      const auto provider_status = refresh_provider_status_footer();
      if (!provider_status) {
        std::cerr << "qt: " << provider_status.error().message << '\n';
        const auto stop = stop_terminal_session();
        (void)stop;
        return EXIT_FAILURE;
      }
      const auto model_catalog_status = refresh_remote_model_catalogs();
      if (!model_catalog_status) {
        std::cerr << "qt: " << model_catalog_status.error().message << '\n';
        const auto stop = stop_terminal_session();
        (void)stop;
        return EXIT_FAILURE;
      }

      const auto version_status = flush_pending_version_notification(false);
      if (!version_status) {
        std::cerr << "qt: " << version_status.error().message << '\n';
        const auto stop = stop_terminal_session();
        (void)stop;
        return EXIT_FAILURE;
      }

      const auto turn_status = flush_completed_turn(false);
      if (!turn_status) {
        std::cerr << "qt: " << turn_status.error().message << '\n';
        const auto stop = stop_terminal_session();
        (void)stop;
        return EXIT_FAILURE;
      }
      const auto bash_status = flush_completed_bash(false);
      if (!bash_status) {
        std::cerr << "qt: " << bash_status.error().message << '\n';
        const auto stop = stop_terminal_session();
        (void)stop;
        return EXIT_FAILURE;
      }
      const auto working_status = refresh_working_indicator(false);
      if (!working_status) {
        std::cerr << "qt: " << working_status.error().message << '\n';
        const auto stop = stop_terminal_session();
        (void)stop;
        return EXIT_FAILURE;
      }
      const auto progress_status = refresh_terminal_progress();
      if (!progress_status) {
        std::cerr << "qt: " << progress_status.error().message << '\n';
        const auto stop = stop_terminal_session();
        (void)stop;
        return EXIT_FAILURE;
      }
      const auto keyboard_status = refresh_keyboard_protocol();
      if (!keyboard_status) {
        std::cerr << "qt: " << keyboard_status.error().message << '\n';
        const auto stop = stop_terminal_session();
        (void)stop;
        return EXIT_FAILURE;
      }
      const auto review_notification_status = poll_review_notifications();
      if (!review_notification_status) {
        std::cerr << "qt: "
                  << review_notification_status.error().message << '\n';
        const auto stop = stop_terminal_session();
        (void)stop;
        return EXIT_FAILURE;
      }
      const auto notcurses_events = notcurses_session.poll_input(false);
      if (!notcurses_events) {
        std::cerr << "qt: " << notcurses_events.error().message << '\n';
        const auto stop = stop_terminal_session();
        (void)stop;
        return EXIT_FAILURE;
      }
      const auto post_poll_raw_mode_status = terminal_adapter.enter_raw_mode();
      if (!post_poll_raw_mode_status) {
        std::cerr << "qt: " << post_poll_raw_mode_status.error().message << '\n';
        const auto stop = stop_terminal_session();
        (void)stop;
        return EXIT_FAILURE;
      }
      if (!notcurses_events.value().empty()) {
        for (const auto& event : notcurses_events.value()) {
          if (event.bytes.empty()) {
            continue;
          }
          if (runtime->services.memory_runtime) {
            runtime->services.memory_runtime->note_activity();
          }
          const auto shortcut = dispatch_extension_shortcut(event.bytes);
          if (!shortcut) {
            std::cerr << "qt: " << shortcut.error().message << '\n';
            return EXIT_FAILURE;
          }
          if (shortcut.value()) continue;
          const auto steps = active_session.handle_input(event.bytes);
          if (!steps) {
            std::cerr << "qt: " << steps.error().message << '\n';
            const auto stop = stop_terminal_session();
            (void)stop;
            return EXIT_FAILURE;
          }
          if (shell.should_quit() || interactive_exit_requested) {
            break;
          }
        }
        continue;
      }

      if (!notcurses_session.using_notcurses() &&
          !terminal_input_pump.running()) {
        (void)terminal_input_pump.start();
      }

      auto stdin_batches = terminal_input_pump.wait_and_drain(
          active_session.has_pending_input() ? std::chrono::milliseconds(5)
                                             : std::chrono::milliseconds(25));
      if (stdin_batches.empty()) {
        if (active_session.has_pending_input()) {
          const auto steps = active_session.handle_input("");
          if (!steps) {
            std::cerr << "qt: " << steps.error().message << '\n';
            const auto stop = stop_terminal_session();
            (void)stop;
            return EXIT_FAILURE;
          }
        }
        if (!terminal_input_pump.running()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        continue;
      }

      for (auto& bytes : stdin_batches) {
        if (!bytes.empty() && runtime->services.memory_runtime) {
          runtime->services.memory_runtime->note_activity();
        }
        std::replace(bytes.begin(), bytes.end(), '\n', '\r');
        const auto shortcut = dispatch_extension_shortcut(bytes);
        if (!shortcut) {
          std::cerr << "qt: " << shortcut.error().message << '\n';
          return EXIT_FAILURE;
        }
        if (shortcut.value()) continue;
        const auto steps = active_session.handle_input(bytes);
        if (!steps) {
          std::cerr << "qt: " << steps.error().message << '\n';
          const auto stop = stop_terminal_session();
          (void)stop;
          return EXIT_FAILURE;
        }
        if (shell.should_quit() || interactive_exit_requested) {
          break;
        }
      }
    }
  } else {
    (void)terminal_input_pump.start();
    while (!shell.should_quit() && !interactive_exit_requested) {
      if (check_signal_shutdown()) {
        active_session.append_transcript_line("[signal: shutting down]");
        break;
      }
      const auto ui_post_status = flush_ui_post_queue(active_session);
      if (!ui_post_status) {
        std::cerr << "qt: " << ui_post_status.error().message << '\n';
        const auto stop = stop_terminal_session();
        (void)stop;
        return EXIT_FAILURE;
      }
      const auto provider_status = refresh_provider_status_footer();
      if (!provider_status) {
        std::cerr << "qt: " << provider_status.error().message << '\n';
        const auto stop = stop_terminal_session();
        (void)stop;
        return EXIT_FAILURE;
      }
      const auto model_catalog_status = refresh_remote_model_catalogs();
      if (!model_catalog_status) {
        std::cerr << "qt: " << model_catalog_status.error().message << '\n';
        const auto stop = stop_terminal_session();
        (void)stop;
        return EXIT_FAILURE;
      }
      const auto turn_status = flush_completed_turn(false);
      if (!turn_status) {
        std::cerr << "qt: " << turn_status.error().message << '\n';
        const auto stop = stop_terminal_session();
        (void)stop;
        return EXIT_FAILURE;
      }
      const auto bash_status = flush_completed_bash(false);
      if (!bash_status) {
        std::cerr << "qt: " << bash_status.error().message << '\n';
        const auto stop = stop_terminal_session();
        (void)stop;
        return EXIT_FAILURE;
      }
      const auto working_status = refresh_working_indicator(false);
      if (!working_status) {
        std::cerr << "qt: " << working_status.error().message << '\n';
        const auto stop = stop_terminal_session();
        (void)stop;
        return EXIT_FAILURE;
      }
      const auto progress_status = refresh_terminal_progress();
      if (!progress_status) {
        std::cerr << "qt: " << progress_status.error().message << '\n';
        const auto stop = stop_terminal_session();
        (void)stop;
        return EXIT_FAILURE;
      }
      const auto keyboard_status = refresh_keyboard_protocol();
      if (!keyboard_status) {
        std::cerr << "qt: " << keyboard_status.error().message << '\n';
        const auto stop = stop_terminal_session();
        (void)stop;
        return EXIT_FAILURE;
      }
      const auto review_notification_status = poll_review_notifications();
      if (!review_notification_status) {
        std::cerr << "qt: "
                  << review_notification_status.error().message << '\n';
        const auto stop = stop_terminal_session();
        (void)stop;
        return EXIT_FAILURE;
      }
      auto stdin_batches = terminal_input_pump.wait_and_drain(
          active_session.has_pending_input() ? std::chrono::milliseconds(5)
                                             : std::chrono::milliseconds(25));
      if (stdin_batches.empty()) {
        if (active_session.has_pending_input()) {
          const auto steps = active_session.handle_input("");
          if (!steps) {
            std::cerr << "qt: " << steps.error().message << '\n';
            const auto stop = stop_terminal_session();
            (void)stop;
            return EXIT_FAILURE;
          }
        }
        if (!terminal_input_pump.running()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        continue;
      }
      for (auto& bytes : stdin_batches) {
        if (!bytes.empty() && runtime->services.memory_runtime) {
          runtime->services.memory_runtime->note_activity();
        }
        std::replace(bytes.begin(), bytes.end(), '\n', '\r');
        const auto shortcut = dispatch_extension_shortcut(bytes);
        if (!shortcut) {
          std::cerr << "qt: " << shortcut.error().message << '\n';
          return EXIT_FAILURE;
        }
        if (shortcut.value()) continue;
        const auto steps = active_session.handle_input(bytes);
        if (!steps) {
          std::cerr << "qt: " << steps.error().message << '\n';
          const auto stop = stop_terminal_session();
          (void)stop;
          return EXIT_FAILURE;
        }
        if (shell.should_quit() || interactive_exit_requested) {
          break;
        }
      }
    }
  }
  ui_post_queue->alive.store(false);
  theme_watcher.stop_watching();
  const auto overlay_clear_status = clear_exit_overlays();
  if (!overlay_clear_status) {
    std::cerr << "qt: " << overlay_clear_status.error().message << '\n';
    const auto stop = stop_terminal_session();
    (void)stop;
    return EXIT_FAILURE;
  }
  const auto final_turn_status = flush_completed_turn(true);
  if (!final_turn_status) {
    std::cerr << "qt: " << final_turn_status.error().message << '\n';
    const auto stop = stop_terminal_session();
    (void)stop;
    return EXIT_FAILURE;
  }
  const auto final_bash_status = flush_completed_bash(true);
  if (!final_bash_status) {
    std::cerr << "qt: " << final_bash_status.error().message << '\n';
    const auto stop = stop_terminal_session();
    (void)stop;
    return EXIT_FAILURE;
  }
  (void)runtime->host.harness().unsubscribe(live_turn_subscription);
  const auto stop_status = stop_terminal_session();
  if (!stop_status) {
    restore_cursor();
    std::cerr << "qt: " << stop_status.error().message << '\n';
    log.error("mode_exit_failed",
              {qt::foundation::log_field("error",
                                         stop_status.error().message)});
    return EXIT_FAILURE;
  }
  log.info("mode_exit", {qt::foundation::log_field("code", EXIT_SUCCESS)});
  return EXIT_SUCCESS;
}

}  // namespace qt::app
