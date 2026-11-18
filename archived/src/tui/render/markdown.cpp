#include "qt/tui/render/markdown.hpp"
#include "qt/tui/terminal/ansi.hpp"

#include "qt/foundation/utf8.hpp"
#include "qt/tui/utils/line_split.hpp"

#include <md4c.h>
#include <uni_algo/ranges_grapheme.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qt::tui {

namespace {

int md4c_noop_enter_block(MD_BLOCKTYPE, void*, void*) { return 0; }
int md4c_noop_leave_block(MD_BLOCKTYPE, void*, void*) { return 0; }

enum class BlockType {
    heading,
    paragraph,
    list,
    blockquote,
    code_block,
    horizontal_rule,
    table,
};

enum class ColumnAlignment {
    left,
    center,
    right,
};

enum class MdNodeType {
    document,
    heading,
    paragraph,
    list,
    list_item,
    blockquote,
    code_block,
    horizontal_rule,
    table,
    table_head,
    table_body,
    table_row,
    table_cell,
    html,
};

struct MdNode {
    MdNodeType type = MdNodeType::document;
    std::size_t level = 0;
    bool ordered = false;
    unsigned start = 1;
    char ordered_delimiter = '.';
    bool task = false;
    char task_mark = ' ';
    bool tight = true;
    ColumnAlignment alignment = ColumnAlignment::left;
    std::string text;
    std::string code_info;
    std::vector<std::unique_ptr<MdNode>> children;
};

struct ListItem {
    std::string first_prefix;
    std::string rest_prefix;
    std::string text;
    bool fenced_code = false;
    std::string fence;
    std::vector<std::string> code_lines;
    std::string closing_fence;
};

struct Block {
    BlockType type;
    std::size_t level = 0;
    std::vector<std::string> lines;
    std::vector<ListItem> items;
    std::vector<ColumnAlignment> alignments;
};

bool is_blank(std::string_view text) {
    for (char ch : text) {
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    return true;
}

std::string trim(std::string_view text) {
    std::size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }

    std::size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }

    return std::string(text.substr(start, end - start));
}

bool is_strict_strike_body(std::string_view text) {
    if (text.empty() ||
        std::isspace(static_cast<unsigned char>(text.front())) ||
        std::isspace(static_cast<unsigned char>(text.back()))) {
        return false;
    }
    return true;
}

std::string attribute_to_string(const MD_ATTRIBUTE& attribute) {
    if (attribute.text == nullptr || attribute.size == 0) {
        return "";
    }
    return std::string(attribute.text, attribute.text + attribute.size);
}

struct InlineFrame {
    std::string text;
    std::string href;
};

struct InlineRenderState {
    std::vector<InlineFrame> stack;
    std::string result;
};

struct Md4cRenderState {
    MdNode document;
    std::vector<MdNode*> stack;
    std::vector<InlineFrame> inline_stack;

    Md4cRenderState() {
        document.type = MdNodeType::document;
        stack.push_back(&document);
    }
};

std::string& inline_output(InlineRenderState& state) {
    if (state.stack.empty()) {
        return state.result;
    }
    return state.stack.back().text;
}

void append_inline_text(InlineRenderState& state, std::string_view text) {
    inline_output(state).append(text);
}

MdNode& current_node(Md4cRenderState& state) {
    return *state.stack.back();
}

std::string& md4c_output(Md4cRenderState& state) {
    if (state.inline_stack.empty()) {
        return current_node(state).text;
    }
    return state.inline_stack.back().text;
}

void append_md4c_text(Md4cRenderState& state, std::string_view text) {
    md4c_output(state).append(text);
}

std::string escape_single_tilde_markers(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char ch = text[index];
        if (ch == '~') {
            const bool previous_is_tilde = index > 0 && text[index - 1] == '~';
            const bool next_is_tilde = index + 1 < text.size() && text[index + 1] == '~';
            if (!previous_is_tilde && !next_is_tilde) {
                escaped.push_back('\\');
            }
        }
        escaped.push_back(ch);
    }
    return escaped;
}

int md4c_inline_enter_span(MD_SPANTYPE type, void* detail, void* userdata) {
    auto& state = *static_cast<InlineRenderState*>(userdata);
    InlineFrame frame{{}, {}};
    if (type == MD_SPAN_A && detail != nullptr) {
        frame.href = attribute_to_string(static_cast<MD_SPAN_A_DETAIL*>(detail)->href);
    } else if (type == MD_SPAN_IMG && detail != nullptr) {
        frame.href = attribute_to_string(static_cast<MD_SPAN_IMG_DETAIL*>(detail)->src);
    }
    state.stack.push_back(std::move(frame));
    return 0;
}

int md4c_inline_leave_span(MD_SPANTYPE type, void*, void* userdata) {
    auto& state = *static_cast<InlineRenderState*>(userdata);
    if (state.stack.empty()) {
        return 0;
    }

    auto frame = std::move(state.stack.back());
    state.stack.pop_back();
    std::string rendered = std::move(frame.text);

    if (type == MD_SPAN_A) {
        std::string comparison_url = frame.href;
        if (comparison_url.rfind("mailto:", 0) == 0) {
            comparison_url.erase(0, 7);
        }
        if (rendered != frame.href && rendered != comparison_url) {
            rendered += " (";
            rendered += frame.href;
            rendered += ")";
        }
    } else if (type == MD_SPAN_IMG) {
        if (rendered.empty()) {
            rendered = std::move(frame.href);
        }
    } else if (type == MD_SPAN_CODE) {
        rendered.insert(0, "`");
        rendered += "`";
    } else if (type == MD_SPAN_DEL && !is_strict_strike_body(rendered)) {
        rendered.insert(0, "~~");
        rendered += "~~";
    } else if (type == MD_SPAN_SUBSCRIPT) {
        rendered.insert(0, "~");
        rendered += "~";
    }

    append_inline_text(state, rendered);
    return 0;
}

int md4c_inline_text(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    auto& state = *static_cast<InlineRenderState*>(userdata);
    if (text == nullptr || size == 0) {
        return 0;
    }

    if (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR) {
        append_inline_text(state, " ");
        return 0;
    }

    append_inline_text(state, std::string_view(text, size));
    return 0;
}

std::string normalize_inline(std::string_view text) {
    InlineRenderState state;
    state.result.reserve(text.size());
    const std::string escaped = escape_single_tilde_markers(text);

    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = MD_DIALECT_GITHUB;
    parser.enter_block = md4c_noop_enter_block;
    parser.leave_block = md4c_noop_leave_block;
    parser.enter_span = md4c_inline_enter_span;
    parser.leave_span = md4c_inline_leave_span;
    parser.text = md4c_inline_text;

    if (md_parse(escaped.data(), static_cast<MD_SIZE>(escaped.size()), &parser, &state) != 0) {
        return std::string(text);
    }
    return state.result;
}

ColumnAlignment md4c_alignment(MD_ALIGN alignment) {
    if (alignment == MD_ALIGN_CENTER) {
        return ColumnAlignment::center;
    }
    if (alignment == MD_ALIGN_RIGHT) {
        return ColumnAlignment::right;
    }
    return ColumnAlignment::left;
}

MdNodeType md4c_node_type(MD_BLOCKTYPE type) {
    switch (type) {
    case MD_BLOCK_DOC:
        return MdNodeType::document;
    case MD_BLOCK_QUOTE:
        return MdNodeType::blockquote;
    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
        return MdNodeType::list;
    case MD_BLOCK_LI:
        return MdNodeType::list_item;
    case MD_BLOCK_HR:
        return MdNodeType::horizontal_rule;
    case MD_BLOCK_H:
        return MdNodeType::heading;
    case MD_BLOCK_CODE:
        return MdNodeType::code_block;
    case MD_BLOCK_HTML:
        return MdNodeType::html;
    case MD_BLOCK_P:
        return MdNodeType::paragraph;
    case MD_BLOCK_TABLE:
        return MdNodeType::table;
    case MD_BLOCK_THEAD:
        return MdNodeType::table_head;
    case MD_BLOCK_TBODY:
        return MdNodeType::table_body;
    case MD_BLOCK_TR:
        return MdNodeType::table_row;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        return MdNodeType::table_cell;
    case MD_BLOCK_ADMONITION:
        return MdNodeType::paragraph;
    }
    return MdNodeType::paragraph;
}

int md4c_block_enter(MD_BLOCKTYPE type, void* detail, void* userdata) {
    auto& state = *static_cast<Md4cRenderState*>(userdata);
    if (type == MD_BLOCK_DOC) {
        return 0;
    }

    auto node = std::make_unique<MdNode>();
    node->type = md4c_node_type(type);

    if (type == MD_BLOCK_H && detail != nullptr) {
        node->level = static_cast<MD_BLOCK_H_DETAIL*>(detail)->level;
    } else if (type == MD_BLOCK_UL && detail != nullptr) {
        const auto* ul = static_cast<MD_BLOCK_UL_DETAIL*>(detail);
        node->ordered = false;
        node->tight = ul->is_tight != 0;
    } else if (type == MD_BLOCK_OL && detail != nullptr) {
        const auto* ol = static_cast<MD_BLOCK_OL_DETAIL*>(detail);
        node->ordered = true;
        node->start = ol->start;
        node->ordered_delimiter = ol->mark_delimiter == ')' ? ')' : '.';
        node->tight = ol->is_tight != 0;
    } else if (type == MD_BLOCK_LI && detail != nullptr) {
        const auto* li = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
        node->task = li->is_task != 0;
        node->task_mark = li->task_mark;
    } else if (type == MD_BLOCK_CODE && detail != nullptr) {
        const auto* code = static_cast<MD_BLOCK_CODE_DETAIL*>(detail);
        node->code_info = attribute_to_string(code->info);
    } else if ((type == MD_BLOCK_TH || type == MD_BLOCK_TD) && detail != nullptr) {
        node->alignment = md4c_alignment(static_cast<MD_BLOCK_TD_DETAIL*>(detail)->align);
    }

    MdNode* raw = node.get();
    current_node(state).children.push_back(std::move(node));
    state.stack.push_back(raw);
    return 0;
}

int md4c_block_leave(MD_BLOCKTYPE type, void*, void* userdata) {
    auto& state = *static_cast<Md4cRenderState*>(userdata);
    if (type != MD_BLOCK_DOC && state.stack.size() > 1) {
        state.stack.pop_back();
    }
    return 0;
}

int md4c_block_enter_span(MD_SPANTYPE type, void* detail, void* userdata) {
    auto& state = *static_cast<Md4cRenderState*>(userdata);
    InlineFrame frame{{}, {}};
    if (type == MD_SPAN_A && detail != nullptr) {
        frame.href = attribute_to_string(static_cast<MD_SPAN_A_DETAIL*>(detail)->href);
    } else if (type == MD_SPAN_IMG && detail != nullptr) {
        frame.href = attribute_to_string(static_cast<MD_SPAN_IMG_DETAIL*>(detail)->src);
    }
    state.inline_stack.push_back(std::move(frame));
    return 0;
}

int md4c_block_leave_span(MD_SPANTYPE type, void*, void* userdata) {
    auto& state = *static_cast<Md4cRenderState*>(userdata);
    if (state.inline_stack.empty()) {
        return 0;
    }

    auto frame = std::move(state.inline_stack.back());
    state.inline_stack.pop_back();
    std::string rendered = std::move(frame.text);

    if (type == MD_SPAN_A) {
        std::string comparison_url = frame.href;
        if (comparison_url.rfind("mailto:", 0) == 0) {
            comparison_url.erase(0, 7);
        }
        if (rendered != frame.href && rendered != comparison_url) {
            rendered += " (";
            rendered += frame.href;
            rendered += ")";
        }
    } else if (type == MD_SPAN_IMG) {
        if (rendered.empty()) {
            rendered = std::move(frame.href);
        }
    } else if (type == MD_SPAN_CODE) {
        rendered.insert(0, "`");
        rendered += "`";
    } else if (type == MD_SPAN_DEL && !is_strict_strike_body(rendered)) {
        rendered.insert(0, "~~");
        rendered += "~~";
    } else if (type == MD_SPAN_SUBSCRIPT) {
        rendered.insert(0, "~");
        rendered += "~";
    }

    append_md4c_text(state, rendered);
    return 0;
}

int md4c_block_text(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    auto& state = *static_cast<Md4cRenderState*>(userdata);
    if (text == nullptr || size == 0) {
        return 0;
    }

    if (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR) {
        append_md4c_text(state, current_node(state).type == MdNodeType::list_item ? "\n" : " ");
        return 0;
    }

    append_md4c_text(state, std::string_view(text, size));
    return 0;
}

std::optional<MdNode> parse_md4c_document(std::string_view text) {
    Md4cRenderState state;

    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = MD_DIALECT_GITHUB;
    parser.enter_block = md4c_block_enter;
    parser.leave_block = md4c_block_leave;
    parser.enter_span = md4c_block_enter_span;
    parser.leave_span = md4c_block_leave_span;
    parser.text = md4c_block_text;

    const std::string escaped = escape_single_tilde_markers(text);
    if (md_parse(escaped.data(), static_cast<MD_SIZE>(escaped.size()), &parser, &state) != 0) {
        return std::nullopt;
    }
    return std::move(state.document);
}

std::vector<std::string> split_lines(std::string_view text) {
    return split_lines_copy(text, SplitLinesOptions{true, true, true});
}

std::string normalize_tabs(std::string_view text) {
    std::string normalized;
    normalized.reserve(text.size());
    for (const char ch : text) {
        if (ch == '\t') {
            normalized += "   ";
        } else {
            normalized.push_back(ch);
        }
    }
    return normalized;
}

bool parse_heading(std::string_view line, std::size_t& level, std::string& text) {
    std::size_t hashes = 0;
    while (hashes < line.size() && line[hashes] == '#') {
        ++hashes;
    }
    if (hashes == 0 || hashes > 6 || hashes >= line.size() || line[hashes] != ' ') {
        return false;
    }

    level = hashes;
    text = trim(line.substr(hashes + 1));
    return true;
}

std::size_t leading_spaces(std::string_view line) {
    std::size_t count = 0;
    while (count < line.size() && line[count] == ' ') {
        ++count;
    }
    return count;
}

std::optional<ListItem> parse_list_item(std::string_view line) {
    const std::size_t spaces = leading_spaces(line);
    const auto content = line.substr(spaces);
    const std::string indent((spaces / 2) * 4, ' ');

    if (content.size() >= 2 && (content[0] == '-' || content[0] == '*') && content[1] == ' ') {
        ListItem item;
        item.first_prefix = indent + "- ";
        item.rest_prefix = indent + "  ";
        item.text = trim(content.substr(2));
        return item;
    }

    std::size_t digits = 0;
    while (digits < content.size() && std::isdigit(static_cast<unsigned char>(content[digits]))) {
        ++digits;
    }
    if (digits > 0 && digits + 1 < content.size() && content[digits] == '.' &&
        content[digits + 1] == ' ') {
        std::string marker(content.substr(0, digits + 2));
        ListItem item;
        item.first_prefix = indent + marker;
        item.rest_prefix = indent + std::string(marker.size(), ' ');
        item.text = trim(content.substr(digits + 2));
        return item;
    }

    return std::nullopt;
}

bool parse_blockquote(std::string_view line, std::string& text) {
    if (line.empty() || line[0] != '>') {
        return false;
    }
    std::size_t start = 1;
    if (start < line.size() && line[start] == ' ') {
        ++start;
    }
    text = trim(line.substr(start));
    return true;
}

bool is_fence(std::string_view line) {
    return line.rfind("```", 0) == 0;
}

bool is_trimmed_fence(std::string_view line) {
    return is_fence(trim(line));
}

std::string strip_continuation_indent(std::string_view line, std::size_t indent) {
    const auto spaces = leading_spaces(line);
    const auto remove = std::min(spaces, indent);
    return std::string(line.substr(remove));
}

bool is_horizontal_rule(std::string_view line) {
    const auto stripped = trim(line);
    if (stripped.size() < 3) {
        return false;
    }
    const char marker = stripped.front();
    if (marker != '-' && marker != '*' && marker != '_') {
        return false;
    }
    for (const char ch : stripped) {
        if (ch != marker && !std::isspace(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> split_table_row(std::string_view line) {
    std::string row = trim(line);
    if (!row.empty() && row.front() == '|') {
        row.erase(row.begin());
    }
    if (!row.empty() && row.back() == '|') {
        row.pop_back();
    }

    std::vector<std::string> cells;
    std::string cell;
    bool escaped = false;
    for (const char ch : row) {
        if (escaped) {
            cell.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '|') {
            cells.push_back(trim(cell));
            cell.clear();
            continue;
        }
        cell.push_back(ch);
    }
    if (escaped) {
        cell.push_back('\\');
    }
    cells.push_back(trim(cell));
    return cells;
}

bool is_table_separator(std::string_view line) {
    const auto cells = split_table_row(line);
    if (cells.empty()) {
        return false;
    }
    for (const auto& cell : cells) {
        std::size_t dashes = 0;
        for (const char ch : cell) {
            if (ch == '-') {
                ++dashes;
            } else if (ch != ':') {
                return false;
            }
        }
        if (dashes < 3) {
            return false;
        }
    }
    return true;
}

bool starts_table(std::string_view header, std::string_view separator) {
    return header.find('|') != std::string_view::npos && separator.find('|') != std::string_view::npos &&
           is_table_separator(separator);
}

std::vector<ColumnAlignment> parse_table_alignments(std::string_view separator) {
    std::vector<ColumnAlignment> alignments;
    for (const auto& cell : split_table_row(separator)) {
        const bool left_marker = !cell.empty() && cell.front() == ':';
        const bool right_marker = !cell.empty() && cell.back() == ':';
        if (left_marker && right_marker) {
            alignments.push_back(ColumnAlignment::center);
        } else if (right_marker) {
            alignments.push_back(ColumnAlignment::right);
        } else {
            alignments.push_back(ColumnAlignment::left);
        }
    }
    return alignments;
}

foundation::Result<std::size_t> display_width(std::string_view text) {
    return foundation::utf8_display_width(text);
}

std::size_t safe_width(std::string_view text) {
    std::size_t width = 0;
    std::size_t offset = 0;
    while (offset < text.size()) {
        if (auto sequence = extract_ansi_sequence(text, offset)) {
            offset += sequence->length;
            continue;
        }
        if (text[offset] == '\x1b') { ++offset; continue; }

        const auto next_escape = text.find('\x1b', offset);
        const auto end = next_escape == std::string_view::npos ? text.size()
                         : next_escape == offset ? offset + 1U : next_escape;
        auto span_width = display_width(text.substr(offset, end - offset));
        if (!span_width) {
            return text.size();
        }
        width += span_width.value();
        offset = end;
    }
    return width;
}

std::string truncate_ansi(std::string_view text, std::size_t width,
                          std::size_t next_escape = std::string_view::npos) {
    std::string result;
    std::size_t columns = 0;
    std::size_t offset = 0;
    while (offset < text.size()) {
        if (auto sequence = extract_ansi_sequence(text, offset)) {
            result.append(text.substr(offset, sequence->length));
            offset += sequence->length;
            continue;
        }
        if (text[offset] == '\x1b') { ++offset; continue; }

        if (next_escape != std::string_view::npos && next_escape < offset) {
            next_escape = text.find('\x1b', offset);
        }
        const auto plain_end = next_escape == std::string_view::npos ? text.size()
                               : next_escape == offset ? offset + 1U : next_escape;
        const auto plain = text.substr(offset, plain_end - offset);
        std::size_t consumed = 0;
        std::string pending_joined;
        for (const std::string_view grapheme :
             una::ranges::grapheme::utf8_view<std::string_view>{plain}) {
            std::string_view glyph = grapheme;
            if (!pending_joined.empty()) {
                pending_joined.append(grapheme);
                glyph = pending_joined;
            }

            if (glyph.size() >= 3 && glyph.substr(glyph.size() - 3) == "\xE2\x80\x8D") {
                if (pending_joined.empty()) {
                    pending_joined.assign(glyph);
                }
                continue;
            }

            const auto glyph_width = display_width(glyph);
            if (!glyph_width || columns + glyph_width.value() > width) {
                return result;
            }
            result.append(glyph);
            columns += glyph_width.value();
            consumed += glyph.size();
            pending_joined.clear();
        }
        if (!pending_joined.empty()) {
            const auto glyph_width = display_width(pending_joined);
            if (!glyph_width || columns + glyph_width.value() > width) {
                return result;
            }
            result.append(pending_joined);
            columns += glyph_width.value();
            consumed += pending_joined.size();
        }
        offset += consumed;
    }
    return result;
}

void update_active_sgr(std::string_view sequence, std::string& active_sgr) {
    if (sequence.size() < 3 || sequence[0] != '\x1b' || sequence[1] != '[' ||
        sequence.back() != 'm') {
        return;
    }

    const auto params = sequence.substr(2, sequence.size() - 3);
    if (params.empty() || params == "0") {
        active_sgr.clear();
        return;
    }

    active_sgr = std::string(sequence);
}

bool is_osc8_sequence(std::string_view sequence) {
    return sequence.rfind("\x1b]8;;", 0) == 0;
}

std::string osc8_close_for(std::string_view sequence) {
    if (!is_osc8_sequence(sequence)) {
        return "";
    }
    if (!sequence.empty() && sequence.back() == '\x07') {
        return "\x1b]8;;\x07";
    }
    if (sequence.size() >= 2 && sequence[sequence.size() - 2] == '\x1b' &&
        sequence[sequence.size() - 1] == '\\') {
        return "\x1b]8;;\x1b\\";
    }
    return "";
}

void update_active_osc8(std::string_view sequence, std::string& active_osc8,
                        std::string& active_osc8_close) {
    if (!is_osc8_sequence(sequence)) {
        return;
    }

    const auto close = osc8_close_for(sequence);
    if (sequence == close) {
        active_osc8.clear();
        active_osc8_close.clear();
        return;
    }

    active_osc8 = std::string(sequence);
    active_osc8_close = close;
}

void update_active_sequences_from_text(std::string_view text, std::string& active_sgr,
                                       std::string& active_osc8,
                                       std::string& active_osc8_close) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        auto sequence = extract_ansi_sequence(text, offset);
        if (!sequence) {
            ++offset;
            continue;
        }

        const auto code = text.substr(offset, sequence->length);
        update_active_sgr(code, active_sgr);
        update_active_osc8(code, active_osc8, active_osc8_close);
        offset += sequence->length;
    }
}

std::string active_prefix(std::string_view active_sgr, std::string_view active_osc8) {
    std::string prefix(active_sgr);
    prefix += active_osc8;
    return prefix;
}

std::string join_with_spaces(const std::vector<std::string>& lines) {
    std::string joined;
    bool first = true;
    for (const auto& line : lines) {
        auto piece = trim(line);
        if (piece.empty()) {
            continue;
        }
        if (!first) {
            joined += ' ';
        }
        joined += piece;
        first = false;
    }
    return joined;
}

[[maybe_unused]] std::vector<Block> parse_blocks(std::string_view text) {
    std::vector<Block> blocks;
    const auto lines = split_lines(text);
    std::size_t index = 0;

    while (index < lines.size()) {
        if (is_blank(lines[index])) {
            ++index;
            continue;
        }

        if (is_fence(lines[index])) {
            Block block{BlockType::code_block, 0, {}, {}, {}};
            ++index;
            while (index < lines.size() && !is_fence(lines[index])) {
                block.lines.push_back(lines[index]);
                ++index;
            }
            if (index < lines.size() && is_fence(lines[index])) {
                ++index;
            }
            blocks.push_back(std::move(block));
            continue;
        }

        std::size_t level = 0;
        std::string content;
        if (index + 1 < lines.size() && starts_table(lines[index], lines[index + 1])) {
            Block block{BlockType::table, 0, {}, {}, {}};
            block.lines.push_back(lines[index]);
            block.alignments = parse_table_alignments(lines[index + 1]);
            index += 2;
            while (index < lines.size() && !is_blank(lines[index]) &&
                   lines[index].find('|') != std::string::npos) {
                block.lines.push_back(lines[index]);
                ++index;
            }
            blocks.push_back(std::move(block));
            continue;
        }

        if (is_horizontal_rule(lines[index])) {
            blocks.push_back(Block{BlockType::horizontal_rule, 0, {}, {}, {}});
            ++index;
            continue;
        }

        if (parse_heading(lines[index], level, content)) {
            Block block{BlockType::heading, level, {}, {}, {}};
            block.lines.push_back(std::move(content));
            blocks.push_back(std::move(block));
            ++index;
            continue;
        }

        if (parse_list_item(lines[index])) {
            Block block{BlockType::list, 0, {}, {}, {}};
            while (index < lines.size()) {
                auto item = parse_list_item(lines[index]);
                if (!item) {
                    break;
                }

                std::vector<std::string> item_lines;
                item_lines.push_back(std::move(item->text));
                ++index;

                if (is_fence(item_lines.front())) {
                    item->fenced_code = true;
                    item->fence = std::move(item_lines.front());
                    while (index < lines.size()) {
                        auto line = strip_continuation_indent(lines[index], item->rest_prefix.size());
                        if (is_trimmed_fence(line)) {
                            item->closing_fence = trim(line);
                            ++index;
                            break;
                        }
                        item->code_lines.push_back(std::move(line));
                        ++index;
                    }
                    if (item->closing_fence.empty()) {
                        item->closing_fence = "```";
                    }
                    block.items.push_back(std::move(*item));
                    continue;
                }

                while (index < lines.size() && !is_blank(lines[index])) {
                    if (is_fence(lines[index])) {
                        break;
                    }
                    std::size_t ignored_level = 0;
                    std::string ignored_text;
                    if (parse_heading(lines[index], ignored_level, ignored_text) ||
                        parse_list_item(lines[index]) ||
                        parse_blockquote(lines[index], ignored_text)) {
                        break;
                    }
                    item_lines.push_back(lines[index]);
                    ++index;
                }

                item->text = join_with_spaces(item_lines);
                block.items.push_back(std::move(*item));
            }
            blocks.push_back(std::move(block));
            continue;
        }

        if (parse_blockquote(lines[index], content)) {
            std::vector<std::string> quote_lines;
            while (index < lines.size()) {
                std::string quote_text;
                if (parse_blockquote(lines[index], quote_text)) {
                    quote_lines.push_back(std::move(quote_text));
                    ++index;
                    continue;
                }
                if (is_blank(lines[index]) || is_fence(lines[index]) ||
                    is_horizontal_rule(lines[index]) ||
                    starts_table(lines[index], index + 1 < lines.size() ? lines[index + 1] : "")) {
                    break;
                }

                std::size_t ignored_level = 0;
                std::string ignored_text;
                if (parse_heading(lines[index], ignored_level, ignored_text) ||
                    parse_list_item(lines[index])) {
                    break;
                }

                quote_lines.push_back(trim(lines[index]));
                ++index;
            }

            Block block{BlockType::blockquote, 0, {}, {}, {}};
            block.lines.push_back(join_with_spaces(quote_lines));
            blocks.push_back(std::move(block));
            continue;
        }

        std::vector<std::string> paragraph_lines;
        while (index < lines.size() && !is_blank(lines[index])) {
            if (is_fence(lines[index])) {
                break;
            }
            std::size_t ignored_level = 0;
            std::string ignored_text;
            if (parse_heading(lines[index], ignored_level, ignored_text) ||
                parse_list_item(lines[index]) ||
                parse_blockquote(lines[index], ignored_text)) {
                break;
            }
            paragraph_lines.push_back(lines[index]);
            ++index;
        }

        Block block{BlockType::paragraph, 0, {}, {}, {}};
        block.lines.push_back(join_with_spaces(paragraph_lines));
        blocks.push_back(std::move(block));
    }

    return blocks;
}

std::vector<std::string> split_words(std::string_view text) {
    std::vector<std::string> words;
    std::size_t index = 0;
    while (index < text.size()) {
        while (index < text.size() &&
               std::isspace(static_cast<unsigned char>(text[index]))) {
            ++index;
        }
        if (index >= text.size()) {
            break;
        }

        std::size_t end = index;
        while (end < text.size() &&
               !std::isspace(static_cast<unsigned char>(text[end]))) {
            ++end;
        }
        words.emplace_back(text.substr(index, end - index));
        index = end;
    }
    return words;
}

std::vector<std::string> split_chunk(std::string_view text, std::size_t width) {
    std::vector<std::string> pieces;
    if (text.empty()) {
        pieces.emplace_back("");
        return pieces;
    }

    std::size_t offset = 0;
    std::size_t next_escape = text.find('\x1b');
    std::string continuation_prefix;
    std::string active_sgr;
    std::string active_osc8;
    std::string active_osc8_close;
    while (offset < text.size()) {
        const auto remaining = text.substr(offset);
        const auto relative_escape = next_escape == std::string_view::npos ? next_escape
            : next_escape >= offset ? next_escape - offset : 0U;
        auto piece = truncate_ansi(remaining, width, relative_escape);
        if (piece.empty()) {
            pieces.emplace_back(remaining);
            break;
        }

        const auto consumed = piece.size();
        piece.insert(0, continuation_prefix);
        update_active_sequences_from_text(piece, active_sgr, active_osc8, active_osc8_close);
        if (!active_osc8_close.empty() && consumed < remaining.size()) {
            piece += active_osc8_close;
        }
        pieces.push_back(std::move(piece));
        offset += consumed;
        if (next_escape != std::string_view::npos && next_escape < offset) {
            next_escape = text.find('\x1b', offset);
        }
        continuation_prefix = active_prefix(active_sgr, active_osc8);
    }

    return pieces;
}

struct ReflowContext {
    std::vector<Markdown::ReflowCacheEntry>* entries = nullptr;
    std::size_t ordinal = 0;
};

thread_local ReflowContext* active_reflow_context = nullptr;

std::vector<std::string> wrap_reflow_full(std::string_view text, std::string_view first_prefix,
                                          std::string_view rest_prefix, std::size_t width,
                                          std::string* last_run = nullptr) {
    std::vector<std::string> lines;
    const auto first_width = safe_width(first_prefix);
    const auto rest_width = safe_width(rest_prefix);
    const auto words = split_words(text);

    if (words.empty()) {
        lines.emplace_back(first_prefix);
        return lines;
    }

    std::string current(first_prefix);
    std::string tail;
    std::size_t current_width = first_width;
    std::size_t limit = width > first_width ? width - first_width : 1;
    bool first_line = true;
    std::string active_sgr;

    for (const auto& word : words) {
        for (const auto& piece : split_chunk(word, limit)) {
            const auto piece_width = safe_width(piece);
            if (current_width > (first_line ? first_width : rest_width)) {
                const auto next_width = current_width + 1 + piece_width;
                if (next_width <= width) {
                    current += ' ';
                    current += piece;
                    tail += ' ';
                    tail += piece;
                    current_width = next_width;
                    continue;
                }

                std::string ignored_osc8;
                std::string ignored_osc8_close;
                update_active_sequences_from_text(current, active_sgr, ignored_osc8,
                                                  ignored_osc8_close);
                lines.push_back(std::move(current));
                const bool piece_has_active =
                    !active_sgr.empty() && piece.rfind(active_sgr, 0) == 0;
                current = std::string(rest_prefix) + (piece_has_active ? "" : active_sgr) + piece;
                tail = (piece_has_active ? "" : active_sgr) + piece;
                current_width = rest_width + piece_width;
                first_line = false;
                limit = width > rest_width ? width - rest_width : 1;
                continue;
            }

            current += piece;
            tail += piece;
            current_width += piece_width;
            first_line = false;
            limit = width > rest_width ? width - rest_width : 1;
        }
    }

    lines.push_back(std::move(current));
    if (last_run != nullptr) {
        *last_run = std::move(tail);
    }
    return lines;
}

bool ends_with_space(std::string_view value) {
    return !value.empty() && std::isspace(static_cast<unsigned char>(value.back()));
}

bool has_incomplete_ansi(std::string_view value) {
    for (std::size_t offset = 0; offset < value.size();) {
        const auto escape = value.find('\x1b', offset);
        if (escape == std::string_view::npos) {
            return false;
        }
        const auto sequence = extract_ansi_sequence(value, escape);
        if (!sequence) {
            return true;
        }
        offset = escape + sequence->length;
    }
    return false;
}

bool has_ansi(std::string_view value) {
    return value.find('\x1b') != std::string_view::npos;
}

std::vector<std::string> wrap_reflow(std::string_view text, std::string_view first_prefix,
                                     std::string_view rest_prefix, std::size_t width) {
    if (active_reflow_context == nullptr || active_reflow_context->entries == nullptr) {
        return wrap_reflow_full(text, first_prefix, rest_prefix, width);
    }

    auto& context = *active_reflow_context;
    const std::size_t ordinal = context.ordinal++;
    auto& entries = *context.entries;
    if (ordinal >= entries.size()) {
        entries.resize(ordinal + 1U);
    }
    auto& entry = entries[ordinal];
    const bool same_shape = entry.width == width && entry.first_prefix == first_prefix &&
                            entry.rest_prefix == rest_prefix;
    if (same_shape && entry.run == text) {
        return entry.lines;
    }

    // md4c has already produced this styled run.  Reuse only lines known to
    // precede its mutable tail; a changed parse/run always falls through.
    if (!has_incomplete_ansi(text) && !has_ansi(text) && same_shape && !entry.lines.empty() && text.size() >= entry.run.size() &&
        text.compare(0, entry.run.size(), entry.run) == 0) {
        std::string tail = entry.tail_run;
        const auto appended = text.substr(entry.run.size());
        if (entry.trailing_space && !tail.empty() && !appended.empty() &&
            !std::isspace(static_cast<unsigned char>(appended.front()))) {
            tail.push_back(' ');
        }
        tail.append(appended);
        const auto tail_prefix = entry.lines.size() == 1U ? first_prefix : rest_prefix;
        std::string rewritten_tail;
        auto rewritten = wrap_reflow_full(tail, tail_prefix, rest_prefix, width, &rewritten_tail);
        std::vector<std::string> result(entry.lines.begin(), entry.lines.end() - 1);
        result.insert(result.end(), rewritten.begin(), rewritten.end());
        entry.run.assign(text);
        entry.lines = result;
        entry.trailing_space = ends_with_space(text);
        entry.tail_run = std::move(rewritten_tail);
        return result;
    }

    std::string tail;
    auto result = wrap_reflow_full(text, first_prefix, rest_prefix, width, &tail);
    entry.run.assign(text);
    entry.first_prefix.assign(first_prefix);
    entry.rest_prefix.assign(rest_prefix);
    entry.width = width;
    entry.tail_run = std::move(tail);
    entry.lines = result;
    entry.trailing_space = ends_with_space(text);
    return result;
}

std::vector<std::string> wrap_verbatim(std::string_view text, std::string_view prefix,
                                       std::size_t width) {
    std::vector<std::string> lines;
    const auto prefix_width = safe_width(prefix);
    const auto content_width = width > prefix_width ? width - prefix_width : 1;
    const auto pieces = split_chunk(text, content_width);

    for (std::size_t index = 0; index < pieces.size(); ++index) {
        std::string line(prefix);
        line += pieces[index];
        lines.push_back(std::move(line));
    }

    if (pieces.empty()) {
        lines.emplace_back(prefix);
    }

    return lines;
}

std::string repeat_to_width(std::string_view unit, std::size_t width) {
    std::string line;
    while (safe_width(line) < width) {
        line += unit;
    }
    return truncate_ansi(line, width);
}

std::vector<std::string> render_table(const Block& block, std::size_t width) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::size_t> widths;
    for (const auto& line : block.lines) {
        auto cells = split_table_row(line);
        if (widths.size() < cells.size()) {
            widths.resize(cells.size(), 0);
        }
        for (std::size_t index = 0; index < cells.size(); ++index) {
            cells[index] = normalize_inline(cells[index]);
            widths[index] = std::max(widths[index], safe_width(cells[index]));
        }
        rows.push_back(std::move(cells));
    }

    if (widths.empty()) {
        return {};
    }

    const std::size_t minimum_table_width = widths.size() * 3U + 1U;
    if (width > minimum_table_width) {
        const std::size_t available_content_width = width - minimum_table_width;
        while (std::accumulate(widths.begin(), widths.end(), std::size_t{0}) >
               available_content_width) {
            auto largest = std::max_element(widths.begin(), widths.end());
            if (largest == widths.end() || *largest <= 1U) {
                break;
            }
            --*largest;
        }
    }

    auto border = [&widths] {
        std::string line = "+";
        for (const auto cell_width : widths) {
            line += std::string(cell_width + 2, '-');
            line += '+';
        }
        return line;
    };

    auto cell_lines = [](std::string_view cell, std::size_t cell_width) {
        auto wrapped = wrap_reflow(cell, "", "", cell_width);
        if (wrapped.empty()) {
            wrapped.emplace_back("");
        }
        return wrapped;
    };

    auto align_cell = [](std::string text, std::size_t cell_width,
                         ColumnAlignment alignment) {
        const auto cell_display_width = safe_width(text);
        const auto extra = cell_width > cell_display_width ? cell_width - cell_display_width : 0;
        std::size_t left_padding = 0;
        std::size_t right_padding = extra;
        if (alignment == ColumnAlignment::right) {
            left_padding = extra;
            right_padding = 0;
        } else if (alignment == ColumnAlignment::center) {
            left_padding = extra / 2U;
            right_padding = extra - left_padding;
        }
        return std::string(left_padding, ' ') + std::move(text) +
               std::string(right_padding, ' ');
    };

    std::vector<std::string> lines;
    lines.push_back(border());
    for (std::size_t row_index = 0; row_index < rows.size(); ++row_index) {
        std::vector<std::vector<std::string>> wrapped_cells;
        std::size_t row_height = 1;
        for (std::size_t cell_index = 0; cell_index < widths.size(); ++cell_index) {
            const auto cell = cell_index < rows[row_index].size() ? rows[row_index][cell_index] : "";
            auto wrapped = cell_lines(cell, widths[cell_index]);
            row_height = std::max(row_height, wrapped.size());
            wrapped_cells.push_back(std::move(wrapped));
        }

        for (std::size_t line_index = 0; line_index < row_height; ++line_index) {
            std::string line = "|";
            for (std::size_t cell_index = 0; cell_index < widths.size(); ++cell_index) {
                const auto cell =
                    line_index < wrapped_cells[cell_index].size() ? wrapped_cells[cell_index][line_index]
                                                                  : "";
                const auto alignment = cell_index < block.alignments.size()
                                           ? block.alignments[cell_index]
                                           : ColumnAlignment::left;
                const auto aligned = align_cell(cell, widths[cell_index], alignment);
                line += ' ';
                line += aligned;
                line += " |";
            }
            lines.push_back(std::move(line));
        }
        if (row_index == 0 || row_index + 1 < rows.size()) {
            lines.push_back(border());
        }
    }

    for (auto& line : lines) {
        line = truncate_ansi(line, width);
    }
    return lines;
}

std::vector<std::string> code_lines_from_text(std::string_view text) {
    auto lines = split_lines(text);
    if (!lines.empty() && lines.back().empty() && !text.empty() && text.back() == '\n') {
        lines.pop_back();
    }
    return lines;
}

void collect_table_rows(const MdNode& node, std::vector<std::vector<std::string>>& rows,
                        std::vector<ColumnAlignment>& alignments) {
    if (node.type == MdNodeType::table_row) {
        std::vector<std::string> row;
        for (const auto& cell : node.children) {
            if (cell->type != MdNodeType::table_cell) {
                continue;
            }
            if (rows.empty()) {
                alignments.push_back(cell->alignment);
            }
            row.push_back(trim(cell->text));
        }
        rows.push_back(std::move(row));
        return;
    }

    for (const auto& child : node.children) {
        collect_table_rows(*child, rows, alignments);
    }
}

std::vector<std::string> render_table_rows(const std::vector<std::vector<std::string>>& rows,
                                           const std::vector<ColumnAlignment>& alignments,
                                           std::size_t width) {
    if (rows.empty()) {
        return {};
    }

    std::vector<std::size_t> widths;
    for (const auto& row : rows) {
        if (widths.size() < row.size()) {
            widths.resize(row.size(), 0);
        }
        for (std::size_t index = 0; index < row.size(); ++index) {
            widths[index] = std::max(widths[index], safe_width(row[index]));
        }
    }

    if (widths.empty()) {
        return {};
    }

    const std::size_t minimum_table_width = widths.size() * 3U + 1U;
    if (width > minimum_table_width) {
        const std::size_t available_content_width = width - minimum_table_width;
        while (std::accumulate(widths.begin(), widths.end(), std::size_t{0}) >
               available_content_width) {
            auto largest = std::max_element(widths.begin(), widths.end());
            if (largest == widths.end() || *largest <= 1U) {
                break;
            }
            --*largest;
        }
    }

    auto border = [&widths] {
        std::string line = "+";
        for (const auto cell_width : widths) {
            line += std::string(cell_width + 2, '-');
            line += '+';
        }
        return line;
    };

    auto align_cell = [](std::string text, std::size_t cell_width,
                         ColumnAlignment alignment) {
        const auto cell_display_width = safe_width(text);
        const auto extra = cell_width > cell_display_width ? cell_width - cell_display_width : 0;
        std::size_t left_padding = 0;
        std::size_t right_padding = extra;
        if (alignment == ColumnAlignment::right) {
            left_padding = extra;
            right_padding = 0;
        } else if (alignment == ColumnAlignment::center) {
            left_padding = extra / 2U;
            right_padding = extra - left_padding;
        }
        return std::string(left_padding, ' ') + std::move(text) +
               std::string(right_padding, ' ');
    };

    std::vector<std::string> lines;
    lines.push_back(border());
    for (std::size_t row_index = 0; row_index < rows.size(); ++row_index) {
        std::vector<std::vector<std::string>> wrapped_cells;
        std::size_t row_height = 1;
        for (std::size_t cell_index = 0; cell_index < widths.size(); ++cell_index) {
            const auto cell = cell_index < rows[row_index].size() ? rows[row_index][cell_index] : "";
            auto wrapped = wrap_reflow(cell, "", "", widths[cell_index]);
            if (wrapped.empty()) {
                wrapped.emplace_back("");
            }
            row_height = std::max(row_height, wrapped.size());
            wrapped_cells.push_back(std::move(wrapped));
        }

        for (std::size_t line_index = 0; line_index < row_height; ++line_index) {
            std::string line = "|";
            for (std::size_t cell_index = 0; cell_index < widths.size(); ++cell_index) {
                const auto cell =
                    line_index < wrapped_cells[cell_index].size() ? wrapped_cells[cell_index][line_index]
                                                                  : "";
                const auto alignment = cell_index < alignments.size()
                                           ? alignments[cell_index]
                                           : ColumnAlignment::left;
                line += ' ';
                line += align_cell(cell, widths[cell_index], alignment);
                line += " |";
            }
            lines.push_back(std::move(line));
        }
        if (row_index == 0 || row_index + 1 < rows.size()) {
            lines.push_back(border());
        }
    }

    for (auto& line : lines) {
        line = truncate_ansi(line, width);
    }
    return lines;
}

std::vector<std::string> render_md4c_node(const MdNode& node, std::size_t width,
                                          std::size_t list_depth = 0);

std::vector<std::string> render_md4c_list(const MdNode& node, std::size_t width,
                                          std::size_t depth) {
    std::vector<std::string> lines;
    unsigned item_number = node.start;
    const std::string indent(depth * 4U, ' ');

    for (const auto& item_ptr : node.children) {
        const auto& item = *item_ptr;
        if (item.type != MdNodeType::list_item) {
            continue;
        }

        std::string bullet;
        if (node.ordered) {
            bullet = std::to_string(item_number++);
            bullet.push_back(node.ordered_delimiter);
            bullet.push_back(' ');
        } else {
            bullet = "- ";
        }
        if (item.task) {
            bullet += "[";
            bullet.push_back(item.task_mark == 'x' || item.task_mark == 'X' ? 'x' : ' ');
            bullet += "] ";
        }

        const std::string first_prefix = indent + bullet;
        const std::string rest_prefix = indent + std::string(safe_width(bullet), ' ');
        bool rendered_any = false;

        if (!trim(item.text).empty()) {
            const auto item_text_lines = split_lines(item.text);
            std::vector<std::string> paragraph_lines;
            for (const auto& item_text_line : item_text_lines) {
                const auto line = trim(item_text_line);
                if (line.empty()) {
                    continue;
                }
                paragraph_lines.push_back(line);
            }

            if (!paragraph_lines.empty()) {
                auto wrapped = wrap_reflow(join_with_spaces(paragraph_lines), first_prefix, rest_prefix, width);
                lines.insert(lines.end(), wrapped.begin(), wrapped.end());
                rendered_any = true;
            }
        }

        for (const auto& child_ptr : item.children) {
            const auto& child = *child_ptr;
            if (child.type == MdNodeType::paragraph) {
                auto wrapped = wrap_reflow(child.text,
                                           rendered_any ? rest_prefix : first_prefix,
                                           rest_prefix, width);
                lines.insert(lines.end(), wrapped.begin(), wrapped.end());
                rendered_any = true;
                continue;
            }
            if (child.type == MdNodeType::list) {
                auto nested = render_md4c_list(child, width, depth + 1);
                lines.insert(lines.end(), nested.begin(), nested.end());
                rendered_any = true;
                continue;
            }
            if (child.type == MdNodeType::blockquote) {
                auto quote = render_md4c_node(child, width - std::min(width, rest_prefix.size()));
                for (auto& quote_line : quote) {
                    const auto& prefix = rendered_any ? rest_prefix : first_prefix;
                    lines.push_back(prefix + quote_line);
                    rendered_any = true;
                }
                continue;
            }
            if (child.type == MdNodeType::code_block) {
                const std::string opening = "```" + child.code_info;
                auto opening_lines = wrap_verbatim(opening, rendered_any ? rest_prefix : first_prefix, width);
                lines.insert(lines.end(), opening_lines.begin(), opening_lines.end());
                for (const auto& code_line : code_lines_from_text(child.text)) {
                    auto wrapped = wrap_reflow(code_line, rest_prefix + "  ", rest_prefix, width);
                    lines.insert(lines.end(), wrapped.begin(), wrapped.end());
                }
                auto closing_lines = wrap_verbatim("```", rest_prefix, width);
                lines.insert(lines.end(), closing_lines.begin(), closing_lines.end());
                rendered_any = true;
                continue;
            }

            auto rendered = render_md4c_node(child, width, depth);
            for (auto& line : rendered) {
                const auto& prefix = rendered_any ? rest_prefix : first_prefix;
                lines.push_back(prefix + line);
                rendered_any = true;
            }
        }

        if (!rendered_any) {
            lines.push_back(first_prefix);
        }
    }

    return lines;
}

std::vector<std::string> render_md4c_node(const MdNode& node, std::size_t width,
                                          std::size_t list_depth) {
    switch (node.type) {
    case MdNodeType::document: {
        std::vector<std::string> lines;
        for (std::size_t index = 0; index < node.children.size(); ++index) {
            auto rendered = render_md4c_node(*node.children[index], width, list_depth);
            lines.insert(lines.end(), rendered.begin(), rendered.end());
            const bool adjacent_lists =
                index + 1 < node.children.size() &&
                node.children[index]->type == MdNodeType::list &&
                node.children[index + 1]->type == MdNodeType::list;
            if (index + 1 < node.children.size() && !adjacent_lists) {
                lines.emplace_back("");
            }
        }
        return lines;
    }
    case MdNodeType::heading: {
        const std::string prefix(node.level, '#');
        return wrap_reflow(node.text, prefix + " ", prefix + " ", width);
    }
    case MdNodeType::paragraph:
    case MdNodeType::html:
        return wrap_reflow(trim(node.text), "", "", width);
    case MdNodeType::list:
        return render_md4c_list(node, width, list_depth);
    case MdNodeType::blockquote: {
        std::vector<std::string> lines;
        for (const auto& child : node.children) {
            auto rendered = render_md4c_node(*child, width > 2U ? width - 2U : 1U, list_depth);
            for (const auto& line : rendered) {
                auto wrapped = wrap_reflow(line, "\u2502 ", "\u2502 ", width);
                lines.insert(lines.end(), wrapped.begin(), wrapped.end());
            }
        }
        return lines;
    }
    case MdNodeType::code_block: {
        std::vector<std::string> lines;
        auto opening = wrap_verbatim("```" + node.code_info, "", width);
        lines.insert(lines.end(), opening.begin(), opening.end());

        const auto code_lines = code_lines_from_text(node.text);
        if (code_lines.empty()) {
            lines.emplace_back("  ");
        } else {
            for (const auto& line : code_lines) {
                auto wrapped = wrap_verbatim(line, "  ", width);
                lines.insert(lines.end(), wrapped.begin(), wrapped.end());
            }
        }

        auto closing = wrap_verbatim("```", "", width);
        lines.insert(lines.end(), closing.begin(), closing.end());
        return lines;
    }
    case MdNodeType::horizontal_rule:
        return {repeat_to_width("-", width)};
    case MdNodeType::table: {
        std::vector<std::vector<std::string>> rows;
        std::vector<ColumnAlignment> alignments;
        collect_table_rows(node, rows, alignments);
        return render_table_rows(rows, alignments, width);
    }
    case MdNodeType::list_item:
    case MdNodeType::table_head:
    case MdNodeType::table_body:
    case MdNodeType::table_row:
    case MdNodeType::table_cell:
        return wrap_reflow(trim(node.text), "", "", width);
    }

    return {};
}

[[maybe_unused]] std::vector<std::string> render_block(const Block& block, std::size_t width) {
    switch (block.type) {
    case BlockType::heading: {
        const std::string prefix(block.level, '#');
        return wrap_reflow(normalize_inline(block.lines.front()), prefix + " ", prefix + " ", width);
    }
    case BlockType::paragraph:
        return wrap_reflow(normalize_inline(block.lines.front()), "", "", width);
    case BlockType::list:
    {
        std::vector<std::string> lines;
        for (const auto& item : block.items) {
            if (item.fenced_code) {
                auto opening = wrap_verbatim(item.fence, item.first_prefix, width);
                lines.insert(lines.end(), opening.begin(), opening.end());
                for (const auto& code_line : item.code_lines) {
                    auto wrapped = wrap_reflow(code_line, item.rest_prefix + "  ",
                                               item.rest_prefix, width);
                    lines.insert(lines.end(), wrapped.begin(), wrapped.end());
                }
                auto closing = wrap_verbatim(item.closing_fence, item.rest_prefix, width);
                lines.insert(lines.end(), closing.begin(), closing.end());
                continue;
            }

            std::string quote_text;
            if (parse_blockquote(item.text, quote_text)) {
                auto wrapped = wrap_reflow(normalize_inline(quote_text), item.first_prefix + "\u2502 ",
                                           item.rest_prefix + "\u2502 ", width);
                lines.insert(lines.end(), wrapped.begin(), wrapped.end());
                continue;
            }

            auto wrapped = wrap_reflow(normalize_inline(item.text), item.first_prefix, item.rest_prefix, width);
            lines.insert(lines.end(), wrapped.begin(), wrapped.end());
        }
        return lines;
    }
    case BlockType::blockquote:
        return wrap_reflow(normalize_inline(block.lines.front()), "\u2502 ", "\u2502 ", width);
    case BlockType::code_block: {
        std::vector<std::string> lines;
        if (block.lines.empty()) {
            lines.emplace_back("    ");
            return lines;
        }
        for (const auto& line : block.lines) {
            auto wrapped = wrap_verbatim(line, "    ", width);
            lines.insert(lines.end(), wrapped.begin(), wrapped.end());
        }
        return lines;
    }
    case BlockType::horizontal_rule:
        return {repeat_to_width("-", width)};
    case BlockType::table:
        return render_table(block, width);
    }

    return {};
}

}  // namespace

Markdown::Markdown(std::string text, std::size_t padding_x, std::size_t padding_y,
                   StyleFunction background)
    : text_(std::move(text)),
      padding_x_(padding_x),
      padding_y_(padding_y),
      background_(std::move(background)) {}

foundation::Result<RenderBuffer> Markdown::render(const RenderConstraints& constraints) const {
    if (render_cache_.valid && render_cache_.revision == revision_ &&
        render_cache_.max_width == constraints.max_width &&
        render_cache_.max_height == constraints.max_height) {
        return render_cache_.buffer;
    }

    if (text_.empty() || trim(text_).empty()) {
        auto empty = make_buffer({}, constraints);
        if (!empty) {
            return empty.error();
        }
        render_cache_.valid = true;
        render_cache_.revision = revision_;
        render_cache_.max_width = constraints.max_width;
        render_cache_.max_height = constraints.max_height;
        render_cache_.buffer = std::move(empty.value());
        return render_cache_.buffer;
    }

    const std::string normalized_text = normalize_tabs(text_);
    const std::size_t horizontal_padding = padding_x_ * 2U;
    const std::size_t content_width =
        constraints.max_width > horizontal_padding ? constraints.max_width - horizontal_padding : 1U;

    std::vector<std::string> lines;
    const auto document = parse_md4c_document(normalized_text);
    if (!document) {
        return foundation::Result<RenderBuffer>::failure(
            foundation::ErrorCode::invalid_argument, "failed to collect markdown blocks with md4c");
    }

    ReflowContext reflow_context{&reflow_cache_, 0};
    struct ReflowContextScope {
        ReflowContext* previous = nullptr;
        explicit ReflowContextScope(ReflowContext& current)
            : previous(active_reflow_context) { active_reflow_context = &current; }
        ~ReflowContextScope() { active_reflow_context = previous; }
    } reflow_scope(reflow_context);
    std::vector<std::string> content_lines = render_md4c_node(*document, content_width);
    reflow_cache_.resize(reflow_context.ordinal);
    if (content_lines.empty()) {
        content_lines.emplace_back("");
    }

    const std::string empty_line(constraints.max_width, ' ');
    for (std::size_t index = 0; index < padding_y_; ++index) {
        lines.push_back(empty_line);
    }

    const std::string left_padding(padding_x_, ' ');
    const std::string right_padding(padding_x_, ' ');
    for (auto& line : content_lines) {
        lines.push_back(left_padding + std::move(line) + right_padding);
    }

    for (std::size_t index = 0; index < padding_y_; ++index) {
        lines.push_back(empty_line);
    }

    if (constraints.max_height < lines.size()) {
        lines.resize(constraints.max_height);
    }
    for (auto& line : lines) {
        auto clamped = clamp_line(line, constraints.max_width);
        if (!clamped) {
            return clamped.error();
        }
        line = background_ ? background_(clamped.value()) : std::move(clamped.value());
    }

    render_cache_.valid = true;
    render_cache_.revision = revision_;
    render_cache_.max_width = constraints.max_width;
    render_cache_.max_height = constraints.max_height;
    render_cache_.buffer = RenderBuffer{constraints.max_width, std::move(lines)};
    return render_cache_.buffer;
}

void Markdown::set_text(std::string text) {
    if (text_ == text) {
        return;
    }
    text_ = std::move(text);
    ++revision_;
    render_cache_.valid = false;
}

void Markdown::set_padding(std::size_t padding_x, std::size_t padding_y) noexcept {
    if (padding_x_ == padding_x && padding_y_ == padding_y) {
        return;
    }
    padding_x_ = padding_x;
    padding_y_ = padding_y;
    ++revision_;
    render_cache_.valid = false;
}

void Markdown::set_background(StyleFunction background) {
    background_ = std::move(background);
    ++revision_;
    render_cache_.valid = false;
}

const std::string& Markdown::text() const noexcept { return text_; }

}  // namespace qt::tui
