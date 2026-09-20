#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "qt/ai/provider.hpp"

namespace qt::ai {

struct HttpRequest {
  std::string method = "POST";
  std::string url;
  std::map<std::string, std::string> headers;
  std::string body;
  std::optional<std::int64_t> timeout_ms;
  std::optional<std::string> proxy_url;
  std::shared_ptr<std::atomic_bool> abort_signal;
  int attempt = 0;
};

struct HttpResponse {
  int status = 200;
  std::map<std::string, std::string> headers;
  std::string body;
};

bool is_retryable_http_status(int status) noexcept;
std::optional<std::int64_t> retry_after_ms(const HttpResponse& response);

class HttpTransport {
 public:
  virtual ~HttpTransport() = default;
  virtual HttpResponse send(const HttpRequest& request) = 0;
};

class BeastHttpTransport final : public HttpTransport {
 public:
  HttpResponse send(const HttpRequest& request) override;
};

class RecordedHttpTransport final : public HttpTransport {
 public:
  HttpResponse send(const HttpRequest& request) override;

  void push_response(HttpResponse response);
  void push_error(std::string error);

  const std::vector<HttpRequest>& requests() const noexcept;
  std::size_t pending_steps() const noexcept;
  std::size_t completed_steps() const noexcept;

 private:
  struct Step {
    std::optional<HttpResponse> response;
    std::optional<std::string> error;
  };

  std::vector<Step> steps_;
  std::vector<HttpRequest> requests_;
  std::size_t next_step_index_ = 0;
};

struct SseEvent {
  std::optional<std::string> event;
  std::string data;
  std::optional<std::string> id;
  std::optional<std::int64_t> retry_ms;
};

std::vector<SseEvent> parse_sse_events(std::string_view transcript);
Json openai_responses_request_body(const CompletionRequest& request,
                                   bool stream);
Json openai_responses_request_body(const CompletionRequest& request,
                                   bool stream,
                                   const Json& options_metadata);
Json openai_codex_responses_request_body(
    const CompletionRequest& request, bool stream,
    const Json& options_metadata = Json::object());
CompletionResponse parse_openai_response(const Json& response);
std::vector<StreamEvent> parse_openai_responses_sse(std::string_view transcript);
Json openai_completions_request_body(const CompletionRequest& request,
                                     bool stream);
Json openai_completions_request_body(const CompletionRequest& request,
                                     bool stream,
                                     const Json& options_metadata);
Json anthropic_messages_request_body(const CompletionRequest& request,
                                     bool stream,
                                     const Json& options_metadata = Json::object());
HttpRequest anthropic_messages_http_request(
    const CompletionRequest& request, const ProviderOptions& options,
    bool stream,
    std::string endpoint = "https://api.anthropic.com/v1/messages");
std::vector<StreamEvent> parse_anthropic_messages_sse(
    std::string_view transcript);
std::vector<StreamEvent> parse_anthropic_messages_sse(
    std::string_view transcript, const std::vector<ToolDefinition>& tools,
    bool claude_code_oauth);
Json google_tools_request_field(const std::vector<ToolDefinition>& tools,
                                bool use_parameters = false);
std::string google_tool_choice_mode(std::string_view choice);
Json google_contents_request_field(const CompletionRequest& request);
Json google_generate_content_request_body(
    const CompletionRequest& request,
    const Json& options_metadata = Json::object());
Json google_thinking_config_request_field(
    const CompletionRequest& request,
    const Json& options_metadata = Json::object());
StopReason google_stop_reason(std::string_view reason);
Usage google_usage_metadata(const Json& usage_metadata);
std::vector<StreamEvent> parse_google_generate_content_stream(
    std::string_view transcript);
bool google_is_thinking_part(const Json& part);
std::optional<std::string> google_retain_thought_signature(
    const std::optional<std::string>& previous,
    const std::optional<std::string>& incoming);
Json google_client_options(
    const ProviderOptions& options,
    const std::map<std::string, std::string>& model_headers = {});
Json google_vertex_client_options(const ProviderOptions& options);
Json bedrock_converse_request_body(
    const CompletionRequest& request, bool stream,
    const Json& options_metadata = Json::object());
Json bedrock_runtime_client_config(const ProviderOptions& options);
StopReason bedrock_stop_reason(std::string_view reason);
Usage bedrock_usage_metadata(const Json& usage_metadata);
struct BedrockThinkingTokenPlan {
  std::int64_t max_tokens = 0;
  std::int64_t thinking_budget = 0;
};
BedrockThinkingTokenPlan bedrock_adjust_max_tokens_for_thinking(
    std::int64_t base_max_tokens,
    std::int64_t model_max_tokens,
    std::string_view thinking_level,
    const Json& custom_budgets = Json::object());
std::vector<StreamEvent> parse_bedrock_converse_stream(
    std::string_view transcript);
std::string bedrock_format_error(std::string_view name,
                                 std::string_view message,
                                 bool service_exception);
Json mistral_conversations_request_body(
    const CompletionRequest& request, bool stream,
    const Json& options_metadata = Json::object());
Json mistral_request_options(
    const ProviderOptions& options,
    const std::map<std::string, std::string>& model_headers = {});
StopReason mistral_chat_stop_reason(std::string_view reason);
std::string mistral_format_error(std::optional<int> status_code,
                                 std::string_view message,
                                 std::optional<std::string_view> body);
std::vector<StreamEvent> parse_mistral_conversations_sse(
    std::string_view transcript);
CompletionResponse parse_openai_completions_response(const Json& response);
std::vector<StreamEvent> parse_openai_completions_sse(
    std::string_view transcript);
CompletionResponse completion_response_from_events(
    const std::vector<StreamEvent>& events);

class OpenAIResponsesProvider final : public Provider {
 public:
  explicit OpenAIResponsesProvider(
      std::unique_ptr<HttpTransport> transport,
      std::string endpoint = "https://api.openai.com/v1/responses");
  OpenAIResponsesProvider(ProviderDescriptor descriptor,
                          std::unique_ptr<HttpTransport> transport,
                          std::string endpoint);

  const ProviderDescriptor& descriptor() const override;
  CompletionResponse complete(const CompletionRequest& request,
                              const ProviderOptions& options) override;
  void stream(const CompletionRequest& request, const ProviderOptions& options,
              EventSink& sink) override;

 private:
  HttpResponse send_request(const CompletionRequest& request,
                            const ProviderOptions& options, bool stream);

  ProviderDescriptor descriptor_;
  std::unique_ptr<HttpTransport> transport_;
  std::string endpoint_;
};

std::shared_ptr<Provider> make_openai_responses_provider(
    std::unique_ptr<HttpTransport> transport,
    std::string endpoint = "https://api.openai.com/v1/responses");

class OpenAICodexResponsesProvider final : public Provider {
 public:
  explicit OpenAICodexResponsesProvider(
      std::unique_ptr<HttpTransport> transport,
      std::string endpoint = "https://chatgpt.com/backend-api/codex/responses");
  OpenAICodexResponsesProvider(ProviderDescriptor descriptor,
                               std::unique_ptr<HttpTransport> transport,
                               std::string endpoint);

  const ProviderDescriptor& descriptor() const override;
  CompletionResponse complete(const CompletionRequest& request,
                              const ProviderOptions& options) override;
  void stream(const CompletionRequest& request, const ProviderOptions& options,
              EventSink& sink) override;

 private:
  HttpResponse send_request(const CompletionRequest& request,
                            const ProviderOptions& options, bool stream);

  ProviderDescriptor descriptor_;
  std::unique_ptr<HttpTransport> transport_;
  std::string endpoint_;
};

std::shared_ptr<Provider> make_openai_codex_responses_provider(
    std::unique_ptr<HttpTransport> transport,
    std::string endpoint = "https://chatgpt.com/backend-api/codex/responses");

class AzureOpenAIResponsesProvider final : public Provider {
 public:
  explicit AzureOpenAIResponsesProvider(
      std::unique_ptr<HttpTransport> transport, std::string base_url = "");
  AzureOpenAIResponsesProvider(ProviderDescriptor descriptor,
                               std::unique_ptr<HttpTransport> transport,
                               std::string base_url);

  const ProviderDescriptor& descriptor() const override;
  CompletionResponse complete(const CompletionRequest& request,
                              const ProviderOptions& options) override;
  void stream(const CompletionRequest& request, const ProviderOptions& options,
              EventSink& sink) override;

 private:
  HttpResponse send_request(const CompletionRequest& request,
                            const ProviderOptions& options, bool stream);

  ProviderDescriptor descriptor_;
  std::unique_ptr<HttpTransport> transport_;
  std::string base_url_;
};

std::shared_ptr<Provider> make_azure_openai_responses_provider(
    std::unique_ptr<HttpTransport> transport, std::string base_url = "");

class OpenAICompletionsProvider final : public Provider {
 public:
  explicit OpenAICompletionsProvider(
      std::unique_ptr<HttpTransport> transport,
      std::string endpoint = "https://api.openai.com/v1/chat/completions");
  OpenAICompletionsProvider(ProviderDescriptor descriptor,
                            std::unique_ptr<HttpTransport> transport,
                            std::string endpoint);

  const ProviderDescriptor& descriptor() const override;
  CompletionResponse complete(const CompletionRequest& request,
                              const ProviderOptions& options) override;
  void stream(const CompletionRequest& request, const ProviderOptions& options,
              EventSink& sink) override;

 private:
  HttpResponse send_request(const CompletionRequest& request,
                            const ProviderOptions& options, bool stream);

  ProviderDescriptor descriptor_;
  std::unique_ptr<HttpTransport> transport_;
  std::string endpoint_;
};

std::shared_ptr<Provider> make_openai_completions_provider(
    std::unique_ptr<HttpTransport> transport,
    std::string endpoint = "https://api.openai.com/v1/chat/completions");

class RecordedTranscriptProvider final : public Provider {
 public:
  RecordedTranscriptProvider(ProviderDescriptor descriptor,
                             std::shared_ptr<HttpTransport> transport,
                             std::string endpoint);

  const ProviderDescriptor& descriptor() const override;
  CompletionResponse complete(const CompletionRequest& request,
                              const ProviderOptions& options) override;
  void stream(const CompletionRequest& request, const ProviderOptions& options,
              EventSink& sink) override;

 private:
  HttpResponse send_request(const CompletionRequest& request,
                            const ProviderOptions& options);
  std::vector<StreamEvent> decode_stream_events(const HttpResponse& response) const;

  ProviderDescriptor descriptor_;
  std::shared_ptr<HttpTransport> transport_;
  std::string endpoint_;
};

}  // namespace qt::ai
