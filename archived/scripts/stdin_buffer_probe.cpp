#include "qt/tui/input/stdin_buffer.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using qt::tui::InputSequenceKind;
using qt::tui::StdinBuffer;

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

double push_plain_text(std::size_t size) {
  StdinBuffer buffer;
  const std::string input(size, 'x');
  const auto started = std::chrono::steady_clock::now();
  buffer.push(input);
  const auto completed = buffer.drain();
  const auto elapsed = std::chrono::steady_clock::now() - started;

  require(completed.size() == size,
          "plain input did not preserve per-byte sequence delivery");
  require(buffer.empty(), "plain input left pending bytes");
  for (const auto& sequence : completed) {
    require(sequence.kind == InputSequenceKind::text && sequence.bytes == "x",
            "plain input sequence changed");
  }
  return std::chrono::duration<double, std::milli>(elapsed).count();
}

void verify_split_sequences() {
  StdinBuffer buffer;
  buffer.push("\x1b");
  require(buffer.pending_bytes() == "\x1b",
          "split escape prefix was not retained");
  buffer.push("[A");
  auto completed = buffer.drain();
  require(completed.size() == 1U &&
              completed.front().kind == InputSequenceKind::escape &&
              completed.front().bytes == "\x1b[A",
          "split arrow sequence changed");

  buffer.push("\x1b[200~pasted text\x1b[201~z");
  completed = buffer.drain();
  require(completed.size() == 2U &&
              completed[0].bytes == "\x1b[200~pasted text\x1b[201~" &&
              completed[1].kind == InputSequenceKind::text &&
              completed[1].bytes == "z",
          "bracketed paste boundary changed");
}

}  // namespace

int main() {
  try {
    verify_split_sequences();
    const double small_ms = push_plain_text(64U * 1024U);
    const double large_ms = push_plain_text(256U * 1024U);
    require(large_ms < 3000.0,
            "256 KiB plain input exceeded the linear-processing guard");
    std::cout << "stdin buffer probe: ok; 64KiB=" << small_ms
              << "ms 256KiB=" << large_ms << "ms\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "stdin buffer probe: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
