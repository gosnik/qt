#include "qt/ai/llama_provider.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace qt::ai;

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

class BlockingBackend final : public LlamaBackendAdapter {
 public:
  void load(const LlamaBackendLoadRequest&) override {}
  void unload() noexcept override {}
  std::optional<std::string> loaded_model_id() const override { return "fake"; }
  bool loaded_embeddings_enabled() const override { return true; }
  LlamaRuntimeStatus runtime_status() const override { return {}; }
  LlamaBackendGenerateResult generate(const LlamaBackendGenerateRequest&,
                                      const LlamaTokenCallback&) override { return {}; }
  LlamaKvCacheState kv_cache_control(const LlamaKvCacheOperation&) override { return {}; }

  LlamaEmbeddingResult embed(const LlamaEmbeddingRequest& request) override {
    const int call = ++calls;
    {
      std::lock_guard<std::mutex> lock(mutex);
      active = true;
    }
    started.notify_all();
    if (call == 1) {
      std::unique_lock<std::mutex> lock(mutex);
      released.wait(lock, [&] { return release; });
    } else {
      while (!(request.cancelled && request.cancelled())) {
        if (call != 3) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      if (request.cancelled && request.cancelled())
        throw std::runtime_error("cooperative embedding cancellation");
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      active = false;
    }
    LlamaEmbeddingResult result;
    result.embedding = {static_cast<float>(call)};
    return result;
  }

  void wait_started() {
    std::unique_lock<std::mutex> lock(mutex);
    require(started.wait_for(lock, std::chrono::seconds(2), [&] { return active; }),
            "backend did not start");
  }
  void release_active() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      release = true;
    }
    released.notify_all();
  }

  std::atomic<int> calls{0};
 private:
  std::mutex mutex;
  std::condition_variable started;
  std::condition_variable released;
  bool active = false;
  bool release = false;
};

struct Cleanup {
  std::function<void()> action;
  ~Cleanup() { action(); }
};

LlamaSettings settings() {
  LlamaSettings value;
  value.embeddings.enabled = true;
  value.models_configured = true;
  value.models.push_back(LlamaModelSettings{"fake", "fake", "", {}, {}, {}, true, {}});
  return value;
}

LlamaEmbeddingRequest request(const std::function<bool()>& cancelled) {
  LlamaEmbeddingRequest value;
  value.model_id = "fake";
  value.text = "probe";
  value.cancelled = cancelled;
  return value;
}

}  // namespace

int main() {
  try {
    auto backend = std::make_unique<BlockingBackend>();
    auto* backend_view = backend.get();
    LlamaProvider provider(settings(), std::move(backend));
    // Constructor injection supplies the chat-tag backend; queue behavior is shared.
    const auto embed = [&](const LlamaEmbeddingRequest& value) {
      return provider.embed(value, LlamaInstanceTag::kChat);
    };

    {
      auto invalid = request([] { return false; });
      invalid.queue_timeout = std::chrono::milliseconds(-1);
      bool rejected = false;
      try { (void)embed(invalid); }
      catch (const std::invalid_argument&) { rejected = true; }
      require(rejected, "negative queue timeout was accepted");
      auto cancelled = request([] { return true; });
      bool stopped = false;
      try { (void)embed(cancelled); }
      catch (const std::runtime_error&) { stopped = true; }
      require(stopped, "already-cancelled embedding was accepted");
      auto expired = request([] { return false; });
      expired.queue_timeout = std::chrono::milliseconds(0);
      bool timed_out = false;
      try { (void)embed(expired); }
      catch (const std::runtime_error& error) {
        timed_out = std::string(error.what()) == "llama embedding queue timeout";
      }
      require(timed_out && backend_view->calls.load() == 0,
              "zero-deadline embedding reached backend");
    }

    std::atomic_bool cancel_queued{false};
    auto active = std::async(std::launch::async, [&] {
      return embed(request([] { return false; }));
    });
    Cleanup release_on_failure{[&] { backend_view->release_active(); }};
    backend_view->wait_started();

    std::atomic_int queued_callback_calls{0};
    auto queued_cancel = std::async(std::launch::async, [&] {
      return embed(request([&] {
        ++queued_callback_calls;
        return cancel_queued.load();
      }));
    });
    for (int i = 0; i != 200 && queued_callback_calls.load() < 2; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(queued_callback_calls.load() >= 2,
            "queued cancellation callback was not observed after enqueue");
    cancel_queued = true;
    require(queued_cancel.wait_for(std::chrono::milliseconds(200)) ==
                std::future_status::ready,
            "queued cancellation did not return before active work released");
    bool queued_failed = false;
    try { (void)queued_cancel.get(); } catch (const std::runtime_error&) { queued_failed = true; }
    require(queued_failed, "queued cancellation succeeded");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(backend_view->calls.load() == 1, "cancelled queued work reached backend");

    auto timed = std::async(std::launch::async, [&] {
      auto value = request([] { return false; });
      value.queue_timeout = std::chrono::milliseconds(20);
      return embed(value);
    });
    require(timed.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready,
            "queued timeout did not return before active work released");
    bool timed_failed = false;
    try { (void)timed.get(); } catch (const std::runtime_error&) { timed_failed = true; }
    require(timed_failed, "queued timeout succeeded");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(backend_view->calls.load() == 1, "timed out queued work reached backend");

    backend_view->release_active();
    require(active.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
            "active embedding did not join after release");
    (void)active.get();
    auto retry = embed(request([] { return false; }));
    require(retry.embedding.size() == 1 && backend_view->calls.load() == 2,
            "retry after queued cancellation/timeout did not reach backend");

    std::atomic_bool cancel_active{false};
    auto cooperative = std::async(std::launch::async, [&] {
      return embed(request([&] { return cancel_active.load(); }));
    });
    Cleanup cancel_on_failure{[&] { cancel_active = true; }};
    backend_view->wait_started();
    cancel_active = true;
    require(cooperative.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
            "active cancellation did not wait for cooperative backend completion");
    bool cooperative_failed = false;
    try { (void)cooperative.get(); } catch (const std::runtime_error&) { cooperative_failed = true; }
    require(cooperative_failed, "active cancellation succeeded");
    std::cout << "llama embedding queue probe passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "llama embedding queue probe failed: " << error.what() << '\n';
    return 1;
  }
}
