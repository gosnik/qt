#pragma once

#include <memory>
#include <optional>
#include <string>

namespace qt::harness {

class RepositoryView {
 public:
  virtual ~RepositoryView() = default;

  virtual std::string cwd() const = 0;
  virtual std::optional<std::string> canonicalize_path(
      const std::string& path) const = 0;
  virtual std::optional<std::string> parent_directory(
      const std::string& path) const = 0;
  virtual std::optional<std::string> find_repository_root(
      const std::string& start_path) const = 0;
  virtual bool file_exists(const std::string& path) const = 0;
  virtual bool directory_exists(const std::string& path) const = 0;
  virtual std::optional<std::string> read_text_file(
      const std::string& path) const = 0;
};

class FilesystemRepository final : public RepositoryView {
 public:
  explicit FilesystemRepository(std::string working_directory);

  std::string cwd() const override;
  std::optional<std::string> canonicalize_path(
      const std::string& path) const override;
  std::optional<std::string> parent_directory(
      const std::string& path) const override;
  std::optional<std::string> find_repository_root(
      const std::string& start_path) const override;
  bool file_exists(const std::string& path) const override;
  bool directory_exists(const std::string& path) const override;
  std::optional<std::string> read_text_file(const std::string& path) const override;

 private:
  std::string working_directory_;
};

}  // namespace qt::harness
