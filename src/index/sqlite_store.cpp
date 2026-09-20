#include "index/sqlite_store.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace qt::index {
namespace {

using json = nlohmann::json;

constexpr int kSchemaVersion = 1;
constexpr std::uintmax_t kMaxFtsFileBytes = 1024U * 1024U;

struct SqliteDeleter {
    void operator()(sqlite3* db) const noexcept {
        if (db != nullptr) {
            sqlite3_close(db);
        }
    }
};

using SqliteHandle = std::unique_ptr<sqlite3, SqliteDeleter>;

struct Statement {
    sqlite3_stmt* stmt = nullptr;

    Statement(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db));
        }
    }

    ~Statement() {
        if (stmt != nullptr) {
            sqlite3_finalize(stmt);
        }
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
};

SqliteHandle open_database(const std::filesystem::path& path) {
    sqlite3* raw = nullptr;
    if (sqlite3_open_v2(path.string().c_str(),
                        &raw,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                        nullptr) != SQLITE_OK) {
        std::string message = raw == nullptr ? "could not open sqlite database"
                                             : sqlite3_errmsg(raw);
        if (raw != nullptr) {
            sqlite3_close(raw);
        }
        throw std::runtime_error(message);
    }
    return SqliteHandle(raw);
}

SqliteHandle open_database_readonly(const std::filesystem::path& path) {
    sqlite3* raw = nullptr;
    if (sqlite3_open_v2(path.string().c_str(), &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX,
                        nullptr) != SQLITE_OK) {
        std::string message = raw == nullptr ? "could not open sqlite database"
                                             : sqlite3_errmsg(raw);
        if (raw != nullptr) sqlite3_close(raw);
        throw std::runtime_error(message);
    }
    return SqliteHandle(raw);
}

void exec_sql(sqlite3* db, const char* sql) {
    char* error = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        std::string message = error == nullptr ? sqlite3_errmsg(db) : error;
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

void bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
    sqlite3_bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

std::string column_text(sqlite3_stmt* stmt, int index) {
    const auto* text = sqlite3_column_text(stmt, index);
    return text == nullptr ? std::string{} : reinterpret_cast<const char*>(text);
}

std::chrono::system_clock::time_point time_from_epoch_text(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const auto seconds = std::chrono::seconds{std::stoll(value)};
    return std::chrono::system_clock::time_point{seconds};
}

std::string time_to_epoch_text(std::chrono::system_clock::time_point value) {
    if (value == std::chrono::system_clock::time_point{}) {
        return {};
    }
    return std::to_string(
        std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch()).count());
}

std::string file_kind_to_string(ProjectFileKind kind) {
    switch (kind) {
        case ProjectFileKind::CSource:
            return "c_source";
        case ProjectFileKind::CxxSource:
            return "cxx_source";
        case ProjectFileKind::RustSource:
            return "rust_source";
        case ProjectFileKind::TypeScriptSource:
            return "typescript_source";
        case ProjectFileKind::VueSource:
            return "vue_source";
        case ProjectFileKind::PythonSource:
            return "python_source";
        case ProjectFileKind::Header:
            return "header";
        case ProjectFileKind::BuildFile:
            return "build_file";
        case ProjectFileKind::Config:
            return "config";
        case ProjectFileKind::Other:
            return "other";
    }
    return "other";
}

ProjectFileKind file_kind_from_string(const std::string& value) {
    if (value == "c_source") {
        return ProjectFileKind::CSource;
    }
    if (value == "cxx_source") {
        return ProjectFileKind::CxxSource;
    }
    if (value == "rust_source") {
        return ProjectFileKind::RustSource;
    }
    if (value == "typescript_source") {
        return ProjectFileKind::TypeScriptSource;
    }
    if (value == "vue_source") {
        return ProjectFileKind::VueSource;
    }
    if (value == "python_source") {
        return ProjectFileKind::PythonSource;
    }
    if (value == "header") {
        return ProjectFileKind::Header;
    }
    if (value == "build_file") {
        return ProjectFileKind::BuildFile;
    }
    if (value == "config") {
        return ProjectFileKind::Config;
    }
    return ProjectFileKind::Other;
}

bool fts_file_kind(ProjectFileKind kind) {
    return kind == ProjectFileKind::CSource || kind == ProjectFileKind::CxxSource ||
           kind == ProjectFileKind::Header || kind == ProjectFileKind::RustSource ||
           kind == ProjectFileKind::TypeScriptSource || kind == ProjectFileKind::VueSource ||
           kind == ProjectFileKind::PythonSource || kind == ProjectFileKind::BuildFile ||
           kind == ProjectFileKind::Config;
}

std::string read_file_for_fts(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > kMaxFtsFileBytes) {
        return {};
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    return std::string{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

json tree_sitter_to_json(const TreeSitterResult& result) {
    return {{"attempted", result.attempted},
            {"available", result.available},
            {"generated", result.generated},
            {"files_considered", result.files_considered},
            {"files_parsed", result.files_parsed},
            {"files_updated", result.files_updated},
            {"files_unchanged", result.files_unchanged},
            {"files_added", result.files_added},
            {"files_changed", result.files_changed},
            {"files_deleted", result.files_deleted},
            {"files_failed", result.files_failed},
            {"files_skipped", result.files_skipped},
            {"output_directory", result.output_directory.string()},
            {"manifest_path", result.manifest_path.string()},
            {"failed_files", result.failed_files},
            {"skipped_files", result.skipped_files},
            {"mode", result.mode},
            {"message", result.message}};
}

TreeSitterResult tree_sitter_from_json(const std::string& text) {
    TreeSitterResult result;
    const auto parsed = json::parse(text, nullptr, false);
    if (!parsed.is_object()) {
        return result;
    }
    result.attempted = parsed.value("attempted", false);
    result.available = parsed.value("available", false);
    result.generated = parsed.value("generated", false);
    result.files_considered = parsed.value("files_considered", 0U);
    result.files_parsed = parsed.value("files_parsed", 0U);
    result.files_updated = parsed.value("files_updated", 0U);
    result.files_unchanged = parsed.value("files_unchanged", 0U);
    result.files_added = parsed.value("files_added", 0U);
    result.files_changed = parsed.value("files_changed", 0U);
    result.files_deleted = parsed.value("files_deleted", 0U);
    result.files_failed = parsed.value("files_failed", 0U);
    result.files_skipped = parsed.value("files_skipped", 0U);
    result.output_directory = parsed.value("output_directory", std::string{});
    result.manifest_path = parsed.value("manifest_path", std::string{});
    result.failed_files = parsed.value("failed_files", std::vector<std::string>{});
    result.skipped_files = parsed.value("skipped_files", std::vector<std::string>{});
    result.mode = parsed.value("mode", std::string{});
    result.message = parsed.value("message", std::string{});
    return result;
}

void create_schema(sqlite3* db) {
    exec_sql(db, "PRAGMA foreign_keys=ON;");
    exec_sql(db, "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);");
    exec_sql(db,
             "CREATE TABLE IF NOT EXISTS project_files("
             "path TEXT PRIMARY KEY, kind TEXT NOT NULL);");
    exec_sql(db,
             "CREATE TABLE IF NOT EXISTS compilation_databases("
             "path TEXT PRIMARY KEY);");
    exec_sql(db,
             "CREATE TABLE IF NOT EXISTS build_file_counts("
             "type TEXT PRIMARY KEY, count INTEGER NOT NULL);");
    exec_sql(db,
             "CREATE TABLE IF NOT EXISTS classes("
             "id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, "
             "qualified_name TEXT NOT NULL, file TEXT NOT NULL, line INTEGER NOT NULL);");
    exec_sql(db,
             "CREATE TABLE IF NOT EXISTS methods("
             "class_id INTEGER NOT NULL REFERENCES classes(id) ON DELETE CASCADE, "
             "name TEXT NOT NULL, qualified_name TEXT NOT NULL, file TEXT NOT NULL, "
             "line INTEGER NOT NULL);");
    exec_sql(db,
             "CREATE TABLE IF NOT EXISTS functions("
             "name TEXT NOT NULL, qualified_name TEXT NOT NULL, file TEXT NOT NULL, "
             "line INTEGER NOT NULL, kind TEXT NOT NULL);");
    exec_sql(db,
             "CREATE TABLE IF NOT EXISTS call_graph("
             "caller TEXT NOT NULL, callee TEXT NOT NULL, file TEXT NOT NULL, "
             "line INTEGER NOT NULL);");
    exec_sql(db,
             "CREATE TABLE IF NOT EXISTS skipped_code_files("
             "file TEXT NOT NULL, reason TEXT NOT NULL);");
    exec_sql(db,
             "CREATE TABLE IF NOT EXISTS scan_fingerprints("
             "path TEXT PRIMARY KEY, fingerprint TEXT NOT NULL);");
    exec_sql(db,
             "CREATE VIRTUAL TABLE IF NOT EXISTS index_fts USING "
             "fts5(path UNINDEXED, kind UNINDEXED, content);");
}

void put_meta(sqlite3* db, const std::string& key, const std::string& value) {
    Statement stmt(db, "INSERT OR REPLACE INTO meta(key, value) VALUES(?1, ?2);");
    bind_text(stmt.stmt, 1, key);
    bind_text(stmt.stmt, 2, value);
    if (sqlite3_step(stmt.stmt) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
}

std::string get_meta(sqlite3* db, const std::string& key) {
    Statement stmt(db, "SELECT value FROM meta WHERE key=?1;");
    bind_text(stmt.stmt, 1, key);
    if (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
        return column_text(stmt.stmt, 0);
    }
    return {};
}

bool complete_scan_fingerprints(
    const ProjectScanResult& scan, const CodeIndex& code_index,
    const std::unordered_map<std::string, std::string>& fingerprints) {
    if (fingerprints.empty()) return false;
    const auto covered = [&fingerprints](const std::filesystem::path& path) {
        if (path.empty()) return true;
        const auto found = fingerprints.find(path.generic_string());
        return found != fingerprints.end() && !found->second.empty();
    };
    for (const auto& file : scan.files) {
        if (!covered(file.path)) return false;
    }
    for (const auto& klass : code_index.classes) {
        if (!covered(klass.file)) return false;
        for (const auto& method : klass.methods) {
            if (!covered(method.file)) return false;
        }
    }
    for (const auto& function : code_index.functions) {
        if (!covered(function.file)) return false;
    }
    for (const auto& edge : code_index.call_graph) {
        if (!covered(edge.file)) return false;
    }
    return true;
}

void insert_fts(sqlite3* db,
                const std::string& path,
                const std::string& kind,
                const std::string& content) {
    if (content.empty()) {
        return;
    }
    Statement stmt(db, "INSERT INTO index_fts(path, kind, content) VALUES(?1, ?2, ?3);");
    bind_text(stmt.stmt, 1, path);
    bind_text(stmt.stmt, 2, kind);
    bind_text(stmt.stmt, 3, content);
    if (sqlite3_step(stmt.stmt) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
}

bool table_has_rows(sqlite3* db, const char* table) {
    const std::string sql = std::string{"SELECT 1 FROM "} + table + " LIMIT 1;";
    Statement stmt(db, sql.c_str());
    return sqlite3_step(stmt.stmt) == SQLITE_ROW;
}

bool table_exists(sqlite3* db, const char* table) {
    Statement stmt(db,
                   "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1 LIMIT 1;");
    bind_text(stmt.stmt, 1, table);
    return sqlite3_step(stmt.stmt) == SQLITE_ROW;
}

}  // namespace

std::filesystem::path sqlite_index_database_path(const std::filesystem::path& state_directory) {
    return state_directory / "index.sqlite3";
}

std::optional<SqliteIndexSnapshot> load_sqlite_index_snapshot(
    const std::filesystem::path& database_path,
    const std::filesystem::path& project_root,
    std::string* error) {
    try {
        if (!std::filesystem::is_regular_file(database_path)) {
            return std::nullopt;
        }
        auto db = open_database_readonly(database_path);
        exec_sql(db.get(), "BEGIN;");
        if (get_meta(db.get(), "schema_version") != std::to_string(kSchemaVersion)) {
            return std::nullopt;
        }
        const auto canonical_root = std::filesystem::weakly_canonical(project_root).string();
        if (get_meta(db.get(), "project_root") != canonical_root) {
            return std::nullopt;
        }

        SqliteIndexSnapshot snapshot;
        snapshot.scan.root = std::filesystem::weakly_canonical(project_root);
        snapshot.generation = static_cast<std::size_t>(
            std::strtoull(get_meta(db.get(), "generation").c_str(), nullptr, 10));
        snapshot.compile_commands = static_cast<std::size_t>(
            std::strtoull(get_meta(db.get(), "compile_commands").c_str(), nullptr, 10));
        snapshot.build_metadata_summary = get_meta(db.get(), "build_metadata_summary");
        snapshot.last_scan_finished = time_from_epoch_text(get_meta(db.get(), "last_scan_finished"));
        snapshot.tree_sitter = tree_sitter_from_json(get_meta(db.get(), "tree_sitter"));
        snapshot.fts_available = table_exists(db.get(), "index_fts") &&
                                 table_has_rows(db.get(), "index_fts");
        snapshot.scan_fingerprints_complete =
            get_meta(db.get(), "scan_fingerprints_complete") == "1";
        if (snapshot.scan_fingerprints_complete && table_exists(db.get(), "scan_fingerprints")) {
            Statement stmt(db.get(),
                           "SELECT path, fingerprint FROM scan_fingerprints ORDER BY path;");
            while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
                snapshot.scan_fingerprints.emplace(column_text(stmt.stmt, 0),
                                                   column_text(stmt.stmt, 1));
            }
        }

        {
            Statement stmt(db.get(), "SELECT path, kind FROM project_files ORDER BY path;");
            while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
                snapshot.scan.files.push_back(
                    {std::filesystem::path{column_text(stmt.stmt, 0)},
                     file_kind_from_string(column_text(stmt.stmt, 1))});
            }
        }
        {
            Statement stmt(db.get(), "SELECT path FROM compilation_databases ORDER BY path;");
            while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
                snapshot.scan.existing_compilation_databases.emplace_back(column_text(stmt.stmt, 0));
            }
        }
        {
            Statement stmt(db.get(), "SELECT type, count FROM build_file_counts ORDER BY type;");
            while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
                snapshot.scan.build_files_by_type[column_text(stmt.stmt, 0)] =
                    static_cast<std::size_t>(sqlite3_column_int64(stmt.stmt, 1));
            }
        }
        snapshot.scan.skipped_directories = static_cast<std::size_t>(
            std::strtoull(get_meta(db.get(), "skipped_directories").c_str(), nullptr, 10));

        {
            Statement class_stmt(
                db.get(), "SELECT id, name, qualified_name, file, line FROM classes ORDER BY id;");
            Statement method_stmt(
                db.get(),
                "SELECT name, qualified_name, file, line FROM methods WHERE class_id=?1 ORDER BY rowid;");
            while (sqlite3_step(class_stmt.stmt) == SQLITE_ROW) {
                const auto class_id = sqlite3_column_int64(class_stmt.stmt, 0);
                ClassSymbol klass;
                klass.name = column_text(class_stmt.stmt, 1);
                klass.qualified_name = column_text(class_stmt.stmt, 2);
                klass.file = column_text(class_stmt.stmt, 3);
                klass.line = static_cast<std::size_t>(sqlite3_column_int64(class_stmt.stmt, 4));
                sqlite3_reset(method_stmt.stmt);
                sqlite3_clear_bindings(method_stmt.stmt);
                sqlite3_bind_int64(method_stmt.stmt, 1, class_id);
                while (sqlite3_step(method_stmt.stmt) == SQLITE_ROW) {
                    klass.methods.push_back({column_text(method_stmt.stmt, 0),
                                             column_text(method_stmt.stmt, 1),
                                             std::filesystem::path{column_text(method_stmt.stmt, 2)},
                                             static_cast<std::size_t>(
                                                 sqlite3_column_int64(method_stmt.stmt, 3))});
                }
                snapshot.code_index.classes.push_back(std::move(klass));
            }
        }
        {
            Statement stmt(db.get(),
                           "SELECT name, qualified_name, file, line, kind FROM functions ORDER BY rowid;");
            while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
                snapshot.code_index.functions.push_back(
                    {column_text(stmt.stmt, 0),
                     column_text(stmt.stmt, 1),
                     std::filesystem::path{column_text(stmt.stmt, 2)},
                     static_cast<std::size_t>(sqlite3_column_int64(stmt.stmt, 3)),
                     column_text(stmt.stmt, 4)});
            }
        }
        {
            Statement stmt(db.get(), "SELECT caller, callee, file, line FROM call_graph ORDER BY rowid;");
            while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
                snapshot.code_index.call_graph.push_back(
                    {column_text(stmt.stmt, 0),
                     column_text(stmt.stmt, 1),
                     std::filesystem::path{column_text(stmt.stmt, 2)},
                     static_cast<std::size_t>(sqlite3_column_int64(stmt.stmt, 3))});
            }
        }
        {
            Statement stmt(db.get(), "SELECT file, reason FROM skipped_code_files ORDER BY rowid;");
            while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
                snapshot.code_index.skipped_files.push_back(
                    {std::filesystem::path{column_text(stmt.stmt, 0)}, column_text(stmt.stmt, 1)});
            }
        }
        snapshot.scan_fingerprints_complete =
            snapshot.scan_fingerprints_complete &&
            complete_scan_fingerprints(snapshot.scan, snapshot.code_index,
                                       snapshot.scan_fingerprints);
        exec_sql(db.get(), "COMMIT;");
        return snapshot;
    } catch (const std::exception& ex) {
        if (error != nullptr) {
            *error = ex.what();
        }
        return std::nullopt;
    }
}

void save_sqlite_index_snapshot(const std::filesystem::path& database_path,
                                const std::filesystem::path& project_root,
                                const ProjectScanResult& scan,
                                const CodeIndex& code_index,
                                std::size_t generation,
                                std::size_t compile_commands,
                                const std::string& build_metadata_summary,
                                const TreeSitterResult& tree_sitter,
                                const std::unordered_map<std::string, std::string>&
                                    scan_fingerprints) {
    std::filesystem::create_directories(database_path.parent_path());
    auto db = open_database(database_path);
    create_schema(db.get());
    exec_sql(db.get(), "BEGIN IMMEDIATE;");
    try {
        exec_sql(db.get(), "DELETE FROM meta;");
        exec_sql(db.get(), "DELETE FROM project_files;");
        exec_sql(db.get(), "DELETE FROM compilation_databases;");
        exec_sql(db.get(), "DELETE FROM build_file_counts;");
        exec_sql(db.get(), "DELETE FROM classes;");
        exec_sql(db.get(), "DELETE FROM functions;");
        exec_sql(db.get(), "DELETE FROM call_graph;");
        exec_sql(db.get(), "DELETE FROM skipped_code_files;");
        exec_sql(db.get(), "DELETE FROM scan_fingerprints;");
        exec_sql(db.get(), "DELETE FROM index_fts;");

        put_meta(db.get(), "schema_version", std::to_string(kSchemaVersion));
        put_meta(db.get(), "project_root", std::filesystem::weakly_canonical(project_root).string());
        put_meta(db.get(), "generation", std::to_string(generation));
        put_meta(db.get(), "compile_commands", std::to_string(compile_commands));
        put_meta(db.get(), "build_metadata_summary", build_metadata_summary);
        put_meta(db.get(), "skipped_directories", std::to_string(scan.skipped_directories));
        put_meta(db.get(), "last_scan_finished", time_to_epoch_text(std::chrono::system_clock::now()));
        put_meta(db.get(), "tree_sitter", tree_sitter_to_json(tree_sitter).dump());
        put_meta(db.get(), "scan_fingerprints_complete",
                 complete_scan_fingerprints(scan, code_index, scan_fingerprints) ? "1" : "0");

        {
            Statement stmt(db.get(),
                           "INSERT INTO scan_fingerprints(path, fingerprint) VALUES(?1, ?2);");
            for (const auto& [path, fingerprint] : scan_fingerprints) {
                sqlite3_reset(stmt.stmt);
                sqlite3_clear_bindings(stmt.stmt);
                bind_text(stmt.stmt, 1, path);
                bind_text(stmt.stmt, 2, fingerprint);
                if (sqlite3_step(stmt.stmt) != SQLITE_DONE) {
                    throw std::runtime_error(sqlite3_errmsg(db.get()));
                }
            }
        }

        {
            Statement stmt(db.get(), "INSERT INTO project_files(path, kind) VALUES(?1, ?2);");
            for (const auto& file : scan.files) {
                sqlite3_reset(stmt.stmt);
                sqlite3_clear_bindings(stmt.stmt);
                const auto path = file.path.generic_string();
                bind_text(stmt.stmt, 1, path);
                bind_text(stmt.stmt, 2, file_kind_to_string(file.kind));
                if (sqlite3_step(stmt.stmt) != SQLITE_DONE) {
                    throw std::runtime_error(sqlite3_errmsg(db.get()));
                }
                if (fts_file_kind(file.kind)) {
                    insert_fts(db.get(),
                               path,
                               "file",
                               path + "\n" +
                                   read_file_for_fts((scan.root / file.path).lexically_normal()));
                }
            }
        }
        {
            Statement stmt(db.get(), "INSERT INTO compilation_databases(path) VALUES(?1);");
            for (const auto& path : scan.existing_compilation_databases) {
                sqlite3_reset(stmt.stmt);
                sqlite3_clear_bindings(stmt.stmt);
                bind_text(stmt.stmt, 1, path.generic_string());
                if (sqlite3_step(stmt.stmt) != SQLITE_DONE) {
                    throw std::runtime_error(sqlite3_errmsg(db.get()));
                }
            }
        }
        {
            Statement stmt(db.get(),
                           "INSERT INTO build_file_counts(type, count) VALUES(?1, ?2);");
            for (const auto& [type, count] : scan.build_files_by_type) {
                sqlite3_reset(stmt.stmt);
                sqlite3_clear_bindings(stmt.stmt);
                bind_text(stmt.stmt, 1, type);
                sqlite3_bind_int64(stmt.stmt, 2, static_cast<sqlite3_int64>(count));
                if (sqlite3_step(stmt.stmt) != SQLITE_DONE) {
                    throw std::runtime_error(sqlite3_errmsg(db.get()));
                }
            }
        }
        {
            Statement class_stmt(
                db.get(),
                "INSERT INTO classes(name, qualified_name, file, line) VALUES(?1, ?2, ?3, ?4);");
            Statement method_stmt(
                db.get(),
                "INSERT INTO methods(class_id, name, qualified_name, file, line) "
                "VALUES(?1, ?2, ?3, ?4, ?5);");
            for (const auto& klass : code_index.classes) {
                sqlite3_reset(class_stmt.stmt);
                sqlite3_clear_bindings(class_stmt.stmt);
                bind_text(class_stmt.stmt, 1, klass.name);
                bind_text(class_stmt.stmt, 2, klass.qualified_name);
                bind_text(class_stmt.stmt, 3, klass.file.generic_string());
                sqlite3_bind_int64(class_stmt.stmt, 4, static_cast<sqlite3_int64>(klass.line));
                if (sqlite3_step(class_stmt.stmt) != SQLITE_DONE) {
                    throw std::runtime_error(sqlite3_errmsg(db.get()));
                }
                const auto class_id = sqlite3_last_insert_rowid(db.get());
                insert_fts(db.get(),
                           klass.file.generic_string(),
                           "class",
                           klass.name + " " + klass.qualified_name);
                for (const auto& method : klass.methods) {
                    sqlite3_reset(method_stmt.stmt);
                    sqlite3_clear_bindings(method_stmt.stmt);
                    sqlite3_bind_int64(method_stmt.stmt, 1, class_id);
                    bind_text(method_stmt.stmt, 2, method.name);
                    bind_text(method_stmt.stmt, 3, method.qualified_name);
                    bind_text(method_stmt.stmt, 4, method.file.generic_string());
                    sqlite3_bind_int64(method_stmt.stmt, 5, static_cast<sqlite3_int64>(method.line));
                    if (sqlite3_step(method_stmt.stmt) != SQLITE_DONE) {
                        throw std::runtime_error(sqlite3_errmsg(db.get()));
                    }
                }
            }
        }
        {
            Statement stmt(
                db.get(),
                "INSERT INTO functions(name, qualified_name, file, line, kind) "
                "VALUES(?1, ?2, ?3, ?4, ?5);");
            for (const auto& function : code_index.functions) {
                sqlite3_reset(stmt.stmt);
                sqlite3_clear_bindings(stmt.stmt);
                bind_text(stmt.stmt, 1, function.name);
                bind_text(stmt.stmt, 2, function.qualified_name);
                bind_text(stmt.stmt, 3, function.file.generic_string());
                sqlite3_bind_int64(stmt.stmt, 4, static_cast<sqlite3_int64>(function.line));
                bind_text(stmt.stmt, 5, function.kind);
                if (sqlite3_step(stmt.stmt) != SQLITE_DONE) {
                    throw std::runtime_error(sqlite3_errmsg(db.get()));
                }
                insert_fts(db.get(),
                           function.file.generic_string(),
                           function.kind.empty() ? "function" : function.kind,
                           function.name + " " + function.qualified_name);
            }
        }
        {
            Statement stmt(
                db.get(),
                "INSERT INTO call_graph(caller, callee, file, line) VALUES(?1, ?2, ?3, ?4);");
            for (const auto& edge : code_index.call_graph) {
                sqlite3_reset(stmt.stmt);
                sqlite3_clear_bindings(stmt.stmt);
                bind_text(stmt.stmt, 1, edge.caller);
                bind_text(stmt.stmt, 2, edge.callee);
                bind_text(stmt.stmt, 3, edge.file.generic_string());
                sqlite3_bind_int64(stmt.stmt, 4, static_cast<sqlite3_int64>(edge.line));
                if (sqlite3_step(stmt.stmt) != SQLITE_DONE) {
                    throw std::runtime_error(sqlite3_errmsg(db.get()));
                }
            }
        }
        {
            Statement stmt(db.get(),
                           "INSERT INTO skipped_code_files(file, reason) VALUES(?1, ?2);");
            for (const auto& skipped : code_index.skipped_files) {
                sqlite3_reset(stmt.stmt);
                sqlite3_clear_bindings(stmt.stmt);
                bind_text(stmt.stmt, 1, skipped.file.generic_string());
                bind_text(stmt.stmt, 2, skipped.reason);
                if (sqlite3_step(stmt.stmt) != SQLITE_DONE) {
                    throw std::runtime_error(sqlite3_errmsg(db.get()));
                }
            }
        }
        exec_sql(db.get(), "COMMIT;");
    } catch (...) {
        sqlite3_exec(db.get(), "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
}

nlohmann::json sqlite_fts_search_payload(const std::filesystem::path& database_path,
                                          const std::filesystem::path& project_root,
                                          const std::string& query,
                                          std::size_t limit) {
    if (query.empty()) {
        throw std::invalid_argument("fts search requires query");
    }
    auto db = open_database(database_path);
    create_schema(db.get());
    if (get_meta(db.get(), "schema_version") != std::to_string(kSchemaVersion) ||
        get_meta(db.get(), "project_root") != std::filesystem::weakly_canonical(project_root).string()) {
        throw std::runtime_error("sqlite index snapshot is not available for this project");
    }

    Statement stmt(
        db.get(),
        "SELECT path, kind, snippet(index_fts, 2, '[', ']', ' ... ', 12), rank "
        "FROM index_fts WHERE index_fts MATCH ?1 ORDER BY rank LIMIT ?2;");
    bind_text(stmt.stmt, 1, query);
    sqlite3_bind_int64(stmt.stmt, 2, static_cast<sqlite3_int64>(limit == 0 ? 100 : limit));
    json matches = json::array();
    while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
        matches.push_back({{"path", column_text(stmt.stmt, 0)},
                           {"kind", column_text(stmt.stmt, 1)},
                           {"snippet", column_text(stmt.stmt, 2)},
                           {"rank", sqlite3_column_double(stmt.stmt, 3)}});
    }
    return {{"query", query},
            {"database", database_path.string()},
            {"matches", matches},
            {"limit", limit == 0 ? 100 : limit}};
}

nlohmann::json sqlite_fts_search_payload_pinned(const std::filesystem::path& database_path,
                                                 const std::filesystem::path& project_root,
                                                 const std::string& query,
                                                 std::size_t limit,
                                                 std::size_t generation) {
    if (query.empty()) throw std::invalid_argument("fts search requires query");
    auto db = open_database_readonly(database_path);
    exec_sql(db.get(), "BEGIN;");
    try {
        if (get_meta(db.get(), "schema_version") != std::to_string(kSchemaVersion) ||
            get_meta(db.get(), "project_root") != std::filesystem::weakly_canonical(project_root).string() ||
            get_meta(db.get(), "generation") != std::to_string(generation))
            throw std::runtime_error("sqlite index snapshot generation is stale");
        Statement stmt(db.get(),
                       "SELECT path, kind, snippet(index_fts, 2, '[', ']', ' ... ', 12), rank "
                       "FROM index_fts WHERE index_fts MATCH ?1 ORDER BY rank LIMIT ?2;");
        bind_text(stmt.stmt, 1, query);
        sqlite3_bind_int64(stmt.stmt, 2, static_cast<sqlite3_int64>(limit));
        json matches = json::array();
        int rc;
        while ((rc = sqlite3_step(stmt.stmt)) == SQLITE_ROW) {
            matches.push_back({{"path", column_text(stmt.stmt, 0)},
                               {"kind", column_text(stmt.stmt, 1)},
                               {"snippet", column_text(stmt.stmt, 2)},
                               {"rank", sqlite3_column_double(stmt.stmt, 3)}});
        }
        if (rc != SQLITE_DONE) throw std::runtime_error("pinned code FTS query failed");
        exec_sql(db.get(), "COMMIT;");
        return {{"query", query}, {"generation", generation}, {"matches", matches}, {"limit", limit}};
    } catch (...) {
        sqlite3_exec(db.get(), "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
}

}  // namespace qt::index
