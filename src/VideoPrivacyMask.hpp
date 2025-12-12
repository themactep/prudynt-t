#pragma once

#include "Config.hpp"
#include <atomic>
#include <mutex>
#include <vector>

#include <imp/imp_osd.h>

class VideoPrivacyMask {
public:
  VideoPrivacyMask(int channel, _stream *stream);
  ~VideoPrivacyMask();

  bool isReady() const;
  bool setEnabled(bool enabled);
  bool isEnabled() const;

private:
  void destroyRegion();

  bool initIndicatorLocked();
  bool buildIndicatorFromText(const _osd_privacy &privacy);
  bool buildIndicatorFromImage(const _osd_privacy &privacy);
  bool showIndicatorLocked(bool enabled);
  void destroyIndicatorLocked();

  int channel_{0};
  int width_{0};
  int height_{0};
  int encGrp_{0};
  IMPRgnHandle region_{INVHANDLE};
  bool registered_{false};
  bool visible_{false};
  bool ready_{false};
  _stream *stream_{nullptr};
  IMPRgnHandle indicatorRegion_{INVHANDLE};
  bool indicatorRegistered_{false};
  bool indicatorReady_{false};
  bool indicatorVisible_{false};
  bool indicatorFromImage_{false};
  int indicatorLayer_{16};
  uint8_t indicatorAlpha_{255};
  IMPOSDRgnAttr indicatorAttr_{};
  std::vector<uint8_t> indicatorPixels_;
  uint16_t indicatorWidth_{0};
  uint16_t indicatorHeight_{0};
  mutable std::mutex mutex_;
};
