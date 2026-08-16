#pragma once
// -- build-time font selection for the burned-in OSD overlay ----------
// Thin selector: OSD code includes this header and uses `osdfont::` so it
// never knows which font is compiled in. Pick the font with the
// USE_OSD_FONT_8X8 preprocessor define (make USE_OSD_BURNIN=1
// USE_OSD_FONT8X8=1, or build.sh --osd-burnin --osd-font8x8).
//
// Both fonts expose the same interface:
//   constexpr int WIDTH, HEIGHT;          // glyph size in pixels
//   constexpr uint8_t columnMask(int rx); // bit mask for column rx (0=leftmost)
//   const uint8_t *glyphFor(char);        // 7 or 8 row bytes, or nullptr

#if defined(USE_OSD_FONT_UNIFONT)
#include "util/FontUnifont.hpp"
namespace osdfont = fontunifont;
#elif defined(USE_OSD_FONT_8X8)
#include "util/Font8x8.hpp"
namespace osdfont = font8x8;
#else
#include "util/Font5x7.hpp"
namespace osdfont = font5x7;
#endif
