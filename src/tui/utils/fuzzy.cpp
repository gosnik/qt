#include "qt/tui/utils/fuzzy.hpp"

#include <uni_algo/case.h>
#include <uni_algo/norm.h>
#include <uni_algo/ranges_grapheme.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace qt::tui {

namespace {

bool is_ascii(std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch < 0x80U;
    });
}

std::string normalize_for_match(std::string_view value) {
    if (is_ascii(value)) {
        std::string normalized;
        normalized.reserve(value.size());
        for (const char ch : value) {
            normalized.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(ch))));
        }
        return normalized;
    }

    const std::string compatible = una::norm::to_nfkc_utf8(value);
    const std::string folded = una::cases::to_casefold_utf8(compatible);
    return una::norm::to_nfkc_utf8(folded);
}

std::vector<std::string> graphemes(std::string_view value) {
    std::vector<std::string> units;
    for (const std::string_view unit :
         una::ranges::grapheme::utf8_view<std::string_view>{value}) {
        units.emplace_back(unit);
    }
    return units;
}

bool is_boundary_previous(std::string_view unit) {
    return unit == " " || unit == "\t" || unit == "\r" || unit == "\n" ||
           unit == "-" || unit == "_" || unit == "." || unit == "/" || unit == ":";
}

FuzzyMatch match_normalized(const std::vector<std::string>& normalized_query,
                            const std::vector<std::string>& normalized_text,
                            std::string_view query_text, std::string_view candidate_text) {
    if (normalized_query.empty()) {
        return {true, 0.0};
    }

    if (normalized_query.size() > normalized_text.size()) {
        return {false, 0.0};
    }

    std::size_t query_index = 0;
    double score = 0.0;
    int last_match_index = -1;
    int consecutive_matches = 0;

    for (std::size_t i = 0; i < normalized_text.size() && query_index < normalized_query.size();
         ++i) {
        if (normalized_text[i] != normalized_query[query_index]) {
            continue;
        }

        const bool is_word_boundary = i == 0 || is_boundary_previous(normalized_text[i - 1]);

        if (last_match_index == static_cast<int>(i) - 1) {
            ++consecutive_matches;
            score -= static_cast<double>(consecutive_matches * 5);
        } else {
            consecutive_matches = 0;
            if (last_match_index >= 0) {
                score += static_cast<double>((static_cast<int>(i) - last_match_index - 1) * 2);
            }
        }

        if (is_word_boundary) {
            score -= 10.0;
        }

        score += static_cast<double>(i) * 0.1;
        last_match_index = static_cast<int>(i);
        ++query_index;
    }

    if (query_index < normalized_query.size()) {
        return {false, 0.0};
    }

    if (query_text == candidate_text) {
        score -= 100.0;
    }

    return {true, score};
}

std::string swapped_alpha_numeric_query(const std::string& query_lower) {
    if (query_lower.empty()) {
        return {};
    }

    const auto is_ascii_alpha = [](char ch) {
        return ch >= 'a' && ch <= 'z';
    };
    const auto is_ascii_digit = [](char ch) {
        return ch >= '0' && ch <= '9';
    };

    const bool alpha_first = is_ascii_alpha(query_lower.front());
    const bool digit_first = is_ascii_digit(query_lower.front());
    if (!alpha_first && !digit_first) {
        return {};
    }

    std::size_t split = 0;
    while (split < query_lower.size() &&
           (alpha_first ? is_ascii_alpha(query_lower[split])
                        : is_ascii_digit(query_lower[split]))) {
        ++split;
    }
    if (split == 0 || split == query_lower.size()) {
        return {};
    }

    for (std::size_t index = split; index < query_lower.size(); ++index) {
        if (alpha_first ? !is_ascii_digit(query_lower[index])
                        : !is_ascii_alpha(query_lower[index])) {
            return {};
        }
    }

    std::string swapped;
    swapped.reserve(query_lower.size());
    swapped.append(query_lower.substr(split));
    swapped.append(query_lower.substr(0, split));
    return swapped;
}

}  // namespace

PreparedFuzzyText prepare_fuzzy_text(std::string_view text) {
    PreparedFuzzyText prepared;
    prepared.normalized = normalize_for_match(text);
    prepared.graphemes = graphemes(prepared.normalized);
    return prepared;
}

PreparedFuzzyQuery prepare_fuzzy_query(std::string_view query) {
    PreparedFuzzyQuery prepared;
    prepared.primary = prepare_fuzzy_text(query);
    const std::string swapped_query =
        swapped_alpha_numeric_query(prepared.primary.normalized);
    if (!swapped_query.empty()) {
        prepared.swapped = PreparedFuzzyText{
            swapped_query,
            graphemes(swapped_query),
        };
    }
    return prepared;
}

FuzzyMatch fuzzy_match_prepared(const PreparedFuzzyQuery& query,
                                const PreparedFuzzyText& text) {
    const auto primary =
        match_normalized(query.primary.graphemes, text.graphemes,
                         query.primary.normalized, text.normalized);
    if (primary.matches) {
        return primary;
    }

    if (!query.swapped) {
        return primary;
    }

    auto swapped = match_normalized(query.swapped->graphemes, text.graphemes,
                                    query.swapped->normalized, text.normalized);
    if (!swapped.matches) {
        return primary;
    }

    swapped.score += 5.0;
    return swapped;
}

FuzzyMatch fuzzy_match(std::string_view query, std::string_view text) {
    return fuzzy_match_prepared(prepare_fuzzy_query(query), prepare_fuzzy_text(text));
}

}  // namespace qt::tui
