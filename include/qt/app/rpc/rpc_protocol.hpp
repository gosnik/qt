#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <qt/foundation/error.hpp>

namespace qt::app {

using Json = nlohmann::json;

enum class RpcStreamingBehavior {
  kSteer,
  kFollowUp,
};

enum class RpcCommandKind {
  kPrompt,
  kSteer,
  kFollowUp,
  kAbort,
  kGetState,
  kGetMessages,
  kSetModel,
  kCycleModel,
  kGetAvailableModels,
  kSetThinkingLevel,
  kCycleThinkingLevel,
  kSetSteeringMode,
  kSetFollowUpMode,
  kCompact,
  kSetAutoCompaction,
  kSetAutoRetry,
  kAbortRetry,
  kBash,
  kAbortBash,
  kNewSession,
  kSwitchSession,
  kFork,
  kClone,
  kGetSessionStats,
  kGetForkMessages,
  kExportHtml,
  kGetLastAssistantText,
  kSetSessionName,
  kGetCommands,
  kMemory,
  kSubagent,
};

enum class RpcResponseKind {
  kSuccess,
  kError,
  kGetState,
  kSetModel,
};

enum class RpcEventKind {
  kAgentStart,
  kAgentEnd,
  kTurnStart,
  kTurnEnd,
  kMessageStart,
  kMessageUpdate,
  kToolExecutionStart,
  kToolExecutionUpdate,
  kToolExecutionEnd,
  kQueueUpdate,
  kCompactionStart,
  kCompactionEnd,
  kAutoRetryStart,
  kAutoRetryEnd,
  kExtensionError,
  kExtensionUiRequest,
  kMessageEnd,
};

struct PromptCommand {
  std::optional<std::string> id;
  RpcCommandKind command = RpcCommandKind::kPrompt;
  std::string message;
  std::optional<Json> images;
  std::optional<RpcStreamingBehavior> streaming_behavior;
};

struct AbortCommand {
  std::optional<std::string> id;
};

struct GetStateCommand {
  std::optional<std::string> id;
};

struct GetMessagesCommand {
  std::optional<std::string> id;
};

struct NewSessionCommand {
  std::optional<std::string> id;
  std::optional<std::string> parent_session;
};

struct SwitchSessionCommand {
  std::optional<std::string> id;
  std::string session_path;
};

struct ForkCommand {
  std::optional<std::string> id;
  std::string entry_id;
};

struct CloneCommand {
  std::optional<std::string> id;
};

struct SetModelCommand {
  std::optional<std::string> id;
  std::string provider;
  std::string model_id;
};

struct CycleModelCommand {
  std::optional<std::string> id;
};

struct GetAvailableModelsCommand {
  std::optional<std::string> id;
};

struct SetThinkingLevelCommand {
  std::optional<std::string> id;
  std::string level;
};

struct CycleThinkingLevelCommand {
  std::optional<std::string> id;
};

struct SetQueueModeCommand {
  std::optional<std::string> id;
  RpcCommandKind command = RpcCommandKind::kSetSteeringMode;
  std::string mode;
};

struct CompactCommand {
  std::optional<std::string> id;
  std::optional<std::string> custom_instructions;
};

struct SetBooleanModeCommand {
  std::optional<std::string> id;
  RpcCommandKind command = RpcCommandKind::kSetAutoCompaction;
  bool enabled = false;
};

struct AbortRetryCommand {
  std::optional<std::string> id;
};

struct BashCommand {
  std::optional<std::string> id;
  std::string command;
};

struct AbortBashCommand {
  std::optional<std::string> id;
};

struct GetSessionStatsCommand {
  std::optional<std::string> id;
};

struct GetForkMessagesCommand {
  std::optional<std::string> id;
};

struct ExportHtmlCommand {
  std::optional<std::string> id;
  std::optional<std::string> output_path;
};

struct GetLastAssistantTextCommand {
  std::optional<std::string> id;
};

struct SetSessionNameCommand {
  std::optional<std::string> id;
  std::string name;
};

struct GetCommandsCommand {
  std::optional<std::string> id;
};

struct MemoryCommand {
  std::optional<std::string> id;
  std::string action;
  Json params = Json::object();
  bool confirm = false;
};

struct SubagentCommand {
  std::optional<std::string> id;
  std::string action;
  Json arguments = Json::object();
};

using RpcCommand =
    std::variant<PromptCommand,
                 AbortCommand,
                 GetStateCommand,
                 GetMessagesCommand,
                 NewSessionCommand,
                 SwitchSessionCommand,
                 ForkCommand,
                 CloneCommand,
                 SetModelCommand,
                 CycleModelCommand,
                 GetAvailableModelsCommand,
                 SetThinkingLevelCommand,
                 CycleThinkingLevelCommand,
                 SetQueueModeCommand,
                 CompactCommand,
                 SetBooleanModeCommand,
                 AbortRetryCommand,
                 BashCommand,
                 AbortBashCommand,
                 GetSessionStatsCommand,
                 GetForkMessagesCommand,
                 ExportHtmlCommand,
                 GetLastAssistantTextCommand,
                 SetSessionNameCommand,
                 GetCommandsCommand,
                 MemoryCommand,
                 SubagentCommand>;

struct RpcSessionState {
  Json model = nullptr;
  std::string thinking_level;
  bool is_streaming = false;
  bool is_compacting = false;
  std::string steering_mode;
  std::string follow_up_mode;
  std::optional<std::string> session_file;
  std::string session_id;
  std::optional<std::string> session_name;
  bool auto_compaction_enabled = false;
  std::size_t message_count = 0;
  std::size_t pending_message_count = 0;
};

struct RpcSuccessResponse {
  std::optional<std::string> id;
  std::string command;
  std::optional<Json> data;
};

struct RpcErrorResponse {
  std::optional<std::string> id;
  std::string command;
  std::string error;
  // Stable machine-readable classification. Older peers may omit this field.
  std::string error_code = "internal_error";
};

struct GetStateResponse {
  std::optional<std::string> id;
  RpcSessionState data;
};

struct SetModelResponse {
  std::optional<std::string> id;
  Json data = Json::object();
};

using RpcResponse = std::variant<RpcSuccessResponse,
                                 RpcErrorResponse,
                                 GetStateResponse,
                                 SetModelResponse>;

struct AgentStartEvent {};

struct AgentEndEvent {
  Json messages = Json::array();
};

struct TurnStartEvent {};

struct TurnEndEvent {
  Json message = Json::object();
  Json tool_results = Json::array();
};

struct MessageStartEvent {
  Json message = Json::object();
};

struct MessageUpdateEvent {
  Json message = Json::object();
  Json assistant_message_event = Json::object();
};

struct ToolExecutionStartEvent {
  std::string tool_call_id;
  std::string tool_name;
  Json args = Json::object();
};

struct ToolExecutionUpdateEvent {
  std::string tool_call_id;
  std::string tool_name;
  Json args = Json::object();
  Json partial_result = Json::object();
};

struct ToolExecutionEndEvent {
  std::string tool_call_id;
  std::string tool_name;
  Json result = Json::object();
  bool is_error = false;
};

struct QueueUpdateEvent {
  std::vector<std::string> steering;
  std::vector<std::string> follow_up;
};

struct CompactionStartEvent {
  std::string reason;
};

struct CompactionEndEvent {
  std::string reason;
  std::optional<Json> result;
  bool aborted = false;
  bool will_retry = false;
  std::optional<std::string> error_message;
};

struct AutoRetryStartEvent {
  std::size_t attempt = 0;
  std::size_t max_attempts = 0;
  std::size_t delay_ms = 0;
  std::string error_message;
};

struct AutoRetryEndEvent {
  bool success = false;
  std::size_t attempt = 0;
  std::optional<std::string> final_error;
};

struct ExtensionErrorEvent {
  std::string extension_path;
  std::string event;
  std::string error;
};

struct ExtensionUiRequestEvent {
  std::string id;
  std::string method;
  Json payload = Json::object();
};

struct MessageEndEvent {
  Json message = Json::object();
};

using RpcEvent = std::variant<AgentStartEvent,
                              AgentEndEvent,
                              TurnStartEvent,
                              TurnEndEvent,
                              MessageStartEvent,
                              MessageUpdateEvent,
                              ToolExecutionStartEvent,
                              ToolExecutionUpdateEvent,
                              ToolExecutionEndEvent,
                              QueueUpdateEvent,
                              CompactionStartEvent,
                              CompactionEndEvent,
                              AutoRetryStartEvent,
                              AutoRetryEndEvent,
                              ExtensionErrorEvent,
                              ExtensionUiRequestEvent,
                              MessageEndEvent>;

struct ExtensionUiResponseFrame {
  std::string id;
  Json payload = Json::object();
};

struct RpcDecodedFrame {
  std::variant<RpcCommand, RpcResponse, RpcEvent, ExtensionUiResponseFrame>
      payload;
};

std::string to_string(RpcStreamingBehavior behavior);
bool parse_rpc_streaming_behavior(std::string_view text,
                                  RpcStreamingBehavior& behavior) noexcept;

std::string to_string(RpcCommandKind kind);
bool parse_rpc_command_kind(std::string_view text, RpcCommandKind& kind) noexcept;
RpcCommandKind rpc_command_kind(const RpcCommand& command) noexcept;

std::string to_string(RpcResponseKind kind);
RpcResponseKind rpc_response_kind(const RpcResponse& response) noexcept;

std::string to_string(RpcEventKind kind);
bool parse_rpc_event_kind(std::string_view text, RpcEventKind& kind) noexcept;
RpcEventKind rpc_event_kind(const RpcEvent& event) noexcept;

qt::foundation::Result<RpcCommand> decode_rpc_command(const Json& json);
qt::foundation::Result<RpcResponse> decode_rpc_response(const Json& json);
qt::foundation::Result<RpcEvent> decode_rpc_event(const Json& json);
qt::foundation::Result<ExtensionUiResponseFrame>
decode_extension_ui_response_frame(const Json& json);
qt::foundation::Result<RpcDecodedFrame> decode_rpc_frame(const Json& json);
qt::foundation::Result<Json> decode_rpc_json_line(std::string_view line);

qt::foundation::Result<RpcCommand> decode_rpc_command_line(std::string_view line);
qt::foundation::Result<RpcResponse> decode_rpc_response_line(std::string_view line);
qt::foundation::Result<RpcEvent> decode_rpc_event_line(std::string_view line);
qt::foundation::Result<RpcDecodedFrame> decode_rpc_frame_line(
    std::string_view line);

qt::foundation::Result<std::string> encode_rpc_command_line(
    const RpcCommand& command);
qt::foundation::Result<std::string> encode_rpc_response_line(
    const RpcResponse& response);
qt::foundation::Result<std::string> encode_rpc_event_line(const RpcEvent& event);
qt::foundation::Result<std::string> encode_rpc_frame_line(
    const RpcDecodedFrame& frame);

void to_json(Json& json, const PromptCommand& command);
void from_json(const Json& json, PromptCommand& command);

void to_json(Json& json, const AbortCommand& command);
void from_json(const Json& json, AbortCommand& command);

void to_json(Json& json, const GetStateCommand& command);
void from_json(const Json& json, GetStateCommand& command);

void to_json(Json& json, const GetMessagesCommand& command);
void from_json(const Json& json, GetMessagesCommand& command);

void to_json(Json& json, const NewSessionCommand& command);
void from_json(const Json& json, NewSessionCommand& command);

void to_json(Json& json, const SwitchSessionCommand& command);
void from_json(const Json& json, SwitchSessionCommand& command);

void to_json(Json& json, const ForkCommand& command);
void from_json(const Json& json, ForkCommand& command);

void to_json(Json& json, const CloneCommand& command);
void from_json(const Json& json, CloneCommand& command);

void to_json(Json& json, const SetModelCommand& command);
void from_json(const Json& json, SetModelCommand& command);

void to_json(Json& json, const CycleModelCommand& command);
void from_json(const Json& json, CycleModelCommand& command);

void to_json(Json& json, const GetAvailableModelsCommand& command);
void from_json(const Json& json, GetAvailableModelsCommand& command);

void to_json(Json& json, const SetThinkingLevelCommand& command);
void from_json(const Json& json, SetThinkingLevelCommand& command);

void to_json(Json& json, const CycleThinkingLevelCommand& command);
void from_json(const Json& json, CycleThinkingLevelCommand& command);

void to_json(Json& json, const SetQueueModeCommand& command);
void from_json(const Json& json, SetQueueModeCommand& command);

void to_json(Json& json, const CompactCommand& command);
void from_json(const Json& json, CompactCommand& command);

void to_json(Json& json, const SetBooleanModeCommand& command);
void from_json(const Json& json, SetBooleanModeCommand& command);

void to_json(Json& json, const AbortRetryCommand& command);
void from_json(const Json& json, AbortRetryCommand& command);

void to_json(Json& json, const BashCommand& command);
void from_json(const Json& json, BashCommand& command);

void to_json(Json& json, const AbortBashCommand& command);
void from_json(const Json& json, AbortBashCommand& command);

void to_json(Json& json, const GetSessionStatsCommand& command);
void from_json(const Json& json, GetSessionStatsCommand& command);

void to_json(Json& json, const GetForkMessagesCommand& command);
void from_json(const Json& json, GetForkMessagesCommand& command);

void to_json(Json& json, const ExportHtmlCommand& command);
void from_json(const Json& json, ExportHtmlCommand& command);

void to_json(Json& json, const GetLastAssistantTextCommand& command);
void from_json(const Json& json, GetLastAssistantTextCommand& command);

void to_json(Json& json, const SetSessionNameCommand& command);
void from_json(const Json& json, SetSessionNameCommand& command);

void to_json(Json& json, const GetCommandsCommand& command);
void from_json(const Json& json, GetCommandsCommand& command);

void to_json(Json& json, const MemoryCommand& command);
void from_json(const Json& json, MemoryCommand& command);
void to_json(Json& json, const SubagentCommand& command);
void from_json(const Json& json, SubagentCommand& command);

void to_json(Json& json, const RpcSessionState& state);
void from_json(const Json& json, RpcSessionState& state);

void to_json(Json& json, const RpcSuccessResponse& response);
void from_json(const Json& json, RpcSuccessResponse& response);

void to_json(Json& json, const RpcErrorResponse& response);
void from_json(const Json& json, RpcErrorResponse& response);

void to_json(Json& json, const GetStateResponse& response);
void from_json(const Json& json, GetStateResponse& response);

void to_json(Json& json, const SetModelResponse& response);
void from_json(const Json& json, SetModelResponse& response);

void to_json(Json& json, const AgentStartEvent& event);
void from_json(const Json& json, AgentStartEvent& event);

void to_json(Json& json, const AgentEndEvent& event);
void from_json(const Json& json, AgentEndEvent& event);

void to_json(Json& json, const TurnStartEvent& event);
void from_json(const Json& json, TurnStartEvent& event);

void to_json(Json& json, const TurnEndEvent& event);
void from_json(const Json& json, TurnEndEvent& event);

void to_json(Json& json, const MessageStartEvent& event);
void from_json(const Json& json, MessageStartEvent& event);

void to_json(Json& json, const MessageUpdateEvent& event);
void from_json(const Json& json, MessageUpdateEvent& event);

void to_json(Json& json, const ToolExecutionStartEvent& event);
void from_json(const Json& json, ToolExecutionStartEvent& event);

void to_json(Json& json, const ToolExecutionUpdateEvent& event);
void from_json(const Json& json, ToolExecutionUpdateEvent& event);

void to_json(Json& json, const ToolExecutionEndEvent& event);
void from_json(const Json& json, ToolExecutionEndEvent& event);

void to_json(Json& json, const QueueUpdateEvent& event);
void from_json(const Json& json, QueueUpdateEvent& event);

void to_json(Json& json, const CompactionStartEvent& event);
void from_json(const Json& json, CompactionStartEvent& event);

void to_json(Json& json, const CompactionEndEvent& event);
void from_json(const Json& json, CompactionEndEvent& event);

void to_json(Json& json, const AutoRetryStartEvent& event);
void from_json(const Json& json, AutoRetryStartEvent& event);

void to_json(Json& json, const AutoRetryEndEvent& event);
void from_json(const Json& json, AutoRetryEndEvent& event);

void to_json(Json& json, const ExtensionErrorEvent& event);
void from_json(const Json& json, ExtensionErrorEvent& event);

void to_json(Json& json, const ExtensionUiRequestEvent& event);
void from_json(const Json& json, ExtensionUiRequestEvent& event);

void to_json(Json& json, const ExtensionUiResponseFrame& frame);
void from_json(const Json& json, ExtensionUiResponseFrame& frame);

void to_json(Json& json, const MessageEndEvent& event);
void from_json(const Json& json, MessageEndEvent& event);

}  // namespace qt::app
