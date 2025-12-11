#include "IMPSystem.hpp"
#include "Config.hpp"
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

void clamp_stream_to_sensor_limits(const char *stream_name, _stream &stream) {
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
  out.video_interface =
      static_cast<IMPSensorVinType>(cfg->sensor.video_interface);
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
  LOG_DEBUG_OR_ERROR(ret, "IMP_OSD_SetPoolSize("
                              << (cfg->general.osd_pool_size * 1024) << ")");

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
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  ret = IMP_ISP_AddSensor(IMPVI_MAIN, &sinfo);
#else
  ret = IMP_ISP_AddSensor(&sinfo);
#endif
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_ISP_AddSensor(&sinfo)");

#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  ret = IMP_ISP_EnableSensor(IMPVI_MAIN, &sinfo);
#else
  ret = IMP_ISP_EnableSensor();
#endif
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_ISP_EnableSensor()");

  /* system */
  ret = IMP_System_Init();
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_System_Init()");

  ret = IMP_ISP_EnableTuning();
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_ISP_EnableTuning()");

#if !defined(NO_TUNINGS)
  ret = IMP_ISP_Tuning_SetContrast(cfg->image.contrast);
  LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_Tuning_SetContrast(" << cfg->image.contrast
                                                        << ")");

  ret = IMP_ISP_Tuning_SetSharpness(cfg->image.sharpness);
  LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_Tuning_SetSharpness(" << cfg->image.sharpness
                                                         << ")");

  ret = IMP_ISP_Tuning_SetSaturation(cfg->image.saturation);
  LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_Tuning_SetSaturation("
                              << cfg->image.saturation << ")");

  ret = IMP_ISP_Tuning_SetBrightness(cfg->image.brightness);
  LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_Tuning_SetBrightness("
                              << cfg->image.brightness << ")");

  ret = IMP_ISP_Tuning_SetContrast(cfg->image.contrast);
  LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_Tuning_SetContrast(" << cfg->image.contrast
                                                        << ")");

  ret = IMP_ISP_Tuning_SetSharpness(cfg->image.sharpness);
  LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_Tuning_SetSharpness(" << cfg->image.sharpness
                                                         << ")");

  ret = IMP_ISP_Tuning_SetSaturation(cfg->image.saturation);
  LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_Tuning_SetSaturation("
                              << cfg->image.saturation << ")");

  ret = IMP_ISP_Tuning_SetBrightness(cfg->image.brightness);
  LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_Tuning_SetBrightness("
                              << cfg->image.brightness << ")");

#if !defined(PLATFORM_T21)
  ret = IMP_ISP_Tuning_SetSinterStrength(cfg->image.sinter_strength);
  LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_Tuning_SetSinterStrength("
                              << cfg->image.sinter_strength << ")");
#endif

  ret = IMP_ISP_Tuning_SetTemperStrength(cfg->image.temper_strength);
  LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_Tuning_SetTemperStrength("
                              << cfg->image.temper_strength << ")");

  ret = IMP_ISP_Tuning_SetISPHflip((IMPISPTuningOpsMode)cfg->image.hflip);
  LOG_DEBUG_OR_ERROR(ret,
                     "IMP_ISP_Tuning_SetISPHflip(" << cfg->image.hflip << ")");

  ret = IMP_ISP_Tuning_SetISPVflip((IMPISPTuningOpsMode)cfg->image.vflip);
  LOG_DEBUG_OR_ERROR(ret,
                     "IMP_ISP_Tuning_SetISPVflip(" << cfg->image.vflip << ")");

  ret = IMP_ISP_Tuning_SetISPRunningMode(
      (IMPISPRunningMode)cfg->image.running_mode);
  LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_Tuning_SetISPRunningMode("
                              << cfg->image.running_mode << ")");

  // Configurable ISP bypass
  ret = IMP_ISP_Tuning_SetISPBypass(cfg->image.isp_bypass
                                        ? IMPISP_TUNING_OPS_MODE_ENABLE
                                        : IMPISP_TUNING_OPS_MODE_DISABLE);
  LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_Tuning_SetISPBypass("
                              << (cfg->image.isp_bypass
                                      ? IMPISP_TUNING_OPS_MODE_ENABLE
                                      : IMPISP_TUNING_OPS_MODE_DISABLE)
                              << ")");

  IMPISPAntiflickerAttr flickerAttr;
  memset(&flickerAttr, 0, sizeof(IMPISPAntiflickerAttr));
  ret = IMP_ISP_Tuning_SetAntiFlickerAttr(
      (IMPISPAntiflickerAttr)cfg->image.anti_flicker);
  LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_Tuning_SetAntiFlickerAttr("
                              << cfg->image.anti_flicker << ")");

#if !defined(PLATFORM_T21)
  ret = IMP_ISP_Tuning_SetAeComp(cfg->image.ae_compensation);
  LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_Tuning_SetAeComp("
                              << cfg->image.ae_compensation << ")");
#endif

  ret = IMP_ISP_Tuning_SetMaxAgain(cfg->image.max_again);
  LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_Tuning_SetMaxAgain(" << cfg->image.max_again
                                                        << ")");

  ret = IMP_ISP_Tuning_SetMaxDgain(cfg->image.max_dgain);
  LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_Tuning_SetMaxDgain(" << cfg->image.max_dgain
                                                        << ")");

  IMPISPWB wb;
  memset(&wb, 0, sizeof(IMPISPWB));
  wb.mode = (isp_core_wb_mode)cfg->image.core_wb_mode;
  wb.rgain = cfg->image.wb_rgain;
  wb.bgain = cfg->image.wb_bgain;
  ret = IMP_ISP_Tuning_SetWB(&wb);
  if (ret != 0) {
    LOG_ERROR("Unable to set white balance. Mode: "
              << cfg->image.core_wb_mode << ", rgain: " << cfg->image.wb_rgain
              << ", bgain: " << cfg->image.wb_bgain);
  } else {
    LOG_DEBUG("Set white balance. Mode: "
              << cfg->image.core_wb_mode << ", rgain: " << cfg->image.wb_rgain
              << ", bgain: " << cfg->image.wb_bgain);
  }

#if defined(PLATFORM_T23) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
  ret = IMP_ISP_Tuning_SetBcshHue(cfg->image.hue);
  LOG_DEBUG_OR_ERROR(ret,
                     "IMP_ISP_Tuning_SetBcshHue(" << cfg->image.hue << ")");

  uint8_t _defog_strength = static_cast<uint8_t>(cfg->image.defog_strength);
  ret = IMP_ISP_Tuning_SetDefog_Strength(
      reinterpret_cast<uint8_t *>(&_defog_strength));
  LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_Tuning_SetDefog_Strength("
                              << cfg->image.defog_strength << ")");

  ret = IMP_ISP_Tuning_SetDPC_Strength(cfg->image.dpc_strength);
  LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_Tuning_SetDPC_Strength("
                              << cfg->image.dpc_strength << ")");
#endif
#if defined(PLATFORM_T21) || defined(PLATFORM_T23) || defined(PLATFORM_T31) || \
    defined(PLATFORM_C100)
  ret = IMP_ISP_Tuning_SetDRC_Strength(cfg->image.drc_strength);
  LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_Tuning_SetDRC_Strength("
                              << cfg->image.drc_strength << ")");
#endif

#if defined(PLATFORM_T23) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
  if (cfg->image.backlight_compensation > 0) {
    ret = IMP_ISP_Tuning_SetBacklightComp(cfg->image.backlight_compensation);
    LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_Tuning_SetBacklightComp("
                                << cfg->image.backlight_compensation << ")");
  } else if (cfg->image.highlight_depress > 0) {
    ret = IMP_ISP_Tuning_SetHiLightDepress(cfg->image.highlight_depress);
    LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_Tuning_SetHiLightDepress("
                                << cfg->image.highlight_depress << ")");
  }
#elif defined(PLATFORM_T21) || defined(PLATFORM_T30)
  ret = IMP_ISP_Tuning_SetHiLightDepress(cfg->image.highlight_depress);
  LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_Tuning_SetHiLightDepress("
                              << cfg->image.highlight_depress << ")");
#endif

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
  ret = IMP_ISP_Tuning_SetSensorFPS(desired_sensor_fps, 1);
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_ISP_Tuning_SetSensorFPS("
                                       << desired_sensor_fps << ", 1)");

#if defined(PLATFORM_T21)
  // T20 T21 only set FPS if it is read after set.
  uint32_t fps_num, fps_den;
  ret = IMP_ISP_Tuning_GetSensorFPS(&fps_num, &fps_den);
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_ISP_Tuning_GetSensorFPS("
                                       << fps_num << ", " << fps_den << ")");
#endif

  // Set the ISP to DAY on launch
  ret = IMP_ISP_Tuning_SetISPRunningMode(IMPISP_RUNNING_MODE_DAY);
  LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_ISP_Tuning_SetISPRunningMode("
                                       << IMPISP_RUNNING_MODE_DAY << ")");
#endif // #if !defined(NO_TUNINGS)

  return ret;
}

int IMPSystem::destroy() {
  int ret;

  ret = IMP_System_Exit();
  LOG_DEBUG_OR_ERROR(ret, "IMP_System_Exit()");

#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  ret = IMP_ISP_DisableSensor(IMPVI_MAIN);
#else
  ret = IMP_ISP_DisableSensor();
#endif
  LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_DisableSensor()");

#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
  ret = IMP_ISP_DelSensor(IMPVI_MAIN, &sinfo);
#else
  ret = IMP_ISP_DelSensor(&sinfo);
#endif
  LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_DelSensor()");

  ret = IMP_ISP_DisableTuning();
  LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_DisableTuning()");

  ret = IMP_ISP_Close();
  LOG_DEBUG_OR_ERROR(ret, "IMP_ISP_Close()");

  return 0;
}
