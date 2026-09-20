#include <qt/foundation/filesystem.hpp>
#include <qt/harness/session_repository.hpp>

#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}
}  // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 2, "expected temporary directory argument");
    const std::filesystem::path directory(argv[1]);
    const auto file = directory / "text.txt";
    const std::string content("hello\0world\n", 12);
    require(static_cast<bool>(qt::foundation::write_text_file(file, content)),
            "regular file write failed");
    const auto loaded = qt::foundation::read_text_file(file);
    require(loaded && loaded.value() == content, "file content was not preserved");
    require(static_cast<bool>(qt::foundation::write_text_file(file, "")),
            "empty file write failed");
    const auto empty = qt::foundation::read_text_file(file);
    require(empty && empty.value().empty(), "empty file read failed");
    const std::string large_content(32769, 'x');
    require(static_cast<bool>(qt::foundation::write_text_file(file, large_content)),
            "large file write failed");
    const auto large = qt::foundation::read_text_file(file);
    require(large && large.value() == large_content, "large file read was truncated");
    const auto directory_read = qt::foundation::read_text_file(directory);
    require(!directory_read && directory_read.error().code == qt::foundation::ErrorCode::io_error,
            "directory read silently returned empty content instead of an I/O error");

    qt::harness::SessionEntry entry;
    qt::harness::SessionMetadata metadata;
    metadata.session_id = "storage-probe";
    metadata.working_directory = directory.string();
    entry.data = qt::harness::SessionMetadataEntry{metadata};
    const qt::harness::SessionRepository repository((directory / "session.jsonl").string());
    require(static_cast<bool>(repository.append(entry)), "session append failed");
    require(static_cast<bool>(repository.append(entry)), "second session append failed");
    auto entries = repository.load();
    require(entries && entries.value().size() == 2, "session append lost entries");
    require(static_cast<bool>(repository.replace_all({entry})), "session replacement failed");
    entries = repository.load();
    require(entries && entries.value().size() == 1, "session replacement did not truncate");
    require(static_cast<bool>(repository.replace_all({})), "empty replacement failed");
    entries = repository.load();
    require(entries && entries.value().empty(), "empty replacement retained entries");

    require(std::filesystem::exists("/dev/full"), "probe requires /dev/full");
    const auto check_failure = [](const qt::foundation::Status& status, const char* message) {
      require(!status && status.error().code == qt::foundation::ErrorCode::io_error,
              message);
    };
    check_failure(qt::foundation::write_text_file("/dev/full", "small buffered write"),
                  "text write silently ignored disk-full error");
    const qt::harness::SessionRepository failing("/dev/full");
    check_failure(failing.append(entry), "session append silently ignored disk-full error");
    check_failure(failing.replace_all({entry}), "session replacement silently ignored disk-full error");
    std::cout << "[PASS] storage writes report buffered failures and preserve successful writes\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "[FAIL] " << error.what() << '\n';
    return 1;
  }
}
