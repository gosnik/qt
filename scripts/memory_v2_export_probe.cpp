#include "qt/memory/database.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <stdexcept>

using namespace qt::memory;

static MemoryScope scope_for(const char* user) {
  MemoryScope scope;
  scope.kind = ScopeKind::Exact;
  scope.user = user;
  scope.project = "export-probe";
  scope.repository = "repo";
  return scope;
}

int main(int argc, char** argv) {
  if (argc != 3) throw std::invalid_argument("requires source and scratch directories");
  const auto scope = scope_for("export-user");
  SQLiteMemoryDatabase source{std::filesystem::path(argv[1])};
  MemoryWrite write;
  write.status = MemoryStatus::Active;
  write.content = "canonical export memory";
  const auto memory_id = source.add(scope, write);
  auto subject = source.add_entity(scope, {"", "service", "ExportService", {"export-alias"}, scope});
  auto object = source.add_entity(scope, {"", "component", "ExportComponent", {}, scope});
  MemoryAssertion assertion;
  assertion.memory_id = memory_id;
  assertion.revision = 1;
  assertion.subject_id = subject.id;
  assertion.predicate = "depends_on";
  assertion.object_id = object.id;
  assertion.qualifiers_json = "{}";
  assertion.evidence.push_back({"source-1", "r1", "uri:export", "hash-1", "bytes:0:4", "probe"});
  source.add_assertion(scope, assertion);
  const auto original_recorded = source.assertion(scope, memory_id)->recorded_at;
  MemoryPatch revision_patch;
  revision_patch.content = "canonical export memory revised";
  source.update(scope, memory_id, revision_patch);
  assertion.revision = 2;
  assertion.evidence.front().source_revision = "r2";
  source.add_assertion(scope, assertion);
  assert(source.assertion(scope, memory_id).has_value());
  CrossStoreLink link;
  link.from = {RetrievalOwnerKind::Memory, memory_id};
  link.to = {RetrievalOwnerKind::Entity, subject.id};
  link.relation = "describes";
  link.evidence_id = "uri:export";
  source.add_cross_store_link(scope, link);

  const auto exported = source.export_state(scope);
  assert(exported.at("entities").size() == 2);
  assert(exported.at("assertions").size() == 2);
  assert(exported.at("cross_store_links").size() == 1);

  auto clone_scope = scope;
  clone_scope.user = "clone-user";
  source.import_state(clone_scope, exported, false);
  const auto clone_subject = source.find_entities(clone_scope, "export-alias", RecallPolicy::Strict, false, 4);
  const auto clone_object = source.find_entities(clone_scope, "ExportComponent", RecallPolicy::Strict, false, 4);
  assert(clone_subject.size() == 1 && clone_object.size() == 1);
  const auto assertions = source.query_assertions(clone_scope, clone_subject.front().id, "depends_on", clone_object.front().id,
                                                   std::nullopt, RecallPolicy::Strict, false, 4);
  MemoryQuery cloned_memory_query;
  cloned_memory_query.text = "canonical export memory revised";
  cloned_memory_query.policy = RecallPolicy::Strict;
  const auto cloned_memories = source.query(clone_scope, cloned_memory_query);
  assert(cloned_memories.size() == 1);
  const auto direct_assertion = source.assertion(clone_scope, cloned_memories.front().id);
  if (!direct_assertion || direct_assertion->evidence.size() != 1)
    throw std::runtime_error("assertion direct roundtrip failed");
  const auto old_cutoff = original_recorded;
  const auto old_assertion = source.assertion(clone_scope, cloned_memories.front().id, old_cutoff);
  assert(old_assertion.has_value() && old_assertion->revision == 1);
  assert(old_assertion->historical_content == "canonical export memory");
  assert(old_assertion->evidence.front().source_revision == "r1");
  assert(direct_assertion->revision == 2);
  assert(direct_assertion->historical_content == "canonical export memory revised");
  assert(source.assertion(scope, memory_id)->revision == 2);
  assert(clone_subject.front().id != subject.id && clone_object.front().id != object.id);
  if (assertions.size() != 1)
    throw std::runtime_error("assertion query roundtrip failed: " + std::to_string(assertions.size()));
  assert(source.links(clone_scope, {RetrievalOwnerKind::Memory, cloned_memories.front().id},
                      RecallPolicy::Strict, false, 4).size() == 1);
  assert(cloned_memories.front().id != memory_id);

  auto wrong_scope = exported;
  wrong_scope["scope"]["organization"] = "forged-org";
  bool rejected = false;
  try { source.import_state(clone_scope, wrong_scope, false); } catch (const std::invalid_argument&) { rejected = true; }
  assert(rejected);
  ExportLimits tiny;
  tiny.memories = 1;
  const auto truncated = source.export_state(scope, tiny);
  assert(truncated.at("truncated").at("entities").get<bool>() == true);

  MemoryWrite replacement_write;
  replacement_write.status = MemoryStatus::Active;
  replacement_write.content = "replacement memory";
  const auto replacement_id = source.add(scope, replacement_write);
  MemoryPatch remove_patch;
  remove_patch.content = "canonical export memory removed";
  source.update(scope, memory_id, remove_patch);
  auto no_assertion_scope = scope;
  no_assertion_scope.user = "removed-active-clone";
  source.import_state(no_assertion_scope, source.export_state(scope), false);
  MemoryQuery no_assertion_query;
  no_assertion_query.text = "canonical export memory removed";
  no_assertion_query.policy = RecallPolicy::Strict;
  const auto removed_active = source.query(no_assertion_scope, no_assertion_query);
  const auto removed_active_it = std::find_if(removed_active.begin(), removed_active.end(),
      [](const auto& item) { return item.content == "canonical export memory removed"; });
  assert(removed_active_it != removed_active.end());
  assert(!source.assertion(no_assertion_scope, removed_active_it->id,
                           std::chrono::system_clock::now() + std::chrono::seconds(1)));
  assert(source.assertion(no_assertion_scope, removed_active_it->id, old_cutoff));
  MemoryPatch supersede_patch;
  supersede_patch.status = MemoryStatus::Superseded;
  supersede_patch.replacement_id = replacement_id;
  source.update(scope, memory_id, supersede_patch);
  const auto removed_export = source.export_state(scope);
  auto removed_clone = scope;
  removed_clone.user = "removed-clone";
  source.import_state(removed_clone, removed_export, false);
  MemoryQuery removed_query;
  removed_query.policy = RecallPolicy::Strict;
  removed_query.include_superseded = true;
  removed_query.limit = 50;
  const auto removed_memories = source.query(removed_clone, removed_query);
  const auto removed_it = std::find_if(removed_memories.begin(), removed_memories.end(),
      [](const auto& item) { return item.content == "canonical export memory removed"; });
  assert(removed_it != removed_memories.end());
  assert(!source.assertion(removed_clone, removed_it->id));
  assert(source.retrieval_index_status(removed_clone).pending > 0U);
  MemoryQuery replacement_query;
  replacement_query.text = "replacement memory";
  replacement_query.policy = RecallPolicy::Strict;
  const auto replacement = source.query(removed_clone, replacement_query);
  assert(replacement.size() == 1 && removed_it->replacement_id != replacement_id &&
         removed_it->replacement_id == replacement.front().id);
}
