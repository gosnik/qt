#include "qt/harness/agent_harness.hpp"

#include "qt/ai/provider_transport.hpp"
#include "qt/foundation/uuid.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <utility>

namespace qt::harness {

namespace {

constexpr const char* kAbortError = "agent turn aborted";
constexpr std::int64_t kDefaultMaxOutputTokensCap = 32000;

class PhaseResetGuard {
 public:
  explicit PhaseResetGuard(AgentHarnessPhase& phase) : phase_(phase) {}
  ~PhaseResetGuard() { phase_ = AgentHarnessPhase::idle; }

  PhaseResetGuard(const PhaseResetGuard&) = delete;
  PhaseResetGuard& operator=(const PhaseResetGuard&) = delete;

 private:
  AgentHarnessPhase& phase_;
};

bool writes_equal(const AgentHarnessWrite& left,
                  const AgentHarnessWrite& right) {
  return left.kind == right.kind &&
         left.sequence == right.sequence &&
         qt::ai::Json(left.message) == qt::ai::Json(right.message);
}

bool writes_equal(const std::vector<AgentHarnessWrite>& left,
                  const std::vector<AgentHarnessWrite>& right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (!writes_equal(left[index], right[index])) {
      return false;
    }
  }
  return true;
}

bool model_refs_equal(const qt::ai::ModelRef& left,
                      const qt::ai::ModelRef& right) {
  return left.provider == right.provider && left.api == right.api &&
         left.name == right.name;
}

bool tool_definitions_equal(
    const std::vector<qt::ai::ToolDefinition>& left,
    const std::vector<qt::ai::ToolDefinition>& right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (left[index].name != right[index].name ||
        left[index].description != right[index].description ||
        left[index].input_schema != right[index].input_schema) {
      return false;
    }
  }
  return true;
}

std::vector<qt::ai::Message> queue_messages(
    const std::vector<qt::agent::QueuedMessage>& queue) {
  std::vector<qt::ai::Message> messages;
  messages.reserve(queue.size());
  for (const auto& queued : queue) {
    messages.push_back(queued.message);
  }
  return messages;
}

std::optional<std::int64_t> metadata_int64(const qt::ai::Json& metadata,
                                           const char* key) {
  if (!metadata.is_object()) {
    return std::nullopt;
  }
  const auto it = metadata.find(key);
  if (it == metadata.end()) {
    return std::nullopt;
  }
  if (it->is_number_integer()) {
    return it->get<std::int64_t>();
  }
  return std::nullopt;
}

std::optional<std::int64_t> default_max_output_tokens(
    const qt::ai::ProviderOptions& options) {
  const auto model_max = metadata_int64(options.metadata, "maxTokens");
  if (!model_max || *model_max <= 0) {
    return std::nullopt;
  }
  return std::min(*model_max, kDefaultMaxOutputTokensCap);
}

std::optional<bool> metadata_bool(const qt::ai::Json& metadata,
                                  const char* key) {
  if (!metadata.is_object()) {
    return std::nullopt;
  }
  const auto it = metadata.find(key);
  if (it == metadata.end()) {
    return std::nullopt;
  }
  if (it->is_boolean()) {
    return it->get<bool>();
  }
  return std::nullopt;
}

std::optional<qt::ai::Json> metadata_object(const qt::ai::Json& metadata,
                                            const char* key) {
  if (!metadata.is_object()) {
    return std::nullopt;
  }
  const auto it = metadata.find(key);
  if (it == metadata.end() || !it->is_object()) {
    return std::nullopt;
  }
  return *it;
}

std::int64_t usage_context_tokens(const qt::ai::Usage& usage) {
  if (usage.total_tokens > 0) {
    return usage.total_tokens;
  }
  return usage.input_tokens + usage.output_tokens + usage.cache_read_tokens +
         usage.cache_write_tokens;
}

void merge_provider_options(qt::ai::ProviderOptions& target,
                            const qt::ai::ProviderOptions& patch) {
  if (patch.api_key) {
    target.api_key = patch.api_key;
  }
  if (patch.timeout_ms) {
    target.timeout_ms = patch.timeout_ms;
  }
  if (patch.max_retries) {
    target.max_retries = patch.max_retries;
  }
  if (patch.max_retry_delay_ms) {
    target.max_retry_delay_ms = patch.max_retry_delay_ms;
  }
  if (patch.proxy_url) {
    target.proxy_url = patch.proxy_url;
  }
  for (const auto& header : patch.headers) {
    target.headers[header.first] = header.second;
  }
  if (patch.metadata.is_object()) {
    if (!target.metadata.is_object()) {
      target.metadata = qt::ai::Json::object();
    }
    for (auto it = patch.metadata.begin(); it != patch.metadata.end(); ++it) {
      if (it.value().is_null()) {
        target.metadata.erase(it.key());
      } else {
        target.metadata[it.key()] = it.value();
      }
    }
  } else if (!patch.metadata.is_null()) {
    target.metadata = patch.metadata;
  }
}

std::vector<SessionUserContentBlock> to_session_user_blocks(
    const std::vector<qt::ai::Content>& content) {
  std::vector<SessionUserContentBlock> blocks;
  blocks.reserve(content.size());
  for (const auto& item : content) {
    if (const auto* text = std::get_if<qt::ai::TextContent>(&item);
        text != nullptr) {
      SessionUserContentBlock block;
      block.kind = SessionUserContentBlockKind::kText;
      block.text = text->text;
      block.text_signature = text->signature;
      blocks.push_back(std::move(block));
      continue;
    }

    if (const auto* image = std::get_if<qt::ai::ImageContent>(&item);
        image != nullptr) {
      SessionUserContentBlock block;
      block.kind = SessionUserContentBlockKind::kImage;
      block.data = image->data;
      block.mime_type = image->media_type;
      blocks.push_back(std::move(block));
      continue;
    }

    throw std::logic_error(
        "Unsupported user/tool content in AgentHarness session mirror");
  }
  return blocks;
}

std::vector<SessionAssistantContentBlock> to_session_assistant_blocks(
    const std::vector<qt::ai::Content>& content) {
  std::vector<SessionAssistantContentBlock> blocks;
  blocks.reserve(content.size());
  for (const auto& item : content) {
    if (const auto* text = std::get_if<qt::ai::TextContent>(&item);
        text != nullptr) {
      SessionAssistantContentBlock block;
      block.kind = SessionAssistantContentBlockKind::kText;
      block.text = text->text;
      block.text_signature = text->signature;
      blocks.push_back(std::move(block));
      continue;
    }

    if (const auto* thinking = std::get_if<qt::ai::ThinkingContent>(&item);
        thinking != nullptr) {
      SessionAssistantContentBlock block;
      block.kind = SessionAssistantContentBlockKind::kThinking;
      block.thinking = thinking->text;
      block.thinking_signature = thinking->signature;
      block.redacted = thinking->redacted;
      blocks.push_back(std::move(block));
      continue;
    }

    if (const auto* tool_call = std::get_if<qt::ai::ToolCallContent>(&item);
        tool_call != nullptr) {
      SessionAssistantContentBlock block;
      block.kind = SessionAssistantContentBlockKind::kToolCall;
      block.tool_call_id = tool_call->id;
      block.tool_name = tool_call->name;
      block.tool_arguments = tool_call->arguments;
      blocks.push_back(std::move(block));
      continue;
    }

    throw std::logic_error(
        "Unsupported assistant content in AgentHarness session mirror");
  }
  return blocks;
}

SessionToolResultMessageEntry to_session_tool_result_entry(
    const qt::ai::Message& message) {
  if (message.content.empty()) {
    throw std::logic_error(
        "Tool result message is missing content for AgentHarness session mirror");
  }

  const auto* tool_result =
      std::get_if<qt::ai::ToolResultContent>(&message.content.front());
  if (tool_result == nullptr) {
    throw std::logic_error(
        "Unsupported tool result content in AgentHarness session mirror");
  }

  SessionUserContentBlock output;
  output.kind = SessionUserContentBlockKind::kText;
  if (tool_result->result.contains("output") &&
      tool_result->result.at("output").is_string()) {
    output.text = tool_result->result.at("output").get<std::string>();
  } else {
    output.text = tool_result->result.dump();
  }

  SessionToolResultMessageEntry entry;
  entry.tool_call_id = tool_result->tool_call_id;
  entry.tool_name = tool_result->name;
  entry.content.push_back(std::move(output));
  entry.details = tool_result->result;
  entry.is_error = tool_result->is_error;
  return entry;
}

}  // namespace

struct AgentHarness::ProviderProxyState {
  AgentHarness* harness = nullptr;
};

class AgentHarnessProviderProxy final : public qt::ai::Provider {
 public:
  explicit AgentHarnessProviderProxy(
      std::shared_ptr<AgentHarness::ProviderProxyState> state)
      : state_(std::move(state)) {}

  const qt::ai::ProviderDescriptor& descriptor() const override {
    return harness().provider_->descriptor();
  }

  qt::ai::CompletionResponse complete(
      const qt::ai::CompletionRequest& request,
      const qt::ai::ProviderOptions& options) override {
    auto& owner = harness();
    const auto snapshotted_request =
        owner.apply_runtime_request_snapshot(request);
    const auto contextual_request =
        owner.apply_context_hooks(snapshotted_request);
    const auto augmented_request =
        owner.apply_model_context_augmentors(contextual_request);
    const auto applied =
        owner.apply_before_provider_request_hooks(augmented_request, options);
    const auto payload_request =
        owner.apply_before_provider_payload_hooks(augmented_request);
    auto response = owner.provider_->complete(payload_request, applied);
    owner.record_after_provider_response(payload_request, applied, response);
    return response;
  }

  void stream(const qt::ai::CompletionRequest& request,
              const qt::ai::ProviderOptions& options,
              qt::ai::EventSink& sink) override {
    auto& owner = harness();
    const auto snapshotted_request =
        owner.apply_runtime_request_snapshot(request);
    const auto contextual_request =
        owner.apply_context_hooks(snapshotted_request);
    const auto augmented_request =
        owner.apply_model_context_augmentors(contextual_request);
    const auto applied =
        owner.apply_before_provider_request_hooks(augmented_request, options);
    const auto payload_request =
        owner.apply_before_provider_payload_hooks(augmented_request);
    class ResponseRecorder final : public qt::ai::EventSink {
     public:
      explicit ResponseRecorder(qt::ai::EventSink& sink) : sink_(sink) {}

      void push(const qt::ai::StreamEvent& event) override {
        events_.push_back(event);
        sink_.push(events_.back());
      }

      const std::vector<qt::ai::StreamEvent>& events() const noexcept {
        return events_;
      }

     private:
      qt::ai::EventSink& sink_;
      std::vector<qt::ai::StreamEvent> events_;
    } recorder{sink};

    owner.provider_->stream(payload_request, applied, recorder);
    auto response = qt::ai::completion_response_from_events(recorder.events());
    owner.record_after_provider_response(payload_request, applied, response);
  }

 private:
  AgentHarness& harness() const {
    if (!state_ || state_->harness == nullptr) {
      throw std::logic_error("AgentHarness provider proxy is no longer bound");
    }
    return *state_->harness;
  }

  std::shared_ptr<AgentHarness::ProviderProxyState> state_;
};

AgentHarness::AgentHarness(qt::agent::AgentConfig config,
                           std::shared_ptr<qt::ai::Provider> provider,
                           qt::ai::ProviderOptions options)
    : config_(std::move(config)),
      provider_(std::move(provider)),
      options_(std::move(options)),
      provider_proxy_state_(std::make_shared<ProviderProxyState>()),
      routed_provider_(provider_
                           ? std::make_shared<AgentHarnessProviderProxy>(
                                 provider_proxy_state_)
                           : nullptr),
      agent_(config_, routed_provider_, options_) {
  bind_provider_proxy_state();
  initialize_cached_agent_hooks();
  tool_definitions_ = config_.tools;
  active_tool_names_.reserve(tool_definitions_.size());
  for (const auto& tool : tool_definitions_) {
    active_tool_names_.push_back(tool.name);
  }
  if (config_.metadata.is_object()) {
    const auto it = config_.metadata.find("thinkingLevel");
    if (it != config_.metadata.end() && it->is_string()) {
      thinking_level_ = it->get<std::string>();
    }
  }
  apply_thinking_level_metadata();
  refresh_applied_turn_snapshot();
  sync_agent_for_next_turn();
}

AgentHarness::~AgentHarness() noexcept { clear_provider_proxy_state(); }

AgentHarness::AgentHarness(AgentHarness&& other) noexcept
    : config_(std::move(other.config_)),
      provider_(std::move(other.provider_)),
      options_(std::move(other.options_)),
      provider_proxy_state_(std::move(other.provider_proxy_state_)),
      routed_provider_(std::move(other.routed_provider_)),
      agent_(config_, routed_provider_, options_),
      thinking_level_(std::move(other.thinking_level_)),
      pending_stream_options_(std::move(other.pending_stream_options_)),
      applied_model_(std::move(other.applied_model_)),
      applied_metadata_(std::move(other.applied_metadata_)),
      applied_tool_definitions_(std::move(other.applied_tool_definitions_)),
      session_manager_(std::move(other.session_manager_)),
      pending_session_entries_(std::move(other.pending_session_entries_)),
      pending_session_transcript_messages_(
          std::move(other.pending_session_transcript_messages_)),
      pending_session_entries_flushed_at_turn_end_(
          other.pending_session_entries_flushed_at_turn_end_),
      phase_(other.phase_),
      events_(std::move(other.events_)),
      committed_writes_(std::move(other.committed_writes_)),
      save_points_(std::move(other.save_points_)),
      next_turn_queue_(std::move(other.next_turn_queue_)),
      queue_update_events_(std::move(other.queue_update_events_)),
      resources_(std::move(other.resources_)),
      resource_update_events_(std::move(other.resource_update_events_)),
      tool_update_events_(std::move(other.tool_update_events_)),
      tool_definitions_(std::move(other.tool_definitions_)),
      active_tool_names_(std::move(other.active_tool_names_)),
      steering_queue_(std::move(other.steering_queue_)),
      follow_up_queue_(std::move(other.follow_up_queue_)),
      before_agent_start_events_(std::move(other.before_agent_start_events_)),
      before_agent_start_hooks_(std::move(other.before_agent_start_hooks_)),
      context_events_(std::move(other.context_events_)),
      context_hooks_(std::move(other.context_hooks_)),
      model_context_events_(std::move(other.model_context_events_)),
      model_context_augmentors_(std::move(other.model_context_augmentors_)),
      logical_input_kinds_(std::move(other.logical_input_kinds_)),
      logical_input_cancellation_(
          std::move(other.logical_input_cancellation_)),
      current_logical_input_sequence_(
          other.current_logical_input_sequence_),
      current_logical_input_kind_(other.current_logical_input_kind_),
      current_model_context_turn_index_(
          other.current_model_context_turn_index_),
      model_context_projection_epoch_(other.model_context_projection_epoch_),
      next_model_context_sequence_(other.next_model_context_sequence_),
      last_augmented_logical_input_sequence_(
          other.last_augmented_logical_input_sequence_),
      last_augmented_projection_epoch_(
          other.last_augmented_projection_epoch_),
      call_index_for_logical_input_(other.call_index_for_logical_input_),
      before_provider_request_events_(
          std::move(other.before_provider_request_events_)),
      before_provider_request_hooks_(
          std::move(other.before_provider_request_hooks_)),
      before_provider_payload_events_(
          std::move(other.before_provider_payload_events_)),
      before_provider_payload_hooks_(
          std::move(other.before_provider_payload_hooks_)),
      after_provider_response_events_(
          std::move(other.after_provider_response_events_)),
      after_provider_response_hooks_(
          std::move(other.after_provider_response_hooks_)),
      tool_call_events_(std::move(other.tool_call_events_)),
      tool_call_hooks_(std::move(other.tool_call_hooks_)),
      tool_result_events_(std::move(other.tool_result_events_)),
      tool_result_hooks_(std::move(other.tool_result_hooks_)),
      before_compact_events_(std::move(other.before_compact_events_)),
      before_compact_hooks_(std::move(other.before_compact_hooks_)),
      compact_events_(std::move(other.compact_events_)),
      session_start_events_(std::move(other.session_start_events_)),
      session_shutdown_events_(std::move(other.session_shutdown_events_)),
      session_before_tree_events_(
          std::move(other.session_before_tree_events_)),
      session_tree_events_(std::move(other.session_tree_events_)),
      next_subscription_id_(other.next_subscription_id_),
      subscribers_(std::move(other.subscribers_)),
      last_result_(std::move(other.last_result_)),
      listener_(std::move(other.listener_)) {
  bind_provider_proxy_state();
  if (provider_ && !routed_provider_) {
    routed_provider_ =
        std::make_shared<AgentHarnessProviderProxy>(provider_proxy_state_);
  }
  initialize_cached_agent_hooks();
  invalidate_filtered_tool_definitions();
  agent_.state() = std::move(other.agent_.state());
  sync_agent_for_next_turn();
  other.phase_ = AgentHarnessPhase::idle;
}

AgentHarness& AgentHarness::operator=(AgentHarness&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  clear_provider_proxy_state();
  config_ = std::move(other.config_);
  provider_ = std::move(other.provider_);
  options_ = std::move(other.options_);
  provider_proxy_state_ = std::move(other.provider_proxy_state_);
  bind_provider_proxy_state();
  routed_provider_ = std::move(other.routed_provider_);
  if (provider_ && !routed_provider_) {
    routed_provider_ =
        std::make_shared<AgentHarnessProviderProxy>(provider_proxy_state_);
  } else if (!provider_) {
    routed_provider_.reset();
  }
  agent_ = qt::agent::Agent(config_, routed_provider_, options_);
  agent_.state() = std::move(other.agent_.state());
  thinking_level_ = std::move(other.thinking_level_);
  pending_stream_options_ = std::move(other.pending_stream_options_);
  applied_model_ = std::move(other.applied_model_);
  applied_metadata_ = std::move(other.applied_metadata_);
  applied_tool_definitions_ = std::move(other.applied_tool_definitions_);
  session_manager_ = std::move(other.session_manager_);
  pending_session_entries_ = std::move(other.pending_session_entries_);
  pending_session_transcript_messages_ =
      std::move(other.pending_session_transcript_messages_);
  pending_session_entries_flushed_at_turn_end_ =
      other.pending_session_entries_flushed_at_turn_end_;
  phase_ = other.phase_;
  events_ = std::move(other.events_);
  committed_writes_ = std::move(other.committed_writes_);
  save_points_ = std::move(other.save_points_);
  next_turn_queue_ = std::move(other.next_turn_queue_);
  queue_update_events_ = std::move(other.queue_update_events_);
  resources_ = std::move(other.resources_);
  resource_update_events_ = std::move(other.resource_update_events_);
  tool_update_events_ = std::move(other.tool_update_events_);
  tool_definitions_ = std::move(other.tool_definitions_);
  active_tool_names_ = std::move(other.active_tool_names_);
  steering_queue_ = std::move(other.steering_queue_);
  follow_up_queue_ = std::move(other.follow_up_queue_);
  before_agent_start_events_ = std::move(other.before_agent_start_events_);
  before_agent_start_hooks_ = std::move(other.before_agent_start_hooks_);
  context_events_ = std::move(other.context_events_);
  context_hooks_ = std::move(other.context_hooks_);
  model_context_events_ = std::move(other.model_context_events_);
  model_context_augmentors_ = std::move(other.model_context_augmentors_);
  logical_input_kinds_ = std::move(other.logical_input_kinds_);
  logical_input_cancellation_ =
      std::move(other.logical_input_cancellation_);
  current_logical_input_sequence_ = other.current_logical_input_sequence_;
  current_logical_input_kind_ = other.current_logical_input_kind_;
  current_model_context_turn_index_ = other.current_model_context_turn_index_;
  model_context_projection_epoch_ = other.model_context_projection_epoch_;
  next_model_context_sequence_ = other.next_model_context_sequence_;
  last_augmented_logical_input_sequence_ =
      other.last_augmented_logical_input_sequence_;
  last_augmented_projection_epoch_ =
      other.last_augmented_projection_epoch_;
  call_index_for_logical_input_ = other.call_index_for_logical_input_;
  before_provider_request_events_ =
      std::move(other.before_provider_request_events_);
  before_provider_request_hooks_ =
      std::move(other.before_provider_request_hooks_);
  before_provider_payload_events_ =
      std::move(other.before_provider_payload_events_);
  before_provider_payload_hooks_ =
      std::move(other.before_provider_payload_hooks_);
  after_provider_response_events_ =
      std::move(other.after_provider_response_events_);
  after_provider_response_hooks_ =
      std::move(other.after_provider_response_hooks_);
  tool_call_events_ = std::move(other.tool_call_events_);
  tool_call_hooks_ = std::move(other.tool_call_hooks_);
  tool_result_events_ = std::move(other.tool_result_events_);
  tool_result_hooks_ = std::move(other.tool_result_hooks_);
  before_compact_events_ = std::move(other.before_compact_events_);
  before_compact_hooks_ = std::move(other.before_compact_hooks_);
  compact_events_ = std::move(other.compact_events_);
  session_start_events_ = std::move(other.session_start_events_);
  session_shutdown_events_ = std::move(other.session_shutdown_events_);
  session_before_tree_events_ = std::move(other.session_before_tree_events_);
  session_tree_events_ = std::move(other.session_tree_events_);
  next_subscription_id_ = other.next_subscription_id_;
  subscribers_ = std::move(other.subscribers_);
  last_result_ = std::move(other.last_result_);
  listener_ = std::move(other.listener_);
  initialize_cached_agent_hooks();
  invalidate_filtered_tool_definitions();
  sync_agent_for_next_turn();
  other.phase_ = AgentHarnessPhase::idle;
  return *this;
}

void AgentHarness::bind_provider_proxy_state() noexcept {
  if (!provider_proxy_state_) {
    provider_proxy_state_ = std::make_shared<ProviderProxyState>();
  }
  provider_proxy_state_->harness = this;
}

void AgentHarness::clear_provider_proxy_state() noexcept {
  if (provider_proxy_state_ && provider_proxy_state_->harness == this) {
    provider_proxy_state_->harness = nullptr;
  }
}

std::vector<AgentHarnessWrite> AgentHarness::pending_writes() const {
  std::vector<AgentHarnessWrite> writes;
  writes.reserve(agent_.state().pending.size() + next_turn_queue_.size());
  for (const auto& message : next_turn_queue_) {
    AgentHarnessWrite write;
    write.kind = AgentHarnessWriteKind::pending_message;
    write.message = message;
    writes.push_back(std::move(write));
  }
  for (const auto& pending : agent_.state().pending) {
    AgentHarnessWrite write;
    write.kind = AgentHarnessWriteKind::pending_message;
    write.sequence = pending.sequence;
    write.message = pending.message;
    writes.push_back(std::move(write));
  }
  return writes;
}

std::vector<AgentHarnessWrite> AgentHarness::session_writes() const {
  std::vector<AgentHarnessWrite> writes = committed_writes_;
  auto pending = pending_writes();
  writes.reserve(writes.size() + pending.size());
  writes.insert(writes.end(), std::make_move_iterator(pending.begin()),
                std::make_move_iterator(pending.end()));
  return writes;
}

const AgentHarnessSavePoint* AgentHarness::last_save_point() const noexcept {
  if (save_points_.empty()) {
    return nullptr;
  }
  return &save_points_.back();
}

void AgentHarness::set_event_listener(EventListener listener) {
  listener_ = std::move(listener);
}

AgentHarness::SubscriptionId AgentHarness::subscribe(Subscriber listener) {
  const auto id = next_subscription_id_++;
  subscribers_.emplace_back(id, std::move(listener));
  return id;
}

bool AgentHarness::unsubscribe(SubscriptionId id) {
  const auto it = std::remove_if(
      subscribers_.begin(), subscribers_.end(),
      [id](const std::pair<SubscriptionId, Subscriber>& entry) {
        return entry.first == id;
      });
  if (it == subscribers_.end()) {
    return false;
  }
  subscribers_.erase(it, subscribers_.end());
  return true;
}

void AgentHarness::add_before_agent_start_hook(BeforeAgentStartHook hook) {
  before_agent_start_hooks_.push_back(std::move(hook));
}

void AgentHarness::clear_before_agent_start_hooks() {
  before_agent_start_hooks_.clear();
}

void AgentHarness::add_before_provider_request_hook(
    BeforeProviderRequestHook hook) {
  before_provider_request_hooks_.push_back(std::move(hook));
}

void AgentHarness::clear_before_provider_request_hooks() {
  before_provider_request_hooks_.clear();
}

void AgentHarness::add_before_provider_payload_hook(
    BeforeProviderPayloadHook hook) {
  before_provider_payload_hooks_.push_back(std::move(hook));
}

void AgentHarness::clear_before_provider_payload_hooks() {
  before_provider_payload_hooks_.clear();
}

void AgentHarness::add_context_hook(ContextHook hook) {
  context_hooks_.push_back(std::move(hook));
}

void AgentHarness::clear_context_hooks() { context_hooks_.clear(); }

void AgentHarness::add_model_context_augmentor(
    ModelContextAugmentor augmentor) {
  model_context_augmentors_.push_back(std::move(augmentor));
}

void AgentHarness::clear_model_context_augmentors() {
  model_context_augmentors_.clear();
}

void AgentHarness::reset_model_context_projection() noexcept {
  std::lock_guard<std::recursive_mutex> lock(queue_mutex_);
  ++model_context_projection_epoch_;
  if (model_context_projection_epoch_ == 0U) {
    model_context_projection_epoch_ = 1U;
  }
  logical_input_kinds_.clear();
  logical_input_cancellation_.clear();
  current_logical_input_sequence_.reset();
  current_logical_input_kind_ = LogicalInputKind::unknown;
  current_model_context_turn_index_ = 0U;
  last_augmented_logical_input_sequence_.reset();
  last_augmented_projection_epoch_ = 0U;
  call_index_for_logical_input_ = 0U;
}

void AgentHarness::add_after_provider_response_hook(
    AfterProviderResponseHook hook) {
  after_provider_response_hooks_.push_back(std::move(hook));
}

void AgentHarness::clear_after_provider_response_hooks() {
  after_provider_response_hooks_.clear();
}

void AgentHarness::add_tool_call_hook(ToolCallHook hook) {
  tool_call_hooks_.push_back(std::move(hook));
}

void AgentHarness::add_before_tool_hook(qt::agent::BeforeToolHook hook) {
  config_.before_tool_hooks.push_back(std::move(hook));
  invalidate_agent_config();
}

void AgentHarness::add_tool_result_transform(qt::agent::ToolResultTransformHook hook) {
  config_.tool_result_transforms.push_back(std::move(hook));
  invalidate_agent_config();
}

void AgentHarness::clear_tool_call_hooks() { tool_call_hooks_.clear(); }

void AgentHarness::add_tool_result_hook(ToolResultHook hook) {
  tool_result_hooks_.push_back(std::move(hook));
}

void AgentHarness::clear_tool_result_hooks() { tool_result_hooks_.clear(); }

void AgentHarness::add_before_compact_hook(BeforeCompactHook hook) {
  before_compact_hooks_.push_back(std::move(hook));
}

void AgentHarness::clear_before_compact_hooks() {
  before_compact_hooks_.clear();
}

void AgentHarness::clear_events() { events_.clear(); }

void AgentHarness::reset_phase() noexcept {
  if (phase_ != AgentHarnessPhase::running) {
    phase_ = AgentHarnessPhase::idle;
  }
}

void AgentHarness::set_model(qt::ai::ModelRef model) {
  reset_model_context_projection();
  const bool config_changed = !model_refs_equal(config_.model, model);
  config_.model = std::move(model);
  if (config_changed) {
    invalidate_agent_config();
  }
  SessionEntry entry;
  entry.data = SessionModelChangeEntry{config_.model.provider,
                                       config_.model.name,
                                       SessionUpstreamTreeFields{}};
  (void)persist_or_queue_session_entry(std::move(entry));
  if (phase_ != AgentHarnessPhase::running) {
    refresh_applied_turn_snapshot();
    sync_agent_for_next_turn();
  }
}

void AgentHarness::set_model(qt::ai::ModelRef model,
                             std::shared_ptr<qt::ai::Provider> provider,
                             qt::ai::ProviderOptions options) {
  if (provider == nullptr) {
    throw std::invalid_argument("agent harness provider is required");
  }

  reset_model_context_projection();
  const auto max_output_tokens = default_max_output_tokens(options);
  bool metadata_changed = false;
  if (config_.metadata.is_object()) {
    const auto provider_metadata = config_.metadata.find("provider");
    const auto model_name = config_.metadata.find("model");
    metadata_changed =
        provider_metadata == config_.metadata.end() ||
        *provider_metadata != model.provider ||
        model_name == config_.metadata.end() || *model_name != model.name;
  }
  const bool config_changed = !model_refs_equal(config_.model, model) ||
                              config_.max_output_tokens != max_output_tokens ||
                              metadata_changed;
  config_.model = std::move(model);
  config_.max_output_tokens = max_output_tokens;
  if (config_.metadata.is_object()) {
    config_.metadata["provider"] = config_.model.provider;
    config_.metadata["model"] = config_.model.name;
  }
  provider_ = std::move(provider);
  bind_provider_proxy_state();
  if (!routed_provider_) {
    routed_provider_ =
        std::make_shared<AgentHarnessProviderProxy>(provider_proxy_state_);
  }
  options_ = std::move(options);
  if (config_changed) {
    invalidate_agent_config();
  }

  SessionEntry entry;
  entry.data = SessionModelChangeEntry{config_.model.provider,
                                       config_.model.name,
                                       SessionUpstreamTreeFields{}};
  (void)persist_or_queue_session_entry(std::move(entry));
  if (phase_ == AgentHarnessPhase::running) {
    pending_stream_options_ = options_;
    return;
  }
  agent_.set_provider_options(options_);
  refresh_applied_turn_snapshot();
  sync_agent_for_next_turn();
}

void AgentHarness::set_provider(std::shared_ptr<qt::ai::Provider> provider,
                                qt::ai::ProviderOptions options) {
  if (provider == nullptr) {
    throw std::invalid_argument("agent harness provider is required");
  }
  reset_model_context_projection();
  provider_ = std::move(provider);
  bind_provider_proxy_state();
  if (!routed_provider_) {
    routed_provider_ =
        std::make_shared<AgentHarnessProviderProxy>(provider_proxy_state_);
  }
  set_stream_options(std::move(options));
}

void AgentHarness::set_thinking_level(std::string level) {
  const bool config_changed = thinking_level_ != level;
  thinking_level_ = std::move(level);
  apply_thinking_level_metadata();
  if (config_changed) {
    invalidate_agent_config();
  }
  SessionEntry entry;
  entry.data = SessionThinkingLevelChangeEntry{thinking_level_,
                                               SessionUpstreamTreeFields{}};
  (void)persist_or_queue_session_entry(std::move(entry));
  if (phase_ != AgentHarnessPhase::running) {
    refresh_applied_turn_snapshot();
    sync_agent_for_next_turn();
  }
}

void AgentHarness::set_stream_options(qt::ai::ProviderOptions options) {
  options_ = std::move(options);
  if (phase_ == AgentHarnessPhase::running) {
    pending_stream_options_ = options_;
    return;
  }
  agent_.set_provider_options(options_);
}

void AgentHarness::set_resources(AgentHarnessResources resources) {
  AgentHarnessResourcesUpdate update;
  update.previous_resources = resources_;
  update.resources = std::move(resources);
  resources_ = update.resources;
  resource_update_events_.push_back(std::move(update));
  emit_subscribed_event(SubscribedEvent{resource_update_events_.back()});
}

void AgentHarness::set_tool_definitions(
    std::vector<qt::ai::ToolDefinition> tools,
    std::optional<std::vector<std::string>> active_tool_names) {
  AgentHarnessToolsUpdate update;
  update.previous_tools = tool_definitions_;
  update.previous_active_tool_names = active_tool_names_;
  const bool definitions_changed =
      !tool_definitions_equal(tool_definitions_, tools);
  tool_definitions_ = std::move(tools);
  std::vector<std::string> next_active_tool_names;
  if (active_tool_names) {
    validate_active_tool_names(*active_tool_names);
    next_active_tool_names = std::move(*active_tool_names);
  } else {
    next_active_tool_names.reserve(tool_definitions_.size());
    for (const auto& tool : tool_definitions_) {
      next_active_tool_names.push_back(tool.name);
    }
  }
  const bool active_tools_changed =
      active_tool_names_ != next_active_tool_names;
  active_tool_names_ = std::move(next_active_tool_names);
  if (definitions_changed || active_tools_changed) {
    invalidate_filtered_tool_definitions();
  }
  update.tools = tool_definitions_;
  update.active_tool_names = active_tool_names_;
  tool_update_events_.push_back(std::move(update));
  emit_subscribed_event(SubscribedEvent{tool_update_events_.back()});
  if (phase_ != AgentHarnessPhase::running) {
    refresh_applied_turn_snapshot();
    sync_agent_for_next_turn();
  }
}

void AgentHarness::set_active_tool_names(
    std::vector<std::string> active_tool_names) {
  validate_active_tool_names(active_tool_names);
  AgentHarnessToolsUpdate update;
  update.previous_tools = tool_definitions_;
  update.previous_active_tool_names = active_tool_names_;
  const bool config_changed = active_tool_names_ != active_tool_names;
  active_tool_names_ = std::move(active_tool_names);
  if (config_changed) {
    invalidate_filtered_tool_definitions();
  }
  update.tools = tool_definitions_;
  update.active_tool_names = active_tool_names_;
  tool_update_events_.push_back(std::move(update));
  emit_subscribed_event(SubscribedEvent{tool_update_events_.back()});
  if (phase_ != AgentHarnessPhase::running) {
    refresh_applied_turn_snapshot();
    sync_agent_for_next_turn();
  }
}

void AgentHarness::set_external_filesystem_access(bool allowed) {
  if (config_.tool_context.allow_external_filesystem_access == allowed) {
    return;
  }
  config_.tool_context.allow_external_filesystem_access = allowed;
  invalidate_agent_config();
  if (phase_ != AgentHarnessPhase::running) {
    refresh_applied_turn_snapshot();
    sync_agent_for_next_turn();
  }
}

void AgentHarness::restore_session_entries(
    std::vector<SessionEntry> entries,
    std::optional<std::string> selected_leaf_id, bool reset_leaf_to_root) {
  if (phase_ != AgentHarnessPhase::idle) {
    throw std::logic_error("restore_session_entries() requires idle harness");
  }

  reset_model_context_projection();

  session_manager_ = SessionManager(std::move(entries));
  if (reset_leaf_to_root) {
    session_manager_.reset_leaf();
  } else if (selected_leaf_id.has_value() &&
             session_manager_.get_entry(*selected_leaf_id) != nullptr) {
    const auto status = session_manager_.branch(*selected_leaf_id);
    if (!status.ok()) {
      throw std::logic_error("Failed to restore harness session leaf");
    }
  }

  pending_session_entries_.clear();
  pending_session_transcript_messages_.clear();
  pending_session_entries_flushed_at_turn_end_ = false;
}

void AgentHarness::set_steering_mode(qt::agent::QueueMode mode) noexcept {
  config_.steering_mode = mode;
  agent_.set_steering_mode(mode);
}

void AgentHarness::set_follow_up_mode(qt::agent::QueueMode mode) noexcept {
  config_.follow_up_mode = mode;
  agent_.set_follow_up_mode(mode);
}

std::optional<std::string> AgentHarness::set_session_name(
    std::optional<std::string> name) {
  SessionEntry entry;
  entry.data = SessionInfoEntry{std::move(name), SessionUpstreamTreeFields{}};
  return persist_or_queue_session_entry(std::move(entry));
}

std::optional<std::string> AgentHarness::append_extension_state(
    std::string custom_type, Json data) {
  SessionEntry entry;
  entry.data = SessionExtensionStateEntry{
      std::move(custom_type), std::move(data), SessionUpstreamTreeFields{}};
  return persist_or_queue_session_entry(std::move(entry));
}

std::optional<std::string> AgentHarness::append_message(qt::ai::Message message) {
  auto entry = make_session_message_entry(message);
  if (!entry) {
    return std::nullopt;
  }

  if (phase_ == AgentHarnessPhase::running) {
    pending_session_entries_.push_back(std::move(*entry));
    pending_session_transcript_messages_.push_back(std::move(message));
    return std::nullopt;
  }

  agent_.state().transcript.push_back(message);
  return session_manager_.append_entry(std::move(*entry));
}

qt::agent::TurnResult AgentHarness::submit_prompt(std::string text) {
  return submit_prompt(make_user_message(std::move(text)));
}

qt::agent::TurnResult AgentHarness::submit_prompt(qt::ai::Message message) {
  require_can_submit();
  sync_agent_for_next_turn();
  refresh_applied_turn_snapshot();

  phase_ = AgentHarnessPhase::running;
  PhaseResetGuard phase_reset(phase_);

  drain_next_turn_queue();
  auto prefix_messages = apply_before_agent_start_hooks(message);
  const auto transcript_start = agent_.state().transcript.size();
  emit_subscribed_event(SubscribedEvent{AgentStartEvent{}});
  for (auto& prefix : prefix_messages) {
    agent_.enqueue(std::move(prefix), this);
  }
  const auto queued = agent_.enqueue(std::move(message), this);
  register_logical_input(queued, LogicalInputKind::prompt);
  qt::agent::TurnResult result = agent_.run_turn(*this);
  last_result_ = result;
  AgentEndEvent end_event;
  end_event.ok = result.ok;
  end_event.turn_index = result.turn_index;
  end_event.tool_request_count = result.tool_requests.size();
  end_event.tool_error_count = static_cast<std::size_t>(std::count_if(
      result.tool_results.begin(), result.tool_results.end(),
      [](const qt::tools::ToolResult& tool_result) {
        return tool_result.status != qt::tools::ToolResultStatus::success;
      }));
  end_event.error = result.error;
  if (result.request) {
    end_event.model = result.request->model;
  }
  if (result.response) {
    end_event.usage = result.response->usage;
  }
  const auto& transcript = agent_.state().transcript;
  if (transcript_start < transcript.size()) {
    end_event.messages.assign(transcript.begin() +
                                  static_cast<std::ptrdiff_t>(transcript_start),
                              transcript.end());
  }
  emit_subscribed_event(SubscribedEvent{std::move(end_event)});
  phase_ = AgentHarnessPhase::idle;
  record_settled();
  maybe_auto_compact_after_turn(result);
  return result;
}

qt::agent::TurnResult AgentHarness::submit_tool_call(
    std::string user_text, qt::tools::ToolRequest request) {
  if (user_text.empty()) {
    throw std::invalid_argument("submit_tool_call() requires user text");
  }
  if (request.tool_name.empty()) {
    throw std::invalid_argument("submit_tool_call() requires a tool name");
  }

  require_can_submit();
  sync_agent_for_next_turn();
  refresh_applied_turn_snapshot();

  if (!request.call_id || request.call_id->empty()) {
    request.call_id = "user-tool-" +
                      qt::foundation::generate_uuid_v4_string();
  }

  phase_ = AgentHarnessPhase::running;
  PhaseResetGuard phase_reset(phase_);

  drain_next_turn_queue();
  const auto transcript_start = agent_.state().transcript.size();
  emit_subscribed_event(SubscribedEvent{AgentStartEvent{}});
  const auto queued = agent_.enqueue(make_user_message(std::move(user_text)), this);
  register_logical_input(queued, LogicalInputKind::tool_call);

  std::shared_ptr<qt::foundation::ProcessCancellationToken> cancellation_token;
  {
    std::lock_guard<std::recursive_mutex> lock(queue_mutex_);
    current_logical_input_sequence_ = queued.sequence;
    current_logical_input_kind_ = LogicalInputKind::tool_call;
    cancellation_token = logical_input_cancellation_.at(queued.sequence);
  }

  qt::agent::TurnResult result = agent_.run_tool_call(
      std::move(request), std::move(cancellation_token), *this);
  last_result_ = result;

  AgentEndEvent end_event;
  end_event.ok = result.ok;
  end_event.turn_index = result.turn_index;
  end_event.tool_request_count = result.tool_requests.size();
  end_event.tool_error_count = static_cast<std::size_t>(std::count_if(
      result.tool_results.begin(), result.tool_results.end(),
      [](const qt::tools::ToolResult& tool_result) {
        return tool_result.status != qt::tools::ToolResultStatus::success;
      }));
  end_event.error = result.error;
  const auto& transcript = agent_.state().transcript;
  if (transcript_start < transcript.size()) {
    end_event.messages.assign(transcript.begin() +
                                  static_cast<std::ptrdiff_t>(transcript_start),
                              transcript.end());
  }
  emit_subscribed_event(SubscribedEvent{std::move(end_event)});
  phase_ = AgentHarnessPhase::idle;
  record_settled();
  return result;
}

qt::agent::QueuedMessage AgentHarness::steer(std::string text) {
  return steer(make_user_message(std::move(text)));
}

qt::agent::QueuedMessage AgentHarness::steer(qt::ai::Message message) {
  require_can_queue_runtime_message("steer");
  std::lock_guard<std::recursive_mutex> lock(queue_mutex_);
  auto queued = agent_.steer(std::move(message), this);
  register_logical_input(queued, LogicalInputKind::steer);
  steering_queue_.push_back(queued);
  record_queue_update();
  return queued;
}

qt::agent::QueuedMessage AgentHarness::follow_up(std::string text) {
  return follow_up(make_user_message(std::move(text)));
}

qt::agent::QueuedMessage AgentHarness::follow_up(qt::ai::Message message) {
  require_can_queue_runtime_message("follow_up");
  std::lock_guard<std::recursive_mutex> lock(queue_mutex_);
  auto queued = agent_.follow_up(std::move(message), this);
  register_logical_input(queued, LogicalInputKind::follow_up);
  follow_up_queue_.push_back(queued);
  record_queue_update();
  return queued;
}

void AgentHarness::next_turn(std::string text) {
  next_turn(make_user_message(std::move(text)));
}

void AgentHarness::next_turn(qt::ai::Message message) {
  if (phase_ == AgentHarnessPhase::aborted) {
    throw std::logic_error("Cannot queue next turn while harness is aborted");
  }
  std::lock_guard<std::recursive_mutex> lock(queue_mutex_);
  next_turn_queue_.push_back(std::move(message));
  record_queue_update();
}

AgentHarnessAbortEvent AgentHarness::request_abort() {
  if (phase_ == AgentHarnessPhase::running) {
    auto event = record_abort();
    {
      std::lock_guard<std::recursive_mutex> lock(queue_mutex_);
      if (current_logical_input_sequence_) {
        const auto token = logical_input_cancellation_.find(
            *current_logical_input_sequence_);
        if (token != logical_input_cancellation_.end() && token->second) {
          token->second->cancel();
        }
      }
    }
    agent_.request_abort();
    return event;
  }
  return AgentHarnessAbortEvent{};
}

qt::foundation::Result<ModelBackedCompactionResult>
AgentHarness::compact_session(ModelBackedCompactionRequest request) {
  if (phase_ != AgentHarnessPhase::idle) {
    return qt::foundation::Result<ModelBackedCompactionResult>::failure(
        qt::foundation::ErrorCode::invalid_argument,
        "compact_session() requires idle harness");
  }

  if (request.model.provider.empty() && request.model.api.empty() &&
      request.model.name.empty()) {
    request.model = config_.model;
  }
  if (!request.provider_options.api_key && !request.provider_options.timeout_ms &&
      !request.provider_options.max_retries &&
      !request.provider_options.max_retry_delay_ms &&
      !request.provider_options.proxy_url &&
      request.provider_options.headers.empty() &&
      request.provider_options.metadata.is_null()) {
    request.provider_options = options_;
  }
  if (request.model_max_output_tokens <= 0) {
    request.model_max_output_tokens =
        metadata_int64(request.provider_options.metadata, "maxTokens")
            .value_or(0);
  }

  phase_ = AgentHarnessPhase::compaction;
  PhaseResetGuard phase_reset(phase_);
  auto emit_compaction_end =
      [&](std::optional<ModelBackedCompactionResult> result,
          bool aborted,
          std::optional<std::string> error_message) {
        CompactionEndEvent event;
        event.reason = request.reason.empty() ? "manual" : request.reason;
        event.result = std::move(result);
        event.aborted = aborted;
        event.will_retry = request.will_retry;
        event.error_message = std::move(error_message);
        emit_subscribed_event(SubscribedEvent{std::move(event)});
      };

  emit_subscribed_event(SubscribedEvent{
      CompactionStartEvent{request.reason.empty() ? "manual" : request.reason}});

  const auto branch_entries = session_manager_.get_branch();
  auto plan = build_model_backed_compaction_plan(branch_entries, request);
  if (!plan) {
    emit_compaction_end(
        std::nullopt,
        false,
        "Compaction failed: " + plan.error().message);
    phase_ = AgentHarnessPhase::idle;
    return plan.error();
  }

  BeforeCompactEvent event;
  event.preparation = plan.value();
  event.branch_entries = branch_entries;
  event.custom_instructions = request.custom_instructions;
  const auto hook_result = apply_before_compact_hooks(event);
  if (hook_result && hook_result->cancel) {
    emit_compaction_end(std::nullopt, true, std::nullopt);
    phase_ = AgentHarnessPhase::idle;
    return qt::foundation::Result<ModelBackedCompactionResult>::failure(
        qt::foundation::ErrorCode::invalid_argument,
        "Compaction cancelled");
  }

  ModelBackedCompactionResult result;
  bool from_hook = false;
  if (hook_result && hook_result->compaction) {
    const auto& provided = *hook_result->compaction;
    result.summary = provided.summary;
    result.first_kept_entry_id = provided.first_kept_entry_id;
    result.tokens_before = provided.tokens_before;
    result.details = provided.details.value_or(Json::object());
    result.plan = plan.value();
    from_hook = true;
  } else {
    if (!provider_) {
      emit_compaction_end(std::nullopt,
                          false,
                          "Compaction failed: No provider available for "
                          "compaction");
      phase_ = AgentHarnessPhase::idle;
      return qt::foundation::Result<ModelBackedCompactionResult>::failure(
          qt::foundation::ErrorCode::invalid_argument,
          "No provider available for compaction");
    }
    auto compacted =
        compact_session_entries_with_model(branch_entries, request, *provider_);
    if (!compacted) {
      emit_compaction_end(
          std::nullopt,
          false,
          "Compaction failed: " + compacted.error().message);
      phase_ = AgentHarnessPhase::idle;
      return compacted.error();
    }
    result = compacted.value();
  }

  const auto entry_id = session_manager_.append_compaction(
      result.summary, result.first_kept_entry_id, result.tokens_before,
      result.details, from_hook);
  const auto* entry = session_manager_.get_entry(entry_id);
  if (entry != nullptr) {
    if (const auto* compaction =
            std::get_if<SessionCompactionEntry>(&entry->data);
        compaction != nullptr) {
      record_compact_event(entry_id, *compaction, from_hook);
    }
  }

  emit_compaction_end(result, false, std::nullopt);
  reset_model_context_projection();
  phase_ = AgentHarnessPhase::idle;
  return result;
}

qt::foundation::Result<ModelBackedCompactionResult>
AgentHarness::compact_session(std::string custom_instructions) {
  ModelBackedCompactionRequest request;
  request.custom_instructions = std::move(custom_instructions);
  return compact_session(std::move(request));
}

qt::foundation::Result<ModelBackedBranchSummaryResult>
AgentHarness::summarize_branch(const std::vector<SessionEntry>& entries,
                               ModelBackedBranchSummaryRequest request) {
  if (phase_ != AgentHarnessPhase::idle) {
    return qt::foundation::Result<ModelBackedBranchSummaryResult>::failure(
        qt::foundation::ErrorCode::invalid_argument,
        "summarize_branch() requires idle harness");
  }

  if (request.model.provider.empty() && request.model.api.empty() &&
      request.model.name.empty()) {
    request.model = config_.model;
  }
  if (!request.provider_options.api_key && !request.provider_options.timeout_ms &&
      !request.provider_options.max_retries &&
      !request.provider_options.max_retry_delay_ms &&
      !request.provider_options.proxy_url &&
      request.provider_options.headers.empty() &&
      request.provider_options.metadata.is_null()) {
    request.provider_options = options_;
  }
  if (!provider_) {
    return qt::foundation::Result<ModelBackedBranchSummaryResult>::failure(
        qt::foundation::ErrorCode::invalid_argument,
        "No provider available for branch summary");
  }

  phase_ = AgentHarnessPhase::branch_summary;
  PhaseResetGuard phase_reset(phase_);
  auto summarized =
      summarize_branch_entries_with_model(entries, request, *provider_);
  phase_ = AgentHarnessPhase::idle;
  return summarized;
}

void AgentHarness::emit_session_start(
    std::string reason, std::optional<std::string> previous_session_file) {
  reset_model_context_projection();
  SessionStartEvent event;
  event.reason = std::move(reason);
  event.previous_session_file = std::move(previous_session_file);
  session_start_events_.push_back(std::move(event));
  emit_subscribed_event(SubscribedEvent{session_start_events_.back()});
}

void AgentHarness::emit_session_shutdown(
    std::string reason, std::optional<std::string> target_session_file) {
  SessionShutdownEvent event;
  event.reason = std::move(reason);
  event.target_session_file = std::move(target_session_file);
  session_shutdown_events_.push_back(std::move(event));
  emit_subscribed_event(SubscribedEvent{session_shutdown_events_.back()});
}

void AgentHarness::emit_session_before_tree(qt::ai::Json preparation) {
  SessionBeforeTreeEvent event;
  event.preparation = std::move(preparation);
  session_before_tree_events_.push_back(std::move(event));
  emit_subscribed_event(SubscribedEvent{session_before_tree_events_.back()});
}

void AgentHarness::emit_session_tree(
    std::optional<std::string> new_leaf_id,
    std::optional<std::string> old_leaf_id,
    std::optional<qt::ai::Json> summary_entry,
    std::optional<bool> from_extension) {
  reset_model_context_projection();
  SessionTreeEvent event;
  event.new_leaf_id = std::move(new_leaf_id);
  event.old_leaf_id = std::move(old_leaf_id);
  event.summary_entry = std::move(summary_entry);
  event.from_extension = from_extension;
  session_tree_events_.push_back(std::move(event));
  emit_subscribed_event(SubscribedEvent{session_tree_events_.back()});
}

void AgentHarness::push(const qt::agent::AgentEvent& event) {
  update_model_context_input(event);
  events_.push_back(event);
  emit_subscribed_event(SubscribedEvent{events_.back()});
  bool needs_queue_update = false;
  {
    std::lock_guard<std::recursive_mutex> lock(queue_mutex_);
    if (event.kind == qt::agent::AgentEventKind::message_committed &&
        event.queued_message &&
        remove_runtime_queue_message(event.queued_message->sequence)) {
      needs_queue_update = true;
    }
    if (event.kind == qt::agent::AgentEventKind::turn_failed && event.error &&
        *event.error == kAbortError &&
        (!steering_queue_.empty() || !follow_up_queue_.empty())) {
      steering_queue_.clear();
      follow_up_queue_.clear();
      needs_queue_update = true;
    }
  }
  if (needs_queue_update) {
    record_queue_update();
  }
  record_committed_write(events_.back());
  if (listener_) {
    listener_(events_.back());
  }
  if (event.kind == qt::agent::AgentEventKind::turn_completed ||
      event.kind == qt::agent::AgentEventKind::turn_failed) {
    pending_session_entries_flushed_at_turn_end_ =
        !pending_session_entries_.empty();
    flush_pending_session_entries();
  }
  record_save_point(events_.back());
}

qt::ai::Message AgentHarness::make_user_message(std::string text) {
  qt::ai::Message message;
  message.role = qt::ai::MessageRole::user;
  message.content = {qt::ai::TextContent{std::move(text), std::nullopt}};
  return message;
}

qt::ai::CompletionRequest AgentHarness::apply_runtime_request_snapshot(
    const qt::ai::CompletionRequest& request) const {
  qt::ai::CompletionRequest transformed = request;
  transformed.model = applied_model_;
  transformed.metadata = applied_metadata_;
  transformed.tools = applied_tool_definitions_;
  return transformed;
}

qt::ai::CompletionRequest AgentHarness::apply_context_hooks(
    const qt::ai::CompletionRequest& request) {
  if (context_hooks_.empty()) {
    return request;
  }

  ContextEvent event;
  event.messages = request.messages;
  {
    std::lock_guard<std::recursive_mutex> lock(queue_mutex_);
    event.model_context_sequence = next_model_context_sequence_;
    event.projection_epoch = model_context_projection_epoch_;
    event.turn_index = current_model_context_turn_index_;
    event.logical_input_sequence = current_logical_input_sequence_;
    event.logical_input_kind = current_logical_input_kind_;
  }
  context_events_.push_back(event);

  std::vector<qt::ai::Message> current = request.messages;
  for (const auto& hook : context_hooks_) {
    context_events_.back().messages = current;
    const auto result = hook(context_events_.back());
    if (result) {
      current = *result;
    }
  }

  qt::ai::CompletionRequest transformed = request;
  transformed.messages = std::move(current);
  return transformed;
}

qt::ai::CompletionRequest AgentHarness::apply_model_context_augmentors(
    const qt::ai::CompletionRequest& request) {
  ModelContextEvent event;
  event.request = request;
  {
    std::lock_guard<std::recursive_mutex> lock(queue_mutex_);
    event.model_context_sequence = next_model_context_sequence_++;
    event.projection_epoch = model_context_projection_epoch_;
    event.turn_index = current_model_context_turn_index_;
    event.logical_input_sequence = current_logical_input_sequence_;
    event.logical_input_kind = current_logical_input_kind_;
    if (event.logical_input_sequence) {
      const auto token = logical_input_cancellation_.find(
          *event.logical_input_sequence);
      if (token != logical_input_cancellation_.end()) {
        event.cancellation_token = token->second;
      }
    }
    if (last_augmented_projection_epoch_ == event.projection_epoch &&
        last_augmented_logical_input_sequence_ ==
            event.logical_input_sequence) {
      ++call_index_for_logical_input_;
    } else {
      last_augmented_projection_epoch_ = event.projection_epoch;
      last_augmented_logical_input_sequence_ =
          event.logical_input_sequence;
      call_index_for_logical_input_ = 1U;
    }
    event.call_index_for_logical_input = call_index_for_logical_input_;
  }
  model_context_events_.push_back(event);

  qt::ai::CompletionRequest current = request;
  for (const auto& augmentor : model_context_augmentors_) {
    model_context_events_.back().request = current;
    const auto replacement = augmentor(model_context_events_.back());
    if (replacement) {
      current = *replacement;
    }
  }
  model_context_events_.back().request = current;
  return current;
}

qt::ai::ProviderOptions AgentHarness::apply_before_provider_request_hooks(
    const qt::ai::CompletionRequest& request,
    const qt::ai::ProviderOptions& options) {
  qt::ai::ProviderOptions current = options;
  for (const auto& hook : before_provider_request_hooks_) {
    BeforeProviderRequestEvent event;
    event.model = request.model;
    event.request = request;
    event.stream_options = current;
    before_provider_request_events_.push_back(event);

    const auto patch = hook(before_provider_request_events_.back());
    if (patch) {
      merge_provider_options(current, *patch);
    }
  }
  return current;
}

qt::ai::CompletionRequest AgentHarness::apply_before_provider_payload_hooks(
    const qt::ai::CompletionRequest& request) {
  if (before_provider_payload_hooks_.empty()) {
    return request;
  }

  qt::ai::Json current = request;
  for (const auto& hook : before_provider_payload_hooks_) {
    BeforeProviderPayloadEvent event;
    event.model = request.model;
    event.payload = current;
    before_provider_payload_events_.push_back(event);

    const auto replacement = hook(before_provider_payload_events_.back());
    if (replacement) {
      current = *replacement;
    }
  }

  return current.get<qt::ai::CompletionRequest>();
}

void AgentHarness::record_after_provider_response(
    const qt::ai::CompletionRequest& request,
    const qt::ai::ProviderOptions& options,
    const qt::ai::CompletionResponse& response) {
  AfterProviderResponseEvent event;
  event.model = request.model;
  event.request = request;
  event.stream_options = options;
  event.response = response;
  after_provider_response_events_.push_back(event);

  for (const auto& hook : after_provider_response_hooks_) {
    hook(after_provider_response_events_.back());
  }
}

void AgentHarness::record_tool_call(
    const qt::agent::ToolHookContext& context,
    const qt::tools::ToolRequest& request) {
  ToolCallEvent event;
  event.turn_index = context.turn_index;
  event.tool_call_index = context.tool_call_index;
  event.completed_tool_calls_before = context.completed_tool_calls_before;
  event.request = request;
  tool_call_events_.push_back(event);

  for (const auto& hook : tool_call_hooks_) {
    hook(tool_call_events_.back());
  }
}

void AgentHarness::record_tool_result(
    const qt::agent::ToolHookContext& context,
    const qt::tools::ToolRequest& request,
    const qt::tools::ToolResult& result) {
  ToolResultEvent event;
  event.turn_index = context.turn_index;
  event.tool_call_index = context.tool_call_index;
  event.completed_tool_calls_before = context.completed_tool_calls_before;
  event.request = request;
  event.result = result;
  tool_result_events_.push_back(event);

  for (const auto& hook : tool_result_hooks_) {
    hook(tool_result_events_.back());
  }
}

std::optional<AgentHarness::BeforeCompactResult>
AgentHarness::apply_before_compact_hooks(const BeforeCompactEvent& event) {
  before_compact_events_.push_back(event);
  emit_subscribed_event(SubscribedEvent{before_compact_events_.back()});

  std::optional<BeforeCompactResult> current;
  for (const auto& hook : before_compact_hooks_) {
    const auto result = hook(before_compact_events_.back());
    if (!result) {
      continue;
    }
    current = *result;
    if (current->cancel) {
      break;
    }
  }
  return current;
}

void AgentHarness::record_compact_event(
    const std::string& entry_id,
    const SessionCompactionEntry& compaction,
    bool from_hook) {
  CompactEvent event;
  event.entry_id = entry_id;
  event.compaction = compaction;
  event.from_hook = from_hook;
  compact_events_.push_back(std::move(event));
  emit_subscribed_event(SubscribedEvent{compact_events_.back()});
}

void AgentHarness::maybe_auto_compact_after_turn(
    const qt::agent::TurnResult& result) {
  if (!result.ok || !result.response.has_value()) {
    return;
  }
  if (result.response->stop_reason == qt::ai::StopReason::aborted ||
      result.response->stop_reason == qt::ai::StopReason::error) {
    return;
  }

  const auto compaction_settings =
      metadata_object(config_.metadata, "compaction");
  const auto enabled =
      metadata_bool(compaction_settings.value_or(qt::ai::Json::object()),
                    "enabled")
          .value_or(metadata_bool(config_.metadata, "autoCompactionEnabled")
                        .value_or(false));
  if (!enabled) {
    return;
  }

  auto context_window = metadata_int64(options_.metadata, "contextWindow");
  if (!context_window.has_value()) {
    context_window = metadata_int64(config_.metadata, "contextWindow");
  }
  if (!context_window.has_value() || *context_window <= 0) {
    return;
  }

  const auto settings =
      compaction_settings.value_or(qt::ai::Json::object());
  const std::int64_t reserve_tokens =
      metadata_int64(settings, "reserveTokens")
          .value_or(metadata_int64(config_.metadata,
                                   "autoCompactionReserveTokens")
                        .value_or(16384));
  const std::int64_t keep_recent_tokens =
      metadata_int64(settings, "keepRecentTokens")
          .value_or(metadata_int64(config_.metadata,
                                   "autoCompactionKeepRecentTokens")
                        .value_or(20000));
  const std::int64_t context_tokens =
      usage_context_tokens(result.response->usage);
  if (context_tokens <= *context_window - reserve_tokens) {
    return;
  }

  ModelBackedCompactionRequest request;
  request.model = config_.model;
  request.provider_options = options_;
  request.reason = "threshold";
  request.reserve_tokens = reserve_tokens;
  request.keep_recent_tokens = keep_recent_tokens;
  (void)compact_session(std::move(request));
}

void AgentHarness::emit_subscribed_event(const SubscribedEvent& event) {
  const auto subscribers = subscribers_;
  for (const auto& entry : subscribers) {
    entry.second(event);
  }
}

void AgentHarness::require_can_submit() const {
  if (phase_ == AgentHarnessPhase::running) {
    throw std::logic_error("AgentHarness is busy");
  }
  if (phase_ == AgentHarnessPhase::compaction) {
    throw std::logic_error("AgentHarness is compacting");
  }
  if (phase_ == AgentHarnessPhase::branch_summary) {
    throw std::logic_error("AgentHarness is summarizing a branch");
  }
  if (phase_ == AgentHarnessPhase::aborted) {
    throw std::logic_error("AgentHarness is aborted");
  }
}

std::vector<qt::ai::Message> AgentHarness::apply_before_agent_start_hooks(
    const qt::ai::Message& prompt) {
  if (before_agent_start_hooks_.empty()) {
    return {};
  }

  BeforeAgentStartEvent event;
  event.prompt = prompt;
  event.resources = resources_;
  before_agent_start_events_.push_back(event);
  emit_subscribed_event(SubscribedEvent{before_agent_start_events_.back()});

  std::vector<qt::ai::Message> messages;
  for (const auto& hook : before_agent_start_hooks_) {
    const auto result = hook(before_agent_start_events_.back());
    if (result) {
      messages = result->messages;
    }
  }
  return messages;
}

void AgentHarness::initialize_cached_agent_hooks() {
  harness_before_tool_hook_ =
      [this](const qt::agent::ToolHookContext& context,
             const qt::tools::ToolRequest& request)
      -> std::optional<qt::tools::ToolResult> {
    record_tool_call(context, request);
    return std::nullopt;
  };
  harness_after_tool_hook_ =
      [this](const qt::agent::ToolHookContext& context,
             const qt::tools::ToolRequest& request,
             const qt::tools::ToolResult& result) {
    record_tool_result(context, request, result);
  };
  harness_prepare_next_turn_hook_ =
      [this]() { apply_pending_runtime_updates_at_save_point(); };
  invalidate_agent_config();
}

void AgentHarness::invalidate_agent_config() noexcept {
  agent_config_dirty_ = true;
}

void AgentHarness::invalidate_filtered_tool_definitions() noexcept {
  filtered_tool_definitions_dirty_ = true;
  invalidate_agent_config();
}

void AgentHarness::sync_agent_for_next_turn() {
  if (!agent_config_dirty_) {
    return;
  }

  effective_config_ = config_;
  effective_config_.tools = filtered_tool_definitions();
  effective_config_.before_tool_hooks.push_back(harness_before_tool_hook_);
  effective_config_.after_tool_hooks.push_back(harness_after_tool_hook_);
  effective_config_.prepare_next_turn_hook = harness_prepare_next_turn_hook_;
  qt::agent::Agent replacement(effective_config_, routed_provider_, options_);
  replacement.state() = std::move(agent_.state());
  agent_ = std::move(replacement);
  agent_config_dirty_ = false;
}

void AgentHarness::apply_pending_runtime_updates_at_save_point() {
  flush_pending_session_entries();
  if (pending_stream_options_) {
    agent_.set_provider_options(*pending_stream_options_);
    pending_stream_options_.reset();
  }
  refresh_applied_turn_snapshot();
}

void AgentHarness::refresh_applied_turn_snapshot() {
  applied_model_ = config_.model;
  applied_metadata_ = config_.metadata;
  applied_tool_definitions_ = filtered_tool_definitions();
}

std::optional<std::string> AgentHarness::persist_or_queue_session_entry(
    SessionEntry entry) {
  if (phase_ == AgentHarnessPhase::running) {
    pending_session_entries_.push_back(std::move(entry));
    return std::nullopt;
  }
  return session_manager_.append_entry(std::move(entry));
}

void AgentHarness::flush_pending_session_entries() {
  if (pending_session_entries_.empty() &&
      pending_session_transcript_messages_.empty()) {
    return;
  }

  auto pending = std::move(pending_session_entries_);
  pending_session_entries_.clear();
  for (auto& entry : pending) {
    session_manager_.append_entry(std::move(entry));
  }

  auto pending_messages = std::move(pending_session_transcript_messages_);
  pending_session_transcript_messages_.clear();
  for (auto& message : pending_messages) {
    agent_.state().transcript.push_back(std::move(message));
  }
}

void AgentHarness::record_committed_session_entry(const qt::ai::Message& message) {
  auto entry = make_session_message_entry(message);
  if (!entry) {
    return;
  }
  session_manager_.append_entry(std::move(*entry));
}

std::optional<SessionEntry> AgentHarness::make_session_message_entry(
    const qt::ai::Message& message) const {
  switch (message.role) {
    case qt::ai::MessageRole::user: {
      SessionUserImageMessageEntry entry;
      entry.content = to_session_user_blocks(message.content);
      entry.serialize_text_content_as_string =
          entry.content.size() == 1U &&
          entry.content.front().kind == SessionUserContentBlockKind::kText &&
          !entry.content.front().text_signature.has_value();
      SessionEntry session_entry;
      session_entry.data = std::move(entry);
      return session_entry;
    }
    case qt::ai::MessageRole::assistant: {
      SessionAssistantMessageEntry entry;
      entry.content = to_session_assistant_blocks(message.content);
      entry.api = config_.model.api;
      entry.provider = config_.model.provider;
      entry.model = config_.model.name;
      entry.response_id = message.id;
      if (message.metadata.contains("usage") &&
          message.metadata.at("usage").is_object()) {
        entry.usage = message.metadata.at("usage");
      }
      entry.stop_reason =
          message.metadata.value("stopReason", std::string{"unknown"});
      SessionEntry session_entry;
      session_entry.data = std::move(entry);
      return session_entry;
    }
    case qt::ai::MessageRole::tool: {
      SessionEntry session_entry;
      session_entry.data = to_session_tool_result_entry(message);
      return session_entry;
    }
    case qt::ai::MessageRole::system:
    case qt::ai::MessageRole::developer:
      return std::nullopt;
  }
  return std::nullopt;
}

void AgentHarness::apply_thinking_level_metadata() {
  if (!config_.metadata.is_object()) {
    config_.metadata = qt::ai::Json::object();
  }
  config_.metadata["thinkingLevel"] = thinking_level_;
}

const std::vector<qt::ai::ToolDefinition>&
AgentHarness::filtered_tool_definitions() {
  if (!filtered_tool_definitions_dirty_) {
    return filtered_tool_definitions_cache_;
  }

  filtered_tool_definitions_cache_.clear();
  if (active_tool_names_.empty()) {
    filtered_tool_definitions_dirty_ = false;
    return filtered_tool_definitions_cache_;
  }

  filtered_tool_definitions_cache_.reserve(active_tool_names_.size());
  for (const auto& name : active_tool_names_) {
    const auto match = std::find_if(
        tool_definitions_.begin(), tool_definitions_.end(),
        [&name](const qt::ai::ToolDefinition& tool) {
          return tool.name == name;
        });
    if (match != tool_definitions_.end()) {
      filtered_tool_definitions_cache_.push_back(*match);
    }
  }
  filtered_tool_definitions_dirty_ = false;
  return filtered_tool_definitions_cache_;
}

void AgentHarness::validate_active_tool_names(
    const std::vector<std::string>& active_tool_names) const {
  for (const auto& name : active_tool_names) {
    const auto match = std::find_if(
        tool_definitions_.begin(), tool_definitions_.end(),
        [&name](const qt::ai::ToolDefinition& tool) {
          return tool.name == name;
        });
    if (match == tool_definitions_.end()) {
      throw std::logic_error("Unknown AgentHarness tool: " + name);
    }
  }
}

void AgentHarness::require_can_queue_runtime_message(
    const char* operation) const {
  if (phase_ != AgentHarnessPhase::running) {
    throw std::logic_error(std::string{"Cannot "} + operation +
                           " while harness is not running");
  }
}

bool AgentHarness::remove_runtime_queue_message(std::uint64_t sequence) {
  const auto remove_from = [sequence](auto& queue) {
    const auto it = std::find_if(
        queue.begin(), queue.end(),
        [sequence](const qt::agent::QueuedMessage& queued) {
          return queued.sequence == sequence;
        });
    if (it == queue.end()) {
      return false;
    }
    queue.erase(it);
    return true;
  };
  return remove_from(steering_queue_) || remove_from(follow_up_queue_);
}

void AgentHarness::record_queue_update() {
  AgentHarnessQueueUpdate event;
  {
    std::lock_guard<std::recursive_mutex> lock(queue_mutex_);
    event.steer = queue_messages(steering_queue_);
    event.follow_up = queue_messages(follow_up_queue_);
    event.next_turn = next_turn_queue_;
  }
  queue_update_events_.push_back(std::move(event));
  emit_subscribed_event(SubscribedEvent{queue_update_events_.back()});
}

void AgentHarness::drain_next_turn_queue() {
  std::vector<qt::ai::Message> drained;
  {
    std::lock_guard<std::recursive_mutex> lock(queue_mutex_);
    if (next_turn_queue_.empty()) {
      return;
    }
    drained = std::move(next_turn_queue_);
    next_turn_queue_.clear();
  }
  for (auto& message : drained) {
    const auto queued = agent_.enqueue(std::move(message), this);
    register_logical_input(queued, LogicalInputKind::next_turn);
  }
  record_queue_update();
}

void AgentHarness::register_logical_input(
    const qt::agent::QueuedMessage& queued, LogicalInputKind kind) {
  if (queued.message.role != qt::ai::MessageRole::user) {
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(queue_mutex_);
  logical_input_kinds_[queued.sequence] = kind;
  logical_input_cancellation_[queued.sequence] =
      std::make_shared<qt::foundation::ProcessCancellationToken>();
}

void AgentHarness::update_model_context_input(
    const qt::agent::AgentEvent& event) {
  std::lock_guard<std::recursive_mutex> lock(queue_mutex_);
  current_model_context_turn_index_ = event.turn_index;

  if (event.kind == qt::agent::AgentEventKind::provider_request) {
    const auto& pending = agent_.state().pending;
    for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
      if (it->message.role != qt::ai::MessageRole::user) {
        continue;
      }
      current_logical_input_sequence_ = it->sequence;
      const auto kind = logical_input_kinds_.find(it->sequence);
      current_logical_input_kind_ =
          kind == logical_input_kinds_.end() ? LogicalInputKind::unknown
                                             : kind->second;
      break;
    }
    return;
  }

  if (event.kind == qt::agent::AgentEventKind::message_committed &&
      event.queued_message && event.committed_message &&
      event.committed_message->role == qt::ai::MessageRole::user) {
    current_logical_input_sequence_ = event.queued_message->sequence;
    const auto kind = logical_input_kinds_.find(*current_logical_input_sequence_);
    current_logical_input_kind_ =
        kind == logical_input_kinds_.end() ? LogicalInputKind::unknown
                                           : kind->second;
  }
}

void AgentHarness::record_committed_write(
    const qt::agent::AgentEvent& event) {
  if (event.kind != qt::agent::AgentEventKind::message_committed ||
      !event.committed_message) {
    return;
  }

  AgentHarnessWrite write;
  write.kind = AgentHarnessWriteKind::committed_message;
  if (event.queued_message) {
    write.sequence = event.queued_message->sequence;
  }
  write.message = *event.committed_message;
  committed_writes_.push_back(std::move(write));
  record_committed_session_entry(*event.committed_message);
}

void AgentHarness::record_save_point(const qt::agent::AgentEvent& event) {
  if (event.kind != qt::agent::AgentEventKind::turn_completed &&
      event.kind != qt::agent::AgentEventKind::turn_failed) {
    return;
  }

  const auto writes = session_writes();
  const auto pending = pending_writes();
  const bool had_pending_session_mutations =
      pending_session_entries_flushed_at_turn_end_;
  pending_session_entries_flushed_at_turn_end_ = false;
  const auto* previous = last_save_point();
  const bool writes_changed =
      previous == nullptr ? !writes.empty()
                          : !writes_equal(previous->writes, writes);
  if (event.kind == qt::agent::AgentEventKind::turn_failed &&
      !writes_changed && !had_pending_session_mutations) {
    return;
  }

  AgentHarnessSavePoint save_point;
  save_point.sequence = static_cast<std::uint64_t>(save_points_.size() + 1U);
  save_point.turn_index = event.turn_index;
  save_point.had_pending_writes = !pending.empty();
  save_point.writes = writes;
  AgentHarnessSavePointEvent own_event;
  own_event.had_pending_mutations =
      save_point.had_pending_writes || had_pending_session_mutations;
  save_points_.push_back(std::move(save_point));
  emit_subscribed_event(SubscribedEvent{own_event});
}

AgentHarnessAbortEvent AgentHarness::record_abort() {
  AgentHarnessAbortEvent event;
  event.cleared_steer = queue_messages(steering_queue_);
  event.cleared_follow_up = queue_messages(follow_up_queue_);

  const bool had_runtime_queues =
      !steering_queue_.empty() || !follow_up_queue_.empty();
  steering_queue_.clear();
  follow_up_queue_.clear();
  agent_.clear_all_queues();
  if (had_runtime_queues) {
    record_queue_update();
  }
  emit_subscribed_event(SubscribedEvent{event});
  return event;
}

void AgentHarness::record_settled() {
  AgentHarnessSettledEvent event;
  event.next_turn_count = next_turn_queue_.size();
  emit_subscribed_event(SubscribedEvent{event});
}

const char* to_string(AgentHarnessPhase value) noexcept {
  switch (value) {
    case AgentHarnessPhase::idle:
      return "idle";
    case AgentHarnessPhase::running:
      return "running";
    case AgentHarnessPhase::compaction:
      return "compaction";
    case AgentHarnessPhase::branch_summary:
      return "branch_summary";
    case AgentHarnessPhase::aborted:
      return "aborted";
  }
  return "unknown";
}

const char* to_string(AgentHarnessWriteKind value) noexcept {
  switch (value) {
    case AgentHarnessWriteKind::committed_message:
      return "committed_message";
    case AgentHarnessWriteKind::pending_message:
      return "pending_message";
  }
  return "unknown";
}

const char* to_string(LogicalInputKind value) noexcept {
  switch (value) {
    case LogicalInputKind::unknown:
      return "unknown";
    case LogicalInputKind::prompt:
      return "prompt";
    case LogicalInputKind::tool_call:
      return "tool_call";
    case LogicalInputKind::steer:
      return "steer";
    case LogicalInputKind::follow_up:
      return "follow_up";
    case LogicalInputKind::next_turn:
      return "next_turn";
  }
  return "unknown";
}

}  // namespace qt::harness
