#include "qt/ai/llama_provider.hpp"

#include <llama.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

using namespace qt::ai;

namespace {

void require(const bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

LlamaSettings settings_for(const std::string& id, const std::string& path) {
  LlamaSettings settings;
  settings.models_configured = true;
  settings.backend.enabled = true;
  settings.backend.gpu_layers = 0;
  settings.context.context_size = 512;
  settings.embeddings.enabled = true;
  settings.embeddings.pooling = "mean";
  settings.embeddings.normalize = true;
  LlamaModelSettings model;
  model.id = id;
  model.name = id;
  model.path = path;
  model.supports_embeddings = true;
  settings.models.push_back(std::move(model));
  return settings;
}

LlamaEmbeddingRequest embedding_request(
    const std::string& model_id, std::function<bool()> cancelled = {}) {
  LlamaEmbeddingRequest request;
  request.model_id = model_id;
  request.text = "search_query: cancellation load probe";
  request.cancelled = std::move(cancelled);
  return request;
}

class BlockedLoadBackend final : public LlamaBackendAdapter {
 public:
  enum class Mode { IgnoreCancellation, ConsumeCancellationThenFail, PollCancellation };

  explicit BlockedLoadBackend(const Mode mode) : mode_(mode) {}

  void load(const LlamaBackendLoadRequest& request) override {
    ++load_calls;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      loading_ = true;
      worker_thread_ = std::this_thread::get_id();
    }
    started_.notify_all();
    if (mode_ == Mode::PollCancellation) {
      while (!begin_polling.load() && !stop_polling.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      while (!stop_polling.load()) {
        ++backend_poll_calls;
        if (request.cancelled) (void)request.cancelled();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      std::lock_guard<std::mutex> lock(mutex_);
      loaded_ = true;
      return;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    released_.wait(lock, [&] { return release_; });
    if (mode_ == Mode::ConsumeCancellationThenFail) {
      callback_was_supplied = static_cast<bool>(request.cancelled);
      const bool cancelled = request.cancelled && request.cancelled();
      lock.unlock();
      if (cancelled) throw std::runtime_error("generic mock backend load failure");
      throw std::runtime_error("mock cancellation callback was not latched");
    }
    loaded_ = true;
  }

  void unload() noexcept override {
    ++unload_calls;
    std::lock_guard<std::mutex> lock(mutex_);
    loaded_ = false;
  }

  std::optional<std::string> loaded_model_id() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return loaded_ ? std::optional<std::string>{"fake"} : std::nullopt;
  }
  bool loaded_embeddings_enabled() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return loaded_;
  }
  LlamaRuntimeStatus runtime_status() const override { return {}; }
  LlamaBackendGenerateResult generate(const LlamaBackendGenerateRequest&,
                                      const LlamaTokenCallback&) override { return {}; }
  LlamaKvCacheState kv_cache_control(const LlamaKvCacheOperation&) override { return {}; }

  LlamaEmbeddingResult embed(const LlamaEmbeddingRequest&) override {
    ++embed_calls;
    LlamaEmbeddingResult result;
    result.embedding = {1.0F, 2.0F};
    return result;
  }

  void wait_for_load() {
    std::unique_lock<std::mutex> lock(mutex_);
    require(started_.wait_for(lock, std::chrono::seconds(2), [&] { return loading_; }),
            "mock load did not start");
  }

  void release_load() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      release_ = true;
    }
    released_.notify_all();
  }

  bool called_from_load_worker() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::this_thread::get_id() == worker_thread_;
  }

  std::atomic_int load_calls{0};
  std::atomic_int unload_calls{0};
  std::atomic_int embed_calls{0};
  std::atomic_int backend_poll_calls{0};
  std::atomic_bool callback_was_supplied{false};
  std::atomic_bool begin_polling{false};
  std::atomic_bool stop_polling{false};

 private:
  mutable std::mutex mutex_;
  std::condition_variable started_;
  std::condition_variable released_;
  bool loading_ = false;
  bool release_ = false;
  bool loaded_ = false;
  std::thread::id worker_thread_;
  Mode mode_;
};

struct ReleaseLoad {
  BlockedLoadBackend* backend = nullptr;
  ~ReleaseLoad() {
    if (backend) backend->release_load();
  }
};

struct StopPolling {
  BlockedLoadBackend* backend = nullptr;
  ~StopPolling() {
    if (backend) {
      backend->stop_polling = true;
      backend->begin_polling = true;
    }
  }
};

class PlanningCancellationBackend final : public LlamaBackendAdapter {
 public:
  explicit PlanningCancellationBackend(std::atomic_bool& cancelled)
      : cancelled_(cancelled) {}

  void load(const LlamaBackendLoadRequest&) override { ++load_calls; }
  void unload() noexcept override { ++unload_calls; }
  std::optional<std::string> loaded_model_id() const override { return "old"; }
  bool loaded_embeddings_enabled() const override { return true; }
  std::vector<LlamaGpuMemory> gpu_memory() const override {
    cancelled_ = true;
    return {{0, 8ULL * 1024ULL * 1024ULL * 1024ULL,
             8ULL * 1024ULL * 1024ULL * 1024ULL}};
  }
  LlamaRuntimeStatus runtime_status() const override {
    LlamaRuntimeStatus status;
    status.loaded_model_id = "old";
    status.model_loaded = true;
    status.context_loaded = true;
    status.embeddings_enabled = true;
    return status;
  }
  LlamaBackendGenerateResult generate(const LlamaBackendGenerateRequest&,
                                      const LlamaTokenCallback&) override { return {}; }
  LlamaEmbeddingResult embed(const LlamaEmbeddingRequest&) override {
    LlamaEmbeddingResult result;
    result.embedding = {1.0F};
    return result;
  }
  LlamaKvCacheState kv_cache_control(const LlamaKvCacheOperation&) override { return {}; }

  std::atomic_int load_calls{0};
  std::atomic_int unload_calls{0};

 private:
  std::atomic_bool& cancelled_;
};

LlamaSettings mock_settings() {
  auto settings = settings_for("fake", "fake");
  // The offload planner tries the configured GPU level and then CPU. A cancelled
  // first attempt must not enter that fallback attempt.
  settings.backend.gpu_layers = 1;
  return settings;
}

void mock_load_cancellation() {
  auto backend = std::make_unique<BlockedLoadBackend>(
      BlockedLoadBackend::Mode::IgnoreCancellation);
  auto* view = backend.get();
  LlamaProvider provider(mock_settings(), std::move(backend));
  std::atomic_bool cancelled{false};
  auto cancelled_load = std::async(std::launch::async, [&] {
    return provider.embed(embedding_request("fake", [&] { return cancelled.load(); }),
                          LlamaInstanceTag::kChat);
  });
  ReleaseLoad release_on_failure{view};
  view->wait_for_load();
  cancelled = true;
  view->release_load(); // The backend deliberately never calls request.cancelled.
  require(cancelled_load.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
          "cancelled load did not finish");
  bool rejected = false;
  try {
    (void)cancelled_load.get();
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  require(rejected, "provider accepted a request cancelled while backend load was blocked");
  require(view->load_calls.load() == 1 && view->embed_calls.load() == 0,
          "cancelled load retried an offload candidate or entered embedding");
  require(view->unload_calls.load() == 1,
          "provider did not unload a backend that ignored load cancellation");
  const auto cancelled_status = provider.runtime_status(LlamaInstanceTag::kChat);
  require(cancelled_status.metadata.at("offload").value("loadCancelled", false) &&
              cancelled_status.metadata.at("offload").at("attemptedGpuLayers") == Json::array({1}),
          "load cancellation status omitted the single interrupted GPU attempt");

  const auto retry = provider.embed(embedding_request("fake"), LlamaInstanceTag::kChat);
  require(retry.embedding.size() == 2 && view->load_calls.load() == 2 &&
              view->embed_calls.load() == 1,
          "retry after cancelled load did not start from an unloaded backend");
  require(!provider.runtime_status(LlamaInstanceTag::kChat).metadata.at("offload")
               .value("loadCancelled", false),
          "successful retry retained a stale cancellation status");

  auto failing_backend = std::make_unique<BlockedLoadBackend>(
      BlockedLoadBackend::Mode::ConsumeCancellationThenFail);
  auto* failing_view = failing_backend.get();
  LlamaProvider failing_provider(mock_settings(), std::move(failing_backend));
  std::atomic_bool failing_cancelled{false};
  auto generic_failure = std::async(std::launch::async, [&] {
    return failing_provider.embed(
        embedding_request("fake", [&] { return failing_cancelled.load(); }),
        LlamaInstanceTag::kChat);
  });
  ReleaseLoad failing_release_on_failure{failing_view};
  failing_view->wait_for_load();
  failing_cancelled = true;
  failing_view->release_load();
  require(generic_failure.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
          "generic cancelled backend failure did not finish");
  bool generic_rejected = false;
  try {
    (void)generic_failure.get();
  } catch (const std::runtime_error&) {
    generic_rejected = true;
  }
  require(generic_rejected && failing_view->callback_was_supplied.load(),
          "provider did not propagate cancellation to the generic-failure backend");
  require(failing_view->load_calls.load() == 1 && failing_view->embed_calls.load() == 0,
          "generic cancelled backend failure entered the CPU fallback or embedding");
}

void serialized_callback_evaluation() {
  auto backend = std::make_unique<BlockedLoadBackend>(
      BlockedLoadBackend::Mode::PollCancellation);
  auto* view = backend.get();
  LlamaProvider provider(mock_settings(), std::move(backend));
  std::atomic_bool in_callback{false};
  std::atomic_bool overlap{false};
  std::atomic_int load_worker_calls{0};
  std::atomic_int caller_calls{0};
  std::atomic_bool measuring{false};
  auto pending = std::async(std::launch::async, [&] {
    return provider.embed(embedding_request("fake", [&] {
      if (measuring.load()) {
        if (view->called_from_load_worker()) {
          ++load_worker_calls;
        } else {
          ++caller_calls;
        }
      }
      if (in_callback.exchange(true)) overlap = true;
      if (measuring.load()) std::this_thread::sleep_for(std::chrono::milliseconds(3));
      in_callback = false;
      return false;
    }), LlamaInstanceTag::kChat);
  });
  StopPolling stop_on_failure{view};
  view->wait_for_load();
  measuring = true;
  view->begin_polling = true;
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (load_worker_calls.load() >= 2 && caller_calls.load() >= 1) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  view->stop_polling = true;
  require(pending.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
          "serialized callback load did not complete");
  const auto result = pending.get();
  require(!overlap.load() && load_worker_calls.load() >= 2 &&
              caller_calls.load() >= 1 && !result.embedding.empty(),
          "caller and load worker concurrently invoked the cancellation predicate");
}

void planning_cancellation_preserves_loaded_backend() {
  std::atomic_bool cancelled{false};
  auto backend = std::make_unique<PlanningCancellationBackend>(cancelled);
  auto* view = backend.get();
  auto settings = settings_for("fake", "fake");
  settings.backend.gpu_layers.reset();
  LlamaProvider provider(std::move(settings), std::move(backend));
  bool rejected = false;
  try {
    (void)provider.embed(embedding_request("fake", [&] { return cancelled.load(); }),
                         LlamaInstanceTag::kChat);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  require(rejected && view->load_calls.load() == 0 && view->unload_calls.load() == 0,
          "planning cancellation loaded or unloaded the previously loaded backend");
  const auto status = provider.runtime_status(LlamaInstanceTag::kChat);
  require(status.loaded_model_id && *status.loaded_model_id == "old" &&
              status.model_loaded && status.context_loaded,
          "planning cancellation discarded the previously loaded backend");
  require(status.metadata.at("offload").value("loadCancelled", false) &&
              status.metadata.at("offload").at("attemptedGpuLayers").empty(),
          "planning cancellation status claimed a backend load attempt");
}

LlamaBackendLoadRequest load_request(const std::string& model_path,
                                     std::function<bool()> cancelled = {}) {
  const auto settings = settings_for("nomic-embed-text-v1.5", model_path);
  LlamaBackendLoadRequest request;
  request.model = settings.models.front();
  request.backend = settings.backend;
  request.context = settings.context;
  request.embeddings = settings.embeddings;
  request.cancelled = std::move(cancelled);
  return request;
}

void require_unloaded(const LlamaBackendAdapter& backend) {
  const auto status = backend.runtime_status();
  require(!status.model_loaded && !status.context_loaded,
          "cancelled real load left a model or context resident");
}

class ModelLoadCancellationLog final {
 public:
  ~ModelLoadCancellationLog() {
    if (installed_) llama_log_set(previous_callback_, previous_user_data_);
  }

  void install() {
    if (installed_) return;
    llama_log_get(&previous_callback_, &previous_user_data_);
    llama_log_set([](ggml_log_level, const char* text, void* user_data) {
      auto* capture = static_cast<ModelLoadCancellationLog*>(user_data);
      if (text && std::strstr(text, "cancelled model load")) {
        capture->observed_.store(true);
      }
    }, this);
    installed_ = true;
  }

  bool observed() const { return observed_.load(); }

 private:
  ggml_log_callback previous_callback_ = nullptr;
  void* previous_user_data_ = nullptr;
  std::atomic_bool observed_{false};
  bool installed_ = false;
};

void real_load_cancellation(const std::string& model_path) {
  constexpr char kModel[] = "nomic-embed-text-v1.5";

  {
    auto backend = make_real_llama_backend_adapter();
    std::atomic_int callback_calls{0};
    ModelLoadCancellationLog load_log;
    bool rejected = false;
    std::string error_message;
    try {
      backend->load(load_request(model_path, [&] {
        if (++callback_calls >= 2) {
          load_log.install();
          return true;
        }
        return false;
      }));
    } catch (const std::runtime_error& error) {
      rejected = true;
      error_message = error.what();
    }
    require(rejected && error_message == "llama embedding cancelled" &&
                callback_calls.load() >= 2 && load_log.observed(),
            "real load cancellation did not reach its progress callback");
    require_unloaded(*backend);
    backend->load(load_request(model_path));
    const auto retry = backend->embed(embedding_request(kModel));
    require(!retry.embedding.empty(), "real embedding retry after cancellation failed");
  }

  {
    auto backend = make_real_llama_backend_adapter();
    std::atomic_int callback_calls{0};
    bool rejected = false;
    try {
      backend->load(load_request(model_path, [&]() -> bool {
        if (++callback_calls >= 2) throw std::runtime_error("probe callback failure");
        return false;
      }));
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    require(rejected && callback_calls.load() >= 2,
            "throwing load callback escaped or did not stop real loading");
    require_unloaded(*backend);
    backend->load(load_request(model_path));
    const auto retry = backend->embed(embedding_request(kModel));
    require(!retry.embedding.empty(), "real embedding retry after throwing callback failed");
  }

  {
    auto backend = make_real_llama_backend_adapter();
    auto retained = std::make_shared<int>(7);
    std::weak_ptr<int> weak = retained;
    {
      auto request = embedding_request(kModel, [retained] { return false; });
      backend->load(load_request(model_path, request.cancelled));
      const auto result = backend->embed(request);
      require(!result.embedding.empty(), "real callback lifetime setup did not embed");
    }
    retained.reset();
    require(weak.expired(), "loaded backend retained a completed load cancellation callback");
    const auto status = backend->runtime_status();
    require(status.model_loaded && status.context_loaded,
            "callback lifetime check unexpectedly unloaded the real backend");
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::string model_path = argc > 1 ? argv[1] : "models/nomic-embed-text-v1.5.Q8_0.gguf";
    require(std::filesystem::exists(model_path), "model file does not exist: " + model_path);
    mock_load_cancellation();
    serialized_callback_evaluation();
    planning_cancellation_preserves_loaded_backend();
    real_load_cancellation(model_path);
    std::cout << "llama embedding load probe passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "llama embedding load probe failed: " << error.what() << '\n';
    return 1;
  }
}
