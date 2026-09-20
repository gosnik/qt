#include "qt/memory/offline_import.hpp"

#include <sqlite3.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace qt::memory {
namespace {

using Json = nlohmann::json;

struct DatabaseCloser {
  void operator()(sqlite3* database) const noexcept {
    if (database != nullptr) sqlite3_close_v2(database);
  }
};

class Statement final {
 public:
  Statement(sqlite3* database, const std::string& sql) {
    if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement_, nullptr) !=
        SQLITE_OK) {
      throw std::runtime_error("qtmem import query failed: " +
                               std::string(sqlite3_errmsg(database)));
    }
  }
  ~Statement() { sqlite3_finalize(statement_); }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  sqlite3_stmt* get() const noexcept { return statement_; }
  void bind(int index, const std::string& value) {
    if (sqlite3_bind_text(statement_, index, value.c_str(),
                          static_cast<int>(value.size()), SQLITE_TRANSIENT) !=
        SQLITE_OK) {
      throw std::runtime_error("qtmem import bind failed");
    }
  }
  int step() { return sqlite3_step(statement_); }

 private:
  sqlite3_stmt* statement_ = nullptr;
};

std::string text(sqlite3_stmt* row, int column) {
  const auto* value = sqlite3_column_text(row, column);
  return value == nullptr
             ? std::string{}
             : std::string(reinterpret_cast<const char*>(value));
}

std::int64_t integer(sqlite3_stmt* row, int column) {
  return sqlite3_column_int64(row, column);
}

double real(sqlite3_stmt* row, int column) {
  return sqlite3_column_double(row, column);
}

void execute(sqlite3* database, const char* sql) {
  char* error = nullptr;
  if (sqlite3_exec(database, sql, nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error == nullptr ? sqlite3_errmsg(database) : error;
    sqlite3_free(error);
    throw std::runtime_error("qtmem import transaction failed: " + message);
  }
}

bool table_exists(sqlite3* database, const std::string& name) {
  Statement statement(
      database,
      "SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1;");
  statement.bind(1, name);
  return statement.step() == SQLITE_ROW;
}

void require_table(sqlite3* database, const char* name) {
  if (!table_exists(database, name)) {
    throw std::invalid_argument("qtmem v3 database is missing table: " +
                                std::string(name));
  }
}

std::vector<std::string> memory_tags(sqlite3* database,
                                     const std::string& id) {
  Statement statement(database,
                      "SELECT tag FROM memory_tags WHERE memory_id=CAST(? AS "
                      "INTEGER) ORDER BY tag;");
  statement.bind(1, id);
  std::vector<std::string> result;
  while (statement.step() == SQLITE_ROW) result.push_back(text(statement.get(), 0));
  return result;
}

std::vector<std::string> review_tags(sqlite3* database,
                                     const std::string& id) {
  Statement statement(
      database,
      "SELECT tag FROM review_item_tags WHERE review_item_id=CAST(? AS INTEGER) "
      "ORDER BY tag;");
  statement.bind(1, id);
  std::vector<std::string> result;
  while (statement.step() == SQLITE_ROW) result.push_back(text(statement.get(), 0));
  return result;
}

std::string scope_description(sqlite3_stmt* row, int first_column) {
  return Json{{"user", text(row, first_column)},
              {"organization", text(row, first_column + 1)},
              {"project", text(row, first_column + 2)},
              {"repository", text(row, first_column + 3)},
              {"branch", text(row, first_column + 4)},
              {"task", text(row, first_column + 5)},
              {"session", text(row, first_column + 6)},
              {"agent", text(row, first_column + 7)},
              {"conversation", text(row, first_column + 8)}}
      .dump();
}

std::string safe_path_component(std::string value) {
  if (value.empty()) return "default";
  for (auto& character : value) {
    const bool alphanumeric =
        (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9');
    if (!alphanumeric && character != '-' && character != '_') character = '_';
  }
  return value;
}

std::optional<std::string> read_qtmem_block(
    const std::filesystem::path& path, std::vector<std::string>* warnings) {
  constexpr std::uintmax_t kMaximumBlockBytes = 256U * 1024U;
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error || status.type() == std::filesystem::file_type::not_found) {
    return std::nullopt;
  }
  if (std::filesystem::is_symlink(status) ||
      !std::filesystem::is_regular_file(status)) {
    if (warnings != nullptr) {
      warnings->push_back("skipped non-regular qtmem block: " +
                          path.filename().generic_string());
    }
    return std::nullopt;
  }
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > kMaximumBlockBytes) {
    if (warnings != nullptr) {
      warnings->push_back("skipped oversized qtmem block: " +
                          path.filename().generic_string());
    }
    return std::nullopt;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("unable to read qtmem block: " +
                             path.filename().generic_string());
  }
  std::string content(static_cast<std::size_t>(size), '\0');
  input.read(content.data(), static_cast<std::streamsize>(content.size()));
  if (input.gcount() != static_cast<std::streamsize>(content.size())) {
    throw std::runtime_error("short read while importing qtmem block: " +
                             path.filename().generic_string());
  }
  return content;
}

}  // namespace

bool QtmemV3ImportAdapter::is_sqlite_database(
    const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::array<char, 16> header{};
  input.read(header.data(), static_cast<std::streamsize>(header.size()));
  return input.gcount() == static_cast<std::streamsize>(header.size()) &&
         std::string(header.data(), header.size()) ==
             std::string("SQLite format 3\0", 16);
}

OfflineImportPackage QtmemV3ImportAdapter::read(
    const std::filesystem::path& path) {
  if (!is_sqlite_database(path)) {
    throw std::invalid_argument("qtmem import source is not a SQLite database");
  }
  sqlite3* raw_database = nullptr;
  const int open_result = sqlite3_open_v2(
      path.c_str(), &raw_database, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
      nullptr);
  std::unique_ptr<sqlite3, DatabaseCloser> database(raw_database);
  if (open_result != SQLITE_OK || database == nullptr) {
    throw std::runtime_error(
        "unable to open qtmem database read-only: " +
        std::string(raw_database == nullptr ? "no SQLite handle"
                                            : sqlite3_errmsg(raw_database)));
  }
  if (sqlite3_db_readonly(database.get(), "main") != 1) {
    throw std::runtime_error("qtmem import source was not opened read-only");
  }
  if (table_exists(database.get(), "memory_schema_migrations")) {
    throw std::invalid_argument(
        "import source is a native qt memory database, not standalone qtmem v3");
  }
  require_table(database.get(), "meta");
  for (const char* table : {
           "memories", "memory_tags", "graph_edges", "procedure_runs",
           "memory_revisions", "sources", "source_events", "review_items",
           "review_item_tags", "audit_events", "session_events"}) {
    require_table(database.get(), table);
  }
  std::size_t schema_version = 0;
  {
    Statement version(database.get(),
                      "SELECT value FROM meta WHERE key='schema_version' LIMIT 1;");
    if (version.step() != SQLITE_ROW) {
      throw std::invalid_argument("qtmem database has no schema_version");
    }
    const auto value = text(version.get(), 0);
    std::size_t consumed = 0;
    schema_version = static_cast<std::size_t>(std::stoull(value, &consumed));
    if (consumed != value.size() || schema_version != 3) {
      throw std::invalid_argument(
          "standalone qtmem import requires schema version 3");
    }
  }

  execute(database.get(), "BEGIN;");
  try {
    OfflineImportPackage package;
    package.format = "qtmem-sqlite-v3";
    package.schema_version = schema_version;
    package.state = Json{{"schema_version", 3},
                         {"memories", Json::array()},
                         {"graph_edges", Json::array()},
                         {"procedure_runs", Json::array()},
                         {"memory_revisions", Json::array()},
                         {"sources", Json::array()},
                         {"source_events", Json::array()},
                         {"review_items", Json::array()},
                         {"audit_events", Json::array()},
                         {"session_events", Json::array()},
                         {"blocks", Json::array()}};
    std::set<std::string> scopes;
    std::set<std::string> projects;

    {
      Statement rows(database.get(),
          "SELECT id,user,organization,project,repository,branch,task,session,"
          "agent_id,conversation_id,scope_json,memory_type,status,provenance_json,"
          "source,source_type,source_uri,source_hash,source_span,created_by,content,"
          "confidence,evidence_strength,quality_score,success_count,failure_count,"
          "last_outcome,access_count,superseded_by,replacement_id,created_at,"
          "updated_at,last_accessed_at FROM memories ORDER BY id;");
      while (rows.step() == SQLITE_ROW) {
        const auto id = text(rows.get(), 0);
        scopes.insert(scope_description(rows.get(), 1));
        if (const auto project = text(rows.get(), 3); !project.empty()) {
          projects.insert(project);
        }
        package.state["memories"].push_back(Json{
            {"id", id}, {"user", text(rows.get(), 1)},
            {"organization", text(rows.get(), 2)},
            {"project", text(rows.get(), 3)},
            {"repository", text(rows.get(), 4)}, {"branch", text(rows.get(), 5)},
            {"task", text(rows.get(), 6)}, {"session", text(rows.get(), 7)},
            {"agent", text(rows.get(), 8)},
            {"conversation", text(rows.get(), 9)},
            {"scope_json", text(rows.get(), 10)},
            {"type", text(rows.get(), 11)}, {"status", text(rows.get(), 12)},
            {"provenance_json", text(rows.get(), 13)},
            {"source", text(rows.get(), 14)},
            {"source_type", text(rows.get(), 15)},
            {"source_uri", text(rows.get(), 16)},
            {"source_hash", text(rows.get(), 17)},
            {"source_span", text(rows.get(), 18)},
            {"created_by", text(rows.get(), 19)},
            {"content", text(rows.get(), 20)},
            {"confidence", real(rows.get(), 21)},
            {"evidence_strength", real(rows.get(), 22)},
            {"quality_score", real(rows.get(), 23)},
            {"success_count", integer(rows.get(), 24)},
            {"failure_count", integer(rows.get(), 25)},
            {"last_outcome", text(rows.get(), 26)},
            {"access_count", integer(rows.get(), 27)},
            {"superseded_by", text(rows.get(), 28)},
            {"replacement_id", text(rows.get(), 29)},
            {"created_at", integer(rows.get(), 30)},
            {"updated_at", integer(rows.get(), 31)},
            {"last_accessed_at", integer(rows.get(), 32)},
            {"tags", memory_tags(database.get(), id)}});
      }
    }
    {
      Statement rows(database.get(),
          "SELECT id,from_id,to_id,type,confidence,source,created_at FROM "
          "graph_edges ORDER BY id;");
      while (rows.step() == SQLITE_ROW) {
        package.state["graph_edges"].push_back(Json{
            {"id", text(rows.get(), 0)}, {"from_id", text(rows.get(), 1)},
            {"to_id", text(rows.get(), 2)}, {"type", text(rows.get(), 3)},
            {"confidence", real(rows.get(), 4)}, {"source", text(rows.get(), 5)},
            {"created_at", integer(rows.get(), 6)}});
      }
    }
    {
      Statement rows(database.get(),
          "SELECT id,procedure_id,task_id,outcome,notes,evidence_uri,created_at "
          "FROM procedure_runs ORDER BY id;");
      while (rows.step() == SQLITE_ROW) {
        package.state["procedure_runs"].push_back(Json{
            {"id", text(rows.get(), 0)},
            {"procedure_id", text(rows.get(), 1)},
            {"task_id", text(rows.get(), 2)}, {"outcome", text(rows.get(), 3)},
            {"notes", text(rows.get(), 4)},
            {"evidence_uri", text(rows.get(), 5)},
            {"created_at", integer(rows.get(), 6)}});
      }
    }
    {
      Statement rows(database.get(),
          "SELECT id,memory_id,revision_kind,entry_json,created_at FROM "
          "memory_revisions ORDER BY id;");
      while (rows.step() == SQLITE_ROW) {
        package.state["memory_revisions"].push_back(Json{
            {"id", text(rows.get(), 0)}, {"memory_id", text(rows.get(), 1)},
            {"kind", text(rows.get(), 2)}, {"entry_json", text(rows.get(), 3)},
            {"created_at", integer(rows.get(), 4)}});
      }
    }
    {
      Statement rows(database.get(),
          "SELECT id,project,source_type,source_uri,source_hash,display_name,"
          "trust_level,hash_status,ingest_count,imported_at,last_ingested_at,"
          "last_checked_at FROM sources ORDER BY id;");
      while (rows.step() == SQLITE_ROW) {
        if (const auto project = text(rows.get(), 1); !project.empty()) {
          projects.insert(project);
        }
        package.state["sources"].push_back(Json{
            {"id", text(rows.get(), 0)}, {"project", text(rows.get(), 1)},
            {"type", text(rows.get(), 2)}, {"uri", text(rows.get(), 3)},
            {"hash", text(rows.get(), 4)},
            {"display_name", text(rows.get(), 5)},
            {"trust_level", text(rows.get(), 6)},
            {"hash_status", text(rows.get(), 7)},
            {"ingest_count", integer(rows.get(), 8)},
            {"imported_at", integer(rows.get(), 9)},
            {"last_ingested_at", integer(rows.get(), 10)},
            {"last_checked_at", integer(rows.get(), 11)}});
      }
    }
    {
      Statement rows(database.get(),
          "SELECT id,source_id,project,action,status,source_hash,note,created_at "
          "FROM source_events ORDER BY id;");
      while (rows.step() == SQLITE_ROW) {
        package.state["source_events"].push_back(Json{
            {"id", text(rows.get(), 0)}, {"source_id", text(rows.get(), 1)},
            {"project", text(rows.get(), 2)}, {"action", text(rows.get(), 3)},
            {"status", text(rows.get(), 4)},
            {"source_hash", text(rows.get(), 5)}, {"note", text(rows.get(), 6)},
            {"created_at", integer(rows.get(), 7)}});
      }
    }
    {
      Statement rows(database.get(),
          "SELECT id,user,organization,project,repository,branch,task,session,"
          "agent_id,conversation_id,scope_json,kind,status,priority,reason,"
          "subject_id,source,source_type,source_uri,source_hash,provenance_json,"
          "candidate_json,decision,decided_by,decision_note,created_at,updated_at,"
          "decided_at FROM review_items ORDER BY id;");
      while (rows.step() == SQLITE_ROW) {
        const auto id = text(rows.get(), 0);
        scopes.insert(scope_description(rows.get(), 1));
        if (const auto project = text(rows.get(), 3); !project.empty()) {
          projects.insert(project);
        }
        package.state["review_items"].push_back(Json{
            {"id", id}, {"kind", text(rows.get(), 11)},
            {"status", text(rows.get(), 12)},
            {"priority", text(rows.get(), 13)},
            {"reason", text(rows.get(), 14)},
            {"subject_id", text(rows.get(), 15)},
            {"source", text(rows.get(), 16)},
            {"source_type", text(rows.get(), 17)},
            {"source_uri", text(rows.get(), 18)},
            {"source_hash", text(rows.get(), 19)},
            {"provenance_json", text(rows.get(), 20)},
            {"candidate_json", text(rows.get(), 21)},
            {"decision", text(rows.get(), 22)},
            {"decided_by", text(rows.get(), 23)},
            {"decision_note", text(rows.get(), 24)},
            {"created_at", integer(rows.get(), 25)},
            {"updated_at", integer(rows.get(), 26)},
            {"decided_at", integer(rows.get(), 27)},
            {"tags", review_tags(database.get(), id)}});
      }
    }
    {
      Statement rows(database.get(),
          "SELECT id,actor,action,user,organization,project,repository,branch,"
          "task,session,agent_id,conversation_id,scope_json,reason,before_id,"
          "after_id,evidence_uri,metadata_json,created_at FROM audit_events "
          "ORDER BY id;");
      while (rows.step() == SQLITE_ROW) {
        scopes.insert(scope_description(rows.get(), 3));
        package.state["audit_events"].push_back(Json{
            {"id", text(rows.get(), 0)}, {"actor", text(rows.get(), 1)},
            {"action", text(rows.get(), 2)}, {"reason", text(rows.get(), 13)},
            {"before_id", text(rows.get(), 14)},
            {"after_id", text(rows.get(), 15)},
            {"evidence_uri", text(rows.get(), 16)},
            {"metadata_json", text(rows.get(), 17)},
            {"created_at", integer(rows.get(), 18)}});
      }
    }
    {
      Statement rows(database.get(),
          "SELECT id,user,organization,project,repository,branch,task,session,"
          "agent_id,conversation_id,scope_json,event_type,source,payload_json,"
          "created_at FROM session_events ORDER BY id;");
      while (rows.step() == SQLITE_ROW) {
        scopes.insert(scope_description(rows.get(), 1));
        package.state["session_events"].push_back(Json{
            {"id", text(rows.get(), 0)}, {"event_type", text(rows.get(), 11)},
            {"source", text(rows.get(), 12)},
            {"payload_json", text(rows.get(), 13)},
            {"created_at", integer(rows.get(), 14)}});
      }
    }
    package.source_scopes.assign(scopes.begin(), scopes.end());
    if (package.source_scopes.size() > 1) {
      package.warnings.push_back(
          "multiple qtmem scopes will be mapped into the selected qt scope");
    }
    std::set<std::string> block_directories;
    for (const auto& project : projects) {
      block_directories.insert(safe_path_component(project));
    }
    const auto blocks_root = path.parent_path() / "blocks";
    if (block_directories.empty()) {
      std::error_code directory_error;
      if (std::filesystem::is_directory(blocks_root, directory_error)) {
        for (std::filesystem::directory_iterator iterator(blocks_root,
                                                           directory_error),
             end;
             !directory_error && iterator != end; iterator.increment(directory_error)) {
          const auto status = iterator->symlink_status(directory_error);
          if (!directory_error && std::filesystem::is_directory(status) &&
              !std::filesystem::is_symlink(status)) {
            block_directories.insert(iterator->path().filename().generic_string());
          }
        }
      }
    }
    if (block_directories.size() == 1U) {
      static constexpr std::array<const char*, 6U> kBlockIds{{
          "user_profile", "project_profile", "repo_rules", "current_task",
          "agent_scratchpad", "active_constraints"}};
      const auto source_directory = blocks_root / *block_directories.begin();
      for (const auto* block_id : kBlockIds) {
        const auto content = read_qtmem_block(source_directory /
                                                  (std::string(block_id) + ".md"),
                                              &package.warnings);
        if (content) {
          package.state["blocks"].push_back(
              Json{{"id", block_id}, {"content", *content}});
        }
      }
    } else if (block_directories.size() > 1U) {
      package.warnings.push_back(
          "qtmem file blocks were not imported because multiple source projects "
          "map into one selected qt scope");
    }
    execute(database.get(), "COMMIT;");
    return package;
  } catch (...) {
    sqlite3_exec(database.get(), "ROLLBACK;", nullptr, nullptr, nullptr);
    throw;
  }
}

}  // namespace qt::memory
