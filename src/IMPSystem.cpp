#include "IMPSystem.hpp"
#include "Config.hpp"
#include "imp_hal.hpp"
#include <fstream>

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
      LOG_INFO("Sensor width updated from procfs: " << cfg->sensor.width << " -> " << proc_width);
    }
    cfg->sensor.width = proc_width;
  }

  if (height_ok && proc_height > 0) {
    if (cfg->sensor.height != proc_height) {
      LOG_INFO("Sensor height updated from procfs: " << cfg->sensor.height << " -> " << proc_height);
    }
    cfg->sensor.height = proc_height;
  }

  if (max_fps_ok && proc_max_fps > 0) {
    if (cfg->sensor.fps != proc_max_fps) {
      LOG_INFO("Sensor max_fps updated from procfs: " << cfg->sensor.fps << " -> " << proc_max_fps);
    }
    cfg->sensor.fps = proc_max_fps;
  }

  if (min_fps_ok && proc_min_fps > 0) {
    if (cfg->sensor.min_fps != proc_min_fps) {
      LOG_INFO("Sensor min_fps updated from procfs: " << cfg->sensor.min_fps << " -> " << proc_min_fps);
    }
    cfg->sensor.min_fps = proc_min_fps;
  }

  if (cfg->sensor.fps > 0 && cfg->sensor.min_fps > 0 && cfg->sensor.fps < cfg->sensor.min_fps) {
    LOG_WARN("Sensor max_fps " << cfg->sensor.fps << " is below min_fps " << cfg->sensor.min_fps
                               << ", clamping to min_fps");
    cfg->sensor.fps = cfg->sensor.min_fps;
  }

  if ((width_ok || height_ok || max_fps_ok || min_fps_ok) && cfg->sensor.width > 0 && cfg->sensor.height > 0) {
    LOG_INFO("Sensor procfs geometry " << cfg->sensor.width << "x" << cfg->sensor.height << " @ " << cfg->sensor.min_fps
                                       << "-" << cfg->sensor.fps << " fps");
  }
}

void clamp_stream_to_sensor_limits(const char *stream_name, _stream &stream) {
  if (!cfg) {
    return;
  }

  const int sensor_width = cfg->sensor.width;
  const int sensor_height = cfg->sensor.height;
  const int sensor_max_fps = cfg->sensor.fps;
  const int sensor_min_fps = cfg->sensor.min_fps;

  auto log_change = [&](const char *what, int old_val, int new_val) {
    LOG_INFO(stream_name << ": " << what << " clamped from " << old_val << " to " << new_val);
  };

  if (sensor_width > 0) {
    if (stream.width <= 0) {
      log_change("width", stream.width, sensor_width);
      stream.width = sensor_width;
    } else if (stream.width > sensor_width) {
      log_change("width", stream.width, sensor_width);
      stream.width = sensor_width;
    }
  }

  if (sensor_height > 0) {
    if (stream.height <= 0) {
      log_change("height", stream.height, sensor_height);
      stream.height = sensor_height;
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
}

void clamp_streams_to_sensor_limits() {
  if (!cfg) {
    return;
  }

  clamp_stream_to_sensor_limits("stream0", cfg->stream0);
  clamp_stream_to_sensor_limits("stream1", cfg->stream1);
  clamp_stream_to_sensor_limits("stream2", cfg->stream2);
  clamp_stream_to_sensor_limits("stream3", cfg->stream3);
}

} // namespace

IMPSensorInfo IMPSystem::create_sensor_info(const char *sensor_name) {
  IMPSensorInfo out;
  memset(&out, 0, sizeof(IMPSensorInfo));
  LOG_INFO("Sensor: " << cfg->sensor.model);
  strcpy(out.name, cfg->sensor.model);
  out.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
  strcpy(out.i2c.type, cfg->sensor.model);
  out.i2c.addr = cfg->sensor.i2c_address;

#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  // Additional fields required for T40/T41 platforms
  out.i2c.i2c_adapter_id = cfg->sensor.i2c_bus;
  out.rst_gpio = cfg->sensor.gpio_reset;
  out.pwdn_gpio = -1;
  out.power_gpio = -1;
  out.sensor_id = 0;
  out.video_interface = static_cast<IMPSensorVinType>(cfg->sensor.video_interface);
  out.mclk = static_cast<IMPSensorMclk>(cfg->sensor.mclk);
  out.default_boot = 0;
#endif

  return out;
}

IMPSystem *IMPSystem::createNew() {
  return new IMPSystem();
}

int IMPSystem::init() {
  LOG_DEBUG("IMPSystem::init()");
  int ret = 0;

  refresh_sensor_properties_from_proc();
  clamp_streams_to_sensor_limits();

  ret = IMP_OSD_SetPoolSize(cfg->general.osd_pool_size * 1024);
  LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_SetPoolSize(" << (cfg->general.osd_pool_size * 1024) << ")");

  IMPVersion impVersion;
  ret = IMP_System_GetVersion(&impVersion);
  LOG_INFO("LIBIMP Version " << impVersion.aVersion);

  SUVersion suVersion;
  ret = SU_Base_GetVersion(&suVersion);
  LOG_INFO("SYSUTILS Version: " << suVersion.chr);

  cfg->sysinfo.cpu = IMP_System_GetCPUInfo();
  LOG_INFO("CPU Information: " << cfg->sysinfo.cpu);

  ret = IMP_ISP_Open();
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_ISP_Open()");

  /* sensor */
  sinfo = create_sensor_info(cfg->sensor.model);
  ret = hal::isp::add_sensor(&sinfo);
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "hal::isp::add_sensor(&sinfo)");

  ret = hal::isp::enable_sensor(&sinfo);
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "hal::isp::enable_sensor(&sinfo)");

  // Refresh sensor properties again after sensor is enabled
  // This updates actual_fps and max_fps based on the resolution mode selected by the sensor driver
  refresh_sensor_properties_from_proc();
  clamp_streams_to_sensor_limits();

  /* system */
  ret = IMP_System_Init();
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_System_Init()");

  ret = IMP_ISP_EnableTuning();
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_ISP_EnableTuning()");

#if !defined(NO_TUNINGS)
  ret = hal::isp::set_contrast(static_cast<unsigned char>(cfg->image.contrast));
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_contrast(" << cfg->image.contrast << ")");

  ret = hal::isp::set_sharpness(static_cast<unsigned char>(cfg->image.sharpness));
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_sharpness(" << cfg->image.sharpness << ")");

  ret = hal::isp::set_saturation(static_cast<unsigned char>(cfg->image.saturation));
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_saturation(" << cfg->image.saturation << ")");

  ret = hal::isp::set_brightness(static_cast<unsigned char>(cfg->image.brightness));
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_brightness(" << cfg->image.brightness << ")");

  ret = hal::isp::set_sinter_strength(static_cast<unsigned char>(cfg->image.sinter_strength));
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_sinter_strength(" << cfg->image.sinter_strength << ")");

  ret = hal::isp::set_temper_strength(static_cast<unsigned char>(cfg->image.temper_strength));
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_temper_strength(" << cfg->image.temper_strength << ")");

  ret = hal::isp::set_hflip(cfg->image.hflip);
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_hflip(" << cfg->image.hflip << ")");

  ret = hal::isp::set_vflip(cfg->image.vflip);
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_vflip(" << cfg->image.vflip << ")");

  ret = hal::isp::set_running_mode(static_cast<hal::isp::RunningMode>(cfg->image.running_mode));
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_running_mode(" << cfg->image.running_mode << ")");

  ret = hal::isp::set_isp_bypass(cfg->image.isp_bypass);
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_isp_bypass(" << cfg->image.isp_bypass << ")");

  ret = hal::isp::set_anti_flicker(cfg->image.anti_flicker);
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_anti_flicker(" << cfg->image.anti_flicker << ")");

  ret = hal::isp::set_ae_compensation(cfg->image.ae_compensation);
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_ae_compensation(" << cfg->image.ae_compensation << ")");

  ret = hal::isp::set_max_again(static_cast<unsigned char>(cfg->image.max_again));
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_max_again(" << cfg->image.max_again << ")");

  ret = hal::isp::set_max_dgain(static_cast<unsigned char>(cfg->image.max_dgain));
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_max_dgain(" << cfg->image.max_dgain << ")");

  ret = hal::isp::set_wb(cfg->image.core_wb_mode, cfg->image.wb_rgain, cfg->image.wb_bgain);
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_wb(mode=" << cfg->image.core_wb_mode << ", rgain="
                                                    << cfg->image.wb_rgain << ", bgain=" << cfg->image.wb_bgain
                                                    << ")");

  ret = hal::isp::set_hue(static_cast<unsigned char>(cfg->image.hue));
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_hue(" << cfg->image.hue << ")");

  ret = hal::isp::set_defog_strength(static_cast<uint8_t>(cfg->image.defog_strength));
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_defog_strength(" << cfg->image.defog_strength << ")");

  ret = hal::isp::set_dpc_strength(static_cast<unsigned char>(cfg->image.dpc_strength));
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_dpc_strength(" << cfg->image.dpc_strength << ")");

  ret = hal::isp::set_drc_strength(static_cast<unsigned char>(cfg->image.drc_strength));
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_drc_strength(" << cfg->image.drc_strength << ")");

  const bool backlight_requested = cfg->image.backlight_compensation > 0;
  const bool highlight_requested = cfg->image.highlight_depress > 0;
  bool applied_backlight = false;
  if (backlight_requested) {
    ret = hal::isp::set_backlight_comp(static_cast<unsigned char>(cfg->image.backlight_compensation));
    LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_backlight_comp(" << cfg->image.backlight_compensation << ")");
    applied_backlight = (ret == 0);
  }
  if ((!applied_backlight || !hal::caps().has_isp_backlight_comp) && highlight_requested) {
    ret = hal::isp::set_highlight_depress(static_cast<unsigned char>(cfg->image.highlight_depress));
    LOG_DEBUG_OR_ERROR(ret, "hal::isp::set_highlight_depress(" << cfg->image.highlight_depress << ")");
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
  ret = hal::isp::set_sensor_fps(desired_sensor_fps, 1);
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "hal::isp::set_sensor_fps(" << desired_sensor_fps << ", 1)");

#if defined(PLATFORM_T21)
  // T20 T21 only set FPS if it is read after set.
  int fps_num, fps_den;
  ret = hal::isp::get_sensor_fps(fps_num, fps_den);
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "hal::isp::get_sensor_fps(" << fps_num << ", " << fps_den << ")");
#endif

  // Set the ISP to DAY on launch
  ret = hal::isp::set_running_mode(hal::isp::RunningMode::Day);
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "hal::isp::set_running_mode(Day)");
#endif // #if !defined(NO_TUNINGS)

  return ret;
}

int IMPSystem::destroy() {
  int ret;

  ret = IMP_System_Exit();
  LOG_DEBUG_OR_ERROR(ret, "IMP_System_Exit()");

  ret = hal::isp::disable_sensor();
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::disable_sensor()");

  ret = hal::isp::del_sensor(&sinfo);
  LOG_DEBUG_OR_ERROR(ret, "hal::isp::del_sensor(&sinfo)");

  ret = IMP_ISP_DisableTuning();
  LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_DisableTuning()");

  ret = IMP_ISP_Close();
  LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_Close()");

  return 0;
}
