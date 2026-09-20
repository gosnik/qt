#include "qt/memory/retrieval_explanation.hpp"
#include "qt/app/rpc/rpc_mode.hpp"

#include "qt/ai/model_catalog.hpp"
#include "qt/ai/openrouter_model_discovery.hpp"
#include "qt/app/export/export_html.hpp"
#include "qt/app/runtime/extensions.hpp"
#include "qt/app/runtime/runtime.hpp"
#include "qt/app/sessions/session_shared.hpp"
#include "qt/app/rpc/rpc_server.hpp"
#include "qt/app/runtime/runtime_host.hpp"
#include "qt/app/config/settings.hpp"
#include "qt/app/export/resource_inspection.hpp"
#include "qt/app/slash/slash_commands.hpp"
#include "qt/app/export/stream_output.hpp"
#include "qt/foundation/filesystem.hpp"
#include "qt/foundation/logging.hpp"
#include "qt/foundation/process.hpp"
#include "qt/harness/compaction.hpp"
#include "qt/harness/repository.hpp"
#include "qt/harness/session_repository.hpp"
#include "qt/memory/runtime.hpp"
#include "qt/memory/review_json.hpp"
#include "qt/tools/bash_tool.hpp"
#include "qt/tools/subagent_tool.hpp"
#include "qt/foundation/hash.hpp"
#include <fnmatch.h>

#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <memory>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#define QT_FILENO _fileno
#else
#include <unistd.h>
#define QT_FILENO fileno
#endif

namespace qt::app {
namespace {

using Json = nlohmann::json;

std::string review_rpc_error(const std::invalid_argument& error) {
  const std::string message = error.what();
  std::string code = "invalid_review_request";
  if (message.find("not found") != std::string::npos) {
    code = "review_not_found";
  } else if (message.find("stale") != std::string::npos ||
             message.find("changed after preview") != std::string::npos) {
    code = "stale_preview";
  } else if (message.find("confirm=true") != std::string::npos ||
             message.find("human authority") != std::string::npos) {
    code = "confirmation_required";
  } else if (message.find("not applicable") != std::string::npos ||
             message.find("requires individual review") != std::string::npos) {
    code = "review_not_applicable";
  } else if (message.find("invalid review action") != std::string::npos) {
    code = "invalid_review_action";
  } else if (message.find(" must be ") != std::string::npos ||
             message.find(" requires string ") != std::string::npos ||
             message.find("requires items") != std::string::npos ||
             message.find("items array") != std::string::npos ||
             message.find("batch item requires") != std::string::npos) {
    code = "invalid_review_schema";
  }
  return "rpc_error:" + code + ":" + message;
}

struct ReferenceRpcInputQueue {
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<std::string> lines;
  bool closed = false;
  bool read_failed = false;
};

const qt::ai::CompletionResponse* selected_turn_response(
    const qt::agent::TurnResult& turn) {
  if (turn.response.has_value()) {
    return &*turn.response;
  }
  if (!turn.responses.empty()) {
    return &turn.responses.back();
  }
  return nullptr;
}

Json memory_statistics_json(const qt::memory::MemoryStatistics& stats) {
  return Json{
      {"catalog",
       {{"total", stats.memory_entries},
        {"by_type", stats.memory_counts},
        {"by_status", stats.trust_states},
        {"hypotheses", stats.hypotheses},
        {"low_confidence", stats.low_confidence},
        {"unprovenanced", stats.unprovenanced},
        {"deprecated", stats.deprecated},
        {"failed_procedures", stats.failed_procedures},
        {"high_use_hypotheses", stats.high_use_hypotheses}}},
      {"usage",
       {{"accessed_memories", stats.accessed_memories},
        {"total_accesses", stats.total_accesses},
        {"successful_outcomes", stats.successful_outcomes},
        {"failed_outcomes", stats.failed_outcomes}}},
      {"governance",
       {{"conflicts", stats.conflict_count},
        {"unresolved_conflicts", stats.unresolved_conflicts},
        {"pending_reviews", stats.pending_reviews}}},
      {"dreaming",
       {{"attempted", stats.dreaming.attempted},
        {"completed", stats.dreaming.completed},
        {"failed", stats.dreaming.failed},
        {"pending", stats.dreaming.pending},
        {"protocol_rejections", stats.dreaming.protocol_rejections},
        {"timed_out", stats.dreaming.timed_out},
        {"cancelled", stats.dreaming.cancelled},
        {"start_failures", stats.dreaming.start_failures},
        {"cpu_fallbacks", stats.dreaming.cpu_fallbacks},
        {"reviews_created", stats.dreaming.reviews_created},
        {"by_trigger", stats.dreaming.by_trigger},
        {"by_model", stats.dreaming.by_model},
        {"failures_by_reason", stats.dreaming.failures_by_reason}}}};
}

Json assistant_message_json(const qt::agent::TurnResult& turn,
                            const qt::ai::CompletionResponse& response) {
  Json assistant_message = response.message;
  if (turn.request) {
    assistant_message["api"] = turn.request->model.api;
    assistant_message["provider"] = turn.request->model.provider;
    assistant_message["model"] = turn.request->model.name;
  }
  assistant_message["usage"] = response.usage;
  assistant_message["stopReason"] =
      qt::ai::to_string(response.stop_reason);
  return assistant_message;
}

void write_rpc_output_lines(const std::vector<std::string>& lines) {
  StreamOutputPlan output;
  output.stdout_chunks = lines;
  emit_stream_output(output, std::cout, std::cerr);
}

void write_rpc_output_lines_locked(
    const std::vector<std::string>& lines, std::mutex& output_mutex) {
  std::lock_guard<std::mutex> lock(output_mutex);
  write_rpc_output_lines(lines);
}

std::optional<std::string> rpc_abort_bash_id(std::string_view line) {
  Json json;
  try {
    json = Json::parse(line);
  } catch (const std::exception&) {
    return std::nullopt;
  }

  if (!json.is_object() || !json.contains("type") ||
      !json.at("type").is_string() ||
      json.at("type").get<std::string>() != "abort_bash") {
    return std::nullopt;
  }

  if (!json.contains("id") || json.at("id").is_null()) {
    return std::string{};
  }
  if (!json.at("id").is_string()) {
    return std::string{};
  }
  return json.at("id").get<std::string>();
}

std::vector<std::string> rpc_abort_bash_success_lines(
    const std::optional<std::string>& id) {
  const auto encoded = encode_rpc_response_line(RpcResponse(RpcSuccessResponse{
      id && id->empty() ? std::optional<std::string>{} : id, "abort_bash",
      std::nullopt}));
  if (!encoded) {
    return {};
  }
  return {encoded.value()};
}

void enqueue_rpc_line(ReferenceRpcInputQueue& queue,
                                std::string line) {
  {
    std::lock_guard<std::mutex> lock(queue.mutex);
    queue.lines.push_back(std::move(line));
  }
  queue.ready.notify_one();
}

qt::foundation::Status persist_rpc_user_settings(
    std::string_view agent_home, const std::function<void(Json&)>& mutate) {
  if (agent_home.empty()) {
    return qt::foundation::Status::failure(
        qt::foundation::ErrorCode::invalid_argument,
        "agent_home is required to persist settings");
  }

  const auto path = std::filesystem::path(std::string(agent_home)) / "settings.json";
  Json document = Json::object();
  std::error_code exists_error;
  if (std::filesystem::exists(path, exists_error) && !exists_error) {
    const auto content = qt::foundation::read_text_file(path);
    if (!content) {
      return qt::foundation::Status(content.error());
    }
    try {
      document = Json::parse(content.value());
    } catch (const std::exception& error) {
      return qt::foundation::Status::failure(
          qt::foundation::ErrorCode::invalid_argument,
          std::string("invalid settings JSON: ") + error.what());
    }
    if (!document.is_object()) {
      document = Json::object();
    }
  }

  mutate(document);
  const auto ensured = qt::foundation::ensure_directory(path.parent_path());
  if (!ensured) {
    return ensured;
  }
  return qt::foundation::write_text_file(path, document.dump(2) + "\n");
}

std::string rpc_queue_mode_name(qt::agent::QueueMode mode) {
  return mode == qt::agent::QueueMode::all ? "all" : "one-at-a-time";
}

qt::foundation::Result<qt::agent::QueueMode> parse_rpc_queue_mode(
    std::string_view mode) {
  if (mode == "all") {
    return qt::foundation::Result<qt::agent::QueueMode>::success(
        qt::agent::QueueMode::all);
  }
  if (mode == "one-at-a-time") {
    return qt::foundation::Result<qt::agent::QueueMode>::success(
        qt::agent::QueueMode::one_at_a_time);
  }
  return qt::foundation::Result<qt::agent::QueueMode>::failure(
      qt::foundation::ErrorCode::invalid_argument,
      "unsupported queue mode: " + std::string(mode));
}

Json rpc_model_json(const qt::ai::ModelRef& model) {
  return Json{{"provider", model.provider}, {"id", model.name}};
}

qt::ai::Message rpc_prompt_message(
    const qt::app::PromptCommand& command) {
  qt::ai::Message message;
  message.role = qt::ai::MessageRole::user;
  if (!command.message.empty()) {
    message.content.push_back(qt::ai::TextContent{command.message, std::nullopt});
  }
  if (command.images && command.images->is_array()) {
    for (const auto& image : *command.images) {
      if (!image.is_object()) {
        continue;
      }
      const std::string data = image.value("data", std::string{});
      const std::string mime_type =
          image.value("mimeType", std::string{"image/png"});
      if (data.empty()) {
        continue;
      }
      message.content.push_back(
          qt::ai::ImageContent{mime_type.empty() ? "image/png" : mime_type,
                               data});
    }
  }
  return message;
}

std::string assistant_text_for_rpc_update(const qt::ai::Message& message) {
  std::string text;
  for (const auto& content : message.content) {
    if (qt::ai::content_kind(content) != qt::ai::ContentKind::text) {
      continue;
    }
    text += std::get<qt::ai::TextContent>(content).text;
  }
  return text;
}

void append_assistant_content_rpc_updates(
    std::vector<qt::app::RpcEvent>& events,
    const Json& assistant_message,
    const qt::ai::Message& message) {
  for (const auto& content : message.content) {
    switch (qt::ai::content_kind(content)) {
      case qt::ai::ContentKind::text: {
        const auto& delta = std::get<qt::ai::TextContent>(content).text;
        if (!delta.empty()) {
          events.push_back(qt::app::RpcEvent(qt::app::MessageUpdateEvent{
              assistant_message,
              Json{{"type", "text_delta"}, {"delta", delta}},
          }));
        }
        break;
      }
      case qt::ai::ContentKind::thinking: {
        const auto& thinking = std::get<qt::ai::ThinkingContent>(content);
        const std::string delta =
            thinking.redacted ? std::string("[redacted]") : thinking.text;
        if (!delta.empty()) {
          events.push_back(qt::app::RpcEvent(qt::app::MessageUpdateEvent{
              assistant_message,
              Json{{"type", "thinking_delta"}, {"delta", delta}},
          }));
        }
        break;
      }
      case qt::ai::ContentKind::image:
      case qt::ai::ContentKind::tool_call:
      case qt::ai::ContentKind::tool_result:
        break;
    }
  }
}

std::string rpc_bash_context_text(
    std::string_view command,
    std::string_view output,
    bool cancelled,
    bool truncated,
    const std::optional<std::string>& full_output_path,
    const std::optional<int>& exit_code);

qt::foundation::Result<std::vector<qt::ai::Message>>
load_rpc_transcript(const ReferenceStartupSession& startup_session) {
  if (!startup_session.repository_path) {
    return qt::foundation::Result<std::vector<qt::ai::Message>>::success({});
  }

  const auto entries =
      qt::harness::SessionRepository(*startup_session.repository_path).load();
  if (!entries) {
    return qt::foundation::Result<std::vector<qt::ai::Message>>::failure(
        entries.error().code, entries.error().message,
        entries.error().system_error);
  }

  std::vector<qt::ai::Message> transcript;
  for (const auto& entry : entries.value()) {
    try {
      const auto encoded = qt::harness::encode_session_entry_jsonl(entry);
      const auto json = Json::parse(encoded);
      if (json.value("type", std::string{}) != "message" ||
          !json.contains("message") || !json.at("message").is_object()) {
        continue;
      }

      const auto& message = json.at("message");
      if (message.value("role", std::string{}) == "bashExecution") {
        if (message.value("excludeFromContext", false)) {
          continue;
        }
        qt::ai::Message bash_message;
        bash_message.role = qt::ai::MessageRole::user;
        bash_message.content.push_back(qt::ai::TextContent{
            rpc_bash_context_text(
                message.value("command", std::string{}),
                message.value("output", std::string{}),
                message.value("cancelled", false),
                message.value("truncated", false),
                message.contains("fullOutputPath") &&
                        message.at("fullOutputPath").is_string()
                    ? std::optional<std::string>(
                          message.at("fullOutputPath").get<std::string>())
                    : std::nullopt,
                message.contains("exitCode") &&
                        message.at("exitCode").is_number_integer()
                    ? std::optional<int>(message.at("exitCode").get<int>())
                    : std::nullopt),
            std::nullopt});
        if (json.contains("id") && json.at("id").is_string()) {
          bash_message.id = json.at("id").get<std::string>();
        }
        transcript.push_back(std::move(bash_message));
        continue;
      }

      transcript.push_back(message.get<qt::ai::Message>());
    } catch (const std::exception&) {
      continue;
    }
  }
  return qt::foundation::Result<std::vector<qt::ai::Message>>::success(
      std::move(transcript));
}

std::string rpc_session_dir(const ReferenceCliArgs& parsed,
                                      const std::string& cwd,
                                      const std::string& agent_home) {
  const auto session_dir =
      resolve_startup_session_dir(parsed, cwd, agent_home);
  if (!session_dir) {
    return (qt::foundation::Path(agent_home) / "sessions").generic_string();
  }
  return session_dir.value();
}

std::vector<qt::harness::SessionEntry> load_rpc_session_entries(
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

std::optional<std::string> rpc_session_name_from_entries(
    const std::vector<qt::harness::SessionEntry>& entries) {
  std::optional<std::string> name;
  for (const auto& entry : entries) {
    std::visit(
        [&name](const auto& value) {
          using Value = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<Value, qt::harness::SessionMetadataEntry>) {
            if (value.metadata.display_name &&
                !value.metadata.display_name->empty()) {
              name = value.metadata.display_name;
            }
          } else if constexpr (std::is_same_v<Value, qt::harness::SessionInfoEntry>) {
            if (value.name && !value.name->empty()) {
              name = value.name;
            }
          }
        },
        entry.data);
  }
  return name;
}

std::optional<std::string> rpc_initial_session_name(
    const ReferenceStartupSession& startup_session) {
  return rpc_session_name_from_entries(
      load_rpc_session_entries(startup_session));
}

qt::harness::SessionStats rpc_seed_session_stats(
    const ReferenceStartupSession& startup_session,
    const qt::ai::ModelRef& model) {
  return qt::harness::compute_session_stats(
      load_rpc_session_entries(startup_session),
      rpc_context_window(model));
}

Json rpc_source_info(std::string path, std::string source,
                               const std::string& cwd,
                               const std::string& agent_home) {
  const auto agent_home_path = std::filesystem::path(agent_home);
  const auto cwd_path = std::filesystem::path(cwd);
  std::string scope = "temporary";
  if (!path.empty()) {
    const auto target = std::filesystem::path(path).lexically_normal();
    const auto normalized_agent_home = agent_home_path.lexically_normal();
    const auto normalized_cwd = cwd_path.lexically_normal();
    if (!normalized_agent_home.empty() &&
        target.generic_string().rfind(normalized_agent_home.generic_string(), 0) ==
            0) {
      scope = "user";
    } else if (!normalized_cwd.empty() &&
               target.generic_string().rfind(normalized_cwd.generic_string(), 0) ==
                   0) {
      scope = "project";
    }
  }

  return Json{{"path", std::move(path)},
              {"source", std::move(source)},
              {"scope", scope},
              {"origin", "top-level"}};
}

Json make_rpc_commands(const ReferenceCliArgs& parsed,
                                 const std::string& cwd,
                                 const std::string& agent_home) {
  qt::harness::FilesystemRepository repository(cwd);

  qt::app::LoadSettingsRequest settings_request;
  settings_request.cwd = cwd;
  if (!agent_home.empty()) {
    settings_request.agent_home = agent_home;
  }
  const auto settings = qt::app::load_app_settings(repository, settings_request);
  if (!settings) {
    return Json{{"commands", Json::array()},
                {"diagnostics", Json::array({settings.error().message})}};
  }

  auto adjusted = settings.value();
  if (parsed.no_prompt_templates) {
    adjusted.features.load_prompt_templates = false;
    adjusted.prompt_roots.clear();
  }
  if (parsed.no_skills) {
    adjusted.features.load_skills = false;
    adjusted.skill_roots.clear();
  }

  const auto resources = qt::app::inspect_app_resources(repository, adjusted);
  const auto extensions = load_extensions(
      repository, cwd, agent_home, adjusted.workspace_root, parsed.extensions,
      qt::app::LoadReferenceExtensionOptions{!parsed.no_extensions});

  qt::app::SlashCommandRegistryOptions registry_options;
  registry_options.enable_skill_commands = adjusted.enable_skill_commands;
  const auto registry = qt::app::build_slash_command_registry(
      resources.prompt_templates, resources.skills, extensions, registry_options);

  Json commands = Json::array();
  const auto append_group = [&](qt::app::SlashCommandSource source) {
    for (const auto& command : registry.commands) {
      if (command.source != source) {
        continue;
      }
      Json payload{
          {"name", command.name},
          {"source", qt::app::to_string(command.source)},
          {"sourceInfo",
           rpc_source_info(command.source_path,
                                     qt::app::to_string(command.source), cwd,
                                     agent_home)}};
      if (!command.description.empty()) {
        payload["description"] = command.description;
      }
      commands.push_back(std::move(payload));
    }
  };

  append_group(qt::app::SlashCommandSource::kExtension);
  append_group(qt::app::SlashCommandSource::kPrompt);
  append_group(qt::app::SlashCommandSource::kSkill);

  Json diagnostics = Json::array();
  for (const auto& diagnostic : registry.diagnostics) {
    diagnostics.push_back(diagnostic);
  }
  for (const auto& diagnostic : extensions.diagnostics) {
    diagnostics.push_back(diagnostic);
  }

  return Json{
      {"commands", std::move(commands)},
      {"diagnostics", std::move(diagnostics)},
  };
}

struct ReferenceRpcRuntime {
  ReferenceStartupSession startup_session;
  std::string agent_home;
  qt::agent::AgentConfig config;
  qt::app::RuntimeHost host;
  qt::agent::QueueMode steering_mode = qt::agent::QueueMode::one_at_a_time;
  qt::agent::QueueMode follow_up_mode = qt::agent::QueueMode::one_at_a_time;
  std::optional<std::string> session_name;
  qt::harness::SessionStats session_stats;
  bool auto_compaction_enabled = false;
  bool auto_retry_enabled = false;
  std::vector<qt::ai::Message> pending_steering;
  std::vector<qt::ai::Message> pending_follow_up;
  std::shared_ptr<qt::memory::MemoryRuntime> memory_runtime;
  MemorySettings memory_settings;
  bool memory_disabled_by_cli = false;
  std::string memory_initialization_error;
  std::shared_ptr<qt::app::runtime::SubagentCoordinator> subagent_coordinator;
  std::unique_ptr<qt::tools::SubagentTool> subagent_tool;
};

std::string rpc_child_executable() {
#if defined(_WIN32)
  return "qt";
#else
  std::error_code error;
  const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
  return error || executable.empty() ? std::string("qt")
                                     : executable.generic_string();
#endif
}

std::string rpc_subagent_namespace(const std::string& session_id) {
  return qt::foundation::stable_hash_hex(session_id);
}

std::shared_ptr<qt::memory::MemoryRuntime> make_rpc_memory_runtime(
    const ReferenceCliArgs& parsed, const AppSettings& settings,
    const ReferenceStartupSession& session) {
  if (parsed.no_memory || !settings.memory.enabled) {
    return nullptr;
  }
  qt::memory::MemoryRuntimeOptions options;
  const auto memory_models = resolve_memory_task_models(settings);
  options.agent_home = settings.agent_home;
  options.cwd = settings.cwd;
  options.session = session.session_id.value_or(std::string{});
  options.agent = session.session_id.value_or(std::string{});
  options.coordinator.enabled = true;
  options.coordinator.automatic_recall = settings.memory.automatic_recall;
  options.coordinator.recall_policy =
      settings.memory.recall_policy == "strict"
          ? qt::memory::RecallPolicy::Strict
          : settings.memory.recall_policy == "exploratory"
                ? qt::memory::RecallPolicy::Broad
                : qt::memory::RecallPolicy::Balanced;
  options.coordinator.context_token_budget =
      settings.memory.context_token_budget;
  options.coordinator.candidate_limit = settings.memory.candidate_limit;
  options.coordinator.model = settings.memory.prehandler.model.value_or(
      memory_models.recall_prehandler.value_or(std::string{}));
  options.coordinator.model_task = "recall_prehandler";
  options.coordinator.thinking = settings.memory.prehandler.thinking;
  options.coordinator.timeout_ms = settings.memory.prehandler.timeout_ms;
  if (!settings.memory.prehandler.model.has_value() &&
      memory_models.recall_prehandler.has_value()) {
    options.coordinator.timeout_ms =
        std::max<std::int64_t>(
            options.coordinator.timeout_ms, 120000);
  }
  options.coordinator.fail_open = settings.memory.prehandler.fail_open;
  options.coordinator.max_model_input_tokens =
      std::min<std::size_t>(
          settings.memory.prehandler.max_model_input_tokens,
          memory_models.recall_prehandler.has_value() ? 3000U : 12000U);
  options.coordinator.model_discovery_roots = memory_models.discovery_roots;
  if (memory_models.context_compilation.has_value() &&
      *memory_models.context_compilation != options.coordinator.model) {
    options.consolidation_model = *memory_models.context_compilation;
    options.consolidation_timeout_ms =
        memory_models.context_compilation_timeout_floor_ms;
    options.consolidation_max_model_input_tokens =
        memory_models.context_compilation_input_token_limit;
    options.consolidation_model_discovery_roots =
        memory_models.discovery_roots;
  }
  options.dreaming.enabled = settings.memory.dreaming.enabled;
  options.dreaming.trigger = settings.memory.dreaming.trigger;
  options.dreaming.interval_seconds = static_cast<std::int64_t>(
      settings.memory.dreaming.interval_seconds);
  options.dreaming.idle_seconds = static_cast<std::int64_t>(
      settings.memory.dreaming.idle_seconds);
  options.dreaming.candidate_threshold =
      settings.memory.dreaming.candidate_threshold;
  options.dreaming.model =
      settings.memory.dreaming.model.value_or(std::string{});
  options.dreaming.verifier_model =
      settings.memory.dreaming.verifier_model.value_or(std::string{});
  options.dreaming.timeout_ms = settings.memory.dreaming.timeout_ms;
  options.dreaming.max_model_input_tokens =
      settings.memory.dreaming.max_model_input_tokens;
  options.dreaming.max_candidates = settings.memory.dreaming.max_candidates;
  options.dreaming.research_policy =
      settings.memory.dreaming.research_policy;
  options.dreaming.promotion_policy =
      settings.memory.dreaming.promotion_policy;
  options.isolated_agent.executable = rpc_child_executable();
  options.automatic_capture = settings.memory.automatic_capture;
  options.capture_post_process = settings.memory.capture.post_process;
  options.max_evidence_characters = settings.memory.capture.max_evidence_chars;
  return std::make_shared<qt::memory::MemoryRuntime>(std::move(options));
}

void attach_rpc_memory(ReferenceRpcRuntime& runtime) {
  if (!runtime.memory_runtime) {
    return;
  }
  const auto memory = runtime.memory_runtime;
  const std::string session_id =
      runtime.startup_session.session_id.value_or(std::string{});
  memory->set_session_context(session_id, session_id);
  if (!runtime.host.harness().session_start_events().empty()) {
    const auto& started = runtime.host.harness().session_start_events().back();
    const auto nonce =
        std::chrono::system_clock::now().time_since_epoch().count();
    memory->record_lifecycle_event(
        "session_start", "qt:harness",
        Json{{"reason", started.reason},
             {"previous_session_file",
              started.previous_session_file.value_or(std::string{})},
             {"sequence", 0U}}
            .dump(),
        session_id + ":rpc:session_start:" + std::to_string(nonce));
  }
  runtime.host.harness().add_model_context_augmentor(
      [memory](const qt::harness::AgentHarness::ModelContextEvent& event)
          -> std::optional<qt::ai::CompletionRequest> {
        if (!event.logical_input_sequence) {
          return std::nullopt;
        }
        const auto augmentation = memory->coordinator().augment(
            event.projection_epoch, *event.logical_input_sequence,
            event.request, event.cancellation_token);
        if (augmentation) {
          memory->note_recall(event.turn_index,
                              augmentation.value().contribution.memory_ids);
        }
        if (!augmentation || !augmentation.value().message) {
          if (!augmentation && !memory->coordinator().settings().fail_open) {
            throw std::runtime_error(augmentation.error().summary());
          }
          return std::nullopt;
        }
        qt::ai::CompletionRequest transformed = event.request;
        auto insertion = transformed.messages.end();
        for (auto current = transformed.messages.end();
             current != transformed.messages.begin();) {
          --current;
          if (current->role == qt::ai::MessageRole::user) {
            insertion = current;
            break;
          }
        }
        transformed.messages.insert(insertion,
                                    *augmentation.value().message);
        return transformed;
      });
  const auto episode = std::make_shared<std::atomic<std::uint64_t>>(0U);
  const auto lifecycle = std::make_shared<std::atomic<std::uint64_t>>(0U);
  runtime.host.harness().subscribe(
      [memory, episode, lifecycle, session_id](
          const qt::harness::AgentHarness::SubscribedEvent& event) {
        if (std::holds_alternative<
                qt::harness::AgentHarness::AgentStartEvent>(event)) {
          memory->set_foreground_active(true);
        }
        const auto* completed =
            std::get_if<qt::harness::AgentHarness::AgentEndEvent>(&event);
        if (completed != nullptr) {
          memory->set_foreground_active(false);
        }
        if (completed != nullptr && !completed->messages.empty()) {
          memory->enqueue_completed_episode(
              completed->messages,
              "qt-session://" + session_id + "/rpc-episode/" +
                  std::to_string(episode->fetch_add(1U) + 1U),
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
          memory->set_foreground_active(false);
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
          memory->set_foreground_active(true);
          event_type = "compaction_start";
          payload["reason"] = compact->reason;
        } else if (const auto* compact = std::get_if<
                       qt::harness::AgentHarness::CompactionEndEvent>(&event)) {
          memory->set_foreground_active(false);
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
        const auto sequence = lifecycle->fetch_add(1U) + 1U;
        payload["sequence"] = sequence;
        memory->record_lifecycle_event(
            event_type, "qt:harness", payload.dump(),
            session_id + ":rpc:" + event_type + ":" +
                std::to_string(sequence));
      });
}

class ReferenceRpcBashState {
 public:
  std::shared_ptr<qt::foundation::ProcessCancellationToken> begin() {
    auto token =
        std::make_shared<qt::foundation::ProcessCancellationToken>();
    std::lock_guard<std::mutex> lock(mutex_);
    active_token_ = token;
    return token;
  }

  void finish(
      const std::shared_ptr<qt::foundation::ProcessCancellationToken>& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_token_ == token) {
      active_token_.reset();
    }
  }

  void abort() {
    std::shared_ptr<qt::foundation::ProcessCancellationToken> token;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      token = active_token_;
    }
    if (token) {
      token->cancel();
    }
  }

 private:
  std::mutex mutex_;
  std::shared_ptr<qt::foundation::ProcessCancellationToken> active_token_;
};

qt::foundation::Status append_rpc_runtime_entry(
    ReferenceRpcRuntime& runtime, const qt::harness::SessionEntry& entry) {
  if (!runtime.startup_session.repository_path) {
    return qt::foundation::Status::success();
  }

  const auto appended =
      qt::harness::SessionRepository(*runtime.startup_session.repository_path)
          .append(repository_ready_session_entry(entry));
  if (!appended.ok()) {
    return appended;
  }
  ++runtime.startup_session.entry_count;
  return qt::foundation::Status::success();
}

qt::foundation::Status replace_rpc_runtime_entries(
    ReferenceRpcRuntime& runtime,
    const std::vector<qt::harness::SessionEntry>& entries) {
  if (!runtime.startup_session.repository_path) {
    return qt::foundation::Status::success();
  }

  std::vector<qt::harness::SessionEntry> persisted;
  persisted.reserve(entries.size());
  for (const auto& entry : entries) {
    persisted.push_back(repository_ready_session_entry(entry));
  }

  const auto replaced =
      qt::harness::SessionRepository(*runtime.startup_session.repository_path)
          .replace_all(persisted);
  if (!replaced.ok()) {
    return replaced;
  }
  runtime.startup_session.entry_count = persisted.size();
  return qt::foundation::Status::success();
}

qt::foundation::Status append_rpc_upstream_payload(
    ReferenceRpcRuntime& runtime, const Json& payload) {
  const std::string entry_id = runtime.host.session_manager().append_custom_entry(
      qt::harness::kUpstreamPiMonoSessionEntryType, payload);
  const auto* entry = runtime.host.session_manager().get_entry(entry_id);
  if (entry == nullptr) {
    return qt::foundation::Status::failure(
        qt::foundation::ErrorCode::internal_error,
        "failed to append rpc session entry");
  }
  return append_rpc_runtime_entry(runtime, *entry);
}

qt::foundation::Status append_rpc_model_change(
    ReferenceRpcRuntime& runtime, std::size_t previous_entry_count) {
  runtime.host.sync_session_manager_from_harness();
  const auto& entries = runtime.host.session_manager().entries();
  if (entries.size() <= previous_entry_count) {
    return qt::foundation::Status::failure(
        qt::foundation::ErrorCode::internal_error,
        "failed to append rpc model change");
  }
  return append_rpc_runtime_entry(runtime, entries.back());
}

qt::foundation::Status append_rpc_thinking_level_change(
    ReferenceRpcRuntime& runtime, std::size_t previous_entry_count) {
  runtime.host.sync_session_manager_from_harness();
  const auto& entries = runtime.host.session_manager().entries();
  if (entries.size() <= previous_entry_count) {
    return qt::foundation::Status::failure(
        qt::foundation::ErrorCode::internal_error,
        "failed to append rpc thinking change");
  }
  return append_rpc_runtime_entry(runtime, entries.back());
}

void refresh_rpc_context_usage(ReferenceRpcRuntime& runtime) {
  const auto context_window =
      rpc_context_window(runtime.host.harness().model());
  if (!context_window) {
    runtime.session_stats.context_usage.reset();
    return;
  }

  qt::harness::SessionContextUsage usage;
  usage.context_window = *context_window;
  if (runtime.session_stats.context_usage &&
      runtime.session_stats.context_usage->tokens) {
    usage.tokens = runtime.session_stats.context_usage->tokens;
    if (*context_window > 0) {
      usage.percent =
          (static_cast<double>(*usage.tokens) / static_cast<double>(*context_window)) *
          100.0;
    }
  } else {
    usage.tokens = 0;
    usage.percent = 0.0;
  }
  runtime.session_stats.context_usage = usage;
}

qt::foundation::Result<qt::app::RuntimeHost> make_rpc_host(
    const qt::agent::AgentConfig& config,
    const ReferenceStartupSession& startup_session,
    std::string session_start_reason = "startup",
    std::optional<std::string> previous_session_file = std::nullopt) {
  qt::app::RuntimeHostRequest request;
  request.session_metadata = runtime_session_metadata(startup_session);
  request.session_entries = load_startup_session_entries(startup_session);
  request.agent_config = config;
  request.provider = make_echo_provider();
  request.session_start_reason = std::move(session_start_reason);
  request.previous_session_file = std::move(previous_session_file);
  return qt::app::create_runtime_host(std::move(request));
}

qt::foundation::Status apply_rpc_transcript_override(
    qt::app::RuntimeHost& host,
    const ReferenceStartupSession& startup_session) {
  auto transcript = load_rpc_transcript(startup_session);
  if (!transcript) {
    return qt::foundation::Status::failure(
        transcript.error().code, transcript.error().message,
        transcript.error().system_error);
  }
  if (!transcript.value().empty()) {
    host.harness().agent().state().transcript = std::move(transcript.value());
  }
  return qt::foundation::Status::success();
}

std::string configured_thinking_level(
    const qt::agent::AgentConfig& config) {
  if (!config.metadata.is_object()) {
    return "off";
  }
  const auto it = config.metadata.find("thinkingLevel");
  if (it == config.metadata.end() || !it->is_string()) {
    return "off";
  }
  return it->get<std::string>();
}

qt::foundation::Result<ReferenceRpcRuntime> make_rpc_runtime(
    const ReferenceCliArgs& parsed,
    const ReferenceStartupSession& startup_session) {
  const auto startup_settings = load_startup_settings(startup_session);
  if (!startup_settings) {
    return qt::foundation::Result<ReferenceRpcRuntime>::failure(
        startup_settings.error().code, startup_settings.error().message,
        startup_settings.error().system_error);
  }
  auto config = make_demo_agent_config(parsed, startup_session);
  config.tool_context =
      make_runtime_tool_context(startup_settings.value().cwd,
                                startup_settings.value().workspace_root,
                                startup_settings.value()
                                    .allow_external_filesystem_access);
  if (parsed.thinking) {
    config.metadata["thinkingLevel"] = *parsed.thinking;
  } else {
    config.metadata["thinkingLevel"] = "medium";
  }

  auto host = make_rpc_host(config, startup_session);
  if (!host) {
    return host.error();
  }
  const auto session_entries =
      load_startup_session_entries(startup_session);
  if (!session_has_thinking_change(session_entries)) {
    host.value().harness().set_thinking_level(
        configured_thinking_level(config));
  }
  const auto transcript_applied =
      apply_rpc_transcript_override(host.value(), startup_session);
  if (!transcript_applied.ok()) {
    return qt::foundation::Result<ReferenceRpcRuntime>::failure(
        transcript_applied.error().code, transcript_applied.error().message,
        transcript_applied.error().system_error);
  }

  ReferenceRpcRuntime runtime{
      startup_session,
      startup_settings.value().agent_home,
      std::move(config),
      std::move(host.value()),
      qt::agent::QueueMode::one_at_a_time,
      qt::agent::QueueMode::one_at_a_time,
      std::nullopt,
      {},
      false,
      startup_settings.value().retry_enabled,
      {},
      {},
      nullptr,
      {},
      false,
      {},
      nullptr,
      nullptr,
  };
  runtime.memory_settings = startup_settings.value().memory;
  runtime.memory_disabled_by_cli = parsed.no_memory;
  try {
    runtime.memory_runtime = make_rpc_memory_runtime(
        parsed, startup_settings.value(), startup_session);
    attach_rpc_memory(runtime);
  } catch (const std::exception& error) {
    runtime.memory_initialization_error = error.what();
    if (!startup_settings.value().memory.prehandler.fail_open) {
      return qt::foundation::Result<ReferenceRpcRuntime>::failure(
          qt::foundation::ErrorCode::io_error,
          std::string("unable to initialize RPC built-in memory: ") +
              error.what());
    }
  }
  if (runtime.host.seed_state().transcript_message_count == 0U) {
    runtime.host.harness().set_thinking_level(
        configured_thinking_level(runtime.config));
  }
  runtime.session_name = rpc_initial_session_name(startup_session);
  runtime.session_stats = rpc_seed_session_stats(
      startup_session, runtime.host.harness().model());
  refresh_rpc_context_usage(runtime);
  return runtime;
}

qt::foundation::Status load_rpc_runtime_session(
    ReferenceRpcRuntime& runtime, ReferenceStartupSession next_session,
    std::string session_replacement_reason = "reload") {
  const bool preserve_pending_queue =
      runtime.startup_session.repository_path.has_value() &&
      next_session.repository_path.has_value() &&
      runtime.startup_session.repository_path ==
          next_session.repository_path &&
      runtime.startup_session.session_id == next_session.session_id;
  const auto previous_session_file = runtime.startup_session.repository_path;
  runtime.host.harness().emit_session_shutdown(
      session_replacement_reason, next_session.repository_path);
  auto host = make_rpc_host(runtime.config, next_session,
                                     std::move(session_replacement_reason),
                                     previous_session_file);
  if (!host) {
    return qt::foundation::Status::failure(host.error().code, host.error().message,
                                           host.error().system_error);
  }
  const auto session_entries =
      load_startup_session_entries(next_session);
  if (!session_has_thinking_change(session_entries)) {
    host.value().harness().set_thinking_level(
        configured_thinking_level(runtime.config));
  }
  const auto transcript_applied =
      apply_rpc_transcript_override(host.value(), next_session);
  if (!transcript_applied.ok()) {
    return transcript_applied;
  }

  runtime.startup_session = std::move(next_session);
  runtime.session_name =
      rpc_initial_session_name(runtime.startup_session);
  runtime.host = std::move(host.value());
  attach_rpc_memory(runtime);
  runtime.host.harness().set_steering_mode(runtime.steering_mode);
  runtime.host.harness().set_follow_up_mode(runtime.follow_up_mode);
  if (runtime.host.seed_state().transcript_message_count == 0U) {
    runtime.host.harness().set_thinking_level(
        configured_thinking_level(runtime.config));
  }
  runtime.session_stats = rpc_seed_session_stats(
      runtime.startup_session, runtime.host.harness().model());
  refresh_rpc_context_usage(runtime);
  if (!preserve_pending_queue) {
    runtime.pending_steering.clear();
    runtime.pending_follow_up.clear();
  }
  return qt::foundation::Status::success();
}

std::string message_text_for_rpc_queue(const qt::ai::Message& message) {
  std::string text;
  for (const auto& content : message.content) {
    if (qt::ai::content_kind(content) != qt::ai::ContentKind::text) {
      continue;
    }
    text += std::get<qt::ai::TextContent>(content).text;
  }
  return text;
}

qt::app::QueueUpdateEvent make_rpc_queue_update_event(
    const ReferenceRpcRuntime& runtime) {
  qt::app::QueueUpdateEvent event;
  for (const auto& message : runtime.pending_steering) {
    event.steering.push_back(message_text_for_rpc_queue(message));
  }
  for (const auto& message : runtime.pending_follow_up) {
    event.follow_up.push_back(message_text_for_rpc_queue(message));
  }
  return event;
}

qt::ai::ModelCatalog rpc_model_catalog() {
  return qt::ai::merge_catalogs(qt::ai::default_model_catalog(),
                                qt::ai::cached_openrouter_models());
}

Json make_rpc_available_models() {
  Json models = Json::array();
  const auto catalog = rpc_model_catalog();
  for (const auto& model : catalog.models) {
    models.push_back(Json{{"provider", model.ref.provider},
                          {"id", model.ref.name},
                          {"displayName", model.display_name},
                          {"reasoning", model.capabilities.reasoning},
                          {"vision", model.capabilities.vision}});
  }
  return Json{{"models", std::move(models)}};
}

std::optional<std::string> cycle_rpc_thinking_level(
    const qt::ai::ModelRef& model,
    std::string_view current) {
  const auto catalog = rpc_model_catalog();
  const auto* record =
      qt::ai::find_model_record(catalog, model.provider, model.name);
  if (record == nullptr || !record->capabilities.reasoning) {
    return std::nullopt;
  }

  const std::vector<std::string> levels =
      qt::ai::supported_thinking_levels(*record);
  if (levels.empty()) {
    return std::nullopt;
  }

  auto current_it = std::find(levels.begin(), levels.end(), current);
  if (current_it == levels.end()) {
    return levels.front();
  }
  const std::size_t index =
      static_cast<std::size_t>(std::distance(levels.begin(), current_it));
  return levels[(index + 1U) % levels.size()];
}

std::string clamp_rpc_thinking_level(const qt::ai::ModelRef& model,
                                     const std::string& level) {
  const auto catalog = rpc_model_catalog();
  const auto* record =
      qt::ai::find_model_record(catalog, model.provider, model.name);
  return record == nullptr ? level : qt::ai::clamp_thinking_level(*record, level);
}

std::vector<qt::ai::ModelRef> rpc_available_model_refs() {
  std::vector<qt::ai::ModelRef> models;
  const auto catalog = rpc_model_catalog();
  for (const auto& record : catalog.models) {
    models.push_back(record.ref);
  }
  return models;
}

std::string rpc_bash_context_text(
    std::string_view command,
    std::string_view output,
    bool cancelled,
    bool truncated,
    const std::optional<std::string>& full_output_path,
    const std::optional<int>& exit_code) {
  std::string text = "Ran `" + std::string(command) + "`\n";
  if (!output.empty()) {
    text += "```\n";
    text += std::string(output);
    text += "\n```";
  } else {
    text += "(no output)";
  }
  if (cancelled) {
    text += "\n\n(command cancelled)";
  } else if (exit_code && *exit_code != 0) {
    text += "\n\nCommand exited with code " + std::to_string(*exit_code);
  }
  if (truncated && full_output_path) {
    text += "\n\n[Output truncated. Full output: " + *full_output_path + "]";
  }
  return text;
}

std::vector<ReferenceRpcForkMessage> rpc_fork_messages(
    const ReferenceRpcRuntime& runtime) {
  return session_fork_messages(runtime.host.session_manager());
}

Json make_rpc_fork_messages_payload(
    const ReferenceRpcRuntime& runtime) {
  Json messages = Json::array();
  for (const auto& message : rpc_fork_messages(runtime)) {
    messages.push_back(
        Json{{"entryId", message.entry_id}, {"text", message.text}});
  }
  return Json{{"messages", std::move(messages)}};
}

std::optional<std::string> rpc_fork_selected_text(
    const ReferenceRpcRuntime& runtime,
    std::string_view entry_id) {
  for (const auto& message : rpc_fork_messages(runtime)) {
    if (message.entry_id == entry_id) {
      return message.text;
    }
  }
  return std::nullopt;
}

Json make_rpc_compaction_payload(
    const qt::harness::ModelBackedCompactionResult& result) {
  Json payload{{"summary", result.summary},
               {"firstKeptEntryId", result.first_kept_entry_id},
               {"tokensBefore", result.tokens_before}};
  if (!result.details.is_null() && !result.details.empty()) {
    payload["details"] = result.details;
  }
  return payload;
}

Json make_rpc_session_stats(const ReferenceRpcRuntime& runtime) {
  Json models = Json::object();
  for (const auto& [model, model_stats] : runtime.session_stats.models) {
    models[model] =
        Json{{"assistantMessages", model_stats.assistant_messages},
             {"tokens",
              {{"input", model_stats.tokens.input},
               {"output", model_stats.tokens.output},
               {"cacheRead", model_stats.tokens.cache_read},
               {"cacheWrite", model_stats.tokens.cache_write},
               {"total", model_stats.tokens.total}}},
             {"cost", model_stats.cost},
             {"unknownUsage", model_stats.has_unknown_usage}};
  }

  Json stats{
      {"sessionFile", runtime.startup_session.repository_path
                          ? Json(*runtime.startup_session.repository_path)
                          : Json(nullptr)},
      {"sessionId", runtime.startup_session.session_id.value_or("in-memory")},
      {"userMessages", runtime.session_stats.user_messages},
      {"assistantMessages", runtime.session_stats.assistant_messages},
      {"toolCalls", runtime.session_stats.tool_calls},
      {"toolResults", runtime.session_stats.tool_results},
      {"totalMessages", runtime.session_stats.total_messages},
      {"tokens",
       {{"input", runtime.session_stats.tokens.input},
        {"output", runtime.session_stats.tokens.output},
        {"cacheRead", runtime.session_stats.tokens.cache_read},
        {"cacheWrite", runtime.session_stats.tokens.cache_write},
        {"total", runtime.session_stats.tokens.total}}},
      {"cost", runtime.session_stats.cost},
      {"unknownUsage", runtime.session_stats.has_unknown_usage},
      {"models", std::move(models)},
  };

  if (runtime.session_stats.context_usage) {
    Json usage{
        {"contextWindow", runtime.session_stats.context_usage->context_window}};
    if (runtime.session_stats.context_usage->tokens) {
      usage["tokens"] = *runtime.session_stats.context_usage->tokens;
    } else {
      usage["tokens"] = nullptr;
    }
    if (runtime.session_stats.context_usage->percent) {
      usage["percent"] = *runtime.session_stats.context_usage->percent;
    } else {
      usage["percent"] = nullptr;
    }
    stats["contextUsage"] = std::move(usage);
  }

  return stats;
}

Json make_rpc_last_assistant_text(const ReferenceRpcRuntime& runtime) {
  for (auto it = runtime.host.harness().agent().state().transcript.rbegin();
       it != runtime.host.harness().agent().state().transcript.rend(); ++it) {
    if (it->role != qt::ai::MessageRole::assistant) {
      continue;
    }
    return Json{{"text", assistant_text_for_rpc_update(*it)}};
  }
  return Json{{"text", nullptr}};
}

qt::app::RpcSessionState make_rpc_state(
    const ReferenceRpcRuntime& runtime) {
  qt::app::RpcSessionState state;
  state.model = rpc_model_json(runtime.host.harness().model());
  state.thinking_level = runtime.host.harness().thinking_level();
  state.is_streaming = runtime.host.harness().busy();
  state.is_compacting = false;
  state.steering_mode = rpc_queue_mode_name(runtime.steering_mode);
  state.follow_up_mode = rpc_queue_mode_name(runtime.follow_up_mode);
  state.session_file = runtime.startup_session.repository_path;
  state.session_id =
      runtime.startup_session.session_id.value_or(std::string("in-memory"));
  state.session_name = runtime.session_name;
  state.auto_compaction_enabled = runtime.auto_compaction_enabled;
  state.message_count = runtime.host.harness().agent().state().transcript.size();
  state.pending_message_count =
      runtime.host.harness().agent().state().pending.size() +
      runtime.pending_steering.size() + runtime.pending_follow_up.size() +
      runtime.host.harness().next_turn_queue().size();
  return state;
}

Json make_rpc_messages(const ReferenceRpcRuntime& runtime) {
  Json messages = Json::array();
  for (const auto& message : runtime.host.harness().agent().state().transcript) {
    messages.push_back(Json(message));
  }
  return Json{{"messages", std::move(messages)}};
}

std::vector<qt::app::RpcEvent> make_rpc_turn_events(
    const qt::agent::TurnResult& result) {
  std::vector<qt::app::RpcEvent> events;
  const auto* response = selected_turn_response(result);
  if (response == nullptr) {
    return events;
  }

  const Json assistant_message = Json(response->message);
  events.push_back(qt::app::RpcEvent(qt::app::AgentStartEvent{}));
  events.push_back(qt::app::RpcEvent(qt::app::TurnStartEvent{}));
  events.push_back(
      qt::app::RpcEvent(qt::app::MessageStartEvent{assistant_message}));

  append_assistant_content_rpc_updates(events, assistant_message,
                                       response->message);

  events.push_back(qt::app::RpcEvent(qt::app::MessageEndEvent{assistant_message}));
  events.push_back(
      qt::app::RpcEvent(qt::app::TurnEndEvent{assistant_message, Json::array()}));
  events.push_back(
      qt::app::RpcEvent(qt::app::AgentEndEvent{Json::array({assistant_message})}));
  return events;
}

}  // namespace

int run_rpc_mode(const ReferenceCliArgs& parsed,
                           const ReferenceStartupSession& startup_session,
                           const std::string& cwd,
                           const std::string& agent_home) {
  auto log = qt::foundation::logger("app", "rpc_mode");
  log.info("mode_begin",
           {qt::foundation::log_field("cwd", cwd),
            qt::foundation::log_field("agent_home", agent_home)});
  auto built = make_rpc_runtime(parsed, startup_session);
  if (!built) {
    log.error("runtime_create_failed",
              {qt::foundation::log_field("error", built.error().message)});
    std::cerr << "qt: " << built.error().message << '\n';
    return EXIT_FAILURE;
  }
  auto runtime = std::make_shared<ReferenceRpcRuntime>(std::move(built.value()));
  const std::string parent_session_id =
      runtime->host.session_state().metadata().session_id;
  const auto subagent_directory =
      std::filesystem::path(agent_home) / "subagents" /
      rpc_subagent_namespace(parent_session_id);
  qt::app::runtime::SubagentCoordinatorOptions coordinator_options;
  coordinator_options.directory = subagent_directory;
  coordinator_options.parent_session_id = parent_session_id;
  coordinator_options.parent_path = "/root";
  coordinator_options.runner.executable = rpc_child_executable();
  runtime->subagent_coordinator =
      std::make_shared<qt::app::runtime::SubagentCoordinator>(
          std::move(coordinator_options));
  const auto child_cwd = runtime->startup_session.working_directory.value_or(cwd);
  runtime->subagent_tool = std::make_unique<qt::tools::SubagentTool>(
      qt::tools::SubagentToolOptions{
          std::filesystem::path(agent_home), std::filesystem::path(child_cwd),
          rpc_child_executable(), {}, 8U, 4U, 2U,
          runtime->subagent_coordinator});
  {
    const std::weak_ptr<ReferenceRpcRuntime> weak_runtime = runtime;
    runtime->subagent_coordinator->set_context_snapshot(
        [weak_runtime]() {
          const auto runtime = weak_runtime.lock();
          return runtime ? runtime->host.harness().agent().state().transcript
                         : std::vector<qt::ai::Message>{};
        });
  }
  std::mutex output_mutex;
  const auto attach_subagent_events = [&output_mutex](
      const std::shared_ptr<qt::app::runtime::SubagentCoordinator>& coordinator) {
    const std::weak_ptr<qt::app::runtime::SubagentCoordinator> weak = coordinator;
    const auto deliver = [&output_mutex, weak](const Json& event) {
      const Json frame{{"type", "subagent_event"}, {"subagent", event}};
      {
        std::lock_guard<std::mutex> lock(output_mutex);
        std::cout << frame.dump() << '\n' << std::flush;
        if (!std::cout) return;
      }
      if (event.value("type", "") == "subagent_complete")
        if (const auto owner = weak.lock())
          (void)owner->acknowledge_event(event.value("child_id", ""), event.value("event_id", 0ULL));
    };
    coordinator->set_event_sink(deliver);
    for (const auto& event : coordinator->take_events()) deliver(event);
  };
  const auto reset_subagents = [runtime, agent_home, cwd,
                                attach_subagent_events]() {
    if (runtime->subagent_coordinator) runtime->subagent_coordinator->shutdown();
    const std::string session_id =
        runtime->host.session_state().metadata().session_id;
    qt::app::runtime::SubagentCoordinatorOptions options;
    options.directory = std::filesystem::path(agent_home) / "subagents" /
                        rpc_subagent_namespace(session_id);
    options.parent_session_id = session_id;
    options.parent_path = "/root";
    options.runner.executable = rpc_child_executable();
    runtime->subagent_coordinator =
        std::make_shared<qt::app::runtime::SubagentCoordinator>(std::move(options));
    runtime->subagent_tool = std::make_unique<qt::tools::SubagentTool>(
        qt::tools::SubagentToolOptions{std::filesystem::path(agent_home),
                                       std::filesystem::path(cwd),
                                       rpc_child_executable(), {}, 8U, 4U, 2U,
                                       runtime->subagent_coordinator});
    const std::weak_ptr<ReferenceRpcRuntime> weak_runtime = runtime;
    runtime->subagent_coordinator->set_context_snapshot(
        [weak_runtime]() {
          const auto runtime = weak_runtime.lock();
          return runtime ? runtime->host.harness().agent().state().transcript
                         : std::vector<qt::ai::Message>{};
        });
    attach_subagent_events(runtime->subagent_coordinator);
  };
  log.info("runtime_ready",
           {qt::foundation::log_field(
                "provider", runtime->config.model.provider),
            qt::foundation::log_field("model", runtime->config.model.name),
            qt::foundation::log_field(
                "tools",
                static_cast<std::uint64_t>(
                    runtime->config.tools.size()))});
  if (!runtime->host.seed_state().restored_thinking_level) {
    runtime->host.harness().set_thinking_level(
        configured_thinking_level(runtime->config));
  }
  auto bash_state = std::make_shared<ReferenceRpcBashState>();

  qt::app::RpcServerHandlers handlers;
  handlers.subagent = [runtime, parsed, cwd](const qt::app::SubagentCommand& command) {
    using qt::foundation::ErrorCode;
    using qt::foundation::Result;
    const bool enabled = !parsed.no_tools && (parsed.tools.empty() ||
        std::any_of(parsed.tools.begin(), parsed.tools.end(), [](const std::string& pattern) {
          return ::fnmatch(pattern.c_str(), "subagent", 0) == 0;
        }));
    if (!enabled || !runtime->subagent_tool) {
      return Result<Json>::failure(ErrorCode::invalid_argument,
                                   "subagent tool is disabled for this runtime");
    }
    qt::tools::ToolRequest request;
    request.tool_name = "subagent";
    request.arguments = command.arguments;
    request.call_id = command.id;
    auto context = runtime->config.tool_context;
    context.metadata["active_tools"] = Json::array();
    const auto registry = make_builtin_tool_registry(parsed.tools);
    for (const auto& tool : build_tool_definitions(*registry))
      context.metadata["active_tools"].push_back(tool.name);
    context.metadata["active_tools"].push_back("subagent");
    const auto& parent_model = runtime->host.harness().model();
    context.metadata["model"] = Json{{"provider", parent_model.provider},
                                      {"id", parent_model.name}};
    const auto result = runtime->subagent_tool->invoke(request, context);
    if (result.status != qt::tools::ToolResultStatus::success) {
      return Result<Json>::failure(ErrorCode::internal_error,
                                   result.error_message.value_or(
                                       "subagent request failed"));
    }
    return Result<Json>::success(result.output);
  };
  handlers.memory = [runtime](const qt::app::MemoryCommand& command) {
    using qt::foundation::ErrorCode;
    using qt::foundation::Result;
    const auto memory = runtime->memory_runtime;
    try {
      if (command.action == "status") {
      Json result{
          {"enabled", memory != nullptr},
          {"configured", runtime->memory_settings.enabled},
          {"disabled_by_cli", runtime->memory_disabled_by_cli},
          {"automatic_recall", runtime->memory_settings.automatic_recall},
          {"automatic_capture", runtime->memory_settings.automatic_capture},
          {"dreaming",
           Json{{"enabled", memory != nullptr &&
                                runtime->memory_settings.dreaming.enabled},
                {"configured",
                 runtime->memory_settings.dreaming.enabled},
                {"trigger", runtime->memory_settings.dreaming.trigger},
                {"interval_seconds",
                 runtime->memory_settings.dreaming.interval_seconds},
                {"idle_seconds",
                 runtime->memory_settings.dreaming.idle_seconds},
                {"candidate_threshold",
                 runtime->memory_settings.dreaming.candidate_threshold},
                {"model", runtime->memory_settings.dreaming.model
                              ? Json(*runtime->memory_settings.dreaming.model)
                              : Json(nullptr)},
                {"verifier_model", runtime->memory_settings.dreaming.verifier_model
                                       ? Json(*runtime->memory_settings.dreaming.verifier_model)
                                       : Json(nullptr)},
                {"timeout_ms",
                 runtime->memory_settings.dreaming.timeout_ms},
                {"max_model_input_tokens",
                 runtime->memory_settings.dreaming.max_model_input_tokens},
                {"max_candidates",
                 runtime->memory_settings.dreaming.max_candidates},
                {"research_policy",
                 runtime->memory_settings.dreaming.research_policy},
                {"promotion_policy",
                 runtime->memory_settings.dreaming.promotion_policy}}},
          {"database",
           (std::filesystem::path(runtime->agent_home) / "memory" /
            "memory.sqlite3")
               .generic_string()},
          {"prehandler",
           memory != nullptr && runtime->memory_settings.automatic_recall
               ? "isolated_child_with_deterministic_fallback"
               : "inactive"},
          {"warnings", Json::array()}};
      if (!runtime->memory_initialization_error.empty()) {
        result["initialization_error"] = runtime->memory_initialization_error;
        result["warnings"].push_back(
            "memory initialization failed open: " +
            runtime->memory_initialization_error);
      }
      if (memory) {
        const auto& scope = memory->scope();
        result["schema_version"] = memory->schema_version();
        const auto statistics = memory->statistics();
        result["scope_memory_count"] = statistics.memory_entries;
        result["statistics"] = memory_statistics_json(statistics);
        result["review_statistics"] =
            qt::memory::review_statistics_json(memory->review_statistics());
        const auto retrieval = memory->retrieval_status();
        result["retrieval"] =
            Json{{"enabled", retrieval.enabled},
                 {"running", retrieval.running},
                 {"generation", retrieval.generation},
                 {"last_error", retrieval.last_error},
                 {"completed_jobs", retrieval.completed_jobs},
                 {"jobs", Json{{"pending", retrieval.jobs.pending},
                                {"leased", retrieval.jobs.leased},
                                {"failed", retrieval.jobs.failed},
                                {"complete", retrieval.jobs.complete},
                                {"chunks", retrieval.jobs.chunks}}},
                 {"queries", Json{{"cache_hits", retrieval.queries.cache_hits},
                                   {"embedding_failures",
                                    retrieval.queries.embedding_failures},
                                   {"cache_entries", retrieval.queries.cache_entries},
                                   {"cache_bytes", retrieval.queries.cache_bytes},
                                   {"last_error", retrieval.queries.last_error}}},
                 {"code", Json{{"enabled", retrieval.code.enabled},
                                {"available", retrieval.code.available},
                                {"stale", retrieval.code.stale},
                                {"generation", retrieval.code.generation},
                                {"repository", retrieval.code.repository},
                                {"worktree", retrieval.code.worktree}}},
                 {"configured", Json{{"graph_enabled",
                                      runtime->memory_settings.retrieval.graph_enabled},
                                     {"dense_enabled",
                                      runtime->memory_settings.retrieval.dense_enabled},
                                     {"code_enabled",
                                      runtime->memory_settings.retrieval.code_enabled},
                                     {"query_cache_entries",
                                      runtime->memory_settings.retrieval.query_cache_entries},
                                     {"max_depth",
                                      runtime->memory_settings.retrieval.max_depth},
                                     {"max_seeds",
                                      runtime->memory_settings.retrieval.max_seeds},
                                     {"max_edges",
                                      runtime->memory_settings.retrieval.max_edges},
                                     {"max_graph_results",
                                      runtime->memory_settings.retrieval.max_graph_results},
                                     {"max_fanout",
                                      runtime->memory_settings.retrieval.max_fanout}}}};
        result["scope"] = Json{{"user", scope.user},
                                {"project", scope.project},
                                {"repository", scope.repository},
                                {"repository_display", scope.repository_display},
                                {"branch", scope.branch},
                                {"session", scope.session}};
        const auto dreaming = memory->dreaming_status();
        result["dreaming"]["runtime"] =
            Json{{"running", dreaming.running},
                 {"idle_seconds", dreaming.idle_seconds},
                 {"idle_for_seconds", dreaming.idle_for_seconds},
                 {"waiting_for_idle", dreaming.waiting_for_idle},
                 {"pending", dreaming.pending_runs},
                 {"completed", dreaming.completed_runs},
                 {"failed", dreaming.failed_runs},
                 {"last_error", dreaming.last_error},
                 {"last_dream_id", dreaming.last_dream_id},
                 {"last_review_id", dreaming.last_review_id}};
      } else {
        result["statistics"] = nullptr;
      }
      return Result<Json>::success(std::move(result));
      }
      if (!memory) {
        return Result<Json>::failure(
            ErrorCode::invalid_argument,
            "built-in memory is disabled for this RPC runtime");
      }
      if (command.action == "dream") {
        const std::string operation =
            command.params.value("operation", std::string("status"));
        if (operation == "status") {
          const auto status = memory->dreaming_status();
          return Result<Json>::success(
              Json{{"enabled", status.enabled},
                   {"trigger", status.trigger},
                   {"idle_seconds", status.idle_seconds},
                   {"idle_for_seconds", status.idle_for_seconds},
                   {"waiting_for_idle", status.waiting_for_idle},
                   {"running", status.running},
                   {"pending", status.pending_runs},
                   {"completed", status.completed_runs},
                   {"failed", status.failed_runs},
                   {"last_error", status.last_error},
                   {"last_dream_id", status.last_dream_id},
                   {"last_review_id", status.last_review_id}});
        }
        if (operation == "run") {
          const bool queued = memory->enqueue_dream_cycle("manual");
          if (!queued) {
            return Result<Json>::failure(
                ErrorCode::invalid_argument,
                "Dreaming cycle was not queued; enable Dreaming and verify its queue/model settings");
          }
          return Result<Json>::success(Json{{"queued", true}});
        }
        if (operation == "research") {
          if (!command.params.contains("dream_id") ||
              !command.params.at("dream_id").is_string() ||
              !command.params.contains("summary") ||
              !command.params.at("summary").is_string()) {
            throw std::invalid_argument(
                "memory dream research requires string dream_id and summary");
          }
          qt::memory::DreamResearchEvidence evidence;
          evidence.summary = command.params.at("summary").get<std::string>();
          evidence.source_memory_ids = command.params.value(
              "source_memory_ids", std::vector<std::string>{});
          evidence.tags =
              command.params.value("tags", std::vector<std::string>{});
          evidence.confidence = command.params.value("confidence", 0.7);
          evidence.provenance.source_type =
              qt::memory::source_type_from_string(command.params.value(
                  "source_type", std::string("document")));
          evidence.provenance.source_uri =
              command.params.value("source_uri", std::string{});
          evidence.provenance.source_hash =
              command.params.value("source_hash", std::string{});
          evidence.provenance.source_span =
              command.params.value("source_span", std::string{});
          evidence.provenance.created_by = "user:rpc";
          evidence.provenance.evidence_strength =
              command.params.value("evidence_strength", 0.7);
          const auto stored = memory->add_dream_research(
              command.params.at("dream_id").get<std::string>(),
              std::move(evidence));
          return Result<Json>::success(
              Json{{"dream_id", stored.id},
                   {"research_id", stored.researched_memory_id
                                       ? Json(*stored.researched_memory_id)
                                       : Json(nullptr)},
                   {"research_review_id", stored.research_review_id
                                              ? Json(*stored.research_review_id)
                                              : Json(nullptr)}});
        }
        throw std::invalid_argument(
            "memory dream operation must be status, run, or research");
      }
      if (command.action == "recall" || command.action == "explain") {
        if (!command.params.contains("query") ||
            !command.params.at("query").is_string() ||
            command.params.at("query").get<std::string>().empty()) {
          throw std::invalid_argument(
              "memory recall requires non-empty string params.query");
        }
        qt::memory::RecallRequest request;
        request.query = command.params.at("query").get<std::string>();
        const auto configured_policy = runtime->memory_settings.recall_policy;
        if (configured_policy == "strict") request.policy = qt::memory::RecallPolicy::Strict;
        else if (configured_policy == "exploratory" || configured_policy == "broad")
          request.policy = qt::memory::RecallPolicy::Broad;
        else if (configured_policy == "balanced") request.policy = qt::memory::RecallPolicy::Balanced;
        else throw std::invalid_argument("memory recall has invalid configured recall policy");
        if (command.params.contains("include_global_user") &&
            !command.params.at("include_global_user").is_boolean())
          throw std::invalid_argument("memory recall include_global_user must be boolean");
        request.include_global_user = command.params.value("include_global_user", false);
        request.graph.enabled = runtime->memory_settings.retrieval.graph_enabled;
        request.graph.max_depth = runtime->memory_settings.retrieval.max_depth;
        request.graph.max_seeds = runtime->memory_settings.retrieval.max_seeds;
        request.graph.max_edges = runtime->memory_settings.retrieval.max_edges;
        request.graph.max_fanout = runtime->memory_settings.retrieval.max_fanout;
        request.graph.max_results = runtime->memory_settings.retrieval.max_graph_results;
        request.dense_enabled = runtime->memory_settings.retrieval.dense_enabled;
        for (const auto* key : {"limit", "token_budget"})
          if (command.params.contains(key) && !command.params.at(key).is_number_integer())
            throw std::invalid_argument(std::string("memory recall ") + key + " must be an integer");
        request.result_limit = command.params.value("limit", std::size_t{10});
        if (request.result_limit == 0U || request.result_limit > 50U) {
          throw std::invalid_argument("memory recall limit must be from 1 to 50");
        }
        request.candidate_limit =
            std::max<std::size_t>(20U, request.result_limit);
        const auto structured_string = [&](const char* key) {
          if (!command.params.contains(key)) return std::string{};
          if (!command.params.at(key).is_string())
            throw std::invalid_argument(std::string("memory recall ") + key + " must be a string");
          return command.params.at(key).get<std::string>();
        };
        request.structured.subject = structured_string("subject");
        request.structured.predicate = structured_string("predicate");
        request.structured.object = structured_string("object");
        if (!request.structured.subject.empty() || !request.structured.predicate.empty() ||
            !request.structured.object.empty()) {
          request.structured.enabled = true;
          request.structured.limit = std::min<std::size_t>(100U, request.candidate_limit);
        }
        if (command.params.contains("as_of")) {
          if (!command.params.at("as_of").is_number_integer())
            throw std::invalid_argument("memory recall as_of must be epoch seconds");
          if (command.params.at("as_of").is_number_unsigned() &&
              command.params.at("as_of").get<std::uint64_t>() > static_cast<std::uint64_t>(
                  std::chrono::duration_cast<std::chrono::seconds>(qt::memory::TimePoint::duration::max()).count()))
            throw std::invalid_argument("as_of is outside supported time range");
          const auto seconds = command.params.at("as_of").get<std::int64_t>();
          const auto max_seconds = std::chrono::duration_cast<std::chrono::seconds>(
              qt::memory::TimePoint::duration::max()).count();
          const auto min_seconds = std::chrono::duration_cast<std::chrono::seconds>(
              qt::memory::TimePoint::duration::min()).count();
          if (seconds < min_seconds || seconds > max_seconds)
            throw std::invalid_argument("memory recall as_of is outside supported time range");
          request.structured.as_of = qt::memory::TimePoint{
              std::chrono::seconds(seconds)};
          request.structured.enabled = true;
        }
        std::optional<qt::memory::RetrievalPreview> preview;
        const auto token_budget = command.params.value(
            "token_budget", runtime->memory_settings.context_token_budget);
        if (command.action == "explain")
          preview = memory->preview_retrieval(request, token_budget);
        const auto recalled = preview ? preview->memories
            : memory->recall().recall(memory->scope(), request);
        if (!preview) memory->recall().record_access(memory->scope(), recalled);
        Json values = Json::array();
        for (const auto& item : recalled)
          values.push_back(qt::memory::recalled_memory_json(item,
              item.historical ? std::nullopt : memory->current_retrieval_revision(
                  item.memory.id, request.policy, request.include_global_user),
              preview ? &preview->context : nullptr));
        Json result{{"memories", std::move(values)}, {"count", recalled.size()}};
        if (preview) {
          result["code_candidates"] = Json::array();
          for (const auto& item : preview->code)
            result["code_candidates"].push_back(
                qt::memory::retrieval_candidate_json(item, &preview->context));
          result["context"] = qt::memory::context_preview_json(preview->context, token_budget);
          result["requested_modes"] = Json{
              {"lexical", true},
              {"graph", request.graph.enabled && !request.structured.as_of},
              {"dense", request.dense_enabled && !request.structured.as_of},
              {"code", runtime->memory_settings.retrieval.code_enabled && !request.structured.as_of}};
          std::vector<std::string> observed;
          const auto note_retriever = [&](const std::string& name) {
            if (std::find(observed.begin(), observed.end(), name) == observed.end()) observed.push_back(name);
          };
          for (const auto& item : preview->memories) {
            if (item.score.lexical_rank) note_retriever("lexical");
            if (item.score.graph_rank) note_retriever("graph");
            if (item.score.dense_rank) note_retriever("dense");
          }
          for (const auto& item : preview->code)
            for (const auto& rank : item.ranks) note_retriever(rank.retriever);
          result["observed_retrievers"] = observed;
          const auto retrieval_status = memory->retrieval_status();
          result["dense_generation"] = retrieval_status.generation;
          result["retrieval_warnings"] = Json::array();
          for (const auto& warning : {retrieval_status.last_error, retrieval_status.queries.last_error})
            if (!warning.empty()) result["retrieval_warnings"].push_back(warning);
        }
        return Result<Json>::success(std::move(result));
      }

      if (command.action == "retrieval_rebuild") {
        if (!command.confirm)
          throw std::invalid_argument("memory retrieval_rebuild requires confirm=true");
        std::optional<std::string> owner_key;
        if (command.params.contains("owner_key")) {
          if (!command.params.at("owner_key").is_string())
            throw std::invalid_argument("memory retrieval_rebuild owner_key must be a string");
          owner_key = command.params.at("owner_key").get<std::string>();
        }
        const auto report = memory->rebuild_retrieval(std::move(owner_key));
        return Result<Json>::success(Json{{"owners", report.owners},
                                          {"chunks_invalidated", report.chunks_invalidated},
                                          {"vectors_invalidated", report.vectors_invalidated},
                                          {"jobs_superseded", report.jobs_superseded},
                                          {"jobs_enqueued", report.jobs_enqueued}});
      }
      if (command.action == "remember") {
        if (!command.confirm) {
          throw std::invalid_argument("memory remember requires confirm=true");
        }
        if (!command.params.contains("content") ||
            !command.params.at("content").is_string() ||
            command.params.at("content").get<std::string>().empty()) {
          throw std::invalid_argument("memory remember requires non-empty params.content");
        }
        const std::string status =
            command.params.value("status", std::string("hypothesis"));
        if (status != "candidate" && status != "hypothesis") {
          throw std::invalid_argument(
              "memory remember status must be candidate or hypothesis");
        }
        qt::memory::MemoryWrite write;
        write.content = command.params.at("content").get<std::string>();
        write.type = qt::memory::memory_type_from_string(
            command.params.value("memory_type", std::string("observation")));
        write.status = qt::memory::memory_status_from_string(status);
        write.source = "user:rpc";
        write.provenance.source_type = qt::memory::SourceType::Manual;
        write.provenance.created_by = "user";
        const auto id = memory->memories().remember(memory->scope(), write);
        return Result<Json>::success(
            Json{{"id", id}, {"status", status}, {"created", true}});
      }
      if (command.action == "review") {
        try {
        if (command.params.contains("action") &&
            !command.params.at("action").is_string()) {
          throw std::invalid_argument("memory review action must be a string");
        }
        const std::string action =
            command.params.value("action", std::string("list"));
        if (action == "list" || action == "next") {
          auto query = qt::memory::review_query_from_json(command.params);
          if (query.limit > 100U) {
            throw std::invalid_argument("memory review limit must be from 1 to 100");
          }
          if (action == "next") query.limit = 1U;
          const auto reviews = memory->review_items(query);
          Json items = Json::array();
          for (const auto& review : reviews) {
            items.push_back(qt::memory::review_item_json(review));
          }
          return Result<Json>::success(
              Json{{"items", std::move(items)}, {"count", reviews.size()}});
        }
        if (action == "show") {
          if (!command.params.contains("id") ||
              !command.params.at("id").is_string()) {
            throw std::invalid_argument("memory review show requires string id");
          }
          return Result<Json>::success(qt::memory::hydrated_review_json(
              memory->review_detail(command.params.at("id").get<std::string>())));
        }
        if (action == "history") {
          std::optional<std::string> id;
          if (command.params.contains("id")) {
            if (!command.params.at("id").is_string()) {
              throw std::invalid_argument("memory review history id must be a string");
            }
            id = command.params.at("id").get<std::string>();
          }
          const auto limit = command.params.value("limit", std::size_t{20});
          Json items = Json::array();
          for (const auto& record : memory->review_history(id, limit)) {
            items.push_back(qt::memory::review_action_record_json(record));
          }
          return Result<Json>::success(Json{{"items", std::move(items)}});
        }
        if (action == "preview") {
          if (!command.params.contains("id") ||
              !command.params.at("id").is_string()) {
            throw std::invalid_argument("memory review preview requires string id");
          }
          Json input = command.params;
          input["action"] = command.params.value(
              "review_action", command.params.value("decision", std::string{}));
          auto review_action = qt::memory::review_action_from_json(
              input, "user:rpc-preview", false);
          return Result<Json>::success(qt::memory::review_preview_json(
              memory->preview_review(command.params.at("id").get<std::string>(),
                                     std::move(review_action))));
        }
        if (action == "decide" || action == "defer" || action == "reopen") {
          if (!command.confirm || !command.params.contains("id") ||
              !command.params.at("id").is_string() ||
              !command.params.contains("expected_revision") ||
              !command.params.contains("idempotency_key")) {
            throw std::invalid_argument(
                "memory review mutation requires id, preview revision, "
                "idempotency key, and confirm=true");
          }
          Json input = command.params;
          input["action"] = action == "decide"
                                ? command.params.value(
                                      "review_action",
                                      command.params.value("decision", std::string{}))
                                : action;
          auto review_action = qt::memory::review_action_from_json(
              input, "user:rpc", true);
          return Result<Json>::success(qt::memory::review_application_json(
              memory->apply_review_action(
                  command.params.at("id").get<std::string>(),
                  std::move(review_action))));
        }
        if (action == "batch_status") {
          if (!command.params.contains("job_id") ||
              !command.params.at("job_id").is_string()) {
            throw std::invalid_argument(
                "memory review batch_status requires string job_id");
          }
          return Result<Json>::success(qt::memory::review_batch_job_json(
              memory->review_batch_job(
                  command.params.at("job_id").get<std::string>())));
        }
        if (action == "preview_batch" || action == "batch" ||
            action == "batch_start") {
          if (!command.params.contains("items") ||
              !command.params.at("items").is_array()) {
            throw std::invalid_argument(
                "memory review batch requires an items array");
          }
          if ((action == "batch" || action == "batch_start") &&
              !command.confirm) {
            throw std::invalid_argument(
                "memory review batch apply requires confirm=true");
          }
          std::vector<qt::memory::ReviewBatchItem> items;
          for (const auto& item : command.params.at("items")) {
            if (!item.is_object() || !item.contains("id") ||
                !item.at("id").is_string()) {
              throw std::invalid_argument("batch item requires string id");
            }
            qt::memory::ReviewBatchItem batch_item;
            batch_item.review_id = item.at("id").get<std::string>();
            batch_item.action = qt::memory::review_action_from_json(
                item, action == "preview_batch" ? "user:rpc-preview"
                                                  : "user:rpc",
                action != "preview_batch");
            items.push_back(std::move(batch_item));
          }
          if (action == "preview_batch") {
            return Result<Json>::success(qt::memory::review_batch_preview_json(
                memory->preview_review_batch(items)));
          }
          if (action == "batch_start") {
            return Result<Json>::success(qt::memory::review_batch_job_json(
                memory->start_review_batch(
                    std::move(items),
                    command.params.value("idempotency_token", std::string{}))));
          }
          return Result<Json>::success(qt::memory::review_batch_result_json(
              memory->apply_review_batch(
                  std::move(items),
                  command.params.value("idempotency_token", std::string{}))));
        }
        throw std::invalid_argument(
            "memory review action must be list, show, next, preview, decide, "
            "defer, reopen, history, preview_batch, batch_start, batch_status, "
            "or batch");
        } catch (const std::invalid_argument& error) {
          throw std::invalid_argument(review_rpc_error(error));
        } catch (const nlohmann::json::exception& error) {
          throw std::invalid_argument(
              "rpc_error:invalid_review_schema:" + std::string(error.what()));
        }
      }
      if (command.action == "feedback") {
        if (!command.confirm || !command.params.contains("id") ||
            !command.params.contains("outcome")) {
          throw std::invalid_argument(
              "memory feedback requires id, outcome, and confirm=true");
        }
        qt::memory::ProcedureFeedback feedback;
        feedback.procedure_id = command.params.at("id").get<std::string>();
        feedback.outcome = qt::memory::procedure_outcome_from_string(
            command.params.at("outcome").get<std::string>());
        if (feedback.outcome == qt::memory::ProcedureOutcome::Unknown) {
          throw std::invalid_argument(
              "memory feedback outcome must be success, failure, partial, or unused");
        }
        feedback.task_id = command.params.value("task_id", std::string{});
        feedback.notes = command.params.value("notes", std::string{});
        const auto recorded = memory->record_feedback(std::move(feedback));
        return Result<Json>::success(
            Json{{"run_id", recorded.first.id},
                 {"memory_id", recorded.second.id}});
      }
      if (command.action == "forget") {
        if (!command.confirm || !command.params.contains("id") ||
            !command.params.at("id").is_string() ||
            command.params.at("id").get<std::string>().empty()) {
          throw std::invalid_argument(
              "memory forget requires string id and confirm=true");
        }
        const auto id = command.params.at("id").get<std::string>();
        const bool removed = memory->memories().forget(memory->scope(), id);
        return Result<Json>::success(
            Json{{"id", id}, {"forgotten", removed}});
      }
      return Result<Json>::failure(ErrorCode::invalid_argument,
                                   "unsupported memory RPC action");
    } catch (const std::invalid_argument& error) {
      return Result<Json>::failure(ErrorCode::invalid_argument, error.what());
    } catch (const std::exception& error) {
      return Result<Json>::failure(ErrorCode::io_error, error.what());
    }
  };
  handlers.abort = [runtime](const qt::app::AbortCommand&) {
    (void)runtime;
    qt::app::RpcServerCommandResult result;
    return qt::foundation::Result<qt::app::RpcServerCommandResult>::success(
        std::move(result));
  };
  handlers.get_state = [runtime](const qt::app::GetStateCommand&) {
    return qt::foundation::Result<qt::app::RpcSessionState>::success(
        make_rpc_state(*runtime));
  };
  handlers.get_messages = [runtime](const qt::app::GetMessagesCommand&) {
    return qt::foundation::Result<Json>::success(
        make_rpc_messages(*runtime));
  };
  handlers.new_session =
      [runtime, parsed, cwd, agent_home, reset_subagents](const qt::app::NewSessionCommand& command) {
        const std::string session_dir =
            rpc_session_dir(parsed, cwd, agent_home);
        const auto next_session =
            command.parent_session
                ? fork_startup_session(*command.parent_session,
                                                 session_dir)
                : create_startup_session(session_dir, "create");
        if (!next_session) {
          return qt::foundation::Result<Json>::failure(
              next_session.error().code, next_session.error().message,
              next_session.error().system_error);
        }

        const auto loaded = load_rpc_runtime_session(
            *runtime, next_session.value(), "new");
        if (!loaded.ok()) {
          return qt::foundation::Result<Json>::failure(
              loaded.error().code, loaded.error().message,
              loaded.error().system_error);
        }
        reset_subagents();
        return qt::foundation::Result<Json>::success(Json{{"cancelled", false}});
      };
  handlers.switch_session =
      [runtime, reset_subagents](const qt::app::SwitchSessionCommand& command) {
        const auto next_session =
            open_startup_session(command.session_path, "switch");
        if (!next_session) {
          return qt::foundation::Result<Json>::failure(
              next_session.error().code, next_session.error().message,
              next_session.error().system_error);
        }

        const auto loaded = load_rpc_runtime_session(
            *runtime, next_session.value(), "resume");
        if (!loaded.ok()) {
          return qt::foundation::Result<Json>::failure(
              loaded.error().code, loaded.error().message,
              loaded.error().system_error);
        }
        reset_subagents();
        return qt::foundation::Result<Json>::success(
            Json{{"cancelled", false}});
      };
  handlers.prompt = [runtime](const qt::app::PromptCommand& command) {
    const bool queue_as_steer =
        command.command == qt::app::RpcCommandKind::kSteer ||
        (command.command == qt::app::RpcCommandKind::kPrompt &&
         command.streaming_behavior == qt::app::RpcStreamingBehavior::kSteer);
    const bool queue_as_follow_up =
        command.command == qt::app::RpcCommandKind::kFollowUp ||
        (command.command == qt::app::RpcCommandKind::kPrompt &&
         command.streaming_behavior ==
             qt::app::RpcStreamingBehavior::kFollowUp);
    if (queue_as_steer || queue_as_follow_up) {
      qt::app::RpcServerCommandResult result;
      const auto message = rpc_prompt_message(command);
      if (queue_as_steer) {
        runtime->pending_steering.push_back(message);
      } else {
        runtime->pending_follow_up.push_back(message);
      }
      result.events.push_back(
          qt::app::RpcEvent(make_rpc_queue_update_event(*runtime)));
      return qt::foundation::Result<qt::app::RpcServerCommandResult>::success(
          std::move(result));
    }
    qt::app::RpcServerCommandResult result;
    std::vector<qt::ai::Message> flushed_steering_messages;
    flushed_steering_messages.reserve(runtime->pending_steering.size());
    std::vector<qt::ai::Message> flushed_follow_up_messages =
        std::move(runtime->pending_follow_up);
    runtime->pending_follow_up.clear();
    runtime->host.harness().clear_events();
    for (const auto& queued : runtime->pending_steering) {
      runtime->host.harness().next_turn(queued);
      flushed_steering_messages.push_back(queued);
    }
    runtime->pending_steering.clear();

    const auto append_turn = [runtime, &result](
                                 const qt::ai::Message& message,
                                 const qt::agent::TurnResult& turn)
        -> qt::foundation::Status {
      const auto appended_user = append_rpc_upstream_payload(
          *runtime, Json{{"type", "message"}, {"message", Json(message)}});
      if (!appended_user.ok()) {
        return appended_user;
      }
      ++runtime->session_stats.user_messages;
      ++runtime->session_stats.total_messages;

      const auto* response = selected_turn_response(turn);
      if (response != nullptr) {
        const auto appended_assistant = append_rpc_upstream_payload(
            *runtime, Json{{"type", "message"},
                           {"message", assistant_message_json(turn, *response)}});
        if (!appended_assistant.ok()) {
          return appended_assistant;
        }

        runtime->session_stats.tokens.input += response->usage.input_tokens;
        runtime->session_stats.tokens.output +=
            response->usage.output_tokens;
        runtime->session_stats.tokens.cache_read +=
            response->usage.cache_read_tokens;
        runtime->session_stats.tokens.cache_write +=
            response->usage.cache_write_tokens;
        runtime->session_stats.tokens.total += response->usage.total_tokens;
        runtime->session_stats.cost += response->usage.total_cost_usd;
        ++runtime->session_stats.assistant_messages;
        ++runtime->session_stats.total_messages;
        const auto model_key = runtime->host.harness().model().provider + "/" +
                               runtime->host.harness().model().name;
        auto& model_stats = runtime->session_stats.models[model_key];
        ++model_stats.assistant_messages;
        model_stats.tokens.input += response->usage.input_tokens;
        model_stats.tokens.output += response->usage.output_tokens;
        model_stats.tokens.cache_read += response->usage.cache_read_tokens;
        model_stats.tokens.cache_write +=
            response->usage.cache_write_tokens;
        model_stats.tokens.total += response->usage.total_tokens;
        model_stats.cost += response->usage.total_cost_usd;
        runtime->session_stats.tool_calls += turn.tool_requests.size();
        runtime->session_stats.tool_results += turn.tool_results.size();
        runtime->session_stats.total_messages += turn.tool_results.size();
        if (const auto context_window =
                rpc_context_window(runtime->host.harness().model())) {
          qt::harness::SessionContextUsage usage;
          usage.context_window = *context_window;
          usage.tokens = response->usage.input_tokens +
                         response->usage.cache_read_tokens +
                         response->usage.cache_write_tokens;
          if (*context_window > 0) {
            usage.percent =
                (static_cast<double>(*usage.tokens) /
                 static_cast<double>(*context_window)) *
                100.0;
          }
          runtime->session_stats.context_usage = usage;
        }
      }

      auto turn_events = make_rpc_turn_events(turn);
      result.events.insert(result.events.end(), turn_events.begin(),
                           turn_events.end());
      return qt::foundation::Status::success();
    };

    for (const auto& queued : flushed_steering_messages) {
      const auto appended_queued = append_rpc_upstream_payload(
          *runtime, Json{{"type", "message"}, {"message", Json(queued)}});
      if (!appended_queued.ok()) {
        return qt::foundation::Result<qt::app::RpcServerCommandResult>::failure(
            appended_queued.error().code, appended_queued.error().message,
            appended_queued.error().system_error);
      }
    }

    const auto message = rpc_prompt_message(command);
    const auto turn = runtime->host.harness().submit_prompt(message);
    if (!turn.ok) {
      return qt::foundation::Result<qt::app::RpcServerCommandResult>::failure(
          qt::foundation::ErrorCode::internal_error,
          turn.error.value_or("rpc prompt failed"));
    }
    const auto appended_turn = append_turn(message, turn);
    if (!appended_turn.ok()) {
      return qt::foundation::Result<qt::app::RpcServerCommandResult>::failure(
          appended_turn.error().code, appended_turn.error().message,
          appended_turn.error().system_error);
    }

    for (const auto& follow_up : flushed_follow_up_messages) {
      const auto follow_up_turn = runtime->host.harness().submit_prompt(follow_up);
      if (!follow_up_turn.ok) {
        return qt::foundation::Result<qt::app::RpcServerCommandResult>::failure(
            qt::foundation::ErrorCode::internal_error,
            follow_up_turn.error.value_or("rpc follow-up failed"));
      }
      const auto appended_follow_up = append_turn(follow_up, follow_up_turn);
      if (!appended_follow_up.ok()) {
        return qt::foundation::Result<qt::app::RpcServerCommandResult>::failure(
            appended_follow_up.error().code, appended_follow_up.error().message,
            appended_follow_up.error().system_error);
      }
    }
    return qt::foundation::Result<qt::app::RpcServerCommandResult>::success(
        std::move(result));
  };
  handlers.set_model = [runtime](const qt::app::SetModelCommand& command) {
    const auto catalog = rpc_model_catalog();
    const auto* model = qt::ai::find_model_record(
        catalog, command.provider, command.model_id);
    if (model == nullptr) {
      return qt::foundation::Result<Json>::failure(
          qt::foundation::ErrorCode::invalid_argument,
          "Model not found: " + command.provider + "/" + command.model_id);
    }

    const auto previous_entry_count =
        runtime->host.session_manager().entries().size();
    runtime->config.model =
        qt::ai::ModelRef{model->ref.provider, model->ref.api, model->ref.name};
    runtime->host.harness().set_model(runtime->config.model);
    refresh_rpc_context_usage(*runtime);
    const auto appended =
        append_rpc_model_change(*runtime, previous_entry_count);
    if (!appended.ok()) {
      return qt::foundation::Result<Json>::failure(
          appended.error().code, appended.error().message,
          appended.error().system_error);
    }
    return qt::foundation::Result<Json>::success(
        rpc_model_json(runtime->host.harness().model()));
  };
  handlers.cycle_model = [runtime](const qt::app::CycleModelCommand&) {
    const auto models = rpc_available_model_refs();
    if (models.size() <= 1U) {
      return qt::foundation::Result<Json>::success(Json(nullptr));
    }

    std::size_t next_index = 0;
    const auto current = runtime->host.harness().model();
    const auto current_it =
        std::find_if(models.begin(), models.end(),
                     [&](const qt::ai::ModelRef& candidate) {
                       return candidate.provider == current.provider &&
                              candidate.name == current.name;
                     });
    if (current_it != models.end()) {
      const auto offset = static_cast<std::size_t>(
          std::distance(models.begin(), current_it));
      next_index = (offset + 1U) % models.size();
    }

    const auto previous_entry_count =
        runtime->host.session_manager().entries().size();
    runtime->config.model = models[next_index];
    runtime->host.harness().set_model(runtime->config.model);
    refresh_rpc_context_usage(*runtime);
    const auto appended =
        append_rpc_model_change(*runtime, previous_entry_count);
    if (!appended.ok()) {
      return qt::foundation::Result<Json>::failure(
          appended.error().code, appended.error().message,
          appended.error().system_error);
    }
    return qt::foundation::Result<Json>::success(
        Json{{"model", rpc_model_json(runtime->host.harness().model())},
             {"thinkingLevel", runtime->host.harness().thinking_level()},
             {"isScoped", false}});
  };
  handlers.get_available_models =
      [](const qt::app::GetAvailableModelsCommand&) {
        return qt::foundation::Result<Json>::success(
            make_rpc_available_models());
      };
  handlers.cycle_thinking_level =
      [runtime](const qt::app::CycleThinkingLevelCommand&) {
        const auto next = cycle_rpc_thinking_level(
            runtime->host.harness().model(),
            runtime->host.harness().thinking_level());
        if (!next) {
          return qt::foundation::Result<Json>::success(Json(nullptr));
        }
        const auto previous_entry_count =
            runtime->host.session_manager().entries().size();
        runtime->config.metadata["thinkingLevel"] = *next;
        runtime->host.harness().set_thinking_level(*next);
        const auto appended =
            append_rpc_thinking_level_change(*runtime,
                                                       previous_entry_count);
        if (!appended.ok()) {
          return qt::foundation::Result<Json>::failure(
              appended.error().code, appended.error().message,
              appended.error().system_error);
        }
        return qt::foundation::Result<Json>::success(Json{{"level", *next}});
      };
  handlers.set_thinking_level =
      [runtime](const qt::app::SetThinkingLevelCommand& command) {
        const auto previous_entry_count =
            runtime->host.session_manager().entries().size();
        const auto level = clamp_rpc_thinking_level(
            runtime->host.harness().model(), command.level);
        runtime->config.metadata["thinkingLevel"] = level;
        runtime->host.harness().set_thinking_level(level);
        const auto appended =
            append_rpc_thinking_level_change(*runtime,
                                                       previous_entry_count);
        if (!appended.ok()) {
          return qt::foundation::Result<Json>::failure(
              appended.error().code, appended.error().message,
              appended.error().system_error);
        }
        return qt::foundation::Result<Json>::success(Json::object());
      };
  handlers.set_steering_mode =
      [runtime](const qt::app::SetQueueModeCommand& command) {
        const auto parsed_mode = parse_rpc_queue_mode(command.mode);
        if (!parsed_mode) {
          return qt::foundation::Result<Json>::failure(
              parsed_mode.error().code, parsed_mode.error().message,
              parsed_mode.error().system_error);
        }
        runtime->steering_mode = parsed_mode.value();
        runtime->host.harness().set_steering_mode(parsed_mode.value());
        return qt::foundation::Result<Json>::success(Json::object());
      };
  handlers.set_follow_up_mode =
      [runtime](const qt::app::SetQueueModeCommand& command) {
        const auto parsed_mode = parse_rpc_queue_mode(command.mode);
        if (!parsed_mode) {
          return qt::foundation::Result<Json>::failure(
              parsed_mode.error().code, parsed_mode.error().message,
              parsed_mode.error().system_error);
        }
        runtime->follow_up_mode = parsed_mode.value();
        runtime->host.harness().set_follow_up_mode(parsed_mode.value());
        return qt::foundation::Result<Json>::success(Json::object());
      };
  handlers.set_auto_compaction =
      [runtime](const qt::app::SetBooleanModeCommand& command) {
        runtime->auto_compaction_enabled = command.enabled;
        return qt::foundation::Result<Json>::success(Json::object());
      };
  handlers.compact = [runtime](const qt::app::CompactCommand& command) {
    if (!runtime->startup_session.repository_path) {
      return qt::foundation::Result<Json>::failure(
          qt::foundation::ErrorCode::invalid_argument,
          "Cannot compact session: no active session file");
    }

    auto compacted = runtime->host.harness().compact_session(
        command.custom_instructions.value_or(std::string{}));
    if (!compacted) {
      return qt::foundation::Result<Json>::failure(
          compacted.error().code, compacted.error().message,
          compacted.error().system_error);
    }

    runtime->host.sync_session_manager_from_harness();
    const auto persisted = replace_rpc_runtime_entries(
        *runtime, runtime->host.session_manager().entries());
    if (!persisted.ok()) {
      return qt::foundation::Result<Json>::failure(
          persisted.error().code, persisted.error().message,
          persisted.error().system_error);
    }

    auto reopened = open_startup_session(
        *runtime->startup_session.repository_path, "compact");
    if (!reopened) {
      return qt::foundation::Result<Json>::failure(
          reopened.error().code, reopened.error().message,
          reopened.error().system_error);
    }
    reopened.value().parent_repository_path =
        runtime->startup_session.parent_repository_path;
    reopened.value().parent_session_id = runtime->startup_session.parent_session_id;

    const auto loaded = load_rpc_runtime_session(
        *runtime, reopened.value(), "reload");
    if (!loaded.ok()) {
      return qt::foundation::Result<Json>::failure(
          loaded.error().code, loaded.error().message,
          loaded.error().system_error);
    }
    if (runtime->session_stats.context_usage) {
      runtime->session_stats.context_usage->tokens.reset();
      runtime->session_stats.context_usage->percent.reset();
    }

    return qt::foundation::Result<Json>::success(
        make_rpc_compaction_payload(compacted.value()));
  };
  handlers.set_auto_retry =
      [runtime](const qt::app::SetBooleanModeCommand& command) {
        const auto persisted = persist_rpc_user_settings(
            runtime->agent_home, [&](Json& document) {
              if (!document.contains("retry") ||
                  !document.at("retry").is_object()) {
                document["retry"] = Json::object();
              }
              document["retry"]["enabled"] = command.enabled;
            });
        if (!persisted.ok()) {
          return qt::foundation::Result<Json>::failure(
              persisted.error().code, persisted.error().message,
              persisted.error().system_error);
        }
        runtime->auto_retry_enabled = command.enabled;
        return qt::foundation::Result<Json>::success(Json::object());
      };
  handlers.abort_retry = [](const qt::app::AbortRetryCommand&) {
    return qt::foundation::Result<Json>::success(Json::object());
  };
  handlers.bash = [runtime, bash_state](const qt::app::BashCommand& command) {
    auto log = qt::foundation::logger("rpc", "tools");
    log.info("tool_invoke_begin",
             {qt::foundation::log_field("tool", "bash")});
    const auto cancellation_token = bash_state->begin();
    auto tool_context = runtime->config.tool_context;
    tool_context.cancellation_token = cancellation_token;
    qt::tools::ToolRequest request;
    request.tool_name = "bash";
    request.arguments = Json{{"command", command.command}};
    const qt::tools::BashTool bash_tool;
    const auto executed = bash_tool.invoke(request, tool_context);
    bash_state->finish(cancellation_token);
    log.info("tool_invoke_end",
             {qt::foundation::log_field("tool", "bash"),
              qt::foundation::log_field("status",
                                        qt::tools::to_string(executed.status))});
    if (executed.status == qt::tools::ToolResultStatus::invalid_request ||
        !executed.output.is_object()) {
      return qt::foundation::Result<Json>::failure(
          qt::foundation::ErrorCode::invalid_argument,
          executed.error_message.value_or("failed to execute bash command"));
    }

    const std::string output =
        executed.output.value("stdout", std::string{}) +
        executed.output.value("stderr", std::string{});
    const bool cancelled = executed.output.value("cancelled", false);
    std::optional<int> exit_code;
    if (!cancelled && executed.output.contains("exit_code") &&
        executed.output.at("exit_code").is_number_integer()) {
      exit_code = executed.output.value("exit_code", 0);
    }
    const auto details = executed.output.find("details");
    const bool truncated =
        details != executed.output.end() && details->is_object() &&
        details->contains("truncation");
    std::optional<std::string> full_output_path;
    if (details != executed.output.end() && details->is_object() &&
        details->contains("fullOutputPath") &&
        details->at("fullOutputPath").is_string()) {
      full_output_path = details->value("fullOutputPath", std::string{});
    }
    qt::ai::Message message;
    message.id = "bash-" + generated_session_id();
    message.role = qt::ai::MessageRole::user;
    message.content.push_back(qt::ai::TextContent{
        rpc_bash_context_text(command.command, output, cancelled, truncated,
                              full_output_path, exit_code),
        std::nullopt});
    runtime->host.harness().agent().state().transcript.push_back(std::move(message));

    Json exit_code_json = nullptr;
    if (exit_code.has_value()) {
      exit_code_json = *exit_code;
    }
    Json bash_execution = Json::object();
    bash_execution["role"] = "bashExecution";
    bash_execution["command"] = command.command;
    bash_execution["output"] = output;
    bash_execution["exitCode"] = exit_code_json;
    bash_execution["cancelled"] = cancelled;
    bash_execution["truncated"] = truncated;
    bash_execution["fullOutputPath"] =
        full_output_path ? Json(*full_output_path) : Json(nullptr);
    const auto appended = append_rpc_upstream_payload(
        *runtime,
        Json{{"type", "message"},
             {"message", std::move(bash_execution)}});
    if (!appended.ok()) {
      return qt::foundation::Result<Json>::failure(
          appended.error().code, appended.error().message,
          appended.error().system_error);
    }

    Json payload = Json::object();
    payload["output"] = output;
    payload["exitCode"] = exit_code_json;
    payload["cancelled"] = cancelled;
    payload["truncated"] = truncated;
    if (full_output_path) {
      payload["fullOutputPath"] = *full_output_path;
    }
    return qt::foundation::Result<Json>::success(std::move(payload));
  };
  handlers.abort_bash = [bash_state](const qt::app::AbortBashCommand&) {
    bash_state->abort();
    return qt::foundation::Result<Json>::success(Json::object());
  };
  handlers.fork =
      [runtime, parsed, cwd, agent_home](const qt::app::ForkCommand& command) {
        const auto selected_text =
            rpc_fork_selected_text(*runtime, command.entry_id);
        if (!selected_text) {
          return qt::foundation::Result<Json>::failure(
              qt::foundation::ErrorCode::invalid_argument,
              "Invalid entry ID for forking");
        }
        if (!runtime->startup_session.repository_path) {
          return qt::foundation::Result<Json>::failure(
              qt::foundation::ErrorCode::invalid_argument,
              "Cannot fork session: no active session file");
        }

        const auto next_session =
            fork_startup_session(*runtime->startup_session.repository_path,
                                           rpc_session_dir(parsed, cwd,
                                                                     agent_home),
                                           command.entry_id);
        if (!next_session) {
          return qt::foundation::Result<Json>::failure(
              next_session.error().code, next_session.error().message,
              next_session.error().system_error);
        }

        const auto loaded = load_rpc_runtime_session(
            *runtime, next_session.value(), "fork");
        if (!loaded.ok()) {
          return qt::foundation::Result<Json>::failure(
              loaded.error().code, loaded.error().message,
              loaded.error().system_error);
        }
        return qt::foundation::Result<Json>::success(
            Json{{"text", *selected_text}, {"cancelled", false}});
      };
  handlers.clone =
      [runtime, parsed, cwd, agent_home](const qt::app::CloneCommand&) {
        const auto next_session = clone_startup_session(
            runtime->startup_session,
            rpc_session_dir(parsed, cwd, agent_home));
        if (!next_session) {
          return qt::foundation::Result<Json>::failure(
              next_session.error().code, next_session.error().message,
              next_session.error().system_error);
        }

        const auto loaded = load_rpc_runtime_session(
            *runtime, next_session.value(), "fork");
        if (!loaded.ok()) {
          return qt::foundation::Result<Json>::failure(
              loaded.error().code, loaded.error().message,
              loaded.error().system_error);
        }
        return qt::foundation::Result<Json>::success(Json{{"cancelled", false}});
      };
  handlers.get_session_stats = [runtime](const qt::app::GetSessionStatsCommand&) {
    return qt::foundation::Result<Json>::success(
        make_rpc_session_stats(*runtime));
  };
  handlers.get_fork_messages =
      [runtime](const qt::app::GetForkMessagesCommand&) {
        return qt::foundation::Result<Json>::success(
            make_rpc_fork_messages_payload(*runtime));
      };
  handlers.export_html =
      [runtime](const qt::app::ExportHtmlCommand& command) {
        if (!runtime->startup_session.repository_path) {
          return qt::foundation::Result<Json>::failure(
              qt::foundation::ErrorCode::invalid_argument,
              "Cannot export HTML: no active session file");
        }
        const auto exported = qt::app::export_session_file_to_html(
            *runtime->startup_session.repository_path, command.output_path, "qt");
        if (!exported) {
          return qt::foundation::Result<Json>::failure(
              exported.error().code, exported.error().message,
              exported.error().system_error);
        }
        return qt::foundation::Result<Json>::success(
            Json{{"path", exported.value()}});
      };
  handlers.get_last_assistant_text =
      [runtime](const qt::app::GetLastAssistantTextCommand&) {
        return qt::foundation::Result<Json>::success(
            make_rpc_last_assistant_text(*runtime));
      };
  handlers.set_session_name =
      [runtime](const qt::app::SetSessionNameCommand& command) {
        runtime->session_name = command.name;
        if (runtime->startup_session.repository_path) {
          const auto entry_id =
              runtime->host.harness().set_session_name(command.name);
          runtime->host.sync_session_manager_from_harness();
          if (!entry_id.has_value()) {
            return qt::foundation::Result<Json>::failure(
                qt::foundation::ErrorCode::internal_error,
                "failed to append session info entry");
          }
          const auto* entry = runtime->host.session_manager().get_entry(*entry_id);
          if (entry == nullptr) {
            return qt::foundation::Result<Json>::failure(
                qt::foundation::ErrorCode::internal_error,
                "failed to append session info entry");
          }
          const auto appended =
              qt::harness::SessionRepository(*runtime->startup_session.repository_path)
                  .append(*entry);
          if (!appended) {
            return qt::foundation::Result<Json>::failure(
                appended.error().code, appended.error().message,
                appended.error().system_error);
          }
          ++runtime->startup_session.entry_count;
        }
        return qt::foundation::Result<Json>::success(Json::object());
      };
  handlers.get_commands =
      [parsed, cwd, agent_home](const qt::app::GetCommandsCommand&) {
        return qt::foundation::Result<Json>::success(
            make_rpc_commands(parsed, cwd, agent_home));
      };

  qt::app::RpcServer server(std::move(handlers));
  attach_subagent_events(runtime->subagent_coordinator);
  ReferenceRpcInputQueue input_queue;
  std::thread input_reader([&input_queue, &bash_state, &output_mutex]() {
    std::array<char, 4096> buffer{};
    std::string pending_input;
    while (true) {
      int bytes_read = 0;
#if defined(_WIN32)
      bytes_read =
          _read(QT_FILENO(stdin), buffer.data(),
                static_cast<unsigned int>(buffer.size()));
#else
      bytes_read =
          static_cast<int>(::read(QT_FILENO(stdin), buffer.data(), buffer.size()));
      if (bytes_read < 0 && errno == EINTR) {
        continue;
      }
#endif
      if (bytes_read == 0) {
        break;
      }
      if (bytes_read < 0) {
        {
          std::lock_guard<std::mutex> lock(input_queue.mutex);
          input_queue.read_failed = true;
          input_queue.closed = true;
        }
        input_queue.ready.notify_one();
        return;
      }

      pending_input.append(buffer.data(), static_cast<std::size_t>(bytes_read));
      std::size_t line_start = 0;
      while (true) {
        const std::size_t line_end = pending_input.find('\n', line_start);
        if (line_end == std::string::npos) {
          break;
        }

        std::string line =
            pending_input.substr(line_start, line_end - line_start + 1);
        if (const auto id = rpc_abort_bash_id(line)) {
          bash_state->abort();
          write_rpc_output_lines_locked(
              rpc_abort_bash_success_lines(*id), output_mutex);
        } else {
          enqueue_rpc_line(input_queue, std::move(line));
        }
        line_start = line_end + 1;
      }
      pending_input.erase(0, line_start);
    }

    if (!pending_input.empty()) {
      if (const auto id = rpc_abort_bash_id(pending_input)) {
        bash_state->abort();
        write_rpc_output_lines_locked(
            rpc_abort_bash_success_lines(*id), output_mutex);
      } else {
        enqueue_rpc_line(input_queue, std::move(pending_input));
      }
    }

    {
      std::lock_guard<std::mutex> lock(input_queue.mutex);
      input_queue.closed = true;
    }
    input_queue.ready.notify_one();
  });

  bool failed = false;
  while (true) {
    std::string line;
    {
      std::unique_lock<std::mutex> lock(input_queue.mutex);
      input_queue.ready.wait(lock, [&input_queue]() {
        return input_queue.closed || !input_queue.lines.empty();
      });
      if (!input_queue.lines.empty()) {
        line = std::move(input_queue.lines.front());
        input_queue.lines.pop_front();
      } else if (input_queue.closed) {
        failed = input_queue.read_failed;
        break;
      }
    }

    const auto output = server.push_input(line);
    if (!output) {
      std::lock_guard<std::mutex> lock(output_mutex);
      std::cerr << "qt: " << output.error().message << '\n';
      failed = true;
      break;
    }
    write_rpc_output_lines_locked(output.value(), output_mutex);
  }

  input_reader.join();
  if (failed) {
    runtime->subagent_coordinator->shutdown();
    if (input_queue.read_failed) {
      std::lock_guard<std::mutex> lock(output_mutex);
      std::cerr << "qt: failed to read rpc input\n";
    }
    log.warn("mode_exit", {qt::foundation::log_field("code", EXIT_FAILURE)});
    return EXIT_FAILURE;
  }

  const auto finish = server.finish();
  if (!finish) {
    log.error("server_finish_failed",
              {qt::foundation::log_field("error", finish.error().message)});
    std::cerr << "qt: " << finish.error().message << '\n';
    return EXIT_FAILURE;
  }
  write_rpc_output_lines_locked(finish.value(), output_mutex);
  runtime->subagent_coordinator->shutdown();
  log.info("mode_exit", {qt::foundation::log_field("code", EXIT_SUCCESS)});
  return EXIT_SUCCESS;
}

}  // namespace qt::app
