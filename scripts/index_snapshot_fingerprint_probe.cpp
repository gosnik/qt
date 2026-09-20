#include "index/sqlite_store.hpp"
#include "index/retrieval_snapshot.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <unordered_map>
#include <sqlite3.h>

using namespace qt::index;

static std::string bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path root = argv[1];
    if (std::filesystem::exists(root)) return 2;
    std::filesystem::create_directories(root / ".state");
    std::ofstream(root / "main.cpp") << "int main() { return 0; }\n";
    ProjectScanResult scan;
    scan.root = root;
    scan.files.push_back({"main.cpp", ProjectFileKind::CxxSource});
    CodeIndex index;
    index.functions.push_back({"main", "main", "main.cpp", 1, "function"});
    const auto fingerprint = retrieval_source_fingerprint(root / "main.cpp");
    const auto db = sqlite_index_database_path(root / ".state");
    const auto default_db = root / ".state" / "default.sqlite3";
    save_sqlite_index_snapshot(default_db, root, scan, index, 3, 0, "probe",
                               TreeSitterResult{});
    std::string error;
    const auto default_loaded = load_sqlite_index_snapshot(default_db, root, &error);
    assert(default_loaded && !default_loaded->scan_fingerprints_complete);
    sqlite3* legacy_raw = nullptr;
    assert(sqlite3_open(default_db.string().c_str(), &legacy_raw) == SQLITE_OK);
    assert(sqlite3_exec(legacy_raw, "DROP TABLE scan_fingerprints;", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(legacy_raw);
    const auto before_legacy_load = bytes(default_db);
    const auto legacy_without_table = load_sqlite_index_snapshot(default_db, root, &error);
    assert(legacy_without_table && !legacy_without_table->scan_fingerprints_complete);
    assert(bytes(default_db) == before_legacy_load);
    const std::unordered_map<std::string, std::string> fingerprints{{"main.cpp", fingerprint}};
    save_sqlite_index_snapshot(db, root, scan, index, 4, 0, "probe", TreeSitterResult{},
                               fingerprints);
    const auto loaded = load_sqlite_index_snapshot(db, root, &error);
    assert(loaded && loaded->scan_fingerprints_complete);
    assert(loaded->scan_fingerprints.at("main.cpp") == fingerprint);

    sqlite3* raw = nullptr;
    assert(sqlite3_open(db.string().c_str(), &raw) == SQLITE_OK);
    assert(sqlite3_exec(raw, "DELETE FROM scan_fingerprints WHERE path='main.cpp';",
                        nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
    const auto legacy = load_sqlite_index_snapshot(db, root, &error);
    assert(legacy && !legacy->scan_fingerprints_complete && legacy->scan_fingerprints.empty());
    std::filesystem::remove_all(root);
    std::cout << "index snapshot fingerprint probe passed\n";
}
