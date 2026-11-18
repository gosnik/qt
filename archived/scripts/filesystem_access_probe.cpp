#include "qt/tools/write_text_tool.hpp"

#include "qt/foundation/filesystem.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using qt::tools::Json;
using qt::tools::ToolContext;
using qt::tools::ToolRequest;
using qt::tools::ToolResultStatus;
using qt::tools::WriteTextTool;

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

ToolRequest write_request(const std::filesystem::path& path,
                          std::string content) {
  ToolRequest request;
  request.tool_name = "write_text";
  request.call_id = "filesystem-access-probe";
  request.arguments =
      Json{{"path", path.generic_string()}, {"content", std::move(content)}};
  return request;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 2, "usage: filesystem_access_probe <runtime-dir>");
    const std::filesystem::path root = argv[1];
    const std::filesystem::path workspace = root / "workspace";
    const std::filesystem::path outside = root / "outside";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(workspace);
    std::filesystem::create_directories(outside);
    std::filesystem::create_directory_symlink(outside,
                                              workspace / "outside-link");

    ToolContext context;
    context.workspace_root = workspace;
    context.current_directory = workspace;
    WriteTextTool tool;

    const auto inside = tool.invoke(
        write_request("inside.txt", "inside"), context);
    require(inside.status == ToolResultStatus::success &&
                read_file(workspace / "inside.txt") == "inside",
            "confined access did not preserve relative workspace writes");

    const auto denied = tool.invoke(
        write_request(outside / "denied.txt", "denied"), context);
    require(denied.status == ToolResultStatus::permission_denied &&
                !std::filesystem::exists(outside / "denied.txt"),
            "confined access allowed an absolute outside write");

    const auto symlink_denied = tool.invoke(
        write_request(workspace / "outside-link" / "denied-link.txt",
                      "denied"),
        context);
    require(symlink_denied.status == ToolResultStatus::permission_denied &&
                !std::filesystem::exists(outside / "denied-link.txt"),
            "confined access allowed a symlink escape");

    context.allow_external_filesystem_access = true;
    const auto allowed = tool.invoke(
        write_request(outside / "allowed.txt", "allowed"), context);
    require(allowed.status == ToolResultStatus::success &&
                read_file(outside / "allowed.txt") == "allowed",
            "enabled external access rejected an absolute outside write");

    const auto symlink_allowed = tool.invoke(
        write_request(workspace / "outside-link" / "allowed-link.txt",
                      "allowed-link"),
        context);
    require(symlink_allowed.status == ToolResultStatus::success &&
                read_file(outside / "allowed-link.txt") == "allowed-link",
            "enabled external access rejected a symlink-resolved outside write");

    std::cout << "filesystem access tool probe passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "filesystem access tool probe failed: " << error.what()
              << '\n';
    return 1;
  }
}
