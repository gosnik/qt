#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace qt::index {

class GitIgnoreMatcher {
public:
    static GitIgnoreMatcher load(const std::filesystem::path& root);

    bool ignores(const std::filesystem::path& path) const;
    bool ignores(const std::filesystem::path& path, bool is_directory) const;

private:
    struct Rule {
        std::filesystem::path base;
        std::string pattern;
        bool negated = false;
        bool directory_only = false;
        bool basename_only = false;
    };

    explicit GitIgnoreMatcher(std::filesystem::path root);

    void load_file(const std::filesystem::path& gitignore);

    std::filesystem::path root_;
    std::map<std::string, std::vector<Rule>> rules_by_base_;
};

}  // namespace qt::index
