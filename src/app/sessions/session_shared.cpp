#include "qt/app/sessions/session_shared.hpp"

#include "qt/ai/model_catalog.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace qt::app {
namespace {

using Json = nlohmann::json;

std::optional<std::string> session_entry_id(
    const qt::harness::SessionEntry& entry) {
  return std::visit(
      [](const auto& value) -> std::optional<std::string> {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, qt::harness::SessionMetadataEntry> ||
                      std::is_same_v<Value, qt::harness::SessionSummaryEntry>) {
          return std::nullopt;
        } else {
          return value.upstream.id;
        }
      },
      entry.data);
}

std::string rpc_user_text_from_json(const Json& message) {
  if (!message.is_object()) {
    return {};
  }
  if (message.contains("content") && message.at("content").is_string()) {
    return message.at("content").get<std::string>();
  }
  if (!message.contains("content") || !message.at("content").is_array()) {
    return {};
  }

  std::string text;
  for (const auto& content : message.at("content")) {
    if (!content.is_object() || content.value("type", std::string{}) != "text") {
      continue;
    }
    text += content.value("text", std::string{});
  }
  return text;
}

std::optional<std::string> session_user_text(
    const qt::harness::SessionEntry& entry) {
  if (const auto* user =
          std::get_if<qt::harness::SessionUserImageMessageEntry>(&entry.data)) {
    std::string text;
    for (const auto& block : user->content) {
      if (block.kind != qt::harness::SessionUserContentBlockKind::kText) {
        continue;
      }
      text += block.text;
    }
    if (!text.empty()) {
      return text;
    }
  }

  const auto* custom = std::get_if<qt::harness::SessionCustomEntry>(&entry.data);
  if (custom == nullptr ||
      custom->entry_type != qt::harness::kUpstreamPiMonoSessionEntryType ||
      !custom->payload.is_object() ||
      custom->payload.value("type", std::string{}) != "message" ||
      !custom->payload.contains("message") ||
      !custom->payload.at("message").is_object()) {
    return std::nullopt;
  }

  const auto& message = custom->payload.at("message");
  if (message.value("role", std::string{}) != "user") {
    return std::nullopt;
  }
  const std::string text = rpc_user_text_from_json(message);
  if (text.empty()) {
    return std::nullopt;
  }
  return text;
}

std::vector<ReferenceRpcForkMessage> collect_session_fork_messages(
    const std::vector<qt::harness::SessionEntry>& entries) {
  std::vector<ReferenceRpcForkMessage> messages;
  for (const auto& entry : entries) {
    const auto entry_id = session_entry_id(entry);
    const auto text = session_user_text(entry);
    if (!entry_id || !text) {
      continue;
    }
    messages.push_back({std::move(*entry_id), std::move(*text)});
  }
  return messages;
}

}  // namespace

std::optional<std::int64_t> rpc_context_window(
    const qt::ai::ModelRef& model) {
  const auto catalog = qt::ai::default_model_catalog();
  return rpc_context_window(catalog, model);
}

std::optional<std::int64_t> rpc_context_window(
    const qt::ai::ModelCatalog& catalog, const qt::ai::ModelRef& model) {
  const auto* record =
      qt::ai::find_model_record(catalog, model.provider, model.name);
  if (record == nullptr) {
    return std::nullopt;
  }
  const auto window = record->metadata.find("contextWindow");
  if (window == record->metadata.end() || !window->is_number_integer()) {
    return std::nullopt;
  }
  return window->get<std::int64_t>();
}

std::vector<ReferenceRpcForkMessage> session_fork_messages(
    const qt::harness::SessionManager& session_manager) {
  auto messages =
      collect_session_fork_messages(session_manager.get_branch());
  if (!messages.empty()) {
    return messages;
  }
  return collect_session_fork_messages(session_manager.entries());
}

}  // namespace qt::app
