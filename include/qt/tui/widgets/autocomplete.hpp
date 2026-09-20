#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace qt::tui {

struct AutocompleteItem {
    std::string value;
    std::string label;
    std::optional<std::string> description;
};

struct SlashCommandCompletion {
    std::string name;
    std::optional<std::string> description;
    std::optional<std::string> argument_hint;
    std::function<std::vector<AutocompleteItem>(const std::string&)> argument_completions;
};

struct AutocompleteSuggestions {
    std::vector<AutocompleteItem> items;
    std::string prefix;
};

struct CompletionApplication {
    std::vector<std::string> lines;
    std::size_t cursor_line = 0;
    std::size_t cursor_column = 0;
};

class AutocompleteState {
  public:
    void set_suggestions(AutocompleteSuggestions suggestions);
    void clear() noexcept;
    void select_next() noexcept;
    void select_previous() noexcept;
    void set_selected_index(std::size_t index) noexcept;

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] const std::optional<AutocompleteSuggestions>& suggestions() const noexcept;
    [[nodiscard]] std::size_t selected_index() const noexcept;
    [[nodiscard]] const AutocompleteItem* selected_item() const noexcept;

  private:
    std::optional<AutocompleteSuggestions> suggestions_;
    std::size_t selected_index_ = 0;
};

class CombinedAutocompleteProvider {
  public:
    explicit CombinedAutocompleteProvider(
        std::vector<SlashCommandCompletion> commands = {},
        std::filesystem::path base_path = std::filesystem::current_path(),
        std::size_t max_file_suggestions = 50,
        std::optional<std::string> fd_path = std::nullopt);

    [[nodiscard]] std::optional<AutocompleteSuggestions> get_suggestions(
        const std::vector<std::string>& lines, std::size_t cursor_line,
        std::size_t cursor_column, bool force = false) const;

    [[nodiscard]] CompletionApplication apply_completion(
        const std::vector<std::string>& lines, std::size_t cursor_line,
        std::size_t cursor_column, const AutocompleteItem& item,
        const std::string& prefix) const;

    [[nodiscard]] bool should_trigger_file_completion(
        const std::vector<std::string>& lines, std::size_t cursor_line,
        std::size_t cursor_column) const;

  private:
    [[nodiscard]] std::optional<std::string> extract_at_prefix(std::string_view text) const;
    [[nodiscard]] std::optional<std::string> extract_path_prefix(std::string_view text,
                                                                 bool force) const;
    [[nodiscard]] std::vector<AutocompleteItem> get_file_suggestions(
        const std::string& prefix) const;
    [[nodiscard]] std::vector<AutocompleteItem> get_recursive_at_suggestions(
        const std::string& prefix, bool quoted) const;

    std::vector<SlashCommandCompletion> commands_;
    std::filesystem::path base_path_;
    std::size_t max_file_suggestions_ = 50;
    std::optional<std::string> fd_path_;
};

}  // namespace qt::tui
