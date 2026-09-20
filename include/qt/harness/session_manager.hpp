#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <qt/ai/models.hpp>
#include <qt/foundation/error.hpp>
#include <qt/harness/session.hpp>

namespace qt::harness {

struct SessionManagerModelState {
  std::string provider;
  std::string model_id;
};

struct SessionManagerContext {
  std::vector<qt::ai::Message> messages;
  std::string thinking_level = "off";
  std::optional<SessionManagerModelState> model;
};

struct SessionEntryTreeNode {
  SessionEntry entry;
  std::vector<SessionEntryTreeNode> children;
};

class SessionManager {
 public:
  SessionManager();
  explicit SessionManager(std::vector<SessionEntry> entries);

  const std::vector<SessionEntry>& entries() const noexcept { return entries_; }
  std::vector<SessionEntry> get_entries() const { return entries_; }

  std::optional<std::string> get_leaf_id() const noexcept { return leaf_id_; }
  const SessionEntry* get_leaf_entry() const noexcept;
  const SessionEntry* get_entry(std::string_view id) const noexcept;

  std::string append_user_message(std::string text);
  std::string append_user_message(
      std::vector<SessionUserContentBlock> content,
      std::optional<std::int64_t> message_timestamp = std::nullopt);
  std::string append_assistant_message(std::string text,
                                       std::string provider = "native",
                                       std::string model = "assistant",
                                       std::string api = "native",
                                       std::string stop_reason = "stop");
  std::string append_assistant_message(SessionAssistantMessageEntry entry);
  std::string append_model_change(std::string provider, std::string model_id);
  std::string append_thinking_level_change(std::string thinking_level);
  std::string append_session_info(
      std::optional<std::string> name = std::nullopt);
  qt::foundation::Result<std::string> append_label_change(
      std::string target_id, std::optional<std::string> label);
  std::string append_compaction(std::string summary,
                                std::string first_kept_entry_id,
                                std::int64_t tokens_before,
                                std::optional<Json> details = std::nullopt,
                                std::optional<bool> from_hook = std::nullopt);
  std::string append_extension_state(std::string custom_type,
                                     Json data = Json::object());
  std::string append_custom_entry(std::string entry_type,
                                  Json payload = Json::object());
  std::string append_custom_message(
      std::string custom_type, SessionCustomMessageContent content,
      bool display, std::optional<Json> details = std::nullopt);
  std::optional<std::string> get_session_name() const;
  std::optional<std::string> get_label(std::string_view target_id) const;

  qt::foundation::Status branch(std::string_view branch_from_id);
  qt::foundation::Result<std::string> branch_with_summary(
      std::optional<std::string> branch_from_id, std::string summary,
      std::optional<Json> details = std::nullopt,
      std::optional<bool> from_hook = std::nullopt);
  std::string append_entry(SessionEntry entry);
  void reset_leaf() noexcept;

  std::vector<SessionEntry> get_branch() const;
  std::vector<SessionEntry> get_branch(std::string_view from_id) const;
  std::vector<SessionEntryTreeNode> get_tree() const;

  SessionManagerContext build_session_context() const;
  SessionManagerContext build_session_context(std::string_view leaf_id) const;

 private:
  void rebuild_index();
  std::string generate_entry_id();
  std::string generate_timestamp();
  std::optional<std::string> last_tree_entry_id() const;

  std::vector<SessionEntry> entries_;
  std::optional<std::string> leaf_id_;
  std::vector<std::string> entry_order_;
  std::unordered_map<std::string, std::size_t> entry_indexes_;
  std::uint64_t next_id_ = 1;
  std::uint64_t next_timestamp_ = 1;
};

}  // namespace qt::harness
