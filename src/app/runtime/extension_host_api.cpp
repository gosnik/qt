#include "qt/app/runtime/extension_host_api.hpp"

#include "qt/app/slash/slash_commands.hpp"
#include "qt/foundation/process.hpp"
#include <algorithm>
#include <exception>

namespace qt::app {
namespace {
using ai::Json;
using foundation::Result;
Result<Json> invalid(std::string message) {
  return Result<Json>::failure(foundation::ErrorCode::invalid_argument,
                               std::move(message));
}

template <typename T>
Result<T> invalid_as(std::string message) {
  return Result<T>::failure(foundation::ErrorCode::invalid_argument,
                            std::move(message));
}

Result<std::vector<ai::Content>> extension_content(const Json& value) {
  std::vector<ai::Content> content;
  if (value.is_string()) {
    content.emplace_back(ai::TextContent{value.get<std::string>(), std::nullopt});
    return content;
  }
  if (!value.is_array()) return invalid_as<std::vector<ai::Content>>("extension message content must be a string or array");
  for (const auto& block : value) {
    if (!block.is_object() || !block.value("type", Json()).is_string()) {
      return invalid_as<std::vector<ai::Content>>("extension message content block is invalid");
    }
    const auto type = block.at("type").get<std::string>();
    if (type == "text" && block.value("text", Json()).is_string()) {
      content.emplace_back(ai::TextContent{block.at("text").get<std::string>(), std::nullopt});
    } else if (type == "image" && block.value("data", Json()).is_string() &&
               (block.value("mimeType", Json()).is_string() ||
                block.value("mime_type", Json()).is_string())) {
      const auto& mime = block.contains("mimeType") ? block.at("mimeType")
                                                     : block.at("mime_type");
      content.emplace_back(ai::ImageContent{mime.get<std::string>(),
                                             block.at("data").get<std::string>()});
    } else {
      return invalid_as<std::vector<ai::Content>>("extension message content block is invalid");
    }
  }
  return content;
}

Result<ai::Message> extension_message(const Json& args, bool custom) {
  const auto content = extension_content(args.at("content"));
  if (!content) return content.error();
  ai::Message message;
  message.role = ai::MessageRole::user;
  message.content = content.value();
  if (custom) {
    const auto type = args.at("customType").get<std::string>();
    if (type.empty()) return invalid_as<ai::Message>("customType must not be empty");
    message.metadata = Json{{"role", "custom"}, {"customType", type},
                            {"display", args.value("display", true)}};
    if (args.contains("details") && !args.at("details").is_null()) {
      message.metadata["details"] = args.at("details");
    }
  }
  return message;
}

Result<Json> queue_extension_message(harness::AgentHarness& harness,
                                      ai::Message message,
                                      std::string_view delivery) {
  try {
    if (delivery == "steer") {
      harness.steer(std::move(message));
    } else if (delivery == "followUp") {
      harness.follow_up(std::move(message));
    } else if (delivery == "nextTurn") {
      harness.next_turn(std::move(message));
    } else {
      return invalid("invalid extension message delivery mode");
    }
  } catch (const std::exception& error) {
    return invalid(error.what());
  }
  return Json(nullptr);
}

Json public_model_metadata(const Json& metadata) {
  Json result = Json::object();
  if (!metadata.is_object()) return result;
  for (const char* key : {"contextWindow", "maxTokens"}) {
    if (metadata.contains(key) && metadata.at(key).is_number_integer()) {
      result[key] = metadata.at(key);
    }
  }
  if (metadata.contains("input") && metadata.at("input").is_array() &&
      std::all_of(metadata.at("input").begin(), metadata.at("input").end(),
                  [](const auto& value) { return value.is_string(); })) {
    result["input"] = metadata.at("input");
  }
  if (metadata.contains("cost") && metadata.at("cost").is_object()) {
    Json cost = Json::object();
    for (const char* key : {"input", "output", "cacheRead", "cacheWrite",
                            "input_usd_per_million",
                            "output_usd_per_million",
                            "cache_read_usd_per_million",
                            "cache_write_usd_per_million"}) {
      if (metadata.at("cost").contains(key) &&
          metadata.at("cost").at(key).is_number()) {
        cost[key] = metadata.at("cost").at(key);
      }
    }
    if (!cost.empty()) result["cost"] = std::move(cost);
  }
  if (metadata.contains("thinkingLevelMap") &&
      metadata.at("thinkingLevelMap").is_object()) {
    Json levels = Json::object();
    for (const char* key : {"off", "minimal", "low", "medium", "high",
                            "xhigh"}) {
      if (!metadata.at("thinkingLevelMap").contains(key)) continue;
      const auto& value = metadata.at("thinkingLevelMap").at(key);
      if (value.is_string() || value.is_null()) levels[key] = value;
    }
    if (!levels.empty()) result["thinkingLevelMap"] = std::move(levels);
  }
  return result;
}

Json extension_model_json(const ai::ModelCatalogModelRecord& model) {
  const Json capabilities = {
      {"streaming", model.capabilities.streaming},
      {"toolCalls", model.capabilities.tool_calls},
      {"vision", model.capabilities.vision},
      {"reasoning", model.capabilities.reasoning},
      {"sessionCaching", model.capabilities.session_caching},
  };
  const auto metadata = public_model_metadata(model.metadata);
  Json result = {
      {"provider", model.ref.provider},
      {"id", model.ref.name},
      {"name", model.display_name.empty() ? model.ref.name
                                           : model.display_name},
      {"api", model.ref.api},
      {"reasoning", model.capabilities.reasoning},
      {"capabilities", capabilities},
      {"metadata", metadata},
  };
  if (metadata.contains("contextWindow")) {
    result["contextWindow"] = metadata.at("contextWindow");
  }
  if (metadata.contains("maxTokens")) {
    result["maxTokens"] = metadata.at("maxTokens");
  }
  if (metadata.contains("input")) {
    result["input"] = metadata.at("input");
  }
  if (metadata.contains("cost")) {
    result["cost"] = metadata.at("cost");
  }
  if (metadata.contains("thinkingLevelMap")) {
    result["thinkingLevelMap"] = metadata.at("thinkingLevelMap");
  }
  return result;
}

Json extension_provider_json(const ai::ModelCatalogProviderRecord& provider) {
  return Json{
      {"id", provider.descriptor.id},
      {"name", provider.descriptor.display_name},
      {"api", provider.descriptor.api},
      {"capabilities",
       Json{{"streaming", provider.descriptor.capabilities.streaming},
            {"toolCalls", provider.descriptor.capabilities.tool_calls},
            {"vision", provider.descriptor.capabilities.vision},
            {"reasoning", provider.descriptor.capabilities.reasoning},
            {"sessionCaching",
             provider.descriptor.capabilities.session_caching}}},
      {"metadata", public_model_metadata(provider.metadata)},
  };
}

std::optional<std::int64_t> usage_integer(const Json& usage,
                                          std::initializer_list<const char*> keys) {
  for (const auto* key : keys) {
    if (!usage.contains(key) || !usage.at(key).is_number()) continue;
    return usage.at(key).get<std::int64_t>();
  }
  return std::nullopt;
}

std::optional<std::int64_t> usage_context_tokens(
    const Json& usage, std::string_view stop_reason) {
  if (stop_reason == "aborted" || stop_reason == "error" ||
      !usage.is_object()) {
    return std::nullopt;
  }
  if (const auto total = usage_integer(usage, {"totalTokens", "total_tokens"});
      total && *total > 0) {
    return total;
  }
  std::int64_t total = 0;
  bool known = false;
  for (const auto keys : {
           std::initializer_list<const char*>{"input", "input_tokens"},
           std::initializer_list<const char*>{"output", "output_tokens"},
           std::initializer_list<const char*>{"cacheRead", "cache_read_tokens",
                                              "cache_read"},
           std::initializer_list<const char*>{"cacheWrite",
                                              "cache_write_tokens",
                                              "cache_write"},
       }) {
    if (const auto value = usage_integer(usage, keys)) {
      total += *value;
      known = true;
    }
  }
  return known && total > 0 ? std::optional<std::int64_t>{total}
                            : std::nullopt;
}

std::optional<std::int64_t> assistant_context_tokens(
    const ai::Message& message) {
  if (message.role != ai::MessageRole::assistant ||
      !message.metadata.is_object() ||
      !message.metadata.contains("usage")) {
    return std::nullopt;
  }
  return usage_context_tokens(
      message.metadata.at("usage"),
      message.metadata.value("stopReason", std::string{}));
}

bool is_compaction_entry(const harness::SessionEntry& entry) {
  if (std::holds_alternative<harness::SessionCompactionEntry>(entry.data)) {
    return true;
  }
  const auto* custom = std::get_if<harness::SessionCustomEntry>(&entry.data);
  return custom != nullptr &&
      custom->entry_type == harness::kUpstreamPiMonoSessionEntryType &&
      custom->payload.is_object() &&
      custom->payload.value("type", std::string{}) == "compaction";
}

std::optional<std::int64_t> entry_context_tokens(
    const harness::SessionEntry& entry) {
  if (const auto* assistant =
          std::get_if<harness::SessionAssistantMessageEntry>(&entry.data)) {
    return usage_context_tokens(assistant->usage, assistant->stop_reason);
  }
  const auto* custom = std::get_if<harness::SessionCustomEntry>(&entry.data);
  if (custom == nullptr ||
      custom->entry_type != harness::kUpstreamPiMonoSessionEntryType ||
      !custom->payload.is_object() ||
      custom->payload.value("type", std::string{}) != "message" ||
      !custom->payload.contains("message") ||
      !custom->payload.at("message").is_object()) {
    return std::nullopt;
  }
  const auto& message = custom->payload.at("message");
  if (message.value("role", std::string{}) != "assistant" ||
      !message.contains("usage")) {
    return std::nullopt;
  }
  return usage_context_tokens(
      message.at("usage"),
      message.value("stopReason", std::string{}));
}

Json extension_context_usage(const harness::AgentHarness& harness,
                             std::int64_t context_window) {
  if (context_window <= 0) return Json(nullptr);

  const auto branch = harness.session_manager().get_branch();
  std::optional<std::size_t> compaction_index;
  for (std::size_t index = 0; index < branch.size(); ++index) {
    if (is_compaction_entry(branch[index])) compaction_index = index;
  }
  if (compaction_index) {
    const auto valid_after_compaction = std::any_of(
        branch.begin() + static_cast<std::ptrdiff_t>(*compaction_index + 1),
        branch.end(), [](const auto& entry) {
          return entry_context_tokens(entry).has_value();
        });
    if (!valid_after_compaction) {
      return Json{{"tokens", Json(nullptr)},
                  {"contextWindow", context_window},
                  {"percent", Json(nullptr)}};
    }
  }

  const auto context = harness.session_manager().build_session_context();
  std::int64_t tokens = 0;
  std::size_t trailing_start = 0;
  bool found_usage = false;
  for (std::size_t index = context.messages.size(); index > 0; --index) {
    if (const auto reported =
            assistant_context_tokens(context.messages[index - 1])) {
      tokens = *reported;
      trailing_start = index;
      found_usage = true;
      break;
    }
  }
  for (std::size_t index = found_usage ? trailing_start : 0;
       index < context.messages.size(); ++index) {
    tokens += qt::ai::estimate_token_count(context.messages[index]);
  }
  const auto percent = (static_cast<double>(tokens) /
                        static_cast<double>(context_window)) * 100.0;
  return Json{{"tokens", tokens},
              {"contextWindow", context_window},
              {"percent", percent}};
}
}  // namespace

Result<Json> dispatch_extension_host_call(
    ReferenceStartupRuntime& runtime, const ReferenceCliArgs& parsed,
    std::string_view method, const Json& args) {
  if (!args.is_object()) return invalid("extension host arguments must be an object");
  auto& harness = runtime.host.harness();
  try {
    if (method == "pi.getSessionName") {
      const auto name = harness.session_manager().get_session_name();
      return name ? Json(*name) : Json(nullptr);
    }
    if (method == "pi.getThinkingLevel") return Json(harness.thinking_level());
    if (method == "pi.getActiveTools") return Json(harness.active_tool_names());
    if (method == "pi.getAllTools") {
      Json tools = Json::array();
      for (const auto& tool : harness.tool_definitions()) {
        tools.push_back(Json{{"name", tool.name}, {"description", tool.description},
                             {"parameters", tool.input_schema}});
      }
      return tools;
    }
    if (method == "pi.getFlag") {
      const auto name = args.at("name").get<std::string>();
      const auto found = parsed.extension_flags.find(name);
      if (found != parsed.extension_flags.end()) return *found;
      for (const auto& flag : runtime.services.resource_bundle.extension_host.registry.flags) {
        if (flag.name == name) return flag.default_value;
      }
      return Json(nullptr);
    }
    if (method == "pi.getCommands") {
      const auto& resources = runtime.services.resource_bundle;
      const auto registry = build_slash_command_registry(
          resources.prompt_templates, resources.skills, resources.extensions);
      Json commands = Json::array();
      for (const auto& command : registry.commands) {
        commands.push_back(Json{{"name", command.name},
                                {"description", command.description},
                                {"source", to_string(command.source)}});
      }
      return commands;
    }
    if (method == "ctx.isIdle") {
      return Json(harness.phase() != harness::AgentHarnessPhase::running &&
                  harness.phase() != harness::AgentHarnessPhase::compaction &&
                  harness.phase() != harness::AgentHarnessPhase::branch_summary);
    }
    if (method == "ctx.hasPendingMessages") return Json(harness.agent().has_queued_messages());
    if (method == "ctx.getSystemPrompt") return Json(runtime.services.resource_bundle.system_prompt.prompt);
    if (method == "ctx.getContextUsage") {
      const auto window = runtime.services.model_selection.model.metadata.value("contextWindow", std::int64_t{0});
      return extension_context_usage(harness, window);
    }
    if (method == "ctx.getContext") {
      const auto context = harness.session_manager().build_session_context();
      const auto window = runtime.services.model_selection.model.metadata.value(
          "contextWindow", std::int64_t{0});
      return Json{
          {"systemPrompt", runtime.services.resource_bundle.system_prompt.prompt},
          {"messages", context.messages},
          {"thinkingLevel", harness.thinking_level()},
          {"model", extension_model_json(runtime.services.model_selection.model)},
          {"usage", extension_context_usage(harness, window)},
      };
    }
    if (method == "ctx.getModel") {
      if (args.contains("provider") || args.contains("id")) {
        if (!args.value("provider", Json()).is_string() ||
            !args.value("id", Json()).is_string()) {
          return invalid("ctx.getModel requires string provider and id fields");
        }
        const auto* model = ai::find_model_record(
            runtime.services.model_catalog,
            args.at("provider").get<std::string>(),
            args.at("id").get<std::string>());
        return model ? extension_model_json(*model) : Json(nullptr);
      }
      return extension_model_json(runtime.services.model_selection.model);
    }
    if (method == "ctx.getModelCatalog") {
      Json providers = Json::array();
      for (const auto& provider : runtime.services.model_catalog.providers) {
        providers.push_back(extension_provider_json(provider));
      }
      Json models = Json::array();
      for (const auto& model : runtime.services.model_catalog.models) {
        models.push_back(extension_model_json(model));
      }
      return Json{{"providers", std::move(providers)},
                  {"models", std::move(models)}};
    }
    if (method == "ctx.abort") { harness.request_abort(); return Json(nullptr); }
    if (method == "pi.sendMessage" || method == "pi.sendUserMessage") {
      const bool custom = method == "pi.sendMessage";
      auto message = extension_message(args, custom);
      if (!message) return message.error();
      if (harness.phase() == harness::AgentHarnessPhase::running) {
        const auto delivery = args.value("deliverAs", custom ? "steer" : "steer");
        return queue_extension_message(harness, std::move(message.value()), delivery);
      }
      if (harness.phase() == harness::AgentHarnessPhase::compaction ||
          harness.phase() == harness::AgentHarnessPhase::branch_summary) {
        return invalid("extension message delivery is unavailable during harness transition");
      }
      if (custom && !args.value("triggerTurn", false)) {
        runtime.host.sync_session_manager_from_harness();
        runtime.host.session_manager().append_custom_message(
            args.at("customType").get<std::string>(),
            args.at("content").is_string()
                ? qt::harness::SessionCustomMessageContent{
                      args.at("content").get<std::string>()}
                : qt::harness::SessionCustomMessageContent{
                      args.at("content").get<std::vector<qt::harness::SessionUserContentBlock>>()},
            args.value("display", true),
            args.contains("details") ? std::optional<Json>{args.at("details")}
                                      : std::nullopt);
        runtime.host.sync_harness_from_session_manager();
        return Json(nullptr);
      }
      const auto result = harness.submit_prompt(std::move(message.value()));
      if (!result.ok) return invalid(result.error.value_or("extension message turn failed"));
      return Json(nullptr);
    }
    if (method == "pi.exec") {
      foundation::ProcessRequest request;
      request.mode = foundation::ProcessLaunchMode::argv;
      request.argv.push_back(args.at("command").get<std::string>());
      const auto argv = args.value("args", std::vector<std::string>{});
      request.argv.insert(request.argv.end(), argv.begin(), argv.end());
      request.cwd = foundation::Path(args.value("cwd", runtime.services.settings.cwd));
      request.timeout_ms = args.value("timeout", std::int64_t{30'000});
      if (*request.timeout_ms <= 0) return invalid("exec timeout must be positive");
      const auto executed = foundation::run_process(request);
      if (!executed) return executed.error();
      return Json{{"stdout", executed.value().stdout_text},
                  {"stderr", executed.value().stderr_text},
                  {"code", executed.value().exit_code},
                  {"killed", executed.value().timed_out || executed.value().cancelled}};
    }
    if (method == "pi.setSessionName" || method == "pi.appendEntry") {
      if (method == "pi.setSessionName") {
        harness.set_session_name(args.at("name").get<std::string>());
      } else {
        harness.append_extension_state(args.at("customType").get<std::string>(),
                                       args.value("data", Json(nullptr)));
      }
      if (harness.phase() != qt::harness::AgentHarnessPhase::running) {
        runtime.host.sync_session_manager_from_harness();
      }
      return Json(nullptr);
    }

    // Mutations are dispatched by the owning mode while the harness is idle.
    // Worker-thread hooks must queue these onto that mode rather than mutate it.
    if (harness.phase() == harness::AgentHarnessPhase::running ||
        harness.phase() == harness::AgentHarnessPhase::compaction ||
        harness.phase() == harness::AgentHarnessPhase::branch_summary) {
      return invalid("extension runtime mutation requires an idle harness");
    }
    if (method == "ctx.compact") {
      qt::harness::ModelBackedCompactionRequest request;
      if (args.contains("customInstructions") &&
          !args.at("customInstructions").is_null()) {
        if (!args.at("customInstructions").is_string()) {
          return invalid("ctx.compact customInstructions must be a string or null");
        }
        request.custom_instructions =
            args.at("customInstructions").get<std::string>();
      }
      auto compacted = harness.compact_session(std::move(request));
      if (!compacted) return compacted.error();
      // Compaction updates the session tree, not the agent's live transcript.
      // Replace only the transcript so queued input is preserved.
      harness.agent().state().transcript =
          harness.session_manager().build_session_context().messages;
      runtime.host.sync_session_manager_from_harness();
      return Json{{"summary", compacted.value().summary},
                  {"firstKeptEntryId", compacted.value().first_kept_entry_id},
                  {"tokensBefore", compacted.value().tokens_before},
                  {"details", compacted.value().details}};
    } else if (method == "pi.setLabel") {
      runtime.host.sync_session_manager_from_harness();
      const auto label = args.contains("label") && !args.at("label").is_null()
          ? std::optional<std::string>{args.at("label").get<std::string>()} : std::nullopt;
      const auto changed = runtime.host.session_manager().append_label_change(
          args.at("entryId").get<std::string>(), label);
      if (!changed) return changed.error();
      runtime.host.sync_harness_from_session_manager();
    } else if (method == "pi.setActiveTools") {
      const auto names = args.at("toolNames").get<std::vector<std::string>>();
      for (const auto& name : names) {
        if (std::none_of(harness.tool_definitions().begin(), harness.tool_definitions().end(),
                        [&](const auto& tool) { return tool.name == name; })) {
          return invalid("unknown active tool: " + name);
        }
      }
      harness.set_active_tool_names(names);
    } else if (method == "pi.setThinkingLevel") {
      const auto level = args.at("level").get<std::string>();
      const std::vector<std::string> levels{"off", "minimal", "low", "medium", "high", "xhigh"};
      if (std::find(levels.begin(), levels.end(), level) == levels.end()) return invalid("invalid thinking level");
      harness.set_thinking_level(ai::clamp_thinking_level(runtime.services.model_selection.model, level));
    } else if (method == "pi.setModel") {
      const auto selected = make_runtime_provider_selection(runtime.services, parsed,
          args.at("provider").get<std::string>(), args.at("id").get<std::string>());
      if (!selected) return selected.error();
      if (selected.value().using_echo_provider) return Json(false);
      runtime.services.model_selection = selected.value().model_selection;
      runtime.using_echo_provider = selected.value().using_echo_provider;
      harness.set_model(selected.value().model_selection.model.ref,
                        selected.value().provider, selected.value().options);
      runtime.host.sync_session_manager_from_harness();
      return Json(true);
    } else {
      return invalid("unsupported extension host API: " + std::string(method));
    }
    runtime.host.sync_session_manager_from_harness();
    return Json(nullptr);
  } catch (const Json::exception&) {
    return invalid("invalid arguments for extension host API: " + std::string(method));
  } catch (const std::exception& error) {
    return invalid(std::string("extension host call failed: ") + error.what());
  }
}
}  // namespace qt::app
