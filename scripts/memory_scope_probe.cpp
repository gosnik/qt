#include "qt/memory/database.hpp"
#include "qt/memory/memory_service.hpp"
#include "qt/memory/recall_service.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace qt::memory;

void require(const bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

MemoryWrite active(std::string content) {
  MemoryWrite write;
  write.content = std::move(content);
  write.status = MemoryStatus::Active;
  write.provenance.created_by = "user";
  return write;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2) {
      throw std::invalid_argument(
          "usage: memory_scope_probe <temporary-agent-home>");
    }
    SQLiteMemoryDatabase database(argv[1]);
    MemoryService memories(database);

    MemoryScope exact;
    exact.user = "user";
    exact.project = "project";
    exact.repository = "repository";
    exact.branch = "main";
    exact.session = "session-a";

    auto branch = exact;
    branch.kind = ScopeKind::Branch;
    branch.task.clear();
    branch.agent.clear();
    branch.session.clear();
    branch.conversation.clear();
    auto repository = branch;
    repository.kind = ScopeKind::Repository;
    repository.branch.clear();

    const auto branch_id =
        memories.remember(branch, active("branch build rule"));
    const auto repository_id =
        memories.remember(repository, active("repository build rule"));

    MemoryQuery query;
    query.policy = RecallPolicy::Balanced;
    query.statuses = {MemoryStatus::Active};
    const auto main_values = memories.query(exact, query);
    require(main_values.size() == 2U &&
                main_values[0].id == branch_id &&
                main_values[1].id == repository_id,
            "balanced fallback did not preserve branch then repository order");

    query.policy = RecallPolicy::Strict;
    require(memories.query(exact, query).empty(),
            "strict recall crossed the exact scope boundary");

    auto other = exact;
    other.branch = "feature";
    query.policy = RecallPolicy::Balanced;
    const auto other_values = memories.query(other, query);
    require(other_values.size() == 1U &&
                other_values.front().id == repository_id,
            "branch-scoped memory leaked to another branch");

    auto stale = active("stale compiler procedure");
    stale.provenance.source_id = "source-1";
    stale.provenance.source_hash = "old-hash";
    stale.provenance.source_uri = "doc://build";
    const auto stale_id = memories.remember(branch, std::move(stale));
    SourceRecord source;
    source.id = "source-1";
    source.uri = "doc://build";
    source.hash = "new-hash";
    source.hash_status = "current";
    SourceEvent event;
    event.action = "check";
    event.status = "ok";
    event.source_hash = "new-hash";
    database.record_event(branch, std::move(source), std::move(event));

    RecallService recall(database, database, database);
    RecallRequest request;
    request.query = "stale compiler procedure";
    request.include_candidates = false;
    request.result_limit = 10U;
    const auto recalled = recall.recall(exact, request);
    bool found_stale = false;
    for (const auto& item : recalled) {
      if (item.memory.id != stale_id) continue;
      found_stale = true;
      require(!item.eligible_for_context && !item.warnings.empty(),
              "source drift was not governed out of context");
    }
    require(found_stale, "source-drifted memory was not recall-visible");
    std::cout << "memory scope probe: ok\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "memory scope probe: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
