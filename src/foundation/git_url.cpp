#include "qt/foundation/git_url.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace qt::foundation {
namespace {

std::string trim(std::string_view text) {
    std::size_t start = 0;
    while (start < text.size() &&
           std::isspace(static_cast<unsigned char>(text[start])) != 0) {
        ++start;
    }
    std::size_t end = text.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return std::string(text.substr(start, end - start));
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() &&
           text.substr(0, prefix.size()) == prefix;
}

bool is_protocol_url(std::string_view url) {
    return starts_with(url, "https://") || starts_with(url, "http://") ||
           starts_with(url, "ssh://") || starts_with(url, "git://");
}

struct SplitRefResult {
    std::string repo;
    std::optional<std::string> ref;
};

SplitRefResult split_ref(std::string_view url) {
    auto find_ref_sep = [](std::string_view text) -> std::optional<std::size_t> {
        // Find the LAST @ that could be a ref separator
        // Skip @ in protocol://user@host parts
        auto at = text.rfind('@');
        if (at == std::string_view::npos) return std::nullopt;
        // Make sure @ is after a / (so it's in the path, not user@host)
        auto slash_after = text.rfind('/', at);
        if (slash_after == std::string_view::npos) return std::nullopt;
        return at;
    };

    // Handle git@host:path@ref form
    if (starts_with(url, "git@")) {
        auto colon = url.find(':');
        if (colon == std::string_view::npos) return {std::string(url), std::nullopt};
        auto path_part = url.substr(colon + 1);
        auto at = find_ref_sep(path_part);
        if (!at) return {std::string(url), std::nullopt};
        auto repo_path = path_part.substr(0, *at);
        auto ref = path_part.substr(*at + 1);
        if (repo_path.empty() || ref.empty()) return {std::string(url), std::nullopt};
        return {std::string(url.substr(0, colon + 1)) + std::string(repo_path),
                std::string(ref)};
    }

    // Handle protocol:// URLs
    if (is_protocol_url(url)) {
        // Skip protocol:// to find path
        auto protocol_end = url.find("://");
        auto path_start = url.find('/', protocol_end + 3);
        if (path_start == std::string_view::npos) return {std::string(url), std::nullopt};
        auto path = url.substr(path_start + 1);
        auto at = find_ref_sep(path);
        if (!at) return {std::string(url), std::nullopt};
        auto repo_path = path.substr(0, *at);
        auto ref = path.substr(*at + 1);
        if (repo_path.empty() || ref.empty()) return {std::string(url), std::nullopt};
        return {std::string(url.substr(0, path_start + 1)) + std::string(repo_path),
                std::string(ref)};
    }

    // Handle host/path@ref shorthand
    auto slash = url.find('/');
    if (slash == std::string_view::npos) return {std::string(url), std::nullopt};
    auto host = url.substr(0, slash);
    auto path_with_maybe_ref = url.substr(slash + 1);
    auto at = find_ref_sep(path_with_maybe_ref);
    if (!at) return {std::string(url), std::nullopt};
    auto repo_path = path_with_maybe_ref.substr(0, *at);
    auto ref = path_with_maybe_ref.substr(*at + 1);
    if (repo_path.empty() || ref.empty()) return {std::string(url), std::nullopt};
    return {std::string(host) + "/" + std::string(repo_path), std::string(ref)};
}

std::string strip_dot_git(std::string path) {
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".git") {
        return path.substr(0, path.size() - 4);
    }
    return path;
}

std::string strip_trailing_slash(std::string path) {
    while (!path.empty() && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

bool looks_like_host(std::string_view host) {
    return host.find('.') != std::string_view::npos || host == "localhost";
}

}  // namespace

std::optional<GitSource> parse_git_url(std::string_view source) {
    auto trimmed = trim(source);
    if (trimmed.empty()) return std::nullopt;

    bool has_git_prefix = starts_with(trimmed, "git:");
    std::string url = has_git_prefix ? trim(trimmed.substr(4)) : trimmed;

    // Without git: prefix, only accept explicit protocol URLs
    if (!has_git_prefix && !is_protocol_url(url) && !starts_with(url, "git@")) {
        return std::nullopt;
    }

    auto split = split_ref(url);
    auto repo = std::move(split.repo);
    auto ref = std::move(split.ref);

    std::string host;
    std::string path;

    // Parse host and path
    if (starts_with(repo, "git@")) {
        auto colon = repo.find(':');
        if (colon == std::string::npos) return std::nullopt;
        host = strip_trailing_slash(repo.substr(4, colon - 4));
        path = strip_dot_git(strip_trailing_slash(repo.substr(colon + 1)));
    } else if (is_protocol_url(repo)) {
        auto protocol_end = repo.find("://");
        if (protocol_end == std::string::npos) return std::nullopt;
        auto host_start = protocol_end + 3;
        auto slash_after = repo.find('/', host_start);
        if (slash_after == std::string::npos) return std::nullopt;
        auto host_port = repo.substr(host_start, slash_after - host_start);
        // Strip user info (git@host -> host)
        auto at_pos = host_port.find('@');
        if (at_pos != std::string::npos) {
            host = std::string(host_port.substr(at_pos + 1));
        } else {
            host = std::string(host_port);
        }
        path = strip_dot_git(strip_trailing_slash(repo.substr(slash_after + 1)));
    } else {
        auto slash = repo.find('/');
        if (slash == std::string::npos) return std::nullopt;
        host = repo.substr(0, slash);
        if (!looks_like_host(host)) return std::nullopt;
        path = strip_dot_git(strip_trailing_slash(repo.substr(slash + 1)));
        repo = "https://" + repo;
    }

    if (host.empty() || path.empty()) return std::nullopt;

    // Validate path has at least user/repo
    auto path_parts = [](std::string_view p) -> std::size_t {
        std::size_t count = 0;
        for (char c : p) {
            if (c == '/') ++count;
        }
        return count + 1;
    }(path);
    if (path_parts < 2) return std::nullopt;

    GitSource result;
    result.type = "git";
    result.repo = std::move(repo);
    result.host = std::move(host);
    result.path = std::move(path);
    result.ref = ref;
    result.pinned = ref.has_value();
    return result;
}

}  // namespace qt::foundation
