#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "imp_hal.hpp"
#include "imp_control.hpp"

/* Include IMP SDK headers */
#include <imp/imp_isp.h>
#include <imp/imp_encoder.h>
#include <imp/imp_audio.h>
#include <imp/imp_osd.h>
#include <imp/imp_system.h>

extern "C" {

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

/* ============================================================================
 * Video/ISP Controls - Implementation
 * ============================================================================ */

int imp_control_set_brightness(unsigned char val) {
    return hal::isp::set_brightness(val);
}

int imp_control_set_contrast(unsigned char val) {
    return hal::isp::set_contrast(val);
}

int imp_control_set_saturation(unsigned char val) {
    return hal::isp::set_saturation(val);
}

int imp_control_set_sharpness(unsigned char val) {
    return hal::isp::set_sharpness(val);
}

int imp_control_set_hue(unsigned char val) {
    return hal::isp::set_hue(val);
}

int imp_control_set_sinter(unsigned char val) {
    return hal::isp::set_sinter_strength(val);
}

int imp_control_set_temper(unsigned char val) {
    return hal::isp::set_temper_strength(val);
}

int imp_control_set_dpc(unsigned char val) {
    return hal::isp::set_dpc_strength(val);
}

int imp_control_set_drc(unsigned char val) {
    return hal::isp::set_drc_strength(val);
}

int imp_control_set_defog(unsigned char val) {
    return hal::isp::set_defog_strength(val);
}

int imp_control_set_ae_compensation(int val) {
    return hal::isp::set_ae_compensation(val);
}

int imp_control_set_ae_it_max(int val) {
    return hal::isp::set_ae_it_max(static_cast<unsigned int>(val));
}

int imp_control_set_ae_min(int min_it, int min_again, int min_it_short, int min_again_short) {
    return hal::isp::set_ae_min(min_it, min_again, min_it_short, min_again_short);
}

int imp_control_set_max_again(unsigned char val) {
    return hal::isp::set_max_again(val);
}

int imp_control_set_max_dgain(unsigned char val) {
    return hal::isp::set_max_dgain(val);
}

int imp_control_set_backlight_comp(unsigned char val) {
    return hal::isp::set_backlight_comp(val);
}

int imp_control_set_highlight_depress(unsigned char val) {
    return hal::isp::set_highlight_depress(val);
}

int imp_control_set_running_mode(int mode) {
    return hal::isp::set_running_mode(mode);
}

int imp_control_set_flicker_mode(int mode) {
    return hal::isp::set_anti_flicker(mode);
}

int imp_control_set_white_balance(int mode, unsigned short rgain, unsigned short bgain) {
    return hal::isp::set_wb(mode == 0 ? ISP_CORE_WB_MODE_AUTO : ISP_CORE_WB_MODE_MANUAL, rgain, bgain);
}

int imp_control_set_sensor_fps(int fps_num, int fps_den) {
    return hal::isp::set_sensor_fps(fps_num, fps_den);
}

int imp_control_set_flip(int mode) {
    int ret = hal::isp::set_hflip((mode & 1) != 0);
    if (ret < 0) return ret;
    return hal::isp::set_vflip((mode & 2) != 0);
}

int imp_control_get_total_gain(int *out_gain) {
    if (!out_gain) return -1;
    return hal::isp::get_total_gain(*out_gain);
}

int imp_control_get_ae_luma(int *out_luma) {
    if (!out_luma) return -1;
    return hal::isp::get_ae_luma(*out_luma);
}

int imp_control_get_awb_color_temp(int *out_ct) {
    if (!out_ct) return -1;
    return hal::isp::get_awb_color_temp(*out_ct);
}

int imp_control_get_ev_attributes(char *buffer, int size) {
    if (!buffer || size < (int)sizeof(IMPISPEVAttr)) return -1;
    IMPISPEVAttr attr{};
    int ret = hal::isp::get_ev_attr(attr);
    if (ret == 0) {
        memcpy(buffer, &attr, sizeof(attr));
    }
    return ret;
}

int imp_control_get_ae_attributes(char *buffer, int size) {
    if (!buffer || size < (int)sizeof(IMPISPAEAttr)) return -1;
    IMPISPAEAttr attr{};
    int ret = hal::isp::get_ae_attr(attr);
    if (ret == 0) {
        memcpy(buffer, &attr, sizeof(attr));
    }
    return ret;
}

/* ============================================================================
 * Audio Input (AI) Controls - Implementation
 * ============================================================================ */

int imp_control_ai_set_hpf(int enable) {
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
    /* HPF is typically set via channel params */
    return IMP_AI_SetHpfCoFrequency(enable ? 20 : 0);
#else
    (void)enable;
    return -1;  /* Not supported on this platform */
#endif
}

int imp_control_ai_set_agc(int gain_level, int max_gain) {
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
    /* Use the AGC mode function directly if available */
    (void)gain_level; /* gain_level parameter usage depends on implementation */
    return IMP_AI_SetAgcMode(max_gain);
#else
    (void)gain_level; (void)max_gain;
    return -1;  /* Not supported on this platform */
#endif
}

int imp_control_ai_set_noise_suppression(int level) {
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
    /* Noise suppression level - implementation depends on platform */
    (void)level;
    return -1;  /* Requires webrtc profile or channel params - complex setup */
#else
    (void)level;
    return -1;  /* Not supported on this platform */
#endif
}

int imp_control_ai_set_echo_cancellation(int enable) {
    /* AEC setup is complex and platform-specific */
    (void)enable;
    return -1;  /* Requires platform-specific AEC setup */
}

int imp_control_ai_set_volume(int vol) {
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
    int audioDevId = 0;
    int aiChn = 0;
    return IMP_AI_SetVol(audioDevId, aiChn, vol);
#else
    (void)vol;
    return -1;  /* Not supported on this platform */
#endif
}

int imp_control_ai_set_gain(int gain) {
    /* Gain parameter not directly in IMPAudioIChnParam for T31 */
    (void)gain;
    return -1;  /* Requires platform-specific implementation */
}

int imp_control_ai_set_alc(int level) {
    /* ALC parameter not directly in IMPAudioIChnParam for T31 */
    (void)level;
    return -1;  /* Requires platform-specific implementation */
}

/* ============================================================================
 * Audio Output (AO) Controls - Implementation
 * ============================================================================ */

int imp_control_ao_set_hpf(int enable) {
    return hal::audio::set_ao_hpf(enable);
}

int imp_control_ao_set_volume(int vol) {
    return hal::audio::set_ao_volume(vol);
}

int imp_control_ao_set_gain(int gain) {
    return hal::audio::set_ao_gain(gain);
}

/* ============================================================================
 * Encoding Controls - Implementation
 * ============================================================================ */

int imp_control_set_bitrate(int channel, int bitrate) {
    return hal::encoder::set_bitrate(channel, bitrate);
}

int imp_control_set_gop_length(int channel, int length) {
    return hal::encoder::set_gop_length(channel, length);
}

int imp_control_set_rc_mode(int channel, int mode) {
    return hal::encoder::set_rc_mode(channel, mode);
}

int imp_control_set_framerate(int channel, int fps_num, int fps_den) {
    return hal::encoder::set_framerate(channel, fps_num, fps_den);
}

int imp_control_set_qp(int channel, int qp) {
    return hal::encoder::set_qp(channel, qp);
}

int imp_control_set_qp_bounds(int channel, int min_qp, int max_qp) {
    return hal::encoder::set_qp_bounds(channel, min_qp, max_qp);
}

int imp_control_set_qp_ip_delta(int channel, int delta) {
    return hal::encoder::set_qp_ip_delta(channel, delta);
}

/* ============================================================================
 * OSD Controls - Implementation
 * ============================================================================ */

int imp_control_osd_show_region(int handle, int show) {
    return hal::osd::show_region(handle, show);
}

int imp_control_osd_set_region_pos(int handle, int x, int y) {
    return hal::osd::set_region_pos(handle, x, y);
}

int imp_control_osd_set_region_attr(int handle, const char *params) {
    return hal::osd::set_region_attr(handle, params);
}

int imp_control_osd_set_region_alpha(int handle, int alpha) {
    return hal::osd::set_region_alpha(handle, alpha);
}

int imp_control_osd_set_region_cover(int handle, const char *params) {
    return hal::osd::set_region_cover(handle, params);
}

int imp_control_osd_get_region_attr(int handle, char *buffer, int size) {
    if (handle < 0 || !buffer || size < (int)sizeof(IMPOSDRgnAttr)) return -1;
    IMPOSDRgnAttr attr{};
    int ret = hal::osd::get_region_attr(handle, attr);
    if (ret == 0) {
        memcpy(buffer, &attr, sizeof(attr));
    }
    return ret;
}

int imp_control_osd_get_group_attr(int handle, int group, char *buffer, int size) {
    if (handle < 0 || group < 0 || !buffer || size < (int)sizeof(IMPOSDGrpRgnAttr)) return -1;
    IMPOSDGrpRgnAttr attr{};
    int ret = hal::osd::get_group_attr(handle, group, attr);
    if (ret == 0) {
        memcpy(buffer, &attr, sizeof(attr));
    }
    return ret;
}

/* ============================================================================
 * System Information - Implementation
 * ============================================================================ */

const char *imp_control_get_device_id(void) {
    static char device_id[64] = {0};
    if (device_id[0] == 0) {
        #if defined(PLATFORM_T10)
        snprintf(device_id, sizeof(device_id), "T10");
        #elif defined(PLATFORM_T20)
        snprintf(device_id, sizeof(device_id), "T20");
        #elif defined(PLATFORM_T21)
        snprintf(device_id, sizeof(device_id), "T21");
        #elif defined(PLATFORM_T23)
        snprintf(device_id, sizeof(device_id), "T23");
        #elif defined(PLATFORM_T30)
        snprintf(device_id, sizeof(device_id), "T30");
        #elif defined(PLATFORM_T31)
        snprintf(device_id, sizeof(device_id), "T31");
        #elif defined(PLATFORM_T40)
        snprintf(device_id, sizeof(device_id), "T40");
        #elif defined(PLATFORM_T41)
        snprintf(device_id, sizeof(device_id), "T41");
        #elif defined(PLATFORM_C100)
        snprintf(device_id, sizeof(device_id), "C100");
        #else
        snprintf(device_id, sizeof(device_id), "unknown");
        #endif
    }
    return device_id;
}

const char *imp_control_get_model_family(void) {
    #if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
    return "NEW_SDK";
    #else
    return "OLD_SDK";
    #endif
}

const char *imp_control_get_sys_version(void) {
    static char version[128] = {0};
    if (version[0] == 0) {
        IMPVersion impVersion;
        if (IMP_System_GetVersion(&impVersion) == 0) {
            snprintf(version, sizeof(version), "%s", impVersion.aVersion);
        } else {
            snprintf(version, sizeof(version), "unknown");
        }
    }
    return version;
}

const char *imp_control_get_imp_version(void) {
    return imp_control_get_sys_version();  /* Same as system version */
}

const char *imp_control_get_cpu_info(void) {
    #if defined(PLATFORM_T10)
    return "XBurst T10";
    #elif defined(PLATFORM_T20)
    return "XBurst T20";
    #elif defined(PLATFORM_T21)
    return "XBurst T21";
    #elif defined(PLATFORM_T23)
    return "XBurst T23";
    #elif defined(PLATFORM_T30)
    return "XBurst T30";
    #elif defined(PLATFORM_T31)
    return "XBurst2 T31";
    #elif defined(PLATFORM_T40)
    return "XBurst2 T40";
    #elif defined(PLATFORM_T41)
    return "XBurst2 T41";
    #elif defined(PLATFORM_C100)
    return "XBurst2 C100";
    #else
    return "unknown";
    #endif
}

const char *imp_control_get_channel_encoding_type(int channel) {
    #if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
    IMPEncoderChnAttr chnAttr;
    #else
    IMPEncoderCHNAttr chnAttr;
    #endif

    if (IMP_Encoder_GetChnAttr(channel, &chnAttr) != 0) {
        return "unknown";
    }

    /* For T31+, determine type from profile */
    #if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
    /* Check profile to determine codec type */
    if (chnAttr.encAttr.eProfile >= IMP_ENC_PROFILE_JPEG) {
        return "JPEG";
    } else if (chnAttr.encAttr.eProfile >= IMP_ENC_PROFILE_HEVC_MAIN) {
        return "H265";
    } else {
        return "H264";
    }
    #else
    /* Older platforms have enType field */
    switch (chnAttr.encAttr.enType) {
        case PT_H264: return "H264";
        case PT_JPEG: return "JPEG";
        default: return "unknown";
    }
    #endif
}

/* ============================================================================
 * Advanced/Specialized Controls - Implementation
 * ============================================================================ */

int imp_control_set_fisheye_status(int enable) {
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
    /* Fisheye control - channel 0 assumed */
    return IMP_Encoder_SetFisheyeEnableStatus(0, enable);
#else
    (void)enable;
    return -1;  /* Not supported on this platform */
#endif
}

int imp_control_set_front_crop(int x, int y, int width, int height) {
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
    IMPISPAutoZoom zoom;
    zoom.chan = 0;
    zoom.scaler_enable = 0;
    zoom.scaler_outwidth = 0;
    zoom.scaler_outheight = 0;
    zoom.crop_enable = 1;
    zoom.crop_left = x;
    zoom.crop_top = y;
    zoom.crop_width = width;
    zoom.crop_height = height;
    return IMP_ISP_Tuning_SetAutoZoom(&zoom);
#else
    (void)x; (void)y; (void)width; (void)height;
    return -1;  /* Not supported on this platform */
#endif
}

int imp_control_set_mask(const char *params) {
    /* Masking requires complex parameter parsing */
    (void)params;
    return -1;  /* Requires custom implementation */
}

int imp_control_set_auto_zoom(const char *params) {
    /* Auto zoom requires parameter parsing */
    (void)params;
    return -1;  /* Requires custom implementation */
}

int imp_control_get_af_metrics(char *buffer, int size) {
    /* AF Metrics not available in standard T31 API */
    (void)buffer; (void)size;
    return -1;  /* Not supported - AF metrics function not available */
}

} // extern "C"

/* ============================================================================
 * Platform-Specific Implementation Notes
 * ============================================================================
 *
 * This wrapper provides a unified API across all Ingenic platforms.
 *
 * Platform Groups:
 *
 * OLD SDK (T10, T20, T21, T23, T30):
 *   - Uses older IMP library API
 *   - Function names may vary slightly
 *
 * NEW SDK (T31, T40, T41, C100):
 *   - Uses newer, restructured IMP library API
 *   - More consistent naming conventions
 *   - Additional features (DPC, DRC, Defog, etc.)
 *
 * Functions are implemented with platform-specific conditional compilation
 * using PLATFORM_* defines set by the build system.
 *
 * Unsupported functions return -1 with (void) casts to suppress warnings.
 *
 * ============================================================================ */
