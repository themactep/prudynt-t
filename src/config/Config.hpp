#pragma once

#include <any>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <json_config.h>
#include <memory>
#include <mutex>
#include <set>
#include <sys/time.h>
#include <vector>

//~65k
#define ENABLE_LOG_DEBUG

// Some more debug output not useful for users (Developer Debug)
// #define DDEBUG

// under development
// #define USE_STEREO_SIMULATOR

// enable audio processing library
#define LIB_AUDIO_PROCESSING
#define USE_AUDIO_STREAM_REPLICATOR

// disable tunings (debugging)
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
#define NO_TUNINGS
#endif

#define IMP_AUTO_VALUE 16384
#define OSD_AUTO_VALUE 16384
#define IVS_AUTO_VALUE 16384

#define THREAD_SLEEP_US 100000
#define GET_STREAM_BLOCKING false

struct roi {
  int p0_x;
  int p0_y;
  int p1_x;
  int p1_y;
};

template <typename T> struct ConfigItem {
  const char *path;
  T &value;
  T defaultValue;
  std::function<bool(const T &)> validate;
  bool noSave = false;
  const char *procPath = nullptr;
};

struct _stream_stats { // has to be before _osd
  uint32_t bps;
  uint8_t fps;
  struct timeval ts;
};
struct _audio {
  // All Ingenic SoCs support 48kHz natively --- capture rate is fixed.
  int mic_sample_rate() const { return mic_hq ? 48000 : 16000; }

  // Default encoding bitrate for AAC/Opus (kbps).
  int mic_bitrate_kbps() const { return mic_hq ? 128 : 32; }
  // Speaker/playback AAC bitrate (kbps).
  static constexpr int kSpkBitrateKbps = 48;

  bool input_enabled;
  const char *input_format;
  int input_vol;
  int input_gain;
  bool tap_enabled;
  const char *tap_path;
  bool mic_is_digital;
  bool mic_hq;
#if defined(LIB_AUDIO_PROCESSING)
  int input_alc_gain;
  int input_noise_suppression;
  bool input_high_pass_filter;
  bool input_agc_enabled;
  int input_agc_target_level_dbfs;
  int input_agc_compression_gain_db;
  bool force_stereo;
  bool output_enabled;
  int output_sample_rate = 48000;
  int output_vol;
  int output_gain;
#endif

};
struct _daynight_controls {
  bool binswitch{true};
  bool color{true};
  bool ircut{true};
  bool ir850{true};
  bool ir940{true};
  bool white{false};
};
struct _daynight_schedule {
  bool enabled{false};
  const char *start_at{nullptr};
  const char *stop_at{nullptr};
};
struct _daynight {
  // User-configurable knobs
  bool enabled{true};
  // Hardware control toggles
  _daynight_controls controls;

  // Time-based schedule
  _daynight_schedule schedule;

  const char *loglevel{nullptr};

  // Expert overrides
  int sample_interval_ms{1000};
  int total_gain_night_threshold{3000};
  int total_gain_day_threshold{300};
  int night_count_threshold{6};
  int day_count_threshold{4};
  const char *script_path{nullptr};

  // IQ bin file paths for day/night modes
  // Manual mode override (set by user via JSON API)
  std::atomic<const char *> force_mode{nullptr};

  /* Live telemetry moved to daynightd --- see /run/thingino/daynight_sensors */
};
struct _general {
  const char *loglevel;
  const char *debug_dump_path;
  int osd_pool_size;
  int imp_polling_timeout_ms;
  bool timestamp_validation_enabled;
  bool audio_debug_verbose;
};
struct _http {
  bool enabled;
  bool mjpeg_enabled;
  bool api_enabled;
  int port;
  bool auth_required;
  const char *username;
  const char *password;
};
struct _image {
  int contrast;
  int sharpness;
  int saturation;
  int brightness;
  int hue;
  int sinter_strength;
  int temper_strength;
  bool isp_bypass;
  bool vflip;
  bool hflip;
  int running_mode;
  int anti_flicker;
  int ae_compensation;
  int dpc_strength;
  int defog_strength;
  int drc_strength;
  int highlight_depress;
  int backlight_compensation;
  int max_again;
  int max_dgain;
  int core_wb_mode;
  int wb_rgain;
  int wb_bgain;
};
struct _motion {
  int monitor_stream;
  int debounce_time_s;
  int post_time_s;
  int cooldown_time_s;
  int motor_settle_ms;
  int init_time_s;
  int min_time_s;
  int ivs_polling_timeout_ms;
  int sensitivity;
  int skip_frame_count;
  int frame_width;
  int frame_height;
  int roi_0_x;
  int roi_0_y;
  int roi_1_x;
  int roi_1_y;
  int roi_count;
  bool enabled;
  const char *script_path;
  std::array<roi, 52> rois;
};
struct _privacy {
  bool enabled;
  bool save_state;
};

constexpr int kBurninMaxScale = 10;

struct _osd {
  _stream_stats stats;
  std::atomic<int> thread_signal;
  struct {
    bool enabled;
  } sei;
  struct {
    bool enabled;
    const char *format;
    const char *background_color;
    int scale;
    const char *fill_color;
    const char *outline_color;
  } burnin;
};
struct _recorder {
  bool enabled;
  const char *mount;
  const char *device_path;
  const char *filename;
  int duration_s;
  int channel;
#ifdef PREBUFFER_ENABLED
  bool prebuffer_enabled;
  int prebuffer_seconds;
  bool prebuffer_keyframe_only;
  int prebuffer_max_memory_mb;
#endif
};
struct _rtsp {
  int port;
  int est_bitrate;
  int out_buffer_size;
  int send_buffer_size;
  int send_timeout_s;
  int session_reclaim;
  bool auth_required;
  const char *username;
  const char *password;
  const char *name;
  float packet_loss_threshold;
  float bandwidth_margin;
  bool audio_only_enabled;
  const char *audio_only_endpoint;
  const char *audio_only_info;
};
struct _sensor {
  int fps;
  int width;
  int height;
  const char *model;
  unsigned int i2c_address;
  int boot;
  int mclk;
  int i2c_bus;
  int video_interface;
  int gpio_reset;
  const char *chip_id;
  const char *version;
  int min_fps;
  int actual_fps;
};
struct _stream {
  int gop;
  int max_gop;
  int fps;
  int buffers;
  int width;
  int height;
  int profile;
  int bitrate;
  int rotation;
  bool enabled;
  bool power_saving;
  bool allow_shared;
  const char *mode;
  // Advanced RC parameters; -1/0 keeps SDK defaults
  int qp_init{-1};
  int qp_min{-1};
  int qp_max{-1};
  int ip_delta{-1};
  int pb_delta{-1};
  int max_bitrate{0};
  const char *rtsp_endpoint;
  const char *rtsp_info;
  const char *format{"JPEG"};
  /* JPEG stream*/
  int jpeg_quality;
  int jpeg_refresh;
  int jpeg_channel;
  int jpeg_idle_fps;
  const char *jpeg_path;
  _stream_stats stats;
  bool audio_enabled;
  bool video_enabled;
};
struct _sysinfo {
  const char *cpu = nullptr;
};
#if defined(WEBSOCKET_ENABLED)
struct _websocket {
  bool enabled;
  bool ws_secured;
  bool http_secured;
  int port;
  int first_image_delay_ms;
  const char *name;
  const char *token{"auto"};
};
#endif

class CFG {
public:
  // Destructor to clean up JSON object
  ~CFG() {
    if (jsonConfig) {
      free_json_value(jsonConfig);
      jsonConfig = nullptr;
    }
  }

  bool config_loaded = false;
  bool config_corrupted = false;
  JsonValue *jsonConfig = nullptr;
  std::string filePath{};
  mutable std::mutex configMutex;

  CFG();
  void load();
  static CFG *createNew();
  bool readConfig();
  bool updateConfig();
  bool saveIntValues(const std::vector<std::pair<std::string, int>> &values);

  _audio audio{};
  _general general{};
  _rtsp rtsp{};
  _sensor sensor{};
  _image image{};
  _osd osd{};
  _stream stream0{};
  _stream stream1{};
  _stream stream2{};
  _stream stream3{};
  _daynight daynight{};
  _motion motion{};
  _privacy privacy{};
#if defined(WEBSOCKET_ENABLED)
  _websocket websocket{};
#endif
  _http http{};
  _sysinfo sysinfo{};
  _recorder recorder{};

  template <typename T> T get(const std::string &name) {
    T result = T{};
    std::vector<ConfigItem<T>> *items = nullptr;
    if constexpr (std::is_same_v<T, bool>) {
      items = &boolItems;
    } else if constexpr (std::is_same_v<T, const char *>) {
      items = &charItems;
    } else if constexpr (std::is_same_v<T, int>) {
      items = &intItems;
    } else if constexpr (std::is_same_v<T, unsigned int>) {
      items = &uintItems;
    } else if constexpr (std::is_same_v<T, float>) {
      items = &floatItems;
    } else {
      return result;
    }
    for (auto &item : *items) {
      if (item.path == name) {
        return item.value;
      }
    }
    return result;
  }

  template <typename T>
  bool set(const std::string &name, T value, bool noSave = false) {
    // std::cout << name << "=" << value << std::endl;
    std::vector<ConfigItem<T>> *items = nullptr;
    if constexpr (std::is_same_v<T, bool>) {
      items = &boolItems;
    } else if constexpr (std::is_same_v<T, const char *>) {
      items = &charItems;
    } else if constexpr (std::is_same_v<T, int>) {
      items = &intItems;
    } else if constexpr (std::is_same_v<T, unsigned int>) {
      items = &uintItems;
    } else if constexpr (std::is_same_v<T, float>) {
      items = &floatItems;
    } else {
      return false;
    }
    for (auto &item : *items) {
      if (item.path == name) {
        if (item.validate(value)) {
          if constexpr (std::is_same_v<T, const char *>) {
            if (item.value) free((void *)item.value);
            item.value = value ? strdup(value) : nullptr;
          } else {
            item.value = value;
          }
          item.noSave = noSave;
          // Keep jsonConfig in sync so GET /api/v1/config/* returns
          // current values without requiring a config file reload.
          if (jsonConfig) {
            std::string valueStr;
            if constexpr (std::is_same_v<T, const char *>) {
              valueStr = item.value ? std::string(item.value) : "";
            } else if constexpr (std::is_same_v<T, bool>) {
              valueStr = item.value ? "true" : "false";
            } else if constexpr (std::is_same_v<T, int>) {
              valueStr = std::to_string(item.value);
            } else if constexpr (std::is_same_v<T, unsigned int>) {
              valueStr = std::to_string(item.value);
            } else if constexpr (std::is_same_v<T, float>) {
              valueStr = std::to_string(item.value);
            }
            set_nested_item(jsonConfig, item.path, valueStr.c_str());
          }
          return true;
        } else {
          return false;
        }
      }
    }
    return false;
  }

private:
  std::vector<ConfigItem<bool>> boolItems{};
  std::vector<ConfigItem<const char *>> charItems{};
  std::vector<ConfigItem<int>> intItems{};
  std::vector<ConfigItem<unsigned int>> uintItems{};
  std::vector<ConfigItem<float>> floatItems{};

  std::vector<ConfigItem<bool>> getBoolItems();
  std::vector<ConfigItem<const char *>> getCharItems();
  std::vector<ConfigItem<int>> getIntItems();
  std::vector<ConfigItem<unsigned int>> getUintItems();
  std::vector<ConfigItem<float>> getFloatItems();
};

// The configuration is kept in a global singleton that's accessed via this
// shared_ptr.
extern std::shared_ptr<CFG> cfg;
