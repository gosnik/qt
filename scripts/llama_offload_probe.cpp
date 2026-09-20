#include "qt/ai/llama_offload.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void require_attempts(const qt::ai::LlamaOffloadPlan& plan,
                      const std::vector<int>& expected,
                      const std::string& scenario) {
  require(plan.gpu_layer_attempts == expected,
          scenario + " produced an unexpected retry ladder");
}

}  // namespace

int main() {
  try {
    qt::ai::LlamaOffloadPlanInput input;
    input.configured_gpu_layers = 17;
    require_attempts(qt::ai::make_llama_offload_plan(input), {17, 0},
                     "configured GPU offload");

    input.configured_gpu_layers = 0;
    require_attempts(qt::ai::make_llama_offload_plan(input), {0},
                     "configured CPU execution");

    input = {};
    input.model_size_bytes = 8 * kGiB;
    input.block_count = 31;
    require_attempts(qt::ai::make_llama_offload_plan(input), {0},
                     "automatic execution without a GPU");

    input.devices = {{0, 10 * kGiB, 12 * kGiB}};
    auto plan = qt::ai::make_llama_offload_plan(input);
    require_attempts(plan, {999, 24, 16, 8, 0}, "full GPU fit");
    require(plan.usable_bytes == 9 * kGiB,
            "full-fit plan must reserve one GiB of device headroom");

    input.model_size_bytes = 10 * kGiB;
    input.block_count = 39;
    input.devices = {{0, 6 * kGiB, 8 * kGiB}};
    plan = qt::ai::make_llama_offload_plan(input);
    require_attempts(plan, {20, 15, 10, 5, 0}, "partial GPU fit");

    input.model_size_bytes = 6 * kGiB;
    input.devices = {{0, 4 * kGiB, 8 * kGiB},
                     {1, 4 * kGiB, 8 * kGiB}};
    require_attempts(qt::ai::make_llama_offload_plan(input),
                     {999, 30, 20, 10, 0}, "multi-GPU fit");

    input.model_size_bytes = 8 * kGiB;
    input.block_count.reset();
    input.devices = {{0, 5 * kGiB, 8 * kGiB}};
    plan = qt::ai::make_llama_offload_plan(input);
    require_attempts(plan, {16, 12, 8, 4, 0},
                     "missing block-count estimate");
    require(plan.block_count_estimated && plan.estimated_block_count == 32,
            "missing GGUF block count must use the documented estimate");

    std::cout << "llama offload probe passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "llama offload probe failed: " << error.what() << '\n';
    return 1;
  }
}
