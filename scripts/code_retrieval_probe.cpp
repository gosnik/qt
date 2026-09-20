#include "index/retrieval_snapshot.hpp"

#include <cassert>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace qt::index;

static void write_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

static ProjectScanResult scan_for(const std::filesystem::path& root) {
    ProjectScanResult scan;
    scan.root = root;
    scan.files.push_back({"main.cpp", ProjectFileKind::CxxSource});
    return scan;
}

int main() {
    const auto base = std::filesystem::temp_directory_path() / "qt-code-retrieval-probe";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base / "a");
    std::filesystem::create_directories(base / "b");
    const std::string source = "int f(int x) { return x; }\nint f(double x) { return int(x); }\nint caller() { return f(1); }\n";
    write_file(base / "a/main.cpp", source);
    write_file(base / "b/main.cpp", source);

    CodeIndex index;
    index.functions.push_back({"f", "f", "main.cpp", 1, "function"});
    index.functions.push_back({"f", "f", "main.cpp", 2, "function"});
    index.functions.push_back({"caller", "caller", "main.cpp", 3, "function"});
    index.call_graph.push_back({"caller", "f", "main.cpp", 3});
    const auto scan = scan_for(base / "a");
    const auto fp = retrieval_source_fingerprint(scan.root / "main.cpp");
    const std::unordered_map<std::string, std::string> fingerprints{{"main.cpp", fp}};
    const auto snapshot = RetrievalSnapshot::build(scan, index, 7, fingerprints);
    assert(snapshot->find_symbols("f").size() == 2);
    assert(snapshot->read_snippet(snapshot->find_symbols("f").front().id));
    bool ambiguous = false;
    for (const auto& edge : snapshot->edges()) {
        if (edge.relation == "calls") {
            ambiguous = edge.ambiguous;
            assert(edge.unresolved_name == "f" && edge.callee_ids.size() == 2);
        }
    }
    assert(ambiguous);
    assert(std::count_if(snapshot->edges().begin(), snapshot->edges().end(), [](const auto& e) {
        return e.relation == "defines";
    }) == 3);
    assert(std::count_if(snapshot->edges().begin(), snapshot->edges().end(), [](const auto& e) {
        return e.relation == "contains_file";
    }) == 1);

    const auto id_a = snapshot->find_symbols("f").front().id;
    const auto snapshot_b = RetrievalSnapshot::build(scan_for(base / "b"), index, 7,
                                                      {{"main.cpp", retrieval_source_fingerprint(base / "b/main.cpp")}});
    assert(snapshot_b->find_symbols("f").front().id != id_a);

    write_file(base / "a/main.cpp", "// inserted\n" + source);
    assert(!snapshot->read_snippet(id_a));
    auto moved_index = index;
    for (auto& symbol : moved_index.functions) ++symbol.line;
    ++moved_index.call_graph.front().line;
    auto moved = RetrievalSnapshot::build(scan, moved_index, 8,
        {{"main.cpp", retrieval_source_fingerprint(base / "a/main.cpp")}});
    assert(moved->find_symbols("f").front().id == id_a);
    assert(moved->find_symbols("f")[0].id != moved->find_symbols("f")[1].id);
    const auto old_caller_id = moved->find_symbols("caller").front().id;
    auto renamed_index = moved_index;
    renamed_index.functions[2].name = "renamed_caller";
    renamed_index.functions[2].qualified_name = "renamed_caller";
    auto renamed = RetrievalSnapshot::build(scan, renamed_index, 9,
        {{"main.cpp", retrieval_source_fingerprint(base / "a/main.cpp")} });
    assert(renamed->find_symbols("caller").empty());
    assert(renamed->find_symbols("renamed_caller").size() == 1);
    assert(renamed->find_symbols("renamed_caller").front().id != old_caller_id);
    assert(!renamed->read_snippet(old_caller_id));

    std::filesystem::rename(base / "a/main.cpp", base / "a/renamed.cpp");
    auto file_renamed_index = moved_index;
    for (auto& symbol : file_renamed_index.functions) symbol.file = "renamed.cpp";
    for (auto& edge : file_renamed_index.call_graph) edge.file = "renamed.cpp";
    ProjectScanResult renamed_scan;
    renamed_scan.root = base / "a";
    renamed_scan.files.push_back({"renamed.cpp", ProjectFileKind::CxxSource});
    auto file_renamed = RetrievalSnapshot::build(renamed_scan, file_renamed_index, 10,
        {{"renamed.cpp", retrieval_source_fingerprint(base / "a/renamed.cpp")} });
    assert(file_renamed->find_symbols("caller").size() == 1);
    assert(file_renamed->find_symbols("caller").front().id != old_caller_id);
    assert(!snapshot->read_snippet(id_a));

    std::filesystem::rename(base / "a/renamed.cpp", base / "a/main.cpp");
    auto raced = RetrievalSnapshot::build(scan, moved_index, 8, fingerprints);
    assert(raced->chunks().empty());
    std::filesystem::remove(base / "a/main.cpp");
    assert(!snapshot->read_snippet(id_a));
    assert(!RetrievalSnapshot::build(scan, index, 7, fingerprints, true)->read_snippet(id_a));

    std::filesystem::create_directories(base / "large");
    write_file(base / "large/main.cpp", std::string(1024U * 1024U + 1U, 'x'));
    auto large_scan = scan_for(base / "large");
    auto large = RetrievalSnapshot::build(large_scan, CodeIndex{}, 1,
                                          {{"main.cpp", retrieval_source_fingerprint(base / "large/main.cpp")}});
    assert(large->chunks().empty());
    std::filesystem::remove_all(base);
    std::cout << "code retrieval probe passed\n";
}
