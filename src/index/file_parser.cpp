#include "index/file_parser.hpp"

#include "index/string_util.hpp"

#include <lexy/action/parse.hpp>
#include <lexy/callback.hpp>
#include <lexy/dsl.hpp>
#include <lexy/input/string_input.hpp>

#include <fstream>
#include <stdexcept>

namespace qt::index {
namespace {

namespace grammar {
namespace dsl = lexy::dsl;

struct key : lexy::token_production {
    static constexpr auto rule = [] {
        auto lead = dsl::ascii::alpha_underscore;
        auto rest = dsl::ascii::alpha_digit_underscore;
        return dsl::identifier(lead, rest);
    }();

    static constexpr auto value = lexy::as_string<std::string>;
};

struct raw_value : lexy::token_production {
    static constexpr auto rule = dsl::capture(dsl::token(dsl::while_(dsl::ascii::print)));
    static constexpr auto value = lexy::as_string<std::string>;
};

struct assignment {
    static constexpr auto whitespace = dsl::ascii::blank;
    static constexpr auto rule = dsl::p<key> + dsl::lit_c<'='> + dsl::p<raw_value> + dsl::eof;
    static constexpr auto value = lexy::callback<ParsedAssignment>(
        [](std::string key, std::string value) {
            return ParsedAssignment{std::move(key), trim_ascii(std::move(value))};
        });
};

}  // namespace grammar

}  // namespace

std::optional<ParsedAssignment> parse_assignment_line(std::string_view line) {
    const lexy::string_input input(line.data(), line.size());
    auto result = lexy::parse<grammar::assignment>(input, lexy::noop);
    if (!result) {
        return std::nullopt;
    }

    return result.value();
}

std::vector<ParsedAssignment> parse_assignment_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("could not open parse file: " + path.string());
    }

    std::vector<ParsedAssignment> assignments;
    std::string line;
    while (std::getline(input, line)) {
        if (trim_ascii(line).empty()) {
            continue;
        }

        auto assignment = parse_assignment_line(line);
        if (!assignment) {
            throw std::runtime_error("invalid assignment line in parse file: " + line);
        }
        assignments.push_back(std::move(*assignment));
    }

    return assignments;
}

}  // namespace qt::index
