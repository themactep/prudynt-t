#include "VideoPrivacyMask.hpp"

#include "Logger.hpp"
#include "imp_hal.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "schrift.h"

#define MODULE "VideoPrivacyMask"

namespace {

struct RenderedGlyph {
  int width{0};
  int height{0};
  int advance{0};
  int xmin{0};
  int ymin{0};
  std::vector<uint8_t> bitmap;
};

int autoFontSize(int width) {
  double m = 0.0046875;
  double b = 9.0;
  return static_cast<int>(m * static_cast<double>(width) + b + 0.5);
}

bool parsePosition(const char *position, int &x, int &y) {
  x = 0;
  y = 0;
  if (!position || !*position) {
    return true;
  }
  const char *comma = std::strchr(position, ',');
  if (!comma) {
    return false;
  }
  char xb[16] = {0};
  char yb[16] = {0};
  size_t xl = static_cast<size_t>(comma - position);
  size_t yl = std::strlen(comma + 1);
  if (xl == 0 || xl >= sizeof(xb) || yl == 0 || yl >= sizeof(yb)) {
    return false;
  }
  std::memcpy(xb, position, xl);
  std::memcpy(yb, comma + 1, yl);
  x = std::atoi(xb);
  y = std::atoi(yb);
  return true;
}

void setPixel(uint8_t *image, int x, int y, const uint8_t *color, int width,
              int height) {
  if (x < 0 || y < 0 || x >= width || y >= height) {
    return;
  }
  const int index = (y * width + x) * 4;
  image[index] = color[0];
  image[index + 1] = color[1];
  image[index + 2] = color[2];
  image[index + 3] = color[3];
}

void drawOutline(std::vector<uint8_t> &pixels, const RenderedGlyph &glyph,
                 int originX, int originY, int outline, int width, int height,
                 const uint8_t *strokeColor) {
  if (outline <= 0 || glyph.bitmap.empty()) {
    return;
  }
  for (int j = -outline; j <= outline; ++j) {
    for (int i = -outline; i <= outline; ++i) {
      if (i * i + j * j > outline * outline) {
        continue;
      }
      for (int y = 0; y < glyph.height; ++y) {
        for (int x = 0; x < glyph.width; ++x) {
          const int srcIndex = y * glyph.width + x;
          uint8_t alpha = glyph.bitmap[srcIndex];
          if (alpha == 0) {
            continue;
          }
          uint8_t combinedAlpha =
              static_cast<uint8_t>((alpha * strokeColor[3]) / 255);
          uint8_t color[4] = {strokeColor[0], strokeColor[1], strokeColor[2],
                              combinedAlpha};
          setPixel(pixels.data(), originX + x + i, originY + y + j, color,
                   width, height);
        }
      }
    }
  }
}

bool renderGlyphSequence(SFT &sft, const std::string &text,
                         std::vector<RenderedGlyph> &glyphs) {
  glyphs.clear();
  glyphs.reserve(text.size());
  for (char c : text) {
    SFT_Glyph glyphId;
    if (sft_lookup(&sft, static_cast<unsigned char>(c), &glyphId) != 0) {
      continue;
    }
    SFT_GMetrics metrics;
    if (sft_gmetrics(&sft, glyphId, &metrics) != 0) {
      continue;
    }
    RenderedGlyph glyph;
    glyph.advance = metrics.advanceWidth;
    glyph.xmin = metrics.leftSideBearing;
    glyph.ymin = metrics.yOffset;
    glyph.width = std::max(metrics.minWidth, 0);
    glyph.height = std::max(metrics.minHeight, 0);
    const size_t pixels = static_cast<size_t>(glyph.width) * glyph.height;
    glyph.bitmap.resize(pixels);
    if (glyph.width > 0 && glyph.height > 0) {
      SFT_Image image;
      image.width = glyph.width;
      image.height = glyph.height;
      image.pixels = glyph.bitmap.data();
      if (sft_render(&sft, glyphId, image) != 0) {
        return false;
      }
    }
    glyphs.push_back(std::move(glyph));
  }
  return !glyphs.empty();
}

void computeTextSize(const std::vector<RenderedGlyph> &glyphs, const SFT &sft,
                     int outline, uint16_t &width, uint16_t &height) {
  width = 0;
  height = 0;
  for (const auto &glyph : glyphs) {
    width += glyph.advance + (outline * 2);
    if (glyph.height > height) {
      height = glyph.height;
    }
  }
  height += sft.yScale;
  width += 1 + outline;
  if (width % 2 != 0) {
    ++width;
  }
}

bool drawTextBitmap(const std::vector<RenderedGlyph> &glyphs, const SFT &sft,
                    int outline, unsigned int fontColor,
                    unsigned int strokeColor, std::vector<uint8_t> &pixels,
                    uint16_t width, uint16_t height) {
  pixels.assign(static_cast<size_t>(width) * height * 4, 0);

  uint8_t textColor[4] = {static_cast<uint8_t>(fontColor & 0xFF),
                          static_cast<uint8_t>((fontColor >> 8) & 0xFF),
                          static_cast<uint8_t>((fontColor >> 16) & 0xFF),
                          static_cast<uint8_t>((fontColor >> 24) & 0xFF)};

  uint8_t outlineColor[4] = {static_cast<uint8_t>(strokeColor & 0xFF),
                             static_cast<uint8_t>((strokeColor >> 8) & 0xFF),
                             static_cast<uint8_t>((strokeColor >> 16) & 0xFF),
                             static_cast<uint8_t>((strokeColor >> 24) & 0xFF)};

  int penX = 1;
  int penY = 1;
  for (const auto &glyph : glyphs) {
    const int originX = penX + glyph.xmin + outline;
    const int originY = penY + (sft.yScale + glyph.ymin);
    drawOutline(pixels, glyph, originX, originY, outline, width, height,
                outlineColor);
    if (!glyph.bitmap.empty()) {
      for (int y = 0; y < glyph.height; ++y) {
        for (int x = 0; x < glyph.width; ++x) {
          const int srcIndex = y * glyph.width + x;
          const uint8_t alpha = glyph.bitmap[srcIndex];
          if (alpha == 0) {
            continue;
          }
          uint8_t combinedAlpha =
              static_cast<uint8_t>((alpha * textColor[3]) / 255);
          uint8_t color[4] = {textColor[0], textColor[1], textColor[2],
                              combinedAlpha};
          setPixel(pixels.data(), originX + x, originY + y, color, width,
                   height);
        }
      }
    }
    penX += glyph.advance + (outline * 2);
  }
  return true;
}

void rotateImage(std::vector<uint8_t> &pixels, uint16_t &width,
                 uint16_t &height, int angle) {
  if (pixels.empty() || angle % 360 == 0) {
    return;
  }
  constexpr double kPi = 3.14159265358979323846;
  double angleRad = static_cast<double>(angle) * (kPi / 180.0);

  int corners[4][2] = {{0, 0},
                       {static_cast<int>(width), 0},
                       {0, static_cast<int>(height)},
                       {static_cast<int>(width), static_cast<int>(height)}};

  int minX = std::numeric_limits<int>::max();
  int maxX = std::numeric_limits<int>::min();
  int minY = std::numeric_limits<int>::max();
  int maxY = std::numeric_limits<int>::min();
  for (auto &corner : corners) {
    int x = corner[0];
    int y = corner[1];
    int newX =
        static_cast<int>(x * std::cos(angleRad) - y * std::sin(angleRad));
    int newY =
        static_cast<int>(x * std::sin(angleRad) + y * std::cos(angleRad));
    minX = std::min(minX, newX);
    maxX = std::max(maxX, newX);
    minY = std::min(minY, newY);
    maxY = std::max(maxY, newY);
  }

  const int newWidth = maxX - minX + 1;
  const int newHeight = maxY - minY + 1;
  const int centerX = width / 2;
  const int centerY = height / 2;
  const int newCenterX = newWidth / 2;
  const int newCenterY = newHeight / 2;

  std::vector<uint8_t> rotated(static_cast<size_t>(newWidth) * newHeight * 4,
                               0);
  for (int y = 0; y < newHeight; ++y) {
    for (int x = 0; x < newWidth; ++x) {
      int newX = x - newCenterX;
      int newY = y - newCenterY;
      int origX = static_cast<int>(newX * std::cos(angleRad) +
                                   newY * std::sin(angleRad)) +
                  centerX;
      int origY = static_cast<int>(-newX * std::sin(angleRad) +
                                   newY * std::cos(angleRad)) +
                  centerY;
      if (origX < 0 || origY < 0 || origX >= width || origY >= height) {
        continue;
      }
      const size_t dstIdx = (static_cast<size_t>(y) * newWidth + x) * 4;
      const size_t srcIdx = (static_cast<size_t>(origY) * width + origX) * 4;
      rotated[dstIdx] = pixels[srcIdx];
      rotated[dstIdx + 1] = pixels[srcIdx + 1];
      rotated[dstIdx + 2] = pixels[srcIdx + 2];
      rotated[dstIdx + 3] = pixels[srcIdx + 3];
    }
  }

  pixels.swap(rotated);
  width = static_cast<uint16_t>(newWidth);
  height = static_cast<uint16_t>(newHeight);
}

uint16_t getAbsPos(uint16_t max, uint16_t size, int pos) {
  if (pos == 0) {
    return static_cast<uint16_t>(max / 2 - size / 2);
  }
  if (pos < 0) {
    return static_cast<uint16_t>(max - size - 1 + pos);
  }
  return static_cast<uint16_t>(pos);
}

void setRegionPos(IMPOSDRgnAttr *rgnAttr, int x, int y, uint16_t width,
                  uint16_t height, uint16_t maxWidth, uint16_t maxHeight) {
  if (width == 0 || height == 0) {
    width = rgnAttr->rect.p1.x - rgnAttr->rect.p0.x + 1;
    height = rgnAttr->rect.p1.y - rgnAttr->rect.p0.y + 1;
  }
  if (x > maxWidth - width) {
    x = maxWidth - width;
  }
  if (y > maxHeight - height) {
    y = maxHeight - height;
  }
  rgnAttr->rect.p0.x = getAbsPos(maxWidth, width, x);
  rgnAttr->rect.p0.y = getAbsPos(maxHeight, height, y);
  rgnAttr->rect.p1.x = rgnAttr->rect.p0.x + width - 1;
  rgnAttr->rect.p1.y = rgnAttr->rect.p0.y + height - 1;
}

} // namespace

VideoPrivacyMask::VideoPrivacyMask(int channel, _stream *stream)
    : channel_(channel), width_(stream ? stream->width : 0),
      height_(stream ? stream->height : 0), encGrp_(channel), stream_(stream) {
  if (!stream_) {
    LOG_ERROR("VideoPrivacyMask: missing stream context for channel "
              << channel_);
    return;
  }

  region_ = IMP_OSD_CreateRgn(nullptr);
  if (region_ == INVHANDLE) {
    LOG_ERROR("VideoPrivacyMask: failed to create region for channel "
              << channel_);
    return;
  }

  IMPOSDRgnAttr rgnAttr{};
  rgnAttr.type = OSD_REG_COVER;
  rgnAttr.rect.p0.x = 0;
  rgnAttr.rect.p0.y = 0;
  rgnAttr.rect.p1.x = width_;
  rgnAttr.rect.p1.y = height_;
  rgnAttr.fmt = PIX_FMT_BGRA;
  rgnAttr.data.coverData.color = hal::osd::black_cover_color();

  int ret = IMP_OSD_SetRgnAttr(region_, &rgnAttr);
  LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_SetRgnAttr(" << region_ << ")");
  if (ret != 0) {
    destroyRegion();
    return;
  }

  ret = IMP_OSD_RegisterRgn(region_, encGrp_, nullptr);
  LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_RegisterRgn(" << region_ << ", " << encGrp_
                                                 << ")");
  if (ret != 0) {
    destroyRegion();
    return;
  }
  registered_ = true;

  IMPOSDGrpRgnAttr grpAttr{};
  grpAttr.show = 0;
  grpAttr.layer = 15;
  grpAttr.gAlphaEn = 1;
  grpAttr.fgAlhpa = 255;
  grpAttr.bgAlhpa = 0;
  ret = IMP_OSD_SetGrpRgnAttr(region_, encGrp_, &grpAttr);
  LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_SetGrpRgnAttr(" << region_ << ", " << encGrp_
                                                   << ")");
  if (ret != 0) {
    destroyRegion();
    return;
  }

  ready_ = true;

  std::lock_guard<std::mutex> lock(mutex_);
  initIndicatorLocked();
}

VideoPrivacyMask::~VideoPrivacyMask() {
  destroyRegion();
}

bool VideoPrivacyMask::isReady() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ready_;
}

bool VideoPrivacyMask::isEnabled() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return visible_;
}

bool VideoPrivacyMask::setEnabled(bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!ready_) {
    return false;
  }
  if (visible_ == enabled) {
    return true;
  }
  int ret = IMP_OSD_ShowRgn(region_, encGrp_, enabled ? 1 : 0);
  LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_ShowRgn(" << region_ << ", " << encGrp_
                                             << ", " << (enabled ? 1 : 0)
                                             << ")");
  if (ret != 0) {
    return false;
  }
  visible_ = enabled;
  showIndicatorLocked(enabled);
  return true;
}

bool VideoPrivacyMask::initIndicatorLocked() {
  if (!stream_) {
    return false;
  }
  auto &privacy = stream_->osd.privacy;
  if (!privacy.enabled) {
    return false;
  }

  const bool hasImage = privacy.image_path && *privacy.image_path &&
                        privacy.image_width > 0 && privacy.image_height > 0;
  const bool hasText = privacy.text && *privacy.text;
  if (!hasImage && !hasText) {
    LOG_WARN("VideoPrivacyMask: privacy indicator enabled but no text or image"
             " provided on channel "
             << channel_);
    return false;
  }

  indicatorRegion_ = IMP_OSD_CreateRgn(nullptr);
  if (indicatorRegion_ == INVHANDLE) {
    LOG_ERROR("VideoPrivacyMask: failed to create indicator region for channel "
              << channel_);
    return false;
  }

  indicatorLayer_ = std::clamp(privacy.layer, 0, 16);
  indicatorAlpha_ = static_cast<uint8_t>(std::clamp(privacy.opacity, 0, 255));

  bool built = false;
  if (hasImage) {
    built = buildIndicatorFromImage(privacy);
    indicatorFromImage_ = true;
  } else {
    built = buildIndicatorFromText(privacy);
    indicatorFromImage_ = false;
  }

  if (!built) {
    destroyIndicatorLocked();
    return false;
  }

  std::memset(&indicatorAttr_, 0, sizeof(IMPOSDRgnAttr));
  indicatorAttr_.type = OSD_REG_PIC;
  indicatorAttr_.fmt = PIX_FMT_BGRA;
  indicatorAttr_.data.picData.pData = indicatorPixels_.data();

  int posX = 0;
  int posY = 0;
  if (!parsePosition(privacy.position, posX, posY)) {
    LOG_WARN("VideoPrivacyMask: invalid privacy indicator position '"
             << (privacy.position ? privacy.position : "")
             << "', defaulting to center");
  }

  setRegionPos(&indicatorAttr_, posX, posY, indicatorWidth_, indicatorHeight_,
               static_cast<uint16_t>(width_), static_cast<uint16_t>(height_));

  int ret = IMP_OSD_SetRgnAttr(indicatorRegion_, &indicatorAttr_);
  LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_SetRgnAttr(" << indicatorRegion_ << ")");
  if (ret != 0) {
    destroyIndicatorLocked();
    return false;
  }

  ret = IMP_OSD_RegisterRgn(indicatorRegion_, encGrp_, nullptr);
  LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_RegisterRgn(" << indicatorRegion_ << ", "
                                                 << encGrp_ << ")");
  if (ret != 0) {
    destroyIndicatorLocked();
    return false;
  }
  indicatorRegistered_ = true;

  IMPOSDGrpRgnAttr grpAttr{};
  grpAttr.show = 0;
  grpAttr.layer = indicatorLayer_;
  grpAttr.gAlphaEn = 1;
  grpAttr.fgAlhpa = indicatorAlpha_;
  grpAttr.bgAlhpa = 0;
  ret = IMP_OSD_SetGrpRgnAttr(indicatorRegion_, encGrp_, &grpAttr);
  LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_SetGrpRgnAttr(" << indicatorRegion_ << ", "
                                                   << encGrp_ << ")");
  if (ret != 0) {
    destroyIndicatorLocked();
    return false;
  }

  indicatorReady_ = true;
  return true;
}

bool VideoPrivacyMask::buildIndicatorFromText(const _osd_privacy &privacy) {
  const char *fontPath = stream_->osd.font_path;
  if (!fontPath || !*fontPath) {
    LOG_ERROR("VideoPrivacyMask: missing font path for privacy indicator");
    return false;
  }

  const std::string text = privacy.text ? privacy.text : "";
  if (text.empty()) {
    LOG_ERROR("VideoPrivacyMask: privacy text is empty");
    return false;
  }

  int fontSize = privacy.font_size;
  if (fontSize == OSD_AUTO_VALUE || fontSize <= 0) {
    fontSize = autoFontSize(width_);
  }
  if (fontSize <= 0) {
    fontSize = 12;
  }

  int strokeSize = std::max(0, privacy.stroke_size);

  std::ifstream fontFile(fontPath, std::ios::binary | std::ios::ate);
  if (!fontFile.is_open()) {
    LOG_ERROR("VideoPrivacyMask: failed to open font " << fontPath);
    return false;
  }

  const std::streamsize fileSize = fontFile.tellg();
  fontFile.seekg(0, std::ios::beg);
  std::vector<uint8_t> fontData(static_cast<size_t>(fileSize));
  if (!fontFile.read(reinterpret_cast<char *>(fontData.data()), fileSize)) {
    LOG_ERROR("VideoPrivacyMask: failed to read font " << fontPath);
    return false;
  }

  SFT sft{};
  sft.flags = SFT_DOWNWARD_Y;
  sft.xScale = fontSize;
  sft.yScale = fontSize;
  int yOffset =
      static_cast<int>(std::round(static_cast<float>(sft.yScale) * 0.1f));
  sft.yOffset = std::max(1, yOffset);
  sft.font = sft_loadmem(fontData.data(), fontData.size());
  if (!sft.font) {
    LOG_ERROR("VideoPrivacyMask: failed to load font " << fontPath);
    return false;
  }

  std::vector<RenderedGlyph> glyphs;
  bool rendered = renderGlyphSequence(sft, text, glyphs);
  if (!rendered) {
    LOG_ERROR("VideoPrivacyMask: failed to render privacy glyphs");
    sft_freefont(sft.font);
    return false;
  }

  computeTextSize(glyphs, sft, strokeSize, indicatorWidth_, indicatorHeight_);
  drawTextBitmap(glyphs, sft, strokeSize, privacy.fill_color,
                 privacy.stroke_color, indicatorPixels_, indicatorWidth_,
                 indicatorHeight_);

  if (privacy.rotation != 0) {
    rotateImage(indicatorPixels_, indicatorWidth_, indicatorHeight_,
                privacy.rotation);
  }

  sft_freefont(sft.font);
  return true;
}

bool VideoPrivacyMask::buildIndicatorFromImage(const _osd_privacy &privacy) {
  const size_t expected = static_cast<size_t>(privacy.image_width) *
                          static_cast<size_t>(privacy.image_height) * 4;
  if (expected == 0) {
    LOG_ERROR(
        "VideoPrivacyMask: invalid image dimensions for privacy indicator");
    return false;
  }

  std::ifstream file(privacy.image_path, std::ios::binary);
  if (!file.is_open()) {
    LOG_ERROR("VideoPrivacyMask: failed to open privacy image "
              << (privacy.image_path ? privacy.image_path : ""));
    return false;
  }

  indicatorPixels_.assign(expected, 0);
  if (!file.read(reinterpret_cast<char *>(indicatorPixels_.data()),
                 static_cast<std::streamsize>(expected))) {
    LOG_ERROR("VideoPrivacyMask: privacy image size mismatch for "
              << (privacy.image_path ? privacy.image_path : ""));
    indicatorPixels_.clear();
    return false;
  }

  indicatorWidth_ = static_cast<uint16_t>(privacy.image_width);
  indicatorHeight_ = static_cast<uint16_t>(privacy.image_height);
  if (privacy.rotation != 0) {
    rotateImage(indicatorPixels_, indicatorWidth_, indicatorHeight_,
                privacy.rotation);
  }
  return true;
}

bool VideoPrivacyMask::showIndicatorLocked(bool enabled) {
  if (!indicatorReady_ || !indicatorRegistered_) {
    return true;
  }
  if (indicatorVisible_ == enabled) {
    return true;
  }
  int ret = IMP_OSD_ShowRgn(indicatorRegion_, encGrp_, enabled ? 1 : 0);
  LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_ShowRgn(" << indicatorRegion_ << ", "
                                             << encGrp_ << ", "
                                             << (enabled ? 1 : 0) << ")");
  if (ret != 0) {
    return false;
  }
  indicatorVisible_ = enabled;
  return true;
}

void VideoPrivacyMask::destroyIndicatorLocked() {
  if (indicatorRegion_ == INVHANDLE) {
    indicatorReady_ = false;
    indicatorVisible_ = false;
    indicatorRegistered_ = false;
    indicatorPixels_.clear();
    indicatorWidth_ = 0;
    indicatorHeight_ = 0;
    return;
  }

  if (indicatorVisible_) {
    IMP_OSD_ShowRgn(indicatorRegion_, encGrp_, 0);
    indicatorVisible_ = false;
  }

  if (indicatorRegistered_) {
    IMP_OSD_UnRegisterRgn(indicatorRegion_, encGrp_);
    indicatorRegistered_ = false;
  }

  IMP_OSD_DestroyRgn(indicatorRegion_);
  indicatorRegion_ = INVHANDLE;
  indicatorReady_ = false;
  indicatorPixels_.clear();
  indicatorWidth_ = 0;
  indicatorHeight_ = 0;
}

void VideoPrivacyMask::destroyRegion() {
  std::lock_guard<std::mutex> lock(mutex_);
  destroyIndicatorLocked();
  if (region_ == INVHANDLE) {
    ready_ = false;
    visible_ = false;
    registered_ = false;
    return;
  }

  if (visible_) {
    int ret = IMP_OSD_ShowRgn(region_, encGrp_, 0);
    LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_ShowRgn(" << region_ << ", " << encGrp_
                                               << ", 0)");
    visible_ = false;
  }

  if (registered_) {
    int ret = IMP_OSD_UnRegisterRgn(region_, encGrp_);
    LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_UnRegisterRgn(" << region_ << ", "
                                                     << encGrp_ << ")");
    registered_ = false;
  }

  IMP_OSD_DestroyRgn(region_);
  region_ = INVHANDLE;
  ready_ = false;
}
