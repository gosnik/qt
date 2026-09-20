#include "qt/app/runtime/initial_message.hpp"
#include "qt/ai/images.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace qt::app {

namespace {

constexpr std::size_t kImageSniffBytes = 16U;
constexpr const char* kImageOmittedNote =
    "[Image omitted: could not be resized below the inline image size limit.]";

std::string path_for_message(const std::filesystem::path& path) {
  return path.lexically_normal().generic_string();
}

InitialMessageDiagnostic make_diagnostic(InitialMessageDiagnosticSeverity severity,
                                         const std::filesystem::path& path,
                                         std::string message) {
  InitialMessageDiagnostic diagnostic;
  diagnostic.severity = severity;
  diagnostic.path = path_for_message(path);
  diagnostic.message = std::move(message);
  return diagnostic;
}

std::string strip_file_argument_marker(const std::string& file_arg) {
  if (!file_arg.empty() && file_arg.front() == '@') {
    return file_arg.substr(1U);
  }
  return file_arg;
}

std::filesystem::path expand_user_home(const std::string& path) {
  if (path == "~" || path.rfind("~/", 0U) == 0U) {
    const char* home = std::getenv("HOME");
    if (home != nullptr && *home != '\0') {
      if (path.size() == 1U) {
        return std::filesystem::path(home);
      }
      return std::filesystem::path(home) / path.substr(2U);
    }
  }
  return std::filesystem::path(path);
}

std::filesystem::path resolve_file_argument(const std::string& file_arg,
                                            const std::filesystem::path& cwd) {
  std::filesystem::path path = expand_user_home(strip_file_argument_marker(file_arg));
  if (path.is_relative()) {
    path = cwd / path;
  }

  std::error_code error;
  const auto absolute = std::filesystem::weakly_canonical(path, error);
  if (!error) {
    return absolute;
  }
  return std::filesystem::absolute(path, error);
}

std::optional<std::string> read_file_bytes(const std::filesystem::path& path,
                                           std::string& output) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return "Could not read file: " + path_for_message(path);
  }

  std::ostringstream stream;
  stream << input.rdbuf();
  if (!input.good() && !input.eof()) {
    return "Could not read file: " + path_for_message(path);
  }
  output = stream.str();
  return std::nullopt;
}

std::string base64_encode(const std::string& data) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  std::string output;
  output.reserve(((data.size() + 2U) / 3U) * 4U);

  std::uint32_t value = 0U;
  int bits = -6;
  for (const unsigned char byte : data) {
    value = (value << 8U) | byte;
    bits += 8;
    while (bits >= 0) {
      output.push_back(kAlphabet[(value >> bits) & 0x3FU]);
      bits -= 6;
    }
  }
  if (bits > -6) {
    output.push_back(kAlphabet[((value << 8U) >> (bits + 8)) & 0x3FU]);
  }
  while (output.size() % 4U != 0U) {
    output.push_back('=');
  }
  return output;
}

bool starts_with(const std::vector<unsigned char>& bytes,
                 std::initializer_list<unsigned char> prefix) {
  if (bytes.size() < prefix.size()) {
    return false;
  }
  return std::equal(prefix.begin(), prefix.end(), bytes.begin());
}

void append_utf8_replacement(std::string& value) { value += "\xEF\xBF\xBD"; }

bool is_utf8_continuation(unsigned char ch) {
  return (ch & 0xC0U) == 0x80U;
}

std::string sanitize_text_file_content(std::string_view value) {
  std::string result;
  result.reserve(value.size());

  for (std::size_t i = 0; i < value.size();) {
    const auto c0 = static_cast<unsigned char>(value[i]);
    if (c0 < 0x80U) {
      result.push_back(static_cast<char>(c0));
      ++i;
      continue;
    }

    std::size_t length = 0;
    bool valid = false;
    if (c0 >= 0xC2U && c0 <= 0xDFU) {
      length = 2;
      valid = i + 1 < value.size() &&
              is_utf8_continuation(static_cast<unsigned char>(value[i + 1]));
    } else if (c0 >= 0xE0U && c0 <= 0xEFU) {
      length = 3;
      if (i + 2 < value.size()) {
        const auto c1 = static_cast<unsigned char>(value[i + 1]);
        const auto c2 = static_cast<unsigned char>(value[i + 2]);
        valid = is_utf8_continuation(c1) && is_utf8_continuation(c2) &&
                (c0 != 0xE0U || c1 >= 0xA0U) &&
                (c0 != 0xEDU || c1 < 0xA0U);
      }
    } else if (c0 >= 0xF0U && c0 <= 0xF4U) {
      length = 4;
      if (i + 3 < value.size()) {
        const auto c1 = static_cast<unsigned char>(value[i + 1]);
        const auto c2 = static_cast<unsigned char>(value[i + 2]);
        const auto c3 = static_cast<unsigned char>(value[i + 3]);
        valid = is_utf8_continuation(c1) && is_utf8_continuation(c2) &&
                is_utf8_continuation(c3) &&
                (c0 != 0xF0U || c1 >= 0x90U) &&
                (c0 != 0xF4U || c1 <= 0x8FU);
      }
    }

    if (valid) {
      result.append(value.substr(i, length));
      i += length;
    } else {
      append_utf8_replacement(result);
      ++i;
    }
  }

  return result;
}

std::string format_resize_limit(std::uint32_t width, std::uint32_t height) {
  return std::to_string(width) + "x" + std::to_string(height);
}

std::string format_large_image_note(const qt::ai::ImageMetadata& metadata,
                                    const ProcessInitialMessageFilesRequest& request) {
  if (metadata.width && metadata.height) {
    return std::string("[Image: original ") +
           format_resize_limit(*metadata.width, *metadata.height) +
           " exceeds native auto-resize target " +
           format_resize_limit(request.max_image_width,
                               request.max_image_height) +
           "; sent without resize.]";
  }
  return "[Image: sent without resize.]";
}

std::string oversized_image_diagnostic(const qt::ai::ImageMetadata& metadata,
                                       const ProcessInitialMessageFilesRequest& request,
                                       std::size_t encoded_size) {
  std::string message =
      "Image omitted because native startup cannot yet auto-resize ";
  if (metadata.width && metadata.height) {
    message += "the original " +
               format_resize_limit(*metadata.width, *metadata.height) +
               " image to fit within ";
  } else {
    message += "the image to fit within ";
  }

  message += "the configured auto-resize limits (" +
             format_resize_limit(request.max_image_width,
                                 request.max_image_height) +
             ", " + std::to_string(request.max_image_base64_bytes) +
             " base64 bytes; observed " + std::to_string(encoded_size) +
             " bytes).";
  return message;
}

}  // namespace

bool ProcessedInitialMessageFiles::ok() const noexcept {
  return std::none_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
    return diagnostic.severity == InitialMessageDiagnosticSeverity::error;
  });
}

std::optional<std::string> detect_supported_initial_image_media_type(
    const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return std::nullopt;
  }

  std::array<char, kImageSniffBytes> buffer{};
  input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
  const auto count = static_cast<std::size_t>(input.gcount());
  if (count == 0U) {
    return std::nullopt;
  }

  std::vector<unsigned char> bytes;
  bytes.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    bytes.push_back(static_cast<unsigned char>(buffer[index]));
  }

  if (starts_with(bytes, {0x89U, 'P', 'N', 'G', '\r', '\n', 0x1AU, '\n'})) {
    return "image/png";
  }
  if (starts_with(bytes, {0xFFU, 0xD8U, 0xFFU})) {
    return "image/jpeg";
  }
  if (starts_with(bytes, {'G', 'I', 'F', '8', '7', 'a'}) ||
      starts_with(bytes, {'G', 'I', 'F', '8', '9', 'a'})) {
    return "image/gif";
  }
  if (bytes.size() >= 12U && starts_with(bytes, {'R', 'I', 'F', 'F'}) &&
      bytes[8] == 'W' && bytes[9] == 'E' && bytes[10] == 'B' && bytes[11] == 'P') {
    return "image/webp";
  }

  return std::nullopt;
}

ProcessedInitialMessageFiles process_initial_message_file_arguments(
    const ProcessInitialMessageFilesRequest& request) {
  ProcessedInitialMessageFiles result;

  for (const auto& file_arg : request.file_args) {
    const auto path = resolve_file_argument(file_arg, request.cwd);

    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (error || !std::filesystem::exists(status)) {
      result.diagnostics.push_back(make_diagnostic(
          InitialMessageDiagnosticSeverity::error, path,
          "File not found: " + path_for_message(path)));
      continue;
    }
    if (!std::filesystem::is_regular_file(status)) {
      result.diagnostics.push_back(make_diagnostic(
          InitialMessageDiagnosticSeverity::error, path,
          "File is not a regular file: " + path_for_message(path)));
      continue;
    }

    const auto size = std::filesystem::file_size(path, error);
    if (error) {
      result.diagnostics.push_back(make_diagnostic(
          InitialMessageDiagnosticSeverity::error, path,
          "Could not stat file: " + path_for_message(path)));
      continue;
    }
    if (size == 0U) {
      continue;
    }

    std::string content;
    if (auto read_error = read_file_bytes(path, content)) {
      result.diagnostics.push_back(make_diagnostic(
          InitialMessageDiagnosticSeverity::error, path, *read_error));
      continue;
    }

    const auto media_type = detect_supported_initial_image_media_type(path);
    if (media_type) {
      const std::vector<unsigned char> image_bytes(content.begin(), content.end());
      const auto metadata = qt::ai::inspect_image_bytes(image_bytes, *media_type);
      const auto encoded_size = qt::ai::base64_encoded_size(image_bytes.size());
      const auto resize_plan =
          metadata ? std::optional<qt::ai::ImageResizePlan>(qt::ai::plan_image_resize(
                         *metadata, encoded_size,
                         qt::ai::ImageResizePlanOptions{
                             request.max_image_width,
                             request.max_image_height,
                             request.max_image_base64_bytes,
                             std::string("image/png")}))
                   : std::nullopt;
      const bool payload_exceeds_limit =
          request.auto_resize_images && resize_plan.has_value() &&
          resize_plan->payload_exceeds_limit;
      const bool dimensions_exceed_limit =
          request.auto_resize_images && resize_plan.has_value() &&
          resize_plan->dimensions_exceed_limit;
      const bool format_conversion_required =
          request.auto_resize_images && resize_plan.has_value() &&
          resize_plan->format_conversion_required;

      if (payload_exceeds_limit || dimensions_exceed_limit ||
          format_conversion_required) {
        std::optional<std::string> native_conversion_error;
        if (resize_plan.has_value() &&
            resize_plan->native_pixel_transform_available) {
          qt::ai::ImageContent original;
          original.media_type = *media_type;
          original.data = base64_encode(content);
          const auto conversion =
              qt::ai::unsupported_image_converter().convert(
                  original,
                  qt::ai::ImageResizePlanOptions{
                      request.max_image_width,
                      request.max_image_height,
                      request.max_image_base64_bytes,
                      std::string("image/png")});
          if (conversion.image.has_value()) {
            InitialMessageImage image;
            image.path = path_for_message(path);
            image.media_type = conversion.image->media_type;
            image.content = *conversion.image;
            image.processing_plan = conversion.decision.plan;
            result.images.push_back(std::move(image));
            result.text += "<file name=\"" + path_for_message(path) + "\">";
            if (conversion.dimension_note.has_value()) {
              result.text += *conversion.dimension_note;
            }
            result.text += "</file>\n";
            continue;
          }
          if (!conversion.error.empty()) {
            native_conversion_error = conversion.error;
          }
        }

        if (!payload_exceeds_limit && dimensions_exceed_limit && metadata.has_value()) {
          InitialMessageImage image;
          image.path = path_for_message(path);
          image.media_type = *media_type;
          image.content.media_type = *media_type;
          image.content.data = base64_encode(content);
          image.processing_plan = resize_plan;
          result.images.push_back(std::move(image));

          result.diagnostics.push_back(make_diagnostic(
              InitialMessageDiagnosticSeverity::warning, path,
              std::string("Image exceeds the configured auto-resize dimensions (") +
                  format_resize_limit(*metadata->width, *metadata->height) +
                  " > " +
                  format_resize_limit(request.max_image_width,
                                      request.max_image_height) +
                  "), but native startup currently sends the original image bytes "
                  "because " +
                  native_conversion_error.value_or(
                      "pixel resize/format conversion is not implemented in "
                      "this path") +
                  "."));
          result.text += "<file name=\"" + path_for_message(path) + "\">" +
                         format_large_image_note(*metadata, request) + "</file>\n";
          continue;
        }

        const auto diagnostic_message =
            metadata ? oversized_image_diagnostic(*metadata, request, encoded_size)
                     : std::string("Image omitted because native startup cannot yet "
                                   "auto-resize the image below the configured "
                                   "inline image limits.");
        result.diagnostics.push_back(make_diagnostic(
            InitialMessageDiagnosticSeverity::warning, path,
            diagnostic_message));
        result.text +=
            "<file name=\"" + path_for_message(path) + "\">" + kImageOmittedNote +
            "</file>\n";
        continue;
      }

      InitialMessageImage image;
      image.path = path_for_message(path);
      image.media_type = *media_type;
      image.content.media_type = *media_type;
      image.content.data = base64_encode(content);
      image.processing_plan = resize_plan;
      result.images.push_back(std::move(image));
      result.text += "<file name=\"" + path_for_message(path) + "\"></file>\n";
      continue;
    }

    result.text += "<file name=\"" + path_for_message(path) + "\">\n";
    result.text += sanitize_text_file_content(content);
    result.text += "\n</file>\n";
  }

  return result;
}

InitialMessageResult build_initial_message(const InitialMessageInput& input) {
  InitialMessageResult result;
  result.initial_images = input.file_images;
  result.diagnostics = input.diagnostics;

  std::vector<std::string> parts;
  if (input.stdin_content.has_value()) {
    parts.push_back(*input.stdin_content);
  }
  if (!input.file_text.empty()) {
    parts.push_back(input.file_text);
  }
  if (!input.messages.empty()) {
    parts.push_back(input.messages.front());
    result.remaining_messages.assign(input.messages.begin() + 1, input.messages.end());
  }

  if (!parts.empty()) {
    std::string combined;
    for (const auto& part : parts) {
      combined += part;
    }
    result.initial_message = std::move(combined);
  } else {
    result.remaining_messages = input.messages;
  }

  return result;
}

InitialMessageResult prepare_initial_message(
    const PrepareInitialMessageRequest& request) {
  const auto files = process_initial_message_file_arguments(
      ProcessInitialMessageFilesRequest{request.file_args,
                                        request.cwd,
                                        request.auto_resize_images,
                                        request.max_image_width,
                                        request.max_image_height,
                                        request.max_image_base64_bytes});

  InitialMessageInput input;
  input.messages = request.messages;
  input.stdin_content = request.stdin_content;
  input.file_text = files.text;
  input.file_images = files.images;
  input.diagnostics = files.diagnostics;
  return build_initial_message(input);
}

}  // namespace qt::app
