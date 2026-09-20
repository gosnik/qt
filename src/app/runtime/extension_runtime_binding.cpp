#include "qt/app/runtime/extension_runtime_binding.hpp"

#include "qt/app/runtime/extension_host_api.hpp"
#include "qt/app/runtime/extension_lifecycle_runner.hpp"

#include <iostream>
#include <mutex>
#include <optional>

namespace qt::app {
namespace {

struct LifecycleBindingState {
  qt::harness::AgentHarness::SubscriptionId subscription_id = 0;
  std::optional<std::string> system_prompt_override;
};

std::pair<std::string, ai::Json> prompt_parts(const ai::Message& message) {
  std::string text;
  ai::Json images = ai::Json::array();
  for (const auto& part : message.content) {
    if (const auto* value = std::get_if<ai::TextContent>(&part)) {
      text += value->text;
    } else if (const auto* value = std::get_if<ai::ImageContent>(&part)) {
      images.push_back(ai::Json{{"mimeType", value->media_type},
                                {"data", value->data}});
    }
  }
  return {std::move(text), std::move(images)};
}

std::optional<ai::Message> custom_message_from_result(const ai::Json& value) {
  if (!value.is_object() || !value.contains("customType") ||
      !value.at("customType").is_string() ||
      value.at("customType").get<std::string>().empty() ||
      !value.contains("content")) {
    return std::nullopt;
  }
  ai::Message message;
  message.role = ai::MessageRole::user;
  const auto& content = value.at("content");
  if (content.is_string()) {
    message.content = {ai::TextContent{content.get<std::string>(), std::nullopt}};
  } else if (content.is_array()) {
    for (const auto& block : content) {
      if (!block.is_object() || !block.contains("type") ||
          !block.at("type").is_string()) return std::nullopt;
      const auto type = block.at("type").get<std::string>();
      if (type == "text" && block.value("text", ai::Json()).is_string()) {
        message.content.push_back(ai::TextContent{
            block.at("text").get<std::string>(), std::nullopt});
      } else if (type == "image" && block.value("data", ai::Json()).is_string() &&
                 (block.value("mimeType", ai::Json()).is_string() ||
                  block.value("mime_type", ai::Json()).is_string())) {
        const auto& mime = block.contains("mimeType") ? block.at("mimeType")
                                                       : block.at("mime_type");
        message.content.push_back(ai::ImageContent{
            mime.get<std::string>(),
            block.at("data").get<std::string>()});
      } else {
        return std::nullopt;
      }
    }
  } else {
    return std::nullopt;
  }
  message.metadata = ai::Json{
      {"role", "custom"},
      {"customType", value.at("customType")},
      {"display", value.value("display", false)},
  };
  if (value.contains("details") && !value.at("details").is_null()) {
    message.metadata["details"] = value.at("details");
  }
  return message;
}

void apply_system_prompt_override(std::vector<ai::Message>& messages,
                                  const std::string& prompt) {
  for (auto it = messages.begin(); it != messages.end();) {
    const bool injected = it->metadata.is_object() &&
                          (it->metadata.value("qt_extension_system_prompt", false) ||
                           it->metadata.value("qt_runtime_system_prompt", false));
    if (injected) {
      it = messages.erase(it);
    } else {
      ++it;
    }
  }
  if (prompt.empty()) return;
  ai::Message system;
  system.role = ai::MessageRole::system;
  system.name = "qt_extension_system_prompt";
  system.content = {ai::TextContent{prompt, std::nullopt}};
  system.metadata = ai::Json{{"qt_extension_system_prompt", true}};
  messages.insert(messages.begin(), std::move(system));
}

}  // namespace

void bind_extension_host_call(
    const std::shared_ptr<ReferenceStartupRuntime>& runtime,
    ReferenceCliArgs parsed) {
  if (!runtime) return;
  auto& host_runtime = runtime->services.resource_bundle.extension_host;
  if (host_runtime.runtime_binding_state) return;
  auto lifecycle_state = std::make_shared<LifecycleBindingState>();
  host_runtime.runtime_binding_state = lifecycle_state;
  auto identity = std::make_shared<qt::extensions::ExtensionHostCall>();
  const std::weak_ptr<ReferenceStartupRuntime> weak_runtime = runtime;
  auto call_mutex = std::make_shared<std::recursive_mutex>();
  *identity = [weak_runtime, call_mutex,
               parsed = std::move(parsed)](
                  std::string_view method,
                  const qt::extensions::Json& arguments)
      -> foundation::Result<ai::Json> {
    const auto locked = weak_runtime.lock();
    if (!locked) {
      return foundation::Result<ai::Json>::failure(
          foundation::ErrorCode::invalid_argument,
          "extension host runtime is no longer available");
    }
    std::lock_guard<std::recursive_mutex> call_lock(*call_mutex);
    if (method == "ctx.ui.confirm") return ai::Json(false);
    if (method == "ctx.ui.select" || method == "ctx.ui.input" ||
        method == "ctx.ui.editor") {
      return ai::Json(nullptr);
    }
    if (method == "ctx.ui.notify") {
      if (arguments.is_object() && arguments.contains("message") &&
          arguments.at("message").is_string()) {
        std::cerr << arguments.at("message").get<std::string>() << '\n';
      }
      return ai::Json(nullptr);
    }
    if (method == "ctx.ui.setStatus" || method == "ctx.ui.setWorkingMessage" ||
        method == "ctx.ui.setWorkingVisible" || method == "ctx.ui.setWidget" ||
        method == "ctx.ui.setFooter" || method == "ctx.ui.setHeader" ||
        method == "ctx.ui.setTitle" || method == "ctx.ui.pasteToEditor" ||
        method == "ctx.ui.setEditorText" || method == "ctx.ui.setTheme" ||
        method == "ctx.ui.setToolsExpanded") return ai::Json(nullptr);
    if (method == "ctx.ui.getEditorText") return ai::Json("");
    if (method == "ctx.ui.getAllThemes") return ai::Json::array();
    if (method == "ctx.ui.getTheme") return ai::Json(nullptr);
    if (method == "ctx.ui.getToolsExpanded") return ai::Json(false);
    return dispatch_extension_host_call(*locked, parsed, method, arguments);
  };

  auto bind = [&](auto& records) {
    for (auto& record : records) {
      if (!record.host_call) record.host_call = std::make_shared<qt::extensions::ExtensionHostCall>();
      *record.host_call = *identity;
    }
  };
  bind(runtime->services.resource_bundle.extensions.loaded_extensions);
  bind(runtime->services.resource_bundle.extension_host.registry.loaded_extensions);

  runtime->host.harness().add_context_hook(
      [weak_runtime, lifecycle_state](const qt::harness::AgentHarness::ContextEvent& event)
          -> std::optional<std::vector<ai::Message>> {
        const auto locked = weak_runtime.lock();
        if (!locked) return std::nullopt;
        auto current = event.messages;
        for (const auto& extension : locked->services.resource_bundle
                                          .extension_host.registry.loaded_extensions) {
          if (!extension.script_runtime || !extension.host_call) continue;
          for (const auto& handler : extension.script_runtime->registration()
                                         .lifecycle_handlers) {
            if (handler.event != "context") continue;
            const auto result = extension.script_runtime->invoke(
                handler.handler,
                ai::Json{{"type", "context"}, {"messages", current}},
                *extension.host_call);
            if (!result) {
              std::cerr << "extension context hook failed: "
                        << result.error().message << '\n';
              continue;
            }
            if (!result.value().is_object() ||
                !result.value().contains("messages")) continue;
            try {
              current = result.value().at("messages")
                            .get<std::vector<ai::Message>>();
            } catch (const std::exception& error) {
              std::cerr << "extension context hook returned invalid messages: "
                        << error.what() << '\n';
            }
          }
        }
        if (lifecycle_state->system_prompt_override.has_value()) {
          apply_system_prompt_override(current,
                                       *lifecycle_state->system_prompt_override);
        }
        return current;
      });

  runtime->host.harness().add_before_agent_start_hook(
      [weak_runtime, lifecycle_state](const qt::harness::AgentHarness::BeforeAgentStartEvent& event)
          -> std::optional<qt::harness::AgentHarness::BeforeAgentStartResult> {
        const auto locked = weak_runtime.lock();
        if (!locked) return std::nullopt;
        lifecycle_state->system_prompt_override.reset();
        const auto [prompt_text, prompt_images] = prompt_parts(event.prompt);
        qt::harness::AgentHarness::BeforeAgentStartResult combined;
        for (const auto& extension : locked->services.resource_bundle
                                          .extension_host.registry.loaded_extensions) {
          if (!extension.script_runtime || !extension.host_call) continue;
          for (const auto& handler : extension.script_runtime->registration()
                                         .lifecycle_handlers) {
            if (handler.event != "before_agent_start") continue;
            const auto result = extension.script_runtime->invoke(
                handler.handler,
                ai::Json{{"type", "before_agent_start"},
                         {"prompt", prompt_text},
                         {"images", prompt_images},
                         {"systemPrompt", lifecycle_state->system_prompt_override.value_or(
                                              locked->services.resource_bundle.system_prompt.prompt)},
                         {"systemPromptOptions", ai::Json::object()}},
                *extension.host_call);
            if (!result) {
              std::cerr << "extension before_agent_start hook failed: "
                        << result.error().message << '\n';
              continue;
            }
            if (!result.value().is_object()) continue;
            const auto message = result.value().find("message");
            if (message != result.value().end() && !message->is_null()) {
              const auto decoded = custom_message_from_result(*message);
              if (decoded.has_value()) {
                combined.messages.push_back(*decoded);
              } else {
                std::cerr << "extension before_agent_start message invalid: "
                          << "expected customType and string or content blocks\n";
              }
            }
            if (result.value().contains("systemPrompt") &&
                result.value().at("systemPrompt").is_string()) {
              lifecycle_state->system_prompt_override =
                  result.value().at("systemPrompt").get<std::string>();
            }
          }
        }
        return combined.messages.empty()
                   ? std::optional<qt::harness::AgentHarness::BeforeAgentStartResult>{}
                   : std::optional<qt::harness::AgentHarness::BeforeAgentStartResult>{
                         std::move(combined)};
      });

  runtime->host.harness().add_before_provider_request_hook(
      [weak_runtime](const qt::harness::AgentHarness::BeforeProviderRequestEvent& event)
          -> std::optional<ai::ProviderOptions> {
        const auto locked = weak_runtime.lock();
        if (!locked) return std::nullopt;
        ai::ProviderOptions current = event.stream_options;
        for (const auto& extension : locked->services.resource_bundle
                                          .extension_host.registry.loaded_extensions) {
          if (!extension.script_runtime || !extension.host_call) continue;
          for (const auto& handler : extension.script_runtime->registration()
                                         .lifecycle_handlers) {
            if (handler.event != "before_provider_request") continue;
            const auto result = extension.script_runtime->invoke(
                handler.handler,
                ai::Json{{"type", "before_provider_request"},
                         {"payload", event.request}},
                *extension.host_call);
            if (!result) {
              std::cerr << "extension before_provider_request hook failed: "
                        << result.error().message << '\n';
              continue;
            }
            if (!result.value().is_object()) continue;
            const auto& patch = result.value();
            if (patch.contains("apiKey")) current.api_key =
                patch.at("apiKey").is_null() ? std::nullopt :
                std::optional<std::string>(patch.at("apiKey").get<std::string>());
            if (patch.contains("timeoutMs")) current.timeout_ms = patch.at("timeoutMs").get<std::int64_t>();
            if (patch.contains("maxRetries")) current.max_retries = patch.at("maxRetries").get<int>();
            if (patch.contains("maxRetryDelayMs")) current.max_retry_delay_ms = patch.at("maxRetryDelayMs").get<std::int64_t>();
            if (patch.contains("proxyUrl")) current.proxy_url = patch.at("proxyUrl").get<std::string>();
            if (patch.contains("headers")) current.headers = patch.at("headers").get<std::map<std::string, std::string>>();
            if (patch.contains("metadata")) current.metadata = patch.at("metadata");
          }
        }
        return current;
      });

  runtime->host.harness().add_before_provider_payload_hook(
      [weak_runtime](const qt::harness::AgentHarness::BeforeProviderPayloadEvent& event)
          -> std::optional<ai::Json> {
        const auto locked = weak_runtime.lock();
        if (!locked) return std::nullopt;
        ai::Json current = event.payload;
        for (const auto& extension : locked->services.resource_bundle
                                          .extension_host.registry.loaded_extensions) {
          if (!extension.script_runtime || !extension.host_call) continue;
          for (const auto& handler : extension.script_runtime->registration()
                                         .lifecycle_handlers) {
            if (handler.event != "before_provider_request") continue;
            const auto result = extension.script_runtime->invoke(
                handler.handler,
                ai::Json{{"type", "before_provider_request"}, {"payload", current}},
                *extension.host_call);
            if (!result) {
              std::cerr << "extension provider payload hook failed: "
                        << result.error().message << '\n';
              continue;
            }
            if (!result.value().is_null()) current = result.value();
          }
        }
        return current;
      });

  runtime->host.harness().add_before_tool_hook(
      [weak_runtime](const qt::agent::ToolHookContext& context,
                     const qt::tools::ToolRequest& request)
          -> std::optional<qt::tools::ToolResult> {
        const auto locked = weak_runtime.lock();
        if (!locked) return std::nullopt;
        for (const auto& extension : locked->services.resource_bundle
                                          .extension_host.registry.loaded_extensions) {
          if (!extension.script_runtime || !extension.host_call) continue;
          for (const auto& handler : extension.script_runtime->registration()
                                         .lifecycle_handlers) {
            if (handler.event != "tool_call") continue;
            const auto result = extension.script_runtime->invoke(
                handler.handler,
                ai::Json{{"type", "tool_call"},
                         {"toolName", request.tool_name},
                         {"args", request.arguments},
                         {"input", request.arguments},
                         {"toolCallId", request.call_id.value_or("")},
                         {"turnIndex", context.turn_index},
                         {"toolCallIndex", context.tool_call_index}},
                *extension.host_call);
            if (!result) {
              std::cerr << "extension tool_call hook failed: "
                        << result.error().message << '\n';
            } else if (result.value().is_object() &&
                       result.value().value("block", false)) {
              qt::tools::ToolResult blocked;
              blocked.status = qt::tools::ToolResultStatus::blocked_by_extension;
              blocked.call_id = request.call_id;
              blocked.output = result.value().value("reason", "blocked by extension");
              blocked.error_message = blocked.output.dump();
              return blocked;
            }
          }
        }
        return std::nullopt;
      });

  runtime->host.harness().add_tool_result_transform(
      [weak_runtime](const qt::agent::ToolHookContext& context,
                     const qt::tools::ToolRequest& request,
                     const qt::tools::ToolResult& tool_result)
          -> std::optional<qt::tools::ToolResult> {
        const auto locked = weak_runtime.lock();
        if (!locked) return std::nullopt;
        auto current = tool_result;
        for (const auto& extension : locked->services.resource_bundle
                                          .extension_host.registry.loaded_extensions) {
          if (!extension.script_runtime || !extension.host_call) continue;
          for (const auto& handler : extension.script_runtime->registration()
                                         .lifecycle_handlers) {
            if (handler.event != "tool_result") continue;
            const auto result = extension.script_runtime->invoke(
                handler.handler,
                ai::Json{{"type", "tool_result"},
                         {"toolName", request.tool_name},
                         {"toolCallId", request.call_id.value_or("")},
                         {"input", request.arguments},
                         {"result", current.output},
                         {"content", current.output.is_array() ? current.output
                            : ai::Json::array({ai::Json{{"type", "text"},
                                {"text", current.output.is_string()
                                    ? current.output.get<std::string>() : current.output.dump()}}})},
                         {"details", current.output},
                         {"isError", current.status !=
                                          qt::tools::ToolResultStatus::success},
                         {"turnIndex", context.turn_index},
                         {"toolCallIndex", context.tool_call_index}},
                *extension.host_call);
            if (!result) {
              std::cerr << "extension tool_result hook failed: "
                        << result.error().message << '\n';
            } else if (result.value().is_object()) {
              const auto& patch = result.value();
              if (patch.contains("content")) current.output = patch.at("content");
              if (patch.contains("isError") && patch.at("isError").is_boolean()) {
                current.status = patch.at("isError").get<bool>()
                    ? qt::tools::ToolResultStatus::execution_error
                    : qt::tools::ToolResultStatus::success;
              }
              current.error_message =
                  current.status == qt::tools::ToolResultStatus::success
                      ? std::nullopt
                      : std::optional<std::string>(current.output.dump());
            }
          }
        }
        return current;
      });

  lifecycle_state->subscription_id = runtime->host.harness().subscribe(
      [weak_runtime](
          const qt::harness::AgentHarness::SubscribedEvent& event) {
        const auto locked = weak_runtime.lock();
        if (!locked) return;
        if (std::holds_alternative<
                qt::harness::AgentHarness::BeforeAgentStartEvent>(event)) {
          return;
        }
        auto plans = qt::extensions::make_extension_harness_lifecycle_routing_plans(
            locked->services.resource_bundle.extension_host, event, {});
        if (!plans) {
          std::cerr << "extension lifecycle planning failed: "
                    << plans.error().message << '\n';
          return;
        }
        for (const auto& plan : plans.value()) {
          auto result = run_extension_harness_lifecycle_subprocess(plan);
          if (!result) {
            std::cerr << "extension lifecycle execution failed: "
                      << result.error().message << '\n';
          }
        }
      });
}

}  // namespace qt::app
