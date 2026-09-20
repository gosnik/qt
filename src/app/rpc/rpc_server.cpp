#include <qt/app/rpc/rpc_server.hpp>

#include <algorithm>
#include <string_view>
#include <utility>

namespace qt::app {
namespace {

using qt::foundation::Result;

std::optional<std::string> decode_request_id_hint(const Json& json) {
  if (!json.is_object() || !json.contains("id") || json.at("id").is_null()) {
    return std::nullopt;
  }
  if (!json.at("id").is_string()) {
    return std::nullopt;
  }
  return json.at("id").get<std::string>();
}

std::optional<std::string> decode_command_type_hint(const Json& json) {
  if (!json.is_object() || !json.contains("type") || !json.at("type").is_string()) {
    return std::nullopt;
  }

  const auto type = json.at("type").get<std::string>();
  RpcCommandKind kind = RpcCommandKind::kPrompt;
  if (!parse_rpc_command_kind(type, kind)) {
    return std::nullopt;
  }
  return type;
}

std::optional<std::string> command_id(const RpcCommand& command) {
  return std::visit(
      [](const auto& value) -> std::optional<std::string> { return value.id; },
      command);
}

bool is_queueing_prompt_command(const PromptCommand& command) noexcept {
  return command.command == RpcCommandKind::kSteer ||
         command.command == RpcCommandKind::kFollowUp ||
         command.streaming_behavior.has_value();
}

bool is_queue_refresh_required(const RpcCommand& command) noexcept {
  if (const auto* prompt = std::get_if<PromptCommand>(&command)) {
    return is_queueing_prompt_command(*prompt);
  }
  return std::holds_alternative<AbortCommand>(command);
}

std::vector<RpcEvent> with_leading_queue_refresh(
    const std::vector<RpcEvent>& events) {
  std::vector<RpcEvent> normalized;
  normalized.reserve(events.size() + 1U);

  const auto first_queue_update =
      std::find_if(events.begin(), events.end(), [](const RpcEvent& event) {
        return std::holds_alternative<QueueUpdateEvent>(event);
      });
  if (first_queue_update == events.end()) {
    normalized.push_back(RpcEvent(QueueUpdateEvent{}));
  } else {
    normalized.push_back(*first_queue_update);
  }
  const RpcEvent* promoted_queue_update =
      first_queue_update == events.end() ? nullptr : &*first_queue_update;
  for (const auto& event : events) {
    if (&event != promoted_queue_update) {
      normalized.push_back(event);
    }
  }

  return normalized;
}

std::vector<RpcEvent> with_abort_queue_clear(
    const std::vector<RpcEvent>& events) {
  std::vector<RpcEvent> normalized;
  normalized.reserve(events.size() + 1U);
  normalized.push_back(RpcEvent(QueueUpdateEvent{}));

  for (const auto& event : events) {
    if (!std::holds_alternative<QueueUpdateEvent>(event)) {
      normalized.push_back(event);
    }
  }

  return normalized;
}

Result<std::vector<std::string>> encode_failure_response(
    std::optional<std::string> id, std::string command, std::string error,
    std::string error_code = "internal_error") {
  constexpr std::string_view marker = "rpc_error:";
  if (error.rfind(marker, 0U) == 0U) {
    const auto separator = error.find(':', marker.size());
    if (separator != std::string::npos && separator > marker.size()) {
      error_code = error.substr(marker.size(), separator - marker.size());
      error.erase(0U, separator + 1U);
    }
  }
  const auto encoded = encode_rpc_response_line(
      RpcResponse(RpcErrorResponse{std::move(id), std::move(command),
                                   std::move(error), std::move(error_code)}));
  if (!encoded) {
    return encoded.error();
  }
  return std::vector<std::string>{encoded.value()};
}

}  // namespace

RpcServer::RpcServer(RpcServerHandlers handlers)
    : handlers_(std::move(handlers)) {}

Result<std::vector<std::string>> RpcServer::push_input(std::string_view input) {
  pending_input_.append(input.data(), input.size());

  std::vector<std::string> output;
  std::size_t line_start = 0;
  while (true) {
    const std::size_t line_end = pending_input_.find('\n', line_start);
    if (line_end == std::string::npos) {
      break;
    }

    const std::string line =
        pending_input_.substr(line_start, line_end - line_start + 1);
    auto processed = process_line(line);
    if (!processed) {
      return processed.error();
    }

    for (auto& encoded : processed.value()) {
      output.push_back(std::move(encoded));
    }
    line_start = line_end + 1;
  }

  pending_input_.erase(0, line_start);
  return output;
}

Result<std::vector<std::string>> RpcServer::finish() {
  if (pending_input_.empty()) {
    return std::vector<std::string>{};
  }

  auto processed = process_line(pending_input_);
  if (!processed) {
    return processed.error();
  }

  pending_input_.clear();
  return processed.value();
}

Result<std::vector<std::string>> RpcServer::dispatch_command(
    const RpcCommand& command) {
  std::vector<std::string> output;

  const auto append_success = [&](const RpcResponse& response,
                                  std::vector<RpcEvent> events)
      -> Result<std::vector<std::string>> {
    const auto encoded_response = encode_response(response);
    if (!encoded_response) {
      return encoded_response.error();
    }
    output.push_back(encoded_response.value());

    for (const auto& event : events) {
      const auto encoded_event = encode_event(event);
      if (!encoded_event) {
        return encoded_event.error();
      }
      output.push_back(encoded_event.value());
    }

    return output;
  };

  if (const auto* prompt = std::get_if<PromptCommand>(&command)) {
    const std::string command_name = to_string(prompt->command);
    if (!handlers_.prompt) {
      return encode_failure_response(prompt->id, command_name,
                                     "rpc prompt handler is not configured");
    }

    const auto handled = handlers_.prompt(*prompt);
    if (!handled) {
      return encode_failure_response(prompt->id, command_name,
                                     handled.error().message);
    }

    return append_success(RpcResponse(RpcSuccessResponse{
                              prompt->id, command_name, std::nullopt}),
                          is_queue_refresh_required(command)
                              ? with_leading_queue_refresh(handled.value().events)
                              : handled.value().events);
  }

  if (const auto* abort = std::get_if<AbortCommand>(&command)) {
    if (!handlers_.abort) {
      return encode_failure_response(abort->id, "abort",
                                     "rpc abort handler is not configured");
    }

    const auto handled = handlers_.abort(*abort);
    if (!handled) {
      return encode_failure_response(abort->id, "abort",
                                     handled.error().message);
    }

    return append_success(
        RpcResponse(RpcSuccessResponse{abort->id, "abort", std::nullopt}),
        with_abort_queue_clear(handled.value().events));
  }

  if (const auto* get_state = std::get_if<GetStateCommand>(&command)) {
    if (!handlers_.get_state) {
      return encode_failure_response(
          get_state->id, "get_state",
          "rpc get_state handler is not configured");
    }

    const auto handled = handlers_.get_state(*get_state);
    if (!handled) {
      return encode_failure_response(get_state->id, "get_state",
                                     handled.error().message);
    }

    return append_success(
        RpcResponse(GetStateResponse{get_state->id, handled.value()}), {});
  }

  if (const auto* get_messages = std::get_if<GetMessagesCommand>(&command)) {
    if (!handlers_.get_messages) {
      return encode_failure_response(
          get_messages->id, "get_messages",
          "rpc get_messages handler is not configured");
    }

    const auto handled = handlers_.get_messages(*get_messages);
    if (!handled) {
      return encode_failure_response(get_messages->id, "get_messages",
                                     handled.error().message);
    }

    return append_success(
        RpcResponse(RpcSuccessResponse{get_messages->id, "get_messages",
                                       handled.value()}),
        {});
  }

  if (const auto* new_session = std::get_if<NewSessionCommand>(&command)) {
    if (!handlers_.new_session) {
      return encode_failure_response(
          new_session->id, "new_session",
          "rpc new_session handler is not configured");
    }

    const auto handled = handlers_.new_session(*new_session);
    if (!handled) {
      return encode_failure_response(new_session->id, "new_session",
                                     handled.error().message);
    }

    return append_success(
        RpcResponse(RpcSuccessResponse{new_session->id, "new_session",
                                       handled.value()}),
        {});
  }

  if (const auto* switch_session = std::get_if<SwitchSessionCommand>(&command)) {
    if (!handlers_.switch_session) {
      return encode_failure_response(
          switch_session->id, "switch_session",
          "rpc switch_session handler is not configured");
    }

    const auto handled = handlers_.switch_session(*switch_session);
    if (!handled) {
      return encode_failure_response(switch_session->id, "switch_session",
                                     handled.error().message);
    }

    return append_success(
        RpcResponse(RpcSuccessResponse{switch_session->id, "switch_session",
                                       handled.value()}),
        {});
  }

  if (const auto* fork = std::get_if<ForkCommand>(&command)) {
    if (!handlers_.fork) {
      return encode_failure_response(fork->id, "fork",
                                     "rpc fork handler is not configured");
    }

    const auto handled = handlers_.fork(*fork);
    if (!handled) {
      return encode_failure_response(fork->id, "fork",
                                     handled.error().message);
    }

    return append_success(
        RpcResponse(RpcSuccessResponse{fork->id, "fork", handled.value()}), {});
  }

  if (const auto* clone = std::get_if<CloneCommand>(&command)) {
    if (!handlers_.clone) {
      return encode_failure_response(clone->id, "clone",
                                     "rpc clone handler is not configured");
    }

    const auto handled = handlers_.clone(*clone);
    if (!handled) {
      return encode_failure_response(clone->id, "clone",
                                     handled.error().message);
    }

    return append_success(
        RpcResponse(RpcSuccessResponse{clone->id, "clone", handled.value()}), {});
  }

  if (const auto* queue_mode = std::get_if<SetQueueModeCommand>(&command)) {
    const std::string command_name = to_string(queue_mode->command);
    const auto& handler =
        queue_mode->command == RpcCommandKind::kSetSteeringMode
            ? handlers_.set_steering_mode
            : handlers_.set_follow_up_mode;
    if (!handler) {
      return encode_failure_response(
          queue_mode->id, command_name,
          "rpc " + command_name + " handler is not configured");
    }

    const auto handled = handler(*queue_mode);
    if (!handled) {
      return encode_failure_response(queue_mode->id, command_name,
                                     handled.error().message);
    }

    return append_success(RpcResponse(RpcSuccessResponse{
                              queue_mode->id, command_name, std::nullopt}),
                          {});
  }

  if (const auto* cycle_model = std::get_if<CycleModelCommand>(&command)) {
    if (!handlers_.cycle_model) {
      return encode_failure_response(
          cycle_model->id, "cycle_model",
          "rpc cycle_model handler is not configured");
    }

    const auto handled = handlers_.cycle_model(*cycle_model);
    if (!handled) {
      return encode_failure_response(cycle_model->id, "cycle_model",
                                     handled.error().message);
    }

    return append_success(
        RpcResponse(RpcSuccessResponse{cycle_model->id, "cycle_model",
                                       handled.value()}),
        {});
  }

  if (const auto* get_available_models =
          std::get_if<GetAvailableModelsCommand>(&command)) {
    if (!handlers_.get_available_models) {
      return encode_failure_response(
          get_available_models->id, "get_available_models",
          "rpc get_available_models handler is not configured");
    }

    const auto handled = handlers_.get_available_models(*get_available_models);
    if (!handled) {
      return encode_failure_response(get_available_models->id,
                                     "get_available_models",
                                     handled.error().message);
    }

    return append_success(
        RpcResponse(RpcSuccessResponse{get_available_models->id,
                                       "get_available_models",
                                       handled.value()}),
        {});
  }

  if (const auto* set_thinking =
          std::get_if<SetThinkingLevelCommand>(&command)) {
    if (!handlers_.set_thinking_level) {
      return encode_failure_response(
          set_thinking->id, "set_thinking_level",
          "rpc set_thinking_level handler is not configured");
    }

    const auto handled = handlers_.set_thinking_level(*set_thinking);
    if (!handled) {
      return encode_failure_response(set_thinking->id, "set_thinking_level",
                                     handled.error().message);
    }

    return append_success(RpcResponse(RpcSuccessResponse{
                              set_thinking->id, "set_thinking_level",
                              std::nullopt}),
                          {});
  }

  if (const auto* cycle_thinking =
          std::get_if<CycleThinkingLevelCommand>(&command)) {
    if (!handlers_.cycle_thinking_level) {
      return encode_failure_response(
          cycle_thinking->id, "cycle_thinking_level",
          "rpc cycle_thinking_level handler is not configured");
    }

    const auto handled = handlers_.cycle_thinking_level(*cycle_thinking);
    if (!handled) {
      return encode_failure_response(cycle_thinking->id,
                                     "cycle_thinking_level",
                                     handled.error().message);
    }

    return append_success(
        RpcResponse(RpcSuccessResponse{cycle_thinking->id,
                                       "cycle_thinking_level",
                                       handled.value()}),
        {});
  }

  if (const auto* compact = std::get_if<CompactCommand>(&command)) {
    if (!handlers_.compact) {
      return encode_failure_response(compact->id, "compact",
                                     "rpc compact handler is not configured");
    }

    const auto handled = handlers_.compact(*compact);
    if (!handled) {
      return encode_failure_response(compact->id, "compact",
                                     handled.error().message);
    }

    return append_success(
        RpcResponse(RpcSuccessResponse{compact->id, "compact", handled.value()}),
        {});
  }

  if (const auto* boolean_mode = std::get_if<SetBooleanModeCommand>(&command)) {
    const std::string command_name = to_string(boolean_mode->command);
    const auto& handler =
        boolean_mode->command == RpcCommandKind::kSetAutoCompaction
            ? handlers_.set_auto_compaction
            : handlers_.set_auto_retry;
    if (!handler) {
      return encode_failure_response(
          boolean_mode->id, command_name,
          "rpc " + command_name + " handler is not configured");
    }

    const auto handled = handler(*boolean_mode);
    if (!handled) {
      return encode_failure_response(boolean_mode->id, command_name,
                                     handled.error().message);
    }

    return append_success(RpcResponse(RpcSuccessResponse{
                              boolean_mode->id, command_name, std::nullopt}),
                          {});
  }

  if (const auto* abort_retry = std::get_if<AbortRetryCommand>(&command)) {
    if (!handlers_.abort_retry) {
      return encode_failure_response(
          abort_retry->id, "abort_retry",
          "rpc abort_retry handler is not configured");
    }

    const auto handled = handlers_.abort_retry(*abort_retry);
    if (!handled) {
      return encode_failure_response(abort_retry->id, "abort_retry",
                                     handled.error().message);
    }

    return append_success(RpcResponse(RpcSuccessResponse{
                              abort_retry->id, "abort_retry", std::nullopt}),
                          {});
  }

  if (const auto* bash = std::get_if<BashCommand>(&command)) {
    if (!handlers_.bash) {
      return encode_failure_response(bash->id, "bash",
                                     "rpc bash handler is not configured");
    }

    const auto handled = handlers_.bash(*bash);
    if (!handled) {
      return encode_failure_response(bash->id, "bash",
                                     handled.error().message);
    }

    return append_success(
        RpcResponse(RpcSuccessResponse{bash->id, "bash", handled.value()}), {});
  }

  if (const auto* abort_bash = std::get_if<AbortBashCommand>(&command)) {
    if (!handlers_.abort_bash) {
      return encode_failure_response(
          abort_bash->id, "abort_bash",
          "rpc abort_bash handler is not configured");
    }

    const auto handled = handlers_.abort_bash(*abort_bash);
    if (!handled) {
      return encode_failure_response(abort_bash->id, "abort_bash",
                                     handled.error().message);
    }

    return append_success(RpcResponse(RpcSuccessResponse{
                              abort_bash->id, "abort_bash", std::nullopt}),
                          {});
  }

  if (const auto* stats = std::get_if<GetSessionStatsCommand>(&command)) {
    if (!handlers_.get_session_stats) {
      return encode_failure_response(
          stats->id, "get_session_stats",
          "rpc get_session_stats handler is not configured");
    }

    const auto handled = handlers_.get_session_stats(*stats);
    if (!handled) {
      return encode_failure_response(stats->id, "get_session_stats",
                                     handled.error().message);
    }

    return append_success(
        RpcResponse(RpcSuccessResponse{stats->id, "get_session_stats",
                                       handled.value()}),
        {});
  }

  if (const auto* fork_messages =
          std::get_if<GetForkMessagesCommand>(&command)) {
    if (!handlers_.get_fork_messages) {
      return encode_failure_response(
          fork_messages->id, "get_fork_messages",
          "rpc get_fork_messages handler is not configured");
    }

    const auto handled = handlers_.get_fork_messages(*fork_messages);
    if (!handled) {
      return encode_failure_response(fork_messages->id, "get_fork_messages",
                                     handled.error().message);
    }

    return append_success(
        RpcResponse(RpcSuccessResponse{fork_messages->id,
                                       "get_fork_messages",
                                       handled.value()}),
        {});
  }

  if (const auto* export_html = std::get_if<ExportHtmlCommand>(&command)) {
    if (!handlers_.export_html) {
      return encode_failure_response(
          export_html->id, "export_html",
          "rpc export_html handler is not configured");
    }

    const auto handled = handlers_.export_html(*export_html);
    if (!handled) {
      return encode_failure_response(export_html->id, "export_html",
                                     handled.error().message);
    }

    return append_success(
        RpcResponse(RpcSuccessResponse{export_html->id, "export_html",
                                       handled.value()}),
        {});
  }

  if (const auto* last_text =
          std::get_if<GetLastAssistantTextCommand>(&command)) {
    if (!handlers_.get_last_assistant_text) {
      return encode_failure_response(
          last_text->id, "get_last_assistant_text",
          "rpc get_last_assistant_text handler is not configured");
    }

    const auto handled = handlers_.get_last_assistant_text(*last_text);
    if (!handled) {
      return encode_failure_response(last_text->id, "get_last_assistant_text",
                                     handled.error().message);
    }

    return append_success(
        RpcResponse(RpcSuccessResponse{last_text->id,
                                       "get_last_assistant_text",
                                       handled.value()}),
        {});
  }

  if (const auto* session_name = std::get_if<SetSessionNameCommand>(&command)) {
    if (!handlers_.set_session_name) {
      return encode_failure_response(
          session_name->id, "set_session_name",
          "rpc set_session_name handler is not configured");
    }

    const auto handled = handlers_.set_session_name(*session_name);
    if (!handled) {
      return encode_failure_response(session_name->id, "set_session_name",
                                     handled.error().message);
    }

    return append_success(RpcResponse(RpcSuccessResponse{
                              session_name->id, "set_session_name",
                              std::nullopt}),
                          {});
  }

  if (const auto* commands = std::get_if<GetCommandsCommand>(&command)) {
    if (!handlers_.get_commands) {
      return encode_failure_response(
          commands->id, "get_commands",
          "rpc get_commands handler is not configured");
    }

    const auto handled = handlers_.get_commands(*commands);
    if (!handled) {
      return encode_failure_response(commands->id, "get_commands",
                                     handled.error().message);
    }

    return append_success(
        RpcResponse(RpcSuccessResponse{commands->id, "get_commands",
                                       handled.value()}),
        {});
  }

  if (const auto* memory = std::get_if<MemoryCommand>(&command)) {
    if (!handlers_.memory) {
      return encode_failure_response(memory->id, "memory",
                                     "rpc memory handler is not configured");
    }
    const auto handled = handlers_.memory(*memory);
    if (!handled) {
      return encode_failure_response(memory->id, "memory",
                                     handled.error().message,
                                     qt::foundation::to_string(
                                         handled.error().code));
    }
    return append_success(
        RpcResponse(RpcSuccessResponse{memory->id, "memory", handled.value()}),
        {});
  }

  if (const auto* subagent = std::get_if<SubagentCommand>(&command)) {
    if (!handlers_.subagent) {
      return encode_failure_response(subagent->id, "subagent",
                                     "rpc subagent handler is not configured");
    }
    const auto handled = handlers_.subagent(*subagent);
    if (!handled) {
      return encode_failure_response(subagent->id, "subagent",
                                     handled.error().message,
                                     qt::foundation::to_string(
                                         handled.error().code));
    }
    return append_success(
        RpcResponse(RpcSuccessResponse{subagent->id, "subagent", handled.value()}),
        {});
  }

  const auto& set_model = std::get<SetModelCommand>(command);
  if (!handlers_.set_model) {
    return encode_failure_response(set_model.id, "set_model",
                                   "rpc set_model handler is not configured");
  }

  const auto handled = handlers_.set_model(set_model);
  if (!handled) {
    return encode_failure_response(set_model.id, "set_model",
                                   handled.error().message);
  }

  return append_success(
      RpcResponse(SetModelResponse{set_model.id, handled.value()}), {});
}

const std::string& RpcServer::pending_input() const noexcept {
  return pending_input_;
}

std::optional<ExtensionUiResponseFrame> RpcServer::take_extension_ui_response(
    std::string_view id) {
  const auto found = extension_ui_responses_.find(std::string(id));
  if (found == extension_ui_responses_.end()) {
    return std::nullopt;
  }

  auto response = std::move(found->second);
  extension_ui_responses_.erase(found);
  return response;
}

Result<std::string> RpcServer::encode_response(const RpcResponse& response) const {
  return encode_rpc_response_line(response);
}

Result<std::string> RpcServer::encode_event(const RpcEvent& event) const {
  return encode_rpc_event_line(event);
}

Result<std::vector<std::string>> RpcServer::process_line(std::string_view line) {
  const auto parsed = decode_rpc_json_line(line);
  if (!parsed) {
    return encode_failure_response(std::nullopt, "parse",
                                   "Failed to parse command: " +
                                       parsed.error().message);
  }

  const auto id_hint = decode_request_id_hint(parsed.value());
  const auto decoded = decode_rpc_frame(parsed.value());
  if (!decoded) {
    return encode_failure_response(std::nullopt, "invalid", decoded.error().message);
  }

  if (const auto* response =
          std::get_if<ExtensionUiResponseFrame>(&decoded.value().payload)) {
    if (extension_ui_responses_.find(response->id) !=
        extension_ui_responses_.end()) {
      return encode_failure_response(
          std::nullopt,
          "invalid",
          "duplicate extension ui response id: " + response->id);
    }
    extension_ui_responses_.emplace(response->id, *response);
    return std::vector<std::string>{};
  }

  if (id_hint.has_value() &&
      completed_request_ids_.find(*id_hint) != completed_request_ids_.end()) {
    return encode_failure_response(std::nullopt, "invalid",
                                   "duplicate rpc request id: " + *id_hint);
  }

  const auto command_type = decode_command_type_hint(parsed.value());
  if (command_type.has_value()) {
    const auto decoded_command = decode_rpc_command(parsed.value());
    if (!decoded_command) {
      if (id_hint.has_value()) {
        completed_request_ids_.insert(*id_hint);
      }
      return encode_failure_response(id_hint, *command_type,
                                     decoded_command.error().message);
    }

    auto dispatched = dispatch_command(decoded_command.value());
    if (!dispatched) {
      return dispatched.error();
    }

    if (const auto id = command_id(decoded_command.value()); id.has_value()) {
      completed_request_ids_.insert(*id);
    }
    return dispatched.value();
  }

  if (const auto* command = std::get_if<RpcCommand>(&decoded.value().payload)) {
    return dispatch_command(*command);
  }

  std::string type = "response";
  if (const auto* event = std::get_if<RpcEvent>(&decoded.value().payload)) {
    type = to_string(rpc_event_kind(*event));
  }

  return encode_failure_response(
      std::nullopt, "invalid",
      "rpc server accepts command frames only, received " + type);
}

}  // namespace qt::app
