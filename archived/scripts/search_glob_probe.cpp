#include "qt/tools/search_text_tool.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using qt::tools::Json;
using qt::tools::SearchTextTool;
using qt::tools::ToolContext;
using qt::tools::ToolRequest;
using qt::tools::ToolResultStatus;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::size_t run_glob(const SearchTextTool& tool,
                     const std::filesystem::path& root,
                     const ToolContext& context,
                     const std::string& glob) {
    ToolRequest request;
    request.tool_name = "search_text";
    request.call_id = "search-glob-probe-" + glob;
    request.arguments = Json{{"path", root.generic_string()},
                             {"query", "needle"},
                             {"glob", glob},
                             {"literal", true}};
    const auto result = tool.invoke(request, context);
    require(result.status == ToolResultStatus::success,
            "search failed for glob " + glob);
    return result.output.at("count").get<std::size_t>();
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: search_glob_probe <runtime-directory>");
        const std::filesystem::path root = argv[1];
        std::filesystem::create_directories(root / "src" / "deep");
        std::filesystem::create_directories(root / "other");
        std::ofstream(root / "root.ts") << "needle\n";
        std::ofstream(root / "src" / "root.ts") << "needle\n";
        std::ofstream(root / "src" / "deep" / "nested.ts") << "needle\n";
        std::ofstream(root / "other" / "nested.ts") << "needle\n";
        std::ofstream(root / "src" / "root.txt") << "needle\n";

        ToolContext context;
        context.workspace_root = root;
        context.current_directory = root;
        SearchTextTool tool;

        const auto recursive_count = run_glob(tool, root, context, "**/*.ts");
        const auto component_count = run_glob(tool, root, context, "src/**/*.ts");
        const auto ordinary_count = run_glob(tool, root, context, "src/*.ts");
        const auto basename_count = run_glob(tool, root, context, "*.ts");
        require(recursive_count == 4, "**/*.ts should find four TypeScript files");
        require(component_count == 2,
                "src/**/*.ts should find src root and deep TypeScript files");
        require(ordinary_count == 1,
                "src/*.ts should find only the direct src TypeScript file");
        require(basename_count == 4,
                "*.ts should match nested TypeScript basenames");
        std::cout << "search glob probe passed (recursive=" << recursive_count
                  << ", component=" << component_count
                  << ", ordinary=" << ordinary_count
                  << ", basename=" << basename_count << ")\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "search glob probe failed: " << error.what() << '\n';
        return 1;
    }
}
