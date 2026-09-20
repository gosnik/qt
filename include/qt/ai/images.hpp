#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "qt/ai/model_catalog.hpp"
#include "qt/ai/models.hpp"
#include "qt/ai/provider_transport.hpp"

namespace qt::ai {

struct ImageDataUrl {
  std::string media_type;
  std::string data;
};

struct ImageMetadata {
  std::string media_type;
  std::size_t byte_size = 0;
  std::optional<std::uint32_t> width;
  std::optional<std::uint32_t> height;
  std::optional<std::uint16_t> exif_orientation;
};

struct ImageResizePlanOptions {
  std::uint32_t max_width = 2000U;
  std::uint32_t max_height = 2000U;
  std::size_t max_base64_bytes = 4718592U;
  std::optional<std::string> target_media_type;
};

struct ImageResizePlan {
  ImageMetadata original;
  std::size_t original_base64_bytes = 0;
  std::optional<std::uint32_t> target_width;
  std::optional<std::uint32_t> target_height;
  std::string output_media_type;
  bool dimensions_exceed_limit = false;
  bool payload_exceeds_limit = false;
  bool format_conversion_required = false;
  bool pixel_transform_required = false;
  bool native_pixel_transform_available = false;
};

enum class ImageConversionStatus {
  not_required,
  converted,
  unsupported,
};

struct ImageConversionDecision {
  ImageResizePlan plan;
  std::string input_media_type;
  std::string output_media_type;
  bool passthrough = false;
  bool resize_required = false;
  bool format_conversion_required = false;
  bool supported_input_format = false;
  bool supported_output_format = false;
  ImageConversionStatus status = ImageConversionStatus::unsupported;
  std::string reason;
};

struct ImageConversionResult {
  ImageConversionDecision decision;
  std::optional<ImageContent> image;
  std::optional<std::string> dimension_note;
  std::string error;
};

struct OpenRouterImageHttpOptions {
  std::string api_key;
  std::map<std::string, std::string> headers;
  std::optional<std::int64_t> timeout_ms;
  std::optional<int> max_retries;
  std::optional<std::int64_t> max_retry_delay_ms;
  std::optional<std::string> proxy_url;
  bool aborted = false;
};

struct OpenRouterImageGenerationResult {
  std::string api;
  std::string provider;
  std::string model;
  std::vector<Content> output;
  StopReason stop_reason = StopReason::end_turn;
  std::optional<std::string> response_id;
  std::optional<Usage> usage;
  std::optional<std::string> error_message;
  int status = 0;
  std::map<std::string, std::string> headers;
};

class ImageConverter {
 public:
  virtual ~ImageConverter() = default;

  virtual ImageConversionResult convert(
      const ImageContent& content,
      const ImageResizePlanOptions& options = {}) const = 0;
};

std::optional<ImageDataUrl> parse_image_data_url(const std::string& value);
std::string image_data_url(const ImageContent& content);

std::optional<ImageMetadata> inspect_image_bytes(
    const std::vector<std::uint8_t>& bytes,
    std::optional<std::string> media_type_hint = std::nullopt);
std::optional<ImageMetadata> inspect_image_content(const ImageContent& content);

std::size_t base64_encoded_size(std::size_t byte_size);
std::optional<std::vector<std::uint8_t>> decode_image_base64(
    const std::string& value);
std::string encode_image_base64(const std::vector<std::uint8_t>& bytes);
ImageResizePlan plan_image_resize(const ImageMetadata& metadata,
                                  std::size_t base64_bytes,
                                  const ImageResizePlanOptions& options = {});
ImageConversionDecision decide_image_conversion(
    const ImageMetadata& metadata,
    std::size_t base64_bytes,
    const ImageResizePlanOptions& options = {});
const ImageConverter& unsupported_image_converter();
std::optional<std::string> format_dimension_note(const ImageResizePlan& plan);

Json openrouter_image_request_body(const ImageModelCatalogRecord& model,
                                   const std::vector<Content>& input);
HttpRequest openrouter_image_http_request(
    const ImageModelCatalogRecord& model, const std::vector<Content>& input,
    const OpenRouterImageHttpOptions& options);
std::vector<Content> openrouter_image_output_content(const Json& message);
Usage openrouter_image_usage(const Json& raw_usage,
                             const ImageModelCatalogRecord& model);
OpenRouterImageGenerationResult parse_openrouter_image_http_response(
    const ImageModelCatalogRecord& model, const HttpResponse& response);
OpenRouterImageGenerationResult generate_openrouter_image(
    HttpTransport& transport, const ImageModelCatalogRecord& model,
    const std::vector<Content>& input,
    const OpenRouterImageHttpOptions& options);

}  // namespace qt::ai
