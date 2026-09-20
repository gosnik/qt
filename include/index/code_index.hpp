#pragma once

#include "index/project_scan.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace qt::index {

struct MethodSymbol {
    std::string name;
    std::string qualified_name;
    std::filesystem::path file;
    std::size_t line = 0;
};

struct ClassSymbol {
    std::string name;
    std::string qualified_name;
    std::filesystem::path file;
    std::size_t line = 0;
    std::vector<MethodSymbol> methods;
};

struct FunctionSymbol {
    std::string name;
    std::string qualified_name;
    std::filesystem::path file;
    std::size_t line = 0;
    std::string kind;
};

struct CallGraphEdge {
    std::string caller;
    std::string callee;
    std::filesystem::path file;
    std::size_t line = 0;
};

struct SkippedCodeIndexFile {
    std::filesystem::path file;
    std::string reason;
};

struct CodeIndex {
    std::vector<ClassSymbol> classes;
    std::vector<FunctionSymbol> functions;
    std::vector<CallGraphEdge> call_graph;
    std::vector<SkippedCodeIndexFile> skipped_files;
};

CodeIndex build_code_index(const ProjectScanResult& scan);
CodeIndex build_code_index(const ProjectScanResult& scan, bool low_priority);
std::string code_index_json(const CodeIndex& index);
std::string code_index_summary_json(const CodeIndex& index);

}  // namespace qt::index
