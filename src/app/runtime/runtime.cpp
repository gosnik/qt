#include "qt/app/runtime/runtime.hpp"
#include "qt/app/runtime/subagent_coordinator.hpp"

#include "qt/ai/llama_provider.hpp"
#include "qt/ai/model_catalog.hpp"
#include "qt/ai/provider_transport.hpp"
#include "qt/foundation/logging.hpp"
#include "qt/harness/agent_harness.hpp"
#include "qt/harness/repository.hpp"
#include "qt/harness/session_manager.hpp"
#include "qt/harness/session_repository.hpp"
#include "qt/memory/runtime.hpp"
#include "qt/tools/mcp_client.hpp"
#include "index/service.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace qt::app {
namespace {

using Json = nlohmann::json;

class EchoProvider final : public qt::ai::Provider {
 public:
  EchoProvider() {
    descriptor_.id = "native";
    descriptor_.display_name = "Native Harness";
    descriptor_.api = "deterministic";
    descriptor_.capabilities.streaming = false;
    descriptor_.capabilities.tool_calls = false;
  }

  const qt::ai::ProviderDescriptor& descriptor() const override {
    return descriptor_;
  }

  qt::ai::CompletionResponse complete(
      const qt::ai::CompletionRequest& request,
      const qt::ai::ProviderOptions& options) override {
    (void)options;

    std::string prompt;
    for (auto it = request.messages.rbegin(); it != request.messages.rend();
         ++it) {
      if (it->role != qt::ai::MessageRole::user) {
        continue;
      }
      for (const auto& content : it->content) {
        if (qt::ai::content_kind(content) == qt::ai::ContentKind::text) {
          prompt = std::get<qt::ai::TextContent>(content).text;
          break;
        }
      }
      if (!prompt.empty()) {
        break;
      }
    }

    qt::ai::CompletionResponse response;
    response.message.id = "native-assistant-1";
    response.message.role = qt::ai::MessageRole::assistant;
    response.message.content = {
        qt::ai::TextContent{
            prompt.empty() ? "Native qt runtime is ready." : prompt,
            std::nullopt,
        },
    };
    response.stop_reason = qt::ai::StopReason::end_turn;
    response.usage.input_tokens =
        static_cast<std::int64_t>(prompt.size() / 4 + 1);
    response.usage.output_tokens =
        static_cast<std::int64_t>(std::max<std::size_t>(1, prompt.size() / 4));
    qt::ai::normalize(response.usage);
    response.metadata = Json{{"native_startup", true}};
    return response;
  }

 private:
  qt::ai::ProviderDescriptor descriptor_;
};

std::string env_or_empty(const char* name) {
  if (const char* value = std::getenv(name)) {
    return std::string(value);
  }
  return {};
}

std::string default_cwd() {
  return std::filesystem::current_path().generic_string();
}

std::string agent_home() {
  const auto env = env_or_empty("QT_CODING_AGENT_DIR");
  if (!env.empty()) {
    if (env == "~") {
      const auto home = env_or_empty("HOME");
      return home.empty() ? env : home;
    }
    if (env.rfind("~/", 0) == 0) {
      const auto home = env_or_empty("HOME");
      return home.empty() ? env : home + env.substr(1);
    }
    return env;
  }
  const auto home = env_or_empty("HOME");
  return home.empty() ? ".qt" : home + "/.qt";
}

Json startup_session_json(const ReferenceStartupSession& session) {
  Json json{
      {"mode", session.mode},
      {"persistent", session.persistent},
      {"entries", session.entry_count},
  };
  if (session.repository_path) {
    json["repository_path"] = *session.repository_path;
  }
  if (session.parent_repository_path) {
    json["parent_repository_path"] = *session.parent_repository_path;
  }
  if (session.session_id) {
    json["session_id"] = *session.session_id;
  }
  if (session.parent_session_id) {
    json["parent_session_id"] = *session.parent_session_id;
  }
  return json;
}

std::optional<std::string> requested_model(
    const ReferenceCliArgs& parsed) {
  if (parsed.model) {
    return parsed.model;
  }
  if (!parsed.models.empty()) {
    return parsed.models.front();
  }
  return std::nullopt;
}

std::string resolve_cli_resource_path(const std::string& cwd,
                                                const std::string& path) {
  if (path.empty()) {
    return path;
  }
  const std::filesystem::path candidate(path);
  if (candidate.is_absolute()) {
    return candidate.lexically_normal().generic_string();
  }
  return (std::filesystem::path(cwd) / candidate)
      .lexically_normal()
      .generic_string();
}

std::vector<std::string> resolve_cli_resource_paths(
    const std::string& cwd, const std::vector<std::string>& paths) {
  std::vector<std::string> resolved;
  resolved.reserve(paths.size());
  for (const auto& path : paths) {
    resolved.push_back(resolve_cli_resource_path(cwd, path));
  }
  return resolved;
}

bool has_explicit_model_selection(const ReferenceCliArgs& parsed) {
  return parsed.provider.has_value() || parsed.model.has_value() ||
         !parsed.models.empty();
}

std::string append_endpoint_path(const std::string& base_url,
                                 std::string_view suffix) {
  if (base_url.empty()) {
    return std::string(suffix);
  }
  if (base_url.size() >= suffix.size() &&
      base_url.compare(base_url.size() - suffix.size(), suffix.size(),
                       suffix) == 0) {
    return base_url;
  }
  if (base_url.back() == '/') {
    return base_url.substr(0, base_url.size() - 1) + std::string(suffix);
  }
  return base_url + std::string(suffix);
}

std::optional<std::string> metadata_string(const Json& metadata,
                                           const char* key) {
  if (!metadata.is_object()) {
    return std::nullopt;
  }
  const auto it = metadata.find(key);
  if (it == metadata.end() || !it->is_string()) {
    return std::nullopt;
  }
  const std::string value = it->get<std::string>();
  return value.empty() ? std::nullopt : std::optional<std::string>(value);
}

qt::ai::MessageRole mcp_sampling_role(const Json& message) {
  const std::string role = message.value("role", "user");
  if (role == "system") {
    return qt::ai::MessageRole::system;
  }
  if (role == "developer") {
    return qt::ai::MessageRole::developer;
  }
  if (role == "assistant") {
    return qt::ai::MessageRole::assistant;
  }
  if (role == "tool") {
    return qt::ai::MessageRole::tool;
  }
  return qt::ai::MessageRole::user;
}

void append_mcp_sampling_content(qt::ai::Message& message, const Json& content) {
  if (content.is_string()) {
    qt::ai::TextContent text;
    text.text = content.get<std::string>();
    message.content.push_back(std::move(text));
    return;
  }
  if (!content.is_object()) {
    return;
  }
  const std::string type = content.value("type", "text");
  if (type == "text") {
    qt::ai::TextContent text;
    text.text = content.value("text", std::string());
    message.content.push_back(std::move(text));
  } else if (type == "image") {
    qt::ai::ImageContent image;
    image.media_type = content.value("mimeType",
                                     content.value("media_type", "image/png"));
    image.data = content.value("data", std::string());
    if (!image.data.empty()) {
      message.content.push_back(std::move(image));
    }
  } else if (type == "tool_use") {
    message.content.push_back(qt::ai::ToolCallContent{
        content.value("id", std::string("mcp_sampling_tool")),
        content.value("name", std::string()),
        content.value("input", Json::object())});
  } else if (type == "tool_result") {
    qt::ai::ToolResultContent result;
    result.tool_call_id = content.value("toolUseId", std::string());
    result.name = content.value("name", std::string());
    result.is_error = content.value("isError", false);
    result.result = content.value("content", Json::object());
    message.content.push_back(std::move(result));
  }
}

qt::ai::Message mcp_sampling_message_from_json(const Json& value) {
  qt::ai::Message message;
  message.role = mcp_sampling_role(value);
  const auto content = value.find("content");
  if (content == value.end()) {
    return message;
  }
  if (content->is_array()) {
    for (const auto& block : *content) {
      append_mcp_sampling_content(message, block);
    }
  } else {
    append_mcp_sampling_content(message, *content);
  }
  return message;
}

std::string first_mcp_sampling_text(const qt::ai::Message& message) {
  std::string text;
  for (const auto& content : message.content) {
    if (qt::ai::content_kind(content) != qt::ai::ContentKind::text) {
      continue;
    }
    const auto& block = std::get<qt::ai::TextContent>(content);
    if (!text.empty()) {
      text.push_back('\n');
    }
    text += block.text;
  }
  return text;
}

qt::foundation::Result<Json> run_mcp_sampling_request(
    const qt::tools::McpSamplingRequest& sampling_request,
    const std::shared_ptr<qt::ai::Provider>& provider,
    qt::ai::ProviderOptions options,
    const qt::ai::ModelRef& model) {
  if (!provider) {
    return qt::foundation::Result<Json>::failure(
        qt::foundation::ErrorCode::internal_error,
        "MCP sampling provider is unavailable");
  }

  const Json& config = sampling_request.config;
  const Json& params = sampling_request.params;
  const auto requested_model = [&]() -> std::string {
    if (config.contains("model") && config.at("model").is_string()) {
      return config.at("model").get<std::string>();
    }
    if (params.contains("modelPreferences") &&
        params.at("modelPreferences").is_object()) {
      const auto& preferences = params.at("modelPreferences");
      if (preferences.contains("hints") && preferences.at("hints").is_array()) {
        for (const auto& hint : preferences.at("hints")) {
          if (hint.is_object() && hint.contains("name") &&
              hint.at("name").is_string()) {
            return hint.at("name").get<std::string>();
          }
        }
      }
    }
    return {};
  }();

  if (config.contains("allowed_models") && config.at("allowed_models").is_array() &&
      !requested_model.empty()) {
    bool allowed = false;
    for (const auto& allowed_model : config.at("allowed_models")) {
      if (allowed_model.is_string() &&
          allowed_model.get<std::string>() == requested_model) {
        allowed = true;
      }
    }
    if (!allowed) {
      return qt::foundation::Result<Json>::failure(
          qt::foundation::ErrorCode::invalid_argument,
          "MCP sampling requested disallowed model: " + requested_model);
    }
  }

  qt::ai::CompletionRequest request;
  request.model = model;
  if (params.contains("systemPrompt") && params.at("systemPrompt").is_string()) {
    qt::ai::Message system;
    system.role = qt::ai::MessageRole::system;
    qt::ai::TextContent text;
    text.text = params.at("systemPrompt").get<std::string>();
    system.content.push_back(std::move(text));
    request.messages.push_back(std::move(system));
  }
  if (params.contains("messages") && params.at("messages").is_array()) {
    for (const auto& message : params.at("messages")) {
      if (message.is_object()) {
        request.messages.push_back(mcp_sampling_message_from_json(message));
      }
    }
  }
  const auto max_tokens_cap =
      config.value("max_tokens_cap", static_cast<std::int64_t>(4096));
  request.max_output_tokens =
      std::min(params.value("maxTokens", max_tokens_cap), max_tokens_cap);
  if (params.contains("temperature") && params.at("temperature").is_number()) {
    request.temperature = params.at("temperature").get<double>();
  }
  if (params.contains("tools") && params.at("tools").is_array()) {
    for (const auto& tool : params.at("tools")) {
      if (!tool.is_object() || !tool.contains("name") ||
          !tool.at("name").is_string()) {
        continue;
      }
      request.tools.push_back(qt::ai::ToolDefinition{
          tool.at("name").get<std::string>(),
          tool.value("description", std::string()),
          tool.value("inputSchema", Json{{"type", "object"},
                                         {"additionalProperties", true}})});
    }
    if (!request.tools.empty()) {
      request.tool_choice =
          qt::ai::ToolChoice{qt::ai::ToolChoiceKind::auto_select, std::nullopt};
    }
  }
  request.metadata = Json{{"mcpSampling", true},
                          {"mcpServer", sampling_request.server_name}};

  const auto timeout_seconds =
      config.value("timeout", static_cast<std::int64_t>(30));
  options.timeout_ms = timeout_seconds * 1000;
  const auto start = std::chrono::steady_clock::now();
  qt::ai::CompletionResponse response;
  try {
    response = provider->complete(request, options);
  } catch (const std::exception& error) {
    return qt::foundation::Result<Json>::failure(
        qt::foundation::ErrorCode::internal_error,
        qt::tools::sanitize_mcp_error(std::string("MCP sampling failed: ") +
                                      error.what()));
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  if (elapsed > std::chrono::seconds(timeout_seconds)) {
    return qt::foundation::Result<Json>::failure(
        qt::foundation::ErrorCode::internal_error,
        "MCP sampling exceeded configured timeout");
  }

  Json content = Json::array();
  for (const auto& block : response.message.content) {
    if (qt::ai::content_kind(block) == qt::ai::ContentKind::tool_call) {
      const auto& tool_call = std::get<qt::ai::ToolCallContent>(block);
      content.push_back(Json{{"type", "tool_use"},
                             {"id", tool_call.id},
                             {"name", tool_call.name},
                             {"input", tool_call.arguments}});
    }
  }
  std::string text = first_mcp_sampling_text(response.message);
  if (content.empty()) {
    content = Json{{"type", "text"},
                   {"text", qt::tools::sanitize_mcp_error(text)}};
  }

  std::string stop_reason = "endTurn";
  if (response.stop_reason == qt::ai::StopReason::max_tokens) {
    stop_reason = "maxTokens";
  } else if (response.stop_reason == qt::ai::StopReason::tool_call ||
             content.is_array()) {
    stop_reason = "toolUse";
  }
  return Json{{"role", "assistant"},
              {"content", std::move(content)},
              {"model", response.response_model.value_or(model.name)},
              {"stopReason", stop_reason}};
}

void install_mcp_sampling_callback(
    const RuntimeServices& services,
    const std::shared_ptr<qt::ai::Provider>& provider,
    const qt::ai::ProviderOptions& options,
    const qt::ai::ModelRef& model) {
  if (!services.mcp_client_manager) {
    return;
  }
  services.mcp_client_manager->set_sampling_callback(
      [provider, options, model](const qt::tools::McpSamplingRequest& request) {
        return run_mcp_sampling_request(request, provider, options, model);
      });
}

std::string clamp_thinking_level_for_model(
    const qt::ai::ModelCatalog& catalog, const qt::ai::ModelRef& model,
    const std::string& level) {
  const auto* record = qt::ai::find_model_record(catalog, model.provider,
                                                 model.name);
  return record == nullptr ? level : qt::ai::clamp_thinking_level(*record, level);
}

std::optional<std::int64_t> default_max_output_tokens_for_model(
    const qt::ai::ModelCatalog& catalog, const qt::ai::ModelRef& model) {
  constexpr std::int64_t kDefaultMaxOutputTokensCap = 32000;
  const auto* record =
      qt::ai::find_model_record(catalog, model.provider, model.name);
  if (record == nullptr || !record->metadata.is_object()) {
    return std::nullopt;
  }
  const auto max_tokens = record->metadata.find("maxTokens");
  if (max_tokens == record->metadata.end() ||
      !max_tokens->is_number_integer()) {
    return std::nullopt;
  }
  const auto model_max = max_tokens->get<std::int64_t>();
  if (model_max <= 0) {
    return std::nullopt;
  }
  return std::min(model_max, kDefaultMaxOutputTokensCap);
}

qt::agent::AgentConfig build_agent_config(
    const ReferenceCliArgs& parsed, const ReferenceStartupSession& startup_session,
    const qt::app::RuntimeServices& services, const qt::ai::ModelRef& model,
    const std::optional<std::string>& thinking_level) {
  qt::agent::AgentConfig config;
  config.model = model;
  if (services.subagent_coordinator) {
    std::weak_ptr<runtime::SubagentCoordinator> coordinator = services.subagent_coordinator;
    config.publish_subagent_context = [coordinator](const std::vector<ai::Message>& messages) {
      if (const auto owner = coordinator.lock())
        owner->set_context_snapshot([messages]() { return messages; });
    };
    config.take_subagent_events = [coordinator]() {
      const auto owner = coordinator.lock();
      return owner ? owner->take_events() : std::vector<ai::Json>{};
    };
  }
  config.tools = parsed.no_tools ? std::vector<qt::ai::ToolDefinition>{}
                                 : services.tool_definitions;
  if (!parsed.no_tools && services.tool_registry &&
      services.tool_registry->size() > 0U) {
    config.tool_choice = qt::ai::ToolChoice{qt::ai::ToolChoiceKind::auto_select,
                                            std::nullopt};
    config.tool_registry = services.tool_registry;
    config.tool_context = services.tool_context;
  } else {
    config.tool_choice = qt::ai::ToolChoice{qt::ai::ToolChoiceKind::none,
                                            std::nullopt};
  }
  config.temperature = 0.0;
  config.max_output_tokens =
      default_max_output_tokens_for_model(services.model_catalog, model);
  config.metadata = Json{
      {"startup_mode", "cli"},
      {"provider", config.model.provider},
      {"model", config.model.name},
      {"session", startup_session_json(startup_session)},
      {"allowSessionModelRestore",
       !(parsed.provider.has_value() || parsed.model.has_value() ||
         !parsed.models.empty())},
      {"allowSessionThinkingRestore", !parsed.thinking.has_value()},
      {"runtimeResources",
       Json{{"skills", services.resource_bundle.skills.loaded_skills.size()},
            {"promptTemplates",
             services.resource_bundle.prompt_templates.loaded_prompt_templates
                 .size()},
            {"extensions",
             services.resource_bundle.extensions.loaded_extensions.size()},
            {"contextFiles",
             services.resource_bundle.context_resources.loaded_context_files
                 .size()},
            {"systemPrompt",
             !services.resource_bundle.system_prompt.prompt.empty()}}},
      {"unknown_flags", parsed.unknown_flags},
      {"extension_flags", parsed.extension_flags},
  };
  if (thinking_level && !thinking_level->empty()) {
    config.metadata["thinkingLevel"] = clamp_thinking_level_for_model(
        services.model_catalog, model, *thinking_level);
  }
  if (model.provider == qt::ai::kLlamaProviderId) {
    const auto llama_metadata =
        services.model_selection.model.metadata.value("llama", Json::object());
    config.metadata["llama"] = llama_metadata;
    if (llama_metadata.is_object() && llama_metadata.contains("generation")) {
      config.metadata["llamaGeneration"] = llama_metadata.at("generation");
    }
  }
  return config;
}

qt::ai::ModelRef resolve_startup_model(
    const qt::app::RuntimeServices& services, const ReferenceCliArgs& parsed,
    const qt::harness::SessionManagerContext& session_context) {
  if (has_explicit_model_selection(parsed) ||
      !session_context.model) {
    return services.model_selection.model.ref;
  }
  if (session_context.model->provider !=
      services.model_selection.model.ref.provider) {
    return services.model_selection.model.ref;
  }

  const auto* resumed = qt::ai::find_model_record(
      services.model_catalog, session_context.model->provider,
      session_context.model->model_id);
  if (resumed == nullptr) {
    return services.model_selection.model.ref;
  }
  return resumed->ref;
}

std::optional<std::string> resolve_startup_thinking_level(
    const qt::app::RuntimeServices& services, const ReferenceCliArgs& parsed,
    const qt::harness::SessionManagerContext& session_context) {
  if (parsed.thinking) {
    return parsed.thinking;
  }
  if (!session_context.thinking_level.empty() &&
      session_context.thinking_level != "off") {
    return session_context.thinking_level;
  }
  if (services.settings.default_thinking_level &&
      !services.settings.default_thinking_level->empty()) {
    return services.settings.default_thinking_level;
  }
  return std::nullopt;
}

void apply_resolved_provider_credential(
    qt::ai::ProviderOptions& options, const std::string& provider,
    const qt::ai::AuthStorage& auth_storage,
    const std::optional<qt::ai::ProviderAuthRecord>& auth_record,
    const std::optional<std::string>& runtime_api_key) {
  qt::ai::ProviderCredentialResolutionRequest request;
  request.provider = provider;
  request.auth_storage = auth_storage;
  request.auth_record = auth_record;
  request.runtime_api_key = runtime_api_key;
  const auto credential = qt::ai::resolve_provider_credential(request);
  if (credential.secret) {
    options.api_key = credential.secret;
  }
}

void apply_provider_retry_settings(qt::ai::ProviderOptions& options,
                                   const qt::app::AppSettings& settings) {
  if (!options.timeout_ms && settings.provider_retry.timeout_ms) {
    options.timeout_ms = settings.provider_retry.timeout_ms;
  }
  if (!options.max_retries && settings.provider_retry.max_retries) {
    options.max_retries = settings.provider_retry.max_retries;
  }
  if (!options.max_retry_delay_ms &&
      settings.provider_retry.max_retry_delay_ms) {
    options.max_retry_delay_ms =
        settings.provider_retry.max_retry_delay_ms;
  }
}

qt::ai::ProviderOptions make_startup_provider_options(
    const qt::app::RuntimeServices& services, const ReferenceCliArgs& parsed) {
  qt::ai::ProviderOptions options =
      qt::ai::provider_options_from_model_metadata(
          services.model_selection.model);
  apply_resolved_provider_credential(
      options, services.model_selection.provider.descriptor.id,
      services.auth_storage, services.model_selection.auth, parsed.api_key);
  apply_provider_retry_settings(options, services.settings);
  return options;
}

struct ReferenceRuntimeProviderBuild {
  std::shared_ptr<qt::ai::Provider> provider;
  bool using_echo_provider = true;
};

ReferenceRuntimeProviderBuild make_runtime_provider(
    const qt::app::AppSettings& settings,
    const ResolvedModelSelection& selection,
    const ReferenceCliArgs& parsed,
    const qt::ai::ProviderOptions& options,
    const std::shared_ptr<qt::ai::LlamaProvider>& llama_provider = nullptr) {
  const auto& descriptor = selection.provider.descriptor;
  if (descriptor.id == qt::ai::kLlamaProviderId ||
      descriptor.api == qt::ai::kLlamaProviderApi) {
    return {llama_provider ? llama_provider
                           : qt::ai::make_llama_provider(settings.llama),
            false};
  }

  if (parsed.offline || env_or_empty("PI_OFFLINE") == "1") {
    return {make_echo_provider(), true};
  }

  const bool auth_optional =
      selection.provider.metadata.is_object() &&
      (!selection.provider.metadata.value("requiresAuth", true) ||
       selection.provider.metadata.value("local", false) ||
       selection.provider.metadata.value("direct", false));
  if (!options.api_key && !auth_optional) {
    return {make_echo_provider(), true};
  }

  auto base_url_value = metadata_string(options.metadata, "baseUrl");
  if (!base_url_value) {
    base_url_value = metadata_string(selection.provider.metadata, "baseUrl");
  }
  const std::string base_url = base_url_value.value_or(std::string{});

  if (descriptor.api == "openai-responses") {
    return {std::make_shared<qt::ai::OpenAIResponsesProvider>(
                descriptor, std::make_unique<qt::ai::BeastHttpTransport>(),
                append_endpoint_path(base_url, "/responses")),
            false};
  }
  if (descriptor.api == "openai-codex-responses") {
    return {std::make_shared<qt::ai::OpenAICodexResponsesProvider>(
                descriptor, std::make_unique<qt::ai::BeastHttpTransport>(),
                append_endpoint_path(base_url, "/codex/responses")),
            false};
  }
  if (descriptor.api == "openai-completions") {
    return {std::make_shared<qt::ai::OpenAICompletionsProvider>(
                descriptor, std::make_unique<qt::ai::BeastHttpTransport>(),
                append_endpoint_path(base_url, "/chat/completions")),
            false};
  }
  if (descriptor.api == "azure-openai-responses") {
    return {std::make_shared<qt::ai::AzureOpenAIResponsesProvider>(
                descriptor, std::make_unique<qt::ai::BeastHttpTransport>(),
                base_url),
            false};
  }

  return {make_echo_provider(), true};
}

void start_llama_runtime_in_background(const qt::app::RuntimeServices& services) {
  // A separately saved /llama selection is lazily loaded by /llama prompt/run.
  // Preloading it while the active agent uses a remote provider competes for
  // disk, CPU, RAM, and VRAM before the first interactive frame.
  if (services.model_selection.model.ref.provider !=
      qt::ai::kLlamaProviderId) {
    return;
  }
  std::optional<std::string> model_id =
      services.model_selection.model.ref.name;
  if (!model_id || model_id->empty()) {
    return;
  }

  auto provider = services.llama_provider;
  if (!provider) {
    return;
  }

  std::thread([provider = std::move(provider), model_id = *model_id]() {
    try {
      provider->load_model(model_id);
    } catch (const std::exception&) {
    }
  }).detach();
}

bool should_start_index_service_for_runtime(const ReferenceCliArgs& parsed) {
  if (parsed.print) {
    return false;
  }
  const std::string mode = parsed.mode.value_or("text");
  return mode != "json" && mode != "rpc";
}

void start_index_service_in_background(qt::app::RuntimeServices& services) {
  if (!services.index_service) {
    return;
  }
  auto log = qt::foundation::logger("runtime", "index");
  try {
    services.index_service->start();
    log.info("index_service_started",
             {qt::foundation::log_field(
                 "project_root",
                 services.index_service->project_root().generic_string())});
  } catch (const std::exception& error) {
    services.diagnostics.push_back(
        std::string("failed to start index service: ") + error.what());
    log.warn("index_service_start_failed",
             {qt::foundation::log_field("error", error.what())});
  }
}

qt::ai::ProviderOptions make_provider_options_for_selection(
    const ReferenceCliArgs& parsed,
    const ResolvedModelSelection& selection,
    const qt::ai::AuthStorage& auth_storage,
    const qt::app::AppSettings& settings) {
  qt::ai::ProviderOptions options =
      qt::ai::provider_options_from_model_metadata(selection.model);
  apply_resolved_provider_credential(options, selection.provider.descriptor.id,
                                     auth_storage, selection.auth,
                                     parsed.api_key);
  apply_provider_retry_settings(options, settings);
  return options;
}

foundation::Result<ResolvedModelSelection> make_explicit_model_selection(
    const RuntimeServices& services, const std::string& provider,
    const std::string& model) {
  const auto* model_record =
      qt::ai::find_model_record(services.model_catalog, provider, model);
  if (model_record == nullptr) {
    return foundation::Result<ResolvedModelSelection>::failure(
        foundation::ErrorCode::invalid_argument,
        "unknown model selection: " + provider + "/" + model);
  }

  const auto* provider_record =
      qt::ai::find_provider_record(services.model_catalog, model_record->ref.provider);
  if (provider_record == nullptr) {
    return foundation::Result<ResolvedModelSelection>::failure(
        foundation::ErrorCode::internal_error,
        "resolved model references unknown provider");
  }

  ResolvedModelSelection selection;
  selection.provider = *provider_record;
  selection.model = *model_record;
  selection.source = ModelResolutionSource::explicit_model;
  selection.auth_storage_path = default_auth_storage_path(services.settings);
  if (const auto* auth = qt::ai::find_provider_auth_record(
          services.auth_storage, provider_record->descriptor.id);
      auth != nullptr) {
    selection.auth = *auth;
  }
  return foundation::Result<ResolvedModelSelection>::success(std::move(selection));
}

std::optional<std::vector<std::string>> saved_active_tools(
    const qt::harness::SessionManager& session_manager,
    const std::vector<qt::ai::ToolDefinition>& tool_definitions) {
  constexpr std::string_view kReferenceToolsConfigEntryType = "tools-config";

  std::unordered_set<std::string> available_tool_names;
  for (const auto& tool : tool_definitions) {
    available_tool_names.insert(tool.name);
  }

  std::optional<std::vector<std::string>> saved_tools;
  for (const auto& entry : session_manager.get_branch()) {
    const auto* custom =
        std::get_if<qt::harness::SessionCustomEntry>(&entry.data);
    if (custom == nullptr ||
        custom->entry_type != kReferenceToolsConfigEntryType ||
        !custom->payload.is_object()) {
      continue;
    }
    const auto enabled_it = custom->payload.find("enabledTools");
    if (enabled_it == custom->payload.end() || !enabled_it->is_array()) {
      continue;
    }

    std::vector<std::string> enabled_tools;
    std::unordered_set<std::string> seen_tools;
    for (const auto& value : *enabled_it) {
      if (!value.is_string()) {
        continue;
      }
      const std::string tool_name = value.get<std::string>();
      if (available_tool_names.find(tool_name) == available_tool_names.end() ||
          !seen_tools.insert(tool_name).second) {
        continue;
      }
      enabled_tools.push_back(tool_name);
    }
    saved_tools = std::move(enabled_tools);
  }

  return saved_tools;
}

}  // namespace

std::shared_ptr<ai::Provider> make_echo_provider() {
  return std::make_shared<EchoProvider>();
}

foundation::Result<ReferenceRuntimeProviderSelection>
make_runtime_provider_selection(
    const RuntimeServices& services, const ReferenceCliArgs& parsed,
    const std::string& provider, const std::string& model) {
  auto log = qt::foundation::logger("runtime", "provider_selection");
  log.info("provider_selection_begin",
           {qt::foundation::log_field("provider", provider),
            qt::foundation::log_field("model", model)});
  auto selection = make_explicit_model_selection(services, provider, model);
  if (!selection) {
    log.warn("provider_selection_failed",
             {qt::foundation::log_field("error", selection.error().message)});
    return foundation::Result<ReferenceRuntimeProviderSelection>::failure(
        selection.error().code, selection.error().message,
        selection.error().system_error);
  }

  auto options = make_provider_options_for_selection(
      parsed, selection.value(), services.auth_storage, services.settings);
  auto provider_selection =
      make_runtime_provider(services.settings, selection.value(), parsed,
                            options, services.llama_provider);
  install_mcp_sampling_callback(services, provider_selection.provider, options,
                                selection.value().model.ref);

  ReferenceRuntimeProviderSelection result;
  result.provider = std::move(provider_selection.provider);
  result.options = std::move(options);
  result.model_selection = std::move(selection.value());
  result.using_echo_provider = provider_selection.using_echo_provider;
  log.info("provider_selection_complete",
           {qt::foundation::log_field(
                "provider", result.model_selection.provider.descriptor.id),
            qt::foundation::log_field(
                "model", result.model_selection.model.ref.name),
            qt::foundation::log_field("echo", result.using_echo_provider)});
  return foundation::Result<ReferenceRuntimeProviderSelection>::success(
      std::move(result));
}

harness::SessionMetadata runtime_session_metadata(
    const ReferenceStartupSession& startup_session) {
  qt::harness::SessionMetadata metadata;
  metadata.session_id =
      startup_session.session_id.value_or(generated_session_id());
  metadata.working_directory =
      startup_session.working_directory.value_or(default_cwd());
  metadata.parent_session_id = startup_session.parent_session_id;
  metadata.prompt_history = startup_session.prompt_history;
  metadata.persistent = startup_session.persistent;
  return metadata;
}

agent::AgentConfig make_demo_agent_config(
    const ReferenceCliArgs& parsed,
    const ReferenceStartupSession& startup_session) {
  qt::agent::AgentConfig config;
  const std::string provider = parsed.provider.value_or("native");
  config.model = qt::ai::ModelRef{
      provider, "deterministic", parsed.model.value_or("native-startup")};
  config.tool_choice = qt::ai::ToolChoice{qt::ai::ToolChoiceKind::none,
                                          std::nullopt};
  config.temperature = 0.0;
  config.max_output_tokens = 1024;
  config.metadata = Json{{"startup_mode", "cli"},
                         {"provider", provider},
                         {"model", config.model.name},
                         {"session", startup_session_json(startup_session)},
                         {"unknown_flags", parsed.unknown_flags},
                         {"extension_flags", parsed.extension_flags}};
  return config;
}

agent::AgentConfig make_agent_config(
    const ReferenceCliArgs& parsed,
    const ReferenceStartupSession& startup_session,
    const RuntimeServices& services, const ai::ModelRef& model,
    const std::optional<std::string>& thinking_level) {
  return build_agent_config(parsed, startup_session, services, model,
                                      thinking_level);
}

std::vector<harness::SessionEntry> load_startup_session_entries(
    const ReferenceStartupSession& startup_session) {
  if (!startup_session.repository_path) {
    return {};
  }
  const auto entries =
      qt::harness::SessionRepository(*startup_session.repository_path).load();
  if (!entries) {
    return {};
  }
  return entries.value();
}

bool session_has_thinking_change(
    const std::vector<harness::SessionEntry>& entries) {
  return std::any_of(entries.begin(), entries.end(), [](const auto& entry) {
    return std::holds_alternative<qt::harness::SessionThinkingLevelChangeEntry>(
        entry.data);
  });
}

std::string default_visible_thinking_level(
    const qt::ai::ModelCatalogModelRecord& record) {
  const auto levels = qt::ai::supported_thinking_levels(record);
  const auto has_level = [&](std::string_view level) {
    return std::find(levels.begin(), levels.end(), level) != levels.end();
  };
  if (has_level("high")) {
    return "high";
  }
  if (has_level("medium")) {
    return "medium";
  }
  for (const auto& level : levels) {
    if (level != "off") {
      return level;
    }
  }
  return "off";
}

foundation::Result<AppSettings> load_startup_settings(
    const ReferenceStartupSession& startup_session) {
  const std::string runtime_cwd =
      startup_session.working_directory.value_or(default_cwd());
  qt::harness::FilesystemRepository repository(runtime_cwd);

  qt::app::LoadSettingsRequest request;
  request.cwd = runtime_cwd;
  request.agent_home = agent_home();
  return qt::app::load_app_settings(repository, request);
}

foundation::Result<ReferenceStartupRuntime> make_startup_runtime(
    const ReferenceCliArgs& parsed,
    const ReferenceStartupSession& startup_session) {
  auto log = qt::foundation::logger("runtime", "startup_runtime");
  const std::string runtime_cwd =
      startup_session.working_directory.value_or(default_cwd());
  log.info("startup_runtime_begin",
           {qt::foundation::log_field("cwd", runtime_cwd),
            qt::foundation::log_field("session_mode", startup_session.mode),
            qt::foundation::log_field("persistent",
                                      startup_session.persistent)});
  qt::harness::FilesystemRepository repository(runtime_cwd);

  qt::app::RuntimeServicesRequest services_request;
  services_request.parent_session_id = startup_session.session_id;
  services_request.cwd = runtime_cwd;
  services_request.agent_home = agent_home();
  services_request.provider = parsed.provider;
  services_request.model = requested_model(parsed);
  const std::string launch_cwd = default_cwd();
  services_request.additional_extension_paths =
      resolve_cli_resource_paths(launch_cwd, parsed.extensions);
  services_request.additional_skill_paths =
      resolve_cli_resource_paths(launch_cwd, parsed.skills);
  services_request.additional_prompt_template_paths =
      resolve_cli_resource_paths(launch_cwd, parsed.prompt_templates);
  services_request.additional_theme_paths =
      resolve_cli_resource_paths(launch_cwd, parsed.themes);
  services_request.local_model_discovery_roots =
      resolve_cli_resource_paths(launch_cwd, parsed.local_model_dirs);
  services_request.system_prompt = parsed.system_prompt;
  services_request.append_system_prompt = parsed.append_system_prompt;
  services_request.enabled_tools = parsed.tools;
  services_request.include_builtin_tools =
      !parsed.no_tools && !parsed.no_builtin_tools;
  services_request.include_extension_tools = !parsed.no_tools;
  services_request.include_extensions = !parsed.no_extensions;
  services_request.include_skills = !parsed.no_skills;
  services_request.include_prompt_templates = !parsed.no_prompt_templates;
  services_request.include_themes = !parsed.no_themes;
  services_request.include_context_files = !parsed.no_context_files;
  services_request.include_memory = !parsed.no_memory;

  auto services = qt::app::load_runtime_services(repository, services_request);
  if (!services) {
    log.error("startup_runtime_services_failed",
              {qt::foundation::log_field("error", services.error().message)});
    return services.error();
  }

  const auto session_entries =
      load_startup_session_entries(startup_session);
  const qt::harness::SessionManager startup_session_manager(session_entries);
  const auto session_context = startup_session_manager.build_session_context();
  const auto model = resolve_startup_model(
      services.value(), parsed, session_context);
  if (const auto* record =
          qt::ai::find_model_record(services.value().model_catalog,
                                    model.provider, model.name);
      record != nullptr) {
    services.value().model_selection.model = *record;
  }
  auto thinking_level = resolve_startup_thinking_level(
      services.value(), parsed, session_context);
  if (!thinking_level) {
    const bool explicit_session_thinking_off =
        session_context.thinking_level == "off" &&
        session_has_thinking_change(session_entries);
    if (explicit_session_thinking_off) {
      thinking_level = std::string("off");
    } else if (const auto* record =
                   qt::ai::find_model_record(services.value().model_catalog,
                                             model.provider, model.name);
               record != nullptr && record->capabilities.reasoning) {
      thinking_level = default_visible_thinking_level(*record);
    }
  }

  auto provider_options =
      make_startup_provider_options(services.value(), parsed);
  if (!has_explicit_model_selection(parsed) &&
      session_context.model) {
    if (const auto* record =
            qt::ai::find_model_record(services.value().model_catalog,
                                      model.provider, model.name);
        record != nullptr) {
      provider_options = qt::ai::provider_options_from_model_metadata(*record);
      apply_resolved_provider_credential(
          provider_options,
          services.value().model_selection.provider.descriptor.id,
          services.value().auth_storage, services.value().model_selection.auth,
          parsed.api_key);
      apply_provider_retry_settings(provider_options,
                                    services.value().settings);
    }
  }

  auto provider_selection =
      make_runtime_provider(services.value().settings,
                            services.value().model_selection, parsed,
                            provider_options, services.value().llama_provider);
  install_mcp_sampling_callback(services.value(), provider_selection.provider,
                                provider_options, model);
  auto config = build_agent_config(parsed, startup_session,
                                             services.value(), model,
                                             thinking_level);
  config.metadata["allowSessionThinkingRestore"] =
      !parsed.thinking.has_value() && session_has_thinking_change(session_entries);

  qt::app::RuntimeHostRequest host_request;
  host_request.session_metadata =
      runtime_session_metadata(startup_session);
  host_request.session_entries = session_entries;
  host_request.agent_config = std::move(config);
  host_request.provider = std::move(provider_selection.provider);
  host_request.provider_options = provider_options;

  auto host = qt::app::create_runtime_host(std::move(host_request));
  if (!host) {
    log.error("startup_runtime_host_failed",
              {qt::foundation::log_field("error", host.error().message)});
    return host.error();
  }
  if (!services.value().resource_bundle.system_prompt.prompt.empty()) {
    const std::string system_prompt =
        services.value().resource_bundle.system_prompt.prompt;
    host.value().harness().add_context_hook(
        [system_prompt](
            const qt::harness::AgentHarness::ContextEvent& event)
            -> std::optional<std::vector<qt::ai::Message>> {
          std::vector<qt::ai::Message> messages = event.messages;
          qt::ai::Message system;
          system.role = qt::ai::MessageRole::system;
          system.name = "qt_system_prompt";
          system.content = {
              qt::ai::TextContent{system_prompt, std::nullopt}};
          system.metadata = qt::ai::Json{{"qt_runtime_system_prompt", true}};
          messages.insert(messages.begin(), std::move(system));
          return messages;
        });
  }
  if (services.value().memory_runtime) {
    const auto metadata = runtime_session_metadata(startup_session);
    services.value().memory_runtime->set_session_context(metadata.session_id,
                                                         metadata.session_id);
    const auto memory_runtime = services.value().memory_runtime;
    if (!host.value().harness().session_start_events().empty()) {
      const auto& started =
          host.value().harness().session_start_events().back();
      const auto nonce = std::chrono::system_clock::now()
                             .time_since_epoch()
                             .count();
      memory_runtime->record_lifecycle_event(
          "session_start", "qt:harness",
          Json{{"reason", started.reason},
               {"previous_session_file",
                started.previous_session_file.value_or(std::string{})},
               {"sequence", 0U}}
              .dump(),
          metadata.session_id + ":session_start:" +
              std::to_string(nonce));
    }
    host.value().harness().add_model_context_augmentor(
        [memory_runtime](
            const qt::harness::AgentHarness::ModelContextEvent& event)
            -> std::optional<qt::ai::CompletionRequest> {
          if (!event.logical_input_sequence.has_value()) {
            return std::nullopt;
          }
          const auto augmentation = memory_runtime->coordinator().augment(
              event.projection_epoch, *event.logical_input_sequence,
              event.request, event.cancellation_token);
          if (!augmentation) {
            qt::foundation::logger("runtime", "memory")
                .warn("memory_augmentation_failed",
                      {qt::foundation::log_field(
                          "error", augmentation.error().summary())});
            if (memory_runtime->coordinator().settings().fail_open) {
              return std::nullopt;
            }
            throw std::runtime_error(augmentation.error().summary());
          }
          for (const auto& diagnostic : augmentation.value().diagnostics) {
            qt::foundation::logger("runtime", "memory")
                .warn("memory_prehandler_diagnostic",
                      {qt::foundation::log_field("message", diagnostic)});
          }
          memory_runtime->note_recall(
              event.turn_index,
              augmentation.value().contribution.memory_ids);
          if (!augmentation.value().message.has_value()) {
            return std::nullopt;
          }
          qt::ai::CompletionRequest transformed = event.request;
          auto insertion = transformed.messages.end();
          for (auto message = transformed.messages.end();
               message != transformed.messages.begin();) {
            --message;
            if (message->role == qt::ai::MessageRole::user) {
              insertion = message;
              break;
            }
          }
          transformed.messages.insert(
              insertion, *augmentation.value().message);
          return transformed;
        });
    const auto episode_sequence =
        std::make_shared<std::atomic<std::uint64_t>>(0U);
    const auto lifecycle_sequence =
        std::make_shared<std::atomic<std::uint64_t>>(0U);
    host.value().harness().subscribe(
        [memory_runtime, episode_sequence, lifecycle_sequence,
         session_id = metadata.session_id](
            const qt::harness::AgentHarness::SubscribedEvent& event) {
          if (std::holds_alternative<
                  qt::harness::AgentHarness::AgentStartEvent>(event)) {
            memory_runtime->set_foreground_active(true);
          }
          const auto* completed = std::get_if<
              qt::harness::AgentHarness::AgentEndEvent>(&event);
          if (completed != nullptr) {
            memory_runtime->set_foreground_active(false);
          }
          if (completed != nullptr && !completed->messages.empty()) {
            const auto sequence = episode_sequence->fetch_add(1U) + 1U;
            memory_runtime->enqueue_completed_episode(
                completed->messages,
                "qt-session://" + session_id + "/episode/" +
                    std::to_string(sequence),
                qt::memory::CompletedEpisodeMetadata{
                    completed->ok,
                    completed->turn_index,
                    completed->tool_request_count,
                    completed->tool_error_count,
                    completed->error.value_or(std::string{}),
                    completed->model ? completed->model->provider : std::string{},
                    completed->model ? completed->model->name : std::string{},
                    completed->usage ? completed->usage->input_tokens : 0,
                    completed->usage ? completed->usage->output_tokens : 0,
                    {}});
            return;
          }
          Json payload = Json::object();
          std::string event_type;
          if (completed != nullptr) {
            event_type = "agent_end";
            payload["outcome_kind"] =
                completed->tool_error_count != 0U
                    ? "tool_failure"
                    : completed->ok ? "completed" : "provider_failure";
            payload["ok"] = completed->ok;
            payload["turn_index"] = completed->turn_index;
            payload["tool_request_count"] = completed->tool_request_count;
            payload["tool_error_count"] = completed->tool_error_count;
            payload["error"] = completed->error.value_or(std::string{});
          } else if (const auto* aborted =
                         std::get_if<qt::harness::AgentHarnessAbortEvent>(
                             &event)) {
            memory_runtime->set_foreground_active(false);
            event_type = "agent_abort";
            payload["outcome_kind"] = "aborted";
            payload["cleared_steer_count"] = aborted->cleared_steer.size();
            payload["cleared_follow_up_count"] =
                aborted->cleared_follow_up.size();
          } else if (const auto* started = std::get_if<
                         qt::harness::AgentHarness::SessionStartEvent>(&event)) {
            event_type = "session_start";
            payload["reason"] = started->reason;
            payload["previous_session_file"] =
                started->previous_session_file.value_or(std::string{});
          } else if (const auto* shutdown = std::get_if<
                         qt::harness::AgentHarness::SessionShutdownEvent>(&event)) {
            event_type = "session_shutdown";
            payload["reason"] = shutdown->reason;
            payload["target_session_file"] =
                shutdown->target_session_file.value_or(std::string{});
          } else if (const auto* compact = std::get_if<
                         qt::harness::AgentHarness::CompactionStartEvent>(&event)) {
            memory_runtime->set_foreground_active(true);
            event_type = "compaction_start";
            payload["reason"] = compact->reason;
          } else if (const auto* compact = std::get_if<
                         qt::harness::AgentHarness::CompactionEndEvent>(&event)) {
            memory_runtime->set_foreground_active(false);
            event_type = "compaction_end";
            payload["reason"] = compact->reason;
            payload["aborted"] = compact->aborted;
            payload["will_retry"] = compact->will_retry;
            payload["error"] =
                compact->error_message.value_or(std::string{});
          } else if (const auto* tree = std::get_if<
                         qt::harness::AgentHarness::SessionTreeEvent>(&event)) {
            event_type = "session_tree";
            payload["new_leaf_id"] =
                tree->new_leaf_id.value_or(std::string{});
            payload["old_leaf_id"] =
                tree->old_leaf_id.value_or(std::string{});
            payload["from_extension"] = tree->from_extension.value_or(false);
          }
          if (event_type.empty()) return;
          const auto sequence = lifecycle_sequence->fetch_add(1U) + 1U;
          payload["sequence"] = sequence;
          memory_runtime->record_lifecycle_event(
              event_type, "qt:harness", payload.dump(),
              session_id + ":" + event_type + ":" +
                  std::to_string(sequence));
        });
  }
  qt::harness::AgentHarnessResources harness_resources;
  harness_resources.skills =
      services.value().resource_bundle.skills.loaded_skills;
  harness_resources.prompt_templates =
      services.value().resource_bundle.prompt_templates.loaded_prompt_templates;
  host.value().harness().set_resources(std::move(harness_resources));
  if (const auto saved_tools = saved_active_tools(
          host.value().session_manager(), services.value().tool_definitions);
      saved_tools) {
    host.value().harness().set_active_tool_names(*saved_tools);
  }
  start_llama_runtime_in_background(services.value());
  if (should_start_index_service_for_runtime(parsed)) {
    start_index_service_in_background(services.value());
  }

  ReferenceStartupRuntime runtime{
      std::move(services.value()),
      std::move(host.value()),
      provider_selection.using_echo_provider,
  };
  log.info("startup_runtime_complete",
           {qt::foundation::log_field(
                "provider",
                runtime.services.model_selection.provider.descriptor.id),
            qt::foundation::log_field(
                "model", runtime.services.model_selection.model.ref.name),
            qt::foundation::log_field(
                "tools",
                static_cast<std::uint64_t>(
                    runtime.services.tool_definitions.size())),
            qt::foundation::log_field("echo", runtime.using_echo_provider)});
  return runtime;
}

}  // namespace qt::app
