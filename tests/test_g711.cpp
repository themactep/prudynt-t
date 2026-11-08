// Run from the repository root (no IMP SDK or codec libraries needed):
// c++ -std=c++20 -Wall -Wextra -Werror -Isrc tests/test_g711.cpp -o /tmp/test_g711
// /tmp/test_g711

#include "audio/G711.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>

int main() {
  // Expected G.711 reconstruction levels, expressed as segment origins and
  // quantization steps rather than duplicating the decoder's bit arithmetic.
  constexpr int ulawOrigin[] = {0, 132, 396, 924, 1980, 4092, 8316, 16764};
  constexpr int ulawStep[] = {8, 16, 32, 64, 128, 256, 512, 1024};
  constexpr int alawOrigin[] = {8, 264, 528, 1056, 2112, 4224, 8448, 16896};
  constexpr int alawStep[] = {16, 16, 32, 64, 128, 256, 512, 1024};
  int checked = 0;

  for (int segment = 0; segment < 8; ++segment) {
    for (int step = 0; step < 16; ++step) {
      for (int sign = 0; sign < 2; ++sign) {
        const int level = segment * 16 + step;
        const auto ulaw = static_cast<uint8_t>((sign ? 127 : 255) - level);
        const auto alaw = static_cast<uint8_t>(level ^ (sign ? 0x55 : 0xd5));
        const int polarity = sign ? -1 : 1;
        const int expectedUlaw =
            polarity * (ulawOrigin[segment] + step * ulawStep[segment]);
        const int expectedAlaw =
            polarity * (alawOrigin[segment] + step * alawStep[segment]);

        if (G711::decodeUlaw(ulaw) != expectedUlaw ||
            G711::decodeAlaw(alaw) != expectedAlaw) {
          std::fprintf(stderr, "G.711 mismatch: segment=%d step=%d sign=%d\n",
                       segment, step, sign);
          return 1;
        }
        checked += 2;
      }
    }
  }

  // Buffer overloads: one input byte per output sample, order preserved.
  constexpr uint8_t ulawBuf[4] = {0xff, 0x00, 0x7f, 0x80};
  constexpr uint8_t alawBuf[4] = {0xd5, 0x55, 0xaa, 0x2a};
  int16_t ulawOut[4] = {};
  int16_t alawOut[4] = {};
  G711::decodeUlaw(ulawBuf, 4, ulawOut);
  G711::decodeAlaw(alawBuf, 4, alawOut);
  for (size_t i = 0; i < 4; ++i) {
    if (ulawOut[i] != G711::decodeUlaw(ulawBuf[i]) ||
        alawOut[i] != G711::decodeAlaw(alawBuf[i])) {
      std::fprintf(stderr, "G.711 buffer overload mismatch at %zu\n", i);
      return 1;
    }
  }

  std::printf("PASS: %d G.711 codewords + buffer overloads (256 PCMU, 256 PCMA)\n",
              checked);
  return 0;
}
