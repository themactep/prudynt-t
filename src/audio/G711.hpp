#ifndef G711_HPP
#define G711_HPP

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

} // namespace G711

#endif // G711_HPP
