#pragma once

/* ============================================================================
 * Platform Detection
 * Supports: T10, T20, T21, T23, T30, T31, T40, T41, C100
 * ============================================================================ */

#if defined(PLATFORM_T31) || defined(PLATFORM_T40) || defined(PLATFORM_T41) || defined(PLATFORM_C100)
    #define PLATFORM_NEW_SDK  /* T31+ use newer SDK API */
#elif defined(PLATFORM_T30) || defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T23) || defined(PLATFORM_T10)
    #define PLATFORM_OLD_SDK  /* T10-T30 use older SDK API */
#else
    #define PLATFORM_OLD_SDK  /* Default to older API */
#endif

#include <cstdint>

// Define stub types for platforms missing IMP ISP attribute records
// Must be before SDK headers are included
#if defined(PLATFORM_T20) || defined(PLATFORM_T10) || defined(PLATFORM_T21) || defined(PLATFORM_T30) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
struct IMPISPAEAttr {};
#endif

#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
struct IMPISPEVAttr {};
#endif

#include <imp/imp_audio.h>
#include <imp/imp_common.h>
#include <imp/imp_encoder.h>
#include <imp/imp_isp.h>
#include <imp/imp_osd.h>

// Compatibility shims for SDK variants where these prototypes are absent.
extern "C" {
int IMP_OSD_SetPoolSize(int size);
#if defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T23) || defined(PLATFORM_T30) || \
    defined(PLATFORM_T31) || defined(PLATFORM_C100)
int IMP_ISP_Tuning_GetAwbHist(IMPISPAWBHist *awb_hist);
#endif
}

struct _stream; // fwd decl

namespace hal {

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
  bool has_isp_switch_bin;

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

// Low-level ISP lifecycle helpers
int open();
int close();
int enable_tuning();
int disable_tuning();

// Basic image quality controls
int set_brightness(unsigned char val);
int get_brightness(unsigned char &out_val);
int set_contrast(unsigned char val);
int get_contrast(unsigned char &out_val);
int set_saturation(unsigned char val);
int get_saturation(unsigned char &out_val);
int set_sharpness(unsigned char val);
int get_sharpness(unsigned char &out_val);
int set_sinter_strength(unsigned char val);
int get_sinter_strength(unsigned char &out_val);
int set_temper_strength(unsigned char val);
int get_temper_strength(unsigned char &out_val);
int set_hue(unsigned char val);
int get_hue(unsigned char &out_val);

// Flip/mirror controls
int set_hflip(bool enable);
int set_vflip(bool enable);

// ISP modes
int set_running_mode(int mode);
int set_isp_bypass(bool enable);
int set_anti_flicker(int mode);
int get_anti_flicker(int &out_mode);

// Exposure controls
int set_ae_compensation(int val);
int get_ae_compensation(int &out_val);
int set_ae_it_max(unsigned int it_max);
int get_ae_it_max(unsigned int &out_it_max);
int set_ae_min(int min_it, int min_again, int min_it_short, int min_again_short);
int get_ae_min(int &out_min_it, int &out_min_again, int &out_min_it_short, int &out_min_again_short);

// Advanced image processing (may not be available on all platforms)
int set_dpc_strength(unsigned char val);
int get_dpc_strength(unsigned char &out_val);
int set_drc_strength(unsigned char val);
int get_drc_strength(unsigned char &out_val);
int set_defog_strength(uint8_t val);
int set_backlight_comp(unsigned char val);
int set_highlight_depress(unsigned char val);

// Gain controls
int set_max_again(unsigned char val);
int get_max_again(unsigned char &out_val);
int set_max_dgain(unsigned char val);
int get_max_dgain(unsigned char &out_val);

// Gamma curve (129 points)
int set_gamma(const uint16_t gamma[129]);
int get_gamma(uint16_t gamma[129]);

// White balance
int set_wb(int mode, unsigned short rgain, unsigned short bgain);

// AWB zone weights (15x15 grid)
int set_awb_weight(const unsigned char weight[15][15]);
int get_awb_weight(unsigned char weight[15][15]);
int get_awb_zone(unsigned char zone_r[225], unsigned char zone_g[225], unsigned char zone_b[225]);

// AE zone weights and ROI (15x15 grid)
int set_ae_weight(const unsigned char weight[15][15]);
int get_ae_weight(unsigned char weight[15][15]);
int set_ae_roi(const unsigned char roi[15][15]);
int get_ae_roi(unsigned char roi[15][15]);
int get_ae_zone(unsigned int zone[15][15]);

// AE histogram (5-bin normalized or 256-bin origin)
int set_ae_hist(const unsigned char thresholds[4], unsigned char stat_nodeh, unsigned char stat_nodev);
int get_ae_hist(unsigned char thresholds[4], unsigned short bins[5], unsigned char &stat_nodeh, unsigned char &stat_nodev);
int get_ae_hist_origin(unsigned int bins[256]);

// Sensor timing
int set_sensor_fps(int fps_num, int fps_den);
int get_sensor_fps(int &fps_num, int &fps_den);

// Running mode (HAL-level)
enum class RunningMode { Day = 0, Night = 1, Custom = 2 };
int set_running_mode(RunningMode mode);
int get_running_mode(int &out_mode);

// IQ bin file switching (available on T23/T31/T40/T41)
int switch_bin(const char *bin_path);

// Statistics getters (platform-normalized)
// Returns 0 on success, -1 on unsupported/failure
int get_ev(int &out_ev);
int get_awb_weighted_gains(int &out_gr, int &out_gb);
int get_total_gain(int &out_gain);
int get_ae_luma(int &out_luma);
int get_awb_color_temp(int &out_ct);
int get_ev_attr(IMPISPEVAttr &out_attr);
int get_ae_attr(IMPISPAEAttr &out_attr);

#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
int set_auto_zoom(const IMPISPAutoZoom &zoom);
#endif

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

// Encoder channel tuning
int set_bitrate(int channel, int bitrate);
int set_gop_length(int channel, int length);
int set_rc_mode(int channel, int mode);
int set_framerate(int channel, int fps_num, int fps_den);
int set_qp(int channel, int qp);
int set_qp_bounds(int channel, int min_qp, int max_qp);
int set_qp_ip_delta(int channel, int delta);

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

// Audio input controls
int set_ai_hpf(int enable);
int set_ai_agc(int gain_level, int max_gain);
int set_ai_noise_suppression(int level);
int set_ai_echo_cancellation(int enable);
int set_ai_volume(int vol);
int set_ai_gain(int gain);
int set_ai_alc(int level);

// Audio output controls
int set_ao_hpf(int enable);
int set_ao_volume(int vol);
int set_ao_gain(int gain);

} // namespace audio

namespace osd {

uint32_t black_cover_color();
int show_region(int handle, int show);
int set_region_pos(int handle, int x, int y);
int set_region_alpha(int handle, int alpha);
int get_region_attr(int handle, IMPOSDRgnAttr &out_attr);
int get_group_attr(int handle, int group, IMPOSDGrpRgnAttr &out_attr);
int set_region_attr(int handle, const char *params); // placeholder for future parsing
int set_region_cover(int handle, const char *params); // placeholder for future parsing

} // namespace osd

} // namespace hal
