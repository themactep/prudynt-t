#pragma once

#include <cstdint>

#include <imp/imp_audio.h>
#include <imp/imp_common.h>
#include <imp/imp_encoder.h>
#include <imp/imp_isp.h>
#include <imp/imp_osd.h>

// Normalize IMP type names across SDKs
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
#define IMPEncoderCHNAttr IMPEncoderChnAttr
#define IMPEncoderCHNStat IMPEncoderChnStat
#define HAL_ENC_ATTR_WIDTH(a) ((a).encAttr.uWidth)
#define HAL_ENC_ATTR_HEIGHT(a) ((a).encAttr.uHeight)
#else
#define HAL_ENC_ATTR_WIDTH(a) ((a).encAttr.picWidth)
#define HAL_ENC_ATTR_HEIGHT(a) ((a).encAttr.picHeight)
#endif

struct _stream; // fwd decl

namespace hal {

// Type compatibility for different platform APIs
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
#define IMPEncoderCHNAttr IMPEncoderChnAttr
#define IMPEncoderCHNStat IMPEncoderChnStat
// OSD field name compatibility

#endif

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
  bool has_isp_hflip;
  bool has_isp_vflip;
  bool has_isp_running_mode;
  bool has_isp_anti_flicker;
  bool has_isp_wb;

  // OSD capabilities
  bool has_osd_region_invert;

  // Framesource capabilities
  bool has_framesource_chn_rotate;

  // Motion detection capabilities
  int motion_sensitivity_max;

  // System capabilities
  bool uses_xburst2;
  bool uses_kernel_4;
};

const PlatformCaps &caps();

namespace defaults {

struct EncoderDefaults {
  const char *stream0_mode;
  const char *stream1_mode;
  int stream0_buffers;
  int stream1_buffers;
};

struct DenoiseDefaults {
  int sinter_default;
  int temper_default;
  int sinter_min;
  int sinter_max;
  int temper_min;
  int temper_max;
};

const EncoderDefaults &encoder();
const DenoiseDefaults &denoise();

} // namespace defaults

// Safe no-op on platforms without user qtable
void set_jpeg_quality_qtable(int encChn, int quality /*1..100*/, const char *cpu_hint);

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

// Running mode (HAL-level)
enum class RunningMode { Day = 0, Night = 1, Custom = 2 };
int set_running_mode(RunningMode mode);

// Statistics getters (platform-normalized)
// Returns 0 on success, -1 on unsupported/failure
int get_ev(int &out_ev);
int get_awb_weighted_gains(int &out_gr, int &out_gb);

// Sensor management functions (abstract IMPVI_MAIN parameter)
int add_sensor(IMPSensorInfo *sinfo);
int enable_sensor(IMPSensorInfo *sinfo);
int disable_sensor();
int del_sensor(IMPSensorInfo *sinfo);

} // namespace isp

// ============================================================================
// Platform Capabilities
// ============================================================================

// Convenience namespace for capability checks

// ============================================================================
// Video Encoder Stream HAL
// ============================================================================

namespace encoder {

struct PackSlices {
  uint8_t *first_ptr{nullptr};
  uint32_t first_len{0};
  uint8_t *second_ptr{nullptr};
  uint32_t second_len{0};
};

// Get pointer to NAL unit data in stream pack
uint8_t *get_pack_data_start(const IMPEncoderStream &stream, int pack_index);

// Get NAL unit data length
uint32_t get_pack_data_length(const IMPEncoderStream &stream, int pack_index);

// Split a pack into contiguous buffer slices (handles ring-buffer wrap on newer platforms)
PackSlices get_pack_slices(const IMPEncoderStream &stream, int pack_index);

// Get H.264 NAL type from stream pack
int get_h264_nal_type(const IMPEncoderPack &pack);

// Get H.265 NAL type from stream pack
int get_h265_nal_type(const IMPEncoderPack &pack);

// Encoder initialization helpers
void init_encoder_channel_attr(IMPEncoderCHNAttr &chnAttr, const char *format, int width, int height);
int get_encoder_rc_mode_smart();
int get_encoder_profile_high(const char *format);
int get_encoder_type(const char *format);
bool supports_jpeg_quality_table();

// Attribute compatibility helpers (fields missing on newer SDKs)
inline bool supports_attr_bufsize() {
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
  return false;
#else
  return true;
#endif
}

inline uint32_t get_attr_bufsize(const IMPEncoderCHNAttr &chnAttr) {
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
  (void)chnAttr;
  return 0;
#else
  return static_cast<uint32_t>(chnAttr.encAttr.bufSize);
#endif
}

inline void set_attr_bufsize(IMPEncoderCHNAttr &chnAttr, uint32_t value) {
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
  (void)chnAttr;
  (void)value;
#else
  chnAttr.encAttr.bufSize = static_cast<int>(value);
#endif
}

inline bool supports_attr_payload() {
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
  return false;
#else
  return true;
#endif
}

inline int get_attr_payload(const IMPEncoderCHNAttr &chnAttr) {
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
  (void)chnAttr;
  return -1;
#else
  return chnAttr.encAttr.enType;
#endif
}

} // namespace encoder

namespace audio {

void init_ai_channel_param(IMPAudioIChnParam &param);

} // namespace audio

namespace osd {

uint32_t black_cover_color();

} // namespace osd

} // namespace hal
