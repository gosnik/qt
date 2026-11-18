#include "qt/tui/widgets/autocomplete.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

namespace fs = std::filesystem;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

std::optional<qt::tui::AutocompleteSuggestions> await_result(
    const qt::tui::CombinedAutocompleteProvider& provider, std::uint64_t generation) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto result = provider.take_recursive_at_suggestions(generation)) return result;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return std::nullopt;
}

void verify_latest_only_async_completion() {
  const fs::path root = fs::temp_directory_path() / "qt-autocomplete-async-probe";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "nested", ec);
  std::ofstream(root / "alpha.txt") << "x";
  std::ofstream(root / "nested" / "beta.txt") << "x";

  {
    qt::tui::CombinedAutocompleteProvider provider({}, root, 20U, std::nullopt);
    const auto started = std::chrono::steady_clock::now();
    require(provider.request_recursive_at_suggestions({"@a"}, 0U, 2U, 1U),
            "attachment request was not accepted");
    require(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(50),
            "attachment request blocked the caller");
    require(provider.request_recursive_at_suggestions({"@nested/b"}, 0U, 9U, 2U),
            "replacement attachment request was not accepted");

    const auto latest = await_result(provider, 2U);
    require(latest.has_value() && !latest->items.empty(),
            "latest attachment request did not complete");
    require(!provider.take_recursive_at_suggestions(1U).has_value(),
            "superseded attachment result was published");
  }
  fs::remove_all(root, ec);
}

}  // namespace

int main() {
  try {
    verify_latest_only_async_completion();
    std::cout << "autocomplete async probe: ok\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "autocomplete async probe: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
