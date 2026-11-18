#include "qt/memory/database.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>

using namespace qt::memory;

static MemoryScope scope(const std::string& repository) {
  MemoryScope value; value.kind = ScopeKind::Repository; value.user = "links-probe";
  value.project = "project"; value.repository = repository; return value;
}

int main(int argc, char** argv) {
  if (argc != 2) throw std::invalid_argument("requires probe directory");
  SQLiteMemoryDatabase db(argv[1]);
  const auto exact = scope("repo");
  MemoryWrite write; write.status = MemoryStatus::Active; write.content = "link endpoint";
  const auto memory_id = db.add(exact, write);
  const auto revision = db.retrieval_revision(exact, memory_id);
  CrossStoreLink link;
  link.from = {RetrievalOwnerKind::Memory, memory_id, revision};
  link.to = {RetrievalOwnerKind::Code, "code-node", 0, "repo-id", "/worktree", "src/a.cpp",
             "fnv1a64:abc", 4};
  link.relation = "documents"; link.evidence_id = "uri:file:///probe/source";
  const auto id = db.add_cross_store_link(exact, link);
  const auto found = db.links(exact, link.from, RecallPolicy::Strict, false, 10);
  assert(found.size() == 1 && found.front().id == id);
  auto other = scope("other");
  assert(db.links(other, link.from, RecallPolicy::Broad, false, 10).empty());
  MemoryPatch patch; patch.content = "updated endpoint"; db.update(exact, memory_id, patch);
  assert(db.links(exact, link.from, RecallPolicy::Strict, false, 10).empty());
  bool rejected = false;
  try { db.add_cross_store_link(exact, link); } catch (const std::invalid_argument&) { rejected = true; }
  assert(rejected);
  std::cout << "retrieval links storage probe passed\n";
}
