#pragma once

#include "qt/app/runtime/isolated_agent_runner.hpp"
#include "qt/foundation/process.hpp"
#include "qt/memory/context_service.hpp"
#include "qt/memory/prehandler_protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace qt::memory {

struct MemoryCoordinatorSettings {
  bool enabled = false;
  bool automatic_recall = true;
  RecallPolicy recall_policy = RecallPolicy::Balanced;
  bool include_global_user = false;
  std::size_t context_token_budget = 900U;
  std::size_t candidate_limit = 50U;
  GraphRecallOptions graph;
  bool dense_enabled = false;
  std::string model;
  std::string thinking = "off";
  std::int64_t timeout_ms = 30000;
  bool fail_open = true;
  std::size_t max_model_input_tokens = 12000U;
  std::vector<std::string> model_discovery_roots;
  std::string model_task = "context_compilation";
  std::string agent_home;
  std::string cwd;
};

struct MemoryAugmentation {
  std::optional<ai::Message> message;
  MemoryContextContribution contribution;
  bool used_isolated_agent = false;
  bool used_deterministic_fallback = false;
  std::vector<std::string> diagnostics;
};

class MemoryCoordinator final {
 public:
  using CodeCandidateProvider = std::function<std::vector<RetrievalCandidate>(
      const MemoryScope&, const RecallRequest&, const std::vector<RecalledMemory>&)>;
  MemoryCoordinator(
      RecallService& recall, ContextService& context, MemoryScope scope,
      MemoryCoordinatorSettings settings,
      std::shared_ptr<app::runtime::IsolatedAgentRunner> isolated_runner,
      SessionEventRepository* receipts = nullptr,
      CodeCandidateProvider code_candidates = {});

  const MemoryScope& scope() const noexcept { return scope_; }
  const MemoryCoordinatorSettings& settings() const noexcept {
    return settings_;
  }

  foundation::Result<MemoryAugmentation> augment(
      std::uint64_t projection_epoch,
      std::uint64_t logical_input_sequence,
      const ai::CompletionRequest& model_request,
      std::shared_ptr<foundation::ProcessCancellationToken> cancellation =
          nullptr);
  void set_scope(MemoryScope scope);
  void reset();
  // Latest prepared transient context in the current projection. Inspection
  // does not run recall or establish that a provider consumed the contribution.
  std::optional<MemoryAugmentation> last_augmentation(std::uint64_t projection_epoch) const;

 private:
  MemoryAugmentation prepare(
      std::uint64_t logical_input_sequence,
      const ai::CompletionRequest& model_request,
      std::shared_ptr<foundation::ProcessCancellationToken> cancellation);

  RecallService& recall_;
  ContextService& context_;
  MemoryScope scope_;
  MemoryCoordinatorSettings settings_;
  std::shared_ptr<app::runtime::IsolatedAgentRunner> isolated_runner_;
  SessionEventRepository* receipts_ = nullptr;
  CodeCandidateProvider code_candidates_;
  std::mutex prepare_mutex_;
  mutable std::mutex mutex_;
  std::uint64_t projection_epoch_ = 0U;
  std::unordered_map<std::uint64_t, MemoryAugmentation> cache_;
};

}  // namespace qt::memory
