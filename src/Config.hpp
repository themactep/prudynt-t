#pragma once

#include <any>
#include <atomic>
#include <chrono>
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

#define THREAD_SLEEP 100000
#define GET_STREAM_BLOCKING false

#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
#define DEFAULT_ENC_MODE_0 "FIXQP"
#define DEFAULT_ENC_MODE_1 "CAPPED_QUALITY"
#define DEFAULT_BUFFERS_0 4
#define DEFAULT_BUFFERS_1 2
#define DEFAULT_SINTER 128
#define DEFAULT_TEMPER 128
#define DEFAULT_SINTER_VALIDATE validateInt255
#define DEFAULT_TEMPER_VALIDATE validateInt255
#elif defined(PLATFORM_T23)
#define DEFAULT_ENC_MODE_0 "SMART"
#define DEFAULT_ENC_MODE_1 "SMART"
#define DEFAULT_BUFFERS_0 2
#define DEFAULT_BUFFERS_1 2
#define DEFAULT_SINTER 128
#define DEFAULT_TEMPER 128
#define DEFAULT_SINTER_VALIDATE validateInt255
#define DEFAULT_TEMPER_VALIDATE validateInt255
#else
#define DEFAULT_ENC_MODE_0 "SMART"
#define DEFAULT_ENC_MODE_1 "SMART"
#define DEFAULT_BUFFERS_0 2
#define DEFAULT_BUFFERS_1 2
#define DEFAULT_SINTER 50
#define DEFAULT_TEMPER 50
#define DEFAULT_SINTER_VALIDATE validateInt50_150
#define DEFAULT_TEMPER_VALIDATE validateInt50_150
#endif

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

struct _osd_privacy { // has to be before _osd
  bool enabled;
  const char *text;
  const char *position;
  int rotation;
  int font_size;
  int stroke_size;
  unsigned int fill_color;
  unsigned int stroke_color;
  const char *image_path;
  int image_width;
  int image_height;
  int layer;
  int opacity;
};

struct _regions { // has to be before _osd
  int time;
  int user;
  int uptime;
  int logo;
  int brightness;
};

struct _stream_stats { // has to be before _osd
  uint32_t bps;
  uint8_t fps;
  struct timeval ts;
};

struct _audio {
  bool input_enabled;
  const char *input_format;
  int input_vol;
  int input_bitrate;
  int input_gain;
  int input_sample_rate;
  bool tap_enabled;
  const char *tap_path;
  bool mic_is_digital;
#if defined(LIB_AUDIO_PROCESSING)
  int input_alc_gain;
  int input_noise_suppression;
  bool input_high_pass_filter;
  bool input_agc_enabled;
  int input_agc_target_level_dbfs;
  int input_agc_compression_gain_db;
  bool force_stereo;
  bool output_enabled;
  int output_sample_rate;
  int output_vol;
  int output_gain;
#endif
  // Buffer tuning (in 20 ms frames per channel)
  int buffer_warn_frames;
  int buffer_cap_frames;
};
struct _daynight {
  // User-configurable knobs
  bool enabled{true};
  int switch_below_percent{15};
  int switch_above_percent{80};
  int tolerance_percent{50};

  // Optional expert overrides
  int sample_interval_ms{1000};
  int ev_night_high{1900000};
  int ev_day_low_primary{479832};
  int ev_day_low_secondary{361880};
  int gb_gain_delta{15};
  int gb_gain_absolute{145};
  int night_count_threshold{6};
  int day_count_threshold{4};
  int settle_samples_for_gb_record{20};
  const char *script_path{nullptr};

  // Live telemetry populated by the worker
  std::atomic<int> live_brightness_percent{-1};
  std::atomic<int> live_ev{-1};
  std::atomic<int> live_gb{-1};
  std::atomic<int> live_gr{-1};
  std::atomic<const char *> live_mode{"unknown"};
};
struct _general {
  const char *loglevel;
  int osd_pool_size;
  int imp_polling_timeout;
  bool timestamp_validation_enabled;
  bool audio_debug_verbose;
};
struct _http_mjpeg {
  bool enabled;
  int port;
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
  int debounce_time;
  int post_time;
  int cooldown_time;
  int init_time;
  int min_time;
  int ivs_polling_timeout;
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
struct _osd {
  int font_size;
  int stroke_size;
  int logo_height;
  int logo_width;
  const char *time_position;
  int time_rotation;
  const char *usertext_position;
  int usertext_rotation;
  const char *uptime_position;
  int uptime_rotation;
  const char *logo_position;
  const char *brightness_position;
  int logo_transparency;
  int logo_rotation;
  int brightness_rotation;
  int start_delay;
  bool enabled;
  bool time_enabled;
  bool usertext_enabled;
  bool uptime_enabled;
  bool logo_enabled;
  bool brightness_enabled;
  const char *font_path;
  const char *time_format;
  const char *uptime_format;
  const char *usertext_format;
  const char *brightness_format;
  const char *logo_path;
  // Individual color settings for each text element
  unsigned int time_fill_color;
  unsigned int time_stroke_color;
  unsigned int uptime_fill_color;
  unsigned int uptime_stroke_color;
  unsigned int usertext_fill_color;
  unsigned int usertext_stroke_color;
  unsigned int brightness_fill_color;
  unsigned int brightness_stroke_color;
  _regions regions;
  _stream_stats stats;
  std::atomic<int> thread_signal;
  _osd_privacy privacy;
};
struct _recorder {
  bool enabled;
  const char *mount;
  const char *device_path;
  const char *filename;
  int duration;
  int channel;
};
struct _rtsp {
  int port;
  int est_bitrate;
  int out_buffer_size;
  int send_buffer_size;
  int session_reclaim;
  ;
  bool auth_required;
  const char *username;
  const char *password;
  const char *name;
  float packet_loss_threshold;
  float bandwidth_margin;
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
  int scale_width;
  int scale_height;
  bool enabled;
  bool scale_enabled;
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
  _osd osd;
  _stream_stats stats;
  bool audio_enabled;
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
  int first_image_delay;
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
  _stream stream0{};
  _stream stream1{};
  _stream stream2{};
  _daynight daynight{};
  _motion motion{};
#if defined(WEBSOCKET_ENABLED)
  _websocket websocket{};
#endif
  _http_mjpeg http_mjpeg{};
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

  template <typename T> bool set(const std::string &name, T value, bool noSave = false) {
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
          item.value = value;
          item.noSave = noSave;
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
