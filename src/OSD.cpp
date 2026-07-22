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
#include <functional>
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

#ifdef OSD_BURN_TIMESTAMP
// ── embedded 5x7 bitmap font for burned-in timestamp ─────────────────
// Only the glyphs needed for "%Y-%m-%d %H:%M:%S" are defined. Each row is
// stored in the low 5 bits; the leftmost pixel is bit 4 (0x10).
constexpr int FONT_W = 5;
constexpr int FONT_H = 7;

const uint8_t FONT_DIGITS[10][FONT_H] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, // 1
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, // 2
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}, // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, // 4
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, // 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, // 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, // 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, // 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, // 9
};
const uint8_t FONT_DASH[FONT_H] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
const uint8_t FONT_COLON[FONT_H] = {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00};

// Uppercase letters needed for the "PRIVACY" status word.
const uint8_t FONT_P[FONT_H] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
const uint8_t FONT_R[FONT_H] = {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
const uint8_t FONT_I[FONT_H] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F};
const uint8_t FONT_V[FONT_H] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04};
const uint8_t FONT_A[FONT_H] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
const uint8_t FONT_C[FONT_H] = {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
const uint8_t FONT_Y[FONT_H] = {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};

// Returns the 7-row glyph for a character, or nullptr for a blank cell.
const uint8_t *glyphFor(char c) {
  if (c >= '0' && c <= '9')
    return FONT_DIGITS[c - '0'];
  switch (c) {
  case '-': return FONT_DASH;
  case ':': return FONT_COLON;
  case 'P': return FONT_P;
  case 'R': return FONT_R;
  case 'I': return FONT_I;
  case 'V': return FONT_V;
  case 'A': return FONT_A;
  case 'C': return FONT_C;
  case 'Y': return FONT_Y;
  default:  return nullptr; // space / unsupported -> blank advance
  }
}
#endif // OSD_BURN_TIMESTAMP
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

// ── burned-in timestamp overlay ──────────────────────────────────────
#ifdef OSD_BURN_TIMESTAMP

void OSD::renderTimestamp(const char *text) {
  const int scale = ts_scale_;
  const int pad = scale * 2;
  const int cell = (FONT_W + 1) * scale; // glyph width + 1 column spacing

  int n = (int)strlen(text);
  int textW = n * cell;
  if (textW > 0)
    textW -= scale; // trailing char has no spacing

  int w = textW + pad * 2;
  int h = FONT_H * scale + pad * 2;
  if (w & 1)
    ++w; // IMP regions expect an even width

  ts_width_ = (uint16_t)w;
  ts_height_ = (uint16_t)h;
  ts_buf_.assign((size_t)w * h * 4, 0);
  uint8_t *img = ts_buf_.data();

  // Subtle dark background box; the opaque per-glyph outline below carries
  // most of the contrast, so the box can stay light.
  const uint8_t bg[4] = {0, 0, 0, 110}; // B, G, R, A
  for (int i = 0; i < w * h; ++i) {
    img[i * 4 + 0] = bg[0];
    img[i * 4 + 1] = bg[1];
    img[i * 4 + 2] = bg[2];
    img[i * 4 + 3] = bg[3];
  }

  const uint8_t outline_color[4] = {0, 0, 0, 255};    // opaque black halo
  const uint8_t fill_color[4] = {255, 255, 255, 255}; // opaque white glyph
  const int outline = std::max(1, scale / 2);         // halo thickness (px)

  // Stamp a solid scale×scale block at a destination top-left position.
  auto putBlock = [&](int dx, int dy, const uint8_t *color) {
    for (int yy = 0; yy < scale; ++yy) {
      for (int xx = 0; xx < scale; ++xx) {
        int px = dx + xx, py = dy + yy;
        if (px < 0 || px >= w || py < 0 || py >= h)
          continue;
        int idx = (py * w + px) * 4;
        img[idx + 0] = color[0];
        img[idx + 1] = color[1];
        img[idx + 2] = color[2];
        img[idx + 3] = color[3];
      }
    }
  };

  // Iterate every set glyph pixel of the whole string, applying `fn`.
  auto forEachGlyphPixel = [&](const std::function<void(int, int)> &fn) {
    int penX = pad;
    for (const char *p = text; *p; ++p) {
      const uint8_t *g = glyphFor(*p);
      if (g) {
        for (int ry = 0; ry < FONT_H; ++ry) {
          uint8_t bits = g[ry];
          for (int rx = 0; rx < FONT_W; ++rx) {
            if (bits & (1 << (FONT_W - 1 - rx)))
              fn(penX + rx * scale, pad + ry * scale);
          }
        }
      }
      penX += cell;
    }
  };

  // Pass 1: black outline — dilate each set pixel by `outline` px (circular).
  forEachGlyphPixel([&](int bx, int by) {
    for (int oy = -outline; oy <= outline; ++oy)
      for (int ox = -outline; ox <= outline; ++ox)
        if (ox * ox + oy * oy <= outline * outline)
          putBlock(bx + ox, by + oy, outline_color);
  });

  // Pass 2: white fill on top of the halo.
  forEachGlyphPixel([&](int bx, int by) { putBlock(bx, by, fill_color); });
}

void OSD::updateTimestampOverlay() {
  char base[32];
  if (strftime(base, sizeof(base), "%Y-%m-%d %H:%M:%S", ltime) == 0)
    return;

  // Append a "PRIVACY" status word after the date while privacy is active on
  // this channel. The overlay stays visible (layer 1) above the privacy cover.
  bool privacy_active = false;
  for (auto v : global_video) {
    if (v && v->encChn == encChn) {
      privacy_active = v->privacy_requested.load(std::memory_order_acquire);
      break;
    }
  }

  char text[48];
  snprintf(text, sizeof(text), "%s%s", base,
           privacy_active ? " PRIVACY" : "");

  bool need_render = !(ts_region_created_ && last_ts_text_ == text);
  uint16_t prevW = ts_width_, prevH = ts_height_;
  if (need_render) {
    renderTimestamp(text);
    last_ts_text_ = text;
  }

  if (!ts_region_created_) {
    // The OSD group is created and bound by IMPEncoder after this object is
    // constructed, so we register the region on the first periodic update
    // once the pipeline is live.
    ts_rgn_ = IMP_OSD_CreateRgn(nullptr);
    if (ts_rgn_ == INVHANDLE) {
      LOG_ERROR("OSD: IMP_OSD_CreateRgn failed for timestamp overlay");
      return;
    }
    IMP_OSD_RegisterRgn(ts_rgn_, osdGrp, nullptr);

    memset(&ts_attr_, 0, sizeof(ts_attr_));
    ts_attr_.type = OSD_REG_PIC;
    ts_attr_.fmt = PIX_FMT_BGRA;
    ts_attr_.rect.p0.x = ts_margin_;
    ts_attr_.rect.p0.y = ts_margin_;
    ts_attr_.rect.p1.x = ts_margin_ + ts_width_ - 1;
    ts_attr_.rect.p1.y = ts_margin_ + ts_height_ - 1;
    ts_attr_.data.picData.pData = ts_buf_.data();
    IMP_OSD_SetRgnAttr(ts_rgn_, &ts_attr_);

    IMPOSDGrpRgnAttr grp;
    memset(&grp, 0, sizeof(grp));
    grp.show = 1;
    // Higher layer = closer to the front. Keep the timestamp one above the
    // privacy cover (layer 1) so it stays visible over the privacy screen.
    grp.layer = 2;
    grp.gAlphaEn = 1; // per-pixel alpha blending
    grp.fgAlhpa = 255;
    grp.bgAlhpa = 0;
    IMP_OSD_SetGrpRgnAttr(ts_rgn_, osdGrp, &grp);

    if (!osd_group_started_) {
      IMP_OSD_Start(osdGrp);
      osd_group_started_ = true;
    }
    ts_region_created_ = true;
    LOG_INFO("OSD: burned-in timestamp overlay enabled ("
             << ts_width_ << "x" << ts_height_ << ", scale " << ts_scale_
             << ")");
  } else if (need_render) {
    if (ts_width_ != prevW || ts_height_ != prevH) {
      ts_attr_.rect.p1.x = ts_margin_ + ts_width_ - 1;
      ts_attr_.rect.p1.y = ts_margin_ + ts_height_ - 1;
      ts_attr_.data.picData.pData = ts_buf_.data();
      IMP_OSD_SetRgnAttr(ts_rgn_, &ts_attr_);
    } else {
      ts_attr_.data.picData.pData = ts_buf_.data();
      IMP_OSD_UpdateRgnAttrData(ts_rgn_, &ts_attr_.data);
    }
  }
  // The overlay stays visible during privacy: its region sits at layer 1,
  // above the privacy cover (layer 0), so the timestamp is composited on top
  // of the black privacy screen.
}

#endif // OSD_BURN_TIMESTAMP

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

#ifdef OSD_BURN_TIMESTAMP
  // Scale the burned-in timestamp glyphs to the stream resolution so the
  // overlay stays readable on both the main and sub streams.
  ts_scale_ = std::max(2, stream_width / 480);
#endif

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
#ifdef OSD_BURN_TIMESTAMP
  // Tear down the burned-in timestamp region. IMPEncoder destroys the OSD
  // group before calling us, so unregister best-effort and always free the
  // region handle to avoid leaking it.
  if (ts_region_created_) {
    IMP_OSD_ShowRgn(ts_rgn_, osdGrp, 0);
    IMP_OSD_UnRegisterRgn(ts_rgn_, osdGrp);
    IMP_OSD_DestroyRgn(ts_rgn_);
    ts_rgn_ = INVHANDLE;
    ts_region_created_ = false;
  }
#endif
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

#ifdef OSD_BURN_TIMESTAMP
  // Burn the timestamp into the video via a hardware OSD region.
  updateTimestampOverlay();
#endif
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
    if (el.text.empty()) continue;
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
