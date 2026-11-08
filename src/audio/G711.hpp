#ifndef G711_HPP
#define G711_HPP

#include <cstddef>
#include <cstdint>

namespace G711 {

// ITU-T G.711 codewords expand to signed 16-bit linear PCM samples.
inline int16_t decodeUlaw(uint8_t code) {
  code = static_cast<uint8_t>(~code);
  int magnitude = ((code & 0x0f) << 3) + 0x84;
  magnitude <<= (code & 0x70) >> 4;
  return static_cast<int16_t>((code & 0x80) ? (0x84 - magnitude)
                                           : (magnitude - 0x84));
}

inline int16_t decodeAlaw(uint8_t code) {
  code ^= 0x55;
  int magnitude = (code & 0x0f) << 4;
  const int segment = (code & 0x70) >> 4;
  switch (segment) {
  case 0:
    magnitude += 8;
    break;
  case 1:
    magnitude += 0x108;
    break;
  default:
    magnitude += 0x108;
    magnitude <<= segment - 1;
    break;
  }
  return static_cast<int16_t>((code & 0x80) ? magnitude : -magnitude);
}

// Expand a G.711 byte stream in order: one input byte yields one signed 16-bit
// sample, and `out` must hold at least `count` samples.
inline void decodeUlaw(const uint8_t *in, size_t count, int16_t *out) {
  for (size_t i = 0; i < count; ++i)
    out[i] = decodeUlaw(in[i]);
}

inline void decodeAlaw(const uint8_t *in, size_t count, int16_t *out) {
  for (size_t i = 0; i < count; ++i)
    out[i] = decodeAlaw(in[i]);
}

} // namespace G711

#endif // G711_HPP
