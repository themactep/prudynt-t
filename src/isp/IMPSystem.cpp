#include "isp/IMPSystem.hpp"
#include "config/Config.hpp"
#include "isp/imp_hal.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <unistd.h>

#define MODULE "IMP_SYSTEM"

namespace {

bool read_int_from_file(const char *path, int &value_out) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return false;
  }
  int value = 0;
  file >> value;
  if (file.fail()) {
    return false;
  }
  value_out = value;
  return true;
}

// Wait for the sensor's procfs properties (resolution and max_fps) to become
// available after enable_sensor().  max_fps is required because it is the
// physical upper limit that every stream fps must be clamped against; until the
// driver has published it the value read from procfs is 0 and the stream fps
// would silently keep its configured (possibly bogus) value.
bool wait_for_sensor_resolution(int max_retries = 20, int delay_ms = 50) {
  constexpr const char *kSensorWidthPath = "/proc/jz/sensor/width";
  constexpr const char *kSensorHeightPath = "/proc/jz/sensor/height";
  constexpr const char *kSensorMaxFpsPath = "/proc/jz/sensor/max_fps";

  for (int i = 0; i < max_retries; ++i) {
    int width = 0, height = 0, max_fps = 0;
    bool have_res = read_int_from_file(kSensorWidthPath, width) &&
                    read_int_from_file(kSensorHeightPath, height) &&
                    width > 0 && height > 0;
    bool have_fps = read_int_from_file(kSensorMaxFpsPath, max_fps) &&
                    max_fps > 0;
    if (have_res && have_fps) {
      LOG_DEBUG("Sensor procfs ready: " << width << "x" << height << " @ "
                                         << max_fps << " fps (attempt "
                                         << (i + 1) << ")");
      return true;
    }

    if (i < max_retries - 1) {
      LOG_DEBUG("Waiting for sensor procfs to be available (attempt "
                << (i + 1) << "/" << max_retries << "; res=" << have_res
                << " fps=" << have_fps << ")");
      usleep(delay_ms * 1000); // usleep takes microseconds
    }
  }

  LOG_ERROR("Sensor procfs (resolution and max_fps) not detected after "
            << max_retries << " attempts");
  return false;
}

void refresh_sensor_properties_from_proc() {
  if (!cfg) {
    return;
  }

  constexpr const char *kSensorWidthPath = "/proc/jz/sensor/width";
  constexpr const char *kSensorHeightPath = "/proc/jz/sensor/height";
  constexpr const char *kSensorMaxFpsPath = "/proc/jz/sensor/max_fps";
  constexpr const char *kSensorMinFpsPath = "/proc/jz/sensor/min_fps";

  int proc_width = 0;
  int proc_height = 0;
  int proc_max_fps = 0;
  int proc_min_fps = 0;
  bool width_ok = read_int_from_file(kSensorWidthPath, proc_width);
  bool height_ok = read_int_from_file(kSensorHeightPath, proc_height);
  bool max_fps_ok = read_int_from_file(kSensorMaxFpsPath, proc_max_fps);
  bool min_fps_ok = read_int_from_file(kSensorMinFpsPath, proc_min_fps);

  if (width_ok && proc_width > 0) {
    if (cfg->sensor.width != proc_width) {
      LOG_INFO("Sensor width updated from procfs: " << cfg->sensor.width
                                                    << " -> " << proc_width);
    }
    cfg->sensor.width = proc_width;
  }

  if (height_ok && proc_height > 0) {
    if (cfg->sensor.height != proc_height) {
      LOG_INFO("Sensor height updated from procfs: " << cfg->sensor.height
                                                     << " -> " << proc_height);
    }
    cfg->sensor.height = proc_height;
  }

  if (max_fps_ok && proc_max_fps > 0) {
    if (cfg->sensor.fps != proc_max_fps) {
      LOG_INFO("Sensor max_fps updated from procfs: "
               << cfg->sensor.fps << " -> " << proc_max_fps);
    }
    cfg->sensor.fps = proc_max_fps;
  }

  if (min_fps_ok && proc_min_fps > 0) {
    if (cfg->sensor.min_fps != proc_min_fps) {
      LOG_INFO("Sensor min_fps updated from procfs: "
               << cfg->sensor.min_fps << " -> " << proc_min_fps);
    }
    cfg->sensor.min_fps = proc_min_fps;
  }

  if (cfg->sensor.fps > 0 && cfg->sensor.min_fps > 0 &&
      cfg->sensor.fps < cfg->sensor.min_fps) {
    LOG_WARN("Sensor max_fps " << cfg->sensor.fps << " is below min_fps "
                               << cfg->sensor.min_fps
                               << ", clamping to min_fps");
    cfg->sensor.fps = cfg->sensor.min_fps;
  }

  if ((width_ok || height_ok || max_fps_ok || min_fps_ok) &&
      cfg->sensor.width > 0 && cfg->sensor.height > 0) {
    LOG_INFO("Sensor procfs geometry "
             << cfg->sensor.width << "x" << cfg->sensor.height << " @ "
             << cfg->sensor.min_fps << "-" << cfg->sensor.fps << " fps");
  }
}

void resolve_stream_geometry(const char *stream_name, _stream &stream,
                            int def_width, int def_height) {
  if (!cfg) {
    return;
  }

  const int sensor_width = cfg->sensor.width;
  const int sensor_height = cfg->sensor.height;
  const int sensor_max_fps = cfg->sensor.fps;
  const int sensor_min_fps = cfg->sensor.min_fps;

  auto log_change = [&](const char *what, int old_val, int new_val) {
    LOG_INFO(stream_name << ": " << what << " clamped from " << old_val
                         << " to " << new_val);
  };

  if (sensor_width > 0) {
    if (stream.width <= 0) {
      int w = def_width > 0 ? def_width : sensor_width;
      if (w > sensor_width)
        w = sensor_width;
      log_change("width", stream.width, w);
      stream.width = w;
    } else if (stream.width > sensor_width) {
      log_change("width", stream.width, sensor_width);
      stream.width = sensor_width;
    }
  }

  if (sensor_height > 0) {
    if (stream.height <= 0) {
      int h = def_height > 0 ? def_height : sensor_height;
      if (h > sensor_height)
        h = sensor_height;
      log_change("height", stream.height, h);
      stream.height = h;
    } else if (stream.height > sensor_height) {
      log_change("height", stream.height, sensor_height);
      stream.height = sensor_height;
    }
  }

  if (sensor_max_fps > 0) {
    if (stream.fps <= 0) {
      log_change("fps", stream.fps, sensor_max_fps);
      stream.fps = sensor_max_fps;
    } else if (stream.fps > sensor_max_fps) {
      log_change("fps", stream.fps, sensor_max_fps);
      stream.fps = sensor_max_fps;
    }
  }

  if (sensor_min_fps > 0 && stream.fps > 0 && stream.fps < sensor_min_fps) {
    log_change("fps", stream.fps, sensor_min_fps);
    stream.fps = sensor_min_fps;
  }

  // T31 encoder hardware is limited to ~26fps at 1080p regardless
  // of RAM size.  Capping to 25 avoids DMA failures and frame-rate
  // collapse that occurs above this threshold.
#if defined(PLATFORM_T31)
  if (stream.width * stream.height >= 1920 * 1080 && stream.fps > 25) {
    LOG_INFO(stream_name << ": fps capped from " << stream.fps
             << " to 25 (T31 1080p encoder limit)");
    stream.fps = 25;
  }
#endif
}

// Single source of truth for stream dimensions: each stream falls back
// to its per-stream default when unset, clamped to the sensor. Runs after
// the sensor geometry is known (IMPSystem::init), before the encoders are
// created.
void resolve_all_stream_geometry() {
  if (!cfg) {
    return;
  }

  resolve_stream_geometry("stream0", cfg->stream0, 0, 0);
  resolve_stream_geometry("stream1", cfg->stream1, 640, 360);
  resolve_stream_geometry("stream2", cfg->stream2, 640, 360);
  resolve_stream_geometry("stream3", cfg->stream3, 640, 360);

  // JPEG idle rate default: keep one frame per second for the web UI
  // thumbnail when no preview/snapshot client is connected.
  if (cfg->stream2.jpeg_idle_fps <= 0) {
    int replacement = cfg->sensor.min_fps > 0 ? cfg->sensor.min_fps : 1;
    replacement = std::clamp(replacement, 1, 30);
    LOG_INFO("stream2: jpeg_idle_fps adjusted from "
             << cfg->stream2.jpeg_idle_fps << " to " << replacement);
    cfg->stream2.jpeg_idle_fps = replacement;
  }
}

// Streams with bitrate 0 (= auto) get a default derived from the encoded
// resolution: roughly 1 Mbps per megapixel, rounded to the nearest 100 kbps.
// For the main stream the encoded size defaults to the sensor geometry, so
// this is a sane starting point for any sensor; a substream scales with its
// own size (640x360 -> ~200 kbps). An explicit bitrate in prudynt.json wins;
// setting it back to 0 re-enables the automatic value. This must run after
// the stream sizes are resolved (resolve_all_stream_geometry()) but
// before the encoders are created, so it is called from IMPSystem::init()
// right alongside the clamping.
static void apply_default_bitrate(const char *stream_name, _stream &stream,
                                  int fallback_kbps) {
  if (stream.bitrate != 0)
    return; // explicit user value

  int kbps = 0;
  if (stream.width > 0 && stream.height > 0) {
    kbps = ((stream.width * stream.height + 50000) / 100000) * 100;
    LOG_INFO(stream_name << ": bitrate auto from " << stream.width << "x"
                         << stream.height << " -> " << kbps << " kbps");
  } else {
    LOG_WARN(stream_name << ": bitrate auto requested but stream size "
                            "unknown, using "
                         << fallback_kbps << " kbps");
    kbps = fallback_kbps;
  }
  stream.bitrate = kbps;
}

void apply_default_bitrates() {
  if (!cfg) {
    return;
  }

  apply_default_bitrate("stream0", cfg->stream0, 3000);
  apply_default_bitrate("stream1", cfg->stream1, 1000);
}

int add_sensor_with_retry(IMPSensorInfo &sensor_info) {
  int ret = hal::isp::add_sensor(&sensor_info);
#if defined(PLATFORM_T23)
  if (ret != 0) {
    LOG_WARN("IMPSystem init: add_sensor failed on first attempt; trying "
             "targeted ISP sensor cleanup and retry");

    int cleanup_ret = hal::isp::disable_sensor();
    LOG_DEBUG_OR_ERROR(
        cleanup_ret,
        "hal::isp::disable_sensor() cleanup before add_sensor retry");

    cleanup_ret = hal::isp::del_sensor(&sensor_info);
    LOG_DEBUG_OR_ERROR(
        cleanup_ret,
        "hal::isp::del_sensor(&sinfo) cleanup before add_sensor retry");

    cleanup_ret = IMP_ISP_Close();
    LOG_DEBUG_OR_ERROR(cleanup_ret,
                       "IMP_ISP_Close() cleanup before add_sensor retry");

    cleanup_ret = IMP_ISP_Open();
    LOG_DEBUG_OR_ERROR(cleanup_ret,
                       "IMP_ISP_Open() retry before add_sensor retry");
    if (cleanup_ret == 0) {
      ret = hal::isp::add_sensor(&sensor_info);
      if (ret == 0) {
        LOG_WARN("IMPSystem init: add_sensor retry succeeded after targeted "
                 "cleanup");
      }
    } else {
      ret = cleanup_ret;
    }
  }
#endif
  return ret;
}

#if defined(PLATFORM_T23)
void cleanup_stale_t23_isp_state(IMPSensorInfo &sensor_info) {
  LOG_WARN("IMPSystem init: running preemptive T23 stale ISP cleanup before "
           "sensor attach");

  int ret = 0;

  ret = hal::isp::disable_sensor();
  LOG_DEBUG_OR_ERROR(
      ret, "hal::isp::disable_sensor() preemptive cleanup before add_sensor");

  ret = hal::isp::del_sensor(&sensor_info);
  LOG_DEBUG_OR_ERROR(
      ret, "hal::isp::del_sensor(&sinfo) preemptive cleanup before add_sensor");

  ret = IMP_ISP_Close();
  LOG_DEBUG_OR_ERROR(ret,
                     "IMP_ISP_Close() preemptive cleanup before add_sensor");

  ret = IMP_ISP_Open();
  LOG_DEBUG_OR_ERROR(
      ret, "IMP_ISP_Open() preemptive cleanup before add_sensor retry");

  LOG_WARN("IMPSystem init: completed preemptive T23 stale ISP cleanup before "
           "sensor attach");
}
#endif

} // namespace

IMPSensorInfo IMPSystem::create_sensor_info(const char *sensor_name) {
  IMPSensorInfo out;
  memset(&out, 0, sizeof(IMPSensorInfo));
  const char *resolved_sensor = sensor_name;
  std::string fallback_sensor;
  if (!resolved_sensor || resolved_sensor[0] == '\0' ||
      strcmp(resolved_sensor, "unknown") == 0) {
    std::ifstream sensor_name_file("/proc/jz/sensor/name");
    if (sensor_name_file) {
      std::getline(sensor_name_file, fallback_sensor);
      if (!fallback_sensor.empty()) {
        resolved_sensor = fallback_sensor.c_str();
      }
    }
  }
  if (!resolved_sensor || resolved_sensor[0] == '\0') {
    resolved_sensor = "unknown";
  }

  LOG_INFO("Sensor: " << resolved_sensor);
  strcpy(out.name, resolved_sensor);
  out.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
  strcpy(out.i2c.type, resolved_sensor);
  out.i2c.addr = cfg->sensor.i2c_address;
  out.i2c.i2c_adapter_id = cfg->sensor.i2c_bus;
  out.rst_gpio = cfg->sensor.gpio_reset;
  out.pwdn_gpio = static_cast<unsigned short>(-1);
  out.power_gpio = static_cast<unsigned short>(-1);

#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  // Additional fields required for T40/T41 platforms
  out.sensor_id = 0;
  out.video_interface =
      static_cast<IMPSensorVinType>(cfg->sensor.video_interface);
  out.mclk = static_cast<IMPSensorMclk>(cfg->sensor.mclk);
  out.default_boot = 0;
#endif

  return out;
}

IMPSystem *IMPSystem::createNew() {
  IMPSystem *sys = new IMPSystem();
  if (sys->init_failed) {
    LOG_ERROR("IMPSystem::createNew failed: error initializing the imp system.");
    return nullptr; // leak: ~IMPSystem() would destroy() a half-initialized system
  }
  return sys;
}

int IMPSystem::init() {
  LOG_DEBUG("IMPSystem::init()");
  int ret = 0;

#if defined(PLATFORM_T23)
  LOG_WARN("IMPSystem init: skipping early sensor procfs refresh on T23 until "
           "after sensor enable");
#else
  refresh_sensor_properties_from_proc();
  resolve_all_stream_geometry();
  apply_default_bitrates();
#endif

  {
    int pool_size_kb;
    if (cfg->general.osd_pool_size > 0) {
      pool_size_kb = cfg->general.osd_pool_size;
    } else {
      // Auto-calculate from the largest enabled stream: ~10% RGBA coverage +
      // 256KB margin
      int max_pixels = 0;
      for (auto *s :
           {&cfg->stream0, &cfg->stream1, &cfg->stream2, &cfg->stream3}) {
        if (s->enabled)
          max_pixels = std::max(max_pixels, s->width * s->height);
      }
      pool_size_kb = (max_pixels * 4 * 0.1) / 1024 + 256;
    }
    ret = IMP_OSD_SetPoolSize(pool_size_kb * 1024);
    LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_SetPoolSize(" << pool_size_kb << "KB)");
  }

  IMPVersion impVersion;
  ret = IMP_System_GetVersion(&impVersion);
  LOG_INFO("LIBIMP Version " << impVersion.aVersion);

  SUVersion suVersion;
  ret = SU_Base_GetVersion(&suVersion);
  LOG_INFO("SYSUTILS Version: " << suVersion.chr);

#if defined(PLATFORM_T23)
  cfg->sysinfo.cpu = "T23";
  LOG_WARN("IMPSystem init: skipping IMP_System_GetCPUInfo on T23 (set "
           "PRUDYNT_FORCE_CPUINFO=1 to enable)");
  const char *force_cpuinfo = std::getenv("PRUDYNT_FORCE_CPUINFO");
  if (force_cpuinfo && force_cpuinfo[0] != '\0' &&
      strcmp(force_cpuinfo, "1") == 0) {
    cfg->sysinfo.cpu = IMP_System_GetCPUInfo();
    LOG_INFO("CPU Information: " << cfg->sysinfo.cpu);
  }
#else
  cfg->sysinfo.cpu = IMP_System_GetCPUInfo();
  LOG_INFO("CPU Information: " << cfg->sysinfo.cpu);
#endif

  ret = IMP_ISP_Open();
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_ISP_Open()");

  /* sensor */
  sinfo = create_sensor_info(cfg->sensor.model);
#if defined(PLATFORM_T23)
  cleanup_stale_t23_isp_state(sinfo);
#endif
  ret = add_sensor_with_retry(sinfo);
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "hal::isp::add_sensor(&sinfo)");

  ret = hal::isp::enable_sensor(&sinfo);
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "hal::isp::enable_sensor(&sinfo)");

  // Wait for sensor to initialize and populate procfs files
  // Some sensors (like sc3332) need time after enable before procfs is ready
  if (!wait_for_sensor_resolution()) {
    LOG_ERROR("Sensor failed to initialize properly - resolution is 0x0");
    LOG_ERROR("This may indicate:");
    LOG_ERROR("  1. Sensor driver not properly loaded");
    LOG_ERROR("  2. Sensor hardware connection issue");
    LOG_ERROR("  3. Incompatible sensor driver");
    // Continue anyway in case sensor properties were set from config
  }

  // Refresh sensor properties again after sensor is enabled
  // This updates actual_fps and max_fps based on the resolution mode selected
  // by the sensor driver
  refresh_sensor_properties_from_proc();
  resolve_all_stream_geometry();
  apply_default_bitrates();

  /* system */
  ret = IMP_System_Init();
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_System_Init()");

  ret = IMP_ISP_EnableTuning();
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_ISP_EnableTuning()");

#if !defined(NO_TUNINGS)
  ret = hal::isp::set_contrast(static_cast<unsigned char>(cfg->image.contrast));
  LOG_DEBUG_OR_ERROR(ret,
                     "hal::isp::set_contrast(" << cfg->image.contrast << ")");

  ret =
      hal::isp::set_sharpness(static_cast<unsigned char>(cfg->image.sharpness));
  LOG_DEBUG_OR_ERROR(ret,
                     "hal::isp::set_sharpness(" << cfg->image.sharpness << ")");

  ret = hal::isp::set_saturation(
      static_cast<unsigned char>(cfg->image.saturation));
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_saturation(" << cfg->image.saturation
                                                      << ")");

  ret = hal::isp::set_brightness(
      static_cast<unsigned char>(cfg->image.brightness));
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_brightness(" << cfg->image.brightness
                                                      << ")");

  ret = hal::isp::set_sinter_strength(
      static_cast<unsigned char>(cfg->image.sinter_strength));
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_sinter_strength("
                              << cfg->image.sinter_strength << ")");

  ret = hal::isp::set_temper_strength(
      static_cast<unsigned char>(cfg->image.temper_strength));
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_temper_strength("
                              << cfg->image.temper_strength << ")");

  ret = hal::isp::set_hflip(cfg->image.hflip);
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_hflip(" << cfg->image.hflip << ")");

  ret = hal::isp::set_vflip(cfg->image.vflip);
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_vflip(" << cfg->image.vflip << ")");

  ret = hal::isp::set_running_mode(
      static_cast<hal::isp::RunningMode>(cfg->image.running_mode));
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_running_mode("
                              << cfg->image.running_mode << ")");

  ret = hal::isp::set_isp_bypass(cfg->image.isp_bypass);
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_isp_bypass(" << cfg->image.isp_bypass
                                                      << ")");

  ret = hal::isp::set_anti_flicker(cfg->image.anti_flicker);
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_anti_flicker("
                              << cfg->image.anti_flicker << ")");

  ret = hal::isp::set_ae_compensation(cfg->image.ae_compensation);
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_ae_compensation("
                              << cfg->image.ae_compensation << ")");

  ret =
      hal::isp::set_max_again(static_cast<unsigned char>(cfg->image.max_again));
  LOG_DEBUG_OR_ERROR(ret,
                     "hal::isp::set_max_again(" << cfg->image.max_again << ")");

#if defined(PLATFORM_T20) || defined(PLATFORM_T23)
  // Legacy XBurst1 ISP SDKs can panic in optional tuning ioctls when RMEM is
  // tight.
  const char *force_advanced_isp = std::getenv("PRUDYNT_FORCE_ADVANCED_ISP");
  bool apply_advanced_isp_tunings =
      (force_advanced_isp && force_advanced_isp[0] != '\0' &&
       strcmp(force_advanced_isp, "1") == 0);
  const char *advanced_isp_platform =
#if defined(PLATFORM_T20)
      "T20";
#else
      "T23";
#endif
  if (!apply_advanced_isp_tunings) {
    LOG_WARN("IMPSystem init: advanced ISP tunings disabled on "
             << advanced_isp_platform
             << " (set PRUDYNT_FORCE_ADVANCED_ISP=1 to enable)");
  }
  if (apply_advanced_isp_tunings) {
    ret = hal::isp::set_max_dgain(
        static_cast<unsigned char>(cfg->image.max_dgain));
    LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_max_dgain(" << cfg->image.max_dgain
                                                       << ")");

    ret = hal::isp::set_wb(cfg->image.core_wb_mode, cfg->image.wb_rgain,
                           cfg->image.wb_bgain);
    LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_wb(mode="
                                << cfg->image.core_wb_mode
                                << ", rgain=" << cfg->image.wb_rgain
                                << ", bgain=" << cfg->image.wb_bgain << ")");

    ret = hal::isp::set_hue(static_cast<unsigned char>(cfg->image.hue));
    LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_hue(" << cfg->image.hue << ")");

    ret = hal::isp::set_defog_strength(
        static_cast<uint8_t>(cfg->image.defog_strength));
    LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_defog_strength("
                                << cfg->image.defog_strength << ")");

    ret = hal::isp::set_dpc_strength(
        static_cast<unsigned char>(cfg->image.dpc_strength));
    LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_dpc_strength("
                                << cfg->image.dpc_strength << ")");
  }

  const char *force_drc = std::getenv("PRUDYNT_FORCE_DRC");
  if (force_drc && force_drc[0] != '\0' && strcmp(force_drc, "1") == 0) {
    ret = hal::isp::set_drc_strength(
        static_cast<unsigned char>(cfg->image.drc_strength));
    LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_drc_strength("
                                << cfg->image.drc_strength << ")");
  } else {
    LOG_WARN("IMPSystem init: skipping hal::isp::set_drc_strength on "
             << advanced_isp_platform
             << " (set PRUDYNT_FORCE_DRC=1 to enable)");
  }
#else
  ret =
      hal::isp::set_max_dgain(static_cast<unsigned char>(cfg->image.max_dgain));
  LOG_DEBUG_OR_ERROR(ret,
                     "hal::isp::set_max_dgain(" << cfg->image.max_dgain << ")");

  ret = hal::isp::set_wb(cfg->image.core_wb_mode, cfg->image.wb_rgain,
                         cfg->image.wb_bgain);
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_wb(mode="
                              << cfg->image.core_wb_mode
                              << ", rgain=" << cfg->image.wb_rgain
                              << ", bgain=" << cfg->image.wb_bgain << ")");

  ret = hal::isp::set_hue(static_cast<unsigned char>(cfg->image.hue));
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_hue(" << cfg->image.hue << ")");

  ret = hal::isp::set_defog_strength(
      static_cast<uint8_t>(cfg->image.defog_strength));
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_defog_strength("
                              << cfg->image.defog_strength << ")");

  ret = hal::isp::set_dpc_strength(
      static_cast<unsigned char>(cfg->image.dpc_strength));
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_dpc_strength("
                              << cfg->image.dpc_strength << ")");

  ret = hal::isp::set_drc_strength(
      static_cast<unsigned char>(cfg->image.drc_strength));
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_drc_strength("
                              << cfg->image.drc_strength << ")");
#endif

  const bool backlight_requested = cfg->image.backlight_compensation > 0;
  const bool highlight_requested = cfg->image.highlight_depress > 0;
  bool applied_backlight = false;
  if (backlight_requested) {
    ret = hal::isp::set_backlight_comp(
        static_cast<unsigned char>(cfg->image.backlight_compensation));
    LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_backlight_comp("
                                << cfg->image.backlight_compensation << ")");
    applied_backlight = (ret == 0);
  }
  if ((!applied_backlight || !hal::caps().has_isp_backlight_comp) &&
      highlight_requested) {
    ret = hal::isp::set_highlight_depress(
        static_cast<unsigned char>(cfg->image.highlight_depress));
    LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_highlight_depress("
                                << cfg->image.highlight_depress << ")");
  }

  LOG_DEBUG("ISP Tuning Defaults set");

  // Clamp sensor FPS to a sane value based on stream0 desired FPS and sensor
  // limits
  int desired_sensor_fps = cfg->stream0.fps > 0 ? cfg->stream0.fps : 25;
  // cfg->sensor.fps is read from /proc/jz/sensor/max_fps; min from min_fps
  if (cfg->sensor.min_fps > 0 && desired_sensor_fps < cfg->sensor.min_fps) {
    desired_sensor_fps = cfg->sensor.min_fps;
  }
  if (cfg->sensor.fps > 0 && desired_sensor_fps > (int)cfg->sensor.fps) {
    desired_sensor_fps = cfg->sensor.fps;
  }

#if defined(PLATFORM_T23)
  const char *force_sensor_fps = std::getenv("PRUDYNT_FORCE_SENSOR_FPS");
  if (force_sensor_fps && force_sensor_fps[0] != '\0' &&
      strcmp(force_sensor_fps, "1") == 0) {
    ret = hal::isp::set_sensor_fps(desired_sensor_fps, 1);
    LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "hal::isp::set_sensor_fps("
                                         << desired_sensor_fps << ", 1)");
  } else {
    LOG_WARN("IMPSystem init: skipping hal::isp::set_sensor_fps on T23 (set "
             "PRUDYNT_FORCE_SENSOR_FPS=1 to enable)");
  }
#else
  ret = hal::isp::set_sensor_fps(desired_sensor_fps, 1);
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "hal::isp::set_sensor_fps("
                                       << desired_sensor_fps << ", 1)");
#endif

#if defined(PLATFORM_T21)
  // T20 T21 only set FPS if it is read after set.
  int fps_num, fps_den;
  ret = hal::isp::get_sensor_fps(fps_num, fps_den);
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "hal::isp::get_sensor_fps("
                                       << fps_num << ", " << fps_den << ")");
#endif

  // Apply the configured running_mode at the end of init.
  // This ensures the ISP starts in the correct mode from boot,
  // avoiding a race where daynightd's API call fails because
  // prudynt's HTTP server isn't listening yet.
  ret = hal::isp::set_running_mode(
      static_cast<hal::isp::RunningMode>(cfg->image.running_mode));
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "hal::isp::set_running_mode("
                                  << cfg->image.running_mode << ")");
#endif // #if !defined(NO_TUNINGS)

  return ret;
}

int IMPSystem::destroy() {
  int ret;

  LOG_INFO("IMPSystem::destroy() begin");

  // Tear down in reverse order of init(): tuning -> system -> sensor -> ISP.
  ret = IMP_ISP_DisableTuning();
  LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_DisableTuning()");

  ret = IMP_System_Exit();
  LOG_DEBUG_OR_ERROR(ret, "IMP_System_Exit()");

  ret = hal::isp::disable_sensor();
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::disable_sensor()");

  ret = hal::isp::del_sensor(&sinfo);
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::del_sensor(&sinfo)");

  ret = IMP_ISP_Close();
  LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_Close()");

  LOG_INFO("IMPSystem::destroy() complete");

  return 0;
}
