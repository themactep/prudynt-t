#pragma once

#include <set>
#include <atomic>
#include <chrono>
#include <iostream>
#include <functional>
#include <json_config.h>
#include <sys/time.h>
#include <any>
#include <mutex>

//~65k
#define ENABLE_LOG_DEBUG

//Some more debug output not usefull for users (Developer Debug)
//#define DDEBUG
//#define DDEBUGWS

// under development
//#define USE_STEREO_SIMULATOR

//enable audio support
#define AUDIO_SUPPORT
//enable audio processing library
#define LIB_AUDIO_PROCESSING
#define USE_AUDIO_STREAM_REPLICATOR

//disable tunings (debugging)
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

struct roi{
    int p0_x;
    int p0_y;
    int p1_x;
    int p1_y;
};

template<typename T>
struct ConfigItem {
    const char *path;
    T& value;
    T defaultValue;
    std::function<bool(const T&)> validate;
    bool noSave = false;
    const char *procPath = nullptr;
};

struct _stream_stats {
    uint32_t bps;
	uint8_t fps;
	struct timeval ts;
};

struct _regions {
    int time;
    int user;
    int uptime;
    int logo;
};
struct _general {
    const char *loglevel;
    int osd_pool_size;
    int imp_polling_timeout;
    bool timestamp_validation_enabled;
    bool audio_debug_verbose;
};
struct _rtsp {
    int port;
    int est_bitrate;
    int out_buffer_size;
    int send_buffer_size;
    int session_reclaim;;
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
#if defined(AUDIO_SUPPORT)
struct _audio {
    bool input_enabled;
    const char *input_format;
    int input_vol;
    int input_bitrate;
    int input_gain;
    int input_sample_rate;
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
#endif
    // Buffer tuning (in 20 ms frames per channel)
    int buffer_warn_frames;
    int buffer_cap_frames;
};
#endif
struct _osd {
    int font_size;
    int font_stroke_size;
    int logo_height;
    int logo_width;
    const char *time_position;
    int time_rotation;
    const char *usertext_position;
    int usertext_rotation;
    const char *uptime_position;
    int uptime_rotation;
    const char *logo_position;
    int logo_transparency;
    int logo_rotation;
    int start_delay;
    bool enabled;
    bool time_enabled;
    bool usertext_enabled;
    bool uptime_enabled;
    bool logo_enabled;
    const char *font_path;
    const char *time_format;
    const char *uptime_format;
    const char *usertext_format;
    const char *logo_path;
    // Individual color settings for each text element
    unsigned int time_font_color;
    unsigned int time_font_stroke_color;
    unsigned int uptime_font_color;
    unsigned int uptime_font_stroke_color;
    unsigned int usertext_font_color;
    unsigned int usertext_font_stroke_color;
    _regions regions;
    _stream_stats stats;
    std::atomic<int> thread_signal;
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
    // Advanced RC parameters (optional; -1/0 = use encoder defaults)
    int qp_init;
    int qp_min;
    int qp_max;
    int ip_delta;
    int pb_delta;
    int max_bitrate;
    const char *rtsp_endpoint;
    const char *rtsp_info;
    const char *format{"JPEG"};
    /* JPEG stream*/
    int jpeg_quality;
    int jpeg_channel;
    int jpeg_idle_fps;
    _osd osd;
    _stream_stats stats;
#if defined(AUDIO_SUPPORT)
    bool audio_enabled;
#endif
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
struct _websocket {
    bool enabled;
    bool ws_secured;
    bool http_secured;
    int port;
    int first_image_delay;
    const char *name;
    const char *token{"auto"};
};
struct _sysinfo {
    const char *cpu = nullptr;
};

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

#if defined(AUDIO_SUPPORT)
        _audio audio{};
#endif
		_general general{};
		_rtsp rtsp{};
		_sensor sensor{};
        _image image{};
		_stream stream0{};
        _stream stream1{};
		_stream stream2{};
		_motion motion{};
        _websocket websocket{};
        _sysinfo sysinfo{};

    template <typename T>
    T get(const std::string &name) {
        T result = T{};
        std::vector<ConfigItem<T>> *items = nullptr;
        if constexpr (std::is_same_v<T, bool>) {
            items = &boolItems;
        } else if constexpr (std::is_same_v<T, const char*>) {
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
        //std::cout << name << "=" << value << std::endl;
        std::vector<ConfigItem<T>> *items = nullptr;
        if constexpr (std::is_same_v<T, bool>) {
            items = &boolItems;
        } else if constexpr (std::is_same_v<T, const char*>) {
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
        std::vector<ConfigItem<const char *>> getCharItems() ;
        std::vector<ConfigItem<int>> getIntItems();
        std::vector<ConfigItem<unsigned int>> getUintItems();
        std::vector<ConfigItem<float>> getFloatItems();
        void migrateOldColorSettings();
};

// The configuration is kept in a global singleton that's accessed via this
// shared_ptr.
extern std::shared_ptr<CFG> cfg;
