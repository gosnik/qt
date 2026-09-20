#include "qt/ai/provider.hpp"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <utility>

namespace qt::ai {
namespace {

std::optional<std::string> faux_string_metadata(const Json& metadata,
                                                const std::string& key) {
  if (!metadata.is_object()) {
    return std::nullopt;
  }
  const auto it = metadata.find(key);
  if (it == metadata.end() || !it->is_string()) {
    return std::nullopt;
  }
  const auto value = it->get<std::string>();
  return value.empty() ? std::optional<std::string>{} : value;
}

std::string faux_cache_retention(const Json& metadata) {
  if (const auto value = faux_string_metadata(metadata, "cacheRetention")) {
    if (*value == "none" || *value == "short" || *value == "long") {
      return *value;
    }
  }
  return "short";
}

std::size_t common_prefix_length(const std::string& a, const std::string& b) {
  const auto limit = std::min(a.size(), b.size());
  std::size_t index = 0;
  while (index < limit && a[index] == b[index]) {
    ++index;
  }
  return index;
}

std::string faux_prompt_cache_text(const CompletionRequest& request) {
  auto normalized_request = normalized(request);
  normalized_request.metadata = Json::object();
  Json json = normalized_request;
  return json.dump();
}

}  // namespace

void Provider::stream(const CompletionRequest& request,
                      const ProviderOptions& options, EventSink& sink) {
  (void)options;

  StreamEvent start;
  start.kind = EventKind::message_start;
  start.role = MessageRole::assistant;
  sink.push(start);

  const auto response = complete(request, options);

  for (const auto& item : response.message.content) {
    StreamEvent event;
    event.kind = content_kind(item) == ContentKind::tool_call
                     ? EventKind::tool_call
                     : content_kind(item) == ContentKind::tool_result
                           ? EventKind::tool_result
                           : EventKind::content_done;
    event.content = item;
    sink.push(event);
  }

  StreamEvent usage_event;
  usage_event.kind = EventKind::usage;
  usage_event.usage = response.usage;
  sink.push(usage_event);

  StreamEvent done;
  done.kind = EventKind::message_done;
  done.stop_reason = response.stop_reason;
  if (response.message.id) {
    done.message_id = response.message.id;
  }
  sink.push(done);
}

ScriptedProvider::ScriptedProvider(ProviderDescriptor descriptor)
    : descriptor_(std::move(descriptor)) {
  if (descriptor_.id.empty()) {
    descriptor_.id = "scripted";
  }
  if (descriptor_.display_name.empty()) {
    descriptor_.display_name = "Scripted Provider";
  }
  if (descriptor_.api.empty()) {
    descriptor_.api = "test-api";
  }
}

const ProviderDescriptor& ScriptedProvider::descriptor() const {
  return descriptor_;
}

CompletionResponse ScriptedProvider::complete(const CompletionRequest& request,
                                              const ProviderOptions& options) {
  requests_.push_back(request);
  options_history_.push_back(options);

  if (next_step_index_ >= steps_.size()) {
    throw std::runtime_error("scripted provider has no remaining steps");
  }

  const ScriptedProviderStep& step = steps_[next_step_index_++];
  if (step.error) {
    throw std::runtime_error(*step.error);
  }

  assert(step.response.has_value());
  return *step.response;
}

void ScriptedProvider::push_response(CompletionResponse response) {
  steps_.push_back(ScriptedProviderStep{std::move(response), std::nullopt});
}

void ScriptedProvider::push_error(std::string error) {
  steps_.push_back(ScriptedProviderStep{std::nullopt, std::move(error)});
}

const std::vector<CompletionRequest>& ScriptedProvider::requests() const noexcept {
  return requests_;
}

const std::vector<ProviderOptions>& ScriptedProvider::options_history() const noexcept {
  return options_history_;
}

std::size_t ScriptedProvider::pending_steps() const noexcept {
  return steps_.size() - next_step_index_;
}

std::size_t ScriptedProvider::completed_steps() const noexcept {
  return next_step_index_;
}

FauxProvider::FauxProvider(ProviderDescriptor descriptor)
    : descriptor_(std::move(descriptor)) {
  if (descriptor_.id.empty()) {
    descriptor_.id = "faux";
  }
  if (descriptor_.display_name.empty()) {
    descriptor_.display_name = "Faux Provider";
  }
  if (descriptor_.api.empty()) {
    descriptor_.api = "faux";
  }
  descriptor_.capabilities.streaming = true;
  descriptor_.capabilities.tool_calls = true;
  descriptor_.capabilities.vision = true;
}

const ProviderDescriptor& FauxProvider::descriptor() const {
  return descriptor_;
}

CompletionResponse FauxProvider::complete(const CompletionRequest& request,
                                          const ProviderOptions& options) {
  ++state_.call_count;

  if (next_step_index_ >= steps_.size()) {
    return exhausted_response(request, options);
  }

  const FauxProviderStep& step = steps_[next_step_index_++];
  CompletionResponse response;
  if (step.factory) {
    response = step.factory(request, options, state_);
  } else {
    assert(step.response.has_value());
    response = *step.response;
  }

  if (options.aborted) {
    return aborted_response(std::move(response), request, options);
  }

  return finalize_response(std::move(response), request, options);
}

void FauxProvider::stream(const CompletionRequest& request,
                          const ProviderOptions& options, EventSink& sink) {
  const auto response = complete(request, options);

  if (response.stop_reason != StopReason::aborted) {
    StreamEvent start;
    start.kind = EventKind::message_start;
    start.role = MessageRole::assistant;
    if (response.message.id) {
      start.message_id = response.message.id;
    }
    sink.push(start);
  }

  for (std::size_t index = 0; index < response.message.content.size();
       ++index) {
    const auto& item = response.message.content[index];
    const auto kind = content_kind(item);

    if (kind == ContentKind::text) {
      const auto& text = std::get<TextContent>(item);

      StreamEvent delta;
      delta.kind = EventKind::content_delta;
      delta.content = item;
      delta.delta = text.text;
      delta.metadata = Json{{"content_index", index}, {"content_type", "text"}};
      sink.push(delta);

      StreamEvent done;
      done.kind = EventKind::content_done;
      done.content = item;
      done.metadata = Json{{"content_index", index}, {"content_type", "text"}};
      sink.push(done);
      continue;
    }

    if (kind == ContentKind::thinking) {
      const auto& thinking = std::get<ThinkingContent>(item);

      StreamEvent delta;
      delta.kind = EventKind::content_delta;
      delta.content = item;
      delta.delta = thinking.text;
      delta.metadata =
          Json{{"content_index", index}, {"content_type", "thinking"}};
      sink.push(delta);

      StreamEvent done;
      done.kind = EventKind::content_done;
      done.content = item;
      done.metadata =
          Json{{"content_index", index}, {"content_type", "thinking"}};
      sink.push(done);
      continue;
    }

    if (kind == ContentKind::tool_call) {
      const auto& call = std::get<ToolCallContent>(item);

      StreamEvent args_delta;
      args_delta.kind = EventKind::content_delta;
      args_delta.delta = call.arguments.dump();
      args_delta.metadata = Json{{"content_index", index},
                                 {"content_type", "tool_call_arguments"},
                                 {"tool_call_id", call.id}};
      sink.push(args_delta);

      StreamEvent tool_call;
      tool_call.kind = EventKind::tool_call;
      tool_call.content = item;
      tool_call.metadata = Json{{"content_index", index}};
      sink.push(tool_call);
      continue;
    }

    StreamEvent event;
    event.kind = kind == ContentKind::tool_result ? EventKind::tool_result
                                                  : EventKind::content_done;
    event.content = item;
    event.metadata = Json{{"content_index", index}};
    sink.push(event);
  }

  StreamEvent usage_event;
  usage_event.kind = EventKind::usage;
  usage_event.usage = response.usage;
  sink.push(usage_event);

  if (response.stop_reason == StopReason::error ||
      response.stop_reason == StopReason::aborted) {
    StreamEvent error_event;
    error_event.kind = EventKind::error;
    error_event.stop_reason = response.stop_reason;
    if (response.metadata.contains("errorMessage") &&
        response.metadata.at("errorMessage").is_string()) {
      error_event.error = response.metadata.at("errorMessage").get<std::string>();
      error_event.metadata["errorMessage"] = *error_event.error;
    } else {
      error_event.error = to_string(response.stop_reason);
    }
    sink.push(error_event);
  }

  StreamEvent done;
  done.kind = EventKind::message_done;
  done.stop_reason = response.stop_reason;
  if (response.message.id) {
    done.message_id = response.message.id;
  }
  sink.push(done);
}

void FauxProvider::set_responses(std::vector<CompletionResponse> responses) {
  steps_.clear();
  next_step_index_ = 0;
  append_responses(std::move(responses));
}

void FauxProvider::append_responses(std::vector<CompletionResponse> responses) {
  for (auto& response : responses) {
    push_response(std::move(response));
  }
}

void FauxProvider::push_response(CompletionResponse response) {
  steps_.push_back(FauxProviderStep{std::move(response), {}});
}

void FauxProvider::push_factory(FauxResponseFactory factory) {
  steps_.push_back(FauxProviderStep{std::nullopt, std::move(factory)});
}

std::size_t FauxProvider::pending_response_count() const noexcept {
  return steps_.size() - next_step_index_;
}

const FauxProviderState& FauxProvider::state() const noexcept {
  return state_;
}

CompletionResponse FauxProvider::finalize_response(
    CompletionResponse response, const CompletionRequest& request,
    const ProviderOptions& options) {
  response.message.role = MessageRole::assistant;
  response.metadata["api"] = descriptor_.api;
  response.metadata["provider"] = descriptor_.id;
  response.metadata["model"] = request.model.name;
  response.response_model = request.model.name;

  const auto session_id = faux_string_metadata(options.metadata, "sessionId");
  const bool cache_enabled =
      session_id.has_value() &&
      faux_cache_retention(options.metadata) != "none";
  if (cache_enabled) {
    const auto prompt_text = faux_prompt_cache_text(request);
    const auto prompt_tokens = estimate_token_count(prompt_text);
    const auto previous = prompt_cache_.find(*session_id);
    if (previous == prompt_cache_.end()) {
      response.usage.input_tokens = prompt_tokens;
      response.usage.cache_read_tokens = 0;
      response.usage.cache_write_tokens = prompt_tokens;
    } else {
      const auto cached_chars =
          common_prefix_length(previous->second, prompt_text);
      response.usage.cache_read_tokens =
          estimate_token_count(std::string_view(previous->second).substr(
              0, cached_chars));
      response.usage.cache_write_tokens =
          estimate_token_count(std::string_view(prompt_text).substr(
              cached_chars));
      response.usage.input_tokens =
          std::max<std::int64_t>(
              0, prompt_tokens - response.usage.cache_read_tokens);
    }
    prompt_cache_[*session_id] = prompt_text;
  } else {
    response.usage.input_tokens = estimate_token_count(request);
    response.usage.cache_read_tokens = 0;
    response.usage.cache_write_tokens = 0;
  }
  response.usage.output_tokens = estimate_token_count(response.message);
  response.usage.input_cost_usd = 0.0;
  response.usage.output_cost_usd = 0.0;
  response.usage.cache_read_cost_usd = 0.0;
  response.usage.cache_write_cost_usd = 0.0;
  normalize(response.usage);

  return response;
}

CompletionResponse FauxProvider::exhausted_response(
    const CompletionRequest& request, const ProviderOptions& options) {
  CompletionResponse response;
  response.message.role = MessageRole::assistant;
  response.stop_reason = StopReason::error;
  response.metadata["errorMessage"] = "No more faux responses queued";
  return finalize_response(std::move(response), request, options);
}

CompletionResponse FauxProvider::aborted_response(
    CompletionResponse response, const CompletionRequest& request,
    const ProviderOptions& options) {
  response.message.content.clear();
  response.message.role = MessageRole::assistant;
  response.stop_reason = StopReason::aborted;
  response.metadata["errorMessage"] = "Request was aborted";
  return finalize_response(std::move(response), request, options);
}

bool ProviderRegistry::register_provider(std::unique_ptr<Provider> provider) {
  if (!provider) {
    return false;
  }

  const auto& id = provider->descriptor().id;
  if (id.empty()) {
    return false;
  }

  return providers_.emplace(id, std::move(provider)).second;
}

bool ProviderRegistry::unregister_provider(const std::string& id) {
  if (id.empty()) {
    return false;
  }
  return providers_.erase(id) > 0U;
}

const Provider* ProviderRegistry::find(const std::string& id) const {
  const auto iterator = providers_.find(id);
  if (iterator == providers_.end()) {
    return nullptr;
  }

  return iterator->second.get();
}

std::vector<ProviderDescriptor> ProviderRegistry::descriptors() const {
  std::vector<ProviderDescriptor> result;
  result.reserve(providers_.size());

  for (const auto& entry : providers_) {
    result.push_back(entry.second->descriptor());
  }

  return result;
}

std::size_t ProviderRegistry::size() const noexcept { return providers_.size(); }

void VectorEventSink::push(const StreamEvent& event) { events_.push_back(event); }

const std::vector<StreamEvent>& VectorEventSink::events() const noexcept {
  return events_;
}

}  // namespace qt::ai
