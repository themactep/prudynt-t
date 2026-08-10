#include "Motion.hpp"
#include "globals.hpp"
#include "imp_hal.hpp"
#include <algorithm>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace std::chrono;
bool ignoreInitialPeriod = true;

namespace {
constexpr const char *kPrudyntRunDir = "/run/prudynt";
constexpr const char *kMotionStatePath = "/run/prudynt/motion.active";
constexpr const char *kMotionDetectedPath =
    "/run/prudynt/motion_detected.active";
constexpr const char *kMotorsActivePath = "/run/motors-active";

void write_motion_detection_state_file() {
  int fd = ::open(kMotionStatePath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0) {
    LOG_WARN("Motion: failed to create state file " << kMotionStatePath);
    return;
  }
  const char *payload = "monitoring=true\n";
  ssize_t ignored = ::write(fd, payload, strlen(payload));
  (void)ignored;
  ::close(fd);
}

void remove_motion_detection_state_file() {
  ::unlink(kMotionStatePath);
}

void write_motion_detected_state_file() {
  int fd = ::open(kMotionDetectedPath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0) {
    LOG_WARN("Motion: failed to create detected state file "
             << kMotionDetectedPath);
    return;
  }
  const char *payload = "motion=true\n";
  ssize_t ignored = ::write(fd, payload, strlen(payload));
  (void)ignored;
  ::close(fd);
}

void remove_motion_detected_state_file() {
  ::unlink(kMotionDetectedPath);
}
} // namespace

std::string Motion::getConfigPath(const char *itemName) {
  return "motion." + std::string(itemName);
}

void Motion::detect() {
  LOG_INFO("Start motion detection thread.");

  int ret;
  int debounce = 0;
  IMP_IVS_MoveOutput *result;
  bool isInCooldown = false;
  auto cooldownEndTime = steady_clock::now();
  auto motionEndTime = steady_clock::now();
  auto startTime = steady_clock::now();

  if (init() != 0)
    return;

  write_motion_detection_state_file();

  global_motion_thread_signal = true;
  bool motorMovementActive = false;
  auto motorSettleWindow =
      std::chrono::milliseconds(std::max(cfg->motion.motor_settle_ms, 0));
  auto lastMotorEventTime = startTime - motorSettleWindow;
  while (global_motion_thread_signal) {
    // Skip motion detection during privacy mode
    if (global_video[0] &&
        global_video[0]->privacy_requested.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(cfg->motion.ivs_polling_timeout_ms));
      continue;
    }

    ret = IMP_IVS_PollingResult(ivsChn, cfg->motion.ivs_polling_timeout_ms);
    if (ret < 0) {
      LOG_WARN("IMP_IVS_PollingResult error: " << ret);
      continue;
    }

    ret = IMP_IVS_GetResult(ivsChn, (void **)&result);
    if (ret < 0) {
      LOG_WARN("IMP_IVS_GetResult error: " << ret);
      continue;
    }

    auto currentTime = steady_clock::now();
    auto elapsedTime = duration_cast<seconds>(currentTime - startTime);

    motorSettleWindow =
        std::chrono::milliseconds(std::max(cfg->motion.motor_settle_ms, 0));
    bool motorFlagPresent = (::access(kMotorsActivePath, F_OK) == 0);

    if (motorFlagPresent) {
      lastMotorEventTime = currentTime;
    }

    auto sinceLastMotor =
        duration_cast<milliseconds>(currentTime - lastMotorEventTime);

    bool motorActiveOrSettling =
        motorFlagPresent || (sinceLastMotor < motorSettleWindow);

    if (motorActiveOrSettling && !motorMovementActive) {
      LOG_INFO("Motion suppressed: motor movement detected (flag="
               << motorFlagPresent
               << ", settle_ms=" << motorSettleWindow.count()
               << ", since_last_ms=" << sinceLastMotor.count() << ")");
    } else if (!motorActiveOrSettling && motorMovementActive) {
      LOG_INFO("Motor movement ended; resuming motion monitoring (cooldown "
               "applies) (flag="
               << motorFlagPresent
               << ", settle_ms=" << motorSettleWindow.count()
               << ", since_last_ms=" << sinceLastMotor.count()
               << ", cooldown_s=" << cfg->motion.cooldown_time_s << ")");
      isInCooldown = true;
      cooldownEndTime = steady_clock::now();
    }
    motorMovementActive = motorActiveOrSettling;

    if (ignoreInitialPeriod && elapsedTime.count() < cfg->motion.init_time_s) {
      continue;
    } else {
      ignoreInitialPeriod = false;
    }

    if (isInCooldown &&
        duration_cast<seconds>(currentTime - cooldownEndTime).count() <
            cfg->motion.cooldown_time_s) {
      continue;
    } else {
      isInCooldown = false;
    }

    bool motionDetected = false;
    if (!motorMovementActive) {
      for (int i = 0; i < IMP_IVS_MOVE_MAX_ROI_CNT; i++) {
        if (result->retRoi[i]) {
          motionDetected = true;
          LOG_INFO("Active motion detected in region " << i);
          debounce++;
          if (debounce >= cfg->motion.debounce_time_s) {
            if (!moving.load()) {
              moving = true;
              LOG_INFO("Motion Start");
              write_motion_detected_state_file();

              char cmd[128];
              memset(cmd, 0, sizeof(cmd));
              snprintf(cmd, sizeof(cmd), "%s start", cfg->motion.script_path);
              ret = system(cmd);
              if (ret != 0) {
                LOG_ERROR("Motion script failed:" << cmd);
              }
            }
            indicator = true;
            motionEndTime = steady_clock::now(); // Update last motion time
          }
        }
      }
    } else {
      debounce = 0;
    }

    if (!motionDetected) {
      debounce = 0;
      auto duration =
          duration_cast<seconds>(currentTime - motionEndTime).count();
      if (moving && duration >= cfg->motion.min_time_s &&
          duration >= cfg->motion.post_time_s) {
        LOG_INFO("End of Motion");
        remove_motion_detected_state_file();
        char cmd[128];
        memset(cmd, 0, sizeof(cmd));
        snprintf(cmd, sizeof(cmd), "%s stop", cfg->motion.script_path);
        ret = system(cmd);
        if (ret != 0) {
          LOG_ERROR("Motion script failed:" << cmd);
        }
        moving = false;
        indicator = false;
        cooldownEndTime = steady_clock::now(); // Start cooldown
        isInCooldown = true;
      }
    }

    ret = IMP_IVS_ReleaseResult(ivsChn, (void *)result);
    if (ret < 0) {
      LOG_WARN("IMP_IVS_ReleaseResult error: " << ret);
      continue;
    }
  }

  exit();
  remove_motion_detection_state_file();

  LOG_INFO("Exit motion detection thread.");
}

int Motion::init() {
  LOG_INFO("Initialize motion detection.");

  if ((cfg->motion.monitor_stream == 0 && !cfg->stream0.enabled) ||
      (cfg->motion.monitor_stream == 1 && !cfg->stream1.enabled)) {
    LOG_ERROR("Monitor stream is disabled, abort.");
    return -1;
  }
  int ret;

  ret = IMP_IVS_CreateGroup(0);
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_IVS_CreateGroup(0)");

  // automatically set frame size / height
  ret = IMP_Encoder_GetChnAttr(cfg->motion.monitor_stream, &channelAttributes);
  if (ret == 0) {
    if (cfg->motion.frame_width == IVS_AUTO_VALUE) {
      cfg->set<int>(getConfigPath("frame_width"),
                    HAL_ENC_ATTR_WIDTH(channelAttributes), true);
    }
    if (cfg->motion.frame_height == IVS_AUTO_VALUE) {
      cfg->set<int>(getConfigPath("frame_height"),
                    HAL_ENC_ATTR_HEIGHT(channelAttributes), true);
    }
    if (cfg->motion.roi_1_x == IVS_AUTO_VALUE) {
      cfg->set<int>(getConfigPath("roi_1_x"),
                    HAL_ENC_ATTR_WIDTH(channelAttributes) - 1, true);
    }
    if (cfg->motion.roi_1_y == IVS_AUTO_VALUE) {
      cfg->set<int>(getConfigPath("roi_1_y"),
                    HAL_ENC_ATTR_HEIGHT(channelAttributes) - 1, true);
    }
  }

  memset(&move_param, 0, sizeof(IMP_IVS_MoveParam));

  // Map web UI sensitivity (1-8) to hardware range (0-max)
  // Hardware max varies by platform: older T20 supports 0-4, newer platforms
  // support 0-8 for panoramic/fisheye cameras
  int hw_sensitivity = cfg->motion.sensitivity - 1;
  if (hw_sensitivity < 0)
    hw_sensitivity = 0;
  int hw_max = hal::caps().motion_sensitivity_max;
  if (hw_sensitivity > hw_max)
    hw_sensitivity = hw_max;

  move_param.sense[0] = hw_sensitivity;
  move_param.skipFrameCnt = cfg->motion.skip_frame_count;

  // Adjust motion frame dimensions for video rotation
  int motion_width = cfg->motion.frame_width;
  int motion_height = cfg->motion.frame_height;

  // Get the monitored stream to check for rotation
  _stream *monitor_stream_cfg = nullptr;
  if (cfg->motion.monitor_stream == 0) {
    monitor_stream_cfg = &cfg->stream0;
  } else if (cfg->motion.monitor_stream == 1) {
    monitor_stream_cfg = &cfg->stream1;
  } else if (cfg->motion.monitor_stream == 2) {
    monitor_stream_cfg = &cfg->stream2;
  } else if (cfg->motion.monitor_stream == 3) {
    monitor_stream_cfg = &cfg->stream3;
  }

  // Swap dimensions if video is rotated
  if (monitor_stream_cfg && monitor_stream_cfg->rotation != 0) {
    std::swap(motion_width, motion_height);
    LOG_DEBUG("Motion detection dimensions adjusted for "
              << monitor_stream_cfg->rotation << " deg rotation: " << motion_width
              << "x" << motion_height);
  }

  move_param.frameInfo.width = motion_width;
  move_param.frameInfo.height = motion_height;

  LOG_INFO("Motion detection: sensitivity: "
           << move_param.sense[0] << " (UI: " << cfg->motion.sensitivity
           << ", HW max: " << hal::caps().motion_sensitivity_max << ")"
           << ", skipCnt:" << move_param.skipFrameCnt
           << ", width:" << move_param.frameInfo.width
           << ", height:" << move_param.frameInfo.height);

  move_param.roiRect[0].p0.x = cfg->motion.roi_0_x;
  move_param.roiRect[0].p0.y = cfg->motion.roi_0_y;
  move_param.roiRect[0].p1.x = cfg->motion.roi_1_x - 1;
  move_param.roiRect[0].p1.y = cfg->motion.roi_1_y - 1;
  move_param.roiRectCnt = cfg->motion.roi_count;

  // Exclude burn-in timestamp rectangle from motion detection.
  // Split the ROI into up to 3 strips around the OSD text area.
  if (cfg->osd.burnin.enabled) {
    int bscale = cfg->osd.burnin.scale > 0
                     ? cfg->osd.burnin.scale
                     : std::clamp(motion_width / 480, 1, 10);
    const int margin = 8;
    const int glyph_w = 5, glyph_h = 7;
    int cell = (glyph_w + 1) * bscale;            // glyph width + 1-col gap
    int pad = bscale * 2;
    int burnin_h = margin + glyph_h * bscale + pad * 2;
    int burnin_y = margin;
    // Estimate text width from format string: ~3 output chars per directive.
    int fmt_len = cfg->osd.burnin.format ? (int)strlen(cfg->osd.burnin.format) : 0;
    int est_chars = std::max(10, fmt_len * 3);    // floor of 10 chars
    int burnin_w = est_chars * cell + pad * 2 - bscale; // last char no gap
    int burnin_x = margin;

    int r0x = cfg->motion.roi_0_x, r0y = cfg->motion.roi_0_y;
    int r1x = cfg->motion.roi_1_x, r1y = cfg->motion.roi_1_y;

    // Build up to 3 ROIs around the burn-in rectangle.
    int cnt = 0;
    auto addRoi = [&](int x0, int y0, int x1, int y1) {
      if (cnt >= IMP_IVS_MOVE_MAX_ROI_CNT || x0 >= x1 || y0 >= y1) return;
      move_param.roiRect[cnt].p0.x = x0;
      move_param.roiRect[cnt].p0.y = y0;
      move_param.roiRect[cnt].p1.x = x1 - 1;
      move_param.roiRect[cnt].p1.y = y1 - 1;
      cnt++;
    };
    // Left of text: full height.
    addRoi(r0x, r0y, burnin_x, r1y);
    // Right of text: full height.
    addRoi(burnin_x + burnin_w, r0y, r1x, r1y);
    // Below text, only its own width.
    addRoi(burnin_x, burnin_y + burnin_h, burnin_x + burnin_w, r1y);
    if (cnt > 0) {
      move_param.roiRectCnt = cnt;
      LOG_INFO("Motion: burn-in exclusion active ("
               << burnin_w << "x" << burnin_h << " at " << burnin_x << ","
               << burnin_y << "), " << cnt << " ROI strips");
    }
  }

  for (int i = 0; i < move_param.roiRectCnt; i++) {
    LOG_INFO("Motion roi[" << i << "]: "
             << move_param.roiRect[i].p0.x << "," << move_param.roiRect[i].p0.y
             << " -> " << move_param.roiRect[i].p1.x << "," << move_param.roiRect[i].p1.y);
  }

  move_intf = IMP_IVS_CreateMoveInterface(&move_param);

  ret = IMP_IVS_CreateChn(ivsChn, move_intf);
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret,
                              "IMP_IVS_CreateChn(" << ivsChn << ", move_intf)");

  ret = IMP_IVS_RegisterChn(ivsGrp, ivsChn);
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_IVS_RegisterChn(" << ivsGrp << ", "
                                                          << ivsChn << ")");

  ret = IMP_IVS_StartRecvPic(ivsChn);
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_IVS_StartRecvPic(" << ivsChn << ")")

  fs = {/**< Device ID */ DEV_ID_FS,
        /**< Group ID */ cfg->motion.monitor_stream,
        /**< output ID */ 1};

  ivs_cell = {/**< Device ID */ DEV_ID_IVS,
              /**< Group ID */ 0,
              /**< output ID */ 0};

  ret = IMP_System_Bind(&fs, &ivs_cell);
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_System_Bind(&fs, &ivs_cell)");

  return ret;
}

int Motion::exit() {
  int ret;

  LOG_DEBUG("Exit motion detection.");

  ret = IMP_IVS_StopRecvPic(ivsChn);
  LOG_DEBUG_OR_ERROR(ret, "IMP_IVS_StopRecvPic(0)");

  ret = IMP_System_UnBind(&fs, &ivs_cell);
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_System_UnBind(&fs, &ivs_cell)");

  ret = IMP_IVS_UnRegisterChn(ivsChn);
  LOG_DEBUG_OR_ERROR(ret, "IMP_IVS_UnRegisterChn(0)");

  ret = IMP_IVS_DestroyChn(ivsChn);
  LOG_DEBUG_OR_ERROR(ret, "IMP_IVS_DestroyChn(0)");

  ret = IMP_IVS_DestroyGroup(ivsGrp);
  LOG_DEBUG_OR_ERROR(ret, "IMP_IVS_DestroyGroup(0)");

  IMP_IVS_DestroyMoveInterface(move_intf);

  return ret;
}

void *Motion::run(void *arg) {
  ((Motion *)arg)->detect();
  return nullptr;
}
