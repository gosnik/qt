#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace qt::tui {

enum class AnsiSequenceKind { csi, osc, apc, dcs, kitty };

struct AnsiSequence {
    AnsiSequenceKind kind;
    std::size_t length = 0;
    char final = '\0';
};

// Returns a complete terminal control sequence at offset.  Incomplete or
// unsupported ESC input deliberately returns nullopt; callers must consume
// that byte as malformed control input to guarantee forward progress.
[[nodiscard]] std::optional<AnsiSequence> extract_ansi_sequence(std::string_view text,
                                                                  std::size_t offset) noexcept;
[[nodiscard]] bool is_sgr_sequence(std::string_view text, const AnsiSequence& sequence) noexcept;
[[nodiscard]] bool is_osc8_sequence(std::string_view text, const AnsiSequence& sequence) noexcept;

}  // namespace qt::tui
