#pragma once

#include "qt/ai/models.hpp"
#include "qt/foundation/error.hpp"
#include "qt/memory/models.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace qt::memory {

constexpr std::uint32_t kMemoryPrehandlerProtocolVersion = 1U;
constexpr const char* kMemoryPrehandlerPromptVersion = "memory-prehandler-v1";

struct MemoryContextWarning {
  std::string code;
  std::string memory_id;
  std::string from_id;
  std::string to_id;
  std::string message;
};

struct MemoryPrehandlerRequest {
  std::uint32_t version = kMemoryPrehandlerProtocolVersion;
  std::uint64_t logical_input_sequence = 0;
  ai::ModelRef model;
  std::optional<std::int64_t> context_window;
  MemoryScope scope;
  std::string cwd;
  RecallPolicy policy = RecallPolicy::Balanced;
  std::size_t context_token_budget = 900U;
  std::size_t max_model_input_tokens = 12000U;
  std::string prompt_version = kMemoryPrehandlerPromptVersion;
  std::string config_hash;
  std::vector<ai::Message> model_input;
  std::vector<RecalledMemory> candidates;
  std::vector<ContextBlock> pinned_blocks;
  std::vector<MemoryContextWarning> warnings;
  std::string required_context_markdown;
  std::vector<std::string> required_memory_ids;
};

struct MemoryContextContribution {
  std::uint32_t version = kMemoryPrehandlerProtocolVersion;
  std::uint64_t logical_input_sequence = 0;
  std::string context_markdown;
  std::vector<std::string> memory_ids;
  std::vector<std::string> block_ids;
  std::vector<MemoryContextWarning> warnings;
  std::size_t estimated_tokens = 0U;
  std::string omission_reason;
  std::string candidate_pack_hash;
  std::string output_hash;
  std::vector<std::string> code_ids;
};

const std::string& memory_prehandler_system_prompt();
std::string memory_candidate_pack_hash(const MemoryPrehandlerRequest& request);
std::string memory_contribution_hash(
    const MemoryContextContribution& contribution);
ai::Json encode_memory_prehandler_request(
    const MemoryPrehandlerRequest& request);
foundation::Result<MemoryContextContribution>
decode_and_validate_memory_contribution(
    const ai::Json& value, const MemoryPrehandlerRequest& request);
foundation::Result<MemoryContextContribution>
decode_and_validate_memory_contribution(
    std::string_view text, const MemoryPrehandlerRequest& request);

}  // namespace qt::memory
