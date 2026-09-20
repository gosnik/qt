#include "qt/ai/images.hpp"

#include "qt/ai/auth_storage.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <type_traits>

#include <zlib.h>

namespace qt::ai {
namespace {

std::string lowercase_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool starts_with(const std::string& value, const std::string& prefix) {
  return value.size() >= prefix.size() &&
         std::equal(prefix.begin(), prefix.end(), value.begin());
}

std::string join_url_path(std::string base_url, const std::string& path) {
  while (!base_url.empty() && base_url.back() == '/') {
    base_url.pop_back();
  }
  return base_url + path;
}

bool equals_ascii_case_insensitive(const std::string& lhs,
                                   const std::string& rhs) {
  return lowercase_copy(lhs) == lowercase_copy(rhs);
}

std::string remove_utf8_surrogate_sequences(const std::string& value) {
  std::string sanitized;
  sanitized.reserve(value.size());
  for (std::size_t i = 0; i < value.size();) {
    const auto c0 = static_cast<unsigned char>(value[i]);
    if (c0 == 0xEDU && i + 2U < value.size()) {
      const auto c1 = static_cast<unsigned char>(value[i + 1U]);
      const auto c2 = static_cast<unsigned char>(value[i + 2U]);
      if (c1 >= 0xA0U && c1 <= 0xBFU &&
          (c2 & 0xC0U) == 0x80U) {
        i += 3U;
        continue;
      }
    }
    sanitized.push_back(value[i]);
    ++i;
  }
  return sanitized;
}

std::optional<std::vector<std::uint8_t>> decode_base64(
    const std::string& value) {
  std::array<int, 256> table{};
  table.fill(-1);
  for (int i = 0; i < 26; ++i) {
    table[static_cast<std::size_t>('A') + static_cast<std::size_t>(i)] = i;
    table[static_cast<std::size_t>('a') + static_cast<std::size_t>(i)] =
        26 + i;
  }
  for (int i = 0; i < 10; ++i) {
    table[static_cast<std::size_t>('0') + static_cast<std::size_t>(i)] =
        52 + i;
  }
  table[static_cast<std::size_t>('+')] = 62;
  table[static_cast<std::size_t>('/')] = 63;

  std::vector<std::uint8_t> output;
  int accumulator = 0;
  int bits = -8;
  bool saw_padding = false;

  for (unsigned char ch : value) {
    if (std::isspace(ch) != 0) {
      continue;
    }
    if (ch == '=') {
      saw_padding = true;
      continue;
    }
    if (saw_padding) {
      return std::nullopt;
    }

    const int decoded = table[ch];
    if (decoded < 0) {
      return std::nullopt;
    }

    accumulator = (accumulator << 6) | decoded;
    bits += 6;
    if (bits >= 0) {
      output.push_back(
          static_cast<std::uint8_t>((accumulator >> bits) & 0xff));
      bits -= 8;
    }
  }

  return output;
}

std::string encode_base64(const std::vector<std::uint8_t>& bytes) {
  static constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  std::string output;
  output.reserve(base64_encoded_size(bytes.size()));
  for (std::size_t index = 0; index < bytes.size(); index += 3U) {
    const std::uint32_t b0 = bytes[index];
    const std::uint32_t b1 = index + 1U < bytes.size() ? bytes[index + 1U] : 0U;
    const std::uint32_t b2 = index + 2U < bytes.size() ? bytes[index + 2U] : 0U;
    const std::uint32_t triple = (b0 << 16U) | (b1 << 8U) | b2;

    output.push_back(alphabet[(triple >> 18U) & 0x3fU]);
    output.push_back(alphabet[(triple >> 12U) & 0x3fU]);
    output.push_back(index + 1U < bytes.size()
                         ? alphabet[(triple >> 6U) & 0x3fU]
                         : '=');
    output.push_back(index + 2U < bytes.size() ? alphabet[triple & 0x3fU]
                                                : '=');
  }
  return output;
}

std::uint32_t read_be32(const std::vector<std::uint8_t>& bytes,
                        std::size_t offset) {
  return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
         static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::uint32_t read_le16(const std::vector<std::uint8_t>& bytes,
                        std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U);
}

std::uint32_t read_le32(const std::vector<std::uint8_t>& bytes,
                        std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

enum class ByteOrder : std::uint8_t { little_endian, big_endian };

std::uint32_t read_u16_endian(const std::vector<std::uint8_t>& bytes,
                              std::size_t offset,
                              ByteOrder byte_order) {
  if (byte_order == ByteOrder::little_endian) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U);
  }
  return (static_cast<std::uint32_t>(bytes[offset]) << 8U) |
         static_cast<std::uint32_t>(bytes[offset + 1]);
}

std::uint32_t read_u32_endian(const std::vector<std::uint8_t>& bytes,
                              std::size_t offset,
                              ByteOrder byte_order) {
  if (byte_order == ByteOrder::little_endian) {
    return read_le32(bytes, offset);
  }
  return read_be32(bytes, offset);
}

bool is_supported_converter_media_type(const std::string& media_type) {
  return equals_ascii_case_insensitive(media_type, "image/png") ||
         equals_ascii_case_insensitive(media_type, "image/jpeg") ||
         equals_ascii_case_insensitive(media_type, "image/bmp");
}

bool native_conversion_available(const ImageResizePlan& plan) {
  if (!plan.pixel_transform_required) {
    return false;
  }
  if (!equals_ascii_case_insensitive(plan.output_media_type, "image/png")) {
    return false;
  }
  return equals_ascii_case_insensitive(plan.original.media_type, "image/bmp") ||
         equals_ascii_case_insensitive(plan.original.media_type, "image/png");
}

struct RgbaImage {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgba;
};

void append_be32(std::vector<std::uint8_t>& output, std::uint32_t value) {
  output.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
  output.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
  output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
  output.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void append_png_chunk(std::vector<std::uint8_t>& output,
                      const std::array<std::uint8_t, 4>& type,
                      const std::vector<std::uint8_t>& data) {
  append_be32(output, static_cast<std::uint32_t>(data.size()));
  const std::size_t type_offset = output.size();
  output.insert(output.end(), type.begin(), type.end());
  output.insert(output.end(), data.begin(), data.end());

  uLong crc = crc32(0L, Z_NULL, 0);
  crc = crc32(crc, output.data() + type_offset,
              static_cast<uInt>(output.size() - type_offset));
  append_be32(output, static_cast<std::uint32_t>(crc));
}

std::optional<std::vector<std::uint8_t>> encode_png_rgba(
    std::uint32_t width,
    std::uint32_t height,
    const std::vector<std::uint8_t>& rgba) {
  if (width == 0U || height == 0U ||
      rgba.size() / 4U != static_cast<std::size_t>(width) *
                              static_cast<std::size_t>(height)) {
    return std::nullopt;
  }

  std::vector<std::uint8_t> filtered;
  const std::size_t row_bytes = static_cast<std::size_t>(width) * 4U;
  filtered.reserve((row_bytes + 1U) * static_cast<std::size_t>(height));
  for (std::uint32_t y = 0; y < height; ++y) {
    filtered.push_back(0U);
    const auto row_begin = rgba.begin() +
                           static_cast<std::ptrdiff_t>(
                               static_cast<std::size_t>(y) * row_bytes);
    filtered.insert(filtered.end(), row_begin,
                    row_begin + static_cast<std::ptrdiff_t>(row_bytes));
  }

  uLongf compressed_size = compressBound(static_cast<uLong>(filtered.size()));
  std::vector<std::uint8_t> compressed(compressed_size);
  const int status =
      compress2(compressed.data(), &compressed_size, filtered.data(),
                static_cast<uLong>(filtered.size()), Z_BEST_COMPRESSION);
  if (status != Z_OK) {
    return std::nullopt;
  }
  compressed.resize(compressed_size);

  std::vector<std::uint8_t> png = {0x89, 'P',  'N',  'G',  '\r',
                                   '\n', 0x1a, '\n'};
  std::vector<std::uint8_t> ihdr;
  ihdr.reserve(13U);
  append_be32(ihdr, width);
  append_be32(ihdr, height);
  ihdr.push_back(8U);
  ihdr.push_back(6U);
  ihdr.push_back(0U);
  ihdr.push_back(0U);
  ihdr.push_back(0U);
  append_png_chunk(png, {'I', 'H', 'D', 'R'}, ihdr);
  append_png_chunk(png, {'I', 'D', 'A', 'T'}, compressed);
  append_png_chunk(png, {'I', 'E', 'N', 'D'}, {});
  return png;
}

std::optional<RgbaImage> decode_bmp_rgba(
    const std::vector<std::uint8_t>& bytes) {
  if (bytes.size() < 54U || bytes[0] != 'B' || bytes[1] != 'M') {
    return std::nullopt;
  }

  const std::uint32_t pixel_offset = read_le32(bytes, 10);
  const std::uint32_t dib_header_size = read_le32(bytes, 14);
  if (dib_header_size < 40U || pixel_offset >= bytes.size()) {
    return std::nullopt;
  }

  const auto raw_width = static_cast<std::int32_t>(read_le32(bytes, 18));
  const auto raw_height = static_cast<std::int32_t>(read_le32(bytes, 22));
  if (raw_width <= 0 || raw_height == 0) {
    return std::nullopt;
  }

  const std::uint32_t planes = read_le16(bytes, 26);
  const std::uint32_t bits_per_pixel = read_le16(bytes, 28);
  const std::uint32_t compression = read_le32(bytes, 30);
  if (planes != 1U || compression != 0U ||
      (bits_per_pixel != 24U && bits_per_pixel != 32U)) {
    return std::nullopt;
  }

  const std::uint32_t width = static_cast<std::uint32_t>(raw_width);
  const std::uint32_t height = static_cast<std::uint32_t>(
      raw_height < 0 ? -static_cast<std::int64_t>(raw_height) : raw_height);
  const bool top_down = raw_height < 0;
  const std::size_t row_stride =
      ((static_cast<std::size_t>(width) * bits_per_pixel + 31U) / 32U) * 4U;
  const std::size_t pixel_bytes =
      row_stride * static_cast<std::size_t>(height);
  if (pixel_offset + pixel_bytes > bytes.size()) {
    return std::nullopt;
  }

  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) *
                                 static_cast<std::size_t>(height) * 4U);
  const std::size_t bytes_per_pixel = bits_per_pixel / 8U;
  for (std::uint32_t y = 0; y < height; ++y) {
    const std::uint32_t source_y = top_down ? y : height - 1U - y;
    const std::size_t source_row =
        static_cast<std::size_t>(pixel_offset) +
        static_cast<std::size_t>(source_y) * row_stride;
    for (std::uint32_t x = 0; x < width; ++x) {
      const std::size_t source =
          source_row + static_cast<std::size_t>(x) * bytes_per_pixel;
      const std::size_t target =
          (static_cast<std::size_t>(y) * width + x) * 4U;
      rgba[target] = bytes[source + 2U];
      rgba[target + 1U] = bytes[source + 1U];
      rgba[target + 2U] = bytes[source];
      rgba[target + 3U] = bits_per_pixel == 32U ? bytes[source + 3U] : 0xffU;
    }
  }

  return RgbaImage{width, height, std::move(rgba)};
}

std::optional<RgbaImage> decode_png_rgba(
    const std::vector<std::uint8_t>& bytes) {
  static const std::array<std::uint8_t, 8> signature = {
      0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  if (bytes.size() < 33U ||
      !std::equal(signature.begin(), signature.end(), bytes.begin())) {
    return std::nullopt;
  }

  std::size_t offset = signature.size();
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint8_t bit_depth = 0;
  std::uint8_t color_type = 0;
  std::vector<std::uint8_t> idat;
  std::vector<std::array<std::uint8_t, 4>> palette;
  std::vector<std::uint8_t> palette_alpha;

  while (offset + 12U <= bytes.size()) {
    const std::uint32_t chunk_size = read_be32(bytes, offset);
    offset += 4U;
    if (offset + 4U + chunk_size + 4U > bytes.size()) {
      return std::nullopt;
    }

    const std::array<std::uint8_t, 4> type = {
        bytes[offset], bytes[offset + 1U], bytes[offset + 2U],
        bytes[offset + 3U]};
    offset += 4U;
    const std::size_t data_offset = offset;

    if (type == std::array<std::uint8_t, 4>{'I', 'H', 'D', 'R'}) {
      if (chunk_size != 13U) {
        return std::nullopt;
      }
      width = read_be32(bytes, data_offset);
      height = read_be32(bytes, data_offset + 4U);
      bit_depth = bytes[data_offset + 8U];
      color_type = bytes[data_offset + 9U];
      const std::uint8_t compression = bytes[data_offset + 10U];
      const std::uint8_t filter = bytes[data_offset + 11U];
      const std::uint8_t interlace = bytes[data_offset + 12U];
      const bool supported_bit_depth =
          bit_depth == 8U || (bit_depth == 16U && color_type != 3U) ||
          ((color_type == 0U || color_type == 3U) &&
           (bit_depth == 1U || bit_depth == 2U || bit_depth == 4U));
      if (width == 0U || height == 0U || !supported_bit_depth ||
          compression != 0U || filter != 0U || interlace > 1U ||
          (color_type != 0U && color_type != 2U && color_type != 3U &&
           color_type != 4U && color_type != 6U)) {
        return std::nullopt;
      }
    } else if (type == std::array<std::uint8_t, 4>{'I', 'D', 'A', 'T'}) {
      idat.insert(idat.end(), bytes.begin() + static_cast<std::ptrdiff_t>(data_offset),
                  bytes.begin() + static_cast<std::ptrdiff_t>(data_offset + chunk_size));
    } else if (type == std::array<std::uint8_t, 4>{'P', 'L', 'T', 'E'}) {
      if (chunk_size == 0U || chunk_size % 3U != 0U) {
        return std::nullopt;
      }
      palette.clear();
      palette.reserve(static_cast<std::size_t>(chunk_size / 3U));
      for (std::size_t index = 0; index < chunk_size; index += 3U) {
        palette.push_back({bytes[data_offset + index],
                           bytes[data_offset + index + 1U],
                           bytes[data_offset + index + 2U],
                           0xffU});
      }
    } else if (type == std::array<std::uint8_t, 4>{'t', 'R', 'N', 'S'}) {
      palette_alpha.assign(
          bytes.begin() + static_cast<std::ptrdiff_t>(data_offset),
          bytes.begin() + static_cast<std::ptrdiff_t>(data_offset + chunk_size));
    } else if (type == std::array<std::uint8_t, 4>{'I', 'E', 'N', 'D'}) {
      break;
    }

    offset += static_cast<std::size_t>(chunk_size) + 4U;
  }

  if (width == 0U || height == 0U || idat.empty()) {
    return std::nullopt;
  }
  if (color_type == 3U) {
    if (palette.empty()) {
      return std::nullopt;
    }
    for (std::size_t index = 0; index < palette.size() &&
                                index < palette_alpha.size();
         ++index) {
      palette[index][3] = palette_alpha[index];
    }
  }

  const std::size_t sample_channels =
      (color_type == 0U || color_type == 3U)
          ? 1U
          : (color_type == 4U ? 2U : (color_type == 6U ? 4U : 3U));
  const std::size_t bits_per_pixel =
      static_cast<std::size_t>(bit_depth) * sample_channels;
  const std::size_t bytes_per_sample = bit_depth == 16U ? 2U : 1U;
  const std::size_t bytes_per_pixel =
      std::max<std::size_t>(1U, (bits_per_pixel + 7U) / 8U);
  const std::size_t sample_stride =
      bit_depth < 8U ? 0U : sample_channels * bytes_per_sample;
  const std::uint8_t interlace = bytes[28U];
  const auto pass_length = [](std::uint32_t limit, std::uint32_t start,
                              std::uint32_t step) -> std::uint32_t {
    if (limit <= start) {
      return 0U;
    }
    return (limit - start + step - 1U) / step;
  };
  const auto row_byte_count = [bits_per_pixel](std::uint32_t row_width) {
    return (static_cast<std::size_t>(row_width) * bits_per_pixel + 7U) / 8U;
  };

  std::size_t inflated_size = 0U;
  if (interlace == 0U) {
    inflated_size =
        (row_byte_count(width) + 1U) * static_cast<std::size_t>(height);
  } else {
    static constexpr std::array<std::uint32_t, 7> x_start = {0U, 4U, 0U, 2U,
                                                             0U, 1U, 0U};
    static constexpr std::array<std::uint32_t, 7> y_start = {0U, 0U, 4U, 0U,
                                                             2U, 0U, 1U};
    static constexpr std::array<std::uint32_t, 7> x_step = {8U, 8U, 4U, 4U,
                                                            2U, 2U, 1U};
    static constexpr std::array<std::uint32_t, 7> y_step = {8U, 8U, 8U, 4U,
                                                            4U, 2U, 2U};
    for (std::size_t pass = 0; pass < x_start.size(); ++pass) {
      const std::uint32_t pass_width =
          pass_length(width, x_start[pass], x_step[pass]);
      const std::uint32_t pass_height =
          pass_length(height, y_start[pass], y_step[pass]);
      if (pass_width == 0U || pass_height == 0U) {
        continue;
      }
      inflated_size += (row_byte_count(pass_width) + 1U) *
                       static_cast<std::size_t>(pass_height);
    }
  }
  std::vector<std::uint8_t> inflated(inflated_size);
  uLongf actual_size = static_cast<uLongf>(inflated.size());
  if (uncompress(inflated.data(), &actual_size, idat.data(),
                 static_cast<uLong>(idat.size())) != Z_OK ||
      actual_size != inflated.size()) {
    return std::nullopt;
  }

  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) *
                                 static_cast<std::size_t>(height) * 4U);
  struct PngRowTarget {
    std::uint32_t width;
    std::uint32_t destination_y;
    std::uint32_t destination_x_start;
    std::uint32_t destination_x_step;
  };

  struct PngDecodePass {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t destination_x_start;
    std::uint32_t destination_y_start;
    std::uint32_t destination_x_step;
    std::uint32_t destination_y_step;
  };

  const auto unfilter_row =
      [bytes_per_pixel](std::vector<std::uint8_t>& current,
                        const std::vector<std::uint8_t>& previous,
                        std::uint8_t filter_type) {
        for (std::size_t x = 0; x < current.size(); ++x) {
          const std::uint8_t left =
              x >= bytes_per_pixel ? current[x - bytes_per_pixel] : 0U;
          const std::uint8_t up = previous[x];
          const std::uint8_t up_left =
              x >= bytes_per_pixel ? previous[x - bytes_per_pixel] : 0U;
          switch (filter_type) {
            case 0:
              break;
            case 1:
              current[x] = static_cast<std::uint8_t>(current[x] + left);
              break;
            case 2:
              current[x] = static_cast<std::uint8_t>(current[x] + up);
              break;
            case 3:
              current[x] = static_cast<std::uint8_t>(
                  current[x] + static_cast<std::uint8_t>(
                                   (static_cast<int>(left) +
                                    static_cast<int>(up)) /
                                   2));
              break;
            case 4: {
              const int p = static_cast<int>(left) + static_cast<int>(up) -
                            static_cast<int>(up_left);
              const int pa = std::abs(p - static_cast<int>(left));
              const int pb = std::abs(p - static_cast<int>(up));
              const int pc = std::abs(p - static_cast<int>(up_left));
              const std::uint8_t predictor =
                  pa <= pb && pa <= pc ? left : (pb <= pc ? up : up_left);
              current[x] = static_cast<std::uint8_t>(current[x] + predictor);
              break;
            }
            default:
              return false;
          }
        }
        return true;
      };

  const auto write_row = [&](const std::vector<std::uint8_t>& current,
                             const PngRowTarget& row) {
    const auto packed_sample = [&current, bit_depth](std::uint32_t x) {
      if (bit_depth == 8U) {
        return current[static_cast<std::size_t>(x)];
      }
      const std::size_t bit_index = static_cast<std::size_t>(x) * bit_depth;
      const std::uint8_t byte = current[bit_index / 8U];
      const std::uint8_t shift =
          static_cast<std::uint8_t>(8U - bit_depth - (bit_index % 8U));
      const std::uint8_t mask =
          static_cast<std::uint8_t>((1U << bit_depth) - 1U);
      return static_cast<std::uint8_t>((byte >> shift) & mask);
    };

    for (std::uint32_t x = 0; x < row.width; ++x) {
      const std::uint32_t destination_x =
          row.destination_x_start + x * row.destination_x_step;
      const std::size_t source = static_cast<std::size_t>(x) * sample_stride;
      const std::size_t target =
          (static_cast<std::size_t>(row.destination_y) * width +
           destination_x) *
          4U;
      if (color_type == 0U) {
        std::uint8_t value = 0U;
        if (bit_depth == 16U) {
          value = current[source];
        } else {
          const std::uint8_t sample = packed_sample(x);
          value = bit_depth == 8U
                      ? sample
                      : static_cast<std::uint8_t>(
                            (static_cast<unsigned>(sample) * 255U) /
                            ((1U << bit_depth) - 1U));
        }
        rgba[target] = value;
        rgba[target + 1U] = value;
        rgba[target + 2U] = value;
        rgba[target + 3U] = 0xffU;
      } else if (color_type == 4U) {
        rgba[target] = current[source];
        rgba[target + 1U] = current[source];
        rgba[target + 2U] = current[source];
        rgba[target + 3U] =
            bit_depth == 16U ? current[source + 2U] : current[source + 1U];
      } else if (color_type == 3U) {
        const auto palette_index = packed_sample(x);
        if (palette_index >= palette.size()) {
          return false;
        }
        rgba[target] = palette[palette_index][0];
        rgba[target + 1U] = palette[palette_index][1];
        rgba[target + 2U] = palette[palette_index][2];
        rgba[target + 3U] = palette[palette_index][3];
      } else {
        rgba[target] = current[source];
        rgba[target + 1U] =
            bit_depth == 16U ? current[source + 2U] : current[source + 1U];
        rgba[target + 2U] =
            bit_depth == 16U ? current[source + 4U] : current[source + 2U];
        rgba[target + 3U] = color_type == 6U
                                ? (bit_depth == 16U ? current[source + 6U]
                                                    : current[source + 3U])
                                : 0xffU;
      }
    }
    return true;
  };

  std::size_t source_offset = 0U;
  const auto decode_pass = [&](const PngDecodePass& pass) {
    const std::size_t row_bytes = row_byte_count(pass.width);
    std::vector<std::uint8_t> previous(row_bytes, 0U);
    std::vector<std::uint8_t> current(row_bytes, 0U);
    for (std::uint32_t y = 0; y < pass.height; ++y) {
      if (source_offset + 1U + row_bytes > inflated.size()) {
        return false;
      }
      const std::uint8_t filter_type = inflated[source_offset++];
      std::copy(
          inflated.begin() + static_cast<std::ptrdiff_t>(source_offset),
          inflated.begin() +
              static_cast<std::ptrdiff_t>(source_offset + row_bytes),
          current.begin());
      source_offset += row_bytes;

      if (!unfilter_row(current, previous, filter_type)) {
        return false;
      }
      const PngRowTarget row{pass.width,
                             pass.destination_y_start +
                                 y * pass.destination_y_step,
                             pass.destination_x_start,
                             pass.destination_x_step};
      if (!write_row(current, row)) {
        return false;
      }
      previous.swap(current);
    }
    return true;
  };

  if (interlace == 0U) {
    if (!decode_pass(PngDecodePass{width, height, 0U, 0U, 1U, 1U})) {
      return std::nullopt;
    }
  } else {
    static constexpr std::array<std::uint32_t, 7> x_start = {0U, 4U, 0U, 2U,
                                                             0U, 1U, 0U};
    static constexpr std::array<std::uint32_t, 7> y_start = {0U, 0U, 4U, 0U,
                                                             2U, 0U, 1U};
    static constexpr std::array<std::uint32_t, 7> x_step = {8U, 8U, 4U, 4U,
                                                            2U, 2U, 1U};
    static constexpr std::array<std::uint32_t, 7> y_step = {8U, 8U, 8U, 4U,
                                                            4U, 2U, 2U};
    for (std::size_t pass = 0; pass < x_start.size(); ++pass) {
      const std::uint32_t pass_width =
          pass_length(width, x_start[pass], x_step[pass]);
      const std::uint32_t pass_height =
          pass_length(height, y_start[pass], y_step[pass]);
      if (pass_width == 0U || pass_height == 0U) {
        continue;
      }
      if (!decode_pass(PngDecodePass{pass_width, pass_height, x_start[pass],
                                     y_start[pass], x_step[pass],
                                     y_step[pass]})) {
        return std::nullopt;
      }
    }
  }
  if (source_offset != inflated.size()) {
    return std::nullopt;
  }

  return RgbaImage{width, height, std::move(rgba)};
}

std::optional<RgbaImage> decode_supported_rgba(
    const std::vector<std::uint8_t>& bytes,
    const std::string& media_type) {
  if (equals_ascii_case_insensitive(media_type, "image/bmp")) {
    return decode_bmp_rgba(bytes);
  }
  if (equals_ascii_case_insensitive(media_type, "image/png")) {
    return decode_png_rgba(bytes);
  }
  return std::nullopt;
}

RgbaImage resize_rgba_nearest(const RgbaImage& input,
                              std::uint32_t target_width,
                              std::uint32_t target_height) {
  if (target_width == input.width && target_height == input.height) {
    return input;
  }

  RgbaImage output;
  output.width = target_width;
  output.height = target_height;
  output.rgba.resize(static_cast<std::size_t>(target_width) *
                     static_cast<std::size_t>(target_height) * 4U);
  for (std::uint32_t y = 0; y < target_height; ++y) {
    const std::uint32_t source_y =
        static_cast<std::uint32_t>((static_cast<std::uint64_t>(y) * input.height) /
                                   target_height);
    for (std::uint32_t x = 0; x < target_width; ++x) {
      const std::uint32_t source_x =
          static_cast<std::uint32_t>((static_cast<std::uint64_t>(x) * input.width) /
                                     target_width);
      const std::size_t source =
          (static_cast<std::size_t>(source_y) * input.width + source_x) * 4U;
      const std::size_t target =
          (static_cast<std::size_t>(y) * target_width + x) * 4U;
      std::copy(input.rgba.begin() + static_cast<std::ptrdiff_t>(source),
                input.rgba.begin() + static_cast<std::ptrdiff_t>(source + 4U),
                output.rgba.begin() + static_cast<std::ptrdiff_t>(target));
    }
  }
  return output;
}

std::optional<std::vector<std::uint8_t>> convert_to_png_bytes(
    const std::vector<std::uint8_t>& bytes,
    const std::string& media_type,
    std::uint32_t target_width,
    std::uint32_t target_height) {
  const auto decoded = decode_supported_rgba(bytes, media_type);
  if (!decoded) {
    return std::nullopt;
  }

  const auto transformed =
      resize_rgba_nearest(*decoded, target_width, target_height);
  return encode_png_rgba(transformed.width, transformed.height,
                         transformed.rgba);
}

void detect_png_dimensions(const std::vector<std::uint8_t>& bytes,
                           ImageMetadata& metadata) {
  static const std::array<std::uint8_t, 8> signature = {
      0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  if (bytes.size() < 24 ||
      !std::equal(signature.begin(), signature.end(), bytes.begin())) {
    return;
  }
  metadata.media_type = "image/png";
  metadata.width = read_be32(bytes, 16);
  metadata.height = read_be32(bytes, 20);
}

void detect_gif_dimensions(const std::vector<std::uint8_t>& bytes,
                           ImageMetadata& metadata) {
  if (bytes.size() < 10) {
    return;
  }
  const bool is_gif = starts_with(std::string(bytes.begin(), bytes.begin() + 6),
                                  "GIF87a") ||
                      starts_with(std::string(bytes.begin(), bytes.begin() + 6),
                                  "GIF89a");
  if (!is_gif) {
    return;
  }
  metadata.media_type = "image/gif";
  metadata.width = read_le16(bytes, 6);
  metadata.height = read_le16(bytes, 8);
}

void detect_bmp_dimensions(const std::vector<std::uint8_t>& bytes,
                           ImageMetadata& metadata) {
  if (bytes.size() < 26 || bytes[0] != 'B' || bytes[1] != 'M') {
    return;
  }

  const std::uint32_t dib_header_size = read_le32(bytes, 14);
  if (dib_header_size == 12U) {
    if (bytes.size() < 26) {
      return;
    }
    metadata.media_type = "image/bmp";
    metadata.width = read_le16(bytes, 18);
    metadata.height = read_le16(bytes, 20);
    return;
  }

  if (dib_header_size < 40U || bytes.size() < 30) {
    return;
  }

  const auto raw_width = static_cast<std::int32_t>(read_le32(bytes, 18));
  const auto raw_height = static_cast<std::int32_t>(read_le32(bytes, 22));
  if (raw_width == 0 || raw_height == 0) {
    return;
  }

  metadata.media_type = "image/bmp";
  metadata.width = static_cast<std::uint32_t>(
      raw_width < 0 ? -static_cast<std::int64_t>(raw_width) : raw_width);
  metadata.height = static_cast<std::uint32_t>(
      raw_height < 0 ? -static_cast<std::int64_t>(raw_height) : raw_height);
}

std::optional<std::uint16_t> parse_exif_orientation(
    const std::vector<std::uint8_t>& bytes,
    std::size_t payload_offset,
    std::size_t payload_size) {
  if (payload_size < 14U || payload_offset + payload_size > bytes.size()) {
    return std::nullopt;
  }

  const std::array<std::uint8_t, 6> exif_signature = {'E', 'x', 'i',
                                                      'f', 0x00, 0x00};
  if (!std::equal(exif_signature.begin(), exif_signature.end(),
                  bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset))) {
    return std::nullopt;
  }

  const std::size_t tiff_offset = payload_offset + exif_signature.size();
  const bool little_endian = bytes[tiff_offset] == 'I' &&
                             bytes[tiff_offset + 1U] == 'I';
  const bool big_endian = bytes[tiff_offset] == 'M' &&
                          bytes[tiff_offset + 1U] == 'M';
  if (!little_endian && !big_endian) {
    return std::nullopt;
  }
  const ByteOrder byte_order =
      little_endian ? ByteOrder::little_endian : ByteOrder::big_endian;

  if (read_u16_endian(bytes, tiff_offset + 2U, byte_order) != 42U) {
    return std::nullopt;
  }

  const std::uint32_t first_ifd_offset =
      read_u32_endian(bytes, tiff_offset + 4U, byte_order);
  const std::size_t ifd_offset =
      tiff_offset + static_cast<std::size_t>(first_ifd_offset);
  if (first_ifd_offset > payload_size || ifd_offset + 2U > payload_offset + payload_size) {
    return std::nullopt;
  }

  const std::uint32_t entry_count =
      read_u16_endian(bytes, ifd_offset, byte_order);
  const std::size_t entries_offset = ifd_offset + 2U;
  const std::size_t payload_end = payload_offset + payload_size;
  if (entry_count > (payload_end - entries_offset) / 12U) {
    return std::nullopt;
  }

  for (std::uint32_t index = 0; index < entry_count; ++index) {
    const std::size_t entry_offset =
        entries_offset + static_cast<std::size_t>(index) * 12U;
    const std::uint32_t tag =
        read_u16_endian(bytes, entry_offset, byte_order);
    if (tag != 0x0112U) {
      continue;
    }

    const std::uint32_t type =
        read_u16_endian(bytes, entry_offset + 2U, byte_order);
    const std::uint32_t count =
        read_u32_endian(bytes, entry_offset + 4U, byte_order);
    if (type != 3U || count != 1U) {
      return std::nullopt;
    }

    const std::uint32_t orientation =
        read_u16_endian(bytes, entry_offset + 8U, byte_order);
    if (orientation >= 1U && orientation <= 8U) {
      return static_cast<std::uint16_t>(orientation);
    }
    return std::nullopt;
  }

  return std::nullopt;
}

bool exif_orientation_swaps_dimensions(std::uint16_t orientation) {
  return orientation >= 5U && orientation <= 8U;
}

void detect_jpeg_dimensions(const std::vector<std::uint8_t>& bytes,
                            ImageMetadata& metadata) {
  if (bytes.size() < 4 || bytes[0] != 0xff || bytes[1] != 0xd8) {
    return;
  }
  metadata.media_type = "image/jpeg";

  std::size_t offset = 2;
  std::optional<std::uint32_t> width;
  std::optional<std::uint32_t> height;
  while (offset + 9 < bytes.size()) {
    if (bytes[offset] != 0xff) {
      ++offset;
      continue;
    }
    while (offset < bytes.size() && bytes[offset] == 0xff) {
      ++offset;
    }
    if (offset >= bytes.size()) {
      break;
    }

    const auto marker = bytes[offset++];
    if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) {
      continue;
    }
    if (marker == 0xd9 || marker == 0xda) {
      break;
    }
    if (offset + 1 >= bytes.size()) {
      break;
    }

    const std::uint32_t segment_size =
        (static_cast<std::uint32_t>(bytes[offset]) << 8U) |
        static_cast<std::uint32_t>(bytes[offset + 1]);
    if (segment_size < 2 || offset + segment_size > bytes.size()) {
      break;
    }

    if (marker == 0xe1) {
      if (const auto orientation =
              parse_exif_orientation(bytes, offset + 2U, segment_size - 2U)) {
        metadata.exif_orientation = orientation;
      }
    }

    const bool is_start_of_frame =
        (marker >= 0xc0 && marker <= 0xc3) ||
        (marker >= 0xc5 && marker <= 0xc7) ||
        (marker >= 0xc9 && marker <= 0xcb) ||
        (marker >= 0xcd && marker <= 0xcf);
    if (is_start_of_frame && segment_size >= 7) {
      height =
          (static_cast<std::uint32_t>(bytes[offset + 3]) << 8U) |
          static_cast<std::uint32_t>(bytes[offset + 4]);
      width =
          (static_cast<std::uint32_t>(bytes[offset + 5]) << 8U) |
          static_cast<std::uint32_t>(bytes[offset + 6]);
    }

    offset += segment_size;
  }

  if (width && height) {
    metadata.width = width;
    metadata.height = height;
    if (metadata.exif_orientation &&
        exif_orientation_swaps_dimensions(*metadata.exif_orientation)) {
      std::swap(metadata.width, metadata.height);
    }
  }
}

class NativeImageConverter final : public ImageConverter {
 public:
  ImageConversionResult convert(
      const ImageContent& content,
      const ImageResizePlanOptions& options = {}) const override {
    const ImageContent normalized_content = normalized(content);
    const auto bytes = decode_base64(normalized_content.data);
    if (!bytes) {
      ImageConversionResult result;
      result.error = "image content is not valid base64";
      result.decision.reason = result.error;
      return result;
    }

    const auto metadata =
        inspect_image_bytes(*bytes, normalized_content.media_type);
    if (!metadata) {
      ImageConversionResult result;
      result.error = "image metadata could not be inspected";
      result.decision.reason = result.error;
      return result;
    }

    ImageConversionResult result;
    result.decision =
        decide_image_conversion(*metadata, normalized_content.data.size(),
                                options);
    if (result.decision.status == ImageConversionStatus::not_required) {
      result.image = normalized_content;
      result.dimension_note = format_dimension_note(result.decision.plan);
      return result;
    }

    if (result.decision.status == ImageConversionStatus::converted ||
        native_conversion_available(result.decision.plan)) {
      std::uint32_t target_width =
          result.decision.plan.target_width.value_or(metadata->width.value_or(1U));
      std::uint32_t target_height =
          result.decision.plan.target_height.value_or(metadata->height.value_or(1U));
      auto converted_bytes = convert_to_png_bytes(
          *bytes, metadata->media_type, target_width, target_height);
      if (!converted_bytes) {
        result.error = "native image converter could not decode image pixels";
        result.decision.status = ImageConversionStatus::unsupported;
        result.decision.reason = result.error;
        return result;
      }

      while (base64_encoded_size(converted_bytes->size()) >=
                 options.max_base64_bytes &&
             (target_width > 1U || target_height > 1U)) {
        target_width =
            target_width == 1U
                ? 1U
                : std::max<std::uint32_t>(1U, target_width * 3U / 4U);
        target_height =
            target_height == 1U
                ? 1U
                : std::max<std::uint32_t>(1U, target_height * 3U / 4U);
        converted_bytes = convert_to_png_bytes(*bytes, metadata->media_type,
                                               target_width, target_height);
        if (!converted_bytes) {
          result.error = "native image converter could not resize image pixels";
          result.decision.status = ImageConversionStatus::unsupported;
          result.decision.reason = result.error;
          return result;
        }
      }

      ImageContent converted;
      converted.media_type = "image/png";
      converted.data = encode_base64(*converted_bytes);
      if (converted.data.size() >= options.max_base64_bytes) {
        result.error =
            "converted image exceeds the inline image size limit";
        result.decision.status = ImageConversionStatus::unsupported;
        result.decision.reason = result.error;
        return result;
      }

      result.decision.plan.target_width = target_width;
      result.decision.plan.target_height = target_height;
      result.image = converted;
      result.dimension_note = format_dimension_note(result.decision.plan);
      result.error.clear();
      return result;
    }

    result.error = result.decision.reason;
    return result;
  }
};

}  // namespace

std::optional<ImageDataUrl> parse_image_data_url(const std::string& value) {
  constexpr const char* prefix = "data:";
  if (!starts_with(value, prefix)) {
    return std::nullopt;
  }

  const auto comma = value.find(',');
  if (comma == std::string::npos) {
    return std::nullopt;
  }

  const std::string metadata = value.substr(5, comma - 5);
  const auto semicolon = metadata.find(';');
  const std::string media_type =
      lowercase_copy(semicolon == std::string::npos
                         ? metadata
                         : metadata.substr(0, semicolon));
  if (media_type.empty() || media_type.find('/') == std::string::npos) {
    return std::nullopt;
  }

  bool base64 = false;
  std::size_t token_start = semicolon == std::string::npos ? metadata.size()
                                                           : semicolon + 1;
  while (token_start < metadata.size()) {
    const auto token_end = metadata.find(';', token_start);
    const std::string token =
        metadata.substr(token_start, token_end - token_start);
    if (equals_ascii_case_insensitive(token, "base64")) {
      base64 = true;
      break;
    }
    if (token_end == std::string::npos) {
      break;
    }
    token_start = token_end + 1;
  }
  if (!base64) {
    return std::nullopt;
  }

  return ImageDataUrl{media_type, value.substr(comma + 1)};
}

std::string image_data_url(const ImageContent& content) {
  if (const auto parsed = parse_image_data_url(content.data)) {
    return "data:" + parsed->media_type + ";base64," + parsed->data;
  }
  return "data:" + lowercase_copy(content.media_type) + ";base64," +
         content.data;
}

std::optional<ImageMetadata> inspect_image_bytes(
    const std::vector<std::uint8_t>& bytes,
    std::optional<std::string> media_type_hint) {
  if (bytes.empty()) {
    return std::nullopt;
  }

  ImageMetadata metadata;
  metadata.byte_size = bytes.size();
  if (media_type_hint) {
    metadata.media_type = lowercase_copy(*media_type_hint);
  }

  detect_png_dimensions(bytes, metadata);
  if (!metadata.width || !metadata.height) {
    detect_gif_dimensions(bytes, metadata);
  }
  if (!metadata.width || !metadata.height) {
    detect_bmp_dimensions(bytes, metadata);
  }
  if (!metadata.width || !metadata.height) {
    detect_jpeg_dimensions(bytes, metadata);
  }

  if (metadata.media_type.empty()) {
    return std::nullopt;
  }
  return metadata;
}

std::optional<ImageMetadata> inspect_image_content(const ImageContent& content) {
  ImageContent normalized_content = normalized(content);
  const auto bytes = decode_base64(normalized_content.data);
  if (!bytes) {
    return std::nullopt;
  }
  return inspect_image_bytes(*bytes, normalized_content.media_type);
}

std::size_t base64_encoded_size(std::size_t byte_size) {
  return ((byte_size + 2U) / 3U) * 4U;
}

std::optional<std::vector<std::uint8_t>> decode_image_base64(
    const std::string& value) {
  return decode_base64(value);
}

std::string encode_image_base64(const std::vector<std::uint8_t>& bytes) {
  return encode_base64(bytes);
}

ImageResizePlan plan_image_resize(const ImageMetadata& metadata,
                                  std::size_t base64_bytes,
                                  const ImageResizePlanOptions& options) {
  ImageResizePlan plan;
  plan.original = metadata;
  plan.original_base64_bytes = base64_bytes;
  plan.output_media_type =
      options.target_media_type ? lowercase_copy(*options.target_media_type)
                                : lowercase_copy(metadata.media_type);
  plan.payload_exceeds_limit = base64_bytes >= options.max_base64_bytes;
  plan.format_conversion_required =
      !plan.output_media_type.empty() &&
      !equals_ascii_case_insensitive(metadata.media_type, plan.output_media_type);

  if (metadata.width && metadata.height) {
    const auto max_width = std::max<std::uint32_t>(1U, options.max_width);
    const auto max_height = std::max<std::uint32_t>(1U, options.max_height);
    std::uint32_t target_width = *metadata.width;
    std::uint32_t target_height = *metadata.height;

    plan.dimensions_exceed_limit =
        target_width > max_width || target_height > max_height;

    if (target_width > max_width) {
      target_height = static_cast<std::uint32_t>(std::max(
          1.0, std::round((static_cast<double>(target_height) *
                           static_cast<double>(max_width)) /
                          static_cast<double>(target_width))));
      target_width = max_width;
    }
    if (target_height > max_height) {
      target_width = static_cast<std::uint32_t>(std::max(
          1.0, std::round((static_cast<double>(target_width) *
                           static_cast<double>(max_height)) /
                          static_cast<double>(target_height))));
      target_height = max_height;
    }

    plan.target_width = target_width;
    plan.target_height = target_height;
  }

  plan.pixel_transform_required = plan.dimensions_exceed_limit ||
                                  plan.payload_exceeds_limit ||
                                  plan.format_conversion_required;
  plan.native_pixel_transform_available = native_conversion_available(plan);
  return plan;
}

ImageConversionDecision decide_image_conversion(
    const ImageMetadata& metadata,
    std::size_t base64_bytes,
    const ImageResizePlanOptions& options) {
  ImageConversionDecision decision;
  decision.plan = plan_image_resize(metadata, base64_bytes, options);
  decision.input_media_type = lowercase_copy(metadata.media_type);
  decision.output_media_type = decision.plan.output_media_type;
  decision.resize_required =
      decision.plan.dimensions_exceed_limit || decision.plan.payload_exceeds_limit;
  decision.format_conversion_required =
      decision.plan.format_conversion_required;
  decision.supported_input_format =
      is_supported_converter_media_type(decision.input_media_type);
  decision.supported_output_format =
      is_supported_converter_media_type(decision.output_media_type);
  decision.passthrough = !decision.resize_required &&
                         !decision.format_conversion_required &&
                         !decision.plan.pixel_transform_required;

  if (decision.passthrough) {
    decision.status = ImageConversionStatus::not_required;
    decision.reason = "image already satisfies requested constraints";
    return decision;
  }

  if (native_conversion_available(decision.plan)) {
    decision.status = ImageConversionStatus::converted;
    if (decision.resize_required) {
      decision.reason =
          "native image converter can resize image pixels and encode image/png";
    } else {
      decision.reason =
          "native image converter can convert image/bmp to image/png";
    }
    return decision;
  }

  decision.status = ImageConversionStatus::unsupported;
  if (decision.resize_required) {
    decision.reason = "native image converter does not implement pixel resizing";
  } else if (decision.format_conversion_required) {
    decision.reason = "native image converter does not implement " +
                      decision.input_media_type + " to " +
                      decision.output_media_type + " conversion";
  } else if (!decision.supported_input_format ||
             !decision.supported_output_format) {
    decision.reason =
        "native image converter only records PNG, JPEG, and BMP decisions";
  } else {
    decision.reason = "native image converter cannot transform this image";
  }
  return decision;
}

const ImageConverter& unsupported_image_converter() {
  static const NativeImageConverter converter;
  return converter;
}

std::optional<std::string> format_dimension_note(const ImageResizePlan& plan) {
  if (!plan.dimensions_exceed_limit || !plan.original.width ||
      !plan.original.height || !plan.target_width || !plan.target_height) {
    return std::nullopt;
  }

  std::ostringstream scale;
  scale << std::fixed << std::setprecision(2)
        << (static_cast<double>(*plan.original.width) /
            static_cast<double>(*plan.target_width));

  return std::string("[Image: original ") +
         std::to_string(*plan.original.width) + "x" +
         std::to_string(*plan.original.height) + ", displayed at " +
         std::to_string(*plan.target_width) + "x" +
         std::to_string(*plan.target_height) +
         ". Multiply coordinates by " + scale.str() +
         " to map to original image.]";
}

Json openrouter_image_request_body(const ImageModelCatalogRecord& model,
                                   const std::vector<Content>& input) {
  Json content = Json::array();
  for (const auto& item : input) {
    std::visit(
        [&](const auto& block) {
          using T = std::decay_t<decltype(block)>;
          if constexpr (std::is_same<T, TextContent>::value) {
            content.push_back(Json{{"type", "text"},
                                   {"text",
                                    remove_utf8_surrogate_sequences(block.text)}});
          } else if constexpr (std::is_same<T, ImageContent>::value) {
            content.push_back(Json{{"type", "image_url"},
                                   {"image_url",
                                    Json{{"url", image_data_url(block)}}}});
          }
        },
        item);
  }

  Json modalities = Json::array({"image"});
  if (std::find(model.output.begin(), model.output.end(), "text") !=
      model.output.end()) {
    modalities.push_back("text");
  }

  return Json{{"model", model.id},
              {"messages",
               Json::array({Json{{"role", "user"}, {"content", content}}})},
              {"stream", false},
              {"modalities", modalities}};
}

HttpRequest openrouter_image_http_request(
    const ImageModelCatalogRecord& model, const std::vector<Content>& input,
    const OpenRouterImageHttpOptions& options) {
  HttpRequest request;
  request.method = "POST";
  request.url = join_url_path(model.base_url, "/chat/completions");
  request.headers = options.headers;
  request.headers.emplace("content-type", "application/json");
  if (!options.api_key.empty()) {
    request.headers["authorization"] = "Bearer " + options.api_key;
  }
  request.body = openrouter_image_request_body(model, input).dump();
  request.timeout_ms = options.timeout_ms;
  request.proxy_url = options.proxy_url;
  return request;
}

std::vector<Content> openrouter_image_output_content(const Json& message) {
  std::vector<Content> output;
  if (!message.is_object()) {
    return output;
  }

  if (message.contains("content") && message.at("content").is_string()) {
    const auto text = message.at("content").get<std::string>();
    if (!text.empty()) {
      output.emplace_back(TextContent{text, std::nullopt});
    }
  }

  const auto images = message.value("images", Json::array());
  if (!images.is_array()) {
    return output;
  }

  for (const auto& image : images) {
    std::optional<std::string> image_url;
    if (image.is_object() && image.contains("image_url")) {
      const auto& raw_url = image.at("image_url");
      if (raw_url.is_string()) {
        image_url = raw_url.get<std::string>();
      } else if (raw_url.is_object() && raw_url.contains("url") &&
                 raw_url.at("url").is_string()) {
        image_url = raw_url.at("url").get<std::string>();
      }
    }
    if (!image_url) {
      continue;
    }

    const auto parsed = parse_image_data_url(*image_url);
    if (!parsed) {
      continue;
    }
    output.emplace_back(ImageContent{parsed->media_type, parsed->data});
  }

  return output;
}

Usage openrouter_image_usage(const Json& raw_usage,
                             const ImageModelCatalogRecord& model) {
  Usage usage;
  if (!raw_usage.is_object()) {
    return usage;
  }

  const auto prompt_tokens = raw_usage.value("prompt_tokens", std::int64_t{0});
  const auto prompt_details =
      raw_usage.value("prompt_tokens_details", Json::object());
  const auto reported_cached_tokens =
      prompt_details.value("cached_tokens", std::int64_t{0});
  usage.cache_write_tokens =
      prompt_details.value("cache_write_tokens", std::int64_t{0});
  usage.cache_read_tokens =
      usage.cache_write_tokens > 0
          ? std::max<std::int64_t>(0, reported_cached_tokens -
                                          usage.cache_write_tokens)
          : reported_cached_tokens;
  usage.input_tokens = std::max<std::int64_t>(
      0, prompt_tokens - usage.cache_read_tokens - usage.cache_write_tokens);
  usage.output_tokens =
      raw_usage.value("completion_tokens", std::int64_t{0});
  normalize(usage);

  if (const auto rates = token_cost_rates_from_image_model_metadata(model)) {
    apply_cost(usage, *rates);
  }
  return usage;
}

OpenRouterImageGenerationResult parse_openrouter_image_http_response(
    const ImageModelCatalogRecord& model, const HttpResponse& response) {
  OpenRouterImageGenerationResult result;
  result.api = model.api;
  result.provider = model.provider;
  result.model = model.id;
  result.status = response.status;
  result.headers = response.headers;

  if (response.status < 200 || response.status >= 300) {
    result.stop_reason = StopReason::error;
    result.error_message =
        response.body.empty() ? std::string("OpenRouter image API error (") +
                                    std::to_string(response.status) + ")"
                              : response.body;
    return result;
  }

  Json payload;
  try {
    payload = Json::parse(response.body);
  } catch (const std::exception& error) {
    result.stop_reason = StopReason::error;
    result.error_message = error.what();
    return result;
  }

  if (payload.contains("id") && payload.at("id").is_string()) {
    result.response_id = payload.at("id").get<std::string>();
  }
  if (payload.contains("usage")) {
    result.usage = openrouter_image_usage(payload.at("usage"), model);
  }

  const auto choices = payload.value("choices", Json::array());
  if (choices.is_array() && !choices.empty() &&
      choices.front().is_object() && choices.front().contains("message")) {
    result.output = openrouter_image_output_content(choices.front().at("message"));
  }

  result.stop_reason = StopReason::end_turn;
  return result;
}

OpenRouterImageGenerationResult generate_openrouter_image(
    HttpTransport& transport, const ImageModelCatalogRecord& model,
    const std::vector<Content>& input,
    const OpenRouterImageHttpOptions& options) {
  OpenRouterImageGenerationResult result;
  result.api = model.api;
  result.provider = model.provider;
  result.model = model.id;

  if (options.aborted) {
    result.stop_reason = StopReason::aborted;
    result.error_message = "Request was aborted";
    return result;
  }
  if (options.api_key.empty()) {
    result.stop_reason = StopReason::error;
    result.error_message = missing_provider_credential_message(model.provider);
    return result;
  }

  auto request = openrouter_image_http_request(model, input, options);
  const int max_retries = std::max(0, options.max_retries.value_or(0));
  const auto max_retry_delay_ms = options.max_retry_delay_ms.value_or(60000);
  for (int attempt = 0;; ++attempt) {
    request.attempt = attempt;
    HttpResponse response;
    try {
      response = transport.send(request);
    } catch (const std::runtime_error& error) {
      if (attempt < max_retries) {
        continue;
      }
      result.stop_reason = StopReason::error;
      result.error_message = error.what();
      return result;
    }

    if (response.status >= 200 && response.status < 300) {
      return parse_openrouter_image_http_response(model, response);
    }
    if (!is_retryable_http_status(response.status) || attempt >= max_retries) {
      return parse_openrouter_image_http_response(model, response);
    }

    const auto delay_ms = retry_after_ms(response);
    if (delay_ms && max_retry_delay_ms > 0 &&
        *delay_ms > max_retry_delay_ms) {
      result.stop_reason = StopReason::error;
      result.status = response.status;
      result.headers = response.headers;
      result.error_message = "provider retry-after delay " +
                             std::to_string(*delay_ms) +
                             "ms exceeds max_retry_delay_ms " +
                             std::to_string(max_retry_delay_ms);
      return result;
    }
  }
}

}  // namespace qt::ai
