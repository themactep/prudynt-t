// Run from the repository root (no IMP SDK or codec libraries needed):
// c++ -std=c++20 -Wall -Wextra -Werror -Isrc tests/test_g711.cpp -o /tmp/test_g711
// /tmp/test_g711

#include "audio/G711.hpp"

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

  std::printf("PASS: %d G.711 codewords (256 PCMU, 256 PCMA)\n", checked);
  return 0;
}
