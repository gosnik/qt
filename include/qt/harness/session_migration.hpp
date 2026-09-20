#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <qt/foundation/error.hpp>
#include <qt/harness/session.hpp>
#include <qt/harness/session_lifecycle.hpp>

namespace qt::harness {

constexpr int kPiMonoCurrentSessionVersion = 3;

struct SessionMigrationStep {
  std::string name;
  int from_version = kPiMonoCurrentSessionVersion;
  int to_version = kPiMonoCurrentSessionVersion;
  std::size_t affected_entry_count = 0;
};

struct SessionMigrationPlan {
  std::string source_format = "native.qt";
  int source_version = kPiMonoCurrentSessionVersion;
  int target_version = kPiMonoCurrentSessionVersion;
  bool migration_required = false;
  std::vector<SessionMigrationStep> steps;
  std::vector<std::string> diagnostics;
  std::vector<SessionEntry> migrated_entries;
};

struct ImportSessionRepositoryRequest {
  std::string source_repository_path;
  std::string destination_repository_path;
  std::optional<std::string> working_directory_override;
  bool apply_migrations = true;
};

struct ImportSessionRepositoryResult {
  PersistentSessionRecord record;
  SessionMigrationPlan migration_plan;
  std::size_t imported_entry_count = 0;
};

struct ExportSessionRepositoryRequest {
  std::string source_repository_path;
  std::string destination_repository_path;
};

struct ExportSessionRepositoryResult {
  std::size_t exported_entry_count = 0;
};

qt::foundation::Result<SessionMigrationPlan> plan_session_migration(
    const std::vector<SessionEntry>& entries,
    const std::optional<std::string>& working_directory_override = std::nullopt);

qt::foundation::Result<ImportSessionRepositoryResult> import_session_repository(
    const ImportSessionRepositoryRequest& request);

qt::foundation::Result<ExportSessionRepositoryResult> export_session_repository(
    const ExportSessionRepositoryRequest& request);

}  // namespace qt::harness
