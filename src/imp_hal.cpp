#include "imp_hal.hpp"

#include "Config.hpp"
#include "Logger.hpp"

#include <cstring>
#include <dlfcn.h>

#include <imp/imp_encoder.h>
#include <imp/imp_isp.h>

extern void MakeTables(int q, uint8_t *lqt, uint8_t *cqt);

namespace hal {

static PlatformCaps g_caps = {
// Encoder capabilities (must match struct order)
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
    .has_h265 = true,
    .has_capped_quality = true,
    .has_capped_vbr = true,
    .has_ip_pb_delta = true,
    .has_bufshare = true,
    .has_jpeg_set_qtable = false,
    .has_smart_rc = true,
    .has_super_frm = true,
    .has_intra_refresh = true,
#elif defined(PLATFORM_T21) || defined(PLATFORM_T30)
    .has_h265 = true,
    .has_capped_quality = false,
    .has_capped_vbr = false,
    .has_ip_pb_delta = false,
    .has_bufshare = false,
    .has_jpeg_set_qtable = true,
    .has_smart_rc = false,
    .has_super_frm = false,
    .has_intra_refresh = false,
#else
    .has_h265 = false,
    .has_capped_quality = false,
    .has_capped_vbr = false,
    .has_ip_pb_delta = false,
    .has_bufshare = false,
    .has_jpeg_set_qtable = true,
    .has_smart_rc = false,
    .has_super_frm = false,
    .has_intra_refresh = false,
#endif

// Audio capabilities (must match struct order)
#if defined(PLATFORM_T23) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
    .has_audio_aec_channel = true,
#else
    .has_audio_aec_channel = false,
#endif

#if defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T23) || defined(PLATFORM_T30) ||                \
    defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
    .has_audio_agc = true,
#else
    .has_audio_agc = false,
#endif

#if defined(PLATFORM_T21) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
    .has_audio_alc = true,
#else
    .has_audio_alc = false,
#endif

#if defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T23) || defined(PLATFORM_T30) ||                \
    defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
    .has_audio_hpf = true,
    .has_audio_ns = true,
#else
    .has_audio_hpf = false,
    .has_audio_ns = false,
#endif

// ISP capabilities (must match struct order)
#if defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T23) ||                \
    defined(PLATFORM_T30) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
    .has_isp_sinter = true,
#else
    .has_isp_sinter = false,
#endif

#if defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T23) ||                \
    defined(PLATFORM_T30) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
    .has_isp_temper = true,
#else
    .has_isp_temper = false,
#endif

#if defined(PLATFORM_T23) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
    .has_isp_hue = true,
#else
    .has_isp_hue = false,
#endif

#if defined(PLATFORM_T31) || defined(PLATFORM_C100)
    .has_isp_dpc = true,
#else
    .has_isp_dpc = false,
#endif

#if defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T23) ||                \
    defined(PLATFORM_T30) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
    .has_isp_drc = true,
#else
    .has_isp_drc = false,
#endif

#if defined(PLATFORM_T23) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
    .has_isp_defog = true,
    .has_isp_backlight_comp = true,
#else
    .has_isp_defog = false,
    .has_isp_backlight_comp = false,
#endif

#if defined(PLATFORM_T21) || defined(PLATFORM_T23) || defined(PLATFORM_T30) || defined(PLATFORM_T31) ||                \
    defined(PLATFORM_C100)
    .has_isp_highlight_depress = true,
#else
    .has_isp_highlight_depress = false,
#endif

#if !defined(PLATFORM_T21) && !defined(PLATFORM_T40) && !defined(PLATFORM_T41)
    .has_isp_ae_comp = true,
#else
    .has_isp_ae_comp = false,
#endif

#if !defined(PLATFORM_T40) && !defined(PLATFORM_T41)
    .has_isp_max_gain = true,
#else
    .has_isp_max_gain = false,
#endif

// ISP control capabilities
#if !defined(PLATFORM_T40) && !defined(PLATFORM_T41)
    .has_isp_hflip = true,
    .has_isp_vflip = true,
#else
    .has_isp_hflip = false, // T40/T41 use combined HVFLIP
    .has_isp_vflip = false, // T40/T41 use combined HVFLIP
#endif

    .has_isp_running_mode = true, // All platforms support running mode
    .has_isp_anti_flicker = true, // All platforms support anti-flicker
    .has_isp_wb = true,           // All platforms support white balance

// OSD capabilities
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
    .has_osd_region_invert = true,
#else
    .has_osd_region_invert = false,
#endif

// Framesource capabilities
#if defined(PLATFORM_T31)
    .has_framesource_chn_rotate = true,
#else
    .has_framesource_chn_rotate = false,
#endif

// Motion detection capabilities
#if defined(PLATFORM_T20)
    .motion_sensitivity_max = 4,
#else
    .motion_sensitivity_max = 8,
#endif

// System capabilities
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
    .uses_xburst2 = true,
    .uses_kernel_4 = true,
#else
    .uses_xburst2 = false,
    .uses_kernel_4 = false,
#endif
};

const PlatformCaps &caps() {
  return g_caps;
}

namespace defaults {

const EncoderDefaults &encoder() {
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
  static constexpr EncoderDefaults defaults{"FIXQP", "CAPPED_QUALITY", 4, 2};
#elif defined(PLATFORM_T23)
  static constexpr EncoderDefaults defaults{"SMART", "SMART", 2, 2};
#else
  static constexpr EncoderDefaults defaults{"SMART", "SMART", 2, 2};
#endif
  return defaults;
}

const DenoiseDefaults &denoise() {
  // Initializer order: {sinter_default, temper_default, sinter_min, sinter_max, temper_min, temper_max}
#if defined(PLATFORM_C100) || defined(PLATFORM_T23) || defined(PLATFORM_T31) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
  static constexpr DenoiseDefaults defaults{128, 128, 0, 255, 0, 255};
#elif defined(PLATFORM_T10) || defined(PLATFORM_T20)
  static constexpr DenoiseDefaults defaults{14, 95, 0, 255, 0, 255};
#else
  static constexpr DenoiseDefaults defaults{50, 50, 50, 150, 50, 150};
#endif
  return defaults;
}

} // namespace defaults

void set_jpeg_quality_qtable(int encChn, int quality, const char *cpu_hint) {
  if (quality < 1 || quality > 100)
    return;

  if (!caps().has_jpeg_set_qtable) {
    // Not supported on T31/T40/T41/C100 SDKs (JPEG tables fixed by SDK)
    return;
  }

#if !(defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41))
  IMPEncoderJpegeQl pst{};
  if (cpu_hint && strncmp(cpu_hint, "T10", 3) == 0) {
    pst.user_ql_en = 0;
    LOG_DEBUG("HAL JPEG: default quantization (T10 family)");
  } else {
    uint8_t lqt[64], cqt[64];
    MakeTables(quality, lqt, cqt);
    for (int i = 0; i < 64; ++i)
      pst.qmem_table[i] = lqt[i];
    for (int i = 0; i < 64; ++i)
      pst.qmem_table[64 + i] = cqt[i];
    pst.user_ql_en = 1;
    LOG_DEBUG("HAL JPEG: custom quantization tables set");
  }
  IMP_Encoder_SetJpegeQl(encChn, &pst);
#else
  (void)encChn;
  (void)cpu_hint; // no-op
#endif
}

int maybe_enable_bufshare(int jpegEncChn, int shareChn, bool allow_shared) {
  if (!allow_shared)
    return 0;
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
  // Call vendor API directly; dlsym fails in static builds (symbol not
  // exported)
  int ret = IMP_Encoder_SetbufshareChn(jpegEncChn, shareChn);
  LOG_DEBUG_OR_ERROR(ret, "IMP_Encoder_SetbufshareChn(" << jpegEncChn << ", " << shareChn << ")");
  return ret;
#else
  (void)jpegEncChn;
  (void)shareChn;
  return 0;
#endif
}

#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
void apply_rc_overrides(IMPEncoderCHNAttr &chnAttr, IMPEncoderRcMode rcMode, const _stream &stream) {
  auto *rcAttr = &chnAttr.rcAttr;
  int qp_init = stream.qp_init;
  int qp_min = stream.qp_min;
  int qp_max = stream.qp_max;
  int ip_delta = stream.ip_delta;
  int pb_delta = stream.pb_delta;
  int max_br = stream.max_bitrate;

  switch (rcMode) {
  case IMP_ENC_RC_MODE_FIXQP:
    if (qp_init >= 0)
      rcAttr->attrRcMode.attrFixQp.iInitialQP = qp_init;
    break;
  case IMP_ENC_RC_MODE_CBR:
    if (qp_init >= 0)
      rcAttr->attrRcMode.attrCbr.iInitialQP = qp_init;
    if (qp_min >= 0)
      rcAttr->attrRcMode.attrCbr.iMinQP = qp_min;
    if (qp_max >= 0)
      rcAttr->attrRcMode.attrCbr.iMaxQP = qp_max;
    if (ip_delta != -1)
      rcAttr->attrRcMode.attrCbr.iIPDelta = ip_delta;
    if (pb_delta != -1)
      rcAttr->attrRcMode.attrCbr.iPBDelta = pb_delta;
    break;
  case IMP_ENC_RC_MODE_VBR:
    if (qp_init >= 0)
      rcAttr->attrRcMode.attrVbr.iInitialQP = qp_init;
    if (qp_min >= 0)
      rcAttr->attrRcMode.attrVbr.iMinQP = qp_min;
    if (qp_max >= 0)
      rcAttr->attrRcMode.attrVbr.iMaxQP = qp_max;
    if (ip_delta != -1)
      rcAttr->attrRcMode.attrVbr.iIPDelta = ip_delta;
    if (pb_delta != -1)
      rcAttr->attrRcMode.attrVbr.iPBDelta = pb_delta;
    if (max_br > 0)
      rcAttr->attrRcMode.attrVbr.uMaxBitRate = max_br;
    break;
  case IMP_ENC_RC_MODE_CAPPED_VBR:
    if (qp_init >= 0)
      rcAttr->attrRcMode.attrCappedVbr.iInitialQP = qp_init;
    if (qp_min >= 0)
      rcAttr->attrRcMode.attrCappedVbr.iMinQP = qp_min;
    if (qp_max >= 0)
      rcAttr->attrRcMode.attrCappedVbr.iMaxQP = qp_max;
    if (ip_delta != -1)
      rcAttr->attrRcMode.attrCappedVbr.iIPDelta = ip_delta;
    if (pb_delta != -1)
      rcAttr->attrRcMode.attrCappedVbr.iPBDelta = pb_delta;
    if (max_br > 0)
      rcAttr->attrRcMode.attrCappedVbr.uMaxBitRate = max_br;
    break;
  case IMP_ENC_RC_MODE_CAPPED_QUALITY:
    if (qp_init >= 0)
      rcAttr->attrRcMode.attrCappedQuality.iInitialQP = qp_init;
    if (qp_min >= 0)
      rcAttr->attrRcMode.attrCappedQuality.iMinQP = qp_min;
    if (qp_max >= 0)
      rcAttr->attrRcMode.attrCappedQuality.iMaxQP = qp_max;
    if (ip_delta != -1)
      rcAttr->attrRcMode.attrCappedQuality.iIPDelta = ip_delta;
    if (pb_delta != -1)
      rcAttr->attrRcMode.attrCappedQuality.iPBDelta = pb_delta;
    if (max_br > 0)
      rcAttr->attrRcMode.attrCappedQuality.uMaxBitRate = max_br;
    break;
  default:
    break;
  }
}
#else
void apply_rc_overrides(IMPEncoderCHNAttr &chnAttr, int rcMode, const _stream &stream) {
  auto *rcAttr = &chnAttr.rcAttr;
  int qp_init = stream.qp_init;
  int qp_min = stream.qp_min;
  int qp_max = stream.qp_max;
  int max_br = stream.max_bitrate;

  if (chnAttr.encAttr.enType == PT_H264) {
    switch (rcMode) {
    case ENC_RC_MODE_FIXQP:
      if (qp_init >= 0)
        rcAttr->attrRcMode.attrH264FixQp.qp = qp_init;
      break;
    case ENC_RC_MODE_CBR:
      if (qp_min >= 0)
        rcAttr->attrRcMode.attrH264Cbr.minQp = qp_min;
      if (qp_max >= 0)
        rcAttr->attrRcMode.attrH264Cbr.maxQp = qp_max;
      break;
    case ENC_RC_MODE_VBR:
      if (qp_min >= 0)
        rcAttr->attrRcMode.attrH264Vbr.minQp = qp_min;
      if (qp_max >= 0)
        rcAttr->attrRcMode.attrH264Vbr.maxQp = qp_max;
      if (max_br > 0)
        rcAttr->attrRcMode.attrH264Vbr.maxBitRate = max_br;
      break;
    case ENC_RC_MODE_SMART:
      if (qp_min >= 0)
        rcAttr->attrRcMode.attrH264Smart.minQp = qp_min;
      if (qp_max >= 0)
        rcAttr->attrRcMode.attrH264Smart.maxQp = qp_max;
      if (max_br > 0)
        rcAttr->attrRcMode.attrH264Smart.maxBitRate = max_br;
      break;
    default:
      break;
    }
  }
#if defined(PLATFORM_T30)
  else if (chnAttr.encAttr.enType == PT_H265) {
    // Only SMART mode used in current code for H265 on T30
    if (qp_min >= 0)
      rcAttr->attrRcMode.attrH265Smart.minQp = qp_min;
    if (qp_max >= 0)
      rcAttr->attrRcMode.attrH265Smart.maxQp = qp_max;
    if (max_br > 0)
      rcAttr->attrRcMode.attrH265Smart.maxBitRate = max_br;
  }
#endif
}
#endif

// ============================================================================
// ISP Tuning HAL Implementation
// ============================================================================

namespace isp {

int open() {
  return IMP_ISP_Open();
}

int close() {
  return IMP_ISP_Close();
}

int enable_tuning() {
  return IMP_ISP_EnableTuning();
}

int disable_tuning() {
  return IMP_ISP_DisableTuning();
}

#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
#define IMPVI IMPVI_MAIN

// Helper to drive combined HV flip on T40/T41
static int set_hvflip(bool h, bool v) {
  IMPISPHVFLIP desired = IMPISP_FLIP_NORMAL_MODE;
  if (h && v) {
    desired = IMPISP_FLIP_HV_MODE;
  } else if (h) {
    desired = IMPISP_FLIP_H_MODE;
  } else if (v) {
    desired = IMPISP_FLIP_V_MODE;
  }

#if defined(PLATFORM_T41)
  IMPISPHVFLIPAttr attr{};
  if (IMP_ISP_Tuning_GetHVFLIP(IMPVI_MAIN, &attr) != 0) {
    // Fall back to writing only sensor_mode if get fails
    attr.sensor_mode = desired;
  }
  attr.isp_mode[0] = desired; // main channel
  return IMP_ISP_Tuning_SetHVFLIP(IMPVI_MAIN, &attr);
#else
  IMPISPHVFLIP current = IMPISP_FLIP_NORMAL_MODE;
  IMP_ISP_Tuning_GetHVFlip(IMPVI_MAIN, &current); // ignore failure, will overwrite below
  current = desired;
  return IMP_ISP_Tuning_SetHVFLIP(IMPVI_MAIN, &current);
#endif
}
#endif

int set_brightness(unsigned char val) {
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  return IMP_ISP_Tuning_SetBrightness(IMPVI, &val);
#else
  return IMP_ISP_Tuning_SetBrightness(val);
#endif
}

int get_brightness(unsigned char &out_val) {
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  return IMP_ISP_Tuning_GetBrightness(IMPVI, &out_val);
#else
  return IMP_ISP_Tuning_GetBrightness(&out_val);
#endif
}

int set_contrast(unsigned char val) {
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  return IMP_ISP_Tuning_SetContrast(IMPVI, &val);
#else
  return IMP_ISP_Tuning_SetContrast(val);
#endif
}

int get_contrast(unsigned char &out_val) {
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  return IMP_ISP_Tuning_GetContrast(IMPVI, &out_val);
#else
  return IMP_ISP_Tuning_GetContrast(&out_val);
#endif
}

int set_saturation(unsigned char val) {
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  return IMP_ISP_Tuning_SetSaturation(IMPVI, &val);
#else
  return IMP_ISP_Tuning_SetSaturation(val);
#endif
}

int get_saturation(unsigned char &out_val) {
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  return IMP_ISP_Tuning_GetSaturation(IMPVI, &out_val);
#else
  return IMP_ISP_Tuning_GetSaturation(&out_val);
#endif
}

int set_sharpness(unsigned char val) {
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  return IMP_ISP_Tuning_SetSharpness(IMPVI, &val);
#else
  return IMP_ISP_Tuning_SetSharpness(val);
#endif
}

int get_sharpness(unsigned char &out_val) {
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  return IMP_ISP_Tuning_GetSharpness(IMPVI, &out_val);
#else
  return IMP_ISP_Tuning_GetSharpness(&out_val);
#endif
}

int set_sinter_strength(unsigned char val) {
  if (!caps().has_isp_sinter) {
    LOG_DEBUG("set_sinter_strength not supported on this platform");
    return 0;
  }
#if defined(PLATFORM_T23) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
  // Simple value API
  return IMP_ISP_Tuning_SetSinterStrength(val);
#elif defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T30)
  // Struct-based API
  IMPISPSinterDenoiseAttr attr;
  memset(&attr, 0, sizeof(attr));
  attr.enable = IMPISP_TUNING_OPS_MODE_ENABLE;
  attr.type = IMPISP_TUNING_OPS_TYPE_MANUAL;
  attr.sinter_strength = val;
  return IMP_ISP_Tuning_SetSinterDnsAttr(&attr);
#else
  return 0;
#endif
}

int get_sinter_strength(unsigned char &out_val) {
  if (!caps().has_isp_sinter) {
    LOG_DEBUG("get_sinter_strength not supported on this platform");
    return -1;
  }
#if defined(PLATFORM_T23) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
  return IMP_ISP_Tuning_GetSinterStrength(&out_val);
#elif defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T30)
  IMPISPSinterDenoiseAttr attr;
  int ret = IMP_ISP_Tuning_GetSinterDnsAttr(&attr);
  if (ret == 0) {
    out_val = attr.sinter_strength;
  }
  return ret;
#else
  (void)out_val;
  return -1;
#endif
}

int set_temper_strength(unsigned char val) {
  if (!caps().has_isp_temper) {
    LOG_DEBUG("set_temper_strength not supported on this platform");
    return 0;
  }
#if defined(PLATFORM_T23) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
  // Simple value API
  return IMP_ISP_Tuning_SetTemperStrength(val);
#elif defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T30)
  // Struct-based API
  IMPISPTemperDenoiseAttr attr;
  memset(&attr, 0, sizeof(attr));
  attr.type = IMPISP_TEMPER_MANUAL;
  attr.temper_strength = val;
  return IMP_ISP_Tuning_SetTemperDnsAttr(&attr);
#else
  return 0;
#endif
}

int get_temper_strength(unsigned char &out_val) {
  if (!caps().has_isp_temper) {
    LOG_DEBUG("get_temper_strength not supported on this platform");
    return -1;
  }
#if defined(PLATFORM_T23) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
  return IMP_ISP_Tuning_GetTemperStrength(&out_val);
#elif defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T30)
  IMPISPTemperDenoiseAttr attr;
  int ret = IMP_ISP_Tuning_GetTemperDnsAttr(&attr);
  if (ret == 0) {
    out_val = attr.temper_strength;
  }
  return ret;
#else
  (void)out_val;
  return -1;
#endif
}

int set_hue(unsigned char val) {
  if (!caps().has_isp_hue) {
    LOG_DEBUG("set_hue not supported on this platform");
    return 0;
  }
#if defined(PLATFORM_T23) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
  return IMP_ISP_Tuning_SetBcshHue(val);
#elif defined(PLATFORM_T40) || defined(PLATFORM_T41)
  return IMP_ISP_Tuning_SetBcshHue(IMPVI_MAIN, &val);
#else
  return 0;
#endif
}

int get_hue(unsigned char &out_val) {
  if (!caps().has_isp_hue) {
    LOG_DEBUG("get_hue not supported on this platform");
    return -1;
  }
#if defined(PLATFORM_T23) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
  return IMP_ISP_Tuning_GetBcshHue(&out_val);
#elif defined(PLATFORM_T40) || defined(PLATFORM_T41)
  return IMP_ISP_Tuning_GetBcshHue(IMPVI_MAIN, &out_val);
#else
  (void)out_val;
  return -1;
#endif
}
#else
  return 0; // Function doesn't exist on this platform
#endif
}

int set_hflip(bool enable) {
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  // Combined HV flip API
  // Preserve the other axis by querying current state
  bool v = false;
#if defined(PLATFORM_T41)
  IMPISPHVFLIPAttr attr{};
  if (IMP_ISP_Tuning_GetHVFLIP(IMPVI_MAIN, &attr) == 0) {
    v = (attr.isp_mode[0] == IMPISP_FLIP_V_MODE || attr.isp_mode[0] == IMPISP_FLIP_HV_MODE);
  }
#else
  IMPISPHVFLIP hv = IMPISP_FLIP_NORMAL_MODE;
  if (IMP_ISP_Tuning_GetHVFlip(IMPVI_MAIN, &hv) == 0) {
    v = (hv == IMPISP_FLIP_V_MODE || hv == IMPISP_FLIP_HV_MODE);
  }
#endif
  return set_hvflip(enable, v);
#else
  IMPISPTuningOpsMode mode = enable ? IMPISP_TUNING_OPS_MODE_ENABLE : IMPISP_TUNING_OPS_MODE_DISABLE;
  return IMP_ISP_Tuning_SetISPHflip(mode);
#endif
}

int set_vflip(bool enable) {
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  bool h = false;
#if defined(PLATFORM_T41)
  IMPISPHVFLIPAttr attr{};
  if (IMP_ISP_Tuning_GetHVFLIP(IMPVI_MAIN, &attr) == 0) {
    h = (attr.isp_mode[0] == IMPISP_FLIP_H_MODE || attr.isp_mode[0] == IMPISP_FLIP_HV_MODE);
  }
#else
  IMPISPHVFLIP hv = IMPISP_FLIP_NORMAL_MODE;
  if (IMP_ISP_Tuning_GetHVFlip(IMPVI_MAIN, &hv) == 0) {
    h = (hv == IMPISP_FLIP_H_MODE || hv == IMPISP_FLIP_HV_MODE);
  }
#endif
  return set_hvflip(h, enable);
#else
  IMPISPTuningOpsMode mode = enable ? IMPISP_TUNING_OPS_MODE_ENABLE : IMPISP_TUNING_OPS_MODE_DISABLE;
  return IMP_ISP_Tuning_SetISPVflip(mode);
#endif
}

int set_running_mode(int mode) {
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  IMPISPRunningMode m = (IMPISPRunningMode)mode;
  return IMP_ISP_Tuning_SetISPRunningMode(IMPVI, &m);
#else
  return IMP_ISP_Tuning_SetISPRunningMode((IMPISPRunningMode)mode);
#endif
}

int set_running_mode(RunningMode mode) {
  return set_running_mode(static_cast<int>(mode));
}

int get_running_mode(int &out_mode) {
  IMPISPRunningMode mode = IMPISP_RUNNING_MODE_DAY;
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  int ret = IMP_ISP_Tuning_GetISPRunningMode(IMPVI, &mode);
#else
  int ret = IMP_ISP_Tuning_GetISPRunningMode(&mode);
#endif
  if (ret == 0) {
    out_mode = static_cast<int>(mode);
  }
  return ret;
}

int get_ev(int &out_ev) {
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  IMPISPAEExprInfo info{};
  if (IMP_ISP_Tuning_GetAeExprInfo(IMPVI, &info) == 0) {
    out_ev = static_cast<int>(info.ExposureValue);
    return 0;
  }
#elif defined(PLATFORM_T23) || defined(PLATFORM_T31) || defined(PLATFORM_T20) || defined(PLATFORM_C100)
  IMPISPEVAttr ev{};
  if (IMP_ISP_Tuning_GetEVAttr(&ev) == 0) {
    out_ev = static_cast<int>(ev.ev);
    return 0;
  }
#endif
  return -1;
}

int get_awb_weighted_gains(int &out_gr, int &out_gb) {
  out_gr = -1;
  out_gb = -1;
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  IMPISPAWBGlobalStatisInfo gw{};
  if (IMP_ISP_Tuning_GetAwbGlobalStatistics(IMPVI, &gw) == 0) {
    out_gr = static_cast<int>(gw.statis_weight_gain.rgain);
    out_gb = static_cast<int>(gw.statis_weight_gain.bgain);
    return 0;
  }
#elif defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T23) || defined(PLATFORM_T30) ||              \
    defined(PLATFORM_T31) || defined(PLATFORM_C100)
  IMPISPAWBHist awb{};
  int ret = IMP_ISP_Tuning_GetAwbHist(&awb);
  if (ret == 0) {
    // NOTE: AWB histogram gains on T23/T31 platforms typically return 0
    // This is a known limitation - the ISP histogram statistics may not be populated
    // or these fields represent histogram weights rather than applied gains.
    // Use EV (exposure) metric instead for reliable sensor response data.
    out_gr = static_cast<int>(awb.awb_stat.r_gain);
    out_gb = static_cast<int>(awb.awb_stat.b_gain);
    return 0;
  }
#endif
  return -1;
}

int get_total_gain(int &out_gain) {
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  IMPISPAEExprInfo info{};
  int ret = IMP_ISP_Tuning_GetAeExprInfo(IMPVI, &info);
  if (ret == 0) {
    out_gain = static_cast<int>(info.TotalGainDb);
  }
  return ret;
#elif defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T23) || \
    defined(PLATFORM_T30) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
  uint32_t gain = 0;
  int ret = IMP_ISP_Tuning_GetTotalGain(&gain);
  if (ret == 0) {
    out_gain = static_cast<int>(gain);
  }
  return ret;
#else
  (void)out_gain;
  return -1;
#endif
}

int get_ae_luma(int &out_luma) {
#if defined(PLATFORM_T21) || defined(PLATFORM_T23) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
  return IMP_ISP_Tuning_GetAeLuma(&out_luma);
#else
  (void)out_luma;
  return -1;
#endif
}

int get_awb_color_temp(int &out_ct) {
#if defined(PLATFORM_T31) || defined(PLATFORM_C100)
  unsigned int ct = 0;
  int ret = IMP_ISP_Tuning_GetAWBCt(&ct);
  if (ret == 0) {
    out_ct = static_cast<int>(ct);
  }
  return ret;
#else
  (void)out_ct;
  return -1;
#endif
}

int get_ev_attr(IMPISPEVAttr &out_attr) {
#if defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T23) || \
    defined(PLATFORM_T30) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
  return IMP_ISP_Tuning_GetEVAttr(&out_attr);
#else
  (void)out_attr;
  return -1;
#endif
}

int get_ae_attr(IMPISPAEAttr &out_attr) {
#if defined(PLATFORM_T23) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
  return IMP_ISP_Tuning_GetAeAttr(&out_attr);
#else
  (void)out_attr;
  return -1;
#endif
}

int set_isp_bypass(bool enable) {
  IMPISPTuningOpsMode mode = enable ? IMPISP_TUNING_OPS_MODE_ENABLE : IMPISP_TUNING_OPS_MODE_DISABLE;
#if defined(PLATFORM_T41)
  return IMP_ISP_SetISPBypass(IMPVI, &mode);
#elif defined(PLATFORM_T40)
  return IMP_ISP_Tuning_SetISPBypass(IMPVI, &mode);
#else
  return IMP_ISP_Tuning_SetISPBypass(mode);
#endif
}

int set_anti_flicker(int mode) {
  // mode: 0=disable, 1=50Hz, 2=60Hz
  if (mode < 0 || mode > 2)
    return -1;

#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  IMPISPAntiflickerAttr attr{};
  attr.mode = (mode == 0) ? IMPISP_ANTIFLICKER_DISABLE_MODE : IMPISP_ANTIFLICKER_NORMAL_MODE;
  attr.freq = (mode == 2) ? 60 : 50;
  return IMP_ISP_Tuning_SetAntiFlickerAttr(IMPVI, &attr);
#else
  IMPISPAntiflickerAttr attr = IMPISP_ANTIFLICKER_DISABLE;
  if (mode == 1) {
    attr = IMPISP_ANTIFLICKER_50HZ;
  } else if (mode == 2) {
    attr = IMPISP_ANTIFLICKER_60HZ;
  }
  return IMP_ISP_Tuning_SetAntiFlickerAttr(attr);
#endif
}

int get_anti_flicker(int &out_mode) {
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  IMPISPAntiflickerAttr attr{};
  int ret = IMP_ISP_Tuning_GetAntiFlickerAttr(IMPVI, &attr);
  if (ret == 0) {
    if (attr.mode == IMPISP_ANTIFLICKER_DISABLE_MODE) {
      out_mode = 0;
    } else {
      out_mode = (attr.freq == 60) ? 2 : 1;
    }
  }
  return ret;
#else
  IMPISPAntiflickerAttr attr;
  int ret = IMP_ISP_Tuning_GetAntiFlickerAttr(&attr);
  if (ret == 0) {
    if (attr == IMPISP_ANTIFLICKER_DISABLE) {
      out_mode = 0;
    } else if (attr == IMPISP_ANTIFLICKER_50HZ) {
      out_mode = 1;
    } else if (attr == IMPISP_ANTIFLICKER_60HZ) {
      out_mode = 2;
    } else {
      out_mode = 0;
    }
  }
  return ret;
#endif
}

int set_ae_compensation(int val) {
  if (!caps().has_isp_ae_comp) {
    LOG_DEBUG("set_ae_compensation not supported on this platform");
    return 0;
  }
#if !defined(PLATFORM_T21) && !defined(PLATFORM_T40) && !defined(PLATFORM_T41)
  return IMP_ISP_Tuning_SetAeComp(val);
#else
  return 0;
#endif
}

int get_ae_compensation(int &out_val) {
  if (!caps().has_isp_ae_comp) {
    LOG_DEBUG("get_ae_compensation not supported on this platform");
    return -1;
  }
#if !defined(PLATFORM_T21) && !defined(PLATFORM_T40) && !defined(PLATFORM_T41)
  return IMP_ISP_Tuning_GetAeComp(&out_val);
#else
  (void)out_val;
  return -1;
#endif
}

int set_ae_it_max(unsigned int it_max) {
#if defined(PLATFORM_T23) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
  return IMP_ISP_Tuning_SetAe_IT_MAX(it_max);
#else
  (void)it_max;
  LOG_DEBUG("set_ae_it_max not supported on this platform");
  return -1;
#endif
}

int get_ae_it_max(unsigned int &out_it_max) {
#if defined(PLATFORM_T23) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
  return IMP_ISP_Tuning_GetAE_IT_MAX(&out_it_max);
#else
  (void)out_it_max;
  return -1;
#endif
}

int set_ae_min(int min_it, int min_again, int min_it_short, int min_again_short) {
#if defined(PLATFORM_T23) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
  IMPISPAEMin ae_min{};
  ae_min.min_it = min_it;
  ae_min.min_again = min_again;
  ae_min.min_it_short = min_it_short;
  ae_min.min_again_short = min_again_short;
  return IMP_ISP_Tuning_SetAeMin(&ae_min);
#else
  (void)min_it;
  (void)min_again;
  (void)min_it_short;
  (void)min_again_short;
  LOG_DEBUG("set_ae_min not supported on this platform");
  return -1;
#endif
}

int get_ae_min(int &out_min_it, int &out_min_again, int &out_min_it_short, int &out_min_again_short) {
#if defined(PLATFORM_T23) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
  IMPISPAEMin ae_min{};
  int ret = IMP_ISP_Tuning_GetAeMin(&ae_min);
  if (ret == 0) {
    out_min_it = ae_min.min_it;
    out_min_again = ae_min.min_again;
    out_min_it_short = ae_min.min_it_short;
    out_min_again_short = ae_min.min_again_short;
  }
  return ret;
#else
  (void)out_min_it;
  (void)out_min_again;
  (void)out_min_it_short;
  (void)out_min_again_short;
  return -1;
#endif
}

int set_dpc_strength(unsigned char val) {
  if (!caps().has_isp_dpc) {
    LOG_DEBUG("set_dpc_strength not supported on this platform");
    return 0;
  }
#if defined(PLATFORM_T31) || defined(PLATFORM_C100)
  return IMP_ISP_Tuning_SetDPC_Strength(val);
#else
  return 0;
#endif
}

int get_dpc_strength(unsigned char &out_val) {
  if (!caps().has_isp_dpc) {
    LOG_DEBUG("get_dpc_strength not supported on this platform");
    return -1;
  }
#if defined(PLATFORM_T31) || defined(PLATFORM_C100)
  return IMP_ISP_Tuning_GetDPC_Strength(&out_val);
#else
  (void)out_val;
  return -1;
#endif
}

int set_drc_strength(unsigned char val) {
  if (!caps().has_isp_drc) {
    LOG_DEBUG("set_drc_strength not supported on this platform");
    return 0;
  }
#if defined(PLATFORM_T23) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
  // Simple value API
  return IMP_ISP_Tuning_SetDRC_Strength(val);
#elif defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T30)
  // Struct-based API
  IMPISPDrcAttr attr;
  memset(&attr, 0, sizeof(attr));
  attr.mode = IMPISP_DRC_MANUAL;
  attr.drc_strength = val;
  return IMP_ISP_Tuning_SetRawDRC(&attr);
#else
  return 0;
#endif
}

int get_drc_strength(unsigned char &out_val) {
  if (!caps().has_isp_drc) {
    LOG_DEBUG("get_drc_strength not supported on this platform");
    return -1;
  }
#if defined(PLATFORM_T23) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
  return IMP_ISP_Tuning_GetDRC_Strength(&out_val);
#elif defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T30)
  IMPISPDrcAttr attr;
  int ret = IMP_ISP_Tuning_GetRawDRC(&attr);
  if (ret == 0) {
    out_val = attr.drc_strength;
  }
  return ret;
#else
  (void)out_val;
  return -1;
#endif
}

int set_defog_strength(uint8_t val) {
  if (!caps().has_isp_defog) {
    LOG_DEBUG("set_defog_strength not supported on this platform");
    return 0;
  }
#if defined(PLATFORM_T23) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
  return IMP_ISP_Tuning_SetDefog_Strength(reinterpret_cast<uint8_t *>(&val));
#else
  return 0;
#endif
}

int set_backlight_comp(unsigned char val) {
  if (!caps().has_isp_backlight_comp) {
    LOG_DEBUG("set_backlight_comp not supported on this platform");
    return 0;
  }
#if defined(PLATFORM_T23) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
  return IMP_ISP_Tuning_SetBacklightComp(val);
#else
  return 0;
#endif
}

int set_highlight_depress(unsigned char val) {
  if (!caps().has_isp_highlight_depress) {
    LOG_DEBUG("set_highlight_depress not supported on this platform");
    return 0;
  }
#if !defined(PLATFORM_T40) && !defined(PLATFORM_T41) && !defined(PLATFORM_T10) && !defined(PLATFORM_T20)
  return IMP_ISP_Tuning_SetHiLightDepress(val);
#else
  return 0;
#endif
}

#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
int set_auto_zoom(const IMPISPAutoZoom &zoom) {
  IMPISPAutoZoom local = zoom;
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  return IMP_ISP_Tuning_SetAutoZoom(IMPVI, &local);
#else
  return IMP_ISP_Tuning_SetAutoZoom(&local);
#endif
}
#endif

int set_max_again(unsigned char val) {
  if (!caps().has_isp_max_gain) {
    LOG_DEBUG("set_max_again not supported on this platform");
    return 0;
  }
#if !defined(PLATFORM_T40) && !defined(PLATFORM_T41)
  return IMP_ISP_Tuning_SetMaxAgain(val);
#else
  return 0;
#endif
}

int get_max_again(unsigned char &out_val) {
  if (!caps().has_isp_max_gain) {
    LOG_DEBUG("get_max_again not supported on this platform");
    return -1;
  }
#if !defined(PLATFORM_T40) && !defined(PLATFORM_T41)
  return IMP_ISP_Tuning_GetMaxAgain(&out_val);
#else
  (void)out_val;
  return -1;
#endif
}

int set_max_dgain(unsigned char val) {
  if (!caps().has_isp_max_gain) {
    LOG_DEBUG("set_max_dgain not supported on this platform");
    return 0;
  }
#if !defined(PLATFORM_T40) && !defined(PLATFORM_T41)
  return IMP_ISP_Tuning_SetMaxDgain(val);
#else
  return 0;
#endif
}

int get_max_dgain(unsigned char &out_val) {
  if (!caps().has_isp_max_gain) {
    LOG_DEBUG("get_max_dgain not supported on this platform");
    return -1;
  }
#if !defined(PLATFORM_T40) && !defined(PLATFORM_T41)
  return IMP_ISP_Tuning_GetMaxDgain(&out_val);
#else
  (void)out_val;
  return -1;
#endif
}
#if !defined(PLATFORM_T40) && !defined(PLATFORM_T41)
  return IMP_ISP_Tuning_SetMaxDgain(val);
#else
  return 0;
#endif
}

int set_gamma(const uint16_t gamma[129]) {
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  IMPISPGammaAttr attr;
  memset(&attr, 0, sizeof(IMPISPGammaAttr));
  attr.Curve_type = IMP_ISP_GAMMA_CURVE_USER;
  memcpy(attr.gamma, gamma, 129 * sizeof(uint16_t));
  return IMP_ISP_Tuning_SetGammaAttr(IMPVI_MAIN, &attr);
#else
  IMPISPGamma g;
  memcpy(g.gamma, gamma, 129 * sizeof(uint16_t));
  return IMP_ISP_Tuning_SetGamma(&g);
#endif
}

int get_gamma(uint16_t gamma[129]) {
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  IMPISPGammaAttr attr;
  memset(&attr, 0, sizeof(IMPISPGammaAttr));
  int ret = IMP_ISP_Tuning_GetGammaAttr(IMPVI_MAIN, &attr);
  if (ret == 0) {
    memcpy(gamma, attr.gamma, 129 * sizeof(uint16_t));
  }
  return ret;
#else
  IMPISPGamma g;
  int ret = IMP_ISP_Tuning_GetGamma(&g);
  if (ret == 0) {
    memcpy(gamma, g.gamma, 129 * sizeof(uint16_t));
  }
  return ret;
#endif
}

int set_wb(int mode, unsigned short rgain, unsigned short bgain) {
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  // Manual WB API not available on these SDKs
  (void)mode;
  (void)rgain;
  (void)bgain;
  return -1;
#else
  IMPISPWB wb;
  memset(&wb, 0, sizeof(IMPISPWB));
  wb.mode = (isp_core_wb_mode)mode;
  wb.rgain = rgain;
  wb.bgain = bgain;
  return IMP_ISP_Tuning_SetWB(&wb);
#endif
}

int set_awb_weight(const unsigned char weight[15][15]) {
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  IMPISPWeight w;
  memcpy(w.weight, weight, sizeof(w.weight));
  return IMP_ISP_Tuning_SetAwbWeight(IMPVI_MAIN, &w);
#else
  IMPISPWeight w;
  memcpy(w.weight, weight, sizeof(w.weight));
  return IMP_ISP_Tuning_SetAwbWeight(&w);
#endif
}

int get_awb_weight(unsigned char weight[15][15]) {
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  IMPISPWeight w;
  int ret = IMP_ISP_Tuning_GetAwbWeight(IMPVI_MAIN, &w);
  if (ret == 0) {
    memcpy(weight, w.weight, sizeof(w.weight));
  }
  return ret;
#else
  IMPISPWeight w;
  int ret = IMP_ISP_Tuning_GetAwbWeight(&w);
  if (ret == 0) {
    memcpy(weight, w.weight, sizeof(w.weight));
  }
  return ret;
#endif
}

int get_awb_zone(unsigned char zone_r[225], unsigned char zone_g[225], unsigned char zone_b[225]) {
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  // AWB zone stats not available on T40/T41
  (void)zone_r;
  (void)zone_g;
  (void)zone_b;
  return -1;
#else
  IMPISPAWBZone zone;
  int ret = IMP_ISP_Tuning_GetAwbZone(&zone);
  if (ret == 0) {
    memcpy(zone_r, zone.zone_r, 225);
    memcpy(zone_g, zone.zone_g, 225);
    memcpy(zone_b, zone.zone_b, 225);
  }
  return ret;
#endif
}

int set_sensor_fps(int fps_num, int fps_den) {
#if defined(PLATFORM_T40)
  uint32_t num = static_cast<uint32_t>(fps_num);
  uint32_t den = static_cast<uint32_t>(fps_den);
  return IMP_ISP_Tuning_SetSensorFPS(IMPVI_MAIN, &num, &den);
#elif defined(PLATFORM_T41)
  IMPISPSensorFps fps{};
  fps.num = static_cast<uint32_t>(fps_num);
  fps.den = static_cast<uint32_t>(fps_den);
  return IMP_ISP_Tuning_SetSensorFPS(IMPVI_MAIN, &fps);
#else
  return IMP_ISP_Tuning_SetSensorFPS(static_cast<uint32_t>(fps_num), static_cast<uint32_t>(fps_den));
#endif
}

int get_sensor_fps(int &fps_num, int &fps_den) {
#if defined(PLATFORM_T40)
  uint32_t num = 0;
  uint32_t den = 0;
  int ret = IMP_ISP_Tuning_GetSensorFPS(IMPVI_MAIN, &num, &den);
#elif defined(PLATFORM_T41)
  IMPISPSensorFps fps{};
  int ret = IMP_ISP_Tuning_GetSensorFPS(IMPVI_MAIN, &fps);
  uint32_t num = fps.num;
  uint32_t den = fps.den;
#else
  uint32_t num = 0;
  uint32_t den = 0;
  int ret = IMP_ISP_Tuning_GetSensorFPS(&num, &den);
#endif
  if (ret == 0) {
    fps_num = static_cast<int>(num);
    fps_den = static_cast<int>(den);
  }
  return ret;
}

int add_sensor(IMPSensorInfo *sinfo) {
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  return IMP_ISP_AddSensor(IMPVI_MAIN, sinfo);
#else
  return IMP_ISP_AddSensor(sinfo);
#endif
}

int enable_sensor(IMPSensorInfo *sinfo) {
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  return IMP_ISP_EnableSensor(IMPVI_MAIN, sinfo);
#else
  (void)sinfo; // Unused on older platforms
  return IMP_ISP_EnableSensor();
#endif
}

int disable_sensor() {
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  return IMP_ISP_DisableSensor(IMPVI_MAIN);
#else
  return IMP_ISP_DisableSensor();
#endif
}

int del_sensor(IMPSensorInfo *sinfo) {
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  return IMP_ISP_DelSensor(IMPVI_MAIN, sinfo);
#else
  return IMP_ISP_DelSensor(sinfo);
#endif
}

} // namespace isp

// ============================================================================
// Platform Capabilities Implementation
// ============================================================================

// ============================================================================
// Video Encoder Stream HAL Implementation
// ============================================================================

namespace encoder {

uint8_t *get_pack_data_start(const IMPEncoderStream &stream, int pack_index) {
#if defined(PLATFORM_T31) || defined(PLATFORM_T40) || defined(PLATFORM_T41) || defined(PLATFORM_C100)
  return (uint8_t *)stream.virAddr + stream.pack[pack_index].offset;
#else
  return (uint8_t *)stream.pack[pack_index].virAddr;
#endif
}

uint32_t get_pack_data_length(const IMPEncoderStream &stream, int pack_index) {
  return stream.pack[pack_index].length;
}

PackSlices get_pack_slices(const IMPEncoderStream &stream, int pack_index) {
  PackSlices slices{};
  slices.first_ptr = get_pack_data_start(stream, pack_index);
  slices.first_len = get_pack_data_length(stream, pack_index);

  if (!slices.first_ptr || slices.first_len == 0)
    return slices;

#if defined(PLATFORM_T31) || defined(PLATFORM_T40) || defined(PLATFORM_T41) || defined(PLATFORM_C100)
  if (stream.virAddr && stream.streamSize > 0) {
    const auto &pack = stream.pack[pack_index];
    if (pack.offset < stream.streamSize) {
      const uint32_t remainder = stream.streamSize - pack.offset;
      if (pack.length > remainder) {
        slices.first_len = remainder;
        slices.second_ptr = reinterpret_cast<uint8_t *>(stream.virAddr);
        slices.second_len = pack.length - remainder;
      }
    }
  }
#endif

  return slices;
}

int get_h264_nal_type(const IMPEncoderPack &pack) {
#if defined(PLATFORM_T31) || defined(PLATFORM_T40) || defined(PLATFORM_T41) || defined(PLATFORM_C100)
  return pack.nalType.h264NalType;
#else
  return pack.dataType.h264Type;
#endif
}

int get_h265_nal_type(const IMPEncoderPack &pack) {
#if defined(PLATFORM_T31) || defined(PLATFORM_T40) || defined(PLATFORM_T41) || defined(PLATFORM_C100)
  return pack.nalType.h265NalType;
#elif defined(PLATFORM_T30)
  return pack.dataType.h265Type;
#else
  // H.265 not supported on T10/T20/T21/T23
  return -1;
#endif
}

int set_bitrate(int channel, int bitrate) {
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
  return IMP_Encoder_SetChnBitRate(channel, bitrate, bitrate);
#else
  (void)channel;
  (void)bitrate;
  return -1;
#endif
}

int set_gop_length(int channel, int length) {
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
  return IMP_Encoder_SetChnGopLength(channel, length);
#else
  (void)channel;
  (void)length;
  return -1;
#endif
}

int set_rc_mode(int channel, int mode) {
#if defined(PLATFORM_T41)
  (void)channel;
  (void)mode;
  LOG_DEBUG("set_rc_mode not supported on this platform");
  return -1;
#elif defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40)
  IMPEncoderAttrRcMode rcModeCfg;
  int ret = IMP_Encoder_GetChnAttrRcMode(channel, &rcModeCfg);
  if (ret != 0)
    return ret;

  rcModeCfg.rcMode = static_cast<IMPEncoderRcMode>(mode);
  return IMP_Encoder_SetChnAttrRcMode(channel, &rcModeCfg);
#else
  (void)channel;
  (void)mode;
  return -1;
#endif
}

int set_framerate(int channel, int fps_num, int fps_den) {
  IMPEncoderFrmRate frmRate{};
  frmRate.frmRateNum = fps_num;
  frmRate.frmRateDen = fps_den;
  return IMP_Encoder_SetChnFrmRate(channel, &frmRate);
}

int set_qp(int channel, int qp) {
#if defined(PLATFORM_T31) || defined(PLATFORM_C100)
  return IMP_Encoder_SetChnQp(channel, qp);
#elif defined(PLATFORM_T40) || defined(PLATFORM_T41)
  LOG_DEBUG("set_qp not supported on this platform");
  (void)channel;
  (void)qp;
  return -1;
#else
  (void)channel;
  (void)qp;
  return -1;
#endif
}

int set_qp_bounds(int channel, int min_qp, int max_qp) {
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
  return IMP_Encoder_SetChnQpBounds(channel, min_qp, max_qp);
#else
  (void)channel;
  (void)min_qp;
  (void)max_qp;
  return -1;
#endif
}

int set_qp_ip_delta(int channel, int delta) {
#if defined(PLATFORM_T31) || defined(PLATFORM_C100)
  return IMP_Encoder_SetChnQpIPDelta(channel, delta);
#elif defined(PLATFORM_T40) || defined(PLATFORM_T41)
  LOG_DEBUG("set_qp_ip_delta not supported on this platform");
  (void)channel;
  (void)delta;
  return -1;
#else
  (void)channel;
  (void)delta;
  return -1;
#endif
}

void init_encoder_channel_attr(IMPEncoderCHNAttr &chnAttr, const char *format, int width, int height) {
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
  // Newer SDK callers must fully populate chnAttr before creation.
  (void)chnAttr;
  (void)format;
  (void)width;
  (void)height;
#else
  // Older SDKs require explicit init of encoder attributes.
  if (strcmp(format, "JPEG") == 0) {
    IMPEncoderAttr *encAttr = &chnAttr.encAttr;
    encAttr->enType = PT_JPEG;
    encAttr->bufSize = 0;
    encAttr->profile = 2;
    encAttr->picWidth = width;
    encAttr->picHeight = height;
  } else if (strcmp(format, "H264") == 0) {
    chnAttr.encAttr.enType = PT_H264;
  }
#if defined(PLATFORM_T30)
  else if (strcmp(format, "H265") == 0) {
    chnAttr.encAttr.enType = PT_H265;
  }
#endif
#endif
}

int get_encoder_rc_mode_smart() {
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
  return IMP_ENC_RC_MODE_CAPPED_QUALITY;
#else
  return ENC_RC_MODE_SMART;
#endif
}

int get_encoder_profile_high(const char *format) {
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
  if (strcmp(format, "H265") == 0) {
    return IMP_ENC_PROFILE_HEVC_MAIN;
  }
  return IMP_ENC_PROFILE_AVC_HIGH;
#else
  // Older platforms use numeric profile values
  return 2; // High profile
#endif
}

int get_encoder_type(const char *format) {
  if (strcmp(format, "JPEG") == 0) {
    return PT_JPEG;
  } else if (strcmp(format, "H264") == 0) {
    return PT_H264;
  }
#if defined(PLATFORM_T21) || defined(PLATFORM_T30) || defined(PLATFORM_T31) || defined(PLATFORM_C100) || \
    defined(PLATFORM_T40) || defined(PLATFORM_T41)
  else if (strcmp(format, "H265") == 0) {
    return PT_H265;
  }
#endif
  return PT_H264; // Default
}

bool supports_jpeg_quality_table() {
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
  return hal::caps().has_jpeg_set_qtable;
#else
  return !hal::caps().has_jpeg_set_qtable;
#endif
}

} // namespace encoder

namespace audio {

void init_ai_channel_param(IMPAudioIChnParam &param) {
  param.usrFrmDepth = 30;
#if defined(PLATFORM_T23) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
  param.aecChn = AUDIO_AEC_CHANNEL_FIRST_LEFT;
#endif
  param.Rev = 0;
}

int set_ai_hpf(int enable) {
  if (!caps().has_audio_hpf)
    return -1;

#if defined(PLATFORM_T23) || defined(PLATFORM_T31) || defined(PLATFORM_C100) || \
    defined(PLATFORM_T40) || defined(PLATFORM_T41)
  return IMP_AI_SetHpfCoFrequency(enable ? 20 : 0);
#else
  (void)enable;
  return -1;
#endif
}

int set_ai_agc(int gain_level, int max_gain) {
  (void)gain_level; // gain_level handling is SDK-specific; preserved for API compatibility
  if (!caps().has_audio_agc)
    return -1;

  // T21 doesn't have IMP_AI_SetAgcMode - AGC is handled differently
  (void)max_gain;
  return 0; // AGC enabled via IMPAudioIOAttr at channel creation
}

int set_ai_noise_suppression(int level) {
  (void)level;
  // Noise suppression setup varies by SDK; not implemented here.
  if (!caps().has_audio_ns)
    return -1;
  return -1;
}

int set_ai_echo_cancellation(int enable) {
  (void)enable;
  // AEC setup is platform-specific and not implemented in this HAL.
  if (!caps().has_audio_aec_channel)
    return -1;
  return -1;
}

int set_ai_volume(int vol) {
#if defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T23) || defined(PLATFORM_T30) ||              \
    defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
  int audioDevId = 0;
  int aiChn = 0;
  return IMP_AI_SetVol(audioDevId, aiChn, vol);
#else
  (void)vol;
  return -1;
#endif
}

int set_ai_gain(int gain) {
  (void)gain;
  return -1; // No generic AI gain API available
}

int set_ai_alc(int level) {
  if (!caps().has_audio_alc)
    return -1;
#if defined(PLATFORM_T21) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
  return IMP_AI_SetAlcGain(0, 0, level);
#else
  (void)level;
  return -1;
#endif
}

int set_ao_hpf(int enable) {
#if defined(PLATFORM_T23) || defined(PLATFORM_T31) || defined(PLATFORM_C100) || \
    defined(PLATFORM_T40) || defined(PLATFORM_T41)
  return IMP_AO_SetHpfCoFrequency(enable ? 20 : 0);
#else
  (void)enable;
  return -1;
#endif
}

int set_ao_volume(int vol) {
#if defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T23) || defined(PLATFORM_T30) ||              \
    defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
  int audioDevId = 0;
  int aoChn = 0;
  return IMP_AO_SetVol(audioDevId, aoChn, vol);
#else
  (void)vol;
  return -1;
#endif
}

int set_ao_gain(int gain) {
#if defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T23) || defined(PLATFORM_T30) ||              \
    defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
  int audioDevId = 0;
  int aoChn = 0;
  return IMP_AO_SetGain(audioDevId, aoChn, gain);
#else
  (void)gain;
  return -1;
#endif
}

} // namespace audio

namespace osd {

uint32_t black_cover_color() {
#if defined(PLATFORM_T23) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
  return OSD_IPU_BLACK;
#else
  return OSD_BLACK;
#endif
}

int show_region(int handle, int show) {
  return IMP_OSD_ShowRgn(static_cast<IMPRgnHandle>(handle), 0, show);
}

int set_region_pos(int handle, int x, int y) {
  IMPOSDRgnAttr rgnAttr{};
  int ret = IMP_OSD_GetRgnAttr(static_cast<IMPRgnHandle>(handle), &rgnAttr);
  if (ret != 0)
    return ret;
  rgnAttr.rect.p0.x = x;
  rgnAttr.rect.p0.y = y;
  return IMP_OSD_SetRgnAttr(static_cast<IMPRgnHandle>(handle), &rgnAttr);
}

int set_region_alpha(int handle, int alpha) {
  if (handle < 0 || alpha < 0 || alpha > 255)
    return -1;

  IMPOSDRgnAttr rgnAttr{};
  int ret = IMP_OSD_GetRgnAttr(static_cast<IMPRgnHandle>(handle), &rgnAttr);
  if (ret != 0)
    return ret;

#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
  rgnAttr.fmt = PIX_FMT_BGRA;
#else
  rgnAttr.fmt = PIX_FMT_MONOWHITE;
#endif
  // Alpha typically carried in pixel data; we still set fmt to safe value
  return IMP_OSD_SetRgnAttr(static_cast<IMPRgnHandle>(handle), &rgnAttr);
}

int get_region_attr(int handle, IMPOSDRgnAttr &out_attr) {
  if (handle < 0)
    return -1;
  return IMP_OSD_GetRgnAttr(static_cast<IMPRgnHandle>(handle), &out_attr);
}

int get_group_attr(int handle, int group, IMPOSDGrpRgnAttr &out_attr) {
  if (handle < 0 || group < 0)
    return -1;
  return IMP_OSD_GetGrpRgnAttr(static_cast<IMPRgnHandle>(handle), group, &out_attr);
}

int set_region_attr(int handle, const char *params) {
  if (handle < 0 || params == nullptr)
    return -1;

  // Expect params to point to an IMPOSDRgnAttr buffer supplied by caller.
  // Minimal sanity: require caller-provided size >= struct and reject invalid fmt.
  IMPOSDRgnAttr attr{};
  memcpy(&attr, params, sizeof(attr));

  switch (attr.fmt) {
  case PIX_FMT_MONOWHITE:
  case PIX_FMT_MONOBLACK:
  case PIX_FMT_BGRA:
  case PIX_FMT_RGBA:
  case PIX_FMT_RGB24:
    break;
  default:
    return -1;
  }

  return IMP_OSD_SetRgnAttr(static_cast<IMPRgnHandle>(handle), &attr);
}

int set_region_cover(int handle, const char *params) {
  // Same shape as set_region_attr; caller pre-fills coverData.
  return set_region_attr(handle, params);
}

} // namespace osd

} // namespace hal

// C-linkage shim for C callers (e.g., imp_control.cpp)
extern "C" int hal_isp_set_running_mode(int mode) {
  return hal::isp::set_running_mode(mode);
}

void init_encoder_channel_attr(IMPEncoderCHNAttr &chnAttr, const char *format, int width, int height) {
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
  // T31/C100/T40/T41 use newer API - initialization handled by caller
  (void)chnAttr;
  (void)format;
  (void)width;
  (void)height;
#else
  // Older platforms need explicit initialization
  if (strcmp(format, "JPEG") == 0) {
    IMPEncoderAttr *encAttr = &chnAttr.encAttr;
    encAttr->enType = PT_JPEG;
    encAttr->bufSize = 0;
    encAttr->profile = 2;
    encAttr->picWidth = width;
    encAttr->picHeight = height;
  } else if (strcmp(format, "H264") == 0) {
    chnAttr.encAttr.enType = PT_H264;
  }
#if defined(PLATFORM_T30)
  else if (strcmp(format, "H265") == 0) {
    chnAttr.encAttr.enType = PT_H265;
  }
#endif
#endif
}

int get_encoder_rc_mode_smart() {
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
  return IMP_ENC_RC_MODE_CAPPED_QUALITY;
#else
  return ENC_RC_MODE_SMART;
#endif
}

int get_encoder_profile_high(const char *format) {
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
  if (strcmp(format, "H265") == 0) {
    return IMP_ENC_PROFILE_HEVC_MAIN;
  }
  return IMP_ENC_PROFILE_AVC_HIGH;
#else
  // Older platforms use different profile values
  return 2; // High profile
#endif
}

int get_encoder_type(const char *format) {
  if (strcmp(format, "JPEG") == 0) {
    return PT_JPEG;
  } else if (strcmp(format, "H264") == 0) {
    return PT_H264;
  }
#if defined(PLATFORM_T21) || defined(PLATFORM_T30) || defined(PLATFORM_T31) || defined(PLATFORM_C100) || \
    defined(PLATFORM_T40) || defined(PLATFORM_T41)
  else if (strcmp(format, "H265") == 0) {
    return PT_H265;
  }
#endif
  return PT_H264; // Default
}

bool supports_jpeg_quality_table() {
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
  return hal::caps().has_jpeg_set_qtable;
#else
  return !hal::caps().has_jpeg_set_qtable;
#endif
}
