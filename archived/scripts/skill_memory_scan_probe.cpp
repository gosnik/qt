#include "qt/memory/database.hpp"
#include "qt/memory/governance_service.hpp"
#include "qt/memory/skill_import.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void write(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << content;
  if (!output) {
    throw std::runtime_error("unable to write probe fixture");
  }
}

qt::memory::MemoryScope repository_scope() {
  qt::memory::MemoryScope scope;
  scope.kind = qt::memory::ScopeKind::Repository;
  scope.user = "probe-user";
  scope.project = "probe-project";
  scope.repository = "probe-repository";
  scope.repository_display = "probe";
  return scope;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 2, "probe requires a temporary directory");
    const std::filesystem::path temporary = argv[1];
    const auto packages = temporary / "packages";
    write(packages / "alpha" / "SKILL.md",
          "---\nname: alpha\ndescription: Alpha skill\n---\n\n# Alpha\n\nAlways inspect evidence.\n");
    write(packages / "alpha" / "references" / "guide.md",
          "Alpha related guide.\n");
    write(packages / "alpha" / "nested" / "SKILL.md",
          "---\nname: hidden\ndescription: Nested skill\n---\n\nNested.\n");
    write(packages / "beta" / "SKILL.md",
          "---\nname: beta\ndescription: Beta skill\n---\n\n# Beta\n\nUse the checklist.\n");
    write(packages / "beta" / "asset.bin", std::string("abc\0def", 7));
    write(packages / "alpha" / ".env", "QT_SKILL_PROBE_SECRET=do-not-import\n");
    write(packages / ".system" / "gamma" / "SKILL.md",
          "---\nname: gamma\ndescription: Hidden-directory skill\n---\n\n# Gamma\n");
    const auto outside = temporary / "outside";
    write(outside / "SKILL.md",
          "---\nname: outside\ndescription: Must not escape\n---\n\nOutside.\n");
    std::filesystem::create_directories(packages / "escape");
    std::error_code symlink_error;
    std::filesystem::create_symlink(outside / "SKILL.md",
                                    packages / "escape" / "SKILL.md",
                                    symlink_error);

    qt::memory::SQLiteMemoryDatabase database(temporary / "agent-home");
    qt::memory::MemoryService memories(database);
    qt::memory::GovernanceService governance(memories, database, database);
    const auto scope = repository_scope();

    qt::memory::SkillImportOptions direct;
    direct.root = packages;
    direct.recursive = false;
    const auto direct_report = qt::memory::import_skill_memories(
        memories, governance, scope, direct);
    require(direct_report.discovered == 0U,
            "non-recursive scan descended into subfolders");

    qt::memory::SkillImportOptions recursive = direct;
    recursive.recursive = true;
    const auto first = qt::memory::import_skill_memories(
        memories, governance, scope, recursive);
    require(first.discovered == 3U && first.created == 3U && first.failed == 0U,
            "recursive scan did not import the expected skill packages");

    qt::memory::SkillImportOptions escaped = recursive;
    escaped.root = outside;
    escaped.containment_root = packages;
    bool containment_rejected = false;
    try {
      (void)qt::memory::import_skill_memories(memories, governance, scope,
                                              escaped);
    } catch (const std::invalid_argument&) {
      containment_rejected = true;
    }
    require(containment_rejected, "scan containment root was not enforced");

    qt::memory::MemoryQuery query;
    query.type = qt::memory::MemoryType::Skill;
    query.policy = qt::memory::RecallPolicy::Broad;
    query.limit = 0U;
    const auto stored = memories.query(scope, query);
    require(stored.size() == 3U, "skill memories were not stored idempotently");
    bool related_found = false;
    bool binary_found = false;
    for (const auto& memory : stored) {
      require(memory.status == qt::memory::MemoryStatus::Active,
              "imported skill memory is not active");
      require(memory.provenance.source_uri.rfind("skill://local/", 0U) == 0U,
              "absolute skill source path was persisted");
      related_found = related_found ||
                      memory.content.find("Alpha related guide") != std::string::npos;
      binary_found = binary_found || memory.content.find("abc") != std::string::npos;
    }
    require(related_found, "related text content was not captured");
    require(!binary_found, "binary related content was imported");
    for (const auto& memory : stored) {
      require(memory.content.find("QT_SKILL_PROBE_SECRET") == std::string::npos,
              "hidden related content was imported");
      require(memory.content.find("Must not escape") == std::string::npos,
              "symlinked SKILL.md escaped the scan root");
    }

    const auto second = qt::memory::import_skill_memories(
        memories, governance, scope, recursive);
    if (!(second.unchanged == 3U && second.created == 0U &&
          second.updated == 0U)) {
      std::cerr << "second scan counts: created=" << second.created
                << " updated=" << second.updated
                << " unchanged=" << second.unchanged
                << " failed=" << second.failed << '\n';
      for (const auto& item : second.items) {
        std::cerr << item.skill_path << ": " << item.action << " "
                  << item.diagnostic << '\n';
      }
    }
    require(second.unchanged == 3U && second.created == 0U && second.updated == 0U,
            "unchanged rescan created duplicate memories");

    write(packages / "alpha" / "references" / "guide.md",
          "Updated alpha related guide.\n");
    const auto third = qt::memory::import_skill_memories(
        memories, governance, scope, recursive);
    require(third.updated == 1U && third.unchanged == 2U,
            "changed skill package was not superseded");
    const auto active = memories.query(scope, query);
    require(active.size() == 3U,
            "superseded skill remained in the active query result");

    write(packages / "delta" / "SKILL.md",
          "---\nname: delta\ndescription: Concurrent import\n---\n\n# Delta\n");
    qt::memory::SkillImportReport concurrent_a;
    qt::memory::SkillImportReport concurrent_b;
    std::thread first_scan([&]() {
      concurrent_a = qt::memory::import_skill_memories(
          memories, governance, scope, recursive);
    });
    std::thread second_scan([&]() {
      concurrent_b = qt::memory::import_skill_memories(
          memories, governance, scope, recursive);
    });
    first_scan.join();
    second_scan.join();
    require(concurrent_a.created + concurrent_b.created == 1U,
            "concurrent scans created duplicate active memories");
    require(memories.query(scope, query).size() == 4U,
            "concurrent scan did not leave one active memory per skill");

    std::cout << "skill_memory_scan_probe: ok\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "skill_memory_scan_probe: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
