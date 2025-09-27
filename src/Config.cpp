#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cjson/cJSON.h>
#include "Config.hpp"
#include "Logger.hpp"

#define WEBSOCKET_TOKEN_LENGTH 32

#define MODULE "CONFIG"

namespace fs = std::filesystem;

bool validateIntGe0(const int &v)
{
    return v >= 0;
}

bool validateInt1(const int &v)
{
    return v >= 0 && v <= 2;
}

bool validateInt2(const int &v)
{
    return v >= 0 && v <= 2;
}

bool validateInt32(const int &v)
{
    return v >= 0 && v <= 32;
}

bool validateInt50_150(const int &v)
{
    return v >= 50 && v <= 150;
}

bool validateInt120(const int &v)
{
    return (v >= 0 && v <= 120) || v == IMP_AUTO_VALUE;
}

bool validateInt255(const int &v)
{
    return v >= 0 && v <= 255;
}

bool validateInt360(const int &v)
{
    return v >= 0 && v <= 360;
}

bool validateInt15360(const int &v)
{
    return v >= -15360 && v <= 15360;
}

bool validateInt65535(const int &v)
{
    return v >= 0 && v <= 65535;
}

bool validateCharDummy(const char *v)
{
    return true;
}

bool validateCharNotEmpty(const char *v)
{
    return std::strlen(v) > 0;
}

bool validateBool(const bool &v)
{
    return true;
}

bool validateUint(const unsigned int &v)
{
    return true;
}

// Utility function to validate hexadecimal color format (#RRGGBBAA)
bool isValidHexColor(const char* str) {
    if (!str || strlen(str) != 9) {
        return false;
    }

    if (str[0] != '#') {
        return false;
    }

    for (int i = 1; i < 9; i++) {
        char c = str[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'A' && c <= 'F') ||
              (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }

    return true;
}

// Utility function to convert RGBA hexadecimal color string to unsigned int
unsigned int hexColorToUint(const char* str) {
    if (!isValidHexColor(str)) {
        return 0;
    }

    // Parse RGBA format: #RRGGBBAA
    // Extract each component separately to handle RGBA format correctly
    char rStr[3] = {str[1], str[2], '\0'};  // RR
    char gStr[3] = {str[3], str[4], '\0'};  // GG
    char bStr[3] = {str[5], str[6], '\0'};  // BB
    char aStr[3] = {str[7], str[8], '\0'};  // AA

    unsigned int r = strtoul(rStr, nullptr, 16);
    unsigned int g = strtoul(gStr, nullptr, 16);
    unsigned int b = strtoul(bStr, nullptr, 16);
    unsigned int a = strtoul(aStr, nullptr, 16);

    // Pack into ARGB format for internal use (A in bits 24-31, R in 16-23, G in 8-15, B in 0-7)
    // This matches the bit extraction logic used in OSD::drawText()
    return (a << 24) | (r << 16) | (g << 8) | b;
}

// Validation function for OSD color fields that accepts both integer and hex string formats
bool validateOSDColor(const unsigned int &v) {
    // For unsigned int values, any value is valid (same as validateUint)
    return true;
}

// Validation function for OSD color fields when parsing from JSON string
bool validateOSDColorString(const char* str) {
    if (!str) {
        return false;
    }

    // Check if it's a valid hexadecimal color format
    return isValidHexColor(str);
}

bool validateSampleRate(const int &v)
{
    std::set<int> allowed_rates = {8000, 16000, 24000, 44100, 48000};
    return allowed_rates.count(v) == 1;
}

std::vector<ConfigItem<bool>> CFG::getBoolItems()
{
    return {
#if defined(AUDIO_SUPPORT)
        {"audio.input_enabled", audio.input_enabled, true, validateBool},
        {"audio.output_enabled", audio.output_enabled, true, validateBool},
        {"audio.force_stereo", audio.force_stereo, false, validateBool},
#if defined(LIB_AUDIO_PROCESSING)
        {"audio.input_high_pass_filter", audio.input_high_pass_filter, false, validateBool},
        {"audio.input_agc_enabled", audio.input_agc_enabled, false, validateBool},
#endif
#endif
        {"image.isp_bypass", image.isp_bypass, true, validateBool},
        {"image.vflip", image.vflip, false, validateBool},
        {"image.hflip", image.hflip, false, validateBool},
        {"motion.enabled", motion.enabled, false, validateBool},
        {"rtsp.auth_required", rtsp.auth_required, true, validateBool},
#if defined(AUDIO_SUPPORT)
        {"stream0.audio_enabled", stream0.audio_enabled, true, validateBool},
#endif
        {"stream0.enabled", stream0.enabled, true, validateBool},
        {"stream0.allow_shared", stream0.allow_shared, true, validateBool},
        {"stream0.osd.enabled", stream0.osd.enabled, true, validateBool},
        {"stream0.osd.logo_enabled", stream0.osd.logo_enabled, true, validateBool},
        {"stream0.osd.time_enabled", stream0.osd.time_enabled, true, validateBool},
        {"stream0.osd.uptime_enabled", stream0.osd.uptime_enabled, true, validateBool},
        {"stream0.osd.usertext_enabled", stream0.osd.usertext_enabled, true, validateBool},
#if defined(AUDIO_SUPPORT)
        {"stream1.audio_enabled", stream1.audio_enabled, true, validateBool},
#endif
        {"stream1.enabled", stream1.enabled, true, validateBool},
        {"stream1.allow_shared", stream1.allow_shared, true, validateBool},
        {"stream1.osd.enabled", stream1.osd.enabled, true, validateBool},
        {"stream1.osd.logo_enabled", stream1.osd.logo_enabled, true, validateBool},
        {"stream1.osd.time_enabled", stream1.osd.time_enabled, true, validateBool},
        {"stream1.osd.uptime_enabled", stream1.osd.uptime_enabled, true, validateBool},
        {"stream1.osd.usertext_enabled", stream1.osd.usertext_enabled, true, validateBool},
        {"stream2.enabled", stream2.enabled, true, validateBool},
        {"websocket.enabled", websocket.enabled, true, validateBool},
        {"websocket.ws_secured", websocket.ws_secured, true, validateBool},
        {"websocket.http_secured", websocket.http_secured, true, validateBool},
    };
};

std::vector<ConfigItem<const char *>> CFG::getCharItems()
{
    return {
#if defined(AUDIO_SUPPORT)
        {"audio.input_format", audio.input_format, "OPUS", [](const char *v) {
            std::set<std::string> a = {"OPUS", "AAC", "PCM", "G711A", "G711U", "G726"};
            return a.count(std::string(v)) == 1;
        }},
#endif
        {"general.loglevel", general.loglevel, "INFO", [](const char *v) {
            std::set<std::string> a = {"EMERGENCY", "ALERT", "CRITICAL", "ERROR", "WARN", "NOTICE", "INFO", "DEBUG"};
            return a.count(std::string(v)) == 1;
        }},
        {"motion.script_path", motion.script_path, "/usr/sbin/motion", validateCharNotEmpty},
        {"rtsp.name", rtsp.name, "thingino prudynt", validateCharNotEmpty},
        {"rtsp.password", rtsp.password, "thingino", validateCharNotEmpty},
        {"rtsp.username", rtsp.username, "thingino", validateCharNotEmpty},
        {"sensor.model", sensor.model, "unknown", validateCharNotEmpty, false, "/proc/jz/sensor/name"},
        {"sensor.chip_id", sensor.chip_id, "unknown", validateCharNotEmpty, false, "/proc/jz/sensor/chip_id"},
        {"sensor.version", sensor.version, "unknown", validateCharNotEmpty, false, "/proc/jz/sensor/version"},
        {"stream0.format", stream0.format, "H264", [](const char *v) { return strcmp(v, "H264") == 0 || strcmp(v, "H265") == 0; }},
        {"stream0.osd.font_path", stream0.osd.font_path, "/usr/share/fonts/UbuntuMono-Regular2.ttf", validateCharNotEmpty},
        {"stream0.osd.logo_path", stream0.osd.logo_path, "/usr/share/images/thingino_logo_1.bgra", validateCharNotEmpty},
        {"stream0.osd.time_format", stream0.osd.time_format, "%F %T", validateCharNotEmpty},
        {"stream0.osd.uptime_format", stream0.osd.uptime_format, "Up: %02lud %02lu:%02lu", validateCharNotEmpty},
        {"stream0.osd.usertext_format", stream0.osd.usertext_format, "%hostname", validateCharNotEmpty},
        {"stream0.osd.time_position", stream0.osd.time_position, "10,10", validateCharNotEmpty},
        {"stream0.osd.uptime_position", stream0.osd.uptime_position, "1600,5", validateCharNotEmpty},
        {"stream0.osd.usertext_position", stream0.osd.usertext_position, "900,5", validateCharNotEmpty},
        {"stream0.osd.logo_position", stream0.osd.logo_position, "1800,1030", validateCharNotEmpty},
        {"stream0.mode", stream0.mode, DEFAULT_ENC_MODE_0, [](const char *v) {
            std::set<std::string> a = {"CBR", "VBR", "SMART", "FIXQP", "CAPPED_VBR", "CAPPED_QUALITY"};
            return a.count(std::string(v)) == 1;
        }},
        {"stream0.rtsp_endpoint", stream0.rtsp_endpoint, "ch0", validateCharNotEmpty},
        {"stream0.rtsp_info", stream0.rtsp_info, "stream0", validateCharNotEmpty},
        {"stream1.format", stream1.format, "H264", [](const char *v) { return strcmp(v, "H264") == 0 || strcmp(v, "H265") == 0; }},
        {"stream1.osd.font_path", stream1.osd.font_path, "/usr/share/fonts/NotoSansDisplay-Condensed2.ttf", validateCharNotEmpty},
        {"stream1.osd.logo_path", stream1.osd.logo_path, "/usr/share/images/thingino_logo_1.bgra", validateCharNotEmpty},
        {"stream1.osd.time_format", stream1.osd.time_format, "%F %T", validateCharNotEmpty},
        {"stream1.osd.uptime_format", stream1.osd.uptime_format, "Up: %02lud %02lu:%02lu", validateCharNotEmpty},
        {"stream1.osd.usertext_format", stream1.osd.usertext_format, "%hostname", validateCharNotEmpty},
        {"stream1.osd.time_position", stream1.osd.time_position, "10,10", validateCharNotEmpty},
        {"stream1.osd.uptime_position", stream1.osd.uptime_position, "500,5", validateCharNotEmpty},
        {"stream1.osd.usertext_position", stream1.osd.usertext_position, "250,5", validateCharNotEmpty},
        {"stream1.osd.logo_position", stream1.osd.logo_position, "530,320", validateCharNotEmpty},
        {"stream1.mode", stream1.mode, DEFAULT_ENC_MODE_1, [](const char *v) {
            std::set<std::string> a = {"CBR", "VBR", "SMART", "FIXQP", "CAPPED_VBR", "CAPPED_QUALITY"};
            return a.count(std::string(v)) == 1;
        }},
        {"stream1.rtsp_endpoint", stream1.rtsp_endpoint, "ch1", validateCharNotEmpty},
        {"stream1.rtsp_info", stream1.rtsp_info, "stream1", validateCharNotEmpty},
       {"stream2.jpeg_path", stream2.jpeg_path, "/tmp/snapshot.jpg", validateCharNotEmpty},
        {"websocket.name", websocket.name, "wss prudynt", validateCharNotEmpty},
        {"websocket.token", websocket.token, "auto", [](const char *v) {
            std::string token(v);
            return token == "auto" || token.empty() || token.length() == WEBSOCKET_TOKEN_LENGTH;
        }},
    };
};

std::vector<ConfigItem<int>> CFG::getIntItems()
{
    return {
#if defined(AUDIO_SUPPORT)
        {"audio.input_bitrate", audio.input_bitrate, 40, [](const int &v) { return v >= 6 && v <= 256; }},
        {"audio.input_sample_rate", audio.input_sample_rate, 16000, validateSampleRate},
        {"audio.output_sample_rate", audio.output_sample_rate, 16000, validateSampleRate},
        {"audio.input_vol", audio.input_vol, 80, [](const int &v) { return v >= -30 && v <= 120; }},
        {"audio.input_gain", audio.input_gain, 25, [](const int &v) { return v >= -1 && v <= 31; }},
#if defined(LIB_AUDIO_PROCESSING)
        {"audio.input_alc_gain", audio.input_alc_gain, 0, [](const int &v) { return v >= -1 && v <= 7; }},
        {"audio.input_agc_target_level_dbfs", audio.input_agc_target_level_dbfs, 10, [](const int &v) { return v >= 0 && v <= 31; }},
        {"audio.input_agc_compression_gain_db", audio.input_agc_compression_gain_db, 0, [](const int &v) { return v >= 0 && v <= 90; }},
        {"audio.input_noise_suppression", audio.input_noise_suppression, 0, [](const int &v) { return v >= 0 && v <= 3; }},
#endif
#endif
        {"general.imp_polling_timeout", general.imp_polling_timeout, 500, [](const int &v) { return v >= 1 && v <= 5000; }},
        {"general.osd_pool_size", general.osd_pool_size, 1024, [](const int &v) { return v >= 0 && v <= 65535; }},
        {"image.ae_compensation", image.ae_compensation, 128, validateInt255},
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
        {"image.sinter_strength", image.sinter_strength, DEFAULT_SINTER, DEFAULT_SINTER_VALIDATE},
        {"image.temper_strength", image.temper_strength, DEFAULT_TEMPER, DEFAULT_TEMPER_VALIDATE},
        {"image.wb_bgain", image.wb_bgain, 0, [](const int &v) { return v >= 0 && v <= 34464; }},
        {"image.wb_rgain", image.wb_rgain, 0, [](const int &v) { return v >= 0 && v <= 34464; }},
        {"motion.debounce_time", motion.debounce_time, 0, validateIntGe0},
        {"motion.post_time", motion.post_time, 0, validateIntGe0},
        {"motion.ivs_polling_timeout", motion.ivs_polling_timeout, 1000, [](const int &v) { return v >= 100 && v <= 10000; }},
        {"motion.cooldown_time", motion.cooldown_time, 5, validateIntGe0},
        {"motion.init_time", motion.init_time, 5, validateIntGe0},
        {"motion.min_time", motion.min_time, 1, validateIntGe0},
        {"motion.sensitivity", motion.sensitivity, 1, validateIntGe0},
        {"motion.skip_frame_count", motion.skip_frame_count, 5, validateIntGe0},
        {"motion.frame_width", motion.frame_width, IVS_AUTO_VALUE, validateIntGe0},
        {"motion.frame_height", motion.frame_height, IVS_AUTO_VALUE, validateIntGe0},
        {"motion.monitor_stream", motion.monitor_stream, 1, validateInt1},
        {"motion.roi_0_x", motion.roi_0_x, 0, validateIntGe0},
        {"motion.roi_0_y", motion.roi_0_y, 0, validateIntGe0},
        {"motion.roi_1_x", motion.roi_1_x, IVS_AUTO_VALUE, validateIntGe0},
        {"motion.roi_1_y", motion.roi_1_y, IVS_AUTO_VALUE, validateIntGe0},
        {"motion.roi_count", motion.roi_count, 1, [](const int &v) { return v >= 1 && v <= 52; }},
        {"rtsp.est_bitrate", rtsp.est_bitrate, 5000, validateIntGe0},
        {"rtsp.out_buffer_size", rtsp.out_buffer_size, 500000, validateIntGe0},
        {"rtsp.port", rtsp.port, 554, validateInt65535},
        {"rtsp.send_buffer_size", rtsp.send_buffer_size, 307200, validateIntGe0},
        {"rtsp.session_reclaim", rtsp.session_reclaim, 65, validateIntGe0},
        {"sensor.i2c_bus", sensor.i2c_bus, 0, validateIntGe0, false, "/proc/jz/sensor/i2c_bus"},
        {"sensor.fps", sensor.fps, 25, validateInt120, false, "/proc/jz/sensor/max_fps"},
        {"sensor.min_fps", sensor.min_fps, 5, validateInt120, false, "/proc/jz/sensor/min_fps"},
        {"sensor.height", sensor.height, 1080, validateIntGe0, false, "/proc/jz/sensor/height"},
        {"sensor.width", sensor.width, 1920, validateIntGe0, false, "/proc/jz/sensor/width"},
        {"sensor.boot", sensor.boot, 0, validateIntGe0, false, "/proc/jz/sensor/boot"},
        {"sensor.mclk", sensor.mclk, 1, validateIntGe0, false, "/proc/jz/sensor/mclk"},
        {"sensor.video_interface", sensor.video_interface, 0, validateIntGe0, false, "/proc/jz/sensor/video_interface"},
        {"sensor.gpio_reset", sensor.gpio_reset, -1, [](const int &v) { return v >= -1; }, false, "/proc/jz/sensor/reset_gpio"},
        {"stream0.bitrate", stream0.bitrate, 3000, validateIntGe0},
        {"stream0.buffers", stream0.buffers, DEFAULT_BUFFERS_0, [](const int &v) { return v >= 1 && v <= 8; }},
        {"stream0.fps", stream0.fps, 25, validateInt120},
        {"stream0.gop", stream0.gop, 20, validateIntGe0},
        {"stream0.height", stream0.height, 1080, validateIntGe0},
        {"stream0.max_gop", stream0.max_gop, 60, validateIntGe0},
        {"stream0.osd.font_size", stream0.osd.font_size, OSD_AUTO_VALUE, validateIntGe0},
        {"stream0.osd.font_stroke_size", stream0.osd.font_stroke_size, 1, validateIntGe0},
        {"stream0.osd.logo_height", stream0.osd.logo_height, 30, validateIntGe0},
        {"stream0.osd.logo_rotation", stream0.osd.logo_rotation, 0, validateInt360},
        {"stream0.osd.logo_transparency", stream0.osd.logo_transparency, 255, validateInt255},
        {"stream0.osd.logo_width", stream0.osd.logo_width, 100, validateIntGe0},
        {"stream0.osd.start_delay", stream0.osd.start_delay, 0, [](const int &v) { return v >= 0 && v <= 5000; }},
        {"stream0.osd.time_rotation", stream0.osd.time_rotation, 0, validateInt360},
        {"stream0.osd.uptime_rotation", stream0.osd.uptime_rotation, 0, validateInt360},
        {"stream0.osd.usertext_rotation", stream0.osd.usertext_rotation, 0, validateInt360},
        {"stream0.rotation", stream0.rotation, 0, validateInt2},
        {"stream0.width", stream0.width, 1920, validateIntGe0},
        {"stream0.profile", stream0.profile, 2, validateInt2},
        {"stream1.bitrate", stream1.bitrate, 1000, validateIntGe0},
        {"stream1.buffers", stream1.buffers, DEFAULT_BUFFERS_1, [](const int &v) { return v >= 1 && v <= 8; }},
        {"stream1.fps", stream1.fps, 25, validateInt120},
        {"stream1.gop", stream1.gop, 20, validateIntGe0},
        {"stream1.height", stream1.height, 360, validateIntGe0},
        {"stream1.max_gop", stream1.max_gop, 60, validateIntGe0},
        {"stream1.osd.font_size", stream1.osd.font_size, OSD_AUTO_VALUE, validateIntGe0},
        {"stream1.osd.font_stroke_size", stream1.osd.font_stroke_size, 1, validateIntGe0},
        {"stream1.osd.logo_height", stream1.osd.logo_height, 30, validateIntGe0},
        {"stream1.osd.logo_rotation", stream1.osd.logo_rotation, 0, validateInt360},
        {"stream1.osd.logo_transparency", stream1.osd.logo_transparency, 255, validateInt255},
        {"stream1.osd.logo_width", stream1.osd.logo_width, 100, validateIntGe0},
        {"stream1.osd.start_delay", stream1.osd.start_delay, 0, [](const int &v) { return v >= 0 && v <= 5000; }},
        {"stream1.osd.time_rotation", stream1.osd.time_rotation, 0, validateInt360},
        {"stream1.osd.uptime_rotation", stream1.osd.uptime_rotation, 0, validateInt360},
        {"stream1.osd.usertext_rotation", stream1.osd.usertext_rotation, 0, validateInt360},
        {"stream1.rotation", stream1.rotation, 0, validateInt2},
        {"stream1.width", stream1.width, 640, validateIntGe0},
        {"stream1.profile", stream1.profile, 2, validateInt2},
        {"stream2.jpeg_channel", stream2.jpeg_channel, 0, validateIntGe0},
        {"stream2.jpeg_quality", stream2.jpeg_quality, 75, [](const int &v) { return v > 0 && v <= 100; }},
        {"stream2.jpeg_idle_fps", stream2.jpeg_idle_fps, 1, [](const int &v) { return v >= 0 && v <= 30; }},
        {"stream2.fps", stream2.fps, 25, [](const int &v) { return v > 1 && v <= 30; }},
        {"websocket.port", websocket.port, 8089, validateInt65535},
        {"websocket.first_image_delay", websocket.first_image_delay, 100, validateInt65535},
    };
};

std::vector<ConfigItem<unsigned int>> CFG::getUintItems()
{
    return {
        {"sensor.i2c_address", sensor.i2c_address, 0x37, [](const unsigned int &v) { return v <= 0x7F; }, false, "/proc/jz/sensor/i2c_addr"},
        // Individual color settings for stream0 text elements
        {"stream0.osd.time_font_color", stream0.osd.time_font_color, 0xFFFFFFFF, validateOSDColor},
        {"stream0.osd.time_font_stroke_color", stream0.osd.time_font_stroke_color, 0xFF000000, validateOSDColor},
        {"stream0.osd.uptime_font_color", stream0.osd.uptime_font_color, 0xFFFFFFFF, validateOSDColor},
        {"stream0.osd.uptime_font_stroke_color", stream0.osd.uptime_font_stroke_color, 0xFF000000, validateOSDColor},
        {"stream0.osd.usertext_font_color", stream0.osd.usertext_font_color, 0xFFFFFFFF, validateOSDColor},
        {"stream0.osd.usertext_font_stroke_color", stream0.osd.usertext_font_stroke_color, 0xFF000000, validateOSDColor},
        // Individual color settings for stream1 text elements
        {"stream1.osd.time_font_color", stream1.osd.time_font_color, 0xFFFFFFFF, validateOSDColor},
        {"stream1.osd.time_font_stroke_color", stream1.osd.time_font_stroke_color, 0xFF000000, validateOSDColor},
        {"stream1.osd.uptime_font_color", stream1.osd.uptime_font_color, 0xFFFFFFFF, validateOSDColor},
        {"stream1.osd.uptime_font_stroke_color", stream1.osd.uptime_font_stroke_color, 0xFF000000, validateOSDColor},
        {"stream1.osd.usertext_font_color", stream1.osd.usertext_font_color, 0xFFFFFFFF, validateOSDColor},
        {"stream1.osd.usertext_font_stroke_color", stream1.osd.usertext_font_stroke_color, 0xFF000000, validateOSDColor},
    };
};

bool CFG::readConfig()
{
    // Clean up any existing JSON object
    if (jsonConfig) {
        cJSON_Delete(jsonConfig);
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

    // Parse JSON file using cJSON
    std::ifstream configFile(configPath);
    if (!configFile.is_open()) {
        LOG_WARN("Failed to open configuration file: " + configPath);
        return false;
    }

    std::string jsonContent((std::istreambuf_iterator<char>(configFile)),
                            std::istreambuf_iterator<char>());
    configFile.close();

    jsonConfig = cJSON_Parse(jsonContent.c_str());
    if (!jsonConfig) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != nullptr) {
            LOG_WARN("JSON parse error: " + std::string(error_ptr) + " in " + configPath);
        } else {
            LOG_WARN("JSON parse error: Failed to parse " + configPath);
        }
        return false; // Exit on parsing error
    }

    return true;
}

template <typename T>
bool processLine(const std::string &line, T &value)
{
    if constexpr (std::is_same_v<T, std::string>)
    {
        value = line;
        return true;
    }
    else if constexpr (std::is_same_v<T, const char *>)
    {
        value = line.c_str();
        return true;
    }
    else if constexpr (std::is_same_v<T, unsigned int>)
    {
        std::istringstream iss(line);
        if (line.find("0x") == 0)
        {
            iss >> std::hex >> value;
        }
        else
        {
            iss >> value;
        }
        return !iss.fail();
    }
    else
    {
        std::istringstream iss(line);
        iss >> value;
        return !iss.fail();
    }
}

// Helper function to check if this is a sensor parameter with proc path
template <typename T>
bool isSensorProcParameter(const ConfigItem<T> &item) {
    return item.procPath != nullptr &&
           item.procPath[0] != '\0' &&
           std::string(item.procPath).find("/proc/jz/sensor/") == 0;
}

template <typename T>
void handleConfigItem(cJSON *jsonConfig, ConfigItem<T> &item)
{
    bool readFromProc = false;
    bool readFromConfig = false;

    if (!jsonConfig) return;

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
        // Parse the path to navigate through nested JSON objects
        std::vector<std::string> pathParts;
        std::string path = item.path;
        size_t pos = 0;
        while ((pos = path.find('.')) != std::string::npos) {
            pathParts.push_back(path.substr(0, pos));
            path.erase(0, pos + 1);
        }
        pathParts.push_back(path);

        // Navigate through the JSON structure
        cJSON *currentJson = jsonConfig;
        for (size_t i = 0; i < pathParts.size() - 1; ++i) {
            cJSON *nextObj = cJSON_GetObjectItem(currentJson, pathParts[i].c_str());
            if (nextObj && cJSON_IsObject(nextObj)) {
                currentJson = nextObj;
            } else {
                return; // Path doesn't exist or not an object
            }
        }

        // Try to read the value from JSON
        const std::string& finalKey = pathParts.back();
        cJSON *valueObj = cJSON_GetObjectItem(currentJson, finalKey.c_str());
        if (valueObj) {
            if constexpr (std::is_same_v<T, const char *>) {
                if (cJSON_IsString(valueObj)) {
                    const char *str = cJSON_GetStringValue(valueObj);
                    item.value = strdup(str);
                    readFromConfig = true;
                }
            } else if constexpr (std::is_same_v<T, bool>) {
                if (cJSON_IsBool(valueObj)) {
                    item.value = cJSON_IsTrue(valueObj);
                    readFromConfig = true;
                }
            } else if constexpr (std::is_same_v<T, int>) {
                if (cJSON_IsNumber(valueObj)) {
                    item.value = static_cast<int>(cJSON_GetNumberValue(valueObj));
                    readFromConfig = true;
                }
            } else if constexpr (std::is_same_v<T, unsigned int>) {
                if (cJSON_IsNumber(valueObj)) {
                    double val = cJSON_GetNumberValue(valueObj);
                    if (val >= 0) {
                        item.value = static_cast<unsigned int>(val);
                        readFromConfig = true;
                    }
                } else if (cJSON_IsString(valueObj)) {
                    // Check if this is an OSD color field that might be in hex format
                    std::string path = item.path;
                    if (path.find("font_color") != std::string::npos ||
                        path.find("font_stroke_color") != std::string::npos) {
                        const char *str = cJSON_GetStringValue(valueObj);
                        if (isValidHexColor(str)) {
                            item.value = hexColorToUint(str);
                            readFromConfig = true;
                        }
                    }
                }
            } else if constexpr (std::is_same_v<T, float>) {
                if (cJSON_IsNumber(valueObj)) {
                    item.value = static_cast<float>(cJSON_GetNumberValue(valueObj));
                    readFromConfig = true;
                }
            }
        }
    }

    // For non-sensor parameters, try proc file as fallback if JSON failed
    if (!readFromConfig && !readFromProc && !isSensorProcParameter(item) &&
        item.procPath != nullptr && item.procPath[0] != '\0') {
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

    if (!readFromConfig && !readFromProc)
    {
        item.value = item.defaultValue; // Assign default value if not found anywhere
    }
    else if (!item.validate(item.value))
    {
        LOG_ERROR("invalid config value. " << item.path << " = " << item.value);
        item.value = item.defaultValue; // Revert to default if validation fails
    }

    if constexpr (std::is_same_v<T, const char *>)
    {
        if (!readFromConfig && !readFromProc)
        {
            item.value = strdup(item.defaultValue);
        }
        else if (!item.validate(item.value))
        {
            LOG_ERROR("invalid config value. " << item.path << " = " << item.value);
            item.value = strdup(item.defaultValue);
        }
    }
}

template <typename T>
void handleConfigItem2(cJSON *jsonConfig, ConfigItem<T> &item)
{
    if (!jsonConfig) return;

    // Parse the path to navigate through nested JSON objects
    std::vector<std::string> pathParts;
    std::string path = item.path;
    size_t pos = 0;
    while ((pos = path.find('.')) != std::string::npos) {
        pathParts.push_back(path.substr(0, pos));
        path.erase(0, pos + 1);
    }
    pathParts.push_back(path);

    // Navigate through the JSON structure, creating objects as needed
    cJSON *currentJson = jsonConfig;
    for (size_t i = 0; i < pathParts.size() - 1; ++i) {
        cJSON *nextObj = cJSON_GetObjectItem(currentJson, pathParts[i].c_str());
        if (!nextObj) {
            // Create new object if it doesn't exist
            nextObj = cJSON_CreateObject();
            cJSON_AddItemToObject(currentJson, pathParts[i].c_str(), nextObj);
        } else if (!cJSON_IsObject(nextObj)) {
            // If the item exists but is not an object, replace it with an object
            cJSON_DeleteItemFromObject(currentJson, pathParts[i].c_str());
            nextObj = cJSON_CreateObject();
            cJSON_AddItemToObject(currentJson, pathParts[i].c_str(), nextObj);
        }
        currentJson = nextObj;
    }

    // Update the final value in place to preserve JSON structure
    const std::string& finalKey = pathParts.back();
    cJSON *existingItem = cJSON_GetObjectItem(currentJson, finalKey.c_str());

    if constexpr (std::is_same_v<T, const char *>) {
        if (existingItem && cJSON_IsString(existingItem)) {
            // Update existing string value in place
            cJSON_SetValuestring(existingItem, item.value);
        } else {
            // Create new string item
            if (existingItem) cJSON_DeleteItemFromObject(currentJson, finalKey.c_str());
            cJSON_AddItemToObject(currentJson, finalKey.c_str(), cJSON_CreateString(item.value));
        }
    } else if constexpr (std::is_same_v<T, bool>) {
        if (existingItem && cJSON_IsBool(existingItem)) {
            // Update existing boolean value in place
            if (item.value) {
                existingItem->type = cJSON_True;
            } else {
                existingItem->type = cJSON_False;
            }
        } else {
            // Create new boolean item
            if (existingItem) cJSON_DeleteItemFromObject(currentJson, finalKey.c_str());
            cJSON_AddItemToObject(currentJson, finalKey.c_str(), cJSON_CreateBool(item.value));
        }
    } else if constexpr (std::is_same_v<T, int>) {
        if (existingItem && cJSON_IsNumber(existingItem)) {
            // Update existing number value in place
            cJSON_SetNumberValue(existingItem, static_cast<double>(item.value));
        } else {
            // Create new number item
            if (existingItem) cJSON_DeleteItemFromObject(currentJson, finalKey.c_str());
            cJSON_AddItemToObject(currentJson, finalKey.c_str(), cJSON_CreateNumber(static_cast<double>(item.value)));
        }
    } else if constexpr (std::is_same_v<T, unsigned int>) {
        // Check if this is a color field that should be formatted as hex string
        std::string path = item.path;
        bool isColorField = (path.find("font_color") != std::string::npos ||
                            path.find("font_stroke_color") != std::string::npos);

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

            if (existingItem && cJSON_IsString(existingItem)) {
                // Update existing string value in place
                cJSON_SetValuestring(existingItem, hexStr);
            } else {
                // Create new string item
                if (existingItem) cJSON_DeleteItemFromObject(currentJson, finalKey.c_str());
                cJSON_AddItemToObject(currentJson, finalKey.c_str(), cJSON_CreateString(hexStr));
            }
        } else {
            // Regular unsigned int - format as number
            if (existingItem && cJSON_IsNumber(existingItem)) {
                // Update existing number value in place
                cJSON_SetNumberValue(existingItem, static_cast<double>(item.value));
            } else {
                // Create new number item
                if (existingItem) cJSON_DeleteItemFromObject(currentJson, finalKey.c_str());
                cJSON_AddItemToObject(currentJson, finalKey.c_str(), cJSON_CreateNumber(static_cast<double>(item.value)));
            }
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

        if (existingItem && cJSON_IsNumber(existingItem)) {
            cJSON_SetNumberValue(existingItem, clean_value);
        } else {
            if (existingItem) cJSON_DeleteItemFromObject(currentJson, finalKey.c_str());
            cJSON_AddItemToObject(currentJson, finalKey.c_str(), cJSON_CreateNumber(clean_value));
        }
    }
}

// Recursively sort all JSON objects to maintain alphabetical key order
void CFG::sortJsonObjectsRecursively(cJSON *json)
{
    if (!json) return;

    if (cJSON_IsObject(json)) {
        // Collect all key-value pairs
        std::vector<std::pair<std::string, cJSON*>> items;

        cJSON *child = json->child;
        while (child) {
            cJSON *next = child->next; // Store next before we modify the list
            items.emplace_back(child->string ? child->string : "", child);
            child = next;
        }

        // Sort by key name
        std::sort(items.begin(), items.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        // Clear the original child list
        json->child = nullptr;

        // Re-add items in sorted order
        cJSON *prev = nullptr;
        for (const auto& item : items) {
            cJSON *node = item.second;
            node->next = nullptr;
            node->prev = prev;

            if (prev) {
                prev->next = node;
            } else {
                json->child = node;
            }
            prev = node;

            // Recursively sort child objects
            sortJsonObjectsRecursively(node);
        }
    } else if (cJSON_IsArray(json)) {
        // For arrays, just recursively sort child objects
        cJSON *child = json->child;
        while (child) {
            sortJsonObjectsRecursively(child);
            child = child->next;
        }
    }
}

bool CFG::updateConfig()
{
    std::lock_guard<std::mutex> lock(configMutex);

    config_loaded = readConfig();

    if (!jsonConfig) return false;

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

    // Now sort all JSON objects to ensure alphabetical key order
    sortJsonObjectsRecursively(jsonConfig);

    // Handle ROIs
    cJSON *roisObj = cJSON_GetObjectItem(jsonConfig, "rois");
    if (roisObj) {
        cJSON_DeleteItemFromObject(jsonConfig, "rois");
    }

    roisObj = cJSON_CreateObject();
    cJSON_AddItemToObject(jsonConfig, "rois", roisObj);

    for (int i = 0; i < motion.roi_count; i++)
    {
        std::string roiKey = "roi_" + std::to_string(i);
        cJSON *roiArray = cJSON_CreateArray();

        cJSON_AddItemToArray(roiArray, cJSON_CreateNumber(motion.rois[i].p0_x));
        cJSON_AddItemToArray(roiArray, cJSON_CreateNumber(motion.rois[i].p0_y));
        cJSON_AddItemToArray(roiArray, cJSON_CreateNumber(motion.rois[i].p1_x));
        cJSON_AddItemToArray(roiArray, cJSON_CreateNumber(motion.rois[i].p1_y));

        cJSON_AddItemToObject(roisObj, roiKey.c_str(), roiArray);
    }

    // Write JSON to file
    char *jsonString = cJSON_Print(jsonConfig);
    if (jsonString) {
        std::ofstream configFile(filePath);
        if (configFile.is_open()) {
            configFile << jsonString;
            configFile.close();
            free(jsonString); // cJSON_Print allocates memory that must be freed
            LOG_DEBUG("Config is written to " << filePath);
            return true;
        } else {
            free(jsonString);
            LOG_ERROR("Failed to write config to " << filePath);
            return false;
        }
    } else {
        LOG_ERROR("Failed to serialize JSON config");
        return false;
    }
};

std::vector<ConfigItem<float>> CFG::getFloatItems()
{
    return {
        {"rtsp.packet_loss_threshold", rtsp.packet_loss_threshold, 0.05f, [](const float &v) { return v >= 0.0f && v <= 1.0f; }},
        {"rtsp.bandwidth_margin", rtsp.bandwidth_margin, 1.2f, [](const float &v) { return v >= 1.0f && v <= 3.0f; }},
    };
};

void CFG::migrateOldColorSettings()
{
    // Helper function to combine RGB color with alpha transparency
    // Creates ARGB format for internal use (matches OSD::drawText() bit extraction)
    auto combineColorWithAlpha = [](unsigned int rgb_color, int transparency) -> unsigned int {
        // Extract RGB components from the input color
        unsigned int r = (rgb_color >> 16) & 0xFF;
        unsigned int g = (rgb_color >> 8) & 0xFF;
        unsigned int b = rgb_color & 0xFF;

        // Combine with alpha (transparency is 0-255, where 255 = opaque)
        unsigned int alpha = (unsigned int)transparency & 0xFF;

        // Pack into ARGB format for internal use (A in bits 24-31)
        return (alpha << 24) | (r << 16) | (g << 8) | b;
    };

    // Check for old configuration format and migrate
    cJSON *stream0Obj = cJSON_GetObjectItem(jsonConfig, "stream0");
    cJSON *stream1Obj = cJSON_GetObjectItem(jsonConfig, "stream1");

    if (stream0Obj) {
        cJSON *osdObj = cJSON_GetObjectItem(stream0Obj, "osd");
        if (osdObj) {
            // Check if old format exists (font_color + transparency fields)
            cJSON *fontColorObj = cJSON_GetObjectItem(osdObj, "font_color");
            cJSON *fontStrokeColorObj = cJSON_GetObjectItem(osdObj, "font_stroke_color");
            cJSON *timeTransparencyObj = cJSON_GetObjectItem(osdObj, "time_transparency");
            cJSON *uptimeTransparencyObj = cJSON_GetObjectItem(osdObj, "uptime_transparency");
            cJSON *userTextTransparencyObj = cJSON_GetObjectItem(osdObj, "usertext_transparency");

            bool hasOldFormat = fontColorObj && fontStrokeColorObj &&
                               (timeTransparencyObj || uptimeTransparencyObj || userTextTransparencyObj);

            if (hasOldFormat) {
                unsigned int fontColor = 0xFFFFFFFF;  // Default white
                unsigned int fontStrokeColor = 0xFF000000;  // Default black
                int timeTransparency = 255;
                int uptimeTransparency = 255;
                int userTextTransparency = 255;

                // Read old values
                if (cJSON_IsNumber(fontColorObj)) {
                    fontColor = static_cast<unsigned int>(cJSON_GetNumberValue(fontColorObj));
                } else if (cJSON_IsString(fontColorObj)) {
                    const char *str = cJSON_GetStringValue(fontColorObj);
                    if (isValidHexColor(str)) {
                        fontColor = hexColorToUint(str);
                    }
                }

                if (cJSON_IsNumber(fontStrokeColorObj)) {
                    fontStrokeColor = static_cast<unsigned int>(cJSON_GetNumberValue(fontStrokeColorObj));
                } else if (cJSON_IsString(fontStrokeColorObj)) {
                    const char *str = cJSON_GetStringValue(fontStrokeColorObj);
                    if (isValidHexColor(str)) {
                        fontStrokeColor = hexColorToUint(str);
                    }
                }

                if (timeTransparencyObj && cJSON_IsNumber(timeTransparencyObj)) {
                    timeTransparency = static_cast<int>(cJSON_GetNumberValue(timeTransparencyObj));
                }
                if (uptimeTransparencyObj && cJSON_IsNumber(uptimeTransparencyObj)) {
                    uptimeTransparency = static_cast<int>(cJSON_GetNumberValue(uptimeTransparencyObj));
                }
                if (userTextTransparencyObj && cJSON_IsNumber(userTextTransparencyObj)) {
                    userTextTransparency = static_cast<int>(cJSON_GetNumberValue(userTextTransparencyObj));
                }

                // Create new individual color settings
                cJSON_AddItemToObject(osdObj, "time_font_color",
                    cJSON_CreateNumber(combineColorWithAlpha(fontColor, timeTransparency)));
                cJSON_AddItemToObject(osdObj, "time_font_stroke_color",
                    cJSON_CreateNumber(combineColorWithAlpha(fontStrokeColor, timeTransparency)));
                cJSON_AddItemToObject(osdObj, "uptime_font_color",
                    cJSON_CreateNumber(combineColorWithAlpha(fontColor, uptimeTransparency)));
                cJSON_AddItemToObject(osdObj, "uptime_font_stroke_color",
                    cJSON_CreateNumber(combineColorWithAlpha(fontStrokeColor, uptimeTransparency)));
                cJSON_AddItemToObject(osdObj, "usertext_font_color",
                    cJSON_CreateNumber(combineColorWithAlpha(fontColor, userTextTransparency)));
                cJSON_AddItemToObject(osdObj, "usertext_font_stroke_color",
                    cJSON_CreateNumber(combineColorWithAlpha(fontStrokeColor, userTextTransparency)));

                // Remove old settings
                cJSON_DeleteItemFromObject(osdObj, "font_color");
                cJSON_DeleteItemFromObject(osdObj, "font_stroke_color");
                cJSON_DeleteItemFromObject(osdObj, "time_transparency");
                cJSON_DeleteItemFromObject(osdObj, "uptime_transparency");
                cJSON_DeleteItemFromObject(osdObj, "usertext_transparency");
                cJSON_DeleteItemFromObject(osdObj, "font_yoffset");
            }
            // Migrate stroke settings: font_stroke/font_stroke_enabled -> font_stroke_size
            {
                cJSON *strokeSizeObj = cJSON_GetObjectItem(osdObj, "font_stroke_size");
                if (!strokeSizeObj) {
                    cJSON *strokeObj = cJSON_GetObjectItem(osdObj, "font_stroke");
                    cJSON *strokeEnabledObj = cJSON_GetObjectItem(osdObj, "font_stroke_enabled");
                    int strokeVal = -1;
                    if (strokeObj && cJSON_IsNumber(strokeObj)) {
                        strokeVal = (int)cJSON_GetNumberValue(strokeObj);
                    }
                    // If explicitly disabled, set size to 0
                    if (strokeEnabledObj && cJSON_IsBool(strokeEnabledObj) && !cJSON_IsTrue(strokeEnabledObj)) {
                        strokeVal = 0;
                    }
                    if (strokeVal >= 0) {
                        cJSON_AddItemToObject(osdObj, "font_stroke_size", cJSON_CreateNumber(strokeVal));
                    }
                    if (strokeObj) cJSON_DeleteItemFromObject(osdObj, "font_stroke");
                    if (strokeEnabledObj) cJSON_DeleteItemFromObject(osdObj, "font_stroke_enabled");
                } else {
                    // Clean up redundant keys if present
                    cJSON_DeleteItemFromObject(osdObj, "font_stroke");
                    cJSON_DeleteItemFromObject(osdObj, "font_stroke_enabled");
                }
            }
            // Migrate position settings: pos_*_x/pos_*_y -> *_position ("x,y")
            {
                auto migrate_pos = [&](const char *new_key, const char *old_x, const char *old_y, const char *alt_old_x = nullptr, const char *alt_old_y = nullptr) {
                    if (!cJSON_GetObjectItem(osdObj, new_key)) {
                        cJSON *xObj = cJSON_GetObjectItem(osdObj, old_x);
                        cJSON *yObj = cJSON_GetObjectItem(osdObj, old_y);
                        if ((!xObj || !yObj) && (alt_old_x && alt_old_y)) {
                            xObj = cJSON_GetObjectItem(osdObj, alt_old_x);
                            yObj = cJSON_GetObjectItem(osdObj, alt_old_y);
                        }
                        if (xObj && yObj && cJSON_IsNumber(xObj) && cJSON_IsNumber(yObj)) {
                            int xi = (int)cJSON_GetNumberValue(xObj);
                            int yi = (int)cJSON_GetNumberValue(yObj);
                            char buf[64];
                            snprintf(buf, sizeof(buf), "%d,%d", xi, yi);
                            cJSON_AddItemToObject(osdObj, new_key, cJSON_CreateString(buf));
                        }
                    }
                    // Cleanup old keys regardless
                    cJSON_DeleteItemFromObject(osdObj, old_x);
                    cJSON_DeleteItemFromObject(osdObj, old_y);
                    if (alt_old_x) cJSON_DeleteItemFromObject(osdObj, alt_old_x);
                    if (alt_old_y) cJSON_DeleteItemFromObject(osdObj, alt_old_y);
                };

                migrate_pos("time_position", "pos_time_x", "pos_time_y");
                migrate_pos("uptime_position", "pos_uptime_x", "pos_uptime_y");
                migrate_pos("usertext_position", "pos_usertext_x", "pos_usertext_y", "pos_user_text_x", "pos_user_text_y");
                migrate_pos("logo_position", "pos_logo_x", "pos_logo_y");
            }


        }
    }

    // Repeat for stream1
    if (stream1Obj) {
        cJSON *osdObj = cJSON_GetObjectItem(stream1Obj, "osd");
        if (osdObj) {
            // Similar migration logic for stream1
            cJSON *fontColorObj = cJSON_GetObjectItem(osdObj, "font_color");
            cJSON *fontStrokeColorObj = cJSON_GetObjectItem(osdObj, "font_stroke_color");
            cJSON *timeTransparencyObj = cJSON_GetObjectItem(osdObj, "time_transparency");
            cJSON *uptimeTransparencyObj = cJSON_GetObjectItem(osdObj, "uptime_transparency");
            cJSON *userTextTransparencyObj = cJSON_GetObjectItem(osdObj, "usertext_transparency");

            bool hasOldFormat = fontColorObj && fontStrokeColorObj &&
                               (timeTransparencyObj || uptimeTransparencyObj || userTextTransparencyObj);

            if (hasOldFormat) {
                unsigned int fontColor = 0xFFFFFFFF;
                unsigned int fontStrokeColor = 0xFF000000;
                int timeTransparency = 255;
                int uptimeTransparency = 255;
                int userTextTransparency = 255;

                // Read old values (similar to stream0)
                if (cJSON_IsNumber(fontColorObj)) {
                    fontColor = static_cast<unsigned int>(cJSON_GetNumberValue(fontColorObj));
                } else if (cJSON_IsString(fontColorObj)) {
                    const char *str = cJSON_GetStringValue(fontColorObj);
                    if (isValidHexColor(str)) {
                        fontColor = hexColorToUint(str);
                    }
                }

                if (cJSON_IsNumber(fontStrokeColorObj)) {
                    fontStrokeColor = static_cast<unsigned int>(cJSON_GetNumberValue(fontStrokeColorObj));
                } else if (cJSON_IsString(fontStrokeColorObj)) {
                    const char *str = cJSON_GetStringValue(fontStrokeColorObj);
                    if (isValidHexColor(str)) {
                        fontStrokeColor = hexColorToUint(str);
                    }
                }

                if (timeTransparencyObj && cJSON_IsNumber(timeTransparencyObj)) {
                    timeTransparency = static_cast<int>(cJSON_GetNumberValue(timeTransparencyObj));
                }
                if (uptimeTransparencyObj && cJSON_IsNumber(uptimeTransparencyObj)) {
                    uptimeTransparency = static_cast<int>(cJSON_GetNumberValue(uptimeTransparencyObj));
                }
                if (userTextTransparencyObj && cJSON_IsNumber(userTextTransparencyObj)) {
                    userTextTransparency = static_cast<int>(cJSON_GetNumberValue(userTextTransparencyObj));
                }

                // Create new individual color settings
                cJSON_AddItemToObject(osdObj, "time_font_color",
                    cJSON_CreateNumber(combineColorWithAlpha(fontColor, timeTransparency)));
                cJSON_AddItemToObject(osdObj, "time_font_stroke_color",
                    cJSON_CreateNumber(combineColorWithAlpha(fontStrokeColor, timeTransparency)));
                cJSON_AddItemToObject(osdObj, "uptime_font_color",
                    cJSON_CreateNumber(combineColorWithAlpha(fontColor, uptimeTransparency)));
                cJSON_AddItemToObject(osdObj, "uptime_font_stroke_color",
                    cJSON_CreateNumber(combineColorWithAlpha(fontStrokeColor, uptimeTransparency)));
                cJSON_AddItemToObject(osdObj, "usertext_font_color",
                    cJSON_CreateNumber(combineColorWithAlpha(fontColor, userTextTransparency)));
                cJSON_AddItemToObject(osdObj, "usertext_font_stroke_color",
                    cJSON_CreateNumber(combineColorWithAlpha(fontStrokeColor, userTextTransparency)));

                // Remove old settings
                cJSON_DeleteItemFromObject(osdObj, "font_color");
                cJSON_DeleteItemFromObject(osdObj, "font_stroke_color");
            // Migrate position settings: pos_*_x/pos_*_y -> *_position ("x,y")
            {
                auto migrate_pos = [&](const char *new_key, const char *old_x, const char *old_y, const char *alt_old_x = nullptr, const char *alt_old_y = nullptr) {
                    if (!cJSON_GetObjectItem(osdObj, new_key)) {
                        cJSON *xObj = cJSON_GetObjectItem(osdObj, old_x);
                        cJSON *yObj = cJSON_GetObjectItem(osdObj, old_y);
                        if ((!xObj || !yObj) && (alt_old_x && alt_old_y)) {
                            xObj = cJSON_GetObjectItem(osdObj, alt_old_x);
                            yObj = cJSON_GetObjectItem(osdObj, alt_old_y);
                        }
                        if (xObj && yObj && cJSON_IsNumber(xObj) && cJSON_IsNumber(yObj)) {
                            int xi = (int)cJSON_GetNumberValue(xObj);
                            int yi = (int)cJSON_GetNumberValue(yObj);
                            char buf[64];
                            snprintf(buf, sizeof(buf), "%d,%d", xi, yi);
                            cJSON_AddItemToObject(osdObj, new_key, cJSON_CreateString(buf));
                        }
                    }
                    // Cleanup old keys regardless
                    cJSON_DeleteItemFromObject(osdObj, old_x);
                    cJSON_DeleteItemFromObject(osdObj, old_y);
                    if (alt_old_x) cJSON_DeleteItemFromObject(osdObj, alt_old_x);
                    if (alt_old_y) cJSON_DeleteItemFromObject(osdObj, alt_old_y);
                };

                migrate_pos("time_position", "pos_time_x", "pos_time_y");
                migrate_pos("uptime_position", "pos_uptime_x", "pos_uptime_y");
                migrate_pos("usertext_position", "pos_usertext_x", "pos_usertext_y", "pos_user_text_x", "pos_user_text_y");
                migrate_pos("logo_position", "pos_logo_x", "pos_logo_y");
            }

                cJSON_DeleteItemFromObject(osdObj, "time_transparency");
                cJSON_DeleteItemFromObject(osdObj, "font_yoffset");

                cJSON_DeleteItemFromObject(osdObj, "uptime_transparency");
                cJSON_DeleteItemFromObject(osdObj, "usertext_transparency");
            }
            // Migrate stroke settings: font_stroke/font_stroke_enabled -> font_stroke_size
            {
                cJSON *strokeSizeObj = cJSON_GetObjectItem(osdObj, "font_stroke_size");
                if (!strokeSizeObj) {
                    cJSON *strokeObj = cJSON_GetObjectItem(osdObj, "font_stroke");
                    cJSON *strokeEnabledObj = cJSON_GetObjectItem(osdObj, "font_stroke_enabled");
                    int strokeVal = -1;
                    if (strokeObj && cJSON_IsNumber(strokeObj)) {
                        strokeVal = (int)cJSON_GetNumberValue(strokeObj);
                    }
                    // If explicitly disabled, set size to 0
                    if (strokeEnabledObj && cJSON_IsBool(strokeEnabledObj) && !cJSON_IsTrue(strokeEnabledObj)) {
                        strokeVal = 0;
                    }
                    if (strokeVal >= 0) {
                        cJSON_AddItemToObject(osdObj, "font_stroke_size", cJSON_CreateNumber(strokeVal));
                    }
                    if (strokeObj) cJSON_DeleteItemFromObject(osdObj, "font_stroke");
                    if (strokeEnabledObj) cJSON_DeleteItemFromObject(osdObj, "font_stroke_enabled");
                } else {
                    // Clean up redundant keys if present
                    cJSON_DeleteItemFromObject(osdObj, "font_stroke");
                    cJSON_DeleteItemFromObject(osdObj, "font_stroke_enabled");
                }
            }

        }
    }
}

CFG::CFG()
{
    load();
}

void CFG::load()
{
    boolItems = getBoolItems();
    charItems = getCharItems();
    intItems = getIntItems();
    uintItems = getUintItems();
    floatItems = getFloatItems();

    config_loaded = readConfig();

    if (jsonConfig) {
        // Handle backward compatibility migration first
        migrateOldColorSettings();

        for (auto &item : boolItems)
            handleConfigItem(jsonConfig, item);
        for (auto &item : charItems)
            handleConfigItem(jsonConfig, item);
        for (auto &item : intItems)
            handleConfigItem(jsonConfig, item);
        for (auto &item : uintItems)
            handleConfigItem(jsonConfig, item);
        for (auto &item : floatItems)
            handleConfigItem(jsonConfig, item);
    }

    if (stream2.jpeg_channel == 0)
    {
        stream2.width = stream0.width;
        stream2.height = stream0.height;
    }
    else
    {
        stream2.width = stream1.width;
        stream2.height = stream1.height;
    }

    // Handle ROIs from JSON
    if (jsonConfig) {
        cJSON *roisObj = cJSON_GetObjectItem(jsonConfig, "rois");
        if (roisObj && cJSON_IsObject(roisObj))
        {
            for (int i = 0; i < motion.roi_count; i++)
            {
                std::string roiKey = "roi_" + std::to_string(i);
                cJSON *roiArray = cJSON_GetObjectItem(roisObj, roiKey.c_str());
                if (roiArray && cJSON_IsArray(roiArray))
                {
                    int arrayLen = cJSON_GetArraySize(roiArray);
                    if (arrayLen == 4)
                    {
                        cJSON *item0 = cJSON_GetArrayItem(roiArray, 0);
                        cJSON *item1 = cJSON_GetArrayItem(roiArray, 1);
                        cJSON *item2 = cJSON_GetArrayItem(roiArray, 2);
                        cJSON *item3 = cJSON_GetArrayItem(roiArray, 3);

                        if (item0 && cJSON_IsNumber(item0)) motion.rois[i].p0_x = static_cast<int>(cJSON_GetNumberValue(item0));
                        if (item1 && cJSON_IsNumber(item1)) motion.rois[i].p0_y = static_cast<int>(cJSON_GetNumberValue(item1));
                        if (item2 && cJSON_IsNumber(item2)) motion.rois[i].p1_x = static_cast<int>(cJSON_GetNumberValue(item2));
                        if (item3 && cJSON_IsNumber(item3)) motion.rois[i].p1_y = static_cast<int>(cJSON_GetNumberValue(item3));
                    }
                }
            }
        }
    }
}
