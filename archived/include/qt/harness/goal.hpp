#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace qt::harness {

/// Status of a thread goal, tracking its lifecycle from active to terminal states.
enum class ThreadGoalStatus {
  kActive,         ///< Goal is actively being worked on
  kPaused,         ///< Goal is temporarily paused by user
  kBlocked,        ///< Goal is blocked waiting for something
  kUsageLimited,   ///< Goal hit platform usage limits
  kBudgetLimited,  ///< Goal exhausted its token budget
  kComplete,       ///< Goal completed successfully
};

/// Returns a human-readable label for the status.
const char* to_string(ThreadGoalStatus status) noexcept;

/// Returns true if the status represents an active goal.
bool is_active(ThreadGoalStatus status) noexcept;

/// Returns true if the status represents a terminal state.
bool is_terminal(ThreadGoalStatus status) noexcept;

/// Parses a status string, returning nullopt if invalid.
std::optional<ThreadGoalStatus> parse_thread_goal_status(std::string_view value);

/// A goal associated with a thread/session, tracking objective and resource usage.
struct ThreadGoal {
  std::string session_id;           ///< Session this goal belongs to
  std::string goal_id;              ///< Unique identifier for this goal
  std::string objective;            ///< The goal's objective text
  ThreadGoalStatus status = ThreadGoalStatus::kActive;
  std::optional<std::int64_t> token_budget;  ///< Optional token budget limit
  std::int64_t tokens_used = 0;     ///< Tokens consumed so far
  std::int64_t time_used_seconds = 0;  ///< Wall-clock time used
  std::chrono::system_clock::time_point created_at;
  std::chrono::system_clock::time_point updated_at;
};

/// Formats elapsed seconds into a human-readable duration string.
/// Examples: "45s", "5m", "1h 30m", "2d 23h 42m"
std::string format_goal_elapsed_seconds(std::int64_t seconds);

/// Returns a human-readable status label for display.
const char* goal_status_label(ThreadGoalStatus status) noexcept;

/// Generates a compact usage summary string for a goal.
std::string goal_usage_summary(const ThreadGoal& goal);

/// Usage string for the /goal command help.
constexpr const char* kGoalUsage =
    "Usage: /goal [<objective>|clear|edit|pause|resume|status]";

/// Generates a continuation prompt for an active goal.
/// This prompt is sent as a hidden user message to drive autonomous goal progress.
std::string goal_continuation_prompt(const ThreadGoal& goal);

/// Generates a budget limit prompt for a goal that has exhausted its token budget.
std::string goal_budget_limit_prompt(const ThreadGoal& goal);

/// Generates a prompt notifying the model that the goal objective was updated.
std::string goal_objective_updated_prompt(const ThreadGoal& goal);

}  // namespace qt::harness
