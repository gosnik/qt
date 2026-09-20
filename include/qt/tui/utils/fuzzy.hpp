#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qt::tui {

struct FuzzyMatch {
    bool matches = false;
    double score = 0.0;
};

struct PreparedFuzzyText {
    std::string normalized;
    std::vector<std::string> graphemes;
};

struct PreparedFuzzyQuery {
    PreparedFuzzyText primary;
    std::optional<PreparedFuzzyText> swapped;
};

[[nodiscard]] PreparedFuzzyText prepare_fuzzy_text(std::string_view text);
[[nodiscard]] PreparedFuzzyQuery prepare_fuzzy_query(std::string_view query);
[[nodiscard]] FuzzyMatch fuzzy_match_prepared(
    const PreparedFuzzyQuery& query, const PreparedFuzzyText& text);
[[nodiscard]] FuzzyMatch fuzzy_match(std::string_view query, std::string_view text);

template <typename T>
struct FuzzyFilterResult {
    T item;
    double score = 0.0;
};

template <typename T, typename GetText>
[[nodiscard]] std::vector<FuzzyFilterResult<T>> fuzzy_filter_scored(
    const std::vector<T>& items, std::string_view query, GetText get_text) {
    auto trimmed_start = query.find_first_not_of(" \t\r\n");
    if (trimmed_start == std::string_view::npos) {
        std::vector<FuzzyFilterResult<T>> unchanged;
        unchanged.reserve(items.size());
        for (const auto& item : items) {
            unchanged.push_back(FuzzyFilterResult<T>{item, 0.0});
        }
        return unchanged;
    }

    auto trimmed_end = query.find_last_not_of(" \t\r\n");
    std::string trimmed(query.substr(trimmed_start, trimmed_end - trimmed_start + 1));
    std::vector<PreparedFuzzyQuery> tokens;
    std::size_t index = 0;
    while (index < trimmed.size()) {
        while (index < trimmed.size() &&
               (trimmed[index] == ' ' || trimmed[index] == '\t' ||
                trimmed[index] == '\r' || trimmed[index] == '\n')) {
            ++index;
        }
        if (index >= trimmed.size()) {
            break;
        }
        const std::size_t start = index;
        while (index < trimmed.size() && trimmed[index] != ' ' &&
               trimmed[index] != '\t' && trimmed[index] != '\r' &&
               trimmed[index] != '\n') {
            ++index;
        }
        tokens.push_back(prepare_fuzzy_query(trimmed.substr(start, index - start)));
    }

    std::vector<std::pair<FuzzyFilterResult<T>, std::size_t>> results;
    results.reserve(items.size());
    for (std::size_t item_index = 0; item_index < items.size(); ++item_index) {
        const auto& item = items[item_index];
        const std::string text = std::invoke(get_text, item);
        const auto prepared_text = prepare_fuzzy_text(text);
        double total_score = 0.0;
        bool all_match = true;

        for (const auto& token : tokens) {
            const auto match = fuzzy_match_prepared(token, prepared_text);
            if (!match.matches) {
                all_match = false;
                break;
            }
            total_score += match.score;
        }

        if (all_match) {
            results.push_back({FuzzyFilterResult<T>{item, total_score}, item_index});
        }
    }

    std::stable_sort(results.begin(), results.end(), [](const auto& left, const auto& right) {
        if (left.first.score == right.first.score) {
            return left.second < right.second;
        }
        return left.first.score < right.first.score;
    });

    std::vector<FuzzyFilterResult<T>> filtered;
    filtered.reserve(results.size());
    for (auto& result : results) {
        filtered.push_back(std::move(result.first));
    }
    return filtered;
}

template <typename T, typename GetText>
[[nodiscard]] std::vector<T> fuzzy_filter(const std::vector<T>& items, std::string_view query,
                                          GetText get_text) {
    auto scored = fuzzy_filter_scored(items, query, std::move(get_text));
    std::vector<T> filtered;
    filtered.reserve(scored.size());
    for (auto& result : scored) {
        filtered.push_back(std::move(result.item));
    }
    return filtered;
}

}  // namespace qt::tui
