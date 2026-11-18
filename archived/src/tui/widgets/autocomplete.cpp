#include "qt/tui/widgets/autocomplete.hpp"

#include "qt/foundation/process.hpp"
#include "qt/tui/utils/fuzzy.hpp"

#include <uni_algo/case.h>
#include <uni_algo/norm.h>
#include <uni_algo/ranges_grapheme.h>

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace qt::tui {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view path_delimiters = " \t\"'=";

bool is_delimiter(char ch) {
    return path_delimiters.find(ch) != std::string_view::npos;
}

bool is_ascii(std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](const char ch) {
        return static_cast<unsigned char>(ch) < 0x80U;
    });
}

std::string lowercase_ascii(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (const char ch : value) {
        lowered.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return lowered;
}

std::string normalize_for_match(std::string_view value) {
    if (is_ascii(value)) {
        return lowercase_ascii(value);
    }
    const std::string compatible = una::norm::to_nfkc_utf8(value);
    const std::string folded = una::cases::to_casefold_utf8(compatible);
    return una::norm::to_nfkc_utf8(folded);
}

std::vector<std::string> normalized_graphemes(std::string_view value) {
    std::vector<std::string> units;
    const std::string normalized = normalize_for_match(value);
    for (const std::string_view unit :
         una::ranges::grapheme::utf8_view<std::string_view>{normalized}) {
        units.emplace_back(unit);
    }
    return units;
}

std::string to_display_path(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}

std::size_t clamped_cursor(std::string_view line, std::size_t cursor_column) {
    return std::min(cursor_column, line.size());
}

std::optional<std::size_t> find_unclosed_quote_start(std::string_view text) {
    bool in_quotes = false;
    std::size_t quote_start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '"') {
            in_quotes = !in_quotes;
            if (in_quotes) {
                quote_start = i;
            }
        }
    }
    if (!in_quotes) {
        return std::nullopt;
    }
    return quote_start;
}

bool is_token_start(std::string_view text, std::size_t index) {
    return index == 0 || is_delimiter(text[index - 1]);
}

std::optional<std::string> extract_quoted_prefix(std::string_view text) {
    const auto quote_start = find_unclosed_quote_start(text);
    if (!quote_start) {
        return std::nullopt;
    }

    if (*quote_start > 0 && text[*quote_start - 1] == '@') {
        if (!is_token_start(text, *quote_start - 1)) {
            return std::nullopt;
        }
        return std::string(text.substr(*quote_start - 1));
    }

    if (!is_token_start(text, *quote_start)) {
        return std::nullopt;
    }
    return std::string(text.substr(*quote_start));
}

std::size_t find_last_delimiter(std::string_view text) {
    for (std::size_t i = text.size(); i > 0; --i) {
        if (is_delimiter(text[i - 1])) {
            return i - 1;
        }
    }
    return std::string::npos;
}

struct ParsedPathPrefix {
    std::string raw_prefix;
    bool at_prefix = false;
    bool quoted_prefix = false;
};

std::optional<std::string_view> slash_command_prefix(std::string_view text,
                                                     std::size_t cursor_line) {
    if (cursor_line != 0U) {
        return std::nullopt;
    }

    const auto slash_offset = text.find_first_not_of(" \t");
    if (slash_offset == std::string_view::npos || text[slash_offset] != '/') {
        return std::nullopt;
    }
    return text.substr(slash_offset);
}

std::string autocomplete_match_prefix(std::string_view prefix) {
    const auto first = prefix.find_first_not_of(" \t");
    if (first != std::string_view::npos) {
        prefix.remove_prefix(first);
    }
    if (!prefix.empty() && prefix.front() == '/') {
        prefix.remove_prefix(1);
    }
    return std::string(prefix);
}

std::optional<std::size_t> best_autocomplete_match_index(
    const std::vector<AutocompleteItem>& items, std::string_view prefix) {
    const std::string match_prefix = autocomplete_match_prefix(prefix);
    if (match_prefix.empty()) {
        return std::nullopt;
    }

    std::optional<std::size_t> first_prefix_match;
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (items[index].value == match_prefix) {
            return index;
        }
        if (!first_prefix_match &&
            items[index].value.rfind(match_prefix, 0) == 0) {
            first_prefix_match = index;
        }
    }
    return first_prefix_match;
}

ParsedPathPrefix parse_path_prefix(std::string_view prefix) {
    if (prefix.rfind("@\"", 0) == 0) {
        return {std::string(prefix.substr(2)), true, true};
    }
    if (prefix.rfind("\"", 0) == 0) {
        return {std::string(prefix.substr(1)), false, true};
    }
    if (prefix.rfind("@", 0) == 0) {
        return {std::string(prefix.substr(1)), true, false};
    }
    return {std::string(prefix), false, false};
}

std::string build_completion_value(std::string path, bool at_prefix, bool quoted_prefix) {
    const bool needs_quotes = quoted_prefix || path.find(' ') != std::string::npos;
    if (!needs_quotes) {
        return (at_prefix ? "@" : "") + path;
    }
    return (at_prefix ? "@\"" : "\"") + path + "\"";
}

bool starts_with_normalized_prefix(
    std::string_view value, const std::vector<std::string>& normalized_prefix_units) {
    const auto value_units = normalized_graphemes(value);
    if (normalized_prefix_units.size() > value_units.size()) {
        return false;
    }
    return std::equal(normalized_prefix_units.begin(), normalized_prefix_units.end(),
                      value_units.begin());
}

std::string escape_regex(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
            case '.':
            case '*':
            case '+':
            case '?':
            case '^':
            case '$':
            case '{':
            case '}':
            case '(':
            case ')':
            case '|':
            case '[':
            case ']':
            case '\\':
                escaped.push_back('\\');
                break;
            default:
                break;
        }
        escaped.push_back(ch);
    }
    return escaped;
}

std::string build_fd_path_query(std::string query) {
    query = to_display_path(std::move(query));
    if (query.find('/') == std::string::npos) {
        return query;
    }

    const bool trailing_separator = !query.empty() && query.back() == '/';
    while (!query.empty() && query.front() == '/') {
        query.erase(query.begin());
    }
    while (!query.empty() && query.back() == '/') {
        query.pop_back();
    }
    if (query.empty()) {
        return trailing_separator ? "/" : query;
    }

    std::string pattern;
    std::stringstream stream(query);
    std::string segment;
    while (std::getline(stream, segment, '/')) {
        if (segment.empty()) {
            continue;
        }
        if (!pattern.empty()) {
            pattern += "[\\\\/]";
        }
        pattern += escape_regex(segment);
    }
    if (trailing_separator && !pattern.empty()) {
        pattern += "[\\\\/]";
    }
    return pattern;
}

bool path_is_directory(const fs::directory_entry& entry) {
    std::error_code ec;
    if (entry.is_directory(ec)) {
        return true;
    }
    ec.clear();
    return entry.is_symlink(ec) && fs::is_directory(entry.path(), ec);
}

fs::path resolve_search_path(const fs::path& base_path, const std::string& raw_path) {
    if (raw_path.empty()) {
        return base_path;
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr && raw_path == "~") {
        return fs::path(home);
    }
    if (home != nullptr && raw_path.rfind("~/", 0) == 0) {
        return fs::path(home) / raw_path.substr(2);
    }
    fs::path path(raw_path);
    if (path.is_absolute()) {
        return path;
    }
    return base_path / path;
}

std::string dirname_display(const std::string& display_prefix) {
    auto slash = display_prefix.find_last_of("/\\");
    if (slash == std::string::npos) {
        return ".";
    }
    if (slash == 0 && display_prefix[0] == '/') {
        return "/";
    }
    return display_prefix.substr(0, slash);
}

std::string join_display(const std::string& left, const std::string& right) {
    if (left.empty() || left == ".") {
        return right;
    }
    if (left == "/") {
        return "/" + right;
    }
    if (left.back() == '/') {
        return left + right;
    }
    return left + "/" + right;
}

std::string relative_path_for_entry(const std::string& raw_prefix, const std::string& name) {
    const std::string display_prefix = to_display_path(raw_prefix);
    if (!display_prefix.empty() && display_prefix.back() == '/') {
        return display_prefix + name;
    }
    if (display_prefix.find('/') != std::string::npos ||
        display_prefix.find('\\') != std::string::npos) {
        auto relative = join_display(dirname_display(display_prefix), name);
        if (display_prefix.rfind("./", 0) == 0 && relative.rfind("./", 0) != 0) {
            relative = "./" + relative;
        }
        return relative;
    }
    if (display_prefix.rfind("~", 0) == 0) {
        return "~/" + name;
    }
    return name;
}

bool is_git_path(const fs::path& relative) {
    for (const auto& part : relative) {
        if (part == ".git") {
            return true;
        }
    }
    return false;
}

struct PreparedFileQuery {
    std::string raw;
    std::string normalized;
    PreparedFuzzyQuery fuzzy;
};

PreparedFileQuery prepare_file_query(std::string query) {
    PreparedFileQuery prepared;
    prepared.raw = std::move(query);
    prepared.normalized = normalize_for_match(prepared.raw);
    prepared.fuzzy = prepare_fuzzy_query(prepared.raw);
    return prepared;
}

struct PreparedFileEntry {
    std::string filename;
    std::string normalized_filename;
    PreparedFuzzyText path_text;
};

PreparedFileEntry prepare_file_entry(std::string_view path) {
    const auto filename = fs::path(path).filename().string();
    return PreparedFileEntry{
        filename, normalize_for_match(filename), prepare_fuzzy_text(path)};
}

int score_file_entry(const PreparedFileEntry& entry, const PreparedFileQuery& query,
                     bool is_directory) {
    if (query.raw.empty()) {
        return is_directory ? 11 : 1;
    }

    int score = 0;
    if (entry.normalized_filename == query.normalized) {
        score = 100;
    } else if (entry.normalized_filename.rfind(query.normalized, 0) == 0) {
        score = 80;
    } else if (entry.normalized_filename.find(query.normalized) != std::string::npos) {
        score = 50;
    } else if (entry.path_text.normalized.find(query.normalized) != std::string::npos) {
        score = 30;
    } else {
        const auto match = fuzzy_match_prepared(query.fuzzy, entry.path_text);
        if (match.matches) {
            score = std::max(1, 25 - static_cast<int>(match.score));
        }
    }

    if (is_directory && score > 0) {
        score += 10;
    }
    return score;
}

std::vector<std::string> split_stdout_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        std::string line(text.substr(start, end == std::string_view::npos
                                                ? std::string_view::npos
                                                : end - start));
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            lines.push_back(std::move(line));
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1U;
    }
    return lines;
}

}  // namespace

void AutocompleteState::set_suggestions(AutocompleteSuggestions suggestions) {
    if (suggestions.items.empty()) {
        clear();
        return;
    }

    const std::string previous_prefix =
        suggestions_ ? suggestions_->prefix : std::string{};
    const std::string previous_value =
        selected_item() != nullptr ? selected_item()->value : std::string{};
    const std::size_t previous_index = selected_index_;
    std::size_t selected_index = 0;

    if (suggestions_ && previous_prefix == suggestions.prefix &&
        previous_index < suggestions.items.size() &&
        suggestions.items[previous_index].value == previous_value) {
        selected_index = previous_index;
    } else if (const auto best =
                   best_autocomplete_match_index(suggestions.items, suggestions.prefix)) {
        selected_index = *best;
    }

    suggestions_ = std::move(suggestions);
    selected_index_ = selected_index;
}

void AutocompleteState::clear() noexcept {
    suggestions_.reset();
    selected_index_ = 0;
}

void AutocompleteState::select_next() noexcept {
    if (!suggestions_ || suggestions_->items.empty()) {
        return;
    }
    selected_index_ = (selected_index_ + 1) % suggestions_->items.size();
}

void AutocompleteState::select_previous() noexcept {
    if (!suggestions_ || suggestions_->items.empty()) {
        return;
    }
    selected_index_ =
        selected_index_ == 0 ? suggestions_->items.size() - 1 : selected_index_ - 1;
}

void AutocompleteState::set_selected_index(std::size_t index) noexcept {
    if (!suggestions_ || suggestions_->items.empty()) {
        selected_index_ = 0;
        return;
    }
    selected_index_ = std::min(index, suggestions_->items.size() - 1U);
}

bool AutocompleteState::active() const noexcept {
    return suggestions_.has_value() && !suggestions_->items.empty();
}

const std::optional<AutocompleteSuggestions>& AutocompleteState::suggestions() const noexcept {
    return suggestions_;
}

std::size_t AutocompleteState::selected_index() const noexcept { return selected_index_; }

const AutocompleteItem* AutocompleteState::selected_item() const noexcept {
    if (!active() || selected_index_ >= suggestions_->items.size()) {
        return nullptr;
    }
    return &suggestions_->items[selected_index_];
}

struct CombinedAutocompleteProvider::AsyncCompletionState {
    struct Job {
        std::uint64_t generation = 0;
        std::string prefix;
        std::string query;
        bool quoted = false;
        std::shared_ptr<foundation::ProcessCancellationToken> cancelled;
    };
    struct Result {
        std::uint64_t generation = 0;
        std::string prefix;
        std::vector<AutocompleteItem> items;
    };

    AsyncCompletionState(fs::path base_path, std::size_t max_suggestions,
                         std::optional<std::string> fd_path)
        : base_path(std::move(base_path)), max_suggestions(max_suggestions),
          fd_path(std::move(fd_path)), worker([this] { run(); }) {}

    ~AsyncCompletionState() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            if (active_cancelled) active_cancelled->cancel();
            if (queued) queued->cancelled->cancel();
        }
        wake.notify_one();
        if (worker.joinable()) worker.join();
    }

    void request(Job job) {
        std::lock_guard<std::mutex> lock(mutex);
        if (active_cancelled) active_cancelled->cancel();
        if (queued) queued->cancelled->cancel();
        queued = std::move(job);
        wake.notify_one();
    }

    std::optional<Result> take(std::uint64_t generation) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!completed || completed->generation != generation) return std::nullopt;
        auto result = std::move(completed);
        completed.reset();
        return std::move(*result);
    }

    bool pending() const {
        std::lock_guard<std::mutex> lock(mutex);
        return queued.has_value() || active_cancelled != nullptr;
    }

    void cancel() {
        std::lock_guard<std::mutex> lock(mutex);
        if (active_cancelled) active_cancelled->cancel();
        if (queued) queued->cancelled->cancel();
        queued.reset();
        completed.reset();
    }

  private:
    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this] { return stopping || queued.has_value(); });
                if (stopping && !queued) return;
                job = std::move(*queued);
                queued.reset();
                active_cancelled = job.cancelled;
            }
            CombinedAutocompleteProvider scanner({}, base_path, max_suggestions, fd_path, false);
            auto items = scanner.get_recursive_at_suggestions(
                job.query, job.quoted, job.cancelled);
            {
                std::lock_guard<std::mutex> lock(mutex);
                active_cancelled.reset();
                if (!stopping && !job.cancelled->is_cancelled()) {
                    completed = Result{job.generation, std::move(job.prefix), std::move(items)};
                }
            }
        }
    }

    fs::path base_path;
    std::size_t max_suggestions;
    std::optional<std::string> fd_path;
    mutable std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    std::optional<Job> queued;
    std::shared_ptr<foundation::ProcessCancellationToken> active_cancelled;
    std::optional<Result> completed;
    std::thread worker;
};

CombinedAutocompleteProvider::CombinedAutocompleteProvider(
    std::vector<SlashCommandCompletion> commands, fs::path base_path,
    std::size_t max_file_suggestions, std::optional<std::string> fd_path)
    : CombinedAutocompleteProvider(std::move(commands), std::move(base_path),
                                   max_file_suggestions, std::move(fd_path), true) {}

CombinedAutocompleteProvider::CombinedAutocompleteProvider(
    std::vector<SlashCommandCompletion> commands, fs::path base_path,
    std::size_t max_file_suggestions, std::optional<std::string> fd_path,
    bool start_async_worker)
    : commands_(std::move(commands)),
      base_path_(std::move(base_path)),
      max_file_suggestions_(max_file_suggestions),
      fd_path_(std::move(fd_path)) {
    if (start_async_worker) {
        async_completion_state_ = std::make_shared<AsyncCompletionState>(
            base_path_, max_file_suggestions_, fd_path_);
    }
}

bool CombinedAutocompleteProvider::request_recursive_at_suggestions(
    const std::vector<std::string>& lines, std::size_t cursor_line,
    std::size_t cursor_column, std::uint64_t generation) const {
    if (!async_completion_state_ || cursor_line >= lines.size()) return false;
    const auto cursor = clamped_cursor(lines[cursor_line], cursor_column);
    const auto prefix = extract_at_prefix(std::string_view(lines[cursor_line]).substr(0, cursor));
    if (!prefix) return false;
    const auto parsed = parse_path_prefix(*prefix);
    AsyncCompletionState::Job job;
    job.generation = generation;
    job.prefix = *prefix;
    job.query = parsed.raw_prefix;
    job.quoted = parsed.quoted_prefix;
    job.cancelled = std::make_shared<foundation::ProcessCancellationToken>();
    async_completion_state_->request(std::move(job));
    return true;
}

std::optional<AutocompleteSuggestions>
CombinedAutocompleteProvider::take_recursive_at_suggestions(
    std::uint64_t generation) const {
    if (!async_completion_state_) return std::nullopt;
    auto result = async_completion_state_->take(generation);
    if (!result || result->items.empty()) return std::nullopt;
    return AutocompleteSuggestions{std::move(result->items), std::move(result->prefix)};
}

bool CombinedAutocompleteProvider::recursive_at_completion_pending() const {
    return async_completion_state_ && async_completion_state_->pending();
}

void CombinedAutocompleteProvider::cancel_recursive_at_completion() const {
    if (async_completion_state_) async_completion_state_->cancel();
}

std::optional<AutocompleteSuggestions> CombinedAutocompleteProvider::get_suggestions(
    const std::vector<std::string>& lines, std::size_t cursor_line,
    std::size_t cursor_column, bool force) const {
    const std::string current_line = cursor_line < lines.size() ? lines[cursor_line] : "";
    const std::size_t cursor = clamped_cursor(current_line, cursor_column);
    const std::string text_before_cursor = current_line.substr(0, cursor);

    if (const auto at_prefix = extract_at_prefix(text_before_cursor)) {
        const auto parsed = parse_path_prefix(*at_prefix);
        auto suggestions = get_recursive_at_suggestions(parsed.raw_prefix, parsed.quoted_prefix);
        if (suggestions.empty()) {
            return std::nullopt;
        }
        return AutocompleteSuggestions{std::move(suggestions), *at_prefix};
    }

    if (const auto slash_prefix = slash_command_prefix(text_before_cursor, cursor_line)) {
        const auto space_index = slash_prefix->find_first_of(" \t");
        if (space_index == std::string::npos) {
            const std::string prefix = std::string(slash_prefix->substr(1));
            std::vector<AutocompleteItem> command_items;
            command_items.reserve(commands_.size());
            for (const auto& command : commands_) {
                std::string description = command.description.value_or("");
                if (command.argument_hint) {
                    description = description.empty()
                                      ? *command.argument_hint
                                      : *command.argument_hint + " - " + description;
                }
                command_items.push_back(
                    AutocompleteItem{command.name, command.name,
                                     description.empty()
                                         ? std::optional<std::string>{}
                                         : std::optional<std::string>{description}});
            }
            std::vector<AutocompleteItem> filtered;
            if (prefix.empty()) {
                filtered = std::move(command_items);
            } else {
                const std::string lower_prefix = lowercase_ascii(prefix);
                for (const auto& item : command_items) {
                    if (lowercase_ascii(item.value).find(lower_prefix) !=
                        std::string::npos) {
                        filtered.push_back(item);
                    }
                }
                if (filtered.empty()) {
                    filtered = fuzzy_filter(
                        command_items, prefix,
                        [](const AutocompleteItem& item) { return item.value; });
                }
            }
            if (filtered.empty()) {
                return std::nullopt;
            }
            return AutocompleteSuggestions{std::move(filtered), std::string(*slash_prefix)};
        }

        const std::string command_name = std::string(slash_prefix->substr(1, space_index - 1));
        const std::string argument_text = std::string(slash_prefix->substr(space_index + 1));
        const auto command = std::find_if(commands_.begin(), commands_.end(),
                                          [&](const auto& candidate) {
                                              return candidate.name == command_name;
                                          });
        if (command == commands_.end() || !command->argument_completions) {
            return std::nullopt;
        }

        auto suggestions = command->argument_completions(argument_text);
        if (suggestions.empty()) {
            return std::nullopt;
        }
        return AutocompleteSuggestions{std::move(suggestions), argument_text};
    }

    const auto path_prefix = extract_path_prefix(text_before_cursor, force);
    if (!path_prefix) {
        return std::nullopt;
    }

    auto suggestions = get_file_suggestions(*path_prefix);
    if (suggestions.empty()) {
        return std::nullopt;
    }
    return AutocompleteSuggestions{std::move(suggestions), *path_prefix};
}

CompletionApplication CombinedAutocompleteProvider::apply_completion(
    const std::vector<std::string>& lines, std::size_t cursor_line,
    std::size_t cursor_column, const AutocompleteItem& item, const std::string& prefix) const {
    std::vector<std::string> new_lines = lines;
    if (new_lines.empty()) {
        new_lines.emplace_back();
    }
    if (cursor_line >= new_lines.size()) {
        cursor_line = new_lines.size() - 1;
    }

    const std::string current_line = new_lines[cursor_line];
    const std::size_t cursor = clamped_cursor(current_line, cursor_column);
    const std::size_t prefix_start = prefix.size() > cursor ? 0 : cursor - prefix.size();
    const std::string before_prefix = current_line.substr(0, prefix_start);
    const std::string after_cursor = current_line.substr(cursor);
    const bool quoted_prefix = prefix.rfind("\"", 0) == 0 || prefix.rfind("@\"", 0) == 0;
    const bool drop_existing_quote =
        quoted_prefix && !item.value.empty() && item.value.back() == '"' &&
        !after_cursor.empty() && after_cursor.front() == '"';
    const std::string adjusted_after =
        drop_existing_quote ? after_cursor.substr(1) : after_cursor;

    const bool slash_command = prefix.rfind("/", 0) == 0 && before_prefix.find_first_not_of(" \t") == std::string::npos &&
                               prefix.substr(1).find('/') == std::string::npos;
    if (slash_command) {
        new_lines[cursor_line] = before_prefix + "/" + item.value + " " + adjusted_after;
        return {std::move(new_lines), cursor_line, before_prefix.size() + item.value.size() + 2};
    }

    const bool attachment = prefix.rfind("@", 0) == 0;
    const bool directory = !item.label.empty() && item.label.back() == '/';
    const std::string suffix = attachment && !directory ? " " : "";
    new_lines[cursor_line] = before_prefix + item.value + suffix + adjusted_after;

    std::size_t cursor_offset = item.value.size();
    if (directory && !item.value.empty() && item.value.back() == '"') {
        --cursor_offset;
    }

    return {std::move(new_lines), cursor_line, before_prefix.size() + cursor_offset + suffix.size()};
}

bool CombinedAutocompleteProvider::should_trigger_file_completion(
    const std::vector<std::string>& lines, std::size_t cursor_line,
    std::size_t cursor_column) const {
    const std::string current_line = cursor_line < lines.size() ? lines[cursor_line] : "";
    const std::size_t cursor = clamped_cursor(current_line, cursor_column);
    const std::string text_before_cursor = current_line.substr(0, cursor);
    const auto first_non_space = text_before_cursor.find_first_not_of(" \t");
    if (cursor_line == 0U && first_non_space != std::string::npos &&
        text_before_cursor[first_non_space] == '/' &&
        text_before_cursor.find_first_of(" \t", first_non_space) == std::string::npos) {
        return false;
    }
    return true;
}

std::optional<std::string> CombinedAutocompleteProvider::extract_at_prefix(
    std::string_view text) const {
    const auto quoted = extract_quoted_prefix(text);
    if (quoted && quoted->rfind("@\"", 0) == 0) {
        return quoted;
    }

    const auto delimiter = find_last_delimiter(text);
    const std::size_t token_start = delimiter == std::string::npos ? 0 : delimiter + 1;
    if (token_start < text.size() && text[token_start] == '@') {
        return std::string(text.substr(token_start));
    }
    return std::nullopt;
}

std::optional<std::string> CombinedAutocompleteProvider::extract_path_prefix(
    std::string_view text, bool force) const {
    if (const auto quoted = extract_quoted_prefix(text)) {
        return quoted;
    }

    const auto delimiter = find_last_delimiter(text);
    const std::string path_prefix =
        delimiter == std::string::npos ? std::string(text) : std::string(text.substr(delimiter + 1));

    if (force) {
        return path_prefix;
    }
    if (path_prefix.find('/') != std::string::npos || path_prefix.rfind(".", 0) == 0 ||
        path_prefix.rfind("~/", 0) == 0) {
        return path_prefix;
    }
    return std::nullopt;
}

std::vector<AutocompleteItem> CombinedAutocompleteProvider::get_file_suggestions(
    const std::string& prefix) const {
    const auto parsed = parse_path_prefix(prefix);
    std::string raw_prefix = to_display_path(parsed.raw_prefix);

    std::string search_dir_display;
    std::string search_prefix;
    if (raw_prefix.empty() || raw_prefix == "./" || raw_prefix == "../" ||
        raw_prefix == "~" || raw_prefix == "~/" || raw_prefix == "/") {
        search_dir_display = raw_prefix;
        search_prefix.clear();
    } else if (!raw_prefix.empty() && raw_prefix.back() == '/') {
        search_dir_display = raw_prefix;
        search_prefix.clear();
    } else {
        search_dir_display = dirname_display(raw_prefix);
        search_prefix = fs::path(raw_prefix).filename().string();
    }

    const fs::path search_dir = resolve_search_path(base_path_, search_dir_display);
    std::error_code ec;
    if (!fs::is_directory(search_dir, ec)) {
        return {};
    }

    const auto normalized_search_prefix_units = normalized_graphemes(search_prefix);
    std::vector<AutocompleteItem> suggestions;
    for (const auto& entry : fs::directory_iterator(search_dir, ec)) {
        if (ec) {
            return {};
        }
        const std::string name = entry.path().filename().string();
        if (!starts_with_normalized_prefix(name, normalized_search_prefix_units)) {
            continue;
        }

        const bool directory = path_is_directory(entry);
        std::string relative_path = relative_path_for_entry(raw_prefix, name);
        relative_path = to_display_path(relative_path);
        const std::string path_value = directory ? relative_path + "/" : relative_path;
        suggestions.push_back(AutocompleteItem{
            build_completion_value(path_value, parsed.at_prefix, parsed.quoted_prefix),
            name + (directory ? "/" : ""), std::nullopt});
    }

    struct SortableSuggestion {
        AutocompleteItem item;
        std::string normalized_label;
        bool directory = false;
    };
    std::vector<SortableSuggestion> sortable;
    sortable.reserve(suggestions.size());
    for (auto& suggestion : suggestions) {
        const bool directory =
            !suggestion.label.empty() && suggestion.label.back() == '/';
        sortable.push_back(SortableSuggestion{
            std::move(suggestion), normalize_for_match(suggestion.label), directory});
    }
    std::sort(sortable.begin(), sortable.end(), [](const auto& left, const auto& right) {
        if (left.directory != right.directory) {
            return left.directory;
        }
        return left.normalized_label < right.normalized_label;
    });
    if (sortable.size() > max_file_suggestions_) {
        sortable.resize(max_file_suggestions_);
    }
    suggestions.clear();
    suggestions.reserve(sortable.size());
    for (auto& suggestion : sortable) {
        suggestions.push_back(std::move(suggestion.item));
    }
    return suggestions;
}

std::vector<AutocompleteItem> CombinedAutocompleteProvider::get_recursive_at_suggestions(
    const std::string& prefix, bool quoted,
    const std::shared_ptr<foundation::ProcessCancellationToken>& cancelled) const {
    if (cancelled != nullptr && cancelled->is_cancelled()) {
        return {};
    }
    const std::string query = to_display_path(prefix);
    fs::path search_root = base_path_;
    std::string display_base;
    std::string query_tail = query;

    const auto slash = query.find_last_of('/');
    if (slash != std::string::npos) {
        display_base = query.substr(0, slash + 1);
        query_tail = query.substr(slash + 1);
        search_root = resolve_search_path(base_path_, display_base);
    }
    const PreparedFileQuery prepared_query = prepare_file_query(query_tail);

    std::error_code ec;
    if (!fs::is_directory(search_root, ec)) {
        return {};
    }

    struct ScoredItem {
        AutocompleteItem item;
        std::string normalized_label;
        int score = 0;
        std::size_t order = 0;
    };

    std::vector<ScoredItem> scored;
    std::size_t order = 0;
    if (fd_path_ && !fd_path_->empty()) {
        std::vector<std::string> argv = {
            *fd_path_,
            "--base-directory",
            search_root.string(),
            "--max-results",
            "100",
            "--type",
            "f",
            "--type",
            "d",
            "--follow",
            "--hidden",
            "--exclude",
            ".git",
            "--exclude",
            ".git/*",
            "--exclude",
            ".git/**",
        };
        if (query_tail.find('/') != std::string::npos) {
            argv.emplace_back("--full-path");
        }
        if (!query_tail.empty()) {
            argv.emplace_back(build_fd_path_query(query_tail));
        }

        qt::foundation::ProcessRequest request;
        request.mode = qt::foundation::ProcessLaunchMode::argv;
        request.argv = std::move(argv);
        request.timeout_ms = 500;
        request.cancellation_token = cancelled;
        const auto result = qt::foundation::run_process(request);
        if (result && !result.value().timed_out && result.value().exit_code == 0) {
            for (const auto& line : split_stdout_lines(result.value().stdout_text)) {
                std::string relative_display = to_display_path(line);
                while (!relative_display.empty() &&
                       relative_display.front() == '.' &&
                       relative_display.size() > 1U &&
                       relative_display[1] == '/') {
                    relative_display.erase(0, 2);
                }
                if (relative_display.empty() ||
                    relative_display == ".git" ||
                    relative_display.rfind(".git/", 0) == 0 ||
                    relative_display.find("/.git/") != std::string::npos) {
                    continue;
                }

                const fs::path absolute_path = search_root / fs::path(relative_display);
                const bool directory = fs::is_directory(absolute_path, ec);
                ec.clear();
                std::string display_path =
                    to_display_path((fs::path(display_base) / relative_display).string());
                if (display_base == "/" && !display_path.empty() &&
                    display_path.front() != '/') {
                    display_path = "/" + display_path;
                }
                const auto prepared_entry = prepare_file_entry(display_path);
                const int score =
                    score_file_entry(prepared_entry, prepared_query, directory);
                if (score <= 0) {
                    continue;
                }
                const std::string completion_path =
                    directory ? display_path + "/" : display_path;
                AutocompleteItem item{build_completion_value(completion_path, true, quoted),
                                      prepared_entry.filename + (directory ? "/" : ""),
                                      display_path};
                scored.push_back(ScoredItem{
                    std::move(item), prepared_entry.normalized_filename, score, order++});
            }
        }
    }

    if (!scored.empty()) {
        std::sort(scored.begin(), scored.end(), [](const auto& left, const auto& right) {
            if (left.score != right.score) {
                return left.score > right.score;
            }
            const bool left_dir = !left.item.label.empty() && left.item.label.back() == '/';
            const bool right_dir = !right.item.label.empty() && right.item.label.back() == '/';
            if (left_dir != right_dir) {
                return left_dir;
            }
            if (left.normalized_label != right.normalized_label) {
                return left.normalized_label < right.normalized_label;
            }
            return left.order < right.order;
        });

        if (scored.size() > max_file_suggestions_) {
            scored.resize(max_file_suggestions_);
        }

        std::vector<AutocompleteItem> suggestions;
        suggestions.reserve(scored.size());
        for (auto& item : scored) {
            suggestions.push_back(std::move(item.item));
        }
        return suggestions;
    }

    constexpr std::size_t max_fallback_entries = 2000U;
    std::size_t visited = 0;
    fs::recursive_directory_iterator it(search_root, fs::directory_options::follow_directory_symlink,
                                        ec);
    const fs::recursive_directory_iterator end;
    while (!ec && it != end) {
        if (cancelled != nullptr && cancelled->is_cancelled()) {
            return {};
        }
        if (++visited > max_fallback_entries) {
            break;
        }
        const auto entry = *it;
        const fs::path relative = fs::relative(entry.path(), search_root, ec);
        if (ec) {
            break;
        }
        if (is_git_path(relative)) {
            if (entry.is_directory(ec)) {
                it.disable_recursion_pending();
            }
            it.increment(ec);
            continue;
        }

        const bool directory = path_is_directory(entry);
        std::string display_path = to_display_path((fs::path(display_base) / relative).string());
        if (display_base == "/" && !display_path.empty() && display_path.front() != '/') {
            display_path = "/" + display_path;
        }
        const auto prepared_entry = prepare_file_entry(display_path);
        const int score = score_file_entry(prepared_entry, prepared_query, directory);
        if (score > 0) {
            const std::string completion_path = directory ? display_path + "/" : display_path;
            AutocompleteItem item{build_completion_value(completion_path, true, quoted),
                                  prepared_entry.filename + (directory ? "/" : ""),
                                  display_path};
            scored.push_back(
                ScoredItem{std::move(item), prepared_entry.normalized_filename, score, order++});
        }

        it.increment(ec);
    }

    std::sort(scored.begin(), scored.end(), [](const auto& left, const auto& right) {
        if (left.score != right.score) {
            return left.score > right.score;
        }
        const bool left_dir = !left.item.label.empty() && left.item.label.back() == '/';
        const bool right_dir = !right.item.label.empty() && right.item.label.back() == '/';
        if (left_dir != right_dir) {
            return left_dir;
        }
        if (left.normalized_label != right.normalized_label) {
            return left.normalized_label < right.normalized_label;
        }
        return left.order < right.order;
    });

    if (scored.size() > max_file_suggestions_) {
        scored.resize(max_file_suggestions_);
    }

    std::vector<AutocompleteItem> suggestions;
    suggestions.reserve(scored.size());
    for (auto& item : scored) {
        suggestions.push_back(std::move(item.item));
    }
    return suggestions;
}

}  // namespace qt::tui
