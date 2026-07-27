#include "OSD.hpp"
#include "Config.hpp"
#include "Logger.hpp"
#include "globals.hpp"
#include "imp_hal.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <json_config.h>
#include <pthread.h>
#include <sstream>
#include <unistd.h>
#include <vector>

namespace {
constexpr const char *PRIMARY_ISP_STATS = "/proc/jz/isp/isp-m0";
constexpr const char *SECONDARY_ISP_STATS = "/tmp/test-isp-m0";
constexpr float MAX_ANALOG_GAIN = 160.0f;
constexpr float MAX_DIGITAL_GAIN = 80.0f;
constexpr float DEFAULT_DAY_BRIGHTNESS = 70.0f;
constexpr float DEFAULT_NIGHT_BRIGHTNESS = 25.0f;
} // namespace

// ── helpers ──────────────────────────────────────────────────────────

static unsigned long getSystemUptime() {
  struct sysinfo info;
  if (sysinfo(&info) != 0)
    return 0;
  return info.uptime;
}

static int getIp(char *addressBuffer) {
  struct ifaddrs *ifAddrStruct = nullptr;
  getifaddrs(&ifAddrStruct);
  for (auto *ifa = ifAddrStruct; ifa; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET) {
      struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
      // skip loopback
      if (sin->sin_addr.s_addr == htonl(INADDR_LOOPBACK))
        continue;
      inet_ntop(AF_INET, &sin->sin_addr, addressBuffer, INET_ADDRSTRLEN);
    }
  }
  if (ifAddrStruct)
    freeifaddrs(ifAddrStruct);
  return 0;
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
    if (file.is_open())
      activePath = SECONDARY_ISP_STATS;
  }
  if (!file.is_open()) {
    if (!lastReadFailed)
      LOG_WARN("BrightnessMeter: unable to read ISP stats from " << PRIMARY_ISP_STATS);
    lastReadFailed = true;
    return false;
  }
  lastReadFailed = false;
  bool parsed = false;
  std::string line;
  while (std::getline(file, line)) {
    if (line.find("ISP Runing Mode :") != std::string::npos) {
      char mb[32] = {};
      if (sscanf(line.c_str(), "ISP Runing Mode : %31s", mb) == 1) {
        stats.mode = mb;
        parsed = true;
      }
    } else if (line.find("SENSOR Integration Time :") != std::string::npos) {
      sscanf(line.c_str(), "SENSOR Integration Time : %d", &stats.integrationTime);
      parsed = true;
    } else if (line.find("SENSOR Max Integration Time :") != std::string::npos) {
      sscanf(line.c_str(), "SENSOR Max Integration Time : %d", &stats.maxIntegrationTime);
      parsed = true;
    } else if (line.find("SENSOR analog gain :") != std::string::npos) {
      sscanf(line.c_str(), "SENSOR analog gain : %d", &stats.analogGain);
      parsed = true;
    } else if (line.find("SENSOR digital gain :") != std::string::npos) {
      sscanf(line.c_str(), "SENSOR digital gain : %d", &stats.digitalGain);
      parsed = true;
    } else if (line.find("ISP digital gain :") != std::string::npos) {
      sscanf(line.c_str(), "ISP digital gain : %d", &stats.ispDigitalGain);
      parsed = true;
    } else if (line.find("ISP EV value:") != std::string::npos) {
      sscanf(line.c_str(), "ISP EV value: %d", &stats.evValue);
      parsed = true;
    } else if (line.find("Brightness :") != std::string::npos) {
      sscanf(line.c_str(), "Brightness : %d", &stats.currentBrightness);
      parsed = true;
    }
  }
  return parsed;
}

float OSD::BrightnessMeter::computeFromStats(const IspStats &stats, std::string &mode) const {
  std::string ispMode = stats.mode;
  if (!ispMode.empty())
    std::transform(ispMode.begin(), ispMode.end(), ispMode.begin(),
                   [](unsigned char c) { return std::toupper(c); });

  float brightness = -1.0f;
  if (stats.integrationTime >= 0 && stats.maxIntegrationTime > 0) {
    float er = (float)stats.integrationTime / (float)stats.maxIntegrationTime;
    brightness = (1.0f - er) * 100.0f;
    if (stats.analogGain >= 0)
      brightness /= 1.0f + (float)stats.analogGain / MAX_ANALOG_GAIN;
    if (stats.ispDigitalGain > 0)
      brightness /= 1.0f + (float)stats.ispDigitalGain / MAX_DIGITAL_GAIN;
    brightness = std::clamp(brightness, 0.0f, 100.0f);
    mode = ispMode.empty() ? "UNKNOWN" : ispMode;
    return brightness;
  }
  if (stats.currentBrightness >= 0) {
    brightness = ((float)stats.currentBrightness / 255.0f) * 100.0f;
    brightness = std::clamp(brightness, 0.0f, 100.0f);
    mode = ispMode.empty() ? "UNKNOWN" : ispMode;
    return brightness;
  }
  if (!ispMode.empty()) {
    mode = ispMode;
    return ispMode == "DAY" ? DEFAULT_DAY_BRIGHTNESS : DEFAULT_NIGHT_BRIGHTNESS;
  }
  return -1.0f;
}

float OSD::BrightnessMeter::fallbackTimeBased(std::string &mode) const {
  time_t now = time(nullptr);
  if (now == (time_t)-1) { mode = "UNKNOWN"; return -1.0f; }
  struct tm *ti = localtime(&now);
  if (!ti) { mode = "UNKNOWN"; return -1.0f; }
  bool isDay = ti->tm_hour >= 6 && ti->tm_hour <= 18;
  mode = isDay ? "DAY" : "NIGHT";
  return isDay ? DEFAULT_DAY_BRIGHTNESS : DEFAULT_NIGHT_BRIGHTNESS;
}

void OSD::BrightnessMeter::updateHistory(float value) {
  history[historyIndex] = value;
  historyIndex = (historyIndex + 1) % history.size();
  if (historyIndex == 0) historyFilled = true;
}

float OSD::BrightnessMeter::historyAverage() const {
  size_t limit = historyFilled ? history.size() : historyIndex;
  if (limit == 0) return -1.0f;
  float sum = 0.0f;
  size_t count = 0;
  for (size_t i = 0; i < limit; ++i)
    if (history[i] >= 0.0f) { sum += history[i]; ++count; }
  return count ? sum / (float)count : -1.0f;
}

OSD::BrightnessSample OSD::BrightnessMeter::measure() {
  BrightnessSample sample;
  IspStats stats;
  std::string mode;

  /* Photosensing delegated to daynightd.
   * Read brightness and gain from daynightd's files for OSD overlay. */
  if (cfg && cfg->get<bool>("daynight.enabled")) {
    FILE *fp = fopen("/run/thingino/daynight_brightness", "r");
    if (fp) {
      char buf[32];
      if (fgets(buf, sizeof(buf), fp)) {
        int pct = atoi(buf);
        if (pct >= 0) {
          sample.current = (float)pct;
          sample.total_gain = pct;  /* brightness % is the unified metric */
          /* Try to get mode string from mode file */
          FILE *mf = fopen("/run/thingino/daynight_mode", "r");
          if (mf) {
            char mbuf[16];
            if (fgets(mbuf, sizeof(mbuf), mf)) {
              size_t l = strlen(mbuf);
              while (l > 0 && (mbuf[l-1] == '\n' || mbuf[l-1] == ' '))
                mbuf[--l] = '\0';
              sample.mode = mbuf;
              if (!sample.mode.empty())
                std::transform(sample.mode.begin(), sample.mode.end(),
                               sample.mode.begin(),
                               [](unsigned char c) { return std::toupper(c); });
            }
            fclose(mf);
          }
          if (sample.mode.empty()) sample.mode = "DAYNIGHTD";
          updateHistory(sample.current);
          sample.average = historyAverage();
          if (sample.average < 0.0f) sample.average = sample.current;
          sample.valid = true;
          fclose(fp);
          return sample;
        }
      }
      fclose(fp);
    }
  }

  float brightness = -1.0f;
  if (readIspStats(stats)) brightness = computeFromStats(stats, mode);
  if (brightness < 0.0f) brightness = fallbackTimeBased(mode);
  sample.current = brightness;
  if (!mode.empty()) sample.mode = mode;
  if (brightness >= 0.0f) {
    updateHistory(brightness);
    sample.average = historyAverage();
    if (sample.average < 0.0f) sample.average = brightness;
    sample.valid = true;
  }
  return sample;
}

std::string OSD::buildBrightnessText(const BrightnessSample &sample) {
  if (sample.total_gain < 0) return "--";
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", sample.total_gain);
  return buf;
}

void OSD::updateBrightnessText() {
  BrightnessSample sample = brightnessMeter.measure();
  if (!sample.valid) sample.mode = "UNAVAIL";
  std::string text = buildBrightnessText(sample);
  if (text.empty()) text = "Brightness unavailable";
  lastBrightnessText = text;
}

// ── element loading from JSON ────────────────────────────────────────

void OSD::loadElements() {
  elements_.clear();

  std::string path = "osd.elements";
  JsonValue *cfgJson = cfg->jsonConfig;
  if (!cfgJson) return;

  JsonValue *elems = get_nested_item(cfgJson, path.c_str());
  if (!elems || elems->type != JSON_OBJECT) return;

  json_object_object_foreach(elems, key, val) {
    if (!val || val->type != JSON_OBJECT) continue;

    OSDElement el;
    el.name = key;

    JsonValue *t = get_object_item(val, "type");
    if (t && t->type == JSON_STRING && t->value.string)
      el.type = t->value.string;

    JsonValue *f = get_object_item(val, "format");
    if (f && f->type == JSON_STRING && f->value.string)
      el.format = f->value.string;

    JsonValue *p = get_object_item(val, "position");
    if (p && p->type == JSON_STRING && p->value.string)
      el.position = p->value.string;

    if (el.type == "text" && !el.format.empty())
      el.text = el.format;

    // timestamp and hostname appear in the RTP subtitle track
    el.in_subtitle = (el.type == "timestamp" || el.type == "hostname");

    if (!el.type.empty())
      elements_.push_back(std::move(el));
  }

}

// ── text generation per element ──────────────────────────────────────

void OSD::updateElementText() {
  char buf[64];
  unsigned long uptime = 0;

  for (auto &el : elements_) {
    if (el.type == "timestamp") {
      strftime(buf, sizeof(buf), el.format.c_str(), ltime);
      el.text = buf;
    } else if (el.type == "hostname") {
      el.text = hostname;
    } else if (el.type == "ipaddress") {
      el.text = ip;
    } else if (el.type == "uptime") {
      if (uptime == 0) uptime = getSystemUptime();
      unsigned long d = uptime / 86400;
      unsigned long h = (uptime % 86400) / 3600;
      unsigned long m = (uptime % 3600) / 60;
      snprintf(buf, sizeof(buf), el.format.c_str(), d, h, m);
      el.text = buf;
    } else if (el.type == "gain") {
      el.text = lastBrightnessText;
    } else if (el.type == "text") {
      // static text, already set by loadElements from format field
    }
  }
}

// ── lifecycle ────────────────────────────────────────────────────────

OSD *OSD::createNew(_osd &osd, int osdGrp, int encChn, const char *parent) {
  return new OSD(osd, osdGrp, encChn, parent);
}

void OSD::init() {
  int ret = IMP_Encoder_GetChnAttr(osdGrp, &channelAttributes);
  if (ret < 0)
    LOG_DEBUG("IMP_Encoder_GetChnAttr() == " << ret);

  stream_width = HAL_ENC_ATTR_WIDTH(channelAttributes);
  stream_height = HAL_ENC_ATTR_HEIGHT(channelAttributes);

  // stream rotation from whichever stream we're attached to
  if (strcmp(parent, "stream0") == 0)
    stream_rotation = cfg->stream0.rotation;
  else if (strcmp(parent, "stream1") == 0)
    stream_rotation = cfg->stream1.rotation;

  LOG_DEBUG("OSD: " << stream_width << "x" << stream_height
            << " rotation=" << stream_rotation);

  getIp(ip);
  gethostname(hostname, 64);
  loadElements();

  LOG_INFO("OSD: " << elements_.size() << " elements loaded, SEI + subtitle only");

  last_updated_second = -1;
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

extern bool global_reload_osd;

void OSD::updateDisplayEverySecond() {
  if (global_reload_osd) {
    global_reload_osd = false;
    loadElements();
  }

  current = time(nullptr);
  ltime = localtime(&current);

  if (ltime->tm_sec == last_updated_second)
    return; // already updated this second

  last_updated_second = ltime->tm_sec;

  // Always refresh gain for gain-type elements
  bool hasGain = false;
  for (auto &el : elements_) {
    if (el.type == "gain") { hasGain = true; break; }
  }
  if (hasGain)
    updateBrightnessText();

  updateElementText();
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

  for (auto &el : elements_) {
    if (el.text.empty()) continue;

    int x = 0, y = 0;
    if (!el.position.empty()) {
      const char *comma = strchr(el.position.c_str(), ',');
      if (comma) {
        x = atoi(el.position.c_str());
        y = atoi(comma + 1);
      }
    }

    if (!first) json += ",";
    first = false;

    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"t\":\"%s\",\"text\":\"%s\",\"x\":%d,\"y\":%d}",
             el.type.c_str(), el.text.c_str(), x, y);
    json += buf;
  }

  json += "]}";
  return json;
}

std::string OSD::getPlaintextInfo() {
  std::lock_guard<std::mutex> lock(stateMutex_);

  std::string text;
  for (auto &el : elements_) {
    if (el.text.empty() || !el.in_subtitle) continue;
    text += el.name;
    text += ":";
    text += el.text;
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
      if (v && v->active && v->imp_encoder && v->imp_encoder->osd) {
        if (v->imp_encoder->osd->is_started)
          v->imp_encoder->osd->updateDisplayEverySecond();
        else if (v->imp_encoder->osd->startup_delay_ticks)
          v->imp_encoder->osd->startup_delay_ticks--;
        else
          v->imp_encoder->osd->start();
      }
    }
    usleep(THREAD_SLEEP_US);
  }
  LOG_DEBUG("exit osd update thread.");
  return nullptr;
}
