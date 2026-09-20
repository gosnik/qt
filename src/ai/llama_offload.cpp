#include "qt/ai/llama_offload.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace qt::ai {
namespace {

constexpr std::uint64_t kDeviceHeadroomBytes = 1024ULL * 1024ULL * 1024ULL;
constexpr std::int64_t kDefaultBlockCount = 32;

void append_unique(std::vector<int>& attempts, int value) {
  value = std::max(0, value);
  if (std::find(attempts.begin(), attempts.end(), value) == attempts.end()) {
    attempts.push_back(value);
  }
}

int scaled_layers(int layers, double scale) {
  return std::max(1, static_cast<int>(
                         std::floor(static_cast<double>(layers) * scale)));
}

}  // namespace

LlamaOffloadPlan make_llama_offload_plan(
    const LlamaOffloadPlanInput& input) {
  LlamaOffloadPlan plan;
  for (const auto& device : input.devices) {
    plan.free_bytes += device.free_bytes;
    const auto reserved = std::min(device.free_bytes, kDeviceHeadroomBytes);
    plan.reserved_bytes += reserved;
    plan.usable_bytes += device.free_bytes - reserved;
  }
  if (input.configured_gpu_layers) {
    append_unique(plan.gpu_layer_attempts, *input.configured_gpu_layers);
    if (*input.configured_gpu_layers > 0) {
      append_unique(plan.gpu_layer_attempts, 0);
    }
    return plan;
  }

  plan.automatic = true;
  if (plan.usable_bytes == 0 || input.model_size_bytes == 0) {
    plan.gpu_layer_attempts.push_back(0);
    return plan;
  }

  const auto configured_blocks = input.block_count.value_or(0);
  plan.block_count_estimated = configured_blocks <= 0;
  plan.estimated_block_count =
      configured_blocks > 0 ? configured_blocks : kDefaultBlockCount;
  const auto total_layers = std::min<std::int64_t>(
      plan.estimated_block_count + 1, std::numeric_limits<int>::max());

  int initial_layers = 999;
  if (plan.usable_bytes < input.model_size_bytes) {
    const long double fraction =
        static_cast<long double>(plan.usable_bytes) /
        static_cast<long double>(input.model_size_bytes);
    initial_layers = std::max(
        1, static_cast<int>(std::floor(fraction * total_layers)));
  }

  append_unique(plan.gpu_layer_attempts, initial_layers);
  const int fallback_base =
      initial_layers == 999 ? static_cast<int>(total_layers) : initial_layers;
  append_unique(plan.gpu_layer_attempts, scaled_layers(fallback_base, 0.75));
  append_unique(plan.gpu_layer_attempts, scaled_layers(fallback_base, 0.5));
  append_unique(plan.gpu_layer_attempts, scaled_layers(fallback_base, 0.25));
  append_unique(plan.gpu_layer_attempts, 0);
  return plan;
}

}  // namespace qt::ai
