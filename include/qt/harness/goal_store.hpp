#pragma once

#include <qt/harness/goal.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace qt::harness {

/// Update specification for modifying an existing goal.
struct GoalUpdate {
  std::optional<std::string> objective;
  std::optional<ThreadGoalStatus> status;
  std::optional<std::optional<std::int64_t>> token_budget;  ///< nullopt = no change, Some(None) = clear
  std::optional<std::string> expected_goal_id;  ///< For optimistic locking
};

/// Outcome of a usage accounting operation.
enum class GoalAccountingOutcomeKind {
  kUnchanged,  ///< Goal was not modified
  kUpdated,    ///< Goal was updated with new usage
};

struct GoalAccountingOutcome {
  GoalAccountingOutcomeKind kind = GoalAccountingOutcomeKind::kUnchanged;
  std::optional<ThreadGoal> goal;
};

/// In-memory goal store for tracking thread goals.
/// Thread-safe for concurrent access.
class GoalStore {
 public:
  GoalStore() = default;
  ~GoalStore() = default;

  GoalStore(const GoalStore&) = delete;
  GoalStore& operator=(const GoalStore&) = delete;
  GoalStore(GoalStore&&) = default;
  GoalStore& operator=(GoalStore&&) = default;

  /// Gets the current goal for a session, if any.
  std::optional<ThreadGoal> get_session_goal(const std::string& session_id) const;

  /// Creates or replaces the goal for a session.
  /// Returns the created/replaced goal.
  ThreadGoal replace_session_goal(
      const std::string& session_id,
      std::string objective,
      ThreadGoalStatus status = ThreadGoalStatus::kActive,
      std::optional<std::int64_t> token_budget = std::nullopt);

  /// Inserts a goal only if no active goal exists for the session.
  /// Returns the inserted goal, or nullopt if an active goal already exists.
  std::optional<ThreadGoal> insert_session_goal(
      const std::string& session_id,
      std::string objective,
      ThreadGoalStatus status = ThreadGoalStatus::kActive,
      std::optional<std::int64_t> token_budget = std::nullopt);

  /// Updates an existing goal with partial changes.
  /// Returns the updated goal, or nullopt if no goal exists or expected_goal_id mismatch.
  std::optional<ThreadGoal> update_session_goal(
      const std::string& session_id,
      const GoalUpdate& update);

  /// Pauses an active goal.
  /// Returns the paused goal, or nullopt if no active goal exists.
  std::optional<ThreadGoal> pause_active_session_goal(const std::string& session_id);

  /// Marks an active goal as usage limited.
  /// Returns the updated goal, or nullopt if no active goal exists.
  std::optional<ThreadGoal> usage_limit_active_session_goal(const std::string& session_id);

  /// Marks an active goal as complete.
  /// Returns the completed goal, or nullopt if no active goal exists.
  std::optional<ThreadGoal> complete_active_session_goal(const std::string& session_id);

  /// Deletes the goal for a session.
  /// Returns the deleted goal, or nullopt if no goal existed.
  std::optional<ThreadGoal> delete_session_goal(const std::string& session_id);

  /// Accounts usage for a goal, potentially transitioning to budget_limited.
  /// Returns the outcome with the updated goal if modified.
  GoalAccountingOutcome account_session_goal_usage(
      const std::string& session_id,
      std::int64_t time_delta_seconds,
      std::int64_t token_delta,
      std::optional<std::string> expected_goal_id = std::nullopt);

  /// Clears all goals (for testing or shutdown).
  void clear();

 private:
  std::string generate_goal_id() const;
  ThreadGoalStatus apply_budget_limit(ThreadGoalStatus status,
                                       std::int64_t tokens_used,
                                       std::optional<std::int64_t> token_budget) const;

  mutable std::mutex mutex_;
  std::unordered_map<std::string, ThreadGoal> goals_;
};

}  // namespace qt::harness
