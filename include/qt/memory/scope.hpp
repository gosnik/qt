#pragma once

#include "qt/memory/models.hpp"

#include <string>

namespace qt::memory {

void validate_scope(const MemoryScope& scope);
std::string scope_key(const MemoryScope& scope);
// Canonical durable form of a scope: repository when no branch is known,
// otherwise branch, with turn-scoped dimensions cleared. Every surface that
// reads the stored review queue must address it through this form so the
// reported counts match the listed items.
MemoryScope durable_memory_scope(MemoryScope scope);
std::vector<MemoryScope> recall_scopes(const MemoryScope& scope, RecallPolicy policy,
                                     bool include_global_user);
bool scope_can_access(const MemoryScope& caller, const MemoryScope& stored,
                      RecallPolicy policy, bool include_global_user) noexcept;
double scope_score(const MemoryScope& caller, const MemoryScope& stored) noexcept;

}  // namespace qt::memory
