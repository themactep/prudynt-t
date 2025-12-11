#pragma once

#include <atomic>
#include <mutex>

#include <imp/imp_osd.h>

class VideoPrivacyMask {
public:
  VideoPrivacyMask(int channel, int width, int height);
  ~VideoPrivacyMask();

  bool isReady() const;
  bool setEnabled(bool enabled);
  bool isEnabled() const;

private:
  void destroyRegion();

  int channel_{0};
  int width_{0};
  int height_{0};
  int encGrp_{0};
  IMPRgnHandle region_{INVHANDLE};
  bool registered_{false};
  bool visible_{false};
  bool ready_{false};
  mutable std::mutex mutex_;
};
