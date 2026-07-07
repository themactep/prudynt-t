#include "OSD.hpp"
#include "Config.hpp"
#include "Logger.hpp"
#include "globals.hpp"
#include "imp_hal.hpp"
#include <cmath>
#include <pthread.h>
#include <unistd.h>
#include <vector>

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace {
constexpr uint8_t OSD_FLAG_TIME = 1U << 0;
constexpr uint8_t OSD_FLAG_USER = 1U << 1;
constexpr uint8_t OSD_FLAG_UPTIME = 1U << 2;
constexpr uint8_t OSD_FLAG_BRIGHTNESS = 1U << 3;
constexpr const char *PRIMARY_ISP_STATS = "/proc/jz/isp/isp-m0";
constexpr const char *SECONDARY_ISP_STATS = "/tmp/test-isp-m0";
constexpr float MAX_ANALOG_GAIN = 160.0f;
constexpr float MAX_DIGITAL_GAIN = 80.0f;
constexpr float DEFAULT_DAY_BRIGHTNESS = 70.0f;
constexpr float DEFAULT_NIGHT_BRIGHTNESS = 25.0f;
} // namespace

// ── helpers ──────────────────────────────────────────────────────────

unsigned long getSystemUptime() {
  struct sysinfo info;
  if (sysinfo(&info) != 0) {
    return 0;
  }
  return info.uptime;
}

int getIp(char *addressBuffer) {
  struct ifaddrs *ifAddrStruct = nullptr;
  struct ifaddrs *ifa = nullptr;
  void *tmpAddrPtr = nullptr;

  getifaddrs(&ifAddrStruct);

  for (ifa = ifAddrStruct; ifa != nullptr; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr) {
      continue;
    }
    if (ifa->ifa_addr->sa_family == AF_INET) {
      tmpAddrPtr = &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
      inet_ntop(AF_INET, tmpAddrPtr, addressBuffer, INET_ADDRSTRLEN);
    }
  }
  if (ifAddrStruct != nullptr)
    freeifaddrs(ifAddrStruct);
  return 0;
}

void replace(std::string &str, const std::string &oldToken,
             const std::string &newToken) {
  size_t pos = 0;
  while ((pos = str.find(oldToken, pos)) != std::string::npos) {
    str.replace(pos, oldToken.length(), newToken);
    pos += newToken.length();
  }
}

// ── BrightnessMeter ──────────────────────────────────────────────────

OSD::BrightnessMeter::BrightnessMeter()
    : history{}, historyIndex(0), historyFilled(false), lastReadFailed(false) {
  history.fill(-1.0f);
}

bool OSD::BrightnessMeter::readIspStats(IspStats &stats) {
  const char *activePath = PRIMARY_ISP_STATS;
  std::ifstream file(activePath);
  if (!file.is_open()) {
    file.open(SECONDARY_ISP_STATS);
    if (file.is_open()) {
      activePath = SECONDARY_ISP_STATS;
    }
  }

  if (!file.is_open()) {
    if (!lastReadFailed) {
      LOG_WARN("BrightnessMeter: unable to read ISP stats from "
               << PRIMARY_ISP_STATS);
      lastReadFailed = true;
    }
    return false;
  }

  lastReadFailed = false;
  bool parsed = false;
  std::string line;
  while (std::getline(file, line)) {
    if (line.find("ISP Runing Mode :") != std::string::npos) {
      char modeBuf[32] = {0};
      if (sscanf(line.c_str(), "ISP Runing Mode : %31s", modeBuf) == 1) {
        stats.mode = modeBuf;
        parsed = true;
      }
    } else if (line.find("SENSOR Integration Time :") != std::string::npos) {
      if (sscanf(line.c_str(), "SENSOR Integration Time : %d",
                 &stats.integrationTime) == 1) {
        parsed = true;
      }
    } else if (line.find("SENSOR Max Integration Time :") !=
               std::string::npos) {
      if (sscanf(line.c_str(), "SENSOR Max Integration Time : %d",
                 &stats.maxIntegrationTime) == 1) {
        parsed = true;
      }
    } else if (line.find("SENSOR analog gain :") != std::string::npos) {
      if (sscanf(line.c_str(), "SENSOR analog gain : %d", &stats.analogGain) ==
          1) {
        parsed = true;
      }
    } else if (line.find("SENSOR digital gain :") != std::string::npos) {
      if (sscanf(line.c_str(), "SENSOR digital gain : %d",
                 &stats.digitalGain) == 1) {
        parsed = true;
      }
    } else if (line.find("ISP digital gain :") != std::string::npos) {
      if (sscanf(line.c_str(), "ISP digital gain : %d",
                 &stats.ispDigitalGain) == 1) {
        parsed = true;
      }
    } else if (line.find("ISP EV value:") != std::string::npos) {
      if (sscanf(line.c_str(), "ISP EV value: %d", &stats.evValue) == 1) {
        parsed = true;
      }
    } else if (line.find("Brightness :") != std::string::npos) {
      if (sscanf(line.c_str(), "Brightness : %d", &stats.currentBrightness) ==
          1) {
        parsed = true;
      }
    }
  }

  return parsed;
}

float OSD::BrightnessMeter::computeFromStats(const IspStats &stats,
                                             std::string &mode) const {
  std::string ispMode = stats.mode;
  if (!ispMode.empty()) {
    std::transform(
        ispMode.begin(), ispMode.end(), ispMode.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  }

  float brightness = -1.0f;
  if (stats.integrationTime >= 0 && stats.maxIntegrationTime > 0) {
    float exposureRatio = static_cast<float>(stats.integrationTime) /
                          static_cast<float>(stats.maxIntegrationTime);
    brightness = (1.0f - exposureRatio) * 100.0f;

    if (stats.analogGain >= 0) {
      float gainFactor =
          1.0f + (static_cast<float>(stats.analogGain) / MAX_ANALOG_GAIN);
      brightness /= gainFactor;
    }

    if (stats.ispDigitalGain > 0) {
      float digitalFactor =
          1.0f + (static_cast<float>(stats.ispDigitalGain) / MAX_DIGITAL_GAIN);
      brightness /= digitalFactor;
    }

    brightness = std::clamp(brightness, 0.0f, 100.0f);
    mode = ispMode.empty() ? "UNKNOWN" : ispMode;
    return brightness;
  }

  if (stats.currentBrightness >= 0) {
    brightness =
        (static_cast<float>(stats.currentBrightness) / 255.0f) * 100.0f;
    brightness = std::clamp(brightness, 0.0f, 100.0f);
    mode = ispMode.empty() ? "UNKNOWN" : ispMode;
    return brightness;
  }

  if (!ispMode.empty()) {
    if (ispMode == "DAY") {
      mode = ispMode;
      return DEFAULT_DAY_BRIGHTNESS;
    }
    if (ispMode == "NIGHT") {
      mode = ispMode;
      return DEFAULT_NIGHT_BRIGHTNESS;
    }
  }

  return -1.0f;
}

float OSD::BrightnessMeter::fallbackTimeBased(std::string &mode) const {
  time_t now = time(nullptr);
  if (now == static_cast<time_t>(-1)) {
    mode = "UNKNOWN";
    return -1.0f;
  }

  struct tm *tmInfo = localtime(&now);
  if (!tmInfo) {
    mode = "UNKNOWN";
    return -1.0f;
  }

  bool isDay = tmInfo->tm_hour >= 6 && tmInfo->tm_hour <= 18;
  mode = isDay ? "DAY" : "NIGHT";
  return isDay ? DEFAULT_DAY_BRIGHTNESS : DEFAULT_NIGHT_BRIGHTNESS;
}

void OSD::BrightnessMeter::updateHistory(float value) {
  history[historyIndex] = value;
  historyIndex = (historyIndex + 1) % history.size();
  if (historyIndex == 0) {
    historyFilled = true;
  }
}

float OSD::BrightnessMeter::historyAverage() const {
  size_t limit = historyFilled ? history.size() : historyIndex;
  if (limit == 0) {
    return -1.0f;
  }

  float sum = 0.0f;
  size_t count = 0;
  for (size_t i = 0; i < limit; ++i) {
    if (history[i] >= 0.0f) {
      sum += history[i];
      ++count;
    }
  }

  if (count == 0) {
    return -1.0f;
  }

  return sum / static_cast<float>(count);
}

OSD::BrightnessSample OSD::BrightnessMeter::measure() {
  BrightnessSample sample;
  IspStats stats;
  std::string mode;

  if (cfg && cfg->get<bool>("daynight.enabled")) {
    int live_pct = cfg->daynight.live_brightness_percent.load();
    int live_total_gain = cfg->daynight.live_total_gain.load();
    if (live_pct >= 0) {
      sample.current = static_cast<float>(live_pct);
      sample.total_gain = live_total_gain;
      const char *mode_ptr = cfg->daynight.live_mode.load();
      if (mode_ptr && *mode_ptr) {
        sample.mode = mode_ptr;
        std::transform(
            sample.mode.begin(), sample.mode.end(), sample.mode.begin(),
            [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
      } else {
        sample.mode = "UNKNOWN";
      }
      updateHistory(sample.current);
      sample.average = historyAverage();
      if (sample.average < 0.0f) {
        sample.average = sample.current;
      }
      sample.valid = true;
      return sample;
    }
  }

  float brightness = -1.0f;
  if (readIspStats(stats)) {
    brightness = computeFromStats(stats, mode);
  }

  if (brightness < 0.0f) {
    brightness = fallbackTimeBased(mode);
  }

  sample.current = brightness;
  if (!mode.empty()) {
    sample.mode = mode;
  }

  if (brightness >= 0.0f) {
    updateHistory(brightness);
    sample.average = historyAverage();
    if (sample.average < 0.0f) {
      sample.average = brightness;
    }
    sample.valid = true;
  }

  return sample;
}

std::string OSD::buildBrightnessText(const BrightnessSample &sample) {
  if (sample.total_gain < 0) {
    return std::string("--");
  }
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%d", sample.total_gain);
  return std::string(buffer);
}

void OSD::updateBrightnessText() {
  BrightnessSample sample = brightnessMeter.measure();
  if (!sample.valid) {
    sample.mode = "UNAVAIL";
  }

  std::string text = buildBrightnessText(sample);
  if (text.empty()) {
    text = "Brightness unavailable";
  }

  lastBrightnessText = text;
}

// ── lifecycle ────────────────────────────────────────────────────────

OSD *OSD::createNew(_osd &osd, int osdGrp, int encChn, const char *parent) {
  return new OSD(osd, osdGrp, encChn, parent);
}

void OSD::init() {
  int ret = 0;
  LOG_DEBUG("OSD init for begin");

  last_updated_second = -1;

  ret = IMP_Encoder_GetChnAttr(osdGrp, &channelAttributes);
  if (ret < 0) {
    LOG_DEBUG("IMP_Encoder_GetChnAttr() == " << ret);
  }

  stream_width = HAL_ENC_ATTR_WIDTH(channelAttributes);
  stream_height = HAL_ENC_ATTR_HEIGHT(channelAttributes);

  if (strcmp(parent, "stream0") == 0)
    stream_rotation = cfg->stream0.rotation;
  else if (strcmp(parent, "stream1") == 0)
    stream_rotation = cfg->stream1.rotation;

  LOG_DEBUG("IMP_Encoder_GetChnAttr read. Stream resolution: "
            << stream_width << "x" << stream_height
            << " rotation=" << stream_rotation);

  LOG_INFO("OSD: text only (SEI + subtitle), no IPU overlay, no font renderer");

  getIp(ip);
  gethostname(hostname, 64);

  is_started = true;
}

int OSD::start() {
  is_started = true;
  return 0;
}

int OSD::exit() {
  return 0;
}

// ── periodic update ──────────────────────────────────────────────────

void OSD::updateDisplayEverySecond() {
  struct timeval tm;
  gettimeofday(&tm, NULL);

  current = time(nullptr);
  ltime = localtime(&current);

  if (ltime->tm_sec != last_updated_second) {
    flag = 0;
    if (osd.time_enabled)      flag |= OSD_FLAG_TIME;
    if (osd.usertext_enabled)  flag |= OSD_FLAG_USER;
    if (osd.uptime_enabled)    flag |= OSD_FLAG_UPTIME;
    if (osd.brightness_enabled) flag |= OSD_FLAG_BRIGHTNESS;
    last_updated_second = ltime->tm_sec;
  } else {
    if (flag != 0) {
      if ((flag & OSD_FLAG_TIME) && osd.time_enabled) {
        strftime(timeFormatted, sizeof(timeFormatted), osd.time_format, ltime);
        flag ^= OSD_FLAG_TIME;
        return;
      }

      if ((flag & OSD_FLAG_USER) && osd.usertext_enabled) {
        std::string usertext = osd.usertext_format;

        if (strstr(osd.usertext_format, "%hostname") != nullptr)
          replace(usertext, "%hostname", hostname);
        if (strstr(osd.usertext_format, "%ipaddress") != nullptr)
          replace(usertext, "%ipaddress", ip);
        if (strstr(osd.usertext_format, "%fps") != nullptr) {
          char fps[4];
          snprintf(fps, 4, "%3d", osd.stats.fps);
          replace(usertext, "%fps", fps);
        }
        if (strstr(osd.usertext_format, "%bps") != nullptr) {
          char bps[8];
          snprintf(bps, 8, "%5d", osd.stats.bps);
          replace(usertext, "%bps", bps);
        }

        formattedUsertext_ = usertext;
        flag ^= OSD_FLAG_USER;
        return;
      }

      if ((flag & OSD_FLAG_UPTIME) && osd.uptime_enabled) {
        unsigned long currentUptime = getSystemUptime();
        unsigned long days = currentUptime / 86400;
        unsigned long hours = (currentUptime % 86400) / 3600;
        unsigned long minutes = (currentUptime % 3600) / 60;

        snprintf(uptimeFormatted, sizeof(uptimeFormatted), osd.uptime_format,
                 days, hours, minutes);
        flag ^= OSD_FLAG_UPTIME;
        return;
      }

      if ((flag & OSD_FLAG_BRIGHTNESS) && osd.brightness_enabled) {
        updateBrightnessText();
        flag ^= OSD_FLAG_BRIGHTNESS;
        return;
      }
    }
  }
}

// ── SEI / subtitle output ────────────────────────────────────────────

std::string OSD::getSEIJson() {
  std::lock_guard<std::mutex> lock(stateMutex_);

  std::string json = "{\"v\":1,";
  {
    char buf[64];
    snprintf(buf, sizeof(buf), "\"sw\":%u,\"sh\":%u,\"rotation\":%d,",
             stream_width, stream_height, stream_rotation);
    json += buf;
  }
  json += "\"elements\":[";
  bool first = true;

  auto addElement = [&](const char *type, const char *text,
                        const char *posStr) {
    if (!text || !text[0])
      return;

    // Parse x,y from position string
    int x = 0, y = 0;
    if (posStr && *posStr) {
      const char *comma = strchr(posStr, ',');
      if (comma) {
        char xb[16] = {0}, yb[16] = {0};
        size_t xl = (size_t)(comma - posStr);
        size_t yl = strlen(comma + 1);
        if (xl > 0 && xl < sizeof(xb)) {
          memcpy(xb, posStr, xl);
          xb[xl] = '\0';
          x = atoi(xb);
        }
        if (yl > 0 && yl < sizeof(yb)) {
          memcpy(yb, comma + 1, yl);
          yb[yl] = '\0';
          y = atoi(yb);
        }
      }
    }

    if (!first)
      json += ",";
    first = false;

    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"t\":\"%s\",\"text\":\"%s\",\"x\":%d,\"y\":%d}",
             type, text, x, y);
    json += buf;
  };

  if (osd.time_enabled)
    addElement("time", timeFormatted, osd.time_position);

  if (osd.usertext_enabled && !formattedUsertext_.empty())
    addElement("usertext", formattedUsertext_.c_str(),
               osd.usertext_position);

  if (osd.uptime_enabled)
    addElement("uptime", uptimeFormatted, osd.uptime_position);

  if (osd.brightness_enabled && !lastBrightnessText.empty())
    addElement("brightness", lastBrightnessText.c_str(),
               osd.brightness_position);

  json += "]}";
  return json;
}

std::string OSD::getPlaintextInfo() {
  std::lock_guard<std::mutex> lock(stateMutex_);

  std::string text;

  if (osd.time_enabled && timeFormatted[0]) {
    text += "TIME:";
    text += timeFormatted;
    text += "\r\n";
  }
  if (osd.usertext_enabled && !formattedUsertext_.empty()) {
    text += "USER:";
    text += formattedUsertext_;
    text += "\r\n";
  }
  if (osd.uptime_enabled && uptimeFormatted[0]) {
    text += "UPTIME:";
    text += uptimeFormatted;
    text += "\r\n";
  }
  if (osd.brightness_enabled && !lastBrightnessText.empty()) {
    text += "BRI:";
    text += lastBrightnessText;
    text += "\r\n";
  }

  return text;
}

// ── thread ───────────────────────────────────────────────────────────

void *OSD::thread_entry(void *arg) {
  LOG_DEBUG("start osd update thread.");

  global_osd_thread_signal = true;
  while (global_osd_thread_signal) {
    for (auto v : global_video) {
      if (v != nullptr) {
        if (v->active) {
          if ((v->imp_encoder->osd != nullptr)) {
            if (v->imp_encoder->osd->is_started) {
              v->imp_encoder->osd->updateDisplayEverySecond();
            } else {
              if (v->imp_encoder->osd->startup_delay_ticks) {
                v->imp_encoder->osd->startup_delay_ticks--;
              } else {
                v->imp_encoder->osd->start();
              }
            }
          }
        }
      }
    }
    usleep(THREAD_SLEEP_US);
  }

  LOG_DEBUG("exit osd update thread.");
  return 0;
}
