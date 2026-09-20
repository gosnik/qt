#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace qt::foundation {

struct GitSource {
    std::string type = "git";
    std::string repo;
    std::string host;
    std::string path;
    std::optional<std::string> ref;
    bool pinned = false;
};

/// Parse a git URL (HTTPS, SSH, git@, or shorthand) into a GitSource.
/// Supports:
/// - git: prefix with all historical shorthand forms
/// - https://, http://, ssh://, git:// protocol URLs
/// - git@host:path SSH URLs
/// - @ref suffixes for branch/tag/commit pinning
/// - .git suffix stripping
[[nodiscard]] std::optional<GitSource> parse_git_url(std::string_view source);

}  // namespace qt::foundation
