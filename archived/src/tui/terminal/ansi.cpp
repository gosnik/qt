#include "qt/tui/terminal/ansi.hpp"

namespace qt::tui {

std::optional<AnsiSequence> extract_ansi_sequence(std::string_view text, std::size_t offset) noexcept {
    if (offset + 1U >= text.size() || text[offset] != '\x1b') return std::nullopt;
    const char introducer = text[offset + 1U];
    if (introducer == '[') {
        for (std::size_t i = offset + 2U; i < text.size(); ++i) {
            const auto ch = static_cast<unsigned char>(text[i]);
            if (ch >= 0x40U && ch <= 0x7eU) return AnsiSequence{AnsiSequenceKind::csi, i - offset + 1U, text[i]};
        }
        return std::nullopt;
    }
    AnsiSequenceKind kind;
    switch (introducer) {
        case ']': kind = AnsiSequenceKind::osc; break;
        case '_': kind = AnsiSequenceKind::apc; break;
        case 'P': kind = AnsiSequenceKind::dcs; break;
        case 'G': kind = AnsiSequenceKind::kitty; break;
        default: return std::nullopt;
    }
    for (std::size_t i = offset + 2U; i < text.size(); ++i) {
        if (text[i] == '\a') return AnsiSequence{kind, i - offset + 1U, '\a'};
        if (text[i] == '\x1b' && i + 1U < text.size() && text[i + 1U] == '\\') {
            return AnsiSequence{kind, i - offset + 2U, '\\'};
        }
    }
    return std::nullopt;
}

bool is_sgr_sequence(std::string_view, const AnsiSequence& sequence) noexcept {
    return sequence.kind == AnsiSequenceKind::csi && sequence.final == 'm';
}

bool is_osc8_sequence(std::string_view text, const AnsiSequence& sequence) noexcept {
    return sequence.kind == AnsiSequenceKind::osc && sequence.length >= 5U &&
           text.substr(0, 4U) == "\x1b]8;";
}

}  // namespace qt::tui
