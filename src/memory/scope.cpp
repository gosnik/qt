#include "qt/memory/scope.hpp"

#include <stdexcept>

namespace qt::memory {

void validate_scope(const MemoryScope& scope) {
  if (scope.user.empty()) {
    throw std::invalid_argument("memory scope user is required");
  }
  switch (scope.kind) {
    case ScopeKind::Exact:
      if (scope.project.empty() || scope.repository.empty()) {
        throw std::invalid_argument("exact memory scope requires project and repository");
      }
      break;
    case ScopeKind::Branch:
      if (scope.project.empty() || scope.repository.empty() ||
          scope.branch.empty()) {
        throw std::invalid_argument(
            "branch memory scope requires project, repository, and branch");
      }
      if (!scope.task.empty() || !scope.agent.empty() ||
          !scope.session.empty() || !scope.conversation.empty()) {
        throw std::invalid_argument(
            "branch memory scope cannot contain turn dimensions");
      }
      break;
    case ScopeKind::Repository:
      if (scope.project.empty() || scope.repository.empty()) {
        throw std::invalid_argument("repository memory scope requires project and repository");
      }
      if (!scope.branch.empty() || !scope.task.empty() || !scope.agent.empty() ||
          !scope.session.empty() || !scope.conversation.empty()) {
        throw std::invalid_argument("repository memory scope cannot contain turn dimensions");
      }
      break;
    case ScopeKind::Project:
      if (scope.project.empty() || !scope.repository.empty() ||
          !scope.branch.empty() || !scope.task.empty() ||
          !scope.agent.empty() || !scope.session.empty() ||
          !scope.conversation.empty()) {
        throw std::invalid_argument(
            "project memory scope requires project and forbids narrower dimensions");
      }
      break;
    case ScopeKind::GlobalUser:
      if (!scope.organization.empty() || !scope.project.empty() ||
          !scope.repository.empty() || !scope.branch.empty() ||
          !scope.task.empty() || !scope.agent.empty() ||
          !scope.session.empty() || !scope.conversation.empty()) {
        throw std::invalid_argument("global-user memory scope may only contain user identity");
      }
      break;
  }
}

std::string scope_key(const MemoryScope& scope) {
  validate_scope(scope);
  auto append = [](std::string& out, const std::string& value) {
    out += std::to_string(value.size());
    out.push_back(':');
    out += value;
    out.push_back('|');
  };
  std::string key(to_string(scope.kind));
  key.push_back('|');
  append(key, scope.user);
  append(key, scope.organization);
  append(key, scope.project);
  append(key, scope.repository);
  append(key, scope.branch);
  append(key, scope.task);
  append(key, scope.agent);
  append(key, scope.session);
  append(key, scope.conversation);
  return key;
}

MemoryScope durable_memory_scope(MemoryScope scope) {
  scope.kind = scope.branch.empty() ? ScopeKind::Repository : ScopeKind::Branch;
  scope.task.clear();
  scope.agent.clear();
  scope.session.clear();
  scope.conversation.clear();
  return scope;
}

std::vector<MemoryScope> recall_scopes(const MemoryScope& scope, RecallPolicy policy,
                                     bool include_global_user) {
  validate_scope(scope);
  std::vector<MemoryScope> result{scope};
  if (policy == RecallPolicy::Strict) return result;
  auto ancestor = scope;
  ancestor.task.clear(); ancestor.agent.clear(); ancestor.session.clear();
  ancestor.conversation.clear();
  const auto append = [&](const MemoryScope& item) {
    const auto key = scope_key(item);
    for (const auto& existing : result)
      if (scope_key(existing) == key) return;
    result.push_back(item);
  };
  if (!ancestor.repository.empty() && !ancestor.branch.empty()) {
    ancestor.kind = ScopeKind::Branch;
    append(ancestor);
  }
  ancestor.branch.clear();
  if (!ancestor.repository.empty()) {
    ancestor.kind = ScopeKind::Repository;
    append(ancestor);
  }
  ancestor.repository.clear(); ancestor.repository_display.clear();
  if (!ancestor.project.empty()) {
    ancestor.kind = ScopeKind::Project;
    append(ancestor);
  }
  if (include_global_user) {
    MemoryScope global;
    global.kind = ScopeKind::GlobalUser; global.user = scope.user;
    append(global);
  }
  return result;
}

bool scope_can_access(const MemoryScope& caller, const MemoryScope& stored,
                      RecallPolicy policy, bool include_global_user) noexcept {
  if (caller.user != stored.user) return false;
  if (stored.kind != ScopeKind::GlobalUser &&
      caller.organization != stored.organization) return false;
  if (caller.kind == stored.kind && caller.organization == stored.organization &&
      caller.project == stored.project && caller.repository == stored.repository &&
      caller.branch == stored.branch && caller.task == stored.task &&
      caller.session == stored.session && caller.agent == stored.agent &&
      caller.conversation == stored.conversation) return true;
  if (policy == RecallPolicy::Strict) return false;
  switch (stored.kind) {
    case ScopeKind::Exact:
      return caller.project == stored.project &&
             caller.repository == stored.repository && caller.branch == stored.branch &&
             caller.task == stored.task && caller.session == stored.session &&
             caller.agent == stored.agent && caller.conversation == stored.conversation;
    case ScopeKind::Branch:
      return policy != RecallPolicy::Strict &&
             caller.project == stored.project &&
             caller.repository == stored.repository &&
             !caller.branch.empty() && caller.branch == stored.branch;
    case ScopeKind::Repository:
      return policy != RecallPolicy::Strict && caller.project == stored.project &&
             caller.repository == stored.repository;
    case ScopeKind::Project:
      return policy != RecallPolicy::Strict && caller.project == stored.project;
    case ScopeKind::GlobalUser:
      return policy != RecallPolicy::Strict && include_global_user;
  }
  return false;
}

double scope_score(const MemoryScope& caller, const MemoryScope& stored) noexcept {
  if (!scope_can_access(caller, stored, RecallPolicy::Broad, true)) return 0.0;
  switch (stored.kind) {
    case ScopeKind::Exact: return 1.0;
    case ScopeKind::Branch: return 0.94;
    case ScopeKind::Repository: return 0.86;
    case ScopeKind::Project: return 0.68;
    case ScopeKind::GlobalUser: return 0.50;
  }
  return 0.0;
}

}  // namespace qt::memory
