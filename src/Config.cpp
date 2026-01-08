#include "Config.hpp"
#include "Logger.hpp"
#include "imp_hal.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <json_config.h>
#include <vector>

#if defined(WEBSOCKET_ENABLED)
#define WEBSOCKET_TOKEN_LENGTH 32
#endif

#define MODULE "CONFIG"

namespace fs = std::filesystem;

namespace {

void log_dimension_adjustment(const char *context, const char *field, int old_value, int new_value) {
  if (old_value == new_value) {
    return;
  }
  LOG_INFO(context << ": " << field << " adjusted from " << old_value << " to " << new_value);
}

void apply_stream_sensor_defaults(CFG &config) {
  const int sensor_w = config.sensor.width;
  const int sensor_h = config.sensor.height;
  if (sensor_w <= 0 || sensor_h <= 0) {
    return;
  }

  auto adjust_dim = [&](const char *stream_name, const char *field, int sensor_max, int &value) {
    if (sensor_max <= 0) {
      return;
    }
    int original = value;
    if (value <= 0) {
      value = sensor_max;
    } else if (value > sensor_max) {
      value = sensor_max;
    }
    log_dimension_adjustment(stream_name, field, original, value);
  };

  auto adjust_stream = [&](const char *stream_name, _stream &stream) {
    adjust_dim(stream_name, "width", sensor_w, stream.width);
    adjust_dim(stream_name, "height", sensor_h, stream.height);
  };

  adjust_stream("stream0", config.stream0);
  adjust_stream("stream1", config.stream1);

  if (config.stream2.jpeg_idle_fps <= 0) {
    const int sensor_min_fps = config.sensor.min_fps;
    int replacement = sensor_min_fps > 0 ? sensor_min_fps : 1;
    replacement = std::clamp(replacement, 1, 30);
    log_dimension_adjustment("stream2", "jpeg_idle_fps", config.stream2.jpeg_idle_fps, replacement);
    config.stream2.jpeg_idle_fps = replacement;
  }
}

void apply_motion_sensor_defaults(CFG &config) {
  const int sensor_w = config.sensor.width;
  const int sensor_h = config.sensor.height;
  if (sensor_w <= 0 || sensor_h <= 0) {
    return;
  }

  auto adjust_frame_dim = [&](const char *field, int sensor_max, int &value) {
    int original = value;
    if (value == IVS_AUTO_VALUE || value <= 0) {
      value = sensor_max;
    } else if (value > sensor_max) {
      value = sensor_max;
    }
    log_dimension_adjustment("motion", field, original, value);
  };

  adjust_frame_dim("frame_width", sensor_w, config.motion.frame_width);
  adjust_frame_dim("frame_height", sensor_h, config.motion.frame_height);

  auto clamp_roi_coord = [&](const char *field, int sensor_max, int &value, bool is_end_coord) {
    if (sensor_max <= 0) {
      int original = value;
      value = 0;
      log_dimension_adjustment("motion", field, original, value);
      return;
    }
    int max_coord = std::max(sensor_max - 1, 0);
    int original = value;

    if (is_end_coord) {
      if (value == IVS_AUTO_VALUE || value <= 0) {
        value = max_coord;
      } else {
        value = std::clamp(value, 0, max_coord);
      }
    } else {
      if (value == IVS_AUTO_VALUE) {
        value = 0;
      } else {
        value = std::clamp(value, 0, max_coord);
      }
    }

    log_dimension_adjustment("motion", field, original, value);
  };

  clamp_roi_coord("roi_0_x", sensor_w, config.motion.roi_0_x, false);
  clamp_roi_coord("roi_0_y", sensor_h, config.motion.roi_0_y, false);
  clamp_roi_coord("roi_1_x", sensor_w, config.motion.roi_1_x, true);
  clamp_roi_coord("roi_1_y", sensor_h, config.motion.roi_1_y, true);

  auto enforce_roi_span = [&](const char *field, int sensor_max, int start, int &end) {
    if (sensor_max <= 0) {
      return;
    }
    if (end > start) {
      return;
    }
    int max_coord = std::max(sensor_max - 1, 0);
    int original = end;
    int candidate = std::min(max_coord, start + 1);
    if (candidate <= start) {
      candidate = start;
    }
    end = candidate;
    log_dimension_adjustment("motion", field, original, end);
  };

  enforce_roi_span("roi_1_x", sensor_w, config.motion.roi_0_x, config.motion.roi_1_x);
  enforce_roi_span("roi_1_y", sensor_h, config.motion.roi_0_y, config.motion.roi_1_y);
}

} // namespace

// Forward declaration
std::string jsonValueToString(JsonValue *value);

bool validateIntGe0(const int &v) {
  return v >= 0;
}

bool validateInt1(const int &v) {
  return v >= 0 && v <= 2;
}

bool validateInt2(const int &v) {
  return v >= 0 && v <= 2;
}

bool validateRotation(const int &v) {
  return v == 0 || v == 90 || v == 270;
}

bool validateInt32(const int &v) {
  return v >= 0 && v <= 32;
}

bool validateInt50_150(const int &v) {
  return v >= 50 && v <= 150;
}

bool validateInt120(const int &v) {
  return (v >= 0 && v <= 120) || v == IMP_AUTO_VALUE;
}

bool validateInt255(const int &v) {
  return v >= 0 && v <= 255;
}

bool validateInt360(const int &v) {
  return v >= 0 && v <= 360;
}

bool validateInt15360(const int &v) {
  return v >= -15360 && v <= 15360;
}

bool validateInt65535(const int &v) {
  return v >= 0 && v <= 65535;
}

bool validateCharDummy(const char *v) {
  return true;
}

bool validateCharNotEmpty(const char *v) {
  return std::strlen(v) > 0;
}

bool validateLogLevelString(const char *v) {
  if (!v || v[0] == '\0') {
    return true;
  }
  static const std::set<std::string> allowed = {"EMERGENCY", "ALERT",   "CRITICAL", "ERROR",
                                                "WARN",      "NOTICE",  "INFO",     "DEBUG",
                                                "TRACE"};
  return allowed.count(std::string(v)) == 1;
}

bool validateBool(const bool &v) {
  return true;
}

bool validateUint(const unsigned int &v) {
  return true;
}

// Utility function to validate hexadecimal color format (#RRGGBBAA)
bool isValidHexColor(const char *str) {
  if (!str || strlen(str) != 9) {
    return false;
  }

  if (str[0] != '#') {
    return false;
  }

  for (int i = 1; i < 9; i++) {
    char c = str[i];
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }

  return true;
}

// Utility function to convert RGBA hexadecimal color string to unsigned int
unsigned int hexColorToUint(const char *str) {
  if (!isValidHexColor(str)) {
    return 0;
  }

  // Parse RGBA format: #RRGGBBAA
  // Extract each component separately to handle RGBA format correctly
  char rStr[3] = {str[1], str[2], '\0'}; // RR
  char gStr[3] = {str[3], str[4], '\0'}; // GG
  char bStr[3] = {str[5], str[6], '\0'}; // BB
  char aStr[3] = {str[7], str[8], '\0'}; // AA

  unsigned int r = strtoul(rStr, nullptr, 16);
  unsigned int g = strtoul(gStr, nullptr, 16);
  unsigned int b = strtoul(bStr, nullptr, 16);
  unsigned int a = strtoul(aStr, nullptr, 16);

  // Pack into ARGB format for internal use (A in bits 24-31, R in 16-23, G in 8-15, B in 0-7)
  // This matches the bit extraction logic used in OSD::drawText()
  return (a << 24) | (r << 16) | (g << 8) | b;
}

// Validation function for OSD color fields that accepts both integer and hex
// string formats
bool validateOSDColor(const unsigned int &v) {
  // For unsigned int values, any value is valid (same as validateUint)
  return true;
}

// Validation function for OSD color fields when parsing from JSON string
bool validateOSDColorString(const char *str) {
  if (!str) {
    return false;
  }

  // Check if it's a valid hexadecimal color format
  return isValidHexColor(str);
}

bool validateSampleRate(const int &v) {
  std::set<int> allowed_rates = {8000, 16000, 24000, 44100, 48000};
  return allowed_rates.count(v) == 1;
}

// Forward declarations for nested JSON helpers used in templates below
JsonValue *getNestedValue(JsonValue *root, const std::string &path);
bool setNestedValue(JsonValue *root, const std::string &path, const std::string &value);

std::vector<ConfigItem<bool>> CFG::getBoolItems() {
  return {
      {"audio.force_stereo", audio.force_stereo, false, validateBool},
      {"audio.mic_enabled", audio.input_enabled, true, validateBool},
      {"audio.mic_is_digital", audio.mic_is_digital, false, validateBool},
      {"audio.spk_enabled", audio.output_enabled, true, validateBool},
      {"audio.tap_enabled", audio.tap_enabled, false, validateBool},
#if defined(LIB_AUDIO_PROCESSING)
      {"audio.mic_high_pass_filter", audio.input_high_pass_filter, false, validateBool},
      {"audio.mic_agc_enabled", audio.input_agc_enabled, false, validateBool},
#endif
      {"daynight.enabled", daynight.enabled, true, validateBool},
      {"http.enabled", http.enabled, true, validateBool},
      {"http.api_enabled", http.api_enabled, true, validateBool},
      {"http.mjpeg_enabled", http.mjpeg_enabled, true, validateBool},
      {"image.isp_bypass", image.isp_bypass, true, validateBool},
      {"image.vflip", image.vflip, false, validateBool},
      {"image.hflip", image.hflip, false, validateBool},
      {"motion.enabled", motion.enabled, false, validateBool},
      {"recorder.enabled", recorder.enabled, false, validateBool},
      {"rtsp.auth_required", rtsp.auth_required, true, validateBool},
      {"rtsp.audio_only_enabled", rtsp.audio_only_enabled, true, validateBool},
      {"stream0.audio_enabled", stream0.audio_enabled, true, validateBool},
      {"stream0.video_enabled", stream0.video_enabled, true, validateBool},
      {"stream0.enabled", stream0.enabled, true, validateBool},
      {"stream0.allow_shared", stream0.allow_shared, true, validateBool},
      {"stream0.osd.brightness.enabled", stream0.osd.brightness_enabled, false, validateBool},
      {"stream0.osd.enabled", stream0.osd.enabled, true, validateBool},
      {"stream0.osd.logo.enabled", stream0.osd.logo_enabled, true, validateBool},
      {"stream0.osd.privacy.enabled", stream0.osd.privacy.enabled, true, validateBool},
      {"stream0.osd.time.enabled", stream0.osd.time_enabled, true, validateBool},
      {"stream0.osd.uptime.enabled", stream0.osd.uptime_enabled, true, validateBool},
      {"stream0.osd.usertext.enabled", stream0.osd.usertext_enabled, true, validateBool},
      {"stream1.audio_enabled", stream1.audio_enabled, true, validateBool},
      {"stream1.video_enabled", stream1.video_enabled, true, validateBool},
      {"stream1.enabled", stream1.enabled, true, validateBool},
      {"stream1.allow_shared", stream1.allow_shared, true, validateBool},
      {"stream1.osd.brightness.enabled", stream1.osd.brightness_enabled, false, validateBool},
      {"stream1.osd.enabled", stream1.osd.enabled, true, validateBool},
      {"stream1.osd.logo.enabled", stream1.osd.logo_enabled, true, validateBool},
      {"stream1.osd.privacy.enabled", stream1.osd.privacy.enabled, true, validateBool},
      {"stream1.osd.time.enabled", stream1.osd.time_enabled, true, validateBool},
      {"stream1.osd.uptime.enabled", stream1.osd.uptime_enabled, true, validateBool},
      {"stream1.osd.usertext.enabled", stream1.osd.usertext_enabled, true, validateBool},
      {"stream2.enabled", stream2.enabled, true, validateBool},
      {"stream3.enabled", stream3.enabled, false, validateBool},
#if defined(WEBSOCKET_ENABLED)
      {"websocket.enabled", websocket.enabled, true, validateBool},
      {"websocket.ws_secured", websocket.ws_secured, true, validateBool},
      {"websocket.http_secured", websocket.http_secured, true, validateBool},
#endif
  };
};

std::vector<ConfigItem<const char *>> CFG::getCharItems() {
  const auto &encDefaults = hal::defaults::encoder();
  return {
      {"audio.mic_format", audio.input_format, "OPUS", [](const char *v) { std::set<std::string> a = {"OPUS", "AAC", "PCM", "G711A", "G711U", "G726"}; return a.count(std::string(v)) == 1; }},
      {"audio.tap_path", audio.tap_path, "/run/prudynt/audio_in.pcm", validateCharNotEmpty},
      {"daynight.script_path", daynight.script_path, "/sbin/daynight", validateCharNotEmpty},
      {"daynight.loglevel", daynight.loglevel, "", validateLogLevelString},
      {"general.loglevel", general.loglevel, "INFO", [](const char *v) { std::set<std::string> a = {"EMERGENCY", "ALERT", "CRITICAL", "ERROR", "WARN", "NOTICE", "INFO", "DEBUG", "TRACE"}; return a.count(std::string(v)) == 1; }},
      {"motion.script_path", motion.script_path, "/usr/sbin/motion", validateCharNotEmpty},
      {"recorder.device_path", recorder.device_path, "%hostname", validateCharDummy},
      {"recorder.filename", recorder.filename, "%Y/%m/%d/%H-%M-%S", validateCharNotEmpty},
      {"recorder.mount", recorder.mount, "/mnt/mmc", validateCharNotEmpty},
      {"rtsp.name", rtsp.name, "thingino prudynt", validateCharNotEmpty},
      {"rtsp.password", rtsp.password, "thingino", validateCharNotEmpty},
      {"rtsp.audio_only_endpoint", rtsp.audio_only_endpoint, "mic", validateCharNotEmpty},
      {"rtsp.audio_only_info", rtsp.audio_only_info, "audio from the microphone", validateCharNotEmpty},
      {"rtsp.username", rtsp.username, "thingino", validateCharNotEmpty},
      {"sensor.model", sensor.model, "unknown", validateCharNotEmpty, false, "/proc/jz/sensor/name"},
      {"sensor.chip_id", sensor.chip_id, "unknown", validateCharNotEmpty, false, "/proc/jz/sensor/chip_id"},
      {"sensor.version", sensor.version, "unknown", validateCharNotEmpty, false, "/proc/jz/sensor/version"},
      {"stream0.format", stream0.format, "H264", [](const char *v) { return strcmp(v, "H264") == 0 || strcmp(v, "H265") == 0; }},
      {"stream0.osd.font_path", stream0.osd.font_path, "/usr/share/fonts/default.ttf", validateCharNotEmpty},
      {"stream0.osd.logo.path", stream0.osd.logo_path, "/usr/share/images/thingino_logo_210x64.bgra", validateCharNotEmpty},
      {"stream0.osd.time.format", stream0.osd.time_format, "%F %T", validateCharNotEmpty},
      {"stream0.osd.uptime.format", stream0.osd.uptime_format, "Up: %02lud %02lu:%02lu", validateCharNotEmpty},
      {"stream0.osd.usertext.format", stream0.osd.usertext_format, "%hostname", validateCharNotEmpty},
      {"stream0.osd.time.position", stream0.osd.time_position, "10,10", validateCharNotEmpty},
      {"stream0.osd.uptime.position", stream0.osd.uptime_position, "1600,5", validateCharNotEmpty},
      {"stream0.osd.usertext.position", stream0.osd.usertext_position, "900,5", validateCharNotEmpty},
      {"stream0.osd.logo.position", stream0.osd.logo_position, "1800,1030", validateCharNotEmpty},
      {"stream0.osd.brightness.format", stream0.osd.brightness_format, "Gain: %b%% Avg: %a%% %m", validateCharNotEmpty},
      {"stream0.osd.brightness.position", stream0.osd.brightness_position, "10,70", validateCharNotEmpty},
      {"stream0.osd.privacy.image_path", stream0.osd.privacy.image_path, "", validateCharDummy},
      {"stream0.osd.privacy.position", stream0.osd.privacy.position, "0,-120", validateCharNotEmpty},
      {"stream0.osd.privacy.text", stream0.osd.privacy.text, "PRIVACY ENABLED", validateCharNotEmpty},
      {"stream0.mode", stream0.mode, encDefaults.stream0_mode, [](const char *v) { std::set<std::string> a = {"CBR", "VBR", "SMART", "FIXQP", "CAPPED_VBR", "CAPPED_QUALITY"}; return a.count(std::string(v)) == 1; }},
      {"stream0.rtsp_endpoint", stream0.rtsp_endpoint, "ch0", validateCharNotEmpty},
      {"stream0.rtsp_info", stream0.rtsp_info, "stream0", validateCharNotEmpty},
      {"stream1.format", stream1.format, "H264", [](const char *v) { return strcmp(v, "H264") == 0 || strcmp(v, "H265") == 0; }},
      {"stream1.osd.font_path", stream1.osd.font_path, "/usr/share/fonts/default.ttf", validateCharNotEmpty},
      {"stream1.osd.logo.path", stream1.osd.logo_path, "/usr/share/images/thingino_logo_100x30.bgra", validateCharNotEmpty},
      {"stream1.osd.time.format", stream1.osd.time_format, "%F %T", validateCharNotEmpty},
      {"stream1.osd.uptime.format", stream1.osd.uptime_format, "Up: %02lud %02lu:%02lu", validateCharNotEmpty},
      {"stream1.osd.usertext.format", stream1.osd.usertext_format, "%hostname", validateCharNotEmpty},
      {"stream1.osd.time.position", stream1.osd.time_position, "10,10", validateCharNotEmpty},
      {"stream1.osd.uptime.position", stream1.osd.uptime_position, "500,5", validateCharNotEmpty},
      {"stream1.osd.usertext.position", stream1.osd.usertext_position, "250,5", validateCharNotEmpty},
      {"stream1.osd.logo.position", stream1.osd.logo_position, "530,320", validateCharNotEmpty},
      {"stream1.osd.brightness.format", stream1.osd.brightness_format, "Gain: %b%% Avg: %a%% %m", validateCharNotEmpty},
      {"stream1.osd.brightness.position", stream1.osd.brightness_position, "10,70", validateCharNotEmpty},
      {"stream1.osd.privacy.image_path", stream1.osd.privacy.image_path, "", validateCharDummy},
      {"stream1.osd.privacy.position", stream1.osd.privacy.position, "0,-120", validateCharNotEmpty},
      {"stream1.osd.privacy.text", stream1.osd.privacy.text, "PRIVACY ENABLED", validateCharNotEmpty},
      {"stream1.mode", stream1.mode, encDefaults.stream1_mode, [](const char *v) { std::set<std::string> a = {"CBR", "VBR", "SMART", "FIXQP", "CAPPED_VBR", "CAPPED_QUALITY"}; return a.count(std::string(v)) == 1; }},
      {"stream1.rtsp_endpoint", stream1.rtsp_endpoint, "ch1", validateCharNotEmpty},
      {"stream1.rtsp_info", stream1.rtsp_info, "stream1", validateCharNotEmpty},
      {"stream2.jpeg_path", stream2.jpeg_path, "/tmp/snapshot.jpg", validateCharNotEmpty},
      {"stream3.jpeg_path", stream3.jpeg_path, "/tmp/snapshot_ch1.jpg", validateCharNotEmpty},
#if defined(WEBSOCKET_ENABLED)
      {"websocket.name", websocket.name, "wss prudynt", validateCharNotEmpty},
      {"websocket.token", websocket.token, "auto", [](const char *v) { std::string token(v); return token == "auto" || token.empty() || token.length() == WEBSOCKET_TOKEN_LENGTH; }},
#endif
  };
};

std::vector<ConfigItem<int>> CFG::getIntItems() {
  const auto &encDefaults = hal::defaults::encoder();
  const auto &denoiseDefaults = hal::defaults::denoise();
  return {
      {"audio.mic_bitrate", audio.input_bitrate, 40, [](const int &v) { return v >= 6 && v <= 256; }},
      {"audio.mic_gain", audio.input_gain, 25, [](const int &v) { return v >= -1 && v <= 31; }},
      {"audio.mic_sample_rate", audio.input_sample_rate, 16000, validateSampleRate},
      {"audio.mic_vol", audio.input_vol, 80, [](const int &v) { return v >= -30 && v <= 120; }},
      {"audio.spk_sample_rate", audio.output_sample_rate, 16000, validateSampleRate},
#if defined(LIB_AUDIO_PROCESSING)
      {"audio.mic_agc_target_level_dbfs", audio.input_agc_target_level_dbfs, 10, [](const int &v) { return v >= 0 && v <= 31; }},
      {"audio.mic_agc_compression_gain_db", audio.input_agc_compression_gain_db, 0, [](const int &v) { return v >= 0 && v <= 90; }},
      {"audio.mic_alc_gain", audio.input_alc_gain, 0, [](const int &v) { return v >= -1 && v <= 7; }},
      {"audio.mic_noise_suppression", audio.input_noise_suppression, 0, [](const int &v) { return v >= 0 && v <= 3; }},
      {"audio.spk_gain", audio.output_gain, 20, [](const int &v) { return v >= 0 && v <= 31; }},
      {"audio.spk_vol", audio.output_vol, 60, [](const int &v) { return v >= -30 && v <= 120; }},
#endif
      {"audio.buffer_warn_frames", audio.buffer_warn_frames, 80, [](const int &v) { return v >= 10 && v <= 1000; }},
      {"audio.buffer_cap_frames", audio.buffer_cap_frames, 100, [](const int &v) { return v >= 10 && v <= 1000; }},
      {"daynight.switch_below_percent", daynight.switch_below_percent, 15, [](const int &v) { return v >= 0 && v <= 100; }},
      {"daynight.switch_above_percent", daynight.switch_above_percent, 80, [](const int &v) { return v >= 0 && v <= 100; }},
      {"daynight.tolerance_percent", daynight.tolerance_percent, 50, [](const int &v) { return v >= 0 && v <= 100; }},
      {"daynight.total_gain_night_threshold", daynight.total_gain_night_threshold, 3000, [](const int &v) { return v >= 0 && v <= 10000; }},
      {"daynight.total_gain_day_threshold", daynight.total_gain_day_threshold, 300, [](const int &v) { return v >= 0 && v <= 10000; }},
      {"daynight.sample_interval_ms", daynight.sample_interval_ms, 1000, [](const int &v) { return v >= 100 && v <= 60000; }},
      {"general.imp_polling_timeout", general.imp_polling_timeout, 500, [](const int &v) { return v >= 1 && v <= 5000; }},
      {"general.osd_pool_size", general.osd_pool_size, 1024, [](const int &v) { return v >= 0 && v <= 65535; }},
      {"image.ae_compensation", image.ae_compensation, 128, validateInt255},
      /* Expert overrides preserved for backward compatibility */
      {"daynight.ev_night_high", daynight.ev_night_high, 1900000, [](const int &v) { return v >= 0; }},
      {"daynight.ev_day_low_primary", daynight.ev_day_low_primary, 479832, [](const int &v) { return v >= 0; }},
      {"daynight.ev_day_low_secondary", daynight.ev_day_low_secondary, 361880, [](const int &v) { return v >= 0; }},
      {"daynight.gb_gain_delta", daynight.gb_gain_delta, 15, [](const int &v) { return v >= 0 && v <= 1000; }},
      {"daynight.gb_gain_absolute", daynight.gb_gain_absolute, 145, [](const int &v) { return v >= 0 && v <= 1000; }},
      {"daynight.night_count_threshold", daynight.night_count_threshold, 6, [](const int &v) { return v >= 1 && v <= 100; }},
      {"daynight.day_count_threshold", daynight.day_count_threshold, 4, [](const int &v) { return v >= 1 && v <= 100; }},
      {"daynight.settle_samples_for_gb_record", daynight.settle_samples_for_gb_record, 20, [](const int &v) { return v >= 0 && v <= 200; }},
      {"http.port", http.port, 8080, validateInt65535},
      {"image.anti_flicker", image.anti_flicker, 2, validateInt2},
      {"image.backlight_compensation", image.backlight_compensation, 0, [](const int &v) { return v >= 0 && v <= 10; }},
      {"image.brightness", image.brightness, 128, validateInt255},
      {"image.contrast", image.contrast, 128, validateInt255},
      {"image.core_wb_mode", image.core_wb_mode, 0, [](const int &v) { return v >= 0 && v <= 9; }},
      {"image.defog_strength", image.defog_strength, 128, validateInt255},
      {"image.dpc_strength", image.dpc_strength, 128, validateInt255},
      {"image.drc_strength", image.drc_strength, 128, validateInt255},
      {"image.highlight_depress", image.highlight_depress, 0, validateInt255},
      {"image.hue", image.hue, 128, validateInt255},
      {"image.max_again", image.max_again, 160, [](const int &v) { return v >= 0 && v <= 160; }},
      {"image.max_dgain", image.max_dgain, 80, [](const int &v) { return v >= 0 && v <= 160; }},
      {"image.running_mode", image.running_mode, 0, validateInt1},
      {"image.saturation", image.saturation, 128, validateInt255},
      {"image.sharpness", image.sharpness, 128, validateInt255},
      {"image.sinter_strength", image.sinter_strength, denoiseDefaults.sinter_default,
       [min = denoiseDefaults.sinter_min, max = denoiseDefaults.sinter_max](const int &v) { return v >= min && v <= max; }},
      {"image.temper_strength", image.temper_strength, denoiseDefaults.temper_default,
       [min = denoiseDefaults.temper_min, max = denoiseDefaults.temper_max](const int &v) { return v >= min && v <= max; }},
      {"image.wb_bgain", image.wb_bgain, 0, [](const int &v) { return v >= 0 && v <= 34464; }},
      {"image.wb_rgain", image.wb_rgain, 0, [](const int &v) { return v >= 0 && v <= 34464; }},
      {"motion.debounce_time", motion.debounce_time, 0, validateIntGe0},
      {"motion.post_time", motion.post_time, 0, validateIntGe0},
      {"motion.ivs_polling_timeout", motion.ivs_polling_timeout, 1000, [](const int &v) { return v >= 100 && v <= 10000; }},
      {"motion.cooldown_time", motion.cooldown_time, 5, validateIntGe0},
      {"motion.init_time", motion.init_time, 5, validateIntGe0},
      {"motion.min_time", motion.min_time, 1, validateIntGe0},
      {"motion.motor_settle_ms", motion.motor_settle_ms, 1200, [](const int &v) { return v >= 0 && v <= 10000; }},
      {"motion.sensitivity", motion.sensitivity, 1, validateIntGe0},
      {"motion.skip_frame_count", motion.skip_frame_count, 5, validateIntGe0},
      {"motion.frame_width", motion.frame_width, IVS_AUTO_VALUE, validateIntGe0},
      {"motion.frame_height", motion.frame_height, IVS_AUTO_VALUE, validateIntGe0},
      {"motion.monitor_stream", motion.monitor_stream, 1, [](const int &v) { return v >= 0 && v <= 3; }},
      {"motion.roi_0_x", motion.roi_0_x, 0, validateIntGe0},
      {"motion.roi_0_y", motion.roi_0_y, 0, validateIntGe0},
      {"motion.roi_1_x", motion.roi_1_x, IVS_AUTO_VALUE, validateIntGe0},
      {"motion.roi_1_y", motion.roi_1_y, IVS_AUTO_VALUE, validateIntGe0},
      {"motion.roi_count", motion.roi_count, 1, [](const int &v) { return v >= 1 && v <= 52; }},
      {"recorder.channel", recorder.channel, 0, [](const int &v) { return v == 0 || v == 1; }},
      {"recorder.duration", recorder.duration, 60, [](const int &v) { return v > 0 && v <= 3600; }},
      {"rtsp.est_bitrate", rtsp.est_bitrate, 5000, validateIntGe0},
      {"rtsp.out_buffer_size", rtsp.out_buffer_size, 500000, validateIntGe0},
      {"rtsp.port", rtsp.port, 554, validateInt65535},
      {"rtsp.send_buffer_size", rtsp.send_buffer_size, 307200, validateIntGe0},
      {"rtsp.session_reclaim", rtsp.session_reclaim, 65, validateIntGe0},
      {"sensor.i2c_bus", sensor.i2c_bus, 0, validateIntGe0, false, "/proc/jz/sensor/i2c_bus"},
      // TODO: set default fps to the maximum supported by the SoC via HAL
      {"sensor.fps", sensor.fps, 25, validateInt120, false, "/proc/jz/sensor/max_fps"},
      {"sensor.min_fps", sensor.min_fps, 5, validateInt120, false, "/proc/jz/sensor/min_fps"},
      // TODO: set default height to the maximum supported by the SoC via HAL
      {"sensor.height", sensor.height, 0, validateIntGe0, false, "/proc/jz/sensor/height"},
      // TODO: set default width to the maximum supported by the SoC via HAL
      {"sensor.width", sensor.width, 0, validateIntGe0, false, "/proc/jz/sensor/width"},
      {"sensor.boot", sensor.boot, 0, validateIntGe0, false, "/proc/jz/sensor/boot"},
      {"sensor.mclk", sensor.mclk, 1, validateIntGe0, false, "/proc/jz/sensor/mclk"},
      {"sensor.video_interface", sensor.video_interface, 0, validateIntGe0, false, "/proc/jz/sensor/video_interface"},
      {"sensor.gpio_reset", sensor.gpio_reset, -1, [](const int &v) { return v >= -1; }, false, "/proc/jz/sensor/reset_gpio"},
      {"stream0.bitrate", stream0.bitrate, 3000, validateIntGe0},
      // Rate control advanced (defaults -1/0 mean use encoder defaults)
      {"stream0.qp_init", stream0.qp_init, -1, [](const int &v) { return (v >= -1 && v <= 51); }},
      {"stream0.qp_min", stream0.qp_min, -1, [](const int &v) { return (v >= -1 && v <= 51); }},
      {"stream0.qp_max", stream0.qp_max, -1, [](const int &v) { return (v >= -1 && v <= 51); }},
      {"stream0.ip_delta", stream0.ip_delta, -1, [](const int &v) { return (v == -1) || (v >= -20 && v <= 20); }},
      {"stream0.pb_delta", stream0.pb_delta, -1, [](const int &v) { return (v == -1) || (v >= -20 && v <= 20); }},
      {"stream0.max_bitrate", stream0.max_bitrate, 0, [](const int &v) { return (v == 0) || (v == -1) || (v >= 64000 && v <= 100000000); }},
      {"stream0.buffers", stream0.buffers, encDefaults.stream0_buffers, [](const int &v) { return v >= 1 && v <= 8; }},
      // TODO: set default fps to the maximum supported by the SoC via HAL
      {"stream0.fps", stream0.fps, 25, validateInt120},
      {"stream0.gop", stream0.gop, 20, validateIntGe0},
      // TODO: set default height to the maximum supported by the SoC via HAL
      {"stream0.height", stream0.height, 0, validateIntGe0},
      {"stream0.max_gop", stream0.max_gop, 60, validateIntGe0},
      {"stream0.osd.font_size", stream0.osd.font_size, OSD_AUTO_VALUE, validateIntGe0},
      {"stream0.osd.stroke_size", stream0.osd.stroke_size, 1, validateIntGe0},
      {"stream0.osd.logo.height", stream0.osd.logo_height, 30, validateIntGe0},
      {"stream0.osd.logo.rotation", stream0.osd.logo_rotation, 0, validateInt360},
      {"stream0.osd.logo.transparency", stream0.osd.logo_transparency, 255, validateInt255},
      {"stream0.osd.logo.width", stream0.osd.logo_width, 100, validateIntGe0},
      {"stream0.osd.start_delay", stream0.osd.start_delay, 0, [](const int &v) { return v >= 0 && v <= 5000; }},
      {"stream0.osd.time.rotation", stream0.osd.time_rotation, 0, validateInt360},
      {"stream0.osd.uptime.rotation", stream0.osd.uptime_rotation, 0, validateInt360},
      {"stream0.osd.usertext.rotation", stream0.osd.usertext_rotation, 0, validateInt360},
      {"stream0.osd.brightness.rotation", stream0.osd.brightness_rotation, 0, validateInt360},
      {"stream0.osd.privacy.font_size", stream0.osd.privacy.font_size, OSD_AUTO_VALUE, validateIntGe0},
      {"stream0.osd.privacy.stroke_size", stream0.osd.privacy.stroke_size, 2, validateIntGe0},
      {"stream0.osd.privacy.rotation", stream0.osd.privacy.rotation, 0, validateInt360},
      {"stream0.osd.privacy.image_width", stream0.osd.privacy.image_width, 0, validateIntGe0},
      {"stream0.osd.privacy.image_height", stream0.osd.privacy.image_height, 0, validateIntGe0},
      {"stream0.osd.privacy.layer", stream0.osd.privacy.layer, 16, [](const int &v) { return v >= 0 && v <= 16; }},
      {"stream0.osd.privacy.opacity", stream0.osd.privacy.opacity, 255, validateInt255},
      {"stream0.rotation", stream0.rotation, 0, validateRotation},
      // TODO: set default width to the maximum supported by the SoC via HAL
      {"stream0.width", stream0.width, 0, validateIntGe0},
      {"stream0.profile", stream0.profile, 2, validateInt2},
      {"stream1.bitrate", stream1.bitrate, 1000, validateIntGe0},
      // Rate control advanced (defaults -1/0 mean use encoder defaults)
      {"stream1.qp_init", stream1.qp_init, -1, [](const int &v) { return (v >= -1 && v <= 51); }},
      {"stream1.qp_min", stream1.qp_min, -1, [](const int &v) { return (v >= -1 && v <= 51); }},
      {"stream1.qp_max", stream1.qp_max, -1, [](const int &v) { return (v >= -1 && v <= 51); }},
      {"stream1.ip_delta", stream1.ip_delta, -1, [](const int &v) { return (v == -1) || (v >= -20 && v <= 20); }},
      {"stream1.pb_delta", stream1.pb_delta, -1, [](const int &v) { return (v == -1) || (v >= -20 && v <= 20); }},
      {"stream1.max_bitrate", stream1.max_bitrate, 0, [](const int &v) { return (v == 0) || (v == -1) || (v >= 64000 && v <= 100000000); }},
      {"stream1.buffers", stream1.buffers, encDefaults.stream1_buffers, [](const int &v) { return v >= 1 && v <= 8; }},
      // TODO: set default fps to the maximum supported by the SoC via HAL
      {"stream1.fps", stream1.fps, 25, validateInt120},
      {"stream1.gop", stream1.gop, 20, validateIntGe0},
      // TODO: set default height to the maximum supported by the SoC via HAL
      {"stream1.height", stream1.height, 0, validateIntGe0},
      {"stream1.max_gop", stream1.max_gop, 60, validateIntGe0},
      {"stream1.osd.font_size", stream1.osd.font_size, OSD_AUTO_VALUE, validateIntGe0},
      {"stream1.osd.stroke_size", stream1.osd.stroke_size, 1, validateIntGe0},
      {"stream1.osd.logo.height", stream1.osd.logo_height, 30, validateIntGe0},
      {"stream1.osd.logo.rotation", stream1.osd.logo_rotation, 0, validateInt360},
      {"stream1.osd.logo.transparency", stream1.osd.logo_transparency, 255, validateInt255},
      {"stream1.osd.logo.width", stream1.osd.logo_width, 100, validateIntGe0},
      {"stream1.osd.start_delay", stream1.osd.start_delay, 0, [](const int &v) { return v >= 0 && v <= 5000; }},
      {"stream1.osd.time.rotation", stream1.osd.time_rotation, 0, validateInt360},
      {"stream1.osd.uptime.rotation", stream1.osd.uptime_rotation, 0, validateInt360},
      {"stream1.osd.usertext.rotation", stream1.osd.usertext_rotation, 0, validateInt360},
      {"stream1.osd.brightness.rotation", stream1.osd.brightness_rotation, 0, validateInt360},
      {"stream1.osd.privacy.font_size", stream1.osd.privacy.font_size, OSD_AUTO_VALUE, validateIntGe0},
      {"stream1.osd.privacy.stroke_size", stream1.osd.privacy.stroke_size, 2, validateIntGe0},
      {"stream1.osd.privacy.rotation", stream1.osd.privacy.rotation, 0, validateInt360},
      // TODO: set default image_height to the maximum supported by the SoC via HAL
      {"stream1.osd.privacy.image_height", stream1.osd.privacy.image_height, 0, validateIntGe0},
      // TODO: set default image_width to the maximum supported by the SoC via HAL
      {"stream1.osd.privacy.image_width", stream1.osd.privacy.image_width, 0, validateIntGe0},
      {"stream1.osd.privacy.layer", stream1.osd.privacy.layer, 16, [](const int &v) { return v >= 0 && v <= 16; }},
      {"stream1.osd.privacy.opacity", stream1.osd.privacy.opacity, 255, validateInt255},
      {"stream1.rotation", stream1.rotation, 0, validateRotation},
      // TODO: set default width to the maximum supported by the SoC via HAL
      {"stream1.width", stream1.width, 0, validateIntGe0},
      {"stream1.profile", stream1.profile, 2, validateInt2},
      // TODO: set default fps to the maximum supported by the SoC via HAL
      {"stream2.fps", stream2.fps, 25, [](const int &v) { return v > 1 && v <= 30; }},
      {"stream2.jpeg_channel", stream2.jpeg_channel, 0, validateIntGe0},
      {"stream2.jpeg_quality", stream2.jpeg_quality, 75, [](const int &v) { return v > 0 && v <= 100; }},
      {"stream2.jpeg_idle_fps", stream2.jpeg_idle_fps, 0, [](const int &v) { return v >= 0 && v <= 30; }},
      // TODO: set default fps to the maximum supported by the SoC via HAL
      {"stream3.fps", stream3.fps, 15, [](const int &v) { return v > 1 && v <= 30; }},
      {"stream3.jpeg_channel", stream3.jpeg_channel, 1, validateIntGe0},
      {"stream3.jpeg_quality", stream3.jpeg_quality, 75, [](const int &v) { return v > 0 && v <= 100; }},
      {"stream3.jpeg_idle_fps", stream3.jpeg_idle_fps, 1, [](const int &v) { return v >= 0 && v <= 30; }},
#if defined(WEBSOCKET_ENABLED)
      {"websocket.port", websocket.port, 8089, validateInt65535},
      {"websocket.first_image_delay", websocket.first_image_delay, 100, validateInt65535},
#endif
  };
};

std::vector<ConfigItem<unsigned int>> CFG::getUintItems() {
  return {
      {"sensor.i2c_address", sensor.i2c_address, 0x37, [](const unsigned int &v) { return v <= 0x7F; }, false, "/proc/jz/sensor/i2c_addr"},
      // Individual color settings for stream0 text elements
      {"stream0.osd.brightness.fill_color", stream0.osd.brightness_fill_color, 0xFFFFFFFF, validateOSDColor},
      {"stream0.osd.brightness.stroke_color", stream0.osd.brightness_stroke_color, 0xFF000000, validateOSDColor},
      {"stream0.osd.privacy.fill_color", stream0.osd.privacy.fill_color, 0xFFFF4C4C, validateOSDColor},
      {"stream0.osd.privacy.stroke_color", stream0.osd.privacy.stroke_color, 0xFF000000, validateOSDColor},
      {"stream0.osd.time.fill_color", stream0.osd.time_fill_color, 0xFFFFFFFF, validateOSDColor},
      {"stream0.osd.time.stroke_color", stream0.osd.time_stroke_color, 0xFF000000, validateOSDColor},
      {"stream0.osd.uptime.fill_color", stream0.osd.uptime_fill_color, 0xFFFFFFFF, validateOSDColor},
      {"stream0.osd.uptime.stroke_color", stream0.osd.uptime_stroke_color, 0xFF000000, validateOSDColor},
      {"stream0.osd.usertext.fill_color", stream0.osd.usertext_fill_color, 0xFFFFFFFF, validateOSDColor},
      {"stream0.osd.usertext.stroke_color", stream0.osd.usertext_stroke_color, 0xFF000000, validateOSDColor},
      // Individual color settings for stream1 text elements
      {"stream1.osd.brightness.fill_color", stream1.osd.brightness_fill_color, 0xFFFFFFFF, validateOSDColor},
      {"stream1.osd.brightness.stroke_color", stream1.osd.brightness_stroke_color, 0xFF000000, validateOSDColor},
      {"stream1.osd.privacy.fill_color", stream1.osd.privacy.fill_color, 0xFFFF4C4C, validateOSDColor},
      {"stream1.osd.privacy.stroke_color", stream1.osd.privacy.stroke_color, 0xFF000000, validateOSDColor},
      {"stream1.osd.time.fill_color", stream1.osd.time_fill_color, 0xFFFFFFFF, validateOSDColor},
      {"stream1.osd.time.stroke_color", stream1.osd.time_stroke_color, 0xFF000000, validateOSDColor},
      {"stream1.osd.uptime.fill_color", stream1.osd.uptime_fill_color, 0xFFFFFFFF, validateOSDColor},
      {"stream1.osd.uptime.stroke_color", stream1.osd.uptime_stroke_color, 0xFF000000, validateOSDColor},
      {"stream1.osd.usertext.fill_color", stream1.osd.usertext_fill_color, 0xFFFFFFFF, validateOSDColor},
      {"stream1.osd.usertext.stroke_color", stream1.osd.usertext_stroke_color, 0xFF000000, validateOSDColor},
  };
};

bool CFG::readConfig() {
  // Clean up any existing JSON object
  if (jsonConfig) {
    free_json_value(jsonConfig);
    jsonConfig = nullptr;
  }

  // Construct the path to the configuration file in the same directory as the program binary
  fs::path binaryPath = fs::read_symlink("/proc/self/exe").parent_path();
  fs::path cfgFilePath = binaryPath / "prudynt.json";
  filePath = cfgFilePath;

  // Try to load the configuration file from the specified paths
  std::string configPath;

  // First try the binary directory
  if (fs::exists(cfgFilePath)) {
    configPath = cfgFilePath.string();
    LOG_INFO("Loaded configuration from " + configPath);
  } else {
    // Try /etc/prudynt.json
    fs::path etcPath = "/etc/prudynt.json";
    filePath = etcPath;

    if (fs::exists(etcPath)) {
      configPath = etcPath.string();
      LOG_INFO("Loaded configuration from " + configPath);
    } else {
      LOG_WARN("Failed to load prudynt configuration file from both locations.");
      return false; // Exit if configuration file is missing
    }
  }

  // Load JSON using JCT
  jsonConfig = load_config(configPath.c_str());
  if (!jsonConfig) {
    LOG_WARN("JSON parse error: Failed to parse " + configPath);
    return false; // Exit on parsing error
  }

  return true;
}

template <typename T> bool processLine(const std::string &line, T &value) {
  if constexpr (std::is_same_v<T, std::string>) {
    value = line;
    return true;
  } else if constexpr (std::is_same_v<T, const char *>) {
    value = line.c_str();
    return true;
  } else if constexpr (std::is_same_v<T, unsigned int>) {
    std::istringstream iss(line);
    if (line.find("0x") == 0) {
      iss >> std::hex >> value;
    } else {
      iss >> value;
    }
    return !iss.fail();
  } else {
    std::istringstream iss(line);
    iss >> value;
    return !iss.fail();
  }
}

// Helper function to check if this is a sensor parameter with proc path
template <typename T> bool isSensorProcParameter(const ConfigItem<T> &item) {
  return item.procPath != nullptr && item.procPath[0] != '\0' &&
         std::string(item.procPath).find("/proc/jz/sensor/") == 0;
}

template <typename T> void handleConfigItem(JsonValue *jsonConfig, ConfigItem<T> &item) {
  bool readFromProc = false;
  bool readFromConfig = false;

  if (!jsonConfig)
    return;

  // For sensor parameters with proc paths, prioritize proc files over JSON
  if (isSensorProcParameter(item)) {
    // Try proc file first for sensor parameters
    std::ifstream procFile(item.procPath);
    if (procFile) {
      T value;
      std::string line;
      if (std::getline(procFile, line)) {
        if (processLine(line, value)) {
          if constexpr (std::is_same_v<T, const char *>) {
            item.value = strdup(value);
          } else {
            item.value = value;
          }
          readFromProc = true;
        }
      }
    }
  }

  // Only read from JSON if proc file failed or this is not a sensor proc parameter
  if (!readFromProc) {
    auto getValueForPath = [&](const char *path) -> JsonValue * {
      if (!path)
        return nullptr;
      return getNestedValue(jsonConfig, path);
    };

    JsonValue *valueObj = getValueForPath(item.path);
    if (valueObj) {
      if constexpr (std::is_same_v<T, const char *>) {
        std::string str = jsonValueToString(valueObj);
        if (!str.empty()) {
          item.value = strdup(str.c_str());
          readFromConfig = true;
        }
      } else if constexpr (std::is_same_v<T, bool>) {
        item.value = jsonValueToBool(valueObj, item.defaultValue);
        readFromConfig = true;
      } else if constexpr (std::is_same_v<T, int>) {
        item.value = jsonValueToNumber<int>(valueObj, item.defaultValue);
        readFromConfig = true;
      } else if constexpr (std::is_same_v<T, unsigned int>) {
        if (valueObj->type == JSON_NUMBER && valueObj->value.number >= 0) {
          item.value = static_cast<unsigned int>(valueObj->value.number);
          readFromConfig = true;
        } else if (valueObj->type == JSON_STRING && valueObj->value.string) {
          // Check if this is an OSD color field that might be in hex format
          std::string path = item.path;
          if (path.find("fill_color") != std::string::npos || path.find("stroke_color") != std::string::npos) {
            if (isValidHexColor(valueObj->value.string)) {
              item.value = hexColorToUint(valueObj->value.string);
              readFromConfig = true;
            }
          }
        }
      } else if constexpr (std::is_same_v<T, float>) {
        item.value = jsonValueToNumber<float>(valueObj, item.defaultValue);
        readFromConfig = true;
      }
    }
  }

  // For non-sensor parameters, try proc file as fallback if JSON failed
  if (!readFromConfig && !readFromProc && !isSensorProcParameter(item) && item.procPath != nullptr &&
      item.procPath[0] != '\0') {
    // Attempt to read from the proc filesystem
    std::ifstream procFile(item.procPath);
    if (procFile) {
      T value;
      std::string line;
      if (std::getline(procFile, line)) {
        if (processLine(line, value)) {
          if constexpr (std::is_same_v<T, const char *>) {
            item.value = strdup(value);
          } else {
            item.value = value;
          }
          readFromProc = true;
        }
      }
    }
  }

  if (!readFromConfig && !readFromProc) {
    item.value = item.defaultValue; // Assign default value if not found anywhere
  } else if (!item.validate(item.value)) {
    LOG_ERROR("invalid config value. " << item.path << " = " << item.value);
    item.value = item.defaultValue; // Revert to default if validation fails
  }

  if constexpr (std::is_same_v<T, const char *>) {
    if (!readFromConfig && !readFromProc) {
      item.value = strdup(item.defaultValue);
    } else if (!item.validate(item.value)) {
      LOG_ERROR("invalid config value. " << item.path << " = " << item.value);
      item.value = strdup(item.defaultValue);
    }
  }
}

template <typename T> void handleConfigItem2(JsonValue *jsonConfig, ConfigItem<T> &item) {
  if (!jsonConfig)
    return;

  // Use JCT to set nested values - it handles path creation automatically
  std::string valueStr;

  if constexpr (std::is_same_v<T, const char *>) {
    valueStr = item.value ? std::string(item.value) : "";
  } else if constexpr (std::is_same_v<T, bool>) {
    valueStr = item.value ? "true" : "false";
  } else if constexpr (std::is_same_v<T, int>) {
    valueStr = std::to_string(item.value);
  } else if constexpr (std::is_same_v<T, unsigned int>) {
    // Check if this is a color field that should be formatted as hex string
    std::string path = item.path;
    bool isColorField =
        (path.find("fill_color") != std::string::npos || path.find("stroke_color") != std::string::npos);

    if (isColorField) {
      // Format as hex string: #RRGGBBAA
      char hexStr[10];
      unsigned int value = item.value;
      // Extract ARGB components (internal format is ARGB)
      unsigned int a = (value >> 24) & 0xFF;
      unsigned int r = (value >> 16) & 0xFF;
      unsigned int g = (value >> 8) & 0xFF;
      unsigned int b = value & 0xFF;
      snprintf(hexStr, sizeof(hexStr), "#%02X%02X%02X%02X", r, g, b, a);
      valueStr = hexStr;
    } else {
      // Regular unsigned int - format as number
      valueStr = std::to_string(item.value);
    }
  } else if constexpr (std::is_same_v<T, float>) {
    // Clean up floating-point precision issues for common decimal values
    double clean_value = static_cast<double>(item.value);

    // Round to 6 decimal places to eliminate floating-point representation errors
    // This handles cases like 1.2000000476837158 -> 1.2 and 0.05000000074505806 -> 0.05
    clean_value = std::round(clean_value * 1000000.0) / 1000000.0;

    // Further clean up: if the value is very close to a simple decimal, use that
    double rounded_2dp = std::round(clean_value * 100.0) / 100.0;
    if (std::abs(clean_value - rounded_2dp) < 1e-10) {
      clean_value = rounded_2dp;
    }

    valueStr = std::to_string(clean_value);
  }

  // Set the value using JCT - it automatically creates nested structure and sorts keys
  if (item.path) {
    setNestedValue(jsonConfig, item.path, valueStr);
  }
}

// JCT automatically sorts keys, so no sorting function needed

// Helper function to get a nested JSON value using dot notation
JsonValue *getNestedValue(JsonValue *root, const std::string &path) {
  if (!root)
    return nullptr;
  return get_nested_item(root, path.c_str());
}

// Helper function to set a nested JSON value using dot notation
bool setNestedValue(JsonValue *root, const std::string &path, const std::string &value) {
  if (!root)
    return false;
  return set_nested_item(root, path.c_str(), value.c_str()) != 0;
}

// Helper function to convert JsonValue to string
std::string jsonValueToString(JsonValue *value) {
  if (!value)
    return "";

  switch (value->type) {
  case JSON_STRING:
    return value->value.string ? std::string(value->value.string) : "";
  case JSON_NUMBER:
    return std::to_string(value->value.number);
  case JSON_BOOL:
    return value->value.boolean ? "true" : "false";
  case JSON_NULL:
    return "null";
  default:
    return "";
  }
}

// Helper function to convert JsonValue to numeric types
template <typename T> T jsonValueToNumber(JsonValue *value, T defaultValue) {
  if (!value)
    return defaultValue;

  if (value->type == JSON_NUMBER) {
    return static_cast<T>(value->value.number);
  } else if (value->type == JSON_STRING && value->value.string) {
    try {
      if constexpr (std::is_integral_v<T>) {
        return static_cast<T>(std::stoll(value->value.string));
      } else {
        return static_cast<T>(std::stod(value->value.string));
      }
    } catch (...) {
      return defaultValue;
    }
  }
  return defaultValue;
}

// Helper function to convert JsonValue to bool
bool jsonValueToBool(JsonValue *value, bool defaultValue) {
  if (!value)
    return defaultValue;

  if (value->type == JSON_BOOL) {
    return value->value.boolean != 0;
  } else if (value->type == JSON_STRING && value->value.string) {
    std::string str = value->value.string;
    return str == "true" || str == "1";
  } else if (value->type == JSON_NUMBER) {
    return value->value.number != 0.0;
  }
  return defaultValue;
}

bool CFG::updateConfig() {
  std::lock_guard<std::mutex> lock(configMutex);

  config_loaded = readConfig();

  if (!jsonConfig)
    return false;

  // First, update all values in the existing JSON structure
  for (auto &item : boolItems)
    handleConfigItem2(jsonConfig, item);
  for (auto &item : charItems)
    handleConfigItem2(jsonConfig, item);
  for (auto &item : intItems)
    handleConfigItem2(jsonConfig, item);
  for (auto &item : uintItems)
    handleConfigItem2(jsonConfig, item);
  for (auto &item : floatItems)
    handleConfigItem2(jsonConfig, item);

  // Handle ROIs - clear existing ROIs and add current ones
  for (int i = 0; i < motion.roi_count; i++) {
    std::string roiPath = "rois.roi_" + std::to_string(i);

    // Create array string: [p0_x, p0_y, p1_x, p1_y]
    std::string roiValue = "[" + std::to_string(motion.rois[i].p0_x) + "," + std::to_string(motion.rois[i].p0_y) + "," +
                           std::to_string(motion.rois[i].p1_x) + "," + std::to_string(motion.rois[i].p1_y) + "]";

    setNestedValue(jsonConfig, roiPath, roiValue);
  }

  // Save config using JCT - it automatically sorts keys and formats nicely
  LOG_DEBUG("CFG::updateConfig() - About to save config to " << filePath);
  LOG_DEBUG("CFG::updateConfig() - jsonConfig pointer: " << (uintptr_t)jsonConfig);

  if (!jsonConfig) {
    LOG_ERROR("CFG::updateConfig() - jsonConfig is null!");
    return false;
  }

  int save_result = save_config(filePath.c_str(), jsonConfig);
  LOG_DEBUG("CFG::updateConfig() - save_config returned: " << save_result);

  if (save_result != 0) {
    LOG_DEBUG("Config is written to " << filePath);
    return true;
  } else {
    LOG_ERROR("Failed to serialize JSON config");
    return false;
  }
};

std::vector<ConfigItem<float>> CFG::getFloatItems() {
  return {
      {"rtsp.packet_loss_threshold", rtsp.packet_loss_threshold, 0.05f,
       [](const float &v) { return v >= 0.0f && v <= 1.0f; }},
      {"rtsp.bandwidth_margin", rtsp.bandwidth_margin, 1.2f, [](const float &v) { return v >= 1.0f && v <= 3.0f; }},
  };
};

CFG::CFG() {
  load();
}

void CFG::load() {
  std::lock_guard<std::mutex> lock(configMutex);
  LOG_DEBUG("CFG::load() - Starting configuration load");
  boolItems = getBoolItems();
  LOG_DEBUG("CFG::load() - Got bool items");
  charItems = getCharItems();
  LOG_DEBUG("CFG::load() - Got char items");
  intItems = getIntItems();
  LOG_DEBUG("CFG::load() - Got int items");
  uintItems = getUintItems();
  LOG_DEBUG("CFG::load() - Got uint items");
  floatItems = getFloatItems();
  LOG_DEBUG("CFG::load() - Got float items");

  config_loaded = readConfig();
  LOG_DEBUG("CFG::load() - Read config, loaded=" << config_loaded);

  if (jsonConfig) {
    LOG_DEBUG("CFG::load() - Processing config items");
    LOG_DEBUG("CFG::load() - Processing bool items (" << boolItems.size() << ")");
    for (auto &item : boolItems)
      handleConfigItem(jsonConfig, item);
    LOG_DEBUG("CFG::load() - Processing char items (" << charItems.size() << ")");
    for (auto &item : charItems)
      handleConfigItem(jsonConfig, item);
    LOG_DEBUG("CFG::load() - Processing int items (" << intItems.size() << ")");
    for (auto &item : intItems)
      handleConfigItem(jsonConfig, item);
    LOG_DEBUG("CFG::load() - Processing uint items (" << uintItems.size() << ")");
    for (auto &item : uintItems)
      handleConfigItem(jsonConfig, item);
    LOG_DEBUG("CFG::load() - Processing float items (" << floatItems.size() << ")");
    for (auto &item : floatItems)
      handleConfigItem(jsonConfig, item);
    LOG_DEBUG("CFG::load() - Finished processing config items");
  }

  if (!daynight.loglevel || daynight.loglevel[0] == '\0') {
    daynight.loglevel = (general.loglevel && general.loglevel[0] != '\0') ? general.loglevel : "INFO";
  }

  apply_stream_sensor_defaults(*this);

  if (stream2.jpeg_channel == 0) {
    stream2.width = stream0.width;
    stream2.height = stream0.height;
  } else {
    stream2.width = stream1.width;
    stream2.height = stream1.height;
  }

  if (stream3.jpeg_channel == 0) {
    stream3.width = stream0.width;
    stream3.height = stream0.height;
  } else {
    stream3.width = stream1.width;
    stream3.height = stream1.height;
  }

  apply_motion_sensor_defaults(*this);

  // TODO: Implement ROI handling with JCT
  /*
  if (jsonConfig) {
      // ROI handling code will be reimplemented with JCT
  }
  */
}

bool CFG::saveIntValues(const std::vector<std::pair<std::string, int>> &values) {
  if (values.empty())
    return true;

  if (filePath.empty()) {
    LOG_WARN("CFG::saveIntValues() - filePath is empty");
    return false;
  }

  std::lock_guard<std::mutex> lock(configMutex);
  JsonValue *doc = load_config(filePath.c_str());
  if (!doc) {
    LOG_ERROR("CFG::saveIntValues() - failed to load config " << filePath);
    return false;
  }

  for (const auto &entry : values) {
    if (!setNestedValue(doc, entry.first, std::to_string(entry.second))) {
      LOG_WARN("CFG::saveIntValues() - failed to set key " << entry.first);
    }
  }

  int rc = save_config(filePath.c_str(), doc);
  if (rc == 0) {
    LOG_ERROR("CFG::saveIntValues() - save_config failed for " << filePath);
  }
  free_json_value(doc);
  return rc != 0;
}
