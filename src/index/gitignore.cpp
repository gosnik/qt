#include "index/gitignore.hpp"

#include "index/path_util.hpp"
#include "index/string_util.hpp"

#include <algorithm>
#include <fstream>
#include <string_view>

namespace qt::index {
namespace {

bool glob_matches(std::string_view pattern, std::string_view text) {
    std::size_t pattern_index = 0;
    std::size_t text_index = 0;
    std::size_t star_index = std::string::npos;
    std::size_t star_text_index = 0;

    while (text_index < text.size()) {
        if (pattern_index < pattern.size() &&
            (pattern[pattern_index] == '?' || pattern[pattern_index] == text[text_index])) {
            ++pattern_index;
            ++text_index;
        } else if (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
            if (pattern_index + 1 < pattern.size() && pattern[pattern_index + 1] == '*') {
                ++pattern_index;
            }
            star_index = pattern_index++;
            star_text_index = text_index;
        } else if (star_index != std::string::npos) {
            pattern_index = star_index + 1;
            text_index = ++star_text_index;
        } else {
            return false;
        }
    }
    while (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
        ++pattern_index;
    }
    return pattern_index == pattern.size();
}

bool basename_rule_matches(std::string_view pattern, std::string_view relative) {
    std::size_t start = 0;
    while (start <= relative.size()) {
        const auto slash = relative.find('/', start);
        const auto end = slash == std::string_view::npos ? relative.size() : slash;
        if (glob_matches(pattern, relative.substr(start, end - start))) {
            return true;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return false;
}

bool path_rule_matches(std::string_view pattern, std::string_view relative) {
    return glob_matches(pattern, relative) ||
           (relative.size() > pattern.size() && relative[pattern.size()] == '/' &&
            glob_matches(pattern, relative.substr(0, pattern.size())));
}

bool directory_rule_matches(std::string_view pattern, std::string_view relative) {
    if (path_rule_matches(pattern, relative)) {
        return true;
    }
    for (std::size_t slash = relative.find('/'); slash != std::string_view::npos;
         slash = relative.find('/', slash + 1)) {
        if (glob_matches(pattern, relative.substr(0, slash))) {
            return true;
        }
    }
    return false;
}

std::filesystem::path lexical_absolute(const std::filesystem::path& root,
                                       const std::filesystem::path& path) {
    return (path.is_absolute() ? path : root / path).lexically_normal();
}

std::string relative_text(const std::filesystem::path& absolute,
                          const std::filesystem::path& base) {
    auto absolute_text = absolute.generic_string();
    auto base_text = base.generic_string();
    if (absolute_text == base_text) {
        return {};
    }
    if (!base_text.empty() && base_text.back() != '/') {
        base_text.push_back('/');
    }
    if (absolute_text.rfind(base_text, 0) != 0) {
        return {};
    }
    return absolute_text.substr(base_text.size());
}

std::string unescape_gitignore_pattern(std::string pattern) {
    std::string result;
    result.reserve(pattern.size());
    bool escaped = false;
    for (const char ch : pattern) {
        if (escaped) {
            result.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        result.push_back(ch);
    }
    if (escaped) {
        result.push_back('\\');
    }
    return result;
}

bool is_comment_or_empty(const std::string& line) {
    return line.empty() || line.front() == '#';
}

}  // namespace

GitIgnoreMatcher::GitIgnoreMatcher(std::filesystem::path root)
    : root_(std::move(root)) {}

GitIgnoreMatcher GitIgnoreMatcher::load(const std::filesystem::path& root) {
    const auto canonical_root = std::filesystem::weakly_canonical(root);
    static std::map<std::string, GitIgnoreMatcher> cache;
    const auto cache_key = canonical_root.string();
    const auto cached = cache.find(cache_key);
    if (cached != cache.end()) {
        return cached->second;
    }

    GitIgnoreMatcher matcher(canonical_root);

    const auto root_gitignore = matcher.root_ / ".gitignore";
    if (std::filesystem::is_regular_file(root_gitignore)) {
        matcher.load_file(root_gitignore);
    }

    std::error_code ec;
    const std::filesystem::recursive_directory_iterator end;
    for (std::filesystem::recursive_directory_iterator it(
             matcher.root_, std::filesystem::directory_options::skip_permission_denied, ec);
        !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec)) {
            if (should_skip_scan_directory(it->path())) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (it->is_regular_file(ec) && it->path().filename() == ".gitignore" &&
            it->path() != root_gitignore) {
            matcher.load_file(it->path());
        }
    }

    const auto [inserted, _] = cache.emplace(cache_key, matcher);
    return inserted->second;
}

void GitIgnoreMatcher::load_file(const std::filesystem::path& gitignore) {
    std::ifstream input(gitignore);
    if (!input) {
        return;
    }

    const auto base = gitignore.parent_path().lexically_normal();
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        line = trim_ascii(std::move(line));
        if (is_comment_or_empty(line)) {
            continue;
        }

        Rule rule;
        rule.base = base;
        if (line.front() == '!') {
            rule.negated = true;
            line.erase(line.begin());
        }
        if (line.empty()) {
            continue;
        }
        if (line.back() == '/') {
            rule.directory_only = true;
            line.pop_back();
        }
        if (!line.empty() && line.front() == '/') {
            line.erase(line.begin());
        }

        rule.basename_only = line.find('/') == std::string::npos;
        line = unescape_gitignore_pattern(std::move(line));
        if (line.empty()) {
            continue;
        }
        rule.pattern = std::move(line);
        rules_by_base_[rule.base.string()].push_back(std::move(rule));
    }
}

bool GitIgnoreMatcher::ignores(const std::filesystem::path& path) const {
    std::error_code ec;
    const bool is_directory = std::filesystem::is_directory(path, ec);
    return ignores(path, is_directory);
}

bool GitIgnoreMatcher::ignores(const std::filesystem::path& path, bool) const {
    const auto absolute = lexical_absolute(root_, path);
    bool ignored = false;

    std::vector<std::filesystem::path> rule_bases;
    for (auto dir = absolute; !dir.empty() && path_has_prefix(dir, root_); dir = dir.parent_path()) {
        rule_bases.push_back(dir);
        if (dir == root_) {
            break;
        }
    }
    std::reverse(rule_bases.begin(), rule_bases.end());

    for (const auto& base : rule_bases) {
        const auto rules = rules_by_base_.find(base.string());
        if (rules == rules_by_base_.end()) {
            continue;
        }

        for (const auto& rule : rules->second) {
            const auto text = relative_text(absolute, rule.base);
            if (text.empty()) {
                continue;
            }
            const bool matches = rule.directory_only
                                 ? directory_rule_matches(rule.pattern, text)
                                     : (rule.basename_only
                                            ? basename_rule_matches(rule.pattern, text)
                                            : path_rule_matches(rule.pattern, text));
            if (matches) {
                ignored = !rule.negated;
            }
        }
    }

    return ignored;
}

}  // namespace qt::index
