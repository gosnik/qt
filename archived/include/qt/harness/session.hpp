#pragma once

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <qt/harness/context_discovery.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace qt::harness {

using Json = nlohmann::json;

constexpr int kSessionEntryVersion = 1;
constexpr const char* kUpstreamPiMonoSessionEntryType = "upstream.pi_mono";

struct SessionMetadata {
  std::string session_id;
  std::string working_directory;
  std::optional<std::string> display_name;
  std::optional<std::string> parent_session_id;
  std::optional<std::string> created_at;
  bool persistent = true;
  std::vector<std::string> prompt_history;
  std::optional<std::string> source_format = std::nullopt;
  std::optional<int> source_version = std::nullopt;
  Json upstream_extra = Json::object();
};

class SessionState {
 public:
  explicit SessionState(SessionMetadata metadata)
      : metadata_(std::move(metadata)) {}

  const SessionMetadata& metadata() const noexcept { return metadata_; }

  SessionMetadata& metadata() noexcept { return metadata_; }

  bool is_persistent() const noexcept {
    return metadata_.persistent && !metadata_.session_id.empty();
  }

  bool has_display_name() const noexcept {
    return metadata_.display_name.has_value() &&
           !metadata_.display_name->empty();
  }

  bool has_repo_context() const noexcept { return context_.has_value(); }

  const std::optional<RepoContextDiscovery>& context() const noexcept {
    return context_;
  }

  void attach_context(RepoContextDiscovery context) {
    context_ = std::move(context);
  }

  std::string storage_namespace() const {
    if (metadata_.working_directory.empty()) {
      return metadata_.session_id;
    }
    if (metadata_.session_id.empty()) {
      return metadata_.working_directory;
    }
    return metadata_.working_directory + "#" + metadata_.session_id;
  }

 private:
  SessionMetadata metadata_;
  std::optional<RepoContextDiscovery> context_;
};

enum class SessionEntryKind {
  kSessionMetadata,
  kLabel,
  kSessionInfo,
  kSummary,
  kModelChange,
  kThinkingLevelChange,
  kBashExecution,
  kBranchSummary,
  kCompaction,
  kUserImageMessage,
  kAssistantMessage,
  kToolResultMessage,
  kCustomMessage,
  kExtensionState,
  kCustom,
};

struct SessionMetadataEntry {
  SessionMetadata metadata;
};

struct SessionSummaryEntry {
  std::string summary;
};

struct SessionUpstreamTreeFields {
  std::optional<std::string> id;
  std::optional<std::string> parent_id;
  std::optional<std::string> timestamp;
  std::optional<std::string> source_format = std::nullopt;
  std::optional<int> source_version = std::nullopt;
};

struct SessionLabelEntry {
  SessionLabelEntry() = default;
  explicit SessionLabelEntry(std::string value) : label(std::move(value)) {}
  SessionLabelEntry(std::string value, std::optional<std::string> target,
                    bool clears, SessionUpstreamTreeFields fields)
      : label(std::move(value)),
        target_id(std::move(target)),
        clears_label(clears),
        upstream(std::move(fields)) {}

  std::string label;
  std::optional<std::string> target_id;
  bool clears_label = false;
  SessionUpstreamTreeFields upstream;
};

struct SessionInfoEntry {
  std::optional<std::string> name;
  SessionUpstreamTreeFields upstream;
};

struct SessionModelChangeEntry {
  std::string provider;
  std::string model_id;
  SessionUpstreamTreeFields upstream;
};

struct SessionThinkingLevelChangeEntry {
  std::string thinking_level;
  SessionUpstreamTreeFields upstream;
};

struct SessionBashExecutionEntry {
  std::string command;
  std::string output;
  std::optional<int> exit_code;
  bool cancelled = false;
  bool truncated = false;
  std::optional<std::string> full_output_path;
  std::optional<bool> exclude_from_context;
  std::optional<std::int64_t> message_timestamp;
  SessionUpstreamTreeFields upstream;
};

struct SessionBranchSummaryEntry {
  std::string from_id;
  std::string summary;
  std::optional<Json> details;
  std::optional<bool> from_hook;
  SessionUpstreamTreeFields upstream;
};

struct SessionCompactionEntry {
  std::string summary;
  std::string first_kept_entry_id;
  std::int64_t tokens_before = 0;
  std::optional<Json> details;
  std::optional<bool> from_hook;
  SessionUpstreamTreeFields upstream;
};

enum class SessionUserContentBlockKind {
  kText,
  kImage,
};

struct SessionUserContentBlock {
  SessionUserContentBlockKind kind = SessionUserContentBlockKind::kText;
  std::string text;
  std::optional<std::string> text_signature;
  std::string data;
  std::string mime_type;
};

struct SessionUserImageMessageEntry {
  std::vector<SessionUserContentBlock> content;
  std::optional<std::int64_t> message_timestamp;
  bool serialize_text_content_as_string = false;
  SessionUpstreamTreeFields upstream;
};

enum class SessionAssistantContentBlockKind {
  kText,
  kThinking,
  kToolCall,
};

struct SessionAssistantContentBlock {
  SessionAssistantContentBlockKind kind = SessionAssistantContentBlockKind::kText;
  std::string text;
  std::optional<std::string> text_signature;
  std::string thinking;
  std::optional<std::string> thinking_signature;
  std::optional<bool> redacted;
  std::string tool_call_id;
  std::string tool_name;
  Json tool_arguments = Json::object();
  std::optional<std::string> thought_signature;
};

struct SessionAssistantMessageEntry {
  std::vector<SessionAssistantContentBlock> content;
  std::string api;
  std::string provider;
  std::string model;
  std::optional<std::string> response_model;
  std::optional<std::string> response_id;
  std::optional<Json> diagnostics;
  Json usage = Json::object();
  std::string stop_reason;
  std::optional<std::string> error_message;
  std::optional<std::int64_t> message_timestamp;
  SessionUpstreamTreeFields upstream;
};

struct SessionToolResultMessageEntry {
  std::string tool_call_id;
  std::string tool_name;
  std::vector<SessionUserContentBlock> content;
  std::optional<Json> details;
  bool is_error = false;
  std::optional<std::int64_t> message_timestamp;
  SessionUpstreamTreeFields upstream;
};

using SessionCustomMessageContent =
    std::variant<std::string, std::vector<SessionUserContentBlock>>;

struct SessionCustomMessageEntry {
  std::string custom_type;
  SessionCustomMessageContent content = std::string{};
  std::optional<Json> details;
  bool display = false;
  bool serialize_as_message_role = false;
  std::optional<std::int64_t> message_timestamp;
  SessionUpstreamTreeFields upstream;
};

struct SessionExtensionStateEntry {
  std::string custom_type;
  Json data = Json::object();
  SessionUpstreamTreeFields upstream;
};

struct SessionCustomEntry {
  std::string entry_type;
  Json payload = Json::object();
  SessionUpstreamTreeFields upstream;
};

struct SessionTokenTotals {
  std::int64_t input = 0;
  std::int64_t output = 0;
  std::int64_t cache_read = 0;
  std::int64_t cache_write = 0;
  std::int64_t total = 0;
};

struct SessionContextUsage {
  std::optional<std::int64_t> tokens;
  std::int64_t context_window = 0;
  std::optional<double> percent;
};

struct SessionModelStats {
  std::size_t assistant_messages = 0;
  SessionTokenTotals tokens;
  double cost = 0.0;
  bool has_unknown_usage = false;
};

struct SessionStats {
  std::size_t user_messages = 0;
  std::size_t assistant_messages = 0;
  std::size_t tool_calls = 0;
  std::size_t tool_results = 0;
  std::size_t total_messages = 0;
  SessionTokenTotals tokens;
  double cost = 0.0;
  bool has_unknown_usage = false;
  std::map<std::string, SessionModelStats> models;
  std::optional<SessionContextUsage> context_usage;
};

using SessionEntryData =
    std::variant<SessionMetadataEntry, SessionLabelEntry, SessionInfoEntry,
                 SessionSummaryEntry, SessionModelChangeEntry,
                 SessionThinkingLevelChangeEntry,
                 SessionBashExecutionEntry, SessionBranchSummaryEntry,
                 SessionCompactionEntry,
                 SessionUserImageMessageEntry,
                 SessionAssistantMessageEntry,
                 SessionToolResultMessageEntry,
                 SessionCustomMessageEntry,
                 SessionExtensionStateEntry,
                 SessionCustomEntry>;

struct SessionEntry {
  int version = kSessionEntryVersion;
  SessionEntryData data;

  SessionEntryKind kind() const noexcept;
};

struct SessionEntryDecodeResult {
  std::optional<SessionEntry> entry;
  std::string error;

  explicit operator bool() const noexcept { return entry.has_value(); }
};

std::string to_string(SessionEntryKind kind);
bool parse_session_entry_kind(std::string_view text, SessionEntryKind& kind);

void to_json(Json& json, const SessionMetadata& metadata);
void from_json(const Json& json, SessionMetadata& metadata);

void to_json(Json& json, const SessionMetadataEntry& entry);
void from_json(const Json& json, SessionMetadataEntry& entry);

void to_json(Json& json, const SessionLabelEntry& entry);
void from_json(const Json& json, SessionLabelEntry& entry);

void to_json(Json& json, const SessionInfoEntry& entry);
void from_json(const Json& json, SessionInfoEntry& entry);

void to_json(Json& json, const SessionSummaryEntry& entry);
void from_json(const Json& json, SessionSummaryEntry& entry);

void to_json(Json& json, const SessionUpstreamTreeFields& fields);
void from_json(const Json& json, SessionUpstreamTreeFields& fields);

void to_json(Json& json, const SessionModelChangeEntry& entry);
void from_json(const Json& json, SessionModelChangeEntry& entry);

void to_json(Json& json, const SessionThinkingLevelChangeEntry& entry);
void from_json(const Json& json, SessionThinkingLevelChangeEntry& entry);

void to_json(Json& json, const SessionBashExecutionEntry& entry);
void from_json(const Json& json, SessionBashExecutionEntry& entry);

void to_json(Json& json, const SessionBranchSummaryEntry& entry);
void from_json(const Json& json, SessionBranchSummaryEntry& entry);

void to_json(Json& json, const SessionCompactionEntry& entry);
void from_json(const Json& json, SessionCompactionEntry& entry);

void to_json(Json& json, const SessionUserContentBlock& block);
void from_json(const Json& json, SessionUserContentBlock& block);

void to_json(Json& json, const SessionUserImageMessageEntry& entry);
void from_json(const Json& json, SessionUserImageMessageEntry& entry);

void to_json(Json& json, const SessionAssistantContentBlock& block);
void from_json(const Json& json, SessionAssistantContentBlock& block);

void to_json(Json& json, const SessionAssistantMessageEntry& entry);
void from_json(const Json& json, SessionAssistantMessageEntry& entry);

void to_json(Json& json, const SessionToolResultMessageEntry& entry);
void from_json(const Json& json, SessionToolResultMessageEntry& entry);

void to_json(Json& json, const SessionCustomMessageEntry& entry);
void from_json(const Json& json, SessionCustomMessageEntry& entry);

void to_json(Json& json, const SessionExtensionStateEntry& entry);
void from_json(const Json& json, SessionExtensionStateEntry& entry);

void to_json(Json& json, const SessionCustomEntry& entry);
void from_json(const Json& json, SessionCustomEntry& entry);

void to_json(Json& json, const SessionEntry& entry);
void from_json(const Json& json, SessionEntry& entry);

class SessionStatsAccumulator {
 public:
  void append(const SessionEntry& entry);
  [[nodiscard]] SessionStats snapshot(
      std::optional<std::int64_t> context_window = std::nullopt) const;
 private:
  SessionStats stats_;
  std::optional<std::int64_t> context_tokens_ = 0;
};

SessionStats compute_session_stats(
    const std::vector<SessionEntry>& entries,
    std::optional<std::int64_t> context_window = std::nullopt);

std::string encode_session_entry_jsonl(const SessionEntry& entry);
SessionEntryDecodeResult decode_session_entry_jsonl(std::string_view line);

}  // namespace qt::harness
