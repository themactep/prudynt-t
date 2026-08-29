#include "video/IMPFramesource.hpp"
#include "util/Logger.hpp"
#include <algorithm>
#include <cstdio>
#include <dlfcn.h>

#define MODULE "IMP_FRAMESOURCE"

#ifndef ISP_CH0_PRE_DEQUEUE_TIME
#define ISP_CH0_PRE_DEQUEUE_TIME 0
#endif

// Returns total system RAM in bytes, read once from /proc/meminfo.
static long get_total_ram_bytes() {
  static long cached = 0;
  if (cached > 0)
    return cached;
  FILE *f = fopen("/proc/meminfo", "r");
  if (!f)
    return 64 * 1024 * 1024; // conservative fallback
  long kb = 0;
  if (fscanf(f, "MemTotal: %ld kB", &kb) == 1)
    cached = kb * 1024L;
  fclose(f);
  return cached > 0 ? cached : 64 * 1024 * 1024;
}

IMPFramesource *IMPFramesource::createNew(_stream *stream, _sensor *sensor,
                                          int chnNr) {
  return new IMPFramesource(stream, sensor, chnNr);
}

int IMPFramesource::init() {
  LOG_DEBUG("IMPFramesource::init()");

  int ret = 0, scale = 0;

  IMPFSChnAttr chnAttr;
  memset(&chnAttr, 0, sizeof(IMPFSChnAttr));

  ret = IMP_FrameSource_GetChnAttr(chnNr, &chnAttr);

  // Set required base attributes
  chnAttr.picWidth = stream->width;
  chnAttr.picHeight = stream->height;
  chnAttr.pixFmt = PIX_FMT_NV12;
  chnAttr.outFrmRateNum = stream->fps;
  chnAttr.outFrmRateDen = 1;
  // Auto-calculate buffer count based on fps when set to -1.
  // Scale with fps for pipeline headroom, then cap to keep framesource
  // buffers within ~15% of total RAM (protects 64MB devices).
  int auto_buffers = std::max(2, (stream->fps + 9) / 10);
  long frame_bytes =
      static_cast<long>(stream->width) * stream->height * 3 / 2; // NV12
  long ram_budget = get_total_ram_bytes() * 15 / 100;
  int mem_cap = std::max(2, static_cast<int>(ram_budget / frame_bytes));
  int min_bufs;
  // T23 encoder requires >=2 buffers; large frames (>4 MB) also need >=2
  // to prevent ISP/encoder read-write collisions that corrupt macroblocks.
#if defined(PLATFORM_T23)
  min_bufs = 2;
#else
  min_bufs = (frame_bytes > 4 * 1024 * 1024) ? 2 : 1;
#endif
  auto_buffers = std::max(min_bufs, std::min(auto_buffers, std::max(mem_cap, min_bufs)));
  if (stream->buffers > 0) {
    chnAttr.nrVBs = stream->buffers;
  } else {
    LOG_INFO("Channel " << chnNr << ": auto buffers=" << auto_buffers
                        << " (fps=" << stream->fps
                        << ", frame=" << frame_bytes / 1024 << "KB"
                        << ", RAM=" << get_total_ram_bytes() / 1024 / 1024
                        << "MB)");
    chnAttr.nrVBs = auto_buffers;
  }
  chnAttr.type = FS_PHY_CHANNEL;

  chnAttr.crop.enable = 0;
  chnAttr.crop.top = 0;
  chnAttr.crop.left = 0;
  chnAttr.crop.width = sensor->width;
  chnAttr.crop.height = sensor->height;

  if ((sensor->width != stream->width) || (sensor->height != stream->height)) {
    scale = 1;
  } else {
    scale = 0;
  }
  chnAttr.scaler.enable = scale;
  chnAttr.scaler.outwidth = stream->width;
  chnAttr.scaler.outheight = stream->height;
  chnAttr.picWidth = stream->width;
  chnAttr.picHeight = stream->height;

#if defined(PLATFORM_T31)
  // tx-isp runs the non-scaled physical channel in a single-buffer
  // schedule when isp_ch0_pre_dequeue_time is configured; requesting
  // 2+ buffers then makes the driver reject the schedule and dump the
  // stack ("one buffer schedule only support nrvbs = 1").  Clamp the
  // auto buffer count to match.  An explicit "buffers" value in the
  // config is trusted as-is.
  if (!scale && stream->buffers <= 0 && ISP_CH0_PRE_DEQUEUE_TIME > 0 &&
      chnAttr.nrVBs > 1) {
    LOG_INFO("Channel " << chnNr
                        << ": isp_ch0_pre_dequeue_time is set --- clamping "
                           "non-scaled channel to nrVBs=1");
    chnAttr.nrVBs = 1;
  }
  fs_scale = (scale != 0);
  nrVBs_was_auto = (stream->buffers <= 0);
#endif

  LOG_DEBUG("Channel " << chnNr << " configuration (post-attr):");
  LOG_DEBUG("  pic: " << chnAttr.picWidth << "x" << chnAttr.picHeight);
  LOG_DEBUG("  crop.enable=" << chnAttr.crop.enable
                             << " crop=" << chnAttr.crop.width << "x"
                             << chnAttr.crop.height);
  LOG_DEBUG("  scaler.enable=" << chnAttr.scaler.enable
                               << " out=" << chnAttr.scaler.outwidth << "x"
                               << chnAttr.scaler.outheight);
  LOG_DEBUG("  fps=" << chnAttr.outFrmRateNum << "/" << chnAttr.outFrmRateDen
                     << " nrVBs=" << chnAttr.nrVBs
                     << " pixFmt=" << chnAttr.pixFmt);

#if !defined(KERNEL_VERSION_4)
#if defined(PLATFORM_T31) && !defined(PLATFORM_C100)

  // Rotation handled client-side via SEI metadata --- skip IMP rotation.
  if (false && stream->rotation != 0) {
    // Validate 64-bit alignment requirement
    // if (stream->width % 64 != 0 || stream->height % 64 != 0) {
    //   LOG_ERROR(
    //       "Rotation requires 64-bit aligned resolution. "
    //       "Current: "
    //       << stream->width << "x" << stream->height
    //       << ". "
    //          "Please use multiples of 64 (e.g., 1920x1080, 1280x720, 640x480)");
    //   return -1;
    // }

    // Check for soft zoom conflict
    // if (stream->scale_enabled) {
    //   LOG_ERROR("Cannot enable rotation while soft zoom is active. Disable "
    //             "scale_enabled or set rotation to 0");
    //   return -1;
    // }

    // Warn about performance constraints
    // if (stream->width > 1280 || stream->height > 704) {
    //   LOG_WARN("Rotation above 1280x704 may impact performance. Recommended "
    //            "<=1280x704 @ <=15fps");
    // }

    // Convert degree values to IMP rotation values
    // 0 degrees = 0 (no rotation)
    // 90 degrees = 1 (90 deg counterclockwise)
    // 270 degrees = 2 (90 deg clockwise, equivalent to 270 deg counterclockwise)
    int imp_rotation = 0;
    if (stream->rotation == 90) {
      imp_rotation = 1;
    } else if (stream->rotation == 270) {
      imp_rotation = 2;
    }

    LOG_DEBUG("Setting video rotation "
              << stream->rotation << " degrees (IMP value " << imp_rotation
              << ")");

    typedef int (*pfn_fs_rotate)(int, int, int, int);
    void *handle = dlopen(nullptr, RTLD_LAZY);
    pfn_fs_rotate rotate_fn =
        handle ? reinterpret_cast<pfn_fs_rotate>(
                     dlsym(handle, "IMP_FrameSource_SetChnRotate"))
               : nullptr;
    if (rotate_fn) {
      ret = rotate_fn(chnNr, imp_rotation, stream->width, stream->height);
      if (ret != 0) {
        LOG_ERROR("IMP_FrameSource_SetChnRotate failed ret=" << ret
                  << ". Falling back to no rotation.");
        chnAttr.scaler.enable = scale;
      } else {
        LOG_DEBUG("IMP_FrameSource_SetChnRotate OK");
      }
    } else {
      LOG_DEBUG("IMP_FrameSource_SetChnRotate not available; skipping rotation");
      ret = 0;
    }
  }
#endif
#endif

  ret = IMP_FrameSource_CreateChn(chnNr, &chnAttr);
  LOG_DEBUG_OR_ERROR(ret,
                     "IMP_FrameSource_CreateChn(" << chnNr << ", &chnAttr)");

  ret = IMP_FrameSource_SetChnAttr(chnNr, &chnAttr);
  LOG_DEBUG_OR_ERROR(ret,
                     "IMP_FrameSource_SetChnAttr(" << chnNr << ", &chnAttr)");

#if !defined(NO_FIFO)
  IMPFSChnFifoAttr fifo;
  ret = IMP_FrameSource_GetChnFifoAttr(chnNr, &fifo);
  LOG_DEBUG_OR_ERROR(ret,
                     "IMP_FrameSource_GetChnFifoAttr(" << chnNr << ", &fifo)");

  fifo.maxdepth = 0;
  ret = IMP_FrameSource_SetChnFifoAttr(chnNr, &fifo);
  LOG_DEBUG_OR_ERROR(ret,
                     "IMP_FrameSource_SetChnFifoAttr(" << chnNr << ", &fifo)");

  ret = IMP_FrameSource_SetFrameDepth(chnNr, 0);
  LOG_DEBUG_OR_ERROR(ret, "IMP_FrameSource_SetFrameDepth(" << chnNr << ", 0)");
#endif

  // ret = IMP_FrameSource_EnableChn(chnNr);
  // LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_FrameSource_EnableChn(" << chnNr <<
  // ")");

  return ret;
}

int IMPFramesource::enable() {
  int ret;

  ret = IMP_FrameSource_EnableChn(chnNr);

  // On low-RAM devices (36MB T31L, etc.) the DMA pool may be exhausted
  // after encoder + OSD + other channel allocations.  Fall back to a
  // single buffer when double-buffering fails --- one buffer is
  // suboptimal (sensor/encoder may collide), but it's far better than
  // zero frames.
  if (ret != 0) {
    IMPFSChnAttr curAttr;
    memset(&curAttr, 0, sizeof(curAttr));
    int getRet = IMP_FrameSource_GetChnAttr(chnNr, &curAttr);
    if (getRet == 0 && curAttr.nrVBs > 1) {
      LOG_WARN("IMP_FrameSource_EnableChn(" << chnNr << ") with "
               << curAttr.nrVBs << " buffers failed (DMA exhausted? "
               << "ret=" << ret << "), retrying with 1 buffer");

      curAttr.nrVBs = 1;
      ret = IMP_FrameSource_SetChnAttr(chnNr, &curAttr);
      if (ret != 0) {
        LOG_ERROR("IMP_FrameSource_SetChnAttr(" << chnNr
                  << ", nrVBs=1) retry failed: " << ret);
        return ret;
      }

      ret = IMP_FrameSource_EnableChn(chnNr);
      if (ret != 0) {
        LOG_ERROR("IMP_FrameSource_EnableChn(" << chnNr
                  << ") retry with nrVBs=1 also failed: " << ret);
        return ret;
      }
      LOG_INFO("IMP_FrameSource_EnableChn(" << chnNr
               << ") succeeded with nrVBs=1 fallback");
      return 0;
    }
  }

  LOG_DEBUG_OR_ERROR_AND_EXIT(ret,
                              "IMP_FrameSource_EnableChn(" << chnNr << ")");

  return 0;
}

int IMPFramesource::disable() {
  int ret;

  ret = IMP_FrameSource_DisableChn(chnNr);
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret,
                              "IMP_FrameSource_DisableChn(" << chnNr << ")");

  return 0;
}

int IMPFramesource::destroy() {
  int ret;

  ret = IMP_FrameSource_DestroyChn(chnNr);
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret,
                              "IMP_FrameSource_DestroyChn(" << chnNr << ")");

  return 0;
}

bool IMPFramesource::canFallbackToSingleBuffer() const {
#if defined(PLATFORM_T31)
  return !fs_scale && nrVBs_was_auto;
#else
  return false;
#endif
}

int IMPFramesource::fallbackToSingleBuffer() {
  IMPFSChnAttr curAttr;
  memset(&curAttr, 0, sizeof(curAttr));

  int ret = IMP_FrameSource_DisableChn(chnNr);
  if (ret != 0) {
    LOG_ERROR("IMPFramesource::fallbackToSingleBuffer: DisableChn("
              << chnNr << ") failed: " << ret);
    return ret;
  }

  ret = IMP_FrameSource_GetChnAttr(chnNr, &curAttr);
  if (ret != 0) {
    LOG_ERROR("IMPFramesource::fallbackToSingleBuffer: GetChnAttr("
              << chnNr << ") failed: " << ret);
    return ret;
  }
  if (curAttr.nrVBs <= 1) {
    LOG_WARN("IMPFramesource::fallbackToSingleBuffer: channel " << chnNr
             << " already at nrVBs=" << curAttr.nrVBs);
    return -1;
  }

  curAttr.nrVBs = 1;
  ret = IMP_FrameSource_SetChnAttr(chnNr, &curAttr);
  if (ret != 0) {
    LOG_ERROR("IMPFramesource::fallbackToSingleBuffer: SetChnAttr(nrVBs=1) "
              "failed: " << ret);
    return ret;
  }

  ret = IMP_FrameSource_EnableChn(chnNr);
  if (ret != 0) {
    LOG_ERROR("IMPFramesource::fallbackToSingleBuffer: EnableChn failed: "
              << ret);
    return ret;
  }

  LOG_WARN("Channel " << chnNr << ": re-enabled with nrVBs=1 "
           "(old tx-isp driver single-buffer schedule)");
  return 0;
}
