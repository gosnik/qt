#pragma once

#include "qt/ai/images.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace qt::app {

enum class InitialMessageDiagnosticSeverity {
  info,
  warning,
  error,
};

struct InitialMessageDiagnostic {
  InitialMessageDiagnosticSeverity severity = InitialMessageDiagnosticSeverity::info;
  std::string path;
  std::string message;
};

struct InitialMessageImage {
  std::string path;
  std::string media_type;
  ai::ImageContent content;
  std::optional<ai::ImageResizePlan> processing_plan;
};

struct ProcessInitialMessageFilesRequest {
  std::vector<std::string> file_args;
  std::filesystem::path cwd = std::filesystem::current_path();
  bool auto_resize_images = true;
  std::uint32_t max_image_width = 2000U;
  std::uint32_t max_image_height = 2000U;
  std::size_t max_image_base64_bytes = 4718592U;
};

struct ProcessedInitialMessageFiles {
  std::string text;
  std::vector<InitialMessageImage> images;
  std::vector<InitialMessageDiagnostic> diagnostics;

  [[nodiscard]] bool ok() const noexcept;
};

struct InitialMessageInput {
  std::vector<std::string> messages;
  std::optional<std::string> stdin_content;
  std::string file_text;
  std::vector<InitialMessageImage> file_images;
  std::vector<InitialMessageDiagnostic> diagnostics;
};

struct InitialMessageResult {
  std::optional<std::string> initial_message;
  std::vector<InitialMessageImage> initial_images;
  std::vector<std::string> remaining_messages;
  std::vector<InitialMessageDiagnostic> diagnostics;
};

struct PrepareInitialMessageRequest {
  std::vector<std::string> messages;
  std::vector<std::string> file_args;
  std::optional<std::string> stdin_content;
  std::filesystem::path cwd = std::filesystem::current_path();
  bool auto_resize_images = true;
  std::uint32_t max_image_width = 2000U;
  std::uint32_t max_image_height = 2000U;
  std::size_t max_image_base64_bytes = 4718592U;
};

std::optional<std::string> detect_supported_initial_image_media_type(
    const std::filesystem::path& path);

ProcessedInitialMessageFiles process_initial_message_file_arguments(
    const ProcessInitialMessageFilesRequest& request);

InitialMessageResult build_initial_message(const InitialMessageInput& input);

InitialMessageResult prepare_initial_message(
    const PrepareInitialMessageRequest& request);

}  // namespace qt::app
