#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qt::index {

struct ParsedAssignment {
    std::string key;
    std::string value;
};

std::optional<ParsedAssignment> parse_assignment_line(std::string_view line);
std::vector<ParsedAssignment> parse_assignment_file(const std::filesystem::path& path);

}  // namespace qt::index
