#include "qt/memory/models.hpp"

#include <stdexcept>

namespace qt::memory {
namespace {

template <typename Enum>
Enum invalid_value(const char* kind, const std::string& value) {
  throw std::invalid_argument(std::string("unknown ") + kind + ": " + value);
}

}  // namespace

const char* to_string(MemoryType value) noexcept {
  switch (value) {
    case MemoryType::Semantic: return "semantic";
    case MemoryType::Episodic: return "episodic";
    case MemoryType::Procedural: return "procedural";
    case MemoryType::Decision: return "decision";
    case MemoryType::Constraint: return "constraint";
    case MemoryType::Bug: return "bug";
    case MemoryType::Preference: return "preference";
    case MemoryType::Skill: return "skill";
    case MemoryType::Observation: return "observation";
    case MemoryType::Todo: return "todo";
    case MemoryType::Unprocessed: return "unprocessed";
    case MemoryType::Dream: return "dream";
    case MemoryType::Conversation: return "conversation";
    case MemoryType::Tool: return "tool";
    case MemoryType::Pattern: return "pattern";
    case MemoryType::Raw: return "raw";
    case MemoryType::Research: return "research";
  }
  return "observation";
}

const char* to_string(MemoryStatus value) noexcept {
  switch (value) {
    case MemoryStatus::Candidate: return "candidate";
    case MemoryStatus::Hypothesis: return "hypothesis";
    case MemoryStatus::Active: return "active";
    case MemoryStatus::Validated: return "validated";
    case MemoryStatus::Deprecated: return "deprecated";
    case MemoryStatus::Superseded: return "superseded";
    case MemoryStatus::Rejected: return "rejected";
    case MemoryStatus::Expired: return "expired";
    case MemoryStatus::DreamArtifact: return "dream_artifact";
  }
  return "hypothesis";
}

const char* to_string(SourceType value) noexcept {
  switch (value) {
    case SourceType::Chat: return "chat";
    case SourceType::File: return "file";
    case SourceType::Commit: return "commit";
    case SourceType::Issue: return "issue";
    case SourceType::PullRequest: return "pull_request";
    case SourceType::Document: return "doc";
    case SourceType::Manual: return "manual";
    case SourceType::ToolResult: return "tool_result";
    case SourceType::TestLog: return "test_log";
    case SourceType::ProcessedText: return "processed_text";
    case SourceType::Dreaming: return "dreaming";
    case SourceType::Unknown: return "unknown";
  }
  return "unknown";
}

const char* to_string(ScopeKind value) noexcept {
  switch (value) {
    case ScopeKind::Exact: return "exact";
    case ScopeKind::Branch: return "branch";
    case ScopeKind::Repository: return "repository";
    case ScopeKind::Project: return "project";
    case ScopeKind::GlobalUser: return "global_user";
  }
  return "exact";
}

const char* to_string(ReviewStatus value) noexcept {
  switch (value) {
    case ReviewStatus::Pending: return "pending";
    case ReviewStatus::Accepted: return "accepted";
    case ReviewStatus::Rejected: return "rejected";
    case ReviewStatus::Deferred: return "deferred";
  }
  return "pending";
}

const char* to_string(ProcedureOutcome value) noexcept {
  switch (value) {
    case ProcedureOutcome::Success: return "success";
    case ProcedureOutcome::Failure: return "failure";
    case ProcedureOutcome::Partial: return "partial";
    case ProcedureOutcome::Unused: return "unused";
    case ProcedureOutcome::Unknown: return "unknown";
  }
  return "unknown";
}

MemoryType memory_type_from_string(const std::string& value) {
  if (value == "semantic") return MemoryType::Semantic;
  if (value == "episodic" || value == "episode") return MemoryType::Episodic;
  if (value == "procedural" || value == "procedure") return MemoryType::Procedural;
  if (value == "decision") return MemoryType::Decision;
  if (value == "constraint") return MemoryType::Constraint;
  if (value == "bug" || value == "failure") return MemoryType::Bug;
  if (value == "preference") return MemoryType::Preference;
  if (value == "skill") return MemoryType::Skill;
  if (value == "observation") return MemoryType::Observation;
  if (value == "todo") return MemoryType::Todo;
  if (value == "unprocessed") return MemoryType::Unprocessed;
  if (value == "dream") return MemoryType::Dream;
  if (value == "conversation") return MemoryType::Conversation;
  if (value == "tool") return MemoryType::Tool;
  if (value == "pattern") return MemoryType::Pattern;
  if (value == "raw") return MemoryType::Raw;
  if (value == "research") return MemoryType::Research;
  return invalid_value<MemoryType>("memory type", value);
}

MemoryStatus memory_status_from_string(const std::string& value) {
  if (value == "candidate") return MemoryStatus::Candidate;
  if (value == "hypothesis") return MemoryStatus::Hypothesis;
  if (value == "active") return MemoryStatus::Active;
  if (value == "validated") return MemoryStatus::Validated;
  if (value == "deprecated") return MemoryStatus::Deprecated;
  if (value == "superseded") return MemoryStatus::Superseded;
  if (value == "rejected") return MemoryStatus::Rejected;
  if (value == "expired") return MemoryStatus::Expired;
  if (value == "dream_artifact") return MemoryStatus::DreamArtifact;
  return invalid_value<MemoryStatus>("memory status", value);
}

SourceType source_type_from_string(const std::string& value) noexcept {
  if (value == "chat") return SourceType::Chat;
  if (value == "file") return SourceType::File;
  if (value == "commit") return SourceType::Commit;
  if (value == "issue") return SourceType::Issue;
  if (value == "pull_request" || value == "pr") return SourceType::PullRequest;
  if (value == "doc" || value == "document") return SourceType::Document;
  if (value == "manual") return SourceType::Manual;
  if (value == "tool_result") return SourceType::ToolResult;
  if (value == "test_log") return SourceType::TestLog;
  if (value == "processed_text") return SourceType::ProcessedText;
  if (value == "dreaming") return SourceType::Dreaming;
  return SourceType::Unknown;
}

ScopeKind scope_kind_from_string(const std::string& value) {
  if (value == "exact") return ScopeKind::Exact;
  if (value == "branch") return ScopeKind::Branch;
  if (value == "repository") return ScopeKind::Repository;
  if (value == "project") return ScopeKind::Project;
  if (value == "global_user") return ScopeKind::GlobalUser;
  return invalid_value<ScopeKind>("scope kind", value);
}

ReviewStatus review_status_from_string(const std::string& value) {
  if (value == "pending") return ReviewStatus::Pending;
  if (value == "accepted") return ReviewStatus::Accepted;
  if (value == "rejected") return ReviewStatus::Rejected;
  if (value == "deferred") return ReviewStatus::Deferred;
  return invalid_value<ReviewStatus>("review status", value);
}

ProcedureOutcome procedure_outcome_from_string(const std::string& value) noexcept {
  if (value == "success") return ProcedureOutcome::Success;
  if (value == "failure") return ProcedureOutcome::Failure;
  if (value == "partial") return ProcedureOutcome::Partial;
  if (value == "unused") return ProcedureOutcome::Unused;
  return ProcedureOutcome::Unknown;
}

}  // namespace qt::memory
