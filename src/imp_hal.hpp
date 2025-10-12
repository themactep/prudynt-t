#pragma once

#include <cstdint>
#include <imp/imp_common.h>
#include <imp/imp_encoder.h>

// Normalize IMP type names across SDKs
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
  #define IMPEncoderCHNAttr IMPEncoderChnAttr
  #define IMPEncoderCHNStat IMPEncoderChnStat
  #define HAL_ENC_ATTR_WIDTH(a)   ((a).encAttr.uWidth)
  #define HAL_ENC_ATTR_HEIGHT(a)  ((a).encAttr.uHeight)
#else
  #define HAL_ENC_ATTR_WIDTH(a)   ((a).encAttr.picWidth)
  #define HAL_ENC_ATTR_HEIGHT(a)  ((a).encAttr.picHeight)
#endif

struct _stream; // fwd decl

namespace hal {

struct PlatformCaps {
    bool has_h265;
    bool has_capped_quality;
    bool has_capped_vbr;
    bool has_ip_pb_delta;
    bool has_bufshare;
    bool has_jpeg_set_qtable;
};

const PlatformCaps& caps();

// Safe no-op on platforms without user qtable
void set_jpeg_quality_qtable(int encChn, int quality /*1..100*/, const char* cpu_hint);

// Buffer-share channel (T31/T40/T41/C100 families)
// Apply optional RC overrides from stream{0,1} fields
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
void apply_rc_overrides(IMPEncoderCHNAttr &chnAttr, IMPEncoderRcMode rcMode, const _stream &stream);
#else
void apply_rc_overrides(IMPEncoderCHNAttr &chnAttr, int rcMode, const _stream &stream);
#endif

int maybe_enable_bufshare(int jpegEncGrp, int srcEncChn, bool allow_shared);

} // namespace hal

