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
} // namespace hal

// ============================================================================
// Platform Capabilities
// ============================================================================

// Platform capability flags
struct PlatformCapabilities {
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
    
    // Encoder capabilities
    bool has_h265;
    bool has_smart_rc;
    bool has_super_frm;
    bool has_intra_refresh;
    
    // OSD capabilities
    bool has_osd_region_invert;
    
    // System capabilities
    bool uses_xburst2;
    bool uses_kernel_4;
};

// Get platform capabilities (singleton)
const PlatformCapabilities& get_platform_caps();

// Convenience namespace for capability checks
namespace caps {
    inline bool has_audio_aec_channel() { return get_platform_caps().has_audio_aec_channel; }
    inline bool has_audio_agc() { return get_platform_caps().has_audio_agc; }
    inline bool has_audio_alc() { return get_platform_caps().has_audio_alc; }
    inline bool has_audio_hpf() { return get_platform_caps().has_audio_hpf; }
    inline bool has_audio_ns() { return get_platform_caps().has_audio_ns; }
    
    inline bool has_isp_sinter() { return get_platform_caps().has_isp_sinter; }
    inline bool has_isp_temper() { return get_platform_caps().has_isp_temper; }
    inline bool has_isp_hue() { return get_platform_caps().has_isp_hue; }
    inline bool has_isp_dpc() { return get_platform_caps().has_isp_dpc; }
    inline bool has_isp_drc() { return get_platform_caps().has_isp_drc; }
    inline bool has_isp_defog() { return get_platform_caps().has_isp_defog; }
    inline bool has_isp_backlight_comp() { return get_platform_caps().has_isp_backlight_comp; }
    inline bool has_isp_highlight_depress() { return get_platform_caps().has_isp_highlight_depress; }
    inline bool has_isp_ae_comp() { return get_platform_caps().has_isp_ae_comp; }
    inline bool has_isp_max_gain() { return get_platform_caps().has_isp_max_gain; }
    
    inline bool has_h265() { return get_platform_caps().has_h265; }
    inline bool has_smart_rc() { return get_platform_caps().has_smart_rc; }
    inline bool has_super_frm() { return get_platform_caps().has_super_frm; }
    inline bool has_intra_refresh() { return get_platform_caps().has_intra_refresh; }
    
    inline bool has_osd_region_invert() { return get_platform_caps().has_osd_region_invert; }
    
    inline bool uses_xburst2() { return get_platform_caps().uses_xburst2; }
    inline bool uses_kernel_4() { return get_platform_caps().uses_kernel_4; }
}

} // namespace isp
} // namespace hal
