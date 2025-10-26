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
    // Encoder capabilities
    bool has_h265;
    bool has_capped_quality;
    bool has_capped_vbr;
    bool has_ip_pb_delta;
    bool has_bufshare;
    bool has_jpeg_set_qtable;
    bool has_smart_rc;
    bool has_super_frm;
    bool has_intra_refresh;
    
    // Audio capabilities
    bool has_audio_aec_channel;
    bool has_audio_agc;
    bool has_audio_alc;
    bool has_audio_hpf;
    bool has_audio_ns;
    
    // ISP capabilities
    bool has_isp_sinter;
    bool has_isp_temper;
    bool has_isp_hue;
    bool has_isp_dpc;
    bool has_isp_drc;
    bool has_isp_defog;
    bool has_isp_backlight_comp;
    bool has_isp_highlight_depress;
    bool has_isp_ae_comp;
    bool has_isp_max_gain;
    
    // OSD capabilities
    bool has_osd_region_invert;
    
    // System capabilities
    bool uses_xburst2;
    bool uses_kernel_4;
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




namespace isp {

// Basic image quality controls
int set_brightness(unsigned char val);
int set_contrast(unsigned char val);
int set_saturation(unsigned char val);
int set_sharpness(unsigned char val);
int set_sinter_strength(unsigned char val);
int set_temper_strength(unsigned char val);
int set_hue(unsigned char val);

// Flip/mirror controls
int set_hflip(bool enable);
int set_vflip(bool enable);

// ISP modes
int set_running_mode(int mode);
int set_isp_bypass(bool enable);
int set_anti_flicker(int mode);

// Exposure controls
int set_ae_compensation(int val);

// Advanced image processing (may not be available on all platforms)
int set_dpc_strength(unsigned char val);
int set_drc_strength(unsigned char val);
int set_defog_strength(uint8_t val);
int set_backlight_comp(unsigned char val);
int set_highlight_depress(unsigned char val);

// Gain controls
int set_max_again(unsigned char val);
int set_max_dgain(unsigned char val);

// White balance
int set_wb(int mode, unsigned short rgain, unsigned short bgain);

} // namespace isp


// ============================================================================
// Platform Capabilities
// ============================================================================





// Convenience namespace for capability checks
} // namespace hal
