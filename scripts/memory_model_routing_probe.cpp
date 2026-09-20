#include "qt/ai/llama_model_discovery.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

namespace fs = std::filesystem;

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

qt::ai::LlamaModelSettings model(const fs::path& root, std::string id) {
  const fs::path path = root / (id + ".gguf");
  std::ofstream(path, std::ios::binary).write("GGUF", 4);
  qt::ai::LlamaModelSettings result;
  result.id = std::move(id);
  result.name = result.id;
  result.path = path.generic_string();
  return result;
}

void require_selected(const qt::ai::LlamaSettings& settings,
                      qt::ai::LlamaMemoryTask task,
                      const std::string& expected) {
  const auto* selected =
      qt::ai::select_llama_model_for_memory_task(settings, task);
  require(selected != nullptr,
          "no model selected for " + qt::ai::to_string(task));
  require(selected->id == expected,
          qt::ai::to_string(task) + " selected " + selected->id +
              " instead of " + expected);
}

void run(const fs::path& root) {
  fs::create_directories(root);
  qt::ai::LlamaSettings settings;
  settings.models = {
      model(root, "nomic-embed-text-v1.5-q8_0"),
      model(root, "bge-small-en-v1.5-q8_0"),
      model(root, "smollm2-135m-instruct-q8_0"),
      model(root, "qwen2.5-0.5b-instruct-q8_0"),
      model(root, "qwen2.5-3b-instruct-q4_k_m"),
      model(root, "ternary-bonsai-27b-q2_0"),
  };

  require_selected(settings, qt::ai::LlamaMemoryTask::document_embedding,
                   "nomic-embed-text-v1.5-q8_0");
  require_selected(settings, qt::ai::LlamaMemoryTask::short_embedding,
                   "bge-small-en-v1.5-q8_0");
  require_selected(settings, qt::ai::LlamaMemoryTask::worthiness,
                   "smollm2-135m-instruct-q8_0");
  require_selected(settings, qt::ai::LlamaMemoryTask::extraction,
                   "qwen2.5-0.5b-instruct-q8_0");
  require_selected(settings, qt::ai::LlamaMemoryTask::recall_prehandler,
                   "qwen2.5-3b-instruct-q4_k_m");
  require_selected(settings, qt::ai::LlamaMemoryTask::context_compilation,
                   "ternary-bonsai-27b-q2_0");

  fs::remove(root / "ternary-bonsai-27b-q2_0.gguf");
  require_selected(settings, qt::ai::LlamaMemoryTask::context_compilation,
                   "qwen2.5-3b-instruct-q4_k_m");

  settings.models.push_back(model(root, "a-ternary-bonsai-27b-q2_0"));
  settings.models.push_back(model(root, "z-ternary-bonsai-27b-q2_0"));
  require_selected(settings, qt::ai::LlamaMemoryTask::context_compilation,
                   "a-ternary-bonsai-27b-q2_0");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 2, "usage: memory_model_routing_probe <temporary-dir>");
    run(fs::path(argv[1]));
    std::cout << "memory model routing probe passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "memory model routing probe failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
