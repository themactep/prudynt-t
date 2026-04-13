#include "IMPEncoder.hpp"
#include "Config.hpp"
#include "imp_hal.hpp"
#include <cstdint>
#include <mutex>
#include <sstream>
#include <utility>

#define MODULE "IMPENCODER"

namespace {
inline uint32_t align_up(uint32_t value, uint32_t alignment) {
  if (alignment == 0) {
    return value;
  }
  uint32_t remainder = value % alignment;
  if (remainder == 0) {
    return value;
  }
  return value + (alignment - remainder);
}

#if defined(PLATFORM_T23)
void t23_encoder_global_preclean_once() {
  static std::once_flag once;
  std::call_once(once, []() {
    LOG_WARN("T23 pre-clean: running one-time global encoder sweep");
    for (int ch = 0; ch < 8; ++ch) {
      (void)IMP_Encoder_StopRecvPic(ch);
      (void)IMP_Encoder_UnRegisterChn(ch);
      (void)IMP_Encoder_DestroyChn(ch);
    }
    for (int grp = 0; grp < 8; ++grp) {
      (void)IMP_Encoder_DestroyGroup(grp);
    }
  });
}
#endif
} // namespace

IMPEncoder *IMPEncoder::createNew(_stream *stream, int encChn, int encGrp, int fsChn, const char *name) {
  IMPEncoder *encoder = new IMPEncoder(stream, encChn, encGrp, fsChn, name);
  if (!encoder) {
    return nullptr;
  }

  int ret = encoder->init();
  if (ret != 0) {
    LOG_ERROR("Failed to initialize encoder channel " << encChn << " (grp " << encGrp << ") ret=" << ret);
    delete encoder;
    return nullptr;
  }

  return encoder;
}

void IMPEncoder::flush(int encChn) {
  LOG_DDEBUG("flush(" << encChn << ")");
  IMP_Encoder_RequestIDR(encChn);
}

void MakeTables(int q, uint8_t *lqt, uint8_t *cqt) {
  // Ensure q is within the expected range
  q = std::max(1, std::min(q, 99));

  // Adjust q based on factor
  if (q < 50) {
    q = 5000 / q;
  } else {
    q = 200 - 2 * q;
  }

  // Fill the quantization tables
  for (int i = 0; i < 64; ++i) {
    int lq = (jpeg_luma_quantizer[i] * q + 50) / 100;
    int cq = (jpeg_chroma_quantizer[i] * q + 50) / 100;

    // Ensure the quantization values are within [1, 255]
    lqt[i] = static_cast<uint8_t>(std::max(1, std::min(lq, 255)));
    cqt[i] = static_cast<uint8_t>(std::max(1, std::min(cq, 255)));
  }
}

void IMPEncoder::initProfile() {
  IMPEncoderRcAttr *rcAttr;
  memset(&chnAttr, 0, sizeof(IMPEncoderCHNAttr));
  rcAttr = &chnAttr.rcAttr;

#ifdef PLATFORM_NEW_SDK
  IMPEncoderRcMode rcMode = IMP_ENC_RC_MODE_CAPPED_QUALITY;
  IMPEncoderProfile encoderProfile = IMP_ENC_PROFILE_AVC_HIGH;

  if (strcmp(stream->format, "H265") == 0) {
    encoderProfile = IMP_ENC_PROFILE_HEVC_MAIN;
  } else if (strcmp(stream->format, "JPEG") == 0) {
    encoderProfile = IMP_ENC_PROFILE_JPEG;
    IMP_Encoder_SetDefaultParam(&chnAttr, encoderProfile, IMP_ENC_RC_MODE_FIXQP, stream->width, stream->height, 24, 1,
                                0, 0, stream->jpeg_quality, 0);
    // 1000 / stream->jpeg_refresh
    LOG_DEBUG("STREAM PROFILE " << encChn << ", " << encGrp << ", " << stream->format << ", "
                                << chnAttr.rcAttr.outFrmRate.frmRateNum << "fps, profile:" << stream->profile << ", "
                                << stream->width << "x" << stream->height);
    return;
  }

  if (strcmp(stream->mode, "FIXQP") == 0) {
    rcMode = IMP_ENC_RC_MODE_FIXQP;
  } else if (strcmp(stream->mode, "VBR") == 0) {
    rcMode = IMP_ENC_RC_MODE_VBR;
  } else if (strcmp(stream->mode, "CBR") == 0) {
    rcMode = IMP_ENC_RC_MODE_CBR;
  } else if (strcmp(stream->mode, "CAPPED_VBR") == 0) {
    rcMode = IMP_ENC_RC_MODE_CAPPED_VBR;
  } else if (strcmp(stream->mode, "CAPPED_QUALITY") == 0) {
    rcMode = IMP_ENC_RC_MODE_CAPPED_QUALITY;
  } else {
    LOG_ERROR("unsupported stream->mode (" << stream->mode
                                           << "). we only support FIXQP, CBR, VBR, CAPPED_VBR and "
                                              "CAPPED_QUALITY on T31");
  }

  IMP_Encoder_SetDefaultParam(&chnAttr, encoderProfile, rcMode, stream->width, stream->height, stream->fps, 1,
                              stream->gop, 2, -1, stream->bitrate);

  switch (rcMode) {
  case IMP_ENC_RC_MODE_FIXQP:
    rcAttr->attrRcMode.attrFixQp.iInitialQP = 38;
    break;
  case IMP_ENC_RC_MODE_CBR:
    rcAttr->attrRcMode.attrCbr.uTargetBitRate = stream->bitrate;
    rcAttr->attrRcMode.attrCbr.iInitialQP = -1;
    rcAttr->attrRcMode.attrCbr.iMinQP = 34;
    rcAttr->attrRcMode.attrCbr.iMaxQP = 51;
    rcAttr->attrRcMode.attrCbr.iIPDelta = -1;
    rcAttr->attrRcMode.attrCbr.iPBDelta = -1;
    // rcAttr->attrRcMode.attrCbr.eRcOptions = IMP_ENC_RC_SCN_CHG_RES |
    // IMP_ENC_RC_OPT_SC_PREVENTION;
    rcAttr->attrRcMode.attrCbr.uMaxPictureSize = stream->bitrate;
    break;
  case IMP_ENC_RC_MODE_VBR:
    rcAttr->attrRcMode.attrVbr.uTargetBitRate = stream->bitrate;
    rcAttr->attrRcMode.attrVbr.uMaxBitRate = stream->bitrate;
    rcAttr->attrRcMode.attrVbr.iInitialQP = -1;
    rcAttr->attrRcMode.attrVbr.iMinQP = 20;
    rcAttr->attrRcMode.attrVbr.iMaxQP = 45;
    rcAttr->attrRcMode.attrVbr.iIPDelta = 3;
    rcAttr->attrRcMode.attrVbr.iPBDelta = 3;
    // rcAttr->attrRcMode.attrVbr.eRcOptions = IMP_ENC_RC_SCN_CHG_RES |
    // IMP_ENC_RC_OPT_SC_PREVENTION;
    rcAttr->attrRcMode.attrVbr.uMaxPictureSize = stream->bitrate;
    break;
  case IMP_ENC_RC_MODE_CAPPED_VBR:
    rcAttr->attrRcMode.attrCappedVbr.uTargetBitRate = stream->bitrate;
    rcAttr->attrRcMode.attrCappedVbr.uMaxBitRate = stream->bitrate;
    rcAttr->attrRcMode.attrCappedVbr.iInitialQP = -1;
    rcAttr->attrRcMode.attrCappedVbr.iMinQP = 20;
    rcAttr->attrRcMode.attrCappedVbr.iMaxQP = 45;
    rcAttr->attrRcMode.attrCappedVbr.iIPDelta = 3;
    rcAttr->attrRcMode.attrCappedVbr.iPBDelta = 3;
    // rcAttr->attrRcMode.attrCappedVbr.eRcOptions = IMP_ENC_RC_SCN_CHG_RES |
    // IMP_ENC_RC_OPT_SC_PREVENTION;
    rcAttr->attrRcMode.attrCappedVbr.uMaxPictureSize = stream->bitrate;
    rcAttr->attrRcMode.attrCappedVbr.uMaxPSNR = 42;
    break;
  case IMP_ENC_RC_MODE_CAPPED_QUALITY:
    rcAttr->attrRcMode.attrCappedQuality.uTargetBitRate = stream->bitrate;
    rcAttr->attrRcMode.attrCappedQuality.uMaxBitRate = stream->bitrate;
    rcAttr->attrRcMode.attrCappedQuality.iInitialQP = -1;
    rcAttr->attrRcMode.attrCappedQuality.iMinQP = 20;
    rcAttr->attrRcMode.attrCappedQuality.iMaxQP = 45;
    rcAttr->attrRcMode.attrCappedQuality.iIPDelta = 3;
    rcAttr->attrRcMode.attrCappedQuality.iPBDelta = 4;
    // rcAttr->attrRcMode.attrCappedQuality.eRcOptions = IMP_ENC_RC_SCN_CHG_RES
    // | IMP_ENC_RC_OPT_SC_PREVENTION;
    rcAttr->attrRcMode.attrCappedQuality.uMaxPictureSize = stream->bitrate;
    rcAttr->attrRcMode.attrCappedQuality.uMaxPSNR = 42;
    break;
  case IMP_ENC_RC_MODE_INVALID:
    break;
  }

  // Apply optional overrides from stream{0,1} via HAL
  hal::apply_rc_overrides(chnAttr, rcMode, *stream);

#elif defined(PLATFORM_OLD_SDK)
  if (strcmp(stream->format, "JPEG") == 0) {
    IMPEncoderAttr *encAttr;
    encAttr = &chnAttr.encAttr;
    encAttr->enType = PT_JPEG;
    encAttr->bufSize = 0;
    encAttr->profile = 2;
    encAttr->picWidth = stream->width;
    encAttr->picHeight = stream->height;
    return;
  } else if (strcmp(stream->format, "H264") == 0) {
    chnAttr.encAttr.enType = PT_H264;
  } else if (strcmp(stream->format, "H265") == 0) {
    chnAttr.encAttr.enType = static_cast<IMPPayloadType>(hal::encoder::get_encoder_type("H265"));
  }

  IMPEncoderRcMode rcMode = ENC_RC_MODE_SMART;

  if (strcmp(stream->mode, "FIXQP") == 0) {
    rcMode = ENC_RC_MODE_FIXQP;
  } else if (strcmp(stream->mode, "VBR") == 0) {
    rcMode = ENC_RC_MODE_VBR;
  } else if (strcmp(stream->mode, "CBR") == 0) {
    rcMode = ENC_RC_MODE_CBR;
  } else if (strcmp(stream->mode, "SMART") == 0) {
    rcMode = ENC_RC_MODE_SMART;
  } else {
    LOG_ERROR("unsupported stream->mode (" << stream->mode << "). we only support FIXQP, CBR, VBR and SMART");
  }

#if defined(PLATFORM_T23)
  if (chnAttr.encAttr.enType == PT_H264 && rcMode == ENC_RC_MODE_SMART) {
    LOG_WARN("T23: forcing H264 RC mode SMART -> CBR for encoder stability");
    rcMode = ENC_RC_MODE_CBR;
  }
#endif

  // 0 = Baseline
  // 1 = Main
  // 2 = High
  // Note: The encoder seems to emit frames at half the
  // requested framerate when the profile is set to Baseline.
  // For this reason, Main or High are recommended.
#if defined(PLATFORM_T23)
  if (chnAttr.encAttr.enType == PT_H264 && (stream->profile < 0 || stream->profile > 1)) {
    LOG_WARN("T23: forcing unsupported H264 profile " << stream->profile << " -> 1 (Main)");
    chnAttr.encAttr.profile = 1;
  } else {
    chnAttr.encAttr.profile = stream->profile;
  }
#else
  chnAttr.encAttr.profile = stream->profile;
#endif
  chnAttr.encAttr.bufSize = 0;

  // Handle video rotation: swap width/height if rotation is applied
  // NOTE: Only swap for H.264/H.265 video streams, NOT for JPEG
  // JPEG is a snapshot format where rotation is already applied at FrameSource level
  int enc_width = stream->width;
  int enc_height = stream->height;
  if (stream->rotation != 0 && strcmp(stream->format, "JPEG") != 0) {
    std::swap(enc_width, enc_height);
    LOG_DEBUG("Encoder dimensions swapped for rotation: " << enc_width << "x" << enc_height << " (original: "
                                                          << stream->width << "x" << stream->height << ")");
  }

  chnAttr.encAttr.picWidth = enc_width;
  chnAttr.encAttr.picHeight = enc_height;
  chnAttr.rcAttr.outFrmRate.frmRateNum = stream->fps;
  chnAttr.rcAttr.outFrmRate.frmRateDen = 1;
  rcAttr->maxGop = stream->max_gop;

  if (chnAttr.encAttr.enType == PT_H264) {
    switch (rcMode) {
    case ENC_RC_MODE_FIXQP:
      rcAttr->attrRcMode.rcMode = ENC_RC_MODE_FIXQP;
      rcAttr->attrRcMode.attrH264FixQp.qp = 42;
      break;
    case ENC_RC_MODE_CBR:
      rcAttr->attrRcMode.rcMode = ENC_RC_MODE_CBR;
      rcAttr->attrRcMode.attrH264Cbr.outBitRate = stream->bitrate;
      rcAttr->attrRcMode.attrH264Cbr.maxQp = 45;
      rcAttr->attrRcMode.attrH264Cbr.minQp = 15;
      rcAttr->attrRcMode.attrH264Cbr.iBiasLvl = 0;
      rcAttr->attrRcMode.attrH264Cbr.frmQPStep = 3;
      rcAttr->attrRcMode.attrH264Cbr.gopQPStep = 15;
      rcAttr->attrRcMode.attrH264Cbr.adaptiveMode = false;
      rcAttr->attrRcMode.attrH264Cbr.gopRelation = false;
      break;
    case ENC_RC_MODE_VBR:
      rcAttr->attrRcMode.rcMode = ENC_RC_MODE_VBR;
      rcAttr->attrRcMode.attrH264Vbr.maxQp = 45;
      rcAttr->attrRcMode.attrH264Vbr.minQp = 15;
      rcAttr->attrRcMode.attrH264Vbr.staticTime = 2;
      rcAttr->attrRcMode.attrH264Vbr.maxBitRate = stream->bitrate;
      rcAttr->attrRcMode.attrH264Vbr.iBiasLvl = 0;
      rcAttr->attrRcMode.attrH264Vbr.changePos = 80;
      rcAttr->attrRcMode.attrH264Vbr.qualityLvl = 2;
      rcAttr->attrRcMode.attrH264Vbr.frmQPStep = 3;
      rcAttr->attrRcMode.attrH264Vbr.gopQPStep = 15;
      rcAttr->attrRcMode.attrH264Vbr.gopRelation = false;
      break;
    case ENC_RC_MODE_SMART:
      rcAttr->attrRcMode.rcMode = ENC_RC_MODE_SMART;
      rcAttr->attrRcMode.attrH264Smart.maxQp = 45;
      rcAttr->attrRcMode.attrH264Smart.minQp = 24;
      rcAttr->attrRcMode.attrH264Smart.staticTime = 2;
      rcAttr->attrRcMode.attrH264Smart.maxBitRate = stream->bitrate;
      rcAttr->attrRcMode.attrH264Smart.iBiasLvl = 0;
      rcAttr->attrRcMode.attrH264Smart.changePos = 80;
      rcAttr->attrRcMode.attrH264Smart.qualityLvl = 2;
      rcAttr->attrRcMode.attrH264Smart.frmQPStep = 3;
      rcAttr->attrRcMode.attrH264Smart.gopQPStep = 15;
      rcAttr->attrRcMode.attrH264Smart.gopRelation = false;
      break;
    case ENC_RC_MODE_INV:
      break;
    }
#if defined(PLATFORM_T21) || defined(PLATFORM_T30)
  } else if (chnAttr.encAttr.enType == PT_H265) {
    // H.265 rate control configuration (T21/T30 with older SDK)
    rcAttr->attrRcMode.rcMode = ENC_RC_MODE_SMART;
    rcAttr->attrRcMode.attrH265Smart.maxQp = 45;
    rcAttr->attrRcMode.attrH265Smart.minQp = 15;
    rcAttr->attrRcMode.attrH265Smart.staticTime = 2;
    rcAttr->attrRcMode.attrH265Smart.maxBitRate = stream->bitrate;
    rcAttr->attrRcMode.attrH265Smart.iBiasLvl = 0;
    rcAttr->attrRcMode.attrH265Smart.changePos = 80;
    rcAttr->attrRcMode.attrH265Smart.qualityLvl = 2;
    rcAttr->attrRcMode.attrH265Smart.frmQPStep = 3;
    rcAttr->attrRcMode.attrH265Smart.gopQPStep = 15;
    rcAttr->attrRcMode.attrH265Smart.flucLvl = 2;
#endif
  }
  // Optional overrides via HAL (legacy platforms)
  hal::apply_rc_overrides(chnAttr, rcMode, *stream);

  rcAttr->attrHSkip.hSkipAttr.skipType = IMP_Encoder_STYPE_N1X;
  rcAttr->attrHSkip.hSkipAttr.m = rcAttr->maxGop - 1;
  rcAttr->attrHSkip.hSkipAttr.n = 1;
  rcAttr->attrHSkip.hSkipAttr.maxSameSceneCnt = 0;
  rcAttr->attrHSkip.hSkipAttr.bEnableScenecut = 0;
  rcAttr->attrHSkip.hSkipAttr.bBlackEnhance = 0;
  rcAttr->attrHSkip.maxHSkipType = IMP_Encoder_STYPE_N1X;
#endif // PLATFORM_OLD_SDK
  LOG_DEBUG("STREAM PROFILE " << stream->rtsp_endpoint << ", "
                              << "fps:" << chnAttr.rcAttr.outFrmRate.frmRateNum << ", "
                              << "bps:" << stream->bitrate << ", "
                              << "gop:" << stream->gop << ", "
                              << "profile:" << stream->profile << ", " <<
            //        "mode:" << rcMode << ", " <<
            stream->width << "x" << stream->height);
}

int IMPEncoder::init() {
  LOG_DEBUG("IMPEncoder::init(" << encChn << ", " << encGrp << ")");

  int ret = 0;
  is_jpeg_stream = (strcmp(stream->format, "JPEG") == 0);
  const bool is_jpeg = is_jpeg_stream;

  initProfile();

  if (!is_jpeg && hal::encoder::supports_attr_bufsize() && hal::encoder::get_attr_bufsize(chnAttr) == 0) {
    uint32_t yuv_size = static_cast<uint32_t>(stream->width) * static_cast<uint32_t>(stream->height) * 3 / 2;
    hal::encoder::set_attr_bufsize(chnAttr, align_up(yuv_size, 1024));
    LOG_DEBUG("Encoder bufSize auto-set to " << hal::encoder::get_attr_bufsize(chnAttr));
  }

  // On T31-family SoCs, JPEG channels (2/3) must share buffers with a video channel (0/1)
  // Call bufshare BEFORE creating the JPEG channel. Use (jpegEncChn=encChn, shareChn=encGrp).
  if (is_jpeg && stream->allow_shared && hal::caps().has_bufshare) {
    ret = hal::maybe_enable_bufshare(encChn, encGrp, stream->allow_shared);
    LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "hal::maybe_enable_bufshare(" << encChn << ", " << encGrp << ")");
  }

#if defined(PLATFORM_T23)
  t23_encoder_global_preclean_once();
  {
    int cleanup_ret = IMP_Encoder_StopRecvPic(encChn);
    if (cleanup_ret == 0) {
      LOG_WARN("T23 pre-clean: stopped stale recv on ch " << encChn);
    }
    cleanup_ret = IMP_Encoder_UnRegisterChn(encChn);
    if (cleanup_ret == 0) {
      LOG_WARN("T23 pre-clean: unregistered stale ch " << encChn);
    }
    cleanup_ret = IMP_Encoder_DestroyChn(encChn);
    if (cleanup_ret == 0) {
      LOG_WARN("T23 pre-clean: destroyed stale ch " << encChn);
    }
    if (!is_jpeg && ownsGroupResources()) {
      cleanup_ret = IMP_Encoder_DestroyGroup(encGrp);
      if (cleanup_ret == 0) {
        LOG_WARN("T23 pre-clean: destroyed stale grp " << encGrp);
      }
    }
  }
#endif

  if (!is_jpeg && ownsGroupResources()) {
    ret = IMP_Encoder_CreateGroup(encGrp);
    if (ret != 0) {
      LOG_ERROR("IMP_Encoder_CreateGroup(" << encGrp << ") failed ret=" << ret);
      return ret;
    }
    group_created = true;
  }

  ret = IMP_Encoder_CreateChn(encChn, &chnAttr);
  if (ret != 0) {
    std::ostringstream oss;
    oss << "IMP_Encoder_CreateChn(" << encChn << ") failed ret=" << ret;
    if (hal::encoder::supports_attr_payload()) {
      oss << " payload=" << hal::encoder::get_attr_payload(chnAttr);
    } else {
      oss << " codec=" << stream->format;
    }
    if (hal::encoder::supports_attr_bufsize()) {
      oss << " bufSize=" << hal::encoder::get_attr_bufsize(chnAttr);
    }
    oss << " res=" << stream->width << "x" << stream->height;
    LOG_ERROR(oss.str());
#if defined(PLATFORM_T23)
    group_created = false;
#endif
    return ret;
  }
  chn_created = true;

  ret = IMP_Encoder_RegisterChn(encGrp, encChn);
  if (ret != 0) {
    LOG_ERROR("IMP_Encoder_RegisterChn(" << encGrp << ", " << encChn << ") failed ret=" << ret);
    return ret;
  }
  chn_registered = true;

  if (!is_jpeg) {
    fs = {DEV_ID_FS, fsChn, 0};
    enc = {DEV_ID_ENC, encGrp, 0};
    osd_cell = {DEV_ID_OSD, encGrp, 0};

    if (!ownsGroupResources()) {
      if (stream->osd.enabled) {
        LOG_ERROR("stream " << name << " cannot enable per-stream OSD while sharing encoder group " << encGrp);
        return -1;
      }
      return ret;
    }

    auto cleanup_manual_osd = [&]() {
      if (!osd_group_manual)
        return;
      if (osd_started_manual) {
        int stop_ret = IMP_OSD_Stop(encGrp);
        LOG_DEBUG_OR_ERROR(stop_ret, "IMP_OSD_Stop(" << encGrp << ")");
        if (stop_ret == 0) {
          osd_started_manual = false;
        }
      }
      int destroy_ret = IMP_OSD_DestroyGroup(encGrp);
      LOG_DEBUG_OR_ERROR(destroy_ret, "IMP_OSD_DestroyGroup(" << encGrp << ")");
      if (destroy_ret == 0) {
        osd_group_manual = false;
      }
    };

    if (stream->osd.enabled) {
      osd = OSD::createNew(stream->osd, encGrp, encChn, name);
    } else {
      ret = IMP_OSD_CreateGroup(encGrp);
      LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_CreateGroup(" << encGrp << ")");
      if (ret != 0) {
        return ret;
      }
      osd_group_manual = true;

      ret = IMP_OSD_Start(encGrp);
      LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_Start(" << encGrp << ")");
      if (ret != 0) {
        cleanup_manual_osd();
        return ret;
      }
      osd_started_manual = true;
    }

    ret = IMP_System_Bind(&fs, &osd_cell);
    LOG_DEBUG_OR_ERROR(ret, "IMP_System_Bind(&fs, &osd_cell)");
    if (ret != 0) {
      cleanup_manual_osd();
      return ret;
    }
    fs_to_osd_bound = true;

    ret = IMP_System_Bind(&osd_cell, &enc);
    LOG_DEBUG_OR_ERROR(ret, "IMP_System_Bind(&osd_cell, &enc)");
    if (ret != 0) {
      cleanup_manual_osd();
      return ret;
    }
    osd_to_enc_bound = true;
  } else {
    hal::set_jpeg_quality_qtable(encChn, stream->jpeg_quality, cfg->sysinfo.cpu);
  }

  return ret;
}

int IMPEncoder::deinit() {
  LOG_DEBUG("IMPEncoder::deinit(" << encChn << ", " << encGrp << ")");

  int ret = 0;

  if (!is_jpeg_stream && ownsGroupResources()) {
    if (osd) {
      if (fs_to_osd_bound) {
        ret = IMP_System_UnBind(&fs, &osd_cell);
        LOG_DEBUG_OR_ERROR(ret, "IMP_System_UnBind(&fs, &osd_cell)");
        fs_to_osd_bound = false;
      }

      if (osd_to_enc_bound) {
        ret = IMP_System_UnBind(&osd_cell, &enc);
        LOG_DEBUG_OR_ERROR(ret, "IMP_System_UnBind(&osd_cell, &enc)");
        osd_to_enc_bound = false;
      }

      osd->exit();
      delete osd;
      osd = nullptr;
    } else {
      if (osd_to_enc_bound) {
        ret = IMP_System_UnBind(&osd_cell, &enc);
        LOG_DEBUG_OR_ERROR(ret, "IMP_System_UnBind(&osd_cell, &enc)");
        osd_to_enc_bound = false;
      }
      if (fs_to_osd_bound) {
        ret = IMP_System_UnBind(&fs, &osd_cell);
        LOG_DEBUG_OR_ERROR(ret, "IMP_System_UnBind(&fs, &osd_cell)");
        fs_to_osd_bound = false;
      }
    }
  }

  if (chn_created) {
    ret = IMP_Encoder_StopRecvPic(encChn);
    LOG_DEBUG_OR_ERROR(ret, "IMP_Encoder_StopRecvPic(" << encChn << ")");
  }

  if (chn_registered) {
    ret = IMP_Encoder_UnRegisterChn(encChn);
    LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_Encoder_UnRegisterChn(" << encChn << ")");
    chn_registered = false;
  }

  if (chn_created) {
    ret = IMP_Encoder_DestroyChn(encChn);
    LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_Encoder_DestroyChn(" << encChn << ")");
    chn_created = false;
  }

  return ret;
}

int IMPEncoder::destroy() {
  int ret = 0;

  // Only destroy group for non-JPEG streams (JPEG streams don't create groups)
  if (!is_jpeg_stream && group_created) {
    ret = IMP_Encoder_DestroyGroup(encGrp);
    LOG_DEBUG_OR_ERROR(ret, "IMP_Encoder_DestroyGroup(" << encGrp << ")");
    if (ret == 0) {
      group_created = false;
    }
  }

  if (!is_jpeg_stream && osd_group_manual) {
    if (osd_started_manual) {
      int stop_ret = IMP_OSD_Stop(encGrp);
      LOG_DEBUG_OR_ERROR(stop_ret, "IMP_OSD_Stop(" << encGrp << ")");
      if (stop_ret == 0) {
        osd_started_manual = false;
      }
    }
    int destroy_ret = IMP_OSD_DestroyGroup(encGrp);
    LOG_DEBUG_OR_ERROR(destroy_ret, "IMP_OSD_DestroyGroup(" << encGrp << ")");
    if (destroy_ret == 0) {
      osd_group_manual = false;
    }
  }

  return ret;
}
