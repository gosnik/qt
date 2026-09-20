#include "index/service.hpp"
#include "qt/tools/index_tool.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("could not write fixture source");
    output << text;
    if (!output) throw std::runtime_error("could not finish fixture source");
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not read tree-sitter artifact");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

qt::index::ServiceStatus wait_for_generation(const qt::index::BackgroundService& service,
                                             std::size_t generation) {
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto status = service.status();
        if (!status.last_error.empty()) {
            throw std::runtime_error("background scan failed: " + status.last_error);
        }
        if (status.generation >= generation && !status.scan_active) return status;
        std::this_thread::sleep_for(20ms);
    }
    throw std::runtime_error("timed out waiting for background scan generation " +
                             std::to_string(generation));
}

qt::tools::ToolResult index_status(const qt::tools::Tool& index,
                                   const qt::tools::ToolContext& context,
                                   qt::tools::Json arguments) {
    qt::tools::ToolRequest request;
    request.tool_name = "index";
    request.arguments = std::move(arguments);
    return index.invoke(request, context);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path root = std::filesystem::absolute(argv[1]);
    if (std::filesystem::exists(root)) return 2;
    std::filesystem::create_directories(root);
    write_file(root / "fixture.cpp",
               "int first_background_round() { return 17; }\n");

    qt::index::ServiceOptions options;
    options.project_root = root;
    options.state_directory = root / ".state";
    options.compile_commands_output = root / "compile_commands.json";
    options.tree_sitter.output_directory = options.state_directory / "tree-sitter";
    options.low_priority_background_scans = true;
    options.scan_interval = std::chrono::seconds(60);
    auto service = std::make_shared<qt::index::BackgroundService>(options);
    service->start();

    try {
        const auto first = wait_for_generation(*service, 1);
        assert(first.running);
        assert(first.tree_sitter.attempted);
        assert(first.tree_sitter.available);
        assert(first.tree_sitter.generated);
        assert(first.tree_sitter.files_considered == 1U);
        assert(first.tree_sitter.files_parsed == 1U);
        assert(first.tree_sitter.files_added == 1U);
        assert(first.tree_sitter.files_updated == 1U);
        const auto tree = first.tree_sitter.output_directory / "trees" / "fixture.cpp.tree";
        assert(std::filesystem::is_regular_file(tree));
        assert(!read_file(tree).empty());
        const auto first_manifest = read_file(first.tree_sitter.manifest_path);

        qt::tools::ToolRegistry registry;
        qt::tools::register_index_tools(registry);
        const auto* index = registry.find("index");
        assert(index != nullptr);
        qt::tools::ToolContext context;
        context.workspace_root = root;
        context.index_service = service;
        const auto default_status = index_status(
            *index, context,
            qt::tools::Json{{"action", "call"}, {"operation", "status"}});
        assert(qt::tools::is_success(default_status));
        assert(default_status.output.at("output").contains("tree_sitter"));
        const auto hidden_status = index_status(
            *index, context,
            qt::tools::Json{{"action", "call"}, {"operation", "status"},
                            {"arguments", qt::tools::Json{{"include_tree_sitter", false}}}});
        assert(qt::tools::is_success(hidden_status));
        assert(!hidden_status.output.at("output").contains("tree_sitter"));
        const auto zero_status = index_status(
            *index, context,
            qt::tools::Json{{"action", "call"}, {"operation", "status"},
                            {"arguments", qt::tools::Json{{"include_tree_sitter", 0}}}});
        assert(qt::tools::is_success(zero_status));
        assert(!zero_status.output.at("output").contains("tree_sitter"));

        write_file(root / "fixture.cpp",
                   "int second_background_round_with_new_content() { return 2718; }\n");
        service->request_scan();
        const auto second = wait_for_generation(*service, 2);
        assert(second.tree_sitter.attempted);
        assert(second.tree_sitter.available);
        assert(second.tree_sitter.generated);
        assert(second.tree_sitter.files_considered == 1U);
        assert(second.tree_sitter.files_parsed == 1U);
        assert(second.tree_sitter.files_changed == 1U);
        assert(second.tree_sitter.files_updated == 1U);
        const auto refreshed_tree = second.tree_sitter.output_directory / "trees" / "fixture.cpp.tree";
        assert(!read_file(refreshed_tree).empty());
        assert(read_file(second.tree_sitter.manifest_path) != first_manifest);
        service->stop();
        std::cout << "background tree-sitter round probe passed\n";
    } catch (...) {
        service->stop();
        std::filesystem::remove_all(root);
        throw;
    }
    std::filesystem::remove_all(root);
}
