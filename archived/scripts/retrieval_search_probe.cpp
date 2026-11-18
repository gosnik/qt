#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sqlite3.h>

#define private public
#include "index/service.hpp"
#undef private

using namespace qt::index;

int main() {
    const auto root = std::filesystem::temp_directory_path() / "qt-retrieval-search-probe";
    std::filesystem::remove_all(root); std::filesystem::create_directories(root / ".index");
    std::ofstream(root / "needle.cpp") << "int needle_symbol() { return 7; }\n";
    ProjectScanResult scan; scan.root = root; scan.files.push_back({"needle.cpp", ProjectFileKind::CxxSource});
    CodeIndex index; index.functions.push_back({"needle_symbol", "needle_symbol", "needle.cpp", 1, "function"});
    const auto snapshot = RetrievalSnapshot::build(scan, index, 7);
    const auto database = root / ".index" / "index.sqlite3";
    sqlite3* db = nullptr; assert(sqlite3_open(database.string().c_str(), &db) == SQLITE_OK);
    const auto sql = std::string("CREATE TABLE meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
        "INSERT INTO meta VALUES('schema_version','1');INSERT INTO meta VALUES('generation','7');INSERT INTO meta VALUES('project_root','") +
        std::filesystem::weakly_canonical(root).string() + "');CREATE VIRTUAL TABLE index_fts USING fts5(path UNINDEXED,kind UNINDEXED,content);INSERT INTO index_fts VALUES('needle.cpp','file','needle');";
    assert(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK); sqlite3_close(db);
    ServiceOptions options; options.project_root = root; options.state_directory = root / ".index"; options.defer_snapshot_load = true;
    options.retrieval_enabled = true;
    BackgroundService service(options); service.retrieval_snapshot_ = snapshot;
    service.status_.generation = 7; service.status_.sqlite_database = database;
    const auto found = service.retrieval_search("needle", 10);
    assert(!found.stale && found.hits.size() == 1 && found.hits.front().generation == 7);
    assert(!service.retrieval_search("needle_symbol", 10).hits.empty());
    const auto natural = service.retrieval_search("Where is needle?", 10);
    assert(!natural.stale && natural.diagnostic.empty() && !natural.hits.empty());
    const auto punctuation = service.retrieval_search("needle: (implementation)", 10);
    assert(!punctuation.stale && punctuation.diagnostic.empty() && !punctuation.hits.empty());
    std::ofstream(root / "needle.cpp", std::ios::app) << "// changed\n";
    assert(service.retrieval_search("needle", 10).hits.empty());
    assert(sqlite3_open(database.string().c_str(), &db) == SQLITE_OK);
    assert(sqlite3_exec(db, "UPDATE meta SET value='8' WHERE key='generation';", nullptr, nullptr, nullptr) == SQLITE_OK); sqlite3_close(db);
    const auto stale = service.retrieval_search("needle", 10); assert(stale.stale && stale.hits.empty());
    std::filesystem::remove_all(root); std::cout << "retrieval search probe passed\n";
}
