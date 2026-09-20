#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "qt/ai/models.hpp"

namespace qt::ai {

struct ProviderCapabilities {
  bool streaming = true;
  bool tool_calls = true;
  bool vision = false;
  bool reasoning = false;
  bool session_caching = false;
};

struct ProviderOptions {
  std::optional<std::string> api_key;
  std::optional<std::int64_t> timeout_ms;
  std::optional<int> max_retries;
  std::optional<std::int64_t> max_retry_delay_ms;
  std::optional<std::string> proxy_url;
  std::shared_ptr<std::atomic_bool> abort_signal;
  bool aborted = false;
  std::map<std::string, std::string> headers;
  Json metadata = Json::object();
};

struct ProviderDescriptor {
  std::string id;
  std::string display_name;
  std::string api;
  ProviderTransport transport = ProviderTransport::auto_detect;
  ProviderCapabilities capabilities;
};

class EventSink {
 public:
  virtual ~EventSink() = default;
  virtual void push(const StreamEvent& event) = 0;
};

class Provider {
 public:
  virtual ~Provider() = default;

  virtual const ProviderDescriptor& descriptor() const = 0;
  virtual CompletionResponse complete(const CompletionRequest& request,
                                      const ProviderOptions& options) = 0;

  virtual void stream(const CompletionRequest& request, const ProviderOptions& options,
                      EventSink& sink);
};

struct ScriptedProviderStep {
  std::optional<CompletionResponse> response;
  std::optional<std::string> error;
};

class ScriptedProvider final : public Provider {
 public:
  explicit ScriptedProvider(ProviderDescriptor descriptor = {});

  const ProviderDescriptor& descriptor() const override;
  CompletionResponse complete(const CompletionRequest& request,
                              const ProviderOptions& options) override;

  void push_response(CompletionResponse response);
  void push_error(std::string error);

  const std::vector<CompletionRequest>& requests() const noexcept;
  const std::vector<ProviderOptions>& options_history() const noexcept;
  std::size_t pending_steps() const noexcept;
  std::size_t completed_steps() const noexcept;

 private:
  ProviderDescriptor descriptor_;
  std::vector<ScriptedProviderStep> steps_;
  std::vector<CompletionRequest> requests_;
  std::vector<ProviderOptions> options_history_;
  std::size_t next_step_index_ = 0;
};

struct FauxProviderState {
  std::size_t call_count = 0;
};

using FauxResponseFactory =
    std::function<CompletionResponse(const CompletionRequest&,
                                     const ProviderOptions&,
                                     const FauxProviderState&)>;

struct FauxProviderStep {
  std::optional<CompletionResponse> response;
  FauxResponseFactory factory;
};

class FauxProvider final : public Provider {
 public:
  explicit FauxProvider(ProviderDescriptor descriptor = {});

  const ProviderDescriptor& descriptor() const override;
  CompletionResponse complete(const CompletionRequest& request,
                              const ProviderOptions& options) override;
  void stream(const CompletionRequest& request, const ProviderOptions& options,
              EventSink& sink) override;

  void set_responses(std::vector<CompletionResponse> responses);
  void append_responses(std::vector<CompletionResponse> responses);
  void push_response(CompletionResponse response);
  void push_factory(FauxResponseFactory factory);

  std::size_t pending_response_count() const noexcept;
  const FauxProviderState& state() const noexcept;

 private:
  CompletionResponse finalize_response(CompletionResponse response,
                                       const CompletionRequest& request,
                                       const ProviderOptions& options);
  CompletionResponse exhausted_response(const CompletionRequest& request,
                                        const ProviderOptions& options);
  CompletionResponse aborted_response(CompletionResponse response,
                                      const CompletionRequest& request,
                                      const ProviderOptions& options);

  ProviderDescriptor descriptor_;
  std::vector<FauxProviderStep> steps_;
  std::size_t next_step_index_ = 0;
  FauxProviderState state_;
  std::map<std::string, std::string> prompt_cache_;
};

class ProviderRegistry {
 public:
  bool register_provider(std::unique_ptr<Provider> provider);
  bool unregister_provider(const std::string& id);
  const Provider* find(const std::string& id) const;
  std::vector<ProviderDescriptor> descriptors() const;
  std::size_t size() const noexcept;

 private:
  std::map<std::string, std::unique_ptr<Provider>> providers_;
};

class VectorEventSink final : public EventSink {
 public:
  void push(const StreamEvent& event) override;
  const std::vector<StreamEvent>& events() const noexcept;

 private:
  std::vector<StreamEvent> events_;
};

}  // namespace qt::ai
