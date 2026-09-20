#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <qt/ai/provider.hpp>
#include <qt/foundation/error.hpp>
#include <qt/harness/session.hpp>
#include <qt/harness/session_graph.hpp>

namespace qt::harness {

constexpr const char* kCompactedSessionEntryType = "compacted_session";

struct SessionCompactionResult {
  SessionBranchSnapshot snapshot;
  std::size_t original_entry_count = 0;
  std::size_t kept_entry_count = 0;
  std::size_t compacted_entry_count = 0;
  std::vector<SessionEntry> kept_entries;
  SessionEntry synthetic_entry;

  bool has_label() const noexcept { return snapshot.latest_label.has_value(); }

  bool has_summary() const noexcept {
    return snapshot.latest_summary.has_value();
  }
};

struct CompactSessionRepositoryRequest {
  std::string repository_path;
  bool persist = false;
  bool include_synthetic_entry = true;
};

struct CompactSessionRepositoryResult {
  std::string repository_path;
  bool persisted = false;
  bool included_synthetic_entry = true;
  std::vector<SessionEntry> rewritten_entries;
  SessionCompactionResult compaction;
};

struct CompactSessionRepositorySummary {
  std::vector<std::string> lines;
  bool persisted = false;
  bool included_synthetic_entry = true;
  std::size_t original_entry_count = 0;
  std::size_t kept_entry_count = 0;
  std::size_t compacted_entry_count = 0;
  std::size_t rewritten_entry_count = 0;
};

struct ModelBackedCompactionRequest {
  qt::ai::ModelRef model;
  qt::ai::ProviderOptions provider_options;
  bool enabled = true;
  std::string reason = "manual";
  bool will_retry = false;
  std::int64_t reserve_tokens = 16384;
  std::int64_t keep_recent_tokens = 20000;
  std::int64_t model_max_output_tokens = 0;
  std::optional<std::string> custom_instructions;
};

struct ModelBackedCompactionPlan {
  qt::ai::CompletionRequest request;
  qt::ai::ProviderOptions provider_options;
  std::string first_kept_entry_id;
  std::size_t first_kept_entry_index = 0;
  std::int64_t tokens_before = 0;
  std::int64_t summarized_tokens = 0;
  std::int64_t turn_prefix_tokens = 0;
  std::int64_t kept_tokens = 0;
  bool is_split_turn = false;
  std::optional<std::string> previous_summary;
  std::optional<qt::ai::CompletionRequest> turn_prefix_request;
  std::vector<SessionEntry> entries_to_summarize;
  std::vector<SessionEntry> turn_prefix_entries;
  std::vector<SessionEntry> kept_entries;
  Json details = Json::object();
};

struct ModelBackedCompactionResult {
  std::string summary;
  std::string first_kept_entry_id;
  std::int64_t tokens_before = 0;
  Json details = Json::object();
  ModelBackedCompactionPlan plan;
  qt::ai::CompletionResponse response;
  std::optional<qt::ai::CompletionResponse> turn_prefix_response;
};

struct ModelBackedBranchSummaryRequest {
  qt::ai::ModelRef model;
  qt::ai::ProviderOptions provider_options;
  std::optional<std::string> custom_instructions;
  bool replace_instructions = false;
  std::int64_t reserve_tokens = 16384;
  std::int64_t model_max_output_tokens = 2048;
};

struct ModelBackedBranchSummaryResult {
  std::string summary;
  Json details = Json::object();
  qt::ai::CompletionRequest request;
  qt::ai::CompletionResponse response;
};

qt::foundation::Result<SessionCompactionResult> compact_session_entries(
    const std::vector<SessionEntry>& entries);

qt::foundation::Result<ModelBackedCompactionPlan>
build_model_backed_compaction_plan(
    const std::vector<SessionEntry>& entries,
    const ModelBackedCompactionRequest& request);

qt::foundation::Result<ModelBackedCompactionResult>
compact_session_entries_with_model(
    const std::vector<SessionEntry>& entries,
    const ModelBackedCompactionRequest& request,
    qt::ai::Provider& provider);

qt::foundation::Result<ModelBackedBranchSummaryResult>
summarize_branch_entries_with_model(
    const std::vector<SessionEntry>& entries,
    const ModelBackedBranchSummaryRequest& request,
    qt::ai::Provider& provider);

qt::foundation::Result<CompactSessionRepositoryResult>
compact_session_repository(const CompactSessionRepositoryRequest& request);

qt::foundation::Result<CompactSessionRepositorySummary>
render_compact_session_repository_summary(
    const CompactSessionRepositoryResult& result);

}  // namespace qt::harness
