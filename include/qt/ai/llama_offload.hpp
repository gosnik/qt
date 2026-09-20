#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace qt::ai {

struct LlamaGpuMemory {
  std::size_t index = 0;
  std::uint64_t free_bytes = 0;
  std::uint64_t total_bytes = 0;
};

struct LlamaOffloadPlanInput {
  std::optional<int> configured_gpu_layers;
  std::optional<std::int64_t> block_count;
  std::uint64_t model_size_bytes = 0;
  std::vector<LlamaGpuMemory> devices;
};

struct LlamaOffloadPlan {
  std::vector<int> gpu_layer_attempts;
  std::uint64_t free_bytes = 0;
  std::uint64_t usable_bytes = 0;
  std::uint64_t reserved_bytes = 0;
  std::int64_t estimated_block_count = 0;
  bool automatic = false;
  bool block_count_estimated = false;
};

LlamaOffloadPlan make_llama_offload_plan(
    const LlamaOffloadPlanInput& input);

}  // namespace qt::ai
