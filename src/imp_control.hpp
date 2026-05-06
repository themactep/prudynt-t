#ifndef IMP_CONTROL_HPP
#define IMP_CONTROL_HPP

#ifdef __cplusplus
extern "C" {
#endif

/*
 * IMP control bindings
 *
 * Direct integration of libimp_control capabilities into prudynt.
 * This layer provides C bindings to imaging, audio, and encoding controls
 * extracted from the libimp_control library, eliminating LD_PRELOAD dependency.
 */

/* ============================================================================
 * Video/ISP Controls
 * ============================================================================
 */

/* Basic image quality controls */
int imp_control_set_brightness(unsigned char val);
int imp_control_set_contrast(unsigned char val);
int imp_control_set_saturation(unsigned char val);
int imp_control_set_sharpness(unsigned char val);
int imp_control_set_hue(unsigned char val);

/* Denoising controls */
int imp_control_set_sinter(unsigned char val); /* Spatial denoise */
int imp_control_set_temper(unsigned char val); /* Temporal denoise */

/* Defect and dynamic range controls */
int imp_control_set_dpc(unsigned char val);   /* Dead pixel correction */
int imp_control_set_drc(unsigned char val);   /* Dynamic range compression */
int imp_control_set_defog(unsigned char val); /* Defog algorithm strength */

/* Exposure and gain controls */
int imp_control_set_ae_compensation(int val);
int imp_control_set_ae_it_max(int val); /* AE integration time max */
int imp_control_set_ae_min(int min_it, int min_again, int min_it_short,
                           int min_again_short);
int imp_control_set_max_again(unsigned char val); /* Max analog gain */
int imp_control_set_max_dgain(unsigned char val); /* Max digital gain */

/* Special effects and modes */
int imp_control_set_backlight_comp(unsigned char val);
int imp_control_set_highlight_depress(unsigned char val);
int imp_control_set_running_mode(int mode);
int imp_control_set_flicker_mode(int mode);       /* 0=off, 1=50Hz, 2=60Hz */
int imp_control_switch_bin(const char *bin_path); /* Switch IQ bin file */

/* White balance */
int imp_control_set_white_balance(int mode, unsigned short rgain,
                                  unsigned short bgain);

/* Sensor controls */
int imp_control_set_sensor_fps(int fps_num, int fps_den);

/* Flip/mirror controls */
int imp_control_set_flip(int mode); /* 0=normal, 1=mirror, 2=flip, 3=both */

/* ISP Query functions (read-only) */
int imp_control_get_total_gain(int *out_gain);
int imp_control_get_ae_luma(int *out_luma);
int imp_control_get_awb_color_temp(int *out_ct);
int imp_control_get_ev_attributes(char *buffer, int size);
int imp_control_get_ae_attributes(char *buffer, int size);

/* ============================================================================
 * Audio Input (AI) Controls
 * ============================================================================
 */

int imp_control_ai_set_hpf(int enable); /* High pass filter */
int imp_control_ai_set_agc(int gain_level, int max_gain);
int imp_control_ai_set_noise_suppression(int level); /* 0-3, 0=off */
int imp_control_ai_set_echo_cancellation(int enable);
int imp_control_ai_set_volume(int vol); /* -30 to 120 dB */
int imp_control_ai_set_gain(int gain);  /* 0-31 dB */
int imp_control_ai_set_alc(int level);  /* ALC 0-7 */

/* ============================================================================
 * Audio Output (AO) Controls
 * ============================================================================
 */

int imp_control_ao_set_hpf(int enable);
int imp_control_ao_set_volume(int vol); /* -30 to 120 dB */
int imp_control_ao_set_gain(int gain);  /* 0-31 dB */

/* ============================================================================
 * Encoding Controls
 * ============================================================================
 */

int imp_control_set_bitrate(int channel, int bitrate);
int imp_control_set_gop_length(int channel, int length);
int imp_control_set_rc_mode(int channel, int mode);
int imp_control_set_framerate(int channel, int fps_num, int fps_den);
int imp_control_set_qp(int channel, int qp); /* Quantization parameter */
int imp_control_set_qp_bounds(int channel, int min_qp, int max_qp);
int imp_control_set_qp_ip_delta(int channel, int delta);

/* ============================================================================
 * OSD Controls
 * ============================================================================
 */

int imp_control_osd_show_region(int handle, int show);
int imp_control_osd_set_region_pos(int handle, int x, int y);
int imp_control_osd_set_region_attr(int handle, const char *params);
int imp_control_osd_set_region_alpha(int handle, int alpha);
int imp_control_osd_set_region_cover(int handle, const char *params);
int imp_control_osd_get_region_attr(int handle, char *buffer, int size);
int imp_control_osd_get_group_attr(int handle, int group, char *buffer,
                                   int size);

/* ============================================================================
 * System Information (read-only)
 * ============================================================================
 */

/* Device identification functions */
const char *imp_control_get_device_id(void);
const char *imp_control_get_model_family(void);
const char *imp_control_get_sys_version(void);
const char *imp_control_get_imp_version(void);
const char *imp_control_get_cpu_info(void);
const char *imp_control_get_channel_encoding_type(int channel);

/* ============================================================================
 * Advanced/Specialized Controls
 * ============================================================================
 */

int imp_control_set_fisheye_status(int enable);
int imp_control_set_front_crop(int x, int y, int width, int height);
int imp_control_set_mask(const char *params);
int imp_control_set_auto_zoom(const char *params);
int imp_control_get_af_metrics(char *buffer, int size);

#ifdef __cplusplus
}
#endif

#endif /* IMP_CONTROL_HPP */
