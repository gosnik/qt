#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <qt/app/rpc/rpc_protocol.hpp>
#include <qt/foundation/error.hpp>

namespace qt::app {

struct RpcServerCommandResult {
  std::optional<Json> data;
  std::vector<RpcEvent> events;
};

struct RpcServerHandlers {
  std::function<qt::foundation::Result<RpcServerCommandResult>(
      const PromptCommand&)>
      prompt;
  std::function<qt::foundation::Result<RpcServerCommandResult>(
      const AbortCommand&)>
      abort;
  std::function<qt::foundation::Result<RpcSessionState>(const GetStateCommand&)>
      get_state;
  std::function<qt::foundation::Result<Json>(const GetMessagesCommand&)>
      get_messages;
  std::function<qt::foundation::Result<Json>(const NewSessionCommand&)>
      new_session;
  std::function<qt::foundation::Result<Json>(const SwitchSessionCommand&)>
      switch_session;
  std::function<qt::foundation::Result<Json>(const ForkCommand&)> fork;
  std::function<qt::foundation::Result<Json>(const CloneCommand&)> clone;
  std::function<qt::foundation::Result<Json>(const SetModelCommand&)> set_model;
  std::function<qt::foundation::Result<Json>(const CycleModelCommand&)>
      cycle_model;
  std::function<qt::foundation::Result<Json>(const GetAvailableModelsCommand&)>
      get_available_models;
  std::function<qt::foundation::Result<Json>(const SetThinkingLevelCommand&)>
      set_thinking_level;
  std::function<qt::foundation::Result<Json>(const CycleThinkingLevelCommand&)>
      cycle_thinking_level;
  std::function<qt::foundation::Result<Json>(const SetQueueModeCommand&)>
      set_steering_mode;
  std::function<qt::foundation::Result<Json>(const SetQueueModeCommand&)>
      set_follow_up_mode;
  std::function<qt::foundation::Result<Json>(const CompactCommand&)> compact;
  std::function<qt::foundation::Result<Json>(const SetBooleanModeCommand&)>
      set_auto_compaction;
  std::function<qt::foundation::Result<Json>(const SetBooleanModeCommand&)>
      set_auto_retry;
  std::function<qt::foundation::Result<Json>(const AbortRetryCommand&)>
      abort_retry;
  std::function<qt::foundation::Result<Json>(const BashCommand&)> bash;
  std::function<qt::foundation::Result<Json>(const AbortBashCommand&)>
      abort_bash;
  std::function<qt::foundation::Result<Json>(const GetSessionStatsCommand&)>
      get_session_stats;
  std::function<qt::foundation::Result<Json>(const GetForkMessagesCommand&)>
      get_fork_messages;
  std::function<qt::foundation::Result<Json>(const ExportHtmlCommand&)>
      export_html;
  std::function<qt::foundation::Result<Json>(
      const GetLastAssistantTextCommand&)>
      get_last_assistant_text;
  std::function<qt::foundation::Result<Json>(const SetSessionNameCommand&)>
      set_session_name;
  std::function<qt::foundation::Result<Json>(const GetCommandsCommand&)>
      get_commands;
  std::function<qt::foundation::Result<Json>(const MemoryCommand&)> memory;
  std::function<qt::foundation::Result<Json>(const SubagentCommand&)> subagent;
};

class RpcServer {
 public:
  explicit RpcServer(RpcServerHandlers handlers);

  qt::foundation::Result<std::vector<std::string>> push_input(
      std::string_view input);
  qt::foundation::Result<std::vector<std::string>> finish();

  qt::foundation::Result<std::vector<std::string>> dispatch_command(
      const RpcCommand& command);

  std::optional<ExtensionUiResponseFrame> take_extension_ui_response(
      std::string_view id);

  const std::string& pending_input() const noexcept;

 private:
  qt::foundation::Result<std::string> encode_response(
      const RpcResponse& response) const;
  qt::foundation::Result<std::string> encode_event(const RpcEvent& event) const;
  qt::foundation::Result<std::vector<std::string>> process_line(
      std::string_view line);

  RpcServerHandlers handlers_;
  std::string pending_input_;
  std::unordered_set<std::string> completed_request_ids_;
  std::unordered_map<std::string, ExtensionUiResponseFrame>
      extension_ui_responses_;
};

}  // namespace qt::app
