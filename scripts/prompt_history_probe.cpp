#include "qt/app/config/prompt_history.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace {

using qt::app::PromptHistoryStore;

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::vector<std::string> load_prompts(const PromptHistoryStore& store,
                                      bool expected_file_exists = true) {
  const auto loaded = store.load();
  require(loaded.ok(), loaded.ok() ? std::string{} : loaded.error().summary());
  require(loaded.value().file_exists == expected_file_exists,
          "unexpected prompt history file existence state");
  return loaded.value().prompts;
}

void require_prompts(const std::vector<std::string>& actual,
                     const std::vector<std::string>& expected,
                     const std::string& message) {
  require(actual == expected, message);
}

void test_missing_and_bounded_history(const std::filesystem::path& home) {
  PromptHistoryStore store(home, 2U);
  require(store.path() == home / "history.jsonl",
          "history path must be rooted directly under the qt home");
  require_prompts(load_prompts(store, false), {},
                  "missing history must load as empty");

  require(store.append("  first prompt  ").ok(), "first append failed");
  require(store.append("first prompt").ok(), "duplicate append failed");
  require(store.append("second\nprompt").ok(), "multiline append failed");
  require(store.append("first prompt").ok(), "non-adjacent append failed");
  require_prompts(load_prompts(store), {"first prompt", "second\nprompt"},
                  "history must be newest-first, bounded, and only suppress adjacent duplicates");

#if !defined(_WIN32)
  struct stat metadata {};
  require(::stat(store.path().c_str(), &metadata) == 0,
          "unable to stat prompt history");
  require((metadata.st_mode & 0777) == 0600,
          "prompt history must be owner-readable and owner-writable only");
  require(::chmod(store.path().c_str(), 0644) == 0,
          "unable to loosen prompt history fixture permissions");
  require(store.load().ok(), "unable to reload prompt history permissions");
  require(::stat(store.path().c_str(), &metadata) == 0,
          "unable to restat prompt history");
  require((metadata.st_mode & 0777) == 0600,
          "loading prompt history must restore owner-only permissions");
#endif

  PromptHistoryStore one_entry(home, 1U);
  require_prompts(load_prompts(one_entry), {"first prompt"},
                  "loading with a smaller limit must prune oldest entries");
  PromptHistoryStore restored(home, 10U);
  require_prompts(load_prompts(restored), {"first prompt"},
                  "pruning must be persisted to disk");
}

void test_concurrent_writers(const std::filesystem::path& home) {
  PromptHistoryStore reset(home, 100U);
  require(reset.save({}).ok(), "unable to reset concurrent history");

  std::mutex error_mutex;
  std::string writer_error;
  const auto writer = [home, &error_mutex,
                       &writer_error](const std::string& prefix) {
    PromptHistoryStore store(home, 100U);
    for (int index = 0; index < 25; ++index) {
      const auto status = store.append(prefix + std::to_string(index));
      if (!status) {
        std::lock_guard<std::mutex> lock(error_mutex);
        writer_error = status.error().summary();
        return;
      }
    }
  };
  std::thread first(writer, "first-");
  std::thread second(writer, "second-");
  first.join();
  second.join();
  require(writer_error.empty(), "concurrent writer failed: " + writer_error);

  const auto prompts = load_prompts(reset);
  require(prompts.size() == 50U,
          "locked concurrent writers must retain every prompt");
  for (int index = 0; index < 25; ++index) {
    require(std::find(prompts.begin(), prompts.end(),
                      "first-" + std::to_string(index)) != prompts.end(),
            "first concurrent writer lost an entry");
    require(std::find(prompts.begin(), prompts.end(),
                      "second-" + std::to_string(index)) != prompts.end(),
            "second concurrent writer lost an entry");
  }
}

void test_recovery_and_disable(const std::filesystem::path& home) {
  PromptHistoryStore store(home, 3U);
  require(store.save({"newest", "older", "oldest", "discarded"}).ok(),
          "history save failed");
  require_prompts(load_prompts(store), {"newest", "older", "oldest"},
                  "save must retain only the configured newest prompts");

  const auto corrupt = qt::foundation::write_text_file(
      store.path(), "{not valid json}\n");
  require(corrupt.ok(), "unable to create corrupt history fixture");
  require(!store.load().ok(), "corrupt history must return a deterministic error");
  require(store.save({"recovered"}).ok(),
          "explicit save must recover corrupt history");
  require_prompts(load_prompts(store), {"recovered"},
                  "recovered history was not readable");

  PromptHistoryStore disabled(home, 0U);
  require_prompts(load_prompts(disabled), {},
                  "zero limit must clear loaded history");
  require(!std::filesystem::exists(disabled.path()),
          "zero limit must remove persisted prompt history");
  require(disabled.append("must not persist").ok(),
          "disabled append must succeed without persistence");
  require(!std::filesystem::exists(disabled.path()),
          "disabled history must not recreate the history file");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 2, "usage: prompt_history_probe <temporary-agent-home>");
    const std::filesystem::path root(argv[1]);
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    test_missing_and_bounded_history(root / "bounded");
    test_concurrent_writers(root / "concurrent");
    test_recovery_and_disable(root / "recovery");
    std::cout << "prompt history probe passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "prompt history probe failed: " << error.what() << '\n';
    return 1;
  }
}
