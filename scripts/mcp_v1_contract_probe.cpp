#include <sstream>
#define private public
#include "index/service.hpp"
#undef private
#include "index/mcp_http_code_index.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

using namespace qt::index;
using json = nlohmann::json;

int main() {
  const auto root = std::filesystem::temp_directory_path() / "qt-mcp-v1-contract-probe";
  std::filesystem::remove_all(root); std::filesystem::create_directories(root / ".index");
  std::ofstream(root / "main.cpp") << "int answer() { return 42; }\nint caller() { return answer(); }\n";
  ProjectScanResult scan; scan.root = root; scan.files.push_back({"main.cpp", ProjectFileKind::CxxSource});
  CodeIndex index;
  index.functions.push_back({"answer", "answer", "main.cpp", 1, "function"});
  index.functions.push_back({"caller", "caller", "main.cpp", 2, "function"});
  index.call_graph.push_back({"caller", "answer", "main.cpp", 2});
  ServiceOptions options; options.project_root = root; options.state_directory = root / ".index";
  options.defer_snapshot_load = true;
  BackgroundService service(options);
  service.scan_ = scan; service.code_index_ = index;

  const auto graph = memory_graph_payload(service);
  assert(graph.is_object());
  assert(graph.size() == 4 && graph.at("format") == "index-codebase-memory-v1" &&
         graph.at("root").is_string() && graph.at("nodes").is_array() && graph.at("edges").is_array());
  for (const auto& node : graph.at("nodes")) {
    assert(node.is_object() && node.contains("id") && node.at("id").is_string());
    assert(node.contains("label") && node.at("label").is_string());
    if (node.at("label") == "Project")
      assert(node.contains("name") && node.contains("path"));
    else if (node.at("label") == "File")
      assert(node.contains("name") && node.contains("path") && node.contains("kind"));
    else
      assert((node.at("label") == "Function" || node.at("label") == "Method") &&
             node.contains("name") && node.contains("qualified_name") &&
             node.contains("file") && node.contains("line") && node.at("line").is_number());
  }
  bool contains = false, defines = false, calls = false;
  for (const auto& edge : graph.at("edges")) {
    assert(edge.is_object() && edge.contains("from") && edge.at("from").is_string() &&
           edge.contains("to") && edge.at("to").is_string() && edge.contains("type") &&
           edge.at("type").is_string());
    if (edge.at("type") == "CONTAINS_FILE") contains = true;
    if (edge.at("type") == "DEFINES") defines = true;
    if (edge.at("type") == "CALLS") {
      calls = true;
      assert(edge.contains("callee") && edge.contains("file") && edge.contains("line"));
    }
  }
  assert(contains && defines && calls);

  HttpRequest snippet_request; snippet_request.query["qualified_name"] = "answer";
  snippet_request.query["before"] = "0"; snippet_request.query["after"] = "0";
  const auto snippet = code_snippet_payload(service, snippet_request);
  assert(snippet.is_object() && snippet.at("qualified_name") == "answer" &&
         snippet.at("file") == "main.cpp" && snippet.at("line") == 1 &&
         snippet.at("lines").is_array() && snippet.at("lines").size() == 1 &&
         snippet.at("lines").front().at("target") == true &&
         snippet.at("lines").front().at("text") == "int answer() { return 42; }");
  std::filesystem::remove_all(root);
  std::cout << "mcp v1 contract probe passed\n";
}
