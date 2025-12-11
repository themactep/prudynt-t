#include "VideoPrivacyMask.hpp"

#include "Logger.hpp"

#define MODULE "VideoPrivacyMask"

VideoPrivacyMask::VideoPrivacyMask(int channel, int width, int height)
    : channel_(channel), width_(width), height_(height), encGrp_(channel) {
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
  rgnAttr.data.coverData.color = OSD_IPU_BLACK;

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
  LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_SetGrpRgnAttr(" << region_ << ", "
                                                  << encGrp_ << ")");
  if (ret != 0) {
    destroyRegion();
    return;
  }

  ready_ = true;
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
  if (ret == 0) {
    visible_ = enabled;
    return true;
  }
  return false;
}

void VideoPrivacyMask::destroyRegion() {
  std::lock_guard<std::mutex> lock(mutex_);
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
