#include <qt/harness/goal_store.hpp>

#include <random>
#include <sstream>

namespace qt::harness {

namespace {

std::string generate_uuid_v4() {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_int_distribution<std::uint64_t> dist;

  const std::uint64_t a = dist(rng);
  const std::uint64_t b = dist(rng);

  // Set version (4) and variant (10xx) bits per RFC 4122
  const std::uint64_t version = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
  const std::uint64_t variant = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

  std::ostringstream out;
  out << std::hex;

  // First 8 hex digits
  for (int i = 60; i >= 32; i -= 4) {
    out << "0123456789abcdef"[(version >> i) & 0xF];
  }
  out << '-';

  // Next 4 hex digits
  for (int i = 28; i >= 16; i -= 4) {
    out << "0123456789abcdef"[(version >> i) & 0xF];
  }
  out << '-';

  // Next 4 hex digits (includes version)
  for (int i = 12; i >= 0; i -= 4) {
    out << "0123456789abcdef"[(version >> i) & 0xF];
  }
  out << '-';

  // Next 4 hex digits (includes variant)
  for (int i = 60; i >= 48; i -= 4) {
    out << "0123456789abcdef"[(variant >> i) & 0xF];
  }
  out << '-';

  // Last 12 hex digits
  for (int i = 44; i >= 0; i -= 4) {
    out << "0123456789abcdef"[(variant >> i) & 0xF];
  }

  return out.str();
}

}  // namespace

std::string GoalStore::generate_goal_id() const {
  return generate_uuid_v4();
}

ThreadGoalStatus GoalStore::apply_budget_limit(
    ThreadGoalStatus status,
    std::int64_t tokens_used,
    std::optional<std::int64_t> token_budget) const {
  if (status == ThreadGoalStatus::kActive &&
      token_budget.has_value() &&
      tokens_used >= *token_budget) {
    return ThreadGoalStatus::kBudgetLimited;
  }
  return status;
}

std::optional<ThreadGoal> GoalStore::get_session_goal(const std::string& session_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = goals_.find(session_id);
  if (it == goals_.end()) {
    return std::nullopt;
  }
  return it->second;
}

ThreadGoal GoalStore::replace_session_goal(
    const std::string& session_id,
    std::string objective,
    ThreadGoalStatus status,
    std::optional<std::int64_t> token_budget) {
  std::lock_guard<std::mutex> lock(mutex_);

  const auto now = std::chrono::system_clock::now();
  const auto effective_status = apply_budget_limit(status, 0, token_budget);

  ThreadGoal goal;
  goal.session_id = session_id;
  goal.goal_id = generate_goal_id();
  goal.objective = std::move(objective);
  goal.status = effective_status;
  goal.token_budget = token_budget;
  goal.tokens_used = 0;
  goal.time_used_seconds = 0;
  goal.created_at = now;
  goal.updated_at = now;

  goals_[session_id] = goal;
  return goal;
}

std::optional<ThreadGoal> GoalStore::insert_session_goal(
    const std::string& session_id,
    std::string objective,
    ThreadGoalStatus status,
    std::optional<std::int64_t> token_budget) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = goals_.find(session_id);
  if (it != goals_.end() && is_active(it->second.status)) {
    return std::nullopt;  // Active goal already exists
  }

  const auto now = std::chrono::system_clock::now();
  const auto effective_status = apply_budget_limit(status, 0, token_budget);

  ThreadGoal goal;
  goal.session_id = session_id;
  goal.goal_id = generate_goal_id();
  goal.objective = std::move(objective);
  goal.status = effective_status;
  goal.token_budget = token_budget;
  goal.tokens_used = 0;
  goal.time_used_seconds = 0;
  goal.created_at = now;
  goal.updated_at = now;

  goals_[session_id] = goal;
  return goal;
}

std::optional<ThreadGoal> GoalStore::update_session_goal(
    const std::string& session_id,
    const GoalUpdate& update) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = goals_.find(session_id);
  if (it == goals_.end()) {
    return std::nullopt;
  }

  auto& goal = it->second;

  // Check optimistic locking
  if (update.expected_goal_id.has_value() &&
      *update.expected_goal_id != goal.goal_id) {
    return std::nullopt;
  }

  // Apply objective update
  if (update.objective.has_value()) {
    goal.objective = *update.objective;
  }

  // Apply token_budget update
  if (update.token_budget.has_value()) {
    goal.token_budget = *update.token_budget;
  }

  // Apply status update with budget limit protection
  if (update.status.has_value()) {
    auto new_status = *update.status;

    // Don't allow transitioning away from BudgetLimited to Active/Paused/Blocked
    // if still over budget
    if (goal.status == ThreadGoalStatus::kBudgetLimited &&
        (new_status == ThreadGoalStatus::kPaused ||
         new_status == ThreadGoalStatus::kBlocked)) {
      // Keep BudgetLimited status
    } else if (new_status == ThreadGoalStatus::kActive &&
               goal.token_budget.has_value() &&
               goal.tokens_used >= *goal.token_budget) {
      // Can't reactivate if over budget
      goal.status = ThreadGoalStatus::kBudgetLimited;
    } else {
      goal.status = new_status;
    }
  }

  goal.updated_at = std::chrono::system_clock::now();
  return goal;
}

std::optional<ThreadGoal> GoalStore::pause_active_session_goal(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = goals_.find(session_id);
  if (it == goals_.end()) {
    return std::nullopt;
  }

  auto& goal = it->second;
  if (goal.status != ThreadGoalStatus::kActive) {
    // Don't pause terminal states
    if (is_terminal(goal.status)) {
      return std::nullopt;
    }
    return std::nullopt;
  }

  goal.status = ThreadGoalStatus::kPaused;
  goal.updated_at = std::chrono::system_clock::now();
  return goal;
}

std::optional<ThreadGoal> GoalStore::usage_limit_active_session_goal(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = goals_.find(session_id);
  if (it == goals_.end()) {
    return std::nullopt;
  }

  auto& goal = it->second;
  // Can transition from Active or BudgetLimited to UsageLimited
  if (goal.status != ThreadGoalStatus::kActive &&
      goal.status != ThreadGoalStatus::kBudgetLimited) {
    return std::nullopt;
  }

  goal.status = ThreadGoalStatus::kUsageLimited;
  goal.updated_at = std::chrono::system_clock::now();
  return goal;
}

std::optional<ThreadGoal> GoalStore::complete_active_session_goal(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = goals_.find(session_id);
  if (it == goals_.end()) {
    return std::nullopt;
  }

  auto& goal = it->second;
  if (goal.status != ThreadGoalStatus::kActive) {
    return std::nullopt;
  }

  goal.status = ThreadGoalStatus::kComplete;
  goal.updated_at = std::chrono::system_clock::now();
  return goal;
}

std::optional<ThreadGoal> GoalStore::delete_session_goal(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = goals_.find(session_id);
  if (it == goals_.end()) {
    return std::nullopt;
  }

  ThreadGoal goal = std::move(it->second);
  goals_.erase(it);
  return goal;
}

GoalAccountingOutcome GoalStore::account_session_goal_usage(
    const std::string& session_id,
    std::int64_t time_delta_seconds,
    std::int64_t token_delta,
    std::optional<std::string> expected_goal_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (time_delta_seconds < 0) time_delta_seconds = 0;
  if (token_delta < 0) token_delta = 0;
  if (time_delta_seconds == 0 && token_delta == 0) {
    auto it = goals_.find(session_id);
    if (it != goals_.end()) {
      return GoalAccountingOutcome{GoalAccountingOutcomeKind::kUnchanged, it->second};
    }
    return GoalAccountingOutcome{GoalAccountingOutcomeKind::kUnchanged, std::nullopt};
  }

  auto it = goals_.find(session_id);
  if (it == goals_.end()) {
    return GoalAccountingOutcome{GoalAccountingOutcomeKind::kUnchanged, std::nullopt};
  }

  auto& goal = it->second;

  // Check optimistic locking
  if (expected_goal_id.has_value() && *expected_goal_id != goal.goal_id) {
    return GoalAccountingOutcome{GoalAccountingOutcomeKind::kUnchanged, goal};
  }

  // Only account for active or budget_limited goals (in-flight usage)
  if (goal.status != ThreadGoalStatus::kActive &&
      goal.status != ThreadGoalStatus::kBudgetLimited) {
    return GoalAccountingOutcome{GoalAccountingOutcomeKind::kUnchanged, goal};
  }

  goal.tokens_used += token_delta;
  goal.time_used_seconds += time_delta_seconds;
  goal.updated_at = std::chrono::system_clock::now();

  // Check if we've crossed the budget threshold
  if (goal.status == ThreadGoalStatus::kActive &&
      goal.token_budget.has_value() &&
      goal.tokens_used >= *goal.token_budget) {
    goal.status = ThreadGoalStatus::kBudgetLimited;
  }

  return GoalAccountingOutcome{GoalAccountingOutcomeKind::kUpdated, goal};
}

void GoalStore::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  goals_.clear();
}

}  // namespace qt::harness
