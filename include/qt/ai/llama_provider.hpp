#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include "qt/ai/llama_config.hpp"
#include "qt/ai/llama_offload.hpp"
#include "qt/ai/provider.hpp"

namespace qt::ai {

enum class LlamaInstanceTag {
  kEmbed,
  kRank,
  kInstruct,
  kChat,
};

std::optional<LlamaInstanceTag> parse_llama_instance_tag(std::string_view value);
std::string_view llama_instance_tag_name(LlamaInstanceTag tag) noexcept;

struct LlamaSamplerPlanStep {
  std::string name;
  Json parameters = Json::object();
};

struct LlamaBackendLoadRequest {
  LlamaModelSettings model;
  LlamaBackendSettings backend;
  LlamaContextSettings context;
  LlamaEmbeddingSettings embeddings;
  // Borrowed for the synchronous load only; adapters must not retain it.
  // Cancellation is cooperative at model-load progress checkpoints.
  std::function<bool()> cancelled;
};

struct LlamaBackendGenerateRequest {
  std::string model_id;
  std::string prompt;
  LlamaGenerationSettings generation;
  LlamaKvCacheSettings kv_cache;
  std::vector<LlamaSamplerPlanStep> sampler_plan;
  std::int64_t max_output_tokens = 0;
  bool aborted = false;
  Json metadata = Json::object();
};

struct LlamaBackendGenerateResult {
  std::string text;
  Usage usage;
  StopReason stop_reason = StopReason::unknown;
  Json metadata = Json::object();
};

struct LlamaEmbeddingRequest {
  std::string model_id;
  std::string text;
  bool normalize = true;
  Json metadata = Json::object();
  // The provider serializes predicate evaluation and latches cancellation.
  std::function<bool()> cancelled;
  // Bounds pending queue time, not model loading or an active decode.
  // Zero expires immediately; negative durations are invalid.
  std::chrono::milliseconds queue_timeout{250};
};

struct LlamaEmbeddingResult {
  std::vector<float> embedding;
  Usage usage;
  Json metadata = Json::object();
};

enum class LlamaKvCacheOperationKind {
  kInspect,
  kClear,
  kKeep,
  kDrop,
  kShift,
};

struct LlamaKvCacheOperation {
  LlamaKvCacheOperationKind kind = LlamaKvCacheOperationKind::kInspect;
  std::int32_t seq_id = 0;
  std::int32_t dst_seq_id = 0;
  std::int32_t from = -1;
  std::int32_t to = -1;
  std::int32_t delta = 0;
};

struct LlamaKvCacheState {
  std::optional<std::string> loaded_model_id;
  std::int32_t seq_id = 0;
  std::int32_t position_min = -1;
  std::int32_t position_max = -1;
  std::int64_t sequence_position = 0;
  bool can_shift = false;
  bool mutated = false;
  Json metadata = Json::object();
};

struct LlamaRuntimeStatus {
  std::optional<std::string> loaded_model_id;
  bool model_loaded = false;
  bool context_loaded = false;
  bool embeddings_enabled = false;
  std::uint64_t model_size_bytes = 0;
  std::uint64_t state_size_bytes = 0;
  std::uint32_t context_size = 0;
  std::int64_t sequence_position = 0;
  bool gpu_offload_supported = false;
  std::size_t max_devices = 0;
  Json backend = Json::object();
  Json memory = Json::object();
  Json metadata = Json::object();
};

using LlamaTokenCallback = std::function<void(std::string_view)>;

class LlamaBackendAdapter {
 public:
  virtual ~LlamaBackendAdapter() = default;

  virtual void load(const LlamaBackendLoadRequest& request) = 0;
  virtual void unload() noexcept = 0;
  [[nodiscard]] virtual std::optional<std::string> loaded_model_id() const = 0;
  [[nodiscard]] virtual bool loaded_embeddings_enabled() const = 0;
  [[nodiscard]] virtual std::vector<LlamaGpuMemory> gpu_memory() const {
    return {};
  }
  [[nodiscard]] virtual LlamaRuntimeStatus runtime_status() const = 0;
  [[nodiscard]] virtual LlamaBackendGenerateResult generate(
      const LlamaBackendGenerateRequest& request,
      const LlamaTokenCallback& on_token) = 0;
  [[nodiscard]] virtual LlamaEmbeddingResult embed(
      const LlamaEmbeddingRequest& request) = 0;
  [[nodiscard]] virtual LlamaKvCacheState kv_cache_control(
      const LlamaKvCacheOperation& operation) = 0;
};

class LlamaProvider final : public Provider {
 public:
  explicit LlamaProvider(
      LlamaSettings settings,
      std::unique_ptr<LlamaBackendAdapter> backend = nullptr);
  ~LlamaProvider() override;

  const ProviderDescriptor& descriptor() const override;
  CompletionResponse complete(const CompletionRequest& request,
                              const ProviderOptions& options) override;
  void stream(const CompletionRequest& request, const ProviderOptions& options,
              EventSink& sink) override;

  void load_model(const std::string& model_id);
  void load_model(const std::string& model_id, LlamaInstanceTag tag);
  void unload() noexcept;
  [[nodiscard]] std::optional<std::string> loaded_model_id() const;
  [[nodiscard]] std::optional<std::string> loaded_model_id(
      LlamaInstanceTag tag) const;
  [[nodiscard]] LlamaRuntimeStatus runtime_status() const;
  [[nodiscard]] LlamaRuntimeStatus runtime_status(LlamaInstanceTag tag) const;
  [[nodiscard]] LlamaEmbeddingResult embed(const LlamaEmbeddingRequest& request);
  [[nodiscard]] LlamaEmbeddingResult embed(const LlamaEmbeddingRequest& request,
                                           LlamaInstanceTag tag);
  [[nodiscard]] LlamaKvCacheState kv_cache_control(
      const LlamaKvCacheOperation& operation);
  [[nodiscard]] LlamaKvCacheState kv_cache_control(
      const LlamaKvCacheOperation& operation, LlamaInstanceTag tag);
  [[nodiscard]] CompletionResponse run_tagged(
      LlamaInstanceTag tag, const CompletionRequest& request,
      const ProviderOptions& options);

 private:
  template <typename Fn>
  auto enqueue_model_call(Fn&& fn, std::shared_ptr<void> identity = {})
      -> std::future<std::invoke_result_t<Fn>>;
  void worker_loop() noexcept;
  void stop_worker() noexcept;

  const LlamaModelSettings& require_model(const std::string& model_id) const;
  LlamaBackendAdapter& ensure_loaded(const LlamaModelSettings& model,
                                     LlamaInstanceTag tag,
                                     bool embeddings_required = false,
                                     const std::function<bool()>& cancelled = {});
  LlamaBackendGenerateRequest make_generate_request(
      const CompletionRequest& request, const ProviderOptions& options,
      const LlamaModelSettings& model) const;
  LlamaBackendAdapter& backend_for_tag(LlamaInstanceTag tag);

  ProviderDescriptor descriptor_;
  LlamaSettings settings_;
  mutable std::mutex backends_mutex_;
  std::map<LlamaInstanceTag, std::unique_ptr<LlamaBackendAdapter>> backends_;
  bool automatic_gpu_offload_ = false;
  std::map<LlamaInstanceTag, Json> offload_status_;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  struct QueuedModelCall {
    std::function<void()> run;
    std::shared_ptr<void> identity;
  };
  std::deque<QueuedModelCall> queued_model_calls_;
  bool stopping_worker_ = false;
  std::thread worker_;
};

template <typename Fn>
auto LlamaProvider::enqueue_model_call(Fn&& fn, std::shared_ptr<void> identity)
    -> std::future<std::invoke_result_t<Fn>> {
  using Result = std::invoke_result_t<Fn>;
  auto promise = std::make_shared<std::promise<Result>>();
  auto future = promise->get_future();
  if (worker_.joinable() && std::this_thread::get_id() == worker_.get_id()) {
    try {
      if constexpr (std::is_void_v<Result>) {
        fn();
        promise->set_value();
      } else {
        promise->set_value(fn());
      }
    } catch (...) {
      promise->set_exception(std::current_exception());
    }
    return future;
  }
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (stopping_worker_) {
      throw std::runtime_error("llama provider is shutting down");
    }
    queued_model_calls_.push_back(
        {[promise, task = std::forward<Fn>(fn)]() mutable {
          try {
            if constexpr (std::is_void_v<Result>) {
              task();
              promise->set_value();
            } else {
              promise->set_value(task());
            }
          } catch (...) {
            promise->set_exception(std::current_exception());
          }
        }, std::move(identity)});
  }
  queue_cv_.notify_one();
  return future;
}

std::string serialize_llama_prompt(const CompletionRequest& request);
LlamaGenerationSettings effective_llama_generation_settings(
    const LlamaSettings& settings, const LlamaModelSettings& model,
    const CompletionRequest& request, const ProviderOptions& options);
std::vector<LlamaSamplerPlanStep> make_llama_sampler_plan(
    const LlamaGenerationSettings& settings);

std::unique_ptr<LlamaBackendAdapter> make_real_llama_backend_adapter();
std::shared_ptr<Provider> make_llama_provider(const LlamaSettings& settings);

}  // namespace qt::ai
