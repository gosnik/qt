#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace qt::web_core {

using Json = nlohmann::json;

struct SessionSummary {
  std::string session_id;
  std::string working_directory;
  std::optional<std::string> display_name;
  std::optional<std::string> latest_summary;
  std::size_t message_count = 0;
};

struct MessagePreview {
  std::string role;
  std::string text;
};

struct UsageSummary {
  std::int64_t input_tokens = 0;
  std::int64_t output_tokens = 0;
  std::int64_t cache_read_tokens = 0;
  std::int64_t cache_write_tokens = 0;
  std::int64_t total_tokens = 0;
  double total_cost_usd = 0.0;
};

struct WebUsageCostBreakdown {
  double input = 0.0;
  double output = 0.0;
  double cache_read = 0.0;
  double cache_write = 0.0;
  double total = 0.0;
};

struct WebSessionUsage {
  std::int64_t input = 0;
  std::int64_t output = 0;
  std::int64_t cache_read = 0;
  std::int64_t cache_write = 0;
  std::int64_t total_tokens = 0;
  WebUsageCostBreakdown cost;
};

struct WebSessionMetadata {
  std::string id;
  std::string title;
  std::string created_at;
  std::string last_modified;
  std::size_t message_count = 0;
  WebSessionUsage usage;
  std::string thinking_level = "off";
  std::string preview;
};

struct WebSessionData {
  std::string id;
  std::string title;
  Json model = Json::object();
  std::string thinking_level = "off";
  std::vector<Json> messages;
  std::string created_at;
  std::string last_modified;
};

struct WebSessionStorageMutation {
  std::string operation;
  std::optional<std::string> session_id;
  std::optional<WebSessionData> data;
  std::optional<WebSessionMetadata> metadata;
  std::optional<std::string> title;
};

struct WebExportToolDefinition {
  std::string name;
  std::string description;
  Json parameters = Json::object();
};

struct WebExportRenderedToolHtml {
  std::optional<std::string> call_html;
  std::optional<std::string> result_html_collapsed;
  std::optional<std::string> result_html_expanded;
};

struct WebExportSessionData {
  Json header = Json::object();
  std::vector<Json> entries;
  std::optional<std::string> leaf_id;
  std::optional<std::string> system_prompt;
  std::vector<WebExportToolDefinition> tools;
  std::map<std::string, WebExportRenderedToolHtml> rendered_tools;
};

struct ProviderKeyStatus {
  std::string provider;
  bool has_key = false;
  bool testing = false;
  bool failed = false;
  bool input_changed = false;
};

struct WebCustomProvider {
  std::string id;
  std::string name;
  std::string type;
  std::string base_url;
  std::optional<std::string> api_key;
  std::vector<Json> models;
};

struct WebDialogRequest {
  std::optional<std::string> id;
  std::string type;
  std::optional<std::string> title;
  std::optional<std::string> description;
  std::optional<std::string> provider;
  bool secret = false;
  std::optional<std::string> default_value;
  Json current_model = Json(nullptr);
  std::vector<std::string> allowed_providers;
  Json sessions = Json::array();
};

struct WebDialogResult {
  std::optional<std::string> id;
  std::string type;
  bool accepted = false;
  std::optional<std::string> value;
  std::optional<std::string> provider;
  std::optional<std::string> session_id;
  std::vector<std::string> deleted_session_ids;
  Json model = Json(nullptr);
  std::optional<bool> persistent_granted;
  std::optional<std::string> error_message;
};

struct WebAuthTokenMutation {
  std::string operation;
  std::string storage_key = "auth-token";
  std::optional<std::string> token;
  bool requested_with_prompt = false;
};

struct WebStorageQuotaInfo {
  std::int64_t usage = 0;
  std::int64_t quota = 0;
  double percent = 0.0;
};

struct WebStorageBackendRequest {
  std::string operation;
  std::optional<std::string> store_name;
  std::optional<std::string> key;
  Json value = Json(nullptr);
  std::optional<std::string> prefix;
  std::optional<std::string> index_name;
  std::string direction = "asc";
  std::vector<std::string> store_names;
  std::string mode = "readonly";
};

struct WebStorageBackendResult {
  std::string operation;
  bool success = true;
  Json value = Json(nullptr);
  std::vector<std::string> keys;
  Json values = Json::array();
  std::optional<bool> exists;
  std::optional<WebStorageQuotaInfo> quota_info;
  std::optional<bool> persistent_granted;
  std::optional<std::string> error_message;
};

struct WebStorageIndexConfig {
  std::string name;
  std::string key_path;
  bool unique = false;
};

struct WebStorageStoreConfig {
  std::string name;
  std::optional<std::string> key_path;
  bool auto_increment = false;
  std::vector<WebStorageIndexConfig> indices;
};

struct WebIndexedDBConfig {
  std::string db_name;
  int version = 1;
  std::vector<WebStorageStoreConfig> stores;
};

struct RenderBlock {
  std::string type;
  std::string text;
  Json data = Json::object();
};

struct WebContentBlock {
  std::string type;
  std::optional<std::string> text;
  std::optional<std::string> text_signature;
  std::optional<std::string> thinking;
  std::optional<std::string> thinking_signature;
  std::optional<bool> redacted;
  std::optional<std::string> data;
  std::optional<std::string> mime_type;
  std::optional<std::string> id;
  std::optional<std::string> name;
  Json arguments = Json::object();
  std::optional<std::string> thought_signature;
};

struct ToolRenderSnapshot {
  std::string call_id;
  std::string name;
  Json arguments = Json::object();
  Json result = Json(nullptr);
  bool pending = false;
  bool aborted = false;
  bool is_streaming = false;
  bool is_error = false;
  std::string state;
  std::string renderer = "default";
  bool is_custom = false;
};

struct WebAttachment {
  std::string id;
  std::string type;
  std::string file_name;
  std::string mime_type;
  std::uint64_t size = 0;
  std::string content;
  std::optional<std::string> extracted_text;
  std::optional<std::string> preview;
};

struct WebArtifactMessage {
  std::string role = "artifact";
  std::string action;
  std::string filename;
  std::optional<std::string> content;
  std::optional<std::string> title;
  std::string timestamp;
};

struct WebArtifactRuntimeOperation {
  std::string type = "artifact-operation";
  std::string action;
  std::optional<std::string> filename;
  std::optional<std::string> content;
  std::optional<std::string> mime_type;
};

struct WebArtifactRuntimeResponse {
  bool success = true;
  Json result = Json(nullptr);
  std::optional<std::string> error;
};

struct WebArtifactsToolParams {
  std::string command;
  std::string filename;
  std::optional<std::string> content;
  std::optional<std::string> old_str;
  std::optional<std::string> new_str;
};

struct WebToolResultMessage {
  std::string role = "toolResult";
  std::string tool_call_id;
  std::string tool_name;
  std::vector<Json> content;
  Json details = Json(nullptr);
  bool is_error = false;
  std::int64_t timestamp = 0;
};

struct WebToolExecutionDisplay {
  std::string tool_call_id;
  std::string tool_name;
  Json arguments = Json::object();
  std::optional<WebToolResultMessage> result;
  bool pending = false;
  bool aborted = false;
  bool is_streaming = false;
  std::string state;
  std::string renderer = "default";
  bool is_custom = false;
};

struct WebMessage {
  std::string id;
  std::string role;
  std::vector<RenderBlock> blocks;
  std::optional<UsageSummary> usage;
  std::optional<std::string> stop_reason;
  std::optional<std::string> error_message;
  Json metadata = Json::object();
};

struct WebSessionState {
  SessionSummary session;
  Json model = Json::object();
  std::string thinking_level;
  bool is_streaming = false;
  bool is_compacting = false;
  std::vector<WebMessage> messages;
};

struct WebSessionTreeNode {
  std::string id;
  std::string title;
  std::string working_directory;
  std::optional<std::string> parent_id;
  std::optional<std::string> branch_from_entry_id;
  std::string last_modified;
  std::size_t message_count = 0;
  bool active = false;
  std::vector<WebSessionTreeNode> children;
};

struct SessionMutationRequest {
  std::string command;
  std::optional<std::string> session_id;
  std::optional<std::string> title;
  std::optional<std::string> working_directory;
  std::optional<std::string> source_session_id;
  std::optional<std::string> branch_from_entry_id;
  Json model = Json::object();
  std::optional<std::string> thinking_level;
};

struct SessionMutationResponse {
  std::string command;
  bool ok = false;
  std::optional<std::string> session_id;
  std::optional<SessionSummary> session;
  std::optional<WebSessionState> state;
  std::optional<std::string> error_message;
};

struct BrowserSessionMutationCommand {
  std::optional<std::string> id;
  std::string type;
  std::optional<std::string> session_id;
  std::optional<std::string> session_path;
  std::optional<std::string> parent_session;
  std::optional<std::string> entry_id;
  std::optional<std::string> working_directory;
  Json options = Json::object();
};

struct BrowserSessionMutationResult {
  std::optional<std::string> id;
  std::string type = "response";
  std::string command;
  bool success = false;
  Json data = Json::object();
  std::optional<std::string> error;
  std::optional<std::string> session_id;
  std::optional<WebSessionState> state;
};

struct WebSessionCommand {
  std::string type;
  std::string session_id;
  std::optional<std::string> input;
  std::vector<WebAttachment> attachments;
  Json model = Json::object();
  std::optional<std::string> thinking_level;
  std::optional<std::string> message_id;
  std::optional<std::string> instructions;
};

struct StreamingDelta {
  std::string type;
  std::optional<std::string> message_id;
  std::optional<int> content_index;
  std::optional<std::string> delta;
  std::optional<std::string> content;
  Json tool_call = Json(nullptr);
  Json message = Json(nullptr);
  std::optional<std::string> reason;
  std::optional<std::string> error_message;
};

struct WebAssistantMessageEvent {
  std::string type;
  std::optional<int> content_index;
  std::optional<std::string> delta;
  std::optional<std::string> content;
  Json partial = Json(nullptr);
  Json tool_call = Json(nullptr);
  std::optional<std::string> reason;
  Json message = Json(nullptr);
  Json error = Json(nullptr);
};

struct StreamingStateSync {
  std::optional<std::string> system_prompt;
  Json model = Json(nullptr);
  std::optional<std::string> thinking_level;
  Json messages = Json::array();
  Json tools = Json::array();
  bool is_streaming = false;
  std::optional<WebMessage> streaming_message;
  std::vector<std::string> pending_tool_calls;
  std::optional<std::string> error_message;
};

struct WebAgentEvent {
  std::string type;
  Json message = Json(nullptr);
  Json messages = Json::array();
  Json assistant_message_event = Json(nullptr);
  Json tool_results = Json::array();
  Json steer = Json::array();
  Json follow_up = Json::array();
  Json next_turn = Json::array();
  std::optional<std::string> extension_path;
  std::optional<std::string> extension_event;
  std::optional<std::string> error;
  std::optional<std::string> tool_call_id;
  std::optional<std::string> tool_name;
  Json args = Json(nullptr);
  Json partial_result = Json(nullptr);
  Json result = Json(nullptr);
  std::optional<bool> is_error;
  std::optional<StreamingStateSync> state;
};

struct ModelStateChange {
  Json model = Json::object();
  Json previous_model = Json(nullptr);
  std::string source = "set";
};

struct ThinkingStateChange {
  std::string thinking_level;
  std::optional<std::string> previous_thinking_level;
};

struct WebRuntimeSyncEvent {
  std::string type;
  std::optional<std::string> session_id;
  std::optional<std::string> request_id;
  std::optional<BrowserSessionMutationCommand> session_command;
  std::optional<BrowserSessionMutationResult> session_result;
  Json message = Json(nullptr);
  Json messages = Json::array();
  Json assistant_message_event = Json(nullptr);
  Json tool_results = Json::array();
  std::optional<std::string> tool_call_id;
  std::optional<std::string> tool_name;
  Json args = Json(nullptr);
  Json partial_result = Json(nullptr);
  Json result = Json(nullptr);
  std::optional<bool> is_error;
  Json model = Json(nullptr);
  Json previous_model = Json(nullptr);
  std::optional<std::string> thinking_level;
  std::optional<std::string> previous_thinking_level;
  Json steer = Json::array();
  Json follow_up = Json::array();
  Json next_turn = Json::array();
  std::optional<std::string> reason;
  std::optional<bool> aborted;
  std::optional<bool> will_retry;
  std::optional<std::string> error_message;
  Json data = Json(nullptr);
  std::optional<StreamingStateSync> state;
};

struct RuntimeMessageEnvelope {
  std::string sandbox_id;
  std::optional<std::string> message_id;
  std::optional<std::string> type;
  Json payload = Json::object();
};

struct RuntimeResponseEnvelope {
  std::string sandbox_id;
  std::string message_id;
  bool success = true;
  std::optional<std::string> error_message;
  Json payload = Json::object();
};

struct WebReturnedFile {
  std::string type = "file-returned";
  std::string file_name;
  Json content = Json(nullptr);
  std::string mime_type;
};

struct WebSandboxAttachment {
  std::string id;
  std::string file_name;
  std::string mime_type;
  std::uint64_t size = 0;
  std::string content;
  std::optional<std::string> extracted_text;
};

struct WebSandboxAttachmentSummary {
  std::string id;
  std::string file_name;
  std::string mime_type;
  std::uint64_t size = 0;
};

struct WebConsoleMessage {
  std::string type = "console";
  std::string method;
  std::string text;
  Json args = Json::array();
};

struct WebExecutionCompletionMessage {
  std::string type;
  Json error = Json(nullptr);
  Json return_value = Json(nullptr);
};

struct WebSandboxFile {
  std::string file_name;
  Json content = Json(nullptr);
  std::string mime_type;
};

struct WebSandboxConsoleLog {
  std::string type;
  std::string text;
  Json args = Json::array();
};

struct WebSandboxResult {
  bool success = false;
  std::vector<WebSandboxConsoleLog> console;
  std::vector<WebSandboxFile> files;
  Json error = Json(nullptr);
  Json return_value = Json(nullptr);
};

struct WebJavaScriptReplFile {
  std::string file_name;
  std::string mime_type;
  std::uint64_t size = 0;
  std::string content_base64;
};

struct WebJavaScriptReplParams {
  std::string title;
  std::string code;
};

struct WebJavaScriptReplResult {
  std::vector<WebContentBlock> content;
  std::vector<WebJavaScriptReplFile> files;
};

struct WebExtractDocumentParams {
  std::string url;
};

struct WebExtractDocumentDetails {
  std::string extracted_text;
  std::string format;
  std::string file_name;
  std::uint64_t size = 0;
};

struct WebExtractDocumentResult {
  std::vector<WebContentBlock> content;
  WebExtractDocumentDetails details;
};

struct WebSandboxControlMessage {
  std::string type;
  std::optional<std::string> sandbox_id;
  std::optional<std::string> code;
  std::optional<std::string> error;
  std::optional<std::string> stack;
  std::optional<std::string> url;
};

struct TransportEnvelope {
  std::string jsonrpc = "2.0";
  std::string method;
  Json params = Json::object();
  Json id = nullptr;
  Json meta = Json::object();
};

struct SessionTranscriptSnapshot {
  SessionSummary session;
  std::vector<MessagePreview> messages;
};

std::vector<RenderBlock> build_render_blocks_from_content(
    const std::vector<WebContentBlock>& content);

std::string build_session_preview(const std::vector<Json>& messages,
                                  std::size_t max_bytes = 2048);

void to_json(Json& json, const SessionSummary& summary);
void from_json(const Json& json, SessionSummary& summary);

void to_json(Json& json, const MessagePreview& preview);
void from_json(const Json& json, MessagePreview& preview);

void to_json(Json& json, const UsageSummary& usage);
void from_json(const Json& json, UsageSummary& usage);

void to_json(Json& json, const WebUsageCostBreakdown& cost);
void from_json(const Json& json, WebUsageCostBreakdown& cost);

void to_json(Json& json, const WebSessionUsage& usage);
void from_json(const Json& json, WebSessionUsage& usage);

void to_json(Json& json, const WebSessionMetadata& metadata);
void from_json(const Json& json, WebSessionMetadata& metadata);

void to_json(Json& json, const WebSessionData& data);
void from_json(const Json& json, WebSessionData& data);

void to_json(Json& json, const WebSessionStorageMutation& mutation);
void from_json(const Json& json, WebSessionStorageMutation& mutation);

void to_json(Json& json, const WebExportToolDefinition& tool);
void from_json(const Json& json, WebExportToolDefinition& tool);

void to_json(Json& json, const WebExportRenderedToolHtml& html);
void from_json(const Json& json, WebExportRenderedToolHtml& html);

void to_json(Json& json, const WebExportSessionData& data);
void from_json(const Json& json, WebExportSessionData& data);

void to_json(Json& json, const ProviderKeyStatus& status);
void from_json(const Json& json, ProviderKeyStatus& status);

void to_json(Json& json, const WebCustomProvider& provider);
void from_json(const Json& json, WebCustomProvider& provider);

void to_json(Json& json, const WebDialogRequest& request);
void from_json(const Json& json, WebDialogRequest& request);

void to_json(Json& json, const WebDialogResult& result);
void from_json(const Json& json, WebDialogResult& result);

void to_json(Json& json, const WebAuthTokenMutation& mutation);
void from_json(const Json& json, WebAuthTokenMutation& mutation);

void to_json(Json& json, const WebStorageQuotaInfo& info);
void from_json(const Json& json, WebStorageQuotaInfo& info);

void to_json(Json& json, const WebStorageBackendRequest& request);
void from_json(const Json& json, WebStorageBackendRequest& request);

void to_json(Json& json, const WebStorageBackendResult& result);
void from_json(const Json& json, WebStorageBackendResult& result);

void to_json(Json& json, const WebStorageIndexConfig& config);
void from_json(const Json& json, WebStorageIndexConfig& config);

void to_json(Json& json, const WebStorageStoreConfig& config);
void from_json(const Json& json, WebStorageStoreConfig& config);

void to_json(Json& json, const WebIndexedDBConfig& config);
void from_json(const Json& json, WebIndexedDBConfig& config);

void to_json(Json& json, const RenderBlock& block);
void from_json(const Json& json, RenderBlock& block);

void to_json(Json& json, const WebContentBlock& block);
void from_json(const Json& json, WebContentBlock& block);

void to_json(Json& json, const ToolRenderSnapshot& snapshot);
void from_json(const Json& json, ToolRenderSnapshot& snapshot);

void to_json(Json& json, const WebAttachment& attachment);
void from_json(const Json& json, WebAttachment& attachment);

void to_json(Json& json, const WebArtifactMessage& message);
void from_json(const Json& json, WebArtifactMessage& message);

void to_json(Json& json, const WebArtifactRuntimeOperation& operation);
void from_json(const Json& json, WebArtifactRuntimeOperation& operation);

void to_json(Json& json, const WebArtifactRuntimeResponse& response);
void from_json(const Json& json, WebArtifactRuntimeResponse& response);

void to_json(Json& json, const WebArtifactsToolParams& params);
void from_json(const Json& json, WebArtifactsToolParams& params);

void to_json(Json& json, const WebToolResultMessage& message);
void from_json(const Json& json, WebToolResultMessage& message);

void to_json(Json& json, const WebToolExecutionDisplay& display);
void from_json(const Json& json, WebToolExecutionDisplay& display);

void to_json(Json& json, const WebMessage& message);
void from_json(const Json& json, WebMessage& message);

void to_json(Json& json, const WebSessionState& state);
void from_json(const Json& json, WebSessionState& state);

void to_json(Json& json, const WebSessionTreeNode& node);
void from_json(const Json& json, WebSessionTreeNode& node);

void to_json(Json& json, const SessionMutationRequest& request);
void from_json(const Json& json, SessionMutationRequest& request);

void to_json(Json& json, const SessionMutationResponse& response);
void from_json(const Json& json, SessionMutationResponse& response);

void to_json(Json& json, const BrowserSessionMutationCommand& command);
void from_json(const Json& json, BrowserSessionMutationCommand& command);

void to_json(Json& json, const BrowserSessionMutationResult& result);
void from_json(const Json& json, BrowserSessionMutationResult& result);

void to_json(Json& json, const WebSessionCommand& command);
void from_json(const Json& json, WebSessionCommand& command);

void to_json(Json& json, const StreamingDelta& delta);
void from_json(const Json& json, StreamingDelta& delta);

void to_json(Json& json, const WebAssistantMessageEvent& event);
void from_json(const Json& json, WebAssistantMessageEvent& event);

void to_json(Json& json, const StreamingStateSync& sync);
void from_json(const Json& json, StreamingStateSync& sync);

void to_json(Json& json, const WebAgentEvent& event);
void from_json(const Json& json, WebAgentEvent& event);

void to_json(Json& json, const ModelStateChange& change);
void from_json(const Json& json, ModelStateChange& change);

void to_json(Json& json, const ThinkingStateChange& change);
void from_json(const Json& json, ThinkingStateChange& change);

void to_json(Json& json, const WebRuntimeSyncEvent& event);
void from_json(const Json& json, WebRuntimeSyncEvent& event);

void to_json(Json& json, const RuntimeMessageEnvelope& envelope);
void from_json(const Json& json, RuntimeMessageEnvelope& envelope);

void to_json(Json& json, const RuntimeResponseEnvelope& envelope);
void from_json(const Json& json, RuntimeResponseEnvelope& envelope);

void to_json(Json& json, const WebReturnedFile& file);
void from_json(const Json& json, WebReturnedFile& file);

void to_json(Json& json, const WebSandboxAttachment& attachment);
void from_json(const Json& json, WebSandboxAttachment& attachment);

void to_json(Json& json, const WebSandboxAttachmentSummary& attachment);
void from_json(const Json& json, WebSandboxAttachmentSummary& attachment);

void to_json(Json& json, const WebConsoleMessage& message);
void from_json(const Json& json, WebConsoleMessage& message);

void to_json(Json& json, const WebExecutionCompletionMessage& message);
void from_json(const Json& json, WebExecutionCompletionMessage& message);

void to_json(Json& json, const WebSandboxFile& file);
void from_json(const Json& json, WebSandboxFile& file);

void to_json(Json& json, const WebSandboxConsoleLog& log);
void from_json(const Json& json, WebSandboxConsoleLog& log);

void to_json(Json& json, const WebSandboxResult& result);
void from_json(const Json& json, WebSandboxResult& result);

void to_json(Json& json, const WebJavaScriptReplFile& file);
void from_json(const Json& json, WebJavaScriptReplFile& file);

void to_json(Json& json, const WebJavaScriptReplParams& params);
void from_json(const Json& json, WebJavaScriptReplParams& params);

void to_json(Json& json, const WebJavaScriptReplResult& result);
void from_json(const Json& json, WebJavaScriptReplResult& result);

void to_json(Json& json, const WebExtractDocumentParams& params);
void from_json(const Json& json, WebExtractDocumentParams& params);

void to_json(Json& json, const WebExtractDocumentDetails& details);
void from_json(const Json& json, WebExtractDocumentDetails& details);

void to_json(Json& json, const WebExtractDocumentResult& result);
void from_json(const Json& json, WebExtractDocumentResult& result);

void to_json(Json& json, const WebSandboxControlMessage& message);
void from_json(const Json& json, WebSandboxControlMessage& message);

void to_json(Json& json, const TransportEnvelope& envelope);
void from_json(const Json& json, TransportEnvelope& envelope);

void to_json(Json& json, const SessionTranscriptSnapshot& snapshot);
void from_json(const Json& json, SessionTranscriptSnapshot& snapshot);

}  // namespace qt::web_core
