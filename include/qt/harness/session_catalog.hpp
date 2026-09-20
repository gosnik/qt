#pragma once

#include <optional>
#include <string>
#include <vector>

#include <qt/foundation/error.hpp>
#include <qt/harness/session_graph.hpp>

namespace qt::harness {

struct SessionCatalogRecord {
  std::string repository_path;
  SessionBranchSnapshot snapshot;
};

struct SessionCatalog {
  std::string sessions_directory;
  std::vector<SessionCatalogRecord> session_records;
  SessionGraph graph;
  std::vector<std::string> root_session_ids;
  std::vector<std::string> diagnostics;

  bool has_sessions() const noexcept { return !session_records.empty(); }

  void add_diagnostic(std::string message) {
    if (!message.empty()) {
      diagnostics.push_back(std::move(message));
    }
  }
};

qt::foundation::Result<SessionCatalog> load_session_catalog(
    const std::string& sessions_directory);

std::optional<std::string> find_session_catalog_path_by_id_prefix(
    const SessionCatalog& catalog, const std::string& id_prefix,
    const std::string& preferred_working_directory = {});

qt::foundation::Result<std::string> resolve_session_catalog_path_by_id_prefix(
    const SessionCatalog& catalog, const std::string& id_prefix,
    const std::string& preferred_working_directory = {});

std::optional<std::string> find_most_recent_session_catalog_path(
    const SessionCatalog& catalog,
    const std::string& preferred_working_directory = {});

}  // namespace qt::harness
