#include "index/tree_sitter_runtime/unicode.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef uint32_t (*DecodeFunction)(const uint8_t *, uint32_t, int32_t *);

static void require_decode(const char *label, DecodeFunction decode,
                           const uint8_t *input, uint32_t length,
                           uint32_t expected_size, int32_t expected_code_point) {
  int32_t code_point = 0;
  const uint32_t size = decode(input, length, &code_point);
  if (size != expected_size || code_point != expected_code_point) {
    fprintf(stderr,
            "%s: got size=%u code_point=%d, expected size=%u code_point=%d\n",
            label, size, code_point, expected_size, expected_code_point);
    exit(EXIT_FAILURE);
  }
}

int main(void) {
  static const uint8_t ascii[] = {0x41};
  static const uint8_t utf8_two[] = {0xc2, 0xa2};
  static const uint8_t utf8_three[] = {0xe2, 0x82, 0xac};
  static const uint8_t utf8_four[] = {0xf0, 0x9f, 0x98, 0x80};
  static const uint8_t utf8_overlong[] = {0xc0, 0x80};
  static const uint8_t utf8_surrogate[] = {0xed, 0xa0, 0x80};
  static const uint8_t utf8_out_of_range[] = {0xf4, 0x90, 0x80, 0x80};
  static const uint8_t utf8_truncated[] = {0xe2, 0x82};
  static const uint8_t utf16_le_bmp[] = {0xac, 0x20};
  static const uint8_t utf16_be_bmp[] = {0x20, 0xac};
  static const uint8_t utf16_le_pair[] = {0x3d, 0xd8, 0x00, 0xde};
  static const uint8_t utf16_le_unpaired[] = {0x3d, 0xd8};
  static const uint8_t utf16_odd[] = {0x41};

  require_decode("utf8 ascii", ts_decode_utf8, ascii, sizeof ascii, 1, 0x41);
  require_decode("utf8 two-byte", ts_decode_utf8, utf8_two, sizeof utf8_two, 2,
                 0xa2);
  require_decode("utf8 three-byte", ts_decode_utf8, utf8_three,
                 sizeof utf8_three, 3, 0x20ac);
  require_decode("utf8 four-byte", ts_decode_utf8, utf8_four,
                 sizeof utf8_four, 4, 0x1f600);
  require_decode("utf8 overlong", ts_decode_utf8, utf8_overlong,
                 sizeof utf8_overlong, 1, TS_DECODE_ERROR);
  require_decode("utf8 surrogate", ts_decode_utf8, utf8_surrogate,
                 sizeof utf8_surrogate, 1, TS_DECODE_ERROR);
  require_decode("utf8 out of range", ts_decode_utf8, utf8_out_of_range,
                 sizeof utf8_out_of_range, 1, TS_DECODE_ERROR);
  require_decode("utf8 truncated", ts_decode_utf8, utf8_truncated,
                 sizeof utf8_truncated, 2, TS_DECODE_ERROR);
  require_decode("utf16 LE BMP", ts_decode_utf16_le, utf16_le_bmp,
                 sizeof utf16_le_bmp, 2, 0x20ac);
  require_decode("utf16 BE BMP", ts_decode_utf16_be, utf16_be_bmp,
                 sizeof utf16_be_bmp, 2, 0x20ac);
  require_decode("utf16 LE pair", ts_decode_utf16_le, utf16_le_pair,
                 sizeof utf16_le_pair, 4, 0x1f600);
  require_decode("utf16 LE unpaired lead", ts_decode_utf16_le,
                 utf16_le_unpaired, sizeof utf16_le_unpaired, 2, 0xd83d);
  require_decode("utf16 odd byte", ts_decode_utf16_le, utf16_odd,
                 sizeof utf16_odd, 1, TS_DECODE_ERROR);
  return EXIT_SUCCESS;
}
